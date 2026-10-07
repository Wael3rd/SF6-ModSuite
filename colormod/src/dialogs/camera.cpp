// dialogs::camera - port of the costume editor's 3D camera preview.
// See camera.hpp header comment and docs/CPP_PORT_PHASE4.md section 2 "4c -- Camera preview".
//
// Rules kept:
//   R1  all GUI writes from the game thread (on_frame runs from the core's frame callback)
//   R2  every cached object (spin controller, camera transform) is re-resolved / revalidated
//       (is_managed_object) before use, same as the Lua's PV.preview_spin / PV.preview_cam
//   R7  text / position / visibility writes go through the core (compare-before-write, R7)
//   R16 /EHa (SEH around the frame tick)

#include "camera.hpp"

#include <windows.h>
#pragma comment(lib, "user32.lib")   // EnumWindows / GetCursorPos / ScreenToClient / GetAsyncKeyState

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ll_common.hpp"

#include "../common.hpp"

namespace dialogs::camera {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

// ---- constants (named, every one citing its Lua source line) -------------

constexpr int kMaxFaults = 5;

// PV.preview_zoom / PV.preview_pan limits and speed (SF6_ColorSpinExtra.lua:1689-1690)
constexpr float kZoomNear = 14.1f;        // edit.zoom_near
constexpr float kZoomFar = 0.0f;          // edit.zoom_far
constexpr float kPanSpeedFar = 0.005f;    // edit.pan_speed, at the far limit

// PV.preview_zoom easing (SF6_ColorSpinExtra.lua:3554-3557)
constexpr float kZoomExpo = 2.5f;
constexpr float kZoomNotchStep = 0.025f;

// PV.preview_pan clamp (SF6_ColorSpinExtra.lua:3573-3574)
constexpr float kPanClampX = 4.0f;
constexpr float kPanClampYNeg = 3.0f;
constexpr float kPanClampYPos = 4.0f;

// preview_spin_tick: mouse (SF6_ColorSpinExtra.lua:3577-3608)
constexpr int kVkLButton = 0x01;          // :3584 left drag = turn
constexpr int kVkMButton = 0x04;          // :3606 middle drag = pan
constexpr int kVkPageUp = 0x21;           // :3603 PageUp = zoom in
constexpr int kVkPageDown = 0x22;         // :3603 PageDown = zoom out
constexpr float kMouseTurnSpeed = 0.5f;   // :3586 (m.x - last_x) * 0.5 / sx
constexpr double kReferenceWidth = 1920.0;  // :3581 sx = d.x / 1920
constexpr double kRightOfDialogXRef = 1040.0; // :3581 m.x > 1040 * sx
constexpr float kWheelNotchDivisor = 120.0f;  // :3595 one notch = 120 raw units
constexpr float kWheelClamp = 2.0f;           // :3597
constexpr float kPageKeyZoomStep = 0.25f;     // :3603 PageUp/PageDown step

// preview_spin_tick: pad (SF6_ColorSpinExtra.lua:3609-3620). These three are only the Lua's INITIAL
// values (:3341): at load, the Lua re-reads them from the live via.hid.GamePadButton enum (fields
// LStickPush / RTrigBottom / LTrigBottom, :3365-3369) and overwrites PAD.L3/R2/L2 if found. install()
// does the same (g_pad_l3/r2/l2 below); these constants are only the fallback when the enum or a
// field is not found.
constexpr uint32_t kPadL3Fallback = 4096;
constexpr uint32_t kPadR2Fallback = 2048;
constexpr uint32_t kPadL2Fallback = 512;
constexpr float kPadStickDeadzone = 0.2f;   // :3616, :3618
constexpr float kPanSpeedPad = 12.0f;       // :3617 px/frame
constexpr float kTurnSpeedPad = 3.0f;       // :3618 deg/frame
constexpr float kZoomSpeedPad = 0.4f;       // :3619 notch/frame

// Camera hints cadence (SF6_ColorSpinExtra.lua:4108, :4110)
constexpr uint64_t kHintsCadenceFrames = 5;
constexpr uint64_t kHintsRehideSafetyFrames = 30;   // :4110 "also re-hidden periodically (the screen may re-show them)"

// Camera hint blocks (SF6_ColorSpinExtra.lua:1691-1693 edit.hint_cfg)
constexpr const char* kMoveBlock = "c_00";
constexpr const char* kZoomBlock = "c_02";
constexpr const char* kMoveIconCtrl = "p_InputGuide_Y_s_gUIWTCameraY";
constexpr const char* kZoomIcon1Ctrl = "p_InputGuide_LT_s_aUIScaleDown";
constexpr const char* kZoomIcon2Ctrl = "p_InputGuide_RT_s_aUIScaleUp";
constexpr float kMoveBlockX = 1648, kMoveBlockY = 646;
constexpr float kMoveTitleX = 158, kMoveTitleY = 42;
constexpr float kMoveIconX = 143, kMoveIconY = 50;
constexpr float kMovePadIconX = 118, kMovePadIconY = 50;
constexpr float kZoomBlockX = 1648, kZoomBlockY = 508;
constexpr float kZoomTitleX = 158, kZoomTitleY = 96;
constexpr float kZoomIcon1X = 125, kZoomIcon2X = 165, kZoomIconY = 97;
constexpr float kZoomPadIcon1X = 125, kZoomPadIcon2X = 160, kZoomPadIconY = 100;

constexpr float kPi = 3.14159265358979323846f;
float deg2rad(float d) { return d * (kPi / 180.0f); }
float rad2deg(float r) { return r * (180.0f / kPi); }
float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

// ---- core helpers (hint paint) --------------------------------------------

const SF6CoreAPI* api() { return cmn::core(); }
SF6CoreClient* cl() { return cmn::client(); }

bool is_managed(MO* o) { return o && API::get()->sdk()->managed_object->is_managed_object(o); }

// ---- fault handling (same pattern as tracker / dipanel / guide) -----------

bool g_on = false;
int  g_faults = 0;

void fault(const char* where) {
    ++g_faults;
    cmn::journal("[camera] fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("[camera] too many faults, off (Lua takes over)");
    }
}

// ---- resolved reflection handles (install() populates these once) --------

struct Resolved {
    API::TypeDefinition* spin_td = nullptr;         // app.FighterSpinCtrl
    API::Method* spin_get_go = nullptr;              // FighterSpinCtrl::get_GameObject
    API::Method* scene_get_current = nullptr;        // via.SceneManager::get_CurrentScene
    API::Method* scene_find_components = nullptr;    // via.Scene::findComponents(System.Type)
    API::Method* arr_len = nullptr;                  // System.Array::GetLength
    API::Method* arr_get = nullptr;                  // System.Array::GetValue(System.Int32)
    API::Method* go_get_tr = nullptr;                // via.GameObject::get_Transform
    API::Method* tr_get_go = nullptr;                // via.Transform::get_GameObject
    API::Method* tr_get_child = nullptr;              // via.Transform::get_Child
    API::Method* tr_get_next = nullptr;               // via.Transform::get_Next
    API::Method* go_get_name = nullptr;               // via.GameObject::get_Name
    API::Method* tr_get_local_euler = nullptr;        // via.Transform::get_LocalEulerAngle
    API::Method* tr_set_local_euler = nullptr;        // via.Transform::set_LocalEulerAngle(via.vec3)
    API::Method* tr_get_local_pos = nullptr;          // via.Transform::get_LocalPosition
    API::Method* tr_set_local_pos = nullptr;          // via.Transform::set_LocalPosition(via.vec3)
    void* mouse_singleton = nullptr;                  // via.hid.Mouse
    void* pad_singleton = nullptr;                    // via.hid.GamePad
    API::Method* mouse_get_merged = nullptr;          // via.hid.Mouse::get_MergedDevice
    API::Method* pad_get_merged = nullptr;            // via.hid.GamePad::get_MergedDevice
};
Resolved R;

// ---- state ------------------------------------------------------------------

std::mutex g_mx;
bool g_active = false;                 // Lua: edit.active and NativeOptionsWindowOpen and fsm.visible
uint64_t g_agent_addr = 0;             // fsm.agent (for hint block resolution)
bool g_pad = false;                     // fsm.pad (input device for hint glyphs)
std::string g_title_move, g_title_zoom; // localized hint titles (dbg.L("hint.move"/"hint.zoom"), passed in by the Lua)

MO* g_spin_ctrl = nullptr;              // edit.spin_ctrl
MO* g_cam = nullptr;                    // edit.cam

bool g_has_orig = false;                // edit.cam_orig
MO*  g_orig_cam = nullptr;
float g_orig_x = 0, g_orig_y = 0, g_orig_z = 0;

bool g_dragging_turn = false;           // edit.spin_last_x ~= nil
float g_spin_last_x = 0;
bool g_dragging_pan = false;            // edit.pan_last ~= nil
float g_pan_last_x = 0, g_pan_last_y = 0;
bool g_wheel_init = false;              // edit.wheel_last ~= nil
float g_wheel_last = 0;

bool g_have_last_mouse = false;         // frozen mouse position while the game is not the foreground window
float g_last_mouse_x = 0, g_last_mouse_y = 0;

uint64_t g_ticks = 0, g_hint_paints = 0;   // diagnostics

// Pad bitmasks, resolved from the live via.hid.GamePadButton enum in install() (review S3); start at
// the Lua's own fallback values, same as the Lua does before its enum re-read at load.
uint32_t g_pad_l3 = kPadL3Fallback;
uint32_t g_pad_r2 = kPadR2Fallback;
uint32_t g_pad_l2 = kPadL2Fallback;

// ---- Win32 input: the game window, mouse position / keys ------------------
//
// REFramework's Lua reads the mouse through imgui.get_mouse() / imgui.get_display_size() (the
// ImGui backend's cached, window-relative mouse state) and keys through reframework:is_key_down
// (GetAsyncKeyState under the hood: SF6_AgentDriver injects keys with SendInput+KEYEVENTF_SCANCODE
// specifically because a posted "tap" message is NOT seen by it, which only a real hardware-level
// key state read would miss). No such API is exposed to native plugins (API.hpp has no
// is_key_down / mouse-position member), so this reimplements the same two primitives directly:
//   - keys/mouse buttons: GetAsyncKeyState, the same mechanism reframework:is_key_down uses.
//   - mouse position: GetCursorPos + ScreenToClient against the game's own window, which is
//     exactly what ImGui's Win32 backend does internally to produce the mouse position the Lua
//     reads; the window's client size stands in for imgui.get_display_size().
// Divergence (flagged): this is reconstructed independently rather than read from the same ImGui
// context the Lua uses (no such access is exposed to native plugins either); it should match in
// the game's normal fullscreen/borderless configuration, but the mouse-drag camera turn/pan and
// the "right of dialog" gate are the one part of 4c that can only be confirmed in game.
//
// Review fix (S1): ImGui's Win32 backend only updates its cached mouse position while the game
// window is the foreground window (Win32 stops delivering WM_MOUSEMOVE to a background window), so
// imgui.get_mouse() is frozen when the game loses focus. GetCursorPos/ScreenToClient has no such
// gate (it reads the live OS cursor regardless of focus), so without this check, dragging the mouse
// in another window could still turn/pan the game's camera. mouse_state() below mirrors the Lua:
// the position (and the wheel delta computed from it in preview_spin_tick) only refreshes while the
// game is the foreground window; otherwise the last known position is kept, which yields a zero
// drag delta. Keys are intentionally NOT gated here: reframework:is_key_down (GetAsyncKeyState) also
// reacts while the game is in the background, so the native path must not be stricter than the Lua.
bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

HWND game_window() {
    static HWND cached = nullptr;
    if (cached && IsWindow(cached)) return cached;
    cached = nullptr;
    struct Ctx { DWORD pid; HWND best = nullptr; long best_area = 0; };
    Ctx ctx{ GetCurrentProcessId() };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0; GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != c->pid || !IsWindowVisible(hwnd)) return TRUE;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return TRUE;
        const long w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return TRUE;
        const long area = w * h;
        if (!c->best || area > c->best_area) { c->best = hwnd; c->best_area = area; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    cached = ctx.best;
    return cached;
}

bool game_has_focus(HWND hw) { return hw && GetForegroundWindow() == hw; }

bool mouse_state(float& x, float& y, float& disp_w, float& disp_h) {
    HWND hw = game_window();
    if (!hw) return false;
    RECT rc{};
    if (!GetClientRect(hw, &rc)) return false;
    disp_w = (float)(rc.right - rc.left);
    disp_h = (float)(rc.bottom - rc.top);
    if (disp_w <= 0 || disp_h <= 0) return false;
    if (game_has_focus(hw)) {                        // S1: only refresh while the game has focus (imgui.get_mouse() parity)
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(hw, &p)) {
            g_last_mouse_x = (float)p.x; g_last_mouse_y = (float)p.y;
            g_have_last_mouse = true;
        }
    }
    if (!g_have_last_mouse) return false;            // never focused yet: no position to report (matches no drag)
    x = g_last_mouse_x; y = g_last_mouse_y;
    return true;
}

