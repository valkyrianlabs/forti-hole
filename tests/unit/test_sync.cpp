// End to end through the CLI against the in-memory FortiGate: feeds, deltas, verification, failure
// handling, budgets, ownership.

#include "fixture.hpp"

#include "state.hpp"

#include <gtest/gtest.h>

#include <set>
#include <format>

using namespace fh;
using namespace fh::test;
using json = nlohmann::json;

namespace {

    // A device reports its domain-entry limit through any loaded domain feed (an operator's here).
    void operator_feed(FakeGateway& device, std::string name = "operator-blocklist", int category = 192) {
        FakeFeed theirs;
        theirs.object = {{"name", name}, {"type", "domain"}, {"update-method", "push"}, {"category", category}, {"comments", "mine"}};
        theirs.entries = {"mine.example"};
        theirs.pending_reads = 0;
        theirs.last_update = 5;
        device.feeds[name] = theirs;
    }

    // What the device's forti-hole feeds hold, as one set of lines.
    std::set<std::string> device_lines(const FakeGateway& device, std::string_view prefix = "fh-") {
        std::set<std::string> lines;
        for (const auto& [name, feed]: device.feeds)
            if (name.starts_with(prefix)) lines.insert(feed.entries.begin(), feed.entries.end());
        return lines;
    }

    std::size_t device_line_count(const FakeGateway& device) {
        std::size_t n = 0;
        for (const auto& [name, feed]: device.feeds)
            if (name.starts_with("fh-")) n += feed.entries.size();
        return n;
    }

    std::size_t snapshots(const FakeGateway& device, std::size_t since = 0) {
        std::size_t n = 0;
        for (std::size_t i = since; i < device.applied.size(); ++i)
            if (device.applied[i].value("op", "") == "action" && device.applied[i]["body"]["commands"][0]["command"] == "snapshot") ++n;
        return n;
    }

}  // namespace

TEST(Sync, FirstSyncCreatesPushesAndVerifies) {
    Harness h;
    h.write_config(small_config());
    int exit = 0;
    auto result = h.run_json({"sync"}, &exit);
    ASSERT_EQ(exit, 0) << h.out.str() << h.err.str();
    const auto& data = result["data"];
    // The medium variant does not fit 1,000 lines per feed? It does (no device limit hit): it is chosen.
    EXPECT_EQ(data["compile"]["selected"]["tif"], "medium");
    // Every compiled line is on the device, once, and was verified there.
    std::size_t compiled = data["compile"]["totals"]["emitted_lines"];
    EXPECT_EQ(device_line_count(h.device), compiled);
    for (const auto& v: data["outcome"]["verification"]) EXPECT_TRUE(v["verified"].get<bool>()) << v.dump();
    // The managed profiles block exactly the categories of their groups.
    ASSERT_TRUE(h.device.profiles.contains("fh-home"));
    ASSERT_TRUE(h.device.profiles.contains("fh-kids"));
    EXPECT_EQ(h.device.profiles["fh-home"]["ftgd-dns"]["options"], "error-allow");
    EXPECT_EQ(h.device.profiles["fh-home"]["block-action"], "block");
    EXPECT_NE(h.device.profiles["fh-home"]["comment"].get<std::string>().find(marker), std::string::npos);
    // Feeds carry the marker and a category in range.
    for (const auto& [name, feed]: h.device.feeds) {
        EXPECT_NE(feed.object["comments"].get<std::string>().find(marker), std::string::npos);
        int c = feed.object["category"];
        EXPECT_TRUE(c >= 192 && c <= 221);
    }
    // State records each feed as confirmed with the device's update time.
    auto state = load_state(h.temp.home);
    for (const auto& [name, feed]: h.device.feeds) {
        ASSERT_TRUE(state.feeds.contains(name));
        EXPECT_TRUE(state.feeds[name].confirmed);
        EXPECT_EQ(state.feeds[name].lines, feed.entries.size());
        EXPECT_EQ(state.feeds[name].device_update, feed.last_update);
    }
    EXPECT_FALSE(load_journal(h.temp.home));
}

