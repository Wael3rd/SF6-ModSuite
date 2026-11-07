// ui/bug_report — "Report a bug" on Battle Settings > Character (Fighter Settings), its Character tab: R / pad X (the game's input 341).
// The key takes a picture of the game (report/capture.hpp) and opens a page of the core's option window, docked on the side as the Color Mod's Edit Drive Colors, the fighter
// in sight. Its rows are real actions: STATUS, then what the status allows. Under them: Character, Outfit, Description; the messages are on the guide's line (report_logic.hpp has
// the rules). The status is asked of the server when the menu opens (the costume's key alone leaves). Send gathers it all off the game thread (report/gather.hpp) and posts it
// through the Community client: the Steam-verified token identifies the player, the server attaches his Steam ID. Back sends nothing and keeps nothing: the picture lives in memory
// only, and goes with the window.
//
// Rules (docs: report_mod/README.md):
//   - On this screen only: its agent drawn, its tab 0, its costume row shown: the agent and its main control stay drawn and ActualVisible
//     on the Titles and Profile tabs, so they prove nothing alone. To add a screen later: another screen_read(), the rest does not change.
//   - The entry is a copy of the guide bar's item template on the resident InputGuide's bar, which every screen shows: a copy left visible
//     comes back with that bar everywhere. So hiding it never depends on the bar being on screen, and goes over every bar of every InputGuide
//     (profile_mod.cpp's two lessons, 07/11). The copy is made inside the screen's own update, the post of its Main.OnUpdate (R19), found again by
//     name (R35), written from the frame through the core (R1).
//   - Asleep until the screen itself runs (Wael, 07/11): no poll, no timer, nothing per frame anywhere, menus, Training and fights alike. The one
//     hook is on this screen's own app.UIFlowMatchingFighterSetting.Main.OnUpdate, which the game calls only while this screen's flow is in its main
//     state (R18: hooked through mech::hooks' code check, so a game update that folds it refuses the hook: no entry, no cost). Its first call
//     wakes the module: it gives our core client a frame callback; when the calls stop (kLeftFrames) and no report is under way, the frame callback
//     hands itself back. No state OnEnter / OnExit hook.
//   - It works wherever the screen is opened from (main menu, the Multi Menu in Training, the hub) and owes nothing to the Recording Manager: it runs on its own frame callback, and
//     runs lang::frame and dlg::frame itself.
//   - Until the first wake of a session the DLL is that one hook and one atomic load per Present. The first wake puts the other hooks in, one per frame (REFramework stops the game's
//     threads while it patches), only on frames where the screen's update ran; no entry and no key until they are all in. They then stay for the session (report_mod/README.md).
//   - Encoding and HTTP off the game thread. No real player's id or name in the journal: counts and codes only.
//   - No squeezed text: the rows are cut with "...", the entry's region is the text's.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "common.hpp"
#include "community/auth.hpp"
#include "community/client.hpp"
#include "community/community.hpp"
#include "../../plugin/src/scroll_logic.hpp"
#include "game/gm.hpp"
#include "model/chars.hpp"
#include "perf.hpp"
#include "report/capture.hpp"
#include "report/gather.hpp"
#include "report/report_logic.hpp"
#include "report_mod.hpp"
#include "ui/details_logic.hpp"
#include "ui/dialogs.hpp"
#include "ui/gx.hpp"
#include "ui/internal.hpp"
#include "ui/lang.hpp"
#include "ui/logic.hpp"

namespace ui::bug_report {
namespace {

namespace gm = game::gm;
namespace lg = ui::logic;
namespace rp = ::report;
namespace cap = ::report::capture;
using gx::MO;

static_assert(rp::kTextMaxUnits == ui::details::kDescMaxUnits, "edit_result cuts to kDescMaxUnits");

constexpr const char* kAgent = "MatchingFighterSetting";
constexpr const char* kCopyName = "e_text_crsm_report";          // not "p_...": never taken for a UI part (R20)
constexpr const char* kGroupKey = "CrsmBugReport";
constexpr const char* kFlowParam = "app.UIFlowMatchingFighterSetting.Param";
constexpr uint64_t kHintPass = 10;                               // the entry placed, the bar read
constexpr uint64_t kShotWait = 20;                               // frames for the next Present to take the picture before the menu opens anyway
constexpr uint64_t kWindowWait = 120;                            // frames for the core to open the window
constexpr uint64_t kSettle = 12;                                 // frames after the text dialog before the page takes a press again (its Confirm)
constexpr uint64_t kEncodeWait = 300;                            // Send: frames the picture may still take
constexpr uint64_t kBusyWait = 900;                              // Send: frames another Community request may take
constexpr int kKeyId = 341;                                      // app.InputAssign.Digital.Id: R on the keyboard, X on the pad
constexpr int kModeGamePad = 1, kModeKeyboard = 2;               // app.InputGuideMode
constexpr int kMaxRefused = 3;                                   // copies refused in a session, then no entry

using F = cmn::Feature;
const SF6CoreAPI* api() { const SF6CoreAPI* a = gx::api(); return (a && SF6CORE_HAS(a, ui_clone_control) && SF6CORE_HAS(a, opt_status)) ? a : nullptr; }

// ---- small reads ----------------------------------------------------------------------------------------------------------------
bool actual_visible(MO* c) { bool v = false; return c && gx::call_bool(c, "get_ActualVisible", &v) && v; }

std::string symbol_of(int mode) {
    static std::string cache[3];
    if (mode < 0 || mode > 2) mode = kModeKeyboard;
    if (!cache[mode].empty()) return cache[mode];
    MO* mgr = gx::singleton("app.InputGuideManager");
    reframework::InvokeRet r{};
    if (!mgr || !gm::call(mgr, "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)",
                          { gm::arg_i32(kKeyId), gm::arg_i32(mode), gm::arg_i32(0), gm::arg_i32(0) }, &r) || !r.ptr) return "";
    return cache[mode] = gx::str(r.ptr);
}
int guide_mode() { return ui::com::guide_mode_pad().value_or(false) ? kModeGamePad : kModeKeyboard; }

// ---- the guide bar's entry -------------------------------------------------------------------------------------------------------
struct Bar { MO* agent = nullptr; MO* part = nullptr; MO* btn = nullptr; MO* parent = nullptr; };
Bar g_bar;
struct { bool shown = false; uintptr_t styled = 0; std::string written; int cp = -1; uint64_t pass_at = 0, bar_at = 0; float region = 0; } g_hint;
std::atomic<bool> g_req_on{false};
std::atomic<uintptr_t> g_req_part{0};
std::atomic<uintptr_t> g_made_parent{0};
std::atomic<int> g_refused{0};
// A one-argument setter by its name: the core's obj_invoke finds a full signature exactly (drill_details' set_prop). Not per frame.
bool set_by_name(MO* o, const char* name, SF6CoreArg v) {
    auto* td = o ? o->get_type_definition() : nullptr;
    auto* m = td ? gm::method_n(td, name, 1) : nullptr;
    if (!m) return false;
    const auto params = m->get_params();
    auto* pt = params.size() == 1 ? reinterpret_cast<reframework::API::TypeDefinition*>(params[0].t) : nullptr;
    return pt && gx::invoke(o, (std::string(name) + "(" + pt->get_full_name() + ")").c_str(), { v });
}
// The bar's template (e_text_0) and its entries (e_text_0_g_*: the mods', e_text_0_d_*: the game's)
MO* bar_template(MO* btn, std::vector<MO*>* entries) {
    MO* tmpl = nullptr;
    int n = 0;
    for (MO* c = btn ? gx::first_child(btn) : nullptr; c && n < 64; c = gx::next(c), ++n) {
        const std::string nm = gx::name(c);
        if (nm == "e_text_0") tmpl = c;
        else if (entries && (nm.rfind("e_text_0_g_", 0) == 0 || nm.rfind("e_text_0_d_", 0) == 0)) entries->push_back(c);
    }
    return tmpl;
}
MO* list_btn(MO* part) {
    const SF6CoreAPI* a = api();
    MO* ctrl = gx::ref(part, "<Control>k__BackingField");
    return (a && ctrl) ? static_cast<MO*>(perf::counted(a)->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
}
void make_copy() {                               // inside the screen's own update, on the list the frame found just now
    const uintptr_t part = g_req_part.exchange(0);
    g_req_on.store(false);
    const SF6CoreAPI* a = api();
    if (!part || !a || !gm::is_managed(reinterpret_cast<void*>(part))) return;
    MO* btn = list_btn(reinterpret_cast<MO*>(part));
    std::vector<MO*> entries;
    MO* tmpl = bar_template(btn, &entries);
    MO* parent = btn ? gx::parent(btn) : nullptr;
    if (!parent || !(tmpl || !entries.empty())) { ++g_refused; return; }
    if (perf::counted(a)->ui_find_child(parent, kCopyName, 0)) return;
    void* c = nullptr;
    const SF6CoreStatus st = perf::counted(a)->ui_clone_control(gx::cl(), tmpl ? tmpl : entries.front(), parent, kCopyName, &c);
    if (st != SF6CORE_OK || !c) { ++g_refused; cmn::journal("bug report: guide entry refused (%d)", (int)st); return; }
    g_made_parent.store(reinterpret_cast<uintptr_t>(parent));
    gx::invoke(static_cast<MO*>(c), "set_MaskType(via.gui.MaskType)", { gx::I(1) });   // NonTarget: no mask of the new parent clips it
    perf::counted(a)->ui_set_visible(gx::cl(), c, 0);   // placed and shown by the next frame
}
bool find_bar(Bar& b) {                          // the InputGuide on screen that has entries
    const SF6CoreAPI* a = api();
    b = Bar{};
    void* buf[4] = {};
    const uint32_t n = a ? perf::counted(a)->ui_agents("InputGuide", buf, 4) : 0;
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        MO* ag = static_cast<MO*>(buf[i]);
        if (!actual_visible(gx::call_obj(ag, "get_ControlMain"))) continue;
        MO* lst = gx::ref(gx::ref(ag, "_PartsManager"), "_List");
        for (int k = 0, cnt = gm::list_count(lst); k < cnt; ++k) {
            MO* part = gm::list_item(lst, k);
            if (!part || !gx::is_type(part, "app.UIPartsInputGuideList")) continue;
            MO* btn = list_btn(part);
            std::vector<MO*> entries;
            bar_template(btn, &entries);
            if (entries.empty()) break;
            MO* parent = gx::parent(btn);
            if (!parent) break;
            b = { ag, part, btn, parent };
            return true;
        }
    }
    return false;
}
bool bar_live() {
    const SF6CoreAPI* a = api();
    if (!a || !g_bar.agent) return false;
    void* buf[4] = {};
    const uint32_t n = perf::counted(a)->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i)
        if (buf[i] == g_bar.agent) return actual_visible(gx::call_obj(g_bar.agent, "get_ControlMain")) && gm::is_managed(g_bar.parent) && gm::is_managed(g_bar.btn);
    return false;
}
// The copy, on every bar of every InputGuide, on screen or not: found by name under each live bar (R35)
void hint_hide() {
    const SF6CoreAPI* a = api();
    if (!g_hint.shown || !a) { g_hint.shown = false; return; }
    g_hint.shown = false;
    void* buf[4] = {};
    const uint32_t n = perf::counted(a)->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        MO* lst = gx::ref(gx::ref(static_cast<MO*>(buf[i]), "_PartsManager"), "_List");
        for (int k = 0, cnt = gm::list_count(lst); k < cnt; ++k) {
            MO* part = gm::list_item(lst, k);
            if (!part || !gx::is_type(part, "app.UIPartsInputGuideList")) continue;
            MO* btn = list_btn(part);
            MO* parent = btn ? gx::parent(btn) : nullptr;
            if (void* c = parent ? perf::counted(a)->ui_find_child(parent, kCopyName, 0) : nullptr) perf::counted(a)->ui_set_visible(gx::cl(), c, 0);
        }
    }
}
// The game uses the key on this screen now: its own entries in the bar show its symbol (ours is not one of them). Read every kHintPass frames.
bool key_taken() {
    static uint64_t at = 0;
    static bool taken = false;
    const uint64_t t = tick();
    if (at && t - at < kHintPass) return taken;
    at = t;
    taken = false;
    if (!bar_live()) {
        Bar nb;
        if (!find_bar(nb)) return false;
        if (nb.parent != g_bar.parent) { hint_hide(); g_hint.pass_at = 0; }
        g_bar = nb;
    }
    const std::string sym = symbol_of(guide_mode());
    if (sym.empty()) return false;
    std::vector<MO*> entries;
    bar_template(g_bar.btn, &entries);
    for (MO* e : entries) if (gx::visible(e) && gx::msg(e).find(sym) != std::string::npos) { taken = true; break; }
    return taken;
}
struct Geo { float x = 0, y = 0, z = 0, sx = 1, sy = 1; int cp = 0; };
bool geo_of(MO* o, Geo& g) {
    float p[3] = {}, s[3] = {};
    if (!gx::vec3_of(o, "get_Position", p)) return false;
    g.x = p[0]; g.y = p[1]; g.z = p[2];
    if (gx::vec3_of(o, "get_Scale", s)) { g.sx = s[0]; g.sy = s[1]; }
    int32_t cp = 0;
    g.cp = gx::call_i32(o, "get_ControlPoint", &cp) ? cp : 0;
    if (g.cp < 0 || g.cp > 10 || g.cp % 4 > 2) g.cp = 0;
    return true;
}
void hint_pass(const std::string& text) {
    const SF6CoreAPI* a = api();
    std::vector<MO*> entries;
    bar_template(g_bar.btn, &entries);
    void* c = a ? perf::counted(a)->ui_find_child(g_bar.parent, kCopyName, 0) : nullptr;
    if (!c) {
        g_hint.shown = false;
        if (g_refused.load() >= kMaxRefused) return;
        if (g_made_parent.load() == reinterpret_cast<uintptr_t>(g_bar.parent)) return;   // never a second copy under one parent
        g_req_part.store(reinterpret_cast<uintptr_t>(g_bar.part));
        g_req_on.store(true, std::memory_order_release);
        return;
    }
    Geo b;
    if (!geo_of(g_bar.btn, b)) { hint_hide(); return; }
    struct Span { double l, r; Geo g; MO* o; };
    std::vector<Span> spans;
    float font_w = 0, font_h = 0;
    for (MO* e : entries) {
        if (!gx::visible(e)) continue;
        Geo g;
        float w = 0, h = 0;
        if (gx::msg(e).empty() || !gx::size_of(e, "get_DisplaySize", &w, &h) || !(w > 0) || !geo_of(e, g)) continue;
        const double l = g.x - (g.cp % 4) * 0.5 * w * g.sx;
        spans.push_back({ l, l + w * g.sx, g, e });
        gx::size_of(e, "get_FontSize", &font_w, &font_h);
    }
    if (spans.empty()) { hint_hide(); return; }
    std::sort(spans.begin(), spans.end(), [](const Span& p, const Span& q) { return p.l < q.l; });
    double gap = 0;
    for (size_t i = 1; i < spans.size(); ++i) {
        const double d = spans[i].l - spans[i - 1].r;
        if (d > 0 && (gap <= 0 || d < gap)) gap = d;
    }
    const Span& last = spans.back();
    if (gap <= 0) gap = font_w > 0 ? font_w * 3 : 40;     // one text holding every entry
    const float x = (float)(b.x + b.sx * (last.r + gap));
    const float y = (float)(b.y + b.sy * last.g.y);
    const int cp = (last.g.cp / 4) * 4;                   // that row, its left point
    MO* copy = static_cast<MO*>(c);
    if (reinterpret_cast<uintptr_t>(c) != g_hint.styled) {
        g_hint.styled = reinterpret_cast<uintptr_t>(c);
        g_hint.written.clear();
        g_hint.cp = -1;
        g_hint.region = 0;
        set_by_name(copy, "set_AutoRegionFit", gx::I(0));   // never squeezed: its region is its text's (below)
    }
    if (cp != g_hint.cp) { gx::invoke(copy, "set_ControlPoint(via.gui.ControlPoint)", { gx::I(cp) }); g_hint.cp = cp; }
    if (text != g_hint.written) { gx::set_text(copy, text); g_hint.written = text; }
    const float want_w = (float)lg::guide_text_w(text, font_w > 0 ? font_w : 28.0f) * 1.15f + 4.0f;
    if (font_h > 0 && std::abs(want_w - g_hint.region) > 0.5f) { gx::set_region(copy, want_w, font_h * 1.5f); g_hint.region = want_w; }
    perf::counted(a)->ui_set_scale(gx::cl(), c, b.sx * last.g.sx, b.sy * last.g.sy, 1.0f);
    gx::set_pos(copy, x, y, last.g.z);
    gx::set_force_invisible(copy, false);                 // the template's lock, copied with it
    gx::set_visible(copy, true);
    g_hint.shown = true;
}
void hint_frame(bool want) {
    if (!want) { hint_hide(); return; }
    const uint64_t t = tick();
    const std::string text = symbol_of(guide_mode()) + " " + ui::T("report_entry");
    if (!bar_live() || t - g_hint.bar_at > 60) {
        g_hint.bar_at = t;
        Bar nb;
        const bool ok = find_bar(nb);
        if (!ok || nb.parent != g_bar.parent) { hint_hide(); g_hint.pass_at = 0; }
        if (!ok) return;
        g_bar = nb;
    }
    if (g_hint.pass_at && t - g_hint.pass_at < kHintPass && text == g_hint.written && g_hint.shown) return;
    g_hint.pass_at = t;
    hint_pass(text);
}

