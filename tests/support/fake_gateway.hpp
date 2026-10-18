//
// An in-memory FortiGate behind the Gateway interface, with the push-feed behaviour measured on a
// FortiGate-60F (FortiOS 7.4.8):
//   - a new feed is "Connect pending" for a few status reads; a push before that is dropped
//     silently (the reply still says success);
//   - snapshot replaces, add/remove change; exact duplicates are kept once;
//   - last_content_update_time advances with every applied push;
//   - one device-wide entry limit; entries past it are not accepted and the feed reports overflow;
//   - used memory grows by bytes_per_line per accepted entry;
//   - deleting a feed whose category a DNS filter profile still blocks (with no other feed of that
//     category) fails, as FortiOS refuses it.
// Plans and applies go through forti-hole's own operation check (apply_operations), as with the
// engine; here they execute directly against these tables.
//

#ifndef FORTI_HOLE_TESTS_FAKE_GATEWAY_HPP
#define FORTI_HOLE_TESTS_FAKE_GATEWAY_HPP

#include "gateway.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fh::test {

    using json = nlohmann::json;

    struct FakeFeed {
        json object;                       // cmdb attributes
        std::vector<std::string> entries;  // insertion order, unique
        int pending_reads = 2;             // status reads before it is loaded
        std::int64_t last_update = 0;
        bool overflow = false;
    };

    class FakeGateway final : public Gateway {
    public:
        std::map<std::string, FakeFeed, std::less<>> feeds;
        std::map<std::string, json, std::less<>> profiles;        // dnsfilter/profile
        std::map<std::uint64_t, json> domain_filters;
        std::map<std::uint64_t, json> policies;
        std::uint64_t entry_limit = 1000000;
        std::uint64_t memory_total = 2'000'000'000, memory_base = 1'000'000'000;
        double bytes_per_line = 150;
        int red_threshold = 88;
        std::string serial = "FGT60FTEST000001", version = "v7.4.8";
        std::int64_t clock = 1000;

        // Fault injection.
        int fail_push_after = -1;          // fail the n-th push request (0-based) of the next apply
        bool drop_pushes = false;          // accept pushes but change nothing (a device that lost them)
        // FortiOS applies a push asynchronously and refuses another push to the same feed until it has
        // ("Unable to unlock external resource push file"). Here a push "applies" once its feed's
        // status has been read after it.
        std::set<std::string, std::less<>> applying;
        std::vector<json> applied;         // every operation applied, in order
        std::size_t plans = 0, applies = 0, push_requests = 0, writes = 0;

        std::uint64_t total_entries() const {
            std::uint64_t n = 0;
            for (const auto& [name, f]: feeds) n += f.entries.size();
            return n;
        }

        json read(const json& request) override {
            std::string resource = request.value("resource", std::string());
            if (resource == "system/external-resource") {
                json out = json::array();
                for (const auto& [name, f]: feeds) out.push_back(f.object);
                return page(out, request);
            }
            if (resource == "dnsfilter/profile") {
                if (request.contains("key")) return one(profiles, request["key"].get<std::string>());
                json out = json::array();
                for (const auto& [name, p]: profiles) out.push_back(p);
                return page(out, request);
            }
            if (resource == "dnsfilter/domain-filter") {
                json out = json::array();
                for (const auto& [id, d]: domain_filters) out.push_back(d);
                return page(out, request);
            }
            if (resource == "firewall/policy") {
                if (request.contains("key")) {
                    auto id = request["key"].is_number() ? request["key"].get<std::uint64_t>() : std::stoull(request["key"].get<std::string>());
                    auto it = policies.find(id);
                    if (it == policies.end()) return error("not_found", "no such policy");
                    return ok(it->second);
                }
                json out = json::array();
                for (const auto& [id, p]: policies) out.push_back(p);
                return page(out, request);
            }
            if (resource == "system/dns-server") return page(json::array(), request);
            if (resource == "monitor/system/performance/status")
                return ok({{"mem", {{"total", memory_total}, {"used", used()}, {"free", memory_total - used()}, {"freeable", 0}}}});
            if (resource == "system/global") return ok({{"memory-use-threshold-red", red_threshold}});
            if (resource == "monitor/system/resource/usage")
                return ok({{"mem", json::array({{{"current", 50}, {"historical", {{"24-hour", {{"max", 60}}}}}}})}});
            if (resource == "monitor/license/status") return ok({{"web_filtering", {{"status", "expired"}}}});
            return error("unknown_resource", "fake: " + resource);
        }

        json discover(const json& request) override {
            if (request.value("what", std::string()) == "capabilities")
                return ok({{"device", {{"serial", serial}, {"version", version}, {"hostname", "FortiGate-60F"}, {"model", "FGT60F"}}}, {"mode", "autonomous"}});
            return error("usage", "fake discover");
        }

        json plan(const json& request) override {
            ++plans;
            std::string id = std::format("20261018-000000-{:06x}", plans);
            stored_[id] = request.value("operations", json::array());
            return ok({{"plan_id", id}});
        }

        json apply(const json& request) override {
            ++applies;
            auto it = stored_.find(request.value("plan_id", std::string()));
            if (it == stored_.end()) return error("not_found", "no such plan");
            json ops = it->second;
            stored_.erase(it);
            int push_index = 0;
            json records = json::array();
            for (std::size_t i = 0; i < ops.size(); ++i) {
                const auto& op = ops[i];
                if (op.value("op", "") == "action") {
                    for (const auto& command: op["body"]["commands"])
                        if (applying.contains(command.value("name", std::string())))
                            return error("partial_failure", std::format("operation {} failed: Unable to unlock external resourch push file.", i),
                                         {{"operations", records}});
                    if (fail_push_after >= 0 && push_index == fail_push_after) {
                        fail_push_after = -1;
                        return error("partial_failure", std::format("fake: push {} failed", i), {{"operations", records}});
                    }
                    ++push_index;
                }
                if (auto why = execute(op); !why.empty()) return error("partial_failure", why, {{"operations", records}});
                applied.push_back(op);
                ++writes;
                records.push_back({{"index", i}, {"status", op.value("op", "") == "action" ? "applied_unverified" : "verified"}});
            }
            return ok({{"plan_id", request["plan_id"]}, {"operations", records}});
        }

        FeedStatus feed_status(std::string_view name) override {
            auto it = feeds.find(name);
            if (it == feeds.end()) throw GatewayError({{"code", "api"}, {"message", "no such feed"}});
            auto& f = it->second;
            applying.erase(std::string(name));
            FeedStatus s;
            if (f.pending_reads > 0) {
                --f.pending_reads;
                s.error = "Connect pending";
                return s;
            }
            s.loaded = true;
            s.file_status = f.last_update ? "downloaded" : "unavailable";
            s.accepted = s.valid = f.entries.size();
            s.limit = entry_limit;
            s.overflow = f.overflow;
            s.last_update = f.last_update;
            return s;
        }

        std::vector<FeedEntry> feed_entries(std::string_view name, std::size_t start, std::size_t count) override {
            auto it = feeds.find(name);
            if (it == feeds.end()) throw GatewayError({{"code", "api"}, {"message", "no such feed"}});
            std::vector<FeedEntry> out;
            for (std::size_t i = start; i < it->second.entries.size() && out.size() < count; ++i) out.push_back({it->second.entries[i], true});
            return out;
        }

        std::uint64_t used() const { return memory_base + static_cast<std::uint64_t>(bytes_per_line * static_cast<double>(total_entries())); }

        // Makes every feed loaded now (tests that do not exercise the load delay).
        void load_all() {
            for (auto& [name, f]: feeds) f.pending_reads = 0;
        }

    private:
        std::map<std::string, json, std::less<>> stored_;

        static json ok(json data, json meta = json::object()) { return {{"ok", true}, {"data", std::move(data)}, {"meta", std::move(meta)}}; }
        static json error(std::string code, std::string message, json details = json::object()) {
            return {{"ok", false}, {"error", {{"code", std::move(code)}, {"message", std::move(message)}, {"details", std::move(details)}}}};
        }

        static json page(const json& all, const json& request) {
            std::size_t start = request.value("start", std::size_t(0)), count = request.value("count", std::size_t(50));
            json out = json::array();
            for (std::size_t i = start; i < all.size() && out.size() < count; ++i) out.push_back(all[i]);
            json meta = json::object();
            if (start + out.size() < all.size()) meta["next_start"] = start + out.size();
            return ok(out, meta);
        }

        template<typename Map>
        static json one(const Map& map, const std::string& key) {
            auto it = map.find(key);
            if (it == map.end()) return error("not_found", "not found");
            return ok(it->second);
        }

        bool category_blocked(int category) const {
            for (const auto& [name, p]: profiles)
                for (const auto& f: p.value("ftgd-dns", json::object()).value("filters", json::array()))
                    if (f.value("category", 0) == category) return true;
            return false;
        }

        std::string execute(const json& op) {
            std::string kind = op.value("op", ""), resource = op.value("resource", "");
            std::string key = op.contains("key") ? (op["key"].is_string() ? op["key"].get<std::string>() : op["key"].dump()) : std::string();
            if (kind == "action") {
                ++push_requests;
                for (const auto& command: op["body"]["commands"]) {
                    auto it = feeds.find(command.value("name", std::string()));
                    if (it == feeds.end()) continue;
                    auto& f = it->second;
                    if (f.pending_reads > 0 || drop_pushes) continue;   // FortiOS drops it and still says success
                    applying.insert(command.value("name", std::string()));
                    std::string verb = command.value("command", "");
                    std::vector<std::string> entries = command.value("entries", std::vector<std::string>{});
                    if (verb == "snapshot") f.entries.clear();
                    if (verb == "remove") {
                        std::set<std::string> gone(entries.begin(), entries.end());
                        std::erase_if(f.entries, [&](const std::string& e) { return gone.contains(e); });
                    } else {
                        std::set<std::string> have(f.entries.begin(), f.entries.end());
                        for (auto& e: entries) {
                            if (have.contains(e)) continue;
                            if (total_entries() >= entry_limit) {
                                f.overflow = true;
                                continue;
                            }
                            have.insert(e);
                            f.entries.push_back(e);
                        }
                    }
                    f.last_update = ++clock;
                }
                return {};
            }
            if (resource == "system/external-resource") {
                if (kind == "create") {
                    std::string name = op["object"]["name"];
                    if (feeds.contains(name)) return "feed exists";
                    FakeFeed f;
                    f.object = op["object"];
                    feeds[name] = std::move(f);
                } else if (kind == "update") {
                    auto it = feeds.find(key);
                    if (it == feeds.end()) return "no such feed";
                    for (const auto& [k, v]: op.value("set", json::object()).items()) it->second.object[k] = v;
                } else if (kind == "delete") {
                    auto it = feeds.find(key);
                    if (it == feeds.end()) return "no such feed";
                    int category = it->second.object.value("category", 0);
                    bool other = std::any_of(feeds.begin(), feeds.end(), [&](const auto& p) {
                        return p.first != key && p.second.object.value("category", 0) == category;
                    });
                    if (!other && category_blocked(category)) return "the category is used by a DNS filter profile";
                    feeds.erase(it);
                }
                return {};
            }
            if (resource == "dnsfilter/profile") {
                // FortiOS: a remote category (192-221) is valid in a profile only while a feed provides it.
                const json& body = kind == "create" ? op["object"] : op.value("set", json::object());
                if (body.contains("ftgd-dns"))
                    for (const auto& f: body["ftgd-dns"].value("filters", json::array())) {
                        int c = f.value("category", 0);
                        bool fed = std::any_of(feeds.begin(), feeds.end(), [&](const auto& p) { return p.second.object.value("category", 0) == c; });
                        if (c >= 192 && c <= 221 && !fed) return std::format("Error: category '{}' is not a valid category ID.", c);
                    }
                if (kind == "create") profiles[op["object"]["name"].get<std::string>()] = op["object"];
                else if (kind == "update") {
                    auto it = profiles.find(key);
                    if (it == profiles.end()) return "no such profile";
                    for (const auto& [k, v]: op.value("set", json::object()).items()) it->second[k] = v;
                } else if (kind == "delete") {
                    for (const auto& [id, p]: policies)
                        if (p.value("dnsfilter-profile", "") == key) return "the profile is used by a policy";
                    profiles.erase(key);
                }
                return {};
            }
            if (resource == "firewall/policy") {
                auto it = policies.find(std::stoull(key));
                if (it == policies.end()) return "no such policy";
                for (const auto& [k, v]: op.value("set", json::object()).items()) it->second[k] = v;
                for (const auto& field: op.value("clear", json::array())) it->second[field.get<std::string>()] = "";
                return {};
            }
            return "fake: unsupported " + resource;
        }
    };

}  // namespace fh::test

#endif  // FORTI_HOLE_TESTS_FAKE_GATEWAY_HPP