TEST(Sync, AnUnchangedRunWritesNothing) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    auto writes = h.device.writes, plans = h.device.plans;
    auto result = h.run_json({"sync"});
    EXPECT_EQ(h.device.writes, writes);
    EXPECT_EQ(h.device.plans, plans);
    EXPECT_FALSE(result["data"]["plan"]["writes"].get<bool>());
    // Upstream answered 304 for every list.
    for (const auto& [key, list]: result["data"]["compile"].items()) (void)key, (void)list;
    std::size_t conditional = 0;
    for (const auto& r: h.lists.requests) conditional += !r.etag.empty();
    EXPECT_GE(conditional, 4u);
}

TEST(Sync, ListChangesArePushedAsDeltas) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    auto before = h.device.applied.size();
    h.lists.pages["https://lists.test/hosts"] = {200, hosts_list(10, 410), "\"h2\""};   // 10 gone, 10 new
    auto result = h.run_json({"sync"});
    ASSERT_EQ(result["ok"], true) << h.out.str();
    EXPECT_EQ(snapshots(h.device, before), 0u);
    EXPECT_EQ(result["data"]["plan"]["adds"], 10);
    EXPECT_EQ(result["data"]["plan"]["removes"], 10);
    auto lines = device_lines(h.device);
    EXPECT_TRUE(lines.contains("h405.example"));
    EXPECT_FALSE(lines.contains("h3.example"));
    for (const auto& v: result["data"]["outcome"]["verification"]) EXPECT_TRUE(v["verified"].get<bool>());
}

TEST(Sync, LargeDeltasGoOutInRoundsTheDeviceCanApply) {
    Harness h;
    h.write_config(small_config());   // chunk_entries 120
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    h.lists.pages["https://lists.test/hosts"] = {200, hosts_list(300, 700), "\"h3\""};   // 300 gone, 300 new
    auto result = h.run_json({"sync"});
    ASSERT_EQ(result["ok"], true) << h.out.str();
    EXPECT_GT(result["data"]["plan"]["requests"]["push_rounds"].get<int>(), 1);
    for (const auto& v: result["data"]["outcome"]["verification"]) EXPECT_TRUE(v["verified"].get<bool>()) << v.dump();
    auto lines = device_lines(h.device);
    EXPECT_TRUE(lines.contains("h699.example"));
    EXPECT_FALSE(lines.contains("h299.example"));
}

TEST(Sync, AFailedDownloadKeepsTheDeviceAsItIs) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0);
    auto lines = device_lines(h.device);
    auto writes = h.device.writes;
    h.lists.pages["https://lists.test/hosts"].fail = true;   // connection refused
    h.lists.pages["https://lists.test/ads.txt"] = {200, "<!DOCTYPE html><html>rate limited</html>", "\"a9\"", "text/html"};
    h.lists.pages["https://lists.test/tif.mini.txt"] = {200, adblock_list(0, 20, "t"), "\"n9\""};   // shrank by 90%
    auto result = h.run_json({"sync"});
    EXPECT_EQ(result["ok"], true) << h.out.str();
    EXPECT_EQ(h.device.writes, writes);
    EXPECT_EQ(device_lines(h.device), lines);
    std::string warnings = result["data"]["compile"]["warnings"].dump();
    EXPECT_NE(warnings.find("download failed"), std::string::npos);
    EXPECT_NE(warnings.find("HTML"), std::string::npos);
}

TEST(Sync, NoLastGoodCopyMeansNoSync) {
    Harness h;
    h.write_config(small_config());
    h.lists.pages["https://lists.test/ads.txt"].fail = true;
    int exit = h.run({"sync"});
    EXPECT_EQ(exit, 6);
    EXPECT_EQ(h.device.writes, 0u);
    EXPECT_NE(h.err.str().find("unavailable"), std::string::npos);
}

TEST(Sync, ADeviceThatLostItsFeedsGetsSnapshots) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0);
    // A reboot / failover: entries gone, update times reset.
    for (auto& [name, feed]: h.device.feeds) {
        feed.entries.clear();
        feed.last_update = 0;
    }
    auto before = h.device.applied.size();
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    EXPECT_EQ(snapshots(h.device, before), h.device.feeds.size());
    auto state = load_state(h.temp.home);
    std::size_t expected = 0;
    for (const auto& [name, f]: state.feeds) expected += f.lines;
    EXPECT_EQ(device_line_count(h.device), expected);
}

