// ui/bug_report — "Report a bug" on Battle Settings > Character (Fighter Settings), its Character tab: R / pad X (the game's input 341).
// The key takes a picture of the game (report/capture.hpp), opens a page of the core's option window (the Color Mod's Edit Drive Colors'
// shape) with the text field, the list of what would be sent and Send. Send gathers it all off the game thread (report/gather.hpp) and
// posts it through the Community client: the Steam-verified token identifies the player, the server attaches his Steam ID. Back sends
// nothing and keeps nothing: the picture lives in memory only, and goes with the window.
//
// Rules (docs: crsm/README.md "Report a bug"):
//   - On this screen only: its agent drawn, its tab 0, its costume row shown: the agent and its main control stay drawn and ActualVisible
//     on the Titles and Profile tabs, so they prove nothing alone. To add a screen later: another screen_read(), the rest does not change.
//   - The entry is a copy of the guide bar's item template on the resident InputGuide's bar, which every screen shows: a copy left visible
//     comes back with that bar everywhere. So hiding it never depends on the bar being on screen, and goes over every bar of every InputGuide
//     (profile_mod.cpp's two lessons, 07/11). The copy is made inside the guide list's own update (R19), found again by name (R35), written from
//     the frame through the core (R1).
//   - No state OnEnter / OnExit hook (R18). The one hook, on the guide list's update, goes in the first time an entry is wanted and costs one
//     atomic load per call. While the screen is not up: a registry read every 15 frames; in Training, nothing.
//   - Encoding and HTTP off the game thread. No real player's id or name in the journal: counts and codes only.
//   - No squeezed text: the rows are cut with "...", the entry's region is the text's.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

