// SF6 Slots — the name of the outfit on Fighter Settings' preview, at the right end of the bottom input guide bar
// (docs/RUNTIME_OPTIONS.md, "Outfit name on Fighter Settings").
//
// Battle Settings > character (Fighter Settings, agent MatchingFighterSetting): the costume row shows the game's
// name of a slot ("Outfit VII"); the bar's line gets, right-aligned, the outfit's real name: one of ours = its label
// in outfits.json (the mod's name, "Aloha Ryu"), a game outfit = the costume row's own text (what the game shows).
//
// Our own text, never the bar's: that text has ONE writer (the Color Mod 0.22.4+, else the plugin, rto_menu.cpp;
// two writers crashed the game on 20/10). The name is a copy of the bar's item template (e_text_0 of c_guideBtn:
// the element the game itself duplicates for every entry, so the same font, size and colour), made through the
// core's ui_clone_control (core 0.8.2+), added under c_guideBtn's parent (outside the game's item list).
//   - R19: duplicate() from LateUpdateBehavior hands back an unfinished object. The copy is made inside the guide
//     list's own update (post-hook of app.UIPartsInputGuideList.OnLateUpdate, that list only, where the game makes
//     its own copies of the template); the frame only asks for it. The one exception to the plugin's rule "hooks
//     only store numbers" (slots.hpp): the hook body is the core's guarded copy, made hidden, one per bar (it
//     lives and dies with the bar's tree), never again after 3 refusals in a session.
//   - R35: the copy is not a managed object: found again by name under its live parent (ui_find_child), every
//     write through the core (type check, compare-before-write, SEH).
//   - R1: every other write (text, place, visibility) from LateUpdateBehavior, by us only.
// Shown while Fighter Settings is on screen and no option window is open (our options menu and the Color Mod's
// colour editor have their own guide bar and camera hints there); hidden otherwise, and when it would run into the
// bar's own text.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "rto_data.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>

