// SF6 Slots — stage menus: pure logic, no game access (plugin/tests/stages_logic_test.cpp). The game side is stages.cpp.
//
// - state.json: what the loader serves ("selected", read by stages/loader/stage_redirect.cpp, must stay first and flat) and
//   the choice of each menu ("menus": classic stage select, Battle Settings, Training, replays).
// - The stage grids (app.UIPartsStageSelectSimple): where the tiles go in the game's list and which entry each grid index is.
// - The cycle of variations on a vanilla entry, its one-line label, the variation key and its guide bar hint, the chosen mark.
#pragma once

#include "json.hpp"

#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace slots::stagelogic {

// ---- menus ---------------------------------------------------------------------------------------------------------------
enum Menu : int { kClassic = 0, kBattle = 1, kTraining = 2, kReplay = 3, kMenus = 4 };
inline const char* menu_key(int m) {
    static const char* k[kMenus] = { "classic", "battle", "training", "replay" };
    return m >= 0 && m < kMenus ? k[m] : "?";
}

using Choice = std::map<uint32_t, std::string>;   // vanilla stage id -> variant key (the vanilla look is not stored)

// Random Stage Settings, per menu: the entries left out of the random pool ("" a stage's own entry, else a tile's key), per
// vanilla stage with tiles (each tile's tick is its own: Wael 05/11), and whether the menu's stage is Random (the game's
// random then lands on a vanilla id; the plugin picks one of that stage's ticked entries for this PC).
using Off = std::map<uint32_t, std::set<std::string>>;

struct State {
    Choice active;                  // what the loader serves now ("selected")
    Choice menu[kMenus];            // each menu's own choice
    Off off[kMenus];                // unticked entries in each menu's Random Stage Settings
    bool random[kMenus] = {};       // the menu's stage is Random (999999)
};

// "selected" first and flat (the loader's tolerant scan reads the first object after the word), then "menus".
inline std::string state_json(const State& s) {
    auto obj = [](const Choice& c, const char* indent) {
        std::string o = "{";
        bool first = true;
        for (auto& [id, key] : c) {
            if (key.empty()) continue;
            o += first ? "\n" : ",\n";
            o += std::string(indent) + "    \"" + std::to_string(id) + "\": \"" + json_escape(key) + "\"";
            first = false;
        }
        o += first ? "}" : "\n" + std::string(indent) + "}";
        return o;
    };
    std::string o = "{\n    \"selected\": " + obj(s.active, "    ") + ",\n    \"menus\": {";
    for (int m = 0; m < kMenus; ++m) o += std::string(m ? ",\n" : "\n") + "        \"" + menu_key(m) + "\": " + obj(s.menu[m], "        ");
    o += "\n    },\n    \"random\": {";
    for (int m = 0; m < kMenus; ++m) {
        o += std::string(m ? ",\n" : "\n") + "        \"" + menu_key(m) + "\": {\"on\": " + (s.random[m] ? "true" : "false") + ", \"off\": {";
        bool first = true;
        for (auto& [id, keys] : s.off[m]) {
            if (keys.empty()) continue;
            o += std::string(first ? "" : ", ") + "\"" + std::to_string(id) + "\": [";
            bool k1 = true;
            for (auto& k : keys) { o += std::string(k1 ? "" : ", ") + "\"" + json_escape(k) + "\""; k1 = false; }
            o += "]";
            first = false;
        }
        o += "}}";
    }
    o += "\n    }\n}\n";
    return o;
}

// False when the text is not a JSON object. A file of an older plugin (only "selected", served everywhere): every menu
// starts from it, so nothing changes until a menu's choice does.
inline bool parse_state(const std::string& text, State& out) {
    out = State{};
    Json j;
    if (!json_parse(text, j) || j.kind != Json::Object) return false;
    auto read = [](const Json* o, Choice& c) {
        if (!o || o->kind != Json::Object) return false;
        for (auto& [k, v] : o->obj) {
            if (v.kind != Json::String || v.s.empty() || k.empty() || k.find_first_not_of("0123456789") != std::string::npos) continue;
            c[static_cast<uint32_t>(std::stoul(k))] = v.s;
        }
        return true;
    };
    read(j.get("selected"), out.active);
    const Json* menus = j.get("menus");
    if (menus && menus->kind == Json::Object) {
        for (int m = 0; m < kMenus; ++m) read(menus->get(menu_key(m)), out.menu[m]);
    } else {
        for (int m = 0; m < kMenus; ++m) out.menu[m] = out.active;   // every menu starts from the look it had
    }
    if (const Json* r = j.get("random"); r && r->kind == Json::Object)
        for (int m = 0; m < kMenus; ++m) {
            const Json* mm = r->get(menu_key(m));
            if (!mm || mm->kind != Json::Object) continue;
            if (const Json* on = mm->get("on"); on && on->kind == Json::Bool) out.random[m] = on->b;
            const Json* off = mm->get("off");
            if (!off || off->kind != Json::Object) continue;
            for (auto& [k, v] : off->obj) {
                if (k.empty() || k.find_first_not_of("0123456789") != std::string::npos || v.kind != Json::Array) continue;
                auto& set = out.off[m][static_cast<uint32_t>(std::stoul(k))];
                for (auto& e : v.arr) if (e.kind == Json::String) set.insert(e.s);
            }
        }
    return true;
}

