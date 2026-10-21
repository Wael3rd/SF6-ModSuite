// screens::dipanel::mode — the DI mode of the character-select fighter panel and of the training popup, as
// func/DIPanelMode.lua ran it (phase 5c-1, docs/CPP_PORT_PHASE5.md section 8): entering takes the panel's rows over
// (Drive Impact / Reversal / Parry / Rush, Off / Follow / the colour library), a left / right steps the setting,
// leaving gives the rows back and saves. The hooks and the per-frame repaint stay in screens/dipanel.cpp; this file
// feeds them the display state the Lua pushed, from the DLL's settings (vfx::drive_cfg).
// Driven by SF6_ColorSpinExtra.lua's triggers until 5c-2 (the X key, the popup's X, the screens' agents).
#pragma once

#include <cstdint>
#include <string>

namespace screens::dipanel::mode {

// character select: L1 / X on that player's panel -> "on", "already on" (nothing: A / B leave, 0.22.6) or "failed"
std::string enter_select(int side, uint64_t param, uint64_t agent, int fighter, int costume);
// training pause > Character Settings popup: L1 / X (edit.di_panel.enter with the MatchingFighterSetting as owner); the
// claim of the side the popup sets is taken as on the select (the Slots Loader's popup costume options borrow its rows)
bool enter_popup(int side, uint64_t param, uint64_t agent, uint64_t mfs, int fighter, int costume);
bool active(int side);
// the training popup's DI mode is on, or was left less than 15 frames ago: the Esc that leaves it must not reach the
// pause menu (it closed the whole menu, and the exit then refreshed a popup being torn down: a NullReference)
bool popup_esc_guard();
// the Lua's DIPanelMode.tick: host_agent = that side's select menu agent now (0 = none / not a select host); drains
// the steps and the exit the hooks captured
void tick(int side, uint64_t host_agent);

}  // namespace screens::dipanel::mode