TEST(Sync, AnInterruptedPushIsRecoveredByTheNextRun) {
    Harness h;
    h.write_config(small_config());
    h.device.fail_push_after = 1;
    int exit = h.run({"sync"});
    EXPECT_EQ(exit, 8) << h.err.str();
    EXPECT_TRUE(load_journal(h.temp.home));
    auto state = load_state(h.temp.home);
    for (const auto& [name, f]: state.feeds) EXPECT_FALSE(f.confirmed) << name;
    // The next run re-sends every feed the journal names, and verifies them.
    auto result = h.run_json({"sync"}, &exit);
    EXPECT_EQ(exit, 0) << h.out.str();
    EXPECT_FALSE(load_journal(h.temp.home));
    for (const auto& v: result["data"]["outcome"]["verification"]) EXPECT_TRUE(v["verified"].get<bool>());
    EXPECT_EQ(device_line_count(h.device), result["data"]["compile"]["totals"]["emitted_lines"].get<std::size_t>());
}

TEST(Sync, ARunThatDiedBeforeSavingKeepsTheDevicesCategories) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0);
    std::map<std::string, int> before;
    for (const auto& [name, feed]: h.device.feeds) before[name] = feed.object["category"];
    // Killed after the device changed but before state.json was written.
    write_private(h.temp.home.state(), "{}");
    write_private(h.temp.home.journal(), R"({"phase":"push","feeds":[]})");
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    for (const auto& [name, feed]: h.device.feeds) EXPECT_EQ(feed.object["category"], before[name]) << name;
    EXPECT_FALSE(load_journal(h.temp.home));
}

TEST(Sync, PushesTheDeviceDroppedFailVerification) {
    Harness h;
    h.write_config(small_config());
    h.device.drop_pushes = true;
    EXPECT_EQ(h.run({"sync"}), 8);
    auto state = load_state(h.temp.home);
    for (const auto& [name, f]: state.feeds) EXPECT_FALSE(f.confirmed);
    h.device.drop_pushes = false;
    EXPECT_EQ(h.run({"sync"}), 0) << h.err.str();
}

TEST(Sync, NewFeedsAreLoadedBeforeAnythingIsPushed) {
    Harness h;
    h.write_config(small_config());
    // Every new fake feed is "Connect pending" for its first status reads; a push then would be lost.
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    for (const auto& [name, feed]: h.device.feeds) EXPECT_FALSE(feed.entries.empty()) << name;
}

TEST(Sync, AutoVariantFallsBackWhenTheDeviceLimitIsSmall) {
    Harness h;
    h.write_config(small_config());
    h.device.entry_limit = 2500;   // medium compiles to ~4,700 lines, mini to ~1,400
    operator_feed(h.device);
    auto result = h.run_json({"sync"});
    ASSERT_EQ(result["ok"], true) << h.out.str();
    EXPECT_EQ(result["data"]["compile"]["selected"]["tif"], "mini");
    EXPECT_NE(result["data"]["compile"]["warnings"].dump().find("does not fit"), std::string::npos);
}

TEST(Sync, ASetThatFitsNowhereIsRefusedAndNothingIsWritten) {
    Harness h;
    h.write_config(small_config());
    h.device.entry_limit = 500;
    operator_feed(h.device);
    int exit = 0;
    auto result = h.run_json({"sync"}, &exit);
    EXPECT_EQ(exit, 9);
    EXPECT_EQ(h.device.writes, 0u);
    EXPECT_TRUE(result["data"].contains("refused"));
}

TEST(Sync, MemoryHeadroomIsEnforced) {
    Harness h;
    auto config = small_config();
    config["budget"] = {{"bytes_per_line", 1000000}};   // an absurd cost: any growth exceeds the ceiling
    h.write_config(config);
    int exit = 0;
    auto result = h.run_json({"sync"}, &exit);
    EXPECT_EQ(exit, 9) << result["data"]["compile"]["budget"].dump();
    EXPECT_EQ(h.device.writes, 0u);
    EXPECT_NE(result["data"].value("refused", std::string()).find("memory"), std::string::npos);
}

