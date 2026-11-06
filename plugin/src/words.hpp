// SF6 Slots -- every word the plugin shows, in the game's language. Pure (no game access; plugin/tests).
//
// Two sources, never mixed on one text:
//   - W, the words of ours: plugin/locale/locale.json, generated into words_table.inc (tools/localisation/gen_plugin_words.py).
//     tr() gives the current language's text; a language or a word left out gives English, never a key or a blank.
//   - G, the words of the game: its own messages, by GUID (plugin/locale/game_words.json), asked of the game on the game thread
//     (resolve_game_words, common.cpp's language_tick) and kept here, so game() and the fighter names can be read from any thread.
//     English (the game's own English) until a message is found.
// The current language is set by common.cpp's language_tick (slots::game_language(), the one detection of the plugin).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>

namespace slots::words {

struct GameWordRow { const char* key; const char* guid; const char* english; };
struct FighterRow { int fid; const char* english; const char* name_guid; const char* caption_guid; };

#include "words_table.inc"

constexpr int kWordCount = static_cast<int>(W::Count);
constexpr int kGameCount = static_cast<int>(G::Count);
constexpr int kFighterCount = static_cast<int>(sizeof kFighterWords / sizeof kFighterWords[0]);

inline int lang_index(const std::string& code) {
    for (int i = 0; i < kLangCount; ++i)
        if (code == kLangs[i]) return i;
    return 0;                                     // kLangs[0] is English
}

// ---- the current language ----
inline std::atomic<int> g_lang{ 0 };
inline std::atomic<uint32_t> g_version{ 0 };      // +1 at each change: what was written in another language is written again
inline const char* lang() { return kLangs[g_lang.load(std::memory_order_relaxed)]; }
inline uint32_t version() { return g_version.load(std::memory_order_relaxed); }

// ---- the words of ours ----
inline const char* tr_in(W w, const std::string& code) {
    const char* t = kText[lang_index(code)][static_cast<int>(w)];
    return t && *t ? t : kText[0][static_cast<int>(w)];
}
inline const char* tr(W w) {
    const char* t = kText[g_lang.load(std::memory_order_relaxed)][static_cast<int>(w)];
    return t && *t ? t : kText[0][static_cast<int>(w)];
}
// The template's %s and %d, one after the other, take the arguments in order (%% is a percent sign); an argument missing leaves
// its place empty. Arguments are never read again, so a %s inside one stays as it is.
inline std::string format(const char* tmpl, std::initializer_list<std::string> args) {
    std::string out;
    auto a = args.begin();
    for (const char* p = tmpl; *p; ++p) {
        if (*p == '%' && (p[1] == 's' || p[1] == 'd')) {
            if (a != args.end()) out += *a++;
            ++p;
        } else if (*p == '%' && p[1] == '%') {
            out += '%';
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}
inline std::string trf(W w, std::initializer_list<std::string> args) { return format(tr(w), args); }

// Sentences one after the other: with a space, none in the languages written without spaces
inline std::string sentences(const std::string& a, const std::string& b) {
    const char* l = lang();
    const bool no_space = !std::strcmp(l, "ja") || !std::strcmp(l, "zh-Hans") || !std::strcmp(l, "zh-Hant");
    return no_space ? a + b : a + " " + b;
}

// ---- the words of the game ----
struct GameCache {
    std::mutex mx;
    std::string word[kGameCount];
    std::string fighter_name[kFighterCount], fighter_caption[kFighterCount];
    bool complete = false;
};
inline GameCache& game_cache() { static GameCache c; return c; }

// Game thread: asks the game (via.gui.message.get, the current language) for what is not known yet; "" = not found (yet)
using GameTextFn = std::string (*)(const char* guid);
inline void resolve_game_words(GameTextFn get) {
    GameCache& c = game_cache();
    if (c.complete) return;
    bool all = true;
    auto fill = [&](std::string& slot, const char* guid) {
        if (!slot.empty()) return;                // only the game thread writes: read without the lock, the game asked without it
        std::string t = guid ? get(guid) : std::string();
        if (t.empty()) { all = false; return; }
        std::lock_guard<std::mutex> lk(c.mx);
        slot = std::move(t);
    };
    for (int i = 0; i < kGameCount; ++i) fill(c.word[i], kGameWords[i].guid);
    for (int i = 0; i < kFighterCount; ++i) {
        fill(c.fighter_name[i], kFighterWords[i].name_guid);
        fill(c.fighter_caption[i], kFighterWords[i].caption_guid);
    }
    c.complete = all;
}
// Game thread: the language changed (or the first read)
inline void set_lang(const std::string& code) {
    const int i = lang_index(code);
    if (i == g_lang.load(std::memory_order_relaxed)) return;
    GameCache& c = game_cache();
    {
        std::lock_guard<std::mutex> lk(c.mx);
        for (auto& s : c.word) s.clear();
        for (auto& s : c.fighter_name) s.clear();
        for (auto& s : c.fighter_caption) s.clear();
        c.complete = false;
    }
    g_lang.store(i, std::memory_order_relaxed);
    g_version.fetch_add(1, std::memory_order_relaxed);
}
inline std::string game(G g) {
    GameCache& c = game_cache();
    std::lock_guard<std::mutex> lk(c.mx);
    const std::string& s = c.word[static_cast<int>(g)];
    return s.empty() ? std::string(kGameWords[static_cast<int>(g)].english) : s;
}
// A message with a {0} (a number): the game's word with its value in place
inline std::string game_arg(G g, const std::string& value) {
    std::string s = game(g);
    const size_t at = s.find("{0}");
    if (at != std::string::npos) s.replace(at, 3, value);
    return s;
}
// A fighter's name as the game writes it in a line ("Ryu") and as its grids and headers do ("RYU"). "" for a fighter not in the table
inline std::string fighter_name(int fid) {
    GameCache& c = game_cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (int i = 0; i < kFighterCount; ++i)
        if (kFighterWords[i].fid == fid) return c.fighter_name[i].empty() ? kFighterWords[i].english : c.fighter_name[i];
    return std::string();
}
inline std::string fighter_caption(int fid) {
    GameCache& c = game_cache();
    std::lock_guard<std::mutex> lk(c.mx);
    for (int i = 0; i < kFighterCount; ++i)
        if (kFighterWords[i].fid == fid) {
            if (!c.fighter_caption[i].empty()) return c.fighter_caption[i];
            std::string r = kFighterWords[i].english;          // English, in capitals: until the game's message is found
            for (auto& ch : r) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            return r;
        }
    return std::string();
}
// The game's name of one of its outfits, by costume number: 0 - 3 Outfit 1 - 4, 4 DriveTech, then Outfit N (N = the number)
inline std::string outfit_name(int costume) {
    if (costume >= 0 && costume <= 3) return game(static_cast<G>(static_cast<int>(G::Outfit1) + costume));
    if (costume == 4) return game(G::OutfitDrivetech);
    return game_arg(G::OutfitN, std::to_string(costume));
}
// A label written in English by the loader's files for one of the game's outfits ("Outfit 2", "Drive Tech", "DriveTech"): the
// game's name. Anything else (a mod's name, the loader's "Outfit VII"): as it is, a name is never translated.
inline std::string outfit_label(const std::string& label) {
    if (label == "DriveTech" || label == "Drive Tech") return outfit_name(4);
    if (label.size() > 7 && label.compare(0, 7, "Outfit ") == 0) {
        int n = 0;
        for (size_t i = 7; i < label.size(); ++i) {
            if (label[i] < '0' || label[i] > '9' || n > 999) return label;
            n = n * 10 + (label[i] - '0');
        }
        return n >= 1 && n <= 4 ? outfit_name(n - 1) : game_arg(G::OutfitN, std::to_string(n));
    }
    return label;
}

// ---- how much room a text takes ----
// Units, about half the font size each (the game's fonts, measured 05/11: 0.41 to 0.47 em for a Latin letter): 2 for a CJK,
// kana, Hangul or full-width character, 1 for any other. What the max of plugin/locale/locale.json counts.
inline bool is_wide(unsigned cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFF);
}
inline unsigned utf8_next(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    unsigned cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += static_cast<size_t>(n);
    return cp;
}
inline size_t units(const std::string& s) {
    size_t u = 0;
    for (size_t i = 0; i < s.size();) u += is_wide(utf8_next(s, i)) ? 2 : 1;
    return u;
}
// Cut to at most `max` units, "..." at its end, whole characters only
inline std::string cut_units(const std::string& s, size_t max) {
    if (units(s) <= max || max < 4) return s;
    size_t u = 0, end = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t at = i;
        u += is_wide(utf8_next(s, i)) ? 2 : 1;
        if (u > max - 3) { end = at; break; }
    }
    return s.substr(0, end) + "...";
}

}  // namespace slots::words
