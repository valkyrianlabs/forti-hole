//
// forti-hole 0.x: its config.yaml and the feeds it left on the device.
//
// `migrate config <config.yaml>` reads the old file's lists, security levels and DNS filters into a
// new config.json. The old file's `fortigate:` section (API key, certificate password) is skipped
// without being stored or printed: the connection is a forti profile now (`forti configure`).
//
// `migrate feeds` finds the old tool's feeds (named <naming prefix>_<security level>-N_<part>-M,
// "forti-hole_security_level-0_part-1" by default), shows them, and with --apply frees their
// categories in the profiles that list them and deletes them. Nothing else is touched.
//

#ifndef FORTI_HOLE_LEGACY_HPP
#define FORTI_HOLE_LEGACY_HPP

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fh {

    class LegacyError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // The subset of YAML the 0.x config uses (mappings, sequences, scalars, comments, quotes), as
    // JSON. Top-level keys in `skip` are dropped as soon as they are read.
    nlohmann::json parse_legacy_yaml(std::string_view text, const std::vector<std::string>& skip = {"fortigate"});

    struct LegacyConfig {
        nlohmann::json config;              // a forti-hole config.json (disabled)
        std::string feed_prefix;            // the old feeds' name prefix ("forti-hole_")
        std::vector<std::string> notes;     // e.g. the firewall policies to attach
    };

    LegacyConfig convert_legacy_config(const nlohmann::json& legacy);

}  // namespace fh

#endif  // FORTI_HOLE_LEGACY_HPP
