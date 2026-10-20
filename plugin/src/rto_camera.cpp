// SF6 Slots — runtime costume options: the preview camera while the options menu is open (rto.hpp).
//
// The same character manipulation as the Color Mod's colour editor on Fighter Settings (its
// colormod/src/dialogs/camera.cpp, ported here with the same constants, limits and input; the Color Mod is
// not changed): turn / zoom / pan of the preview (app.FighterSpinCtrl's "Rot" transform and its
// ess_DefaultCamera child), mouse left drag = turn, middle drag = pan, wheel / PageUp / PageDown = zoom (mouse
// only right of the docked dialog), pad right stick = turn, L3 + right stick = pan, R2 / L2 = zoom; the "Move"
// and "Zoom" hint blocks of Fighter Settings (c_00, c_02) shown with the game's glyphs of the device in use,
// titles from the shared locale file (reframework/data/SF6_NativeLocale_data/locale.json, the game's text
// language; English when the file or the key is missing). On close the camera position goes back to what it was
// before the first zoom / pan (the turn stays, as in the editor) and the two blocks are hidden.
//
// Only while our menu is open (rto_menu.cpp drives it): the colour editor and this menu never open together
// (both need the option window closed).
//
// Hint blocks: the Color Mod hides c_00 / c_02 every 30 of its frames while Fighter Settings is up and its own
// editor is closed (so also while our menu is open), from the core's LateUpdateBehavior callback. Ours paint
// from a post-LateUpdateBehavior callback (main.cpp), after every pre callback of the frame: a block it hid is
// shown again before the frame is drawn.
//
// Rules (slots.hpp): game thread only; every kept object checked alive before use; the GUI written through the
// core (compare-before-write); input read natively (GetAsyncKeyState, the game window's cursor only while it
// has the focus, via.hid.Mouse / via.hid.GamePad merged devices), never hooked or swallowed.
#include "rto.hpp"
#include "slots_ui.hpp"