TEST(Sync, NothingIsWrittenUntilEnabledOrWhilePaused) {
    Harness h;
    auto config = small_config();
    config["enabled"] = false;
    h.write_config(config);
    EXPECT_EQ(h.run({"sync"}), 7);
    EXPECT_EQ(h.run({"plan"}), 0) << h.err.str();
    EXPECT_EQ(h.device.writes, 0u);
    config["enabled"] = true;
    h.write_config(config);
    EXPECT_EQ(h.run({"pause"}), 0);
    EXPECT_EQ(h.run({"sync"}), 0);
    EXPECT_EQ(h.device.writes, 0u);
    EXPECT_EQ(h.run({"resume"}), 0);
    EXPECT_EQ(h.run({"sync"}), 0);
    EXPECT_GT(h.device.writes, 0u);
}

TEST(Sync, OperatorObjectsAreNeverTouched) {
    Harness h;
    h.write_config(small_config());
    FakeFeed theirs;
    theirs.object = {{"name", "operator-blocklist"}, {"type", "domain"}, {"update-method", "push"}, {"category", 192}, {"comments", "mine"}};
    theirs.entries = {"mine.example"};
    theirs.pending_reads = 0;
    theirs.last_update = 5;
    h.device.feeds["operator-blocklist"] = theirs;
    h.device.profiles["office"] = {{"name", "office"}, {"comment", ""}, {"ftgd-dns", {{"filters", json::array({{{"category", 192}, {"action", "block"}}})}}}};
    ASSERT_EQ(h.run({"sync"}), 0) << h.err.str();
    EXPECT_EQ(h.device.feeds["operator-blocklist"].object, theirs.object);
    EXPECT_EQ(h.device.feeds["operator-blocklist"].entries, theirs.entries);
    EXPECT_EQ(h.device.profiles["office"]["ftgd-dns"]["filters"].size(), 1u);
    // The operator's category is never reused.
    for (const auto& [name, feed]: h.device.feeds)
        if (name.starts_with("fh-")) {
            EXPECT_NE(feed.object["category"], 192);
        }
    // Their entries count against the budget.
    auto result = h.run_json({"plan"});
    EXPECT_EQ(result["data"]["compile"]["budget"]["entries"]["other_feeds"], 1);
}

TEST(Sync, APrefixedObjectWithoutTheMarkerStopsTheRun) {
    Harness h;
    h.write_config(small_config());
    FakeFeed impostor;
    impostor.object = {{"name", "fh-all-1"}, {"type", "domain"}, {"update-method", "push"}, {"category", 200}, {"comments", "hand made"}};
    impostor.pending_reads = 0;
    h.device.feeds["fh-all-1"] = impostor;
    EXPECT_EQ(h.run({"sync"}), 6);
    EXPECT_EQ(h.device.writes, 0u);
    EXPECT_EQ(h.device.feeds["fh-all-1"].object["comments"], "hand made");
}

TEST(Sync, ObsoleteFeedsAreDeletedAfterTheirReplacementsVerify) {
    Harness h;
    auto config = small_config();
    h.write_config(config);
    ASSERT_EQ(h.run({"sync"}), 0);
    // One profile left: every group label changes to "all", the old groups' feeds become obsolete.
    config["profiles"] = {{"home", {{"tiers", {"base", "threat"}}}}};
    h.write_config(config);
    auto result = h.run_json({"sync"});
    ASSERT_EQ(result["ok"], true) << h.out.str();
    for (const auto& [name, feed]: h.device.feeds) EXPECT_TRUE(name.starts_with("fh-all-")) << name;
    EXPECT_FALSE(result["data"]["outcome"]["deleted"].empty());
    auto state = load_state(h.temp.home);
    for (const auto& [name, f]: state.feeds) EXPECT_TRUE(h.device.feeds.contains(name)) << name;
    // Every name is still blocked for home.
    EXPECT_EQ(device_line_count(h.device), result["data"]["compile"]["totals"]["emitted_lines"].get<std::size_t>());
}

