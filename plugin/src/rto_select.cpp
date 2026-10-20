// SF6 Slots — runtime costume options on the character select (rto.hpp, docs/RUNTIME_OPTIONS.md).
//
// Each side's panel of the character select (agents ui10505 P1 / ui10507 P2, four rows: Costume, Color, Control
// Type, Button Preset) can show the options of the outfit that side wears: R1 on that player's pad, or C on the
// keyboard (settings.json "select_options_button" / "select_options_key"), while the outfit has options (the select model wears one of the manifest's outfits). The four
// rows then show the option rows of the Battle Settings menu (rto_data rows: one choice row per pick group, one On /
// Off row per toggle), Left / Right change the focused one, Up / Down move through them (more rows than four: a window
// wrapping at the ends like the CRSM recording slots, "▲" / "▼" in the left margin of the first / last row while rows
// lie above / below). A change is saved in costume_choices.json for that side (online: P1's, the player's own fighter
// wears P1's options) and shown at once on that side's select model (rto.cpp select_tick). B / Esc or Decide put the
// panel back; R1 / the key do nothing while the options show (and the guide corner no longer offers them).
//
// The arrows: a copy of the row's title (ui_clone_control, core 0.8.2+) named e_text_slots_arrow, added next to the
// title (same parent, font, size and colour), placed left of it near the row's outline; the title is never moved nor
// prefixed. R19: the copy is made inside the select flow's own update (post-hook of app.UIFlowUI10505.CharaSetting
// OnUpdate, the place LL's row clones on this panel use); the frame only asks for it (one per row, it lives and dies
// with the panel's tree, never again after 3 refusals). R35: the copy is not a managed object, found again by name
// under the title's live parent, every write through the core. Hidden (and ForceInvisible) when not wanted. When the
// copy cannot be made (refused, or that update never seen within a second) the arrows go before the titles instead.
//
// The technique is the Color Mod's DI mode on this very panel (colormod/src/screens/dipanel.cpp, dipanel_mode.cpp):
// the panel's own four rows are borrowed (no row is created, nothing added to the pool, R12 / R13): their spins get
// their own range, their titles lose their message link (kept, put back), the colour chips and unused rows are hidden;
// hooks on the spin, the panel's Param, the agent and the group turn the panel's input into our steps and moves (they
// only store numbers, R1), every write is done on LateUpdateBehavior through the core's display API. Leaving, the
// game's own repaint first (setCostumeAndColor, SetMessage*), then only what it did not put back (R36); never the Color
// row's value text. The Param's getters answer the costume and colour it had, so the outfit never changes meanwhile.
//
// ONE owner per panel: the Color Mod's DI mode borrows the same rows (two writers on one UI crashed the game before).
// The owner of each side's panel is kept here (SF6Slots_SelectPanelClaim, below): this mode takes it before borrowing
// the rows and gives it back after, the Color Mod (0.22.5 and later) does the same for its DI mode and refuses X while
// we hold it. A Color Mod that does not know the claim (no SF6ColorMod_DiPanelActive export) turns this mode off.
#include "rto.hpp"
#include "slots_ui.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>

namespace slots::rto::select {
namespace {

constexpr const char* kRowNames[kPanelRows] = { "p_Spin_costume", "p_Spin_color", "p_Spin_type", "p_Spin_preset" };
constexpr int kColorRow = 1;                        // the Color row: its value text is never written back (a fault)
constexpr const char* kAgentNames[2] = { "ui10505", "ui10507" };
constexpr const char* kSettings = "reframework/data/SF6_CostumeSlots_data/settings.json";
constexpr const char* kEntryLabel = "Costume options";   // English only: the plugin has no locale system
// app.InputDigitalFlag (Down 1, Trigger 2, Release 4, Repeat 8): a press is one Trigger, a held key a Repeat (the DI
// mode's rule: only those act)
constexpr uint32_t kActFlags = 2 | 8;
constexpr uint32_t kSettleFrames = 20;              // the panel active this long before it is borrowed (DI mode: 20)
constexpr uint32_t kCancelBlock = 15;               // the press that left the mode neither confirms nor backs out
constexpr size_t kTitleChars = 24, kLabelChars = 28;
enum Owner { kFree = 0, kOurs = 1, kColorMod = 2 };

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// ---- the owner of each side's panel (any thread: the exports) ----
std::atomic<int> g_owner[3];

// ---- the hooks' side: what they map and what they captured (any thread, under g_hmx) ----
struct Act { enum Kind { Step, Move, Leave } kind = Step; int row = -1; int dir = 0; };   // row -1: the focused one
struct HookSide {
    bool active = false;
    uint64_t param = 0, agent = 0, group = 0;
    uint64_t spin[kPanelRows] = {};
    int32_t ci = -1, co = -1;
    std::deque<Act> acts;
    uint32_t last_step = 0, last_move = 0;
};
std::mutex g_hmx;
HookSide g_h[3];
std::atomic<bool> g_any{ false };
std::unordered_map<uint64_t, uint32_t> g_cancel;    // param -> frame until which IsDecide / IsCancel answer false
std::atomic<uint32_t> g_cancel_until{ 0 };          // the last frame of any cancel block (the gate's fast exit)
std::atomic<uint32_t> g_hook_faults{ 0 };
thread_local int64_t tl_ret = INT64_MIN;

bool acts(void* flag) { return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(flag) & 0xFFFFFFFF) & kActFlags) != 0; }
uint32_t u32_of(void* a) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFF); }

int side_by_spin(uint64_t a, int* row) {
    for (int s = 1; s <= 2; ++s) {
        if (!g_h[s].active) continue;
        for (int k = 0; k < kPanelRows; ++k) if (g_h[s].spin[k] == a) { *row = k; return s; }
    }
    return 0;
}
int side_by_param(uint64_t a) { for (int s = 1; s <= 2; ++s) if (g_h[s].active && g_h[s].param == a) return s; return 0; }
int side_by_agent(uint64_t a) { for (int s = 1; s <= 2; ++s) if (g_h[s].active && g_h[s].agent == a) return s; return 0; }
int side_by_group(uint64_t a) { for (int s = 1; s <= 2; ++s) if (g_h[s].active && g_h[s].group == a) return s; return 0; }
bool cancel_blocked(uint64_t a) { auto it = g_cancel.find(a); return it != g_cancel.end() && g_frame < it->second; }

void push_step(int s, int row, int dir) {
    HookSide& h = g_h[s];
    if (h.last_step && g_frame == h.last_step) return;   // one step per frame (two paths of one press)
    h.last_step = g_frame;
    h.acts.push_back({ Act::Step, row, dir });
}
void push_move(int s, int dir) {
    HookSide& h = g_h[s];
    if (h.last_move && g_frame == h.last_move) return;
    h.last_move = g_frame;
    h.acts.push_back({ Act::Move, -1, dir });
}

// The hooks (REFramework pre / post). Each one: nothing unless a side is on, then a lookup of `this` in the map.
constexpr int kCall = REFRAMEWORK_HOOK_CALL_ORIGINAL, kSkip = REFRAMEWORK_HOOK_SKIP_ORIGINAL;

