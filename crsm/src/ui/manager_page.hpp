// ui/manager_page — the Manager page (sub-page 0 of the Recording strip) on the Native Core's training pause-menu
// service (ADR-0016, core 0.8.0: pm_*, pm_row_of), owner "ui" (phase P5).
//
// Only when settings.json gives "ui" to the DLL (default "lua": nothing at all), with every other layer native (common.cpp),
// and only when the core has the service (SF6CORE_HAS(api, pm_status); else one log line and "ui" stays with the Lua).
// The page is asked of the core only when the Lua loader hands the UI over (claim(), from ui.cpp's ui_claim): before that,
// or when the loader keeps the Lua UI, the strip belongs to the Lua's own engine (one engine per strip). Then:
//  * one page (title rsm.page, the English guide of v1.6) at index 0 of the Recording strip: Manager > Record > Replay;
//  * eight rows: the loadout spin (64 option slots built once, labels retexted in place, arrows 70 px apart: the core
//    wraps Q / D and fits the font) and seven buttons (Open, Save, Save As, Reset, Erase, Share, Download), all keeping
//    the menu open; T (or pad Y) on the page renames the selected loadout (no hint, as in v1.6): P4's key reader
//    (ui/windows.cpp) routes the key here (rename_key);
//  * the key-guide hints "Edit slot name" (Y) / "Delete slot name" (X) on the Record page's slot rows only;
//  * the sub-page memory: RecordType set at the end of P4's CreateViewData pre-hook (before_create_view_data);
//  * the texts from SF6_CRSM_data/locale.json (ui/lang) in the game's language, retexted by pm_set_texts when it changes.
// The core's callbacks run on the game's UI job threads (R29): they only queue (decide, spin) or read; the actions run
// on the core's frame callback (game thread): files layer calls under mech::lock(), P4's dialogs (ui/dialogs.hpp) and
// the tickers outside it. Given back (faults, a layer gone back): pm_remove of the page.
#pragma once

#include <cstdint>

namespace ui::manager {

void init();                  // before cmn::init(): "ui" implemented, frame / status / hand-back registered
void start();                 // after cmn::init(), before ui::start(): the core's pm service and files native, else ui declined
// The Lua loader's claim (ui_claim, the loader's thread, once): the page, its rows and callbacks asked of the core. false =
// it could not be asked (ui declined: the loader keeps the Lua UI).
bool claim();

// For the other native UI pieces (P4: Record / Replay windows, keys, Community dialog):
bool page_shown();            // the Manager page is the one the menu shows (live); false when ui is not native
uint64_t rows_epoch();        // +1 each time the Record / Replay slot rows must be redrawn (RMT rows_dirty: Open, Reset, Erase)
// After a Community download: the loadout list read again and the spin synced on the next frame (RMT:276-278); any thread.
// false = no Manager page running (the caller re-reads the files list itself).
bool refresh_list();
void rename_key();            // T / pad Y on this page (P4's key reader): the selected loadout renamed (RMT:1764)
void before_create_view_data();   // P4's CreateViewData pre-hook, last: RecordType = the sub-page to reopen on
void game_page_built(bool replay);  // P4's Function pre-hook, 107 Record / 108 Replay built: the sub-page to reopen on
bool needs_pause();           // frame thread: the page saw the menu up (its close edge follows the pause)

}  // namespace ui::manager
