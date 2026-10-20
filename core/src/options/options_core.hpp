// dialogs::options::core - the option-dialog engine's state machine, without any game dependency (phase 4b).
//
// A line-for-line port of the per-frame callback and the public functions of func/NativeOptions.lua
// (docs/CPP_PORT_PHASE4.md section 4b). Every game operation goes through Backend, so the same file is compiled
// into the DLL (real backend, options.cpp) and into the offline harness (scripted backend,
// tools/options_harness): what the harness compares with the original Lua engine is the code that ships.
//
// The Lua wrapper drives the ticks (func/NativeOptions.lua, native block) in the Lua engine's own order, because
// three things stay on the Lua side until phases 4d / 4e: the callers' callbacks, the Lua globals
// (TrainingGamePaused, TrainingModeActive) and the function-valued titles / option lists.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dialogs::options::core {

using Obj = void*;   // a game object (unit, dialog handle, dialog param, row part); never dereferenced here

// ---- timing constants (func/NativeOptions.lua line cited) ----------------------------------------------------
constexpr int kOpenDelay          = 3;    // :821  ticks between the flag raise and the dialog start
constexpr int kSeamlessOldEnd     = 8;    // :784  ticks before the replaced dialog is ended
constexpr int kCloseRebuildSafety = 90;   // :828  max wait for the agent to vanish after a close
constexpr int kReopenWaitSafety   = 90;   // :836  max wait for the agent to vanish before a reopen
constexpr int kReopenPauseRelease = 30;   // :840  pause released if the old dialog will not die
constexpr int kReholdWaitMin      = 5;    // :845  new dialog up before the pause is taken again
constexpr int kReholdWaitSafety   = 60;   // :845  max wait for it
constexpr int kOpenMenuSuppress   = 20;   // :671  ticks after a close during which the training menu is refused
constexpr int kDeferredStable     = 5;    // :805  ticks with both flags false before a deferred open
constexpr int kValueFullPass      = 10;   // :945  cadence of the close detection and of the full value pass
constexpr int kBuildRetry         = 60;   // :798  cadence of the first build attempts
constexpr int kCloseDetectMin     = 10;   // :946  ticks after an open before the close detection may fire
constexpr int kPlaceCheck          = 60;   // cadence of the check of the rows placed in a game section (increment 11)
constexpr const char* kComposite  = "SF6 Tools";      // :147 COMPOSITE (the composite window itself is not ported)
constexpr const char* kAgentName  = "OptionDialog";   // :828 agent_present("OptionDialog")

enum class Kind { Toggle, Choice, Slider, Button };

struct Entry {
    int64_t id = -1;
    Kind kind = Kind::Toggle;
    std::string key, title, desc;
    std::vector<std::string> options;   // toggle: off / on; choice: labels; slider, button: empty
    int32_t min_val = 0, max_val = 0;   // slider range (toggle and choice: 0 .. #options - 1 at build)
    int64_t value = 0;                  // the game's int: toggle 0 / 1, choice 0-based, slider raw (the Lua converts)
    int64_t last_int = 0;               // last value read from, or written to, the unit
    // set by the build (backend)
    int32_t type_id = 0;
    Obj unit = nullptr;
    uint64_t title_guid[2] = {0, 0};
    std::vector<uint64_t> option_guids; // hi, lo per option
    bool placed = false;                // its group is placed in a section of the game's Options screen (increment 11)
};

struct Group {
    int64_t id = -1;
    std::string title, desc, key;       // key: "" when the caller gave none (:140 g.key == nil)
    std::vector<std::shared_ptr<Entry>> entries;
    Obj unit = nullptr;                 // a placed group has none: its entries are rows of the game's section
    int32_t place_tab = -1;             // increment 11: app.Option.TabType, -1 = the core's own window
    int32_t place_section = -1;         // increment 11: app.Option.GroupType of the section
};

// The game operations. `false` from a bool-returning call = the call failed (the Lua's pcall returned false).
struct Backend {
    void (*retain)(Obj) = nullptr;                    // a reference is held while the engine keeps the object
    void (*release)(Obj) = nullptr;                   // (the Lua wrappers hold one for as long as they live)
    bool (*get_is_end)(Obj handle, bool& ended) = nullptr;
    void (*end_handle)(Obj handle) = nullptr;         // End() unless get_IsEnd (:733)
    Obj  (*start_dialog)(Obj unit) = nullptr;         // UIFlowOptionBGDialog.Start (:757-760)
    bool (*agent_present)(const char* name) = nullptr;
    bool (*request_pause)(bool on) = nullptr;         // PauseManager.requestPause(on, 8) (:719)
    bool (*build_units)() = nullptr;                  // :262-392, reports through the build_* functions below
    void (*purge_roots)(bool keep_live) = nullptr;    // :593-606
    bool (*get_value)(Obj unit, int32_t& out) = nullptr;
    void (*set_value)(Obj unit, int32_t value, bool refresh) = nullptr;
    int32_t (*type_id_base)() = nullptr;              // :110-118 first id of this load
    void (*sync_placed)(bool keep_live) = nullptr;    // increment 11: the placed rows in their game sections (re-insert / remove)
};
void set_backend(const Backend& b);