int spin_step(void** argv, int dir, bool flagged) {
    std::lock_guard<std::mutex> lk(g_hmx);
    int row = -1;
    const int s = side_by_spin(reinterpret_cast<uint64_t>(argv[1]), &row);
    if (!s) return kCall;
    if (flagged && !acts(argv[2])) return kSkip;         // held / released: nothing (our spin never moves itself)
    push_step(s, row, dir);
    return kSkip;
}
template <int Dir, bool Flagged> int pre_spin_step(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try { return spin_step(argv, Dir, Flagged); } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
int pre_spin_dir(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputDirection(dir, flag)
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        int row = -1;
        const int s = side_by_spin(reinterpret_cast<uint64_t>(argv[1]), &row);
        if (!s) return kCall;
        const uint32_t d = u32_of(argv[2]);
        if ((d & 12) && !acts(argv[3])) return kSkip;
        if (d & 4) { push_step(s, row, -1); return kSkip; }
        if (d & 8) { push_step(s, row, 1); return kSkip; }
        return kCall;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
int pre_param_skip(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        return side_by_param(reinterpret_cast<uint64_t>(argv[1])) ? kSkip : kCall;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
// IsDecide / IsCancel / isSelectOperationType: false while the side is ours, and just after (cancel block)
int pre_param_gate(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_ret = INT64_MIN;
    if (g_closing.load(std::memory_order_relaxed)) return kCall;
    if (!g_any.load(std::memory_order_relaxed) && g_frame >= g_cancel_until.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const uint64_t a = reinterpret_cast<uint64_t>(argv[1]);
        if (side_by_param(a) || cancel_blocked(a)) { tl_ret = 0; return kSkip; }
        return kCall;
    } catch (...) { tl_ret = INT64_MIN; g_hook_faults.fetch_add(1); return kCall; }
}
// getCostume / getColor: the indexes the panel had (its spins show our values meanwhile)
template <bool Costume> int pre_param_get(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_ret = INT64_MIN;
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const int s = side_by_param(reinterpret_cast<uint64_t>(argv[1]));
        const int32_t v = s ? (Costume ? g_h[s].ci : g_h[s].co) : -1;
        if (v < 0) return kCall;
        tl_ret = v;
        return kSkip;
    } catch (...) { tl_ret = INT64_MIN; g_hook_faults.fetch_add(1); return kCall; }
}
void post_ret(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_ret != INT64_MIN && ret_val) *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(tl_ret));
    tl_ret = INT64_MIN;
}
template <int Dir> int pre_agent_lr(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputLeft / Right(flag)
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const int s = side_by_agent(reinterpret_cast<uint64_t>(argv[1]));
        if (!s) return kCall;
        if (acts(argv[2])) push_step(s, -1, Dir);
        return kSkip;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
template <int Dir> int pre_agent_ud(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputUp / Down(flag)
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const int s = side_by_agent(reinterpret_cast<uint64_t>(argv[1]));
        if (!s) return kCall;
        if (acts(argv[2])) push_move(s, Dir);
        return kSkip;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
int pre_agent_dir(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputDirection(dir, flag)
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const int s = side_by_agent(reinterpret_cast<uint64_t>(argv[1]));
        if (!s) return kCall;
        const uint32_t d = u32_of(argv[2]);
        if ((d & 15) && !acts(argv[3])) return kSkip;
        if (d & 4) { push_step(s, -1, -1); return kSkip; }
        if (d & 8) { push_step(s, -1, 1); return kSkip; }
        if (d & 3) { push_move(s, (d & 1) ? -1 : 1); return kSkip; }
        return kCall;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
int pre_agent_leave(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputDecide / InputCancel
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const int s = side_by_agent(reinterpret_cast<uint64_t>(argv[1]));
        if (!s) return kCall;
        g_h[s].acts.push_back({ Act::Leave, -1, 0 });
        return kSkip;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}
int pre_group_move(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // MoveFocus(.., dir)
    if (!g_any.load(std::memory_order_relaxed) || g_closing.load(std::memory_order_relaxed)) return kCall;
    try {
        std::lock_guard<std::mutex> lk(g_hmx);
        const int s = side_by_group(reinterpret_cast<uint64_t>(argv[1]));
        if (!s) return kCall;
        push_move(s, (u32_of(argv[3]) & 1) ? -1 : 1);
        return kSkip;
    } catch (...) { g_hook_faults.fetch_add(1); return kCall; }
}

// ---- the arrows' copies, made inside the select flow's update (any thread; the frame asks, header) ----
constexpr const char* kArrowName = "e_text_slots_arrow";   // not "p_...": never taken for a UI part (R20)
constexpr int kArrowRows[2] = { 0, kPanelRows - 1 };       // ▲ on the first row, ▼ on the last
constexpr int kArrowMaxRefused = 3;
struct CopyReq { uintptr_t agent = 0; uint32_t frame = 0; };
std::mutex g_copy_mx;
CopyReq g_copy_req[3];
std::atomic<bool> g_copy_on{ false };
std::atomic<int> g_copy_refused{ 0 }, g_copy_made{ 0 }, g_copy_status{ 1000 };
std::atomic<uint32_t> g_flow_updates{ 0 };                  // the flow's update seen (its post-hook ran)

void make_copies() {
    CopyReq req[3];
    {
        std::lock_guard<std::mutex> lk(g_copy_mx);
        for (int s = 1; s <= 2; ++s) {
            if (g_copy_req[s].agent && g_frame - g_copy_req[s].frame <= 2) req[s] = g_copy_req[s];   // older: asked again
            g_copy_req[s] = CopyReq{};
        }
        g_copy_on.store(false, std::memory_order_release);
    }
    for (int s = 1; s <= 2; ++s) {
        Obj* agent = reinterpret_cast<Obj*>(req[s].agent);
        if (!agent || !alive(agent)) continue;
        void* cm = call_obj(agent, "get_ControlMain");
        for (int k : kArrowRows) {
            void* row = cm ? A()->ui_find_child(cm, kRowNames[k], 8) : nullptr;
            void* title = row ? A()->ui_find_child(row, "e_text_title", 4) : nullptr;
            Obj* parent = title ? call_obj(reinterpret_cast<Obj*>(title), "get_Parent") : nullptr;
            if (!parent) { ++g_copy_refused; g_copy_status.store(-3000, std::memory_order_release); continue; }
            if (A()->ui_find_child(parent, kArrowName, 0)) continue;
            void* c = nullptr;
            const SF6CoreStatus st = A()->ui_clone_control(C(), title, parent, kArrowName, &c);
            g_copy_status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st), std::memory_order_release);
            if (st != SF6CORE_OK || !c) { ++g_copy_refused; continue; }
            ++g_copy_made;
            A()->ui_set_visible(C(), c, 0);                 // placed and shown by the frame
            A()->ui_set_force_invisible(C(), c, 1);
        }
    }
}
void post_flow_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return;
    g_flow_updates.fetch_add(1, std::memory_order_relaxed);
    if (!g_copy_on.load(std::memory_order_acquire)) return;
    try { make_copies(); } catch (...) { ++g_copy_refused; g_copy_status.store(-2000, std::memory_order_release); }   // /EHa: SEH too
}

// Installed the first time a side is borrowed (a player who never uses the mode pays nothing): the DI mode's set
bool g_hooks_tried = false, g_hooks_ok = false;
bool g_arrow_hook = false;                          // the flow's update hooked: arrow copies can be made
void install_hooks() {
    g_hooks_tried = true;
    auto* tdb = api().tdb();
    int n = 0, missing = 0;
    auto hook = [&](TD* td, const char* name, REFPreHookFn pre, REFPostHookFn post) {
        Method* m = td ? td->find_method(name) : nullptr;
        if (m && m->add_hook(pre, post ? post : hook_post_pass, false)) ++n;
        else ++missing;
    };
    TD* spin = tdb->find_type("app.UIPartsSpin");
    hook(spin, "Add", pre_spin_step<1, false>, nullptr);
    hook(spin, "AddNum", pre_spin_step<1, false>, nullptr);
    hook(spin, "MouseAdd", pre_spin_step<1, false>, nullptr);
    hook(spin, "Sub", pre_spin_step<-1, false>, nullptr);
    hook(spin, "SubNum", pre_spin_step<-1, false>, nullptr);
    hook(spin, "MouseSub", pre_spin_step<-1, false>, nullptr);
    hook(spin, "InputRight", pre_spin_step<1, true>, nullptr);
    hook(spin, "InputLeft", pre_spin_step<-1, true>, nullptr);
    hook(spin, "InputDirection", pre_spin_dir, nullptr);
    TD* param = tdb->find_type("app.UIFlowUI10505.Param");
    for (const char* nm : { "SelectChangeCostume", "SelectChangeColorChip", "SelectChangeControllerType", "SetMessageCostume",
                            "SetMessageColor", "SetMessagePreset", "SetMessageList", "setCostume", "setColor", "setCostumeAndColor" })
        hook(param, nm, pre_param_skip, nullptr);
    for (const char* nm : { "IsDecide", "IsCancel", "isSelectOperationType" }) hook(param, nm, pre_param_gate, post_ret);
    hook(param, "getCostume", pre_param_get<true>, post_ret);
    hook(param, "getColor", pre_param_get<false>, post_ret);
    TD* agent = tdb->find_type("app.UIAgent");
    hook(agent, "InputLeft", pre_agent_lr<-1>, nullptr);
    hook(agent, "InputRight", pre_agent_lr<1>, nullptr);
    hook(agent, "InputUp", pre_agent_ud<-1>, nullptr);
    hook(agent, "InputDown", pre_agent_ud<1>, nullptr);
    hook(agent, "InputDirection", pre_agent_dir, nullptr);
    hook(agent, "InputDecide", pre_agent_leave, nullptr);
    hook(agent, "InputCancel", pre_agent_leave, nullptr);
    hook(tdb->find_type("app.UIPartsGroup"), "MoveFocus", pre_group_move, nullptr);
    // the steps, the moves and the way out are what the mode needs; without them it never starts
    g_hooks_ok = spin && param && agent && n >= 20;
    logf("rto select: %d hooks installed, %d not found: the costume options mode on the character select %s", n, missing,
         g_hooks_ok ? "is ready" : "is off");
    // the arrows' copies (R19: inside the select flow's own update); without it the arrows go before the titles
    // Its own OnUpdate only (never a parent's: that one runs for every flow of the game)
    TD* cs = tdb->find_type("app.UIFlowUI10505.CharaSetting");
    Method* upd = nullptr;
    if (cs) for (auto* m : cs->get_methods()) if (m && m->get_name() && std::strcmp(m->get_name(), "OnUpdate") == 0) { upd = m; break; }
    g_arrow_hook = SF6CORE_HAS(A(), ui_clone_control) && upd && upd->get_function_raw() && upd->add_hook(hook_pass, post_flow_update, false);
    logf("rto select: arrows %s", g_arrow_hook ? "in the rows' left margin (copies made in app.UIFlowUI10505.CharaSetting.OnUpdate)"
         : !SF6CORE_HAS(A(), ui_clone_control) ? "before the titles (the core has no ui_clone_control, 0.8.2)"
         : "before the titles (app.UIFlowUI10505.CharaSetting.OnUpdate not hooked)");
}