namespace slots::fsname {
namespace {

const char* kCopyName = "e_text_slots_outfit";    // not "p_...": never taken for a UI part (R20)
constexpr size_t kNameChars = 40;                 // the menu title's cut (rto_menu.cpp)
constexpr int kMaxRefused = 3;                    // copies refused per session, then the label stays off
constexpr uint32_t kPassFrames = 10;              // placement, overlap and the bar's text re-read
constexpr uint32_t kNameFrames = 5;               // the outfit shown re-read
constexpr float kGap = 48.0f;                     // free space kept between the bar's text and the name (1080p units)

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

bool g_on = false;                                // installed (core with ui_clone_control, hook in place)
bool g_off_logged = false, g_lost_logged = false;
bool g_bs = false, g_bs_closed = false, g_select = false;
uint32_t g_bs_seen = 0;

// ---- the copy request, taken by the guide list's update (any thread) ----
struct Req { uintptr_t part = 0; uint32_t frame = 0; };   // the bar's list (its address, matched to `this`), when
std::mutex g_req_mx;
Req g_req;
std::atomic<bool> g_req_on{ false };
std::atomic<int> g_req_status{ 1000 };            // last copy status (1000: none yet), read on the frame
std::atomic<int> g_refused{ 0 };
std::atomic<uintptr_t> g_made_parent{ 0 };       // the parent the last copy was made under (one copy per parent)
thread_local uintptr_t tl_part = 0;

int pre_guide_late(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_req_on.load(std::memory_order_acquire) && !g_closing.load(std::memory_order_relaxed))
        tl_part = reinterpret_cast<uintptr_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void bar_texts(Obj* btn, Obj** text, Obj** tmpl);
// Inside the list's own update: the template and the parent are found from `this` (live), never kept from a frame
void make_copy(uintptr_t part) {
    {
        std::lock_guard<std::mutex> lk(g_req_mx);
        if (!g_req.part || g_req.part != part) return;
        if (g_frame - g_req.frame > 2) return;    // asked more than 2 frames ago: left for the frame to ask again
        g_req = Req{};
        g_req_on.store(false, std::memory_order_release);
    }
    const SF6CoreAPI* a = A();
    Obj* ctrl = get_obj(reinterpret_cast<Obj*>(part), "<Control>k__BackingField");
    Obj* btn = ctrl ? reinterpret_cast<Obj*>(a->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
    Obj* text = nullptr; Obj* tmpl = nullptr;
    bar_texts(btn, &text, &tmpl);
    Obj* parent = btn ? call_obj(btn, "get_Parent") : nullptr;
    if (!parent || !text) { ++g_refused; g_req_status.store(-3000, std::memory_order_release); return; }
    if (a->ui_find_child(parent, kCopyName, 0)) return;
    void* c = nullptr;
    const SF6CoreStatus st = a->ui_clone_control(C(), tmpl ? tmpl : text, parent, kCopyName, &c);
    g_req_status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st), std::memory_order_release);
    if (st != SF6CORE_OK || !c) { ++g_refused; return; }
    g_made_parent.store(reinterpret_cast<uintptr_t>(parent), std::memory_order_release);
    SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;    // via.gui.MaskType.NonTarget: no mask of the new parent clips it
    a->obj_invoke(C(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
    a->ui_set_visible(C(), c, 0);                 // placed and shown by the next frame
}
void post_guide_late(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t p = tl_part;
    tl_part = 0;
    if (!p || !g_req_on.load(std::memory_order_acquire) || g_closing.load(std::memory_order_relaxed)) return;
    try { make_copy(p); } catch (...) { ++g_refused; g_req_status.store(-2000, std::memory_order_release); }   // /EHa: SEH too
}

// ---- the bar (InputGuide agent > app.UIPartsInputGuideList > ... > c_guideBtn) ----
struct Bar {
    Obj* agent = nullptr;                         // checked against the core's agent registry each frame
    Obj* part = nullptr;
    Obj* btn = nullptr;                           // c_guideBtn
    Obj* parent = nullptr;                        // c_guideBtn's parent: where the copy lives
};
Bar g_bar;
uint32_t g_bar_at = 0;
bool g_shown = false;                             // the copy was shown (to be hidden)
std::string g_name, g_written;                    // the name wanted, the text last written
uint32_t g_name_at = 0, g_pass_at = 0;
uintptr_t g_styled = 0;                           // the copy given its text settings (address)
int g_cp = -1;                                    // the ControlPoint written on it
int g_status_log = 1000;
std::string g_src_log, g_geo_log;
std::string g_meas_name;                          // the name whose drawn width was last measured, and that width
double g_meas_w = 0;

bool visible(Obj* o) {
    Obj* cm = o ? call_obj(o, "get_ControlMain") : nullptr;
    bool ok = false;
    const bool v = cm && call_bool(cm, "get_ActualVisible", {}, &ok);
    return ok && v;
}
// The bar's text (the Color Mod's / the game's entries): e_text_0_g_*, else the first e_text_0_d_*; the template e_text_0
void bar_texts(Obj* btn, Obj** text, Obj** tmpl) {
    *text = *tmpl = nullptr;
    Obj* first_d = nullptr;
    int n = 0;
    for (Obj* c = btn ? call_obj(btn, "get_Child") : nullptr; c && n < 64; c = call_obj(c, "get_Next"), ++n) {
        const std::string nm = str(call_obj(c, "get_Name"));
        if (nm == "e_text_0") *tmpl = c;
        else if (nm.rfind("e_text_0_g_", 0) == 0 && !*text) *text = c;
        else if (nm.rfind("e_text_0_d_", 0) == 0 && !first_d) first_d = c;
    }
    if (!*text) *text = first_d;
}
// The visible bar with a text (the one rto_menu.cpp's find_guide_text takes)
bool find_bar(Bar& b) {
    b = Bar{};
    const SF6CoreAPI* a = A();
    void* buf[4] = {};
    const uint32_t n = a->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* ag = reinterpret_cast<Obj*>(buf[i]);
        if (!visible(ag)) continue;
        Obj* lst = get_obj(get_obj(ag, "_PartsManager"), "_List");
        for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
            Obj* part = list_item(lst, k);
            if (!part || type_name(part) != "app.UIPartsInputGuideList") continue;
            Obj* ctrl = get_obj(part, "<Control>k__BackingField");
            Obj* btn = ctrl ? reinterpret_cast<Obj*>(a->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
            Obj* text = nullptr; Obj* tmpl = nullptr;
            bar_texts(btn, &text, &tmpl);
            if (!text) break;                     // this bar has no text: next agent
            Obj* parent = call_obj(btn, "get_Parent");
            if (!parent) break;
            b.agent = ag; b.part = part; b.btn = btn; b.parent = parent;
            return true;
        }
    }
    return false;
}
// The cached bar, while its agent is still one the registry lists (and on screen)
bool bar_live(bool on_screen) {
    if (!g_bar.agent) return false;
    void* buf[4] = {};
    const uint32_t n = A()->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i)
        if (buf[i] == g_bar.agent) return (!on_screen || visible(g_bar.agent)) && alive(g_bar.parent) && alive(g_bar.btn);
    return false;
}
void* copy_now() { return g_bar.parent ? A()->ui_find_child(g_bar.parent, kCopyName, 0) : nullptr; }

void hide(const char* why) {
    if (g_shown) {
        if (bar_live(false))                      // its agent hidden too: the copy would come back with it
            if (void* c = copy_now()) A()->ui_set_visible(C(), c, 0);
        g_shown = false;
        logf("fs name: hidden (%s)", why);
    }
}

// ---- what to show ----
Obj* fs_agent() {
    void* buf[4] = {};
    return A()->ui_agents("MatchingFighterSetting", buf, 4) ? reinterpret_cast<Obj*>(buf[0]) : nullptr;
}
// The fighter and costume Fighter Settings shows (its app.UIPartsMatchingFighterSetting part)
bool fs_selection(Obj* agent, int& fid, int& cos) {
    Obj* lst = get_obj(get_obj(agent, "_PartsManager"), "_List");
    for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
        Obj* part = list_item(lst, k);
        if (!part || type_name(part) != "app.UIPartsMatchingFighterSetting") continue;
        int32_t f = -1;
        if (!get_i32(part, "FighterId", f)) return false;
        const Ret r = call(part, "GetCostumeId");
        if (r.exception_thrown) return false;
        fid = f; cos = static_cast<int32_t>(r.dword);
        return true;
    }
    return false;
}
std::string read_name(Obj* agent) {
    const SF6CoreAPI* a = A();
    Obj* cm = call_obj(agent, "get_ControlMain");
    Obj* spin = cm ? reinterpret_cast<Obj*>(a->ui_find_cached(cm, "p_Spin_costume")) : nullptr;
    Obj* text = spin ? reinterpret_cast<Obj*>(a->ui_find_cached(spin, "e_text")) : nullptr;
    std::string row = text ? str(call_obj(text, "get_Message")) : std::string();
    if (const size_t at = row.find("  <ICON"); at != std::string::npos) row.resize(at);   // a key icon of ours (rto_menu.cpp)
    int fid = -1, cos = -1;
    const bool sel = fs_selection(agent, fid, cos);
    const std::string label = sel ? costumes::slot_label(fid, cos) : std::string();
    char src[160];
    if (!sel) std::snprintf(src, sizeof src, "Fighter Settings' part not found: the costume row's text");
    else std::snprintf(src, sizeof src, "F%d outfit %d: %s", fid, cos, label.empty() ? "the costume row's text (the game's)" : "our outfit's label");
    if (g_src_log != src) { g_src_log = src; logf("fs name: %s", src); }
    return rto::clip_name(label.empty() ? row : label, kNameChars);
}

