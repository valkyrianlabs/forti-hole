#include "core/domain.hpp"

#include <array>
#include <cstdint>

namespace fh {

    std::string_view to_string(NameError error) {
        switch (error) {
            case NameError::empty: return "empty";
            case NameError::too_long: return "longer than 253 characters";
            case NameError::label_empty: return "empty label";
            case NameError::label_too_long: return "label longer than 63 characters";
            case NameError::bad_character: return "character not allowed in a domain name";
            case NameError::bad_hyphen: return "label starts or ends with '-'";
            case NameError::single_label: return "single-label name";
            case NameError::numeric_tld: return "top-level label without a letter (an address?)";
            case NameError::bad_utf8: return "invalid UTF-8";
            case NameError::punycode_overflow: return "label cannot be punycode-encoded";
        }
        return "invalid";
    }

    namespace {

        constexpr std::array<bool, 256> label_chars = [] {
            std::array<bool, 256> table{};
            for (int c = 'a'; c <= 'z'; ++c) table[static_cast<unsigned char>(c)] = true;
            for (int c = '0'; c <= '9'; ++c) table[static_cast<unsigned char>(c)] = true;
            table['-'] = true;
            table['_'] = true;
            return table;
        }();

        // Validates an ASCII lowercase name; nullopt when valid.
        std::optional<NameError> check(std::string_view name) {
            if (name.empty()) return NameError::empty;
            if (name.size() > 253) return NameError::too_long;
            std::size_t labels = 0, start = 0;
            bool tld_has_letter = false;
            while (start <= name.size()) {
                auto dot = name.find('.', start);
                std::string_view label = name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
                if (label.empty()) return NameError::label_empty;
                if (label.size() > 63) return NameError::label_too_long;
                if (label.front() == '-' || label.back() == '-') return NameError::bad_hyphen;
                bool letter = false;
                for (unsigned char c: label) {
                    if (!label_chars[c]) return NameError::bad_character;
                    letter = letter || (c >= 'a' && c <= 'z');
                }
                ++labels;
                if (dot == std::string_view::npos) {
                    tld_has_letter = letter;
                    break;
                }
                start = dot + 1;
            }
            if (labels < 2) return NameError::single_label;
            if (!tld_has_letter) return NameError::numeric_tld;
            return std::nullopt;
        }

        // Decodes UTF-8; nullopt on malformed input (overlongs, surrogates, > U+10FFFF).
        std::optional<std::u32string> decode_utf8(std::string_view text) {
            std::u32string out;
            for (std::size_t i = 0; i < text.size();) {
                auto c = static_cast<unsigned char>(text[i]);
                std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
                if (length == 0 || i + length > text.size()) return std::nullopt;
                char32_t point = length == 1 ? c : length == 2 ? (c & 0x1F) : length == 3 ? (c & 0x0F) : (c & 0x07);
                for (std::size_t k = 1; k < length; ++k) {
                    auto next = static_cast<unsigned char>(text[i + k]);
                    if ((next >> 6) != 0x2) return std::nullopt;
                    point = (point << 6) | (next & 0x3F);
                }
                constexpr std::array<char32_t, 5> minimum = {0, 0, 0x80, 0x800, 0x10000};
                if (point < minimum[length] || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return std::nullopt;
                out.push_back(point);
                i += length;
            }
            return out;
        }

    }  // namespace

    std::optional<std::string> punycode_encode(std::u32string_view input) {
        // RFC 3492, section 6.3, with its parameters.
        constexpr std::uint32_t base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700, initial_bias = 72, initial_n = 0x80;
        constexpr std::uint32_t max = 0xFFFFFFFF;
        auto digit = [](std::uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); };
        auto adapt = [&](std::uint32_t delta, std::uint32_t points, bool first) {
            delta = first ? delta / damp : delta / 2;
            delta += delta / points;
            std::uint32_t k = 0;
            while (delta > ((base - tmin) * tmax) / 2) {
                delta /= base - tmin;
                k += base;
            }
            return k + (base - tmin + 1) * delta / (delta + skew);
        };

        std::string out;
        for (char32_t c: input)
            if (c < 0x80) out.push_back(static_cast<char>(c));
        auto basic = static_cast<std::uint32_t>(out.size());
        std::uint32_t handled = basic;
        if (basic > 0) out.push_back('-');

        std::uint32_t n = initial_n, delta = 0, bias = initial_bias;
        while (handled < input.size()) {
            std::uint32_t m = max;
            for (char32_t c: input)
                if (c >= n && c < m) m = c;
            if (m - n > (max - delta) / (handled + 1)) return std::nullopt;
            delta += (m - n) * (handled + 1);
            n = m;
            for (char32_t c: input) {
                if (c < n && ++delta == 0) return std::nullopt;
                if (c != n) continue;
                std::uint32_t q = delta;
                for (std::uint32_t k = base;; k += base) {
                    std::uint32_t t = k <= bias ? tmin : k >= bias + tmax ? tmax : k - bias;
                    if (q < t) break;
                    out.push_back(digit(t + (q - t) % (base - t)));
                    q = (q - t) / (base - t);
                }
                out.push_back(digit(q));
                bias = adapt(delta, handled + 1, handled == basic);
                delta = 0;
                ++handled;
            }
            ++delta;
            ++n;
        }
        // An all-ASCII input never reaches here with a trailing '-' of its own: the caller only
        // encodes labels that contain non-ASCII code points.
        return out;
    }

    bool is_canonical(std::string_view name) {
        if (name.empty() || name.back() == '.') return false;
        return !check(name);
    }

    CanonicalName canonical_name(std::string_view text) {
        if (!text.empty() && text.back() == '.') text.remove_suffix(1);
        if (text.empty()) return {{}, NameError::empty};
        std::string name;
        name.reserve(text.size());
        bool ascii = true;
        for (char c: text) {
            auto u = static_cast<unsigned char>(c);
            if (u >= 0x80) ascii = false;
            name.push_back(u >= 'A' && u <= 'Z' ? static_cast<char>(u - 'A' + 'a') : c);
        }
        if (!ascii) {
            std::string encoded;
            std::size_t start = 0;
            while (true) {
                auto dot = name.find('.', start);
                std::string_view label = std::string_view(name).substr(start, dot == std::string::npos ? std::string::npos : dot - start);
                bool plain = true;
                for (char c: label) plain = plain && static_cast<unsigned char>(c) < 0x80;
                if (plain) {
                    encoded += label;
                } else {
                    auto points = decode_utf8(label);
                    if (!points) return {{}, NameError::bad_utf8};
                    auto puny = punycode_encode(*points);
                    if (!puny) return {{}, NameError::punycode_overflow};
                    encoded += "xn--";
                    encoded += *puny;
                }
                if (dot == std::string::npos) break;
                encoded += '.';
                start = dot + 1;
            }
            name = std::move(encoded);
        }
        if (auto error = check(name)) return {{}, *error};
        return {std::move(name), std::nullopt};
    }

}  // namespace fh
