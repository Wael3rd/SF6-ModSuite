// dialogs::camera - port of the costume editor's 3D camera preview (phase 4 sub-phase 4c, owner
// "dialogs_camera"). See docs/CPP_PORT_PHASE4.md section 2 "4c -- Camera preview" for the design.
//
// Ported from SF6_ColorSpinExtra.lua's PV.* table and preview_spin_tick (~lines 3483-3691):
// turn / zoom / pan of the app.FighterSpinCtrl's "Rot" transform and its ess_DefaultCamera child,
// input routing (mouse drag + wheel, pad stick + triggers, PageUp/PageDown), and the two camera
// hint blocks (c_00 "Move", c_02 "Zoom") on the Fighter Settings screen.
//
// Bridge protocol (see common.hpp BridgeCamera):
//   Lua -> DLL: open(agent_addr, pad, title_move, title_zoom) - called every ~5 frames while the
//               editor is open on Fighter Settings (same cadence/gate the Lua used for its own
//               camera_hints refresh: edit.active and NativeOptionsWindowOpen and fsm.visible).
//               Idempotent: keeps the agent address, input device and localized hint titles fresh.
//   Lua -> DLL: close() - called once when that gate goes false, and unconditionally from
//               editor_teardown (belt and suspenders): restores the camera's original position and
//               hides the hint blocks.
//   DLL -> Lua: status() - diagnostics.
//
// Divergence from docs/CPP_PORT_PHASE4.md's bridge table (flagged in the phase 4c report):
//   - The design lists four functions (camera_open, camera_close, camera_hints_setup, camera_status).
//     This implementation merges camera_hints_setup into camera_open: both need the same agent
//     address and are driven by the same per-tick gate in the Lua, so one call carries everything
//     (agent address, pad flag, localized hint titles) instead of splitting state across two calls.
//   - camera_open's spin_ctrl_addr argument is dropped: the DLL resolves app.FighterSpinCtrl itself
//     (scene scan, same as the Lua's PV.preview_find_spin), exactly like the Lua needed no address
//     either.
//   - Localized hint titles ("Move" / "Zoom") are passed in by the Lua instead of looked up via a
//     locale module, so 4c has no dependency on 4a (dialogs_locale), matching the design's
//     dependency graph (section 2: "4c has no dependency on 4a or 4b").
//
// Input: read natively in on_frame (no hook) -- GetAsyncKeyState for the same VK codes the Lua's
// key_down()/reframework:is_key_down() reads (left/middle mouse buttons share VK codes with
// keyboard keys, so one check covers both); GetCursorPos + ScreenToClient against the game's own
// window for the mouse position (the same data REFramework's ImGui backend derives its mouse
// position from, which is what the Lua's imgui.get_mouse() returns); via.hid.Mouse / via.hid.GamePad
// native singletons for the wheel and the pad axis/buttons (same singletons the Lua calls).
//
// No offline harness (docs/CPP_PORT_PHASE4.md section 5): the 3D transforms operate on live game
// objects (FighterSpinCtrl, ess_DefaultCamera) that only exist in a running game. Testing is in-game
// only.
#pragma once

#include <cstdint>
#include <string>

namespace dialogs::camera {

bool install();                         // resolve reflection handles; returns false on hard failure
bool ready();                           // installed and not switched off by faults

// ---- Lua -> DLL (push) ----------------------------------------------------

// Camera system active with this agent (for hint block resolution) and this input device.
// Idempotent; call every ~5 frames while the editor is open on Fighter Settings.
void open(uint64_t agent_addr, bool pad, const std::string& title_move, const std::string& title_zoom);

// Camera system inactive: restores the camera to its remembered original position (if it moved)
// and hides the hint blocks. Safe to call even when never opened, or already closed.
void close();

// Hides the hint blocks only, without restoring the camera position. Mirrors the Lua's periodic
// re-hide (SF6_ColorSpinExtra.lua: "also re-hidden periodically (the screen may re-show them)",
// cadence kHintsRehideSafetyFrames = 30): the Lua calls this every 30 frames while the Fighter
// Settings screen is visible and the hints are not currently shown, so a screen re-show doesn't
// leave stale blocks on display. Calling close() there instead would re-run the camera restore
// every 30 frames, which is not what the Lua does. agent_addr is the screen's current agent (the
// Lua's fsm.agent): independent of whether the camera/editor was ever opened this session, same as
// the Lua's PV.camera_hints(false), which only needs fsm.agent.
void hide_hints(uint64_t agent_addr);

// ---- frame callback (input polling, 3D writes, hint paint) ---------------

void on_frame();

std::string status();

}  // namespace dialogs::camera
