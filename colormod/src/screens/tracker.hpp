// screens::tracker - colour-spin tracker ported to native paint (phase 3
// sub-step 3c, owner "screens_tracker"). The Lua keeps all game-state
// reads, entry tracking, key detection and editor logic. The DLL
// receives display data from the Lua and writes it through the core
// display API (labels, chip colours, camera hints, dot markers, hint
// block visibility).
//
// Bridge protocol:
//   Lua -> DLL: push display data per side (char select label + chips),
//               Fighter Settings setup (hint block hide), camera hints,
//               dot markers
//   DLL -> Lua: none (tracker does not capture user actions)
//
// Owner "screens_tracker": native = the DLL writes the UI elements
// (Lua pushes what to show); lua = everything in the Lua as before.
// SEH + fault counter: 5 faults switch the module off and hand back
// to the Lua.
#pragma once

#include <cstdint>
#include <string>

namespace screens::tracker {

bool install();                         // check core APIs; returns false on hard failure
bool ready();                           // installed and not switched off by faults

// ---- Lua -> DLL (push) --------------------------------------------------

// Per-side char select paint data. side = 1 or 2.
// text_addr = address of the spin's e_text control (0 = not resolved).
// label = UTF-8 label to display ("RW 1", "MC 3", etc.), "" = game owns the text.
// chip1/chip2 = RGBA uint32, or -1 = no chips.
// param_addr / colour_index: the menu's Param and its _cacheColorIndex when the label was computed (0 / -1 = no
// check): the paint stops as soon as the cursor is elsewhere.
void push_menu(int side, uint64_t text_addr, uint64_t rect0_addr, uint64_t rect1_addr,
               const std::string& label, int64_t chip1, int64_t chip2, uint64_t param_addr = 0, int64_t colour_index = -1);

// Side deactivated (no more paint for this side).
void leave_menu(int side);

// Fighter Settings screen opened: hide hint blocks c_00 and c_02.
// cm_addr = address of the agent's ControlMain.
void push_fs_setup(uint64_t cm_addr);


// ---- frame callback (tick / repaint) ------------------------------------

void on_frame();

std::string status();

}  // namespace screens::tracker