#pragma comment(lib, "user32.lib")   // EnumWindows / GetCursorPos / ScreenToClient / GetAsyncKeyState

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace slots::rto::camera {
namespace {

// ---- constants (the colour editor's, camera.cpp) ----
constexpr int kMaxFaults = 5;

constexpr float kZoomNear = 14.1f;
constexpr float kZoomFar = 0.0f;
constexpr float kPanSpeedFar = 0.005f;
constexpr float kZoomExpo = 2.5f;
constexpr float kZoomNotchStep = 0.025f;
constexpr float kPanClampX = 4.0f;
constexpr float kPanClampYNeg = 3.0f;
constexpr float kPanClampYPos = 4.0f;

constexpr int kVkLButton = 0x01;              // left drag = turn
constexpr int kVkMButton = 0x04;              // middle drag = pan
constexpr int kVkPageUp = 0x21;
constexpr int kVkPageDown = 0x22;
constexpr float kMouseTurnSpeed = 0.5f;
constexpr double kReferenceWidth = 1920.0;
constexpr double kRightOfDialogXRef = 1040.0; // the mouse acts right of the docked dialog only
constexpr float kWheelNotchDivisor = 120.0f;
constexpr float kWheelClamp = 2.0f;
constexpr float kPageKeyZoomStep = 0.25f;

constexpr uint32_t kPadL3Fallback = 4096;     // via.hid.GamePadButton read at the first open, these if absent
constexpr uint32_t kPadR2Fallback = 2048;
constexpr uint32_t kPadL2Fallback = 512;
constexpr float kPadStickDeadzone = 0.2f;
constexpr float kPanSpeedPad = 12.0f;
constexpr float kTurnSpeedPad = 3.0f;
constexpr float kZoomSpeedPad = 0.4f;

constexpr uint32_t kHintsCadenceFrames = 5;

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

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// ---- state ----
struct Resolved {
    bool tried = false, ok = false;
    TD* spin_td = nullptr;                        // app.FighterSpinCtrl
    Method *spin_get_go = nullptr, *go_get_tr = nullptr, *go_get_name = nullptr;
    Method *tr_get_go = nullptr, *tr_get_child = nullptr, *tr_get_next = nullptr;
    Method *tr_get_euler = nullptr, *tr_set_euler = nullptr, *tr_get_pos = nullptr, *tr_set_pos = nullptr;
    Method *mouse_merged = nullptr, *pad_merged = nullptr;
    Method *wheel = nullptr, *axis_r = nullptr, *button = nullptr;   // on the merged devices' types
} R;

bool g_off = false;                       // faults
int g_faults = 0;
bool g_active = false;
uint32_t g_ticked = 0;                    // the last frame tick() ran (post_frame paints only right after one)
Obj* g_agent = nullptr;                   // Fighter Settings' agent (hint blocks)
bool g_pad = false;
std::string g_title_move = "Move", g_title_zoom = "Zoom";

Obj* g_spin = nullptr;
Obj* g_cam = nullptr;
bool g_has_orig = false;
Obj* g_orig_cam = nullptr;
float g_orig[3] = {};

bool g_drag_turn = false;
float g_spin_last_x = 0;
bool g_drag_pan = false;
float g_pan_last_x = 0, g_pan_last_y = 0;
bool g_wheel_init = false;
float g_wheel_last = 0;
bool g_have_mouse = false;
float g_mouse_x = 0, g_mouse_y = 0;

uint32_t g_pad_l3 = kPadL3Fallback, g_pad_r2 = kPadR2Fallback, g_pad_l2 = kPadL2Fallback;
uint32_t g_turns = 0, g_zooms = 0, g_pans = 0, g_paints = 0;   // log at close

void fault(const char* where) {
    ++g_faults;
    logf("rto: camera: EXCEPTION in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) { g_off = true; g_active = false; logf("rto: camera: 5 faults, off for this session"); }
}

bool resolve() {
    if (R.tried) return R.ok;
    R.tried = true;
    auto* tdb = api().tdb();
    R.spin_td = tdb->find_type("app.FighterSpinCtrl");
    TD* go = tdb->find_type("via.GameObject");
    TD* tr = tdb->find_type("via.Transform");
    R.spin_get_go = method_of(R.spin_td, "get_GameObject", 0);
    R.go_get_tr = method_of(go, "get_Transform", 0);
    R.go_get_name = method_of(go, "get_Name", 0);
    R.tr_get_go = method_of(tr, "get_GameObject", 0);
    R.tr_get_child = method_of(tr, "get_Child", 0);
    R.tr_get_next = method_of(tr, "get_Next", 0);
    R.tr_get_euler = method_of(tr, "get_LocalEulerAngle", 0);
    R.tr_set_euler = tr ? tr->find_method("set_LocalEulerAngle(via.vec3)") : nullptr;
    R.tr_get_pos = method_of(tr, "get_LocalPosition", 0);
    R.tr_set_pos = tr ? tr->find_method("set_LocalPosition(via.vec3)") : nullptr;
    R.mouse_merged = method_of(tdb->find_type("via.hid.Mouse"), "get_MergedDevice", 0);
    R.pad_merged = method_of(tdb->find_type("via.hid.GamePad"), "get_MergedDevice", 0);
    TD* btn = tdb->find_type("via.hid.GamePadButton");
    auto bit = [&](const char* field, uint32_t fallback) -> uint32_t {
        auto* f = btn ? btn->find_field(field) : nullptr;
        return f ? static_cast<uint32_t>(f->get_data<int32_t>(nullptr)) : fallback;
    };
    g_pad_l3 = bit("LStickPush", kPadL3Fallback);
    g_pad_r2 = bit("RTrigBottom", kPadR2Fallback);
    g_pad_l2 = bit("LTrigBottom", kPadL2Fallback);
    const SF6CoreAPI* a = A();
    R.ok = R.spin_td && R.spin_get_go && R.go_get_tr && R.go_get_name && R.tr_get_go && R.tr_get_child &&
           R.tr_get_next && R.tr_get_euler && R.tr_set_euler && R.tr_get_pos && R.tr_set_pos &&
           a && SF6CORE_HAS(a, ui_find_cached) && SF6CORE_HAS(a, ui_set_text) && SF6CORE_HAS(a, ui_set_position) &&
           SF6CORE_HAS(a, ui_set_visible) && SF6CORE_HAS(a, ui_set_font_size);
    logf("rto: camera %s (pad bits L3 %u R2 %u L2 %u)", R.ok ? "ready" : "off: a game method or a core display function is missing",
         g_pad_l3, g_pad_r2, g_pad_l2);
    return R.ok;
}

// ---- hint titles: the shared locale file, the game's text language (the Color Mod's dialogs/locale.cpp) ----
const char* kLocale = "reframework/data/SF6_NativeLocale_data/locale.json";
const char* kLocaleLegacy = "reframework/data/SF6_ColorSpinExtra_data/locale.json";

const char* lang_code(int32_t v) {
    switch (v) {
    case 0: return "ja"; case 1: return "en"; case 2: return "fr"; case 3: return "it"; case 4: return "de";
    case 5: return "es"; case 6: return "ru"; case 7: return "pl"; case 8: return "nl"; case 9: case 10: return "pt";
    case 11: return "ko"; case 12: return "zh-Hant"; case 13: return "zh-Hans"; case 21: return "ar"; case 32: return "es-419";
    default: return nullptr;
    }
}
const char* lang_near(const std::string& c) {
    if (c == "es-419") return "es";
    if (c == "zh-Hant") return "zh-Hans";
    if (c == "pt") return "es";
    return nullptr;
}

std::string game_language() {
    auto* tdb = api().tdb();
    int32_t val = 0;
    bool got = false;
    Method* opt_get = tdb->find_method("app.Option", "GetOptionValue");
    API::Field* list_f = tdb->find_field("app.Option", "LanguageList");
    if (opt_get && list_f) {
        auto ri = opt_get->invoke(nullptr, std::vector<void*>{ arg_i32(611) });           // app.Option.ValueType.DispLanguage
        void* slot = ri.exception_thrown ? nullptr : list_f->get_data_raw(nullptr, false);
        Obj* arr = slot ? *reinterpret_cast<Obj**>(slot) : nullptr;
        const int32_t idx = static_cast<int32_t>(ri.dword);
        if (arr && idx >= 0) {
            TD* td = arr->get_type_definition();
            auto rl = call(method_of(td, "get_Length", 0), arr);
            const int32_t n = rl.exception_thrown ? 0 : static_cast<int32_t>(rl.dword);
            if (idx < n) {
                auto rv = call(method_of(td, "get_Item(System.Int32)", 1), arr, { arg_i32(idx) });
                if (!rv.exception_thrown) { val = static_cast<int32_t>(rv.dword); got = true; }
            }
        }
    }
    if (!got) {
        if (Method* sys = tdb->find_method("via.SystemService", "get_Language")) {
            auto r = sys->invoke(nullptr, std::vector<void*>{});
            if (!r.exception_thrown) { val = static_cast<int32_t>(r.dword); got = true; }
        }
    }
    const char* c = got ? lang_code(val) : nullptr;
    return c ? c : "en";
}

void load_titles() {
    g_title_move = "Move"; g_title_zoom = "Zoom";
    std::string text = read_file(kLocale);
    Json root;
    if (text.empty() || !json_parse(text, root) || root.kind != Json::Object) {
        text = read_file(kLocaleLegacy);
        if (text.empty() || !json_parse(text, root) || root.kind != Json::Object) return;
    }
    const std::string lang = game_language();
    auto lookup = [&](const char* key, std::string& out) {
        const char* close_lang = lang_near(lang);
        for (const char* code : { lang.c_str(), close_lang, "en" }) {
            const Json* t = code ? root.get(code) : nullptr;
            const Json* v = t && t->kind == Json::Object ? t->get(key) : nullptr;
            if (v && v->kind == Json::String) { out = v->s; return; }
        }
    };
    lookup("hint.move", g_title_move);
    lookup("hint.zoom", g_title_zoom);
}

// ---- Win32 input: the game window (largest visible window of this process), cursor only while focused ----
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
        if (!c->best || w * h > c->best_area) { c->best = hwnd; c->best_area = w * h; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    cached = ctx.best;
    return cached;
}
bool game_has_focus(HWND hw) { return hw && GetForegroundWindow() == hw; }

bool mouse_state(float& x, float& y, float& dw, float& dh) {
    HWND hw = game_window();
    if (!hw) return false;
    RECT rc{};
    if (!GetClientRect(hw, &rc)) return false;
    dw = static_cast<float>(rc.right - rc.left);
    dh = static_cast<float>(rc.bottom - rc.top);
    if (dw <= 0 || dh <= 0) return false;
    if (game_has_focus(hw)) {                     // frozen while another window has the focus: no drag from there
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(hw, &p)) { g_mouse_x = static_cast<float>(p.x); g_mouse_y = static_cast<float>(p.y); g_have_mouse = true; }
    }
    if (!g_have_mouse) return false;
    x = g_mouse_x; y = g_mouse_y;
    return true;
}

Obj* merged(const char* singleton, Method* get_merged) {
    void* s = api().get_native_singleton(singleton);
    return (s && get_merged) ? call_obj(get_merged, reinterpret_cast<Obj*>(s)) : nullptr;
}
float wheel_raw(Obj* dev) {                       // System.Single, handed back as a double
    if (!R.wheel) R.wheel = method_of(dev->get_type_definition(), "get_Wheel", 0);
    auto r = call(R.wheel, dev);
    return r.exception_thrown ? 0.0f : static_cast<float>(r.d);
}
bool axis_r(Obj* dev, float& x, float& y) {
    if (!R.axis_r) R.axis_r = method_of(dev->get_type_definition(), "get_AxisR", 0);
    auto r = call(R.axis_r, dev);
    if (r.exception_thrown) return false;
    std::memcpy(&x, r.bytes.data(), 4);
    std::memcpy(&y, r.bytes.data() + 4, 4);
    return true;
}
uint32_t buttons(Obj* dev) {
    if (!R.button) R.button = method_of(dev->get_type_definition(), "get_Button", 0);
    auto r = call(R.button, dev);
    return r.exception_thrown ? 0u : r.dword;
}

// ---- via.vec3 (a pointer argument for a value type over 8 bytes) ----
bool get_vec3(Method* m, Obj* o, float out[3]) {
    auto r = call(m, o);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), 12);
    return true;
}
bool set_vec3(Method* m, Obj* o, float x, float y, float z) {
    std::array<uint8_t, 16> buf{};
    std::memcpy(buf.data(), &x, 4);
    std::memcpy(buf.data() + 4, &y, 4);
    std::memcpy(buf.data() + 8, &z, 4);
    return !call(m, o, { buf.data() }).exception_thrown;
}

