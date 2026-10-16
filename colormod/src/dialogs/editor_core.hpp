// Costume colour editor, native (phase 4d, docs/CPP_PORT_PHASE4_4DE.md).
//
// Port of the editor of SF6_ColorSpinExtra.lua: its helpers (lines ~2088-2265 at commit 4dab476), the save / reset /
// erase / random actions (~2386-2522), the colour maths (~2523-2625), editor_build_pages (~2749-3211), editor_open
// (~3214-3266), the layout and the three option callbacks (~3269-3409) and the loop, editor_tick from the DI menu's
// tick to its end (~3898-4253). What comes before in editor_tick (the screens' triggers: keys on Fighter Settings,
// the DI panel, the Drive rows) stays in the Lua, which calls open() here.
//
// Pure: no game header, no REFramework. Everything outside the editor is a Backend function: the option engine as
// the editor uses it (func/NativeOptions' API), the dialog's UI nodes, input, clock, random, locale, files, the
// preview, the guide bar, the camera gate, the screens' state. tools/editor_harness runs the SHIPPED Lua editor and
// this core against the same model of that boundary and compares their traces line by line.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../engine/rsz.hpp"

namespace dialogs::editor::core {

using Obj = void*;                        // a UI control or a dialog Param: opaque here
namespace rsz = engine::rsz;

// nil / false / true, where the Lua passes an optional boolean (the option engine tells them apart in its trace)
enum Tri : int { kNil = -1, kFalse = 0, kTrue = 1 };
constexpr int kNoFocus = -1;              // focus index nil

// What a button's callback answers (the Lua: nil, "close", or { open, rebuild, push, pop })
struct ButtonResult {
    enum Kind { None, Close, Open } kind = None;
    std::string open;
    Tri rebuild = kNil, push = kNil, pop = kNil;
};

// One mechanic's setting of the DI menu, as the script's table holds it (nil kept apart from false / 0)
struct DiCfg {
    Tri enabled = kNil, follow = kNil, ll = kNil;
    std::optional<uint32_t> c1, c2;
    std::optional<double> selected;
    std::optional<std::string> sel;
};
struct DiPreset { std::optional<std::string> name; std::optional<uint32_t> c1, c2; };
enum DiList : int { kPresetsDI = 0, kPresetsDR = 1, kPresetsSP = 2 };

// The colour pages' palette (0.21.0): the avatar editor's colour grid, five lines of ten, 0xRRGGBB
constexpr int kPaletteLines = 5, kPaletteCols = 10;
extern const uint32_t kPalette[kPaletteLines][kPaletteCols];

// One slider / palette row of the page on top, for the game side's row looks (dialogs/row_look, 0.20.0)
struct RowLook {
    int eid = -1;                         // the editor's entry id
    int kind = 0;                         // 0 the option window's look, 1 hue slider (the rainbow), 2 palette row (0.21.0)
    int line = 0;                         // palette row: its line of the grid (0-based)
    int cursor = -1;                      // palette row: the cell under the cursor (0-based)
    int checked = -1;                     // palette row: the cell equal to the colour edited, -1 = none
    bool focused = false;                 // palette row: the cursor is on it
};

struct Backend {
    // ---- the option engine (func/NativeOptions as the editor calls it) ----
    int (*opt_group)(const std::string& key, const std::string& title, const std::string& desc) = nullptr;   // group id
    void (*opt_remove_group)(int gid) = nullptr;
    int (*opt_button)(int gid, const std::string& key, const std::string& title, const std::string& desc) = nullptr;   // entry id
    int (*opt_slider)(int gid, const std::string& key, const std::string& title, const std::string& desc, int mn, int mx, int init) = nullptr;
    int (*opt_choice)(int gid, const std::string& key, const std::string& title, const std::string& desc, const std::vector<std::string>& options, int init) = nullptr;
    void (*opt_group_def)(int gid, const std::string& title, const std::string& desc) = nullptr;   // g.title / g.desc assigned after the group was made
    void (*opt_rebuild)() = nullptr;
    bool (*opt_ensure_built)() = nullptr;
    void (*opt_open_menu)(const std::string& key, int focus) = nullptr;                       // focus kNoFocus = nil
    void (*opt_navigate)(const std::string& key, Tri rebuild, int focus, Tri now, Tri push, Tri pop) = nullptr;
    void (*opt_close)() = nullptr;
    bool (*opt_window_open)() = nullptr;                                                      // _G.NativeOptionsWindowOpen
    int (*opt_focused)() = nullptr;                                                           // entry id, -1 = none
    Obj (*opt_row_control)(int eid, Obj param) = nullptr;                                     // param null = the current page
    Obj (*opt_current_param)() = nullptr;
    Obj (*opt_parent_param)() = nullptr;
    Obj (*param_control_main)(Obj param) = nullptr;                                           // Param.<Agent>.get_ControlMain
    std::vector<Obj> (*dialog_control_mains)() = nullptr;                                     // every "OptionDialog" agent's
    // ---- UI nodes (reads as :call("get_..."), writes as func/CoreUI) ----
    Obj (*ui_child)(Obj) = nullptr;
    Obj (*ui_next)(Obj) = nullptr;
    std::string (*ui_name)(Obj) = nullptr;
    std::string (*ui_message)(Obj) = nullptr;
    bool (*ui_visible)(Obj) = nullptr;
    bool (*ui_actual_visible)(Obj, bool& value) = nullptr;                                    // false = the call failed (the Lua's pcall)
    bool (*ui_force_invisible)(Obj) = nullptr;
    bool (*ui_position)(Obj, double& x, double& y, double& z) = nullptr;                      // false = nil
    bool (*ui_size)(Obj, double& w, double& h) = nullptr;
    bool (*ui_scale)(Obj, double& x) = nullptr;
    bool (*ui_font_size)(Obj, double& h) = nullptr;
    Obj (*ui_find)(Obj root, const std::string& name) = nullptr;                              // func/UIFind (cached)
    void (*ui_set_text)(Obj, const std::string&) = nullptr;
    void (*ui_set_position)(Obj, double x, double y, double z) = nullptr;
    void (*ui_set_size)(Obj, double w, double h) = nullptr;
    void (*ui_set_scale)(Obj, double x, double y, double z) = nullptr;
    void (*ui_set_visible)(Obj, bool) = nullptr;
    void (*ui_set_force_invisible)(Obj, bool) = nullptr;
    void (*ui_set_font_size)(Obj, double w, double h) = nullptr;
    // ---- input, time, random, locale ----
    bool (*key_down)(int vk) = nullptr;
    // the keyboard key of the game's own previous / next selection (page turning): the symbol the game shows for it
    // ("<ICON KeyA>" on an AZERTY keyboard, "<ICON KeyQ>" on a QWERTY one) and the key to read; false = unknown
    bool (*page_key)(bool next, std::string& symbol, int& vk) = nullptr;
    uint32_t (*pad_mask)() = nullptr;
    bool (*mouse)(double& x, double& y, double& display_w, double& display_h) = nullptr;      // false = no imgui mouse
    int64_t (*frame)() = nullptr;                                                             // dbg.frame
    double (*clock)() = nullptr;                                                              // os.clock()
    int (*rand_int)(int lo, int hi) = nullptr;                                                // math.random(lo, hi)
    double (*rand_float)() = nullptr;                                                         // math.random()
    std::string (*L)(const std::string& key, const std::vector<std::string>& args) = nullptr;
    // ---- files, relative to reframework/data ----
    bool (*file_read)(const std::string& rel, std::vector<uint8_t>& bytes) = nullptr;         // false = missing
    bool (*file_write)(const std::string& rel, const std::vector<uint8_t>& bytes) = nullptr;  // creates the folders
    bool (*file_delete)(const std::string& rel) = nullptr;
    std::vector<std::string> (*glob_colors)() = nullptr;                                      // fs.glob of the colour files
    // ---- the preview and the game data behind it ----
    void (*refresh)() = nullptr;                                 // editor_refresh: the working copy to every preview controller
    bool (*has_preview)() = nullptr;                             // editor_ctrl() ~= nil
    rsz::NodeRef (*live_tree)(int color_id) = nullptr;           // live_to_tree(color_data(ctrl, id)); null = none
    // the slots ("pi_ci_k") enabled in ANY game colour of the costume: read once per fighter / costume from the live data
    std::vector<std::string> (*slot_union)(int fighter, int costume) = nullptr;
    void (*snapshot_pristine)(int fighter, int costume) = nullptr;
    void (*apply_original)(int color_id, const rsz::NodeRef& tree) = nullptr;   // RESET of a game colour: the live data back
    bool (*game_chips)(int fighter, int costume, int num, uint32_t& c0, uint32_t& c1) = nullptr;   // dbg.game_chips
    void (*purge_empty_files)() = nullptr;
    // ---- the screens around (the Lua's fsm) ----
    bool (*screen_visible)() = nullptr;                          // fsm.visible: Fighter Settings is up
    bool (*screen_pad)() = nullptr;                              // fsm.pad == true: the last device used is a pad
    bool (*screen_busy)() = nullptr;                             // fsm.refresh ~= nil or fsm.restore_steps > 0
    void (*screen_refresh_list)(bool has_path, const std::string& path) = nullptr;   // refresh_list(path)
    bool (*open_args)(int& fighter, int& costume, int& color_id, int& k) = nullptr;   // what editor_open reads from the screen
    void (*catalog_changed)() = nullptr;                         // build_catalog() / invalidate_caches() on the Lua's copies
    void (*caches_invalidated)() = nullptr;
    // ---- guide bar, camera gate ----
    void (*guide_bar)(const std::string& want, const std::string& desc, Obj param) = nullptr;
    void (*camera_open)(bool pad, const std::string& title_move, const std::string& title_zoom) = nullptr;
    void (*camera_close)() = nullptr;
    void (*camera_hide_hints)() = nullptr;
    void (*teardown_restore)() = nullptr;                        // editor_teardown's own game-side undo (zoom, blocked agents, menu input)
    void (*journal)(const std::string& line) = nullptr;
    // ---- the DI colour menu's data (SF6_DIRecolor / SF6_DriveRecolor) ----
    bool (*di_available)() = nullptr;                            // _G.SF6_DIRecolor is loaded
    std::string (*di_char_name)(int fighter) = nullptr;          // A.char_name(fighter); "" = none
    bool (*di_cfg)(int mech, int fighter, int side, DiCfg& out) = nullptr;   // cfg_of: false = nil (module or mechanic missing)
    // one assignment on the script's table: kind 0 = nil, 1 = boolean, 2 = number, 3 = string
    void (*di_cfg_set)(int mech, const char* field, int kind, bool b, double n, const std::string& s) = nullptr;
    bool (*di_presets)(int list, std::vector<DiPreset>& out) = nullptr;      // false = that list is not there
    void (*di_preset_set)(int list, int index, const std::optional<uint32_t>& c1, const std::optional<uint32_t>& c2) = nullptr;
    void (*di_preset_add)(int list, const std::string& name, const std::optional<uint32_t>& c1, const std::optional<uint32_t>& c2) = nullptr;
    void (*di_preset_remove)(int list, int index) = nullptr;
    void (*di_save_presets)() = nullptr;
    void (*di_save_cfg)(int fighter) = nullptr;
    void (*di_mark_dirty)() = nullptr;
    void (*drv_save_cfg)(int fighter) = nullptr;
    bool (*di_special_enabled)() = nullptr;                      // func/SpecialVFXGate
    // ---- the rows' own look (0.20.0, dialogs/row_look) ----
    void (*row_looks)(Obj param, const std::vector<RowLook>& rows) = nullptr;   // every frame while the window is up
    bool (*opt_set_value)(int eid, int value) = nullptr;         // a row's value in place (the unit's integer: a choice 0-based)
    void (*select_lr)(bool& left, bool& right) = nullptr;        // the menus' LEFT / RIGHT held now (keyboard and pad, 0.21.0)
};

void set_backend(const Backend& b);
void set_data_dir(const std::string& dir);                       // the folder standing for reframework/data (catalogue scan)

// ---- the Lua's entry points ----
void open();                                                     // editor_open (reads open_args)
// the DI colour menu (func/DIColorMenu.lua M.open): mech "" = the first page; at_select = opened from a select screen
void open_di(int fighter, int costume, bool simple, int side, const std::string& mech, bool at_select);
void frame();                                                    // the loop, once per frame
ButtonResult on_button(int eid);                                 // an entry's callback (the engine's B event)
void on_value(int eid, int value);                               // (the engine's V event: e.value = v, then the callback)
void on_dialog_created(Obj param);                               // Opt.on_dialog_created
void on_page_setup(Obj param, int rows);                         // Opt.on_page_setup (rows = entries of ours on that page)
void on_page_popped(Obj param);                                  // Opt.on_page_popped
bool active();                                                   // edit.active
bool di_active();                                                // edit.di
bool closed_at(int64_t& frame);                                  // edit.closed_at (false = nil)
// the working copy and its pulse, for the preview: false = nothing to show (closed, or the DI menu)
bool preview(rsz::NodeRef& tree, bool& hl_on, double& hl_k, std::vector<const rsz::Node*>& hl_targets);
void set_pad_bits(uint32_t btn, uint32_t a, uint32_t y, uint32_t b, uint32_t l1, uint32_t r1, uint32_t r3, uint32_t l3);   // via.hid.GamePadButton, read from the live enum
bool reopen_now();                                               // fsm.reopen_now
void reset();                                                    // script reset / tests

// ---- diagnostics and the harness' trace (same text as tools/editor_harness/ref.lua) ----
std::vector<std::string> debug_state();
std::vector<std::string> debug_groups();
std::string refresh_signature();                                 // "tree=<sum> hl=<on> k=<k> targets=<ids>" of the working copy
std::string status();

}  // namespace dialogs::editor::core
