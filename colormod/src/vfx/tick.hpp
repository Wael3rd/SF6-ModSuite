// vfx::tick - the Drive VFX battle loop (phase 2 step 3, sub-steps C-D-F, owner "vfx", docs/CPP_PORT_PHASE2_STEP3.md).
// Port of the UpdateBehavior blocks of SF6_DriveRecolor.lua (fight enter / exit, spots load / scan / revalidation,
// in-fight rescans, relay rebuild every 60 frames) and SF6_DIRecolor.lua (providers every 30 frames, static path).
// The Lua keeps the settings and the menus: it pushes the resolved colours every 6 frames (vfx_dr_colors,
// vfx_di_state) and reads the parts list back (vfx_parts). Owner vfx lua = shadow: each relay the Lua writes is
// rebuilt here from the same spots and compared line by line (journal), nothing is written.
#pragma once

#include <cstdint>
#include <string>

namespace vfx::tick {

bool install(bool own);                       // own = owner vfx native (needs vfx_engine and vfx_body native)
bool ready();                                 // owner and installed
void set_dr_colors(const int64_t c[12]);      // P1 parry c1 c2, rush c1 c2, special c1 c2, then P2 (-1 = nil)
void set_di_state(const std::string& text);   // "S side fighter enabled follow" / "C side mech c1|- c2|- follow [key=HEX ...]"
void ui(const std::string& req);              // "scan", "rescan", "restore", "paint:<side>:<mech>" (the DR debug panel)
std::string parts(int fighter);               // "key\tname" lines (SF6_DIRecolor collect_parts)
std::string status();

}  // namespace vfx::tick
