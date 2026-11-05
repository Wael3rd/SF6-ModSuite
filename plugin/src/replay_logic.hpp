// SF6 Slots — "Replay Outfits" (the Replays menu's 6th tab): pure logic, no game access (plugin/tests).
// The game side is replay_tab.cpp.
//
// The data file, read by the replay playback override (another module of this plugin):
//   reframework/data/SF6_CostumeSlots_data/replay_outfits.json
//   {"version":1,"fighters":{"<fighter id>":{"mode":"forced","costume":<FighterDesc.Costume>,
//                                            "mod_id":"<registry mod_id, extra outfits only>","color":<FighterDesc.Color>}}}
// No entry = Default (the replay's own outfit).
#pragma once

#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace slots::replay {

constexpr const char* kFile = "reframework/data/SF6_CostumeSlots_data/replay_outfits.json";

struct Outfit {
    int costume = 0;                  // FighterDesc.Costume
    std::string mod_id;               // registry.json mod_id of an extra outfit, "" for a game outfit
    std::string name;                 // what the row shows ("Outfit 2", "Aloha Ryu")
    std::vector<int> colors;          // the colours that exist for it (FighterDesc.Color), ascending
};

struct Fighter {
    int fid = 0;
    std::string name;
    std::vector<Outfit> outfits;      // game outfits (by costume number), then extra outfits (by name)
};

struct Choice {
    bool forced = false;              // false = Default (no entry in the file)
    int costume = 0;
    std::string mod_id;
    int color = 0;
    bool operator==(const Choice& o) const {
        return forced == o.forced && (!forced || (costume == o.costume && mod_id == o.mod_id && color == o.color));
    }
};

using Choices = std::map<int, Choice>;   // fighter id -> forced choice (Default fighters are absent)

// The outfit list of a fighter: game outfits first in costume order, then extra outfits by name (case-insensitive),
// ties by costume number. Outfits without any colour are dropped.
inline void sort_outfits(std::vector<Outfit>& v) {
    auto lower = [](std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return s; };
    v.erase(std::remove_if(v.begin(), v.end(), [](const Outfit& o) { return o.colors.empty(); }), v.end());
    std::stable_sort(v.begin(), v.end(), [&](const Outfit& a, const Outfit& b) {
        const bool ea = !a.mod_id.empty(), eb = !b.mod_id.empty();
        if (ea != eb) return !ea;
        if (!ea) return a.costume < b.costume;
        const std::string la = lower(a.name), lb = lower(b.name);
        if (la != lb) return la < lb;
        return a.costume < b.costume;
    });
}

// The outfit of a choice in the fighter's list: -1 for Default or an outfit no longer listed
inline int outfit_index(const Fighter& f, const Choice& c) {
    if (!c.forced) return -1;
    for (size_t i = 0; i < f.outfits.size(); ++i) {
        const Outfit& o = f.outfits[i];
        if (o.costume == c.costume && o.mod_id == c.mod_id) return static_cast<int>(i);
    }
    return -1;
}

// The colour a choice gets on outfit `o`: `keep` when the outfit has it, else its first colour
inline int color_on(const Outfit& o, int keep) {
    for (int c : o.colors) if (c == keep) return c;
    return o.colors.empty() ? 0 : o.colors.front();
}

// Left / Right on a row: Default -> outfit 0 -> ... -> last outfit -> Default (dir +1), the other way for -1.
// The colour is kept when the new outfit has it.
inline Choice cycle_outfit(const Fighter& f, const Choice& c, int dir) {
    const int n = static_cast<int>(f.outfits.size());
    if (n == 0) return Choice{};
    int pos = outfit_index(f, c) + 1;                       // 0 = Default, 1..n = outfits
    pos = ((pos + (dir >= 0 ? 1 : -1)) % (n + 1) + (n + 1)) % (n + 1);
    if (pos == 0) return Choice{};
    const Outfit& o = f.outfits[static_cast<size_t>(pos - 1)];
    Choice r;
    r.forced = true;
    r.costume = o.costume;
    r.mod_id = o.mod_id;
    r.color = color_on(o, c.forced ? c.color : 0);
    return r;
}

// The colour key: the next (dir +1) or previous colour of the chosen outfit, wrapping. Default stays Default.
inline Choice cycle_color(const Fighter& f, const Choice& c, int dir) {
    const int i = outfit_index(f, c);
    if (i < 0) return c;
    const Outfit& o = f.outfits[static_cast<size_t>(i)];
    if (o.colors.empty()) return c;
    const int n = static_cast<int>(o.colors.size());
    int at = -1;
    for (int k = 0; k < n; ++k) if (o.colors[static_cast<size_t>(k)] == c.color) at = k;
    Choice r = c;
    if (at < 0) r.color = o.colors.front();
    else r.color = o.colors[static_cast<size_t>(((at + (dir >= 0 ? 1 : -1)) % n + n) % n)];
    return r;
}

// "Color 3" for the game's colours 0-9 (shown 1-10); the special colours (200+) as the game numbers them past 10
inline std::string color_label(int color) {
    char b[32];
    if (color >= 0 && color < 100) std::snprintf(b, sizeof b, "Color %d", color + 1);
    else if (color >= 200 && color < 300) std::snprintf(b, sizeof b, "Color %d", color - 200 + 11);
    else std::snprintf(b, sizeof b, "Color #%d", color);
    return b;
}

