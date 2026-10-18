#include "core/budget.hpp"
#include "core/compile.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <random>
#include <set>

using namespace fh;

namespace {

    CompileOptions three_profiles(std::size_t max_entries = 131072) {
        CompileOptions o;
        o.tiers = {"base", "plus", "ultimate"};
        o.profiles = {{"light", {"base"}}, {"standard", {"base", "plus"}}, {"strict", {"base", "plus", "ultimate"}}};
        o.max_entries_per_feed = max_entries;
        return o;
    }

    std::set<std::string> all_lines(const CompileResult& r) {
        std::set<std::string> lines;
        for (const auto& f: r.feeds) lines.insert(f.lines.begin(), f.lines.end());
        return lines;
    }

    // The lines a profile's device filter blocks (every feed whose profiles include it).
    std::set<std::string> blocked_for(const CompileResult& r, const std::string& profile) {
        std::set<std::string> lines;
        for (const auto& f: r.feeds)
            if (std::find(f.profiles.begin(), f.profiles.end(), profile) != f.profiles.end()) lines.insert(f.lines.begin(), f.lines.end());
        return lines;
    }

    // FortiOS matching of feed lines, as measured: "n" exact, "*.n" below, "*n" suffix.
    bool fortios_matches(const std::set<std::string>& lines, const std::string& query) {
        if (lines.contains(query)) return true;
        for (const auto& line: lines) {
            if (line.starts_with("*.") && query.size() > line.size() - 1 && query.ends_with(line.substr(1))) return true;
            if (line.starts_with("*") && !line.starts_with("*.") && query.ends_with(line.substr(1))) return true;
        }
        return false;
    }

}  // namespace

TEST(Compile, DeduplicatesWithinAndAcrossSources) {
    Compiler c(three_profiles());
    auto& a = c.add_source("a", "base", "||ads.example^\n||ads.example^\n||track.example^\n", Format::adblock);
    EXPECT_EQ(a.unique, 2u);
    EXPECT_EQ(a.duplicates_within, 1u);
    auto& b = c.add_source("b", "plus", "||ads.example^\n||more.example^\n", Format::adblock);
    EXPECT_EQ(b.duplicates_across, 1u);
    EXPECT_EQ(b.unique, 1u);
    auto r = c.compile();
    EXPECT_EQ(r.stats.duplicates, 2u);
    EXPECT_EQ(r.stats.names, 3u);
    // ads.example is in base (all profiles): published once, in the all-profiles group, not again in plus.
    std::size_t copies = 0;
    for (const auto& f: r.feeds) copies += std::count(f.lines.begin(), f.lines.end(), "ads.example");
    EXPECT_EQ(copies, 1u);
}

TEST(Compile, StrictEncodingKeepsAdblockSemanticsOnFortiOS) {
    Compiler c(three_profiles());
    c.add_source("a", "base", "||ads.example^\n", Format::adblock);
    c.add_source("h", "base", "0.0.0.0 exact.example\n", Format::hosts);
    auto r = c.compile();
    auto lines = all_lines(r);
    EXPECT_EQ(lines, (std::set<std::string>{"ads.example", "*.ads.example", "exact.example"}));
    EXPECT_TRUE(fortios_matches(lines, "ads.example"));
    EXPECT_TRUE(fortios_matches(lines, "a.b.ads.example"));
    EXPECT_FALSE(fortios_matches(lines, "badads.example"));
    EXPECT_TRUE(fortios_matches(lines, "exact.example"));
    EXPECT_FALSE(fortios_matches(lines, "sub.exact.example"));
}

TEST(Compile, CollapsesNamesCoveredByAParentSubtree) {
    Compiler c(three_profiles());
    c.add_source("a", "base", "||example.com^\n||ads.example.com^\n", Format::adblock);
    c.add_source("h", "base", "0.0.0.0 x.ads.example.com\n0.0.0.0 example.com\n", Format::hosts);
    auto r = c.compile();
    EXPECT_EQ(all_lines(r), (std::set<std::string>{"example.com", "*.example.com"}));
    EXPECT_EQ(r.stats.collapsed, 3u);   // ads.example.com, x.ads.example.com, exact example.com under its own subtree
}

