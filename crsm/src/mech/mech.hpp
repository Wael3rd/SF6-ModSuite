// mech — the four non-UI layers of phase P3: files (P3a), replay (P3b), reversal
// (P3c), record (P3d). Each is "lua" unless settings.json says "native" (a missing settings.json: owners.hpp); a "lua"
// layer changes nothing. With a layer native, the Lua keeps its UI and calls the DLL through the bridge
// (reframework/autorun/crsm/CrsmNative.lua): the shims in MAIN, RMT and RSL.
//
// Threading: the Lua serialized everything on the script lock; here the bridge calls (Lua threads), the
// game hooks (fight / menu threads) and the frame callback (game thread) are concurrent. One recursive mutex, lock(),
// guards every layer's state; it is recursive because a game call made under it may enter one of our hooks on the same
// thread. It is never taken by the journal. Game writes happen only on the game thread (frame callback = core
// LateUpdateBehavior), inside a game hook, or inside a bridge call that the Lua makes from inside a game hook (the
// moment the Lua already wrote then).
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "sf6core_api.h"

namespace model::files { class Engine; }

namespace mech {

using json = nlohmann::json;

std::recursive_mutex& lock();

// Game state kept by the frame callback (game thread). No game read per frame when nothing can change:
//  * training: flow 3 / 9 (the core's flow callback) and TrainingManager._tData (the loader's TrainingModeActive), _tData
//    looked for only after a flow change (at once, then every 60 frames until it is there);
//  * the pause (PauseManager._CurrentPauseTypeBit not 64 / 2112, GameState.in_pause_menu) and the core's option window
//    are polled only while something waits on them: a menu edge in progress (the Record / Replay windows' open menu, the
//    Manager page's menu, a dialog, the picker, the 30-entry arrays installed, a hand-back waiting for the unpause) or for
//    120 frames after menu_event(). Otherwise paused / window are false and polled is false: a reader that needs the real
//    value at such a moment (the record watch, an import's allocation, a check before a one-shot read) calls pause_now().
struct State {
    bool training = false;
    bool paused = false;          // the pause bit (only when polled)
    bool window = false;          // an option window of the core is up (only when polled)
    bool polled = false;          // paused / window were read this frame
    uint64_t frame = 0;
};
State state();
struct Pause {
    bool bit = false;             // the pause bit
    bool window = false;          // an option window
    bool up() const { return bit || window; }   // the loader's TrainingGamePaused (+ option windows, GameState.lua:74)
};
// Game thread (frame callback) only: this frame's poll, or a read now (kept for the rest of the frame).
Pause pause_now();
// Any thread (a hook): the pause menu is being built (CreateViewData): the pause is polled for the next 120 frames.
void menu_event();
// +1 at each flow change, each training edge (in / out) and each menu close the poll saw: the modules' one-shot work
// (pages capture, the dummy's id, the language, the loadout list read ahead, the HUD label watch) follows these.
uint64_t events();

// Tickers of asynchronous work: popped by frame_poll for the Lua to show, or by the DLL's own UI (owner "ui" native, P4:
// frame_poll then leaves them alone).
void push_ticker(const std::string& text);
bool pop_ticker(std::string* text);
// The menu edges come from their driver: the Lua UI (it polls frame_poll every frame while it hands a layer to the DLL:
// false when it stopped for 120 frames) or the DLL's own UI (owner "ui" native: always true).
bool lua_alive();
// The REC / replay HUD's slot label (record first, then replay; 0 = none): what frame_poll gives the Lua.
int hud_label();

// Called by main.cpp: init() before cmn::init() (layers declared, bridge functions and frame hooks registered), start()
// after it (owners known: hooks of the native layers installed, R18 checked).
void init();
void start();

// ---- the layers' entry points (each in its own file) --------------------------------------------------------------
namespace files {
void init();                      // declares the layer and its bridge functions
void start();
void frame(uint64_t frame);
model::files::Engine& engine();   // under lock()
int fighter();                    // the dummy's FighterID the files layer holds (-1 = none), under lock()
// fill_record (MAIN:1025-1044) into a RecordData (a managed object): buffer allocated when missing or short, Num, Cursor 0
int64_t fill_record(void* record_data, const json& entry);
// called after any files call that may have imported slots 9-30 (the replay layer reacts as RMT:2101-2102)
void set_import_listener(void (*fn)());
// MAIN's ensure_files with the id the Manager page reads (SelectMenu.PlayerDatas[1].FighterID), as files_ensure does
bool ensure();
// a files call made by the DLL's own UI (P4) that may have imported slots 9-30: the replay layer follows (after_files_call)
void after_ui_call();
// For the native UI's own calls into engine() (the Manager page, P5): what the bridge functions do around them.
void after_call();                // after an import / reopen: the import listener, as f_import_selected
int select_menu_id();             // TrainingManager._tData.SelectMenu.PlayerDatas[1].FighterID, -1 = unknown (MAIN:757-762)
}

// HUD slot label sources for frame_poll (record first, then replay): 0 = none
void set_label_source(int which, int (*fn)());

namespace hooks { struct Spec; }

namespace replay {
void init();
void start();
void hook_specs(std::vector<hooks::Spec>& out);
void unload_for_record();         // the record layer's arm: slot 8's own content back when the DLL's playback holds it
void own8_back();                 // the record layer's finalize put slot 8's own content back with the game running
// The menu edges and the Delete of a virtual row, for the DLL's own UI (P4): what the bridge functions replay_menu_open /
// replay_menu_close / replay_forget do for the Lua UI. menu_open returns the installed 30-entry array (nullptr = none).
void* menu_open();
void menu_close();
void forget(int n);
bool needs_pause();               // under lock(): the 30-entry array installed, or a hand-back waiting for the unpause
}

namespace record {
void init();
void start();
// The record layer for the DLL's own UI (P4), as the bridge functions rec_state / rec_arm / rec_retarget / rec_drop /
// rec_finalize / rec_menu_open give it to the Lua UI.
struct View { bool armed = false; int slot = 0; bool seen_now = false; };
View view();
void arm(int n);
void retarget(int n);
void drop();
void finalize(bool paused);
void menu_open();
}

namespace reversal {
void init();
void start();
void hook_specs(std::vector<hooks::Spec>& out);
// For the DLL's own UI (P4): rev_menu_open / rev_menu_close / rev_map_get of the bridge.
bool menu_open();                 // true = the 30-row arrays are in place (the page windows are then reset)
void menu_close();
std::optional<int64_t> map_get(int64_t rt, int64_t row);
bool needs_pause();               // under lock(): the 30-row arrays installed, or a hand-back waiting for the unpause
}

// ---- bridge helpers ---------------------------------------------------------------------------------------------------
namespace br {
const char* str(const SF6CoreValue* a, uint32_t n, uint32_t i);              // "" when not a string
bool is_str(const SF6CoreValue* a, uint32_t n, uint32_t i);
int64_t i64(const SF6CoreValue* a, uint32_t n, uint32_t i, int64_t def = 0); // ints and integral doubles
bool boolean(const SF6CoreValue* a, uint32_t n, uint32_t i);                 // Lua truthiness
json value(const SF6CoreValue* a, uint32_t n, uint32_t i);                    // nil / bool / number / string as json
// A table the Lua sent with json.dump_string: parsed as is, then its nulls (= empty Lua tables: dump_string writes an
// empty table as null and a table never holds nil) turned back into empty tables. discarded() on a parse error.
json from_lua(const char* text);
// What json.load_string gives back from this text = what json.load_file gives from the file the DLL writes.
std::string to_lua(const json& v);

struct Out {
    SF6CoreValue* r; uint32_t max; uint32_t n = 0;
    void nil();
    void boolean(bool v);
    void i64(int64_t v);
    void str(const std::string& v);              // copied into a per-thread buffer that outlives the call
    void val(const json& v);                     // a scalar json as its Lua value (tables: as to_lua text)
    int32_t done() const { return (int32_t)n; }
};
}  // namespace br

}  // namespace mech
