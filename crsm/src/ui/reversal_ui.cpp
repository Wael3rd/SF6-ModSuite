// ui/reversal_ui — the Reversal pages over 30 rows and the action picker's Recording tab of 30 (phase P4, owner "ui"),
// ReversalSlots.lua (RSL) in C++. The rows' data (rows 11-30, the virtual-slot map, the trigger) are the reversal layer's
// (mech/layer_reversal.cpp); here: the page window (the 10 game rows' _SlotID, the cursor slide), the ▲ / ▼ marks, the row
// text of a recording ("Combo A (78F)" / "Slot 15 (78F)"), and the picker's 30 entries (names, frames, the scroll bar +40).
//
// The Lua's live tuning (CFG.live / anim / mirror, RSL:131) is off and its values are frozen: the constants below. Its
// per-frame mirror_anim was called with an undefined `q` (RSL:691) and never ran: dropped (no visible change).
#include <windows.h>

#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

#include "common.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "hookstack.hpp"
#include "internal.hpp"
#include "logic.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/labels.hpp"

namespace ui::reversal_ui {
namespace {

namespace gm = game::gm;
namespace lg = ui::logic;
using gx::MO;

constexpr int kRecording = 4;                   // app.training.ReversalType.RECORDING (RSL:32)
constexpr int kWork = 7;                        // live slot 8: what the game's data holds for a virtual reversal (RSL:33)
constexpr int kReversalTab = 5;                 // the Reversal Settings primary tab's _FuncType (RSL:493)
constexpr int kRowType = 8, kRowFunc = 137;     // a reversal row (RSL:533)
const char* const kRtFields[3] = { "DownReversalDatas", "GuardReversalDatas", "DamageReversalDatas" };   // RSL:35
// _G.__rev_cfg, frozen (RSL:131-133, Wael 08/09)
constexpr float kNameX = 0, kRegion = 1400, kNameY = 0;
constexpr float kSlotDx = -490, kSlotDy = 0, kCenterDx = 445, kCenterDy = 0;
constexpr float kRowRegion = 1000, kRowFont = 0, kRowX = 0, kArrowX = 980, kArrowY = 0;
constexpr float kBarExtra = 40;                 // BAR_EXTRA (RSL:123)
const char* const kDonors[] = { "p_InputGuide_0_s_StickLL", "p_InputGuide_0_s_StickRR", "p_InputGuide_0_s_DirL", "p_InputGuide_0_s_DirR" };

bool g_installed = false;                       // rev_native.installed: the reversal layer's 30-row arrays are in place
MO* g_pick_param = nullptr;                     // the picker's Recording tab while it is up (pick.param / pick.list)
MO* g_pick_list = nullptr;
std::optional<float> g_bar_base;
std::set<uintptr_t> g_styled, g_arrow_styled;
struct Orig { float rw = 0, rh = 0, fw = 0, fh = 0, x = 0, y = 0, z = 0; };
std::map<uintptr_t, Orig> g_row_orig;
std::optional<std::pair<float, float>> g_name_base;
std::string g_prefix = "Slot ", g_suffix;       // the game's "Slot N" wording (RSL:106)
uint64_t g_shifts = 0, g_picker_opens = 0, g_bar_fixes = 0;
bool ok_item = false, ok_init = false, ok_update = false, ok_start = false, ok_list[5]{};
// the picker seen from the frame (route 2A): UIPartsScrollList.set_SelectedIndex and the picker's HidObject are not
// hooked (R18: set_SelectedIndex shares its code with UIPartsScrollGrid's, HidObject with 5 other flows' HidObject)
gm::TD* g_pick_td = nullptr;                    // the picker's param type (proof of life when the flows do not list it)
bool g_pick_in_flows = false;                   // its param was seen in UIFlowManager._Handles
int32_t g_seen_scroll = INT_MIN, g_seen_sel = INT_MIN;   // the scroll / selected index the items were last written for
uint64_t g_frame_rewrites = 0, g_frame_rewrites_open = 0, g_picker_closes = 0;

int i32arg(void* v) { return (int)(int32_t)(uint32_t)(uintptr_t)v; }
int64_t i64f(MO* o, const char* f, int64_t d = 0) { int64_t v = d; if (!gx::get_i64(o, f, &v)) v = d; return v; }
MO* tm() { return gx::singleton("app.training.TrainingManager"); }
MO* reversal_setting() { return gx::ref(gx::ref(tm(), "_tData"), "ReversalSetting"); }
std::optional<int64_t> current_rtype() {
    int64_t v = 0;
    if (!gx::get_i64(reversal_setting(), "ReversalType", &v)) return std::nullopt;
    return v;
}
MO* row_data(int64_t rt, int64_t row) {          // RSL:73-80: the game's ReversalData of (rtype, row) for the dummy
    if (rt < 0 || rt > 2) return nullptr;
    MO* rs = reversal_setting();
    int fid = -1;
    { std::lock_guard lk(mech::lock()); fid = mech::files::fighter(); }
    int32_t fi = -1;
    if (!rs || fid < 0 || !gm::call_i32(rs, "GetFighterIndex", &fi, { gm::arg_i32(fid) }) || fi < 0) return nullptr;
    MO* fd = gm::list_item(gx::ref(rs, "FighterDataList"), fi);
    return gm::array_get(gx::ref(fd, kRtFields[rt]), (int32_t)row);
}
std::optional<int> is_virtual_row(int64_t rt, int64_t row) {   // RSL:81-86: mapped AND the game's data says "slot 8 recording"
    const auto n = mech::reversal::map_get(rt, row);
    if (!n) return std::nullopt;
    MO* d = row_data(rt, row);
    if (d && i64f(d, "Type", -1) == kRecording && i64f(d, "SkillIndex", -1) == kWork) return (int)*n;
    return std::nullopt;
}
std::string frames_text(int n) {                 // RSL:116-119
    if (n <= 8) { const auto t = windows::live_frames_text(n); return t ? *t : std::string(); }
    return windows::virtual_frames_text(n);
}

// ---- the page window (RSL:488-545) ------------------------------------------------------------------------------------
void reset_page_windows() {                      // the pages' rows back to _SlotID = their index (menu open)
    MO* menu = gx::call_obj(gx::ref(tm(), "_UIData"), "get_MenuData");
    const int32_t n = gm::array_len(menu);
    for (int32_t i = 0; i < n; ++i) {
        MO* t = gm::array_get(menu, i);
        if (!t || i64f(t, "_FuncType", -1) != kReversalTab) continue;
        MO* pages = gx::ref(gm::array_get(gx::ref(t, "_ChildData"), 0), "_ChildData");
        const int32_t np = gm::array_len(pages);
        for (int32_t k = 0; k < np; ++k) {
            MO* rows = gx::ref(gm::array_get(pages, k), "_ChildData");
            const int32_t nr = gm::array_len(rows);
            for (int32_t j = 0; j < nr; ++j) gx::set_field_i64(gm::array_get(rows, j), "_SlotID", j);
        }
    }
}

bool rev_page(MO* param, MO** list_out, int* win) {   // the reversal page shown? -> its row list, window
    MO* list = gx::ref(param, "_ViewDataList");
    if (!list || gm::list_count(list) < 1 + lg::kRevPage) return false;
    MO* d = gx::ref(gm::list_item(list, 1), "Data");
    if (!(d && i64f(d, "_Type", -1) == kRowType && i64f(d, "_FuncType", -1) == kRowFunc)) return false;
    *list_out = list;
    *win = (int)i64f(d, "_SlotID", 0);
    return true;
}

void rev_shift(MO* param, MO* list, int nw) {    // RSL:536-545 (the caller checked nw != win)
    MO* groups = gx::ref(gx::ref(param, "_SecondaryList"), "_Children");
    for (int idx = 1; idx <= lg::kRevPage; ++idx)
        if (MO* d = gx::ref(gm::list_item(list, idx), "Data")) gx::set_field_i64(d, "_SlotID", nw + idx - 1);
    for (int idx = 1; idx <= lg::kRevPage; ++idx)
        if (MO* g = gm::list_item(groups, idx)) gx::invoke(param, "UpdateReversalItem(app.UIPartsGroup, System.Int32)", { gx::P(g), gx::I(idx) });
    ++g_shifts;
    cmn::journal("ui: reversal rows %d-%d", nw + 1, nw + lg::kRevPage);
}

// ---- the marks and the row text (RSL:320-418) ---------------------------------------------------------------------------
MO* arrow_text(MO* ctrl, MO* item) {             // under p_GroupItem_8: found again after a reload, or taken from the strip
    for (MO* c = gx::first_child(item); c; c = gx::next(c)) if (gx::name(c) == "e_text_0") return c;
    MO* strip = gx::child(ctrl, "p_TrainingSecondaryTab_19");
    MO* tab = strip ? gx::child(strip, "c_tab") : nullptr;
    if (!tab) return nullptr;
    for (const char* donor : kDonors) {
        MO* g = gx::child(tab, donor);
        MO* t = g ? gx::child(g, "e_text_0") : nullptr;
        if (!t) continue;
        SF6CoreArg ret{};
        bool ok = gx::invoke(item, "addChild(via.gui.PlayObject)", { gx::P(t) }, &ret) && (ret.v.i & 0xFF) != 0;
        if (!ok) {                               // RSL:332 (R22 applies: the Lua's own fallback, kept)
            gx::invoke(t, "remove()");
            ok = gx::invoke(item, "addChild(via.gui.PlayObject)", { gx::P(t) }, &ret) && (ret.v.i & 0xFF) != 0;
        }
        if (ok) { g_arrow_styled.erase((uintptr_t)t); return t; }   // a fresh donor (the menu's GUI is built again at each
    }                                                               // opening, at old addresses): styled again
    return nullptr;
}

void arrow_update(MO* ctrl, int idx, int win) {  // RSL:337-353
    MO* item = ctrl ? gx::child(ctrl, "p_GroupItem_8") : nullptr;
    if (!item) return;
    MO* t = arrow_text(ctrl, item);
    if (!t) return;
    if (g_arrow_styled.insert((uintptr_t)t).second) {
        if (MO* ref = gx::find_text(item, "e_txt_center")) windows::style_text(t, ref);
        gx::set_visible(t, true);
    }
    float rw = 0, rh = 0, p[3]{};
    gx::size_of(t, "get_RegionSize", &rw, &rh);
    const float x = kArrowX - rw / 2;            // the text is centred in its region box: box left = centre - half
    if (gx::vec3_of(t, "get_Position", p) && (std::fabs(p[0] - x) > 0.5f || std::fabs(p[1] - kArrowY) > 0.5f)) gx::set_pos(t, x, kArrowY, p[2]);
    gx::set_text(t, lg::reversal_arrow(idx, win));
}

void row_text_geometry(MO* t) {                  // RSL:356-370: the centre text's region / font / x
    const uintptr_t a = (uintptr_t)t;
    auto it = g_row_orig.find(a);
    if (it == g_row_orig.end()) {
        Orig o;
        float p[3]{};
        if (!gx::size_of(t, "get_RegionSize", &o.rw, &o.rh) || !gx::size_of(t, "get_FontSize", &o.fw, &o.fh) || !gx::vec3_of(t, "get_Position", p)) return;
        o.x = p[0]; o.y = p[1]; o.z = p[2];
        it = g_row_orig.emplace(a, o).first;
    }
    const Orig& o = it->second;
    const float rw = kRowRegion > 0 ? kRowRegion : o.rw;
    float cw = 0, ch = 0;
    if (gx::size_of(t, "get_RegionSize", &cw, &ch) && std::fabs(cw - rw) > 0.5f) gx::set_region(t, rw, ch);
    const float fw = kRowFont > 0 ? kRowFont : o.fw;
    const bool want_fit = kRowFont <= 0;         // the game's auto-fit would undo our size
    bool fitn = false;
    if (gx::call_bool(t, "get_AutoRegionFit", &fitn) && fitn != want_fit) gx::invoke(t, "set_AutoRegionFit(System.Boolean)", { gx::B(want_fit) });
    float f_w = 0, f_h = 0;
    if (gx::size_of(t, "get_FontSize", &f_w, &f_h) && std::fabs(f_w - fw) > 0.5f && o.fw > 0) gx::set_font(t, fw, o.fh * (fw / o.fw));
    const float x = o.x + kRowX;
    float p[3]{};
    if (gx::vec3_of(t, "get_Position", p) && std::fabs(p[0] - x) > 0.5f) gx::set_pos(t, x, p[1], p[2]);
}

// ---- the picker (RSL:121-297) ------------------------------------------------------------------------------------------
void learn_slot_words(const std::string& msg) {  // only the game's own "Slot N" (never our "Slot N  name")
    std::string p;
    if (model::labels::learn_slot_words(msg, &p)) { g_prefix = p; g_suffix.clear(); }
}

void fit_picker(MO* t0, const std::string& name) {   // RSL:136-160: a wide region, the base font (no shrink, Wael 08/09)
    if (!g_name_base) { float w = 0, h = 0; if (!gx::size_of(t0, "get_FontSize", &w, &h)) return; g_name_base = std::make_pair(w, h); }
    float cw = 0, ch = 0;
    gx::size_of(t0, "get_FontSize", &cw, &ch);
    float rw = 0, rh = 0;
    if (gx::size_of(t0, "get_RegionSize", &rw, &rh) && std::fabs(rw - kRegion) > 0.5f) gx::set_region(t0, kRegion, rh);
    gx::set_text(t0, name);
    const float want = g_name_base->first;
    if (std::fabs(cw - want) > 0.5f) gx::set_font(t0, want, g_name_base->second);
}

void shift_game_texts(MO* tn, MO* tc) {          // RSL:161-166: "Slot N" / frames of a picker item as set in the panel
    for (auto [t, dx, dy] : { std::tuple<MO*, float, float>{ tn, kSlotDx, kSlotDy }, std::tuple<MO*, float, float>{ tc, kCenterDx, kCenterDy } }) {
        float p[3]{};
        if (t && gx::vec3_of(t, "get_Position", p) && (std::fabs(p[0] - dx) > 0.5f || std::fabs(p[1] - dy) > 0.5f)) gx::set_pos(t, dx, dy, p[2]);
    }
}

// A picker item's texts, found once per picker (InitScrollList starts one): its two game texts (moved into the input-guide
// panel at its styling: the same objects, found again there before), the panel and our name text. The pooled items keep
// them for the picker's life: each scroll rewrote every item by walking its tree again (find_text, depth 4, 5 times an
// item: 3-3.6 ms an InitScrollList / UpdateScrollList in the perf log of 10/10).
struct PickItem { MO* tn = nullptr; MO* tc = nullptr; MO* g = nullptr; MO* t0 = nullptr; };
std::map<uintptr_t, PickItem> g_pick_items;

void rewrite_items(MO* q) {                      // RSL:167-214: every pooled item = scroll index + pool index
    MO* sl = gx::ref(q, "_pScrollList");
    if (!sl) return;
    int32_t base = 0;
    if (MO* L = gx::ref(sl, "_List")) gx::call_i32(L, "get_ScrollIndex", &base);
    MO* items = gx::call_obj(sl, "get_Items");
    if (!items) return;
    int32_t sel = -1;
    gx::call_i32(sl, "get_SelectedIndex", &sel);
    const int32_t len = gm::array_len(items);
    for (int32_t k = 0; k < len; ++k) {
        MO* it = gm::array_get(items, k);
        const int n = base + k + 1;
        if (!it || n < 1 || n > lg::kSlotTotal) continue;
        const auto known = g_pick_items.find((uintptr_t)it);
        const bool kept = known != g_pick_items.end();
        MO* tn = kept ? known->second.tn : gx::find_text(it, "e_txt_name");
        if (n <= 8 && tn) learn_slot_words(gx::msg(tn));
        if (tn) gx::set_text(tn, g_prefix + std::to_string(n) + g_suffix);
        MO* tc = kept ? known->second.tc : gx::find_text(it, "e_txt_center");
        if (tc) gx::set_text(tc, frames_text(n));
        shift_game_texts(tn, tc);
        // the RSM name on the left: the item's unused input-guide text moved to the left (RSL:182-211)
        MO* g = kept ? known->second.g : gx::child(it, "p_InputGuide_0_s");
        MO* t0 = kept ? known->second.t0 : (g ? gx::child(g, "e_text_0") : nullptr);
        if (!t0) continue;
        if (g_styled.insert((uintptr_t)t0).second) {   // once per session: the centre text's look
            for (MO* t : { tn, tc }) {           // the game's two texts moved into this unanimated panel
                if (t && gx::parent(t) != g) gx::invoke(g, "addChild(via.gui.PlayObject)", { gx::P(t) });
            }
            if (tc) windows::style_text(t0, tc);
            gx::set_pos(g, 0, 0, 0);
            gx::set_visible(t0, true);
        }
        if (!kept) g_pick_items[(uintptr_t)it] = PickItem{ tn, tc, g, t0 };
        fit_picker(t0, windows::slot_display_name(n));
        const float x = kNameX - kRegion / 2;    // the position is the region box's left edge
        float p[3]{}, q0[3]{};
        if (gx::vec3_of(g, "get_Position", p) && (std::fabs(p[0]) > 0.5f || std::fabs(p[1]) > 0.5f)) gx::set_pos(g, 0, 0, 0);
        if (gx::vec3_of(t0, "get_Position", q0) && (std::fabs(q0[0] - x) > 0.5f || std::fabs(q0[1] - kNameY) > 0.5f)) gx::set_pos(t0, x, kNameY, q0[2]);
        windows::tint(t0, n - 1 == sel);         // alphas by state (the static variant)
    }
    if (q == g_pick_param) { g_seen_scroll = base; g_seen_sel = sel; }   // what the frame compares with (route 2A)
}

int current_row() {                              // the reversal row the picker was opened from (RSL:215-218)
    int64_t v = -1;
    MO* d = gx::call_obj(gx::find_param(), "get_CurrentData");
    return (d && gx::get_i64(d, "_SlotID", &v)) ? (int)v : -1;
}

MO* find_scroll_bar(MO* e, int depth) {          // RSL:236-243
    for (MO* c = gx::first_child(e); c; c = gx::next(c)) {
        if (gx::is_type(c, "via.gui.ScrollBar")) return c;
        if (depth < 4) if (MO* b = find_scroll_bar(c, depth + 1)) return b;
    }
    return nullptr;
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

// -- UpdateReversalItem(UIPartsGroup, Int32) post: the marks and the recording row's text (RSL:385-418)
struct ItemCall { MO* group = nullptr; MO* param = nullptr; int idx = -1; };
thread_local HookStack<ItemCall> s_item;
int item_pre_body(void** argv, ItemCall* s) { *s = ItemCall{ static_cast<MO*>(argv[2]), static_cast<MO*>(argv[1]), i32arg(argv[3]) }; return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void item_post_body(void**, const ItemCall* cp) {
    const ItemCall c = *cp;
    if (!c.group || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try {
        MO* list = gx::ref(c.param, "_ViewDataList");
        if (c.idx < 0 || c.idx >= gm::list_count(list)) return;
        MO* d = gx::ref(gm::list_item(list, c.idx), "Data");
        if (!d) return;
        const int64_t row = i64f(d, "_SlotID", 0);
        const auto rt = current_rtype();
        if (!rt) return;
        MO* ctrl = gx::call_obj(c.group, "get_Control");
        {                                        // the window's marks (first / last shown row)
            MO* first = gx::ref(gm::list_item(list, 1), "Data");
            arrow_update(ctrl, c.idx, first ? (int)i64f(first, "_SlotID", 0) : 0);
        }
        auto n = is_virtual_row(*rt, row);
        if (!n) {                                // a live recording row too: its RSM name (was RSM's D2D overlay)
            MO* rd = row_data(*rt, row);
            if (!(rd && i64f(rd, "Type", -1) == kRecording)) return;
            const int64_t k = i64f(rd, "SkillIndex", -1);
            if (k < 0 || k > 7) return;
            n = (int)k + 1;
        }
        MO* holder = ctrl ? gx::child(ctrl, "p_GroupItem_8") : nullptr;
        MO* t = gx::find_text(holder ? holder : ctrl, "e_txt_center");
        if (!t) return;
        // "Name (78F)", or "Slot 15 (78F)" when unnamed (Wael, 08/09)
        gx::set_text(t, model::labels::reversal_row_text(g_prefix, *n, g_suffix, windows::slot_display_name(*n), frames_text(*n)));
        row_text_geometry(t);
    } catch (...) { ui::fault("UpdateReversalItem post"); }
}
UI_PRE_S(item_pre, s_item, item_pre_body)
UI_POST_S(item_post, s_item, item_post_body)

// -- the picker's Recording tab: InitScrollList post (30 entries, the stored virtual slot selected, the bar +40)
struct QCall { MO* q = nullptr; bool skipped = false; };
thread_local HookStack<QCall> s_init, s_update, s_start;
int q_pre_body(void** argv, QCall* s) { s->q = static_cast<MO*>(argv[1]); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void init_post_body(void**, const QCall* c) {
    MO* q = c->q;
    if (!q || !ui::active()) return;
    if (!(ok_init && ok_update)) return;         // the game's writer stops at 8: without our UpdateScrollList, 8 entries
    std::lock_guard lk(ui::lock());
    try {
        MO* sl = gx::ref(q, "_pScrollList");
        g_pick_param = q;
        g_pick_list = sl;
        g_pick_td = q->get_type_definition();
        g_pick_in_flows = false;
        g_seen_scroll = g_seen_sel = INT_MIN;
        g_frame_rewrites_open = 0;
        if (!sl) return;
        g_styled.clear();                        // a new picker: its items (possibly at the old ones' addresses) styled again
        g_pick_items.clear();
        { std::lock_guard lk2(mech::lock()); mech::files::engine().vslots(); }   // vslots_load
        int32_t sel = -1;
        gx::call_i32(sl, "get_SelectedIndex", &sel);
        const auto rt = current_rtype();
        const int row = current_row();
        const auto n = (rt && row >= 0) ? is_virtual_row(*rt, row) : std::nullopt;
        if (n) sel = *n - 1;
        gx::invoke(sl, "InitList(System.Int32, System.Int32, System.Int32)", { gx::I(lg::kSlotTotal), gx::I(sel), gx::I(-1) });   // as the game did with 8
        ++g_picker_opens;
        // the scroll bar's track a little longer (Wael, 08/09)
        MO* ctrl = gx::call_obj(sl, "get_Control");
        MO* root = ctrl ? gx::parent(ctrl) : nullptr;
        MO* bar = root ? find_scroll_bar(root, 0) : nullptr;
        if (bar) {
            float fw = 0, fh = 0;
            if (gx::size_of(bar, "get_BarFullSize", &fw, &fh)) {
                if (!g_bar_base) g_bar_base = fh;
                const float want = *g_bar_base + kBarExtra;
                if (std::fabs(fh - want) > 0.5f) {
                    const float sz[2] = { fw, want };
                    gx::invoke(bar, "set_BarFullSize(via.Size)", { gx::P(sz) });
                    if (MO* base = gx::child(bar, "e_tex_base")) { float zw = 0, zh = 0; if (gx::size_of(base, "get_Size", &zw, &zh)) gx::set_size(base, zw, want); }
                    if (MO* arrow = gx::child(bar, "e_tex_arrow_B")) { float p[3]{}; if (gx::vec3_of(arrow, "get_Position", p)) gx::set_pos(arrow, p[0], p[1] + (want - fh), p[2]); }
                    ++g_bar_fixes;
                    cmn::journal("ui: picker scroll bar track %.0f -> %.0f", fh, want);
                }
            }
        } else {
            cmn::journal("ui: picker scroll bar not found under %s", root ? gx::name(root).c_str() : "-");
        }
        cmn::journal("ui: picker: %d entries, selected %d%s", lg::kSlotTotal, sel, n ? (" (virtual " + std::to_string(*n) + ")").c_str() : "");
    } catch (...) { ui::fault("InitScrollList post"); }
}
UI_PRE_S(init_pre, s_init, q_pre_body)
UI_POST_S(init_post, s_init, init_post_body)

// -- UpdateScrollList: past 8 entries the items are ours (the game's writer stops at RECORD_MAX, RSL:265-277)
int update_pre_body(void** argv, QCall* s) {
    s->q = static_cast<MO*>(argv[1]);
    if (!(ok_init && ok_update)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(ui::lock());
    try {
        MO* sl = gx::ref(s->q, "_pScrollList");
        int32_t max = 0;
        if (sl && gx::call_i32(sl, "get_ItemMax", &max) && max > 8) { s->skipped = true; rewrite_items(s->q); }
    } catch (...) { s->skipped = false; ui::fault("UpdateScrollList pre"); }
    return s->skipped ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void update_post_body(void**, const QCall* c) {
    MO* q = c->q; const bool skipped = c->skipped;
    if (!q || skipped || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try { rewrite_items(q); } catch (...) { ui::fault("UpdateScrollList post"); }   // the names on the game's 8
}
UI_PRE_S(update_pre, s_update, update_pre_body)
UI_POST_S(update_post, s_update, update_post_body)

// -- Start post: the tab shown, a second pass with rendered sizes (RSL:278-283)
void start_post_body(void**, const QCall* c) {
    MO* q = c->q;
    if (!q || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try { rewrite_items(q); } catch (...) { ui::fault("picker Start post"); }
}
UI_PRE_S(start_pre, s_start, q_pre_body)
UI_POST_S(start_post, s_start, start_post_body)

// -- the picker's tab hidden (RSL:284-285): its HidObject is not hooked (R18), the frame sees it (frame, picker_live)

// -- UIPartsScrollList.Next / Prev / OnScrollUp / OnScrollDown / SetSelectedIndex: the pooled items show other entries
//    (RSL:286-296); type-wide, our list only. set_SelectedIndex (R18) is covered by the frame (route 2A).
struct ListCall { bool ours = false; };
thread_local HookStack<ListCall> s_list;      // Next / Prev / ... call SetSelectedIndex inside: one slot per call
int list_pre_body(void** argv, ListCall* s) {
    s->ours = g_pick_list != nullptr && argv[1] == g_pick_list;   // a compare (stale at worst: one extra rewrite)
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void list_post_body(void**, const ListCall* c) {
    const bool ours = c->ours;
    if (!ours || !ui::active()) return;
    std::lock_guard lk(ui::lock());
    try {
        if (g_pick_param && gm::is_managed(g_pick_param)) rewrite_items(g_pick_param);
    } catch (...) { ui::fault("UIPartsScrollList post"); }
}
UI_PRE_S(list_pre, s_list, list_pre_body)
UI_POST_S(list_post, s_list, list_post_body)

}  // namespace

void menu_open() {                               // ext_install with the layer native (RSL:503-509)
    g_installed = mech::reversal::menu_open();
    if (g_installed) reset_page_windows();
}

void menu_close() {                              // ext_uninstall (RSL:521-522)
    g_installed = false;
    mech::reversal::menu_close();
}

bool installed() { return g_installed; }
bool needs_pause() { return g_pick_param != nullptr; }

namespace {
// The picker is still up: its param is listed by UIFlowManager._Handles (as the training menu's, gx::find_param). A param
// never listed there keeps the proof of life of R2 (managed, its type, its own list).
bool picker_live() {
    MO* hs = gm::ref(gm::singleton("app.UIFlowManager"), "_Handles");
    const int32_t n = gm::list_count(hs);
    for (int32_t i = 0; i < n; ++i)
        if (gm::ref(gm::list_item(hs, i), "<Param>k__BackingField") == g_pick_param) { g_pick_in_flows = true; return true; }
    if (g_pick_in_flows) return false;
    return gm::is_managed(g_pick_param) && g_pick_param->get_type_definition() == g_pick_td &&
           gx::ref(g_pick_param, "_pScrollList") == g_pick_list;
}
void picker_closed(const char* why) {
    ++g_picker_closes;
    cmn::journal("ui: picker closed (%s): %llu rewrite(s) from the frame", why, (unsigned long long)g_frame_rewrites_open);
    g_pick_param = nullptr;
    g_pick_items.clear();
    g_pick_list = nullptr;
    g_seen_scroll = g_seen_sel = INT_MIN;
}
}  // namespace

// Route 2A, frame callback (game thread, ui::lock held): while the picker is up, its scroll index and selected index are
// read; a change the hooked paths (Next / Prev / OnScroll* / SetSelectedIndex / UpdateScrollList) did not write the items
// for rewrites them. Its close is seen here too: gone from the flows, or the pause menu closed.
void frame(bool paused) {
    if (!g_pick_param) return;
    if (!paused) { picker_closed("the menu closed"); return; }
    if (!picker_live()) { picker_closed("gone from the flows"); return; }
    MO* sl = g_pick_list;
    if (!sl) return;
    int32_t si = -1, sel = -1;
    if (MO* L = gx::ref(sl, "_List")) gx::call_i32(L, "get_ScrollIndex", &si);
    gx::call_i32(sl, "get_SelectedIndex", &sel);
    if (si == g_seen_scroll && sel == g_seen_sel) return;
    if (g_seen_scroll == INT_MIN) { g_seen_scroll = si; g_seen_sel = sel; return; }   // the opening state (Init / Start write it)
    rewrite_items(g_pick_param);
    g_seen_scroll = si;
    g_seen_sel = sel;
    ++g_frame_rewrites;
    ++g_frame_rewrites_open;
}

Claim cursor_claim(void* list, int cur, int dir) {   // RSL:551-561
    Claim c;
    if (!g_installed) return c;
    MO* param = gx::find_param();
    if (!param || list != gx::ref(param, "_SecondaryList")) return c;
    int32_t sel = -1;
    gx::call_i32(param, "get_SelectChildIndex", &sel);
    if (sel != cur || cur < 0 || cur > lg::kRevPage) return c;   // a slot row, or the strip row above them (0)
    MO* rows = nullptr;
    int win = 0;
    if (!rev_page(param, &rows, &win)) return c;
    c.on = true; c.cur = cur; c.dir = dir; c.win = win; c.param = param;
    return c;
}

bool cursor_answer(const Claim& c, int nxt, int* ret) {   // RSL:562-570
    const lg::CursorAnswer a = lg::reversal_cursor(c.cur, nxt, c.dir, c.win);
    if (!a.move) return false;
    MO* rows = nullptr;
    int win = 0;
    if (!rev_page(c.param, &rows, &win)) return false;
    rev_shift(c.param, rows, a.win);
    *ret = a.index;
    return a.replace;
}

void specs(std::vector<mech::hooks::Spec>& out) {
    const char* R = "app.training.UIFlowTrainingMenu_Reversal_Recording.Param";
    ui::spec(out, "app.training.UIFlowTrainingMenu.Param", "UpdateReversalItem(app.UIPartsGroup, System.Int32)", item_pre, item_post,
             "reversal rows without ▲ / ▼ and recording names", &ok_item);
    ui::spec(out, R, "InitScrollList", init_pre, init_post, "the picker keeps the game's 8 recordings", &ok_init);
    ui::spec(out, R, "UpdateScrollList", update_pre, update_post, "the picker keeps the game's 8 recordings", &ok_update);
    ui::spec(out, R, "Start", start_pre, start_post, "the picker's names fitted at the first scroll only", &ok_start);
    const char* L = "app.UIPartsScrollList";
    const char* names[5] = { "Next", "Prev", "OnScrollUp", "OnScrollDown", "SetSelectedIndex" };
    for (int i = 0; i < 5; ++i)
        ui::spec(out, L, names[i], list_pre, list_post, "the picker's items keep stale texts after a scroll", &ok_list[i]);
}

void hand_back() {
    try { if (g_installed) reset_page_windows(); } catch (...) {}
    if (g_installed) { g_installed = false; if (cmn::owns(cmn::Feature::Reversal)) mech::reversal::menu_close(); }
    g_pick_param = nullptr;
    g_pick_items.clear();
    g_pick_list = nullptr;
}

std::string status() {
    char b[240];
    std::snprintf(b, sizeof(b), "rows %s, shifts %llu, picker opens %llu / closes %llu, frame rewrites %llu, bar fixes %llu; hooks item %d init %d update %d start %d list %d%d%d%d%d",
                  g_installed ? "30" : "10", (unsigned long long)g_shifts, (unsigned long long)g_picker_opens, (unsigned long long)g_picker_closes,
                  (unsigned long long)g_frame_rewrites, (unsigned long long)g_bar_fixes,
                  ok_item, ok_init, ok_update, ok_start, ok_list[0], ok_list[1], ok_list[2], ok_list[3], ok_list[4]);
    return b;
}

}  // namespace ui::reversal_ui
