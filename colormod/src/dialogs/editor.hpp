// dialogs::editor - the costume colour editor and the DI colour menu, native (phase 4d + 4e): the game side.
// Design and working notes: docs/CPP_PORT_PHASE4_4DE.md.
//
// The editor itself is editor_core.{hpp,cpp}, free of any game header (tools/editor_harness runs it against the
// shipped Lua editor). This file is what the core cannot do without the game:
//   - the option engine: the core's registrations and requests are ORDERS queued here and applied by the Lua relay
//     (SF6_ColorSpinExtra.lua, edit.ned) on func/NativeOptions, the wrapper that already drives the native engine;
//     its queries (window open, focused row, a row's control, the current page) are read from dialogs::options;
//   - the dialog's controls: reads by reflection, writes through the shared core's display API;
//   - the preview: the working copy is shared with engine::live, which patches the model;
//   - files under reframework/data, the locale, the guide bar, the camera gate.
// What the screens know (Fighter Settings up, input device, the colour under the cursor, the live data of the
// model, the DI settings) is pushed by the relay before the calls that need it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sf6core_api.h"

namespace dialogs::editor {

bool install();              // backend in place; false = something it needs is missing (the feature stays with the Lua)
bool ready();                // installed and not switched off by faults

struct LuaFn { const char* name; SF6CoreLuaFn fn; };
const std::vector<LuaFn>& bridge_fns();      // the relay's entry points ("ed_*"), registered by common.cpp

// ---- the same entry points for the DLL's own screens (phase 5c-2: screens/colour_track) -------------------------------
// What edit.ned pushed and called: the screen (ed_screen), the input (ed_input), the open arguments (ed_open_args),
// a costume's slot union (ed_union), the opens and the frame loop. State = what the relay read back.
struct State { bool active = false, di = false; int nops = 0; bool has_closed_at = false; int64_t closed_at = 0; bool reopen = false; };
void set_screen(bool visible, bool pad, bool busy, uint64_t agent);
void set_input(uint32_t pad_mask, bool has_mouse, double x, double y, double w, double h);
void set_pad_bits(uint32_t btn, uint32_t a, uint32_t y, uint32_t b, uint32_t l1, uint32_t r1, uint32_t r3, uint32_t l3);
void set_open_args(int fighter, int costume, int color_id, int k, bool has_preview, bool has_live, bool has_chips, uint32_t g0, uint32_t g1);
void set_union(int fighter, int costume, std::vector<std::string> keys);
State open();
State open_di(int fighter, int costume, bool simple, int side, const std::string& mech, bool at_select);
State frame(int64_t frame);
State state();

// ---- the option engine's driver (phase 5d, dialogs/options_driver): the relay's calls without the Lua ----------
// kind 0 nothing, 1 close, 2 open (key); rebuild / push / pop: -1 nil, 0 false, 1 true (lua_ed_on_button's answer)
struct ButtonOut { int kind = 0; std::string key; int rebuild = -1, push = -1, pop = -1; };
std::string take_ops();                                   // ed_ops: the orders since the last call
void map_entry(int id, int64_t eid);                      // ed_map: the relay made that row
ButtonOut on_button(int id);                              // ed_on_button
void on_value(int id, int value);                         // ed_on_value
int on_created(uint64_t param);                           // ed_created -> orders waiting
int on_page_setup(uint64_t param, int rows);              // ed_page_setup -> orders waiting
int on_popped(uint64_t param);                            // ed_popped -> orders waiting

}  // namespace dialogs::editor
