// screens::dipanel - DIPanelMode ported to native hooks (phase 3 sub-step 3b,
// docs/CPP_PORT_PHASE3.md). The Lua keeps the business logic (presets, configs,
// enter/exit decisions); the DLL installs the hooks (UIPartsSpin, Param, MFS,
// UIAgent, UIPartsGroup) and does the tick repaint. The bridge protocol:
//   Lua -> DLL: push display state (texts, spin values, addresses, geometry)
//   DLL -> Lua: pull captured user actions (step direction on a mechanic, exit)
// Owner "screens_dipanel": native = hooks + repaint in the DLL (Lua hooks stand
// by); lua = everything in the Lua as before. SEH + fault counter: 5 faults
// switch the module off and hand back to the Lua.
#pragma once

#include <cstdint>
#include <string>

namespace screens::dipanel {

bool install();                         // hook all methods; returns false on hard failure
bool ready();                           // hooks in place and not switched off by faults

// ---- Lua -> DLL (push) --------------------------------------------------

// The Lua calls these in sequence when the DI mode enters or the display
// changes (step applied). The DLL stores the state and repaints from it.
void enter(int side, uint64_t param, uint64_t agent, uint64_t owner,
           uint64_t group, int32_t ci, int32_t co, int32_t n_mecs);
void set_spin(int side, int which, uint64_t addr, int32_t mn, int32_t mx, int32_t num);
void set_text(int side, int which, const std::string& title, const std::string& value);
void set_names(int side, const std::string& row_csv, const std::string& hide_csv);
void set_geom(int side, double ref_pos_x, double ref_region_w);
void leave(int side);
void set_cancel_block(uint64_t owner_addr, int32_t frames);

// ---- DLL -> Lua (pull) --------------------------------------------------

// Returns the next captured action for that side.
// evt: 0 = none, 1 = step, 2 = exit.  which: 1-based mec index.  dir: -1 or 1.
void pull(int side, int32_t& evt, int32_t& which, int32_t& dir);

// ---- frame callback (tick / repaint) ------------------------------------

void on_frame();

std::string status();

}  // namespace screens::dipanel