// ---- placement (1080p GUI units; the bar's own space, every scale applied) ----
struct Geo { float x = 0, y = 0, z = 0, sx = 1, sy = 1; int cp = 0; };
bool floats(Obj* o, const char* m, float* out, int n) {
    const Ret r = call(o, m);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * static_cast<size_t>(n));
    return true;
}
bool geo(Obj* o, Geo& g) {
    float p[3], s[3];
    if (!floats(o, "get_Position", p, 3)) return false;
    g.x = p[0]; g.y = p[1]; g.z = p[2];
    if (floats(o, "get_Scale", s, 3)) { g.sx = s[0]; g.sy = s[1]; }
    const Ret r = call(o, "get_ControlPoint");
    g.cp = r.exception_thrown ? 0 : static_cast<int>(r.dword);
    if (g.cp < 0 || g.cp > 10 || g.cp % 4 > 2) g.cp = 0;
    return true;
}
float width_of(Obj* o) {                          // the drawn text's width (its own space), 0 unknown
    float s[2] = {};
    return floats(o, "get_DisplaySize", s, 2) && std::isfinite(s[0]) && s[0] > 0 ? s[0] : 0.0f;
}
// Characters drawn (UTF-8), a tag such as <ICON KeyF> counted as two (one glyph and its space)
double glyphs(const std::string& s) {
    double n = 0;
    bool tag = false;
    for (unsigned char ch : s) {
        if (ch == '<') { tag = true; n += 2; continue; }
        if (tag) { if (ch == '>') tag = false; continue; }
        if ((ch & 0xC0) != 0x80) n += 1;
    }
    return n;
}