// ---- the screen ------------------------------------------------------------------------------------------------------------------
// The agent of Fighter Settings really drawn, nullptr otherwise
MO* drawn_agent(MO** main) {
    const SF6CoreAPI* a = api();
    void* buf[2] = {};
    const uint32_t n = a ? perf::counted(a)->ui_agents(kAgent, buf, 2) : 0;
    for (uint32_t i = 0; i < n && i < 2; ++i) {
        MO* agent = static_cast<MO*>(buf[i]);
        bool draw = false;
        MO* go = gx::call_obj(agent, "get_GameObject");
        if (!go || !gx::call_bool(go, "get_DrawSelf", &draw) || !draw) continue;
        MO* cm = gx::call_obj(agent, "get_ControlMain");
        if (actual_visible(cm)) { *main = cm; return agent; }
    }
    return nullptr;
}
MO* costume_spin(MO* main) {
    const SF6CoreAPI* a = api();
    return a ? static_cast<MO*>(perf::counted(a)->ui_find_cached(main, "p_Spin_costume")) : nullptr;
}
// Everything entry_allowed asks of this screen (but the key's owner, read after: it needs the bar)
rp::ScreenRead screen_read(MO* agent, MO* main) {
    rp::ScreenRead s;
    s.agent_drawn = agent != nullptr;
    if (!agent) return s;
    int32_t tab = -1;
    if (MO* ti = gx::ref(agent, "_TabItem")) gx::call_i32(ti, "get_SelectedIndex", &tab);
    s.tab = tab;
    s.costume_row = actual_visible(costume_spin(main));
    int64_t input = 1;                           // (read in game 07/11: true on this screen's tabs, from the main menu and from Training's Multi Menu)
    s.input_on = !gx::get_i64(agent, "_IsEnableInput", &input) || input != 0;
    const SF6CoreAPI* a = api();
    s.window_open = !a || perf::counted(a)->opt_window_open() != 0;
    s.dialog_up = ui::dialogs::busy();
    s.can_report = community::on();
    return s;
}
std::string row_text(MO* main, const char* row) {
    const SF6CoreAPI* a = api();
    MO* spin = a ? static_cast<MO*>(perf::counted(a)->ui_find_cached(main, row)) : nullptr;
    MO* text = spin ? static_cast<MO*>(perf::counted(a)->ui_find_cached(spin, "e_text")) : nullptr;
    const std::string m = text ? gx::msg(text) : std::string();
    return m == "nil" ? std::string() : m;
}
// This screen's flow Param: UIFlowManager._Handles, by its exact type (the first one, read in game on every path)
MO* flow_param() {
    MO* hs = gx::ref(gx::singleton("app.UIFlowManager"), "_Handles");
    for (int i = 0, n = gm::list_count(hs); i < n; ++i) {
        MO* p = gx::ref(gm::list_item(hs, i), "<Param>k__BackingField");
        if (p && gm::is_managed(p) && gx::is_type(p, kFlowParam)) return p;
    }
    return nullptr;
}
// The preview model's name, "esf022v00": the only fighter this screen shows on most builds (read in game: its settings part is not there).
// On the press only: findComponents walks the scene.
int preview_fighter() {
    gx::TD* crf = gm::type("app.worldtour.CaptureRoomFighter");
    gx::TD* sm = gm::type("via.SceneManager");
    void* mgr = reframework::API::get()->get_native_singleton("via.SceneManager");
    if (!crf || !sm || !mgr) return -1;
    reframework::API::Method* get_scene = gm::method(sm, "get_CurrentScene");
    reframework::InvokeRet r{};
    if (!get_scene || !gm::call(reinterpret_cast<MO*>(mgr), get_scene, {}, &r) || !r.ptr) return -1;
    MO* scene = static_cast<MO*>(r.ptr);
    MO* type = reframework::API::get()->typeof(crf->get_full_name().c_str());
    reframework::InvokeRet lr{};
    if (!type || !gm::call(scene, "findComponents(System.Type)", { type }, &lr) || !lr.ptr) return -1;
    MO* list = static_cast<MO*>(lr.ptr);
    for (int i = 0, n = gm::list_count(list); i < n; ++i) {
        MO* go = gx::call_obj(gm::list_item(list, i), "get_FighterObj");
        const std::string nm = go ? gx::name(go) : std::string();
        if (nm.size() >= 6 && nm.compare(0, 3, "esf") == 0 && isdigit((unsigned char)nm[3]) && isdigit((unsigned char)nm[4]) && isdigit((unsigned char)nm[5]))
            return (nm[3] - '0') * 100 + (nm[4] - '0') * 10 + (nm[5] - '0');
    }
    return -1;
}
// The character and the outfit: the flow's Param has the fighter, and its part (an app.UIPartsMatchingFighterSettingCapture, whose base is the
// app.UIPartsMatchingFighterSetting of the character select) the outfit number; the preview model when they cannot be read.
void read_selection(rp::Snapshot* snap) {
    MO* prm = flow_param();
    MO* part = gx::ref(prm, "MatchingFighterSetting");
    int64_t f = -1;
    if (!(prm && gx::get_i64(prm, "FighterId", &f) && f > 0) && !(part && gx::get_i64(part, "FighterId", &f) && f > 0)) f = preview_fighter();
    snap->fighter = (int)f;
    int32_t c = -1;
    if (part && gx::call_i32(part, "GetCostumeId", &c) && c >= 0) snap->costume_no = c;
}

