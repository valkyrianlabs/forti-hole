#include "config.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

namespace fh {

    namespace {

        using json = nlohmann::json;

        [[noreturn]] void fail(std::string_view where, std::string_view message) {
            throw ConfigError(std::format("{}: {}", where, message));
        }

        void allow_keys(const json& object, std::initializer_list<std::string_view> keys, std::string_view where) {
            if (!object.is_object()) fail(where, "must be an object");
            for (const auto& [key, value]: object.items())
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) fail(where, std::format("unknown key \"{}\"", key));
        }

        std::string string_at(const json& object, std::string_view key, std::string_view where, std::string fallback = {}) {
            auto it = object.find(key);
            if (it == object.end() || it->is_null()) return fallback;
            if (!it->is_string()) fail(where, std::format("\"{}\" must be a string", key));
            return it->get<std::string>();
        }

        bool bool_at(const json& object, std::string_view key, std::string_view where, bool fallback) {
            auto it = object.find(key);
            if (it == object.end() || it->is_null()) return fallback;
            if (!it->is_boolean()) fail(where, std::format("\"{}\" must be true or false", key));
            return it->get<bool>();
        }

        double number_at(const json& object, std::string_view key, std::string_view where, double fallback, double min, double max) {
            auto it = object.find(key);
            if (it == object.end() || it->is_null()) return fallback;
            if (!it->is_number()) fail(where, std::format("\"{}\" must be a number", key));
            double value = it->get<double>();
            if (value < min || value > max) fail(where, std::format("\"{}\" must be between {:g} and {:g}", key, min, max));
            return value;
        }

        std::vector<std::string> strings_at(const json& object, std::string_view key, std::string_view where) {
            std::vector<std::string> out;
            auto it = object.find(key);
            if (it == object.end() || it->is_null()) return out;
            if (!it->is_array()) fail(where, std::format("\"{}\" must be a list of strings", key));
            for (const auto& item: *it) {
                if (!item.is_string()) fail(where, std::format("\"{}\" must be a list of strings", key));
                out.push_back(item.get<std::string>());
            }
            return out;
        }

        bool matches(std::string_view text, std::size_t min, std::size_t max, bool allow_dots) {
            if (text.size() < min || text.size() > max) return false;
            if (!((text.front() >= 'a' && text.front() <= 'z') || (text.front() >= '0' && text.front() <= '9'))) return false;
            for (char c: text) {
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (allow_dots && c == '.');
                if (!ok) return false;
            }
            return true;
        }

        bool url_ok(std::string_view url) { return url.starts_with("https://") || url.starts_with("file:///"); }

        Scope scope_at(const json& object, std::string_view where) {
            auto text = string_at(object, "scope", where, "exact");
            if (text == "exact") return Scope::exact;
            if (text == "subtree") return Scope::subtree;
            fail(where, "\"scope\" must be \"exact\" or \"subtree\"");
        }

