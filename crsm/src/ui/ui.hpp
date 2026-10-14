// ui — the DLL's own screens, phase P4 of the port (owner "ui", default "lua"): the Record / Replay
// windows over 30 slots, the Reversal windows over 30 rows and the picker of 30, the REC HUD "Slot n", the text dialog,
// the message box, the tickers, the tab blocks and keys, the Community dialog on the core's option service. The Manager
// page is phase P5 (ui/manager_page, the core's pm_* service): asked of the core at the claim below, it calls ui::dialogs
// (dialogs.hpp); its T / pad Y comes through this layer's key reader, its RecordType through this layer's CreateViewData.
//
// Owner "ui" = native needs every other layer native (common.cpp). It is decided at the start, and the Lua loader
// (SF6_RecordingManager.lua) asks the DLL once at load through the core's Lua bridge (ui_claim): true = the loader does not
// load the Lua UI modules (NativeOptions, NativePauseMenu, NativeTicker, RecordingManagerTab, ReversalSlots), the Manager
// page was asked of the core and the DLL's screens start; false or no answer (no DLL, no core, a page the core refused, an
// error) = the Lua UI as before. P6: with no CRSM Lua installed and "standalone": "auto" (settings.json) the DLL claims the
// UI itself, in training (standalone.hpp, ui.cpp). The DLL's UI hooks are installed
// at the start with the other layers' (one R18 pass, mech::start) and stay inert until the claim: never two UIs at once.
//
// Threading: the UI hooks run on the game's UI job threads, concurrently (R29); every hook body and the UI
// part of the frame callback take ui::lock() (recursive: a game call made under it re-enters our own hooks on the same
// thread, as the Lua's script lock allowed), then mech::lock() when they touch a layer, never the reverse.
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace mech::hooks { struct Spec; }

namespace ui {

void init();                                   // before cmn::init(): layer declared, bridge function, frame callback
void start();                                  // after cmn::init() (owners known)
void hook_specs(std::vector<mech::hooks::Spec>& out);   // collected by mech::start (owner "ui" native only)
bool active();                                 // owner "ui" native, claimed (Lua loader or standalone), not stopped by faults
// The DLL claimed the UI itself (P6, settings.json "standalone": "auto" with no CRSM Lua installed) and it runs: the work the
// Lua still did next to the DLL's UI is the DLL's too (the dummy's character: MAIN's UpdateGameInfo forward).
bool standalone();
std::recursive_mutex& lock();
// Frame thread: a screen of the UI waits on the pause now (the pause menu seen open by the Record / Replay windows or the
// Manager page, a dialog or the Community dialog, the reversal picker, a hand-back waiting for the unpause): mech polls it.
bool needs_pause();
// RecordingManagerTab's T(key, ...): rsm.<key> of SF6_CRSM_data/locale.json in the game's text language (the key itself
// when missing everywhere)
std::string T(const std::string& key, const std::vector<std::string>& args = {});
// The slot names or contents changed outside the Record / Replay rows (the Manager's Open, Reset, Erase: P5's rows_dirty):
// the rows are redrawn at the next pass of their own list, then the names re-fitted (RMT rows_dirty). Any thread, no lock.
void rows_changed();

}  // namespace ui