#include "common.hpp"
#include "community/auth.hpp"
#include "community/client.hpp"
#include "community/community.hpp"
#include "details_logic.hpp"
#include "dialogs.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "internal.hpp"
#include "lang.hpp"
#include "logic.hpp"
#include "model/chars.hpp"
#include "perf.hpp"
#include "report/capture.hpp"
#include "report/gather.hpp"
#include "report/report_logic.hpp"

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
constexpr const char* kClientId = "SF6_CRSMNative_BugReport";    // the core's option callbacks are per client: the Community dialog keeps its own
constexpr uint64_t kPollEvery = 15;                              // the screen looked for while it is not up
constexpr uint64_t kHintPass = 10;                               // the entry placed, the bar read
constexpr uint64_t kShotWait = 20;                               // frames for the next Present to take the picture before the menu opens anyway
constexpr uint64_t kWindowWait = 120;                            // frames for the core to open the window
constexpr uint64_t kSettle = 12;                                 // frames after the text dialog before the page takes a press again (its Confirm)
constexpr uint64_t kEncodeWait = 300;                            // Send: frames the picture may still take
constexpr uint64_t kBusyWait = 900;                              // Send: frames another Community request may take
constexpr int kKeyId = 341;                                      // app.InputAssign.Digital.Id: R on the keyboard, X on the pad
constexpr int kModeGamePad = 1, kModeKeyboard = 2;               // app.InputGuideMode
constexpr int kMaxRefused = 3;                                   // copies refused in a session, then no entry

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
thread_local uintptr_t tl_part = 0;
bool g_hook_tried = false, g_hook_on = false;

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
void make_copy(uintptr_t part) {                 // inside the list's own update, from `this`, never kept from a frame
    if (g_req_part.load() != part) return;
    g_req_part.store(0);
    g_req_on.store(false);
    const SF6CoreAPI* a = api();
    MO* btn = a ? list_btn(reinterpret_cast<MO*>(part)) : nullptr;
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
int pre_late(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_req_on.load(std::memory_order_acquire)) tl_part = reinterpret_cast<uintptr_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_late(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t p = tl_part;
    tl_part = 0;
    if (!p || !g_req_on.load(std::memory_order_acquire) || cmn::closing()) return;
    try { make_copy(p); } catch (...) { ++g_refused; }
}
bool hint_hook() {                               // only the first time an entry is wanted
    if (g_hook_tried) return g_hook_on;
    g_hook_tried = true;
    gx::TD* td = gm::type("app.UIPartsInputGuideList");
    reframework::API::Method* m = td ? gm::method(td, "OnLateUpdate") : nullptr;
    g_hook_on = m && m->get_function_raw() && m->add_hook(pre_late, post_late, false);
    cmn::journal("bug report: guide entry %s", g_hook_on ? "on" : "off (no hook on the guide list's update)");
    return g_hook_on;
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
        gx::invoke(copy, "set_AutoRegionFit(System.Boolean)", { gx::B(false) });   // never squeezed: its region is its text's (below)
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
    if (!hint_hook()) return;
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
    int64_t input = 1;
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
// The fighter of an app.UIPartsMatchingFighterSetting
int mfs_fighter(MO* mfs) {
    int64_t f = -1;
    return mfs && gm::is_managed(mfs) && gx::is_type(mfs, "app.UIPartsMatchingFighterSetting") && gx::get_i64(mfs, "FighterId", &f) && f > 0 ? (int)f : -1;
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
int read_fighter(MO* agent) {
    MO* lst = gx::ref(gx::ref(agent, "_PartsManager"), "_List");
    for (int k = 0, cnt = gm::list_count(lst); k < cnt; ++k) {
        const int f = mfs_fighter(gm::list_item(lst, k));
        if (f > 0) return f;
    }
    const int f = mfs_fighter(gx::ref(gx::ref(gx::ref(gx::ref(agent, "<InputDelegates>k__BackingField"), "<Flow>k__BackingField"), "_Param"), "MatchingFighterSetting"));
    return f > 0 ? f : preview_fighter();
}

// ---- the report in progress -------------------------------------------------------------------------------------------------------
SF6CoreClient* g_opt_client = nullptr;           // our own core client for the option window
bool g_cb_set = false;
rp::Session g_session;
rp::Snapshot g_snap;
std::string g_what;                              // "Ryu, Outfit VII, Color 3": what the row shows
uint64_t g_shot_at = 0, g_window_deadline = 0, g_settle_until = 0, g_send_at = 0, g_busy_until = 0;
bool g_window_seen = false, g_dirty = false, g_naming = false;
MO* g_blocked = nullptr;                         // the option dialog's agent, its input off under the text dialog
int64_t g_group = -1;
std::vector<int64_t> g_entries;
std::atomic<int> g_pressed{-1};                  // a row's Confirm (the core's callback): run on the frame
// Send: the thread's result
std::mutex g_prep_mx;
std::optional<nlohmann::json> g_prep;
bool g_prep_running = false, g_prep_failed = false;
enum class SendStep { None, WaitPicture, Preparing, WaitClient, Posted } g_step = SendStep::None;
uint64_t g_reports = 0, g_sent = 0, g_failed = 0, g_backed = 0;
// the key
bool g_up = false, g_pad_was = false;
uint32_t g_key_count = 0;
unsigned g_scan = 0;
uint64_t g_scan_at = 0;

void SF6CORE_CALL cb_button(void*, int64_t entry, SF6CoreOptButtonResult* out) {
    if (out) out->kind = 0;                      // nothing more: the frame decides
    for (size_t i = 0; i < g_entries.size(); ++i) if (g_entries[i] == entry) { g_pressed.store((int)i); return; }
}

bool ensure_client() {
    const SF6CoreAPI* a = api();
    if (!a) return false;
    if (!g_opt_client) {
        SF6CoreClientDesc d{};
        d.struct_size = sizeof d;
        d.client_id = kClientId;
        d.client_version = cmn::version();
        d.priority = 100;
        if (perf::counted(a)->register_client(&d, &g_opt_client) != SF6CORE_OK) { g_opt_client = nullptr; cmn::journal("bug report: the core refused the client"); return false; }
    }
    if (!g_cb_set) {
        SF6CoreOptCallbacks cb{};
        cb.struct_size = sizeof cb;
        cb.on_button = cb_button;
        g_cb_set = perf::counted(a)->opt_set_callbacks(g_opt_client, &cb) == SF6CORE_OK;
        if (!g_cb_set) cmn::journal("bug report: the core's option service refused the callbacks");
    }
    return g_cb_set;
}

std::vector<rp::RowText> rows_now() {
    rp::MenuInput in;
    in.session = &g_session;
    in.what = g_what;
    in.shot_ok = cap::state() != cap::State::Failed && cap::state() != cap::State::None;
    in.row_em = lg::kComRowEm;
    in.cut = [](const std::string& s, double em) { return lg::cut_em(s, em); };
    return rp::menu_rows(in, [](const std::string& k, const std::vector<std::string>& args) { return ui::T(k, args); });
}
// The group made again with the same key (the Color Mod's pattern for per-page texts)
bool build_group() {
    const SF6CoreAPI* a = api();
    if (!a || !ensure_client()) return false;
    if (g_group >= 0) perf::counted(a)->opt_remove_group(g_opt_client, g_group);
    g_entries.clear();
    const std::string title = ui::T("report_title");
    g_group = perf::counted(a)->opt_group(g_opt_client, kGroupKey, title.c_str(), "");
    if (g_group < 0) { cmn::journal("bug report: opt_group failed"); return false; }
    int i = 0;
    for (const rp::RowText& r : rows_now()) {
        const std::string key = "report_r" + std::to_string(i++);
        SF6CoreOptEntryDesc d{};
        d.struct_size = sizeof d;
        d.kind = SF6CORE_OPT_BUTTON;
        d.key = key.c_str();
        d.title = r.title.c_str();
        d.desc = r.desc.c_str();
        g_entries.push_back(perf::counted(a)->opt_entry(g_opt_client, g_group, &d));
    }
    return true;
}
void open_window() {
    const SF6CoreAPI* a = api();
    if (!a || !build_group()) { g_session.closed(); cap::discard(); cmn::journal("bug report: no window"); return; }
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
    int focus = 0;
    const int64_t e = perf::counted(a)->opt_focused_entry();
    for (size_t i = 0; i < g_entries.size(); ++i) if (g_entries[i] >= 0 && g_entries[i] == e) focus = (int)i;
    if (!build_group() || !perf::counted(a)->opt_window_open()) return;
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
// Nothing stays: the picture, the text, what the screen showed
void forget() {
    cap::discard();
    g_snap = rp::Snapshot{};
    g_what.clear();
    g_step = SendStep::None;
    std::lock_guard lk(g_prep_mx);
    g_prep.reset();
    g_prep_failed = false;
}
void closed_now(const char* why) {
    input_block(false);
    g_naming = false;
    if (g_session.closed()) {
        ++g_backed;
        forget();
        cmn::journal("bug report: closed (%s): nothing kept", why);
    } else {
        cmn::journal("bug report: closed (%s) with the report on its way", why);
    }
    g_entries.clear();
}

void start_prep() {
    rp::Snapshot snap = g_snap;
    snap.text = g_session.text();
    if (cap::state() == cap::State::Ready) { g_snap.jpeg = cap::take(); snap.jpeg = g_snap.jpeg; }
    else if (g_snap.jpeg.empty()) snap.jpeg_note = cap::why();
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
void on_answer(bool ok, const nlohmann::json& data) {   // Community's poll: the frame
    const bool sent = ok && data.is_object() && data.contains("ok") && data["ok"] == true;
    const std::string code = sent ? std::string() : community::auth::error_code(data);
    cmn::journal("bug report: %s", sent ? "sent" : ("not sent (" + (code.empty() ? std::string("no answer") : code) + ")").c_str());
    if (sent) ++g_sent; else ++g_failed;
    g_step = SendStep::None;
    const bool gone = g_session.window_gone();
    g_session.answered(sent, code);
    if (sent || gone) { g_snap.jpeg.clear(); g_snap.text.clear(); }
    if (gone) forget();
    g_dirty = true;
}
void send_tick() {
    const uint64_t t = tick();
    switch (g_step) {
        case SendStep::WaitPicture:              // the JPEG is seldom not ready by now
            if (cap::state() != cap::State::Encoding || t - g_send_at > kEncodeWait) start_prep();
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
void press_row(int row) {
    if (row == (int)rp::Row::Text) {
        if (g_session.stage() != rp::Stage::Open || g_session.outcome() == rp::Session::Outcome::Sent || !ui::dialogs::text_available()) return;
        input_block(true);
        g_naming = true;
        ui::dialogs::text_input("bug report", ui::details::edit_text(g_session.text()), rp::kTextMaxUnits, [](bool ok, const std::string& text) {
            if (ok) { g_session.set_text(ui::details::edit_result(text)); cmn::journal("bug report: text set (%zu bytes)", g_session.text().size()); }
            g_naming = false;
            g_settle_until = tick() + kSettle;       // its Confirm must not reach the page; the page takes input again after it
            g_dirty = true;
        });
    } else if (row == (int)rp::Row::Send) {
        if (!g_session.send()) { g_dirty = true; return; }
        ++g_reports;
        g_send_at = tick();
        g_step = SendStep::WaitPicture;
        g_dirty = true;
        cmn::journal("bug report: Send");
    }
}
void session_tick() {
    const SF6CoreAPI* a = api();
    const uint64_t t = tick();
    const rp::Stage st = g_session.stage();
    if (st == rp::Stage::Shooting) {
        if (cap::settled(cap::state()) || t - g_shot_at > kShotWait) {
            if (!cap::settled(cap::state())) cmn::journal("bug report: no Present within %llu frames: no picture", (unsigned long long)kShotWait);
            g_session.set_text_field_ok(ui::dialogs::text_available());
            g_session.shot_settled();
            open_window();
        }
        return;
    }
    send_tick();
    if (g_session.stage() == rp::Stage::Idle) return;    // (an answer after the window went)
    if (g_session.window_gone()) return;                 // the report on its way, nobody looking: only its answer is left
    const bool window = a && perf::counted(a)->opt_window_open() != 0;
    if (window) g_window_seen = true;
    if (!window) {
        if (g_window_seen) closed_now("the window closed");
        else if (t > g_window_deadline) closed_now("the window never opened");
        return;
    }
    if (g_naming) return;                                // the text dialog is over it
    if (g_blocked && t >= g_settle_until) input_block(false);
    if (g_blocked) return;
    const int pressed = g_pressed.exchange(-1);
    if (pressed >= 0) press_row(pressed);
    if (g_dirty && !g_naming) { g_dirty = false; rebuild_page(); }
}
void begin_report(MO* agent, MO* main) {
    if (!g_session.begin()) return;
    cap::request();
    g_shot_at = tick();
    g_pressed.store(-1);
    g_dirty = false;
    g_step = SendStep::None;
    g_snap = rp::Snapshot{};
    const SF6CoreAPI* a = api();
    g_snap.fighter = read_fighter(agent);
    g_snap.character = g_snap.fighter > 0 ? model::chars::get_char_name(g_snap.fighter) : std::string();
    g_snap.outfit_row = row_text(main, "p_Spin_costume");
    g_snap.colour_row = row_text(main, "p_Spin_color");
    g_snap.crsm_version = cmn::version();
    g_snap.core_version = a && a->get_version ? a->get_version() : "";
    const std::string shown = g_snap.fighter > 0 ? ui::lang::fighter(g_snap.character) : std::string();
    g_what = shown;
    for (const std::string& part : { rp::row_name(g_snap.outfit_row), rp::row_name(g_snap.colour_row) })
        if (!part.empty()) g_what += (g_what.empty() ? "" : ", ") + part;
    cmn::journal("bug report: key pressed (fighter %d)", g_snap.fighter);
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
void went_away() {
    hint_hide();
    g_up = false;
}

}  // namespace

void frame(bool training) {
    if (!api() || !community::on()) return;
    if (g_session.stage() != rp::Stage::Idle) {
        if (g_session.stage() == rp::Stage::Shooting || g_session.stage() == rp::Stage::Open || g_session.stage() == rp::Stage::Sending) {
            if (g_hint.shown) hint_hide();
            session_tick();
        }
        return;
    }
    const uint64_t t = tick();
    if (!g_up && t % kPollEvery != 0) return;
    if (training || cmn::closing()) { if (g_up) went_away(); return; }
    MO* main = nullptr;
    MO* agent = drawn_agent(&main);
    if (!agent) { if (g_up) went_away(); return; }
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
    if (allowed && press) { hint_hide(); begin_report(agent, main); }
}

void start() {
    cap::set_log([](const std::string& s) { cmn::journal("%s", s.c_str()); });
    const auto* param = reframework::API::get()->param();
    if (!param || !param->functions || !param->functions->on_present) { cmn::journal("bug report: no present callback: reports go without a picture"); return; }
    param->functions->on_present([] {            // every Present: one atomic load inside, no D3D call unless a picture was asked
        const REFrameworkRendererData* rd = reframework::API::get()->param()->renderer_data;
        if (rd && rd->renderer_type == REFRAMEWORK_RENDERER_D3D12) cap::present(rd->device, rd->swapchain, rd->command_queue);
    });
}

void hand_back() {
    const SF6CoreAPI* a = api();
    if (g_session.stage() != rp::Stage::Idle && a && g_opt_client) perf::counted(a)->opt_close(g_opt_client);
    closed_now("the UI stopped");
    try { hint_hide(); } catch (...) { g_hint.shown = false; }
}

std::string status() {
    char b[200];
    std::snprintf(b, sizeof b, "stage %d, entry %s, reports %llu sent %llu failed %llu closed unsent %llu, picture %d", (int)g_session.stage(), g_hint.shown ? "shown" : "hidden",
                  (unsigned long long)g_reports, (unsigned long long)g_sent, (unsigned long long)g_failed, (unsigned long long)g_backed, (int)cap::state());
    return b;
}

}  // namespace ui::bug_report
