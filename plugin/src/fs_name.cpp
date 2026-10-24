// SF6 Slots — the name of the outfit on Fighter Settings' preview, at the right end of the bottom input guide bar
// (docs/RUNTIME_OPTIONS.md, "Outfit name on Fighter Settings").
//
// Battle Settings > character (Fighter Settings, agent MatchingFighterSetting): the costume row shows the game's
// name of a slot ("Outfit VII"); the bar's line gets, right-aligned, the outfit's real name, the one the options
// menu's title shows: an outfit of the options manifest = its label (the mod's name, "Aloha Ryu"; a game outfit
// with options: its name, "Outfit 1"), another of ours = its label in outfits.json, any other game outfit = the
// costume row's own text (what the game shows).
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
#include "rto.hpp"

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
constexpr int kQuickPasses = 10;                  // passes on the next frames while the name's drawn width is not read yet

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

bool g_on = false;                                // installed (core with ui_clone_control, hook in place)
bool g_off_logged = false, g_lost_logged = false;
bool g_bs = false, g_bs_closed = false, g_select = false;
uint32_t g_bs_seen = 0;
bool g_fs_vis = false;                            // Fighter Settings on screen at the last frame
uint32_t g_fs_since = 0;                          // the frame it came on screen
int g_fid = -1;                                   // its fighter, kept while it is up (a screen shows one fighter)

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
int g_quick = 0;                                  // passes left on the next frames (the drawn width not read yet)

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
// The fighter and outfit Fighter Settings shows: an app.UIPartsMatchingFighterSetting (the screen's parts list, else
// its flow's settings, as the Training popup keeps one), else the fighter of the preview model built since the
// screen came up (kept for the screen: it shows one fighter) and the outfit found by the costume row's name (one of
// ours, or a game outfit of the options manifest). On 20/10 the screen's parts list had no such part.
struct Sel { int fid = -1, cos = -1; const char* how = "nothing"; };
bool mfs_read(Obj* mfs, Sel& s, const char* how) {
    if (!alive(mfs) || type_name(mfs) != "app.UIPartsMatchingFighterSetting") return false;
    int32_t f = -1;
    if (!get_i32(mfs, "FighterId", f) || f <= 0) return false;
    const Ret r = call(mfs, "GetCostumeId");
    if (r.exception_thrown) return false;
    s.fid = f; s.cos = static_cast<int32_t>(r.dword); s.how = how;
    return true;
}
Sel fs_selection(Obj* agent, const std::string& row) {
    Sel s;
    Obj* lst = get_obj(get_obj(agent, "_PartsManager"), "_List");
    for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
        Obj* part = list_item(lst, k);
        if (part && type_name(part) == "app.UIPartsMatchingFighterSetting" && mfs_read(part, s, "the screen's part")) return s;
    }
    Obj* flow = get_obj(get_obj(agent, "<InputDelegates>k__BackingField"), "<Flow>k__BackingField");
    Obj* prm = alive(flow) ? get_obj(flow, "_Param") : nullptr;
    if (alive(prm) && mfs_read(get_obj(prm, "MatchingFighterSetting"), s, "its flow's settings")) return s;
    uint32_t at = 0;
    const int f = rto::preview_fighter(&at);
    if (f > 0 && at >= g_fs_since) g_fid = f;
    if (g_fid <= 0) return s;
    s.fid = g_fid; s.how = "the preview's fighter, the outfit by the costume row's name";
    s.cos = costumes::slot_by_name(g_fid, row);
    if (s.cos < 0 && rto::on())
        for (auto& k : rto::manifest().slots) if (k.fighter == g_fid && k.name == row) { s.cos = k.costume_no; break; }
    return s;
}
std::string read_name(Obj* agent) {
    const SF6CoreAPI* a = A();
    Obj* cm = call_obj(agent, "get_ControlMain");
    Obj* spin = cm ? reinterpret_cast<Obj*>(a->ui_find_cached(cm, "p_Spin_costume")) : nullptr;
    Obj* text = spin ? reinterpret_cast<Obj*>(a->ui_find_cached(spin, "e_text")) : nullptr;
    std::string row = text ? str(call_obj(text, "get_Message")) : std::string();
    if (const size_t at = row.find("  <ICON"); at != std::string::npos) row.resize(at);   // a key icon of ours (rto_menu.cpp)
    const Sel sel = fs_selection(agent, row);
    // the options menu's title: the manifest's label, else its name (rto_menu.cpp); else our outfits.json label
    std::string label;
    const char* what = "the costume row's text (the game's)";
    if (sel.fid > 0 && sel.cos >= 0) {
        if (rto::on())
            if (const rto::Slot* k = rto::manifest().slot_of(sel.fid, sel.cos)) {
                label = k->label.empty() ? k->name : k->label;
                what = k->game ? "the options manifest (a game outfit)" : "the options manifest";
            }
        if (label.empty()) {
            label = costumes::slot_label(sel.fid, sel.cos);
            if (!label.empty()) what = "our outfit's label (outfits.json)";
        }
    }
    char src[256];
    if (sel.fid <= 0) std::snprintf(src, sizeof src, "fighter not found (no settings part, no preview model yet): %s", what);
    else std::snprintf(src, sizeof src, "F%d outfit %d from %s: %s", sel.fid, sel.cos, sel.how, what);
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

// The bottom guide's left margin (Wael 20/10: the name ends as far from the screen's right edge as the guide text
// "Set your chosen character." starts from its left edge, about 60 at 1080p): the leftmost visible text drawn in the
// bottom band of the screen (its lowest 15 %), left half, under the guide bar's agent or Fighter Settings', in root
// space. Read every 60 frames (a walk of both trees); 0: none found.
Obj* g_fs_ag = nullptr;                           // Fighter Settings' agent, this frame (update)
double g_guide_l = 0;
uint32_t g_guide_at = 0;
std::string g_guide_log;
bool shown_el(Obj* o) {
    bool ok = false;
    const bool v = call_bool(o, "get_ActualVisible", {}, &ok);
    if (ok) return v;
    return call_bool(o, "get_Visible", {}, &ok) && ok;
}
void guide_walk(Obj* o, int depth, int& budget, double canvas, double& best, std::string& what) {
    for (; o && budget > 0; o = call_obj(o, "get_Next")) {
        --budget;
        if (!shown_el(o)) continue;
        if (type_name(o) == "via.gui.Text") {
            const std::string nm = str(call_obj(o, "get_Name"));
            const std::string msg = str(call_obj(o, "get_Message"));
            float gp[3] = {}, gs[3] = { 1, 1, 1 };
            Geo g;
            if (nm != kCopyName && !msg.empty() && floats(o, "get_GlobalPosition", gp, 2) && geo(o, g)) {
                if (!floats(o, "get_GlobalScale", gs, 2) || !(std::fabs(gs[0]) > 1e-3)) gs[0] = 1;
                const double l = gp[0] - (g.cp % 4) * 0.5 * width_of(o) * gs[0];
                if (gp[1] > canvas * 9 / 16 * 0.85 && l >= 8 && l < canvas / 2 && (best <= 0 || l < best)) {
                    best = l;
                    what = nm + " '" + msg.substr(0, 40) + "'";
                }
            }
        }
        if (depth < 12) guide_walk(call_obj(o, "get_Child"), depth + 1, budget, canvas, best, what);
    }
}
double guide_left(double canvas) {
    if (g_guide_at && g_frame - g_guide_at < 60) return g_guide_l;
    g_guide_at = g_frame;
    double best = 0;
    std::string what;
    int budget = 1500;
    for (Obj* ag : { g_bar.agent, g_fs_ag })
        if (Obj* cm = ag ? call_obj(ag, "get_ControlMain") : nullptr) guide_walk(call_obj(cm, "get_Child"), 0, budget, canvas, best, what);
    g_guide_l = best;
    char s[200];
    std::snprintf(s, sizeof s, "%.0f (%s)", best, best > 0 ? what.c_str() : "no text found");
    if (g_guide_log != s) { g_guide_log = s; logf("fs name: the guide's left margin: %s", s); }
    return best;
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
    if (!(left >= 16 && left <= canvas * 0.25)) left = 96;
    // the guide text's margin mirrored right; the bar text's when the guide is not found
    const double gl = guide_left(canvas);
    const double right = canvas - (gl >= 16 && gl <= canvas * 0.25 ? gl : left);

    // The name's drawn width: the copy's own once drawn (get_DisplaySize), else the bar's glyph width times the name's
    // glyphs (then the next frames read it and place again)
    const double k = sc * b.sx * t.sx;                                      // copy units -> root space
    const double bar_glyphs = glyphs(str(call_obj(text, "get_Message")));
    if (g_shown && name == g_written) {           // the copy: reached by name under its live parent (R35)
        float cw = 0;
        const Ret r = call(reinterpret_cast<Obj*>(c), "get_DisplaySize");
        if (!r.exception_thrown) std::memcpy(&cw, r.bytes.data(), sizeof cw);
        if (std::isfinite(cw) && cw > 0) { g_meas_name = name; g_meas_w = cw; }
    }
    const bool measured = name == g_meas_name && g_meas_w > 0;   // no show / hide back and forth once measured
    const double nw = measured ? g_meas_w : bar_glyphs > 0 ? tw / bar_glyphs * glyphs(name) : 0;
    // Placed by its LEFT point at (right - drawn width): a text is laid out from the left of its region, and the copy
    // keeps the template's region (about the bar's width). Placed by its right point it was drawn at that region's
    // far left, off screen (20/10: "...ch Wea" left of "Esc Back" while the name was meant to end at x 1759).
    const double name_l = right - k * nw;
    const float x = static_cast<float>((name_l - off) / sc);
    const float y = static_cast<float>(b.y + b.sy * t.y);
    const int cp = (t.cp / 4) * 4;                                          // the bar text's row, its left point

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

    // Into the bar's own text (its entries change with the focused row): hidden
    const double text_r = left + k * tw;
    const bool overlap = tw > 0 && nw > 0 && name_l < text_r + kGap;
    // Logged once per change: the rectangle meant (root space), how it was written, and where the copy was found
    // drawn (its region and global position, as they were before this pass's writes)
    float gp[3] = {}, rs[2] = {};
    const bool have_gp = floats(reinterpret_cast<Obj*>(c), "get_GlobalPosition", gp, 2);
    const bool have_rs = floats(reinterpret_cast<Obj*>(c), "get_RegionSize", rs, 2);
    char geo_s[400];
    std::snprintf(geo_s, sizeof geo_s, "canvas %.0f, bar text %.0f-%.0f, name %.0f-%.0f (width %.0f %s, cp %d, x %.1f y %.1f "
                  "in a parent at %.1f scale %.2f, copy scale %.2f; found at global x %s, region %s)",
                  canvas, left, text_r, name_l, right, k * nw, measured ? "drawn" : "estimated", cp, x, y, off, sc,
                  b.sx * t.sx, have_gp ? std::to_string(static_cast<int>(std::lround(gp[0]))).c_str() : "?",
                  have_rs ? (std::to_string(static_cast<int>(std::lround(rs[0]))) + "x" +
                             std::to_string(static_cast<int>(std::lround(rs[1])))).c_str() : "?");
    if (g_geo_log != geo_s) { g_geo_log = geo_s; logf("fs name: '%s': %s", name.c_str(), geo_s); }
    if (name.empty() || overlap) { hide(name.empty() ? "no name" : "it would run into the bar's text"); return; }
    a->ui_set_force_invisible(C(), c, 0);         // the template's lock, copied with it
    a->ui_set_visible(C(), c, 1);
    if (!g_shown) { g_shown = true; logf("fs name: shown '%s'", name.c_str()); }
    if (!measured && g_quick < kQuickPasses) ++g_quick;   // read its drawn width on the next frames, then place again
    else g_quick = kQuickPasses;
}

void update(Obj* agent) {
    g_fs_ag = agent;
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
        if (n != g_name) { g_name = n; g_pass_at = 0; g_quick = 0; }
    }
    if (g_pass_at && g_frame - g_pass_at < kPassFrames && !(g_quick > 0 && g_quick < kQuickPasses)) return;
    g_pass_at = g_frame;
    pass(g_name);
}

} // namespace