// ---- the report in progress -------------------------------------------------------------------------------------------------------
SF6CoreClient* g_opt_client = nullptr;           // our own core client for the option window
bool g_cb_set = false;
rp::Session g_session;
rp::Snapshot g_snap;
rp::MenuValues g_values;                         // what the text under the rows names
std::string g_key;                               // the costume key, once the files were read
uint64_t g_gen = 0;                              // this report: an answer of an earlier one is dropped
uint64_t g_shot_at = 0, g_window_deadline = 0, g_settle_until = 0, g_send_at = 0, g_busy_until = 0;
bool g_window_seen = false, g_dirty = false, g_naming = false, g_asked = false;
MO* g_blocked = nullptr;                         // the option dialog's agent, its input off under the text dialog
int64_t g_group = -1;
std::vector<int64_t> g_entries;                  // the rows now
std::vector<rp::Row> g_roles;                    // what each of them is
std::string g_built;                             // what the page was built from
std::atomic<bool> g_paste_on{false}, g_pasting{false};   // the text dialog is up; a Ctrl+V is being typed
std::atomic<int> g_pressed{-1};                  // a row's Confirm (the core's callback): run on the frame
std::atomic<int> g_chosen{-1};                   // STATUS moved (the core's callback): run on the frame
bool g_mark_due = false;                         // MARK AS FIXED pressed: goes out as soon as the client is free
// What is read off the game thread: the summary's values and the key when the menu opens, the whole command at Send
std::mutex g_prep_mx;
std::optional<nlohmann::json> g_prep;
bool g_prep_failed = false;
struct Preview { bool ready = false, failed = false; uint64_t gen = 0; std::string key, outfit; };
Preview g_preview;
enum class SendStep { None, WaitPicture, Preparing, WaitClient, Posted } g_step = SendStep::None;
uint64_t g_reports = 0, g_sent = 0, g_failed = 0, g_backed = 0;
// the key
bool g_up = false, g_pad_was = false;
uint32_t g_key_count = 0;
unsigned g_scan = 0;
uint64_t g_scan_at = 0;

// ---- the window on the side, as the Color Mod's Edit Drive Colors (colormod/src/dialogs/editor_core.cpp, apply_layout and fix_frame): the fighter stays in sight -----
constexpr float kDockX = -430.0f;                // docked left on Fighter Settings
struct Frame { float shadow_w, shadow_h, bg_w, bg_h, line_w, line_h, main_y, title_y, guide_y; };
constexpr Frame kFrame = { 1058, 892, 1000, 834, 990, 824, -282, -352, 262 };   // its 8-row page
// The text under the rows, strictly under the last one: GuideMessageText grown upward to just below it, its bottom kept, aligned LeftTop, its fit off: the game otherwise shrinks
// what does not fit. Read in game on 07/11 at 1080p: the rows at y 258 + 64 a row, 48 high; this text's bottom at 857; at its font x 1.1 it drew 38 px a line and 21 px per em of
// lg::text_em. So at x 0.9: 31 px a line: rp::kSummaryLines (12) in the 405 px under three rows. As wide as the rows, 800 px, the description's bar at their right edge: its text
// stops at kTextPx. The game never wraps this text (Wael's 78.png, 07/11: a line a little too wide for it broke again at a '.', each line cost two, and the text ran over the guide):
// its auto wrap is off, so the lines are exactly the ones made here, 12 at most, and it cannot grow past its place whatever the widths. The estimate (rp::wrap_px, a few % off either
// way depending on the text) starts with a margin; the game's own measure of what it shows then tightens it (measure_summary), and a clip stops what would still reach the bar.
// The rows' own descriptions stay empty: the game writes the focused row's there, and the same long text written twice is R7's crash (two rows with this text, or a page
// rebuilt, would do that). So this one place writes it, comparing the whole text first (the core's compare reads 511 bytes).
constexpr float kSummaryAreaH3 = 405.0f, kRowPitch = 64.0f, kSummaryScale = 0.9f;
constexpr float kTextPx = 784.0f, kSummaryRegionW = 4000.0f;   // (a region wider than any line: no wrap, and its real width can be read)
constexpr float kWrapMargin = 0.97f, kWrapScaleMin = 0.75f;

