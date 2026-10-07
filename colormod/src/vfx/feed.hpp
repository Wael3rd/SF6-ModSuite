// vfx::feed — what SF6_DIRecolor.lua and SF6_DriveRecolor.lua pushed to the VFX engine from their UpdateBehavior
// (phase 5c-1, docs/CPP_PORT_PHASE5.md section 8), now computed by the DLL from its own settings (vfx::drive_cfg):
//   every 6 frames  the DI state of both sides (fighter, Impact / Reversal setting, colours, Follow, parts) and the
//                   Drive colours (parry / rush / special) -> vfx::tick
//   every 60 frames the parts list the providers found, kept in the character's DI file (parts_list)
//   every frame     in a fight, each fighter's action id on change -> vfx::body (the DI_ACTIONS fallback)
// The side's fighter: the battle, then the training select data (vfx::scene::side_char), then, for the DI state
// only, the colour being chosen on a select screen (pushed by SF6_ColorSpinExtra until 5c-2: set_side_hint).
// The text builders are pure (tools/settings_harness checks them against the Lua).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace vfx::feed {

// SF6_DIRecolor di_state_text for those fighters (fid[0] = P1, 0 = none); nullopt where the Lua raised (no push).
// Parts are written in key order (the Lua's pairs order varied; the reader keeps a map).
std::optional<std::string> di_state_text(const int fid[2]);
// SF6_DriveRecolor colors_for x 3 mechanics x 2 sides as vfx_dr_colors' twelve integers (-1 = nil)
std::optional<std::array<int64_t, 12>> dr_colors(const int fid[2]);
// SF6_DIRecolor parts_pull for one fighter, given vfx_parts' reply ("ok\n" + "key\tname\n" lines): true when the
// file's parts_list changed (it is then saved)
bool parts_merge(int fighter, const std::string& reply);

void on_frame();                                // the DLL's frame callback (game thread)
void set_side_hint(int side, int fighter);     // SF6_ColorSpinExtra.to_apply's fighter for that side (0 = none)
void mark_di_dirty();                           // SF6_DIRecolor.mark_dirty: a menu changed a DI setting
std::string status();

}  // namespace vfx::feed