// One pass: the copy asked for, or placed, written and shown / hidden
void pass(const std::string& name) {
    const SF6CoreAPI* a = A();
    Obj* text = nullptr; Obj* tmpl = nullptr;
    bar_texts(g_bar.btn, &text, &tmpl);
    if (!text) { hide("the bar has no text"); return; }
    void* c = copy_now();
    if (!c) {
        g_shown = false;
        if (g_refused.load() >= kMaxRefused) return;
        if (g_made_parent.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(g_bar.parent)) {
            if (!g_lost_logged) { g_lost_logged = true; logf("fs name: the copy made under this bar is not found by name: no other copy"); }
            return;                               // never a second copy under one parent
        }
        std::lock_guard<std::mutex> lk(g_req_mx);
        g_req.part = reinterpret_cast<uintptr_t>(g_bar.part);
        g_req.frame = g_frame;
        if (!g_req_on.exchange(true, std::memory_order_acq_rel))
            logf("fs name: copy of the bar's %s asked", tmpl ? "item template e_text_0" : "text (no e_text_0)");
        return;
    }

    // geometry: the bar's text in c_guideBtn, c_guideBtn in the copy's parent, the parent's chain to the root
    Geo t, b;
    if (!geo(text, t) || !geo(g_bar.btn, b)) { hide("the bar's place unread"); return; }
    double off = 0, sc = 1;                       // parent space -> root space: x_root = off + sc * x
    double canvas = 0;
    Obj* o = g_bar.parent;
    for (int d = 0; o && d < 24; ++d) {
        Geo g;
        if (!geo(o, g)) break;
        off = g.x + g.sx * off; sc *= g.sx;
        Obj* up = call_obj(o, "get_Parent");
        if (!up) { float s[2] = {}; if (floats(o, "get_Size", s, 2)) canvas = s[0]; }
        o = up;
    }
    if (!(canvas >= 1000 && canvas <= 10000)) canvas = 1920;
    if (!(std::fabs(sc) > 1e-3)) sc = 1;
    const float tw = width_of(text);
    const double text_l_btn = t.x - (t.cp % 4) * 0.5 * tw * t.sx;              // the bar text's left edge
    double left = off + sc * (b.x + b.sx * text_l_btn);                      // in root space
    if (!(left >= 16 && left <= canvas * 0.25)) left = 96;                  // the bar's margin, mirrored right
    const double right = canvas - left;
    const float x = static_cast<float>((right - off) / sc);
    const float y = static_cast<float>(b.y + b.sy * t.y);
    const int cp = (t.cp / 4) * 4 + 2;                                      // the text's row, its right point

    if (reinterpret_cast<uintptr_t>(c) != g_styled) {      // a new copy: its text settings, once
        g_styled = reinterpret_cast<uintptr_t>(c);
        g_written.clear();
        g_cp = -1;
        SF6CoreArg on{}; on.type = SF6CORE_ARG_BOOL; on.v.i = 1;
        a->obj_invoke(C(), c, "set_DisableMessageTag(System.Boolean)", &on, 1, nullptr);   // a name is shown as typed
        a->obj_invoke(C(), c, "set_AutoRegionFit(System.Boolean)", &on, 1, nullptr);      // the region is the text
    }
    if (cp != g_cp) {
        SF6CoreArg pc{}; pc.type = SF6CORE_ARG_I64; pc.v.i = cp;
        if (a->obj_invoke(C(), c, "set_ControlPoint(via.gui.ControlPoint)", &pc, 1, nullptr) == SF6CORE_OK) g_cp = cp;
    }
    if (name != g_written && a->ui_set_text(C(), c, name.c_str()) == SF6CORE_OK) g_written = name;
    a->ui_set_scale(C(), c, b.sx * t.sx, b.sy * t.sy, 1.0f);           // compared first by the core
    a->ui_set_position(C(), c, x, y, t.z);

    // Into the bar's own text (its entries change with the focused row): hidden. The name's width is the copy's
    // drawn width while it is shown, else the bar's own glyph width times the name's glyphs.
    const double k = sc * b.sx * t.sx;
    const double text_r = left + k * tw;
    const double bar_glyphs = glyphs(str(call_obj(text, "get_Message")));
    double nw = bar_glyphs > 0 ? tw / bar_glyphs * glyphs(name) : 0;
    if (g_shown && name == g_written) {           // the copy: reached by name under its live parent (R35)
        float cw = 0;
        const Ret r = call(reinterpret_cast<Obj*>(c), "get_DisplaySize");
        if (!r.exception_thrown) std::memcpy(&cw, r.bytes.data(), sizeof cw);
        if (std::isfinite(cw) && cw > 0) { g_meas_name = name; g_meas_w = cw; }
    }
    if (name == g_meas_name && g_meas_w > 0) nw = g_meas_w;   // measured once drawn: no show / hide back and forth
    const double name_l = right - k * nw;
    const bool overlap = tw > 0 && nw > 0 && name_l < text_r + kGap;
    char geo_s[200];
    std::snprintf(geo_s, sizeof geo_s, "canvas %.0f, bar text %.0f-%.0f, name %.0f-%.0f (cp %d, scale %.2f)",
                  canvas, left, text_r, name_l, right, cp, b.sx * t.sx);
    if (g_geo_log != geo_s && (!g_shown || overlap)) { g_geo_log = geo_s; logf("fs name: '%s': %s", name.c_str(), geo_s); }
    if (name.empty() || overlap) { hide(name.empty() ? "no name" : "it would run into the bar's text"); return; }
    a->ui_set_force_invisible(C(), c, 0);         // the template's lock, copied with it
    a->ui_set_visible(C(), c, 1);
    if (!g_shown) { g_shown = true; logf("fs name: shown '%s'", name.c_str()); }
}