bool near_f(float a, float b, float eps) { return std::abs(a - b) <= eps; }
void dock_control(MO* cm) {
    float p[3] = {}, sc[3] = {};
    if (!gx::vec3_of(cm, "get_Position", p) || !near_f(p[0], kDockX, 0.5f) || !near_f(p[1], 0, 0.5f)) gx::set_pos(cm, kDockX, 0, 0);
    const SF6CoreAPI* a = api();
    if (a && (!gx::vec3_of(cm, "get_Scale", sc) || !near_f(sc[0], 1.0f, 0.001f))) perf::counted(a)->ui_set_scale(gx::cl(), cm, 1.0f, 1.0f, 1.0f);
}
void fix_frame(MO* cm) {
    const SF6CoreAPI* a = api();
    MO* dlg = a ? static_cast<MO*>(perf::counted(a)->ui_find_child(cm, "c_dialog", 6)) : nullptr;
    for (MO* c = dlg ? gx::first_child(dlg) : nullptr; c; c = gx::next(c)) {
        const std::string nm = gx::name(c);
        float w = 0, h = 0, cw = 0, ch = 0;
        if (nm == "e_scale9grid_shadow") { w = kFrame.shadow_w; h = kFrame.shadow_h; }
        else if (nm == "e_bg" || nm == "e_bg_cfn" || nm == "e_texture_dot") { w = kFrame.bg_w; h = kFrame.bg_h; }
        else if (nm == "e_scale9grid_line") { w = kFrame.line_w; h = kFrame.line_h; }
        if (w > 0 && (!gx::size_of(c, "get_Size", &cw, &ch) || !near_f(ch, h, 0.5f))) gx::set_size(c, w, h);
        float y = 0;
        bool has_y = true;
        if (nm == "p_Group_main_v_kc") y = kFrame.main_y; else if (nm == "e_title") y = kFrame.title_y; else if (nm == "p_InputGuideList_0") y = kFrame.guide_y; else has_y = false;
        float p[3] = {};
        if (has_y && gx::vec3_of(c, "get_Position", p) && !near_f(p[1], y, 0.5f)) gx::set_pos(c, p[0], y, p[2]);
    }
}
void set_clip(MO* t, float l, float top, float r, float b);
struct SummaryBox { MO* text = nullptr; bool ok = false; float pos[3] = {}; float w = 0, h = 0, fw = 0, fh = 0, top = 0; int32_t cp = -1; };
SummaryBox g_box;
void summary_box(MO* gmt, int rows) {
    if (g_box.text != gmt) {
        g_box = SummaryBox{};
        g_box.text = gmt;
        g_box.ok = gx::vec3_of(gmt, "get_Position", g_box.pos) && gx::size_of(gmt, "get_RegionSize", &g_box.w, &g_box.h) && gx::size_of(gmt, "get_FontSize", &g_box.fw, &g_box.fh) &&
                   gx::call_i32(gmt, "get_ControlPoint", &g_box.cp) && g_box.h > 1.0f && g_box.h < kSummaryAreaH3 - 1.0f && g_box.w > 100.0f && g_box.fh > 1.0f;
        set_by_name(gmt, "set_AutoRegionFit", gx::I(0));   // never squeezed: the lines are cut to the width, rp::kSummaryLines of them
        set_by_name(gmt, "set_PageAlignment", gx::I(0));   // LeftTop, under the rows whatever its length (the game's: at the bottom)
        set_by_name(gmt, "set_ForceAutoWrap", gx::B(false));
        set_by_name(gmt, "set_AutoWrap", gx::B(false));    // the lines are the ones made here: the game breaking one again is what ran over the guide
        if (g_box.cp % 4 == 0) set_clip(gmt, 0, -2000.0f, kTextPx, 4000.0f);   // (its left point: x 0. Width only: the lines cannot pass its place)
        bool wrap = true;
        const bool read = gx::call_bool(gmt, "get_AutoWrap", &wrap);
        cmn::journal("bug report: the text under the rows: %s (font %.1f, region %.0f x %.0f, at %.0f, point %d, auto wrap %s)", g_box.ok ? "placed" : "left as the game made it", g_box.fh, g_box.w, g_box.h, g_box.pos[1], g_box.cp, read ? (wrap ? "ON" : "off") : "not read");
    }
    if (!g_box.ok) return;
    const float area = kSummaryAreaH3 + (float)(3 - std::clamp(rows, 1, 3)) * kRowPitch;
    float y_new = 0;
    if (!lg::keep_bottom_y(g_box.cp, g_box.pos[1], g_box.h, area, &y_new)) return;
    g_box.top = y_new - (float)(g_box.cp / 4) * 0.5f * area;
    const float fw = g_box.fw * kSummaryScale, fh = g_box.fh * kSummaryScale;
    float w = 0, h = 0, p[3] = {};
    if (!gx::size_of(gmt, "get_FontSize", &w, &h) || !near_f(w, fw, 0.05f) || !near_f(h, fh, 0.05f)) gx::set_font(gmt, fw, fh);
    if (!gx::size_of(gmt, "get_RegionSize", &w, &h) || !near_f(w, kSummaryRegionW, 0.5f) || !near_f(h, area, 0.5f)) gx::set_region(gmt, kSummaryRegionW, area);
    const float x_new = slots::scroll::left_of(g_box.pos[0], g_box.cp, g_box.w) + (float)(g_box.cp % 4) * 0.5f * kSummaryRegionW;   // its left edge kept, under the rows'
    if (!gx::vec3_of(gmt, "get_Position", p) || !near_f(p[0], x_new, 0.5f) || !near_f(p[1], y_new, 0.5f)) gx::set_pos(gmt, x_new, y_new, g_box.pos[2]);
}
// The row Send is on (the third), dimmed while it does nothing: its text says why. The dialog's own row: put back before the window is forgotten, the pool may keep it.
constexpr const char* kSendUnit = "p_OptionUnit_2_switch_show";
void send_row_look(MO* cm, bool dim) {
    const SF6CoreAPI* a = api();
    MO* unit = a && cm ? static_cast<MO*>(perf::counted(a)->ui_find_cached(cm, kSendUnit)) : nullptr;
    if (!unit) return;
    const float want = dim ? 0.5f : 1.0f;
    float v[3] = {};
    if (gx::vec3_of(unit, "get_ColorScale", v) && near_f(v[0], want, 0.01f)) return;
    float f[4] = { want, want, want, want };
    SF6CoreArg arg{};
    arg.type = SF6CORE_ARG_PTR;
    arg.v.p = f;                                 // a value type goes by address
    gx::invoke(unit, "set_ColorScale(via.Float4)", { arg });
}
std::string g_summary;                           // what the text under the rows should say
std::string g_wrote, g_read_back;                // the last text written, and how the engine reads it back
bool g_read_back_due = false;
float g_wrap_scale = kWrapMargin;                // of kTextPx: what the lines are wrapped at; the game's measure tightens it
uint64_t g_measure_at = 0;
void write_summary(MO* gmt) {
    const std::string cur = gx::msg(gmt);
    if (g_read_back_due) { g_read_back_due = false; if (!cur.empty()) g_read_back = cur; }
    if (cur == g_summary || (g_wrote == g_summary && cur == g_read_back)) return;   // R7: never the same long text again, even if the engine spells it its own way
    gx::set_text(gmt, g_summary);
    g_wrote = g_summary;
    g_read_back_due = true;
    g_measure_at = tick() + 3;                   // its layout, a few frames later
}
// ---- the message and the description's bar: copies in the dialog's guide row ----------------------------------------------------------------------------------------------
// The message: a copy of the game's last guide entry (its style) on the guide's line, its right end at the window's right edge, never over the entries: past its room it slides as
// a long outfit name does (Position and a local ScissorRect, the same timing: plugin/src/scroll_logic.hpp). Measured by the game once its region is wide.
// The bar: two copies of a via.gui.Rect of the dialog, as the drill details' (the game's own via.gui.ScrollBar could not be looked at without the game), following scroll_top.
// R19: a copy is made only in the option dialog's own update (Community's post-hook calls dialog_update). R35: found again by name each time, hidden until placed.
constexpr const char* kMsgName = "e_text_crsm_report_msg";
constexpr const char* kTrackName = "e_rect_crsm_report_track";
constexpr const char* kThumbName = "e_rect_crsm_report_thumb";
constexpr float kMsgBeyond = 70.0f;              // the window's inner right edge: that far past the rows' right end (read in game: 930 and 1022 of its frame)
constexpr float kMeasureW = 4000.0f, kBarWidth = 8.0f;
constexpr float kSummaryLinePx = 31.0f;          // a line of the text under the rows, at its font
constexpr uint32_t kTrackColor = 0xCC463C3Cu, kThumbColor = 0xFFCC24CEu;   // the drill details' (0xAABBGGRR)
constexpr int kMaxCopies = 8, kCopyPriority = 10;
constexpr uint64_t kRowSettle = 12, kLayoutFrames = 3;
std::atomic<bool> g_copies_due{false}, g_want_bar{false};
std::atomic<void*> g_copy_param{nullptr};
int g_copy_tries = 0;
void* g_copy_for = nullptr;
struct GuideItem { MO* t = nullptr; float x = 0, y = 0, z = 0, w = 0, fw = 0; };
GuideItem last_item(MO* row) {                   // the game's entries now (e_text_0_d_*): the last one, left to right
    GuideItem best;
    int n = 0;
    for (MO* e = row ? gx::first_child(row) : nullptr; e && n < 64; e = gx::next(e), ++n) {
        if (gx::name(e).rfind("e_text_0_d_", 0) != 0 || !gx::is_type(e, "via.gui.Text") || !gx::visible(e) || gx::force_invisible(e)) continue;
        float p[3] = {}, rw = 0, rh = 0, f = 0, fh = 0;
        if (!gx::vec3_of(e, "get_Position", p) || !gx::size_of(e, "get_RegionSize", &rw, &rh) || !gx::size_of(e, "get_FontSize", &f, &fh)) continue;
        if (!best.t || p[0] > best.x) best = GuideItem{ e, p[0], p[1], p[2], rw, f };
    }
    return best;
}
MO* first_rect(MO* e, int depth) {
    int n = 0;
    for (MO* k = gx::first_child(e); k && n < 64; k = gx::next(k), ++n) {
        if (gx::is_type(k, "via.gui.Rect")) return k;
        if (depth < 6) if (MO* r = first_rect(k, depth + 1)) return r;
    }
    return nullptr;
}
MO* copy_in(MO* row, const char* name) {
    const SF6CoreAPI* a = api();
    return a && row ? static_cast<MO*>(perf::counted(a)->ui_find_child(row, name, 0)) : nullptr;
}
void ask_copies(void* param) {
    if (param != g_copy_for) { g_copy_for = param; g_copy_tries = 0; }
    if (g_copy_tries >= kMaxCopies) return;
    g_copy_param.store(param);
    g_copies_due.store(true);
}
void make_copies() {
    const SF6CoreAPI* a = api();
    void* pv = g_copy_param.load();
    g_copies_due.store(false);
    if (!a || !SF6CORE_HAS(a, ui_clone_control) || !pv || !gm::is_managed(pv)) return;
    MO* param = static_cast<MO*>(pv);
    MO* gmt = gx::ref(param, "GuideMessageText");
    MO* row = gmt ? gx::parent(gmt) : nullptr;
    if (!row) return;
    const auto clone = [&](MO* tmpl, const char* name) {
        ++g_copy_tries;                          // (a row being made again has no template yet: that is no try)
        void* made = nullptr;
        const SF6CoreStatus st = perf::counted(a)->ui_clone_control(gx::cl(), tmpl, row, name, &made);
        if (st == SF6CORE_OK && made) perf::counted(a)->ui_set_visible(gx::cl(), made, 0);
        cmn::journal("bug report: copy %s of %s: %s (%d)", name, gx::name(tmpl).c_str(), st == SF6CORE_OK && made ? "made" : "refused", (int)st);
    };
    if (!copy_in(row, kMsgName)) {
        const GuideItem last = last_item(row);
        if (last.t) clone(last.t, kMsgName);
    }
    if (g_want_bar.load() && (!copy_in(row, kTrackName) || !copy_in(row, kThumbName))) {
        MO* agent = gx::ref(param, "<Agent>k__BackingField");
        MO* cm = agent ? gx::call_obj(agent, "get_ControlMain") : nullptr;
        MO* rect = cm ? first_rect(cm, 0) : nullptr;
        if (!rect) { cmn::journal("bug report: no Rect in the dialog: no bar"); g_copy_tries = kMaxCopies; return; }
        if (!copy_in(row, kTrackName)) clone(rect, kTrackName);
        if (!copy_in(row, kThumbName)) clone(rect, kThumbName);
    }
}
void set_clip(MO* t, float l, float top, float r, float b) {
    float rect[4] = { l, top, r, b };
    SF6CoreArg arg{};
    arg.type = SF6CORE_ARG_PTR;
    arg.v.p = rect;                              // a value type goes by address
    gx::invoke(t, "set_ScissorRect(via.Rect)", { arg });
}
struct MsgLine {
    uintptr_t copy = 0;
    std::string text;
    bool measuring = false, shown = false, have_row = false;
    uint64_t asked_at = 0, seen_at = 0;
    ULONGLONG t0 = 0;
    float natural = 0, y = 0, z = 0, h = 0, entries_end = 0, seen_end = -1, last_left = 0;
    int last_off = -1;
};
MsgLine g_msg;
std::string g_message;                           // what the guide's line should say
void message_frame(MO* gmt, void* param) {
    namespace sc = slots::scroll;
    MO* row = gx::parent(gmt);
    MO* t = copy_in(row, kMsgName);
    if (g_message.empty()) {
        if (t && g_msg.shown) gx::set_visible(t, false);
        g_msg.shown = false;
        g_msg.text.clear();
        g_msg.measuring = false;
        return;
    }
    if (!t) { ask_copies(param); return; }
    if (reinterpret_cast<uintptr_t>(t) != g_msg.copy) {
        g_msg = MsgLine{};
        g_msg.copy = reinterpret_cast<uintptr_t>(t);
        set_by_name(t, "set_AutoRegionFit", gx::I(0));
        set_by_name(t, "set_AutoWrap", gx::B(false));
        set_by_name(t, "set_PageAlignment", gx::I(4));   // LeftCenter: placed by its left point, as a sliding name
    }
    const uint64_t now = tick();
    const GuideItem last = last_item(row);
    if (last.t) {                                // the row is made again at each cursor move: its end counts once it holds still
        float fw = 0;
        g_msg.y = last.y;
        g_msg.z = last.z;
        if (gx::size_of(last.t, "get_FontSize", &fw, &g_msg.h)) g_msg.have_row = true;
        const float end = last.x + last.w + last.fw;
        if (!near_f(end, g_msg.seen_end, 1.0f)) { g_msg.seen_end = end; g_msg.seen_at = now; }
        else if (now - g_msg.seen_at >= kRowSettle) g_msg.entries_end = end;
    }
    if (!g_box.ok || !g_msg.have_row || g_msg.entries_end <= 0) return;   // (hidden until the row is laid out)
    const float right = sc::left_of(g_box.pos[0], g_box.cp, g_box.w) + g_box.w + kMsgBeyond;
    if (g_message != g_msg.text) {
        g_msg.text = g_message;
        g_msg.natural = 0;
        g_msg.last_off = -1;
        g_msg.asked_at = now;
        g_msg.measuring = true;
        set_clip(t, 0, sc::kClipTop, 1, sc::kClipBottom);   // nothing shows while it is measured
        gx::set_pos(t, right, g_msg.y, g_msg.z);
        gx::set_text(t, g_message);
        gx::set_region(t, kMeasureW, g_msg.h * 1.5f);
        gx::set_force_invisible(t, false);       // the template's lock, copied with it
        gx::set_visible(t, true);
        g_msg.shown = true;
    }
    if (g_msg.measuring) {
        if (now - g_msg.asked_at < kLayoutFrames) return;
        float w = 0, h = 0;
        const bool got = gx::size_of(t, "get_DisplaySize", &w, &h) && std::isfinite(w) && w > 0;
        g_msg.natural = got ? w : (float)lg::guide_text_w(g_message, last.t ? last.fw : 28.0f) * 1.15f;
        gx::set_region(t, g_msg.natural + 4.0f, g_msg.h * 1.5f);
        g_msg.measuring = false;
        g_msg.t0 = GetTickCount64();
        cmn::journal("bug report: message %s: %.0f wide in %.0f", got ? "measured" : "estimated", g_msg.natural, right - g_msg.entries_end);
    }
    const float place = (std::max)(sc::kSpanMinWidth, right - g_msg.entries_end);
    const float width = (std::min)(g_msg.natural + 2.0f, place);
    const float left = right - width;
    const float overflow = g_msg.natural + 2.0f - width;
    const int off = sc::offset_at(overflow, (double)(GetTickCount64() - g_msg.t0) / 1000.0, sc::text_is_rtl(g_message));
    if (off == g_msg.last_off && near_f(left, g_msg.last_left, 0.5f)) return;   // only when the whole offset changes
    g_msg.last_off = off;
    g_msg.last_left = left;
    const sc::Window w = sc::window_at(left, width, off);
    set_clip(t, w.clip_left, w.clip_top, w.clip_right, w.clip_bottom);
    gx::set_pos(t, w.x, g_msg.y, g_msg.z);
}
rp::SummaryScroll g_scroll;                      // the description's window now
struct BarState { uintptr_t track = 0, thumb = 0; bool shown = false; float x = 0, y = 0, h = 0, th_y = 0, th_h = 0; };
BarState g_bar_state;
void bar_frame(MO* gmt, void* param) {
    MO* row = gx::parent(gmt);
    const bool want = g_box.ok && g_scroll.window > 0 && g_scroll.total > g_scroll.window;
    g_want_bar.store(want);
    MO* track = copy_in(row, kTrackName);
    MO* th = copy_in(row, kThumbName);
    if (!want) {
        if (g_bar_state.shown) { if (track) gx::set_visible(track, false); if (th) gx::set_visible(th, false); }
        g_bar_state.shown = false;
        return;
    }
    if (!track || !th) { ask_copies(param); return; }
    const SF6CoreAPI* a = api();
    if (!a) return;
    if (reinterpret_cast<uintptr_t>(track) != g_bar_state.track || reinterpret_cast<uintptr_t>(th) != g_bar_state.thumb) {
        g_bar_state = BarState{};
        g_bar_state.track = reinterpret_cast<uintptr_t>(track);
        g_bar_state.thumb = reinterpret_cast<uintptr_t>(th);
        int priority = kCopyPriority;            // above the backgrounds: a copy into another parent is drawn under them otherwise (row_look.cpp)
        for (MO* r : { track, th }) {
            set_by_name(r, "set_ControlPoint", gx::I(0));   // LeftTop
            perf::counted(a)->ui_set_scale(gx::cl(), r, 1.0f, 1.0f, 1.0f);
            gx::invoke(r, "set_Priority(System.UInt16)", { gx::I(priority++) });
            gx::set_force_invisible(r, false);
        }
        perf::counted(a)->ui_set_color(gx::cl(), track, kTrackColor);
        perf::counted(a)->ui_set_color(gx::cl(), th, kThumbColor);
    }
    const float x = slots::scroll::left_of(g_box.pos[0], g_box.cp, g_box.w) + g_box.w - kBarWidth;   // at the rows' right edge
    const float y = g_box.top + (float)g_scroll.first_line * kSummaryLinePx;
    const float h = (float)g_scroll.window * kSummaryLinePx;
    const ui::details::Thumb tb = ui::details::thumb(h, g_scroll.total, g_scroll.window, g_scroll.top);
    if (!near_f(x, g_bar_state.x, 0.25f) || !near_f(y, g_bar_state.y, 0.25f) || !near_f(h, g_bar_state.h, 0.25f)) {
        gx::set_size(track, kBarWidth, h);
        gx::set_pos(track, x, y, g_box.pos[2]);
    }
    if (!near_f(x, g_bar_state.x, 0.25f) || !near_f(y + tb.y, g_bar_state.th_y, 0.25f) || !near_f(tb.h, g_bar_state.th_h, 0.25f)) {
        gx::set_size(th, kBarWidth, tb.h);
        gx::set_pos(th, x, y + tb.y, g_box.pos[2]);
    }
    g_bar_state.x = x; g_bar_state.y = y; g_bar_state.h = h; g_bar_state.th_y = y + tb.y; g_bar_state.th_h = tb.h;
    if (!g_bar_state.shown) { gx::set_visible(track, true); gx::set_visible(th, true); g_bar_state.shown = true; }
}
// What the game says it draws: wider than kTextPx, or more lines than made (the game's wrap still on: a way this could fail): the lines made again tighter, and it looks again.
void measure_summary(MO* gmt) {
    if (!g_measure_at || tick() < g_measure_at) return;
    g_measure_at = 0;
    float w = 0, h = 0;
    if (g_wrote.empty() || !gx::size_of(gmt, "get_DisplaySize", &w, &h) || !std::isfinite(w) || !std::isfinite(h) || w <= 0) return;
    const float lines = (float)std::count(g_wrote.begin(), g_wrote.end(), '\n') + 1.0f;
    const bool wide = w > kTextPx, tall = h > (lines + 0.6f) * kSummaryLinePx * 1.15f;
    if ((!wide && !tall) || g_wrap_scale <= kWrapScaleMin) return;
    const float was = g_wrap_scale;
    g_wrap_scale = (std::max)(kWrapScaleMin, wide ? g_wrap_scale * kTextPx / w * 0.98f : g_wrap_scale * 0.95f);
    g_dirty = true;
    cmn::journal("bug report: the game draws %.0f x %.0f for %.0f lines: wrapped at %.3f, was %.3f", w, h, lines, g_wrap_scale, was);
}
// Every option dialog up (a page rebuilt: the old one may still be there, and the game cascades stacked dialogs): docked, its frame the big one
void dock_all() {
    const SF6CoreAPI* a = api();
    if (!a) return;
    void* agents[8] = {};
    const uint32_t n = perf::counted(a)->ui_agents("OptionDialog", agents, 8);
    for (uint32_t i = 0; i < n && i < 8; ++i)
        if (MO* cm = gx::call_obj(static_cast<MO*>(agents[i]), "get_ControlMain")) { dock_control(cm); fix_frame(cm); }
    if (void* param = perf::counted(a)->opt_current_param()) {
        if (MO* gmt = gx::ref(static_cast<MO*>(param), "GuideMessageText")) {
            summary_box(gmt, (int)g_roles.size());
            write_summary(gmt);
            measure_summary(gmt);
            message_frame(gmt, param);
            bar_frame(gmt, param);
        }
        MO* agent = gx::ref(static_cast<MO*>(param), "<Agent>k__BackingField");
        const bool send_shown = std::find(g_roles.begin(), g_roles.end(), rp::Row::Send) != g_roles.end();
        if (MO* cm = agent ? gx::call_obj(agent, "get_ControlMain") : nullptr) send_row_look(cm, send_shown && g_session.send_disabled() && g_session.busy() == rp::Busy::None);
    }
}

