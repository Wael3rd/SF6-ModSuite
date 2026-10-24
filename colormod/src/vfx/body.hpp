// vfx::body - the Drive body glow (phase 2 step 3, sub-step B, docs/CPP_PORT_PHASE2_STEP3.md). Port of the two Lua
// material hooks on app.vfx.VfxActorEffectManager.addMaterialEffect / requestMaterialEffect: SF6_DIRecolor.lua's
// body_repaint (Drive Impact / Reversal: the side's two colours on Emit_Color / Emit_BaseColor) then
// SF6_DriveRecolor.lua's repaint_param (parry / rush: the pushed Drive-blue tinted with the side's colour), in the
// order the Lua registers them. The colours stay decided by the Lua (configs, presets, Follow): it pushes them every
// 6 frames (vfx_body_dr / vfx_body_di), and the fighters' actions on change (vfx_body_act, DI_ACTIONS fallback).
// Owner "vfx_body": native = the DLL writes and the Lua hooks stand by; lua = the DLL computes the same thing without
// writing (shadow) so vfx_body_probe can be compared with the Lua's own counters.
#pragma once

#include <cstdint>
#include <string>

namespace vfx::body {

bool install(bool write);                                // hooks; write = owner "vfx_body" native
void on_add_material(int argc, void** argv);            // called by the engine's addMaterialEffect hook when it carries ours
bool ready();                                            // hooks in place and not switched off by faults
void set_dr(const int64_t c[4]);                         // parry / rush colour, P1 then P2 (-1 = none)
void set_di(const int64_t c[8]);                         // impact c1 c2, reversal c1 c2, P1 then P2 (-1 = none)
void set_act(int side, int64_t act);                     // the side's current action id
std::string probe();

}  // namespace vfx::body