// ---- registry (the Lua's groups / entries) -------------------------------------------------------------------
int64_t add_group(const std::string& key);                    // key "" = the caller gave none
void remove_group(int64_t group_id);
int64_t add_entry(int64_t group_id, Kind kind, const std::string& key);
// Increment 11: the group's entries become rows of a section of the game's Options screen (tab = app.Option.TabType,
// section = app.Option.GroupType) at the next build, instead of a page of the core's window; -1, -1 = back to the window.
void set_group_place(int64_t group_id, int32_t tab, int32_t section);
// What build() reads from the Lua tables, pushed by the wrapper before each build (titles and option lists may be
// functions there): :291 group title, :302 entry title, :227 descriptions, :309-315 options, :326 range and value.
void set_group_def(int64_t group_id, const std::string& title, const std::string& desc);
void set_entry_def(int64_t entry_id, const std::string& title, const std::string& desc,
                   const std::vector<std::string>& options, int32_t min_val, int32_t max_val, int64_t value);
std::shared_ptr<Entry> entry_by_id(int64_t entry_id);          // nullptr once nothing references it any more
std::shared_ptr<Entry> entry_by_type(int32_t type_id);         // known_ids[TypeId]
bool known(int32_t type_id);
bool placed_type(int32_t type_id);                             // known and placed in a game section (increment 11)
void set_value(int64_t entry_id, int64_t value, bool refresh); // Group:set / M.set_value (:193-197, :1095-1101)

// ---- build (:262-392, :608-612) ------------------------------------------------------------------------------
const std::vector<std::shared_ptr<Group>>& groups();
std::string id_key_of(const Group& g);                         // tostring(g.key or g.title) (:290)
int32_t type_id_for_key(const std::string& key);               // ids_by_key (:209-217)
bool has_type_id_for_key(const std::string& key, int32_t& out);
void build_set_root(Obj root);
void build_set_group(Group& g, Obj unit);
void build_set_entry(const std::shared_ptr<Entry>& e, int32_t type_id, Obj unit);
Obj root_unit();
bool built();
bool build();        // pcall(build): true when the tree is built
void rebuild();      // :608-612

// ---- requests (M.open, M.open_menu, M.open_after_unpause, M.navigate, M.close) --------------------------------
void request_open(const std::string& title);                   // :678 (the Lua's two guards are in the wrapper)
void request_open_menu(const std::string& title, int focus_index);   // :682-687
void open_after_unpause(const std::string& title);             // :703-706
// :690-695. Returns true when the page change runs right now (the wrapper refreshed the titles before the call
// when a rebuild is asked); otherwise the request is queued behind the pending button presses.
void navigate(const std::string& key, bool rebuild_tree, int focus_index, bool now, bool push, bool pop);
bool navigate_runs_now(bool now);                              // now and menu_mode and the window is open
void close();                                                  // :749 M.close = end_dialog()

// ---- the per-frame callback, in the Lua's order (:794-1002) --------------------------------------------------
enum TickCode : int {
    kTickNone = 0,
    kTickNotBuilt = 1,      // :797-800 not built, nothing to do this frame
    kTickBuild = 2,         // :798 the wrapper builds (fresh titles), then the frame is over
    kTickPhase1 = 3,        // :817 the wrapper checks its lists and getters, then calls phase1()
    kTickRebuild = 4,       // :828-830 the wrapper rebuilds (fresh titles)
};
int tick_a(int64_t tick, bool paused, bool training_active);   // :796-849
void phase1();                                                  // :821
void tick_a2();                                                 // :850-861 replaced dialogs ended
// :872-894, after the wrapper ran the queued callbacks
void button_result(bool want_close, bool want_open, const std::string& open_key, bool want_rebuild,
                   bool want_push, bool want_pop, bool want_refresh);
bool button_result_builds(bool want_close, bool want_open, bool want_rebuild, bool want_refresh);
void tick_b();                                                  // :908-1001 (composite window excepted)
void reset();                                                   // :1003-1015 + a fresh Lua state

// ---- hooks (state effects only; the game reads are in options.cpp) --------------------------------------------
bool window_open();                                             // _G.NativeOptionsWindowOpen
void on_created_object(Obj param);                              // :462-472 (caller checked window_open, :455)
int take_pending_focus();                                       // :481-484, -1 = none
struct PagePart { int32_t type_id; int32_t event_type; Obj part; };
bool on_page_setup(Obj param, const std::vector<PagePart>& parts);   // :487-526, true = one of our pages
bool on_button(int32_t type_id);                                // :412-417 / :430-440, true = queued
bool suppress_open_menu();                                      // :671

// ---- queries ---------------------------------------------------------------------------------------------------
Obj current_param();
Obj parent_param();                                             // :1028

// ---- events for the wrapper, in the order they happened --------------------------------------------------------
// Records separated by \x1e, fields by \x1f:
//   V eid n            a value read back from its unit changed (:928-931, :992-995)
//   B eid              one of our buttons was pressed (:415, :437)
//   N key flags        M.navigate queued (:694); flags: 1 rebuild, 2 push, 4 pop
//   C param            dialog created (:468-471)
//   S param eid part eid part ...   page built (:507, :514-524)
//   P param            the top page was closed by the game, the parent is current again (:911-913)
//   X                  the window is closed (:949-956)
std::string pull_events();

std::string status();

}  // namespace dialogs::options::core