void SF6CORE_CALL cb_button(void*, int64_t entry, SF6CoreOptButtonResult* out) {
    if (out) out->kind = 0;                      // nothing more: the frame decides
    for (size_t i = 0; i < g_entries.size(); ++i) if (g_entries[i] == entry) { g_pressed.store((int)i); return; }
}
void SF6CORE_CALL cb_value(void*, int64_t entry, int64_t value) {
    if (!g_entries.empty() && g_entries[0] == entry) g_chosen.store((int)value - 1);    // STATUS: 1-based
}
void place_now(void* param) {                    // before the first draw of a dialog of ours
    if (g_session.stage() != rp::Stage::Open || !param) return;
    MO* agent = gx::ref(static_cast<MO*>(param), "<Agent>k__BackingField");
    if (MO* cm = agent ? gx::call_obj(agent, "get_ControlMain") : nullptr) { dock_control(cm); fix_frame(cm); }
}
void SF6CORE_CALL cb_created(void*, void* param) { try { place_now(param); } catch (...) {} }
void SF6CORE_CALL cb_page(void*, void* param, uint32_t rows) { if (rows) try { place_now(param); } catch (...) {} }

// Our own core client: registered at the start, nothing else until the screen runs. Its option callbacks when a report opens its window: they are what
// installs the core's option engine, which nothing should pay for before.
bool register_client() {
    g_opt_client = report_host::core_client();   // the DLL's one core client (host.cpp), made at the first wake
    return g_opt_client != nullptr;
}
bool ensure_client() {
    const SF6CoreAPI* a = api();
    if (!a || !register_client()) return false;
    if (!g_cb_set) {
        SF6CoreOptCallbacks cb{};
        cb.struct_size = sizeof cb;
        cb.on_button = cb_button;
        cb.on_value = cb_value;
        cb.on_dialog_created = cb_created;
        cb.on_page_setup = cb_page;
        g_cb_set = perf::counted(a)->opt_set_callbacks(g_opt_client, &cb) == SF6CORE_OK;
        if (!g_cb_set) cmn::journal("bug report: the core's option service refused the callbacks");
    }
    return g_cb_set;
}

