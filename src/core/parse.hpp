//
// Blocklist parsers: one pass over a downloaded list, no regular expressions.
//
// Every rule becomes an entry with a scope, because FortiOS matches differently from the lists'
// own syntax and forti-hole has to carry the meaning across (compile.hpp encodes it):
//
//   exact    the name only                          hosts files, plain domain lists
//   subtree  the name and every name below it        adblock "||name^", wildcard "*.name"
//
// Formats (Source::format in config.json):
//
//   adblock          "||name^" (subtree), "@@||name^" (allow, subtree), "$important" tolerated; other
//                    modifiers, paths, mid-name wildcards and cosmetic rules are unsupported and
//                    counted. A bare "name" line is exact (as Pi-hole reads it), never widened.
//   hosts            "0.0.0.0 name [name...]" with a sinkhole address (0.0.0.0, 127.0.0.1, ::, ::1);
//                    localhost-style names are ignored; other addresses (redirects) are unsupported.
//   domains          one name per line (exact).
//   wildcard         "*.name" (subtree) and "name" (exact).
//   subtree-domains  one name per line, each covering its subdomains (hagezi "onlydomains" lists).
//   tranco           "rank,name" CSV (the reference list compact encoding checks against).
//
// '#' starts a comment in every format but adblock, where '!' does (and '#' lines are skipped).
//

#ifndef FORTI_HOLE_CORE_PARSE_HPP
#define FORTI_HOLE_CORE_PARSE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace fh {

    enum class Format { adblock, hosts, domains, wildcard, subtree_domains, tranco };

    [[nodiscard]] std::string_view to_string(Format format);
    [[nodiscard]] std::optional<Format> parse_format(std::string_view text);

    enum class Scope : std::uint8_t { exact, subtree };

    [[nodiscard]] std::string_view to_string(Scope scope);

    struct ParseStats {
        std::uint64_t lines = 0;          // every line, blank and comment included
        std::uint64_t blank = 0;
        std::uint64_t comments = 0;
        std::uint64_t rules = 0;          // lines that are neither blank nor comments
        std::uint64_t accepted = 0;       // valid entries handed on (block and allow)
        std::uint64_t allow = 0;          // of accepted: allow (exception) rules
        std::uint64_t canonicalized = 0;  // of accepted: spelled differently in the list (case, dot, IDN)
        std::uint64_t invalid = 0;        // rules whose name is malformed
        std::uint64_t unsupported = 0;    // rules forti-hole cannot express (modifiers, paths, redirects)
        std::uint64_t ignored = 0;        // hosts-file housekeeping (localhost, broadcasthost, ...)
        // "Number of entries: N" (hagezi), "Number of unique domains: N" (StevenBlack) in the header.
        std::optional<std::uint64_t> declared;
        // Up to a few examples per kind of rejection, for the report ("line 12: bad..name: empty label").
        std::map<std::string, std::uint64_t, std::less<>> invalid_reasons;
        std::string first_invalid, first_unsupported;
    };

    // Called once per accepted entry. The name is canonical; it may point into the parsed text, so
    // copy it to keep it.
    using EntrySink = std::function<void(std::string_view name, Scope scope, bool allow)>;

    ParseStats parse_list(std::string_view text, Format format, const EntrySink& sink);

}  // namespace fh

#endif  // FORTI_HOLE_CORE_PARSE_HPP
