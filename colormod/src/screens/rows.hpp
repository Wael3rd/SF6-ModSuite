// screens::rows - the 4 (+1) cloned Drive colour rows (phase 3 step 3a, owner "screens_rows").
// Port of SF6_MenuRowsNative.cpp (creation) + DriveRows.lua (paint, input hijacks) into one module
// inside SF6_ColorModNative. When screens_rows is native, SF6_MenuRowsNative.dll must be absent (or
// renamed .dll.off), like vfx_engine / SF6_MemWriterNative.dll.
//
// The rows are created and painted natively. Settings stay owned by the Lua: the Lua pushes display
// data per row (title, value text, chip colours, enabled/arrows), and pulls user actions (add/sub,
// decide) each frame through the bridge. The DLL never reads or writes config files.
#pragma once

#include <cstdint>
#include <string>

namespace screens::rows {

bool install();                              // hooks (UIFlowMatchingSetting, UIFlowMatchingFighterSetting,
                                             // CharaSetting OnUpdate; UIPartsSpin Add/Sub/...; UIAgent
                                             // InputLeft/Right/Direction/Decide); false = refused
                                             // (SF6_MenuRowsNative loaded)
bool ready();                                // installed and not switched off by faults
std::string status();

// Bridge: the Lua pushes display data for each row whenever it changes (per row: title, value text,
// chip RGBA x2 or -1 for no chips, enabled flag, arrows visible). The DLL pulls user actions each
// frame (row index, direction +1/-1, decide flag) and the Lua applies them.

// Push display: called from the Lua bridge, one row at a time.
// row = 0..4 (impact, reversal, parry, rush, special), title and value are UTF-8.
// chip1/chip2 = RGBA uint32, or -1 when there are no chips (the chip group is hidden).
// enabled = whether the row is in the navigation (set_IsDisable false). arrows = show arrows.
void push_row(int row, const char* title, const char* value,
              int64_t chip1, int64_t chip2, bool enabled, bool arrows);

// Pull actions: returns 0 when nothing happened. Otherwise returns the count of actions placed
// in out[] (up to max). Each action: row (0..4), dir (+1 or -1, 0 for decide).
struct RowAction { int row; int dir; };      // dir: +1 = right, -1 = left, 0 = decide
int pull_actions(RowAction* out, int max);

// F6: suppress paint while the Lua says the screen is transitioning (editor open/close)
void set_suppress(bool on);

// Phase 5c-1: the model of func/DriveRows.lua (its tick's native path) in the DLL, on the settings of vfx::drive_cfg:
// the rows' values, left / right, the save 45 frames after the last change, the Color row's value placement. The
// Lua's screen tracking calls host() on its tick until 5c-2: Fighter Settings shown, its agent, the fighter / costume
// / side being edited, suppress (editor transitions).
void host(bool visible, uint64_t agent, int fighter, int costume, int side, bool suppress);
std::string focused_mech();                  // "impact", "reversal", "parry", "rush", "special" or "" (none)
std::string take_decide();                   // the mechanic of a Decide on a Drive row since the last call ("" none)

}  // namespace screens::rows