std::string tr(const std::string& k, const std::vector<std::string>& args) { return ui::T(k, args); }
// The text under the rows, ready for the game
std::string g_scroll_text;
uint64_t g_scroll_since = 0;
std::string summary_now() {
    if (g_scroll_text != g_session.text()) { g_scroll_text = g_session.text(); g_scroll_since = tick(); g_wrap_scale = kWrapMargin; }
    return rp::summary_text(g_session, g_values, tr, [](const std::string& line, int max_lines) { return rp::wrap_px(line, kTextPx * g_wrap_scale, max_lines, [](const std::string& ch) { return lg::text_em(ch); }); }, tick() - g_scroll_since, &g_scroll);
}
void forget();
// The group made again with the same key (the Color Mod's pattern for per-page texts)
bool build_group() {
    const SF6CoreAPI* a = api();
    if (!a || !ensure_client()) return false;
    if (g_group >= 0) perf::counted(a)->opt_remove_group(g_opt_client, g_group);
    g_entries.clear();
    g_roles.clear();
    const std::string title = ui::T("report_title");
    g_summary = summary_now();
    g_group = perf::counted(a)->opt_group(g_opt_client, kGroupKey, title.c_str(), "");
    if (g_group < 0) { cmn::journal("bug report: opt_group failed"); return false; }
    for (const rp::Row role : g_session.rows()) {
        const std::string name = lg::cut_em(rp::row_title(g_session, role, tr), lg::kComRowEm);
        const char* key = role == rp::Row::Status ? "report_status" : role == rp::Row::Text ? "report_text" : role == rp::Row::Send ? "report_send" : "report_mark_fixed";
        SF6CoreOptEntryDesc d{};
        d.struct_size = sizeof d;
        d.key = key;
        d.title = name.c_str();
        d.desc = "";                             // (the text under the rows has one writer: see write_summary)
        std::vector<std::string> labels;
        std::vector<const char*> ptrs;
        if (role == rp::Row::Status) {
            for (const std::string& k : g_session.options()) labels.push_back(ui::T(k));
            for (const std::string& l : labels) ptrs.push_back(l.c_str());
            d.kind = SF6CORE_OPT_CHOICE;
            d.options = ptrs.data();
            d.n_options = (uint32_t)ptrs.size();
            d.value = g_session.option_index() + 1;
        } else {
            d.kind = SF6CORE_OPT_BUTTON;
        }
        g_entries.push_back(perf::counted(a)->opt_entry(g_opt_client, g_group, &d));
        g_roles.push_back(role);
    }
    return true;
}
// The page's rows: it is built again when this changes (the text under them just follows, see write_summary)
std::string page_now() {
    std::string s;
    for (const rp::Row role : g_session.rows()) s += "\x1f" + rp::row_title(g_session, role, tr);
    for (const std::string& k : g_session.options()) s += "\x1e" + k;
    return s + "\x1d" + std::to_string(g_session.option_index());
}
void open_window() {
    const SF6CoreAPI* a = api();
    if (!a || !build_group()) { g_session.closed(); forget(); cmn::journal("bug report: no window"); return; }
    g_built = page_now();
    g_window_seen = false;
    g_window_deadline = tick() + kWindowWait;
    if (!perf::counted(a)->opt_window_open()) {
        perf::counted(a)->opt_rebuild(g_opt_client);
        perf::counted(a)->opt_ensure_built(g_opt_client);
    }
    perf::counted(a)->opt_open_menu(g_opt_client, kGroupKey, 0);
    cmn::journal("bug report: menu asked (picture %d)", (int)cap::state());
}
void rebuild_page() {
    const SF6CoreAPI* a = api();
    if (!a) return;
    rp::Row focused = rp::Row::Status;
    const int64_t e = perf::counted(a)->opt_focused_entry();
    for (size_t i = 0; i < g_entries.size() && i < g_roles.size(); ++i) if (g_entries[i] >= 0 && g_entries[i] == e) focused = g_roles[i];
    if (!build_group() || !perf::counted(a)->opt_window_open()) return;
    int focus = 0;
    for (size_t i = 0; i < g_roles.size(); ++i) if (g_roles[i] == focused) focus = (int)i;
    perf::counted(a)->opt_navigate(g_opt_client, kGroupKey, SF6CORE_OPT_NAV_REBUILD | SF6CORE_OPT_NAV_NOW, focus);
}
void input_block(bool on) {
    const SF6CoreAPI* a = api();
    if (on) {
        void* param = a ? perf::counted(a)->opt_current_param() : nullptr;
        g_blocked = param ? gx::ref(static_cast<MO*>(param), "<Agent>k__BackingField") : nullptr;
        if (g_blocked) gx::set_block_input(g_blocked, true);
    } else if (g_blocked) {
        if (gm::is_managed(g_blocked)) gx::set_block_input(g_blocked, false);
        g_blocked = nullptr;
    }
}
// Nothing stays: the picture, the text, what the screen showed, and the core's copy of the rows' texts
void forget() {
    cmn::set_present_fn(nullptr);
    cap::discard();
    ++g_gen;
    g_snap = rp::Snapshot{};
    g_values = rp::MenuValues{};
    g_key.clear();
    g_built.clear();
    g_summary.clear();
    g_wrote.clear();
    g_read_back.clear();
    g_read_back_due = false;
    g_scroll_text.clear();
    g_wrap_scale = kWrapMargin;
    g_measure_at = 0;
    g_scroll = rp::SummaryScroll{};
    g_message.clear();
    g_msg = MsgLine{};
    g_bar_state = BarState{};
    g_copies_due.store(false);
    g_want_bar.store(false);
    g_copy_param.store(nullptr);
    g_copy_for = nullptr;
    g_copy_tries = 0;
    g_paste_on.store(false);
    g_asked = false;
    g_mark_due = false;
    g_step = SendStep::None;
    g_box = SummaryBox{};
    const SF6CoreAPI* a = api();
    if (a && g_opt_client && g_group >= 0) perf::counted(a)->opt_remove_group(g_opt_client, g_group);
    g_group = -1;
    g_entries.clear();
    g_roles.clear();
    std::lock_guard lk(g_prep_mx);
    g_prep.reset();
    g_prep_failed = false;
    g_preview = Preview{};
}
void closed_now(const char* why) {
    g_paste_on.store(false);
    input_block(false);
    g_naming = false;
    const SF6CoreAPI* a = api();
    void* dialogs[8] = {};
    const uint32_t n = a ? perf::counted(a)->ui_agents("OptionDialog", dialogs, 8) : 0;
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        MO* cm = gx::call_obj(static_cast<MO*>(dialogs[i]), "get_ControlMain");
        send_row_look(cm, false);
        for (const char* name : { kMsgName, kTrackName, kThumbName })
            if (MO* e = cm ? static_cast<MO*>(perf::counted(a)->ui_find_child(cm, name, 8)) : nullptr) gx::set_visible(e, false);
    }
    if (g_session.closed()) {
        ++g_backed;
        forget();
        cmn::journal("bug report: closed (%s): nothing kept", why);
    } else {
        cmn::journal("bug report: closed (%s) with a request on its way", why);
    }
}

// ---- what is read off the game thread -------------------------------------------------------------------------------------------------
// The menu's values and the costume key: right after the key, once
void start_preview() {
    rp::Snapshot snap = g_snap;                  // (no picture in it yet, and it is not sent)
    const uint64_t gen = g_gen;
    try {
        std::thread([snap = std::move(snap), gen] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            Preview pv;
            pv.gen = gen;
            pv.ready = true;
            try {
                const std::filesystem::path root = std::filesystem::current_path();
                const rp::Report r = rp::gather(snap, root, rp::scrub_here(root));
                pv.failed = !rp::costume_key_ok(r.costume_key);
                pv.key = r.costume_key;
                pv.outfit = r.outfit;
            } catch (...) { pv.failed = true; }
            std::lock_guard lk(g_prep_mx);
            g_preview = std::move(pv);
        }).detach();
    } catch (...) {
        std::lock_guard lk(g_prep_mx);
        g_preview.ready = true;
        g_preview.failed = true;
        g_preview.gen = gen;
    }
}
// The report itself, at Send: the logs as they are then
void start_prep() {
    rp::Snapshot snap = g_snap;
    snap.text = g_session.text();
    { std::lock_guard lk(g_prep_mx); g_prep.reset(); g_prep_failed = false; }
    g_step = SendStep::Preparing;
    try {
        std::thread([snap = std::move(snap)] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            nlohmann::json cmd;
            bool failed = false;
            try {
                const std::filesystem::path root = std::filesystem::current_path();
                const rp::Report r = rp::gather(snap, root, rp::scrub_here(root));
                failed = !rp::costume_key_ok(r.costume_key);
                if (!failed) cmd = rp::make_command(r);
            } catch (...) { failed = true; }
            std::lock_guard lk(g_prep_mx);
            if (failed) g_prep_failed = true;
            else g_prep = std::move(cmd);
        }).detach();
    } catch (...) {
        std::lock_guard lk(g_prep_mx);
        g_prep_failed = true;
    }
}

