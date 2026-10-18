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
#include "../perf_hook.hpp"

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
struct Build { void* tmpl; void* parent; const char* const* names; int n; uint64_t frame; uint16_t priority; float scale; };
std::mutex g_q_mx;
std::vector<Build> g_queue;
std::atomic<bool> g_queued{ false };
std::atomic<uint64_t> g_made{ 0 }, g_refused{ 0 };

// priority: set_Priority of each copy (0 = left as copied); scale: set on each copy (0 = left as copied)
void request(void* tmpl, void* parent, const char* const* names, int n, uint16_t priority, float scale) {
    std::lock_guard<std::mutex> lk(g_q_mx);
    for (const Build& b : g_queue) if (b.parent == parent && b.names == names) return;   // already asked
    g_queue.push_back({ tmpl, parent, names, n, cmn::frame(), priority, scale });
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
            if (b.priority) {                                  // drawn above the dialog's backgrounds
                SF6CoreArg p{}; p.type = SF6CORE_ARG_I64; p.v.i = b.priority;
                a->obj_invoke(cl(), c, "set_Priority(System.UInt16)", &p, 1, nullptr);
            }
            if (b.scale > 0) a->ui_set_scale(cl(), c, b.scale, b.scale, 1.0f);
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
    if (m && m->get_function_raw() && m->add_hook(update_pre, perf::timed_post<perf::HkLookOnUpdate, update_post>, false)) {
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

// The bar is drawn with Priority 10 (above the track); the pointer's two textures go one above it on hue rows, their
// own Priority (read once by name) back on the others
std::unordered_map<std::string, uint32_t> g_prio;
void pointer_above(void* el, bool on) {
    if (!el) return;
    const std::string nm = name_of(el);
    reframework::InvokeRet r;
    if (!g_prio.count(nm)) { if (!call0(el, "get_Priority", r)) return; g_prio[nm] = r.dword & 0xFFFF; }
    SF6CoreArg p{}; p.type = SF6CORE_ARG_I64; p.v.i = on ? 11 : g_prio[nm];
    if (call0(el, "get_Priority", r) && (int64_t)(r.dword & 0xFFFF) == p.v.i) return;
    api()->obj_invoke(cl(), el, "set_Priority(System.UInt16)", &p, 1, nullptr);
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
    if (!bar) { request(point, gage, kBar, 1, 10, 0); why = "waiting"; return 0; }   // above the track (it looked dim)
    const Box b = box(tg);
    const float w = b.r - b.l, h = (b.b - b.t) + kBarExtra, cy = (b.t + b.b) * 0.5f;
    const bool ok = a->ui_set_texture_rect(cl(), bar, g_holder, kU, kV, kW, kH) == SF6CORE_OK &&
                    a->ui_set_color(cl(), bar, kWhite) == SF6CORE_OK && place(bar, pg.cp, b.l, cy - h * 0.5f, w, h, tg.z);
    if (!ok) { shown(bar, false); why = "writes refused"; return -1; }
    shown(bar, true);
    if (fill) a->ui_set_force_invisible(cl(), fill, 1);      // nothing over the bar
    for (const char* nm : { "e_tex_point_bg", "e_tex_point" }) pointer_above(child(ptr_grp, nm), true);   // the pointer over it
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
    void* ptr_grp = slider ? child(slider, "p_SliderPointer_0") : nullptr;
    for (const char* nm : { "e_tex_point_bg", "e_tex_point" }) pointer_above(child(ptr_grp, nm), false);
}

// ---- the palette grid (0.21.0): the game's own colour cells, as the avatar editor's ----
// 50 copies of Fighter Settings' theme cell (a via.gui.SelectItem of its theme ScrollGrid), made into the top dialog's
// c_dialog (never items added into a ScrollGrid: that crashed the game), scale 0.35, drawn above the dialog (Priority 10:
// without it a copy into another parent is drawn under the backgrounds). Each cell's picture = the texture's opaque
// white block tinted with the palette colour; its title, badge and darkening hidden; the game's own check mark on the
// colour edited, the game's own focus glow on the cursor. The five palette rows hold the navigation only: hidden.
// Proven by a probe in game (16/10) before this code.
constexpr float kCellScale = 0.35f, kCellPitch = 184.0f * kCellScale;   // the theme grid's pitch (184 px), scaled: 64.4
constexpr float kWhiteU = 873.0f / 1024, kWhiteV = 10.0f / 1024, kWhiteW = 20.0f / 1024, kWhiteH = 20.0f / 1024;
constexpr int kCells = core::kPaletteLines * core::kPaletteCols;
constexpr uint16_t kAbove = 10;                                // Priority of our copies
const char* const* cell_names() {
    static std::string s[kCells];
    static const char* p[kCells] = {};
    if (!p[0]) for (int i = 0; i < kCells; ++i) { s[i] = "p_cmcell_" + std::to_string(i); p[i] = s[i].c_str(); }
    return p;
}
struct GridState {
    bool placed = false, failed = false;
    int tries = 0;
    uint64_t next_try = 0, recheck = 0;
    int glow = -2, check = -2;                                 // the cells showing the focus glow / the check now
};
GridState g_grid;

// Fighter Settings' theme cell, the template: agent MatchingSetting, its theme ScrollGrid's first item
void* cell_template() {
    static const char* const kPath[] = { "p_Group_0_switch_show", "c_list", "p_FighterProfileSetting_3_h", "p_Group_0_switch",
                                         "p_ThemeSelect_0_h", "p_ScrollGrid_1_v", "c_GroupItem_0" };
    void* agents[4] = {};
    const uint32_t n = api()->ui_agents("MatchingSetting", agents, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        void* c = obj(agents[i], "get_ControlMain");
        for (const char* p : kPath) c = c ? child(c, p) : nullptr;
        if (c) return c;
    }
    return nullptr;
}
// the top dialog's body (c_main/c_dialog), the cells' parent
void* dialog_body(void* param) {
    MO* agent = alive(param) ? ll::read_field_object(reinterpret_cast<MO*>(param), "<Agent>k__BackingField") : nullptr;
    void* cm = agent ? obj(agent, "get_ControlMain") : nullptr;
    return cm ? child(cm, "c_dialog") : nullptr;
}
// a row's centre in the dialog body's space: the row's background (the slider prefab, laid out on every row), else its
// gauge, then up the parents (position + scale each) to the body
bool row_centre(void* unit, void* body, float& x, float& y) {
    void* slider = child(unit, "p_Slider_0_s");
    Geo sg, bsg, bgg, gg, tg;
    if (!slider || !pos_scale(slider, sg)) return false;
    void* bgs = child(slider, "c_bg_s");
    void* bg = bgs ? child(bgs, "e_scale9grid_default") : nullptr;
    Box row;
    if (bg && pos_scale(bgs, bsg) && geo(bg, bgg)) row = in_parent(sg, in_parent(bsg, box(bgg)));
    else {
        void* gage = child(slider, "c_gage");
        void* track = gage ? child(gage, "e_rect_bg") : nullptr;
        if (!track || !pos_scale(gage, gg) || !geo(track, tg)) return false;
        row = in_parent(sg, in_parent(gg, box(tg)));
    }
    x = (row.l + row.r) * 0.5f; y = (row.t + row.b) * 0.5f;
    void* n = unit;
    for (int i = 0; i < 8 && n && n != body; ++i) {
        Geo g;
        if (!pos_scale(n, g)) return false;
        x = g.x + g.sx * x; y = g.y + g.sy * y;
        n = obj(n, "get_Parent");
    }
    return n == body;
}
// the centre of a cell's picture in the cell's own space (read on the template: the copies are not managed objects)
void cell_centre(void* tmpl, float& ox, float& oy) {
    ox = oy = 0;
    void* bgs = child(tmpl, "c_bg_s");
    void* bg = bgs ? child(bgs, "c_bg") : nullptr;
    void* tex = bg ? child(bg, "e_texture_theme") : nullptr;
    Geo g1, g2, g3;
    if (!tex || !pos_scale(bgs, g1) || !pos_scale(bg, g2) || !geo(tex, g3)) return;
    const Box b = in_parent(g1, in_parent(g2, box(g3)));
    ox = (b.l + b.r) * 0.5f; oy = (b.t + b.b) * 0.5f;
}
void* sub(void* c, const char* nm) { return c ? api()->ui_find_child(c, nm, 0) : nullptr; }
void cell_part(void* body, int cell, const char* part, const char* inner, bool on) {   // a cell's check / focus glow
    if (cell < 0 || cell >= kCells) return;
    void* c = sub(copy(body, cell_names()[cell]), part);
    if (inner) c = sub(c, inner);
    if (c) api()->ui_set_visible(cl(), c, on ? 1 : 0);
}
void grid_step(void* param, const std::vector<const core::RowLook*>& rows, RowControlFn rc) {
    const SF6CoreAPI* a = api();
    const uint64_t f = cmn::frame();
    if (g_grid.failed) return;
    void* body = dialog_body(param);
    if (!g_grid.placed) {
        if (f < g_grid.next_try) return;
        auto retry = [&](const char* why, bool count) {
            g_grid.next_try = f + 1;
            if (count && ++g_grid.tries > 90) { g_grid.failed = true; log_once(std::string("[look] palette grid: ") + why + " (the palette rows stay as text)"); }
        };
        if (!body) { retry("the dialog's body not found", true); return; }
        if (g_no_rainbow) { g_grid.failed = true; log_once("[look] palette grid: no texture (the palette rows stay as text)"); return; }
        if (!g_holder || !a->ui_texture_ready(g_holder)) { retry("texture", false); return; }
        void* tmpl = cell_template();
        if (!tmpl) { g_grid.failed = true; log_once("[look] palette grid: Fighter Settings' theme cell not found (the palette rows stay as text)"); return; }
        if (!copy(body, cell_names()[kCells - 1])) {           // made last: all there
            request(tmpl, body, cell_names(), kCells, kAbove, kCellScale);
            retry("the cells were not made (see sf6core.log)", true);
            return;
        }
        float ox, oy, cx = 0, cy[core::kPaletteLines] = {};
        bool have[core::kPaletteLines] = {};
        cell_centre(tmpl, ox, oy);
        Geo tg;
        pos_scale(tmpl, tg);
        for (const core::RowLook* r : rows) {
            void* unit = rc(r->eid, param);
            float x = 0, y = 0;
            if (!unit || r->line < 0 || r->line >= core::kPaletteLines || !row_centre(unit, body, x, y)) continue;
            if (r->line == 0) cx = x;
            cy[r->line] = y; have[r->line] = true;
        }
        for (bool h : have) if (!h) { retry("palette rows not placed yet", true); return; }
        bool ok = true;
        for (int i = 0; i < kCells && ok; ++i) {
            const int line = i / core::kPaletteCols, col = i % core::kPaletteCols;
            void* cell = copy(body, cell_names()[i]);
            void* tex = sub(sub(sub(cell, "c_bg_s"), "c_bg"), "e_texture_theme");
            const uint32_t rgb = core::kPalette[line][col];
            const uint32_t rgba = 0xFF000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);   // via.Color: r in the low byte
            ok = cell && tex && a->ui_set_scale(cl(), cell, kCellScale, kCellScale, 1.0f) == SF6CORE_OK &&
                 a->ui_set_position(cl(), cell, cx + ((float)col - 4.5f) * kCellPitch - kCellScale * ox, cy[line] - kCellScale * oy, tg.z) == SF6CORE_OK &&
                 a->ui_set_texture_rect(cl(), tex, g_holder, kWhiteU, kWhiteV, kWhiteW, kWhiteH) == SF6CORE_OK &&
                 a->ui_set_color(cl(), tex, rgba) == SF6CORE_OK;
            if (!ok) break;
            for (const char* nm : { "e_text_title", "e_darken", "c_batch", "c_check" }) if (void* e = sub(cell, nm)) a->ui_set_visible(cl(), e, 0);
            // drawn whole: no mask of the dialog clips the cell's parts (the cell's own mask elements keep theirs)
            for (void* e : { sub(cell, "c_bg_s"), sub(sub(cell, "c_bg_s"), "c_bg"), tex, sub(cell, "c_outline"),
                             sub(sub(cell, "c_outline"), "c_inner"), sub(cell, "c_check") })
                if (e) { SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1; a->obj_invoke(cl(), e, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr); }
            if (void* in = sub(sub(cell, "c_outline"), "c_inner")) a->ui_set_visible(cl(), in, 0);
            shown(cell, true);
        }
        if (!ok) { g_grid.failed = true; log_once("[look] palette grid: a cell's writes were refused (the palette rows stay as text)"); return; }
        for (const core::RowLook* r : rows) if (void* unit = rc(r->eid, param)) a->ui_set_force_invisible(cl(), unit, 1);   // the rows: navigation only
        for (int i : { 0, core::kPaletteCols - 1 }) {               // the outer columns' pictures on screen
            void* tex = sub(sub(sub(copy(body, cell_names()[i]), "c_bg_s"), "c_bg"), "e_texture_theme");
            SF6CoreArg gp{}, sz{};
            if (tex && a->obj_invoke(cl(), tex, "get_GlobalPosition", nullptr, 0, &gp) == SF6CORE_OK &&
                a->obj_invoke(cl(), tex, "get_Size", nullptr, 0, &sz) == SF6CORE_OK) {
                float p[2], w[2];
                std::memcpy(p, &gp.v.i, sizeof p); std::memcpy(w, &sz.v.i, sizeof w);
                char line[200];
                std::snprintf(line, sizeof line, "[look] grid cell %d: picture at (%.1f, %.1f) global, %.1fx%.1f before the cell's scale %.2f",
                              i, p[0], p[1], w[0], w[1], kCellScale);
                log_once(line);
            }
        }
        for (int l = 0; l < core::kPaletteLines; ++l) {
            char line[320];
            std::snprintf(line, sizeof line, "[look] grid line %d: row centre (%.1f, %.1f) in c_dialog -> cells centred at y %.1f, x %.1f .. %.1f "
                          "(pitch %.1f, scale %.2f, picture centre offset (%.1f, %.1f) in the cell)", l + 1, cx, cy[l], cy[l],
                          cx - 4.5f * kCellPitch, cx + 4.5f * kCellPitch, kCellPitch, kCellScale, ox, oy);
            log_once(line);
        }
        g_grid.placed = true; g_grid.glow = g_grid.check = -2; g_grid.recheck = f + 30;
        ++g_done;
    }
    if (!body) return;
    int glow = -1, check = -1;
    for (const core::RowLook* r : rows) {
        if (r->focused && r->cursor >= 0) glow = r->line * core::kPaletteCols + r->cursor;
        if (r->checked >= 0) check = r->line * core::kPaletteCols + r->checked;
    }
    if (glow != g_grid.glow) { cell_part(body, g_grid.glow, "c_outline", "c_inner", false); cell_part(body, glow, "c_outline", "c_inner", true); g_grid.glow = glow; }
    if (check != g_grid.check) { cell_part(body, g_grid.check, "c_check", nullptr, false); cell_part(body, check, "c_check", nullptr, true); g_grid.check = check; }
    if (f >= g_grid.recheck) {                                 // the rows stay hidden (the game's own focus handling)
        g_grid.recheck = f + 30;
        for (const core::RowLook* r : rows) if (void* unit = rc(r->eid, param)) a->ui_set_force_invisible(cl(), unit, 1);
    }
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
    if (param != g_param) { g_param = param; g_rows.clear(); g_grid = GridState{}; }
    const uint64_t f = cmn::frame();
    std::vector<const core::RowLook*> grid_rows;
    for (const core::RowLook& r : rows) if (r.kind == 2) grid_rows.push_back(&r);
    if (!grid_rows.empty()) grid_step(param, grid_rows, row_control);
    for (const core::RowLook& r : rows) {
        if (r.kind == 2) continue;
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

void forget() { g_param = nullptr; g_rows.clear(); g_logged.clear(); g_grid = GridState{}; }

std::string status() {
    char b[200];
    std::snprintf(b, sizeof b, "look: core %s, hook %s, rainbow %s, rows %zu, done %llu, failed %llu, copies made %llu refused %llu",
                  api() ? "0.8.2+" : "older", g_hooked ? "on" : (g_hook_failed ? "failed" : "-"),
                  g_no_rainbow ? "not loaded" : (g_holder ? "loaded" : "-"), g_rows.size(), (unsigned long long)g_done,
                  (unsigned long long)g_failed, (unsigned long long)g_made.load(), (unsigned long long)g_refused.load());
    return b;
}

}  // namespace dialogs::editor::look
