// NSFW keywords: whether a mod's name, or an option's, says it is NSFW (Slots Loader, docs/RUNTIME_OPTIONS.md "NSFW
// outfits"). Header only, no Windows API: the loader uses it, plugin/tests/rto_test.cpp checks it.
//
// The keyword list and the matching rule are ZZtaii's (Slotsloader Manager 1.0.8: LibraryFilters.NsfwKeywords,
// LibraryFilters.ParseKeywords, LibrarySearch.Compact / WordTokens / HasCompact), ported to C++ with his permission:
// - case, accents, spaces, dashes and every other non letter / digit character do not matter: a keyword matches when its
//   letters and digits appear as one run in the text ("see through" finds "See-Through" and "seethrough", never
//   "see ... through"; "no panties" never matches "Normal panties");
// - a keyword of one or two letters / digits must start a word of the text (one letter: be the whole word); words break
//   at spaces and punctuation, lower -> upper case, "NSFWMod" -> NSFW, Mod, and letter <-> digit; a word joined to the
//   next one counts too ("v", "2" -> "v2");
// - a text holding '/' is a folder path (an option's folders): each folder and the whole path count.
// Clothing words (bikini, lingerie, thong, skimpy) are left out of the default list on purpose; "sex" ("passexport"),
// "tless" ("hatless"), "transparent" (glasses) and "r18" (version numbers) matched harmless mods in his 2,700-mod library.
// Not ported: the character name in a mod's words (an NSFW keyword never names a fighter).
//
// Text: UTF-8, or the ANSI code page (Western: a byte that does not start a valid UTF-8 sequence is read as Latin-1).
// Accents dropped for Latin-1 and Latin Extended-A letters (U+00C0-U+017F, as Unicode NFD does); other letters are kept
// as they are, punctuation and symbol blocks dropped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nsfw {

inline const std::vector<std::string>& default_keywords() {
    static const std::vector<std::string> k = {
        "nsfw", "nude", "naked", "topless", "bottomless", "pubic", "pubes", "nipple", "pasties", "lewd", "futa",
        "see through", "see thru", "no panties", "no underwear", "erotic"
    };
    return k;
}

