// Configuration, list loading and caching, the write guard, Pi-hole import, 0.x migration, CLI basics.

#include "fixture.hpp"

#include "config.hpp"
#include "guard.hpp"
#include "legacy.hpp"
#include "pihole_import.hpp"
#include "sources.hpp"
#include "state.hpp"

#include <gtest/gtest.h>

using namespace fh;
using namespace fh::test;
using json = nlohmann::json;

TEST(Config, TheExampleValidatesAndRoundTrips) {
    Config c = parse_config(example_config());
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(c.sources.size(), 5u);
    EXPECT_EQ(c.profiles.size(), 3u);
    const Source* tif = nullptr;
    for (const auto& s: c.sources)
        if (s.id == "hagezi-tif") tif = &s;
    ASSERT_TRUE(tif);
    EXPECT_EQ(tif->variant, "auto");
    EXPECT_EQ(tif->candidates(), (std::vector<std::string>{"medium", "mini"}));
    Config again = parse_config(to_json(c));
    EXPECT_EQ(to_json(again), to_json(c));
}

TEST(Config, TheShippedExampleIsWhatSetupWrites) {
    const char* root = std::getenv("FH_SOURCE_ROOT");
    if (!root) GTEST_SKIP() << "FH_SOURCE_ROOT is not set";
    auto text = read_file(std::filesystem::path(root) / "packaging" / "config.example.json");
    ASSERT_TRUE(text);
    EXPECT_EQ(json::parse(*text), example_config()) << "regenerate: forti-hole setup --offline --force --config packaging/config.example.json";
}

TEST(Config, RejectsMistakesWithTheirLocation) {
    auto expect_error = [](json j, std::string_view where) {
        try {
            parse_config(j);
            ADD_FAILURE() << "accepted: " << j.dump();
        } catch (const ConfigError& e) {
            EXPECT_NE(std::string(e.what()).find(where), std::string::npos) << e.what();
        }
    };
    json base = example_config();
    json j = base;
    j["unknown"] = 1;
    expect_error(j, "unknown key");
    j = base;
    j["sources"][0]["url"] = "http://insecure.example/list";
    expect_error(j, "sources[0]");
    j = base;
    j["sources"][0]["tier"] = "nope";
    expect_error(j, "unknown tier");
    j = base;
    j["profiles"]["all"] = {{"tiers", {"base"}}};
    expect_error(j, "profiles.all");
    j = base;
    j["sources"][1]["id"] = j["sources"][0]["id"];
    expect_error(j, "twice");
    j = base;
    j["encoding"] = "compact";
    expect_error(j, "reference");
    j = base;
    j["allow"] = json::array({{{"name", "x.example"}, {"profiles", {"ghost"}}}});
    expect_error(j, "unknown profile");
    j = base;
    j["prefix"] = "fh";
    expect_error(j, "prefix");
}

TEST(Config, OperatorListsSitBesideTheDefaults) {
    json j = example_config();
    j["sources"].push_back({{"id", "my-list"}, {"tier", "base"}, {"format", "domains"}, {"url", "file:///etc/forti-hole/my-list.txt"}});
    j["block"] = json::array({{{"name", "evil.example"}, {"scope", "subtree"}}});
    j["allow"] = json::array({{{"name", "needed.example"}}});
    Config c = parse_config(j);
    EXPECT_EQ(c.sources.back().url, "file:///etc/forti-hole/my-list.txt");
    EXPECT_EQ(c.block[0].tier, "base");
}