// ---- the preview's transforms ----
std::string name_of_tr(Obj* tr) {
    Obj* go = call_obj(R.tr_get_go, tr);
    return go ? str(call_obj(R.go_get_name, go)) : std::string();
}
Obj* direct_child(Obj* parent, const char* name) {
    for (Obj* ch = call_obj(R.tr_get_child, parent); ch; ch = call_obj(R.tr_get_next, ch))
        if (name_of_tr(ch) == name) return ch;
    return nullptr;
}
Obj* descendant(Obj* tr, const char* name, int depth) {     // self + descendants, depth <= 6
    if (!tr || depth > 6) return nullptr;
    if (name_of_tr(tr) == name) return tr;
    for (Obj* ch = call_obj(R.tr_get_child, tr); ch; ch = call_obj(R.tr_get_next, ch))
        if (Obj* f = descendant(ch, name, depth + 1)) return f;
    return nullptr;
}
Obj* spin_tr(Obj* spin) {
    Obj* go = call_obj(R.spin_get_go, spin);
    return go ? call_obj(R.go_get_tr, go) : nullptr;
}
Obj* find_spin() {                                // every FighterSpinCtrl of the scene: the last with a "Rot" child
    g_spin = nullptr;
    Obj* comps = find_components(R.spin_td);
    for (int i = 0, n = list_count(comps); i < n; ++i) {
        Obj* c = list_item(comps, i);
        Obj* tr = c ? spin_tr(c) : nullptr;
        if (tr && direct_child(tr, "Rot")) g_spin = c;
    }
    return g_spin;
}
Obj* spin() { return alive(g_spin) ? g_spin : find_spin(); }
Obj* cam() {
    if (alive(g_cam)) return g_cam;
    g_cam = nullptr;
    Obj* s = spin();
    Obj* tr = s ? spin_tr(s) : nullptr;
    g_cam = tr ? descendant(tr, "ess_DefaultCamera", 0) : nullptr;
    return g_cam;
}

