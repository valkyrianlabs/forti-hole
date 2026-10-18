#include "core/compile.hpp"

#include "core/domain.hpp"
#include "core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace fh {

    std::string_view to_string(Encoding encoding) { return encoding == Encoding::strict ? "strict" : "compact"; }

    std::optional<Encoding> parse_encoding(std::string_view text) {
        if (text == "strict") return Encoding::strict;
        if (text == "compact") return Encoding::compact;
        return std::nullopt;
    }

    std::uint64_t stable_hash(std::string_view text) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (unsigned char c: text) {
            hash ^= c;
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    std::string group_label(std::uint32_t mask, const std::vector<ProfileSpec>& profiles, std::size_t max_length) {
        std::uint32_t all = profiles.size() >= 32 ? 0xFFFFFFFFu : (1u << profiles.size()) - 1;
        if (mask == all) return "all";
        std::string label;
        for (std::size_t i = 0; i < profiles.size(); ++i) {
            if (!(mask & (1u << i))) continue;
            if (!label.empty()) label += '_';
            label += profiles[i].name;
        }
        if (label.size() > max_length) label = std::format("p{:x}", mask);
        return label;
    }

    namespace {

        // Stable storage for names: map keys are views into it.
        class Arena {
            std::vector<std::unique_ptr<char[]>> blocks_;
            char* next_ = nullptr;
            std::size_t left_ = 0;
            static constexpr std::size_t block_size = 1u << 20;

        public:
            std::string_view store(std::string_view text) {
                if (text.size() > left_) {
                    std::size_t size = std::max(block_size, text.size());
                    blocks_.push_back(std::make_unique<char[]>(size));
                    next_ = blocks_.back().get();
                    left_ = size;
                }
                std::copy(text.begin(), text.end(), next_);
                std::string_view stored(next_, text.size());
                next_ += text.size();
                left_ -= text.size();
                return stored;
            }
        };

        struct Node {
            std::uint32_t exact_tiers = 0, subtree_tiers = 0;
            std::uint32_t last_source = 0xFFFFFFFFu;
            std::uint8_t last_scopes = 0;   // bit 0 exact, bit 1 subtree: what last_source contributed
        };

        struct AllowNode {
            std::uint32_t exact = 0, subtree = 0;   // profile masks
        };

        struct Line {
            std::string text;
            std::string_view base;   // the name it encodes (partition key)
        };

    }  // namespace

    struct Compiler::Impl {
        CompileOptions options;
        std::uint32_t all_profiles = 0;
        std::vector<std::uint32_t> tier_profiles;   // tier index -> profiles that block it
        Arena arena;
        std::unordered_map<std::string_view, Node> nodes;
        std::unordered_map<std::string_view, AllowNode> allows;
        std::vector<std::pair<std::regex, std::uint32_t>> allow_regexes;
        std::vector<std::pair<std::string, std::uint32_t>> allow_regex_text;
        std::unordered_set<std::string_view> reference;
        std::vector<SourceReport> sources;

        explicit Impl(CompileOptions o) : options(std::move(o)) {
            if (options.tiers.empty()) throw CompileError("no tiers");
            if (options.tiers.size() > max_tiers) throw CompileError(std::format("at most {} tiers", max_tiers));
            if (options.profiles.empty()) throw CompileError("no profiles");
            if (options.profiles.size() > max_profiles) throw CompileError(std::format("at most {} profiles", max_profiles));
            if (options.max_entries_per_feed < 2) throw CompileError("max_entries_per_feed must be at least 2");
            all_profiles = options.profiles.size() >= 32 ? 0xFFFFFFFFu : (1u << options.profiles.size()) - 1;
            tier_profiles.assign(options.tiers.size(), 0);
            for (std::size_t p = 0; p < options.profiles.size(); ++p)
                for (const auto& tier: options.profiles[p].tiers) tier_profiles[tier_index(tier)] |= 1u << p;
        }

        std::size_t tier_index(std::string_view tier) const {
            for (std::size_t i = 0; i < options.tiers.size(); ++i)
                if (options.tiers[i] == tier) return i;
            throw CompileError(std::format("unknown tier \"{}\"", tier));
        }

        std::uint32_t profile_mask(const std::vector<std::string>& names) const {
            if (names.empty()) return all_profiles;
            std::uint32_t mask = 0;
            for (const auto& name: names) {
                bool found = false;
                for (std::size_t p = 0; p < options.profiles.size(); ++p)
                    if (options.profiles[p].name == name) {
                        mask |= 1u << p;
                        found = true;
                    }
                if (!found) throw CompileError(std::format("unknown profile \"{}\"", name));
            }
            return mask;
        }

        std::uint32_t profiles_of(std::uint32_t tiers) const {
            std::uint32_t mask = 0;
            while (tiers) {
                auto bit = static_cast<std::size_t>(std::countr_zero(tiers));
                mask |= tier_profiles[bit];
                tiers &= tiers - 1;
            }
            return mask;
        }

        std::vector<std::string> profile_names(std::uint32_t mask) const {
            std::vector<std::string> names;
            for (std::size_t p = 0; p < options.profiles.size(); ++p)
                if (mask & (1u << p)) names.push_back(options.profiles[p].name);
            return names;
        }

        // Inserts a block entry; returns 0 new, 1 duplicate within the source, 2 duplicate across sources.
        int insert(std::string_view name, Scope scope, std::size_t tier, std::uint32_t source) {
            std::uint8_t bit = scope == Scope::exact ? 1 : 2;
            auto it = nodes.find(name);
            if (it == nodes.end()) it = nodes.emplace(arena.store(name), Node{}).first;
            Node& node = it->second;
            std::uint32_t& tiers = scope == Scope::exact ? node.exact_tiers : node.subtree_tiers;
            int result = 0;
            if (node.last_source == source && (node.last_scopes & bit)) result = 1;
            else if (tiers != 0) result = 2;
            if (node.last_source != source) {
                node.last_source = source;
                node.last_scopes = 0;
            }
            node.last_scopes |= bit;
            tiers |= 1u << tier;
            return result;
        }

        void allow(std::string_view name, Scope scope, std::uint32_t profiles) {
            auto it = allows.find(name);
            if (it == allows.end()) it = allows.emplace(arena.store(name), AllowNode{}).first;
            (scope == Scope::exact ? it->second.exact : it->second.subtree) |= profiles;
        }

        // Profiles that a subtree entry strictly above `name` blocks it for.
        std::uint32_t covered_above(std::string_view name) const {
            std::uint32_t covered = 0;
            for (auto parent = parent_of(name); parent.find('.') != std::string_view::npos; parent = parent_of(parent))
                if (auto it = nodes.find(parent); it != nodes.end()) covered |= profiles_of(it->second.subtree_tiers);
            return covered;
        }

        // Profiles for which a subtree exception covers `name` (on the name itself or above it).
        std::uint32_t allowed_subtree(std::string_view name) const {
            if (allows.empty()) return 0;
            std::uint32_t allowed = 0;
            for (auto at = name; !at.empty(); at = parent_of(at))
                if (auto it = allows.find(at); it != allows.end()) allowed |= it->second.subtree;
            return allowed;
        }

        std::uint32_t allowed_exact(std::string_view name) const {
            if (allows.empty()) return 0;
            auto it = allows.find(name);
            return it == allows.end() ? 0 : it->second.exact;
        }
    };

    Compiler::Compiler(CompileOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
    Compiler::~Compiler() = default;

    const SourceReport& Compiler::add_source(std::string id, std::string_view tier, std::string_view text, Format format) {
        auto& impl = *impl_;
        std::size_t t = impl.tier_index(tier);
        auto source = static_cast<std::uint32_t>(impl.sources.size());
        SourceReport report;
        report.id = std::move(id);
        report.tier = std::string(tier);
        std::uint32_t list_profiles = impl.tier_profiles[t];
        report.parse = parse_list(text, format, [&](std::string_view name, Scope scope, bool allow) {
            if (allow) {
                if (list_profiles) impl.allow(name, Scope::subtree, list_profiles);
                return;
            }
            switch (impl.insert(name, scope, t, source)) {
                case 0: ++report.unique; break;
                case 1: ++report.duplicates_within; break;
                default: ++report.duplicates_across; break;
            }
        });
        impl.sources.push_back(std::move(report));
        return impl.sources.back();
    }

    void Compiler::add_block(std::string_view name, Scope scope, std::string_view tier) {
        auto canonical = canonical_name(name);
        if (!canonical.ok()) throw CompileError(std::format("block entry \"{}\": {}", name, to_string(*canonical.error)));
        impl_->insert(canonical.name, scope, impl_->tier_index(tier), 0xFFFFFFFEu);
    }

    void Compiler::add_allow(std::string_view name, Scope scope, const std::vector<std::string>& profiles) {
        auto canonical = canonical_name(name);
        if (!canonical.ok()) throw CompileError(std::format("allow entry \"{}\": {}", name, to_string(*canonical.error)));
        impl_->allow(canonical.name, scope, impl_->profile_mask(profiles));
    }

    void Compiler::add_allow_regex(std::string pattern, const std::vector<std::string>& profiles) {
        std::uint32_t mask = impl_->profile_mask(profiles);
        try {
            impl_->allow_regexes.emplace_back(std::regex(pattern, std::regex::extended | std::regex::nosubs | std::regex::optimize), mask);
        } catch (const std::regex_error& error) {
            throw CompileError(std::format("allow regex \"{}\": {}", pattern, error.what()));
        }
        impl_->allow_regex_text.emplace_back(std::move(pattern), mask);
    }

    void Compiler::set_reference(std::string_view text, Format format) {
        auto& impl = *impl_;
        parse_list(text, format, [&](std::string_view name, Scope, bool allow) {
            if (!allow && !impl.reference.contains(name)) impl.reference.insert(impl.arena.store(name));
        });
    }

    CompileResult Compiler::compile(bool materialize) const {
        const auto& impl = *impl_;
        const auto& options = impl.options;
        CompileResult result;
        result.sources = impl.sources;
        auto& stats = result.stats;
        for (const auto& source: impl.sources) {
            stats.source_lines += source.parse.lines;
            stats.parsed += source.parse.accepted;
            stats.invalid += source.parse.invalid;
            stats.unsupported += source.parse.unsupported;
            stats.duplicates += source.duplicates_within + source.duplicates_across;
        }

        // Effective profile sets per line kind: "name" (exact), "*.name" (below), or both via
        // subtree; computed once per node.
        struct Effective {
            std::string_view name;
            std::uint32_t exact = 0, below = 0;   // profiles that need "name" / need "*.name"
            std::uint32_t subtree = 0;            // of those, profiles where both come from one subtree entry
        };
        std::vector<Effective> effective;
        effective.reserve(impl.nodes.size());
        std::vector<std::uint64_t> tier_names(options.tiers.size(), 0);

        for (const auto& [name, node]: impl.nodes) {
            for (std::size_t t = 0; t < options.tiers.size(); ++t)
                if ((node.exact_tiers | node.subtree_tiers) & (1u << t)) ++tier_names[t];
            std::uint32_t listed_exact = impl.profiles_of(node.exact_tiers), listed_subtree = impl.profiles_of(node.subtree_tiers);
            stats.unique_entries += (node.exact_tiers ? 1 : 0) + (node.subtree_tiers ? 1 : 0);
            if ((node.exact_tiers && !listed_exact) || (node.subtree_tiers && !listed_subtree)) ++stats.unused_tier;

            std::uint32_t covered = impl.covered_above(name);
            std::uint32_t subtree = listed_subtree & ~covered;
            std::uint32_t exact = listed_exact & ~covered & ~listed_subtree;
            // An entry is collapsed when nothing of it is needed (an ancestor's subtree, or the name's
            // own subtree entry, covers every profile), narrowed when only some profiles still need it.
            if (listed_subtree) {
                if (!subtree) ++stats.collapsed;
                else if (subtree != listed_subtree) ++stats.narrowed;
            }
            if (listed_exact) {
                if (!exact) ++stats.collapsed;
                else if (exact != listed_exact) ++stats.narrowed;
            }

            // Exceptions. A subtree exception on the name or above removes it outright; an exact
            // exception keeps the names below a subtree entry but not the name itself; a regex removes
            // the name for its profiles.
            Effective e{name, exact | subtree, subtree, subtree};
            std::uint32_t before = e.exact | e.below;
            if (std::uint32_t gone = impl.allowed_subtree(name)) {
                e.exact &= ~gone;
                e.below &= ~gone;
                e.subtree &= ~gone;
            }
            if (std::uint32_t gone = impl.allowed_exact(name)) {
                e.exact &= ~gone;
                e.subtree &= ~gone;   // those profiles keep only "*.name"
            }
            for (const auto& [regex, mask]: impl.allow_regexes) {
                if (!((e.exact | e.below) & mask)) continue;
                // The expression matched this name, not necessarily the names below it: those keep
                // "*.name", and the static regex exception lets through any of them it matches.
                if (std::regex_search(name.begin(), name.end(), regex)) {
                    e.exact &= ~mask;
                    e.subtree &= ~mask;
                }
            }
            if ((e.exact | e.below) != before) ++stats.allowlisted;
            if (e.exact | e.below) effective.push_back(e);
        }
        for (std::size_t t = 0; t < options.tiers.size(); ++t) stats.names_per_tier[options.tiers[t]] = tier_names[t];
        stats.names = effective.size();

        // Static exceptions: allows that sit below a blocked subtree (a feed cannot carve them out).
        for (const auto& [name, allow]: impl.allows) {
            std::uint32_t blocked_above = 0;
            for (auto parent = parent_of(name); parent.find('.') != std::string_view::npos; parent = parent_of(parent))
                if (auto it = impl.nodes.find(parent); it != impl.nodes.end())
                    blocked_above |= impl.profiles_of(it->second.subtree_tiers) & ~impl.allowed_subtree(parent);
            for (auto [mask, kind]: {std::pair{allow.exact, StaticException::Kind::exact}, std::pair{allow.subtree, StaticException::Kind::subtree}}) {
                std::uint32_t need = mask & blocked_above;
                if (need) result.exceptions.push_back({std::string(name), kind, impl.profile_names(need)});
            }
        }
        for (const auto& [pattern, mask]: impl.allow_regex_text)
            result.exceptions.push_back({pattern, StaticException::Kind::regex, impl.profile_names(mask)});
        std::sort(result.exceptions.begin(), result.exceptions.end(),
                  [](const StaticException& a, const StaticException& b) { return std::tie(a.pattern, a.kind) < std::tie(b.pattern, b.kind); });

        // Compact encoding: a subtree name is unsafe as "*name" when a real name ends in it without a
        // label boundary ("uploads.com" for "ads.com").
        std::unordered_set<std::string_view> unsafe;
        if (options.encoding == Encoding::compact) {
            if (impl.reference.empty())
                throw CompileError("compact encoding needs a reference list of real names (encoding.reference) to check over-matching");
            std::unordered_set<std::string_view> candidates;
            for (const auto& e: effective)
                if (e.subtree) candidates.insert(e.name);
            for (std::string_view real: impl.reference)
                for (std::size_t i = 1; i < real.size(); ++i)
                    if (real[i - 1] != '.' && candidates.contains(real.substr(i))) unsafe.insert(real.substr(i));
            stats.compact_unsafe = unsafe.size();
        }

        if (!materialize) {
            // The line count per profile set, exactly as the materialized path below produces it.
            std::map<std::uint32_t, std::uint64_t> counts;
            auto count = [&](std::uint32_t mask) {
                if (mask) ++counts[mask];
            };
            for (const auto& e: effective) {
                if (options.encoding == Encoding::compact && e.subtree && !unsafe.contains(e.name)) {
                    count(e.subtree);
                    ++stats.compact_lines;
                    count(e.exact & ~e.subtree);
                    count(e.below & ~e.subtree);
                } else {
                    count(e.exact);
                    count(e.below);
                }
            }
            for (const auto& [mask, lines]: counts) {
                stats.lines += lines;
                for (std::size_t p = 0; p < options.profiles.size(); ++p)
                    if (mask & (1u << p)) stats.lines_per_profile[options.profiles[p].name] += lines;
            }
            return result;
        }

        // Lines, grouped by the exact set of profiles that block each one.
        std::map<std::uint32_t, std::vector<Line>> groups;
        auto add_line = [&](std::uint32_t mask, std::string text, std::string_view base) {
            if (mask) groups[mask].push_back({std::move(text), base});
        };
        for (const auto& e: effective) {
            if (options.encoding == Encoding::compact && e.subtree && !unsafe.contains(e.name)) {
                // "*name" covers the name and everything below it for the subtree profiles; the
                // others still need their own lines.
                add_line(e.subtree, "*" + std::string(e.name), e.name);
                ++stats.compact_lines;
                add_line(e.exact & ~e.subtree, std::string(e.name), e.name);
                add_line(e.below & ~e.subtree, "*." + std::string(e.name), e.name);
            } else {
                add_line(e.exact, std::string(e.name), e.name);
                add_line(e.below, "*." + std::string(e.name), e.name);
            }
        }

        // Feeds: each group split into partitions by a stable hash of the name.
        std::size_t label_room = max_feed_name - options.prefix.size() - 4;   // "-NNN"
        for (auto& [mask, lines]: groups) {
            std::string label = group_label(mask, options.profiles, label_room);
            std::size_t count = std::max<std::size_t>(1, (lines.size() + options.max_entries_per_feed - 1) / options.max_entries_per_feed);
            if (auto it = options.partitions.find(label); it != options.partitions.end()) count = std::max(count, it->second);
            std::vector<std::vector<std::string>> parts;
            while (true) {
                parts.assign(count, {});
                for (auto& line: lines) parts[stable_hash(line.base) % count].push_back(line.text);
                bool fits = std::all_of(parts.begin(), parts.end(), [&](const auto& part) { return part.size() <= options.max_entries_per_feed; });
                if (fits) break;
                ++count;
            }
            if (count > 999) throw CompileError(std::format("group {} needs {} feeds; raise max_entries_per_feed", label, count));
            result.partitions[label] = count;
            for (std::size_t i = 0; i < count; ++i) {
                CompiledFeed feed;
                feed.name = std::format("{}{}-{}", options.prefix, label, i + 1);
                feed.group = label;
                feed.profile_mask = mask;
                feed.profiles = impl.profile_names(mask);
                feed.partition = i + 1;
                feed.partitions = count;
                feed.lines = std::move(parts[i]);
                std::sort(feed.lines.begin(), feed.lines.end());
                // Lines are unique by construction (one record per name, one group per line kind);
                // the check keeps that a guarantee rather than an assumption.
                if (std::adjacent_find(feed.lines.begin(), feed.lines.end()) != feed.lines.end())
                    throw CompileError(std::format("internal: duplicate line in {}", feed.name));
                Sha256 hash;
                for (const auto& line: feed.lines) {
                    hash.update(line);
                    hash.update("\n");
                    feed.bytes += line.size() + 1;
                }
                feed.sha256 = hash.hex();
                stats.lines += feed.lines.size();
                stats.bytes += feed.bytes;
                for (const auto& profile: feed.profiles) stats.lines_per_profile[profile] += feed.lines.size();
                result.feeds.push_back(std::move(feed));
            }
        }
        std::sort(result.feeds.begin(), result.feeds.end(), [](const CompiledFeed& a, const CompiledFeed& b) { return a.name < b.name; });
        return result;
    }

}  // namespace fh