// ---- the grid: tiles after the game's stages ------------------------------------------------------------------------------
constexpr uint32_t kBattleHubStage = 500000, kRandomStage = 999999;

struct TileRef { uint32_t base = 0; std::string key; };
struct Slot {
    uint32_t base = 0;             // the vanilla stage id the game holds at this index
    std::string key;               // "" = the game's own entry (its variations cycle on it), else the tile's variant key
    bool tile = false;
};

// Where the tiles go: before the Battle Hub / Random entries the game puts last, else at the end.
inline size_t tile_insert_pos(const std::vector<uint32_t>& list) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == kBattleHubStage || list[i] == kRandomStage) return i;
    return list.size();
}

// The grid's entries once the tiles are in: the game's list with the tiles (those whose stage the list holds, in their
// order) inserted at tile_insert_pos. inserted: the ids to insert there, in order.
inline std::vector<Slot> plan_slots(const std::vector<uint32_t>& list, const std::vector<TileRef>& tiles, size_t& pos,
                                    std::vector<uint32_t>& inserted) {
    pos = tile_insert_pos(list);
    inserted.clear();
    std::vector<Slot> out;
    for (size_t i = 0; i < pos; ++i) out.push_back({ list[i], "", false });
    for (auto& t : tiles) {
        bool held = false;
        for (uint32_t id : list) held = held || (id == t.base && id != kBattleHubStage && id != kRandomStage);
        if (!held || t.key.empty()) continue;
        out.push_back({ t.base, t.key, true });
        inserted.push_back(t.base);
    }
    for (size_t i = pos; i < list.size(); ++i) out.push_back({ list[i], "", false });
    return out;
}

// The grid index of the entry for (stage, chosen key): the tile's when the key is a tile of this grid, else the game's
// own entry of the stage (a variation or the vanilla look); -1 when the grid does not hold the stage.
inline int entry_index(const std::vector<Slot>& slots, uint32_t base, const std::string& key) {
    int own = -1;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].base != base) continue;
        if (slots[i].tile && !key.empty() && slots[i].key == key) return static_cast<int>(i);
        if (!slots[i].tile && own < 0) own = static_cast<int>(i);
    }
    return own;
}

// ---- variations ----------------------------------------------------------------------------------------------------------
// 0 = the vanilla look, i = variations[i - 1]; step -1 / +1, wrapping over count + 1 options.
inline int cycle(int current, int count, int step) {
    const int n = count + 1;
    if (n <= 1) return 0;
    return ((current + step) % n + n) % n;
}

