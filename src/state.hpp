//
// What forti-hole remembers between runs (state.json), and the confirmed feed contents
// (feeds/<feed>.txt).
//
// A feed's record says what the device was last *confirmed* to hold: written only after a push was
// verified on the device (counts and sampled entries), never on the strength of an HTTP 200. If a
// run is interrupted, or the device no longer matches (reboot, someone else's push, failover), the
// record is marked unconfirmed and the next run sends a full snapshot of that feed.
//

#ifndef FORTI_HOLE_STATE_HPP
#define FORTI_HOLE_STATE_HPP

#include "files.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fh {

    struct SourceCache {
        std::string url;
        std::string etag, last_modified;
        std::string sha256;              // of the cached list file
        std::uint64_t entries = 0;       // valid entries parsed from it
        std::uint64_t bytes = 0;
        std::string fetched_at;          // when this copy was downloaded
        std::string checked_at;          // last time upstream was asked (304 included)
    };

    struct GroupState {
        int category = 0;
        std::size_t partitions = 1;
    };

    struct FeedState {
        std::string group;
        int category = 0;
        bool confirmed = false;
        std::string sha256;              // of feeds/<name>.txt
        std::uint64_t lines = 0;
        std::int64_t device_update = 0;  // the device's last_content_update_time when confirmed
        std::string confirmed_at;
    };

    struct Measurement {
        std::string at;
        std::uint64_t lines_before = 0, lines_after = 0;
        std::uint64_t used_before = 0, used_after = 0;
        double bytes_per_line = 0;
    };

    // A profile forti-hole attached (attach), and what it replaced, so detach restores exactly that.
    struct Attachment {
        std::string kind;                 // "policy"
        std::string target;               // policy id
        std::string profile;              // the DNS filter profile attached
        std::string previous_profile;     // what the target had before ("" none)
        std::string previous_utm;         // policy utm-status before ("enable"/"disable")
        bool created = false;             // reserved
        std::string at;
    };

    struct State {
        bool paused = false;
        std::string paused_at;
        std::string device_serial;
        std::optional<std::uint64_t> entry_limit;     // last reported by the device
        std::map<std::string, SourceCache, std::less<>> sources;   // key: list key (source id, or id.variant)
        std::map<std::string, std::string, std::less<>> selected;  // source id -> variant chosen last
        std::map<std::string, GroupState, std::less<>> groups;
        std::map<std::string, FeedState, std::less<>> feeds;
        std::map<std::string, Attachment, std::less<>> attachments;   // key "policy:<id>"
        std::optional<double> bytes_per_line;          // learned on this device
        std::vector<Measurement> measurements;         // the last few
        nlohmann::json last_run;

        [[nodiscard]] nlohmann::json to_json() const;
        static State from_json(const nlohmann::json& json);
    };

    class StateError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // Missing file: an empty State. Corrupt: StateError (never silently reset).
    State load_state(const Home& home);
    void save_state(const Home& home, const State& state);

    // The confirmed contents of a feed, sorted; nullopt when missing or not matching `sha256`.
    std::optional<std::vector<std::string>> load_feed_lines(const Home& home, std::string_view feed, std::string_view sha256);
    void save_feed_lines(const Home& home, std::string_view feed, const std::vector<std::string>& lines);
    void remove_feed_lines(const Home& home, std::string_view feed);

    // The journal: written before the first device write of a sync, removed after the last state
    // save. Its presence at startup means the previous sync stopped part-way.
    struct Journal {
        std::string started_at;
        std::string phase;                      // "cmdb", "push", "verify"
        std::vector<std::string> feeds;         // feeds this sync may have changed
        std::vector<std::string> plan_ids;
        [[nodiscard]] nlohmann::json to_json() const;
        static Journal from_json(const nlohmann::json& json);
    };
    std::optional<Journal> load_journal(const Home& home);
    void save_journal(const Home& home, const Journal& journal);
    void clear_journal(const Home& home);

    std::string now_iso8601();

}  // namespace fh

#endif  // FORTI_HOLE_STATE_HPP
