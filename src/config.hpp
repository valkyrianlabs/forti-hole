//
// forti-hole's configuration: what to block, for which DNS filter profiles, within what budget.
//
//   $FORTI_HOME/hole/config.json   (default ~/.forti/hole/config.json; --config overrides)
//
// The FortiGate connection is a forti profile (`forti configure`, forti-cli): this file names the
// profile, never a host or a secret. `setup` writes it with `"enabled": false`; nothing is written to
// the FortiGate until the operator sets it to true.
//

#ifndef FORTI_HOLE_CONFIG_HPP
#define FORTI_HOLE_CONFIG_HPP

#include "core/compile.hpp"
#include "core/parse.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fh {

    class ConfigError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // One upstream list. A source with variants (hagezi TIF: full, medium, mini) downloads the
    // selected one; "auto" tries `auto_order` and keeps the first that fits the device's budget.
    struct Source {
        std::string id;          // [a-z0-9][a-z0-9._-]{0,47}
        std::string tier;
        Format format = Format::adblock;
        std::string url;         // https:// or file:// (empty when variants are used)
        std::map<std::string, std::string, std::less<>> variants;   // name -> url
        std::string variant;     // a variants key, or "auto"
        std::vector<std::string> auto_order;
        bool enabled = true;

        [[nodiscard]] bool has_variants() const { return !variants.empty(); }
        // The variants a run must consider, in preference order.
        [[nodiscard]] std::vector<std::string> candidates() const;
        [[nodiscard]] std::string url_for(std::string_view variant_name) const;
    };

    struct AllowEntry {
        std::string name;
        Scope scope = Scope::exact;
        std::vector<std::string> profiles;   // empty: every profile
        std::string comment;
    };

    struct AllowRegex {
        std::string pattern;
        std::vector<std::string> profiles;
        std::string comment;
    };

    struct BlockEntry {
        std::string name;
        Scope scope = Scope::exact;
        std::string tier;
        std::string comment;
    };

    // Two budgets decide whether a compiled set may go to the device (budget.hpp):
    //   entries  the device's domain-entry limit (FortiOS 7.4.4+ reports it; 1,000,000 on entry-level
    //            models) minus other feeds' entries, keeping entry_headroom_percent free;
    //   memory   the device's RAM after the change, predicted from the lines added and removed times
    //            the cost of one line, must leave memory_headroom_percent free and stay below the
    //            conserve-mode red threshold.
    struct BudgetConfig {
        // Used when the device does not report its domain-entry limit (FortiOS before 7.4.4 or no
        // feed yet): the smallest documented limit (entry-level models).
        std::uint64_t fallback_limit = 1000000;
        double entry_headroom_percent = 10;
        double memory_headroom_percent = 15;
        // Device RAM one feed line costs. Empty: what forti-hole measured on this device in earlier
        // runs, else the conservative default (budget.hpp, default_bytes_per_line).
        std::optional<double> bytes_per_line;
        // "refuse": a set that does not fit is not pushed (the device keeps its current feeds).
        // "warn": it is pushed with a warning in the report and exit status.
        bool refuse = true;
    };

    struct FeedConfig {
        std::size_t max_entries = 131072;
        int category_min = 192, category_max = 221;
    };

    struct SyncConfig {
        // A source whose parsed entries drop by more than this against its last good copy is
        // rejected and the last good copy is used (a truncated or broken upstream).
        double max_shrink_percent = 20;
        std::size_t chunk_entries = 20000;     // entries per push request
        int load_timeout_seconds = 120;        // waiting for FortiOS to load a new feed or apply a push
        std::size_t verify_samples = 3;        // pages sampled per changed feed after a push
        std::size_t verify_page = 500;
    };

    struct FetchConfig {
        int timeout_seconds = 120;
        std::uint64_t max_bytes = 128ull << 20;
        std::size_t parallel = 6;
        std::string ca_bundle;                 // extra trusted CAs (PEM); verification stays on
    };

    struct DnsFilterConfig {
        bool manage_profiles = true;           // create and maintain <prefix><profile> DNS filter profiles
        std::string block_action = "block";    // block (NXDOMAIN) | redirect (block portal) | block-sevrfail
        // ftgd-dns "error-allow": answer normally when FortiGuard rating is unavailable (no licence,
        // no connectivity) instead of failing the query. On by default: forti-hole's feeds do not need
        // FortiGuard, and an unlicensed device must not stop resolving.
        bool error_allow = true;
        bool log = true;
    };

    struct ReferenceConfig {
        std::string url;                       // a list of real names (Tranco CSV), compact encoding only
        Format format = Format::tranco;
    };

    struct Config {
        std::filesystem::path path;            // where it was read from
        bool enabled = false;
        std::string profile;                   // forti profile; empty: the default profile
        std::string vdom;
        std::string prefix = "fh-";
        std::vector<std::string> tiers;
        std::vector<Source> sources;
        std::vector<ProfileSpec> profiles;
        std::vector<AllowEntry> allow;
        std::vector<AllowRegex> allow_regex;
        std::vector<BlockEntry> block;
        Encoding encoding = Encoding::strict;
        ReferenceConfig reference;
        BudgetConfig budget;
        FeedConfig feeds;
        SyncConfig sync;
        FetchConfig fetch;
        DnsFilterConfig dnsfilter;
    };

    inline constexpr std::string_view marker = "managed by forti-hole";

    // Parses and validates; throws ConfigError naming the offending key.
    Config parse_config(const nlohmann::json& json);
    Config load_config(const std::filesystem::path& path);
    nlohmann::json to_json(const Config& config);

    // What `setup` writes: the five lists the project defaults to, three profiles, disabled.
    nlohmann::json example_config();

    // The DNS filter profile name for a profile.
    [[nodiscard]] std::string dnsfilter_profile_name(const Config& config, std::string_view profile);

    CompileOptions compile_options(const Config& config);

}  // namespace fh

#endif  // FORTI_HOLE_CONFIG_HPP
