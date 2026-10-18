//
// The compiler: parsed lists in, FortiOS feed contents out.
//
// Tiers and profiles. Every source belongs to one tier ("base", "plus", "ultimate", "threat"). A
// profile (one FortiOS DNS filter profile) blocks a set of tiers. So each name ends up blocked by a
// set of profiles, and that set is what decides where it goes on the device.
//
// What compile() does, in order:
//
//   1. Deduplicates. One record per name across every source and tier; duplicates within a source and
//      across sources are counted, never stored twice.
//   2. Collapses. A name below a subtree entry is dropped for every profile the ancestor already
//      blocks it for (a.x.com under ||x.com^); an exact entry under its own subtree likewise. Only
//      the profiles that still need it keep it, so this never loses filtering.
//   3. Applies the allowlist. Exceptions remove names from the feeds. An exception below a blocked
//      subtree cannot be expressed in a feed (FortiOS feeds have no negation), so it is reported as a
//      static exception for the DNS filter profile's domain filter (profiles.hpp writes those).
//   4. Encodes. FortiOS matches feed lines as: "name" only that name; "*.name" only names below it;
//      "*name" the name, names below it, and any name merely ending in it ("*ads.com" hits
//      "uploads.com"). strict encoding writes a subtree as "name" + "*.name" (exact semantics);
//      compact writes "*name" where a reference list of real names (Tranco) shows no over-match,
//      and strict elsewhere. compact is opt-in.
//   5. Groups lines by the set of profiles that block them. Each distinct line is published exactly
//      once, in the feed group ("atom") of its profile set; every profile in the set blocks that
//      group's category. Nothing is sent to the device twice.
//   6. Partitions a group into feeds of at most max_entries_per_feed lines by a stable hash of the
//      name, so adding one name changes one feed, and sorts each feed. The bytes of a feed depend
//      only on its contents: its SHA-256 is reproducible anywhere.
//

#ifndef FORTI_HOLE_CORE_COMPILE_HPP
#define FORTI_HOLE_CORE_COMPILE_HPP

#include "core/parse.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fh {

    enum class Encoding { strict, compact };

    [[nodiscard]] std::string_view to_string(Encoding encoding);
    [[nodiscard]] std::optional<Encoding> parse_encoding(std::string_view text);

    struct ProfileSpec {
        std::string name;                 // [a-z0-9]{1,12}
        std::vector<std::string> tiers;   // the tiers this profile blocks
    };

    inline constexpr std::size_t max_tiers = 32, max_profiles = 32;

    struct CompileOptions {
        std::vector<std::string> tiers;   // in priority order
        std::vector<ProfileSpec> profiles;
        Encoding encoding = Encoding::strict;
        // Lines per device feed. 131,072 is the per-feed limit of FortiOS before 7.4.4 and keeps a
        // feed well inside every model's file-size limit after it.
        std::size_t max_entries_per_feed = 131072;
        std::string prefix = "fh-";
        // Feeds per group from the last run (group label -> count), so partitioning stays stable.
        std::map<std::string, std::size_t, std::less<>> partitions;
    };

    struct SourceReport {
        std::string id;
        std::string tier;
        ParseStats parse;
        std::uint64_t duplicates_within = 0;   // the same name and scope again in this source
        std::uint64_t duplicates_across = 0;   // already contributed by an earlier source
        std::uint64_t unique = 0;              // names (with scope) this source added first
    };

    struct CompiledFeed {
        std::string name;                  // device object name: <prefix><group>-<n>
        std::string group;                 // the profile-set label ("all", "standard_strict")
        std::uint32_t profile_mask = 0;
        std::vector<std::string> profiles;
        std::size_t partition = 1, partitions = 1;
        std::vector<std::string> lines;    // sorted, unique
        std::uint64_t bytes = 0;           // sum of line lengths + one newline each
        std::string sha256;                // over "line\n" for every line, in order
    };

    // An exception FortiOS feeds cannot express, for the profiles' static domain filter.
    struct StaticException {
        std::string pattern;               // the name, or a regular expression
        enum class Kind { exact, subtree, regex } kind = Kind::exact;
        std::vector<std::string> profiles;
    };

    struct CompileStats {
        std::uint64_t source_lines = 0;        // every line of every source
        std::uint64_t parsed = 0;              // valid entries parsed (block and allow)
        std::uint64_t invalid = 0;
        std::uint64_t unsupported = 0;
        std::uint64_t duplicates = 0;          // within and across sources
        std::uint64_t unique_entries = 0;      // distinct (name, scope) block entries after dedupe
        std::uint64_t collapsed = 0;           // entries dropped entirely: covered by an ancestor subtree
        std::uint64_t narrowed = 0;            // entries kept for fewer profiles than listed (partly covered)
        std::uint64_t allowlisted = 0;         // entries removed (for at least one profile) by an exception
        std::uint64_t unused_tier = 0;         // entries of a tier no profile blocks
        std::uint64_t names = 0;               // distinct names published
        std::uint64_t lines = 0;               // feed lines published (the device-entry count)
        std::uint64_t bytes = 0;
        std::uint64_t compact_lines = 0;       // "*name" lines (compact encoding)
        std::uint64_t compact_unsafe = 0;      // subtree names kept strict because they would over-match
        std::map<std::string, std::uint64_t, std::less<>> lines_per_profile;
        std::map<std::string, std::uint64_t, std::less<>> names_per_tier;   // distinct names listed in each tier
    };

    struct CompileResult {
        std::vector<CompiledFeed> feeds;       // sorted by name
        std::vector<StaticException> exceptions;
        std::vector<SourceReport> sources;
        CompileStats stats;
        // group label -> feeds, for the next run's CompileOptions::partitions
        std::map<std::string, std::size_t, std::less<>> partitions;
    };

    class CompileError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class Compiler {
    public:
        explicit Compiler(CompileOptions options);
        ~Compiler();
        Compiler(const Compiler&) = delete;
        Compiler& operator=(const Compiler&) = delete;

        // Parses one source into the tier. "@@" rules become exceptions for the profiles that block
        // this tier (an exception in a list applies where the list applies).
        const SourceReport& add_source(std::string id, std::string_view tier, std::string_view text, Format format);
        // Operator entries (config.json): block in a tier, allow for profiles (empty: every profile).
        void add_block(std::string_view name, Scope scope, std::string_view tier);
        void add_allow(std::string_view name, Scope scope, const std::vector<std::string>& profiles = {});
        // A POSIX extended regular expression, as Pi-hole uses: matching names are removed from the
        // feeds and the expression becomes a static exception.
        void add_allow_regex(std::string pattern, const std::vector<std::string>& profiles = {});
        // Real names compact encoding must not over-match (a Tranco top list).
        void set_reference(std::string_view text, Format format);

        // With `materialize` false only the statistics are computed (no feed lines): what a variant
        // choice needs to compare candidates, at a fraction of the memory.
        [[nodiscard]] CompileResult compile(bool materialize = true) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // The group label for a set of profiles: "all", or the names joined by '_' in config order;
    // a hex mask when that would make device names too long.
    [[nodiscard]] std::string group_label(std::uint32_t mask, const std::vector<ProfileSpec>& profiles, std::size_t max_length);

    // FNV-1a 64, the partition hash (stable across platforms and releases).
    [[nodiscard]] std::uint64_t stable_hash(std::string_view text);

    // Device name limit for system/external-resource.
    inline constexpr std::size_t max_feed_name = 35;

}  // namespace fh

#endif  // FORTI_HOLE_CORE_COMPILE_HPP
