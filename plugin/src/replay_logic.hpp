// SF6 Slots — "Replay Outfits" (the Replays menu's 6th tab): pure logic, no game access (plugin/tests).
// The game side is replay_tab.cpp.
//
// The data file, read by the replay playback override (another module of this plugin):
//   reframework/data/SF6_CostumeSlots_data/replay_outfits.json
//   {"version":1,"fighters":{"<fighter id>":{"mode":"forced","costume":<FighterDesc.Costume>,
//                                            "mod_id":"<registry mod_id, extra outfits only>","color":<FighterDesc.Color>,
//                                            "colormod":"<a Color Mod entry's id: its file path; colour 1 then>",
//                                            "color_random":true (the Color row's Random: a draw at each playback; colour 0 then)}},
//    "stage":{"mode":"forced","id":<StageDesc.StageId: a vanilla stage>,"entry":"<a stage slot's key: tile / variation>"},
//    "bgm":{"mode":"character"}}
// No entry = Default (the replay's own outfit); no "stage" = the replay's own stage. A fighter's costume options:
// replay_options.hpp ("options" under its entry; "mode":"off" keeps them while it is not forced). The music: replay_bgm.hpp
// (no "bgm" = Random, what a replay does by itself).
#pragma once

#include "json.hpp"
#include "replay_bgm.hpp"
#include "replay_options.hpp"
#include "words.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
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
    bool random = false;              // the character's Random outfit (registry "random": true): the last one, its one colour
                                      // reads Random; a replay set to it wears a draw (costumes::replay_random), its preview Outfit 1
};

struct Fighter {
    int fid = 0;
    std::string name;                 // as the game writes it in a line ("Ryu", its language)
    std::string caption;              // as the game's grids and headers do ("RYU"); empty: grid_caption(name)
    std::vector<Outfit> outfits;      // game outfits (by costume number), then extra outfits (by name)
};

struct Choice {
    bool forced = false;              // false = Default (no entry in the file)
    int costume = 0;
    std::string mod_id;
    int color = 0;
    std::string colormod;             // a Color Mod entry's id (color is then kColorModCarrier); "" = the game colour
    // The Color row's last value, Random: each playback draws one of the outfit's colours (costumes::replay_random_colour);
    // color is then 0 (the preview's 01), colormod "". File: "color_random": true (absent in older files: false)
    bool random_color = false;
    bool operator==(const Choice& o) const {
        return forced == o.forced && (!forced || (costume == o.costume && mod_id == o.mod_id && color == o.color &&
                                                  colormod == o.colormod && random_color == o.random_color));
    }
};

using Choices = std::map<int, Choice>;   // fighter id -> forced choice (Default fighters are absent)
using OptionStore = std::map<int, OutfitOptions>;   // fighter id -> its outfits' costume options, On or Off (replay_options.hpp)

struct StageChoice {
    bool forced = false;              // false = Default (the replay's recorded stage)
    int id = 0;                       // StageDesc.StageId (the game's stage table ManageId): always a vanilla stage
    std::string entry;                // a stage slot of that stage (stages.hpp key: a tile or a variation), "" its own look
    bool operator==(const StageChoice& o) const { return forced == o.forced && (!forced || (id == o.id && entry == o.entry)); }
};