// the position before the FIRST zoom / pan of the session (a session without one restores nothing)
void remember_orig(Obj* c, const float p[3]) {
    if (!g_has_orig) { g_has_orig = true; g_orig_cam = c; std::memcpy(g_orig, p, sizeof g_orig); }
}
void restore_orig() {
    if (g_has_orig) {
        if (alive(g_orig_cam)) set_vec3(R.tr_set_pos, g_orig_cam, g_orig[0], g_orig[1], g_orig[2]);
        g_has_orig = false;
        g_orig_cam = nullptr;
    }
    g_cam = nullptr;
}

void turn(float delta_deg) {
    Obj* s = spin();
    if (!s || delta_deg == 0.0f) return;
    Obj* tr = spin_tr(s);
    Obj* rot = tr ? direct_child(tr, "Rot") : nullptr;
    float e[3];
    if (!rot || !get_vec3(R.tr_get_euler, rot, e)) return;
    const float yaw = rad2deg(e[1]) - delta_deg;
    set_vec3(R.tr_set_euler, rot, e[0], deg2rad(yaw), e[2]);
    if (void* p = field_ptr(s, "mAngle")) std::memcpy(p, &yaw, sizeof yaw);
    ++g_turns;
}

void zoom(float notches) {                        // exponential easing, ~40 notches end to end
    if (notches == 0.0f) return;
    Obj* c = cam();
    float p[3];
    if (!c || !get_vec3(R.tr_get_pos, c, p)) return;
    remember_orig(c, p);
    const float z_far = -kZoomFar, z_near = -kZoomNear;           // more negative = closer
    const float u = clamp01((p[2] - z_far) / (z_near - z_far));
    float t = 1.0f - std::pow(1.0f - u, 1.0f / kZoomExpo);
    t = clamp01(t + notches * kZoomNotchStep);
    const float z = z_far + (z_near - z_far) * (1.0f - std::pow(1.0f - t, kZoomExpo));
    set_vec3(R.tr_set_pos, c, p[0], p[1], z);
    ++g_zooms;
}

