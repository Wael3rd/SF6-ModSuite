// SF6 Slots — "Replay Outfits" (the Replays menu's 6th tab): pure logic, no game access (plugin/tests).
// The game side is replay_tab.cpp.
//
// The data file, read by the replay playback override (another module of this plugin):
//   reframework/data/SF6_CostumeSlots_data/replay_outfits.json
//   {"version":1,"fighters":{"<fighter id>":{"mode":"forced","costume":<FighterDesc.Costume>,
//                                            "mod_id":"<registry mod_id, extra outfits only>","color":<FighterDesc.Color>,
//                                            "colormod":"<a Color Mod entry's id: its file path; colour 1 then>"}},
//    "stage":{"mode":"forced","id":<StageDesc.StageId>}}
// No entry = Default (the replay's own outfit); no "stage" = the replay's own stage.
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

// A Color Mod entry (RW / MC) of an outfit: not a game colour, it rides on colour 1 (colormod/include/sf6colormod_entries.h)
struct ColorEntry {
    std::string id;                   // stable id: the entry file's path
    std::string label;                // the Color Mod's own label ("RW 3", "MC 1")
};
constexpr int kColorModCarrier = 1;   // the game colour every Color Mod entry rides on ("02")

struct Outfit {
    int costume = 0;                  // FighterDesc.Costume
    std::string mod_id;               // registry.json mod_id of an extra outfit ("" for a game outfit, or when unknown)
    std::string name;                 // what the row shows ("Outfit 2", "Aloha Ryu")
    bool extra = false;               // one of the Slots Loader's outfits
    std::vector<int> colors;          // the colours that exist for it (FighterDesc.Color), ascending
    std::vector<ColorEntry> cm;       // the Color Mod's entries for it, its order (after the game colours on the page)
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
    std::string colormod;             // a Color Mod entry's id (color is then kColorModCarrier); "" = the game colour
    bool operator==(const Choice& o) const {
        return forced == o.forced &&
               (!forced || (costume == o.costume && mod_id == o.mod_id && color == o.color && colormod == o.colormod));
    }
};

using Choices = std::map<int, Choice>;   // fighter id -> forced choice (Default fighters are absent)

struct StageChoice {
    bool forced = false;              // false = Default (the replay's recorded stage)
    int id = 0;                       // StageDesc.StageId (the game's stage table ManageId)
    bool operator==(const StageChoice& o) const { return forced == o.forced && (!forced || id == o.id); }
};