// ---- the server ----------------------------------------------------------------------------------------------------------------------------
bool answer_ok(bool ok, const nlohmann::json& d) { return ok && d.is_object() && d.contains("ok") && d["ok"] == true; }
void finish_if_gone(bool gone) { if (gone) { forget(); cmn::journal("bug report: its answer came after the window: nothing kept"); } }
void on_answer(bool ok, const nlohmann::json& data) {   // Community's poll: the frame
    const bool sent = answer_ok(ok, data);
    const std::string code = sent ? std::string() : community::auth::error_code(data);
    cmn::journal("bug report: %s", sent ? "sent" : ("not sent (" + (code.empty() ? std::string("no answer") : code) + ")").c_str());
    if (sent) ++g_sent; else ++g_failed;
    g_step = SendStep::None;
    const bool gone = g_session.window_gone();
    g_session.answered(sent, code);
    finish_if_gone(gone);
    g_dirty = true;
}
// The status of this costume's report, asked once, when the menu opened: the key alone leaves, no picture and no log
void ask_status() {
    if (g_asked || g_key.empty() || g_session.server() != rp::Server::Checking || community::client().busy()) return;
    g_asked = true;
    const uint64_t gen = g_gen;
    nlohmann::json cmd = nlohmann::json::object();
    cmd["cmd"] = "report_get";
    cmd["costume_key"] = g_key;
    community::client().send(std::move(cmd), [gen](bool ok, const nlohmann::json& d) {
        if (gen != g_gen) return;
        if (!answer_ok(ok, d)) {
            cmn::journal("bug report: status not read (%s)", community::auth::error_code(d).c_str());
            g_session.server_unreachable();
        } else {
            const bool exists = d.contains("exists") && d["exists"] == true;
            const std::string status = d.contains("status") && d["status"].is_string() ? d["status"].get<std::string>() : std::string();
            g_session.server_answer(rp::server_from(exists, status), d.contains("text") && d["text"].is_string() ? d["text"].get<std::string>() : std::string());
            cmn::journal("bug report: status read: %s", exists ? status.c_str() : "no report");
        }
        g_dirty = true;
    });
}
bool post_status(const char* action, rp::Server after) {
    if (g_key.empty() || community::client().busy() || !g_session.start_status()) return false;
    nlohmann::json cmd = nlohmann::json::object();
    cmd["cmd"] = "report_status";
    cmd["costume_key"] = g_key;
    cmd["action"] = action;
    community::client().send(std::move(cmd), [after](bool ok, const nlohmann::json& d) {
        const bool done = answer_ok(ok, d);
        const std::string code = done ? std::string() : community::auth::error_code(d);
        cmn::journal("bug report: status change %s", done ? "done" : code.c_str());
        const bool gone = g_session.window_gone();
        g_session.status_answered(done, after, code);
        finish_if_gone(gone);
        g_dirty = true;
    });
    g_dirty = true;
    return true;
}
// The picture, once encoded: into the report and the text under the rows
void take_picture() {
    if (!g_snap.jpeg.empty() || cap::state() != cap::State::Ready) return;
    g_snap.jpeg = cap::take();
}
void send_tick() {
    const uint64_t t = tick();
    switch (g_step) {
        case SendStep::WaitPicture:              // the JPEG is seldom not ready by now
            if (cap::state() != cap::State::Encoding || t - g_send_at > kEncodeWait) {
                take_picture();
                if (g_snap.jpeg.empty() && !cap::why().empty()) g_snap.jpeg_note = cap::why();
                start_prep();
            }
            break;
        case SendStep::Preparing: {
            std::optional<nlohmann::json> cmd;
            bool failed = false;
            {
                std::lock_guard lk(g_prep_mx);
                cmd = std::move(g_prep);
                g_prep.reset();
                failed = g_prep_failed;
                g_prep_failed = false;
            }
            if (failed) { on_answer(false, nlohmann::json::object()); break; }
            if (!cmd) break;
            {
                std::lock_guard lk(g_prep_mx);
                g_prep = std::move(cmd);
            }
            g_step = SendStep::WaitClient;
            g_busy_until = t + kBusyWait;
            [[fallthrough]];
        }
        case SendStep::WaitClient: {
            if (community::client().busy()) {
                if (t > g_busy_until) on_answer(false, nlohmann::json::object());
                break;
            }
            std::lock_guard lk(g_prep_mx);
            if (!g_prep) break;
            nlohmann::json cmd = std::move(*g_prep);
            g_prep.reset();
            g_step = SendStep::Posted;
            community::client().send(std::move(cmd), [](bool ok, const nlohmann::json& d) { on_answer(ok, d); }, community::kReportTimeoutTicks);
            cmn::journal("bug report: posted");
            break;
        }
        default: break;
    }
}
// Ctrl+V in the text dialog (Wael, 07/11: the game's own does nothing here, though it does from Training's pause menu; same dialog, same request, cause not found). While the
// dialog is up only: a moment after the keys, if the field shows the same as before them (the game's own would have changed it: the Multi Menu is Training too), the clipboard's
// text is typed into the field with real key events, so that the game's caret, cap and filters apply. Off the game thread; stops if the dialog goes.
uint32_t g_v_seen = 0;
uint64_t g_paste_check_at = 0;
std::string g_paste_before;
constexpr uint64_t kPasteCheckFrames = 10;
bool game_in_front() {                           // key events go to whichever window has the focus, and Ctrl's state is global
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    return front && GetWindowThreadProcessId(front, &pid) && pid == GetCurrentProcessId();
}
bool modifier_down() {
    for (const int vk : { VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN }) if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}
rp::PasteStop paste_stop_now() { return rp::paste_stop(game_in_front(), modifier_down(), g_paste_on.load() && !cmn::closing()); }
void paste_body() {
    for (int i = 0; i < 300 && (GetAsyncKeyState(VK_CONTROL) & 0x8000 || GetAsyncKeyState('V') & 0x8000) && game_in_front(); ++i) Sleep(10);   // typed with Ctrl down they would be shortcuts
    rp::PasteStop stop = paste_stop_now();
    if (stop != rp::PasteStop::None) { cmn::journal("bug report: Ctrl+V stopped: %s", rp::paste_stop_name(stop)); return; }
    std::wstring raw;
    for (int i = 0; i < 5 && !OpenClipboard(nullptr); ++i) { Sleep(20); if (i == 4) return; }
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            const size_t n = wcsnlen(w, GlobalSize(h) / sizeof(wchar_t));
            raw.assign(w, (std::min)(n, (size_t)16384));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    const std::wstring text = rp::paste_text(raw, rp::kTextMaxUnits);
    constexpr size_t kBatch = 16;                // what a change of window between the check and the send could still reach, at most
    for (size_t i = 0; i < text.size(); i += kBatch) {
        stop = paste_stop_now();                 // before every batch
        if (stop != rp::PasteStop::None) { cmn::journal("bug report: Ctrl+V stopped at %zu of %zu: %s", i, text.size(), rp::paste_stop_name(stop)); return; }
        INPUT in[kBatch * 2] = {};
        UINT n = 0;
        for (size_t k = i; k < text.size() && k < i + kBatch; ++k) {
            for (const DWORD flags : { (DWORD)KEYEVENTF_UNICODE, (DWORD)(KEYEVENTF_UNICODE | KEYEVENTF_KEYUP) }) {
                in[n].type = INPUT_KEYBOARD;
                in[n].ki.wScan = (WORD)text[k];
                in[n].ki.dwFlags = flags;
                ++n;
            }
        }
        SendInput(n, in, sizeof(INPUT));
        Sleep(2);
    }
}
void paste_tick() {
    const uint32_t n = cmn::key_presses('V');
    if (n != g_v_seen) {
        g_v_seen = n;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && game_in_front() && !g_paste_check_at && !g_pasting.load()) {
            g_paste_before = ui::dialogs::text_field_shown();
            g_paste_check_at = tick() + kPasteCheckFrames;
        }
    }
    if (!g_paste_check_at || tick() < g_paste_check_at) return;
    g_paste_check_at = 0;
    if (!game_in_front()) { cmn::journal("bug report: Ctrl+V stopped: the game is not in front"); return; }
    const std::string now = ui::dialogs::text_field_shown();
    const bool own = g_paste_before.empty() || now == g_paste_before;
    cmn::journal("bug report: Ctrl+V in the text dialog: %s", own ? "ours" : "the game's own");
    if (!own || g_pasting.exchange(true)) return;
    try {
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            paste_body();
            g_pasting.store(false);
        }).detach();
    } catch (...) { g_pasting.store(false); }
}
void press_row(rp::Row row) {
    if (row == rp::Row::Text) {
        if (g_session.busy() != rp::Busy::None || !ui::dialogs::text_available()) return;
        input_block(true);
        g_naming = true;
        g_v_seen = cmn::key_presses('V');
        g_paste_check_at = 0;
        g_paste_on.store(true);
        ui::dialogs::text_input("bug report", ui::details::edit_text(g_session.text()), rp::kTextMaxUnits, [](bool ok, const std::string& text) {
            g_paste_on.store(false);
            if (ok) { g_session.set_text(ui::details::edit_result(text)); cmn::journal("bug report: text set (%zu bytes)", g_session.text().size()); }
            g_naming = false;
            g_settle_until = tick() + kSettle;       // its Confirm must not reach the page; the page takes input again after it
            g_dirty = true;
        }, false);                                   // not the training pause menu's input: wherever this screen was opened from, it is not on top
    } else if (row == rp::Row::MarkFixed) {
        g_mark_due = true;
        cmn::journal("bug report: Mark as fixed");
    } else if (row == rp::Row::Send) {
        if (!g_session.send()) { g_dirty = true; return; }
        ++g_reports;
        g_send_at = tick();
        g_step = SendStep::WaitPicture;
        g_dirty = true;
        cmn::journal("bug report: Send");
    }
}
// What came in since the last frame: the picture, the files' values
void take_results() {
    take_picture();
    Preview pv;
    {
        std::lock_guard lk(g_prep_mx);
        if (!g_preview.ready || g_preview.gen != g_gen) return;
        pv = std::move(g_preview);
        g_preview = Preview{};
    }
    if (pv.failed) {
        g_session.server_unreachable();
    } else {
        g_key = pv.key;
        if (!pv.outfit.empty()) g_values.outfit = pv.outfit;
    }
    g_dirty = true;
}
void session_tick() {
    const SF6CoreAPI* a = api();
    const uint64_t t = tick();
    if (g_session.stage() == rp::Stage::Shooting) {
        if (cap::settled(cap::state()) || t - g_shot_at > kShotWait) {
            if (!cap::settled(cap::state())) {
                cmn::set_present_fn(nullptr);
                cap::discard();
                g_snap.jpeg_note = "no frame was presented within " + std::to_string(kShotWait) + " frames";
                cmn::journal("bug report: no Present within %llu frames: no picture", (unsigned long long)kShotWait);
            }
            g_session.set_text_field_ok(ui::dialogs::text_available());
            g_session.shot_settled();
            open_window();
        }
        return;
    }
    send_tick();
    if (g_session.stage() == rp::Stage::Idle) return;    // (an answer after the window went)
    if (g_session.window_gone()) return;                 // a request on its way, nobody looking: only its answer is left
    const bool window = a && perf::counted(a)->opt_window_open() != 0;
    if (window) g_window_seen = true;
    if (!window) {
        if (g_window_seen) closed_now("the window closed");
        else if (t > g_window_deadline) closed_now("the window never opened");
        return;
    }
    if ((t & 3) == 0 || g_message.empty()) {     // the message follows the cursor
        rp::Row focus = rp::Row::Status;
        const int64_t e = perf::counted(a)->opt_focused_entry();
        for (size_t i = 0; i < g_entries.size() && i < g_roles.size(); ++i) if (g_entries[i] >= 0 && g_entries[i] == e) focus = g_roles[i];
        g_message = rp::message_text(g_session, focus, tr);
    }
    dock_all();
    take_results();
    ask_status();
    if (g_naming) { paste_tick(); g_pressed.store(-1); g_chosen.store(-1); return; }   // the text dialog is over it: what reached the page counts for nothing
    if (g_blocked) {
        g_pressed.store(-1);
        g_chosen.store(-1);
        if (t < g_settle_until) return;
        input_block(false);
    }
    const int chosen = g_chosen.exchange(-1);
    if (chosen >= 0) {
        if (g_session.choose(chosen)) g_dirty = true;
        else if (chosen != g_session.option_index()) { g_built.clear(); g_dirty = true; }     // refused (a request is on its way): the row goes back to the truth
        g_mark_due = false;                      // moved away: nothing goes
    }
    const int pressed = g_pressed.exchange(-1);
    if (pressed >= 0 && pressed < (int)g_roles.size()) press_row(g_roles[(size_t)pressed]);
    if (g_mark_due && post_status(rp::action_confirm(), rp::Server::Confirmed)) g_mark_due = false;       // (as soon as the client is free)
    if ((t & 15) == 0) g_summary = summary_now();       // (the description's window moves by itself)
    if (g_dirty && !g_naming) {
        g_dirty = false;
        g_summary = summary_now();
        std::string now = page_now();
        if (now != g_built) { g_built = std::move(now); rebuild_page(); }
    }
}
// The render thread, the next Present after the key: the one Present that does any D3D work
void present_now() {
    cmn::set_present_fn(nullptr);
    const REFrameworkRendererData* rd = reframework::API::get()->param()->renderer_data;
    if (rd && rd->renderer_type == REFRAMEWORK_RENDERER_D3D12) cap::present(rd->device, rd->swapchain, rd->command_queue);
    else cap::present(nullptr, nullptr, nullptr);
}
void begin_report(MO* main) {
    if (!g_session.begin()) return;
    cap::request();
    cmn::set_present_fn(present_now);
    g_shot_at = tick();
    g_pressed.store(-1);
    g_chosen.store(-1);
    g_mark_due = false;
    g_dirty = false;
    g_step = SendStep::None;
    g_snap = rp::Snapshot{};
    g_values = rp::MenuValues{};
    g_key.clear();
    g_asked = false;
    ++g_gen;
    const SF6CoreAPI* a = api();
    read_selection(&g_snap);
    g_snap.character = g_snap.fighter > 0 ? model::chars::get_char_name(g_snap.fighter) : std::string();
    g_snap.outfit_row = row_text(main, "p_Spin_costume");
    g_snap.colour_row = row_text(main, "p_Spin_color");
    g_snap.reporter_version = cmn::version();
    g_snap.core_version = a && a->get_version ? a->get_version() : "";
    g_values.character = g_snap.fighter > 0 ? ui::lang::fighter(g_snap.character) : std::string();
    g_values.outfit = rp::row_name(g_snap.outfit_row);
    g_values.colour = rp::row_name(g_snap.colour_row);
    start_preview();
    cmn::journal("bug report: key pressed (fighter %d, outfit %d)", g_snap.fighter, g_snap.costume_no);
}
bool key_pressed() {
    const uint64_t t = tick();
    if (!g_scan || t - g_scan_at >= 120) { g_scan_at = t; g_scan = rp::qwerty_scan(symbol_of(kModeKeyboard)); }
    const int vk = g_scan ? (int)MapVirtualKeyW(g_scan, MAPVK_VSC_TO_VK) : 0;
    const uint32_t count = vk ? cmn::key_presses(vk) : 0;
    const bool key = count != g_key_count;
    g_key_count = count;
    const bool pad = windows::pad_held().x;
    const bool edge = pad && !g_pad_was;
    g_pad_was = pad;
    return key || edge;
}
// ---- asleep / awake -----------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_awake{false};
std::atomic<uint32_t> g_beats{0};                // calls of the screen's update
rp::LeftWatch g_left;
bool ok_hook = false;
uint64_t g_wakes = 0, g_sleeps = 0, g_wake_frame = 0;
bool g_hooks_in = false;                         // the first wake's hooks are all in
rp::HookSteps g_hook_steps;
uint32_t g_wake_beats = 0;

