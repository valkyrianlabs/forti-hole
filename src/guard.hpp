//
// The write allowlist (AGENTS.md rule 2), enforced twice:
//
//   check_operations  before planning: every engine operation is one forti-hole may make;
//   GuardTransport    under the engine: any POST/PUT/DELETE outside the allowlist is refused before
//                     it is sent, so a bug in the planner, or a change in what the engine sends, still
//                     cannot reach anything else.
//
// Always allowed (forti-hole's own objects: name starting with the prefix):
//   system/external-resource <prefix>*        create (with the marker), update, delete
//   monitor/system/external-resource/dynamic  push commands naming only <prefix>* feeds
//   dnsfilter/profile <prefix>*               create (with the marker), update, delete
//   dnsfilter/domain-filter                   create (<prefix>* name, marker); update/delete only of
//                                             tables recorded as forti-hole's (permits.domain_filters)
// Only with a permit for one apply (PermitScope):
//   dnsfilter/profile <name>                  an operator profile the operator adopted (its ftgd-dns
//                                             filters for forti-hole's categories), or one that still
//                                             blocks a legacy category being freed (migrate)
//   firewall/policy <id>                      attach / detach only (dnsfilter-profile, utm-status)
//   system/external-resource <legacy name>    migrate only: forti-hole 0.x feeds, delete
//

#ifndef FORTI_HOLE_GUARD_HPP
#define FORTI_HOLE_GUARD_HPP

#include <forti_api/client.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace fh {

    class GuardViolation : public std::logic_error {
    public:
        using std::logic_error::logic_error;
    };

    struct WritePermits {
        std::set<std::string, std::less<>> profiles;        // operator DNS filter profiles
        std::set<std::uint64_t> domain_filters;             // forti-hole's domain-filter tables (by id)
        std::set<std::uint64_t> policies;                   // firewall policies (attach/detach)
        std::set<std::string, std::less<>> legacy_feeds;    // forti-hole 0.x feeds (migrate)
    };

    // Grants permits for the lifetime of the object (an apply), then withdraws them.
    class PermitScope {
        std::shared_ptr<WritePermits> permits_;

    public:
        PermitScope(std::shared_ptr<WritePermits> permits, WritePermits granted) : permits_(std::move(permits)) {
            *permits_ = std::move(granted);
        }
        ~PermitScope() { *permits_ = {}; }
        PermitScope(const PermitScope&) = delete;
        PermitScope& operator=(const PermitScope&) = delete;
    };

    // Throws GuardViolation naming the first offending operation (engine plan operations).
    void check_operations(const nlohmann::json& operations, std::string_view prefix, const WritePermits& permits = {});

    // Empty when the request may be sent; otherwise why not.
    std::string refuse_write(fortiapi::Method method, std::string_view url, std::string_view body, std::string_view prefix,
                             const WritePermits& permits = {});

    class GuardTransport final : public fortiapi::Transport {
        std::shared_ptr<fortiapi::Transport> inner_;
        std::string prefix_;
        std::shared_ptr<WritePermits> permits_;

    public:
        GuardTransport(std::shared_ptr<fortiapi::Transport> inner, std::string prefix,
                       std::shared_ptr<WritePermits> permits = std::make_shared<WritePermits>())
                : inner_(std::move(inner)), prefix_(std::move(prefix)), permits_(std::move(permits)) {}

        fortiapi::HttpResponse perform(const fortiapi::HttpRequest& request) override;
    };

}  // namespace fh

#endif  // FORTI_HOLE_GUARD_HPP
