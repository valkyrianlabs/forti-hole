#include "core/domain.hpp"
#include "core/parse.hpp"
#include "core/sha256.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace fh;

namespace {

    struct Collected {
        std::vector<std::tuple<std::string, Scope, bool>> entries;
        ParseStats stats;
    };

    Collected parse(std::string_view text, Format format) {
        Collected c;
        c.stats = parse_list(text, format, [&](std::string_view name, Scope scope, bool allow) { c.entries.emplace_back(std::string(name), scope, allow); });
        return c;
    }

}  // namespace

TEST(Domain, CanonicalFormLowercasesAndDropsTheTrailingDot) {
    EXPECT_EQ(canonical_name("Ads.Example.COM.").name, "ads.example.com");
    EXPECT_TRUE(is_canonical("ads.example.com"));
    EXPECT_FALSE(is_canonical("Ads.example.com"));
    EXPECT_FALSE(is_canonical("ads.example.com."));
}

TEST(Domain, InternationalizedLabelsBecomePunycode) {
    // RFC 3492 / well-known encodings.
    EXPECT_EQ(canonical_name("bücher.example").name, "xn--bcher-kva.example");
    EXPECT_EQ(canonical_name("münchen.de").name, "xn--mnchen-3ya.de");
    EXPECT_EQ(canonical_name("例え.テスト").name, "xn--r8jz45g.xn--zckzah");
    EXPECT_EQ(canonical_name("xn--bcher-kva.example").name, "xn--bcher-kva.example");
}

TEST(Domain, RejectsMalformedNames) {
    EXPECT_EQ(canonical_name("").error, NameError::empty);
    EXPECT_EQ(canonical_name("bad..name").error, NameError::label_empty);
    EXPECT_EQ(canonical_name(".lead.example").error, NameError::label_empty);
    EXPECT_EQ(canonical_name("-lead.example").error, NameError::bad_hyphen);
    EXPECT_EQ(canonical_name("trail-.example").error, NameError::bad_hyphen);
    EXPECT_EQ(canonical_name("localhost").error, NameError::single_label);
    EXPECT_EQ(canonical_name("1.2.3.4").error, NameError::numeric_tld);
    EXPECT_EQ(canonical_name("a b.example").error, NameError::bad_character);
    EXPECT_EQ(canonical_name("a*b.example").error, NameError::bad_character);
    EXPECT_EQ(canonical_name(std::string(64, 'a') + ".example").error, NameError::label_too_long);
    std::string long_name;
    for (int i = 0; i < 50; ++i) long_name += "abcde.";
    long_name += "com";
    EXPECT_EQ(canonical_name(long_name).error, NameError::too_long);
    EXPECT_EQ(canonical_name("\xff\xfe.example").error, NameError::bad_utf8);
    // Digits-only labels are fine below the top level; underscores appear in real hostnames.
    EXPECT_TRUE(canonical_name("123.example.com").ok());
    EXPECT_TRUE(canonical_name("_dmarc.example.com").ok());
}

TEST(Domain, WithinAndParent) {
    EXPECT_TRUE(within("a.example.com", "example.com"));
    EXPECT_TRUE(within("example.com", "example.com"));
    EXPECT_FALSE(within("badexample.com", "example.com"));
    EXPECT_EQ(parent_of("a.b.c"), "b.c");
    EXPECT_EQ(parent_of("c"), "");
}

TEST(Parse, AdblockRulesAreSubtreesAndExceptionsAllow) {
    auto c = parse("[Adblock Plus]\n! Title: test\n! Number of entries: 4\n||ads.example.com^\n@@||ok.example.com^\n"
                   "||Tracker.Example.^$important\n||x.example^$third-party\n||path.example^/ads\nplain.example\n##.banner\n",
                   Format::adblock);
    ASSERT_EQ(c.entries.size(), 4u);
    EXPECT_EQ(c.entries[0], std::make_tuple(std::string("ads.example.com"), Scope::subtree, false));
    EXPECT_EQ(c.entries[1], std::make_tuple(std::string("ok.example.com"), Scope::subtree, true));
    EXPECT_EQ(c.entries[2], std::make_tuple(std::string("tracker.example"), Scope::subtree, false));
    // A bare name in an adblock list is exact: never widened to its subdomains.
    EXPECT_EQ(c.entries[3], std::make_tuple(std::string("plain.example"), Scope::exact, false));
    EXPECT_EQ(c.stats.unsupported, 2u);   // $third-party, a path (a "##" cosmetic rule reads as a comment)
    EXPECT_EQ(c.stats.allow, 1u);
    EXPECT_EQ(c.stats.canonicalized, 1u);
    EXPECT_EQ(c.stats.declared, 4u);
    EXPECT_EQ(c.stats.comments, 4u);
}

TEST(Parse, HostsFilesKeepOnlySinkholedNames) {
    auto c = parse("# Number of unique domains: 3\n127.0.0.1 localhost\n::1 localhost ip6-localhost\n255.255.255.255 broadcasthost\n"
                   "0.0.0.0 0.0.0.0\n0.0.0.0 ads.example.com # comment\n127.0.0.1 a.example b.example\r\n10.0.0.5 redirect.example\n0.0.0.0 bad..name\n",
                   Format::hosts);
    ASSERT_EQ(c.entries.size(), 3u);
    EXPECT_EQ(std::get<0>(c.entries[0]), "ads.example.com");
    EXPECT_EQ(std::get<1>(c.entries[0]), Scope::exact);
    EXPECT_EQ(std::get<0>(c.entries[2]), "b.example");
    EXPECT_EQ(c.stats.ignored, 5u);
    EXPECT_EQ(c.stats.unsupported, 1u);   // a redirect to a real address
    EXPECT_EQ(c.stats.invalid, 1u);
    EXPECT_EQ(c.stats.declared, 3u);
    EXPECT_FALSE(c.stats.first_invalid.empty());
}

TEST(Parse, DomainWildcardAndSubtreeFormats) {
    auto plain = parse("# c\nOne.Example\ntwo.example # trailing\n*.three.example\n", Format::domains);
    ASSERT_EQ(plain.entries.size(), 2u);
    EXPECT_EQ(plain.stats.unsupported, 1u);
    auto wildcard = parse("*.w.example\nexact.example\n*mid.example\n", Format::wildcard);
    ASSERT_EQ(wildcard.entries.size(), 2u);
    EXPECT_EQ(std::get<1>(wildcard.entries[0]), Scope::subtree);
    EXPECT_EQ(std::get<1>(wildcard.entries[1]), Scope::exact);
    auto subtree = parse("s.example\n", Format::subtree_domains);
    EXPECT_EQ(std::get<1>(subtree.entries[0]), Scope::subtree);
    auto tranco = parse("1,google.com\n2,Example.COM\n", Format::tranco);
    ASSERT_EQ(tranco.entries.size(), 2u);
    EXPECT_EQ(std::get<0>(tranco.entries[1]), "example.com");
}

TEST(Parse, EmptyAndCommentOnlyInput) {
    auto c = parse("", Format::adblock);
    EXPECT_TRUE(c.entries.empty());
    auto d = parse("\n\n! only comments\n", Format::adblock);
    EXPECT_EQ(d.stats.lines, 3u);
    EXPECT_EQ(d.stats.blank, 2u);
    EXPECT_EQ(d.stats.accepted, 0u);
}

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    EXPECT_EQ(sha256_hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Incremental equals one-shot, across block boundaries.
    Sha256 h;
    for (int i = 0; i < 1000; ++i) h.update(std::string(1000, 'a'));
    EXPECT_EQ(h.hex(), sha256_hex(million));
}
