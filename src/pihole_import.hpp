//
// `forti-hole import pihole`: turn a Pi-hole v6's blocklists, groups and exceptions into a
// forti-hole configuration. Read-only on the Pi-hole (GETs after logging in; the session is
// deleted afterwards). Nothing is written to the FortiGate.
//
// The mapping keeps Pi-hole's per-group filtering:
//   - every enabled Pi-hole group with at least one enabled blocklist becomes a forti-hole profile
//     (one FortiOS DNS filter profile);
//   - blocklists used by exactly the same groups form one tier, so each profile blocks the tiers of
//     its group's lists, and forti-hole's deduplication then publishes every name once;
//   - hagezi TIF (full, medium, mini) becomes a source with variants and "auto" selection;
//   - exact allow entries become exceptions (for the profiles of their groups), regex allow entries
//     allow_regex, exact deny entries block entries; regex deny entries cannot be expressed in a
//     feed and are reported.
//

#ifndef FORTI_HOLE_PIHOLE_IMPORT_HPP
#define FORTI_HOLE_PIHOLE_IMPORT_HPP

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fh {

    struct PiholeData {
        nlohmann::json lists = nlohmann::json::array();     // /api/lists "lists"
        nlohmann::json groups = nlohmann::json::array();    // /api/groups "groups"
        nlohmann::json domains = nlohmann::json::array();   // /api/domains "domains"
    };

    class PiholeReader {
    public:
        virtual ~PiholeReader() = default;
        virtual PiholeData read() = 0;
    };

    // The Pi-hole v6 API at `url` with an app password (or the web password).
    std::unique_ptr<PiholeReader> pihole_api(std::string url, std::string password, std::string ca_bundle);

    struct ImportResult {
        nlohmann::json config;               // a complete forti-hole config.json (disabled)
        std::vector<std::string> notes;      // guesses to check, entries that were not carried over
    };

    ImportResult import_pihole(const PiholeData& data);

}  // namespace fh

#endif  // FORTI_HOLE_PIHOLE_IMPORT_HPP
