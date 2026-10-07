// dialogs::options::driver — the colour editor's option dialogs, through the core's option service (sf6core 0.6.0,
// increment 6; docs/CPP_PORT_PHASE5.md section 10).
//
// The option engine (registry, dialog life cycle, the eight hooks, the GetMessage text provider) and its driver
// (the per-frame tick before the clients' frames, the BeginRendering pump) moved into the core (core/), so that every
// mod can use the game's option window. What stays here is the mod's side of it: the relay of SF6_ColorSpinExtra.lua
// (N) — the colour editor's queued orders (G R B S C D RB EB OM NV CL TD) turned into core calls after each call into
// the editor, and the core's callbacks for our entries and dialogs turned into the editor's.
#pragma once

#include <cstdint>
#include <string>

namespace dialogs::options::driver {

bool install();              // the core's option service, with our callbacks; false when the core has none
bool ready();                // installed and the core's service not failed
void apply();                // the relay's N.apply: the editor's orders since the last call
bool window_open();          // _G.NativeOptionsWindowOpen (the core's)
void* current_param();       // the dialog page on top / its parent (the core's)
void* parent_param();
int64_t focused_entry();     // the core's entry under the cursor, -1 when none of ours
void* row_control(int64_t entry, void* param);
std::string status();

}  // namespace dialogs::options::driver