// ---- native devices: via.hid.Mouse (wheel) / via.hid.GamePad (axis, buttons) ----
// Same singletons and calls the Lua uses (sdk.get_native_singleton / sdk.call_native_func).

MO* merged_device(void* singleton, API::Method* get_merged) {
    if (!singleton || !get_merged) return nullptr;
    auto r = get_merged->invoke(reinterpret_cast<MO*>(singleton), std::vector<void*>{});
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

// get_Wheel is a System.Single (cumulative notches); REFramework hands a float back as a double (InvokeRet.d).
// Read as an integer it gave bit patterns, and the zoom did nothing sensible (found in game, 04/10).
float mouse_wheel_raw(MO* device) {
    static API::Method* m = nullptr;
    if (!device) return 0;
    if (!m) m = ll::find_method_noargs(device->get_type_definition(), "get_Wheel");
    if (!m) return 0;
    auto r = m->invoke(device, std::vector<void*>{});
    return r.exception_thrown ? 0.0f : (float)r.d;
}

bool pad_axis_r(MO* device, float& x, float& y) {
    static API::Method* m = nullptr;
    if (!device) return false;
    if (!m) m = ll::find_method_noargs(device->get_type_definition(), "get_AxisR");
    if (!m) return false;
    auto r = m->invoke(device, std::vector<void*>{});
    if (r.exception_thrown) return false;
    std::memcpy(&x, r.bytes.data(), 4);
    std::memcpy(&y, r.bytes.data() + 4, 4);
    return true;
}

uint32_t pad_button_mask(MO* device) {
    static API::Method* m = nullptr;
    if (!device) return 0;
    if (!m) m = ll::find_method_noargs(device->get_type_definition(), "get_Button");
    if (!m) return 0;
    auto r = m->invoke(device, std::vector<void*>{});
    return r.exception_thrown ? 0u : r.dword;
}

// ---- via.vec3 get / set (System.Array-style: pointer arg for >8 byte value types,
// proven by SF6_MenuRowsNative.cpp's set_Position(via.vec3) / get_Position() use) ----

bool get_vec3(API::Method* m, MO* obj, float out[3]) {
    if (!m || !obj) return false;
    auto r = m->invoke(obj, std::vector<void*>{});
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), 12);
    return true;
}
bool set_vec3(API::Method* m, MO* obj, float x, float y, float z) {
    if (!m || !obj) return false;
    std::array<uint8_t, 16> buf{};
    std::memcpy(buf.data(), &x, 4);
    std::memcpy(buf.data() + 4, &y, 4);
    std::memcpy(buf.data() + 8, &z, 4);
    auto r = m->invoke(obj, std::vector<void*>{ buf.data() });
    return !r.exception_thrown;
}