        json scope_json(Scope scope) { return std::string(to_string(scope)); }

    }  // namespace

    std::vector<std::string> Source::candidates() const {
        if (!has_variants()) return {""};
        if (variant == "auto") return auto_order;
        return {variant};
    }

    std::string Source::url_for(std::string_view variant_name) const {
        if (!has_variants()) return url;
        auto it = variants.find(variant_name);
        return it == variants.end() ? std::string() : it->second;
    }

    Config parse_config(const json& j) {
        allow_keys(j, {"format", "enabled", "profile", "vdom", "prefix", "tiers", "sources", "profiles", "allow", "allow_regex", "block",
                       "encoding", "reference", "budget", "feeds", "sync", "fetch", "dnsfilter", "comment"},
                   "config");
        if (auto it = j.find("format"); it != j.end() && *it != 1) fail("config", "unsupported \"format\" (this forti-hole reads format 1)");
        Config c;
        c.enabled = bool_at(j, "enabled", "config", false);
        c.profile = string_at(j, "profile", "config");
        c.vdom = string_at(j, "vdom", "config");
        c.prefix = string_at(j, "prefix", "config", "fh-");
        if (!matches(c.prefix, 2, 8, false) || c.prefix.back() != '-') fail("prefix", "2-8 characters of [a-z0-9_-], ending in '-'");

        c.tiers = strings_at(j, "tiers", "config");
        if (c.tiers.empty()) fail("tiers", "at least one tier");
        if (c.tiers.size() > max_tiers) fail("tiers", std::format("at most {}", max_tiers));
        std::set<std::string, std::less<>> tiers;
        for (const auto& tier: c.tiers) {
            if (!matches(tier, 1, 24, false)) fail("tiers", std::format("\"{}\": 1-24 characters of [a-z0-9_-]", tier));
            if (!tiers.insert(tier).second) fail("tiers", std::format("\"{}\" twice", tier));
        }
        auto check_tier = [&](const std::string& tier, std::string_view where) {
            if (!tiers.contains(tier)) fail(where, std::format("unknown tier \"{}\"", tier));
        };

        auto sources = j.find("sources");
        if (sources == j.end() || !sources->is_array() || sources->empty()) fail("sources", "a non-empty list");
        std::set<std::string, std::less<>> ids;
        for (std::size_t i = 0; i < sources->size(); ++i) {
            const auto& s = (*sources)[i];
            std::string where = std::format("sources[{}]", i);
            allow_keys(s, {"id", "tier", "format", "url", "variants", "variant", "auto", "enabled", "comment"}, where);
            Source source;
            source.id = string_at(s, "id", where);
            if (!matches(source.id, 1, 48, true)) fail(where, "\"id\": 1-48 characters of [a-z0-9._-]");
            if (!ids.insert(source.id).second) fail(where, std::format("id \"{}\" twice", source.id));
            source.tier = string_at(s, "tier", where);
            check_tier(source.tier, where);
            auto format = parse_format(string_at(s, "format", where, "adblock"));
            if (!format || *format == Format::tranco) fail(where, "\"format\": adblock, hosts, domains, wildcard or subtree-domains");
            source.format = *format;
            source.url = string_at(s, "url", where);
            source.enabled = bool_at(s, "enabled", where, true);
            if (auto v = s.find("variants"); v != s.end()) {
                if (!v->is_object() || v->empty()) fail(where, "\"variants\": an object of name -> url");
                for (const auto& [name, url]: v->items()) {
                    if (!matches(name, 1, 16, false) || name == "auto") fail(where, std::format("variant name \"{}\"", name));
                    if (!url.is_string() || !url_ok(url.get<std::string>())) fail(where, std::format("variant \"{}\": an https:// or file:/// URL", name));
                    source.variants[name] = url.get<std::string>();
                }
                if (!source.url.empty()) fail(where, "\"url\" and \"variants\" are exclusive");
                source.variant = string_at(s, "variant", where, "auto");
                source.auto_order = strings_at(s, "auto", where);
                if (source.auto_order.empty())
                    for (const auto& [name, url]: source.variants) source.auto_order.push_back(name);
                for (const auto& name: source.auto_order)
                    if (!source.variants.contains(name)) fail(where, std::format("\"auto\" names unknown variant \"{}\"", name));
                if (source.variant != "auto" && !source.variants.contains(source.variant))
                    fail(where, std::format("\"variant\": \"auto\" or one of the variants, not \"{}\"", source.variant));
            } else {
                if (!url_ok(source.url)) fail(where, "\"url\": an https:// or file:/// URL");
                if (s.contains("variant") || s.contains("auto")) fail(where, "\"variant\" and \"auto\" need \"variants\"");
            }
            c.sources.push_back(std::move(source));
        }

        auto profiles = j.find("profiles");
        if (profiles == j.end() || !profiles->is_object() || profiles->empty()) fail("profiles", "an object of name -> {\"tiers\": [...]}");
        if (profiles->size() > max_profiles) fail("profiles", std::format("at most {}", max_profiles));
        for (const auto& [name, p]: profiles->items()) {
            std::string where = std::format("profiles.{}", name);
            if (!matches(name, 1, 12, false) || name.find('_') != std::string::npos || name == "all")
                fail(where, "profile names are 1-12 characters of [a-z0-9-], not \"all\"");
            allow_keys(p, {"tiers", "comment"}, where);
            ProfileSpec spec{name, strings_at(p, "tiers", where)};
            if (spec.tiers.empty()) fail(where, "\"tiers\": at least one");
            for (const auto& tier: spec.tiers) check_tier(tier, where);
            c.profiles.push_back(std::move(spec));
        }
        std::set<std::string, std::less<>> profile_names;
        for (const auto& p: c.profiles) profile_names.insert(p.name);
        auto check_profiles = [&](const std::vector<std::string>& names, std::string_view where) {
            for (const auto& name: names)
                if (!profile_names.contains(name)) fail(where, std::format("unknown profile \"{}\"", name));
        };

        if (auto a = j.find("allow"); a != j.end()) {
            if (!a->is_array()) fail("allow", "a list");
            for (std::size_t i = 0; i < a->size(); ++i) {
                std::string where = std::format("allow[{}]", i);
                allow_keys((*a)[i], {"name", "scope", "profiles", "comment"}, where);
                AllowEntry entry{string_at((*a)[i], "name", where), scope_at((*a)[i], where), strings_at((*a)[i], "profiles", where),
                                 string_at((*a)[i], "comment", where)};
                if (entry.name.empty()) fail(where, "\"name\" is required");
                check_profiles(entry.profiles, where);
                c.allow.push_back(std::move(entry));
            }
        }
        if (auto a = j.find("allow_regex"); a != j.end()) {
            if (!a->is_array()) fail("allow_regex", "a list");
            for (std::size_t i = 0; i < a->size(); ++i) {
                std::string where = std::format("allow_regex[{}]", i);
                allow_keys((*a)[i], {"pattern", "profiles", "comment"}, where);
                AllowRegex entry{string_at((*a)[i], "pattern", where), strings_at((*a)[i], "profiles", where), string_at((*a)[i], "comment", where)};
                if (entry.pattern.empty()) fail(where, "\"pattern\" is required");
                check_profiles(entry.profiles, where);
                c.allow_regex.push_back(std::move(entry));
            }
        }
        if (auto b = j.find("block"); b != j.end()) {
            if (!b->is_array()) fail("block", "a list");
            for (std::size_t i = 0; i < b->size(); ++i) {
                std::string where = std::format("block[{}]", i);
                allow_keys((*b)[i], {"name", "scope", "tier", "comment"}, where);
                BlockEntry entry{string_at((*b)[i], "name", where), scope_at((*b)[i], where), string_at((*b)[i], "tier", where, c.tiers.front()),
                                 string_at((*b)[i], "comment", where)};
                if (entry.name.empty()) fail(where, "\"name\" is required");
                check_tier(entry.tier, where);
                c.block.push_back(std::move(entry));
            }
        }

        auto encoding = parse_encoding(string_at(j, "encoding", "config", "strict"));
        if (!encoding) fail("encoding", "\"strict\" or \"compact\"");
        c.encoding = *encoding;
        if (auto r = j.find("reference"); r != j.end() && !r->is_null()) {
            allow_keys(*r, {"url", "format", "comment"}, "reference");
            c.reference.url = string_at(*r, "url", "reference");
            if (!c.reference.url.empty() && !url_ok(c.reference.url)) fail("reference", "\"url\": an https:// or file:/// URL");
            auto format = parse_format(string_at(*r, "format", "reference", "tranco"));
            if (!format) fail("reference", "unknown \"format\"");
            c.reference.format = *format;
        }
        if (c.encoding == Encoding::compact && c.reference.url.empty())
            fail("encoding", "compact needs \"reference\": a list of real names to check over-matching against");

        if (auto b = j.find("budget"); b != j.end()) {
            allow_keys(*b, {"fallback_limit", "entry_headroom_percent", "memory_headroom_percent", "bytes_per_line", "on_overrun", "comment"},
                       "budget");
            c.budget.fallback_limit = static_cast<std::uint64_t>(number_at(*b, "fallback_limit", "budget", 1000000, 1000, 100000000));
            c.budget.entry_headroom_percent = number_at(*b, "entry_headroom_percent", "budget", 10, 0, 90);
            c.budget.memory_headroom_percent = number_at(*b, "memory_headroom_percent", "budget", 15, 5, 90);
            if (b->contains("bytes_per_line") && !(*b)["bytes_per_line"].is_null())
                c.budget.bytes_per_line = number_at(*b, "bytes_per_line", "budget", 0, 1, 1e6);
            auto overrun = string_at(*b, "on_overrun", "budget", "refuse");
            if (overrun != "refuse" && overrun != "warn") fail("budget", "\"on_overrun\": \"refuse\" or \"warn\"");
            c.budget.refuse = overrun == "refuse";
        }
        if (auto f = j.find("feeds"); f != j.end()) {
            allow_keys(*f, {"max_entries", "category_min", "category_max", "comment"}, "feeds");
            c.feeds.max_entries = static_cast<std::size_t>(number_at(*f, "max_entries", "feeds", 131072, 1000, 5000000));
            c.feeds.category_min = static_cast<int>(number_at(*f, "category_min", "feeds", 192, 192, 221));
            c.feeds.category_max = static_cast<int>(number_at(*f, "category_max", "feeds", 221, 192, 221));
            if (c.feeds.category_min > c.feeds.category_max) fail("feeds", "category_min must not exceed category_max");
        }
        if (auto s = j.find("sync"); s != j.end()) {
            allow_keys(*s, {"max_shrink_percent", "chunk_entries", "load_timeout_seconds", "verify_samples", "verify_page", "comment"}, "sync");
            c.sync.max_shrink_percent = number_at(*s, "max_shrink_percent", "sync", 20, 0, 100);
            c.sync.chunk_entries = static_cast<std::size_t>(number_at(*s, "chunk_entries", "sync", 20000, 100, 200000));
            c.sync.load_timeout_seconds = static_cast<int>(number_at(*s, "load_timeout_seconds", "sync", 120, 5, 3600));
            c.sync.verify_samples = static_cast<std::size_t>(number_at(*s, "verify_samples", "sync", 3, 0, 1000));
            c.sync.verify_page = static_cast<std::size_t>(number_at(*s, "verify_page", "sync", 500, 1, 10000));
        }
        if (auto f = j.find("fetch"); f != j.end()) {
            allow_keys(*f, {"timeout_seconds", "max_bytes", "parallel", "ca_bundle", "comment"}, "fetch");
            c.fetch.timeout_seconds = static_cast<int>(number_at(*f, "timeout_seconds", "fetch", 120, 5, 3600));
            c.fetch.max_bytes = static_cast<std::uint64_t>(number_at(*f, "max_bytes", "fetch", 128.0 * (1 << 20), 1 << 16, 4.0 * (1u << 30)));
            c.fetch.parallel = static_cast<std::size_t>(number_at(*f, "parallel", "fetch", 6, 1, 32));
            c.fetch.ca_bundle = string_at(*f, "ca_bundle", "fetch");
        }
        if (auto d = j.find("dnsfilter"); d != j.end()) {
            allow_keys(*d, {"manage_profiles", "block_action", "error_allow", "log", "comment"}, "dnsfilter");
            c.dnsfilter.manage_profiles = bool_at(*d, "manage_profiles", "dnsfilter", true);
            c.dnsfilter.block_action = string_at(*d, "block_action", "dnsfilter", "block");
            if (c.dnsfilter.block_action != "block" && c.dnsfilter.block_action != "redirect" && c.dnsfilter.block_action != "block-sevrfail")
                fail("dnsfilter", "\"block_action\": block, redirect or block-sevrfail");
            c.dnsfilter.error_allow = bool_at(*d, "error_allow", "dnsfilter", true);
            c.dnsfilter.log = bool_at(*d, "log", "dnsfilter", true);
        }

        // Device names must fit FortiOS's 35-character limit: <prefix><group>-<n>, <prefix><profile>.
        for (const auto& p: c.profiles)
            if (c.prefix.size() + p.name.size() > 35) fail("profiles", std::format("{}{} is longer than 35 characters", c.prefix, p.name));
        for (const auto& tier: c.tiers) {
            bool used = std::any_of(c.profiles.begin(), c.profiles.end(), [&](const ProfileSpec& p) {
                return std::find(p.tiers.begin(), p.tiers.end(), tier) != p.tiers.end();
            });
            bool listed = std::any_of(c.sources.begin(), c.sources.end(), [&](const Source& s) { return s.tier == tier; });
            if (listed && !used) fail("tiers", std::format("tier \"{}\" has sources but no profile blocks it", tier));
        }
        return c;
    }

    Config load_config(const std::filesystem::path& path) {
        std::ifstream in(path);
        if (!in) throw ConfigError(std::format("cannot read {} (run `forti-hole setup`)", path.string()));
        std::stringstream text;
        text << in.rdbuf();
        json j;
        try {
            j = json::parse(text.str());
        } catch (const json::parse_error& error) {
            throw ConfigError(std::format("{}: not valid JSON: {}", path.string(), error.what()));
        }
        Config c = parse_config(j);
        c.path = path;
        return c;
    }

    json to_json(const Config& c) {
        json j = {{"format", 1}, {"enabled", c.enabled}, {"profile", c.profile}, {"prefix", c.prefix}, {"tiers", c.tiers}};
        if (!c.vdom.empty()) j["vdom"] = c.vdom;
        j["sources"] = json::array();
        for (const auto& s: c.sources) {
            json source = {{"id", s.id}, {"tier", s.tier}, {"format", std::string(to_string(s.format))}};
            if (s.has_variants()) {
                source["variants"] = s.variants;
                source["variant"] = s.variant;
                source["auto"] = s.auto_order;
            } else {
                source["url"] = s.url;
            }
            if (!s.enabled) source["enabled"] = false;
            j["sources"].push_back(std::move(source));
        }
        j["profiles"] = json::object();
        for (const auto& p: c.profiles) j["profiles"][p.name] = {{"tiers", p.tiers}};
        if (!c.allow.empty()) {
            j["allow"] = json::array();
            for (const auto& a: c.allow) {
                json entry = {{"name", a.name}, {"scope", scope_json(a.scope)}};
                if (!a.profiles.empty()) entry["profiles"] = a.profiles;
                if (!a.comment.empty()) entry["comment"] = a.comment;
                j["allow"].push_back(std::move(entry));
            }
        }
        if (!c.allow_regex.empty()) {
            j["allow_regex"] = json::array();
            for (const auto& a: c.allow_regex) {
                json entry = {{"pattern", a.pattern}};
                if (!a.profiles.empty()) entry["profiles"] = a.profiles;
                if (!a.comment.empty()) entry["comment"] = a.comment;
                j["allow_regex"].push_back(std::move(entry));
            }
        }
        if (!c.block.empty()) {
            j["block"] = json::array();
            for (const auto& b: c.block) {
                json entry = {{"name", b.name}, {"scope", scope_json(b.scope)}, {"tier", b.tier}};
                if (!b.comment.empty()) entry["comment"] = b.comment;
                j["block"].push_back(std::move(entry));
            }
        }
        j["encoding"] = std::string(to_string(c.encoding));
        if (!c.reference.url.empty()) j["reference"] = {{"url", c.reference.url}, {"format", std::string(to_string(c.reference.format))}};
        j["budget"] = {{"fallback_limit", c.budget.fallback_limit},
                       {"entry_headroom_percent", c.budget.entry_headroom_percent},
                       {"memory_headroom_percent", c.budget.memory_headroom_percent},
                       {"on_overrun", c.budget.refuse ? "refuse" : "warn"}};
        if (c.budget.bytes_per_line) j["budget"]["bytes_per_line"] = *c.budget.bytes_per_line;
        j["feeds"] = {{"max_entries", c.feeds.max_entries}, {"category_min", c.feeds.category_min}, {"category_max", c.feeds.category_max}};
        j["sync"] = {{"max_shrink_percent", c.sync.max_shrink_percent},
                     {"chunk_entries", c.sync.chunk_entries},
                     {"load_timeout_seconds", c.sync.load_timeout_seconds},
                     {"verify_samples", c.sync.verify_samples},
                     {"verify_page", c.sync.verify_page}};
        j["fetch"] = {{"timeout_seconds", c.fetch.timeout_seconds}, {"max_bytes", c.fetch.max_bytes}, {"parallel", c.fetch.parallel}};
        if (!c.fetch.ca_bundle.empty()) j["fetch"]["ca_bundle"] = c.fetch.ca_bundle;
        j["dnsfilter"] = {{"manage_profiles", c.dnsfilter.manage_profiles},
                          {"block_action", c.dnsfilter.block_action},
                          {"error_allow", c.dnsfilter.error_allow},
                          {"log", c.dnsfilter.log}};
        return j;
    }

    json example_config() {
        constexpr std::string_view hagezi = "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/";
        auto list = [&](std::string_view name) { return std::format("{}{}.txt", hagezi, name); };
        json j = {
                {"format", 1},
                {"enabled", false},
                {"profile", ""},
                {"prefix", "fh-"},
                {"tiers", {"base", "plus", "ultimate", "threat"}},
                {"sources",
                 {{{"id", "stevenblack"}, {"tier", "base"}, {"format", "hosts"}, {"url", "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"}},
                  {{"id", "hagezi-pro"}, {"tier", "base"}, {"format", "adblock"}, {"url", list("pro")}},
                  {{"id", "hagezi-pro-plus"}, {"tier", "plus"}, {"format", "adblock"}, {"url", list("pro.plus")}},
                  {{"id", "hagezi-ultimate"}, {"tier", "ultimate"}, {"format", "adblock"}, {"url", list("ultimate")}},
                  {{"id", "hagezi-tif"},
                   {"tier", "threat"},
                   {"format", "adblock"},
                   {"variants", {{"full", list("tif")}, {"medium", list("tif.medium")}, {"mini", list("tif.mini")}}},
                   {"variant", "auto"},
                   {"auto", {"medium", "mini"}}}}},
                {"profiles",
                 {{"light", {{"tiers", {"base", "threat"}}}},
                  {"standard", {{"tiers", {"base", "plus", "threat"}}}},
                  {"strict", {{"tiers", {"base", "plus", "ultimate", "threat"}}}}}},
                {"encoding", "strict"},
                {"budget", {{"entry_headroom_percent", 10}, {"memory_headroom_percent", 15}, {"on_overrun", "refuse"}}},
                {"dnsfilter", {{"manage_profiles", true}, {"block_action", "block"}, {"error_allow", true}, {"log", true}}},
        };
        return j;
    }

    std::string dnsfilter_profile_name(const Config& config, std::string_view profile) { return config.prefix + std::string(profile); }

    CompileOptions compile_options(const Config& config) {
        CompileOptions options;
        options.tiers = config.tiers;
        options.profiles = config.profiles;
        options.encoding = config.encoding;
        options.max_entries_per_feed = config.feeds.max_entries;
        options.prefix = config.prefix;
        return options;
    }

}  // namespace fh