// ---- the tick's side (LateUpdateBehavior only) ----
std::optional<int32_t> ret_i32(Obj* o, const char* name) {
    auto r = call(o, name);
    if (r.exception_thrown) return std::nullopt;
    return static_cast<int32_t>(r.dword);
}
bool ret_floats(Obj* o, const char* name, float* out, int n) {
    auto r = call(o, name);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), static_cast<size_t>(n) * 4);
    return true;
}
std::string message(void* t) { return t ? str(call_obj(reinterpret_cast<Obj*>(t), "get_Message")) : std::string(); }
void* find(void* root, const char* name, int depth = 8) { return root ? A()->ui_find_child(root, name, depth) : nullptr; }
void inv(void* o, const char* sig) { if (o) A()->obj_invoke(C(), o, sig, nullptr, 0, nullptr); }
void inv_i(void* o, const char* sig, int64_t v) {
    if (!o) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v;
    A()->obj_invoke(C(), o, sig, &a, 1, nullptr);
}
void inv_ii(void* o, const char* sig, int64_t v0, int64_t v1) {
    if (!o) return;
    SF6CoreArg a[2]{}; a[0].type = a[1].type = SF6CORE_ARG_I64; a[0].v.i = v0; a[1].v.i = v1;
    A()->obj_invoke(C(), o, sig, a, 2, nullptr);
}
void inv_b(void* o, const char* sig, bool v) {
    if (!o) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_BOOL; a.v.i = v ? 1 : 0;
    A()->obj_invoke(C(), o, sig, &a, 1, nullptr);
}
void vis(void* c, bool on) { if (c) A()->ui_set_visible(C(), c, on ? 1 : 0); }
void put_text(void* t, const std::string& s) { if (t) A()->ui_set_text(C(), t, s.c_str()); }   // compare-before-write (core)
void spin_set(Obj* sp, int32_t mn, int32_t mx, int32_t num) {
    inv_i(sp, "set_MinNum", mn); inv_i(sp, "set_MaxNum", mx);
    if (sp) A()->obj_set_i64(C(), sp, "_Num", num);
}

Obj* param_of(Obj* agent) {
    Obj* deleg = get_obj(agent, "<InputDelegates>k__BackingField");
    Obj* flow = deleg ? get_obj(deleg, "<Flow>k__BackingField") : nullptr;
    return flow ? get_obj(flow, "_Param") : nullptr;
}
Obj* control_main(Obj* agent) { return alive(agent) ? call_obj(agent, "get_ControlMain") : nullptr; }
std::string focused_row(Obj* agent) {
    Obj* item = call_obj(agent, "GetFocusItem");
    Obj* ctrl = item ? get_obj(item, "<Control>k__BackingField") : nullptr;
    return ctrl ? str(call_obj(ctrl, "get_Name")) : std::string();
}
int row_index(const std::string& nm) { for (int k = 0; k < kPanelRows; ++k) if (nm == kRowNames[k]) return k; return -1; }

struct RowParts { void* ctrl = nullptr; void* title = nullptr; void* text = nullptr; void* list = nullptr; void* chip = nullptr;
                  void* rect0 = nullptr; void* rect1 = nullptr; void* arrow[2] = {}; };
RowParts parts_of(void* cm, int k) {
    RowParts p;
    p.ctrl = find(cm, kRowNames[k]);
    if (!p.ctrl) return p;
    p.title = find(p.ctrl, "e_text_title", 4);
    p.text = find(p.ctrl, "e_text", 4);
    p.list = find(p.ctrl, "p_TextList_color_h", 4);
    if (!p.list) p.list = find(p.ctrl, "p_TextList_type_h", 4);
    p.arrow[0] = find(p.ctrl, "p_MouseDecideOperable_L", 4);
    p.arrow[1] = find(p.ctrl, "p_MouseDecideOperable_R", 4);
    if (k == kColorRow) { p.chip = find(p.ctrl, "c_chip", 4); p.rect0 = find(p.ctrl, "e_rect_color_0", 4); p.rect1 = find(p.ctrl, "e_rect_color_1", 4); }
    return p;
}

// What a borrowed row was, put back when the mode ends
struct RowSave {
    Obj* spin = nullptr;
    int32_t smin = 0, smax = 0, snum = 0;
    bool disabled = false, has_disabled = false;
    bool visible = true;
    bool has_text = false; std::string text;
    bool has_pos = false; float pos[3] = {};
    bool has_region = false; float region[2] = {};
    bool has_mid = false; uint8_t mid[16] = {};
    bool arrow_locked[2] = {}, has_arrow[2] = {};
};

struct Side {
    // the panel, followed while the screen is up (game objects: never add_ref'd, R21; checked alive before each use)
    Obj* agent = nullptr;
    Obj* param = nullptr;
    bool was_active = false;
    uint32_t active_since = 0;
    int fighter = -1, costume = -1;
    uint32_t changed_at = 0;                        // the frame the panel's fighter / costume last changed
    bool key_was = false;
    // the mode
    bool on = false;
    Obj* group = nullptr;
    const Slot* slot = nullptr;
    int opt = 0;                                    // the choices' side (0 P1, 1 P2)
    std::vector<Row> rows;
    std::vector<std::string> chosen, shown;
    PanelWindow w;
    int32_t ci = -1, co = -1;
    RowSave rs[kPanelRows];
    float ref_pos[3] = {}; bool has_ref_pos = false;
    float ref_region[2] = { 300, 0 }; bool has_ref_region = false;
    uint32_t entered_at = 0;
    // the arrows (kArrowRows): where each copy goes, measured once a visit (the glyph's width until it is drawn)
    struct Arrow {
        bool geo = false, failed = false, styled = false, shown = false, measured = false, logged = false;
        int passes = 0;
        float x = 0, y = 0, z = 0;                  // the title's place (its parent's space)
        int cp = 0;
        double title_l = 0, title_lg = 0, row_lg = 0, k = 1, gsx = 1, glyph = 0;
        const char* row_how = "";
        std::string text, row_el;
    } arrow[2];
    uint32_t arrow_asked = 0;                       // the first frame a copy was asked for and not found since
};
Side g_s[3];
bool g_was_up = false;                              // the character select was up last frame
int g_faults = 0;
bool g_off = false;
bool g_arrows_off = false;                          // no arrow copy can be made: the arrows go before the titles

// ---- the trigger: R1 / the key, read per player ----
struct Keys {
    bool read = false;
    std::string pad_name = "R1";                    // settings.json "select_options_button": R1 L1 R2 L2 R3 L3, "off"
    uint32_t pad_bit = 0;
    std::string pad_icon;
    std::string key_cfg = "C";                      // "select_options_key": a letter / digit (C), "auto" (the game's keyboard
                                                    // key for R1's UI action, UITabR: E on AZERTY), "off"
    int vk = 0;
    std::string key_icon;
    uint32_t at = 0;
};
Keys g_keys;