void pan(float dx_px, float dy_px) {              // 1080p-reference pixels, slower when close
    if (dx_px == 0.0f && dy_px == 0.0f) return;
    Obj* c = cam();
    float p[3];
    if (!c || !get_vec3(R.tr_get_pos, c, p)) return;
    remember_orig(c, p);
    const float span = std::max(0.1f, kZoomNear - kZoomFar);
    const float closeness = clamp01((std::fabs(p[2]) - kZoomFar) / span);
    const float k = kPanSpeedFar * (1.0f - 0.95f * closeness);
    const float x = clampf(p[0] - dx_px * k, -kPanClampX, kPanClampX);
    const float y = clampf(p[1] + dy_px * k, -kPanClampYNeg, kPanClampYPos);
    set_vec3(R.tr_set_pos, c, x, y, p[2]);
    ++g_pans;
}

void input_tick() {
    float mx = 0, my = 0, dw = 0, dh = 0;
    const bool have_mouse = mouse_state(mx, my, dw, dh);
    const float sx = have_mouse ? static_cast<float>(dw / kReferenceWidth) : 1.0f;
    const bool right_of_dialog = have_mouse && (mx > static_cast<float>(kRightOfDialogXRef * sx));

    // mouse: left drag = turn
    const bool down = have_mouse && right_of_dialog && key_down(kVkLButton);
    if (down && !g_drag_turn) find_spin();       // drag start: fresh lookup (an outfit change rebuilds the model)
    if (down && g_drag_turn) turn((mx - g_spin_last_x) * kMouseTurnSpeed / sx);
    g_drag_turn = down;
    if (down) g_spin_last_x = mx;

    // mouse: wheel (or PageUp / PageDown) = zoom; no wheel delta while the game is not in front
    const bool focused = game_has_focus(game_window());
    float wheel = 0.0f;
    if (Obj* dev = merged("via.hid.Mouse", R.mouse_merged)) {
        const float w = wheel_raw(dev);
        if (focused && g_wheel_init) {
            float dd = w - g_wheel_last;
            if (std::fabs(dd) >= kWheelNotchDivisor) dd = dd / kWheelNotchDivisor;
            wheel = clampf(dd, -kWheelClamp, kWheelClamp);
        }
        g_wheel_last = w;
        g_wheel_init = true;
    }
    const bool page_up = key_down(kVkPageUp), page_down = key_down(kVkPageDown);
    if (page_up) wheel += kPageKeyZoomStep; else if (page_down) wheel -= kPageKeyZoomStep;
    if (wheel != 0.0f && (right_of_dialog || page_up || page_down)) zoom(wheel);

    // mouse: middle drag = pan
    const bool mid = have_mouse && right_of_dialog && key_down(kVkMButton);
    if (mid && g_drag_pan) pan((mx - g_pan_last_x) / sx, (my - g_pan_last_y) / sx);
    g_drag_pan = mid;
    if (mid) { g_pan_last_x = mx; g_pan_last_y = my; }

    // pad: right stick = turn, L3 + right stick = pan, R2 / L2 = zoom
    if (Obj* dev = merged("via.hid.GamePad", R.pad_merged)) {
        float ax = 0, ay = 0;
        const bool have_axis = axis_r(dev, ax, ay);
        const uint32_t mask = buttons(dev);
        if (mask & g_pad_l3) {
            if (have_axis && (std::fabs(ax) > kPadStickDeadzone || std::fabs(ay) > kPadStickDeadzone))
                pan(ax * kPanSpeedPad, -ay * kPanSpeedPad);
        } else if (have_axis && std::fabs(ax) > kPadStickDeadzone) {
            turn(ax * kTurnSpeedPad);
        }
        if (mask & g_pad_r2) zoom(kZoomSpeedPad);
        else if (mask & g_pad_l2) zoom(-kZoomSpeedPad);
    }
}

