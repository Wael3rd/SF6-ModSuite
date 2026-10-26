// ui/internal — what the modules of the DLL's screens (P4) share. Not for the other layers (they include ui.hpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <reframework/API.hpp>

#include "logic.hpp"
#include "mech/hooks.hpp"
#include "ui.hpp"

namespace ui {

using MO = reframework::API::ManagedObject;

// The loader's flags (SF6_RecordingManager.lua): TrainingModeActive (flow 3 / 9 and _tData) and TrainingGamePaused (the
// pause bit, or an option window up), as read on the last frame callback.
bool training();
bool paused();
uint64_t tick();                               // frames of the core's frame callback (the Lua's `tick`)

// One hook spec of the UI layer (never "required": a hook refused by the R18 check or missing degrades its own feature,
// logged; `ok` tells the feature whether its hook is in place).
void spec(std::vector<mech::hooks::Spec>& out, const char* type, const char* sig, REFPreHookFn pre, REFPostHookFn post,
          const char* without, bool* ok);
void fault(const char* where);                 // a fault of the UI layer (5 = the UI stops, the game's menus stay)

// ---- windows (windows.cpp): Record / Replay pages, keys, menu edges ---------------------------------------------------
namespace windows {
void specs(std::vector<mech::hooks::Spec>& out);
void frame(bool training, bool paused);        // the LUB part of RecordingManagerTab (menu close edge, keys, install)
void hand_back();
std::string status();
bool needs_pause();                            // under ui::lock: the menu seen open (CreateViewData), its close not seen yet
// shared text helpers (RMT:756-771, 837-840, 1090-1095; RSL:111-119)
void tint(MO* t, bool focused);
void style_text(MO* t, MO* ref);               // RecordingManagerTab.style_text: the 65 properties, tags off, priority + 2
std::string slot_display_name(int n);          // RSM name of 1-8 / virtual name of 9-30
std::optional<std::string> live_frames_text(int n);   // "59F" / the empty word of live slot n (nullopt = no slot data)
std::string virtual_frames_text(int n);        // "78F" / the empty word of virtual slot n
struct PadHeld { bool l1 = false, r1 = false, y = false, x = false; };   // Y = RUp (Triangle), X = RLeft (Square)
PadHeld pad_held();                            // L1 / R1 / Y / X held on a connected pad now (the name keys' pad read; under ui::lock)
}  // namespace windows

// ---- reversal pages and picker (reversal_ui.cpp) ------------------------------------------------------------------------
namespace reversal_ui {
void specs(std::vector<mech::hooks::Spec>& out);
void menu_open();                              // rev_menu_open (+ the page windows reset)
void menu_close();
bool installed();
void frame(bool paused);                       // the picker's scroll seen from the frame (route 2A), its close
bool needs_pause();                            // under ui::lock: the picker is up (its close follows the pause)
// the shared cursor hook (UIPartsListBase.GetNextIndex / GetPrevIndex): the reversal window's claim and answer
struct Claim { bool on = false; int cur = 0; int dir = 0; int win = 0; MO* param = nullptr; };
Claim cursor_claim(void* list, int cur, int dir);
bool cursor_answer(const Claim& c, int nxt, int* ret);
void hand_back();
std::string status();
}  // namespace reversal_ui

// ---- dialogs (dialogs.cpp): text input, message box, tab blocks ---------------------------------------------------------
namespace dlg {
void specs(std::vector<mech::hooks::Spec>& out);
void frame(bool paused);                       // finished text dialogs, the message box poll (paused only)
bool naming();                                 // a text dialog is up
bool modal();                                  // a text dialog, the Community dialog, or a box under the pause: the tab held
// The menu's tab under CRSM's dialogs (logic.hpp TabGuard; L1+R1 / A+E): the data (TrainingManager._tData) put back to
// the spot the dialog opened on when it moved. at: the frame / CreateViewData's pre (the cursor row too) / InitTabItem's
// pre; fallback = the tab held when nothing was kept yet (-1: none). Under ui::lock; returns modal().
bool hold_tab(MO* tdata, logic::TabAt at, int64_t fallback);
void hand_back();
std::string status();
}  // namespace dlg

// ---- ticker (ticker.cpp) --------------------------------------------------------------------------------------------------
namespace ticker {
void specs(std::vector<mech::hooks::Spec>& out);
void frame();
void push(const std::string& text);
std::string status();
}  // namespace ticker

// ---- Community dialog (community_dialog.cpp) ----------------------------------------------------------------------------
namespace com {
void specs(std::vector<mech::hooks::Spec>& out);
void frame();
bool open();                                   // _com.dialog_open
void hand_back();
std::string status();
}  // namespace com

// ---- REC HUD (hud.cpp) ----------------------------------------------------------------------------------------------------
namespace hud {
void specs(std::vector<mech::hooks::Spec>& out);
void frame();                                  // the widget cache, and route 1B's write (frame callback)
void hand_back();                              // the UI stopped: the held widgets let go
std::string status();
}

}  // namespace ui
