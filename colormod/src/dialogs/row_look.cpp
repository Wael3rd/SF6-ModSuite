// dialogs::editor::look - see row_look.hpp.
//
// Copies are made ONLY from the option dialog's own update (a post-hook of app.UIFlowOptionBGDialog.Main.OnUpdate,
// R19: duplicate() called from REFramework's LateUpdateBehavior hands back an unfinished object, seen in game 16/10).
// The editor's frame (LateUpdateBehavior) finds what is missing and queues it; the hook makes the copies, hidden; the
// next frame places and shows them. Everything else (placement, colours, visibility) is plain display writes.
//
// Geometry: every rectangle is computed from the live controls (position, size, scale, ControlPoint = which point of
// the control its position names: 0 left-top ... 5 centre ... 10 right-bottom; y grows downwards), never assumed.
// A child's position is relative to its parent's position (its scale applied). Reads go through reflection on the
// game's own controls; the copies (not managed objects, R35) are only reached through the core.
#include "row_look.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#pragma warning(push, 0)                                       // REFramework's header (size_t -> unsigned int)
#include "ll_common.hpp"
#pragma warning(pop)

#include "../common.hpp"

namespace dialogs::editor::look {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;

// The avatar editor's hue bar, loaded as a texture (its UV sequence draws nothing outside the avatar editor,
// ADR-0020): pattern 7 of UVS_Avatar.uvs = 400 x 40 px at (281, 941) of the 1024 x 1024 texture, a 2 px white rim
// around the rainbow. The rectangle below keeps the rainbow only: 3 texels in from each end (no rim blended in by the
// filtering), the middle 20 rows. Red at both ends (hue 0 = 360), the game's own spacing of the hues.
const char* const kTexture = "Product/GUI/wt/ui61000/part/TEX_Avatar_IAM.tex";
constexpr float kU = 286.0f / 1024, kV = 950.0f / 1024, kW = 389.0f / 1024, kH = 20.0f / 1024;
constexpr float kBarExtra = 3.0f;              // the bar: the gauge's track, same ends, 3 px taller (5 -> 8), same centre
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr int kMaxTries = 40;                  // a row not found on its page: tried every 3 frames, then left as it is
const char* const kBar[1] = { "e_cmhue_bar" };
const SF6CoreAPI* api() {
    const SF6CoreAPI* a = cmn::core();
    return (a && SF6CORE_HAS(a, ui_clone_control)) ? a : nullptr;
}
SF6CoreClient* cl() { return cmn::client(); }

void* g_holder = nullptr;                      // the rainbow's texture, kept for the game session
bool g_no_rainbow = false;                     // it did not load: the hue sliders keep the option window's look
bool g_said_old = false;
bool g_hooked = false, g_hook_failed = false;  // the option dialog's update hook (the copies are made there)
void* g_param = nullptr;                       // the page the row states belong to (its address, never dereferenced)
struct RowState {
    int kind = -1;
    bool done = false;
    int tries = 0;
    uint64_t next_try = 0, recheck = 0;
};
std::unordered_map<int, RowState> g_rows;     // the editor's entry id -> its row on g_param
std::unordered_set<std::string> g_logged;     // one line per row and geometry, per editor session
uint64_t g_done = 0, g_failed = 0;

// ---- the queue of copies, made by the option dialog's update ----
struct Build { void* tmpl; void* parent; const char* const* names; int n; uint64_t frame; };
std::mutex g_q_mx;
std::vector<Build> g_queue;
std::atomic<bool> g_queued{ false };
std::atomic<uint64_t> g_made{ 0 }, g_refused{ 0 };

void request(void* tmpl, void* parent, const char* const* names, int n) {
    std::lock_guard<std::mutex> lk(g_q_mx);
    for (const Build& b : g_queue) if (b.parent == parent && b.names == names) return;   // already asked
    g_queue.push_back({ tmpl, parent, names, n, cmn::frame() });
    g_queued.store(true, std::memory_order_release);
}
// SF6CORE_OK for each copy, made hidden: the editor's next frame places and shows it
void run_queue() {
    std::unique_lock<std::mutex> lk(g_q_mx, std::try_to_lock);
    if (!lk.owns_lock()) return;                               // another dialog's update has it
    std::vector<Build> q;
    q.swap(g_queue);
    g_queued.store(false, std::memory_order_release);
    lk.unlock();
    const SF6CoreAPI* a = api();
    if (!a) return;
    const uint64_t f = cmn::frame();
    for (const Build& b : q) {
        if (f > b.frame + 2) continue;                         // asked more than 2 frames ago: its page may be gone
        for (int i = 0; i < b.n; ++i) {
            if (a->ui_find_child(b.parent, b.names[i], 0)) continue;
            void* c = nullptr;
            if (a->ui_clone_control(cl(), b.tmpl, b.parent, b.names[i], &c) != SF6CORE_OK || !c) { ++g_refused; break; }
            SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;   // via.gui.MaskType.NonTarget: no mask of the new parent clips it
            a->obj_invoke(cl(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
            a->ui_set_visible(cl(), c, 0);
            ++g_made;
        }
    }
}
int update_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
#pragma warning(push)
#pragma warning(disable: 4733)
void update_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_queued.load(std::memory_order_acquire) || cmn::closing()) return;   // nothing asked: one load per update
    __try { run_queue(); } __except (EXCEPTION_EXECUTE_HANDLER) { ++g_refused; }
}
#pragma warning(pop)
void install_hook() {
    if (g_hooked || g_hook_failed) return;
    auto* td = API::get()->tdb()->find_type("app.UIFlowOptionBGDialog.Main");
    auto* m = td ? td->find_method("OnUpdate()") : nullptr;
    if (!m && td) m = ll::find_method_noargs(td, "OnUpdate");
    if (m && m->get_function_raw() && m->add_hook(update_pre, update_post, false)) {
        g_hooked = true;
        cmn::log_info("[look] hook app.UIFlowOptionBGDialog.Main.OnUpdate installed (the copies are made there)");
    } else {
        g_hook_failed = true;
        cmn::log_info("[look] app.UIFlowOptionBGDialog.Main.OnUpdate %s: the hue rows keep the option window's look",
                      td ? "not hooked" : "not found");
    }
}

// ---- reads (the game's own controls) ----
bool alive(void* o) { return o && API::get()->sdk()->managed_object->is_managed_object(o); }
bool call0(void* o, const char* name, reframework::InvokeRet& r) {
    if (!alive(o)) return false;
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(o)->get_type_definition(), name);
    if (!m) return false;
    r = m->invoke(reinterpret_cast<MO*>(o), std::vector<void*>{});
    return !r.exception_thrown;
}
void* obj(void* o, const char* name) { reframework::InvokeRet r; return call0(o, name, r) ? r.ptr : nullptr; }
std::string name_of(void* o) {
    reframework::InvokeRet r;
    return (call0(o, "get_Name", r) && r.ptr) ? ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr)) : std::string();
}
// a direct child by name; the walk stops at the copies (last children, not managed objects)
void* child(void* parent, const char* nm) {
    void* c = obj(parent, "get_Child");
    for (int i = 0; c && i < 64; ++i, c = obj(c, "get_Next")) if (name_of(c) == nm) return c;
    return nullptr;
}
// one of our copies under its parent (through the core: a copy fails the managed-object check)
void* copy(void* parent, const char* nm) { return parent ? api()->ui_find_child(parent, nm, 0) : nullptr; }

