// engine::decide pieces the colour screens use (screens/colour_track, phase 5c-2): same DLL, not a public interface.
// Call with live::mx() held (the decisions' lock).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <reframework/API.hpp>

#include "catalog.hpp"
#include "chips.hpp"

namespace engine::decide::internal {

using MO = reframework::API::ManagedObject;

Catalog& catalog();                                      // the decisions' catalogue (the Lua's catalog / entries_for)
const chips::Config& chip_cfg();                         // chip_slots.json as the decisions read it
std::vector<MO*> scene_ctrls();                          // dbg.scene_ctrls
MO* find_visual_ctrl(int side);                          // find_visual_ctrl: FighterVisual_0<side>'s controller
std::optional<chips::Pair> game_chips(int fighter, int costume, uint32_t num);   // dbg.game_chips (cached)
MO* battle_ctrl(int side);                               // gBattle.PBManager.Players[side - 1]._ColorCtrl
// dbg.set_color: only on a live colour controller still in the current scene (fresh = found in this frame's list)
bool set_color(MO* ctrl, std::optional<uint32_t> num, const std::string& why, bool fresh);
int ctrl_fighter(MO* ctrl);                              // dbg.ctrl_fighter: the fighter of its colour file (-1 = nil)

}  // namespace engine::decide::internal
