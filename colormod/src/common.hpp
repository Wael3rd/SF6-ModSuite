// SF6_ColorModNative - shared pieces: core client, feature ownership, journal, kill switch.
// Plan: docs/CPP_PORT_PLAN.md (phase 0 = foundations, no visible change).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sf6core_api.h"

namespace cmn {

// Features handed over from the Lua scripts one at a time (plan section 3). Until a feature is implemented here
// and validated in game, its owner stays "lua" whatever the settings say.
// Editor = the costume editor's preview patch (phase 1, step 1); Engine = the colour engine (step 2: the DLL writes
// every other patch, the Lua still decides what each controller wears); Decisions = step 3: the DLL also decides
// (sides, wanted entry, GetColorList entries, mirror rule, battle pass, squares). Decisions needs Engine.
// VfxEngine = phase 2 step 2: SF6_MemWriterNative absorbed (vfx/engine), the Lua hands it the relay through the bridge.
// phase 3 (native screens), one switch per screen: ScreensRows = the 4 Drive rows (DriveRows + MenuRows absorbed),
// ScreensDipanel = DIPanelMode, ScreensTracker = the colour-spin tracker, ScreensGuide = guide hints + corner_tick
// phase 4 (dialogs and editors, docs/CPP_PORT_PHASE4.md), one switch per sub-phase: DialogsLocale = 4a, the
// NativeLocale text service (language probe + locale.json lookup, L()/lang() served through the bridge).
// DialogsEditor = 4d + 4e, the costume colour editor and the DI colour menu (docs/CPP_PORT_PHASE4_4DE.md).
// ScreensColour = phase 5c-2, the colour screens' tracking, the selection memory, the triggers (screens/colour_track).
enum class Feature : int { Engine = 0, Vfx, Screens, Dialogs, Editor, Decisions, VfxEngine, VfxBody,
                           ScreensRows, ScreensDipanel, ScreensTracker, ScreensGuide, DialogsLocale, DialogsCamera,
                           DialogsOptions, DialogsEditor, ScreensColour, Count };

const char* feature_name(Feature f);
bool owns(Feature f);            // true = this DLL runs that feature (Lua stands down)
bool active();                   // false = kill file present, or the core is missing / refused us
bool closing();                  // true from the moment the game window gets WM_CLOSE / WM_DESTROY / WM_ENDSESSION
bool bridge_owns(const std::string& name);   // the Lua's edit.native_bridge(name) ~= nil: owned, and ready
// phase 5e: the option engine's frame and the colour screens' frame, last in the core's frame callback (where the Lua
// script called them, after the core's callback); set by main.cpp
extern void (*g_screens_frame)();

// Logging: REFramework log (and the core log when registered) + the rolling journal.
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
// Journal only: data/SF6_ColorModNative_data/debug_log.json, 400 lines "[f<frame>] ...", same format as the
// Lua journal (data/SF6_ColorSpinExtra_data/debug_log.json), flushed every 30 frames when it changed.
void journal(const char* fmt, ...);

uint64_t frame();                // frames seen by the core's frame callback (0 before the first one)
// Light fight (default, 0.19.7): in a fight the colours are written once and nothing is recomputed (vfx tick,
// decisions). Off while reframework/data/SF6_ColorModNative_data/live_fight.txt exists (looked at every 5 s):
// the per-effect painting and the periodic checks as before.
bool light_fight();
const SF6CoreAPI* core();        // nullptr when the core is absent
SF6CoreClient* client();

// Called once from reframework_plugin_initialize. Returns false only on a hard failure (the plugin then stays
// loaded but inert).
bool init();

// Colour-patch module (engine/live), filled by main.cpp before init(). Since phase 5, only status/ready
// are used by the Lua bridge; the native editor (dialogs/editor) calls engine::live directly.
struct BridgeEditor {
    std::string (*status)() = nullptr;
    bool (*ready)() = nullptr;
};
extern BridgeEditor g_bridge_editor;

// Decisions module (engine/decide), filled by main.cpp before init(). The Lua queries squares and
// follow_debug for diagnostics; the colour screens push/pull natively (screens/colour_track).
struct BridgeDecide {
    bool (*ready)() = nullptr;                        // installed and not switched off by faults
    void (*frame)() = nullptr;                        // core frame callback
    bool (*squares)(int fighter, int only_side, uint32_t& c0, uint32_t& c1) = nullptr;
    std::string (*follow_debug)(int fighter) = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeDecide g_bridge_decide;

// VFX write engine (vfx/engine), filled by main.cpp before init(). Since phase 5, only status/probe
// functions are used by the Lua bridge; the tick/feed modules are driven natively.
struct BridgeVfx {
    bool (*ready)() = nullptr;
    std::string (*status)() = nullptr;
    std::string (*mech_probe)() = nullptr;
    std::string (*body_probe)() = nullptr;
    bool (*body_ready)() = nullptr;
    bool (*tick_ready)() = nullptr;
    std::string (*tick_status)() = nullptr;
    void (*feed_frame)() = nullptr;
};
extern BridgeVfx g_bridge_vfx;

// Drive rows module (screens/rows), filled by main.cpp before init(). The colour screens
// (screens/colour_track) call rows::host directly; only status is still on the Lua bridge.
struct BridgeRows {
    bool (*ready)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeRows g_bridge_rows;
// DIPanelMode module (screens/dipanel), filled by main.cpp. The colour screens call dipanel
// directly; only on_frame and status are still on the Lua bridge.
struct BridgeDipanel {
    bool (*ready)() = nullptr;
    void (*on_frame)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeDipanel g_bridge_dipanel;

// Tracker module (screens/tracker), filled by main.cpp. The colour screens call tracker
// directly; only on_frame and status are still on the Lua bridge.
struct BridgeTracker {
    bool (*ready)() = nullptr;
    void (*on_frame)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeTracker g_bridge_tracker;

// Guide module (screens/guide), filled by main.cpp. The colour screens call guide
// directly; only on_frame and status are still on the Lua bridge.
struct BridgeGuide {
    bool (*ready)() = nullptr;
    void (*on_frame)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeGuide g_bridge_guide;

// Locale module (dialogs/locale), filled by main.cpp. Phase 4a, docs/CPP_PORT_PHASE4.md section 2 "4a".
struct BridgeLocale {
    bool (*ready)() = nullptr;
    void (*on_frame)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeLocale g_bridge_locale;
// Camera module (dialogs/camera), filled by main.cpp. Phase 4 sub-phase 4c. The colour screens
// and the native editor handle the camera gate; only on_frame and status are still on the Lua bridge.
struct BridgeCamera {
    bool (*ready)() = nullptr;
    void (*on_frame)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeCamera g_bridge_camera;
// Options engine (dialogs/options, phase 4b), filled by main.cpp. Since sf6core 0.6.0 the option engine
// is the core's service; only its state (ready/status) is needed.
struct BridgeOptions {
    bool (*ready)() = nullptr;
    std::string (*status)() = nullptr;
};
extern BridgeOptions g_bridge_options;
// The native colour editor and DI menu (dialogs/editor, phase 4d + 4e), filled by main.cpp: its relay's entry
// points ("ed_*", registered with the others) and whether it can run.
struct BridgeDialogEditor {
    bool (*ready)() = nullptr;
    struct Fn { const char* name; SF6CoreLuaFn fn; };
    std::vector<Fn> fns;
};
extern BridgeDialogEditor g_bridge_dialog_editor;

// Background file writer: a single thread that drains queued file writes off the game thread.
// Only for plain file I/O (journal, status, spots, perf log); never touches game objects.
namespace bgwriter {
void start();                                   // call once from cmn::init()
void write(const char* path, std::string content);   // truncate-write
void append(const char* path, std::string content);  // append-write
}  // namespace bgwriter

}  // namespace cmn