TEST(Sources, CachesAcceptsAndFallsBack) {
    TempHome temp;
    Config config = parse_config(small_config());
    State state;
    FakeFetcher fetcher;
    fetcher.pages["https://lists.test/hosts"] = {200, hosts_list(0, 100), "\"e1\""};
    fetcher.pages["https://lists.test/ads.txt"] = {200, adblock_list(0, 50), "\"e2\""};
    fetcher.pages["https://lists.test/tif.medium.txt"] = {200, adblock_list(0, 80, "t"), ""};
    fetcher.pages["https://lists.test/tif.mini.txt"] = {200, adblock_list(0, 30, "t"), ""};
    auto lists = load_lists(config, temp.home, state, fetcher);
    EXPECT_EQ(lists["hosts"].origin, LoadedList::Origin::downloaded);
    EXPECT_EQ(lists["hosts"].entries, 100u);
    EXPECT_EQ(lists["tif.medium"].entries, 80u);

    // 304: the cached copy, no body transferred.
    lists = load_lists(config, temp.home, state, fetcher);
    EXPECT_EQ(lists["hosts"].origin, LoadedList::Origin::not_modified);
    EXPECT_EQ(lists["hosts"].text, hosts_list(0, 100));

    // A truncated download (its header says 100, it has 40) and a shrink beyond the guard.
    std::string truncated = hosts_list(0, 100).substr(0, 1000);
    fetcher.pages["https://lists.test/hosts"] = {200, truncated, "\"e3\""};
    fetcher.pages["https://lists.test/ads.txt"] = {200, adblock_list(0, 10), "\"e4\""};
    lists = load_lists(config, temp.home, state, fetcher);
    EXPECT_EQ(lists["hosts"].origin, LoadedList::Origin::cached);
    EXPECT_NE(lists["hosts"].note.find("declares"), std::string::npos);
    EXPECT_EQ(lists["ads"].origin, LoadedList::Origin::cached);
    EXPECT_NE(lists["ads"].note.find("smaller"), std::string::npos);
    EXPECT_EQ(lists["ads"].entries, 50u);

    // A server error and a refused connection.
    fetcher.pages["https://lists.test/hosts"] = {503, "busy", ""};
    fetcher.pages["https://lists.test/ads.txt"].fail = true;
    lists = load_lists(config, temp.home, state, fetcher);
    EXPECT_EQ(lists["hosts"].origin, LoadedList::Origin::cached);
    EXPECT_EQ(lists["ads"].origin, LoadedList::Origin::cached);

    // A damaged cache file is not used.
    write_private(temp.home.cache() / "ads.list", "tampered");
    lists = load_lists(config, temp.home, state, fetcher);
    EXPECT_EQ(lists["ads"].origin, LoadedList::Origin::unavailable);

    // Offline reads only the cache.
    auto before = fetcher.requests.size();
    lists = load_lists(config, temp.home, state, fetcher, {true, false});
    EXPECT_EQ(fetcher.requests.size(), before);
    EXPECT_EQ(lists["tif.mini"].origin, LoadedList::Origin::cached);
}