void SF6CORE_CALL on_core_frame(uint64_t, void*);

// The game's thread for the screen's update: only atomics and the core's pointer. g_awake goes first: a second thread cannot wake twice.
void make_copies_seh() {
    __try {
        make_copies();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_copy_tries = kMaxCopies;
        cmn::journal("bug report: a copy faulted: no more");
    }
}
// The option dialog's own update (R19: copies are made only there): one load while the window wants none
bool ok_copies = false;
int dialog_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void dialog_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_copies_due.load(std::memory_order_relaxed) || cmn::closing()) return;
    make_copies_seh();
}
void wake_up() {
    const SF6CoreAPI* a = cmn::core();
    if (!a || cmn::closing() || !cmn::owns(F::Ui) || !community::on() || !register_client()) { g_awake.store(false); return; }
    a->set_frame_callback(g_opt_client, on_core_frame, nullptr);
}
int beat_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_beats.fetch_add(1, std::memory_order_relaxed);
    if (!g_awake.load(std::memory_order_relaxed) && !g_awake.exchange(true)) wake_up();
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void beat_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_req_on.load(std::memory_order_acquire) || cmn::closing()) return;
    try { make_copy(); } catch (...) { ++g_refused; }
}

// The frame callback gives itself back; g_awake goes last, so a call of the update just then wakes it again at its next call, never lost.
void go_to_sleep() {
    try { hint_hide(); } catch (...) { g_hint.shown = false; }
    g_req_on.store(false);
    g_up = false;
    g_left.reset();
    ui::report_needs_dialogs(false);
    if (const SF6CoreAPI* a = cmn::core()) if (g_opt_client) a->set_frame_callback(g_opt_client, nullptr, nullptr);
    ++g_sleeps;
    // what the screen's update really did in this waking: the proof, in game, that it fires once a frame on this screen and stops when it is left
    cmn::journal("bug report: asleep (waking %llu: %u calls of the screen's update in %llu frames)", (unsigned long long)g_wakes,
                 g_beats.load() - g_wake_beats, (unsigned long long)(cmn::frame() - g_wake_frame));
    cmn::journal("bug report: %s", status().c_str());
    report_host::flush_journal();
    report_host::asleep();
    g_awake.store(false);
}

void frame_body() {
    const uint64_t t = cmn::frame();
    if (!g_left.seen()) {
        ++g_wakes;
        g_wake_beats = g_beats.load();
        g_wake_frame = t;
        cmn::journal("bug report: awake");
    }
    if (!g_hooks_in) {                           // the first opening: nothing reaches the menu until the hooks are all in, and a press meanwhile is lost
        if (g_hook_steps.due(g_beats.load(std::memory_order_relaxed)) && mech::hooks::hook_next() == 0) {
            g_hooks_in = true;
            cmn::journal("bug report: every hook in, after %llu frames", (unsigned long long)(t - g_wake_frame));
        }
        if (!g_hooks_in) {
            g_up = false;
            if (g_left.left(g_beats.load(std::memory_order_relaxed), t)) go_to_sleep();   // (the screen was left: the next wake goes on)
            return;
        }
    }
    ui::report_needs_dialogs(g_session.stage() != rp::Stage::Idle);
    ui::lang::frame(t, true, 0);
    {                                            // over Training's pause menu (the Multi Menu) the text dialog holds the menu's tab too; asked only while a report is open
        const bool paused = g_session.stage() != rp::Stage::Idle && gx::pause_agent() != nullptr;
        report_host::note_paused(paused);
        dlg::frame(paused);
    }
    if (g_session.stage() != rp::Stage::Idle) {
        if (g_hint.shown) hint_hide();
        g_left.beat(g_beats.load(std::memory_order_relaxed), t);
        session_tick();
        return;
    }
    if (g_left.left(g_beats.load(std::memory_order_relaxed), t) && !community::client().busy()) { go_to_sleep(); return; }   // (an answer on its way is taken first: asleep nothing polls)
    MO* main = nullptr;
    MO* agent = drawn_agent(&main);
    if (!agent) {
        if (g_hint.shown) hint_hide();
        g_up = false;                            // (the screen's flow goes on behind another screen: the calls of its update say when it is over)
        return;
    }
    rp::ScreenRead s = screen_read(agent, main);
    if (!g_up) {                                 // a key held on arrival acts only once released
        g_up = true;
        g_key_count = 0;
        key_pressed();
    }
    if (rp::entry_allowed(s)) s.key_taken = key_taken();
    const bool allowed = rp::entry_allowed(s);
    hint_frame(allowed);
    const bool press = key_pressed();            // read every frame here: a press while it is not allowed is lost, never queued
    if (allowed && press) {
        hint_hide();
        g_up = false;                            // back on the screen after it: as an arrival (an R typed in the text field is no press)
        ui::report_needs_dialogs(true);
        begin_report(main);
    }
}

void SF6CORE_CALL on_core_frame(uint64_t core_frame, void*) {
    if (cmn::closing() || !api() || !community::on()) { go_to_sleep(); return; }
    if (!cmn::owns(F::Ui)) { hand_back(); return; }
    std::lock_guard lk(ui::lock());
    try {
        report_host::frame(core_frame);
        frame_body();
    } catch (...) {
        ui::fault("bug report frame");
    }
}

}  // namespace

void specs(std::vector<mech::hooks::Spec>& out) {
    ui::spec(out, "app.UIFlowMatchingFighterSetting.Main", "OnUpdate", beat_pre, beat_post, "no Report a bug: nothing wakes it (a game update folded this screen's update?)", &ok_hook);
    ui::spec(out, "app.UIFlowOptionBGDialog.Main", "OnUpdate", dialog_pre, dialog_post, "no message line and no bar in the menu", &ok_copies);
}

void start() {
    cap::set_log([](const std::string& s) { cmn::journal("%s", s.c_str()); });
}

void hand_back() {                               // the UI stopped: the next frame callback, game thread
    const SF6CoreAPI* a = api();
    if (g_session.stage() != rp::Stage::Idle) {
        if (a && g_opt_client) perf::counted(a)->opt_close(g_opt_client);
        closed_now("the UI stopped");
    }
    try { dlg::hand_back(); } catch (...) {}
    if (g_awake.load()) go_to_sleep();
}

std::string status() {
    char b[240];
    std::snprintf(b, sizeof b, "%s, hook %d, woken %llu, stage %d, entry %s, reports %llu sent %llu failed %llu closed unsent %llu, picture %d", g_awake.load() ? "awake" : "asleep", ok_hook ? 1 : 0,
                  (unsigned long long)g_wakes, (int)g_session.stage(), g_hint.shown ? "shown" : "hidden", (unsigned long long)g_reports, (unsigned long long)g_sent, (unsigned long long)g_failed,
                  (unsigned long long)g_backed, (int)cap::state());
    return b;
}

}  // namespace ui::bug_report