TEST(Compile, CollapseOnlyForTheProfilesTheParentCovers) {
    Compiler c(three_profiles());
    c.add_source("u", "ultimate", "||example.com^\n", Format::adblock);   // strict only
    c.add_source("b", "base", "||ads.example.com^\n", Format::adblock);   // every profile
    auto r = c.compile();
    // light and standard still need ads.example.com; strict has it through example.com.
    for (const std::string profile: {"light", "standard"}) {
        auto lines = blocked_for(r, profile);
        EXPECT_TRUE(fortios_matches(lines, "ads.example.com")) << profile;
        EXPECT_TRUE(fortios_matches(lines, "x.ads.example.com")) << profile;
        EXPECT_FALSE(fortios_matches(lines, "other.example.com")) << profile;
    }
    auto strict = blocked_for(r, "strict");
    EXPECT_TRUE(fortios_matches(strict, "other.example.com"));
    EXPECT_FALSE(strict.contains("ads.example.com"));   // not published for strict: it is redundant there
    EXPECT_EQ(r.stats.narrowed, 1u);
}

TEST(Compile, EachLineIsPublishedOnce) {
    Compiler c(three_profiles(50));
    std::string base, plus, ultimate;
    for (int i = 0; i < 300; ++i) {
        base += std::format("||b{}.example^\n", i);
        plus += std::format("||b{}.example^\n||p{}.example^\n", i % 50, i);
        ultimate += std::format("0.0.0.0 u{}.example\n0.0.0.0 p{}.example\n", i, i % 70);
    }
    c.add_source("base", "base", base, Format::adblock);
    c.add_source("plus", "plus", plus, Format::adblock);
    c.add_source("ult", "ultimate", ultimate, Format::hosts);
    auto r = c.compile();
    std::size_t total = 0;
    std::set<std::string> seen;
    for (const auto& f: r.feeds) {
        EXPECT_LE(f.lines.size(), 50u) << f.name;
        EXPECT_TRUE(std::is_sorted(f.lines.begin(), f.lines.end())) << f.name;
        total += f.lines.size();
        seen.insert(f.lines.begin(), f.lines.end());
    }
    EXPECT_EQ(total, seen.size());   // no identical line in two feeds
    EXPECT_EQ(total, r.stats.lines);
    // Every listed name is still blocked for exactly the profiles that list it.
    for (int i = 0; i < 300; ++i) {
        for (const std::string p: {"light", "standard", "strict"}) EXPECT_TRUE(fortios_matches(blocked_for(r, p), std::format("b{}.example", i)));
        EXPECT_FALSE(fortios_matches(blocked_for(r, "light"), std::format("p{}.example", i)));
        EXPECT_TRUE(fortios_matches(blocked_for(r, "standard"), std::format("p{}.example", i)));
        EXPECT_TRUE(fortios_matches(blocked_for(r, "strict"), std::format("u{}.example", i)));
        EXPECT_FALSE(fortios_matches(blocked_for(r, "standard"), std::format("u{}.example", i)));
    }
}

TEST(Compile, OutputIsDeterministicWhateverTheInputOrder) {
    std::vector<std::string> names;
    for (int i = 0; i < 2000; ++i) names.push_back(std::format("n{}.example", i));
    auto build = [&](unsigned seed) {
        auto shuffled = names;
        std::mt19937 rng(seed);
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::string a, b;
        for (std::size_t i = 0; i < shuffled.size(); ++i) (i % 2 ? a : b) += "||" + shuffled[i] + "^\n";
        Compiler c(three_profiles(500));
        c.add_source("a", "base", a, Format::adblock);
        c.add_source("b", "base", b, Format::adblock);
        return c.compile();
    };
    auto one = build(1), two = build(99);
    ASSERT_EQ(one.feeds.size(), two.feeds.size());
    for (std::size_t i = 0; i < one.feeds.size(); ++i) {
        EXPECT_EQ(one.feeds[i].name, two.feeds[i].name);
        EXPECT_EQ(one.feeds[i].sha256, two.feeds[i].sha256);
    }
}

