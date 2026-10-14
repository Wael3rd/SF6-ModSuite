// ui/windows — the Record and Replay pages over 30 slots (phase P4, owner "ui"), RecordingManagerTab.lua (RMT) in C++:
// the game's 8 slot rows of each page are kept and become a window over 30 slots BY DATA (their _SlotID), the game's own
// row updaters draw them, our post-hooks add the slot number, the name (a borrowed text), the arrows and the virtual slots'
// frames; the cursor slides the window at the block's edges (UIPartsListBase.GetNextIndex / GetPrevIndex); the Replay
// page skips empty slots (greyed, IsCantFocus). Also here, because the Lua did them in the same module: the menu edges
// that drive the replay / reversal / record layers (CreateViewData = open, the pause falling edge = close), the F on a
// Record row showing slot 9-30 (the record layer arms), Delete on a Record row (a virtual slot cleared, a live slot's name
// cleared), the slot-name keys (T / pad Y, G / R / pad X) and the slot-name dialog.
//
// Not here (P5, ui/manager_page on the core's pause-menu service): the Manager page, the Y / X hints, the sub-page memory
// (RecordType: run at the end of this module's CreateViewData pre-hook, as the Lua's one hook did; the Record / Replay
// page builds of the Function pre-hook feed it, RMT:2014). The name keys below are
// the one reader of T / pad Y for both: on the Manager page T renames the loadout (ui::manager::rename_key).
//
// R4: no menu update is called from its own chain: the rows are redrawn from the list's cursor answer (GetNextIndex), from
// the page build's Function post-hook, and from our own list's UpdateScroll post (rows_dirty), as the Lua did (RMT:1487-1552).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "common.hpp"
#include "dialogs.hpp"
#include "dump.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "hookstack.hpp"
#include "internal.hpp"
#include "logic.hpp"
#include "manager_page.hpp"
#include "mech/bigc.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/labels.hpp"
#include "perf.hpp"

