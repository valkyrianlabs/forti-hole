#include "report.hpp"

#include <format>

namespace fh {

    using json = nlohmann::json;

    std::string grouped(std::uint64_t value) {
        std::string digits = std::to_string(value), out;
        for (std::size_t i = 0; i < digits.size(); ++i) {
            if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return out;
    }

    namespace {

        std::string mb(double bytes) { return std::format("{:.0f} MB", bytes / 1e6); }

        json parse_json(const ParseStats& p) {
            json j = {{"lines", p.lines},       {"comments", p.comments},       {"blank", p.blank},           {"rules", p.rules},
                      {"valid", p.accepted},    {"allow", p.allow},             {"canonicalized", p.canonicalized},
                      {"invalid", p.invalid},   {"unsupported", p.unsupported}, {"ignored", p.ignored}};
            if (p.declared) j["declared"] = *p.declared;
            if (!p.invalid_reasons.empty()) j["invalid_reasons"] = p.invalid_reasons;
            if (!p.first_invalid.empty()) j["first_invalid"] = p.first_invalid;
            if (!p.first_unsupported.empty()) j["first_unsupported"] = p.first_unsupported;
            return j;
        }

    }  // namespace

    json lists_json(const std::map<std::string, LoadedList, std::less<>>& lists) {
        json out = json::object();
        for (const auto& [key, list]: lists) {
            json j = {{"url", list.url}, {"origin", std::string(to_string(list.origin))}, {"entries", list.entries}, {"bytes", list.bytes}};
            if (!list.note.empty()) j["note"] = list.note;
            if (list.seconds > 0) j["seconds"] = list.seconds;
            out[key] = std::move(j);
        }
        return out;
    }

    std::string lists_text(const std::map<std::string, LoadedList, std::less<>>& lists) {
        std::string out = "Lists\n";
        for (const auto& [key, list]: lists) {
            out += std::format("  {:<28} {:<15} {:>11} entries {:>7}", key, to_string(list.origin), grouped(list.entries), mb(static_cast<double>(list.bytes)));
            if (!list.note.empty()) out += "  (" + list.note + ")";
            out += "\n";
        }
        return out;
    }

    json budget_json(const BudgetCheck& b) {
        json j = {{"lines", b.lines},
                  {"fits", b.fits()},
                  {"entries",
                   {{"limit", b.entry_limit},
                    {"limit_reported_by_device", b.entry_limit_reported},
                    {"other_feeds", b.other_entries},
                    {"headroom_percent", b.entry_headroom_percent},
                    {"available", b.entry_available},
                    {"fits", b.entries_fit}}}};
        if (b.memory_known)
            j["memory"] = {{"bytes_per_line", b.bytes_per_line},
                           {"delta_bytes", b.memory_delta},
                           {"used_percent", b.memory_used_percent},
                           {"after_percent", b.memory_after_percent},
                           {"ceiling_percent", b.memory_ceiling_percent},
                           {"fits", b.memory_fits}};
        else
            j["memory"] = nullptr;
        if (!b.fits()) j["reason"] = b.reason();
        if (!b.warnings.empty()) j["warnings"] = b.warnings;
        return j;
    }

    json compile_json(const CompileRun& run, bool lines) {
        const auto& r = run.result;
        const auto& s = r.stats;
        json sources = json::array();
        for (const auto& source: r.sources)
            sources.push_back({{"id", source.id},
                               {"tier", source.tier},
                               {"parse", parse_json(source.parse)},
                               {"duplicates_within", source.duplicates_within},
                               {"duplicates_across", source.duplicates_across},
                               {"unique", source.unique}});
        json feeds = json::array();
        for (const auto& feed: r.feeds) {
            json f = {{"name", feed.name},   {"group", feed.group},   {"profiles", feed.profiles}, {"partition", feed.partition},
                      {"partitions", feed.partitions}, {"lines", feed.lines.size()}, {"bytes", feed.bytes}, {"sha256", feed.sha256}};
            if (lines) f["entries"] = feed.lines;
            feeds.push_back(std::move(f));
        }
        json exceptions = json::array();
        for (const auto& e: r.exceptions)
            exceptions.push_back({{"pattern", e.pattern},
                                  {"kind", e.kind == StaticException::Kind::exact ? "exact" : e.kind == StaticException::Kind::subtree ? "subtree" : "regex"},
                                  {"profiles", e.profiles}});
        json attempts = json::array();
        for (const auto& a: run.attempts) {
            json j = {{"source", a.source_id}, {"variant", a.variant}, {"chosen", a.chosen}};
            if (!a.skipped.empty()) j["skipped"] = a.skipped;
            else {
                j["lines"] = a.lines;
                j["names"] = a.names;
                j["budget"] = budget_json(a.budget);
            }
            attempts.push_back(std::move(j));
        }
        return {{"sources", sources},
                {"totals",
                 {{"source_lines", s.source_lines},
                  {"valid_parsed", s.parsed},
                  {"invalid", s.invalid},
                  {"unsupported", s.unsupported},
                  {"duplicates_removed", s.duplicates},
                  {"unique_entries", s.unique_entries},
                  {"parent_subdomain_removed", s.collapsed},
                  {"narrowed", s.narrowed},
                  {"allowlisted", s.allowlisted},
                  {"unused_tier", s.unused_tier},
                  {"unique_names", s.names},
                  {"emitted_lines", s.lines},
                  {"emitted_bytes", s.bytes},
                  {"compact_lines", s.compact_lines},
                  {"compact_unsafe", s.compact_unsafe},
                  {"lines_per_profile", s.lines_per_profile},
                  {"names_per_tier", s.names_per_tier}}},
                {"feeds", feeds},
                {"exceptions", exceptions},
                {"selected", run.selected},
                {"variant_attempts", attempts},
                {"budget", budget_json(run.budget)},
                {"warnings", run.warnings},
                {"seconds", run.seconds}};
    }

    std::string compile_text(const CompileRun& run) {
        const auto& r = run.result;
        const auto& s = r.stats;
        std::string out = "Sources                       tier        lines        valid   invalid  unsupp.   dup.within   dup.across       unique\n";
        for (const auto& source: r.sources)
            out += std::format("  {:<27} {:<9} {:>9} {:>12} {:>9} {:>8} {:>12} {:>12} {:>12}\n", source.id, source.tier, grouped(source.parse.lines),
                               grouped(source.parse.accepted), grouped(source.parse.invalid), grouped(source.parse.unsupported),
                               grouped(source.duplicates_within), grouped(source.duplicates_across), grouped(source.unique));
        out += "\nDeduplication\n";
        out += std::format("  valid entries parsed           {:>12}\n", grouped(s.parsed));
        out += std::format("  duplicates removed             {:>12}  (within and across sources)\n", grouped(s.duplicates));
        out += std::format("  unique entries                 {:>12}\n", grouped(s.unique_entries));
        out += std::format("  covered by a parent subtree    {:>12}  (dropped; {} more kept for fewer profiles)\n", grouped(s.collapsed), grouped(s.narrowed));
        if (s.allowlisted) out += std::format("  removed by exceptions          {:>12}\n", grouped(s.allowlisted));
        out += std::format("  unique names published         {:>12}\n", grouped(s.names));
        out += std::format("  FortiOS feed lines             {:>12}  ({}){}\n", grouped(s.lines), mb(static_cast<double>(s.bytes)),
                           s.compact_lines ? std::format(", {} compact", grouped(s.compact_lines)) : "");
        out += "\nPer profile: ";
        bool first = true;
        for (const auto& [profile, lines]: s.lines_per_profile) {
            out += std::format("{}{} {}", first ? "" : ", ", profile, grouped(lines));
            first = false;
        }
        out += " lines\n\nFeeds\n";
        for (const auto& feed: r.feeds)
            out += std::format("  {:<24} {:>9} lines  sha256 {}  ({})\n", feed.name, grouped(feed.lines.size()), feed.sha256.substr(0, 16),
                               [&] {
                                   std::string p;
                                   for (const auto& name: feed.profiles) p += (p.empty() ? "" : " ") + name;
                                   return p;
                               }());
        if (!run.attempts.empty()) {
            out += "\nVariant choice\n";
            for (const auto& a: run.attempts) {
                if (!a.skipped.empty()) {
                    out += std::format("  {} {}: skipped ({})\n", a.source_id, a.variant, a.skipped);
                    continue;
                }
                out += std::format("  {} {}: {} lines, {}{}\n", a.source_id, a.variant, grouped(a.lines), a.budget.fits() ? "fits" : "does not fit: " + a.budget.reason(),
                                   a.chosen ? "  <- chosen" : "");
            }
        }
        const auto& b = run.budget;
        out += "\nBudget\n";
        out += std::format("  entries: {} of {} available ({} limit{}, {} in other feeds, {:g}% headroom) -> {}\n", grouped(b.lines),
                           grouped(b.entry_available), grouped(b.entry_limit), b.entry_limit_reported ? " reported by the device" : " assumed",
                           grouped(b.other_entries), b.entry_headroom_percent, b.entries_fit ? "fits" : "DOES NOT FIT");
        if (b.memory_known)
            out += std::format("  memory:  {:.1f}% used now, {:.1f}% predicted ({:+.0f} MB at {:.0f} bytes/line), ceiling {:.1f}% -> {}\n",
                               b.memory_used_percent, b.memory_after_percent, static_cast<double>(b.memory_delta) / 1e6, b.bytes_per_line,
                               b.memory_ceiling_percent, b.memory_fits ? "fits" : "DOES NOT FIT");
        else
            out += "  memory:  unknown (no device figures; run with the device reachable)\n";
        if (!r.exceptions.empty()) {
            out += "\nExceptions FortiOS feeds cannot express (allow entries below blocked subtrees)\n";
            for (const auto& e: r.exceptions) out += std::format("  {}\n", e.pattern);
        }
        for (const auto& w: run.warnings) out += "warning: " + w + "\n";
        out += std::format("\nCompiled in {:.2f} s\n", run.seconds);
        return out;
    }

    json plan_json(const SyncPlan& plan) {
        json feeds = json::array();
        for (const auto& a: plan.feeds) {
            json j = {{"feed", a.feed}, {"action", std::string(to_string(a.kind))}, {"category", a.category},
                      {"lines_device", a.lines_device}, {"lines_after", a.lines_after}, {"reason", a.reason}};
            if (!a.group.empty()) j["group"] = a.group;
            if (a.kind == FeedAction::Kind::delta) {
                j["adds"] = a.adds.size();
                j["removes"] = a.removes.size();
            }
            feeds.push_back(std::move(j));
        }
        json profiles = json::array();
        for (const auto& p: plan.profiles)
            profiles.push_back({{"profile", p.name},
                                {"action", p.kind == ProfileAction::Kind::create ? "create" : p.kind == ProfileAction::Kind::update ? "update" : "unchanged"},
                                {"categories", p.categories}});
        return {{"feeds", feeds},
                {"profiles", profiles},
                {"categories", plan.categories},
                {"requests",
                 {{"configuration", plan.phase1.size()}, {"pushes", plan.pushes.size()}, {"cleanup", plan.phase3.size()}}},
                {"lines_after", plan.lines_after},
                {"snapshot_lines", plan.snapshot_lines},
                {"adds", plan.adds},
                {"removes", plan.removes},
                {"writes", plan.writes()},
                {"notes", plan.notes}};
    }

    std::string plan_text(const SyncPlan& plan) {
        std::string out = "Feeds\n";
        for (const auto& a: plan.feeds) {
            out += std::format("  {:<24} {:<9} cat {:>3}  {:>9} -> {:>9}  {}\n", a.feed, to_string(a.kind), a.category, grouped(a.lines_device),
                               grouped(a.lines_after), a.reason);
        }
        if (!plan.profiles.empty()) {
            out += "DNS filter profiles\n";
            for (const auto& p: plan.profiles) {
                std::string cats;
                for (int c: p.categories) cats += (cats.empty() ? "" : " ") + std::to_string(c);
                out += std::format("  {:<24} {:<9} blocks categories {}\n", p.name,
                                   p.kind == ProfileAction::Kind::create ? "create" : p.kind == ProfileAction::Kind::update ? "update" : "unchanged", cats);
            }
        }
        if (!plan.writes()) out += "\nNothing to write: the device already holds the compiled set.\n";
        else
            out += std::format("\nRequests: {} configuration, {} push ({} snapshot lines, {} additions, {} removals), {} cleanup\n", plan.phase1.size(),
                               plan.pushes.size(), grouped(plan.snapshot_lines), grouped(plan.adds), grouped(plan.removes), plan.phase3.size());
        for (const auto& note: plan.notes) out += "note: " + note + "\n";
        return out;
    }

    json outcome_json(const SyncOutcome& o) {
        json verified = json::array();
        for (const auto& v: o.verified) {
            json j = {{"feed", v.feed},       {"verified", v.verified}, {"expected", v.expected}, {"accepted", v.accepted},
                      {"valid", v.valid},     {"invalid", v.invalid},   {"overflow", v.overflow}, {"sampled", v.sampled},
                      {"sample_mismatches", v.sample_mismatches}};
            if (!v.problem.empty()) j["problem"] = v.problem;
            verified.push_back(std::move(j));
        }
        json j = {{"plan_ids", o.plan_ids}, {"verification", verified}, {"deleted", o.deleted}, {"problems", o.problems}};
        if (o.measurement)
            j["memory_measurement"] = {{"lines_before", o.measurement->lines_before},
                                       {"lines_after", o.measurement->lines_after},
                                       {"used_before", o.measurement->used_before},
                                       {"used_after", o.measurement->used_after},
                                       {"bytes_per_line", o.measurement->bytes_per_line}};
        return j;
    }

    std::string outcome_text(const SyncOutcome& o) {
        std::string out;
        for (const auto& v: o.verified)
            out += std::format("  {:<24} {}  device {} accepted, {} sampled{}\n", v.feed, v.verified ? "verified" : "NOT VERIFIED", grouped(v.accepted),
                               grouped(v.sampled), v.problem.empty() ? "" : ": " + v.problem);
        for (const auto& name: o.deleted) out += std::format("  {:<24} deleted\n", name);
        if (o.measurement)
            out += std::format("Memory: {} -> {} used for {} -> {} lines: {:.0f} bytes per line\n", mb(static_cast<double>(o.measurement->used_before)),
                               mb(static_cast<double>(o.measurement->used_after)), grouped(o.measurement->lines_before),
                               grouped(o.measurement->lines_after), o.measurement->bytes_per_line);
        for (const auto& p: o.problems) out += "problem: " + p + "\n";
        return out;
    }

    json device_json(const Inventory& i) {
        json feeds = json::array();
        for (const auto& f: i.feeds) {
            json j = {{"name", f.name}, {"type", f.type}, {"update_method", f.update_method}, {"category", f.category}, {"owned", f.owned}};
            if (f.state)
                j["state"] = {{"loaded", f.state->loaded}, {"accepted", f.state->accepted}, {"valid", f.state->valid}, {"invalid", f.state->invalid},
                              {"overflow", f.state->overflow}, {"last_update", f.state->last_update}};
            feeds.push_back(std::move(j));
        }
        json j = {{"serial", i.serial}, {"version", i.version}, {"hostname", i.hostname}, {"model", i.model}, {"feeds", feeds},
                  {"owned_entries", i.facts.owned_entries}, {"other_entries", i.facts.other_entries}};
        j["entry_limit"] = i.facts.entry_limit ? json(*i.facts.entry_limit) : json(nullptr);
        if (i.facts.memory_total)
            j["memory"] = {{"total", *i.facts.memory_total}, {"used", *i.facts.memory_used},
                           {"red_percent", i.facts.memory_red_percent ? json(*i.facts.memory_red_percent) : json(nullptr)},
                           {"peak_24h_percent", i.facts.memory_peak_percent ? json(*i.facts.memory_peak_percent) : json(nullptr)}};
        if (i.fortiguard_web_filtering) j["fortiguard_web_filtering"] = *i.fortiguard_web_filtering;
        if (!i.warnings.empty()) j["warnings"] = i.warnings;
        return j;
    }

    std::string device_text(const Inventory& i) {
        std::string out = std::format("Device {} {} ({}), FortiOS {}\n", i.model, i.hostname, i.serial, i.version);
        out += std::format("  domain-entry limit {}; forti-hole feeds hold {}, other feeds {}\n",
                           i.facts.entry_limit ? grouped(*i.facts.entry_limit) : std::string("not reported"), grouped(i.facts.owned_entries),
                           grouped(i.facts.other_entries));
        if (i.facts.memory_total)
            out += std::format("  memory {} of {} used ({:.1f}%), 24 h peak {}, conserve mode at {}\n", mb(static_cast<double>(*i.facts.memory_used)),
                               mb(static_cast<double>(*i.facts.memory_total)), 100.0 * static_cast<double>(*i.facts.memory_used) / static_cast<double>(*i.facts.memory_total),
                               i.facts.memory_peak_percent ? std::format("{:.0f}%", *i.facts.memory_peak_percent) : std::string("?"),
                               i.facts.memory_red_percent ? std::format("{:.0f}%", *i.facts.memory_red_percent) : std::string("?"));
        for (const auto& w: i.warnings) out += "  warning: " + w + "\n";
        return out;
    }

}  // namespace fh
