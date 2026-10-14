// ui/dialogs — the game's text input dialog and message box driven by the DLL (phase P4, owner "ui"), the tab blocks
// while one of CRSM's dialogs is up, and the interface of the Manager page (dialogs.hpp, phase P5).
// RecordingManagerTab.lua (RMT) 77-98 (message box), 1399-1747 (text dialog, input lock, tab blocks).
//
// Requests may come from any thread (the Manager page's decides run on the menu's job threads): they are queued and run
// on the frame callback (game thread), where the Lua's own T key opened its dialog too (RMT:1765); 'done' is called there.
// The dialog's own events arrive in game hooks (job threads): they record the outcome; the frame callback applies it.
#include "dialogs.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <optional>

#include "common.hpp"
#include "dump.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "hookstack.hpp"
#include "internal.hpp"
#include "logic.hpp"
#include "mech/hooks.hpp"

namespace ui {
namespace com_api { void open(); void share(); }   // community_dialog.cpp
}

namespace ui::dlg {
namespace {

namespace gm = game::gm;
namespace lg = ui::logic;
using gx::MO;

constexpr int32_t kRequestMemo = 16;            // UITextInputDialog.RequestType.ReciperMemo: a free memo (RMT:1379)
constexpr int32_t kVisualFG = 1;                // VisualType FG (RMT:1430)
constexpr int32_t kBoxDefault = 1, kBoxCancel = 1, kBoxYesNo = 4, kBoxNoIcon = -1, kBoxFG = 1;   // MessageBox.Start (RMT:84)
// the dialog's setup (SetupText, UIPartsTextInput.Init, then the texts formatted by RequestType): UITextInputDialog.Init()
// (route 3B: a post-hook on it when its code is its own, R18). Found by its code address (0x145CD74F0, the scan named
// Init, the only method entering it; its wrapper 0x141671F40 = FlowEvent_Setup) until the game update of 13/10 moved the
// code: found by its name now.
constexpr const char* kDialogSetupSig = "Init()";
constexpr int kLineSearchFrames = 300;          // route 3A: the explanation line looked for ~5 s at most
constexpr int kLineSettleFrames = 30;           // ... and the walk kept 30 frames after it was found (a later rebuild)

struct TextReq { std::string title, initial; size_t max = 64; std::function<void(bool, const std::string&)> done; };
struct BoxReq { std::string text, title; std::function<void(bool)> done; };

// the text dialog in progress (`naming`, RMT:1399)
struct Job {
    std::string title;
    size_t max = 64;
    uintptr_t addr = 0;                          // the dialog object (compared in the hooks, RMT:1650)
    lg::NameJob nj;
    std::function<void(bool, const std::string&)> done;
    bool finished = false;
    std::optional<std::string> outcome;          // what finish_naming applies (nullopt = nothing)
    std::string why;
    // the explanation line ("Maximum 15 characters"): the texts relabelled and what the game had written there
    std::vector<std::pair<uintptr_t, std::string>> lines;
    int walk_frames = 0, found_at = -1;
    int again_run = 0;                           // frames running the game's line came back (3: left to the game)
    bool again_off = false;
};

std::mutex g_q_mx;                               // the request queues (any thread)
std::deque<TextReq> g_text_q;
std::deque<BoxReq> g_box_q;
std::optional<Job> g_job;                        // under ui::lock
std::atomic<bool> g_naming{false};               // read by the tab-block hooks without the lock
std::atomic<bool> g_job_up{false}, g_box_up{false};   // busy() reads these without ui::lock (any thread, P5's callbacks)
struct Box { MO* handle = nullptr; std::function<void(bool)> done; std::string title; };
std::optional<Box> g_box;
bool g_input_off = false;                        // menu_input_off (RMT:1417)
uint64_t g_dialogs = 0, g_boxes = 0, g_blocks = 0;
// the hooks in place: a text dialog is opened only with its result / cancel / hide hooks (no delegate is given: the
// InvokeResultEvent pre-hook must skip the game's call)
bool ok_result = false, ok_init = false, ok_typed = false, ok_decide = false, ok_decide2 = false, ok_cancel = false, ok_cancel2 = false;
bool ok_setup = false, ok_hide = false, ok_setup_post = false;
uint64_t g_relabels = 0, g_relabel_again = 0;
bool ok_tabs[6]{}, ok_ptabs[2]{}, ok_flow[6]{};
lg::TabGuard g_tab;                              // the menu's tab held under CRSM's dialogs (under ui::lock)
uint64_t g_tab_holds = 0;                        // the tab / cursor row / tab list put back
std::atomic<void*> g_tab_list{nullptr};          // the primary tab list while a dialog is up (its hook compares, any thread)
std::atomic<int64_t> g_tab_list_held{-1};        // its selected index held (-1: not read)
bool ok_list[2]{};

MO* tm() { return gx::singleton("app.training.TrainingManager"); }

void menu_input_blocked(bool on) {              // RMT:1418-1424 (B3 would count these; the Lua's direct calls kept)
    if (g_input_off == on) return;
    g_input_off = on;
    gx::set_block_input(gx::pause_agent(), on);
    gx::invoke(tm(), "SetAllMenuInput(System.Boolean)", { gx::B(!on) });
    cmn::journal("ui: menu input %s", on ? "blocked" : "released");
}

std::optional<std::string> dialog_text(const Job& job) {   // what the dialog holds now (RMT:1628-1638)
    if (!job.addr || !gm::is_managed(reinterpret_cast<void*>(job.addr))) return std::nullopt;
    MO* dlg = reinterpret_cast<MO*>(job.addr);
    const std::string cur = gx::str(gx::ref(dlg, "CurremtMessage"));
    if (!cur.empty()) return cur;
    reframework::InvokeRet r{};
    MO* parts = gx::ref(dlg, "TextInputParts");
    if (parts && gm::call(parts, "GetString", {}, &r) && r.ptr) { const std::string raw = gx::str(r.ptr); if (!raw.empty()) return raw; }
    return std::nullopt;
}

void finish(const char* why) {                   // finish_naming (RMT:1639-1647): recorded here, applied on the frame
    if (!g_job || g_job->finished) return;
    g_job->finished = true;
    g_naming.store(false);
    g_job->why = why;
    g_job->outcome = lg::naming_outcome(g_job->nj, dialog_text(*g_job));
}

bool ours(void* self) { return g_job && !g_job->finished && (uintptr_t)self == g_job->addr; }

// -- opening a text dialog (RMT:1426-1468): on the game thread
void open_text(TextReq r) {
    if (g_job || g_box || !(ok_result && ok_cancel && ok_hide)) {
        cmn::journal("ui: text dialog for %s refused (%s)", r.title.c_str(), (g_job || g_box) ? "a dialog is up" : "its hooks are not in place");
        if (r.done) r.done(false, r.initial);
        return;
    }
    gx::TD* td = gm::type("app.UITextInputDialog");
    reframework::API::Method* m = td ? gm::method_n(td, "OpenTextInputDialog", 5) : nullptr;
    if (!m) { cmn::journal("ui: name dialog: method missing"); if (r.done) r.done(false, r.initial); return; }
    MO* initial = reframework::API::get()->create_managed_string_normal(r.initial.c_str());
    reframework::InvokeRet ret{};
    const bool called = gm::call(nullptr, m, { nullptr, gm::arg_i32(kRequestMemo), gm::arg_i32(kVisualFG), initial, gm::arg_i32((int32_t)r.max) }, &ret);
    MO* dlg = called ? static_cast<MO*>(ret.ptr) : nullptr;
    if (!dlg) { cmn::journal("ui: name dialog failed"); if (r.done) r.done(false, r.initial); return; }
    // no explanation line (the memo request's "15 characters" text)
    gx::invoke(dlg, "SetupDefaultMessage(System.String)", { gx::S("") });
    gx::set_field_str(dlg, "DefaultMessage", "");
    gx::set_field_str(dlg, "CurremtMessage", "");
    Job j;
    j.title = r.title;
    j.max = r.max;
    j.addr = (uintptr_t)dlg;
    j.nj.initial = r.initial;
    j.done = std::move(r.done);
    g_job = std::move(j);
    g_job_up.store(true);
    g_naming.store(true);
    ++g_dialogs;
    menu_input_blocked(true);
    cmn::journal("ui: name dialog for %s", g_job->title.c_str());
}

// -- the message box (RMT:80-98)
void open_box(BoxReq r) {
    if (g_box) { if (r.done) r.done(false); return; }   // only one box at a time: a second request is ignored
    gx::TD* td = gm::type("app.UIFlowDialog.MessageBox");
    reframework::API::Method* m = td ? gm::method_n(td, "Start", 7) : nullptr;
    if (!m) { if (r.done) r.done(true); return; }       // no box: the action runs without confirming
    MO* msg = reframework::API::get()->create_managed_string_normal(r.text.c_str());
    MO* title = reframework::API::get()->create_managed_string_normal(r.title.c_str());
    reframework::InvokeRet ret{};
    const bool called = gm::call(nullptr, m, { msg, title, gm::arg_i32(kBoxDefault), gm::arg_i32(kBoxCancel), gm::arg_i32(kBoxYesNo),
                                               gm::arg_i32(kBoxNoIcon), gm::arg_i32(kBoxFG) }, &ret);
    MO* h = called ? static_cast<MO*>(ret.ptr) : nullptr;
    if (!h) { cmn::journal("ui: confirm box failed"); if (r.done) r.done(true); return; }
    h->add_ref();                                // held while polled (the Lua's confirm_box table held it)
    g_box = Box{ h, std::move(r.done), r.title };
    g_box_up.store(true);
    ++g_boxes;
    cmn::journal("ui: confirm box: %s", r.title.c_str());
}

void box_tick() {                                // confirm_tick (RMT:88-98), under the pause
    if (!g_box) return;
    bool ended = false;
    if (!gm::call_bool(g_box->handle, "get_IsEnd", &ended)) {   // the handle unreadable: dropped, nothing runs (RMT:91)
        Box b = std::move(*g_box);
        g_box.reset();
        g_box_up.store(false);
        b.handle->release();
        if (b.done) b.done(false);
        return;
    }
    if (!ended) return;
    Box b = std::move(*g_box);
    g_box.reset();
    g_box_up.store(false);
    int32_t v = -1;
    gx::TD* td = gm::type("app.UIFlowDialog.MessageBox");
    reframework::API::Method* m = td ? gm::method(td, "GetSelectValue") : nullptr;
    reframework::InvokeRet ret{};
    const bool ok = m && gm::call(nullptr, m, { b.handle }, &ret);
    if (ok) v = (int32_t)ret.dword;
    b.handle->release();
    cmn::journal("ui: confirm box result: %d", ok ? v : -1);
    if (b.done) b.done(ok && v == 0);            // 0 = the first button (Yes)
}

// ---- hooks ------------------------------------------------------------------------------------------------------------
#define UI_PRE(name, body_fn)                                                                    \
    int name(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {          \
        if (cmn::closing() || !ui::active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;             \
        int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;                                                \
        __try { rc = body_fn(argv); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault(#name " (SEH)"); } \
        return rc;                                                                              \
    }

#define UI_PRE_S(name, stack, body_fn)                                                           \
    int name(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {          \
        auto* s_ = stack.push();                                                                \
        if (!s_ || cmn::closing() || !ui::active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;      \
        int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;                                                \
        __try { rc = body_fn(argv, s_); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault(#name " (SEH)"); } \
        return rc;                                                                              \
    }
#define UI_POST_S(name, stack, body_fn)                                                          \
    void name(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {                \
        auto c_ = stack.pop();                                                                  \
        if (cmn::closing()) return;                                                             \
        __try { body_fn(ret, &c_); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault(#name " (SEH)"); } \
    }

// An outer function without the ui::active() gate: the result, the cancel and the hide of OUR dialog are always taken,
// also after the UI stopped by faults while the dialog was up (its result must never reach the game's call, which would
// invoke the delegate we did not give; the hide ends the job so the address is never matched again).
#define UI_PRE_ALWAYS(name, body_fn) \
    int name(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { \
        if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL; \
        int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL; \
        __try { rc = body_fn(argv); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault(#name " (SEH)"); } \
        return rc; \
    }

// InvokeResultEvent(String) pre: the decided text, SKIP (no delegate was given), RMT:1651-1658
int result_pre_body(void** argv) {
    if (!g_job_up.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try {
        if (!ours(argv[1])) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        g_job->nj.result = gx::str(argv[2]);
        g_job->nj.decided = true;
        finish("result");
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    } catch (...) { ui::fault("InvokeResultEvent pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE_ALWAYS(result_pre, result_pre_body)

// UIPartsTextInput.Init pre: the field's maximum (RMT:1659-1664; the Lua filtered on the flag only, kept)
int init_pre_body(void** argv) {
    if (!g_naming.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try { if (g_job && !g_job->finished) gx::set_field_i64(static_cast<MO*>(argv[2]), "MaxLength", (int64_t)g_job->max); }
    catch (...) { ui::fault("UIPartsTextInput.Init pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE(init_pre, init_pre_body)

// UIPartsTextInput.TextInputDecided(String) pre: the text as typed (RMT:1665-1669)
int typed_pre_body(void** argv) {
    if (!g_naming.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try { if (g_job && !g_job->finished) { g_job->nj.raw = gx::str(argv[2]); cmn::journal("ui: typed (%zu bytes)", g_job->nj.raw->size()); } }
    catch (...) { ui::fault("TextInputDecided pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE(typed_pre, typed_pre_body)

// Decide / InputEvent_Decide pre (RMT:1670-1677)
int decide_pre_body(void** argv) {
    if (!g_naming.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try { if (ours(argv[1])) lg::naming_decide(g_job->nj, dialog_text(*g_job)); } catch (...) { ui::fault("Decide pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE(decide_pre, decide_pre_body)

// Cancel / InputEvent_Cancel pre (RMT:1678-1684)
int cancel_pre_body(void** argv) {
    if (!g_job_up.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try { if (ours(argv[1])) { g_job->nj.cancelled = true; finish("cancel"); } } catch (...) { ui::fault("Cancel pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE_ALWAYS(cancel_pre, cancel_pre_body)

// The "Maximum 15 characters" line of the memo request reads "Press Enter To Edit" (RMT:1686-1713). The Lua relabelled
// it after FlowEvent_BeforeShow; that method's code is the game's empty stub shared by 18352 methods (R18: not hooked).
// SetupText's post comes before the dialog's setup writes its texts (by RequestType). So (route 3A) the frame callback
// walks the dialog from TextFormPanel (3 parents up, depth <= 6) each frame until the line is found, and 30 frames more;
// the texts found are then only checked, and written again if the game puts its own line back (compared first, R7).
// The text field (TextInputParts' control) is never walked, and a text showing the initial text is never taken: a slot
// name, or what the player types, may hold "15".
void relabel_walk(MO* e, int depth, Job& j, const std::string& want, MO* skip, int* hits) {
    for (MO* c = gx::first_child(e); c; c = gx::next(c)) {
        if (skip && c == skip) continue;
        if (gx::is_type(c, "via.gui.Text")) {
            const std::string m = gx::msg(c);
            if (m.find("15") != std::string::npos && m != want && m != j.nj.initial) {
                gx::invoke(c, "set_DisableMessageTag(System.Boolean)", { gx::B(true) });
                gx::set_text(c, want);
                bool known = false;
                for (auto& [a, orig] : j.lines) if (a == (uintptr_t)c) { known = true; orig = m; }
                if (!known) j.lines.emplace_back((uintptr_t)c, m);
                ++*hits;
            }
        }
        if (depth < 6) relabel_walk(c, depth + 1, j, want, skip, hits);
    }
}
// one walk of our dialog (ui::lock held); `where` names the caller in the journal
int relabel_dialog(Job& j, const char* where) {
    MO* dlg = reinterpret_cast<MO*>(j.addr);
    if (!dlg || !gm::is_managed(dlg)) return 0;
    MO* panel = gx::ref(dlg, "TextFormPanel");
    if (!panel) return 0;
    MO* root = panel;
    for (int i = 0; i < 3; ++i) { MO* p = gx::parent(root); if (!p) break; root = p; }
    MO* field = gx::call_obj(gx::ref(dlg, "TextInputParts"), "get_Control");
    int hits = 0;
    relabel_walk(root, 0, j, ui::T("press_enter"), field, &hits);
    if (hits) {
        g_relabels += (uint64_t)hits;
        if (j.found_at < 0) j.found_at = j.walk_frames;
        cmn::journal("ui: dialog line relabelled (%d) from %s, frame %d of the dialog", hits, where, j.walk_frames);
    }
    return hits;
}
// Route 3A, frame callback (game thread, ui::lock held)
void relabel_tick(Job& j) {
    if (!j.addr || !gm::is_managed(reinterpret_cast<void*>(j.addr))) return;
    const std::string want = ui::T("press_enter");
    bool back = false;
    for (auto& [a, orig] : j.lines) {           // the line found: the game's own text back on it = written again
        if (j.again_off) break;
        MO* t = reinterpret_cast<MO*>(a);
        if (!gm::is_managed(t) || !gx::is_type(t, "via.gui.Text")) continue;
        if (gx::msg(t) == orig) {
            gx::set_text(t, want);
            if (g_relabel_again++ == 0 || j.again_run == 0) cmn::journal("ui: dialog line written again (the game's line was back)");
            back = true;
        }
    }
    j.again_run = back ? j.again_run + 1 : 0;
    if (j.again_run >= 3 && !j.again_off) {     // the game writes it each frame: no per-frame fight
        j.again_off = true;
        cmn::journal("ui: the dialog writes its line every frame: left to it");
    }
    if (j.again_off) return;
    const bool searching = j.found_at < 0 ? j.walk_frames < kLineSearchFrames : j.walk_frames < j.found_at + kLineSettleFrames;
    if (!searching) return;
    relabel_dialog(j, "the frame");
    if (++j.walk_frames == kLineSearchFrames && j.found_at < 0)
        cmn::journal("ui: dialog line not found in %d frames (the dialog keeps its own explanation line)", kLineSearchFrames);
}

// SetupText post (and route 3B: the dialog setup's post, when hooked): one walk, early
struct ShowCall { void* self = nullptr; };
thread_local HookStack<ShowCall> s_show;
int show_pre_body(void** argv, ShowCall* s) { s->self = argv[1]; return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void show_post_body(void**, const ShowCall* c) {
    void* self = c->self;
    if (!self || !g_naming.load() || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try { if (ours(self)) relabel_dialog(*g_job, "SetupText"); } catch (...) { ui::fault("text dialog relabel"); }
}
UI_PRE_S(show_pre, s_show, show_pre_body)
UI_POST_S(show_post, s_show, show_post_body)
// Route 3B: the setup's arguments are not read (its signature is only known from the survey): our dialog is the one the
// job holds (one text dialog at a time)
int setup_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void setup_post_body() {
    if (!g_naming.load() || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try { if (g_job && !g_job->finished) relabel_dialog(*g_job, "the dialog setup's post"); } catch (...) { ui::fault("dialog setup post"); }
}
void setup_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing()) return;
    __try { setup_post_body(); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("dialog setup post (SEH)"); }
}

// FlowEvent_AfterHide pre: the dialog gone, whatever closed it (RMT:1714-1715)
int hide_pre_body(void** argv) {
    if (!g_job_up.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try { if (ours(argv[1])) finish("hidden"); } catch (...) { ui::fault("FlowEvent_AfterHide pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE_ALWAYS(hide_pre, hide_pre_body)

// -- the menu's tab changes blocked while a text dialog or the Community dialog is up (RMT:1717-1745). As the Lua: every
//    agent's tab inputs (the Community option dialog's own tab handling would otherwise take A / E); UIAgent is every
//    screen's, so a box (which has no tab) does not block it
bool blocking() { return g_naming.load() || com::open(); }
int agent_tab_pre_body(void**) {
    if (!blocking()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    ++g_blocks;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
UI_PRE(agent_tab_pre, agent_tab_pre_body)
int param_tab_pre_body(void**) {
    if (!modal()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    ++g_blocks;
    cmn::journal("ui: tab change blocked");
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
UI_PRE(param_tab_pre, param_tab_pre_body)
// L1+R1 (A+E) is a primary-tab jump (logic.hpp TabGuard): blocked at the list update and the tab index, the object
// type-checked first (a folded hook would otherwise skip an unrelated method, rule 13); the data's tab is put back by
// hold_tab (CreateViewData / InitTabItem pre, the frame). The menu's flow changes (4, 5: not on that path) stay blocked
// under the text dialog and the Community dialog only, as before
const char* const kFlowTypes[6] = { "app.training.UIFlowTrainingMenu.Param", "app.UIPartsPageIndex", "app.UIPartsPageIndex",
                                    "app.UIPartsPageIndex", "app.training.TrainingManager", "app.training.UITrainingMenu" };
const char* const kFlowMethods[6] = { "UpdatePrimaryList", "set_PageIndex", "Prev", "Next", "ChangeMainMenuFlow", "ChangeFlow" };
template <int I> int flow_pre_body(void** argv) {
    if (!(I < 4 ? modal() : blocking())) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    void* self = argv[1];
    const bool same = self && gm::is_managed(self) && gx::is_type(static_cast<MO*>(self), kFlowTypes[I]);
    if (same) { ++g_blocks; cmn::journal("ui: menu flow change blocked (%s)", kFlowMethods[I]); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    cmn::journal("ui: menu flow change seen (%s), another object", kFlowMethods[I]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE(flow_pre0, flow_pre_body<0>)
UI_PRE(flow_pre1, flow_pre_body<1>)
UI_PRE(flow_pre2, flow_pre_body<2>)
UI_PRE(flow_pre3, flow_pre_body<3>)
UI_PRE(flow_pre4, flow_pre_body<4>)
UI_PRE(flow_pre5, flow_pre_body<5>)
// via.gui.ScrollList / ItemsControl .setSelectedIndex(Int32, Boolean) pre: the training menu's primary tab list keeps its
// selection under CRSM's dialogs (the L1+R1 shortcut's move, at its source; logic.hpp TabGuard). Every other list of the
// game: one pointer compare (the pointer is set only while a dialog is up)
int list_select_pre_body(void** argv) {
    void* self = argv[1];
    if (!self || self != g_tab_list.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int64_t v = (int32_t)(uint32_t)(uintptr_t)argv[2], held = g_tab_list_held.load();
    if (!lg::block_list_select(modal(), true, held, v)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    ++g_blocks;
    cmn::journal("ui: menu flow change blocked (tab list setSelectedIndex %lld, held %lld)", (long long)v, (long long)held);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
UI_PRE(list_select_pre, list_select_pre_body)

// TrainingManager._UITrainingMenu._ParamData._PrimaryList: the primary tab bar (a via.gui.ScrollList), the way the game's
// L1+R1 update reaches it
MO* primary_list() { return gx::ref(gx::ref(gx::ref(tm(), "_UITrainingMenu"), "_ParamData"), "_PrimaryList"); }

}  // namespace

bool naming() { return g_naming.load(); }

bool modal() { return lg::tabs_held(g_naming.load(), com::open(), g_box_up.load(), ui::paused()); }

bool hold_tab(MO* tdata, lg::TabAt at, int64_t fallback) {
    static const char* const kAt[3] = { "frame", "CreateViewData", "InitTabItem" };
    const char* where = kAt[(int)at];
    const bool m = modal();
    if (!m && !g_tab.holding()) return false;    // nothing up, nothing held: no read
    lg::TabSpot now;
    MO* list = nullptr;
    if (m && tdata) {
        if (!gx::get_i64(tdata, "SelectIndex", &now.tab)) now.tab = -1;
        if (!gx::get_i64(tdata, "SelectChildIndex", &now.child)) now.child = -1;
    }
    if (m && at == lg::TabAt::Frame) {           // the tab list: from the frame only (never inside the game's chain)
        int32_t v = -1;
        list = primary_list();
        if (list && gx::call_i32(list, "get_SelectedIndex", &v)) now.list = v;
    }
    const bool was = g_tab.holding();
    const lg::TabFix f = g_tab.check(m, now, at, fallback);
    if (!was && g_tab.holding())
        cmn::journal("ui: tab held under CRSM's dialog: tab %lld, row %lld, tab list %lld (%s)", (long long)g_tab.held().tab,
                     (long long)g_tab.held().child, (long long)g_tab.held().list, where);
    if (at == lg::TabAt::Frame) {                // what the tab list's hook compares
        g_tab_list_held.store(g_tab.holding() ? g_tab.held().list : -1);
        g_tab_list.store(m ? list : nullptr);
    }
    if (f.tab) {
        gx::set_field_i64(tdata, "SelectIndex", f.to.tab);
        ++g_tab_holds;
        cmn::journal("ui: tab %lld put back to %lld under CRSM's dialog (%s)", (long long)now.tab, (long long)f.to.tab, where);
    }
    if (f.child) {
        gx::set_field_i64(tdata, "SelectChildIndex", f.to.child);
        ++g_tab_holds;
        cmn::journal("ui: cursor row %lld put back to %lld under CRSM's dialog (%s)", (long long)now.child, (long long)f.to.child, where);
    }
    if (f.list) {                                // the inverse of the shortcut's own call (its hook let this one through)
        const bool ok = gm::call(list, "setSelectedIndex(System.Int32, System.Boolean)", { gm::arg_i32((int32_t)f.to.list), nullptr });
        ++g_tab_holds;
        cmn::journal("ui: tab list %lld put back to %lld under CRSM's dialog%s", (long long)now.list, (long long)f.to.list, ok ? "" : " (call failed)");
    }
    return m;
}

void frame(bool is_paused) {
    // a finished text dialog: the menu's input back, then its outcome (RMT:1641-1646)
    if (g_job && g_job->finished) {
        Job j = std::move(*g_job);
        g_job.reset();
        g_job_up.store(false);
        menu_input_blocked(false);
        cmn::journal("ui: name dialog done (%s)%s", j.why.c_str(), j.outcome ? (j.nj.decided ? ": applied (decided)" : ": applied") : ": nothing to apply");
        if (j.done) j.done(j.outcome.has_value(), j.outcome ? *j.outcome : std::string());
    }
    std::deque<TextReq> texts;
    std::deque<BoxReq> boxes;
    {
        std::lock_guard lk(g_q_mx);
        texts.swap(g_text_q);
        boxes.swap(g_box_q);
    }
    for (auto& r : texts) open_text(std::move(r));
    for (auto& r : boxes) open_box(std::move(r));
    if (is_paused) box_tick();                   // polled under the pause (RMT:2102)
    if (g_job && !g_job->finished) relabel_tick(*g_job);   // route 3A: the explanation line, from the frame
    // the menu's tab: kept on the frame a dialog opens (a text dialog / box opened above; the Community dialog's one frame
    // later, before its window shows), put back if it moved without a rebuild yet; menu closed: left alone. No dialog: one
    // call to forget, then no game read
    const bool m = modal();
    if (m ? is_paused : g_tab.holding()) hold_tab(m ? gx::ref(tm(), "_tData") : nullptr, lg::TabAt::Frame, -1);
    if (!m) { g_tab_list.store(nullptr); g_tab_list_held.store(-1); }
}

void specs(std::vector<mech::hooks::Spec>& out) {
    const char* D = "app.UITextInputDialog";
    const char* T = "app.UIPartsTextInput";
    ui::spec(out, D, "InvokeResultEvent", result_pre, nullptr, "no text dialog (its result could not be taken)", &ok_result);
    ui::spec(out, T, "Init", init_pre, nullptr, "the text field keeps the game's maximum length", &ok_init);
    ui::spec(out, T, "TextInputDecided(System.String)", typed_pre, nullptr, "the typed text read from the dialog only", &ok_typed);
    ui::spec(out, D, "Decide", decide_pre, nullptr, "a decide with the initial text applies nothing", &ok_decide);
    ui::spec(out, D, "InputEvent_Decide", decide_pre, nullptr, "a decide with the initial text applies nothing", &ok_decide2);
    ui::spec(out, D, "Cancel", cancel_pre, nullptr, "no text dialog (a cancel would apply the text)", &ok_cancel);
    ui::spec(out, D, "InputEvent_Cancel", cancel_pre, nullptr, "a cancel by input may apply the text", &ok_cancel2);
    ui::spec(out, D, "SetupText", show_pre, show_post, "the explanation line relabelled from the frame only", &ok_setup);
    mech::hooks::Spec s{ cmn::Feature::Ui, D, kDialogSetupSig, setup_pre, setup_post,
                         "the explanation line relabelled from the frame only (route 3A)", false };
    s.installed = &ok_setup_post;
    out.push_back(s);
    ui::spec(out, D, "FlowEvent_AfterHide", hide_pre, nullptr, "no text dialog (its end could not be seen)", &ok_hide);
    const char* tabs[6] = { "InputTab", "InputTabNext", "InputTabBack", "InputChildTab", "InputChildTabNext", "InputChildTabBack" };
    for (int i = 0; i < 6; ++i) ui::spec(out, "app.UIAgent", tabs[i], agent_tab_pre, nullptr, "tab inputs not blocked under CRSM's dialogs", &ok_tabs[i]);
    ui::spec(out, "app.training.UIFlowTrainingMenu.Param", "OnChangedTab", param_tab_pre, nullptr, "tab changes not blocked under CRSM's dialogs", &ok_ptabs[0]);
    ui::spec(out, "app.training.UIFlowTrainingMenu.Param", "ChangeTabIndex", param_tab_pre, nullptr, "tab changes not blocked under CRSM's dialogs", &ok_ptabs[1]);
    REFPreHookFn flows[6] = { flow_pre0, flow_pre1, flow_pre2, flow_pre3, flow_pre4, flow_pre5 };
    for (int i = 0; i < 6; ++i) ui::spec(out, kFlowTypes[i], kFlowMethods[i], flows[i], nullptr, "L1+R1 not blocked under CRSM's dialogs", &ok_flow[i]);
    // the L1+R1 shortcut's move of the tab list (ScrollList overrides ItemsControl's: both, one pre; a shared body is one hook)
    const char* sel = "setSelectedIndex(System.Int32, System.Boolean)";
    ui::spec(out, "via.gui.ScrollList", sel, list_select_pre, nullptr, "L1+R1 moves the tab list under CRSM's dialogs: put back from the frame", &ok_list[0]);
    ui::spec(out, "via.gui.ItemsControl", sel, list_select_pre, nullptr, "L1+R1 moves the tab list under CRSM's dialogs: put back from the frame", &ok_list[1]);
    if (ui::dump::tab_switch()) {                // tab_dump_on.txt: name the shortcut's update and the tab change handler
        mech::hooks::add_survey(0x145CC5A80, "the L1+R1 shortcut's update (build of 13/10)");
        mech::hooks::add_survey(0x14344CC40, "the tab list's change handler (build of 13/10)");
    }
}

void hand_back() {                               // the UI stopped: nothing applied, the menu's input back, at once
    g_naming.store(false);                       // (the tab blocks end)
    g_tab = lg::TabGuard{};                      // (the tab held no more)
    g_tab_list.store(nullptr);
    g_tab_list_held.store(-1);
    if (g_box) { if (g_box->handle) g_box->handle->release(); auto d = std::move(g_box->done); g_box.reset(); g_box_up.store(false); if (d) d(false); }
    if (g_job) {
        auto d = std::move(g_job->done);
        g_job->done = nullptr;
        if (g_job->finished) { g_job.reset(); g_job_up.store(false); }
        // a dialog still up stays known to its result / cancel / hide hooks (never handed to the game's delegate call); it
        // applies nothing
        if (d) d(false, std::string());
    }
    menu_input_blocked(false);
    std::deque<TextReq> texts;
    std::deque<BoxReq> boxes;
    { std::lock_guard lk(g_q_mx); texts.swap(g_text_q); boxes.swap(g_box_q); }
    for (auto& r : texts) if (r.done) r.done(false, r.initial);
    for (auto& r : boxes) if (r.done) r.done(false);
}

std::string status() {
    char b[360];
    std::snprintf(b, sizeof(b), "text dialogs %llu, boxes %llu, tab blocks %llu, tab holds %llu%s (list hook %d), line relabels %llu (again %llu)%s%s; hooks result %d "
                  "cancel %d hide %d setup text %d, setup post %d (route %s)",
                  (unsigned long long)g_dialogs, (unsigned long long)g_boxes, (unsigned long long)g_blocks, (unsigned long long)g_tab_holds,
                  g_tab.holding() ? " (holding)" : "", ok_list[0] || ok_list[1], (unsigned long long)g_relabels,
                  (unsigned long long)g_relabel_again, g_job ? ", a text dialog is up" : "", g_box ? ", a box is up" : "", ok_result, ok_cancel,
                  ok_hide, ok_setup, ok_setup_post, ok_setup_post ? "3A + 3B" : "3A");
    return b;
}

}  // namespace ui::dlg

// ---- the interface of the Manager page (dialogs.hpp) ----------------------------------------------------------------------
namespace ui::dialogs {

void text_input(const std::string& title, const std::string& initial, size_t max_bytes,
                std::function<void(bool ok, const std::string& text)> done) {
    if (!ui::active()) { if (done) done(false, initial); return; }   // (called back at once: the UI is off)
    std::lock_guard lk(dlg::g_q_mx);
    dlg::g_text_q.push_back(dlg::TextReq{ title, initial, max_bytes ? max_bytes : 64, std::move(done) });
}

void confirm(const std::string& text, std::function<void(bool yes)> done) { confirm(text, std::string(), std::move(done)); }

void confirm(const std::string& text, const std::string& title, std::function<void(bool yes)> done) {
    if (!ui::active()) { if (done) done(false); return; }
    std::lock_guard lk(dlg::g_q_mx);
    dlg::g_box_q.push_back(dlg::BoxReq{ text, title, std::move(done) });
}

void community_open() { ui::com_api::open(); }
void community_share() { ui::com_api::share(); }
void ticker(const std::string& text) { ui::ticker::push(text); }

bool busy() {                                    // lock-free but for the request queue's own small mutex
    if (dlg::g_naming.load() || dlg::g_job_up.load() || dlg::g_box_up.load() || ui::com::open()) return true;
    std::lock_guard lk(dlg::g_q_mx);
    return !dlg::g_text_q.empty() || !dlg::g_box_q.empty();
}

}  // namespace ui::dialogs
