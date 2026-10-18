#include "guard.hpp"

#include "config.hpp"

#include <format>

namespace fh {

    using json = nlohmann::json;

    namespace {

        std::string percent_decode(std::string_view text) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            std::string out;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
                    out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
                    i += 2;
                } else out += text[i];
            }
            return out;
        }

        bool parse_id(std::string_view text, std::uint64_t& value) {
            if (text.empty() || text.size() > 10) return false;
            value = 0;
            for (char c: text) {
                if (c < '0' || c > '9') return false;
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
            }
            return true;
        }

        std::string key_text(const json& value) { return value.is_string() ? value.get<std::string>() : value.dump(); }

        bool marked(const json& object, const char* field) {
            auto it = object.find(field);
            return it != object.end() && it->is_string() && it->get<std::string>().find(marker) != std::string::npos;
        }

        std::string canonical(std::string_view resource) {
            if (resource.starts_with("cmdb/")) resource.remove_prefix(5);
            std::string out(resource);
            for (auto& c: out)
                if (c == '.') c = '/';   // "system.dns-server" style is not used by these paths, but be strict
            return out;
        }

        // The push body: every command names a prefixed feed.
        std::string check_push(const json& body, std::string_view prefix) {
            if (!body.is_object() || !body.contains("commands") || !body["commands"].is_array() || body["commands"].empty())
                return "a push must carry a non-empty \"commands\" list";
            for (const auto& command: body["commands"]) {
                if (!command.is_object()) return "a push command must be an object";
                auto name = command.value("name", std::string());
                if (!name.starts_with(prefix)) return std::format("a push may only name {}* feeds, not \"{}\"", prefix, name);
                auto verb = command.value("command", std::string());
                if (verb != "snapshot" && verb != "add" && verb != "remove") return std::format("push command \"{}\" is not allowed", verb);
            }
            return {};
        }

    }  // namespace

    void check_operations(const json& operations, std::string_view prefix, const WritePermits& permits) {
        if (!operations.is_array()) throw GuardViolation("operations must be a list");
        for (std::size_t i = 0; i < operations.size(); ++i) {
            const auto& op = operations[i];
            std::string where = std::format("operations[{}]", i);
            auto kind = op.value("op", std::string());
            auto resource = canonical(op.value("resource", std::string()));
            std::string key = op.contains("key") ? key_text(op["key"]) : std::string();
            const json object = op.value("object", json::object());
            const json set = op.value("set", json::object());
            if (op.contains("child") || op.contains("parent"))
                throw GuardViolation(where + ": forti-hole never writes single entries of a table attribute");

            if (kind == "action") {
                if (resource != "monitor/system/external-resource/dynamic")
                    throw GuardViolation(std::format("{}: monitor action {} is not allowed", where, resource));
                if (auto why = check_push(op.value("body", json()), prefix); !why.empty()) throw GuardViolation(where + ": " + why);
                continue;
            }
            if (kind != "create" && kind != "update" && kind != "delete") throw GuardViolation(std::format("{}: operation \"{}\"", where, kind));

            if (resource == "system/external-resource") {
                std::string name = kind == "create" ? object.value("name", std::string()) : key;
                if (kind == "delete" && permits.legacy_feeds.contains(name)) continue;
                if (!name.starts_with(prefix)) throw GuardViolation(std::format("{}: feed \"{}\" is not forti-hole's", where, name));
                if (kind == "create" && !marked(object, "comments")) throw GuardViolation(where + ": a new feed must carry the ownership marker");
                if (kind == "update" && set.contains("comments") && !marked(set, "comments"))
                    throw GuardViolation(where + ": an update must keep the ownership marker");
                if (kind == "update" && set.contains("name")) throw GuardViolation(where + ": feeds are never renamed");
                continue;
            }
            if (resource == "dnsfilter/profile") {
                std::string name = kind == "create" ? object.value("name", std::string()) : key;
                bool owned = name.starts_with(prefix);
                if (!owned && !(kind == "update" && permits.profiles.contains(name)))
                    throw GuardViolation(std::format("{}: DNS filter profile \"{}\" is not forti-hole's", where, name));
                if (kind == "create" && !marked(object, "comment")) throw GuardViolation(where + ": a new profile must carry the ownership marker");
                if (!owned) {
                    // An adopted operator profile: only its FortiGuard filter list.
                    for (const auto& [field, value]: set.items())
                        if (field != "ftgd-dns") throw GuardViolation(std::format("{}: only ftgd-dns may change on an operator profile", where));
                    if (op.contains("clear") && !op["clear"].empty()) throw GuardViolation(where + ": nothing may be cleared on an operator profile");
                }
                if (kind == "update" && set.contains("name")) throw GuardViolation(where + ": profiles are never renamed");
                continue;
            }
            if (resource == "dnsfilter/domain-filter") {
                if (kind == "create") {
                    if (!object.value("name", std::string()).starts_with(prefix) || !marked(object, "comment"))
                        throw GuardViolation(where + ": a new domain filter must be named " + std::string(prefix) + "* and carry the marker");
                    continue;
                }
                std::uint64_t id = 0;
                if (!parse_id(key, id) || !permits.domain_filters.contains(id))
                    throw GuardViolation(std::format("{}: domain filter {} is not forti-hole's", where, key));
                continue;
            }
            if (resource == "firewall/policy") {
                std::uint64_t id = 0;
                if (kind != "update" || !parse_id(key, id) || !permits.policies.contains(id))
                    throw GuardViolation(std::format("{}: firewall policy {} may change only through attach/detach", where, key));
                for (const auto& [field, value]: set.items())
                    if (field != "dnsfilter-profile" && field != "utm-status")
                        throw GuardViolation(std::format("{}: only dnsfilter-profile and utm-status may change on a policy", where));
                if (op.contains("clear"))
                    for (const auto& field: op["clear"])
                        if (field != "dnsfilter-profile") throw GuardViolation(where + ": only dnsfilter-profile may be cleared on a policy");
                continue;
            }
            throw GuardViolation(std::format("{}: forti-hole never writes {}", where, resource));
        }
    }

    std::string refuse_write(fortiapi::Method method, std::string_view url, std::string_view body, std::string_view prefix,
                             const WritePermits& permits) {
        if (method == fortiapi::Method::Get) return {};
        auto at = url.find("/api/v2/");
        if (at == std::string_view::npos) return "not a FortiOS API request";
        std::string_view path = url.substr(at + 8);
        if (auto query = path.find('?'); query != std::string_view::npos) path = path.substr(0, query);

        auto json_body = [&]() { return json::parse(body, nullptr, false); };
        auto under = [&](std::string_view collection, std::string& key) {
            if (path == collection) {
                key.clear();
                return true;
            }
            if (path.size() > collection.size() && path.starts_with(collection) && path[collection.size()] == '/') {
                key = percent_decode(path.substr(collection.size() + 1));
                return key.find('/') == std::string::npos;
            }
            return false;
        };

        std::string key;
        if (path == "monitor/system/external-resource/dynamic") {
            if (method != fortiapi::Method::Post) return "only POST to the push endpoint";
            auto why = check_push(json_body(), prefix);
            return why;
        }
        if (under("cmdb/system/external-resource", key)) {
            if (key.empty()) {
                if (method != fortiapi::Method::Post) return "only POST to the feed collection";
                auto b = json_body();
                if (!b.is_object() || !b.value("name", std::string()).starts_with(prefix) || !marked(b, "comments"))
                    return std::format("a new feed must be named {}* and carry the marker", prefix);
                return {};
            }
            if (key.starts_with(prefix)) return {};
            if (method == fortiapi::Method::Delete && permits.legacy_feeds.contains(key)) return {};
            return std::format("feed \"{}\" is not forti-hole's", key);
        }
        if (under("cmdb/dnsfilter/profile", key)) {
            if (key.empty()) {
                if (method != fortiapi::Method::Post) return "only POST to the profile collection";
                auto b = json_body();
                if (!b.is_object() || !b.value("name", std::string()).starts_with(prefix) || !marked(b, "comment"))
                    return std::format("a new DNS filter profile must be named {}* and carry the marker", prefix);
                return {};
            }
            if (key.starts_with(prefix)) return {};
            if (method == fortiapi::Method::Put && permits.profiles.contains(key)) {
                auto b = json_body();
                if (!b.is_object()) return "unreadable profile update";
                for (const auto& [field, value]: b.items())
                    if (field != "ftgd-dns") return "only ftgd-dns may change on an operator profile";
                return {};
            }
            return std::format("DNS filter profile \"{}\" is not forti-hole's", key);
        }
        if (under("cmdb/dnsfilter/domain-filter", key)) {
            if (key.empty()) {
                if (method != fortiapi::Method::Post) return "only POST to the domain-filter collection";
                auto b = json_body();
                if (!b.is_object() || !b.value("name", std::string()).starts_with(prefix) || !marked(b, "comment"))
                    return std::format("a new domain filter must be named {}* and carry the marker", prefix);
                return {};
            }
            std::uint64_t id = 0;
            if (parse_id(key, id) && permits.domain_filters.contains(id)) return {};
            return std::format("domain filter {} is not forti-hole's", key);
        }
        if (under("cmdb/firewall/policy", key)) {
            std::uint64_t id = 0;
            if (method == fortiapi::Method::Put && parse_id(key, id) && permits.policies.contains(id)) {
                auto b = json_body();
                if (!b.is_object()) return "unreadable policy update";
                for (const auto& [field, value]: b.items())
                    if (field != "dnsfilter-profile" && field != "utm-status") return "only dnsfilter-profile and utm-status may change on a policy";
                return {};
            }
            return std::format("firewall policy {} may change only through attach/detach", key);
        }
        return std::format("forti-hole never writes {}", path);
    }

    fortiapi::HttpResponse GuardTransport::perform(const fortiapi::HttpRequest& request) {
        if (auto why = refuse_write(request.method, request.url, request.body, prefix_, *permits_); !why.empty())
            throw GuardViolation(std::format("refused before sending {} {}: {}", fortiapi::to_string(request.method),
                                             request.url.substr(0, request.url.find('?')), why));
        return inner_->perform(request);
    }

}  // namespace fh