void read_keys() {
    Keys k;
    k.read = true;
    k.at = g_frame;
    Json j;
    std::string text = read_file(kSettings);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    if (!text.empty() && json_parse(text, j)) {
        if (const Json* b = j.get("select_options_button"); b && b->kind == Json::String && !b->s.empty()) k.pad_name = b->s;
        if (const Json* v = j.get("select_options_key"); v && v->kind == Json::String && !v->s.empty()) k.key_cfg = v->s;
    }
    for (auto& c : k.pad_name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    struct { const char* name; const char* field; const char* icon; } pads[] = {
        { "R1", "RTrigTop", "<ICON BtnR1>" }, { "L1", "LTrigTop", "<ICON BtnL1>" }, { "R2", "RTrigBottom", "<ICON BtnR2>" },
        { "L2", "LTrigBottom", "<ICON BtnL2>" }, { "R3", "RStickPush", "<ICON BtnR3>" }, { "L3", "LStickPush", "<ICON BtnL3>" } };
    for (auto& p : pads) {
        if (k.pad_name != p.name) continue;
        k.pad_icon = p.icon;
        if (auto* td = api().tdb()->find_type("via.hid.GamePadButton"))
            if (auto* f = td->find_field(p.field)) k.pad_bit = static_cast<uint32_t>(f->get_data<int32_t>(nullptr));
    }
    std::string kc = k.key_cfg;
    for (auto& c : kc) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (kc == "AUTO") {
        std::string sym;
        k.vk = menu::tab_key(true, &sym);           // the game's keyboard key for R1's action (UITabR): E on AZERTY
        k.key_icon = sym;
    } else if (kc.size() == 1 && ((kc[0] >= 'A' && kc[0] <= 'Z') || (kc[0] >= '0' && kc[0] <= '9'))) {
        k.vk = kc[0];
        k.key_icon = std::string("<ICON Key") + kc[0] + ">";
    }
    const std::string sig = k.pad_name + "/" + k.key_cfg + "/" + std::to_string(k.pad_bit) + "/" + std::to_string(k.vk);
    const std::string old = g_keys.read ? g_keys.pad_name + "/" + g_keys.key_cfg + "/" + std::to_string(g_keys.pad_bit) + "/" + std::to_string(g_keys.vk) : "";
    if (sig != old) {
        logf("rto select: costume options key: pad %s (%s), keyboard %s (%s)", k.pad_bit ? k.pad_name.c_str() : "none",
             k.pad_bit ? k.pad_icon.c_str() : "off or unknown", k.vk ? k.key_icon.c_str() : "none",
             k.key_cfg.c_str());
        // the Color Mod (0.22.6+) opens its DI mode with L1 / X on this screen: the same press would do both
        if ((k.pad_bit && k.pad_name == "L1") || (k.vk == 'X'))
            logf("rto select: WARNING: %s is the Color Mod's Drive colours key on the character select (change "
                 "select_options_button / select_options_key in settings.json)", k.vk == 'X' ? "X" : "L1");
    }
    g_keys = k;
}

bool game_has_focus() {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
uint32_t any_pad_mask() {
    static Method* m_dev = nullptr;
    static bool tried = false;
    void* gp = api().get_native_singleton("via.hid.GamePad");
    if (!gp) return 0;
    if (!tried) {
        tried = true;
        if (auto* td = api().tdb()->find_type("via.hid.GamePad")) m_dev = method_of(td, "get_ConnectingDevices", 0);
    }
    Obj* devs = m_dev ? call_obj(m_dev, reinterpret_cast<Obj*>(gp)) : nullptr;
    uint32_t mask = 0;
    for (int i = 0, n = list_count(devs); i < n; ++i)
        if (Obj* pad = list_item(devs, i)) { auto r = call(pad, "get_Button"); if (!r.exception_thrown) mask |= r.dword; }
    return mask;
}
// The button / key of one player: app.InputManager's own per-player reading (the Color Mod's X rule), else any device
bool trigger_down(int side) {
    if (!game_has_focus()) return false;
    const bool key = g_keys.vk && (GetAsyncKeyState(g_keys.vk) & 0x8000) != 0;
    if (Obj* im = reinterpret_cast<Obj*>(api().get_managed_singleton("app.InputManager"))) {
        static Method* m_flags = nullptr;
        static Method* m_kb = nullptr;
        if (!m_flags) m_flags = method_of(im->get_type_definition(), "GetGamePadRawFlags", 2);
        if (!m_kb) m_kb = method_of(im->get_type_definition(), "GetKeyboardMousePlayerIndex", 0);
        if (m_flags) {
            if (g_keys.pad_bit) {
                auto r = call(m_flags, im, { reinterpret_cast<void*>(static_cast<uintptr_t>(g_keys.pad_bit)), arg_i32(side - 1) });
                if (!r.exception_thrown && (r.dword & 1)) return true;
            }
            auto kb = call(m_kb, im);
            if (!kb.exception_thrown) return key && static_cast<int32_t>(kb.dword) == side - 1;
        }
    }
    return key || (g_keys.pad_bit && (any_pad_mask() & g_keys.pad_bit) != 0);
}

// ---- the Color Mod: its DI mode borrows the same panel ----
using DiActiveFn = int (*)(int side);
DiActiveFn g_cm_di = nullptr;
bool g_cm_checked = false, g_cm_loaded = false;
uint32_t g_cm_stale_since[3] = {};
void color_mod_check() {
    if (g_cm_checked) return;
    g_cm_checked = true;
    HMODULE h = GetModuleHandleW(L"SF6_ColorModNative.dll");
    g_cm_loaded = h != nullptr;
    g_cm_di = h ? reinterpret_cast<DiActiveFn>(GetProcAddress(h, "SF6ColorMod_DiPanelActive")) : nullptr;
    logf("rto select: Color Mod %s", !g_cm_loaded ? "not loaded" : g_cm_di
         ? "loaded, its DI mode takes the panel's claim: one owner per panel"
         : "loaded but older (no SF6ColorMod_DiPanelActive): costume options on the character select are OFF (its DI mode "
           "borrows the same rows; two writers on one panel crashed the game)");
}
bool color_mod_ok() { color_mod_check(); return !g_cm_loaded || g_cm_di; }
bool di_on(int side) { return g_cm_di && g_cm_di(side) != 0; }

// A claim of the Color Mod left behind (its DI mode no longer on for a second): freed
void claims_tick() {
    for (int s = 1; s <= 2; ++s) {
        if (g_owner[s].load() != kColorMod || di_on(s)) { g_cm_stale_since[s] = 0; continue; }
        if (!g_cm_stale_since[s]) { g_cm_stale_since[s] = g_frame; continue; }
        if (g_frame - g_cm_stale_since[s] < 60) continue;
        int want = kColorMod;
        if (g_owner[s].compare_exchange_strong(want, kFree)) logf("rto select: P%d panel: the Color Mod's claim had no DI mode for 60 frames, freed", s);
        g_cm_stale_since[s] = 0;
    }
}

// ---- the guide entry, published for the Color Mod (SF6Slots_SelectGuideEntry, any thread) ----
std::mutex g_pub_mx;
struct Pub { std::string pad, key; uint64_t ms = 0; };
Pub g_pub[3];
void publish(int side, bool show) {
    std::lock_guard<std::mutex> lk(g_pub_mx);
    Pub& p = g_pub[side];
    p.pad = show && !g_keys.pad_icon.empty() && g_keys.pad_bit ? g_keys.pad_icon + " " + kEntryLabel : std::string();
    p.key = show && g_keys.vk ? g_keys.key_icon + " " + kEntryLabel : std::string();
    p.ms = GetTickCount64();
}

// ---- the mode ----
void hook_side_set(int s, const Side& st) {
    std::lock_guard<std::mutex> lk(g_hmx);
    HookSide& h = g_h[s];
    h = HookSide{};
    h.active = true;
    h.param = reinterpret_cast<uint64_t>(st.param);
    h.agent = reinterpret_cast<uint64_t>(st.agent);
    h.group = reinterpret_cast<uint64_t>(st.group);
    for (int k = 0; k < kPanelRows; ++k) h.spin[k] = reinterpret_cast<uint64_t>(st.rs[k].spin);
    h.ci = st.ci; h.co = st.co;
    g_any.store(true);
}
void hook_side_clear(int s) {
    std::lock_guard<std::mutex> lk(g_hmx);
    if (g_h[s].active && g_h[s].param) {                                   // before the map is cleared
        g_cancel[g_h[s].param] = g_frame + kCancelBlock;
        g_cancel_until.store(g_frame + kCancelBlock);
    }
    g_h[s] = HookSide{};
    g_any.store(g_h[1].active || g_h[2].active);
    for (auto it = g_cancel.begin(); it != g_cancel.end();) it = g_frame >= it->second + 600 ? g_cancel.erase(it) : std::next(it);
}
bool pull(int s, Act& a) {
    std::lock_guard<std::mutex> lk(g_hmx);
    if (g_h[s].acts.empty()) return false;
    a = g_h[s].acts.front();
    g_h[s].acts.pop_front();
    return true;
}

void release(int s) { int want = kOurs; g_owner[s].compare_exchange_strong(want, kFree); }

// The mode dropped without a write (the panel is gone)
void drop(int s, const char* why) {
    if (!g_s[s].on) return;
    hook_side_clear(s);
    release(s);
    const int keep_f = g_s[s].fighter, keep_c = g_s[s].costume;
    const uint32_t keep_at = g_s[s].changed_at;
    g_s[s] = Side{};
    g_s[s].fighter = keep_f; g_s[s].costume = keep_c; g_s[s].changed_at = keep_at;
    logf("rto select: P%d costume options mode dropped (%s)", s, why);
}

void commit(Side& st) {
    auto& sd = choices().mods[st.slot->mod][static_cast<size_t>(st.opt)];
    const std::vector<std::string> saved = sd.changed ? sd.ids : st.slot->defaults;
    sd.changed = true;
    sd.ids = keep_hidden(manifest(), *st.slot, saved, st.shown, st.chosen);   // a hidden NSFW choice stays saved
    st.shown = st.chosen;
    sd.names.clear();
    save_choices();
    choices_changed();                              // the select model wears it on the next pass of rto.cpp
}

std::string row_title(const Side& st, int k) {
    const int i = st.w.win + k;
    // the arrow is a copy in the row's left margin (paint_arrows); in the title only when no copy can be made
    const char* a = g_arrows_off ? panel_arrow(k, st.w.win, static_cast<int>(st.rows.size())) : "";
    return std::string(a) + clip_name(st.rows[static_cast<size_t>(i)].title, kTitleChars);
}

// ---- the arrows: copies of a row's title in its left margin (header) ----
constexpr double kArrowMargin = 8;                  // from the row's outline (1080p units)
constexpr uint32_t kArrowWait = 60;                 // frames a copy may be waited for, then the arrows go in the titles
constexpr int kArrowPasses = 6;                     // placements while the glyph's drawn width is not read yet

bool shown_el(Obj* o) {
    bool ok = false;
    const bool v = call_bool(o, "get_ActualVisible", {}, &ok);
    if (ok) return v;
    return call_bool(o, "get_Visible", {}, &ok) && ok;
}
int cp_of(Obj* o) {
    const Ret r = call(o, "get_ControlPoint");
    int cp = r.exception_thrown ? 0 : static_cast<int>(r.dword);
    return (cp < 0 || cp > 10 || cp % 4 > 2) ? 0 : cp;
}
// The row's outline: the leftmost left edge (root space) of the row's visible elements drawn as a shape (not a text,
// at least 40 wide: its background / frame), NAN when none is found
void outline_walk(Obj* o, int depth, int& budget, double& best, std::string& what) {
    for (; o && budget > 0; o = call_obj(o, "get_Next")) {
        --budget;
        if (!shown_el(o)) continue;
        const std::string nm = str(call_obj(o, "get_Name"));
        if (nm == kArrowName) continue;
        if (type_name(o) != "via.gui.Text") {
            float sz[2] = {}, gp[3] = {}, gs[3] = { 1, 1, 1 };
            if (ret_floats(o, "get_Size", sz, 2) && std::isfinite(sz[0]) && sz[0] >= 40 && ret_floats(o, "get_GlobalPosition", gp, 2)) {
                if (!ret_floats(o, "get_GlobalScale", gs, 2) || !(std::fabs(gs[0]) > 1e-3)) gs[0] = 1;
                const double l = gp[0] - (cp_of(o) % 4) * 0.5 * sz[0] * gs[0];
                if (std::isfinite(l) && (std::isnan(best) || l < best)) { best = l; what = nm; }
            }
        }
        if (depth < 5) outline_walk(call_obj(o, "get_Child"), depth + 1, budget, best, what);
    }
}
// Where the arrow goes, read once a visit: the title's place and left edge, the row's outline (root space)
bool arrow_geo(Side::Arrow& ar, void* row, void* title) {
    Obj* t = reinterpret_cast<Obj*>(title);
    float p[3] = {}, sc[3] = { 1, 1, 1 }, gp[3] = {}, gs[3] = { 1, 1, 1 }, dw[2] = {}, fs[2] = {};
    if (!ret_floats(t, "get_Position", p, 3) || !ret_floats(t, "get_GlobalPosition", gp, 2)) return false;
    if (!ret_floats(t, "get_Scale", sc, 2) || !(std::fabs(sc[0]) > 1e-3)) sc[0] = 1;
    if (!ret_floats(t, "get_GlobalScale", gs, 2) || !(std::fabs(gs[0]) > 1e-3)) gs[0] = sc[0];
    const double tw = ret_floats(t, "get_DisplaySize", dw, 2) && std::isfinite(dw[0]) && dw[0] > 0 ? dw[0] : 0.0;
    ar.x = p[0]; ar.y = p[1]; ar.z = p[2];
    ar.cp = cp_of(t);
    ar.gsx = gs[0];
    ar.k = sc[0] / gs[0];                           // root units -> the title's parent's units
    ar.title_l = p[0] - (ar.cp % 4) * 0.5 * tw * sc[0];
    ar.title_lg = gp[0] - (ar.cp % 4) * 0.5 * tw * gs[0];
    ar.glyph = (ret_floats(t, "get_FontSize", fs, 2) && std::isfinite(fs[0]) && fs[0] > 0 ? fs[0] : 24.0) * std::fabs(gs[0]);
    double best = NAN;
    std::string what;
    int budget = 96;
    outline_walk(call_obj(reinterpret_cast<Obj*>(row), "get_Child"), 0, budget, best, what);
    ar.row_el = what;
    if (!std::isnan(best) && best < ar.title_lg) { ar.row_lg = best; ar.row_how = "the row's outline"; }
    else { ar.row_lg = ar.title_lg - ar.glyph - 2 * kArrowMargin; ar.row_how = "no outline found: next to the title"; }
    ar.geo = true;
    return true;
}
void arrow_hide(void* c, Side::Arrow& ar) {
    if (!ar.shown) return;
    A()->ui_set_visible(C(), c, 0);
    A()->ui_set_force_invisible(C(), c, 1);
    ar.shown = false;
}
void* arrow_copy(void* title) {
    Obj* parent = title ? call_obj(reinterpret_cast<Obj*>(title), "get_Parent") : nullptr;
    return parent ? A()->ui_find_child(parent, kArrowName, 0) : nullptr;
}
// Each frame while on: each arrow row's copy shown with its glyph (or hidden), asked for when missing
void paint_arrows(Side& st, int s, void* cm) {
    if (g_arrows_off) {                             // a copy already shown on the other row goes too
        for (int a = 0; a < 2; ++a)
            if (st.arrow[a].shown) if (void* c = arrow_copy(parts_of(cm, kArrowRows[a]).title)) arrow_hide(c, st.arrow[a]);
        return;
    }
    const int n = static_cast<int>(st.rows.size());
    bool missing = false;
    for (int a = 0; a < 2; ++a) {
        Side::Arrow& ar = st.arrow[a];
        const int k = kArrowRows[a];
        RowParts p = parts_of(cm, k);
        std::string glyph = panel_arrow(k, st.w.win, n);
        while (!glyph.empty() && glyph.back() == ' ') glyph.pop_back();
        void* c = arrow_copy(p.title);
        if (!c) { if (!glyph.empty()) missing = true; continue; }
        if (glyph.empty() || ar.failed) { arrow_hide(c, ar); continue; }
        if (!ar.styled) {                           // its text settings, once a visit
            ar.styled = true;
            SF6CoreArg on{}; on.type = SF6CORE_ARG_BOOL; on.v.i = 1;
            A()->obj_invoke(C(), c, "set_AutoRegionFit(System.Boolean)", &on, 1, nullptr);   // the region is the glyph
        }
        if (!ar.geo && !arrow_geo(ar, p.ctrl, p.title)) { ar.failed = true; logf("rto select: P%d arrow row %d: the title's place unread, no arrow", s, k); continue; }
        if (glyph != ar.text && A()->ui_set_text(C(), c, glyph.c_str()) == SF6CORE_OK) { ar.text = glyph; ar.passes = 0; ar.measured = false; }
        // the glyph's drawn width once shown (the copy keeps the title's scale): placed again with it
        if (ar.shown && !ar.measured) {
            float dw[2] = {};
            if (ret_floats(reinterpret_cast<Obj*>(c), "get_DisplaySize", dw, 2) && std::isfinite(dw[0]) && dw[0] > 0) {
                ar.measured = true;
                const double g = dw[0] * std::fabs(ar.gsx);
                if (std::fabs(g - ar.glyph) > 0.5) { ar.glyph = g; ar.passes = 0; }
            }
        }
        if (ar.passes < kArrowPasses) {
            ++ar.passes;
            const double left = panel_arrow_left(ar.row_lg, ar.title_lg, ar.glyph, kArrowMargin);
            const float x = static_cast<float>(ar.title_l + (left - ar.title_lg) * ar.k);
            SF6CoreArg pc{}; pc.type = SF6CORE_ARG_I64; pc.v.i = (ar.cp / 4) * 4;              // the title's row, its left point
            A()->obj_invoke(C(), c, "set_ControlPoint(via.gui.ControlPoint)", &pc, 1, nullptr);
            A()->ui_set_position(C(), c, x, ar.y, ar.z);
            if (!ar.logged && (ar.measured || ar.passes == kArrowPasses)) {
                ar.logged = true;
                logf("rto select: P%d arrow row %d: outline x %.0f (%s %s), title x %.0f, glyph %.0f wide (%s), arrow x %.0f "
                     "(root units; %.1f in the title's parent, control point %d)", s, k, ar.row_lg, ar.row_how,
                     ar.row_el.c_str(), ar.title_lg, ar.glyph, ar.measured ? "drawn" : "font size", left, x, (ar.cp / 4) * 4);
            }
        }
        if (!ar.shown) {
            A()->ui_set_force_invisible(C(), c, 0);     // the copy's lock, put on when it was made
            A()->ui_set_visible(C(), c, 1);
            ar.shown = true;
        }
    }
    if (!missing) { st.arrow_asked = 0; return; }
    if (!st.arrow_asked) st.arrow_asked = g_frame;
    const int refused = g_copy_refused.load();
    if (!g_arrow_hook || refused >= kArrowMaxRefused || g_frame - st.arrow_asked > kArrowWait) {
        g_arrows_off = true;
        logf("rto select: arrows before the titles from now on (%s; copies made %d, refused %d, last status %d, flow updates seen %u)",
             !g_arrow_hook ? "no hook" : refused >= kArrowMaxRefused ? "copies refused" : "no copy within a second",
             g_copy_made.load(), refused, g_copy_status.load(), g_flow_updates.load());
        return;
    }
    std::lock_guard<std::mutex> lk(g_copy_mx);
    g_copy_req[s] = { reinterpret_cast<uintptr_t>(st.agent), g_frame };
    g_copy_on.store(true, std::memory_order_release);
}
// Leaving: both copies hidden (and locked)
void hide_arrows(Side& st, void* cm) {
    for (int a = 0; a < 2; ++a) {
        RowParts p = parts_of(cm, kArrowRows[a]);
        if (void* c = arrow_copy(p.title)) { st.arrow[a].shown = true; arrow_hide(c, st.arrow[a]); }
    }
}
std::string row_value(const Row& r) {
    const int v = std::max(1, std::min(r.value, static_cast<int>(r.labels.size())));
    return r.labels.empty() ? std::string() : clip_name(r.labels[static_cast<size_t>(v - 1)], kLabelChars);
}

// Each frame while on (the game's own refresh rewrites the rows: the DI mode's repaint)
void paint(Side& st, int s) {
    void* cm = control_main(st.agent);
    if (!cm) return;
    const int n = static_cast<int>(st.rows.size());
    for (int k = 0; k < kPanelRows; ++k) {
        RowParts p = parts_of(cm, k);
        if (!p.ctrl) continue;
        const int i = st.w.win + k;
        if (i >= n) { vis(p.ctrl, false); continue; }   // no option row for it: hidden (shown again when leaving)
        const Row& r = st.rows[static_cast<size_t>(i)];
        vis(p.ctrl, true);
        put_text(p.title, row_title(st, k));
        put_text(p.text, row_value(r));
        vis(p.chip, false);                         // the colour chips (every row looks the same)
        for (void* a : p.arrow) if (a) { A()->ui_set_force_invisible(C(), a, 0); vis(a, true); }
        if (p.list && st.has_ref_pos) {
            float cur[3] = {};
            if (!ret_floats(reinterpret_cast<Obj*>(p.list), "get_Position", cur, 3) || std::fabs(cur[0] - st.ref_pos[0]) > 0.5f
                || std::fabs(cur[1] - st.ref_pos[1]) > 0.5f)
                A()->ui_set_position(C(), p.list, st.ref_pos[0], st.ref_pos[1], st.ref_pos[2]);
        }
        if (p.text && st.has_ref_region) A()->ui_set_region_size(C(), p.text, st.ref_region[0], st.ref_region[1]);   // compares
        spin_set(st.rs[k].spin, 0, 1 + static_cast<int32_t>(r.labels.size()), r.value);
    }
    paint_arrows(st, s, cm);
}

// The focus row the game shows -> the cursor (a mouse hover moves the focus without our hooks)
void sync_focus(Side& st) {
    const int k = row_index(focused_row(st.agent));
    if (k < 0) return;
    const PanelWindow w = panel_focus(st.w, static_cast<int>(st.rows.size()), k);
    if (w.cur != st.w.cur) st.w = w;
}
void focus_row(Side& st, int k) {
    SF6CoreArg out{};
    if (A()->obj_invoke(C(), st.group, "GetFocusIndex", nullptr, 0, &out) == SF6CORE_OK && out.v.i == k) return;
    inv_i(st.group, "ChangeFocusIndex", k);
}

bool enter(int s, const char** why) {
    Side& st = g_s[s];
    const Slot* slot = select_slot(s, nullptr);
    if (!slot) { *why = "this outfit has no options"; return false; }
    if (!g_hooks_tried) install_hooks();
    if (!g_hooks_ok) { *why = "the hooks are not installed"; return false; }
    Obj* cm = control_main(st.agent);
    Obj* spins = get_obj(st.param, "Spins");
    Obj* group = get_obj(st.param, "Group");
    if (!cm || !spins || !group || list_count(spins) < kPanelRows) { *why = "the panel is not built"; return false; }
    int want = kFree;
    if (!g_owner[s].compare_exchange_strong(want, kOurs) && want != kOurs) { *why = "the Color Mod's DI mode holds this panel"; return false; }
    Side n;
    n.agent = st.agent; n.param = st.param; n.was_active = st.was_active; n.active_since = st.active_since;
    n.fighter = st.fighter; n.costume = st.costume; n.changed_at = st.changed_at; n.key_was = st.key_was;
    n.group = group;
    n.slot = slot;
    n.opt = costumes::online() ? 0 : s - 1;
    n.chosen = chosen_of(manifest(), *slot, choices(), n.opt);
    n.shown = n.chosen;
    n.rows = rows(manifest(), *slot, n.chosen);
    if (n.rows.empty()) { release(s); *why = "no option row"; return false; }
    for (int k = 0; k < kPanelRows; ++k) {
        RowSave& rs = n.rs[k];
        rs.spin = list_item(spins, k);
        if (!rs.spin) { release(s); *why = "a row's spin is missing"; return false; }
        rs.smin = ret_i32(rs.spin, "get_MinNum").value_or(0);
        rs.smax = ret_i32(rs.spin, "get_MaxNum").value_or(0);
        get_i32(rs.spin, "_Num", rs.snum);
        bool ok = false;
        rs.disabled = call_bool(rs.spin, "get_IsDisable", {}, &ok);
        rs.has_disabled = ok;
        RowParts p = parts_of(cm, k);
        if (!p.ctrl) { release(s); *why = "a row is missing"; return false; }
        rs.visible = call_bool(reinterpret_cast<Obj*>(p.ctrl), "get_Visible");
        if (p.text) { rs.text = message(p.text); rs.has_text = true; }
        if (p.list) rs.has_pos = ret_floats(reinterpret_cast<Obj*>(p.list), "get_Position", rs.pos, 3);
        if (p.text) rs.has_region = ret_floats(reinterpret_cast<Obj*>(p.text), "get_RegionSize", rs.region, 2);
        for (int a = 0; a < 2; ++a)
            if (p.arrow[a]) { rs.arrow_locked[a] = call_bool(reinterpret_cast<Obj*>(p.arrow[a]), "get_ForceInvisible", {}, &ok); rs.has_arrow[a] = ok; }
        if (p.title) rs.has_mid = A()->ui_get_message_id(p.title, rs.mid) == SF6CORE_OK;
    }
    n.has_ref_pos = n.rs[0].has_pos;
    std::copy(n.rs[0].pos, n.rs[0].pos + 3, n.ref_pos);
    n.has_ref_region = n.rs[0].has_region;
    std::copy(n.rs[0].region, n.rs[0].region + 2, n.ref_region);
    n.ci = ret_i32(st.param, "get__cacheCostumeIndex").value_or(-1);
    n.co = ret_i32(st.param, "get__cacheColorIndex").value_or(-1);
    // the cursor: where the game's focus is when that row exists, else the first row
    const int fk = row_index(focused_row(st.agent));
    n.w = panel_clamp({ fk >= 0 ? fk : 0, 0 }, static_cast<int>(n.rows.size()));
    n.on = true;
    n.entered_at = g_frame;
    st = std::move(n);
    hook_side_set(s, st);                           // from now on the panel's input is ours
    // the titles: their message link cut (kept), so the text written stays; a title already showing what we write
    // gets one write that differs first (the real write must differ, the DI mode's rule)
    const uint8_t empty[16] = {};
    for (int k = 0; k < kPanelRows; ++k) {
        RowParts p = parts_of(cm, k);
        if (!p.title || !st.rs[k].has_mid) continue;
        A()->ui_set_message_id(C(), p.title, empty);
        const int i = st.w.win + k;
        if (i >= static_cast<int>(st.rows.size())) continue;
        const std::string want_t = row_title(st, k);
        if (message(p.title) == want_t) put_text(p.title, want_t + " ");
    }
    for (int k = 0; k < kPanelRows; ++k) inv_b(st.rs[k].spin, "set_IsDisable", false);
    if (st.w.cur - st.w.win != fk) focus_row(st, st.w.cur - st.w.win);
    paint(st, s);
    logf("rto select: P%d costume options mode ON: %s/%s (%s), %zu rows, options of %s%s", s, slot->fighter_dir.c_str(),
         slot->folder.c_str(), slot->label.c_str(), st.rows.size(), st.opt ? "P2" : "P1", costumes::online() ? " (online)" : "");
    return true;
}

// Leaving: the rows given back, the game's repaint first, then what it did not put back (R36)
void leave(int s, const char* why) {
    Side st = std::move(g_s[s]);
    g_s[s] = Side{};
    g_s[s].agent = st.agent; g_s[s].param = st.param; g_s[s].was_active = st.was_active; g_s[s].active_since = st.active_since;
    g_s[s].fighter = st.fighter; g_s[s].costume = st.costume; g_s[s].changed_at = st.changed_at; g_s[s].key_was = st.key_was;
    hook_side_clear(s);                             // the game's own calls below must run (and IsCancel blocked 15 frames)
    void* cm = control_main(st.agent);
    if (cm) hide_arrows(st, cm);
    for (int k = 0; k < kPanelRows; ++k) {
        const RowSave& rs = st.rs[k];
        spin_set(rs.spin, rs.smin, rs.smax, rs.snum);
        if (rs.has_disabled) inv_b(rs.spin, "set_IsDisable", rs.disabled);
        RowParts p = cm ? parts_of(cm, k) : RowParts{};
        if (p.title && rs.has_mid) A()->ui_set_message_id(C(), p.title, rs.mid);
        if (p.ctrl) vis(p.ctrl, rs.visible);
        if (p.chip) { vis(p.chip, true); vis(p.rect0, true); vis(p.rect1, true); }
        if (p.list && rs.has_pos) A()->ui_set_position(C(), p.list, rs.pos[0], rs.pos[1], rs.pos[2]);
        if (p.text && rs.has_region) A()->ui_set_region_size(C(), p.text, rs.region[0], rs.region[1]);
        for (int a = 0; a < 2; ++a) if (p.arrow[a] && rs.has_arrow[a] && rs.arrow_locked[a]) A()->ui_set_force_invisible(C(), p.arrow[a], 1);
    }
    if (st.ci >= 0) inv_i(st.param, "set__cacheCostumeIndex", st.ci);
    if (st.co >= 0) inv_i(st.param, "set__cacheColorIndex", st.co);
    inv(st.param, "SetMessageCostume"); inv(st.param, "SetMessageColor");
    inv(st.param, "SelectChangeColorChip");
    // The select screen's SetMessageCostume / SetMessageColor alone reset the selection to costume 0 / colour 0 (the
    // DI mode's finding, 06/10): the game's own setter puts the selection, the texts, the chips and the model back
    if (st.ci >= 0 && st.co >= 0) inv_ii(st.param, "setCostumeAndColor", st.ci, st.co);
    if (cm)
        for (int k = 0; k < kPanelRows; ++k) {                         // never the Color row's value text (a native fault)
            if (k == kColorRow || !st.rs[k].has_text) continue;
            RowParts p = parts_of(cm, k);
            if (p.text && !st.rs[k].text.empty()) put_text(p.text, st.rs[k].text);
        }
    release(s);
    logf("rto select: P%d costume options mode OFF (%s)", s, why);
}

// One step of a row's value (row: the screen row, -1 the focused one)
void step(Side& st, int s, int screen_row, int dir) {
    if (screen_row < 0) {
        screen_row = row_index(focused_row(st.agent));
        if (screen_row < 0) screen_row = st.w.cur - st.w.win;
    }
    const int i = st.w.win + screen_row;
    if (i < 0 || i >= static_cast<int>(st.rows.size())) return;
    st.w.cur = i;
    const Row r = st.rows[static_cast<size_t>(i)];
    const int v = step_value(r.value, static_cast<int>(r.labels.size()), dir);
    if (v == r.value) return;
    std::vector<std::string> notes;
    apply_row(manifest(), *st.slot, st.chosen, r, v, &notes);
    for (auto& n : notes) logf("rto select: %s", n.c_str());
    commit(st);
    st.rows = rows(manifest(), *st.slot, st.chosen);
    st.w = panel_clamp(st.w, static_cast<int>(st.rows.size()));
    logf("rto select: P%d %s: %s = %s (%zu options, saved for %s)", s, st.slot->mod.c_str(), r.title.c_str(),
         v >= 1 && v <= static_cast<int>(r.labels.size()) ? r.labels[static_cast<size_t>(v - 1)].c_str() : "?", st.chosen.size(),
         st.opt ? "P2" : "P1");
}

// Once per visit, a second after a panel settled: the game's own guide bars (their texts with the glyphs), and a
// warning when they already show our button or key (the game would act on the same press). The select screen's
// bindings could not be read offline (no dump of its input table): this is the check in the game.
bool g_guide_logged = false;
void log_game_guide() {
    g_guide_logged = true;
    void* buf[4] = {};
    const uint32_t n = A()->ui_agents("InputGuide", buf, 4);
    std::string all;
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* ag = reinterpret_cast<Obj*>(buf[i]);
        Obj* lst = get_obj(get_obj(ag, "_PartsManager"), "_List");
        for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
            Obj* part = list_item(lst, k);
            if (!part || type_name(part) != "app.UIPartsInputGuideList") continue;
            Obj* ctrl = get_obj(part, "<Control>k__BackingField");
            Obj* btn = ctrl ? reinterpret_cast<Obj*>(A()->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
            for (Obj* c = btn ? call_obj(btn, "get_Child") : nullptr; c; c = call_obj(c, "get_Next")) {
                if (str(call_obj(c, "get_Name")) == "e_text_0") continue;   // the corner: the Color Mod's (and our) entries
                const std::string m = message(c);
                if (!m.empty()) all += (all.empty() ? "" : " | ") + m;
            }
        }
    }
    logf("rto select: the game's guide on the character select: %s", all.empty() ? "(no text found)" : all.c_str());
    const bool pad_used = !g_keys.pad_icon.empty() && all.find(g_keys.pad_icon) != std::string::npos;
    const bool key_used = !g_keys.key_icon.empty() && all.find(g_keys.key_icon) != std::string::npos;
    if (pad_used || key_used)
        logf("rto select: WARNING: the game's guide here already shows %s%s%s: the game may act on that press too (change "
             "select_options_button / select_options_key in settings.json)", pad_used ? g_keys.pad_icon.c_str() : "",
             pad_used && key_used ? " and " : "", key_used ? g_keys.key_icon.c_str() : "");
}

// ---- per frame ----
void side_tick(int s, bool net) {
    Side& st = g_s[s];
    // the panel of this side: the last agent of that name with a Param (the Color Mod's rule)
    void* buf[16] = {};
    const uint32_t na = A()->ui_agents(kAgentNames[s - 1], buf, 16);
    Obj* agent = nullptr;
    Obj* param = nullptr;
    for (uint32_t i = 0; i < na && i < 16; ++i)
        if (Obj* p = param_of(reinterpret_cast<Obj*>(buf[i]))) { agent = reinterpret_cast<Obj*>(buf[i]); param = p; }
    if (st.on) {
        if (!alive(st.agent) || !alive(st.param)) { drop(s, "the panel is gone"); return; }
        if (agent != st.agent) { leave(s, "the panel was rebuilt"); return; }
    }
    if (agent != st.agent || param != st.param) {
        st.agent = agent; st.param = param; st.was_active = false; st.active_since = 0;
    }
    bool ok = false;
    const bool active = param && call_bool(param, "IsActive", {}, &ok) && ok;
    if (active && !st.was_active) st.active_since = g_frame;
    st.was_active = active;
    if (!st.on && param) {                          // the outfit shown (its options: identified after a change only)
        const int f = ret_i32(param, "get_FighterId").value_or(-1), c = ret_i32(param, "GetCostumeId").value_or(-1);
        if (f != st.fighter || c != st.costume) { if (st.fighter != -1 || st.costume != -1) st.changed_at = g_frame; st.fighter = f; st.costume = c; }
    }
    const bool down = (active || st.on) && trigger_down(s);
    const bool press = down && !st.key_was;
    st.key_was = down;

    if (st.on) {
        sync_focus(st);
        Act a;
        for (int guard = 0; guard < 8 && pull(s, a); ++guard) {
            if (a.kind == Act::Leave) { leave(s, "Decide / Back"); publish(s, false); return; }
            if (a.kind == Act::Move) {
                st.w = panel_move(st.w, static_cast<int>(st.rows.size()), a.dir);
                focus_row(st, st.w.cur - st.w.win);
            } else {
                step(st, s, a.row, a.dir);
                if (st.rows.empty()) { leave(s, "no option row left"); publish(s, false); return; }
            }
        }
        // R1 / the key do nothing while the options show (Decide or Back leave); the guide corner offers nothing
        if (press) logf("rto select: P%d %s pressed while the options show: nothing (Decide / Back leave)", s, g_keys.pad_name.c_str());
        paint(st, s);
        publish(s, false);
        return;
    }

    // not on: the entry and the key
    uint32_t since = 0;
    const Slot* slot = active ? select_slot(s, &since) : nullptr;
    const bool settled = active && g_frame - st.active_since > kSettleFrames;
    if (active && !g_guide_logged && g_frame - st.active_since > 60) log_game_guide();
    const bool has = slot && since >= st.changed_at;
    const bool other_active = [&] {
        Side& o = g_s[3 - s];
        bool ok2 = false;
        return o.param && alive(o.param) && call_bool(o.param, "IsActive", {}, &ok2) && ok2;
    }();
    const char* why = nullptr;
    if (!color_mod_ok()) why = "the Color Mod loaded is older (no panel claim)";
    else if (!settled) why = "the panel is not settled";
    else if (!has) why = slot ? "the select model was not seen again since the outfit changed" : "this outfit has no options";
    else if (g_owner[s].load() == kColorMod || di_on(s)) why = "the Color Mod's DI mode holds this panel";
    else if (net && other_active) why = "online, both panels active: whose is the player's is not known";
    else if (SF6CORE_HAS(A(), opt_window_open) && A()->opt_window_open()) why = "an option window is open (the colour editor's C)";
    const bool on_row = settled && focused_row(agent).rfind("p_Spin_", 0) == 0;
    if (!why && !on_row) why = "the panel is not focused";
    publish(s, !why);
    if (!press) return;
    if (!why) {
        const char* err = nullptr;
        if (!enter(s, &err)) logf("rto select: P%d costume options not opened: %s", s, err ? err : "?");
        return;
    }
    logf("rto select: P%d %s pressed: %s", s, g_keys.pad_name.c_str(), why ? why : "the mode just closed");
}

} // namespace