// ---- the hint blocks (Fighter Settings' c_00 "Move", c_02 "Zoom") ----
struct Icon { const char* ctrl; std::string text; float x, y; };

bool visible(Obj* o) { bool ok = false; const bool v = call_bool(o, "get_Visible", {}, &ok); return ok && v; }

void paint_block(const SF6CoreAPI* a, Obj* cm, bool show, const char* block, const std::string& title, float tx, float ty,
                 const std::vector<Icon>& icons, bool hide_textures, float bx, float by, float ref_w, float ref_h) {
    Obj* blk = reinterpret_cast<Obj*>(a->ui_find_cached(cm, block));
    if (!blk) return;
    if (hide_textures)                            // the camera-Y block's own arrows
        for (Obj* c = call_obj(blk, "get_Child"); c; c = call_obj(c, "get_Next"))
            if (type_name(c) == "via.gui.Texture" && visible(c)) a->ui_set_visible(C(), c, 0);
    if (!show) {
        if (visible(blk)) a->ui_set_visible(C(), blk, 0);
        return;
    }
    Obj* t = reinterpret_cast<Obj*>(a->ui_find_cached(blk, "e_text_title0"));
    if (!t) t = reinterpret_cast<Obj*>(a->ui_find_cached(blk, "e_text_title"));
    if (t) {
        a->ui_set_text(C(), t, title.c_str());
        a->ui_set_position(C(), t, tx, ty, 0);
    }
    for (const Icon& ic : icons) {
        void* panel = a->ui_find_cached(blk, ic.ctrl);
        if (!panel) continue;
        a->ui_set_visible(C(), panel, 1);
        a->ui_set_position(C(), panel, ic.x, ic.y, 0);
        if (void* it = a->ui_find_cached(panel, "e_text_0")) {
            a->ui_set_text(C(), it, ic.text.c_str());
            if (ref_w > 0 || ref_h > 0) a->ui_set_font_size(C(), it, ref_w, ref_h);
        }
    }
    a->ui_set_position(C(), blk, bx, by, 0);
    a->ui_set_visible(C(), blk, 1);
}

