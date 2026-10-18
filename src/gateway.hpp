//
// The one door to the FortiGate. Production uses the forti-engine (profiles, credentials, read
// redaction, plan/apply with digests, audit, rollback plans, the profile's mode) with the write guard
// under it. Tests use an in-memory FortiGate (tests/support/fake_gateway.hpp).
//
// Writes are engine plans and applies, always. Feed status and entry pages are reads through
// forti-api's threat-feed helpers on the engine's own connection (one request for the counts; paged
// entries), because Engine::read has to fetch a whole entry list to window it.
//

#ifndef FORTI_HOLE_GATEWAY_HPP
#define FORTI_HOLE_GATEWAY_HPP

#include "guard.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fh {

    struct Config;

    struct FeedStatus {
        bool loaded = false;                // FortiOS has loaded the feed; pushes are accepted
        std::string file_status;            // "downloaded" once entries were applied
        std::string error;                  // "Connect pending" before it is loaded
        std::uint64_t accepted = 0, valid = 0, invalid = 0;
        std::uint64_t limit = 0;            // the device-wide domain-entry limit (0: not reported)
        bool overflow = false;              // the limit truncated this feed
        std::int64_t last_update = 0;       // last_content_update_time
    };

    struct FeedEntry {
        std::string entry;
        bool valid = false;
    };

    class Gateway {
    public:
        virtual ~Gateway() = default;
        // Engine envelopes: {"ok": true, "data", "meta"} or {"ok": false, "error"}.
        virtual nlohmann::json read(const nlohmann::json& request) = 0;
        virtual nlohmann::json plan(const nlohmann::json& request) = 0;
        virtual nlohmann::json apply(const nlohmann::json& request) = 0;
        virtual nlohmann::json discover(const nlohmann::json& request) = 0;
        // Throw GatewayError.
        virtual FeedStatus feed_status(std::string_view feed) = 0;
        virtual std::vector<FeedEntry> feed_entries(std::string_view feed, std::size_t start, std::size_t count) = 0;
    };

    class GatewayError : public std::runtime_error {
        nlohmann::json error_;

    public:
        explicit GatewayError(nlohmann::json error)
                : std::runtime_error(error.value("message", std::string("FortiGate request failed"))), error_(std::move(error)) {}
        [[nodiscard]] const nlohmann::json& error() const { return error_; }
        [[nodiscard]] std::string code() const { return error_.value("code", std::string("internal")); }
    };

    // The data of a successful envelope; GatewayError otherwise.
    nlohmann::json expect_ok(const nlohmann::json& envelope);
    // Every page of a cmdb listing (the engine caps a page at 1000 items).
    nlohmann::json read_all(Gateway& gateway, nlohmann::json request);

    // A forti-engine on the user's forti home, with the guard transport sharing `permits`.
    std::unique_ptr<Gateway> engine_gateway(const Config& config, std::shared_ptr<WritePermits> permits);

}  // namespace fh

#endif  // FORTI_HOLE_GATEWAY_HPP
