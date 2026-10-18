#include "legacy.hpp"

#include "config.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace fh {

    using json = nlohmann::json;

    namespace {

        struct Line {
            std::size_t indent = 0;
            std::string text;   // without indentation and comments
            std::size_t number = 0;
        };

        std::string strip_comment(std::string_view line) {
            char quote = 0;
            for (std::size_t i = 0; i < line.size(); ++i) {
                char c = line[i];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                    return std::string(line.substr(0, i));
                }
            }
            return std::string(line);
        }

        std::string trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return std::string(s);
        }

        json scalar(const std::string& raw) {
            std::string s = trim(raw);
            if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"'))) return s.substr(1, s.size() - 2);
            if (s == "true" || s == "True") return true;
            if (s == "false" || s == "False") return false;
            if (s == "null" || s == "~" || s.empty()) return nullptr;
            if (!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }) && s.size() < 10) return std::stol(s);
            return s;
        }

        // "key: value" -> (key, value); the colon must be followed by space or end of line.
        bool split_key(const std::string& text, std::string& key, std::string& value) {
            char quote = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                char c = text[i];
                if (quote) {
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '\'' || c == '"') quote = c;
                else if (c == ':' && (i + 1 == text.size() || text[i + 1] == ' ')) {
                    key = trim(text.substr(0, i));
                    if (key.size() >= 2 && (key.front() == '\'' || key.front() == '"')) key = key.substr(1, key.size() - 2);
                    value = trim(text.substr(i + 1));
                    return true;
                }
            }
            return false;
        }

        class Parser {
            std::vector<Line> lines_;
            std::size_t at_ = 0;

        public:
            explicit Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

            [[noreturn]] void fail(std::string_view why) const {
                throw LegacyError(std::format("line {}: {}", at_ < lines_.size() ? lines_[at_].number : lines_.empty() ? 0 : lines_.back().number, why));
            }

            // The block starting at the current line (a mapping or a sequence, at that line's indentation).
            json block() {
                if (at_ >= lines_.size()) return nullptr;
                if (lines_[at_].text.starts_with("- ") || lines_[at_].text == "-") return sequence(lines_[at_].indent);
                return mapping(lines_[at_].indent);
            }

            json mapping(std::size_t indent) {
                json out = json::object();
                while (at_ < lines_.size() && lines_[at_].indent == indent && !lines_[at_].text.starts_with("- ")) {
                    std::string key, value;
                    if (!split_key(lines_[at_].text, key, value)) fail("expected \"key: value\"");
                    ++at_;
                    if (!value.empty()) out[key] = scalar(value);
                    else if (at_ < lines_.size() && (lines_[at_].indent > indent || (lines_[at_].indent == indent && lines_[at_].text.starts_with("- "))))
                        out[key] = block();
                    else out[key] = nullptr;
                }
                if (at_ < lines_.size() && lines_[at_].indent > indent) fail("unexpected indentation");
                return out;
            }

            json sequence(std::size_t indent) {
                json out = json::array();
                while (at_ < lines_.size() && lines_[at_].indent == indent && (lines_[at_].text.starts_with("- ") || lines_[at_].text == "-")) {
                    std::string rest = lines_[at_].text == "-" ? std::string() : trim(lines_[at_].text.substr(2));
                    std::size_t item_indent = indent + 2;
                    std::string key, value;
                    if (rest.empty()) {
                        ++at_;
                        out.push_back(at_ < lines_.size() && lines_[at_].indent > indent ? block() : json(nullptr));
                    } else if (split_key(rest, key, value)) {
                        // "- key: value" starts a mapping whose other keys are indented to match.
                        lines_[at_].indent = item_indent;
                        lines_[at_].text = rest;
                        out.push_back(mapping(item_indent));
                    } else {
                        ++at_;
                        out.push_back(scalar(rest));
                    }
                }
                return out;
            }
        };

        std::string sanitize(std::string_view text, std::size_t max, bool dots = false) {
            std::string out;
            for (char c: text) {
                char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || (dots && lower == '.')) out += lower;
                else if (!out.empty() && out.back() != '-') out += '-';
            }
            while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
            if (out.size() > max) out.resize(max);
            return out;
        }

    }  // namespace

    json parse_legacy_yaml(std::string_view text, const std::vector<std::string>& skip) {
        std::vector<Line> lines;
        std::size_t number = 0, start = 0;
        while (start <= text.size()) {
            auto end = text.find('\n', start);
            std::string raw(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            ++number;
            start = end == std::string_view::npos ? text.size() + 1 : end + 1;
            std::string body = strip_comment(raw);
            std::size_t indent = 0;
            while (indent < body.size() && body[indent] == ' ') ++indent;
            std::string content = trim(body);
            if (content.empty()) continue;
            if (body[indent] == '\t') throw LegacyError(std::format("line {}: tabs are not allowed for indentation", number));
            lines.push_back({indent, content, number});
        }
        // Drop the skipped top-level sections before parsing: their values never reach a JSON value.
        std::vector<Line> kept;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string key, value;
            if (lines[i].indent == 0 && split_key(lines[i].text, key, value) && std::find(skip.begin(), skip.end(), key) != skip.end()) {
                while (i + 1 < lines.size() && lines[i + 1].indent > 0) ++i;
                continue;
            }
            kept.push_back(std::move(lines[i]));
        }
        if (kept.empty()) return json::object();
        Parser parser(std::move(kept));
        return parser.mapping(0);
    }

    LegacyConfig convert_legacy_config(const json& legacy) {
        LegacyConfig out;
        if (!legacy.is_object()) throw LegacyError("the legacy configuration is not a mapping");
        const auto& naming = legacy.value("naming_convention", json::object());
        out.feed_prefix = (naming.is_object() ? naming.value("prefix", std::string("forti-hole")) : std::string("forti-hole")) + "_";

        // Security levels in use, from the sources.
        std::map<long, std::string> tiers;
        json sources = json::array();
        std::set<std::string> ids;
        bool tif_done = false;
        for (const auto& group: legacy.value("blocklist_sources", json::array())) {
            std::string base = group.value("url", std::string()), postfix = group.value("postfix", std::string());
            std::string prefix = group.value("name_prefix", std::string("list"));
            while (base.ends_with('/')) base.pop_back();
            for (const auto& source: group.value("sources", json::array())) {
                long level = source.value("security_level", 0L);
                std::string tier = std::format("level{}", level);
                tiers[level] = tier;
                std::string name = source.value("name", std::string());
                std::string url = std::format("{}/{}{}.txt", base, name, postfix);
                if (base.find("StevenBlack") != std::string::npos) url = base + "/" + name;   // the old tool appended .txt; the file has none
                std::string format = url.find("/adblock") != std::string::npos ? "adblock"
                                     : url.find("StevenBlack") != std::string::npos ? "hosts"
                                     : url.find("/adguard") != std::string::npos   ? "adblock"
                                                                                   : "hosts";
                json entry;
                if (url.find("hagezi") != std::string::npos && (name == "tif" || name == "tif.medium" || name == "tif.mini") && !tif_done) {
                    std::string root = base + "/";
                    entry = {{"id", "hagezi-tif"},
                             {"tier", tier},
                             {"format", "adblock"},
                             {"variants", {{"full", root + "tif.txt"}, {"medium", root + "tif.medium.txt"}, {"mini", root + "tif.mini.txt"}}},
                             {"variant", "auto"},
                             {"auto", {"medium", "mini"}}};
                    tif_done = true;
                } else {
                    std::string id = sanitize(prefix + "-" + name, 48, true);
                    std::string unique = id;
                    for (int n = 2; ids.contains(unique); ++n) unique = std::format("{}-{}", id, n);
                    entry = {{"id", unique}, {"tier", tier}, {"format", format}, {"url", url}};
                }
                ids.insert(entry["id"].get<std::string>());
                sources.push_back(std::move(entry));
            }
        }
        if (sources.empty()) throw LegacyError("blocklist_sources has no lists");

        json profiles = json::object();
        for (const auto& filter: legacy.value("forti_hole_automated_dns_filters", json::array())) {
            std::string name = sanitize(filter.value("dns_filter", std::string("profile")), 12);
            if (name.empty() || name == "all") name = "legacy";
            json profile_tiers = json::array();
            for (const auto& level: filter.value("filters", json::array())) {
                long l = level.value("security_level", 0L);
                std::string access = level.value("access", std::string("block"));
                if (!tiers.contains(l)) continue;
                if (access == "block") profile_tiers.push_back(tiers[l]);
                else out.notes.push_back(std::format("{}: security level {} was \"{}\"; forti-hole profiles only block, so it is left out", name, l, access));
            }
            if (profile_tiers.empty()) {
                out.notes.push_back(std::format("{}: blocks no security level; left out", name));
                continue;
            }
            profiles[name] = {{"tiers", profile_tiers}, {"comment", std::format("forti-hole 0.x DNS filter \"{}\"", filter.value("dns_filter", name))}};
            for (const auto& policy: filter.value("firewall_policies", json::array()))
                if (policy.is_string())
                    out.notes.push_back(std::format("the old tool set {}{} on policy \"{}\": `forti-hole attach --profile {} --policy <id>` does that now",
                                                    "fh-", name, policy.get<std::string>(), name));
        }
        if (profiles.empty()) throw LegacyError("forti_hole_automated_dns_filters blocks nothing");

        // Tiers no profile blocks would make the configuration invalid: drop their sources.
        std::set<std::string> blocked;
        for (const auto& [name, p]: profiles.items())
            for (const auto& t: p["tiers"]) blocked.insert(t.get<std::string>());
        json kept = json::array();
        for (auto& s: sources) {
            if (blocked.contains(s["tier"].get<std::string>())) kept.push_back(std::move(s));
            else out.notes.push_back(std::format("{}: its security level is blocked by no DNS filter; left out", s["id"].get<std::string>()));
        }
        json tier_list = json::array();
        for (const auto& [level, tier]: tiers)
            if (blocked.contains(tier)) tier_list.push_back(tier);

        json config = example_config();
        config["tiers"] = tier_list;
        config["sources"] = kept;
        config["profiles"] = profiles;
        config["comment"] = "migrated from forti-hole 0.x config.yaml";
        parse_config(config);
        out.config = std::move(config);
        out.notes.push_back("the old config's fortigate: section (API key, certificate) was not read into this file: use `forti configure`");
        return out;
    }

}  // namespace fh
