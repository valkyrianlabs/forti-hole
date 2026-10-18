#include "core/parse.hpp"

#include "core/domain.hpp"

#include <array>
#include <charconv>
#include <format>

namespace fh {

    std::string_view to_string(Format format) {
        switch (format) {
            case Format::adblock: return "adblock";
            case Format::hosts: return "hosts";
            case Format::domains: return "domains";
            case Format::wildcard: return "wildcard";
            case Format::subtree_domains: return "subtree-domains";
            case Format::tranco: return "tranco";
        }
        return "adblock";
    }

    std::optional<Format> parse_format(std::string_view text) {
        for (Format format: {Format::adblock, Format::hosts, Format::domains, Format::wildcard, Format::subtree_domains, Format::tranco})
            if (to_string(format) == text) return format;
        return std::nullopt;
    }

    std::string_view to_string(Scope scope) { return scope == Scope::exact ? "exact" : "subtree"; }

    namespace {

        bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

        std::string_view trim(std::string_view text) {
            while (!text.empty() && space(text.front())) text.remove_prefix(1);
            while (!text.empty() && space(text.back())) text.remove_suffix(1);
            return text;
        }

        // "Number of entries: 229,327" -> 229327, in a comment line.
        std::optional<std::uint64_t> declared_count(std::string_view comment) {
            for (std::string_view key: {"Number of entries:", "Number of unique domains:"}) {
                auto at = comment.find(key);
                if (at == std::string_view::npos) continue;
                std::string digits;
                for (char c: comment.substr(at + key.size())) {
                    if (c >= '0' && c <= '9') digits.push_back(c);
                    else if (c == ',' || c == ' ' || c == '.' || c == '_') {
                        if (!digits.empty() && c == ' ') break;
                    } else break;
                }
                std::uint64_t value = 0;
                if (!digits.empty() && std::from_chars(digits.data(), digits.data() + digits.size(), value).ec == std::errc())
                    return value;
            }
            return std::nullopt;
        }

        class Parser {
            ParseStats& stats_;
            const EntrySink& sink_;
            std::uint64_t line_number_ = 0;

        public:
            Parser(ParseStats& stats, const EntrySink& sink) : stats_(stats), sink_(sink) {}

            void set_line(std::uint64_t number) { line_number_ = number; }

            void comment(std::string_view text) {
                ++stats_.comments;
                if (!stats_.declared)
                    if (auto count = declared_count(text)) stats_.declared = count;
            }

            void unsupported(std::string_view rule) {
                ++stats_.unsupported;
                if (stats_.first_unsupported.empty())
                    stats_.first_unsupported = std::format("line {}: {}", line_number_, rule.substr(0, 200));
            }

            // Canonicalizes and hands on one name; counts an invalid one.
            void emit(std::string_view raw, Scope scope, bool allow) {
                if (is_canonical(raw)) {
                    accept(raw, scope, allow, false);
                    return;
                }
                auto canonical = canonical_name(raw);
                if (!canonical.ok()) {
                    ++stats_.invalid;
                    auto reason = std::string(to_string(*canonical.error));
                    ++stats_.invalid_reasons[reason];
                    if (stats_.first_invalid.empty())
                        stats_.first_invalid = std::format("line {}: {}: {}", line_number_, raw.substr(0, 200), reason);
                    return;
                }
                accept(canonical.name, scope, allow, true);
            }

        private:
            void accept(std::string_view name, Scope scope, bool allow, bool changed) {
                ++stats_.accepted;
                if (allow) ++stats_.allow;
                if (changed) ++stats_.canonicalized;
                sink_(name, scope, allow);
            }
        };

        // A name token may contain only these before canonicalization; anything else (paths, ports,
        // wildcards, regex characters) makes the rule unsupported rather than invalid.
        bool plain_token(std::string_view token) {
            for (char c: token)
                if (c == '*' || c == '/' || c == ':' || c == '|' || c == '^' || c == '$' || c == '@' || c == '#' || c == '[' ||
                    c == ']' || c == '(' || c == ')' || c == '?' || c == '=' || c == '&' || c == ',' || c == ' ' || c == '\t' ||
                    c == '"' || c == '\'' || c == '\\' || c == '!' || c == '~' || c == '+' || c == '%')
                    return false;
            return !token.empty();
        }