TEST(Sync, StatusAndFullVerification) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0);
    EXPECT_EQ(h.run({"status", "--verify", "--full"}), 0) << h.out.str();
    // Someone pushes to a forti-hole feed behind its back.
    auto& feed = h.device.feeds.begin()->second;
    feed.entries.push_back("intruder.example");
    feed.last_update += 1;
    EXPECT_EQ(h.run({"status"}), 8);
    EXPECT_NE(h.out.str().find("differs"), std::string::npos);
}

TEST(Manage, AttachAndDetachRestoreThePolicy) {
    Harness h;
    h.write_config(small_config());
    ASSERT_EQ(h.run({"sync"}), 0);
    h.device.policies[7] = {{"policyid", 7}, {"name", "lan -> wan"}, {"action", "accept"}, {"dnsfilter-profile", "default"}, {"utm-status", "disable"}};
    ASSERT_EQ(h.run({"attach", "home", "--policy", "7"}), 0) << h.err.str();
    EXPECT_EQ(h.device.policies[7]["dnsfilter-profile"], "fh-home");
    EXPECT_EQ(h.device.policies[7]["utm-status"], "enable");
    EXPECT_EQ(h.run({"attach", "home", "--policy", "7"}), 0);   // idempotent
    EXPECT_EQ(h.run({"remove", "--yes"}), 6);                   // still attached
    ASSERT_EQ(h.run({"detach", "--policy", "7"}), 0) << h.err.str();
    EXPECT_EQ(h.device.policies[7]["dnsfilter-profile"], "default");
    EXPECT_EQ(h.run({"attach", "nope", "--policy", "7"}), 2);
    EXPECT_EQ(h.run({"attach", "home", "--policy", "99"}), 5);
}

TEST(Manage, RemoveDeletesOnlyForthHolesObjects) {
    Harness h;
    h.write_config(small_config());
    FakeFeed theirs;
    theirs.object = {{"name", "operator-blocklist"}, {"type", "domain"}, {"update-method", "push"}, {"category", 210}, {"comments", ""}};
    theirs.pending_reads = 0;
    h.device.feeds["operator-blocklist"] = theirs;
    ASSERT_EQ(h.run({"sync"}), 0);
    EXPECT_EQ(h.run({"remove"}), 2);   // needs --yes
    ASSERT_EQ(h.run({"remove", "--yes"}), 0) << h.err.str();
    ASSERT_EQ(h.device.feeds.size(), 1u);
    EXPECT_TRUE(h.device.feeds.contains("operator-blocklist"));
    EXPECT_FALSE(h.device.profiles.contains("fh-home"));
    EXPECT_TRUE(load_state(h.temp.home).feeds.empty());
}

TEST(Manage, MigrateLegacyFeeds) {
    Harness h;
    h.write_config(small_config());
    for (int i = 1; i <= 2; ++i) {
        FakeFeed old;
        old.object = {{"name", std::format("forti-hole_security_level-0_part-{}", i)}, {"type", "domain"}, {"update-method", "push"}, {"category", 192 + i}, {"comments", ""}};
        old.pending_reads = 0;
        h.device.feeds[old.object["name"]] = old;
    }
    h.device.profiles["home"] = {{"name", "home"}, {"comment", ""},
                                 {"ftgd-dns", {{"options", ""}, {"filters", json::array({{{"id", 1}, {"category", 193}, {"action", "block"}},
                                                                                         {{"id", 2}, {"category", 26}, {"action", "block"}}})}}}};
    auto listed = h.run_json({"migrate", "feeds"});
    EXPECT_EQ(listed["data"]["feeds"].size(), 2u);
    EXPECT_EQ(h.device.feeds.size(), 2u);   // nothing deleted without --apply
    auto applied = h.run_json({"migrate", "feeds", "--apply"});
    EXPECT_TRUE(applied["data"]["applied"].get<bool>()) << h.out.str();
    EXPECT_TRUE(h.device.feeds.empty());
    ASSERT_EQ(h.device.profiles["home"]["ftgd-dns"]["filters"].size(), 1u);
    EXPECT_EQ(h.device.profiles["home"]["ftgd-dns"]["filters"][0]["category"], 26);
    EXPECT_EQ(h.run({"migrate", "feeds", "--prefix", "fh-"}), 2);   // never its own feeds
}