void leave_all(const char* why) {
    for (int s = 1; s <= 2; ++s) {
        if (!g_s[s].on) { g_s[s] = Side{}; publish(s, false); continue; }
        if (alive(g_s[s].agent) && alive(g_s[s].param)) leave(s, why); else drop(s, why);
        g_s[s] = Side{};
        publish(s, false);
    }
    g_was_up = false;
    g_guide_logged = false;
}

void tick() {
    if (g_off || !A() || !C() || !SF6CORE_HAS(A(), ui_agents)) return;
    try {
        if (!g_was_up) {                            // the screen came: the keys read again (settings.json)
            g_was_up = true;
            read_keys();
            color_mod_check();
        }
        if (const uint32_t hf = g_hook_faults.exchange(0)) logf("rto select: %u exceptions in the hooks", hf);
        claims_tick();
        const bool net = costumes::online();
        for (int s = 1; s <= 2; ++s) side_tick(s, net);
    } catch (...) {
        ++g_faults;
        logf("rto select: EXCEPTION (%d)", g_faults);
        for (int s = 1; s <= 2; ++s) drop(s, "exception");
        if (g_faults >= 5) { g_off = true; logf("rto select: 5 faults, the costume options mode is off for this session"); }
    }
}

bool active(int side) { return side >= 1 && side <= 2 && g_s[side].on; }