// The outfit list of a fighter: game outfits first in costume order, then extra outfits by name (case-insensitive),
// ties by costume number. Outfits without any colour are dropped.
inline void sort_outfits(std::vector<Outfit>& v) {
    auto lower = [](std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return s; };
    v.erase(std::remove_if(v.begin(), v.end(), [](const Outfit& o) { return o.colors.empty(); }), v.end());
    std::stable_sort(v.begin(), v.end(), [&](const Outfit& a, const Outfit& b) {
        const bool ea = a.extra, eb = b.extra;
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
// keep_color: the colour is kept when the new outfit has it; otherwise (a costume change on the page) the outfit's first
// colour, "01".
inline Choice cycle_outfit(const Fighter& f, const Choice& c, int dir, bool keep_color = true) {
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
    r.color = color_on(o, c.forced && keep_color ? c.color : 0);
    if (c.forced && keep_color && !c.colormod.empty())        // a Color Mod entry: kept only when this outfit has it too
        for (const ColorEntry& e : o.cm)
            if (e.id == c.colormod && color_on(o, kColorModCarrier) == kColorModCarrier) { r.color = kColorModCarrier; r.colormod = e.id; }
    return r;
}

// The colour key: the next (dir +1) or previous colour of the chosen outfit, wrapping: its game colours, then the
// Color Mod's entries (each one rides on colour 1, the carrier, so only when the outfit has colour 1). Default stays.
inline Choice cycle_color(const Fighter& f, const Choice& c, int dir) {
    const int i = outfit_index(f, c);
    if (i < 0) return c;
    const Outfit& o = f.outfits[static_cast<size_t>(i)];
    if (o.colors.empty()) return c;
    const bool carrier = color_on(o, kColorModCarrier) == kColorModCarrier;
    const int ng = static_cast<int>(o.colors.size()), nm = carrier ? static_cast<int>(o.cm.size()) : 0, n = ng + nm;
    int at = -1;
    if (!c.colormod.empty()) { for (int k = 0; k < nm; ++k) if (o.cm[static_cast<size_t>(k)].id == c.colormod) at = ng + k; }
    else for (int k = 0; k < ng; ++k) if (o.colors[static_cast<size_t>(k)] == c.color) at = k;
    Choice r = c;
    r.colormod.clear();
    const int next = at < 0 ? 0 : ((at + (dir >= 0 ? 1 : -1)) % n + n) % n;
    if (next < ng) r.color = o.colors[static_cast<size_t>(next)];
    else { r.color = kColorModCarrier; r.colormod = o.cm[static_cast<size_t>(next - ng)].id; }
    return r;
}

// The colour's name as the game's Opponent Outfit Settings grid shows it: colours 0-9 "01".."10", 100.. "EX 1"..,
// 200.. "DX 1".. (the costume colour table holds 0-9, 100-105 and 200-207 only; Luke's Outfit 1: 100, 101 = EX 1, EX 2,
// 200-207 = DX 1-8 in the game's grid, read 05/11. The grid's RW / MC entries are not colour numbers of that table.)
inline std::string color_label(int color) {
    char b[32];
    if (color >= 0 && color < 10) std::snprintf(b, sizeof b, "%02d", color + 1);
    else if (color >= 100 && color < 200) std::snprintf(b, sizeof b, "EX %d", color - 99);
    else if (color >= 200 && color < 300) std::snprintf(b, sizeof b, "DX %d", color - 199);
    else std::snprintf(b, sizeof b, "#%d", color);
    return b;
}

// A choice's colour as the page shows it: the game's name ("01", "EX 1", "DX 1"), or a Color Mod entry's own label
// ("RW 3", "MC 1"); an entry the Color Mod no longer lists (or no Color Mod): what is worn then, the game's colour 1
inline std::string choice_color_label(const Outfit& o, const Choice& c) {
    if (!c.colormod.empty())
        for (const ColorEntry& e : o.cm) if (e.id == c.colormod) return e.label;
    return color_label(c.color);
}

// What a row shows on its right: "Default", or "<outfit name> - Color N"
inline std::string row_text(const Fighter& f, const Choice& c) {
    const int i = outfit_index(f, c);
    if (i < 0) return "Default";
    const Outfit& o = f.outfits[static_cast<size_t>(i)];
    return o.name + " - Color " + choice_color_label(o, c);
}

// ---- the file ------------------------------------------------------------------------------------------------------------
inline std::string to_json(const Choices& cs, const StageChoice& stage = {}) {
    std::string o = "{\n    \"version\": 1,\n    \"fighters\": {";
    bool first = true;
    for (const auto& [fid, c] : cs) {
        if (!c.forced) continue;
        o += first ? "\n" : ",\n";
        first = false;
        o += "        \"" + std::to_string(fid) + "\": {\"mode\": \"forced\", \"costume\": " + std::to_string(c.costume);
        if (!c.mod_id.empty()) o += ", \"mod_id\": \"" + json_escape(c.mod_id) + "\"";
        o += ", \"color\": " + std::to_string(c.color);
        if (!c.colormod.empty()) o += ", \"colormod\": \"" + json_escape(c.colormod) + "\"";
        o += "}";
    }
    o += first ? "}" : "\n    }";
    if (stage.forced) o += ",\n    \"stage\": {\"mode\": \"forced\", \"id\": " + std::to_string(stage.id) + "}";
    o += "\n}\n";
    return o;
}

// false: not a file of this format (out left empty). Entries that are not "forced" or lack a costume are skipped.
// `stage` (when given): the forced stage, Default when absent or not "forced".
inline bool from_json(const std::string& text, Choices& out, StageChoice* stage = nullptr) {
    out.clear();
    if (stage) *stage = StageChoice{};
    Json j;
    if (!json_parse(text, j) || j.kind != Json::Object) return false;
    const Json* v = j.get("version");
    if (!v || v->as_int(-1) != 1) return false;
    const Json* fs = j.get("fighters");
    if (!fs || fs->kind != Json::Object) return false;
    if (const Json* st = j.get("stage"); stage && st && st->kind == Json::Object) {
        const Json* mode = st->get("mode");
        const Json* id = st->get("id");
        if (mode && mode->as_str() == "forced" && id && id->kind == Json::Number && id->as_int(-1) >= 0) {
            stage->forced = true;
            stage->id = static_cast<int>(id->as_int());
        }
    }
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
        if (const Json* cm = e.get("colormod"); cm && !cm->as_str().empty()) { c.colormod = cm->as_str(); c.color = kColorModCarrier; }
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

// Fighter order: the game's Opponent Outfit Settings grid, 4 columns (read in game on 05/11: Luke, Jamie, Manon, Kimberly,
// Marisa, Lily, JP, Juri, ...); ids not listed follow, ascending
inline const std::vector<int>& grid_order() {
    static const std::vector<int> v = { 2, 21, 5, 3, 17, 12, 7, 16, 11, 9, 1, 20, 15, 18, 10, 4,
                                        6, 8, 14, 13, 19, 22, 26, 27, 28, 29, 25, 30, 31, 32, 33, 34 };
    return v;
}
// The portrait the game's grid shows (TextureResourceHolder path; the older fighters' folder is spelt "caracterselect")
inline std::string face_path(int fid) {
    char b[160];
    std::snprintf(b, sizeof b, "Product/GUI/data/Character_Image/%03d/%s/tex_Esf%03d_face_L_s_IAM.tex", fid,
                  fid >= 30 ? "characterselect" : "caracterselect", fid);
    return b;
}
// A stage's picture, as the game's Stage Settings shows it (thumbnail and banner are the same texture): the stage
// table's ManageId / 100 on four digits (40000 -> ess0400, 400000 -> ess4000, 0 -> ess0000, the Training Room)
inline std::string stage_image_path(int id) {
    char b[160];
    std::snprintf(b, sizeof b, "Product/GUI/data/Area_Image/ess/ess%04d_00/tex_StageImage_ess%04d_00_IM.tex", id / 100, id / 100);
    return b;
}
// The picture of the Default cell: the game's own "no stage in particular" globe (its Random cell's)
constexpr const char* kStageDefaultImage = "Product/GUI/data/Area_Image/random/tex_random_IM.tex";

// The grid's caption: the name in capitals ("C. VIPER")
inline std::string grid_caption(const std::string& name) {
    std::string r = name;
    for (auto& ch : r) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return r;
}
// Grid moves (4 columns): wrap inside the row / column like the game's grid
inline int grid_move(int cursor, int dx, int dy, int count, int cols) {
    if (count <= 0) return 0;
    const int rows = (count + cols - 1) / cols;
    int c = cursor % cols, r = cursor / cols;
    if (dx) {
        const int in_row = std::min(cols, count - r * cols);
        c = ((c + dx) % in_row + in_row) % in_row;
    }
    if (dy) {
        r = ((r + dy) % rows + rows) % rows;
        while (r * cols + c >= count) r = dy > 0 ? 0 : r - 1;   // the last row may be short
    }
    return r * cols + c;
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