// The outfit list of a fighter: game outfits first in costume order, then extra outfits by name (case-insensitive),
// ties by costume number, the Random outfit last (as in the game's Costume row). Outfits without any colour are dropped.
inline void sort_outfits(std::vector<Outfit>& v) {
    auto lower = [](std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return s; };
    v.erase(std::remove_if(v.begin(), v.end(), [](const Outfit& o) { return o.colors.empty(); }), v.end());
    std::stable_sort(v.begin(), v.end(), [&](const Outfit& a, const Outfit& b) {
        if (a.random != b.random) return !a.random;
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
    if (c.forced && keep_color && c.random_color && !o.random) { r.color = 0; r.colormod.clear(); r.random_color = true; }
    return r;
}

// The colour key: the next (dir +1) or previous colour of the chosen outfit, wrapping: its game colours, then the
// Color Mod's entries (each one rides on colour 1, the carrier, so only when the outfit has colour 1), then Random (a draw
// at each playback; not on the Random outfit, whose one colour is Random already). Default stays.
inline Choice cycle_color(const Fighter& f, const Choice& c, int dir) {
    const int i = outfit_index(f, c);
    if (i < 0) return c;
    const Outfit& o = f.outfits[static_cast<size_t>(i)];
    if (o.colors.empty()) return c;
    const bool carrier = color_on(o, kColorModCarrier) == kColorModCarrier;
    const int ng = static_cast<int>(o.colors.size()), nm = carrier ? static_cast<int>(o.cm.size()) : 0;
    const int nr = o.random ? 0 : 1, n = ng + nm + nr;
    int at = -1;
    if (c.random_color && nr) at = ng + nm;
    else if (!c.colormod.empty()) { for (int k = 0; k < nm; ++k) if (o.cm[static_cast<size_t>(k)].id == c.colormod) at = ng + k; }
    else for (int k = 0; k < ng; ++k) if (o.colors[static_cast<size_t>(k)] == c.color) at = k;
    Choice r = c;
    r.colormod.clear();
    r.random_color = false;
    const int next = at < 0 ? 0 : ((at + (dir >= 0 ? 1 : -1)) % n + n) % n;
    if (next < ng) r.color = o.colors[static_cast<size_t>(next)];
    else if (next < ng + nm) { r.color = kColorModCarrier; r.colormod = o.cm[static_cast<size_t>(next - ng)].id; }
    else { r.color = 0; r.random_color = true; }
    return r;
}

// The colour's name as the game's Opponent Outfit Settings grid shows it: colours 0-9 "01".."10", 100.. "EX 1"..,
// 200.. "DX 1".. (the costume colour table holds 0-9, 100-105 and 200-207 only; Luke's Outfit 1: 100, 101 = EX 1, EX 2,
// 200-207 = DX 1-8 in the game's grid, read 05/11. The grid's RW / MC entries are not colour numbers of that table.)
inline std::string color_label(int color) {
    char b[32];
    if (color >= 0 && color < 10) std::snprintf(b, sizeof b, "%02d", color + 1);
    else if (color >= 100 && color < 200) return words::game_arg(words::G::ColorEx, std::to_string(color - 99));
    else if (color >= 200 && color < 300) return words::game_arg(words::G::ColorDx, std::to_string(color - 199));
    else std::snprintf(b, sizeof b, "#%d", color);
    return b;
}

// A choice's colour as the page shows it: the game's name ("01", "EX 1", "DX 1"), or a Color Mod entry's own label
// ("RW 3", "MC 1"); an entry the Color Mod no longer lists (or no Color Mod): what is worn then, the game's colour 1. The
// Random outfit's one colour: Random (the game's word), as its Color row reads in the game.
inline std::string choice_color_label(const Outfit& o, const Choice& c) {
    if (o.random || c.random_color) return words::game(words::G::Random);
    if (!c.colormod.empty())
        for (const ColorEntry& e : o.cm) if (e.id == c.colormod) return e.label;
    return color_label(c.color);
}

// A choice in one line, for the log: "Default", or "<outfit name> - Color N"
inline std::string row_text(const Fighter& f, const Choice& c) {
    const int i = outfit_index(f, c);
    if (i < 0) return "Default";
    const Outfit& o = f.outfits[static_cast<size_t>(i)];
    return o.name + " - Color " + choice_color_label(o, c);
}

// ---- the summary on the right (as the game's: Battle Settings > Other's "Game Settings", the Opponent Outfit page's) ------
// A character set to On: "Luke: Outfit 4, 01" (its outfit, its colour as the page shows it); an outfit no longer listed:
// "Luke: Default", as its row
// In two parts, from the language's own pattern (fmt.fighter_outfit: "%s : %s, %s" in French, "%s\uFF1A%s\u3001%s" in Japanese): what stays when the
// line doesn't fit (the character's name, the game's, and what follows it up to the second value) and what moves (the rest)
struct LineParts {
    std::string fixed, moving;
    std::string whole() const { return fixed + moving; }
};
inline LineParts line_parts(const char* tmpl, std::initializer_list<std::string> args) {
    LineParts out;
    auto a = args.begin();
    int seen = 0;
    for (const char* p = tmpl; *p; ++p) {
        if (*p == '%' && (p[1] == 's' || p[1] == 'd')) {
            std::string& to = seen == 0 ? out.fixed : out.moving;
            if (a != args.end()) to += *a++;
            ++seen;
            ++p;
        } else {
            std::string& to = seen >= 2 ? out.moving : out.fixed;
            if (*p == '%' && p[1] == '%') { to += '%'; ++p; }
            else to += *p;
        }
    }
    return out;
}
inline LineParts summary_line_parts(const Fighter& f, const Choice& c) {
    const int i = outfit_index(f, c);
    if (i < 0) return line_parts(words::tr(words::W::FmtNameValue), { f.name, words::game(words::G::Default) });
    const Outfit& o = f.outfits[static_cast<size_t>(i)];
    return line_parts(words::tr(words::W::FmtFighterOutfit), { f.name, o.name, choice_color_label(o, c) });
}
inline std::string summary_line(const Fighter& f, const Choice& c) { return summary_line_parts(f, c).whole(); }
// The lines under "Replay Outfit Settings": one per character set to On (the catalog's order, the grid's), at most `max`
// (the last one then says how many more); "Default" when none is On
// (a line with no fixed part: "Default", "+ 6 more")
inline std::vector<LineParts> summary_parts(const std::vector<Fighter>& cat, const Choices& cs, size_t max) {
    std::vector<LineParts> on;
    for (const Fighter& f : cat) {
        auto it = cs.find(f.fid);
        if (it != cs.end() && it->second.forced) on.push_back(summary_line_parts(f, it->second));
    }
    if (on.empty()) return { { std::string(), words::game(words::G::Default) } };
    if (max > 0 && on.size() > max) {
        const size_t more = on.size() - (max - 1);
        on.resize(max - 1);
        on.push_back({ std::string(), words::trf(words::W::ReplayMore, { std::to_string(more) }) });
    }
    return on;
}
inline std::vector<std::string> summary_lines(const std::vector<Fighter>& cat, const Choices& cs, size_t max) {
    std::vector<std::string> out;
    for (const LineParts& p : summary_parts(cat, cs, max)) out.push_back(p.whole());
    return out;
}
// The panel's items, top to bottom, and their place in the game's units (the panel is drawn at 0.81): a name's or a line's
// middle, a picture's top. Read 05/11 on Battle Settings > Other's "Game Settings" (MatchingSetting
// p_BattleSettingProfileGameSetting_: Stage Settings at 63, its picture (248 x 62) at 82, Battle BGM at 184 and its value at
// 214, Commentary Settings at 261 and its lines from 291 every 31, Side Settings at 493...) and on the Opponent Outfit
// page's c_CostumeSetting (the same 63, 93, every 31). A line under a picture (ours: the stage's name): 20 under it.
enum class SumKind { Name, Line, Picture };
constexpr float kSumFirstY = 63, kSumLineGap = 30, kSumLineStep = 31, kSumBlockGap = 47;
constexpr float kSumPicGap = 19, kSumPicH = 62, kSumAfterPic = 40, kSumLineAfterPic = 20;
inline std::vector<float> summary_layout(const std::vector<SumKind>& kinds) {
    std::vector<float> y;
    float at = 0;
    for (size_t i = 0; i < kinds.size(); ++i) {
        const SumKind k = kinds[i];
        if (i == 0) at = kSumFirstY;
        else {
            const SumKind p = kinds[i - 1];
            const float below = p == SumKind::Picture ? at + kSumPicH : at;   // under a picture: from its bottom
            if (k == SumKind::Name) at = p == SumKind::Picture ? below + kSumAfterPic : below + kSumBlockGap;
            else if (k == SumKind::Line) at = p == SumKind::Name ? below + kSumLineGap : p == SumKind::Picture ? below + kSumLineAfterPic
                                                                                            : below + kSumLineStep;
            else at = below + kSumPicGap;
        }
        y.push_back(at);
    }
    return y;
}

// ---- the file ------------------------------------------------------------------------------------------------------------
inline std::string to_json(const Choices& cs, const StageChoice& stage = {}, const OptionStore& options = {}, const BgmChoice& bgm = {}) {
    std::string o = "{\n    \"version\": 1,\n    \"fighters\": {";
    bool first = true;
    std::set<int> fids;
    for (const auto& [fid, c] : cs) if (c.forced) fids.insert(fid);
    for (const auto& [fid, opts] : options) if (!opts.empty()) fids.insert(fid);
    for (int fid : fids) {
        o += first ? "\n" : ",\n";
        first = false;
        o += "        \"" + std::to_string(fid) + "\": {";
        auto ci = cs.find(fid);
        if (ci != cs.end() && ci->second.forced) {
            const Choice& c = ci->second;
            o += "\"mode\": \"forced\", \"costume\": " + std::to_string(c.costume);
            if (!c.mod_id.empty()) o += ", \"mod_id\": \"" + json_escape(c.mod_id) + "\"";
            o += ", \"color\": " + std::to_string(c.color);
            if (!c.colormod.empty()) o += ", \"colormod\": \"" + json_escape(c.colormod) + "\"";
            if (c.random_color) o += ", \"color_random\": true";
        } else {
            o += "\"mode\": \"off\"";                  // Off: only its options, kept for when it is On again
        }
        auto oi = options.find(fid);
        if (oi != options.end() && !oi->second.empty()) o += ", " + outfit_options_json(oi->second);
        o += "}";
    }
    o += first ? "}" : "\n    }";
    if (stage.forced) {
        o += ",\n    \"stage\": {\"mode\": \"forced\", \"id\": " + std::to_string(stage.id);
        if (!stage.entry.empty()) o += ", \"entry\": \"" + json_escape(stage.entry) + "\"";
        o += "}";
    }
    if (const std::string b = bgm_json(bgm); !b.empty()) o += ",\n    " + b;
    o += "\n}\n";
    return o;
}

// false: not a file of this format (out left empty). Entries that are not "forced" or lack a costume are skipped.
// `stage` (when given): the forced stage, Default when absent or not "forced". `options` (when given): every fighter's
// costume options, whatever its mode. `bgm` (when given): the Battle BGM row, Random when absent.
inline bool from_json(const std::string& text, Choices& out, StageChoice* stage = nullptr, OptionStore* options = nullptr, BgmChoice* bgm = nullptr) {
    out.clear();
    if (stage) *stage = StageChoice{};
    if (options) options->clear();
    if (bgm) *bgm = BgmChoice{};
    Json j;
    if (!json_parse(text, j) || j.kind != Json::Object) return false;
    const Json* v = j.get("version");
    if (!v || v->as_int(-1) != 1) return false;
    const Json* fs = j.get("fighters");
    if (!fs || fs->kind != Json::Object) return false;
    if (bgm) *bgm = parse_bgm(j);
    if (const Json* st = j.get("stage"); stage && st && st->kind == Json::Object) {
        const Json* mode = st->get("mode");
        const Json* id = st->get("id");
        if (mode && mode->as_str() == "forced" && id && id->kind == Json::Number && id->as_int(-1) >= 0) {
            stage->forced = true;
            stage->id = static_cast<int>(id->as_int());
            if (const Json* en = st->get("entry"); en && en->kind == Json::String) stage->entry = en->as_str();
        }
    }
    for (const auto& [key, e] : fs->obj) {
        if (e.kind != Json::Object) continue;
        char* end = nullptr;
        const long fid = std::strtol(key.c_str(), &end, 10);
        if (!end || *end || fid <= 0) continue;
        if (options) {
            OutfitOptions oo;
            parse_outfit_options(e, oo);
            if (!oo.empty()) (*options)[static_cast<int>(fid)] = std::move(oo);
        }
        const Json* mode = e.get("mode");
        const Json* cos = e.get("costume");
        if (!mode || mode->as_str() != "forced" || !cos || cos->kind != Json::Number) continue;
        Choice c;
        c.forced = true;
        c.costume = static_cast<int>(cos->as_int());
        if (const Json* m = e.get("mod_id")) c.mod_id = m->as_str();
        if (const Json* col = e.get("color")) c.color = static_cast<int>(col->as_int(0));
        if (const Json* cm = e.get("colormod"); cm && !cm->as_str().empty()) { c.colormod = cm->as_str(); c.color = kColorModCarrier; }
        if (const Json* rc = e.get("color_random"); rc && rc->kind == Json::Bool && rc->b) { c.random_color = true; c.color = 0; c.colormod.clear(); }
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
// The game's outfit names by costume number (what Battle Settings shows)
inline std::string game_outfit_name(int costume) {
    return words::outfit_name(costume);
}

}  // namespace slots::replay
