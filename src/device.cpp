#include "device.hpp"

#include <format>

namespace fh {

    using json = nlohmann::json;

    namespace {

        std::string str(const json& object, const char* key) {
            if (!object.is_object()) return {};
            auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        // An object attribute, or {} when it is missing or not an object (a device or mock may send null).
        json object_at(const json& object, const char* key) {
            if (!object.is_object()) return json::object();
            auto it = object.find(key);
            return it != object.end() && it->is_object() ? *it : json::object();
        }

        json array_at(const json& object, const char* key) {
            if (!object.is_object()) return json::array();
            auto it = object.find(key);
            return it != object.end() && it->is_array() ? *it : json::array();
        }

        std::uint64_t num(const json& object, const char* key) {
            if (!object.is_object()) return 0;
            auto it = object.find(key);
            if (it == object.end()) return 0;
            if (it->is_number_unsigned()) return it->get<std::uint64_t>();
            if (it->is_number_integer()) return static_cast<std::uint64_t>(std::max<std::int64_t>(0, it->get<std::int64_t>()));
            if (it->is_number_float()) return static_cast<std::uint64_t>(std::max(0.0, it->get<double>()));
            return 0;
        }

    }  // namespace

    bool is_owned(std::string_view name, std::string_view comment, const Config& config) {
        return name.starts_with(config.prefix) && comment.find(marker) != std::string_view::npos;
    }

    std::string ownership_comment(std::string_view detail) {
        return detail.empty() ? std::string(marker) : std::format("{}: {}", marker, detail).substr(0, 255);
    }

    const DeviceFeed* Inventory::feed(std::string_view name) const {
        for (const auto& f: feeds)
            if (f.name == name) return &f;
        return nullptr;
    }

    const DeviceProfile* Inventory::profile(std::string_view name) const {
        for (const auto& p: profiles)
            if (p.name == name) return &p;
        return nullptr;
    }

    Inventory read_inventory(Gateway& gateway, const Config& config, const InventoryOptions& options) {
        Inventory inventory;
        json request_base = json::object();
        if (!config.profile.empty()) request_base["profile"] = config.profile;
        auto with = [&](json request) {
            for (const auto& [k, v]: request_base.items()) request[k] = v;
            if (!config.vdom.empty() && !request.contains("vdom")) request["vdom"] = config.vdom;
            return request;
        };

        auto capabilities = expect_ok(gateway.discover(with({{"what", "capabilities"}})));
        const json device = object_at(capabilities, "device");
        inventory.serial = str(device, "serial");
        inventory.version = str(device, "version");
        inventory.hostname = str(device, "hostname");
        inventory.model = str(device, "model");

        auto feeds = read_all(gateway, with({{"resource", "system/external-resource"},
                                             {"fields", {"name", "type", "update-method", "category", "comments", "status"}}}));
        for (const auto& item: feeds) {
            DeviceFeed feed;
            feed.name = str(item, "name");
            feed.type = str(item, "type");
            feed.update_method = str(item, "update-method");
            feed.comments = str(item, "comments");
            feed.status = str(item, "status");
            feed.category = static_cast<int>(num(item, "category"));
            feed.owned = is_owned(feed.name, feed.comments, config);
            if (!feed.owned && feed.name.starts_with(config.prefix)) inventory.collisions.push_back("feed " + feed.name);
            if (!feed.owned && feed.category >= 192 && feed.category <= 221 && (feed.type == "domain" || feed.type == "category"))
                inventory.foreign_categories.insert(feed.category);
            if (options.feed_states && feed.type == "domain" && feed.status != "disable") {
                try {
                    feed.state = gateway.feed_status(feed.name);
                    if (feed.state->limit > 0) inventory.facts.entry_limit = feed.state->limit;
                    (feed.owned ? inventory.facts.owned_entries : inventory.facts.other_entries) += feed.state->accepted;
                } catch (const GatewayError& error) {
                    inventory.warnings.push_back(std::format("feed {}: status unavailable: {}", feed.name, error.what()));
                }
            }
            inventory.feeds.push_back(std::move(feed));
        }

        auto profiles = read_all(gateway, with({{"resource", "dnsfilter/profile"}, {"fields", {"name", "comment", "ftgd-dns", "block-action", "domain-filter"}}}));
        for (const auto& item: profiles) {
            DeviceProfile profile;
            profile.name = str(item, "name");
            profile.comment = str(item, "comment");
            profile.block_action = str(item, "block-action");
            profile.ftgd_dns = object_at(item, "ftgd-dns");
            profile.domain_filter_table = num(object_at(item, "domain-filter"), "domain-filter-table");
            profile.owned = is_owned(profile.name, profile.comment, config);
            if (!profile.owned && profile.name.starts_with(config.prefix)) inventory.collisions.push_back("DNS filter profile " + profile.name);
            for (const auto& filter: array_at(profile.ftgd_dns, "filters")) {
                auto category = static_cast<int>(num(filter, "category"));
                if (category >= 192 && category <= 221) profile.remote_categories[category] = str(filter, "action");
            }
            inventory.profiles.push_back(std::move(profile));
        }

        auto filters = read_all(gateway, with({{"resource", "dnsfilter/domain-filter"}, {"fields", {"id", "name", "comment", "entries"}}}));
        for (const auto& item: filters) {
            DeviceDomainFilter filter;
            filter.id = num(item, "id");
            filter.name = str(item, "name");
            filter.comment = str(item, "comment");
            filter.entries = array_at(item, "entries");
            filter.owned = is_owned(filter.name, filter.comment, config);
            if (!filter.owned && filter.name.starts_with(config.prefix)) inventory.collisions.push_back("domain filter " + filter.name);
            inventory.domain_filters.push_back(std::move(filter));
        }

        if (options.memory) {
            try {
                auto performance = expect_ok(gateway.read(with({{"resource", "monitor/system/performance/status"}})));
                const json mem = object_at(performance, "mem");
                if (num(mem, "total") > 0) {
                    inventory.facts.memory_total = num(mem, "total");
                    inventory.facts.memory_used = num(mem, "used");
                }
                auto global = expect_ok(gateway.read(with({{"resource", "system/global"}, {"fields", {"memory-use-threshold-red"}}})));
                if (auto red = num(global, "memory-use-threshold-red"); red > 0) inventory.facts.memory_red_percent = static_cast<double>(red);
                auto usage = expect_ok(gateway.read(with({{"resource", "monitor/system/resource/usage"}, {"params", {{"resource", "mem"}, {"interval", "1-day"}}}})));
                for (const auto& sample: array_at(usage, "mem")) {
                    const json day = object_at(object_at(sample, "historical"), "24-hour");
                    if (day.contains("max") && day["max"].is_number()) inventory.facts.memory_peak_percent = day["max"].get<double>();
                }
            } catch (const GatewayError& error) {
                inventory.warnings.push_back(std::format("memory figures unavailable: {}", error.what()));
            }
        }

        if (options.licence) {
            try {
                auto licence = expect_ok(gateway.read(with({{"resource", "monitor/license/status"}})));
                if (auto web = object_at(licence, "web_filtering"); !web.empty()) inventory.fortiguard_web_filtering = str(web, "status");
            } catch (const GatewayError&) {
                // diagnostics only
            }
        }
        return inventory;
    }

}  // namespace fh