TEST(Guard, OnlyForthHolesObjectsCanBeWritten) {
    using fortiapi::Method;
    std::string base = "https://fw.test:8443/api/v2/";
    std::string marked = json{{"name", "fh-all-1"}, {"comments", std::string(marker)}}.dump();
    EXPECT_EQ(refuse_write(Method::Get, base + "cmdb/firewall/policy", "", "fh-"), "");
    EXPECT_EQ(refuse_write(Method::Post, base + "cmdb/system/external-resource", marked, "fh-"), "");
    EXPECT_NE(refuse_write(Method::Post, base + "cmdb/system/external-resource", json{{"name", "fh-x"}}.dump(), "fh-"), "");
    EXPECT_NE(refuse_write(Method::Post, base + "cmdb/system/external-resource", json{{"name", "other"}, {"comments", std::string(marker)}}.dump(), "fh-"), "");
    EXPECT_EQ(refuse_write(Method::Put, base + "cmdb/system/external-resource/fh-all-1?vdom=root", "{}", "fh-"), "");
    EXPECT_NE(refuse_write(Method::Delete, base + "cmdb/system/external-resource/operator-feed", "", "fh-"), "");
    EXPECT_NE(refuse_write(Method::Delete, base + "cmdb/system/external-resource/fh-a%2Fb", "", "fh-"), "");   // encoded slash
    std::string push_ok = json{{"commands", json::array({{{"name", "fh-all-1"}, {"command", "add"}, {"entries", json::array()}}})}}.dump();
    std::string push_bad = json{{"commands", json::array({{{"name", "operator-feed"}, {"command", "snapshot"}, {"entries", json::array()}}})}}.dump();
    EXPECT_EQ(refuse_write(Method::Post, base + "monitor/system/external-resource/dynamic", push_ok, "fh-"), "");
    EXPECT_NE(refuse_write(Method::Post, base + "monitor/system/external-resource/dynamic", push_bad, "fh-"), "");
    EXPECT_NE(refuse_write(Method::Put, base + "cmdb/firewall/policy/7", json{{"dnsfilter-profile", "fh-home"}}.dump(), "fh-"), "");
    WritePermits permits;
    permits.policies.insert(7);
    EXPECT_EQ(refuse_write(Method::Put, base + "cmdb/firewall/policy/7", json{{"dnsfilter-profile", "fh-home"}}.dump(), "fh-", permits), "");
    EXPECT_NE(refuse_write(Method::Put, base + "cmdb/firewall/policy/7", json{{"action", "deny"}}.dump(), "fh-", permits), "");
    EXPECT_NE(refuse_write(Method::Put, base + "cmdb/dnsfilter/profile/office", json{{"ftgd-dns", json::object()}}.dump(), "fh-"), "");
    permits.profiles.insert("office");
    EXPECT_EQ(refuse_write(Method::Put, base + "cmdb/dnsfilter/profile/office", json{{"ftgd-dns", json::object()}}.dump(), "fh-", permits), "");
    EXPECT_NE(refuse_write(Method::Put, base + "cmdb/dnsfilter/profile/office", json{{"comment", "x"}}.dump(), "fh-", permits), "");
    EXPECT_NE(refuse_write(Method::Post, base + "cmdb/firewall/address", "{}", "fh-"), "");
    EXPECT_NE(refuse_write(Method::Post, base + "monitor/system/os/reboot", "{}", "fh-"), "");

    EXPECT_NO_THROW(check_operations(json::array({{{"op", "create"}, {"resource", "system/external-resource"},
                                                   {"object", {{"name", "fh-1"}, {"comments", std::string(marker)}}}}}),
                                     "fh-"));
    EXPECT_THROW(check_operations(json::array({{{"op", "delete"}, {"resource", "firewall/policy"}, {"key", 7}}}), "fh-"), GuardViolation);
    EXPECT_THROW(check_operations(json::array({{{"op", "delete"}, {"resource", "system/external-resource"}, {"key", "operator"}}}), "fh-"), GuardViolation);
    EXPECT_THROW(check_operations(json::array({{{"op", "action"}, {"resource", "monitor/system/config/restore"}, {"body", json::object()}}}), "fh-"),
                 GuardViolation);
}