struct Geo { float x = 0, y = 0, z = 0, w = 0, h = 0, sx = 1, sy = 1; int cp = 0; };
bool floats(void* o, const char* m, float* out, int n) {
    reframework::InvokeRet r;
    if (!call0(o, m, r)) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * (size_t)n);
    return true;
}
bool pos_scale(void* o, Geo& g) {
    float p[3], s[3];
    if (!floats(o, "get_Position", p, 3)) return false;
    g.x = p[0]; g.y = p[1]; g.z = p[2];
    if (floats(o, "get_Scale", s, 3)) { g.sx = s[0]; g.sy = s[1]; }
    return true;
}
bool cp_ok(int cp) { return cp >= 0 && cp <= 10 && cp % 4 <= 2; }
float fx(int cp) { return (float)(cp % 4) * 0.5f; }
float fy(int cp) { return (float)(cp / 4) * 0.5f; }
bool geo(void* o, Geo& g) {
    float sz[2];
    reframework::InvokeRet r;
    if (!pos_scale(o, g) || !floats(o, "get_Size", sz, 2) || !call0(o, "get_ControlPoint", r)) return false;
    g.w = sz[0]; g.h = sz[1]; g.cp = (int)r.dword;
    return cp_ok(g.cp);
}
struct Box { float l = 0, t = 0, r = 0, b = 0; };
Box box(const Geo& g) {                         // the drawn rectangle, in the parent's space
    const float w = g.w * g.sx, h = g.h * g.sy;
    const float l = g.x - fx(g.cp) * w, t = g.y - fy(g.cp) * h;
    return { l, t, l + w, t + h };
}
Box in_parent(const Geo& p, const Box& b) {     // a box of p's space in p's parent's space
    return { p.x + p.sx * b.l, p.y + p.sy * b.t, p.x + p.sx * b.r, p.y + p.sy * b.b };
}

