// model::lutil — the few Lua string / number behaviours the model has to reproduce (C locale, byte strings), and the
// character-safe cut of a name. Header only.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace model::lutil {

using json = nlohmann::json;

// %s of a Lua pattern in the C locale: space, \t \n \v \f \r
inline bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alnum(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

inline std::string lower(std::string_view s) { std::string r(s); for (char& c : r) c = to_lower(c); return r; }

// s:gsub("^%s+", ""):gsub("%s+$", "")
inline std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// normalize_name (MAIN:269): s:lower():gsub("[%.%s%-]", "")
inline std::string normalize_name(std::string_view s) {
    std::string r;
    for (char c : s) { if (c == '.' || c == '-' || is_space(c)) continue; r += to_lower(c); }
    return r;
}

// s:gsub("%.json$", "")
inline std::string strip_json_ext(std::string_view s) { return std::string(ends_with(s, ".json") ? s.substr(0, s.size() - 5) : s); }

// Cuts a UTF-8 string to at most max_bytes bytes, on a character boundary (Wael's decision 2026-10-09: the
// Lua cut with string.sub inside a multi-byte character, which made nlohmann's strict dump throw: the file was not
// written). The cut is moved back to the start of the character it would split.
inline std::string utf8_cut(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    size_t cut = max_bytes, back = 0;
    // s[cut] is the first byte left out: while it is a continuation byte (10xxxxxx) the cut is inside a character
    while (cut > 0 && back < 3 && ((unsigned char)s[cut] & 0xC0) == 0x80) { --cut; ++back; }
    if (((unsigned char)s[cut] & 0xC0) == 0x80) cut = max_bytes;     // more than 3 continuation bytes in a row: not UTF-8, plain cut
    return std::string(s.substr(0, cut));
}

// tostring() of a Lua number: an integer as %d, a float as %.14g with ".0" added when it looks like an integer.
inline std::string number_to_string(const json& n) {
    char b[64];
    if (n.is_number_integer()) {
        if (n.is_number_unsigned()) std::snprintf(b, sizeof(b), "%llu", (unsigned long long)n.get<uint64_t>());
        else std::snprintf(b, sizeof(b), "%lld", (long long)n.get<int64_t>());
        return b;
    }
    const double d = n.get<double>();
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    if (std::isnan(d)) return "nan";                          // Lua prints "-nan" or "nan" by platform; never in the mod's data
    std::snprintf(b, sizeof(b), "%.14g", d);
    std::string s = b;
    if (s.find_first_not_of("-0123456789") == std::string::npos) s += ".0";
    return s;
}

// tonumber(v) for a json value: a number is itself; a string that reads as a decimal number (Lua accepts spaces around,
// an optional sign, a fraction, an exponent; hexadecimal is not handled) gives that number; else nullopt.
inline std::optional<json> to_number(const json& v) {
    if (v.is_number()) return v;
    if (!v.is_string()) return std::nullopt;
    const std::string s = trim(v.get<std::string>());
    if (s.empty()) return std::nullopt;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    size_t digits = 0, j = i;
    while (j < s.size() && is_digit(s[j])) { ++j; ++digits; }
    bool is_float = false;
    if (j < s.size() && s[j] == '.') { is_float = true; ++j; while (j < s.size() && is_digit(s[j])) { ++j; ++digits; } }
    if (digits == 0) return std::nullopt;
    if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
        is_float = true; ++j;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
        size_t ed = 0;
        while (j < s.size() && is_digit(s[j])) { ++j; ++ed; }
        if (ed == 0) return std::nullopt;
    }
    if (j != s.size()) return std::nullopt;
    if (!is_float) {
        try { return json((int64_t)std::stoll(s)); } catch (...) { /* too large for an integer: a float, as Lua does */ }
    }
    try { return json(std::stod(s)); } catch (...) { return std::nullopt; }
}

// A number as a table key: Lua normalises a float with an integer value to the integer key. nullopt for a fraction.
inline std::optional<int64_t> to_int_key(const json& n) {
    if (n.is_number_integer()) return n.get<int64_t>();
    if (n.is_number_float()) {
        const double d = n.get<double>();
        if (std::floor(d) == d && std::fabs(d) < 9.0e15) return (int64_t)d;
    }
    return std::nullopt;
}

// Lua truthiness of a json value (nil and false are falsy)
inline bool truthy(const json& v) { return !(v.is_null() || (v.is_boolean() && !v.get<bool>())); }

}  // namespace model::lutil