inline std::string norm_name(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const char* blank = " \x09";
    const size_t a = s.find_first_not_of(blank), b = s.find_last_not_of(blank);
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// The one name line of the banner for a variation (Wael 05/11: never two names side by side): the variation's own name
// ("Dhalsimer Temple Banner", "Training Room Red"); a variation named like its stage (a lighting pack: "Dhalsimer
// Temple") shows its bundle ("Stage Lighting Overhaul"), as the classic Stage Select and the VS screen do; no variation:
// the stage's name.
inline std::string variation_label(const std::string& stage, const std::string& variation, const std::string& bundle) {
    if (variation.empty()) return stage;
    if (!bundle.empty() && norm_name(variation) == norm_name(stage)) return bundle;
    return variation;
}

// ---- the banner's name: never squeezed (Wael 05/11) ------------------------------------------------------------------------
// A name that fits stays where the game puts it (centred, 180 from the banner's right edge, its region 361 wide). A longer one
// is right-aligned 10 from that edge and runs left over the picture at full size; only one wider than the banner is cut with
// "...". Decided from the text alone, before it is written, so the first frame is already right (a drawn width is only known
// a frame later). Its width: the advances of the game's name font (slot 6, 30), within 2 % on the names measured: "The Macho
// Ring" 187 (read, DisplaySize), "Training Room" 169, "SIRN Training Room" 233, "Suzaku Castle Over Training Room" 412
// (screenshots). The fit limit stays under 361 by that error: such a name right-aligned moves by a few units only.
constexpr float kNameFitW = 355, kNameMaxW = 756;  // the banner's 776 less 10 on each side
constexpr float kNameRightFromCentre = 170;        // the right-aligned edge (1526) from the centred place (1356)
inline float name_advance(unsigned char c) {
    if (c == ' ') return 7.5f;
    if (c == 'M' || c == 'W' || c == 'm' || c == 'w') return 20;
    if (c == 'i' || c == 'j' || c == 'l' || c == 'I' || c == '.' || c == ',' || c == '\'' || c == ':' || c == ';' || c == '!' || c == '|') return 7;
    if (c == '(' || c == ')' || c == '-' || c == '/' || c == '[' || c == ']') return 9;
    if (c == 'f' || c == 't' || c == 'r') return 10;
    if (c >= '0' && c <= '9') return 14;
    if (c >= 'A' && c <= 'Z') return 15.5f;
    if (c < 0x80) return 14.5f;
    return 0;
}
inline float name_width(const std::string& s) {
    float w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) w += name_advance(c);
        else if ((c & 0xC0) != 0x80) w += c >= 0xE0 ? 30.0f : 15.5f;   // a character's first byte: CJK and the like are square
    }
    return w;
}
struct BannerName { std::string text; bool right = false; };
inline BannerName banner_name(const std::string& name) {
    const float w = name_width(name);
    if (w <= kNameFitW) return { name, false };
    if (w <= kNameMaxW) return { name, true };
    const float room = kNameMaxW - name_width("...");
    size_t end = 0;
    for (size_t i = 0; i < name.size();) {             // whole characters only
        size_t n = 1;
        while (i + n < name.size() && (static_cast<unsigned char>(name[i + n]) & 0xC0) == 0x80) ++n;
        if (name_width(name.substr(0, i + n)) > room) break;
        i += n;
        end = i;
    }
    return { name.substr(0, end) + "...", true };
}

// ---- the variation key ---------------------------------------------------------------------------------------------------
// One press of the pad's X button shows the focused stage's next variation (Wael 05/11; LT / RT are gone). The game's
// input id of that button in Battle Settings: app.InputAssign.Digital.Id UIOpponentCosSettingToggleEXDXColors (341,
// Opponent Outfit Settings' "EX/DX Colors On/Off"): its symbol is the bar's icon, its keyboard binding the key read.
constexpr int kVariationKeyId = 341;
// The guide bar's "<key> Stage Variation": only while the stage grid has the focus, on a stage's own entry (not a tile)
// that has variations, and with a key symbol to show.
inline bool variation_hint(bool grid_focused, bool focused_is_tile, int variations, bool have_symbol) {
    return grid_focused && !focused_is_tile && variations > 0 && have_symbol;
}
inline std::string variation_hint_text(const std::string& symbol) { return symbol + " Stage Variation"; }

// The keyboard key of a game symbol: the game names keys by their QWERTY place ("<ICON KeyR>"; menus read scan codes:
// "W" moves up on an AZERTY keyboard too), so the letter's QWERTY scan code; 0 when the symbol names no letter or digit.
inline unsigned qwerty_scan(const std::string& symbol) {
    const std::string tag = "<ICON Key";
    const size_t at = symbol.find(tag);
    if (at == std::string::npos || at + tag.size() + 1 >= symbol.size() || symbol[at + tag.size() + 1] != '>') return 0;
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(symbol[at + tag.size()])));
    static const char* rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const unsigned base[] = { 0x10, 0x1E, 0x2C };
    for (int r = 0; r < 3; ++r)
        for (const char* k = rows[r]; *k; ++k)
            if (*k == c) return base[r] + static_cast<unsigned>(k - rows[r]);
    if (c >= '1' && c <= '9') return 0x02 + static_cast<unsigned>(c - '1');
    if (c == '0') return 0x0B;
    return 0;
}

