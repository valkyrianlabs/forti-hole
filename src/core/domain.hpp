//
// DNS names as forti-hole stores and emits them: canonical, validated, ASCII.
//
// canonical_name() turns one token from a list into the form FortiOS will match, or says why it
// cannot. The form is what resolvers put on the wire: lowercase ASCII, no trailing dot, internationalized
// labels as punycode ("xn--..."). Two spellings of one name (case, trailing dot, UTF-8 vs punycode)
// therefore deduplicate to one entry.
//
// Internationalized labels are lowercased only in their ASCII letters and then punycode-encoded
// (RFC 3492). Full UTS #46 mapping (Unicode case folding, NFC) is not applied: blocklists publish
// punycode already, so a raw UTF-8 label is rare, and mapping tables would be a dependency.
//

#ifndef FORTI_HOLE_CORE_DOMAIN_HPP
#define FORTI_HOLE_CORE_DOMAIN_HPP

#include <optional>
#include <string>
#include <string_view>

namespace fh {

    enum class NameError {
        empty,
        too_long,           // more than 253 characters
        label_empty,        // "a..b", ".a"
        label_too_long,     // a label of more than 63 characters
        bad_character,      // anything but letters, digits, '-' and '_' (after punycode)
        bad_hyphen,         // a label starting or ending with '-'
        single_label,       // "localhost": FortiOS feeds hold domain names
        numeric_tld,        // "1.2.3.4" and other names whose last label has no letter
        bad_utf8,
        punycode_overflow,
    };

    [[nodiscard]] std::string_view to_string(NameError error);

    struct CanonicalName {
        std::string name;                 // empty when error is set
        std::optional<NameError> error;
        [[nodiscard]] bool ok() const { return !error; }
    };

    // Canonical form of one name: ASCII letters lowercased, one trailing dot dropped, non-ASCII labels
    // punycode-encoded, then validated (RFC 1035 lengths; letters, digits, '-', '_'; at least two
    // labels; a top-level label with a letter).
    [[nodiscard]] CanonicalName canonical_name(std::string_view text);

    // True when `name` is a valid canonical name (what canonical_name returns), without copying.
    // The fast path for list lines that are already canonical, which is nearly all of them.
    [[nodiscard]] bool is_canonical(std::string_view name);

    // RFC 3492 punycode of a label's Unicode code points (no "xn--" prefix). nullopt on overflow.
    [[nodiscard]] std::optional<std::string> punycode_encode(std::u32string_view label);

    // The name with its leftmost label removed ("a.b.c" -> "b.c"); empty for a single label.
    [[nodiscard]] constexpr std::string_view parent_of(std::string_view name) {
        auto dot = name.find('.');
        return dot == std::string_view::npos ? std::string_view() : name.substr(dot + 1);
    }

    // True when `name` equals `ancestor` or lies below it ("a.b.c" is within "b.c", "xb.c" is not).
    [[nodiscard]] constexpr bool within(std::string_view name, std::string_view ancestor) {
        if (name.size() == ancestor.size()) return name == ancestor;
        return name.size() > ancestor.size() && name.ends_with(ancestor) && name[name.size() - ancestor.size() - 1] == '.';
    }

}  // namespace fh

#endif  // FORTI_HOLE_CORE_DOMAIN_HPP
