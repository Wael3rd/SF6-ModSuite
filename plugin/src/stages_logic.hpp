// SF6 Slots — stage menus: pure logic, no game access (plugin/tests/stages_logic_test.cpp). The game side is stages.cpp.
//
// - state.json: what the loader serves ("selected", read by stages/loader/stage_redirect.cpp, must stay first and flat) and
//   the choice of each menu ("menus": classic stage select, Battle Settings, Training, replays).
// - The stage grids (app.UIPartsStageSelectSimple): where the tiles go in the game's list and which entry each grid index is.
// - The cycle of variations on a vanilla entry, its label, and which pair of keys a menu leaves free.
#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
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

struct State {
    Choice active;                  // what the loader serves now ("selected")
    Choice menu[kMenus];            // each menu's own choice
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

// "<L> Training Room · Red <R>" (the variation's name), "<L> Training Room <R>" (vanilla look: the keys say it cycles).
// No key symbols (none free): the names alone.
inline std::string cycle_label(const std::string& left, const std::string& stage, const std::string& variation,
                               const std::string& right) {
    std::string mid = variation.empty() ? stage : stage + " \xC2\xB7 " + variation;   // U+00B7 middle dot
    if (left.empty() || right.empty()) return mid;
    return left + " " + mid + " " + right;
}

// ---- keys ----------------------------------------------------------------------------------------------------------------
// The pair of keys a menu cycles variations with: LT / RT (UIChildTabL / R) unless the screen uses them (its input
// delegates handle EventChildTabBack / Next) or turns them off (DisableSystemInputFlags ChildTab); then LB / RB
// (UITabL / R) on the same conditions; else none (the menu still has its tiles).
enum Keys : int { kKeysChildTab = 0, kKeysTab = 1, kKeysNone = 2 };
inline Keys pick_keys(bool childtab_taken, bool childtab_off, bool tab_taken, bool tab_off) {
    if (!childtab_taken && !childtab_off) return kKeysChildTab;
    if (!tab_taken && !tab_off) return kKeysTab;
    return kKeysNone;
}
inline const char* keys_name(int k) { return k == kKeysChildTab ? "LT / RT (child tab)" : k == kKeysTab ? "LB / RB (tab)" : "none"; }
// app.InputAssign.Digital.Id of each pair (the symbols of the label)
inline int key_left_id(int k)  { return k == kKeysChildTab ? 75 : k == kKeysTab ? 73 : -1; }
inline int key_right_id(int k) { return k == kKeysChildTab ? 76 : k == kKeysTab ? 74 : -1; }

}  // namespace slots::stagelogic
