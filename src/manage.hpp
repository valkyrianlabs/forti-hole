//
// The explicit, operator-driven changes: attaching a forti-hole DNS filter profile to the firewall
// policies DNS queries cross (threat feeds act there, not on the FortiGate's own DNS service), detaching it,
// removing everything forti-hole created, and cleaning up after forti-hole 0.x. None of these runs
// as part of a sync; each is one engine plan/apply with permits for exactly the objects it names.
//

#ifndef FORTI_HOLE_MANAGE_HPP
#define FORTI_HOLE_MANAGE_HPP

#include "config.hpp"
#include "device.hpp"
#include "gateway.hpp"
#include "state.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace fh {

    struct ManageContext {
        Gateway& gateway;
        const Config& config;
        State& state;
        const Home& home;
        std::shared_ptr<WritePermits> permits;
    };

    // Each returns a report (JSON) of what it changed; SyncError on refusal or failure.
    nlohmann::json attach_policy(ManageContext& ctx, std::string_view profile, std::uint64_t policy);
    nlohmann::json detach_policy(ManageContext& ctx, std::uint64_t policy, bool force);
    // Where forti-hole's profiles are in use (firewall policies, DNS services), for status, doctor, remove.
    nlohmann::json profile_references(Gateway& gateway, const Config& config);

    // Deletes forti-hole's DNS filter profiles (refused while one is attached) and feeds.
    nlohmann::json remove_all(ManageContext& ctx, const Inventory& inventory);

    // forti-hole 0.x feeds: the report, and with apply, the cleanup.
    nlohmann::json migrate_feeds(ManageContext& ctx, const Inventory& inventory, std::string_view legacy_prefix, bool apply);

}  // namespace fh

#endif  // FORTI_HOLE_MANAGE_HPP