TEST(Compile, AddingANameChangesOneFeed) {
    std::string list;
    for (int i = 0; i < 5000; ++i) list += std::format("0.0.0.0 h{}.example\n", i);
    auto build = [&](const std::string& extra) {
        Compiler c(three_profiles(1000));
        c.add_source("h", "base", list + extra, Format::hosts);
        return c.compile();
    };
    auto before = build(""), after = build("0.0.0.0 new.example\n");
    ASSERT_EQ(before.feeds.size(), after.feeds.size());
    int changed = 0;
    for (std::size_t i = 0; i < before.feeds.size(); ++i) changed += before.feeds[i].sha256 != after.feeds[i].sha256;
    EXPECT_EQ(changed, 1);
}

TEST(Compile, PartitionCountIsKeptFromThePreviousRun) {
    std::string list;
    for (int i = 0; i < 100; ++i) list += std::format("0.0.0.0 h{}.example\n", i);
    auto options = three_profiles(1000);
    options.partitions["all"] = 3;
    Compiler c(options);
    c.add_source("h", "base", list, Format::hosts);
    auto r = c.compile();
    EXPECT_EQ(r.feeds.size(), 3u);
    EXPECT_EQ(r.partitions.at("all"), 3u);
}

TEST(Compile, ExceptionsRemoveNamesAndReportWhatFeedsCannotExpress) {
    Compiler c(three_profiles());
    c.add_source("a", "base", "||example.com^\n||ads.net^\n||tracker.org^\n@@||ok.ads.net^\n", Format::adblock);
    c.add_source("h", "base", "0.0.0.0 bad.io\n", Format::hosts);
    c.add_allow("bad.io", Scope::exact);                      // removed from the feeds
    c.add_allow("tracker.org", Scope::exact, {"light"});      // light keeps only *.tracker.org
    c.add_allow("safe.example.com", Scope::exact);            // below a blocked subtree: static exception
    auto r = c.compile();
    auto light = blocked_for(r, "light");
    EXPECT_FALSE(light.contains("bad.io"));
    EXPECT_FALSE(light.contains("tracker.org"));
    EXPECT_TRUE(light.contains("*.tracker.org"));
    EXPECT_TRUE(blocked_for(r, "strict").contains("tracker.org"));
    std::set<std::string> exceptions;
    for (const auto& e: r.exceptions) exceptions.insert(e.pattern);
    EXPECT_TRUE(exceptions.contains("safe.example.com"));
    EXPECT_TRUE(exceptions.contains("ok.ads.net"));           // a list's own @@ exception under its subtree
    EXPECT_FALSE(exceptions.contains("bad.io"));
}

TEST(Compile, RegexExceptionsRemoveMatchingNames) {
    Compiler c(three_profiles());
    c.add_source("h", "base", "0.0.0.0 cdn1.good.example\n0.0.0.0 cdn2.good.example\n0.0.0.0 ads.example\n", Format::hosts);
    c.add_allow_regex("^cdn[0-9]+\\.good\\.example$");
    auto r = c.compile();
    EXPECT_EQ(all_lines(r), (std::set<std::string>{"ads.example"}));
    ASSERT_EQ(r.exceptions.size(), 1u);
    EXPECT_EQ(r.exceptions[0].kind, StaticException::Kind::regex);
    EXPECT_THROW(c.add_allow_regex("(unclosed"), CompileError);
}

TEST(Compile, CompactEncodingNeverOverMatchesAReferenceName) {
    auto options = three_profiles();
    options.encoding = Encoding::compact;
    Compiler c(options);
    c.add_source("a", "base", "||ads.com^\n||telemetry.vendor.example^\n", Format::adblock);
    c.set_reference("1,uploads.com\n2,google.com\n", Format::tranco);
    auto r = c.compile();
    auto lines = all_lines(r);
    // "*ads.com" would block uploads.com: kept strict. The other compacts to one line.
    EXPECT_EQ(lines, (std::set<std::string>{"ads.com", "*.ads.com", "*telemetry.vendor.example"}));
    EXPECT_FALSE(fortios_matches(lines, "uploads.com"));
    EXPECT_TRUE(fortios_matches(lines, "x.telemetry.vendor.example"));
    EXPECT_EQ(r.stats.compact_unsafe, 1u);

    Compiler no_reference(options);
    no_reference.add_source("a", "base", "||ads.com^\n", Format::adblock);
    EXPECT_THROW((void)no_reference.compile(), CompileError);
}