// ---- System.Array walk (REFramework's Lua get_size/get_element quirk, engine/live.cpp:96-103) --

uint32_t arr_len_of(MO* a) {
    if (!a) return 0;
    auto r = R.arr_len->invoke(a, std::vector<void*>{ ll::arg_i32(0) });
    return r.exception_thrown ? 0u : (uint32_t)std::max<int32_t>(0, (int32_t)r.dword);
}
MO* arr_at(MO* a, uint32_t i) {
    auto r = R.arr_get->invoke(a, std::vector<void*>{ ll::arg_i32((int32_t)i) });
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

// ---- Transform / GameObject tree walk (PV.preview_find_spin / find_named) ------

// Direct children only, no self-check: SF6_ColorSpinExtra.lua:3499-3503 (preview_find_spin's "Rot" scan).
bool has_direct_child_named(MO* parent_tr, const char* name) {
    MO* ch = ll::invoke_object_ret(parent_tr, R.tr_get_child, {});
    while (ch) {
        MO* go = ll::invoke_object_ret(ch, R.tr_get_go, {});
        MO* nm = go ? ll::invoke_object_ret(go, R.go_get_name, {}) : nullptr;
        if (ll::managed_string_to_utf8(nm) == name) return true;
        ch = ll::invoke_object_ret(ch, R.tr_get_next, {});
    }
    return false;
}
MO* get_direct_child_named(MO* parent_tr, const char* name) {
    MO* ch = ll::invoke_object_ret(parent_tr, R.tr_get_child, {});
    while (ch) {
        MO* go = ll::invoke_object_ret(ch, R.tr_get_go, {});
        MO* nm = go ? ll::invoke_object_ret(go, R.go_get_name, {}) : nullptr;
        if (ll::managed_string_to_utf8(nm) == name) return ch;
        ch = ll::invoke_object_ret(ch, R.tr_get_next, {});
    }
    return nullptr;
}
// Self + descendants, depth <= 6: SF6_ColorSpinExtra.lua:3517-3525 (PV.preview_cam's find_named).
MO* find_descendant_named(MO* tr, const char* name, int depth) {
    if (!tr || depth > 6) return nullptr;
    MO* go = ll::invoke_object_ret(tr, R.tr_get_go, {});
    MO* nm = go ? ll::invoke_object_ret(go, R.go_get_name, {}) : nullptr;
    if (ll::managed_string_to_utf8(nm) == name) return tr;
    MO* child = ll::invoke_object_ret(tr, R.tr_get_child, {});
    while (child) {
        if (MO* found = find_descendant_named(child, name, depth + 1)) return found;
        child = ll::invoke_object_ret(child, R.tr_get_next, {});
    }
    return nullptr;
}

// SF6_ColorSpinExtra.lua:3490-3508: scan the scene for every app.FighterSpinCtrl, keep the last one
// that owns a direct "Rot" child ("the last one owning a Rot child wins").
MO* preview_find_spin() {
    g_spin_ctrl = nullptr;
    void* sm = API::get()->get_native_singleton("via.SceneManager");
    if (!sm) return nullptr;
    MO* scene = ll::invoke_object_ret(reinterpret_cast<MO*>(sm), R.scene_get_current, {});
    if (!scene) return nullptr;
    MO* type_obj = R.spin_td->get_runtime_type();
    if (!type_obj) return nullptr;
    MO* comps = ll::invoke_object_ret(scene, R.scene_find_components, std::vector<void*>{ (void*)type_obj });
    if (!comps) return nullptr;
    const uint32_t n = arr_len_of(comps);
    for (uint32_t i = 0; i < n; ++i) {
        MO* c = arr_at(comps, i);
        if (!c) continue;
        MO* go = ll::invoke_object_ret(c, R.spin_get_go, {});
        MO* tr = go ? ll::invoke_object_ret(go, R.go_get_tr, {}) : nullptr;
        if (tr && has_direct_child_named(tr, "Rot")) g_spin_ctrl = c;
    }
    return g_spin_ctrl;
}
MO* preview_spin() {
    if (g_spin_ctrl && is_managed(g_spin_ctrl)) return g_spin_ctrl;
    return preview_find_spin();
}
// SF6_ColorSpinExtra.lua:3513-3529: ess_DefaultCamera under the spin controller's transform, depth 6.
MO* preview_cam() {
    if (g_cam && is_managed(g_cam)) return g_cam;
    g_cam = nullptr;
    MO* ctrl = preview_spin();
    if (!ctrl) return nullptr;
    MO* go = ll::invoke_object_ret(ctrl, R.spin_get_go, {});
    MO* tr = go ? ll::invoke_object_ret(go, R.go_get_tr, {}) : nullptr;
    g_cam = tr ? find_descendant_named(tr, "ess_DefaultCamera", 0) : nullptr;
    return g_cam;
}

// SF6_ColorSpinExtra.lua:3544-3546: remembers the position the camera had before the FIRST zoom/pan
// of the session (not at open time) -- mirrored exactly so a session with no zoom/pan needs no restore.
void remember_orig(MO* cam, float x, float y, float z) {
    if (!g_has_orig) { g_has_orig = true; g_orig_cam = cam; g_orig_x = x; g_orig_y = y; g_orig_z = z; }
}
// SF6_ColorSpinExtra.lua:2176-2180 (editor_teardown): restore + forget, re-resolve cam next time.
void restore_orig() {
    if (g_has_orig) {
        if (g_orig_cam && is_managed(g_orig_cam)) set_vec3(R.tr_set_local_pos, g_orig_cam, g_orig_x, g_orig_y, g_orig_z);
        g_has_orig = false;
        g_orig_cam = nullptr;
    }
    g_cam = nullptr;
}

// SF6_ColorSpinExtra.lua:3530-3542 (PV.preview_turn). Re-resolves "Rot" fresh every call (R2).
void preview_turn(float delta_deg) {
    MO* ctrl = preview_spin();
    if (!ctrl || delta_deg == 0.0f) return;
    MO* go = ll::invoke_object_ret(ctrl, R.spin_get_go, {});
    MO* tr = go ? ll::invoke_object_ret(go, R.go_get_tr, {}) : nullptr;
    MO* rot = tr ? get_direct_child_named(tr, "Rot") : nullptr;
    if (!rot) return;
    float e[3];
    if (!get_vec3(R.tr_get_local_euler, rot, e)) return;
    const float yaw_deg = rad2deg(e[1]) - delta_deg;
    set_vec3(R.tr_set_local_euler, rot, e[0], deg2rad(yaw_deg), e[2]);
    ll::write_field_f32(ctrl, "mAngle", yaw_deg);
}

// SF6_ColorSpinExtra.lua:3547-3561 (PV.preview_zoom): exponential easing, ~40 notches end to end.
void preview_zoom(float notches) {
    if (notches == 0.0f) return;
    MO* cam = preview_cam();
    if (!cam) return;
    float pos[3];
    if (!get_vec3(R.tr_get_local_pos, cam, pos)) return;
    remember_orig(cam, pos[0], pos[1], pos[2]);
    // more negative = closer (Lua comment, checked 02/09); named z_far/z_near, not far/near (Windows.h macros)
    const float z_far = -kZoomFar, z_near = -kZoomNear;
    const float u = clamp01((pos[2] - z_far) / (z_near - z_far));
    float t = 1.0f - std::pow(1.0f - u, 1.0f / kZoomExpo);
    t = clamp01(t + notches * kZoomNotchStep);
    const float z = z_far + (z_near - z_far) * (1.0f - std::pow(1.0f - t, kZoomExpo));
    set_vec3(R.tr_set_local_pos, cam, pos[0], pos[1], z);
}

// SF6_ColorSpinExtra.lua:3562-3576 (PV.preview_pan): screen pixels (1080p-reference), speed shrinks
// when close.
void preview_pan(float dx_px, float dy_px) {
    if (dx_px == 0.0f && dy_px == 0.0f) return;
    MO* cam = preview_cam();
    if (!cam) return;
    float pos[3];
    if (!get_vec3(R.tr_get_local_pos, cam, pos)) return;
    remember_orig(cam, pos[0], pos[1], pos[2]);
    const float span = std::max(0.1f, kZoomNear - kZoomFar);
    const float closeness = clamp01((std::fabs(pos[2]) - kZoomFar) / span);
    const float k = kPanSpeedFar * (1.0f - 0.95f * closeness);
    const float x = clampf(pos[0] - dx_px * k, -kPanClampX, kPanClampX);
    const float y = clampf(pos[1] + dy_px * k, -kPanClampYNeg, kPanClampYPos);
    set_vec3(R.tr_set_local_pos, cam, x, y, pos[2]);
}

// SF6_ColorSpinExtra.lua:3577-3620 (preview_spin_tick): mouse drag / wheel / pad input routing.
void preview_spin_tick() {
    float mx = 0, my = 0, dw = 0, dh = 0;
    const bool have_mouse = mouse_state(mx, my, dw, dh);
    const float sx = have_mouse ? (float)(dw / kReferenceWidth) : 1.0f;
    const bool right_of_dialog = have_mouse && (mx > (float)(kRightOfDialogXRef * sx));

    // mouse: left drag = turn
    const bool down = have_mouse && right_of_dialog && key_down(kVkLButton);
    if (down && !g_dragging_turn) preview_find_spin();            // drag start: fresh lookup (outfit change rebuilds the model)
    if (down && g_dragging_turn) preview_turn((mx - g_spin_last_x) * kMouseTurnSpeed / sx);
    g_dragging_turn = down;
    if (down) g_spin_last_x = mx;

    // mouse: wheel (or PageUp / PageDown) = zoom
    // S1: no wheel delta while the game is not the foreground window (same focus gate as the mouse
    // position above); the baseline (g_wheel_last) is still kept fresh so refocusing does not produce
    // a jump from a stale reading accrued while in the background.
    const bool focused = game_has_focus(game_window());
    float wheel = 0.0f;
    {
        if (!R.mouse_singleton) R.mouse_singleton = API::get()->get_native_singleton("via.hid.Mouse");   // null at plugin load
        MO* dev = merged_device(R.mouse_singleton, R.mouse_get_merged);
        static int s_told = 0;
        if (!dev && s_told == 0) { s_told = 1; cmn::journal("[camera] no mouse device (singleton %p, method %p)", R.mouse_singleton, (void*)R.mouse_get_merged); }
        if (dev) {
            const float w = mouse_wheel_raw(dev);
            if (focused && g_wheel_init) {
                float dd = w - g_wheel_last;
                if (std::fabs(dd) >= kWheelNotchDivisor) dd = dd / kWheelNotchDivisor;
                wheel = clampf(dd, -kWheelClamp, kWheelClamp);
            }
            g_wheel_last = w;
            g_wheel_init = true;
        }
    }
    const bool page_up = key_down(kVkPageUp), page_down = key_down(kVkPageDown);
    if (page_up) wheel += kPageKeyZoomStep; else if (page_down) wheel -= kPageKeyZoomStep;
    if (wheel != 0.0f && (right_of_dialog || page_up || page_down)) preview_zoom(wheel);

    // mouse: middle drag = pan
    const bool mid = have_mouse && right_of_dialog && key_down(kVkMButton);
    if (mid && g_dragging_pan) preview_pan((mx - g_pan_last_x) / sx, (my - g_pan_last_y) / sx);
    g_dragging_pan = mid;
    if (mid) { g_pan_last_x = mx; g_pan_last_y = my; }

    // pad: right stick = turn, L3 + right stick = pan, R2/L2 = zoom
    {
        if (!R.pad_singleton) R.pad_singleton = API::get()->get_native_singleton("via.hid.GamePad");   // null at plugin load
        MO* dev = merged_device(R.pad_singleton, R.pad_get_merged);
        if (dev) {
            float ax = 0, ay = 0;
            const bool have_axis = pad_axis_r(dev, ax, ay);
            const uint32_t mask = pad_button_mask(dev);
            if (mask & g_pad_l3) {
                if (have_axis && (std::fabs(ax) > kPadStickDeadzone || std::fabs(ay) > kPadStickDeadzone))
                    preview_pan(ax * kPanSpeedPad, -ay * kPanSpeedPad);
            } else if (have_axis && std::fabs(ax) > kPadStickDeadzone) {
                preview_turn(ax * kTurnSpeedPad);
            }
            if (mask & g_pad_r2) preview_zoom(kZoomSpeedPad);
            else if (mask & g_pad_l2) preview_zoom(-kZoomSpeedPad);
        }
    }
}

// ---- camera hints (SF6_ColorSpinExtra.lua:3629-3691 PV.camera_hints) ------------------------
// Painted through the core display API, same pattern as screens::tracker's (unrelated) push_camera.

struct HintIcon { const char* ctrl_name; std::string text; float x; float y; };

void paint_one_block(const SF6CoreAPI* api_p, void* cm, bool show, const char* block_name,
                      const std::string& title, float title_x, float title_y,
                      const std::vector<HintIcon>& icons, bool hide_textures,
                      float block_x, float block_y, float ref_w, float ref_h) {
    void* blk = api_p->ui_find_cached(cm, block_name);
    if (!blk) return;
    auto* blk_mo = reinterpret_cast<MO*>(blk);

    // Method lookups cached across calls (optional review fix): via.gui's Child/Next/Visible walk is
    // the same handful of methods every paint, only the instances differ.
    static API::Method* s_get_child = nullptr;
    static API::Method* s_get_next = nullptr;
    static API::Method* s_get_visible = nullptr;

    if (hide_textures) {                                        // the camera-Y block's arrows (:3634-3636)
        if (!s_get_child) s_get_child = ll::find_method_noargs(blk_mo->get_type_definition(), "get_Child");
        if (s_get_child) {
            auto cr = s_get_child->invoke(blk_mo, std::vector<void*>{});
            void* c = cr.exception_thrown ? nullptr : cr.ptr;
            while (c) {
                auto* c_mo = reinterpret_cast<MO*>(c);
                if (!s_get_visible) s_get_visible = ll::find_method_noargs(c_mo->get_type_definition(), "get_Visible");
                if (s_get_visible && std::string(c_mo->get_type_definition()->get_full_name()) == "via.gui.Texture") {
                    auto vr = s_get_visible->invoke(c_mo, std::vector<void*>{});
                    if (!vr.exception_thrown && vr.dword) api_p->ui_set_visible(cl(), c, 0);
                }
                if (!s_get_next) s_get_next = ll::find_method_noargs(c_mo->get_type_definition(), "get_Next");
                if (!s_get_next) break;
                auto nr = s_get_next->invoke(c_mo, std::vector<void*>{});
                c = nr.exception_thrown ? nullptr : nr.ptr;
            }
        }
    }

    if (!show) {
        if (!s_get_visible) s_get_visible = ll::find_method_noargs(blk_mo->get_type_definition(), "get_Visible");
        if (s_get_visible) {
            auto vr = s_get_visible->invoke(blk_mo, std::vector<void*>{});
            if (!vr.exception_thrown && vr.dword) api_p->ui_set_visible(cl(), blk, 0);
        }
        return;
    }

    void* title_ctrl = api_p->ui_find_cached(blk, "e_text_title0");
    if (!title_ctrl) title_ctrl = api_p->ui_find_cached(blk, "e_text_title");
    if (title_ctrl) {
        api_p->ui_set_text(cl(), title_ctrl, title.c_str());
        api_p->ui_set_position(cl(), title_ctrl, title_x, title_y, 0);
    }

    for (auto& ic : icons) {
        void* panel = api_p->ui_find_cached(blk, ic.ctrl_name);
        if (!panel) continue;
        api_p->ui_set_visible(cl(), panel, 1);
        api_p->ui_set_position(cl(), panel, ic.x, ic.y, 0);
        void* t = api_p->ui_find_cached(panel, "e_text_0");
        if (t) {
            api_p->ui_set_text(cl(), t, ic.text.c_str());
            if (ref_w > 0 || ref_h > 0) api_p->ui_set_font_size(cl(), t, ref_w, ref_h);   // font 0 in both blocks: always the reference size
        }
    }

    api_p->ui_set_position(cl(), blk, block_x, block_y, 0);
    api_p->ui_set_visible(cl(), blk, 1);
}

// agent_addr is passed in explicitly (not read from g_agent_addr) so hide_hints() can hide the
// blocks using the screen's CURRENT agent even when the camera/editor was never opened this session
// (review S2): the Lua's own PV.camera_hints(false) only needs fsm.agent, independent of edit.active.
void paint_hints(bool show, uint64_t agent_addr) {
    if (!agent_addr) return;
    auto* api_p = api();
    if (!api_p) return;
    MO* agent = reinterpret_cast<MO*>(agent_addr);
    if (!is_managed(agent)) return;
    auto* get_cm = ll::find_method_noargs(agent->get_type_definition(), "get_ControlMain");
    if (!get_cm) return;
    MO* cm = ll::invoke_object_ret(agent, get_cm, {});
    if (!cm) return;

    // reference glyph size = the Rotation block's icon text (c_01, game-owned) (SF6_ColorSpinExtra.lua:3647-3652)
    float ref_w = 0, ref_h = 0;
    {
        void* c01 = api_p->ui_find_cached(cm, "c_01");
        void* ref_t = c01 ? api_p->ui_find_cached(c01, "e_text_0") : nullptr;
        if (ref_t) {
            auto* fs_m = ll::find_method_noargs(reinterpret_cast<MO*>(ref_t)->get_type_definition(), "get_FontSize");
            if (fs_m) {
                auto fr = fs_m->invoke(reinterpret_cast<MO*>(ref_t), std::vector<void*>{});
                if (!fr.exception_thrown) { std::memcpy(&ref_w, fr.bytes.data(), 4); std::memcpy(&ref_h, fr.bytes.data() + 4, 4); }
            }
        }
    }

    // move block ("c_00"): BtnL3+StickR / MouseM, one icon, hides the block's own texture arrows
    {
        const std::string move_text = g_pad ? (std::string("<ICON BtnL3>") + "\xEF\xBC\x8B" + "<ICON StickR>")  // "+" = U+FF0B fullwidth plus, SF6_ColorSpinExtra.lua:1692
                                             : std::string("<ICON MouseM>");
        std::vector<HintIcon> icons{ { kMoveIconCtrl, move_text, g_pad ? kMovePadIconX : kMoveIconX, g_pad ? kMovePadIconY : kMoveIconY } };
        paint_one_block(api_p, cm, show, kMoveBlock, g_title_move, kMoveTitleX, kMoveTitleY, icons, /*hide_textures=*/true,
                         kMoveBlockX, kMoveBlockY, ref_w, ref_h);
    }
    // zoom block ("c_02"): BtnL2/BtnR2 or MouseWD/MouseWU, two icons
    {
        std::vector<HintIcon> icons{
            { kZoomIcon1Ctrl, g_pad ? "<ICON BtnL2>" : "<ICON MouseWD>", g_pad ? kZoomPadIcon1X : kZoomIcon1X, g_pad ? kZoomPadIconY : kZoomIconY },
            { kZoomIcon2Ctrl, g_pad ? "<ICON BtnR2>" : "<ICON MouseWU>", g_pad ? kZoomPadIcon2X : kZoomIcon2X, g_pad ? kZoomPadIconY : kZoomIconY },
        };
        paint_one_block(api_p, cm, show, kZoomBlock, g_title_zoom, kZoomTitleX, kZoomTitleY, icons, /*hide_textures=*/false,
                         kZoomBlockX, kZoomBlockY, ref_w, ref_h);
    }
    ++g_hint_paints;
}

// ---- tick (called from on_frame) -----------------------------------------

void tick_all() {
    std::lock_guard lk(g_mx);
    if (!g_active) return;
    preview_spin_tick();
    if (cmn::frame() % kHintsCadenceFrames == 0) paint_hints(true, g_agent_addr);
    ++g_ticks;
}

}  // namespace