// The hook on the guide list's update runs every frame for every guide bar (battle too), so it is not put at startup:
// the first Battle Settings opening installs it (tick). No unhook exists on plugin API 1.10: it stays after.
bool g_ready = false;                             // the core and the method are there, the hook not installed yet
void hook_now() {
    g_ready = false;
    auto* td = api().tdb()->find_type("app.UIPartsInputGuideList");
    Method* m = td ? method_of(td, "OnLateUpdate", 0) : nullptr;
    if (!m || !m->get_function_raw() || !m->add_hook(pre_guide_late, post_guide_late, false)) {
        logf("fs name: app.UIPartsInputGuideList.OnLateUpdate %s: the outfit name on Fighter Settings is off", td ? "not hooked" : "not found");
        return;
    }
    g_on = true;
    logf("fs name: on (the copy is made in app.UIPartsInputGuideList.OnLateUpdate)");
}

void install_hooks() {
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, opt_window_open)) {
        logf("fs name: the core has no ui_clone_control (0.8.2) or no core: the outfit name on Fighter Settings is off");
        return;
    }
    g_ready = true;
}

bool busy() { return g_on && (g_bs || g_shown); }

void tick(uint32_t ev) {
    if (!g_on) {
        if (!g_ready || !(ev & EV_MENU_OPEN)) return;      // hooked the first time Battle Settings opens
        hook_now();
        if (!g_on) return;
    }
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
        if (!g_bs || g_select) { g_fs_vis = false; hide(g_select ? "character select" : "Battle Settings closed"); return; }
        Obj* agent = fs_agent();
        const bool fs_vis = agent && visible(agent);
        if (fs_vis && !g_fs_vis) { g_fs_since = g_frame; g_fid = -1; }   // a new screen: its fighter found again
        g_fs_vis = fs_vis;
        if (!fs_vis) {
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