namespace detail {

// One character of a text: its code point, or kSep for a separator
constexpr uint32_t kSep = 0xFFFFFFFFu;

// Next code point at s[i] (i advanced): UTF-8 when the bytes form a valid sequence, else the byte as Latin-1
inline uint32_t next_cp(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    const size_t n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    if (n && i + n < s.size()) {
        uint32_t cp = c & (n == 1 ? 0x1Fu : n == 2 ? 0x0Fu : 0x07u);
        bool ok = true;
        for (size_t k = 1; ok && k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            ok = (d & 0xC0) == 0x80;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if (ok && cp >= (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) && cp <= 0x10FFFFu) { i += n + 1; return cp; }
    }
    ++i;
    return c;   // ANSI (Latin-1 for the Western code page)
}

inline void put_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Kinds: 0 separator, 1 lower case letter, 2 upper case letter, 3 other letter, 4 digit, 5 combining mark (dropped,
// not a separator)
struct Char { int kind = 0; std::string low; };   // low: what the character is in a compact text

inline Char classify(uint32_t cp) {
    Char c;
    if (cp < 0x80) {
        if (cp >= 'a' && cp <= 'z') { c.kind = 1; c.low = std::string(1, static_cast<char>(cp)); }
        else if (cp >= 'A' && cp <= 'Z') { c.kind = 2; c.low = std::string(1, static_cast<char>(cp - 'A' + 'a')); }
        else if (cp >= '0' && cp <= '9') { c.kind = 4; c.low = std::string(1, static_cast<char>(cp)); }
        return c;
    }
    if (cp >= 0x300 && cp <= 0x36F) { c.kind = 5; return c; }                  // combining accents (NFD input)
    if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) {
        // Latin-1 letters: the base letter, or the letter itself when it has no accent to drop (Æ Ð Ø Þ ß æ ð ø þ)
        static const char base[] = "aaaaaa*ceeeeiiii*nooooo**uuuuy**aaaaaa*ceeeeiiii*nooooo**uuuuy*y";
        const char b = base[cp - 0xC0];
        c.kind = cp <= 0xDE ? 2 : 1;
        if (b != '*') c.low = std::string(1, b);
        else put_utf8(c.low, cp <= 0xDE && cp != 0xDF ? cp + 0x20 : cp);
        return c;
    }
    if (cp == 0xAA || cp == 0xB5 || cp == 0xBA) { c.kind = 3; put_utf8(c.low, cp); return c; }
    if (cp >= 0x100 && cp <= 0x17F) {
        static const char base[] =
            "aaaaaaccccccccdd" "**eeeeeeeeeegggg" "gggghh**iiiiiiii" "i***jjkk*llllll*"
            "***nnnnnn***oooo" "oo**rrrrrrssssss" "sstttt**uuuuuuuu" "uuuuwwyyyzzzzzz*";
        const char b = base[cp - 0x100];
        const bool upper = (cp <= 0x137 || (cp >= 0x14A && cp <= 0x177)) ? (cp % 2 == 0)
                         : cp == 0x178 ? true : (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E) ? (cp % 2 == 1) : false;
        c.kind = upper ? 2 : 1;
        if (b != '*') c.low = std::string(1, b);
        else put_utf8(c.low, cp);
        return c;
    }
    // punctuation and symbol blocks (Latin-1 punctuation, general punctuation .. symbols, CJK punctuation, fullwidth
    // punctuation, private use, emoji): separators
    if (cp < 0xC0 || (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x3004) || (cp >= 0x3008 && cp <= 0x303F)
        || (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF0F)
        || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65)
        || cp >= 0x1F000 || cp == 0xFEFF)
        return c;
    c.kind = 3;                                                                 // any other letter: kept as it is
    put_utf8(c.low, cp);
    return c;
}

inline std::vector<Char> chars_of(const std::string& text) {
    std::vector<Char> out;
    for (size_t i = 0; i < text.size();) out.push_back(classify(next_cp(text, i)));
    return out;
}

inline bool letter(int k) { return k >= 1 && k <= 3; }

// LibrarySearch.WordTokens: the compact words of a text
inline std::vector<std::string> words(const std::string& text) {
    const std::vector<Char> cs = chars_of(text);
    std::vector<std::string> out;
    std::string cur;
    int prev = 0;   // kind of the last letter / digit of the current word (marks do not change it)
    auto flush = [&] { if (!cur.empty()) out.push_back(cur); cur.clear(); prev = 0; };
    for (size_t i = 0; i < cs.size(); ++i) {
        const int k = cs[i].kind;
        if (k == 0) { flush(); continue; }
        if (k == 5) continue;
        bool brk = false;
        if (prev == 1 && k == 2) brk = true;                                   // lower -> upper
        if (prev == 2 && k == 2) {                                             // "NSFWMod": a break before "Mod"
            size_t j = i + 1;
            while (j < cs.size() && cs[j].kind == 5) ++j;
            if (j < cs.size() && cs[j].kind == 1) brk = true;
        }
        if ((letter(prev) && k == 4) || (prev == 4 && letter(k))) brk = true;  // letter <-> digit
        if (brk) { out.push_back(cur); cur.clear(); }
        cur += cs[i].low;
        prev = k;
    }
    flush();
    return out;
}

inline bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

inline size_t chars_in(const std::string& compact) {
    size_t n = 0;
    for (unsigned char c : compact) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

} // namespace detail

// LibrarySearch.Compact: the letters and digits of a text, lower case, accents dropped ("C-Viper-Barefoot" ->
// "cviperbarefoot", "Élena" -> "elena")
inline std::string compact(const std::string& text) {
    std::string out;
    for (auto& c : detail::chars_of(text)) out += c.low;
    return out;
}

// LibraryFilters.ParseKeywords: commas, semicolons or new lines (and the fullwidth comma, the ideographic comma)
// between keywords; spaces inside one stay ("see through"); quotes around one dropped; a keyword without a letter or
// digit dropped; duplicates (ignoring case and spacing) kept once; 60 characters and 200 keywords at most
inline std::vector<std::string> parse_keywords(const std::string& text) {
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i < text.size();) {
        const size_t at = i;
        const uint32_t cp = detail::next_cp(text, i);
        if (cp == ',' || cp == ';' || cp == '\r' || cp == '\n' || cp == 0xFF0C || cp == 0x3001) { parts.push_back(cur); cur.clear(); }
        else cur.append(text, at, i - at);
    }
    parts.push_back(cur);
    std::vector<std::string> out, seen;
    for (auto& p : parts) {
        std::string k;   // spaces collapsed, trimmed
        bool space = false;
        for (char c : p) {
            if (c == ' ' || c == '\t') { space = !k.empty(); continue; }
            if (space) k += ' ';
            space = false;
            k += c;
        }
        while (!k.empty() && (k.front() == '"' || k.front() == '\'' || k.front() == ' ')) k.erase(0, 1);
        while (!k.empty() && (k.back() == '"' || k.back() == '\'' || k.back() == ' ')) k.pop_back();
        if (k.size() > 60) { k.resize(60); while (!k.empty() && k.back() == ' ') k.pop_back(); }
        const std::string key = compact(k);
        bool dup = false;
        for (auto& s : seen) dup = dup || s == key;
        if (key.empty() || dup) continue;
        seen.push_back(key);
        out.push_back(k);
        if (out.size() >= 200) break;
    }
    return out;
}

// The keyword of `keywords` the text holds (LibrarySearch.HasCompact), empty when none. A text holding '/' is a folder
// path: each folder and the whole path count.
inline std::string match(const std::vector<std::string>& keywords, const std::string& text) {
    if (text.empty() || keywords.empty()) return {};
    std::vector<std::string> segments;
    {
        size_t a = 0;
        while (a <= text.size()) {
            size_t b = text.find('/', a);
            if (b == std::string::npos) b = text.size();
            if (b > a) segments.push_back(text.substr(a, b - a));
            a = b + 1;
        }
    }
    std::vector<std::string> keys{compact(text)}, tokens;
    if (segments.size() > 1) for (auto& s : segments) keys.push_back(compact(s));
    for (auto& s : segments) {   // the words of each folder, and each word joined to the next
        const auto w = detail::words(s);
        for (size_t i = 0; i < w.size(); ++i) {
            tokens.push_back(w[i]);
            if (i + 1 < w.size()) tokens.push_back(w[i] + w[i + 1]);
        }
    }
    for (auto& k : keywords) {
        const std::string c = compact(k);
        if (c.empty()) continue;
        const size_t n = detail::chars_in(c);
        bool hit = false;
        if (n <= 2) {
            for (auto& t : tokens) hit = hit || (n == 1 ? t == c : detail::starts_with(t, c));
        } else {
            for (auto& key : keys) hit = hit || key.find(c) != std::string::npos;
        }
        if (hit) return k;
    }
    return {};
}

} // namespace nsfw
