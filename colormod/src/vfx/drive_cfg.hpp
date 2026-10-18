// vfx::drive_cfg — the Drive colour settings held by the DLL (phase 5c-1, docs/CPP_PORT_PHASE5.md section 8).
//   - the tables SF6_DIRecolor.lua / SF6_DriveRecolor.lua kept: one per character (loaded on first use through
//     vfx::settings, which migrates as get_cfg did), the presets; saved on demand (save_cfg / save_presets);
//   - the value model of the Drive rows and of the DI panel (func/DriveRows.lua, func/DIPanelMode.lua): a row shows
//     0 = Off, 1 = Follow, 2 + rank of the chosen preset; a step wraps around; the label carries one swatch per colour.
// Pure (no REFramework): the locale and the worn colours (decisions' squares) come through set_env, so the offline
// harness (tools/settings_harness, model part) runs it against the Lua it replaces.
// Every function takes the module's lock; a json* handed out stays valid while the caller holds lock().
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace vfx::drive_cfg {

using json = nlohmann::json;

enum class Mech { Impact = 0, Reversal, Parry, Rush, Special };
constexpr int kMechs = 5;
enum class Kind { Di, Hue };                       // Impact / Reversal: two colours; Parry / Rush / Special: a hue
struct MechDesc { Mech mech; const char* name; Kind kind; const char* tkey; };
const MechDesc& desc(Mech m);                      // "impact" / Kind::Di / "vfx.impact", ...

// root = the REFramework data folder; L = the locale (key -> text); squares = the two chip colours a side wears
// (engine::decide::squares), nullopt when unknown.
struct Env {
    std::string root = "reframework/data";
    std::function<std::string(const std::string&)> L;
    std::function<std::optional<std::pair<uint32_t, uint32_t>>(int fighter, int side)> squares;
    // save_di's writer: (path, bytes) written off the game thread, in call order (the last save wins); unset = written
    // at once (offline harness)
    std::function<void(const std::string& path, std::string bytes)> write_async;
};
void set_env(Env env);
void preload();                                                         // every fighter's files, once at start
std::optional<std::pair<uint32_t, uint32_t>> squares(int fighter, int side);   // env.squares (nullopt without one)

std::recursive_mutex& lock();

// ---- the tables ----------------------------------------------------------------------------------------------------
// The side's table of a character file (get_cfg(id, side)): nullptr when the file could not be loaded.
json* di_side(int fighter, int side);              // SF6_DIRecolor: { enabled, follow, c1, c2, selected, sel, rev, ... }
json* dr_side(int fighter, int side);              // SF6_DriveRecolor: { parry, rush, special }
json* di_file(int fighter);                        // the whole { p1, p2 } (parts_list lives there)
json& presets();                                   // { presets, presets_dr, presets_sp } (empty object when unloadable)
// cfg_of(desc, fighter, side): the table of one mechanic. Reversal = mech_cfg(base, "reversal"): the "rev" sub-table,
// created from the Impact setting the first time.
json* cfg_of(Mech m, int fighter, int side);
void save_di(int fighter);                         // SF6_DIRecolor save_cfg (nothing when not loaded)
void save_dr(int fighter);                         // SF6_DriveRecolor save_cfg
void save_presets();
void forget();                                     // drop every table (the next use reloads the files)

// ---- the value model -----------------------------------------------------------------------------------------------
struct Item { json name, c1, c2; std::string key; int index = 0; };
std::vector<Item> items(Mech m);                   // preset_items of the mechanic's family (DI, DR, SP)
int value_of(const json* cfg, const std::vector<Item>& items);
void apply_value(json& cfg, const std::vector<Item>& items, int v);
int stepped(const json* cfg, const std::vector<Item>& items, int dir);   // (value_of + dir) % (2 + #items), Lua's %

// ---- texts ---------------------------------------------------------------------------------------------------------
std::string camel(const std::string& s);           // "DRIVE IMPACT" -> "Drive Impact" (ASCII letters, as Lua's %a)
std::string title(Mech m);                         // camel(L(tkey))
std::string rgb_hex(int64_t col);                  // ABGR int -> "RRGGBB"
std::string swatch(const json& col);               // <color RRGGBB>■</color> (nil = black)
// DIPanelMode label_of: Off / Follow / "name  ■■" (nullopt where the Lua raised an error: a name that is not text)
std::optional<std::string> label(const json* cfg, const std::vector<Item>& items, Kind kind, bool no_squares = false);
// DriveRows push_display, one row: the label (no swatch), the two chips (-1 = none), enabled (enabled ~= false)
struct RowShow { std::string title, label; int64_t chip1 = -1, chip2 = -1; bool enabled = true; };
std::optional<RowShow> row_show(Mech m, const json* cfg, const std::vector<Item>& items, int fighter, int side);

}  // namespace vfx::drive_cfg