void update(Obj* agent) {
    if (!bar_live(true) || g_frame - g_bar_at > 60) {
        g_bar_at = g_frame;
        Bar nb;
        const bool ok = find_bar(nb);
        if (!ok || nb.parent != g_bar.parent) { hide("the guide bar changed"); g_pass_at = 0; }   // with the old bar
        if (!ok) return;
        g_bar = nb;
    }
    if (g_frame - g_name_at >= kNameFrames || g_name_at == 0) {
        g_name_at = g_frame;
        const std::string n = read_name(agent);
        if (n != g_name) { g_name = n; g_pass_at = 0; }
    }
    if (g_pass_at && g_frame - g_pass_at < kPassFrames) return;
    g_pass_at = g_frame;
    pass(g_name);
}

} // namespace

void install_hooks() {
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, opt_window_open)) {
        logf("fs name: the core has no ui_clone_control (0.8.2) or no core: the outfit name on Fighter Settings is off");
        return;
    }
    auto* td = api().tdb()->find_type("app.UIPartsInputGuideList");
    Method* m = td ? method_of(td, "OnLateUpdate", 0) : nullptr;
    if (!m || !m->get_function_raw() || !m->add_hook(pre_guide_late, post_guide_late, false)) {
        logf("fs name: app.UIPartsInputGuideList.OnLateUpdate %s: the outfit name on Fighter Settings is off", td ? "not hooked" : "not found");
        return;
    }
    g_on = true;
    logf("fs name: on (the copy is made in app.UIPartsInputGuideList.OnLateUpdate)");
}

bool busy() { return g_on && (g_bs || g_shown); }

void tick(uint32_t ev) {
    if (!g_on) return;
    try {
        const int st = g_req_status.load(std::memory_order_acquire);
        if (st != g_status_log) {
            g_status_log = st;
            logf("fs name: copy %s (status %d%s)", st == SF6CORE_OK ? "made" : "not made", st,
                 st == -3000 ? ": the bar's parts not found from its list" : st == -2000 ? ": exception" : "");
        }
        if (g_refused.load() >= kMaxRefused && !g_off_logged) {
            g_off_logged = true;
            logf("fs name: %d copies refused: no new copy this session", kMaxRefused);
        }
        if (ev & EV_SELECT_START) g_select = true;
        if (ev & EV_SELECT_END) g_select = false;
        if (ev & EV_MENU_OPEN) { g_bs = true; g_bs_closed = false; g_bs_seen = g_frame; }
        if ((ev & EV_MENU_CLOSE) && g_bs) g_bs_closed = true;
        if (!g_bs || g_select) { hide(g_select ? "character select" : "Battle Settings closed"); return; }
        Obj* agent = fs_agent();
        if (!agent || !visible(agent)) {
            // closed (HidObject also comes when a sub-screen hides it, rto.cpp): followed until gone 3 s
            if (g_bs_closed && g_frame - g_bs_seen > 180) { g_bs = false; g_bar = Bar{}; g_name.clear(); g_name_at = 0; }
            hide("Fighter Settings not on screen");
            return;
        }
        g_bs_seen = g_frame;
        if (A()->opt_window_open()) { hide("an option window is open"); return; }
        update(agent);
    } catch (...) {
        logf("fs name: EXCEPTION in tick");      // g_shown kept: the next frames still hide it
    }
}

} // namespace slots::fsname