// ---- the chosen mark -----------------------------------------------------------------------------------------------------
// The game marks one cell: SetSelectIndex(i) puts the corner mark on the cell whose ListIndex is i, i =
// ConvertStageIdToGridIndex(the decided id), which stages.cpp answers with the chosen entry (entry_index). Decide first
// compares that index with the focused one and does nothing when they match ("already chosen"): during Decide the index
// must be the one of the choice from BEFORE it, else confirming a tile over its own stage (or the stage after one of its
// tiles) was taken for "already chosen" and the mark moved only with the next refresh (Wael 05/11: "only after the
// cursor moves"). True when the game goes on with the Decide (marks the focused entry at once).
inline bool decide_marks_now(const std::vector<Slot>& slots, uint32_t base, const std::string& before, int focused) {
    return entry_index(slots, base, before) != focused;
}

// ---- the variation key previews, Confirm chooses (Wael 05/11) ---------------------------------------------------------------
// One source per frame for what a stage's own entry shows: the focused entry the variation the key moved it to (the
// preview), the chosen entry its chosen variation, any other its own look. The preview lives while the focus stays on its
// entry: moving off puts the entry back as it was (picture, banner, mark); Confirm makes it the choice.
struct Preview {
    int entry = -1;                // the entry the key moved off its resting look (-1: none)
    int var = 0;                   // the variation it shows (0 its own look, i: the i-th variation)
    bool operator==(const Preview& o) const { return entry == o.entry && var == o.var; }
};
inline int shown_variation(int entry, int chosen_entry, int chosen_var, const Preview& p) {
    if (p.entry >= 0 && entry == p.entry) return p.var;
    return entry == chosen_entry ? chosen_var : 0;
}
// The key on the focused entry: the variation after the one it shows now (wrapping over its own look)
inline Preview preview_next(int focused, int chosen_entry, int chosen_var, const Preview& p, int count) {
    return { focused, cycle(shown_variation(focused, chosen_entry, chosen_var, p), count, 1) };
}
// The focus is now on `focused`: a preview of another entry goes
inline Preview preview_follow(const Preview& p, int focused) { return p.entry == focused ? p : Preview{}; }

// ---- Random Stage Settings: each entry its own tick -----------------------------------------------------------------------
// The game ticks vanilla ids: a stage and its tiles share one tick. The plugin keeps the entries' ticks (`off`: the unticked
// keys of a stage, "" its own entry); the game's id is in its pool (not in its disable list) while one of them is ticked.
// The game's state wins when they disagree: its id out of the pool = none ticked; in the pool with every entry unticked
// (changed elsewhere) = all ticked.
inline bool is_ticked(const std::vector<std::string>& keys, const std::set<std::string>& off, bool game_in_pool, const std::string& key) {
    if (!game_in_pool) return false;
    bool any = false;
    for (auto& k : keys) any = any || !off.count(k);
    return !any || !off.count(key);
}
inline std::set<std::string> effective_off(const std::vector<std::string>& keys, const std::set<std::string>& off, bool game_in_pool) {
    std::set<std::string> out;
    for (auto& k : keys) if (!is_ticked(keys, off, game_in_pool, k)) out.insert(k);
    return out;
}
// Confirm on an entry in Random Stage Settings: its tick flips. game_toggles: the stage's id enters or leaves the game's
// pool, so the game's own Decide runs (with its own checks); otherwise the game's Decide is skipped (it would flip the
// shared id, so every entry of the stage).
struct TickStep { std::set<std::string> off; bool game_toggles = false; bool in_pool_after = false; };
inline TickStep toggle_tick(const std::vector<std::string>& keys, const std::set<std::string>& off, bool game_in_pool, const std::string& key) {
    TickStep t;
    t.off = effective_off(keys, off, game_in_pool);
    const bool any_before = t.off.size() < keys.size();
    if (t.off.count(key)) t.off.erase(key); else t.off.insert(key);
    t.in_pool_after = t.off.size() < keys.size();
    t.game_toggles = any_before != t.in_pool_after;
    return t;
}
// The game's random landed on this stage: the entry served, one of the ticked ones (r: a random draw); "" (its own entry)
// when none is
inline std::string pick_ticked(const std::vector<std::string>& keys, const std::set<std::string>& off, unsigned r) {
    std::vector<std::string> on;
    for (auto& k : keys) if (is_ticked(keys, off, true, k)) on.push_back(k);
    return on.empty() ? std::string() : on[r % on.size()];
}

}  // namespace slots::stagelogic