Obj* control_main() {
    return alive(g_agent) ? call_obj(g_agent, "get_ControlMain") : nullptr;
}

void paint(bool show) {
    const SF6CoreAPI* a = A();
    Obj* cm = a ? control_main() : nullptr;
    if (!cm) return;
    float ref_w = 0, ref_h = 0;                   // the glyph size of the game's own Rotation block (c_01)
    void* c01 = a->ui_find_cached(cm, "c_01");
    if (Obj* rt = c01 ? reinterpret_cast<Obj*>(a->ui_find_cached(c01, "e_text_0")) : nullptr) {
        auto r = call(rt, "get_FontSize");
        if (!r.exception_thrown) { std::memcpy(&ref_w, r.bytes.data(), 4); std::memcpy(&ref_h, r.bytes.data() + 4, 4); }
    }
    const std::string move = g_pad ? std::string("<ICON BtnL3>") + "\xEF\xBC\x8B" + "<ICON StickR>"   // U+FF0B fullwidth plus
                                   : std::string("<ICON MouseM>");
    paint_block(a, cm, show, kMoveBlock, g_title_move, kMoveTitleX, kMoveTitleY,
                { { kMoveIconCtrl, move, g_pad ? kMovePadIconX : kMoveIconX, g_pad ? kMovePadIconY : kMoveIconY } },
                true, kMoveBlockX, kMoveBlockY, ref_w, ref_h);
    paint_block(a, cm, show, kZoomBlock, g_title_zoom, kZoomTitleX, kZoomTitleY,
                { { kZoomIcon1Ctrl, g_pad ? "<ICON BtnL2>" : "<ICON MouseWD>", g_pad ? kZoomPadIcon1X : kZoomIcon1X, g_pad ? kZoomPadIconY : kZoomIconY },
                  { kZoomIcon2Ctrl, g_pad ? "<ICON BtnR2>" : "<ICON MouseWU>", g_pad ? kZoomPadIcon2X : kZoomIcon2X, g_pad ? kZoomPadIconY : kZoomIconY } },
                false, kZoomBlockX, kZoomBlockY, ref_w, ref_h);
    if (show) ++g_paints;
}

// a block hidden since the last paint (the Color Mod's periodic hide, the screen): shown again now
bool blocks_hidden() {
    const SF6CoreAPI* a = A();
    Obj* cm = a ? control_main() : nullptr;
    if (!cm) return false;
    for (const char* b : { kMoveBlock, kZoomBlock })
        if (Obj* blk = reinterpret_cast<Obj*>(a->ui_find_cached(cm, b))) if (!visible(blk)) return true;
    return false;
}

} // namespace

void open(Obj* fs_agent, bool pad) {
    if (g_off) return;
    try {
        if (!g_active) {
            if (!resolve()) { g_off = true; return; }
            load_titles();
            g_turns = g_zooms = g_pans = g_paints = 0;
            logf("rto: camera on (hints '%s' / '%s')", g_title_move.c_str(), g_title_zoom.c_str());
        }
        g_active = true;
        g_agent = fs_agent;
        g_pad = pad;
    } catch (...) {
        fault("open");
    }
}

void close() {
    if (!g_active) return;
    try {
        const bool moved = g_has_orig;
        restore_orig();
        paint(false);
        logf("rto: camera off (turns %u, zooms %u, pans %u, hint paints %u%s)", g_turns, g_zooms, g_pans, g_paints,
             moved ? ", position restored" : "");
    } catch (...) {
        fault("close");
    }
    g_drag_turn = false;
    g_active = false;
    g_agent = nullptr;
}

bool active() { return g_active; }

void tick() {
    if (!g_active || g_off) return;
    try {
        input_tick();
        g_ticked = g_frame;
    } catch (...) {
        fault("tick");
    }
}

void post_frame() {
    if (!g_active || g_off || g_ticked != g_frame) return;   // only on a frame the menu drove the camera
    try {
        if (g_frame % kHintsCadenceFrames == 0 || blocks_hidden()) paint(true);
    } catch (...) {
        fault("hints");
    }
}

} // namespace slots::rto::camera