// ---- writes (the core) ----
bool place(void* c, int cp, float l, float t, float w, float h, float z) {   // drawn at (l, t), w x h
    const SF6CoreAPI* a = api();
    return a->ui_set_scale(cl(), c, 1.0f, 1.0f, 1.0f) == SF6CORE_OK && a->ui_set_size(cl(), c, w, h) == SF6CORE_OK &&
           a->ui_set_position(cl(), c, l + fx(cp) * w, t + fy(cp) * h, z) == SF6CORE_OK;
}
void shown(void* c, bool on) {
    if (!c) return;
    if (on) api()->ui_set_force_invisible(cl(), c, 0);
    api()->ui_set_visible(cl(), c, on ? 1 : 0);
}
void log_once(const std::string& line) {
    if (g_logged.insert(line).second) cmn::log_info("%s", line.c_str());
}

// ---- the hue slider: the rainbow on the gauge's track ----
// 1 done, 0 not yet (rows, texture or copy not ready), -1 failed
int hue_on(void* unit, std::string& why) {
    const SF6CoreAPI* a = api();
    if (g_no_rainbow || !g_holder) { why = "no texture"; return -1; }
    if (!a->ui_texture_ready(g_holder)) { why = "loading"; return 0; }
    void* slider = child(unit, "p_Slider_0_s");
    void* gage = slider ? child(slider, "c_gage") : nullptr;
    void* track = gage ? child(gage, "e_rect_bg") : nullptr;
    void* fill = gage ? child(gage, "e_texture_default") : nullptr;
    void* ptr_grp = slider ? child(slider, "p_SliderPointer_0") : nullptr;
    void* point = ptr_grp ? child(ptr_grp, "e_tex_point") : nullptr;
    if (!track || !point) { why = "gauge not found"; return 0; }
    Geo tg, pg;
    if (!geo(track, tg) || !geo(point, pg)) { why = "geometry not read"; return -1; }
    void* bar = copy(gage, kBar[0]);                           // a Texture of the row: the pointer's (drawn above it)
    if (!bar) { request(point, gage, kBar, 1); why = "waiting"; return 0; }
    const Box b = box(tg);
    const float w = b.r - b.l, h = (b.b - b.t) + kBarExtra, cy = (b.t + b.b) * 0.5f;
    const bool ok = a->ui_set_texture_rect(cl(), bar, g_holder, kU, kV, kW, kH) == SF6CORE_OK &&
                    a->ui_set_color(cl(), bar, kWhite) == SF6CORE_OK && place(bar, pg.cp, b.l, cy - h * 0.5f, w, h, tg.z);
    if (!ok) { shown(bar, false); why = "writes refused"; return -1; }
    shown(bar, true);
    if (fill) a->ui_set_force_invisible(cl(), fill, 1);      // nothing over the bar; the pointer stays on top
    SF6CoreTextureState ts{};
    ts.struct_size = sizeof ts;
    char st[160] = "state not read";
    if (a->ui_get_texture_state(cl(), bar, &ts) == SF6CORE_OK) {
        std::snprintf(st, sizeof st, "asset %d uv type %d (%.4f, %.4f, %.4f, %.4f) %.1fx%.1f", ts.asset_type, ts.uv_type,
                      ts.u, ts.v, ts.w, ts.h, ts.size_w, ts.size_h);
        if (ts.texture_holder) API::get()->sdk()->managed_object->release((REFrameworkManagedObjectHandle)ts.texture_holder);
    }
    char line[512];
    std::snprintf(line, sizeof line, "[look] hue %s: track (%.1f, %.1f)-(%.1f, %.1f) cp %d scale %.2f -> bar (%.1f, %.1f) %.1fx%.1f "
                  "(cp %d, under c_gage), fill %s; %s", name_of(unit).c_str(), b.l, b.t, b.r, b.b, tg.cp, tg.sx, b.l,
                  cy - h * 0.5f, w, h, pg.cp, fill ? "hidden" : "not found", st);
    log_once(line);
    return 1;
}
void hue_off(void* unit) {                                     // the game's look back, if the row had the bar
    void* slider = child(unit, "p_Slider_0_s");
    void* gage = slider ? child(slider, "c_gage") : nullptr;
    void* bar = copy(gage, kBar[0]);
    if (!bar) return;
    shown(bar, false);
    if (void* fill = child(gage, "e_texture_default")) api()->ui_set_force_invisible(cl(), fill, 0);
}

}  // namespace

