//
// One read of everything a run needs from the FortiGate: identity, feeds (forti-hole's and the
// operator's), DNS filter profiles and domain filters, memory. Nothing here writes.
//
// Ownership: an object is forti-hole's only when its name starts with the prefix AND it carries the
// marker ("managed by forti-hole") in its comment. A prefixed object without the marker is the
// operator's, read-only, and a name collision with it stops the run rather than overwriting it.
//

#ifndef FORTI_HOLE_DEVICE_HPP
#define FORTI_HOLE_DEVICE_HPP

#include "config.hpp"
#include "core/budget.hpp"
#include "gateway.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fh {

    struct DeviceFeed {
        std::string name, type, update_method, comments, status;
        int category = 0;
        bool owned = false;
        std::optional<FeedStatus> state;   // domain feeds only
    };

    struct DeviceProfile {
        std::string name, comment, block_action;
        nlohmann::json ftgd_dns = nlohmann::json::object();
        std::uint64_t domain_filter_table = 0;
        bool owned = false;
        // Remote categories (192-221) this profile lists, with their action.
        std::map<int, std::string> remote_categories;
    };

    struct DeviceDomainFilter {
        std::uint64_t id = 0;
        std::string name, comment;
        nlohmann::json entries = nlohmann::json::array();
        bool owned = false;
    };

    struct Inventory {
        std::string serial, version, hostname, model;
        std::vector<DeviceFeed> feeds;
        std::vector<DeviceProfile> profiles;
        std::vector<DeviceDomainFilter> domain_filters;
        DeviceFacts facts;
        std::set<int> foreign_categories;   // remote categories used by feeds forti-hole does not own
        std::vector<std::string> collisions;   // prefixed objects without the marker
        std::optional<std::string> fortiguard_web_filtering;   // licence status, diagnostics only
        std::vector<std::string> warnings;

        [[nodiscard]] const DeviceFeed* feed(std::string_view name) const;
        [[nodiscard]] const DeviceProfile* profile(std::string_view name) const;
    };

    struct InventoryOptions {
        bool feed_states = true;     // one status read per domain feed
        bool memory = true;
        bool licence = false;        // doctor only
    };

    Inventory read_inventory(Gateway& gateway, const Config& config, const InventoryOptions& options = {});

    [[nodiscard]] bool is_owned(std::string_view name, std::string_view comment, const Config& config);

    // The comment forti-hole writes on its objects (always starts with the marker).
    [[nodiscard]] std::string ownership_comment(std::string_view detail);

}  // namespace fh

#endif  // FORTI_HOLE_DEVICE_HPP