TEST(Import, PiholeGroupsBecomeProfilesAndSharedListsTiers) {
    PiholeData data;
    data.groups = json::array({{{"id", 0}, {"name", "Default"}, {"enabled", true}},
                               {{"id", 1}, {"name", "Work"}, {"enabled", true}},
                               {{"id", 2}, {"name", "Cooper"}, {"enabled", true}},
                               {{"id", 3}, {"name", "Old"}, {"enabled", false}}});
    std::string hz = "https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest/adblock/";
    data.lists = json::array({{{"address", "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"}, {"type", "block"}, {"enabled", true}, {"groups", {0, 1, 2}}},
                              {{"address", hz + "pro.plus.txt"}, {"type", "block"}, {"enabled", true}, {"groups", {0}}},
                              {{"address", hz + "tif.txt"}, {"type", "block"}, {"enabled", true}, {"groups", {0, 1, 2}}},
                              {{"address", hz + "ultimate.txt"}, {"type", "block"}, {"enabled", true}, {"groups", {2, 3}}},
                              {{"address", hz + "pro.txt"}, {"type", "block"}, {"enabled", true}, {"groups", {1}}},
                              {{"address", "https://disabled.example/list"}, {"type", "block"}, {"enabled", false}, {"groups", {0}}}});
    data.domains = json::array({{{"domain", "ok.example"}, {"type", "allow"}, {"kind", "exact"}, {"groups", {0, 1, 2}}, {"enabled", true}},
                                {{"domain", "^cdn[0-9]\\.example$"}, {"type", "allow"}, {"kind", "regex"}, {"groups", {1}}, {"enabled", true}},
                                {{"domain", "bad.example"}, {"type", "deny"}, {"kind", "exact"}, {"groups", {2}}, {"enabled", true}},
                                {{"domain", "(.*)\\.zip$"}, {"type", "deny"}, {"kind", "regex"}, {"groups", {0}}, {"enabled", true}}});
    auto result = import_pihole(data);
    Config c = parse_config(result.config);
    std::set<std::string> profiles;
    for (const auto& p: c.profiles) profiles.insert(p.name);
    EXPECT_EQ(profiles, (std::set<std::string>{"default", "work", "cooper"}));
    // StevenBlack and TIF are shared by every group: one tier, "common".
    const Source* tif = nullptr;
    for (const auto& s: c.sources) {
        if (s.id == "stevenblack") {
            EXPECT_EQ(s.tier, "common");
        }
        if (s.id == "hagezi-tif") tif = &s;
    }
    ASSERT_TRUE(tif);
    EXPECT_EQ(tif->variant, "auto");
    EXPECT_EQ(tif->tier, "common");
    EXPECT_EQ(c.allow.size(), 1u);
    EXPECT_TRUE(c.allow[0].profiles.empty());   // every group
    EXPECT_EQ(c.allow_regex[0].profiles, (std::vector<std::string>{"work"}));
    EXPECT_EQ(c.block.size(), 1u);
    EXPECT_FALSE(c.enabled);
    std::string notes;
    for (const auto& n: result.notes) notes += n + "\n";
    EXPECT_NE(notes.find("regex deny"), std::string::npos);
    EXPECT_NE(notes.find("full TIF"), std::string::npos);
}

TEST(Legacy, TheOldConfigBecomesANewOneWithoutItsCredentials) {
    std::string yaml = R"(# forti-hole 0.x
fortigate:
  api_key: SECRET-KEY-VALUE  # never read
  gateway_ip: 192.168.1.1
  certificates:
    ssl_cert_password: SECRET-PASSWORD
naming_convention:
  prefix: forti-hole
  security_level: security_level
  file_index: part
categories:
  min: 192
forti_hole_automated_dns_filters:
  - dns_filter: home
    firewall_policies:
      - internal -> wan1
    filters:
      - security_level: 0
        access: block
      - security_level: 1
        access: monitor
  - dns_filter: advanced
    firewall_policies:
      - computers -> wan1
    filters:
      - security_level: 0
        access: block
      - security_level: 1
        access: block
blocklist_sources:
  - name_prefix: 'hagezi'
    url: 'https://cdn.jsdelivr.net/gh/hagezi/dns-blocklists@latest/adblock'
    postfix: ''
    sources:
      - name: 'pro.plus'
        security_level: 0
      - name: 'tif'
        security_level: 0
      - name: 'ultimate'
        security_level: 1
  - name_prefix: 'StevenBlack'
    url: 'https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts'
    postfix: ''
    sources:
      - name: 'default'
        security_level: 0
)";
    json parsed = parse_legacy_yaml(yaml);
    EXPECT_FALSE(parsed.contains("fortigate"));
    EXPECT_EQ(parsed.dump().find("SECRET"), std::string::npos);
    auto legacy = convert_legacy_config(parsed);
    EXPECT_EQ(legacy.feed_prefix, "forti-hole_");
    Config c = parse_config(legacy.config);
    EXPECT_EQ(c.tiers, (std::vector<std::string>{"level0", "level1"}));
    ASSERT_EQ(c.profiles.size(), 2u);
    EXPECT_EQ(legacy.config.dump().find("SECRET"), std::string::npos);
    bool stevenblack = false;
    for (const auto& s: c.sources)
        if (s.url == "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts/default") stevenblack = s.format == Format::hosts;
    EXPECT_TRUE(stevenblack);
    std::string notes;
    for (const auto& n: legacy.notes) notes += n + "\n";
    EXPECT_NE(notes.find("internal -> wan1"), std::string::npos);
    EXPECT_NE(notes.find("monitor"), std::string::npos);
    EXPECT_THROW(parse_legacy_yaml("a:\n\t- tab\n"), LegacyError);
}