        void adblock_line(Parser& parser, std::string_view line) {
            bool allow = false;
            std::string_view rule = line;
            if (rule.starts_with("@@")) {
                allow = true;
                rule.remove_prefix(2);
            }
            if (rule.starts_with("||")) {
                rule.remove_prefix(2);
                auto caret = rule.find('^');
                if (caret == std::string_view::npos) return parser.unsupported(line);
                std::string_view name = rule.substr(0, caret), rest = rule.substr(caret + 1);
                // "$important" only raises a rule's priority over exceptions in AdGuard: the name and
                // scope are unchanged. Every other modifier narrows or rewrites the rule.
                if (!rest.empty() && rest != "$important") return parser.unsupported(line);
                if (!plain_token(name)) return parser.unsupported(line);
                return parser.emit(name, Scope::subtree, allow);
            }
            if (!allow && plain_token(rule)) return parser.emit(rule, Scope::exact, false);
            parser.unsupported(line);
        }

        constexpr std::array<std::string_view, 6> sinkhole_addresses = {"0.0.0.0", "127.0.0.1", "::", "::1", "0", "0:0:0:0:0:0:0:0"};
        constexpr std::array<std::string_view, 12> housekeeping_names = {
                "localhost", "localhost.localdomain", "local", "broadcasthost", "ip6-localhost", "ip6-loopback",
                "localhost4", "localhost6", "ip6-localnet", "ip6-mcastprefix", "ip6-allnodes", "0.0.0.0"};

        void hosts_line(Parser& parser, ParseStats& stats, std::string_view line) {
            std::size_t at = 0;
            auto next = [&]() -> std::string_view {
                while (at < line.size() && space(line[at])) ++at;
                std::size_t start = at;
                while (at < line.size() && !space(line[at])) ++at;
                return line.substr(start, at - start);
            };
            std::string_view address = next();
            bool sinkhole = false;
            for (auto known: sinkhole_addresses) sinkhole = sinkhole || address == known;
            // The header of a hosts file maps the machine's own names; they are not blocklist entries.
            bool housekeeping_address = address == "255.255.255.255" || address.starts_with("fe80::") || address.starts_with("ff0") ||
                                        address == "127.0.1.1";
            std::size_t names = 0;
            for (auto name = next(); !name.empty(); name = next()) {
                ++names;
                bool housekeeping = housekeeping_address;
                for (auto skip: housekeeping_names) housekeeping = housekeeping || name == skip;
                if (housekeeping) {
                    ++stats.ignored;
                    continue;
                }
                if (!sinkhole) {
                    parser.unsupported(line);
                    continue;
                }
                if (!plain_token(name)) {
                    parser.unsupported(line);
                    continue;
                }
                parser.emit(name, Scope::exact, false);
            }
            if (names == 0) parser.unsupported(line);
        }

    }  // namespace

    ParseStats parse_list(std::string_view text, Format format, const EntrySink& sink) {
        ParseStats stats;
        Parser parser(stats, sink);
        std::size_t start = 0;
        std::uint64_t number = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            std::string_view raw = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            start = end == std::string_view::npos ? text.size() : end + 1;
            parser.set_line(++number);
            ++stats.lines;

            std::string_view line = trim(raw);
            if (line.empty()) {
                ++stats.blank;
                continue;
            }
            if (format == Format::adblock) {
                if (line.front() == '!' || line.front() == '[' || line.front() == '#') {
                    parser.comment(line);
                    continue;
                }
                ++stats.rules;
                adblock_line(parser, line);
                continue;
            }
            if (line.front() == '#') {
                parser.comment(line);
                continue;
            }
            // A trailing comment after the rule.
            if (auto hash = line.find('#'); hash != std::string_view::npos) line = trim(line.substr(0, hash));
            ++stats.rules;
            switch (format) {
                case Format::hosts: hosts_line(parser, stats, line); break;
                case Format::domains:
                    if (plain_token(line)) parser.emit(line, Scope::exact, false);
                    else parser.unsupported(line);
                    break;
                case Format::subtree_domains:
                    if (plain_token(line)) parser.emit(line, Scope::subtree, false);
                    else parser.unsupported(line);
                    break;
                case Format::wildcard:
                    if (line.starts_with("*.") && plain_token(line.substr(2))) parser.emit(line.substr(2), Scope::subtree, false);
                    else if (plain_token(line)) parser.emit(line, Scope::exact, false);
                    else parser.unsupported(line);
                    break;
                case Format::tranco: {
                    auto comma = line.find(',');
                    std::string_view name = comma == std::string_view::npos ? line : trim(line.substr(comma + 1));
                    if (plain_token(name)) parser.emit(name, Scope::exact, false);
                    else parser.unsupported(line);
                    break;
                }
                case Format::adblock: break;
            }
        }
        return stats;
    }

}  // namespace fh
