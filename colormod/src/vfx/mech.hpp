// vfx::mech - Drive mechanic detection per side (phase 2 step 3, sub-step A,
// docs/CPP_PORT_PHASE2_STEP3.md). Port of SF6_DriveRecolor.lua's requestEffect hook (lines ~1119-1153): which
// player a VfxActorEffectManager belongs to (side_of: getComponent(app.PlayerBehavior)._PlayerID, once per manager
// address), the VfxID label (category @0x10, container @0x14, element @0x18), ETIQUETTES / ETIQ_INV, the DI
// container rule (di_cont, pushed by the Lua), last_mech / mech_at and recent_mech (MECH_HOLD 150 frames).
// Nothing is written here. With vfx_body native, vfx::body reads it and the Lua's requestEffect hook stands by (the
// relay builder reads the managers through vfx_mech_mgrs); with vfx_body lua it runs in shadow next to the Lua, and
// the two can be compared label by label (vfx_mech_probe against SF6_DriveRecolor._probe).
#pragma once

#include <cstdint>
#include <string>

namespace vfx::mech {

bool install();                                          // types; false = shadow off
// from vfx::engine's requestEffect hook (game thread, inside its SEH guard), after the relay writes
void on_request(uint64_t mgr, uint32_t cat, uint32_t cont, uint32_t el);
void on_combat_exit();                                   // the relay was emptied: forget the managers (Lua mgr_vus)
void set_di_cont(int64_t c1, int64_t c2);                // the Lua's di_cont[1..2] (-1 = nil), refreshed every 60 frames
std::string probe();                                     // "mech1=.. mech2=.. mgr1=.. mgr2=.. req_all=.. req_n=.. seen1=lab:n,... seen2=..."
// for vfx::body (sub-step B)
int side_now(void* mgr);                                 // Lua side_of(this:get_GameObject()): _PlayerID + 1, 0 = none
const char* recent(int side);                            // Lua recent_mech(side): the mechanic if seen within MECH_HOLD
uint64_t manager(int side);                              // Lua mgr_addr[side] (0 = not seen yet in this fight)
// the effect managers of the fighters found in the scene (PlayerBehavior -> its GameObject's VfxActorEffectManager),
// so the relay has both sides before their first effect: a side whose first effect of the fight was a Drive Impact
// used to get that first DI in the game's colours (Wael, 2026-10-03). Returns the sides set.
int discover();

}  // namespace vfx::mech