int claim(int side, int who, int op) {
    if (side < 1 || side > 2) return 0;
    if (op < 0) return g_owner[side].load();
    if (who != kOurs && who != kColorMod) return 0;
    if (op == 0) { int want = who; g_owner[side].compare_exchange_strong(want, kFree); return 1; }
    int want = kFree;
    if (g_owner[side].compare_exchange_strong(want, who)) return 1;
    return want == who ? 1 : 0;
}

int guide_entry(int side, int pad, char* buf, int cap) {
    if (!buf || cap <= 0) return 0;
    buf[0] = 0;
    if (side < 1 || side > 2) return 0;
    std::lock_guard<std::mutex> lk(g_pub_mx);
    const Pub& p = g_pub[side];
    if (GetTickCount64() - p.ms > 500) return 0;
    const std::string& e = pad ? (p.pad.empty() ? p.key : p.pad) : (p.key.empty() ? p.pad : p.key);
    const int n = static_cast<int>(e.size());
    if (n == 0 || n >= cap) return 0;               // never a cut icon tag
    std::memcpy(buf, e.data(), static_cast<size_t>(n));
    buf[n] = 0;
    return n;
}

} // namespace slots::rto::select

// The owner of a side's panel on the character select (side 1 / 2), the one writer rule between the Slots Loader's
// costume options mode (who 1) and the Color Mod's DI mode (who 2), which borrow the same four rows. op 1: take it,
// 1 = granted (it was free, or already who's), 0 = the other one holds it; op 0: give it back (only who's), 1; op -1:
// the owner now (0 free, 1, 2). Cheap, any thread, no game call.
extern "C" __declspec(dllexport) int SF6Slots_SelectPanelClaim(int side, int who, int op) {
    try { return slots::rto::select::claim(side, who, op); } catch (...) { return 0; }
}

// The character select's guide entry of a side (1 / 2) for the Color Mod, which writes that side's guide corner
// ("X Edit Drive Colors") alone and adds this after its own: "<ICON BtnR1> Costume options" (pad != 0) or the keyboard
// key's glyph ("<ICON KeyE> Costume options" on AZERTY), UTF-8, NUL-terminated, copied into buf; its length, or 0
// (buf empty) when that side's outfit has no options, its panel is not ready or held by the DI mode, or cap is too
// small. Empty too while the side's panel shows the options (the key does nothing then; Decide / Back leave). Cheap, any
// thread, no game call.
extern "C" __declspec(dllexport) int SF6Slots_SelectGuideEntry(int side, int pad, char* buf, int cap) {
    try { return slots::rto::select::guide_entry(side, pad, buf, cap); } catch (...) { return 0; }
}
