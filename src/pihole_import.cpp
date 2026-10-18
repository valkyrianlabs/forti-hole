#include "pihole_import.hpp"

#include "config.hpp"
#include "fetch/https.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace fh {

    using json = nlohmann::json;

    namespace {

        class ApiReader final : public PiholeReader {
            std::string base_, password_, ca_;

            json call(const std::string& method, const std::string& path, const std::string& sid, const std::string& body = {}) {
                HttpsRequest request;
                request.method = method;
                request.url = base_ + path;
                request.ca_bundle = ca_;
                request.body = body;
                if (!sid.empty()) request.headers.emplace_back("X-FTL-SID", sid);
                auto response = https_request(request);
                if (response.status == 401) throw HttpsError(std::format("{} {}: the Pi-hole refused the password", method, path));
                if (response.status < 200 || response.status >= 300) throw HttpsError(std::format("{} {}: HTTP {}", method, path, response.status));
                if (response.body.empty()) return json::object();
                auto parsed = json::parse(response.body, nullptr, false);
                if (parsed.is_discarded()) throw HttpsError(std::format("{} {}: the reply is not JSON", method, path));
                return parsed;
            }

        public:
            ApiReader(std::string url, std::string password, std::string ca)
                    : base_(validated_base_url(url)), password_(std::move(password)), ca_(std::move(ca)) {
                if (base_.ends_with("/api")) base_.resize(base_.size() - 4);
            }

            PiholeData read() override {
                auto auth = call("POST", "/api/auth", "", json{{"password", password_}}.dump());
                auto session = auth.value("session", json::object());
                std::string sid = session.value("sid", std::string());
                if (!session.value("valid", false)) throw HttpsError("the Pi-hole did not open a session (wrong password?)");
                PiholeData data;
                try {
                    data.lists = call("GET", "/api/lists", sid).value("lists", json::array());
                    data.groups = call("GET", "/api/groups", sid).value("groups", json::array());
                    data.domains = call("GET", "/api/domains", sid).value("domains", json::array());
                } catch (...) {
                    try {
                        if (!sid.empty()) call("DELETE", "/api/auth", sid);
                    } catch (...) {
                    }
                    throw;
                }
                // Pi-hole keeps a limited number of sessions: give this one back.
                if (!sid.empty()) {
                    try {
                        call("DELETE", "/api/auth", sid);
                    } catch (...) {
                    }
                }
                return data;
            }
        };

        std::string sanitize(std::string_view text, std::size_t max) {
            std::string out;
            for (char c: text) {
                char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) out += lower;
                else if (!out.empty() && out.back() != '-') out += '-';
            }
            while (!out.empty() && out.back() == '-') out.pop_back();
            if (out.size() > max) out.resize(max);
            while (!out.empty() && out.back() == '-') out.pop_back();
            return out;
        }

        std::string guess_format(std::string_view url, std::vector<std::string>& notes) {
            if (url.find("/adblock/") != std::string_view::npos) return "adblock";
            if (url.find("/wildcard/") != std::string_view::npos) return url.find("-onlydomains") != std::string_view::npos ? "subtree-domains" : "wildcard";
            if (url.find("/hosts/") != std::string_view::npos || url.find("StevenBlack/hosts") != std::string_view::npos || url.ends_with("/hosts") ||
                url.find("hosts.txt") != std::string_view::npos)
                return "hosts";
            if (url.find("/domains/") != std::string_view::npos) return "domains";
            notes.push_back(std::format("{}: format guessed as hosts; check it (adblock, hosts, domains, wildcard)", url));
            return "hosts";
        }

        // hagezi TIF: ".../tif.txt", ".../tif.medium.txt", ".../tif.mini.txt" -> base URL and variant.
        bool hagezi_tif(std::string_view url, std::string& prefix, std::string& suffix, std::string& variant) {
            if (url.find("hagezi") == std::string_view::npos) return false;
            for (std::string_view name: {"tif.medium", "tif.mini", "tif"}) {
                auto at = url.rfind(std::string("/") + std::string(name) + ".txt");
                if (at == std::string_view::npos) continue;
                prefix = std::string(url.substr(0, at + 1));
                suffix = ".txt";
                variant = name == "tif" ? "full" : std::string(name.substr(4));
                return true;
            }
            return false;
        }

        std::vector<int> group_ids(const json& item) {
            std::vector<int> ids;
            for (const auto& g: item.value("groups", json::array()))
                if (g.is_number_integer()) ids.push_back(g.get<int>());
            std::sort(ids.begin(), ids.end());
            return ids;
        }

    }  // namespace

    std::unique_ptr<PiholeReader> pihole_api(std::string url, std::string password, std::string ca_bundle) {
        return std::make_unique<ApiReader>(std::move(url), std::move(password), std::move(ca_bundle));
    }

    ImportResult import_pihole(const PiholeData& data) {
        ImportResult result;
        auto& notes = result.notes;

        // Groups that are enabled.
        std::map<int, std::string> group_names;
        for (const auto& g: data.groups)
            if (g.value("enabled", true)) group_names[g.value("id", 0)] = g.value("name", std::string("group"));

        // Enabled blocklists and the enabled groups using each.
        struct List {
            std::string url;
            std::vector<int> groups;
        };
        std::vector<List> lists;
        for (const auto& l: data.lists) {
            if (l.value("type", std::string("block")) != "block" || !l.value("enabled", true)) continue;
            List list{l.value("address", std::string()), {}};
            for (int id: group_ids(l))
                if (group_names.contains(id)) list.groups.push_back(id);
            if (list.url.empty()) continue;
            if (list.groups.empty()) {
                notes.push_back(std::format("{}: no enabled group uses it; left out", list.url));
                continue;
            }
            if (!list.url.starts_with("https://") && !list.url.starts_with("file:///")) {
                notes.push_back(std::format("{}: forti-hole downloads over https:// only; left out", list.url));
                continue;
            }
            lists.push_back(std::move(list));
        }
        if (lists.empty()) throw ConfigError("the Pi-hole has no enabled blocklist used by an enabled group");

        // Profiles: groups with lists. Names: sanitized, unique, 1-12 characters.
        std::set<int> used_groups;
        for (const auto& l: lists) used_groups.insert(l.groups.begin(), l.groups.end());
        std::map<int, std::string> profile_of;
        std::set<std::string> taken = {"all"};
        for (int id: used_groups) {
            std::string name = sanitize(group_names[id], 12);
            if (name.empty() || taken.contains(name)) {
                std::string base = name.empty() ? "group" : name.substr(0, 9);
                for (int n = 2; taken.contains(name) || name.empty(); ++n) name = std::format("{}{}", base, n);
            }
            taken.insert(name);
            profile_of[id] = name;
        }

        // Tiers: one per distinct group set, in order of first appearance.
        std::map<std::vector<int>, std::string> tier_of;
        std::vector<std::string> tiers;
        auto tier_for = [&](const std::vector<int>& groups) {
            if (auto it = tier_of.find(groups); it != tier_of.end()) return it->second;
            std::string name = groups.size() == used_groups.size() ? "common" : std::format("tier{}", tiers.size() + 1);
            if (std::find(tiers.begin(), tiers.end(), name) != tiers.end()) name = std::format("tier{}", tiers.size() + 1);
            tier_of[groups] = name;
            tiers.push_back(name);
            return name;
        };

        json sources = json::array();
        std::set<std::string> ids;
        bool have_auto = false;
        for (const auto& l: lists) {
            std::string tier = tier_for(l.groups);
            std::string prefix, suffix, variant;
            json source;
            if (hagezi_tif(l.url, prefix, suffix, variant) && !have_auto) {
                source = {{"id", "hagezi-tif"},
                          {"tier", tier},
                          {"format", "adblock"},
                          {"variants", {{"full", prefix + "tif" + suffix}, {"medium", prefix + "tif.medium" + suffix}, {"mini", prefix + "tif.mini" + suffix}}},
                          {"variant", "auto"},
                          {"auto", {"medium", "mini"}}};
                have_auto = true;
                if (variant == "full")
                    notes.push_back("hagezi TIF (full) became variant \"auto\" (medium, else mini): full TIF does not fit entry-level FortiGates; "
                                    "set \"variant\": \"full\" on a larger model");
            } else {
                auto slash = l.url.find_last_of('/');
                std::string id = sanitize(l.url.substr(slash == std::string::npos ? 0 : slash + 1), 40);
                if (l.url.find("hagezi") != std::string::npos) id = "hagezi-" + id;
                else if (l.url.find("StevenBlack") != std::string::npos) id = "stevenblack";
                if (id.empty()) id = "list";
                std::string unique = id;
                for (int n = 2; ids.contains(unique); ++n) unique = std::format("{}-{}", id, n);
                id = unique;
                source = {{"id", id}, {"tier", tier}, {"format", guess_format(l.url, notes)}, {"url", l.url}};
            }
            ids.insert(source["id"].get<std::string>());
            sources.push_back(std::move(source));
        }

        json profiles = json::object();
        for (int id: used_groups) {
            json profile_tiers = json::array();
            for (const auto& [groups, tier]: tier_of)
                if (std::binary_search(groups.begin(), groups.end(), id)) profile_tiers.push_back(tier);
            // Config order of tiers.
            std::vector<std::string> ordered;
            for (const auto& t: tiers)
                if (std::find(profile_tiers.begin(), profile_tiers.end(), t) != profile_tiers.end()) ordered.push_back(t);
            profiles[profile_of[id]] = {{"tiers", ordered}, {"comment", std::format("Pi-hole group \"{}\"", group_names[id])}};
        }

        auto profile_list = [&](const json& item) {
            json names = json::array();
            auto groups = group_ids(item);
            std::set<int> relevant;
            for (int id: groups)
                if (profile_of.contains(id)) relevant.insert(id);
            if (relevant.size() == used_groups.size()) return names;   // every profile: leave empty
            for (int id: relevant) names.push_back(profile_of[id]);
            return names;
        };
        json allow = json::array(), allow_regex = json::array(), block = json::array();
        for (const auto& d: data.domains) {
            if (!d.value("enabled", true)) continue;
            std::string type = d.value("type", std::string()), kind = d.value("kind", std::string()), domain = d.value("domain", std::string());
            json names = profile_list(d);
            bool any = !group_ids(d).empty();
            if (!any) continue;
            if (type == "allow" && kind == "exact") {
                json entry = {{"name", domain}, {"scope", "exact"}, {"comment", "from Pi-hole"}};
                if (!names.empty()) entry["profiles"] = names;
                allow.push_back(std::move(entry));
            } else if (type == "allow" && kind == "regex") {
                json entry = {{"pattern", domain}, {"comment", "from Pi-hole"}};
                if (!names.empty()) entry["profiles"] = names;
                allow_regex.push_back(std::move(entry));
            } else if (type == "deny" && kind == "exact") {
                // A deny entry belongs to the tier of its groups (created when no list has that set).
                std::vector<int> groups;
                for (int id: group_ids(d))
                    if (profile_of.contains(id)) groups.push_back(id);
                if (groups.empty()) continue;
                std::string tier = tier_for(groups);
                if (!profiles.empty())
                    for (int id: groups) {
                        auto& t = profiles[profile_of[id]]["tiers"];
                        if (std::find(t.begin(), t.end(), tier) == t.end()) t.push_back(tier);
                    }
                block.push_back({{"name", domain}, {"scope", "exact"}, {"tier", tier}, {"comment", "from Pi-hole"}});
            } else if (type == "deny" && kind == "regex") {
                notes.push_back(std::format("regex deny entry {}: FortiOS feeds hold names, not expressions; left out", domain));
            }
        }

        json config = example_config();
        config["tiers"] = tiers;
        config["sources"] = sources;
        config["profiles"] = profiles;
        if (!allow.empty()) config["allow"] = allow;
        if (!allow_regex.empty()) config["allow_regex"] = allow_regex;
        if (!block.empty()) config["block"] = block;
        config["comment"] = "imported from Pi-hole by forti-hole import pihole";
        // A configuration that does not validate is a bug here, not the operator's problem.
        parse_config(config);
        result.config = std::move(config);
        return result;
    }

}  // namespace fh
