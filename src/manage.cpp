#include "manage.hpp"

#include "sync.hpp"

#include <format>
#include <set>

namespace fh {

    using json = nlohmann::json;

    namespace {

        json with_target(const Config& config, json request) {
            if (!config.profile.empty()) request["profile"] = config.profile;
            if (!config.vdom.empty()) request["vdom"] = config.vdom;
            return request;
        }

        std::string str(const json& object, const char* key) {
            auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        // A single object, or nullopt when it does not exist.
        std::optional<json> read_one(Gateway& gateway, const Config& config, std::string_view resource, const json& key, std::vector<std::string> fields) {
            auto envelope = gateway.read(with_target(config, {{"resource", resource}, {"key", key}, {"fields", fields}}));
            if (envelope.value("ok", false)) return envelope["data"];
            if (envelope["error"].value("code", std::string()) == "not_found") return std::nullopt;
            throw GatewayError(envelope["error"]);
        }

        std::string managed_profile(ManageContext& ctx, std::string_view profile) {
            bool known = false;
            for (const auto& p: ctx.config.profiles) known = known || p.name == profile;
            if (!known) throw SyncError(std::format("\"{}\" is not a profile in config.json", profile), 2);
            std::string name = dnsfilter_profile_name(ctx.config, profile);
            auto device = read_one(ctx.gateway, ctx.config, "dnsfilter/profile", name, {"name", "comment"});
            if (!device) throw SyncError(std::format("DNS filter profile {} does not exist yet: run `forti-hole sync` first", name), 5);
            if (!is_owned(name, str(*device, "comment"), ctx.config))
                throw SyncError(std::format("DNS filter profile {} is not forti-hole's (no marker)", name), 6);
            return name;
        }

        const std::vector<std::string> utm_fields = {"av-profile",          "webfilter-profile", "ips-sensor",       "application-list",
                                                     "emailfilter-profile", "file-filter-profile", "dlp-profile",    "voip-profile",
                                                     "waf-profile",         "icap-profile",      "videofilter-profile", "casb-profile"};

    }  // namespace

    json attach_policy(ManageContext& ctx, std::string_view profile, std::uint64_t policy) {
        std::string name = managed_profile(ctx, profile);
        auto current = read_one(ctx.gateway, ctx.config, "firewall/policy", policy, {"policyid", "name", "action", "dnsfilter-profile", "utm-status"});
        if (!current) throw SyncError(std::format("firewall policy {} does not exist", policy), 5);
        if (str(*current, "action") != "accept") throw SyncError(std::format("policy {} does not accept traffic; a DNS filter has nothing to inspect", policy), 6);
        std::string before = str(*current, "dnsfilter-profile"), utm = str(*current, "utm-status");
        json result = {{"policy", policy}, {"policy_name", str(*current, "name")}, {"profile", name}, {"previous_profile", before}};
        if (before == name && utm == "enable") {
            result["changed"] = false;
            return result;
        }
        json set = {{"dnsfilter-profile", name}};
        if (utm != "enable") set["utm-status"] = "enable";
        WritePermits granted;
        granted.policies.insert(policy);
        json ops = json::array({{{"op", "update"}, {"resource", "firewall/policy"}, {"key", policy}, {"set", set}}});
        std::string id;
        apply_operations(ctx.gateway, ctx.config, ops, ctx.permits, granted, &id);
        auto key = std::format("policy:{}", policy);
        // Keep the very first "previous" across re-attaching another forti-hole profile.
        auto existing = ctx.state.attachments.find(key);
        Attachment a{"policy", std::to_string(policy), name, before, utm, false, now_iso8601()};
        if (existing != ctx.state.attachments.end()) {
            a.previous_profile = existing->second.previous_profile;
            a.previous_utm = existing->second.previous_utm;
        }
        ctx.state.attachments[key] = a;
        save_state(ctx.home, ctx.state);
        result["changed"] = true;
        result["plan_id"] = id;
        return result;
    }

    json detach_policy(ManageContext& ctx, std::uint64_t policy, bool force) {
        auto key = std::format("policy:{}", policy);
        auto record = ctx.state.attachments.find(key);
        std::vector<std::string> fields = {"policyid", "name", "dnsfilter-profile", "utm-status"};
        fields.insert(fields.end(), utm_fields.begin(), utm_fields.end());
        auto current = read_one(ctx.gateway, ctx.config, "firewall/policy", policy, fields);
        if (!current) throw SyncError(std::format("firewall policy {} does not exist", policy), 5);
        std::string now = str(*current, "dnsfilter-profile");
        if (!now.starts_with(ctx.config.prefix)) throw SyncError(std::format("policy {} has no forti-hole profile (it has \"{}\")", policy, now), 6);
        if (record == ctx.state.attachments.end() && !force)
            throw SyncError(std::format("forti-hole has no record of attaching {} to policy {}; --force clears it", now, policy), 6);
        std::string previous = record != ctx.state.attachments.end() ? record->second.previous_profile : std::string();
        json op = {{"op", "update"}, {"resource", "firewall/policy"}, {"key", policy}};
        json set = json::object();
        if (previous.empty() || previous.starts_with(ctx.config.prefix)) op["clear"] = json::array({"dnsfilter-profile"});
        else set["dnsfilter-profile"] = previous;
        // utm-status goes back off only if forti-hole turned it on and nothing else needs it.
        bool others = false;
        for (const auto& field: utm_fields) others = others || !str(*current, field.c_str()).empty();
        if (record != ctx.state.attachments.end() && record->second.previous_utm == "disable" && !others && set.value("dnsfilter-profile", "").empty())
            set["utm-status"] = "disable";
        if (!set.empty()) op["set"] = set;
        WritePermits granted;
        granted.policies.insert(policy);
        std::string id;
        apply_operations(ctx.gateway, ctx.config, json::array({op}), ctx.permits, granted, &id);
        if (record != ctx.state.attachments.end()) ctx.state.attachments.erase(record);
        save_state(ctx.home, ctx.state);
        return {{"policy", policy}, {"removed", now}, {"restored", previous}, {"plan_id", id}};
    }