TEST(Compile, CountingMatchesMaterializing) {
    Compiler c(three_profiles(100));
    std::string list;
    for (int i = 0; i < 1000; ++i) list += std::format("||n{}.example^\n0.0.0.0 e{}.example\n", i, i);
    c.add_source("a", "plus", list, Format::adblock);
    c.add_source("h", "base", list, Format::hosts);
    auto counted = c.compile(false);
    auto full = c.compile();
    EXPECT_TRUE(counted.feeds.empty());
    EXPECT_EQ(counted.stats.lines, full.stats.lines);
    EXPECT_EQ(counted.stats.lines_per_profile, full.stats.lines_per_profile);
}

TEST(Compile, RejectsBadOptions) {
    CompileOptions o;
    EXPECT_THROW(Compiler{o}, CompileError);
    o.tiers = {"base"};
    o.profiles = {{"p", {"nope"}}};
    EXPECT_THROW(Compiler{o}, CompileError);
}

TEST(Budget, EntryLimitWithOtherFeedsAndHeadroom) {
    DeviceFacts device;
    device.entry_limit = 1000000;
    device.other_entries = 50000;
    BudgetPolicy policy;
    auto fits = check_budget(800000, device, policy);
    EXPECT_TRUE(fits.entries_fit);
    EXPECT_EQ(fits.entry_available, 850000u);
    auto too_big = check_budget(900000, device, policy);
    EXPECT_FALSE(too_big.fits());
    EXPECT_NE(too_big.reason().find("850000 available"), std::string::npos);
    DeviceFacts unknown;
    auto assumed = check_budget(100, unknown, policy);
    EXPECT_FALSE(assumed.entry_limit_reported);
    EXPECT_EQ(assumed.entry_limit, 1000000u);
}

TEST(Budget, MemoryIsPredictedFromTheLinesAddedAndKeepsHeadroom) {
    DeviceFacts device;
    device.entry_limit = 10000000;
    device.memory_total = 2'000'000'000;
    device.memory_used = 1'100'000'000;   // 55%
    device.owned_entries = 100000;
    device.memory_red_percent = 88;
    BudgetPolicy policy;
    policy.bytes_per_line = 400;
    auto ok = check_budget(700000, device, policy);   // +600k lines = +240 MB -> 67%
    EXPECT_TRUE(ok.memory_fits);
    EXPECT_NEAR(ok.memory_after_percent, 67.0, 0.01);
    EXPECT_NEAR(ok.memory_ceiling_percent, 85.0, 0.01);
    auto no = check_budget(1500000, device, policy);  // +1.4M lines = +560 MB -> 83%... still under 85
    EXPECT_TRUE(no.memory_fits);
    auto worse = check_budget(1800000, device, policy);   // +680 MB -> 89%
    EXPECT_FALSE(worse.memory_fits);
    EXPECT_NE(worse.reason().find("ceiling"), std::string::npos);
    // The red threshold caps the ceiling below what the headroom would allow.
    device.memory_red_percent = 70;
    policy.memory_headroom_percent = 5;
    EXPECT_NEAR(check_budget(700000, device, policy).memory_ceiling_percent, 67.0, 0.01);
    // Shrinking is always allowed, even above the ceiling.
    device.memory_used = 1'900'000'000;
    EXPECT_TRUE(check_budget(50000, device, policy).memory_fits);
}

TEST(Budget, LearningBytesPerLine) {
    EXPECT_FALSE(measure_bytes_per_line(100, 200, 0, 1000));            // too small a change
    EXPECT_FALSE(measure_bytes_per_line(200, 100, 0, 100000));          // memory went down: noise
    EXPECT_DOUBLE_EQ(*measure_bytes_per_line(1'000'000'000, 1'015'000'000, 0, 100000), 150.0);
    EXPECT_DOUBLE_EQ(learn_bytes_per_line(std::nullopt, 150), 150);
    EXPECT_DOUBLE_EQ(learn_bytes_per_line(150.0, 300), 300);             // up at once
    EXPECT_DOUBLE_EQ(learn_bytes_per_line(300.0, 100), 250);             // down slowly
    EXPECT_DOUBLE_EQ(learn_bytes_per_line(110.0, 1), default_bytes_per_line / 4);
}