void preload() {
    if (g_holder || g_no_rainbow) return;
    const SF6CoreAPI* a = api();
    if (!a) {
        if (!g_said_old) {
            g_said_old = true;
            const SF6CoreAPI* c = cmn::core();
            cmn::log_info("[look] sf6core %s has no ui_clone_control (0.8.2): the hue rows keep the option window's look",
                          c && c->get_version ? c->get_version() : "?");
        }
        return;
    }
    install_hook();
    void* h = nullptr;
    const SF6CoreStatus st = a->ui_texture_load(cl(), kTexture, &h);
    if (st != SF6CORE_OK || !h) {
        g_no_rainbow = true;
        cmn::log_info("[look] %s not loaded (status %d): the hue sliders keep the option window's look", kTexture, (int)st);
        return;
    }
    g_holder = h;
}

void apply(void* param, const std::vector<core::RowLook>& rows, RowControlFn row_control) {
    if (!api() || !param || !row_control) { if (!api()) preload(); return; }
    if (!g_holder && !g_no_rainbow) preload();
    if (!g_hooked) return;                                     // no copy can be made: the option window's look
    if (param != g_param) { g_param = param; g_rows.clear(); }
    const uint64_t f = cmn::frame();
    for (const core::RowLook& r : rows) {
        RowState& s = g_rows[r.eid];
        if (s.kind != r.kind) { s = RowState{}; s.kind = r.kind; }
        if (s.done) continue;
        if (f < s.next_try || s.tries >= kMaxTries) continue;
        void* unit = row_control(r.eid, param);
        if (!unit) { s.next_try = f + 3; ++s.tries; continue; }   // the page's rows are not bound yet
        std::string why;
        int res = 1;
        if (r.kind == 1) res = hue_on(unit, why);
        else hue_off(unit);
        if (res > 0) {
            s.done = true;
            if (r.kind) ++g_done;
        } else if (res == 0) {
            s.next_try = f + (why == "waiting" ? 1 : 3);
            if (why != "loading" && why != "waiting") ++s.tries;
            else if (why == "waiting" && ++s.tries > kMaxTries - 10) log_once("[look] hue row " + name_of(unit) +
                                                                       ": the copy was not made (see sf6core.log)");
        } else {
            s.next_try = f + 30; s.tries += 10; ++g_failed;
            log_once("[look] hue row " + name_of(unit) + ": " + why + " (the option window's look stays)");
        }
    }
}

void forget() { g_param = nullptr; g_rows.clear(); g_logged.clear(); }

std::string status() {
    char b[200];
    std::snprintf(b, sizeof b, "look: core %s, hook %s, rainbow %s, rows %zu, done %llu, failed %llu, copies made %llu refused %llu",
                  api() ? "0.8.2+" : "older", g_hooked ? "on" : (g_hook_failed ? "failed" : "-"),
                  g_no_rainbow ? "not loaded" : (g_holder ? "loaded" : "-"), g_rows.size(), (unsigned long long)g_done,
                  (unsigned long long)g_failed, (unsigned long long)g_made.load(), (unsigned long long)g_refused.load());
    return b;
}

}  // namespace dialogs::editor::look
