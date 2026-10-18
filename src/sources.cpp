#include "sources.hpp"

#include "core/sha256.hpp"

#include <cmath>
#include <format>

namespace fh {

    std::string_view to_string(LoadedList::Origin origin) {
        switch (origin) {
            case LoadedList::Origin::downloaded: return "downloaded";
            case LoadedList::Origin::not_modified: return "not modified";
            case LoadedList::Origin::cached: return "last good copy";
            case LoadedList::Origin::unavailable: return "unavailable";
        }
        return "unavailable";
    }

    std::string list_key(std::string_view source_id, std::string_view variant) {
        return variant.empty() ? std::string(source_id) : std::format("{}.{}", source_id, variant);
    }

    namespace {

        struct Wanted {
            std::string key, source_id, variant, url;
            Format format = Format::adblock;
        };

        std::filesystem::path cache_path(const Home& home, std::string_view key) { return home.cache() / (std::string(key) + ".list"); }

        std::uint64_t count_entries(std::string_view text, Format format, ParseStats* out = nullptr) {
            auto stats = parse_list(text, format, [](std::string_view, Scope, bool) {});
            if (out) *out = stats;
            return stats.accepted;
        }

        // Why a fresh copy is not acceptable; empty when it is.
        std::string implausible(const FetchResponse& response, Format format, const SourceCache* cached, double max_shrink_percent,
                                std::uint64_t& entries) {
            if (response.content_type.find("text/html") != std::string::npos) return "the server sent an HTML page, not a list";
            std::string_view body = response.body;
            while (!body.empty() && (body.front() == ' ' || body.front() == '\n' || body.front() == '\r' || body.front() == '\t'))
                body.remove_prefix(1);
            if (body.starts_with("<")) return "the server sent markup, not a list";
            ParseStats stats;
            entries = count_entries(response.body, format, &stats);
            if (entries == 0) return "no valid entries";
            if (stats.declared) {
                auto declared = static_cast<double>(*stats.declared);
                auto seen = static_cast<double>(stats.accepted + stats.invalid + stats.unsupported);
                if (std::fabs(seen - declared) > std::max(10.0, declared * 0.02))
                    return std::format("{} entries but its header declares {} (truncated download?)", stats.accepted, *stats.declared);
            }
            if (cached && cached->entries > 0 && max_shrink_percent < 100) {
                double floor = static_cast<double>(cached->entries) * (1.0 - max_shrink_percent / 100.0);
                if (static_cast<double>(entries) < floor)
                    return std::format("{} entries, down from {} (more than {:g}% smaller; sync.max_shrink_percent)", entries, cached->entries,
                                       max_shrink_percent);
            }
            return {};
        }

    }  // namespace

    std::map<std::string, LoadedList, std::less<>> load_lists(const Config& config, const Home& home, State& state, Fetcher& fetcher,
                                                               const LoadOptions& options) {
        std::vector<Wanted> wanted;
        for (const auto& source: config.sources) {
            if (!source.enabled) continue;
            if (!source.has_variants()) {
                wanted.push_back({source.id, source.id, "", source.url, source.format});
                continue;
            }
            std::vector<std::string> names = source.candidates();
            if (options.all_variants) {
                names.clear();
                for (const auto& [name, url]: source.variants) names.push_back(name);
            }
            for (const auto& name: names) wanted.push_back({list_key(source.id, name), source.id, name, source.url_for(name), source.format});
        }
        if (config.encoding == Encoding::compact && !config.reference.url.empty())
            wanted.push_back({"reference", "reference", "", config.reference.url, config.reference.format});

        std::map<std::string, LoadedList, std::less<>> lists;
        std::vector<FetchRequest> requests;
        std::vector<std::size_t> requested;   // index into wanted
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            const auto& w = wanted[i];
            LoadedList list;
            list.key = w.key;
            list.source_id = w.source_id;
            list.variant = w.variant;
            list.url = w.url;
            lists[w.key] = std::move(list);
            if (options.offline) continue;
            FetchRequest request{w.url, {}, {}};
            // Conditional only when the cached copy is there and is the copy of this URL.
            if (auto it = state.sources.find(w.key); it != state.sources.end() && it->second.url == w.url && read_file(cache_path(home, w.key))) {
                request.etag = it->second.etag;
                request.last_modified = it->second.last_modified;
            }
            requests.push_back(std::move(request));
            requested.push_back(i);
        }

        std::vector<FetchResponse> responses = requests.empty() ? std::vector<FetchResponse>{} : fetcher.fetch(requests);
        std::map<std::string, const FetchResponse*, std::less<>> by_key;
        for (std::size_t r = 0; r < responses.size() && r < requested.size(); ++r) by_key[wanted[requested[r]].key] = &responses[r];

        for (const auto& w: wanted) {
            auto& list = lists[w.key];
            const SourceCache* cached = nullptr;
            if (auto it = state.sources.find(w.key); it != state.sources.end() && it->second.url == w.url) cached = &it->second;

            // The last good copy, verified against its recorded hash.
            auto use_cache = [&](std::string note) {
                auto text = read_file(cache_path(home, w.key));
                if (text && cached && sha256_hex(*text) == cached->sha256) {
                    list.text = std::move(*text);
                    list.origin = LoadedList::Origin::cached;
                    list.entries = cached->entries;
                    list.bytes = list.text.size();
                    list.note = std::move(note);
                } else {
                    list.origin = LoadedList::Origin::unavailable;
                    list.note = note + (text ? "; the cached copy does not match its recorded hash" : "; there is no last good copy");
                }
            };

            auto found = by_key.find(w.key);
            if (found == by_key.end()) {
                use_cache(options.offline ? "offline" : "not requested");
                if (options.offline && list.origin == LoadedList::Origin::cached) list.note.clear();
                continue;
            }
            const FetchResponse& response = *found->second;
            list.seconds = response.seconds;
            auto& record = state.sources[w.key];
            if (!response.transferred) {
                use_cache(std::format("download failed: {}", response.error));
                continue;
            }
            record.checked_at = now_iso8601();
            if (response.status == 304) {
                use_cache("");
                if (list.origin == LoadedList::Origin::cached) {
                    list.origin = LoadedList::Origin::not_modified;
                    list.note.clear();
                }
                continue;
            }
            if (response.status != 200) {
                use_cache(std::format("HTTP {}", response.status));
                continue;
            }
            std::uint64_t entries = 0;
            if (auto why = implausible(response, w.format, cached, config.sync.max_shrink_percent, entries); !why.empty()) {
                use_cache(std::format("rejected the new copy: {}", why));
                continue;
            }
            write_private(cache_path(home, w.key), response.body);
            record.url = w.url;
            record.etag = response.etag;
            record.last_modified = response.last_modified;
            record.sha256 = sha256_hex(response.body);
            record.entries = entries;
            record.bytes = response.body.size();
            record.fetched_at = record.checked_at;
            list.text = response.body;
            list.origin = LoadedList::Origin::downloaded;
            list.entries = entries;
            list.bytes = list.text.size();
        }
        return lists;
    }

}  // namespace fh