// ---- public API ----------------------------------------------------------

bool install() {
    if (!api() || !SF6CORE_HAS(api(), ui_set_text) || !SF6CORE_HAS(api(), ui_set_position) ||
        !SF6CORE_HAS(api(), ui_set_visible) || !SF6CORE_HAS(api(), ui_set_font_size) ||
        !SF6CORE_HAS(api(), ui_find_cached)) {
        cmn::log_warn("[camera] core without required display APIs: off");
        return false;
    }
    auto* tdb = API::get()->tdb();
    R.spin_td = tdb->find_type("app.FighterSpinCtrl");
    auto* scene_mgr_td = tdb->find_type("via.SceneManager");
    auto* scene_td = tdb->find_type("via.Scene");
    auto* go_td = tdb->find_type("via.GameObject");
    auto* tr_td = tdb->find_type("via.Transform");
    auto* arr_td = tdb->find_type("System.Array");
    auto* mouse_td = tdb->find_type("via.hid.Mouse");
    auto* pad_td = tdb->find_type("via.hid.GamePad");

    if (R.spin_td) R.spin_get_go = ll::find_method_noargs(R.spin_td, "get_GameObject");
    if (scene_mgr_td) R.scene_get_current = ll::find_method_noargs(scene_mgr_td, "get_CurrentScene");
    if (scene_td) R.scene_find_components = ll::find_method_full(scene_td, "findComponents(System.Type)");
    if (arr_td) { R.arr_len = arr_td->find_method("GetLength"); R.arr_get = arr_td->find_method("GetValue(System.Int32)"); }
    if (go_td) {
        R.go_get_tr = ll::find_method_noargs(go_td, "get_Transform");
        R.go_get_name = ll::find_method_noargs(go_td, "get_Name");
    }
    if (tr_td) {
        R.tr_get_go = ll::find_method_noargs(tr_td, "get_GameObject");
        R.tr_get_child = ll::find_method_noargs(tr_td, "get_Child");
        R.tr_get_next = ll::find_method_noargs(tr_td, "get_Next");
        R.tr_get_local_euler = ll::find_method_noargs(tr_td, "get_LocalEulerAngle");
        R.tr_set_local_euler = tr_td->find_method("set_LocalEulerAngle(via.vec3)");
        R.tr_get_local_pos = ll::find_method_noargs(tr_td, "get_LocalPosition");
        R.tr_set_local_pos = tr_td->find_method("set_LocalPosition(via.vec3)");
    }
    R.mouse_singleton = API::get()->get_native_singleton("via.hid.Mouse");
    R.pad_singleton = API::get()->get_native_singleton("via.hid.GamePad");
    if (mouse_td) R.mouse_get_merged = ll::find_method_noargs(mouse_td, "get_MergedDevice");
    if (pad_td) R.pad_get_merged = ll::find_method_noargs(pad_td, "get_MergedDevice");

    // Review fix (S3): resolve the pad bitmasks from the live via.hid.GamePadButton enum, same fields
    // the Lua reads (LStickPush/RTrigBottom/LTrigBottom, SF6_ColorSpinExtra.lua:3365-3369); keep the
    // Lua's own initial numbers as the fallback when the enum or a field is not found, and journal
    // which source was used (enum vs fallback) for each.
    {
        auto* pad_button_td = tdb->find_type("via.hid.GamePadButton");
        auto resolve_pad_bit = [&](const char* field_name, uint32_t fallback, const char* label) -> uint32_t {
            auto* f = pad_button_td ? pad_button_td->find_field(field_name) : nullptr;
            if (!f) {
                cmn::journal("[camera] pad bit %s: via.hid.GamePadButton.%s not found, using fallback %u", label, field_name, fallback);
                return fallback;
            }
            const uint32_t v = (uint32_t)f->get_data<int32_t>(nullptr);
            cmn::journal("[camera] pad bit %s: resolved %s = %u from the live enum", label, field_name, v);
            return v;
        };
        g_pad_l3 = resolve_pad_bit("LStickPush", kPadL3Fallback, "L3");
        g_pad_r2 = resolve_pad_bit("RTrigBottom", kPadR2Fallback, "R2");
        g_pad_l2 = resolve_pad_bit("LTrigBottom", kPadL2Fallback, "L2");
    }

    if (!R.spin_td || !R.spin_get_go || !R.scene_get_current || !R.scene_find_components ||
        !R.arr_len || !R.arr_get || !R.go_get_tr || !R.go_get_name ||
        !R.tr_get_go || !R.tr_get_child || !R.tr_get_next ||
        !R.tr_get_local_euler || !R.tr_set_local_euler || !R.tr_get_local_pos || !R.tr_set_local_pos) {
        cmn::log_warn("[camera] reflection resolution incomplete: off");
        return false;
    }
    // the pad / mouse singletons and get_MergedDevice are best-effort: their absence only disables
    // that input source (wheel / pad), same graceful degradation the Lua's own pcall wrapping gives.
    g_on = true;
    cmn::journal("[camera] installed");
    return true;
}