TEST(Cli, VersionUsageAndErrors) {
    Harness h;
    EXPECT_EQ(h.run({"version"}), 0);
    EXPECT_NE(h.out.str().find("forti-hole "), std::string::npos);
    EXPECT_EQ(h.run({}), 2);
    EXPECT_EQ(h.run({"--help"}), 0);
    EXPECT_EQ(h.run({"bogus"}), 2);
    EXPECT_EQ(h.run({"sync", "--nope"}), 2);
    EXPECT_EQ(h.run({"compile"}), 3);   // no config yet
    int exit = 0;
    auto j = h.run_json({"compile"}, &exit);
    EXPECT_EQ(j["ok"], false);
    EXPECT_EQ(j["error"]["code"], "config");
}

TEST(Cli, SetupWritesADisabledConfigOnce) {
    Harness h;
    ASSERT_EQ(h.run({"setup", "--offline"}), 0) << h.err.str();
    Config c = load_config(h.temp.home.config());
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(h.run({"setup", "--offline"}), 3);
    EXPECT_EQ(h.run({"setup", "--offline", "--force", "--profile", "lab"}), 0);
    EXPECT_EQ(load_config(h.temp.home.config()).profile, "lab");
}

TEST(Cli, CompileReportsDeduplicationAndWritesFeeds) {
    Harness h;
    auto config = small_config();
    h.write_config(config);
    // Duplicates within a list and across lists.
    h.lists.pages["https://lists.test/ads.txt"].body = adblock_list(0, 300) + "||a1.example^\n||a2.example^\n";
    h.lists.pages["https://lists.test/ads.txt"].body.replace(0, h.lists.pages["https://lists.test/ads.txt"].body.find('\n'), "! Number of entries: 302");
    h.lists.pages["https://lists.test/tif.medium.txt"].body = adblock_list(0, 2000, "t") + "||a7.example^\n";
    h.lists.pages["https://lists.test/tif.medium.txt"].body.replace(0, h.lists.pages["https://lists.test/tif.medium.txt"].body.find('\n'), "! Number of entries: 2001");
    int exit = 0;
    auto j = h.run_json({"compile", "--write", (h.temp.root / "out").string()}, &exit);
    ASSERT_EQ(exit, 0) << h.out.str();
    const auto& totals = j["data"]["totals"];
    EXPECT_EQ(totals["duplicates_removed"].get<int>(), 3);
    EXPECT_EQ(totals["valid_parsed"].get<int>(), 400 + 302 + 2001);
    const auto& sources = j["data"]["sources"];
    EXPECT_EQ(sources[1]["duplicates_within"], 2);
    EXPECT_EQ(sources[2]["duplicates_across"], 1);
    EXPECT_TRUE(std::filesystem::exists(h.temp.root / "out" / "report.json"));
    for (const auto& feed: j["data"]["feeds"]) EXPECT_TRUE(std::filesystem::exists(h.temp.root / "out" / (feed["name"].get<std::string>() + ".txt")));
    // Same input, same hashes.
    auto again = h.run_json({"compile", "--offline"});
    EXPECT_EQ(again["data"]["feeds"], j["data"]["feeds"]);
}
