//
// Upstream lists in, validated text out, with a last good copy behind every one of them.
//
// For each list a run needs (every enabled source; each candidate variant of a source with
// variants; the reference list for compact encoding):
//
//   1. ask upstream conditionally (If-None-Match / If-Modified-Since of the cached copy);
//   2. 304: the cached copy is current;
//   3. 200: accept the new copy only if it is plausible: not HTML, at least one valid entry, its
//      entry count within 2% of what its header declares (hagezi, StevenBlack), and not more than
//      sync.max_shrink_percent smaller than the last good copy. Then it replaces the cache;
//   4. anything else (transport error, 4xx/5xx, an implausible copy): the last good copy is used and
//      the run reports why. Without a last good copy the list is unavailable and the sync stops
//      before writing anything (fail-static: a feed is never emptied because a download failed).
//

#ifndef FORTI_HOLE_SOURCES_HPP
#define FORTI_HOLE_SOURCES_HPP

#include "config.hpp"
#include "fetch/fetch.hpp"
#include "files.hpp"
#include "state.hpp"

#include <map>
#include <string>
#include <vector>

namespace fh {

    struct LoadedList {
        std::string key;                 // source id, "id.variant", or "reference"
        std::string source_id, variant;
        std::string url;
        std::string text;                // empty when unavailable
        enum class Origin { downloaded, not_modified, cached, unavailable } origin = Origin::unavailable;
        std::string note;                // why the cache was used, or why the list is unavailable
        std::uint64_t entries = 0;       // valid entries (block and allow) in the text used
        std::uint64_t bytes = 0;
        double seconds = 0;
        [[nodiscard]] bool usable() const { return origin != Origin::unavailable; }
    };

    [[nodiscard]] std::string_view to_string(LoadedList::Origin origin);

    struct LoadOptions {
        bool offline = false;            // use cached copies only (compile --offline)
        bool all_variants = false;       // fetch every variant, not only the candidates (reports)
    };

    // Loads every list the configuration needs; updates the cache files and `state.sources`.
    std::map<std::string, LoadedList, std::less<>> load_lists(const Config& config, const Home& home, State& state, Fetcher& fetcher,
                                                               const LoadOptions& options = {});

    [[nodiscard]] std::string list_key(std::string_view source_id, std::string_view variant);

}  // namespace fh

#endif  // FORTI_HOLE_SOURCES_HPP