namespace ui::windows {
namespace {

namespace gm = game::gm;
namespace lg = ui::logic;
using gx::MO;

// ---- constants (RMT line numbers) -----------------------------------------------------------------------------------
constexpr int kRecordFunc = 4;                  // TrainingFuncType.RECORD: the "Recording Settings" primary tab (RMT:31)
constexpr int kFtRecord = 107, kFtReplay = 108; // RECORD_SETTING_0 / RECORD_SETTING_1 (RMT:32)
constexpr int kFtRecordSlot = 118;              // RECORD_SLOT (RMT:525)
constexpr int kFtPlaySlot = 131;                // play row (RMT:928)
constexpr int kDyn = 345;                       // DYNAMIC (RMT:29)
constexpr int kStripType2 = 18, kStripType3 = 19;   // RECORD_TAB / REVERSAL_TAB strips (RMT:30, 1891)
constexpr int kStashType = 9;                   // an older build's stash row (RMT:660)
constexpr uint64_t kInstallEvery = 60, kInstallPhase = 33;   // tick % 60 == 33, menu closed (RMT:2114)
// The pages are the training menu's data, built with the training session (the core's page install keeps the game's page
// objects): looked at after each mech event (flow change, training edge, menu close), at the same phase, until they were
// found 5 times or 30 tries ran out (a room without the Recording strip), instead of every second for the whole session.
constexpr int kInstallTries = 30, kInstallSeen = 5;
constexpr int kRefitAfterRowUpdate = 10;        // RMT:1358
constexpr int kRefitAfterRename = 30;           // RMT:1472, 1484
constexpr int kRefitAfterRedraw = 30;           // RMT:1526
constexpr float kXLeft = 40, kXRight = 555, kXName = -180;   // borrowed texts' x (RMT:728)
constexpr uint32_t kColRest = 0xBFFFF3FA, kGlowRest = 0x00CC24CE, kColFocus = 0xFFFFF3FA, kGlowFocus = 0xFFCC24CE;   // RMT:750
const char* const kDonors[] = { "p_InputGuide_0_s_StickLL", "p_InputGuide_0_s_StickRR", "p_InputGuide_0_s_DirL", "p_InputGuide_0_s_DirR" };   // RMT:729
constexpr size_t kNameMax = 64;                 // NAME_MAX (RMT:1378)
constexpr int kKeyT = 0x54, kKeyG = 0x47, kKeyR = 0x52;     // RMT:1380-1381
constexpr uint32_t kPadXFallback = 64;          // via.hid.GamePadButton.RLeft (RMT:1383)
constexpr uint32_t kPadYFallback = 16;          // RUp, read from the live enum (RMT:1382, 1384-1387)

// ---- state (under ui::lock) ---------------------------------------------------------------------------------------------
struct Phys { MO* page = nullptr; MO* rows[8]{}; bool have = false; };
Phys g_rec, g_rep;                               // phys / rphys
bool g_installed = false;
uint64_t g_install_events = UINT64_MAX;          // mech::events() the checks below follow
int g_install_tries = 0, g_install_seen = 0;
int g_win = 0, g_rwin = 0;
uintptr_t g_sec = 0;                             // sec_addr: the page's secondary list (the pooled row groups)
std::optional<int> g_pending_win;                // the Record window to put back after a virtual recording
MO* g_big = nullptr;                             // the replay layer's 30-entry array while the menu is open
bool g_menu_open = false, g_paused_seen = false;
bool g_rows_dirty = false;
std::atomic<bool> g_rows_request{false};          // ui::rows_changed() (any thread): taken at our list's next update
int g_fit_watch = 0;
// the game's wording, learnt on its rows
bool g_name_learnt = false;
std::string g_name_pre, g_name_suf;
std::string g_empty_word = "Empty";
std::optional<std::string> g_start_text;
// Borrowed texts given the centre text's look, by address. The menu's GUI is built again at each opening (test B: 96
// borrows in one session, 32 at most per GUI): a new donor can sit at an old text's address, and a set keyed by address
// then skipped its styling (the donor's input-guide look: other font and size, no glow on the focused row). So a fresh
// borrow is always styled and its base font read again (fit); this set only spares the texts found again under their row.
std::set<uintptr_t> g_styled;
std::map<uintptr_t, std::pair<float, float>> g_base_font;
bool g_native_names = false;
std::string g_state_dis = "DISABLE", g_state_def = "DEFAULT";
bool g_states_read = false, g_states_logged = false;
// keys
lg::KeyEdge g_t, g_del;
uint32_t g_pad_y = kPadYFallback, g_pad_x = kPadXFallback;
bool g_pad_read = false;
// the pads (pad_mask)
constexpr uint64_t kPadListEvery = 30;
std::vector<MO*> g_pads;                         // the connected devices of the last list, held
bool g_pads_held = false;                        // the last list's devices are held (else: listed again every frame)
uint64_t g_pads_next = 0;                        // the frame of the next list
void* g_gamepad = nullptr;                       // via.hid.GamePad (a native singleton: the process's)
reframework::API::Method* g_connecting = nullptr;
uint64_t g_pad_lists = 0;
// the warm-up of the first opening (gx::warm_step), menu closed
constexpr double kWarmBudgetMs = 0.5;
bool g_warm_done = false;
// counters
uint64_t g_shifts = 0, g_rshifts = 0, g_borrows = 0, g_borrow_fail = 0, g_borrow_reused = 0, g_redraws = 0;
// the hooks in place (R18 / missing: the feature they carry is off)
bool ok_function = false, ok_cvd = false, ok_next = false, ok_prev = false, ok_rec_item = false, ok_play_item = false;
bool ok_seclist = false, ok_scroll = false, ok_select = false, ok_remove = false, ok_inittab = false;

model::files::Engine& files() { return mech::files::engine(); }

// ---- small game reads ---------------------------------------------------------------------------------------------------
int64_t i64f(MO* o, const char* f, int64_t d = 0) { int64_t v = d; if (!gx::get_i64(o, f, &v)) v = d; return v; }
MO* list_at(MO* list, int i) { return (list && i >= 0 && i < gm::list_count(list)) ? gm::list_item(list, i) : nullptr; }
int select_child(MO* param) { int32_t v = -1; gx::call_i32(param, "get_SelectChildIndex", &v); return v; }
MO* groups_of(MO* param) { return gx::ref(gx::ref(param, "_SecondaryList"), "_Children"); }
MO* tm() { return gx::singleton("app.training.TrainingManager"); }

// the strip row of the Recording Settings tab (strip_item, RMT:1884-1894)
MO* strip_item() {
    MO* menu = gx::call_obj(gx::ref(tm(), "_UIData"), "get_MenuData");
    const int32_t n = gm::array_len(menu);
    for (int32_t i = 0; i < n; ++i) {
        MO* t = gm::array_get(menu, i);
        if (!t || i64f(t, "_FuncType", -1) != kRecordFunc) continue;
        MO* row = gm::array_get(gx::ref(t, "_ChildData"), 0);
        const int64_t ty = i64f(row, "_Type", -1);
        if (row && (ty == kStripType2 || ty == kStripType3)) return row;
    }
    return nullptr;
}

int recording_tab_index() {                      // recording_tab_index (RMT:2031-2034)
    MO* menu = gx::call_obj(gx::ref(tm(), "_UIData"), "get_MenuData");
    const int32_t n = gm::array_len(menu);
    for (int32_t i = 0; i < n; ++i)
        if (i64f(gm::array_get(menu, i), "_FuncType", -1) == kRecordFunc) return i;
    return -1;
}

void release(Phys& p) {
    if (!p.have) return;
    for (MO*& r : p.rows) { if (r) r->release(); r = nullptr; }
    if (p.page) p.page->release();
    p = Phys{};
}

// capture_physical / capture_replay (RMT:656-676, 937-948): the page's 8 slot rows (array order = slot order), _SlotID
// put back to k - 1, held for the session (the Lua's add_ref)
bool capture(MO* page, int ft, Phys& out) {
    MO* kids = gx::ref(page, "_ChildData");
    if (!kids) return false;
    if (ft == kFtRecordSlot) {                   // a stash row left by an older build of ours: the original array back
        const int32_t n = gm::array_len(kids);
        for (int32_t i = 0; i < n; ++i) {
            MO* c = gm::array_get(kids, i);
            MO* inner = gx::ref(c, "_ChildData");
            if (c && i64f(c, "_FuncType", -1) == kDyn && i64f(c, "_Type", -1) == kStashType && inner) {
                if (gx::api()) perf::counted(gx::api())->obj_set_ref(gx::cl(), page, "_ChildData", inner);
                kids = gx::ref(page, "_ChildData");
                cmn::journal("ui: an older build's stash row found on the Record page: its rows put back");
                break;
            }
        }
    }
    MO* rows[8]{};
    int found = 0;
    const int32_t n = gm::array_len(kids);
    for (int32_t i = 0; i < n; ++i) {
        MO* c = gm::array_get(kids, i);
        if (!c || i64f(c, "_FuncType", -1) != ft) continue;
        if (found < 8) rows[found] = c;
        ++found;
    }
    if (found != 8) return false;
    release(out);
    for (int k = 0; k < 8; ++k) {
        gx::set_field_i64(rows[k], "_SlotID", k);
        rows[k]->add_ref();
        out.rows[k] = rows[k];
    }
    page->add_ref();
    out.page = page;
    out.have = true;
    return true;
}

// install / still_installed (RMT:1898-1938) without the Manager page (P5): the Record and Replay pages found by their
// FuncType in the strip, whatever else the strip holds (2 pages, or 3 with a client page of the core's pause-menu service)
void install_check() {
    MO* row = strip_item();
    MO* pages = gx::ref(row, "_ChildData");
    MO* rec = nullptr, *rep = nullptr;
    const int32_t n = gm::array_len(pages);
    for (int32_t i = 0; i < n; ++i) {
        MO* p = gm::array_get(pages, i);
        const int64_t ft = i64f(p, "_FuncType", -1);
        if (ft == kFtRecord) rec = p; else if (ft == kFtReplay) rep = p;
    }
    if (!rec || !rep) { g_installed = false; return; }
    bool changed = false;
    if (!g_rec.have || g_rec.page != rec) { changed = true; if (!capture(rec, kFtRecordSlot, g_rec)) release(g_rec); }
    if (!g_rep.have || g_rep.page != rep) { changed = true; if (!capture(rep, kFtPlaySlot, g_rep)) release(g_rep); }
    const bool was = g_installed;
    g_installed = g_rec.have && g_rep.have;
    if (changed || was != g_installed)
        cmn::journal("ui: Record / Replay pages %s (record rows %d, replay rows %d)", g_installed ? "captured" : "not ready",
                     g_rec.have ? 8 : 0, g_rep.have ? 8 : 0);
}

// page_shown (RMT:957-964): Record / Replay / none, by the first slot row's data
enum Page { kNone = 0, kRecord = 1, kReplay = 2 };
Page page_shown(MO* param) {
    if (!param) return kNone;
    MO* list = gx::ref(param, "_ViewDataList");
    if (!list || gm::list_count(list) <= lg::kFirstRow) return kNone;
    MO* d = gx::ref(gm::list_item(list, lg::kFirstRow), "Data");
    if (!d) return kNone;
    if (g_rec.have && d == g_rec.rows[0]) return kRecord;
    if (g_rep.have && d == g_rep.rows[0]) return kReplay;
    return kNone;
}

// The page shown and the cursor row, as the row updaters' pre-hooks read them: the game updates the 8 rows one after the
// other with nothing in between, so they are read once per batch: kept for the frame until anything that changes them
// runs (CreateViewData, a page build, the row list, the cursor, a delete, the tab: g_view_gen).
struct PreView { MO* param = nullptr; uint64_t tick = UINT64_MAX; uint64_t gen = 0; Page pg = kNone; int sel = -1; };
PreView g_pre_view;
std::atomic<uint64_t> g_view_gen{0};
Page page_and_cursor(MO* param, int* sel) {
    const uint64_t gen = g_view_gen.load();
    if (g_pre_view.param != param || g_pre_view.tick != ui::tick() || g_pre_view.gen != gen) {
        const Page pg = page_shown(param);
        g_pre_view = PreView{ param, ui::tick(), gen, pg, pg == kNone ? -1 : select_child(param) };
    }
    *sel = g_pre_view.sel;
    return g_pre_view.pg;
}
void view_changed() { g_view_gen.fetch_add(1); }

// ---- windows -----------------------------------------------------------------------------------------------------------
void set_window(int nw) {                        // RMT:678-687
    g_win = lg::clamp_win(nw);
    if (!g_rec.have) return;
    for (int k = 1; k <= 8; ++k) gx::set_field_i64(g_rec.rows[k - 1], "_SlotID", lg::record_slot_id(g_win + k));
}

void set_rwindow(int nw) {                       // RMT:949-956
    g_rwin = lg::clamp_win(nw);
    if (!g_rep.have) return;
    for (int k = 1; k <= 8; ++k) gx::set_field_i64(g_rep.rows[k - 1], "_SlotID", lg::replay_slot_id(g_rwin + k, g_big != nullptr));
}

void update_row(MO* param, const char* sig, MO* group, int idx) {
    gx::invoke(param, sig, { gx::P(group), gx::I(idx) });
}

bool apply_shift(MO* param, int delta) {         // RMT:689-700
    const int nw = lg::clamp_win(g_win + delta);
    if (nw == g_win) return false;
    set_window(nw);
    MO* groups = groups_of(param);
    for (int idx = lg::kFirstRow; idx <= lg::kLastRow; ++idx)
        if (MO* g = list_at(groups, idx)) update_row(param, "UpdateRecordSlotItem(app.UIPartsGroup, System.Int32)", g, idx);
    ++g_shifts;
    cmn::journal("ui: window -> slots %d-%d", g_win + 1, g_win + lg::kWindow);
    return true;
}

bool row_disabled(int n) {                       // RMT:1180-1183
    if (!g_big) return false;
    MO* sd = gm::array_get(g_big, n - 1);
    bool valid = true;
    return sd && gm::get_bool(sd, "IsValid", &valid) && !valid;
}

bool apply_rwindow(MO* param, int nw_in) {       // RMT:1246-1263
    const int nw = lg::clamp_win(nw_in);
    if (nw == g_rwin) return false;
    set_rwindow(nw);
    MO* list = gx::ref(param, "_ViewDataList");
    MO* groups = groups_of(param);
    for (int idx = lg::kFirstRow; idx <= lg::kLastRow; ++idx) {
        const int k = idx - lg::kFirstRow + 1, n = g_rwin + k;
        const bool dis = row_disabled(n);
        MO* r = g_rep.rows[k - 1];               // the game derives the disabled look / focus from the row data
        gx::set_field_i64(r, "IsEnabled", dis ? 0 : 1);
        gx::set_field_i64(r, "IsCantFocus", dis ? 1 : 0);
        if (MO* vd = list_at(list, idx)) gx::set_field_i64(vd, "IsDisable", dis ? 1 : 0);
        if (MO* g = list_at(groups, idx)) update_row(param, "UpdatePlaySlotItem(app.UIPartsGroup, System.Int32)", g, idx);
    }
    ++g_rshifts;
    cmn::journal("ui: replay window -> slots %d-%d", g_rwin + 1, g_rwin + lg::kWindow);
    return true;
}

// ---- the menu session's cache (under ui::lock) ----------------------------------------------------------------------------
// The row paths (a row update, the cursor's 8-row pass, the re-fit) found every element by name, walking the GUI tree each
// time (get_Child / get_Next / get_Name per child, 6 to 8 walks per row): hundreds of game calls a frame while the Record /
// Replay rows were updated. Within one build of the menu's GUI a row's elements stay where they are, so what was found is
// kept for the session: the elements by (parent, name) (found ones only: a missing one is looked for again), the two
// texts placed under each row item (once both are there), whether a row control has the DISABLE play state, the
// training menu's param, the dummy's slot array, and what we last wrote into each text (the re-fit follows real changes
// only). Emptied at every boundary: CreateViewData (menu open, tab rebuild, the rebuild under the text dialog), a Record /
// Replay page build, a new row list, the menu close, the hand-back. The borrow path (donor texts that move) never uses it.
struct EdgeTexts { MO* N = nullptr; MO* R = nullptr; };
struct KidKey {
    uintptr_t parent; uintptr_t name;            // the name literal's address
    bool operator<(const KidKey& o) const { return parent != o.parent ? parent < o.parent : name < o.name; }
};
std::map<KidKey, MO*> g_kids;
std::map<uintptr_t, EdgeTexts> g_edges;          // a row item -> its name text and arrow (both found or borrowed)
std::map<uintptr_t, bool> g_dis_state;           // a row control -> it has the DISABLE play state
std::map<uintptr_t, std::string> g_last_text;    // a text -> what we wrote into it last
std::map<uintptr_t, MO*> g_ctrl;                 // a row group -> its control (get_Control)
MO* g_param_cache = nullptr;                     // gx::find_param()
MO* g_slots_arr = nullptr;                       // the dummy's RecordSlots (the 30-entry array while the menu is open)
bool g_text_changed = false;                     // a put_text changed a text / a text was styled (the re-fit follows)
uint64_t g_refit_tick = UINT64_MAX;              // the frame of the last re-fit (one a frame)
uint64_t g_kid_walks = 0, g_kid_hits = 0, g_sessions = 0, g_soft_kept = 0;
uint64_t g_session_tick = UINT64_MAX;            // the frame of the last reset
// A row's last full pass (texts, tint, the edge texts in place). The menu opening runs the game's row updater batch and
// set_SelectChildIndex 5 times in ONE frame (perf log 13/10: 40 row updates and 5 cursor posts): a cursor post whose row
// was drawn in that same frame for the same slot, focus and window, with nothing it reads changed since (g_epoch), draws
// it again identically: skipped. Only within the frame of that pass (the next frame's posts always draw).
struct RowPass { uint64_t tick = UINT64_MAX; bool replay = false; int n = 0; bool focused = false; int win = -1; uint64_t epoch = 0; };
std::map<uintptr_t, RowPass> g_row_pass;         // a row group -> its last full pass
std::atomic<uint64_t> g_epoch{0};                // +1 when a row's inputs other than slot / focus / window change
uint64_t g_pass_skips = 0;

void session_reset() {
    g_kids.clear();
    g_edges.clear();
    g_dis_state.clear();
    g_last_text.clear();
    g_ctrl.clear();
    g_row_pass.clear();
    g_param_cache = nullptr;
    g_slots_arr = nullptr;
    g_session_tick = ui::tick();
    g_epoch.fetch_add(1);
    ++g_sessions;
}

// The boundaries inside an opening (a page build, a new row list: 3 to 5 of them in the opening's frame, each walking the
// 8 rows' trees again) come in the frame of the menu's own reset (CreateViewData): the elements found since were found in
// that frame's GUI, and a GUI element is released at a frame's end, not in the middle of the menu's update (a cached
// address cannot be another element's yet), so the cache is kept then. A boundary in a later frame resets as before.
void session_reset_soft() {
    if (g_session_tick == ui::tick()) { ++g_soft_kept; return; }
    session_reset();
}

MO* kid(MO* parent, const char* nm) {            // gx::child, kept for the session (nm: a literal)
    if (!parent) return nullptr;
    const KidKey k{ (uintptr_t)parent, (uintptr_t)nm };
    if (auto it = g_kids.find(k); it != g_kids.end()) { ++g_kid_hits; return it->second; }
    ++g_kid_walks;
    MO* c = gx::child(parent, nm);
    if (c) g_kids.emplace(k, c);
    return c;
}

MO* control_of(MO* group) {                      // UIPartsGroup.get_Control, kept for the session
    if (!group) return nullptr;
    if (auto it = g_ctrl.find((uintptr_t)group); it != g_ctrl.end()) return it->second;
    MO* c = gx::call_obj(group, "get_Control");
    if (c) g_ctrl.emplace((uintptr_t)group, c);
    return c;
}

// The row's last full pass drew exactly this (same frame, slot, focus, window, nothing it reads changed since)
bool drawn_now(MO* group, bool replay, int n, bool focused, int win) {
    auto it = g_row_pass.find((uintptr_t)group);
    if (it == g_row_pass.end()) return false;
    const RowPass& p = it->second;
    return p.tick == ui::tick() && p.replay == replay && p.n == n && p.focused == focused && p.win == win && p.epoch == g_epoch.load();
}
void note_pass(MO* group, bool replay, int n, bool focused, int win) {
    g_row_pass[(uintptr_t)group] = RowPass{ ui::tick(), replay, n, focused, win, g_epoch.load() };
}

MO* param_now() {                                // gx::find_param, kept for the session
    if (!g_param_cache) g_param_cache = gx::find_param();
    return g_param_cache;
}

// A text we write: always written (the game's row updater may have put its own text back: the core compares, R7); true
// (and g_text_changed) when it differs from what we wrote into it last this session.
bool put_text(MO* t, const std::string& v) {
    if (!t) return false;
    gx::set_text(t, v);
    auto [it, fresh] = g_last_text.try_emplace((uintptr_t)t, v);
    if (!fresh && it->second == v) return false;
    it->second = v;
    g_text_changed = true;
    return true;
}

// ---- texts -------------------------------------------------------------------------------------------------------------
std::optional<std::string> live_frames(int n) {  // live_frames_text (RMT:1090-1095), path B
    MO* sd = nullptr;
    {
        std::lock_guard lk(mech::lock());
        if (!g_slots_arr) g_slots_arr = gx::ref(mech::bigc::fighter_data(), "RecordSlots");
        sd = gm::array_get(g_slots_arr, n - 1);
    }
    if (!sd) return std::nullopt;
    bool valid = false;
    gm::get_bool(sd, "IsValid", &valid);
    int64_t frames = 0;                          // SlotData.Frame: an integer field (P3 reads and writes it so)
    gm::get_int(sd, "Frame", &frames);
    return model::labels::live_frames_text(valid, (double)frames, g_empty_word);
}

std::string display_name(int n) {                // slot_display_name (RMT:837-840)
    std::lock_guard lk(mech::lock());
    if (n <= 8) {
        const auto v = files().slot_name(n - 1);
        return v.is_string() ? v.get<std::string>() : std::string();
    }
    return files().vslots().name(n);
}

std::string virtual_text(int n) {                // virtual_slot_text (RMT:516-518)
    std::lock_guard lk(mech::lock());
    return files().vslots().text(n, g_empty_word);
}

void tint_text(MO* t, bool focused) {            // RMT:756-771: the fill's alphas, the glow
    auto colour = [&](const char* getter, const char* setter, uint32_t want) {
        uint32_t cur = 0;
        if (gx::color_of(t, getter, &cur) && cur == want) return;
        gx::set_color(t, setter, want);
    };
    auto alpha = [&](const char* getter, const char* setter, uint32_t a) {
        uint32_t cur = 0;
        if (!gx::color_of(t, getter, &cur)) return;
        const uint32_t want = (cur & 0x00FFFFFFu) | (a << 24);
        if (cur != want) gx::set_color(t, setter, want);
    };
    colour("get_Color", "set_Color(via.Color)", focused ? kColFocus : kColRest);
    alpha("get_ColorTop", "set_ColorTop(via.Color)", focused ? 0xFFu : 0xBFu);
    alpha("get_ColorBottom", "set_ColorBottom(via.Color)", focused ? 0xFFu : 0xBFu);
    colour("get_GlowColor", "set_GlowColor(via.Color)", focused ? kGlowFocus : kGlowRest);
}

// (share: the centre text's values read for the row's first borrowed text, given to its second: one read per row)
void style(MO* t, char key, MO* ref, gx::StyleShare* share) {   // style (RMT:791-804)
    g_styled.insert((uintptr_t)t);
    g_text_changed = true;                       // its font is the centre text's now: the re-fit follows
    if (dump::on()) dump::borrowed(t, ref, false);
    std::string failed;
    const int copied = ref ? gx::copy_style(t, ref, &failed, share) : 0;
    static bool logged = false;                  // once per session: the copy took (font size and glow as the centre text's)
    if (ref && !logged) {
        logged = true;
        float tw = 0, th = 0, rw = 0, rh = 0;
        bool tg = false, rg = false;
        gx::size_of(t, "get_FontSize", &tw, &th);
        gx::size_of(ref, "get_FontSize", &rw, &rh);
        gx::call_bool(t, "get_GlowEnable", &tg);
        gx::call_bool(ref, "get_GlowEnable", &rg);
        cmn::journal("ui: borrowed text styled: %d properties copied%s%s; font %.0fx%.0f (centre text %.0fx%.0f), glow %d (centre text %d)",
                     copied, failed.empty() ? "" : ", not copied: ", failed.c_str(), tw, th, rw, rh, tg ? 1 : 0, rg ? 1 : 0);
    }
    gx::invoke(t, "set_DisableMessageTag(System.Boolean)", { gx::B(true) });   // "<" and ">" shown as typed
    int32_t prio = 0;
    if (ref) gx::call_i32(ref, "get_Priority", &prio);
    gx::invoke(t, "set_Priority(System.Int32)", { gx::I(prio + 2) });   // above the row's highlight bar
    const float x = key == 'L' ? kXLeft : key == 'R' ? kXRight : kXName;
    gx::set_pos(t, x, 0, 0);
    gx::set_text(t, "");
    gx::set_visible(t, true);
    if (dump::on()) dump::borrowed(t, ref, true);
}
void style_once(MO* t, char key, MO* ref, gx::StyleShare* share) {   // a text found again under its row: styled once per address
    if (!g_styled.count((uintptr_t)t)) style(t, key, ref, share);
}

// edge_texts (RMT:780-835): two donor texts of the hidden 3-tab strip prefab re-parented into the slot item (the name
// after "Slot N", the arrow at the right edge); found again by parent / position after a reload (once per session)
bool edge_texts(MO* ctrl, MO* item, EdgeTexts* out) {
    if (!item) return false;
    if (auto it = g_edges.find((uintptr_t)item); it != g_edges.end()) { *out = it->second; ++g_kid_hits; return true; }
    MO* parN = item, *parR = kid(item, "c_ItemBase_slot"), *parL = kid(item, "c_disable");
    if (!parR || !parL) return false;
    MO* found[3]{};                              // N, R, L
    auto slot_of = [](char k) { return k == 'N' ? 0 : k == 'R' ? 1 : 2; };
    const std::pair<char, MO*> pars[3] = { { 'N', parN }, { 'R', parR }, { 'L', parL } };
    for (const auto& [key, par] : pars) {
        for (MO* c = gx::first_child(par); c; c = gx::next(c)) {
            if (gx::name(c) != "e_text_0") continue;
            char k = key;
            if (key == 'N') {                    // classify: the item itself can hold an older build's left arrow
                float p[3]{};
                if (gx::vec3_of(c, "get_Position", p)) k = p[0] < -400 ? 'L' : p[0] > 400 ? 'R' : 'N';
            }
            found[slot_of(k)] = c;
        }
    }
    MO* ref = kid(item, "e_txt_center");         // centred at 0, no box offset (RMT:790)
    gx::StyleShare share;                        // the centre text's style read once for the row's two texts
    if (found[0]) style_once(found[0], 'N', ref, &share);
    if (found[1]) style_once(found[1], 'R', ref, &share);
    if (!(found[0] && found[1])) {
        MO* strip = gx::child(ctrl, "p_TrainingSecondaryTab_19");
        MO* tab = strip ? gx::child(strip, "c_tab") : nullptr;
        if (!tab) return false;
        std::vector<MO*> free_texts;
        for (const char* donor : kDonors) {
            MO* g = gx::child(tab, donor);
            if (MO* t = g ? gx::child(g, "e_text_0") : nullptr) free_texts.push_back(t);
        }
        if (found[2]) {                          // an older build's left arrow: the panel back to the game
            gx::set_text(found[2], "");
            MO* par = gx::parent(found[2]);
            MO* icon = par ? gx::child(par, "e_tex_disable") : nullptr;
            if (icon && !gx::visible(icon)) gx::set_visible(icon, true);
            if (par && gx::visible(par)) gx::set_visible(par, false);
        }
        for (char key : { 'N', 'R' }) {
            MO*& slot = found[slot_of(key)];
            if (slot) continue;
            if (free_texts.empty()) break;
            MO* t = free_texts.front();
            free_texts.erase(free_texts.begin());
            MO* par = key == 'N' ? parN : parR;
            SF6CoreArg ret{};
            bool ok = gx::invoke(par, "addChild(via.gui.PlayObject)", { gx::P(t) }, &ret) && (ret.v.i & 0xFF) != 0;
            if (!ok) {                           // RMT:823 (R22: remove() breaks a text; the Lua's own fallback, kept)
                gx::invoke(t, "remove()");
                ok = gx::invoke(par, "addChild(via.gui.PlayObject)", { gx::P(t) }, &ret) && (ret.v.i & 0xFF) != 0;
            }
            if (ok) {                            // a fresh donor: always styled, its base font read again (fit)
                if (g_styled.count((uintptr_t)t)) ++g_borrow_reused;
                g_base_font.erase((uintptr_t)t);
                style(t, key, ref, &share);
                slot = t;
                ++g_borrows;
            }
            else { ++g_borrow_fail; cmn::journal("ui: borrow failed for %c", key); }
        }
    }
    if (!(found[0] && found[1])) return false;
    out->N = found[0];
    out->R = found[1];
    g_edges[(uintptr_t)item] = *out;
    return true;
}

void fit(MO* t, MO* name_el, MO* frames_el) {    // fit_name (RMT:849-875)
    if (!t) return;
    const uintptr_t a = (uintptr_t)t;
    auto it = g_base_font.find(a);
    if (it == g_base_font.end()) {
        float w = 0, h = 0;
        if (!gx::size_of(t, "get_FontSize", &w, &h)) return;
        it = g_base_font.emplace(a, std::make_pair(w, h)).first;
    }
    lg::FitIn in;
    float h = 0, pos[3]{};
    in.has_name = name_el && gx::size_of(name_el, "get_DisplaySize", &in.name_w, &h);
    in.has_frames = frames_el && gx::size_of(frames_el, "get_DisplaySize", &in.frames_w, &h);
    if (!gx::size_of(t, "get_DisplaySize", &in.own_w, &h)) return;
    if (!gx::size_of(t, "get_FontSize", &in.font_w, &h)) return;
    if (!gx::vec3_of(t, "get_Position", pos)) return;
    in.base_w = it->second.first;
    in.base_h = it->second.second;
    in.pos_x = pos[0];
    const lg::FitOut o = lg::fit_name(in);
    if (o.set_font) gx::set_font(t, o.font_w, o.font_h);
    if (o.set_x) gx::set_pos(t, o.x, 0, 0);
}

void learn_name(MO* name, int win, int n) {      // RMT:880-883 (Record), 1224-1226 (Replay)
    if (!name || win != 0 || n != 1 || g_name_learnt) return;
    const std::string t = gx::msg(name);
    if (lg::learn_name_format(t, &g_name_pre, &g_name_suf)) { g_name_learnt = true; g_epoch.fetch_add(1); }
}

// repeat: the row was drawn in this frame for the same slot, focus and window (drawn_now) and the game's updater ran again
// (the opening's 5 batches): our two texts are only ours (their text, size, position and tint are what this frame's pass
// wrote: nothing else writes them), so only what the game's updater can touch is checked again, their visibility.
void edge_finish(const EdgeTexts& tx, int n, int win, bool focused, MO* name, MO* centre, bool repeat = false) {
    g_native_names = true;
    if (!repeat) {
        put_text(tx.R, lg::window_arrow(n - win, win));
        put_text(tx.N, display_name(n));
        fit(tx.N, name, centre);
    }
    for (MO* t : { tx.N, tx.R }) {
        if (!repeat) tint_text(t, focused);
        if (!gx::visible(t)) gx::set_visible(t, true);   // the focus animation hides what it does not know
        if (gx::force_invisible(t)) gx::set_force_invisible(t, false);
    }
}

// slot_row_texts (RMT:876-918): a Record row
void slot_row_texts(MO* group, int n, bool focused) {
    const bool repeat = drawn_now(group, false, n, focused, g_win);   // (before this pass learns anything)
    MO* ctrl = control_of(group);
    MO* item = ctrl ? kid(ctrl, "p_GroupItem_6") : nullptr;
    if (!item) return;
    MO* name = kid(item, "e_txt_name");
    learn_name(name, g_win, n);
    if (name && (g_name_learnt || n > 8)) put_text(name, lg::slot_text(g_name_learnt, g_name_pre, g_name_suf, n));   // always (back at 0 too)
    else if (name) g_text_changed = true;        // (the game's own name text: not followed, re-fit as before)
    MO* centre = kid(item, "e_txt_center");
    if (n > 8) put_text(centre, virtual_text(n));
    else if (centre) {                           // a live slot: the game's text lags an import; ours follows the data
        const std::string cur = gx::msg(centre);
        if (lg::learnable_empty_word(cur) && cur != g_empty_word) { g_empty_word = cur; g_epoch.fetch_add(1); }   // learn "Empty" / "Vide"
        if (const auto want = live_frames(n)) put_text(centre, *want);
        else g_text_changed = true;              // (the game's own frames text: not followed, re-fit as before)
    }
    MO* sub = kid(item, "e_txt_sub");
    if (!sub) return;
    const int k = n - g_win;
    const std::string up = (k == 1 && g_win > 0) ? lg::kUp : "";
    const std::string down = (k == lg::kWindow && g_win < lg::kMaxWin) ? lg::kDown : "";
    const std::string m = gx::msg(sub);
    if (focused && !g_start_text && lg::learnable_start_text(m)) { g_start_text = m; g_epoch.fetch_add(1); }
    const std::string base = g_start_text ? *g_start_text : "Start Recording";
    EdgeTexts tx;
    if (edge_texts(ctrl, item, &tx)) {
        edge_finish(tx, n, g_win, focused, name, centre, repeat && drawn_now(group, false, n, focused, g_win));
        if (m != base) gx::set_text(sub, base);  // the game's own right column back
        if (dump::on()) dump::slot_row(n, focused, tx.N, name, centre, sub, tx.R);
        note_pass(group, false, n, focused, g_win);
        return;
    }
    // fallback: the right column ("Start Recording" of the focused row) carries the arrow of the edge rows
    const std::string hint = !down.empty() ? down : up;
    const std::string want = focused ? (!hint.empty() ? base + "  " + hint : base) : hint;
    if (!want.empty()) { gx::set_text(sub, want); if (!gx::visible(sub)) gx::set_visible(sub, true); }
    else gx::set_text(sub, base);                // unfocused, no arrow: the game's own text back
}

// state_consts / has_state / pose_disabled (RMT:1186-1217): the disabled look is a play state of the row's controls
void read_states() {
    if (g_states_read) return;
    g_states_read = true;
    gx::TD* ptd = gm::type("app.training.UIFlowTrainingMenu.Param");
    bool ok_d = false, ok_n = false;
    for (auto [nm, dst, ok] : { std::tuple<const char*, std::string*, bool*>{ "STATE_DISABLE", &g_state_dis, &ok_d },
                                std::tuple<const char*, std::string*, bool*>{ "STATE_DEFAULT", &g_state_def, &ok_n } }) {
        reframework::API::Field* f = ptd ? gm::field(ptd, nm) : nullptr;
        if (!f || !f->is_static() || f->is_literal()) continue;
        void* p = f->get_data_raw(nullptr, false);
        if (p && *static_cast<void**>(p)) { *dst = gx::str(*static_cast<void**>(p)); *ok = !dst->empty(); }
        if (!*ok) *dst = (std::string(nm) == "STATE_DISABLE") ? "DISABLE" : "DEFAULT";
    }
    cmn::journal("ui: row states: %s / %s%s", g_state_dis.c_str(), g_state_def.c_str(), (ok_d && ok_n) ? "" : " (constants not read)");
}

bool has_state(MO* c, const std::string& nm) {
    int32_t cnt = 0;
    if (!gx::call_i32(c, "getPlayStateNamesCount", &cnt)) return false;
    for (int32_t i = 0; i < (cnt < 16 ? cnt : 16); ++i) {
        reframework::InvokeRet r{};
        if (gm::call(c, "getPlayStateNames", { gm::arg_i32(i) }, &r) && r.ptr && gx::str(r.ptr) == nm) return true;
    }
    return false;
}

void pose_disabled(MO* ctrl, MO* item, bool disabled) {
    read_states();
    for (MO* c : { item, ctrl }) {
        if (!c) continue;
        auto st = g_dis_state.find((uintptr_t)c);  // (its play state names, read once per session)
        if (st == g_dis_state.end()) st = g_dis_state.emplace((uintptr_t)c, has_state(c, g_state_dis)).first;
        if (!st->second) continue;
        if (!g_states_logged) {
            g_states_logged = true;
            std::string all;
            int32_t cnt = 0;
            gx::call_i32(c, "getPlayStateNamesCount", &cnt);
            for (int32_t i = 0; i < (cnt < 16 ? cnt : 16); ++i) {
                reframework::InvokeRet r{};
                if (gm::call(c, "getPlayStateNames", { gm::arg_i32(i) }, &r) && r.ptr) all += (all.empty() ? "" : "|") + gx::str(r.ptr);
            }
            cmn::journal("ui: row control states: %s", all.c_str());
        }
        reframework::InvokeRet r{};
        const std::string cur = (gm::call(c, "get_PlayState", {}, &r) && r.ptr) ? gx::str(r.ptr) : std::string();
        // only in and out of DISABLE: the focus / select states belong to the game (the highlight vanished, 08/09)
        if (disabled && cur != g_state_dis) gx::invoke(c, "set_PlayState(System.String)", { gx::S(g_state_dis.c_str()) });
        else if (!disabled && cur == g_state_dis) gx::invoke(c, "set_PlayState(System.String)", { gx::S(g_state_def.c_str()) });
        return;
    }
}

// play_row_texts (RMT:1219-1245): a Replay row
void play_row_texts(MO* group, int n, bool focused) {
    const bool repeat = drawn_now(group, true, n, focused, g_rwin);    // (before this pass learns anything)
    MO* ctrl = control_of(group);
    MO* item = ctrl ? kid(ctrl, "p_GroupItem_5") : nullptr;
    if (!item) return;
    if (g_big) pose_disabled(ctrl, item, row_disabled(n));
    MO* name = kid(item, "e_text_name");
    learn_name(name, g_rwin, n);
    if (name && (g_name_learnt || n > 8)) put_text(name, lg::slot_text(g_name_learnt, g_name_pre, g_name_suf, n));
    MO* centre = kid(item, "e_txt_center");
    if (n > 8) put_text(centre, virtual_text(n));        // the game's frames text comes from its 8-entry MenuSlotData
    else if (centre) { if (const auto want = live_frames(n)) put_text(centre, *want); }
    EdgeTexts tx;
    if (edge_texts(ctrl, item, &tx)) {
        edge_finish(tx, n, g_rwin, focused, name, centre, repeat && drawn_now(group, true, n, focused, g_rwin));
        note_pass(group, true, n, focused, g_rwin);
    }
}

void refit_rows() {                              // RMT:1488-1500
    MO* param = param_now();
    const Page pg = page_shown(param);
    if (pg == kNone) return;
    MO* groups = groups_of(param);
    for (int idx = lg::kFirstRow; idx <= lg::kLastRow; ++idx) {
        MO* g = list_at(groups, idx);
        MO* ctrl = control_of(g);
        MO* item = ctrl ? kid(ctrl, pg == kRecord ? "p_GroupItem_6" : "p_GroupItem_5") : nullptr;
        if (!item) continue;
        EdgeTexts tx;
        if (edge_texts(ctrl, item, &tx)) fit(tx.N, kid(item, pg == kRecord ? "e_txt_name" : "e_text_name"), kid(item, "e_txt_center"));
    }
}

void redraw_rows() {                             // RMT:1501-1509
    MO* param = param_now();
    const Page pg = page_shown(param);
    if (pg == kNone) return;
    MO* groups = groups_of(param);
    for (int idx = lg::kFirstRow; idx <= lg::kLastRow; ++idx)
        if (MO* g = list_at(groups, idx))
            update_row(param, pg == kRecord ? "UpdateRecordSlotItem(app.UIPartsGroup, System.Int32)" : "UpdatePlaySlotItem(app.UIPartsGroup, System.Int32)", g, idx);
    ++g_redraws;
}

// ---- the menu edges (RMT:1140-1175) ---------------------------------------------------------------------------------
void on_menu_open() {
    g_pads_next = 0;                             // the pads listed again at the menu's first frame (name_key_tick)
    if (cmn::owns(cmn::Feature::Reversal)) reversal_ui::menu_open();   // menu_fns.open first (ReversalSlots' 30 rows)
    g_big = cmn::owns(cmn::Feature::Replay) ? static_cast<MO*>(mech::replay::menu_open()) : nullptr;   // then the replay part
    cmn::journal("ui: menu open: 30-entry array %s", g_big ? "in place" : "not installed");
}

void on_menu_close() {                           // (a layer handed back after its faults cleans up itself: not called then)
    cmn::journal("ui: menu close detected");
    if (cmn::owns(cmn::Feature::Reversal)) reversal_ui::menu_close();
    g_big = nullptr;                             // (RMT:1157's native-action hold: the Manager page owns it, ui/manager_page)
    if (cmn::owns(cmn::Feature::Replay)) mech::replay::menu_close();
    session_reset();                             // the menu's GUI goes with it
}

// ---- slot names (RMT:1447-1486, 1748-1770) -------------------------------------------------------------------------
void set_slot_name(int n, const std::string& name) {
    {
        std::lock_guard lk(mech::lock());
        files().name_slot(n, model::files::json(name));   // 1-8 memory, 9-30 the file, cut on a character boundary (fix 7.4)
    }
    g_rows_dirty = true;
    g_epoch.fetch_add(1);
    g_fit_watch = kRefitAfterRename;
    cmn::journal("ui: slot %d named", n);
}

int slot_under_cursor() {                        // RMT:1748-1754
    MO* param = param_now();
    if (!param || page_shown(param) != kRecord) return 0;
    const int cur = select_child(param);
    if (cur < lg::kFirstRow || cur > lg::kLastRow) return 0;
    MO* d = gx::call_obj(param, "get_CurrentData");
    if (!d || d != g_rec.rows[cur - lg::kFirstRow]) return 0;
    return g_win + cur - lg::kFirstRow + 1;
}

// The pads' buttons (RMT:1388-1398: every connected pad's), read every frame while the menu is up (a press must be seen
// on its frame). The connected devices are listed 1 frame in 30 (and at each menu opening) and held meanwhile (add_ref):
// a frame between two lists reads each held device's buttons only, one call per connected pad, none without a pad. A
// device that is not a managed object is not held: the list is then read every frame, as before.

void pads_drop() {
    for (MO* p : g_pads) p->release();
    g_pads.clear();
    g_pads_held = false;
}

uint32_t pad_buttons(MO* p) {
    reframework::InvokeRet rb{};
    return (p && gm::call(p, "get_Button", {}, &rb)) ? rb.dword : 0;
}

uint32_t pads_list() {                           // lists the devices (held when they can be) and reads their buttons
    pads_drop();
    ++g_pad_lists;
    if (!g_gamepad) g_gamepad = reframework::API::get()->get_native_singleton("via.hid.GamePad");
    if (!g_connecting) {
        if (gx::TD* gt = gm::type("via.hid.GamePad")) g_connecting = gt->find_method("get_ConnectingDevices");
    }
    if (!g_gamepad || !g_connecting) return 0;
    reframework::InvokeRet r{};
    if (!gm::call(reinterpret_cast<MO*>(g_gamepad), g_connecting, {}, &r)) return 0;
    MO* devices = static_cast<MO*>(r.ptr);
    if (!devices) return 0;
    uint32_t combined = 0;
    bool held = true;
    const int32_t n = gm::list_count(devices);
    for (int32_t i = 0; i < n; ++i) {
        MO* p = gm::list_item(devices, i);
        if (!p) continue;
        combined |= pad_buttons(p);
        if (held && gm::is_managed(p)) { p->add_ref(); g_pads.push_back(p); }
        else held = false;
    }
    if (!held) pads_drop();
    g_pads_held = held;
    return combined;
}

uint32_t pad_mask() {
    if (!g_pad_read) {
        g_pad_read = true;
        if (gx::TD* pb = gm::type("via.hid.GamePadButton")) {
            if (reframework::API::Field* f = pb->find_field("RUp")) { const int32_t v = f->get_data<int32_t>(nullptr); if (v) g_pad_y = (uint32_t)v; }
        }
    }
    if (!g_pads_held || tick() >= g_pads_next) {
        g_pads_next = tick() + kPadListEvery;
        return pads_list();
    }
    uint32_t combined = 0;
    for (MO* p : g_pads) combined |= pad_buttons(p);
    return combined;
}

bool key_down(int vk) { return cmn::key_down(vk); }   // reframework:is_key_down: the game window's keys only

// RMT:1755-1770: the one reader of the name keys (T / pad Y, G / R / pad X) for the slot rows (P4) and the Manager page (P5)
void name_key_tick() {
    if (com::open()) { g_t.reset(); g_del.reset(); return; }   // the Community dialog has its own T (like)
    const uint32_t mask = pad_mask();
    const bool pressed = g_t.press(key_down(kKeyT) || (mask & g_pad_y) != 0);
    const bool dpressed = g_del.press(key_down(kKeyG) || key_down(kKeyR) || (mask & g_pad_x) != 0);
    if (!pressed && !dpressed) return;
    const bool manager = pressed && ui::manager::page_shown();   // (asked only on a T press: one core call)
    switch (lg::route_name_key(pressed, dpressed, ui::dialogs::busy(), g_installed && g_rec.have, manager)) {
        case lg::NameKey::RenameLoadout:         // T on the Manager page: the selected loadout renamed (P5)
            cmn::journal("ui: T on the Manager page: rename");
            ui::manager::rename_key();
            break;
        case lg::NameKey::NameSlot: {
            const int n = slot_under_cursor();
            if (!n) return;
            ui::dialogs::text_input("slot " + std::to_string(n), display_name(n), kNameMax, [n](bool ok, const std::string& text) {
                if (!ok) return;
                std::lock_guard lk(ui::lock());
                set_slot_name(n, text);
            });
            break;
        }
        case lg::NameKey::DeleteSlotName: {      // G / R / pad X on a Record slot row: the name only (the recording stays)
            const int n = slot_under_cursor();
            if (n && !display_name(n).empty()) { set_slot_name(n, ""); cmn::journal("ui: slot %d name deleted", n); }
            break;
        }
        case lg::NameKey::None: break;
    }
}

// ---- hooks ------------------------------------------------------------------------------------------------------------
// Each hook: an outer function (closing / active checks, SEH) and an inner body (lock, C++ objects, try / catch).
// Pre -> post state per call on a per-thread stack (R29, ui/hookstack.hpp): every pre pushes, every post pops.

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
#define UI_PRE(name, body_fn)                                                                    \
    int name(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {          \
        if (cmn::closing() || !ui::active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;             \
        int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;                                                \
        __try { rc = body_fn(argv); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault(#name " (SEH)"); } \
        return rc;                                                                              \
    }

int i32arg(void* v) { return (int)(int32_t)(uint32_t)(uintptr_t)v; }

// -- UpdateSecondaryList post: the page's row list address (RMT:701-715)
struct SecCall { MO* param = nullptr; };
thread_local HookStack<SecCall> s_sec;
int sec_pre_body(void** argv, SecCall* s) { s->param = static_cast<MO*>(argv[1]); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void sec_post_body(void**, const SecCall* c) {
    view_changed();
    if (!c->param || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try {
        if (g_installed) {
            const uintptr_t sec = (uintptr_t)gx::ref(c->param, "_SecondaryList");
            if (sec != g_sec) session_reset_soft();   // a new row list: its rows looked for again (session_reset_soft)
            g_sec = sec;
        }
    } catch (...) { ui::fault("UpdateSecondaryList post"); }
}
UI_PRE_S(sec_pre, s_sec, sec_pre_body)
UI_POST_S(sec_post, s_sec, sec_post_body)

// -- UpdateRecordSlotItem pre / post (RMT:1338-1362)
struct RowCall { MO* group = nullptr; int n = 0; bool focused = false; };
thread_local HookStack<RowCall> s_rec_row, s_play_row;
int rec_item_pre_body(void** argv, RowCall* s) {
    std::lock_guard lk(ui::lock());
    try {
        if (!g_installed || !g_rec.have) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        MO* param = static_cast<MO*>(argv[1]);
        int sel = -1;
        if (page_and_cursor(param, &sel) != kRecord) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const int idx = i32arg(argv[3]);
        if (idx >= lg::kFirstRow && idx <= lg::kLastRow)
            *s = RowCall{ static_cast<MO*>(argv[2]), g_win + idx - lg::kFirstRow + 1, sel == idx };
    } catch (...) { ui::fault("UpdateRecordSlotItem pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void rec_item_post_body(void**, const RowCall* c) {
    if (!c->group || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    g_text_changed = false;
    try { slot_row_texts(c->group, c->n, c->focused); } catch (...) { ui::fault("UpdateRecordSlotItem post"); }
    // the re-fit only when a text of the row changed (or was styled): same texts = same rendered sizes = same fit
    if (g_text_changed) g_fit_watch = g_fit_watch > kRefitAfterRowUpdate ? g_fit_watch : kRefitAfterRowUpdate;
}
UI_PRE_S(rec_item_pre, s_rec_row, rec_item_pre_body)
UI_POST_S(rec_item_post, s_rec_row, rec_item_post_body)

// -- UpdatePlaySlotItem pre / post (RMT:1279-1298)
int play_item_pre_body(void** argv, RowCall* s) {
    std::lock_guard lk(ui::lock());
    try {
        if (!g_installed || !g_rep.have) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        MO* param = static_cast<MO*>(argv[1]);
        int sel = -1;
        if (page_and_cursor(param, &sel) != kReplay) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const int idx = i32arg(argv[3]);
        if (idx >= lg::kFirstRow && idx <= lg::kLastRow)
            *s = RowCall{ static_cast<MO*>(argv[2]), g_rwin + idx - lg::kFirstRow + 1, sel == idx };
    } catch (...) { ui::fault("UpdatePlaySlotItem pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void play_item_post_body(void**, const RowCall* c) {
    if (!c->group || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try { play_row_texts(c->group, c->n, c->focused); } catch (...) { ui::fault("UpdatePlaySlotItem post"); }
}
UI_PRE_S(play_item_pre, s_play_row, play_item_pre_body)
UI_POST_S(play_item_post, s_play_row, play_item_post_body)

// -- set_SelectChildIndex post: the 8 rows follow the cursor (texts, tint), RMT:1774-1806
struct SelCall { MO* param = nullptr; int nw = -1; };
thread_local HookStack<SelCall> s_sel;
int select_pre_body(void** argv, SelCall* s) {
    view_changed();
    if (!g_installed) return REFRAMEWORK_HOOK_CALL_ORIGINAL;  // (read without the lock: a stale answer only skips a redraw)
    *s = SelCall{ static_cast<MO*>(argv[1]), i32arg(argv[2]) };
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void select_post_body(void**, const SelCall* c) {
    view_changed();
    if (!c->param || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try {
        if (!g_installed || !(g_rec.have || g_rep.have)) return;
        MO* list = gx::ref(c->param, "_ViewDataList");
        const int cnt = gm::list_count(list);
        if (cnt <= lg::kFirstRow) return;
        MO* d = gx::ref(gm::list_item(list, lg::kFirstRow), "Data");
        MO* groups = groups_of(c->param);
        const int gc = gm::list_count(groups);   // (the row list is not changed by the passes below: counted once)
        auto group_at = [&](int idx) { return (groups && idx >= 0 && idx < gc) ? gm::list_item(groups, idx) : nullptr; };
        // a row drawn in this frame for the same slot, focus and window is not drawn again (drawn_now); a Replay row keeps
        // its disabled look checked (the cursor's states belong to the game, ours is DISABLE: pose_disabled)
        if (d && g_rep.have && d == g_rep.rows[0]) {   // the Replay page
            for (int idx = lg::kFirstRow; idx <= lg::kLastRow; ++idx) {
                MO* g = group_at(idx);
                if (!g || idx >= cnt) continue;
                const int n = g_rwin + idx - lg::kFirstRow + 1;
                if (!drawn_now(g, true, n, c->nw == idx, g_rwin)) { play_row_texts(g, n, c->nw == idx); continue; }
                ++g_pass_skips;
                MO* ctrl = g_big ? control_of(g) : nullptr;
                if (MO* item = ctrl ? kid(ctrl, "p_GroupItem_5") : nullptr) pose_disabled(ctrl, item, row_disabled(n));
            }
            return;
        }
        if (!(g_rec.have && d && d == g_rec.rows[0])) return;   // the Record page is not shown
        // all 8 rows: the game's own updater batch runs BEFORE the index changes (traced 08/09)
        for (int idx = lg::kFirstRow; idx <= lg::kLastRow; ++idx) {
            MO* g = group_at(idx);
            if (!g) continue;
            const int n = g_win + idx - lg::kFirstRow + 1;
            if (drawn_now(g, false, n, c->nw == idx, g_win)) { ++g_pass_skips; continue; }
            slot_row_texts(g, n, c->nw == idx);
        }
    } catch (...) { ui::fault("set_SelectChildIndex post"); }
}
UI_PRE_S(select_pre, s_sel, select_pre_body)
UI_POST_S(select_post, s_sel, select_post_body)

// -- UIPartsListBase.GetNextIndex / GetPrevIndex: the cursor at the edges (RMT:1810-1881, RSL:546-573). One stack for the
//    two methods (they share the post)
struct Cursor {
    bool on = false; int cur = 0; int dir = 0; bool replay = false; bool outside = false; MO* param = nullptr;
    reversal_ui::Claim rev;
};
thread_local HookStack<Cursor> s_cur;
int cursor_pre_common(void** argv, int dir, Cursor* s) {
    std::lock_guard lk(ui::lock());
    try {
        const int cur = i32arg(argv[2]);
        if (g_installed && g_sec && (uintptr_t)argv[1] == g_sec) {
            MO* param = param_now();             // the first argument must be the flow's cursor row (checked)
            if (param && select_child(param) == cur) {
                const Page pg = page_shown(param);
                if ((pg == kReplay || pg == kRecord) && (cur < lg::kFirstRow || cur > lg::kLastRow)) {   // around the block
                    if (cur <= lg::kLastRow + 3 && cur >= lg::kFirstRow - 2) *s = Cursor{ true, cur, dir, pg == kReplay, true, param, {} };
                } else if (pg != kNone) {
                    // the row must be one of the game's slot rows: the row list is one object for every page of the menu
                    MO* d = gx::call_obj(param, "get_CurrentData");
                    MO* mine = (pg == kRecord ? g_rec : g_rep).rows[cur - lg::kFirstRow];
                    if (d && d == mine) *s = Cursor{ true, cur, dir, pg == kReplay, false, param, {} };
                }
            }
        }
        if (!s->on) s->rev = reversal_ui::cursor_claim(argv[1], cur, dir);   // ReversalSlots' hook on the same methods
    } catch (...) { *s = Cursor{}; ui::fault("GetNext/PrevIndex pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int next_pre_body(void** argv, Cursor* s) { return cursor_pre_common(argv, 1, s); }
int prev_pre_body(void** argv, Cursor* s) { return cursor_pre_common(argv, -1, s); }
void cursor_post_body(void** ret, const Cursor* cp) {
    const Cursor& c = *cp;
    if (c.on || c.rev.on) view_changed();
    if (!(c.on || c.rev.on) || !ui::active() || !ret) return;
    std::lock_guard lk(ui::lock());
    try {
        const int nxt = i32arg(*ret);
        if (c.rev.on) {
            int r = 0;
            if (reversal_ui::cursor_answer(c.rev, nxt, &r)) *ret = (void*)(uintptr_t)(uint32_t)r;
            return;
        }
        lg::CursorAnswer a;
        if (c.replay) {
            a = lg::replay_cursor(c.cur, nxt, c.dir, g_rwin, c.outside, [](int n) { return row_disabled(n); });
            if (a.move) apply_rwindow(c.param, a.win);
            cmn::journal("ui: replay cursor %d -> %d, %s (win %d)", c.cur, nxt, a.replace ? std::to_string(a.index).c_str() : "game's", g_rwin);
        } else {
            a = lg::record_cursor(c.cur, nxt, c.dir, g_win, c.outside);
            if (a.move) apply_shift(c.param, a.win - g_win);
            if (a.move || a.replace) cmn::journal("ui: cursor %d -> %d (win %d)", c.cur, nxt, g_win);
        }
        if (a.replace) *ret = (void*)(uintptr_t)(uint32_t)a.index;
    } catch (...) { ui::fault("GetNext/PrevIndex post"); }
}
UI_PRE_S(next_pre, s_cur, next_pre_body)
UI_PRE_S(prev_pre, s_cur, prev_pre_body)
UI_POST_S(cursor_post, s_cur, cursor_post_body)

// -- UIPartsGroupScroll.UpdateScroll: the redraw after a rename / import, the name re-fit (RMT:1511-1552)
struct ScrollCall { bool ours = false; };
thread_local HookStack<ScrollCall> s_scroll;
int scroll_pre_body(void** argv, ScrollCall* s) {
    s->ours = g_installed && g_sec && (uintptr_t)argv[1] == g_sec;   // every scroll group, every frame: a compare
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void scroll_post_body(void**, const ScrollCall* c) {
    const bool ours = c->ours;
    if (!ui::active()) return;
    if (!ours && g_fit_watch <= 0) return;       // back at once unless our list or a re-fit is pending (ours: a compare)
    std::lock_guard lk(ui::lock());
    try {
        if (ours && g_rows_request.exchange(false)) g_rows_dirty = true;
        if (ours && g_rows_dirty) {
            g_rows_dirty = false;
            redraw_rows();
            if (g_fit_watch < kRefitAfterRedraw) g_fit_watch = kRefitAfterRedraw;   // rendered sizes lag a frame
        }
        // one re-fit a frame (every scroll group of the game calls UpdateScroll: the sizes lag a frame, not a call)
        if (g_fit_watch > 0 && g_refit_tick != ui::tick()) { g_refit_tick = ui::tick(); --g_fit_watch; refit_rows(); }
    } catch (...) { ui::fault("UpdateScroll post"); }
}
UI_PRE_S(scroll_pre, s_scroll, scroll_pre_body)
UI_POST_S(scroll_post, s_scroll, scroll_post_body)

// -- TrainingMenuFunc.Function: windows reset on the page builds, F on a Record row (RMT:1950-2017)
struct FnCall { MO* rec_param = nullptr; MO* restore_rec = nullptr; int rec_win = 0; MO* restore_rep = nullptr; int rep_win = 0; };
thread_local HookStack<FnCall> s_fn;
int function_pre_body(void** argv, FnCall* s) {
    view_changed();
    std::lock_guard lk(ui::lock());
    try {
        const int ft = i32arg(argv[2]);
        MO* param = static_cast<MO*>(argv[3]);
        if (ft == kFtRecord || ft == kFtReplay) {
            ui::manager::game_page_built(ft == kFtReplay);   // the sub-page to reopen on (RMT:2014, P5's memory)
            session_reset_soft();                // a page build: its rows looked for again (session_reset_soft)
            const int save_win = g_win, save_rwin = g_rwin;
            set_window(0);
            set_rwindow(0);
            if (ft == kFtRecord) cmn::journal("ui: record page build: window %d, pending %d", save_win, g_pending_win ? *g_pending_win : -1);
            if (ft == kFtRecord && g_pending_win) s->rec_param = param;
            else if (ft == kFtRecord && save_win > 0) { s->restore_rec = param; s->rec_win = save_win; }
            if (ft == kFtReplay && save_rwin > 0) { s->restore_rep = param; s->rep_win = save_rwin; }
        }
        if (ft == kFtRecordSlot && g_rec.have) {  // the game's F on a slot row: virtual when the row shows slot > 8
            MO* d = gx::ref(static_cast<MO*>(argv[4]), "Data");
            for (int k = 0; k < 8 && d; ++k) {
                if (d != g_rec.rows[k]) continue;
                const int n = g_win + k + 1;
                const mech::record::View cur = mech::record::view();
                if (cur.armed && !cur.seen_now) {   // an earlier virtual recording never started: re-targeted
                    if (n > 8) { mech::record::retarget(n); g_pending_win = g_win; cmn::journal("ui: pending recording re-targeted to slot %d", n); }
                    else { mech::record::drop(); g_pending_win.reset(); cmn::journal("ui: pending virtual recording dropped (live slot %d)", n); }
                } else if (n > 8) {
                    if (cur.armed) mech::record::finalize(true);
                    g_pending_win = g_win;       // at the reopen: the same window (the recorded slot back on its row)
                    cmn::journal("ui: F on slot %d: window %d kept for the reopen", n, g_win);
                    mech::record::arm(n);
                }
            }
        }
    } catch (...) { ui::fault("Function pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void function_post_body(void**, const FnCall* cp) {
    const FnCall& c = *cp;
    if (!(c.rec_param || c.restore_rec || c.restore_rep) || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try {
        if (c.rec_param && g_pending_win) {      // Record page rebuilt after a virtual recording: the window back
            const int w = *g_pending_win;
            if (!mech::record::view().armed) g_pending_win.reset();   // kept while the recording is still pending
            apply_shift(c.rec_param, w - g_win);
            cmn::journal("ui: reopen after recording: window %d, cursor row %d", w, select_child(c.rec_param));
        }
        if (c.restore_rec) { apply_shift(c.restore_rec, c.rec_win); cmn::journal("ui: record window restored to %d", g_win); }
        if (c.restore_rep) { apply_rwindow(c.restore_rep, c.rep_win); cmn::journal("ui: replay window restored to %d", g_rwin); }
    } catch (...) { ui::fault("Function post"); }
}
UI_PRE_S(function_pre, s_fn, function_pre_body)
UI_POST_S(function_post, s_fn, function_post_body)

// -- Param.CreateViewData pre: the menu-open edge, the text dialog's rebuild (RMT:2035-2066)
int cvd_pre_body(void** argv) {
    mech::menu_event();                          // the pause menu is being built: mech polls the pause (the close edge)
    view_changed();
    std::lock_guard lk(ui::lock());
    session_reset();                             // the menu's GUI is (re)built: its rows looked for again
    try {
        mech::record::menu_open();               // menu opened while recording: stored + slot 8 restored BEFORE the rows
        if (g_installed && !g_menu_open) { g_menu_open = true; g_paused_seen = false; on_menu_open(); }   // 30 slots BEFORE the rows
        MO* td = gx::ref(tm(), "_tData");
        const int64_t sel = i64f(td, "SelectIndex", -1);
        const int rec_idx = recording_tab_index();
        const bool naming = dlg::naming(), modal = dlg::modal();
        cmn::journal("ui: menu open: record window %d, pending %d, tab %lld (recording %d)%s", g_win, g_pending_win ? *g_pending_win : -1,
                     (long long)sel, rec_idx, naming ? " [naming]" : modal ? " [dialog]" : "");
        // L1+R1 (A+E) under one of CRSM's dialogs: the tab already moved in the data; it and the cursor row are put back
        // before the rebuild reads them (the text dialog: the Recording tab when nothing was kept yet)
        if (dlg::hold_tab(td, lg::TabAt::Rebuild, naming ? rec_idx : -1)) {
            cmn::journal("ui: rebuild under the dialog: param child %d", select_child(static_cast<MO*>(argv[1])));
        } else {                                 // (under the dialog the slot windows stay where they are)
            if (g_win != 0 && !g_pending_win) set_window(0);
            set_rwindow(0);
        }
        ui::manager::before_create_view_data();  // last, as RMT:2061-2065: RecordType = the sub-page to reopen on (P5)
    } catch (...) { ui::fault("CreateViewData pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE(cvd_pre, cvd_pre_body)

// -- Param.InitTabItem() pre: the tab items set up from the data after a rebuild; under CRSM's dialogs the data's tab is
//    checked once more. RMT:2067-2075 hooked "InitTabItem(System.Int32)": the method has no parameter (the type database
//    of the build of 13/10, read offline; "not found" in every run since 10/10, before that update too), so neither the
//    Lua's hook nor the DLL's was ever installed
int inittab_pre_body(void**) {
    view_changed();
    if (!dlg::modal()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try { dlg::hold_tab(gx::ref(tm(), "_tData"), lg::TabAt::Items, dlg::naming() ? recording_tab_index() : -1); }
    catch (...) { ui::fault("InitTabItem pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
UI_PRE(inittab_pre, inittab_pre_body)

// -- tf_RecordSetting.FuncData.RemoveRecord: Delete on a Record row (RMT:1587-1612)
struct RemoveCall { int clear = 0; };
thread_local HookStack<RemoveCall> s_remove;
int remove_pre_body(void** argv, RemoveCall* s) {
    std::lock_guard lk(ui::lock());
    try {
        if (!(g_installed && g_rec.have)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const int slot = i32arg(argv[3]);
        MO* param = param_now();
        if (page_shown(param) != kRecord) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const int cur = select_child(param);
        if (cur < lg::kFirstRow || cur > lg::kLastRow) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        MO* d = gx::call_obj(param, "get_CurrentData");
        if (!(d && d == g_rec.rows[cur - lg::kFirstRow])) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const int n = g_win + cur - lg::kFirstRow + 1;
        if (n > 8) {                             // virtual: ours to clear, the game's slot 8 untouched
            { std::lock_guard lk2(mech::lock()); files().vslot_delete(n); }
            mech::replay::forget(n);             // the replay layer's entry and slot 8
            g_rows_dirty = true;
            g_epoch.fetch_add(1);
            cmn::journal("ui: virtual slot %d deleted", n);
            return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
        }
        if (slot == n - 1) s->clear = n;
    } catch (...) { ui::fault("RemoveRecord pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void remove_post_body(void**, const RemoveCall* c) {
    const int n = c->clear;
    if (!n || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try {
        { std::lock_guard lk2(mech::lock()); files().set_slot_name(n - 1, model::files::json("")); }
        g_rows_dirty = true;
        g_epoch.fetch_add(1);
        cmn::journal("ui: slot %d deleted, name cleared", n);
    } catch (...) { ui::fault("RemoveRecord post"); }
}
UI_PRE_S(remove_pre, s_remove, remove_pre_body)
UI_POST_S(remove_post, s_remove, remove_post_body)

}  // namespace

// ---- shared helpers -----------------------------------------------------------------------------------------------------
void tint(MO* t, bool focused) { tint_text(t, focused); }
void style_text(MO* t, MO* ref) {                // RMT:2193-2197
    if (!t || !ref) return;
    gx::copy_style(t, ref);
    gx::invoke(t, "set_DisableMessageTag(System.Boolean)", { gx::B(true) });
    int32_t prio = 0;
    gx::call_i32(ref, "get_Priority", &prio);
    gx::invoke(t, "set_Priority(System.Int32)", { gx::I(prio + 2) });
}
std::string slot_display_name(int n) { return display_name(n); }
std::optional<std::string> live_frames_text(int n) { return live_frames(n); }
std::string virtual_frames_text(int n) { return virtual_text(n); }

void specs(std::vector<mech::hooks::Spec>& out) {
    const char* P = "app.training.UIFlowTrainingMenu.Param";
    ui::spec(out, P, "UpdateSecondaryList", sec_pre, sec_post, "the slot windows do not slide (no row list address)", &ok_seclist);
    ui::spec(out, P, "UpdateRecordSlotItem(app.UIPartsGroup, System.Int32)", rec_item_pre, rec_item_post,
             "Record rows 9-30 show the game's slot 8 texts, no names, no arrows", &ok_rec_item);
    ui::spec(out, P, "UpdatePlaySlotItem(app.UIPartsGroup, System.Int32)", play_item_pre, play_item_post,
             "Replay rows without names / arrows / disabled look for 9-30", &ok_play_item);
    ui::spec(out, P, "set_SelectChildIndex(System.Int32)", select_pre, select_post, "the slot rows' tint lags the cursor", &ok_select);
    ui::spec(out, "app.UIPartsListBase", "GetNextIndex", next_pre, cursor_post, "the windows do not slide down", &ok_next);
    ui::spec(out, "app.UIPartsListBase", "GetPrevIndex", prev_pre, cursor_post, "the windows do not slide up", &ok_prev);
    ui::spec(out, "app.UIPartsGroupScroll", "UpdateScroll", scroll_pre, scroll_post, "no redraw after a rename, no name re-fit", &ok_scroll);
    ui::spec(out, "app.training.TrainingMenuFunc", "Function", function_pre, function_post,
             "F on Record rows 9-30 records slot 8 only; the windows are not kept across sub-pages; the menu may reopen on Record "
             "after Replay", &ok_function);
    ui::spec(out, P, "CreateViewData", cvd_pre, nullptr,
             "no menu-open edge: the Replay page keeps 8 slots, slot 8 not restored at the open; the menu reopens on the game's own "
             "sub-page pick", &ok_cvd);
    ui::spec(out, P, "InitTabItem()", inittab_pre, nullptr, "the tab under CRSM's dialogs is checked at the rebuild and the frame only", &ok_inittab);
    ui::spec(out, "app.training.tf_RecordSetting.FuncData", "RemoveRecord", remove_pre, remove_post,
             "Delete on a Record row 9-30 deletes live slot 8; a deleted slot keeps its name", &ok_remove);
}

void frame(bool is_training, bool is_paused) {   // RMT:2090-2123 (the UI part)
    if (!is_training) { g_installed = false; return; }
    if (is_paused) {
        if (g_menu_open) { g_paused_seen = true; name_key_tick(); }
        return;
    }
    if (g_menu_open && g_paused_seen) { g_menu_open = false; on_menu_close(); }   // the game's 8 slots back, flags saved
    // install / re-install (rule 10: menu closed, read now when nothing polled the pause this frame)
    if (const uint64_t ev = mech::events(); ev != g_install_events) { g_install_events = ev; g_install_tries = kInstallTries; g_install_seen = 0; }
    if (g_install_tries > 0 && !g_menu_open && tick() % kInstallEvery == kInstallPhase && !mech::pause_now().up()) {
        --g_install_tries;
        install_check();
        if (g_installed && ++g_install_seen >= kInstallSeen) g_install_tries = 0;
    }
    // the first opening's lookups asked beforehand, within 0.5 ms a frame, until done (once per process, gx::warm_step)
    if (!g_warm_done && !g_menu_open) { PERF_SUB(pw, "UI:warm_up"); g_warm_done = gx::warm_step(kWarmBudgetMs); }
}

bool needs_pause() { return g_menu_open; }

void hand_back() {                               // the UI stopped (faults): the game's own rows back, the menu edges closed
    try {
        set_window(0);
        set_rwindow(0);
        if (g_menu_open) { g_menu_open = false; on_menu_close(); }
    } catch (...) {}
    session_reset();
    g_installed = false;
}

std::string status() {
    char b[800];
    std::snprintf(b, sizeof(b), "pages %s, window %d / replay %d, shifts %llu / %llu, borrows %llu (failed %llu, at a known address %llu), redraws %llu, element walks %llu (kept %llu, %llu sessions, %llu kept in their frame), row passes skipped %llu, pad lists %llu, %s, names %s, menu %s; hooks fn %d cvd %d next %d prev %d rec %d play %d list %d scroll %d sel %d del %d tab %d",
                  g_installed ? "captured" : "-", g_win, g_rwin, (unsigned long long)g_shifts, (unsigned long long)g_rshifts,
                  (unsigned long long)g_borrows, (unsigned long long)g_borrow_fail, (unsigned long long)g_borrow_reused,
                  (unsigned long long)g_redraws, (unsigned long long)g_kid_walks, (unsigned long long)g_kid_hits,
                  (unsigned long long)g_sessions, (unsigned long long)g_soft_kept, (unsigned long long)g_pass_skips,
                  (unsigned long long)g_pad_lists, gx::warm_status().c_str(),
                  g_native_names ? "native" : "-", g_menu_open ? "open" : "closed", ok_function, ok_cvd, ok_next, ok_prev,
                  ok_rec_item, ok_play_item, ok_seclist, ok_scroll, ok_select, ok_remove, ok_inittab);
    return b;
}

}  // namespace ui::windows

namespace ui {
void rows_changed() { windows::g_epoch.fetch_add(1); windows::g_rows_request.store(true); }
}