// What a row shows on its right: "Default", or "<outfit name> - Color N"
inline std::string row_text(const Fighter& f, const Choice& c) {
    const int i = outfit_index(f, c);
    if (i < 0) return "Default";
    return f.outfits[static_cast<size_t>(i)].name + " - " + color_label(c.color);
}

// ---- the file ------------------------------------------------------------------------------------------------------------
inline std::string to_json(const Choices& cs) {
    std::string o = "{\n    \"version\": 1,\n    \"fighters\": {";
    bool first = true;
    for (const auto& [fid, c] : cs) {
        if (!c.forced) continue;
        o += first ? "\n" : ",\n";
        first = false;
        o += "        \"" + std::to_string(fid) + "\": {\"mode\": \"forced\", \"costume\": " + std::to_string(c.costume);
        if (!c.mod_id.empty()) o += ", \"mod_id\": \"" + json_escape(c.mod_id) + "\"";
        o += ", \"color\": " + std::to_string(c.color) + "}";
    }
    o += first ? "}\n}\n" : "\n    }\n}\n";
    return o;
}

// false: not a file of this format (out left empty). Entries that are not "forced" or lack a costume are skipped.
inline bool from_json(const std::string& text, Choices& out) {
    out.clear();
    Json j;
    if (!json_parse(text, j) || j.kind != Json::Object) return false;
    const Json* v = j.get("version");
    if (!v || v->as_int(-1) != 1) return false;
    const Json* fs = j.get("fighters");
    if (!fs || fs->kind != Json::Object) return false;
    for (const auto& [key, e] : fs->obj) {
        if (e.kind != Json::Object) continue;
        const Json* mode = e.get("mode");
        const Json* cos = e.get("costume");
        if (!mode || mode->as_str() != "forced" || !cos || cos->kind != Json::Number) continue;
        char* end = nullptr;
        const long fid = std::strtol(key.c_str(), &end, 10);
        if (!end || *end || fid <= 0) continue;
        Choice c;
        c.forced = true;
        c.costume = static_cast<int>(cos->as_int());
        if (const Json* m = e.get("mod_id")) c.mod_id = m->as_str();
        if (const Json* col = e.get("color")) c.color = static_cast<int>(col->as_int(0));
        out[static_cast<int>(fid)] = c;
    }
    return true;
}

// ---- the page's cursor and window ----------------------------------------------------------------------------------------
// Up / Down wrap around the list (the last row's Down goes to the first)
inline int move_cursor(int cursor, int dir, int count) {
    if (count <= 0) return 0;
    return ((cursor + dir) % count + count) % count;
}
// The first row shown so that the cursor stays inside a window of `rows` rows
inline int window_top(int top, int cursor, int count, int rows) {
    if (count <= rows) return 0;
    if (cursor < top) top = cursor;
    if (cursor >= top + rows) top = cursor - rows + 1;
    return std::max(0, std::min(top, count - rows));
}

// Fighter order: the game's Opponent Outfit Settings grid (launch roster, then DLC by release); ids not listed follow,
// ascending
inline const std::vector<int>& grid_order() {
    static const std::vector<int> v = { 1, 2, 21, 4, 18, 3, 16, 10, 15, 8, 20, 11, 5, 17, 7, 6, 12, 9,
                                        14, 13, 19, 22, 26, 27, 28, 29, 25, 30, 31, 32, 33, 34 };
    return v;
}
inline void order_fighters(std::vector<Fighter>& fs) {
    const auto& g = grid_order();
    auto rank = [&](int fid) {
        for (size_t i = 0; i < g.size(); ++i) if (g[i] == fid) return static_cast<int>(i);
        return 1000 + fid;
    };
    std::stable_sort(fs.begin(), fs.end(), [&](const Fighter& a, const Fighter& b) { return rank(a.fid) < rank(b.fid); });
}
inline const char* fighter_name(int fid) {
    switch (fid) {
    case 1: return "Ryu";        case 2: return "Luke";      case 3: return "Kimberly";  case 4: return "Chun-Li";
    case 5: return "Manon";      case 6: return "Zangief";   case 7: return "JP";        case 8: return "Dhalsim";
    case 9: return "Cammy";      case 10: return "Ken";      case 11: return "Dee Jay";  case 12: return "Lily";
    case 13: return "A.K.I.";    case 14: return "Rashid";   case 15: return "Blanka";   case 16: return "Juri";
    case 17: return "Marisa";    case 18: return "Guile";    case 19: return "Ed";       case 20: return "E. Honda";
    case 21: return "Jamie";     case 22: return "Akuma";    case 25: return "Sagat";    case 26: return "M. Bison";
    case 27: return "Terry";     case 28: return "Mai";      case 29: return "Elena";    case 30: return "C. Viper";
    case 31: return "Alex";      case 32: return "Ingrid";   case 33: return "Yasmine";  case 34: return "Arjun";
    default: return nullptr;
    }
}
// The game's outfit names by costume number (what Battle Settings shows)
inline std::string game_outfit_name(int costume) {
    if (costume == 4) return "DriveTech";
    return "Outfit " + std::to_string(costume < 4 ? costume + 1 : costume);
}

}  // namespace slots::replay