bool ready() { return g_on; }

void open(uint64_t agent_addr, bool pad, const std::string& title_move, const std::string& title_zoom) {
    std::lock_guard lk(g_mx);
    g_active = true;
    g_agent_addr = agent_addr;
    g_pad = pad;
    g_title_move = title_move;
    g_title_zoom = title_zoom;
}

void close() {
    std::lock_guard lk(g_mx);
    if (g_active) {
        restore_orig();          // SF6_ColorSpinExtra.lua:2177-2179 (editor_teardown)
        paint_hints(false, g_agent_addr);
        g_dragging_turn = false; // SF6_ColorSpinExtra.lua:4105 (the else branch nils spin_last_x)
    }
    g_active = false;
    g_agent_addr = 0;
}

// Review fix (S2): periodic re-hide only (no camera restore), called by the Lua every
// kHintsRehideSafetyFrames (30) frames while Fighter Settings is visible and the hints are not
// currently shown -- same condition/cadence as the Lua's own "elseif edit.hints_shown or (fsm.visible
// and dbg.frame % 30 == 0)" (SF6_ColorSpinExtra.lua:4110), split out so it does not re-run
// restore_orig() every 30 frames the way close() would.
void hide_hints(uint64_t agent_addr) {
    std::lock_guard lk(g_mx);
    paint_hints(false, agent_addr);
}

void on_frame() {
    if (!g_on) return;
    __try { tick_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("tick"); }
}

std::string status() {
    std::lock_guard lk(g_mx);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "camera: on=%d faults=%d active=%d spin=%s cam=%s moved=%d ticks=%llu hints=%llu",
                  g_on ? 1 : 0, g_faults, g_active ? 1 : 0,
                  g_spin_ctrl ? "yes" : "no", g_cam ? "yes" : "no", g_has_orig ? 1 : 0,
                  (unsigned long long)g_ticks, (unsigned long long)g_hint_paints);
    return buf;
}

}  // namespace dialogs::camera