    json profile_references(Gateway& gateway, const Config& config) {
        json refs = json::array();
        for (const auto& policy: read_all(gateway, with_target(config, {{"resource", "firewall/policy"}, {"fields", {"policyid", "name", "dnsfilter-profile"}}}))) {
            std::string profile = str(policy, "dnsfilter-profile");
            if (profile.starts_with(config.prefix))
                refs.push_back({{"kind", "policy"}, {"target", policy.value("policyid", 0)}, {"name", str(policy, "name")}, {"profile", profile}});
        }
        // The FortiGate's own DNS service can use a profile too (set by the operator; read only here).
        json servers = json::array();
        try {
            servers = read_all(gateway, with_target(config, {{"resource", "system/dns-server"}, {"fields", {"name", "dnsfilter-profile"}}}));
        } catch (const GatewayError& error) {
            if (error.code() != "not_found") throw;   // a VDOM without the DNS service table has no references there
        }
        for (const auto& server: servers) {
            std::string profile = str(server, "dnsfilter-profile");
            if (profile.starts_with(config.prefix)) refs.push_back({{"kind", "dns-server"}, {"target", str(server, "name")}, {"profile", profile}});
        }
        return refs;
    }

    json remove_all(ManageContext& ctx, const Inventory& inventory) {
        auto refs = profile_references(ctx.gateway, ctx.config);
        if (!refs.empty()) {
            std::string where;
            for (const auto& r: refs) where += std::format(" {} {} ({});", r["kind"].get<std::string>(), r["target"].dump(), r["profile"].get<std::string>());
            throw SyncError(std::format("forti-hole profiles are still attached:{} detach them first", where), 6);
        }
        json ops = json::array();
        json removed = json::array();
        for (const auto& profile: inventory.profiles)
            if (profile.owned) {
                ops.push_back({{"op", "delete"}, {"resource", "dnsfilter/profile"}, {"key", profile.name}});
                removed.push_back("dnsfilter/profile " + profile.name);
            }
        for (const auto& feed: inventory.feeds)
            if (feed.owned) {
                ops.push_back({{"op", "delete"}, {"resource", "system/external-resource"}, {"key", feed.name}});
                removed.push_back("system/external-resource " + feed.name);
            }
        std::string id;
        if (!ops.empty()) apply_operations(ctx.gateway, ctx.config, ops, ctx.permits, {}, &id);
        for (const auto& [name, f]: ctx.state.feeds) remove_feed_lines(ctx.home, name);
        ctx.state.feeds.clear();
        ctx.state.groups.clear();
        save_state(ctx.home, ctx.state);
        return {{"removed", removed}, {"plan_id", id}};
    }

    json migrate_feeds(ManageContext& ctx, const Inventory& inventory, std::string_view legacy_prefix, bool apply) {
        std::set<int> legacy_categories, shared;
        json feeds = json::array();
        for (const auto& feed: inventory.feeds) {
            if (!feed.name.starts_with(legacy_prefix) || feed.owned) continue;
            legacy_categories.insert(feed.category);
            feeds.push_back({{"name", feed.name}, {"category", feed.category}, {"entries", feed.state ? feed.state->accepted : 0}});
        }
        for (const auto& feed: inventory.feeds)
            if (!feed.name.starts_with(legacy_prefix) && legacy_categories.contains(feed.category)) shared.insert(feed.category);
        std::set<int> freed;
        for (int c: legacy_categories)
            if (!shared.contains(c)) freed.insert(c);

        json ops = json::array();
        json profiles = json::array();
        WritePermits granted;
        for (const auto& profile: inventory.profiles) {
            json filters = json::array();
            bool touched = false;
            json filters_now = profile.ftgd_dns.contains("filters") && profile.ftgd_dns["filters"].is_array() ? profile.ftgd_dns["filters"] : json::array();
            for (const auto& f: filters_now) {
                if (freed.contains(f.value("category", 0))) {
                    touched = true;
                    continue;
                }
                json kept = f;
                kept.erase("q_origin_key");
                filters.push_back(std::move(kept));
            }
            if (!touched) continue;
            profiles.push_back(profile.name);
            json ftgd = {{"options", profile.ftgd_dns.contains("options") && profile.ftgd_dns["options"].is_string() ? profile.ftgd_dns["options"] : json("")},
                         {"filters", filters}};
            ops.push_back({{"op", "update"}, {"resource", "dnsfilter/profile"}, {"key", profile.name}, {"set", {{"ftgd-dns", ftgd}}}});
            if (!profile.owned) granted.profiles.insert(profile.name);
        }
        for (const auto& feed: feeds) {
            ops.push_back({{"op", "delete"}, {"resource", "system/external-resource"}, {"key", feed["name"]}});
            granted.legacy_feeds.insert(feed["name"].get<std::string>());
        }
        json report = {{"feeds", feeds}, {"profiles_updated", profiles}, {"categories_freed", freed}, {"applied", false}};
        if (!apply || feeds.empty()) return report;
        std::string id;
        apply_operations(ctx.gateway, ctx.config, ops, ctx.permits, granted, &id);
        report["applied"] = true;
        report["plan_id"] = id;
        return report;
    }

}  // namespace fh
