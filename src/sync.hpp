//
// The sync: make the device's forti-hole feeds hold exactly the compiled set, with as few writes
// as possible, and prove it.
//
// Planning (plan_sync, no writes):
//   - each group of feeds keeps its remote category across runs (state.json); a new group takes the
//     lowest free one in feeds.category_min..max that no operator feed uses;
//   - per feed, the baseline is what the device was last *confirmed* to hold (feeds/<feed>.txt), and
//     only if the device still reports that: the same entry count and the same content-update time.
//     Otherwise the baseline is unknown and the feed gets one full snapshot;
//   - with a baseline: identical content -> nothing; otherwise add/remove deltas, unless the delta is
//     larger than the feed (then a snapshot is cheaper);
//   - feeds forti-hole owns that the compiled set no longer has are deleted, last.
//
// Execution (execute_sync), in phases, each through engine plan/apply:
//   1. create new feeds, set categories, and add new categories to the managed DNS filter profiles
//      (profiles keep every category they block now until phase 3, so nothing stops being blocked
//      while names move between feeds);
//   2. wait for FortiOS to load new feeds, write the journal, then push: snapshots (one request per
//      feed, at most feeds.max_entries lines) and deltas in chunks (adds before removes, again so a
//      moving name is never absent);
//   3. verify every pushed feed on the device: the content-update time moved, the accepted count is
//      the compiled line count, nothing invalid, no overflow, and sampled pages contain only compiled
//      lines. Only then is the feed's new content recorded as confirmed;
//   4. drop categories that are gone from the managed profiles and delete obsolete feeds.
// A run that stops part-way leaves the journal; the next run treats every feed it names as
// unconfirmed (one snapshot each), so an interrupted push can never be mistaken for a good one.
//

#ifndef FORTI_HOLE_SYNC_HPP
#define FORTI_HOLE_SYNC_HPP

#include "config.hpp"
#include "device.hpp"
#include "gateway.hpp"
#include "pipeline.hpp"
#include "state.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fh {

    struct FeedAction {
        enum class Kind { unchanged, create, snapshot, delta, remove };
        Kind kind = Kind::unchanged;
        std::string feed, group;
        int category = 0;
        bool category_changes = false;
        std::vector<std::string> profiles;
        std::uint64_t lines_device = 0;    // the device's count now
        std::uint64_t lines_after = 0;
        std::vector<std::string> adds, removes;   // delta only
        std::string reason;
        const CompiledFeed* compiled = nullptr;   // null for remove
        std::int64_t last_update_before = 0;
    };

    [[nodiscard]] std::string_view to_string(FeedAction::Kind kind);

    struct ProfileAction {
        std::string name;                      // device name
        enum class Kind { unchanged, create, update } kind = Kind::unchanged;
        bool owned = true;                     // false: an adopted operator profile
        nlohmann::json phase1, phase3;         // the update body ({"ftgd-dns": ...}, ...) per phase; null: no change
        std::vector<int> categories;           // what it blocks after the sync
    };

    struct SyncPlan {
        std::vector<FeedAction> feeds;
        std::vector<ProfileAction> profiles;
        std::map<std::string, int, std::less<>> categories;   // group -> category
        nlohmann::json phase1 = nlohmann::json::array();       // engine operations
        nlohmann::json pushes = nlohmann::json::array();
        nlohmann::json phase3 = nlohmann::json::array();
        std::uint64_t lines_after = 0, adds = 0, removes = 0, snapshot_lines = 0;
        std::vector<std::string> notes;
        [[nodiscard]] bool writes() const { return !phase1.empty() || !pushes.empty() || !phase3.empty(); }
    };

    class SyncError : public std::runtime_error {
        int exit_;

    public:
        SyncError(std::string message, int exit) : std::runtime_error(std::move(message)), exit_(exit) {}
        [[nodiscard]] int exit_code() const { return exit_; }
    };

    SyncPlan plan_sync(const Config& config, const State& state, const Home& home, const Inventory& inventory, const CompileRun& run);

    struct Clock {
        std::function<std::int64_t()> now = [] { return std::int64_t(0); };   // seconds; production: steady clock
        std::function<void(int milliseconds)> sleep = [](int) {};
    };

    struct FeedVerification {
        std::string feed;
        bool verified = false;
        std::uint64_t expected = 0, accepted = 0, valid = 0, invalid = 0;
        std::size_t sampled = 0, sample_mismatches = 0;
        bool overflow = false;
        std::int64_t last_update = 0;
        std::string problem;
    };

    struct SyncOutcome {
        std::vector<std::string> plan_ids;
        std::vector<FeedVerification> verified;
        std::vector<std::string> deleted;
        std::optional<Measurement> measurement;
        bool waiting_for_approval = false;
        std::string waiting_plan;
        std::vector<std::string> problems;
        int exit_code = 0;
    };

    // Runs the phases above. `permits` is the guard's (the gateway's) permit set.
    SyncOutcome execute_sync(Gateway& gateway, const Config& config, State& state, const Home& home, const Inventory& inventory,
                             const SyncPlan& plan, std::shared_ptr<WritePermits> permits, const Clock& clock,
                             const std::function<void(const std::string&)>& progress);

    // Verifies one feed against its expected sorted lines (also `status --verify`).
    FeedVerification verify_feed(Gateway& gateway, const Config& config, std::string_view feed, const std::vector<std::string>& expected,
                                 std::int64_t updated_after, const Clock& clock, bool full = false);

    // The engine plan/apply of `operations` (guard-checked first). Returns the apply data; throws
    // SyncError (exit 7 waiting for approval / read-only, 8 partial or unknown outcome, 6 refused).
    nlohmann::json apply_operations(Gateway& gateway, const Config& config, const nlohmann::json& operations,
                                    const std::shared_ptr<WritePermits>& permits, const WritePermits& granted, std::string* plan_id = nullptr);

}  // namespace fh

#endif  // FORTI_HOLE_SYNC_HPP
