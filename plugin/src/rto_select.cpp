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
// title (same parent, font, size and colour), its left edge 45 units (root, 1080p) left of the title's, on the title's
// line, anchored on the title's own point (the game puts a copy back at its title's place: a position written
// elsewhere flickered; a fixed region reaching left instead, arrow_place), drawn above the row's focus highlight
// (Priority), hollow while its row is not focused like the game's own row arrows; the title is never moved nor
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
//
// The Training pause menu's character popup ("P1 / P2 Character Select": agent GenericCharacterDetailSetting_FG, flow
// app.UIFlowCharacterDetailSetting; six rows: Costume, Color, Control Type, Button Preset, Button Release Input, Stick
// Sensitivity) gets the same mode (index kPop below), for the outfit its Costume row shows (its live
// UIPartsMatchingFighterSetting: FighterId, GetCostumeId; not necessarily the one worn in the fight) and its target
// side (Param SettingTargetPlayerIndex): R1 on any pad or the key (the pause menu answers whoever opened it; one panel,
// no other player's to tell apart), the six rows borrowed as the select's four (the Color Mod's popup DI mode: the
// part's mArrSpin / mGroup, its SetMessage* / Setup* skipped, GetCostumeId / GetColorId = what it had), the arrows'
// copies made in the popup flow's own update (app.UIFlowCharacterDetailSetting.Wait OnUpdate). A change there is
// PENDING: saved (costume_choices.json, the target side) only when the popup is confirmed (its Param's IsDecide); the
// popup closed without Confirm drops it. Every option file of the outfit is asked for when its options open
// (rto::popup_preload, kept while the popup is up). Confirmed with the same costume and colour (the game rebuilds
// nothing): the fighter keeps what it wears (rto::fighters_refresh with a hold side), the battle is restarted as the
// menu's Restart Battle does as soon as the pause menu has closed (TrainingManager.set_IsReqRefresh(true), offline
// Training only, once; not when the player restarted himself, nor when a Confirm of this pause changed the costume or
// colour: the game reloads then), and the swap is made in the restart's black screen (_TrainingState leaving NOW). Decide / Back leave the mode
// keeping the changes; the Esc that leaves it must not close the pause menu: the Color Mod's Esc guard covers this
// mode (SF6Slots_PopupGuard), so the popup mode needs a Color Mod that says it does (SF6ColorMod_PanelCaps bit 2: its popup DI mode takes the claim
// of the target side, its popup guide carries our entry, its guard asks ours); otherwise it stays off. The popup is
// found from an event (EV_POPUP: its MatchingFighterSetting set up or written, its flow's update), then followed each
// frame while it shows.
#include "rto.hpp"
#include "slots_ui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace slots::rto::select {
namespace {

constexpr int kPop = 3;                             // g_s / g_h index of the Training popup (1 / 2: the select's sides)
constexpr int kMaxRows = 6;                         // the popup's rows
constexpr const char* kRowNames[kPanelRows] = { "p_Spin_costume", "p_Spin_color", "p_Spin_type", "p_Spin_preset" };
constexpr const char* kPopupRows[kMaxRows] = { "p_Spin_costume", "p_Spin_color", "p_Spin_type", "p_Spin_preset",
                                               "p_Spin_disen", "p_Spin_stick" };
constexpr int kColorRow = 1;                        // the Color row: its value text is never written back (a fault)
constexpr const char* kAgentNames[2] = { "ui10505", "ui10507" };
constexpr const char* kPopupAgent = "GenericCharacterDetailSetting_FG";
// A panel: a side's panel of the select (its Param: Spins / Group) or the Training popup (its
// UIPartsMatchingFighterSetting: mArrSpin / mGroup, as the Color Mod's popup DI mode)
struct Kind { int rows; const char* const* names; const char* spins; const char* group; bool popup; };
constexpr Kind kSelectKind{ kPanelRows, kRowNames, "Spins", "Group", false };
constexpr Kind kPopupKind{ kMaxRows, kPopupRows, "mArrSpin", "mGroup", true };
const char* tag(int s) { return s == 1 ? "P1" : s == 2 ? "P2" : "popup"; }
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
    uint64_t param = 0, agent = 0, group = 0;       // param: the select's Param, the popup's MatchingFighterSetting
    uint64_t spin[kMaxRows] = {};
    int32_t ci = -1, co = -1;
    std::deque<Act> acts;
    uint32_t last_step = 0, last_move = 0;
};
std::mutex g_hmx;
HookSide g_h[kPop + 1];
std::atomic<bool> g_any{ false };
std::unordered_map<uint64_t, uint32_t> g_cancel;    // param -> frame until which IsDecide / IsCancel answer false
std::atomic<uint32_t> g_cancel_until{ 0 };          // the last frame of any cancel block (the gate's fast exit)
std::atomic<uint32_t> g_hook_faults{ 0 };
thread_local int64_t tl_ret = INT64_MIN;

bool acts(void* flag) { return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(flag) & 0xFFFFFFFF) & kActFlags) != 0; }
uint32_t u32_of(void* a) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFF); }

int side_by_spin(uint64_t a, int* row) {
    for (int s = 1; s <= kPop; ++s) {
        if (!g_h[s].active) continue;
        for (int k = 0; k < kMaxRows; ++k) if (g_h[s].spin[k] && g_h[s].spin[k] == a) { *row = k; return s; }
    }
    return 0;
}
int side_by_param(uint64_t a) { for (int s = 1; s <= kPop; ++s) if (g_h[s].active && g_h[s].param == a) return s; return 0; }
int side_by_agent(uint64_t a) { for (int s = 1; s <= kPop; ++s) if (g_h[s].active && g_h[s].agent == a) return s; return 0; }
int side_by_group(uint64_t a) { for (int s = 1; s <= kPop; ++s) if (g_h[s].active && g_h[s].group == a) return s; return 0; }
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

// ---- the arrows' copies, made inside the panel flow's own update (any thread; the frame asks, header) ----
constexpr const char* kArrowName = "e_text_slots_arrow";   // not "p_...": never taken for a UI part (R20)
constexpr int kArrowMaxRefused = 3;
struct CopyReq { uintptr_t agent = 0; uint32_t frame = 0; int last = 0; };   // last: the last row shown (its ▼)
std::mutex g_copy_mx;
CopyReq g_copy_req[kPop + 1];
std::atomic<bool> g_copy_on{ false };
// per kind of panel (0 the select's, 1 the popup): its flow's update hooked, seen, the copies made and refused, the
// arrows before the titles for the session (no copy can be made there)
struct CopyKind {
    std::atomic<int> refused{ 0 }, made{ 0 }, status{ 1000 };
    std::atomic<uint32_t> updates{ 0 };             // the flow's update seen (its post-hook ran)
    bool hook = false, off = false;
};
CopyKind g_ck[2];
CopyKind& ck(int s) { return g_ck[s == kPop ? 1 : 0]; }

void make_copies(int lo, int hi) {
    CopyReq req[kPop + 1];
    {
        std::lock_guard<std::mutex> lk(g_copy_mx);
        bool left = false;
        for (int s = 1; s <= kPop; ++s) {
            if (s < lo || s > hi) { left = left || g_copy_req[s].agent; continue; }
            if (g_copy_req[s].agent && g_frame - g_copy_req[s].frame <= 2) req[s] = g_copy_req[s];   // older: asked again
            g_copy_req[s] = CopyReq{};
        }
        g_copy_on.store(left, std::memory_order_release);
    }
    for (int s = lo; s <= hi; ++s) {
        Obj* agent = reinterpret_cast<Obj*>(req[s].agent);
        if (!agent || !alive(agent)) continue;
        const Kind& kd = s == kPop ? kPopupKind : kSelectKind;
        CopyKind& c_k = ck(s);
        void* cm = call_obj(agent, "get_ControlMain");
        for (int k : { 0, req[s].last }) {           // ▲ on the first row, ▼ on the last shown
            if (k < 0 || k >= kd.rows) continue;
            void* row = cm ? A()->ui_find_child(cm, kd.names[k], 8) : nullptr;
            void* title = row ? A()->ui_find_child(row, "e_text_title", 4) : nullptr;
            Obj* parent = title ? call_obj(reinterpret_cast<Obj*>(title), "get_Parent") : nullptr;
            if (!parent) { ++c_k.refused; c_k.status.store(-3000, std::memory_order_release); continue; }
            if (A()->ui_find_child(parent, kArrowName, 0)) continue;
            void* c = nullptr;
            const SF6CoreStatus st = A()->ui_clone_control(C(), title, parent, kArrowName, &c);
            c_k.status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st), std::memory_order_release);
            if (st != SF6CORE_OK || !c) { ++c_k.refused; continue; }
            ++c_k.made;
            A()->ui_set_visible(C(), c, 0);                 // placed and shown by the frame
            A()->ui_set_force_invisible(C(), c, 1);
        }
    }
}
void post_flow_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return;
    g_ck[0].updates.fetch_add(1, std::memory_order_relaxed);
    if (!g_copy_on.load(std::memory_order_acquire)) return;
    try { make_copies(1, 2); } catch (...) { ++g_ck[0].refused; g_ck[0].status.store(-2000, std::memory_order_release); }   // /EHa: SEH too
}
// The popup flow's own update: the popup is up (EV_POPUP keeps it followed), and the arrows' copies of the popup
void post_popup_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return;
    g_ck[1].updates.fetch_add(1, std::memory_order_relaxed);
    post(EV_POPUP);
    if (!g_copy_on.load(std::memory_order_acquire)) return;
    try { make_copies(kPop, kPop); } catch (...) { ++g_ck[1].refused; g_ck[1].status.store(-2000, std::memory_order_release); }
}
// What else tells the popup is up: its MatchingFighterSetting set up or written (Fighter Settings' too: the popup is then
// not found, nothing more)
int pre_popup_event(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_closing.load(std::memory_order_relaxed)) post(EV_POPUP);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// A type's own method of that name (never a parent's: a flow's parent runs for every flow of the game)
Method* own_method(TD* td, const char* name) {
    if (td) for (auto* m : td->get_methods()) if (m && m->get_name() && std::strcmp(m->get_name(), name) == 0) return m;
    return nullptr;
}

// Installed the first time a side is borrowed (a player who never uses the mode pays nothing): the DI mode's set
bool g_hooks_tried = false, g_hooks_ok = false;
bool g_mfs_ok = false;                              // the popup's MatchingFighterSetting hooked: the popup mode can start
void install_hooks() {
    g_hooks_tried = true;
    auto* tdb = api().tdb();
    int n = 0, missing = 0;
    auto hook = [&](TD* td, const char* name, REFPreHookFn pre, REFPostHookFn post) {
        Method* m = td ? td->find_method(name) : nullptr;
        if (m && m->add_hook(pre, post ? post : hook_post_pass, false)) { ++n; return true; }
        ++missing;
        return false;
    };
    TD* spin = tdb->find_type("app.UIPartsSpin");
    struct H { const char* name; REFPreHookFn pre; };
    int n_input = 0;                                // the spin / agent / group set: both modes need it
    for (const H& h : { H{ "Add", pre_spin_step<1, false> }, H{ "AddNum", pre_spin_step<1, false> }, H{ "MouseAdd", pre_spin_step<1, false> },
                        H{ "Sub", pre_spin_step<-1, false> }, H{ "SubNum", pre_spin_step<-1, false> }, H{ "MouseSub", pre_spin_step<-1, false> },
                        H{ "InputRight", pre_spin_step<1, true> }, H{ "InputLeft", pre_spin_step<-1, true> }, H{ "InputDirection", pre_spin_dir } })
        n_input += hook(spin, h.name, h.pre, nullptr) ? 1 : 0;
    TD* param = tdb->find_type("app.UIFlowUI10505.Param");
    for (const char* nm : { "SelectChangeCostume", "SelectChangeColorChip", "SelectChangeControllerType", "SetMessageCostume",
                            "SetMessageColor", "SetMessagePreset", "SetMessageList", "setCostume", "setColor", "setCostumeAndColor" })
        hook(param, nm, pre_param_skip, nullptr);
    for (const char* nm : { "IsDecide", "IsCancel", "isSelectOperationType" }) hook(param, nm, pre_param_gate, post_ret);
    hook(param, "getCostume", pre_param_get<true>, post_ret);
    hook(param, "getColor", pre_param_get<false>, post_ret);
    TD* agent = tdb->find_type("app.UIAgent");
    for (const H& h : { H{ "InputLeft", pre_agent_lr<-1> }, H{ "InputRight", pre_agent_lr<1> }, H{ "InputUp", pre_agent_ud<-1> },
                        H{ "InputDown", pre_agent_ud<1> }, H{ "InputDirection", pre_agent_dir }, H{ "InputDecide", pre_agent_leave },
                        H{ "InputCancel", pre_agent_leave } })
        n_input += hook(agent, h.name, h.pre, nullptr) ? 1 : 0;
    n_input += hook(tdb->find_type("app.UIPartsGroup"), "MoveFocus", pre_group_move, nullptr) ? 1 : 0;
    // the Training popup's part (the Color Mod's popup DI mode set): its own repaints skipped, its getters answer what
    // it had; the popup's Param is not a method gate (its IsDecide is a field: our Decide never reaches the flow)
    TD* mfs = tdb->find_type("app.UIPartsMatchingFighterSetting");
    int n_mfs = 0;
    for (const char* nm : { "SelectChangeCostume", "SelectChangeColorChip", "SelectChangeControllerType", "SelectChangePresetIndex",
                            "SetMessage", "SetMessageColor", "SetupSpin", "SetupPresetSpin", "SetupPresetSpinCursor",
                            "SelectionChanged", "SelectionChanged_Spin", "ChangeEverything" })
        if (hook(mfs, nm, pre_param_skip, nullptr)) ++n_mfs;
    const bool getters = hook(mfs, "GetCostumeId", pre_param_get<true>, post_ret) && hook(mfs, "GetColorId", pre_param_get<false>, post_ret);
    // the steps, the moves and the way out are what the mode needs; without them it never starts
    g_hooks_ok = spin && param && agent && n >= 20;
    g_mfs_ok = spin && agent && mfs && getters && n_mfs >= 8 && n_input >= 12;
    logf("rto select: %d hooks installed, %d not found: the costume options mode on the character select %s, in the "
         "Training popup %s (%d of its part's hooks)", n, missing, g_hooks_ok ? "is ready" : "is off",
         g_mfs_ok ? "is ready" : "is off", n_mfs + (getters ? 2 : 0));
    // the arrows' copies (R19: inside the select flow's own update); without it the arrows go before the titles
    Method* upd = own_method(tdb->find_type("app.UIFlowUI10505.CharaSetting"), "OnUpdate");
    g_ck[0].hook = SF6CORE_HAS(A(), ui_clone_control) && upd && upd->get_function_raw() && upd->add_hook(hook_pass, post_flow_update, false);
    logf("rto select: arrows %s", g_ck[0].hook ? "in the rows' left margin (copies made in app.UIFlowUI10505.CharaSetting.OnUpdate)"
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
struct RowParts { void* ctrl = nullptr; void* title = nullptr; void* text = nullptr; void* list = nullptr; void* chip = nullptr;
                  void* rect0 = nullptr; void* rect1 = nullptr; void* arrow[2] = {}; };
RowParts parts_of(void* cm, const Kind& kd, int k) {
    RowParts p;
    if (k < 0 || k >= kd.rows) return p;
    p.ctrl = find(cm, kd.names[k]);
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
    int claim = 0;                                  // the claim held (SF6Slots_SelectPanelClaim side: 1 / 2), 0 none
    std::vector<Row> rows;
    std::vector<std::string> chosen, shown;
    PanelWindow w;
    int32_t ci = -1, co = -1;
    RowSave rs[kMaxRows];
    float ref_pos[3] = {}; bool has_ref_pos = false;
    float ref_region[2] = { 300, 0 }; bool has_ref_region = false;
    uint32_t entered_at = 0;
    // the arrows (on the first and the last row shown): where each copy goes, read once a visit (the glyph's width until it is drawn), its
    // anchored layout written every frame while shown (the same values the game puts back: never a fight)
    struct Arrow {
        bool geo = false, failed = false, styled = false, shown = false, measured = false, checked = false, laid = false;
        uint32_t shown_at = 0, text_at = 0;
        int frames = 0, resets = 0;                 // frames laid out, frames its layout was found changed by the game
        float x = 0, y = 0, z = 0;                  // the title's place (its parent's space)
        float region_h = 0;                         // the title's region height
        float rw = 0;                               // the region width last written (the copy's units)
        int cp = 0, prio = -1, row_prio = 0;
        double title_l = 0, title_lg = 0, title_gx = 0, row_lg = 0, k = 1, gsx = 1, glyph = 0;
        std::string text, row_el, prio_el;
    } arrow[2];
    uint32_t arrow_asked = 0;                       // the first frame a copy was asked for and not found since
    // the panel's kind and how many of its rows are borrowed (the select: 4; the popup: its six, fewer when the
    // last ones are missing)
    const Kind* kind = &kSelectKind;
    int n = kPanelRows;
    int found = kPanelRows;                         // the rows found (n .. found - 1: left out, hidden while on)
};
Side g_s[kPop + 1];
// The rows of the panel: the screen row of a row name, -1 if not one of ours
int row_index(const Side& st, const std::string& nm) {
    for (int k = 0; k < st.n; ++k) if (nm == st.kind->names[k]) return k;
    return -1;
}
// A side reset, its panel still followed (agent, param, the outfit it showed, the key's state)
Side follow(int s, const Side& old) {
    Side n;
    n.agent = old.agent; n.param = old.param; n.was_active = old.was_active; n.active_since = old.active_since;
    n.fighter = old.fighter; n.costume = old.costume; n.changed_at = old.changed_at; n.key_was = old.key_was;
    if (s == kPop) { n.kind = &kPopupKind; n.n = n.found = kMaxRows; }
    return n;
}
bool g_was_up = false;                              // the character select was up last frame
int g_faults = 0;
bool g_off = false;
int g_hollow = -1;                                  // the hollow glyphs drawn by the game's font: -1 unknown, 0 no, 1 yes

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
// The button / key of one player: app.InputManager's own per-player reading (the Color Mod's X rule), else any device.
// side 0: any player (the Training popup: the pause menu answers whoever opened it, and it has one panel only)
bool trigger_down(int side) {
    if (!game_has_focus()) return false;
    const bool key = g_keys.vk && (GetAsyncKeyState(g_keys.vk) & 0x8000) != 0;
    if (side == 0 && key) return true;
    if (Obj* im = reinterpret_cast<Obj*>(api().get_managed_singleton("app.InputManager"))) {
        static Method* m_flags = nullptr;
        static Method* m_kb = nullptr;
        if (!m_flags) m_flags = method_of(im->get_type_definition(), "GetGamePadRawFlags", 2);
        if (!m_kb) m_kb = method_of(im->get_type_definition(), "GetKeyboardMousePlayerIndex", 0);
        if (m_flags) {
            if (g_keys.pad_bit)
                for (int p = side ? side - 1 : 0; p <= (side ? side - 1 : 1); ++p) {
                    auto r = call(m_flags, im, { reinterpret_cast<void*>(static_cast<uintptr_t>(g_keys.pad_bit)), arg_i32(p) });
                    if (!r.exception_thrown && (r.dword & 1)) return true;
                }
            if (side) {
                auto kb = call(m_kb, im);
                if (!kb.exception_thrown) return key && static_cast<int32_t>(kb.dword) == side - 1;
            }
        }
    }
    return key || (g_keys.pad_bit && (any_pad_mask() & g_keys.pad_bit) != 0);
}

// ---- the Color Mod: its DI mode borrows the same panel ----
using DiActiveFn = int (*)(int side);
using CapsFn = int (*)();
DiActiveFn g_cm_di = nullptr;
int g_cm_caps = 0;                                  // SF6ColorMod_PanelCaps: 1 the select's claim, 2 the popup's (below)
bool g_cm_checked = false, g_cm_loaded = false;
uint32_t g_cm_stale_since[3] = {};
void color_mod_check() {
    if (g_cm_checked) return;
    g_cm_checked = true;
    HMODULE h = GetModuleHandleW(L"SF6_ColorModNative.dll");
    g_cm_loaded = h != nullptr;
    g_cm_di = h ? reinterpret_cast<DiActiveFn>(GetProcAddress(h, "SF6ColorMod_DiPanelActive")) : nullptr;
    const CapsFn caps = h ? reinterpret_cast<CapsFn>(GetProcAddress(h, "SF6ColorMod_PanelCaps")) : nullptr;
    g_cm_caps = caps ? caps() : (g_cm_di ? 1 : 0);
    logf("rto select: Color Mod %s", !g_cm_loaded ? "not loaded" : g_cm_di
         ? "loaded, its DI mode takes the panel's claim: one owner per panel"
         : "loaded but older (no SF6ColorMod_DiPanelActive): costume options on the character select are OFF (its DI mode "
           "borrows the same rows; two writers on one panel crashed the game)");
    logf("rto select: costume options in the Training popup %s", (g_cm_caps & 2)
         ? "on (the Color Mod's popup DI mode takes the claim, its guide and Esc guard cover them)"
         : !g_cm_loaded ? "OFF: they need the Color Mod (its Esc guard keeps the pause menu open when Back leaves them)"
         : "OFF: the Color Mod loaded does not share the popup (no SF6ColorMod_PanelCaps bit 2: its DI mode borrows the "
           "same rows without the claim)");
}
bool color_mod_ok() { color_mod_check(); return !g_cm_loaded || g_cm_di; }
bool popup_cm_ok() { color_mod_check(); return g_cm_loaded && (g_cm_caps & 2) != 0; }
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
    for (int k = 0; k < st.n; ++k) h.spin[k] = reinterpret_cast<uint64_t>(st.rs[k].spin);
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
    g_any.store(g_h[1].active || g_h[2].active || g_h[kPop].active);
    for (auto it = g_cancel.begin(); it != g_cancel.end();) it = g_frame >= it->second + 600 ? g_cancel.erase(it) : std::next(it);
}
bool pull(int s, Act& a) {
    std::lock_guard<std::mutex> lk(g_hmx);
    if (g_h[s].acts.empty()) return false;
    a = g_h[s].acts.front();
    g_h[s].acts.pop_front();
    return true;
}

void release(int claim) { if (claim < 1 || claim > 2) return; int want = kOurs; g_owner[claim].compare_exchange_strong(want, kFree); }

// ---- the Training popup: this visit (the pending changes) and the Esc guard (SF6Slots_PopupGuard, any thread) ----
struct Pending { const Slot* slot = nullptr; int opt = 0; std::vector<std::string> shown, chosen; };
struct Popup {
    Obj* agent = nullptr;                           // GenericCharacterDetailSetting_FG (never add_ref'd, R21)
    Obj* flow = nullptr;                            // its flow's Param (SettingTargetPlayerIndex, IsDecide, MatchingFighterSetting)
    bool up = false;
    uint32_t since = 0;                             // the frame this visit started
    int target = 1;                                 // the side it sets: 1 P1, 2 P2
    bool decided = false;
    std::map<std::string, Pending> pending;         // mod key -> the options changed this visit, saved on Confirm
    bool base = false;                              // the fighter / costume / colour it showed when it opened (read once)
    int32_t bf = -1, bc = -1, bn = -1;
    std::vector<const Slot*> preloaded;             // the outfits whose option files are asked for ahead (rto::popup_preload)
    int left = -1;                                  // the pending options' files not ready yet, last count (-1 unknown)
};
Popup g_pop;
// After an options-only Confirm in the popup: the fighter's swap held, the training restart asked for as soon as the
// pause menu closes (the pause menu's own "Restart Battle": set_IsReqRefresh), the swap released on its black screen
struct PauseWatch {
    bool on = false, hold = false, restart = false;
    bool player = false;                            // the player's own Restart Battle seen (MenuState 1 / 2)
    bool reload = false;                            // a Confirm of this pause changed the costume / colour: the game reloads
    bool retried = false, released = false;
    int state = -2, ts = -2;                        // the menu's and the training's states last seen
    uint32_t since = 0, closed_at = 0, req_at = 0, released_at = 0;
};
PauseWatch g_pw;
uint32_t g_pop_event = 0;                           // the last EV_POPUP
std::atomic<bool> g_pop_on{ false };
std::atomic<uint32_t> g_pop_left{ 0 };              // the frame the popup's mode was left
constexpr uint32_t kPopGuard = 15;                  // frames after leaving during which the Esc guard still holds
void pop_flag(int s, bool on) {
    if (s != kPop) return;
    g_pop_on.store(on);
    if (!on) g_pop_left.store(g_frame);
}

// The mode dropped without a write (the panel is gone)
void drop(int s, const char* why) {
    if (!g_s[s].on) return;
    hook_side_clear(s);
    release(g_s[s].claim);
    pop_flag(s, false);
    Side n = follow(s, g_s[s]);
    n.agent = nullptr; n.param = nullptr; n.was_active = false; n.active_since = 0; n.key_was = false;
    g_s[s] = std::move(n);
    logf("rto select: %s costume options mode dropped (%s)", tag(s), why);
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
// The popup: kept for the Confirm (shown: the set its saved choices gave when first edited this visit, for keep_hidden)
void pend(const Side& st) {
    Pending& p = g_pop.pending[st.slot->mod];
    if (!p.slot) { p.slot = st.slot; p.opt = st.opt; p.shown = st.shown; }
    p.chosen = st.chosen;
}

CopyKind& ck_of(const Side& st) { return g_ck[st.kind->popup ? 1 : 0]; }
std::string row_title(const Side& st, int k) {
    const int i = st.w.win + k;
    // the arrow is a copy in the row's left margin (paint_arrows); in the title only when no copy can be made
    const char* a = ck_of(st).off ? panel_arrow(k, st.w.win, static_cast<int>(st.rows.size()), st.n) : "";
    return std::string(a) + clip_name(st.rows[static_cast<size_t>(i)].title, kTitleChars);
}

// ---- the arrows: copies of a row's title in its left margin (header) ----
constexpr double kArrowOffset = 45;                 // the arrow's left edge, left of the title's (root units, 1080p)
constexpr double kArrowGap = 8;                     // at least this between the glyph and the title
constexpr uint32_t kArrowWait = 60;                 // frames a copy may be waited for, then the arrows go in the titles
constexpr uint32_t kArrowCheck = 3;                 // frames shown before where it is drawn is read (logged once a visit)
constexpr uint16_t kArrowAbove = 10;                // its draw priority at least this (the Color Mod's grid copies: 10)

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
// The row's visible frame, for the log only: the leftmost left edge (root space) of the row's visible elements drawn
// as a shape (not a text, at least 40 wide), the glow left out (e_tex_glow reaches off the panel: x -32 for a frame
// drawn from x 28, 20/10). NAN when none is found
void outline_walk(Obj* o, int depth, int& budget, double& best, std::string& what) {
    for (; o && budget > 0; o = call_obj(o, "get_Next")) {
        --budget;
        if (!shown_el(o)) continue;
        const std::string nm = str(call_obj(o, "get_Name"));
        if (nm == kArrowName) continue;
        std::string low = nm;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (type_name(o) != "via.gui.Text" && low.find("glow") == std::string::npos) {
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
// The highest draw priority (set_Priority) of the row's elements, shown or not (the focus highlight shows on the focused
// row only), the arrow's copy left out
void prio_walk(Obj* o, int depth, int& budget, int& best, std::string& what) {
    for (; o && budget > 0; o = call_obj(o, "get_Next")) {
        --budget;
        const std::string nm = str(call_obj(o, "get_Name"));
        if (nm == kArrowName) continue;
        const Ret r = call(o, "get_Priority");
        const int p = r.exception_thrown ? -1 : static_cast<int>(r.dword & 0xFFFF);
        if (p > best) { best = p; what = nm; }
        if (depth < 5) prio_walk(call_obj(o, "get_Child"), depth + 1, budget, best, what);
    }
}
// Where the arrow goes, read once a visit: the title's place, point and left edge (root space), its region's height
bool arrow_geo(Side::Arrow& ar, void* row, void* title) {
    Obj* t = reinterpret_cast<Obj*>(title);
    float p[3] = {}, sc[3] = { 1, 1, 1 }, gp[3] = {}, gs[3] = { 1, 1, 1 }, dw[2] = {}, fs[2] = {}, rs[2] = {};
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
    ar.title_gx = gp[0];
    ar.region_h = ret_floats(t, "get_RegionSize", rs, 2) && std::isfinite(rs[1]) && rs[1] > 0 ? rs[1] : 0.0f;
    ar.glyph = (ret_floats(t, "get_FontSize", fs, 2) && std::isfinite(fs[0]) && fs[0] > 0 ? fs[0] : 24.0) * std::fabs(gs[0]);
    double best = NAN;
    std::string what;
    int budget = 96;
    outline_walk(call_obj(reinterpret_cast<Obj*>(row), "get_Child"), 0, budget, best, what);
    ar.row_lg = std::isnan(best) ? 0 : best;
    ar.row_el = std::isnan(best) ? std::string("none found") : what;
    int top = 0;
    budget = 96;
    ar.prio_el = "none";
    prio_walk(call_obj(reinterpret_cast<Obj*>(row), "get_Child"), 0, budget, top, ar.prio_el);
    ar.row_prio = top;
    ar.geo = true;
    return true;
}
// via.gui.PageAlignment's value for the same row with its left point (the enum read by name from the game's types:
// named like the control points, LeftTop .. RightBottom); -1 when not found
int64_t page_align_left(int64_t cur, std::string* name) {
    static TD* pa = api().tdb()->find_type("via.gui.PageAlignment");
    if (!pa) return -1;
    std::unordered_map<std::string, int64_t> by_name;
    std::string cur_name;
    for (auto* f : pa->get_fields()) {
        if (!f || !f->get_name() || !f->is_static()) continue;
        void* p = f->get_data_raw(nullptr, false);
        if (!p) continue;
        const int64_t v = *static_cast<int32_t*>(p);
        by_name[f->get_name()] = v;
        if (v == cur && cur_name.empty()) cur_name = f->get_name();
    }
    if (cur_name.empty()) return -1;
    std::string want = cur_name;
    if (want.rfind("Center", 0) == 0) want = "Left" + want.substr(6);
    else if (want.rfind("Right", 0) == 0) want = "Left" + want.substr(5);
    auto it = by_name.find(want);
    if (it == by_name.end()) return -1;
    *name = cur_name + " -> " + want;
    return it->second;
}
void set_cp(void* c, int cp) {
    if (cp_of(reinterpret_cast<Obj*>(c)) == cp) return;
    SF6CoreArg pc{}; pc.type = SF6CORE_ARG_I64; pc.v.i = cp;
    A()->obj_invoke(C(), c, "set_ControlPoint(via.gui.ControlPoint)", &pc, 1, nullptr);
}
// Its settings, once a visit: no text fitting, the left alignment of the title's row (the glyph drawn at the region's
// left), drawn above every element of its row and above the row's focus highlight (Priority, at least kArrowAbove)
void arrow_style(void* c, Side::Arrow& ar, int s, int k) {
    ar.styled = true;
    SF6CoreArg fit{}; fit.type = SF6CORE_ARG_BOOL; fit.v.i = 0;
    A()->obj_invoke(C(), c, "set_AutoRegionFit(System.Boolean)", &fit, 1, nullptr);
    const Ret r = call(reinterpret_cast<Obj*>(c), "get_PageAlignment");
    const int64_t cur = r.exception_thrown ? -1 : static_cast<int32_t>(r.dword);
    std::string how;
    const int64_t left = cur < 0 ? -1 : page_align_left(cur, &how);
    if (left >= 0 && left != cur) {
        SF6CoreArg pa{}; pa.type = SF6CORE_ARG_I64; pa.v.i = left;
        A()->obj_invoke(C(), c, "set_PageAlignment(via.gui.PageAlignment)", &pa, 1, nullptr);
    }
    const Ret rp = call(reinterpret_cast<Obj*>(c), "get_Priority");
    const int had = rp.exception_thrown ? -1 : static_cast<int>(rp.dword & 0xFFFF);
    ar.prio = std::min(0xFFFF, std::max<int>(kArrowAbove, ar.row_prio + 1));
    if (had != ar.prio) {
        SF6CoreArg pr{}; pr.type = SF6CORE_ARG_I64; pr.v.i = ar.prio;
        A()->obj_invoke(C(), c, "set_Priority(System.UInt16)", &pr, 1, nullptr);
    }
    logf("rto select: %s arrow row %d: anchored, page alignment %s, priority %d (was %d; row's highest %d, %s)", tag(s), k,
         left < 0 ? "unread (the title's kept)" : how.c_str(), ar.prio, had, ar.row_prio, ar.prio_el.c_str());
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
// The arrow laid out, every frame while it shows: ANCHORED on its title. The copy inherits the title's layout and
// animation (R31): the game puts its position back at the title's place (20/10 19:59, placed six times then left
// alone: drawn on the titles' first letters), and a position written every frame elsewhere flickered (20/10 night:
// our write and the game's alternate, whatever their order in a frame). So the position is never moved off the
// title's own point: the game's write and ours are the same value, there is nothing to alternate. The arrow's place
// comes from what the game does not lay out: the control point on the right of the title's row, a fixed region
// reaching from that point left to the arrow's place, the glyph drawn at the region's left (arrow_style). A control
// point, region or position found changed by the game before our write is counted (logged once a visit).
void arrow_place(void* c, Side::Arrow& ar, void* title, int s, int k, bool focused) {
    const double left = panel_arrow_left(ar.title_lg, ar.glyph, kArrowOffset, kArrowGap);
    const int cp = (ar.cp / 4) * 4 + 2;                                            // the title's row, its right point
    Obj* co = reinterpret_cast<Obj*>(c);
    float tp[3] = {};                                                              // the title's place now
    if (!ret_floats(reinterpret_cast<Obj*>(title), "get_Position", tp, 3)) { tp[0] = ar.x; tp[1] = ar.y; tp[2] = ar.z; }
    const float w = static_cast<float>(std::max(1.0, (ar.title_gx - left) / std::max(1e-3, std::fabs(ar.gsx))));   // copy units
    const float h = ar.region_h > 0 ? ar.region_h : 40.0f;
    if (ar.laid) {
        float pos[3] = {}, rs[2] = {};
        const bool moved = ret_floats(co, "get_Position", pos, 3) && (std::fabs(pos[0] - tp[0]) > 0.5f || std::fabs(pos[1] - tp[1]) > 0.5f);
        const bool region = ret_floats(co, "get_RegionSize", rs, 2) && std::fabs(rs[0] - ar.rw) > 0.5f;
        if (moved || region || cp_of(co) != cp) ++ar.resets;
    }
    set_cp(c, cp);
    A()->ui_set_region_size(C(), c, w, h);                                         // compared first by the core
    A()->ui_set_position(C(), c, tp[0], tp[1], tp[2]);                             // compared first by the core
    ar.laid = true; ar.rw = w;
    ++ar.frames;
    // where it is drawn: read once a visit, a few frames after it shows
    if (ar.shown && !ar.checked && g_frame - ar.shown_at >= kArrowCheck) {
        ar.checked = true;
        float gp[3] = {};
        uint32_t tc = 0, cc = 0;
        const Ret rt = call(reinterpret_cast<Obj*>(title), "get_Color"), rc = call(co, "get_Color");
        if (!rt.exception_thrown) std::memcpy(&tc, rt.bytes.data(), 4);
        if (!rc.exception_thrown) std::memcpy(&cc, rc.bytes.data(), 4);
        char at[48] = "?";
        if (ret_floats(co, "get_GlobalPosition", gp, 2))
            std::snprintf(at, sizeof at, "%.0f .. %.0f", gp[0] - w * std::fabs(ar.gsx), gp[0]);
        logf("rto select: %s arrow row %d: drawn at x %s, glyph at %.0f (title x %.0f - %.0f, glyph %.0f wide %s; row frame "
             "x %.0f %s, for reference; anchored, priority %d; colour %08x, title's %08x, row %s; its layout found changed "
             "by the game on %d of %d frames)", tag(s), k, at, left, ar.title_lg, kArrowOffset, ar.glyph,
             ar.measured ? "drawn" : "font size", ar.row_lg, ar.row_el.c_str(), ar.prio, cc, tc,
             focused ? "focused" : "not focused", ar.resets, ar.frames);
    }
}
// Each frame while on: each arrow row's copy shown with its glyph (or hidden) and laid out, asked for when missing.
// The glyph follows the game's own arrows on the rows: hollow (U+25B3 / U+25BD) while its row is not focused, filled
// (U+25B2 / U+25BC) on the focused row; filled only when the game's font does not draw the hollow ones (measured
// once, logged once)
void paint_arrows(Side& st, int s, void* cm) {
    CopyKind& c_k = ck_of(st);
    const int arrow_rows[2] = { 0, st.n - 1 };      // ▲ on the first row, ▼ on the last shown
    if (c_k.off) {                                  // a copy already shown on the other row goes too
        for (int a = 0; a < 2; ++a)
            if (st.arrow[a].shown) if (void* c = arrow_copy(parts_of(cm, *st.kind, arrow_rows[a]).title)) arrow_hide(c, st.arrow[a]);
        return;
    }
    const int n = static_cast<int>(st.rows.size());
    const int focus = st.w.cur - st.w.win;          // the focused screen row
    bool missing = false;
    for (int a = 0; a < 2; ++a) {
        Side::Arrow& ar = st.arrow[a];
        const int k = arrow_rows[a];
        RowParts p = parts_of(cm, *st.kind, k);
        const bool hollow = k != focus && g_hollow != 0;
        std::string glyph = panel_arrow(k, st.w.win, n, st.n, hollow);
        while (!glyph.empty() && glyph.back() == ' ') glyph.pop_back();
        void* c = arrow_copy(p.title);
        if (!c) { if (!glyph.empty()) missing = true; continue; }
        if (glyph.empty() || ar.failed) { arrow_hide(c, ar); continue; }
        if (!ar.geo && !arrow_geo(ar, p.ctrl, p.title)) { ar.failed = true; logf("rto select: %s arrow row %d: the title's place unread, no arrow", tag(s), k); continue; }
        if (!ar.styled) arrow_style(c, ar, s, k);
        // written only when it changes (never an identical set_Message; the core compares too)
        if (glyph != ar.text && A()->ui_set_text(C(), c, glyph.c_str()) == SF6CORE_OK) { ar.text = glyph; ar.text_at = g_frame; ar.measured = false; }
        // the glyph's drawn width, read once drawn: shown, two frames after its text changed
        if (ar.shown && !ar.measured && g_frame - ar.text_at >= 2 && g_frame - ar.shown_at >= 2) {
            float dw[2] = {};
            if (ret_floats(reinterpret_cast<Obj*>(c), "get_DisplaySize", dw, 2) && std::isfinite(dw[0]) && dw[0] >= 0) {
                ar.measured = true;
                const bool drawn = dw[0] > 0.5f;
                // the region's width read back: the text's size is not readable here, nothing learnt (never a glyph
                // as wide as the region: the region would grow with it)
                const bool region = ar.laid && std::fabs(dw[0] - ar.rw) <= 0.5f;
                if (drawn && !region) ar.glyph = dw[0] * std::fabs(ar.gsx);
                if (hollow && g_hollow < 0) {
                    g_hollow = drawn ? 1 : 0;
                    logf(!drawn ? "rto select: the game's font does not draw the hollow arrows (U+25B3 / U+25BD, %.0f wide): "
                                  "the arrows stay filled"
                         : region ? "rto select: the hollow arrows (U+25B3 / U+25BD) kept, their width unread (%.0f = the region)"
                                  : "rto select: the hollow arrows (U+25B3 / U+25BD) are drawn by the game's font (%.0f wide)",
                         dw[0] * std::fabs(ar.gsx));
                }
            }
        }
        arrow_place(c, ar, p.title, s, k, k == focus);
        if (!ar.shown) {
            A()->ui_set_force_invisible(C(), c, 0);     // the copy's lock, put on when it was made
            A()->ui_set_visible(C(), c, 1);
            ar.shown = true;
            ar.shown_at = g_frame;
        }
    }
    if (!missing) { st.arrow_asked = 0; return; }
    if (!st.arrow_asked) st.arrow_asked = g_frame;
    const int refused = c_k.refused.load();
    if (!c_k.hook || refused >= kArrowMaxRefused || g_frame - st.arrow_asked > kArrowWait) {
        c_k.off = true;
        logf("rto select: %sarrows before the titles from now on (%s; copies made %d, refused %d, last status %d, flow updates seen %u)",
             st.kind->popup ? "popup: " : "", !c_k.hook ? "no hook" : refused >= kArrowMaxRefused ? "copies refused" : "no copy within a second",
             c_k.made.load(), refused, c_k.status.load(), c_k.updates.load());
        return;
    }
    std::lock_guard<std::mutex> lk(g_copy_mx);
    g_copy_req[s] = { reinterpret_cast<uintptr_t>(st.agent), g_frame, st.n - 1 };
    g_copy_on.store(true, std::memory_order_release);
}
// Leaving: both copies hidden (and locked)
void hide_arrows(Side& st, void* cm) {
    const int arrow_rows[2] = { 0, st.n - 1 };
    for (int a = 0; a < 2; ++a) {
        RowParts p = parts_of(cm, *st.kind, arrow_rows[a]);
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
    for (int k = 0; k < st.n; ++k) {
        RowParts p = parts_of(cm, *st.kind, k);
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
    for (int k = st.n; k < st.found; ++k) vis(parts_of(cm, *st.kind, k).ctrl, false);   // a row left out (no spin): hidden
    paint_arrows(st, s, cm);
}

// The focus row the game shows -> the cursor (a mouse hover moves the focus without our hooks)
void sync_focus(Side& st) {
    const int k = row_index(st, focused_row(st.agent));
    if (k < 0) return;
    const PanelWindow w = panel_focus(st.w, static_cast<int>(st.rows.size()), k, st.n);
    if (w.cur != st.w.cur) st.w = w;
}
void focus_row(Side& st, int k) {
    SF6CoreArg out{};
    if (A()->obj_invoke(C(), st.group, "GetFocusIndex", nullptr, 0, &out) == SF6CORE_OK && out.v.i == k) return;
    inv_i(st.group, "ChangeFocusIndex", k);
}

// The popup's spin of a row: the one whose control is that row (its own index first), nullptr when none is; the k-th
// when no spin's control name can be read (the Color Mod's popup DI mode takes them by index)
Obj* spin_of_row(Obj* spins, const char* row, int k) {
    bool named = false;
    auto is = [&](Obj* sp) {
        Obj* c = sp ? get_obj(sp, "<Control>k__BackingField") : nullptr;
        const std::string nm = c ? str(call_obj(c, "get_Name")) : std::string();
        named = named || !nm.empty();
        return nm == row;
    };
    if (Obj* sp = list_item(spins, k); is(sp)) return sp;
    for (int i = 0, cnt = list_count(spins); i < cnt; ++i) if (Obj* sp = list_item(spins, i); is(sp)) return sp;
    return named ? nullptr : list_item(spins, k);
}

bool enter(int s, const char** why) {
    Side& st = g_s[s];
    const bool pop = s == kPop;
    const Kind& kd = pop ? kPopupKind : kSelectKind;
    const Slot* slot = nullptr;
    if (pop) {                                      // the outfit the popup's Costume row shows (its live part)
        int32_t f = -1;
        get_i32(st.param, "FighterId", f);
        slot = outfit_slot(f, ret_i32(st.param, "GetCostumeId").value_or(-1));
    } else {
        slot = select_slot(s, nullptr);
    }
    if (!slot) { *why = "this outfit has no options"; return false; }
    if (!g_hooks_tried) install_hooks();
    if (pop ? !g_mfs_ok : !g_hooks_ok) { *why = "the hooks are not installed"; return false; }
    Obj* cm = control_main(st.agent);
    Obj* spins = get_obj(st.param, kd.spins);
    Obj* group = get_obj(st.param, kd.group);
    if (!cm || !spins || !group || list_count(spins) < kPanelRows) { *why = "the panel is not built"; return false; }
    const int claim = pop ? g_pop.target : s;       // the popup: the claim of the side it sets (the select is not up)
    int want = kFree;
    if (!g_owner[claim].compare_exchange_strong(want, kOurs) && want != kOurs) { *why = "the Color Mod's DI mode holds this panel"; return false; }
    Side n = follow(s, st);
    n.group = group;
    n.slot = slot;
    n.claim = claim;
    n.opt = pop ? claim - 1 : costumes::online() ? 0 : s - 1;
    // the popup: this visit's pending changes of that outfit, if any
    const auto pend_it = pop ? g_pop.pending.find(slot->mod) : g_pop.pending.end();
    if (pend_it != g_pop.pending.end() && pend_it->second.opt == n.opt) { n.chosen = pend_it->second.chosen; n.shown = pend_it->second.shown; }
    else { n.chosen = chosen_of(manifest(), *slot, choices(), n.opt); n.shown = n.chosen; }
    n.rows = rows(manifest(), *slot, n.chosen);
    if (n.rows.empty()) { release(claim); *why = "no option row"; return false; }
    // the rows: the select's four; the popup's found in order (four at least), each one's spin found by its row; a
    // row found without a spin and the ones after it are left out (hidden while on)
    if (pop) {
        n.found = 0;
        while (n.found < kd.rows && parts_of(cm, kd, n.found).ctrl) ++n.found;
        n.n = n.found;
    }
    for (int k = 0; k < n.found; ++k) {
        RowSave& rs = n.rs[k];
        RowParts p = parts_of(cm, kd, k);
        if (!p.ctrl) { release(claim); *why = "a row is missing"; return false; }
        rs.visible = call_bool(reinterpret_cast<Obj*>(p.ctrl), "get_Visible");
        if (k >= n.n) continue;
        rs.spin = pop ? spin_of_row(spins, kd.names[k], k) : list_item(spins, k);
        if (!rs.spin && pop && k >= kPanelRows) { n.n = k; continue; }
        if (!rs.spin) { release(claim); *why = "a row's spin is missing"; return false; }
        rs.smin = ret_i32(rs.spin, "get_MinNum").value_or(0);
        rs.smax = ret_i32(rs.spin, "get_MaxNum").value_or(0);
        get_i32(rs.spin, "_Num", rs.snum);
        bool ok = false;
        rs.disabled = call_bool(rs.spin, "get_IsDisable", {}, &ok);
        rs.has_disabled = ok;
        if (p.text) { rs.text = message(p.text); rs.has_text = true; }
        if (p.list) rs.has_pos = ret_floats(reinterpret_cast<Obj*>(p.list), "get_Position", rs.pos, 3);
        if (p.text) rs.has_region = ret_floats(reinterpret_cast<Obj*>(p.text), "get_RegionSize", rs.region, 2);
        for (int a = 0; a < 2; ++a)
            if (p.arrow[a]) { rs.arrow_locked[a] = call_bool(reinterpret_cast<Obj*>(p.arrow[a]), "get_ForceInvisible", {}, &ok); rs.has_arrow[a] = ok; }
        if (p.title) rs.has_mid = A()->ui_get_message_id(p.title, rs.mid) == SF6CORE_OK;
    }
    if (n.n < kPanelRows) { release(claim); *why = "a row is missing"; return false; }
    n.has_ref_pos = n.rs[0].has_pos;
    std::copy(n.rs[0].pos, n.rs[0].pos + 3, n.ref_pos);
    n.has_ref_region = n.rs[0].has_region;
    std::copy(n.rs[0].region, n.rs[0].region + 2, n.ref_region);
    // the indexes the getters answer meanwhile: the select's cache pair; the popup's part has none, its getters are asked
    n.ci = ret_i32(st.param, pop ? "GetCostumeId" : "get__cacheCostumeIndex").value_or(-1);
    n.co = ret_i32(st.param, pop ? "GetColorId" : "get__cacheColorIndex").value_or(-1);
    if (pop) {                                      // the popup's spins hold the indexes (the Color Mod's popup DI mode)
        if (n.ci >= 0) n.rs[0].snum = n.ci;
        if (n.co >= 0) n.rs[1].snum = n.co;
    }
    // the cursor: where the game's focus is when that row exists, else the first row
    const int fk = row_index(n, focused_row(st.agent));
    n.w = panel_clamp({ fk >= 0 ? fk : 0, 0 }, static_cast<int>(n.rows.size()), n.n);
    n.on = true;
    n.entered_at = g_frame;
    st = std::move(n);
    hook_side_set(s, st);                           // from now on the panel's input is ours
    pop_flag(s, true);
    if (pop && std::find(g_pop.preloaded.begin(), g_pop.preloaded.end(), slot) == g_pop.preloaded.end()) {
        g_pop.preloaded.push_back(slot);            // a pick then swaps whole at the Confirm (the preview's rule)
        popup_preload(g_pop.preloaded);
    }
    // the titles: their message link cut (kept), so the text written stays; a title already showing what we write
    // gets one write that differs first (the real write must differ, the DI mode's rule)
    const uint8_t empty[16] = {};
    for (int k = 0; k < st.n; ++k) {
        RowParts p = parts_of(cm, kd, k);
        if (!p.title || !st.rs[k].has_mid) continue;
        A()->ui_set_message_id(C(), p.title, empty);
        const int i = st.w.win + k;
        if (i >= static_cast<int>(st.rows.size())) continue;
        const std::string want_t = row_title(st, k);
        if (message(p.title) == want_t) put_text(p.title, want_t + " ");
    }
    for (int k = 0; k < st.n; ++k) inv_b(st.rs[k].spin, "set_IsDisable", false);
    if (st.w.cur - st.w.win != fk) focus_row(st, st.w.cur - st.w.win);
    paint(st, s);
    logf("rto select: %s costume options mode ON: %s/%s (%s), %zu rows in %d, options of %s%s", tag(s), slot->fighter_dir.c_str(),
         slot->folder.c_str(), slot->label.c_str(), st.rows.size(), st.n, st.opt ? "P2" : "P1",
         pop ? " (kept until the popup is confirmed)" : costumes::online() ? " (online)" : "");
    return true;
}

// Leaving: the rows given back, the game's repaint first, then what it did not put back (R36)
void leave(int s, const char* why) {
    Side st = std::move(g_s[s]);
    g_s[s] = follow(s, st);
    hook_side_clear(s);                             // the game's own calls below must run (and IsCancel blocked 15 frames)
    pop_flag(s, false);
    const Kind& kd = *st.kind;
    void* cm = control_main(st.agent);
    if (cm) hide_arrows(st, cm);
    for (int k = 0; k < st.found; ++k) {
        const RowSave& rs = st.rs[k];
        RowParts p = cm ? parts_of(cm, kd, k) : RowParts{};
        if (k >= st.n) { if (p.ctrl) vis(p.ctrl, rs.visible); continue; }   // a row left out: as it was
        spin_set(rs.spin, rs.smin, rs.smax, rs.snum);
        if (rs.has_disabled) inv_b(rs.spin, "set_IsDisable", rs.disabled);
        if (p.title && rs.has_mid) A()->ui_set_message_id(C(), p.title, rs.mid);
        if (p.ctrl) vis(p.ctrl, rs.visible);
        if (p.chip) { vis(p.chip, true); vis(p.rect0, true); vis(p.rect1, true); }
        if (p.list && rs.has_pos) A()->ui_set_position(C(), p.list, rs.pos[0], rs.pos[1], rs.pos[2]);
        if (p.text && rs.has_region) A()->ui_set_region_size(C(), p.text, rs.region[0], rs.region[1]);
        for (int a = 0; a < 2; ++a) if (p.arrow[a] && rs.has_arrow[a] && rs.arrow_locked[a]) A()->ui_set_force_invisible(C(), p.arrow[a], 1);
    }
    if (kd.popup) {
        // The popup's own repaint (the Color Mod's popup DI mode exit): its part reads GetCostumeId / GetColorId again,
        // which answer the game's values now (no cache pair to put back, no setter: the selection never moved)
        inv(st.param, "SetMessageColor"); inv(st.param, "SetMessage");
        inv(st.param, "SelectChangeColorChip");
    } else {
        if (st.ci >= 0) inv_i(st.param, "set__cacheCostumeIndex", st.ci);
        if (st.co >= 0) inv_i(st.param, "set__cacheColorIndex", st.co);
        inv(st.param, "SetMessageCostume"); inv(st.param, "SetMessageColor");
        inv(st.param, "SelectChangeColorChip");
        // The select screen's SetMessageCostume / SetMessageColor alone reset the selection to costume 0 / colour 0 (the
        // DI mode's finding, 06/10): the game's own setter puts the selection, the texts, the chips and the model back
        if (st.ci >= 0 && st.co >= 0) inv_ii(st.param, "setCostumeAndColor", st.ci, st.co);
    }
    if (cm)
        for (int k = 0; k < st.n; ++k) {                               // never the Color row's value text (a native fault)
            if (k == kColorRow || !st.rs[k].has_text) continue;
            RowParts p = parts_of(cm, kd, k);
            if (p.text && !st.rs[k].text.empty()) put_text(p.text, st.rs[k].text);
        }
    release(st.claim);
    logf("rto select: %s costume options mode OFF (%s)", tag(s), why);
}

// One step of a row's value (row: the screen row, -1 the focused one)
void step(Side& st, int s, int screen_row, int dir) {
    if (screen_row < 0) {
        screen_row = row_index(st, focused_row(st.agent));
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
    if (s == kPop) pend(st); else commit(st);
    st.rows = rows(manifest(), *st.slot, st.chosen);
    st.w = panel_clamp(st.w, static_cast<int>(st.rows.size()), st.n);
    logf("rto select: %s %s: %s = %s (%zu options, %s %s)", tag(s), st.slot->mod.c_str(), r.title.c_str(),
         v >= 1 && v <= static_cast<int>(r.labels.size()) ? r.labels[static_cast<size_t>(v - 1)].c_str() : "?", st.chosen.size(),
         s == kPop ? "pending for" : "saved for", st.opt ? "P2" : "P1");
}

// Once per visit, a second after a panel settled: the game's own guide bars (their texts with the glyphs), and a
// warning when they already show our button or key (the game would act on the same press). The select screen's
// bindings could not be read offline (no dump of its input table): this is the check in the game.
bool g_guide_logged = false;
void log_game_guide(const char* where) {
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
                std::string m = message(c);
                if (const auto sep = m.find("      <ICON"); sep != std::string::npos) m.resize(sep);   // the Color Mod's suffix
                if (!m.empty()) all += (all.empty() ? "" : " | ") + m;
            }
        }
    }
    logf("rto select: the game's guide %s: %s", where, all.empty() ? "(no text found)" : all.c_str());
    const bool pad_used = !g_keys.pad_icon.empty() && all.find(g_keys.pad_icon) != std::string::npos;
    const bool key_used = !g_keys.key_icon.empty() && all.find(g_keys.key_icon) != std::string::npos;
    if (pad_used || key_used)
        logf("rto select: WARNING: the game's guide %s already shows %s%s%s: the game may act on that press too (change "
             "select_options_button / select_options_key in settings.json)", where, pad_used ? g_keys.pad_icon.c_str() : "",
             pad_used && key_used ? " and " : "", key_used ? g_keys.key_icon.c_str() : "");
}

// The mode's frame while on: the acts the hooks caught, the focus, the paint. False when it left. pub: the side whose
// guide entry this panel publishes
bool run(Side& st, int s, bool press, int pub) {
    sync_focus(st);
    Act a;
    for (int guard = 0; guard < 8 && pull(s, a); ++guard) {
        if (a.kind == Act::Leave) { leave(s, "Decide / Back"); publish(pub, false); return false; }
        if (a.kind == Act::Move) {
            st.w = panel_move(st.w, static_cast<int>(st.rows.size()), a.dir, st.n);
            focus_row(st, st.w.cur - st.w.win);
        } else {
            step(st, s, a.row, a.dir);
            if (st.rows.empty()) { leave(s, "no option row left"); publish(pub, false); return false; }
        }
    }
    // R1 / the key do nothing while the options show (Decide or Back leave); the guide corner offers nothing
    if (press) logf("rto select: %s %s pressed while the options show: nothing (Decide / Back leave)", tag(s), g_keys.pad_name.c_str());
    paint(st, s);
    publish(pub, false);
    return true;
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

    if (st.on) { run(st, s, press, s); return; }

    // not on: the entry and the key
    uint32_t since = 0;
    const Slot* slot = active ? select_slot(s, &since) : nullptr;
    const bool settled = active && g_frame - st.active_since > kSettleFrames;
    if (active && !g_guide_logged && g_frame - st.active_since > 60) log_game_guide("on the character select");
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

// ---- the Training popup ----
// Confirmed: this visit's changes saved for the popup's side and worn by that side's fighter at once (the game rebuilds
// the fighters only when the costume or the colour changed: rto's fighters watch applies them either way)
int popup_confirm(bool same) {
    int n = 0;
    std::string what;
    for (auto& [mod, p] : g_pop.pending) {
        if (!p.slot || same_set(p.chosen, p.shown)) continue;
        auto& sd = choices().mods[mod][static_cast<size_t>(p.opt)];
        const std::vector<std::string> saved = sd.changed ? sd.ids : p.slot->defaults;
        sd.changed = true;
        sd.ids = keep_hidden(manifest(), *p.slot, saved, p.shown, p.chosen);   // a hidden NSFW choice stays saved
        sd.names.clear();
        what += (what.empty() ? "" : ", ") + p.slot->fighter_dir + "/" + p.slot->folder;
        ++n;
    }
    g_pop.pending.clear();
    g_pop.left = -1;
    // costume or colour changed too: the game rebuilds the fighters itself once the menu closes (no restart of ours,
    // one asked for by an earlier Confirm of this pause dropped); options only: the fighter keeps what it wears, the
    // battle is restarted as soon as the menu closes and the swap made in its black screen
    const int ms = training_menu_state();
    if (!same && ms > 0) {
        if (g_pw.restart) logf("rto select: popup confirmed with a costume / colour change: the game reloads, no restart of ours");
        if (!g_pw.on) { g_pw = PauseWatch{}; g_pw.on = true; g_pw.since = g_frame; }
        g_pw.reload = true;
        g_pw.restart = false;
    }
    if (!n) return 0;
    save_choices();
    choices_changed();
    const bool hold = same && ms > 0;
    fighters_refresh("the Training popup confirmed", hold ? g_pop.target - 1 : -1);
    if (same && ms > 0) {
        if (!g_pw.on) { g_pw = PauseWatch{}; g_pw.on = true; g_pw.since = g_frame; }
        g_pw.hold = true;
        g_pw.restart = !g_pw.reload;
    }
    logf("rto select: popup confirmed: the options of %d outfit(s) saved for P%d (%s), %s", n, g_pop.target, what.c_str(),
         !same ? "the costume / colour changed too: worn once the game rebuilt the fighter"
         : ms > 0 && !g_pw.reload ? "the battle restarted as the pause menu closes, the options worn in its black screen"
         : ms > 0 ? "worn when the pause menu closes (the game reloads the fighter: a costume / colour change of this pause)"
                  : "worn by the fighter now");
    return n;
}
// The popup's fighter, costume and colour now are the ones it opened with (the game then rebuilds nothing)
bool popup_same(Obj* mfs) {
    int32_t f = -1;
    if (!g_pop.base || !alive(mfs) || !get_i32(mfs, "FighterId", f)) return false;
    const auto c = ret_i32(mfs, "GetCostumeId"), n = ret_i32(mfs, "GetColorId");
    return c && n && f == g_pop.bf && *c == g_pop.bc && *n == g_pop.bn;
}
// Each frame from an options-only Confirm until the restart is over:
// - the restart is asked for once the pause menu has closed: a closed training pause menu stays at CLOSING (3; NONE is
//   rare, seen 21/10: 4 -> 3 and no further), asked kClosedFrames after 4 -> 3 (the earliest the request was not tried
//   earlier than: the fight shows until the restart's fade), offline Training only, once; the player's own Restart
//   Battle meanwhile (MenuState 1 / 2) makes ours needless (its black screen releases the swap the same way);
// - the swap is released on the restart's black screen: the training state leaving NOW (2 REFRESH_START, 3
//   REFRESH_WAIT, observed 21/10 for a few frames, the screen black around them). The request not taken (the state
//   still NOW kRetryFrames after it and IsReqRefresh read back false): asked once more. Never left NOW within
//   kGiveUpFrames: released anyway (the swap as before). No restart of ours (a costume / colour change too, not a
//   Training battle): released when the menu closes, as before.
// The fade controller's fields are logged at the request and at each training state change (a sharper "screen is
// black" signal for later; read only).
constexpr uint32_t kClosedFrames = 2, kRetryFrames = 20, kGiveUpFrames = 120, kDoneFrames = 300;
void pw_release(const char* why) {
    if (!g_pw.hold) return;
    g_pw.hold = false;
    g_pw.released = true;
    g_pw.released_at = g_frame;
    fighters_hold_end(why);
}
bool request_restart(Obj* tm) {
    const Ret r = call(tm, "set_IsReqRefresh", { arg_bool(true) });
    if (r.exception_thrown) { logf("rto select: training restart NOT requested (set_IsReqRefresh raised)"); return false; }
    return true;
}
void pause_tick() {
    if (!g_pw.on) return;
    const int ms = training_menu_state(), ts = training_state();
    if (ms != g_pw.state) {
        if (g_pw.state != -2) logf("rto select: training pause menu state %d -> %d at frame %u", g_pw.state, ms, g_frame);
        if (ms == 3 && g_pw.state != 3) g_pw.closed_at = g_frame;
        g_pw.state = ms;
        g_pw.since = g_frame;
    }
    if (ts != g_pw.ts) {
        if (g_pw.ts != -2) logf("rto select: training state %d -> %d at frame %u (menu state %d); fade controller: %s", g_pw.ts, ts,
                                g_frame, ms, fade_fields().c_str());
        g_pw.ts = ts;
    }
    if ((ms == 1 || ms == 2) && !g_pw.player) {
        g_pw.player = true;
        if (g_pw.restart) { g_pw.restart = false; logf("rto select: the battle restarts from the pause menu: no restart of ours"); }
    }
    // the black screen of a restart (ours or the player's): the held swap made now
    if (g_pw.hold && (ts == 2 || ts == 3) && (g_pw.req_at || g_pw.player)) pw_release("the restart's black screen");
    if (ms < 0) { pw_release("no TrainingManager any more"); g_pw = PauseWatch{}; return; }
    if (!g_pw.req_at) {
        if (ms > 0 && ms != 3) return;              // still in the pause menu (or the player's restart under way)
        if (!g_pw.closed_at) g_pw.closed_at = g_frame;
        if (!g_pw.restart) {
            if (g_pw.player && g_pw.hold) { g_pw.req_at = g_frame; return; }   // the player's restart: its black screen
            pw_release("the pause menu closed, no restart of ours");
            g_pw = PauseWatch{};
            return;
        }
        if (ms == 3 && g_frame - g_pw.closed_at < kClosedFrames) return;   // CLOSING: kClosedFrames
        Obj* tm = reinterpret_cast<Obj*>(api().get_managed_singleton("app.training.TrainingManager"));
        const int mode = costumes::battle_mode();
        if (!alive(tm) || mode != 2 || costumes::online()) {
            logf("rto select: training restart not requested (%s)", !alive(tm) ? "no TrainingManager any more"
                 : mode != 2 ? "not a Training battle" : "online");
            pw_release("no restart");
            g_pw = PauseWatch{};
            return;
        }
        if (!request_restart(tm)) { pw_release("no restart"); g_pw = PauseWatch{}; return; }
        g_pw.req_at = g_frame;
        logf("rto select: training restart requested at frame %u, %u frames after the pause menu closed (set_IsReqRefresh, as "
             "Restart Battle; menu state %d, training state %d); fade controller: %s", g_frame, g_frame - g_pw.closed_at, ms, ts,
             fade_fields().c_str());
        return;
    }
    // requested (or the player's restart): the black screen awaited
    if (g_pw.hold && !g_pw.player && !g_pw.retried && ts == 1 && g_frame - g_pw.req_at >= kRetryFrames) {
        g_pw.retried = true;
        Obj* tm = reinterpret_cast<Obj*>(api().get_managed_singleton("app.training.TrainingManager"));
        bool ok = false;
        const bool pending = alive(tm) && call_bool(tm, "get_IsReqRefresh", {}, &ok);
        if (alive(tm) && ok && !pending) {
            logf("rto select: training restart not taken %u frames after the request (still NOW, IsReqRefresh false): asked once more",
                 g_frame - g_pw.req_at);
            request_restart(tm);
        } else {
            logf("rto select: training restart still awaited %u frames after the request (IsReqRefresh %s)", g_frame - g_pw.req_at,
                 !ok ? "unread" : pending ? "true" : "false");
        }
    }
    if (g_pw.hold && g_frame - g_pw.req_at >= kGiveUpFrames) {
        logf("rto select: the training state never left NOW within %u frames of the restart: the swap released anyway", kGiveUpFrames);
        pw_release("no black screen seen");
    }
    // over: the restart done (back to NOW after the release) or long enough
    if ((g_pw.released && ts == 1 && g_frame > g_pw.released_at) || g_frame - g_pw.req_at >= kDoneFrames) g_pw = PauseWatch{};
}
// The popup closed or rebuilt (or the module stops): its mode left (dropped when gone), the changes not confirmed dropped
void popup_end(const char* why) {
    if (!g_pop.up && !g_s[kPop].on) return;
    Side& st = g_s[kPop];
    if (st.on) { if (alive(st.agent) && alive(st.param)) leave(kPop, why); else drop(kPop, why); }
    size_t n = 0;
    for (auto& [mod, p] : g_pop.pending) if (p.slot && !same_set(p.chosen, p.shown)) ++n;
    if (n) logf("rto select: popup left without Confirm (%s): the pending options of %zu outfit(s) dropped", why, n);
    if (g_pop.up) publish(g_pop.target, false);
    if (!g_pop.preloaded.empty()) popup_preload({});   // the confirmed ones stay loaded as the choices' (choices_changed)
    g_pop = Popup{};
    g_s[kPop] = follow(kPop, Side{});
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
    popup_end(why);
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

// ---- the Training popup: found from EV_POPUP, then followed each frame while it shows ----
void install_popup_hooks() {
    auto* tdb = api().tdb();
    int n = 0;
    // its part set up or written (it is built when the popup opens; Fighter Settings' part fires too: nothing found)
    TD* mfs = tdb->find_type("app.UIPartsMatchingFighterSetting");
    for (const char* nm : { "SetupSpin", "SetMessage" })
        if (Method* m = mfs ? mfs->find_method(nm) : nullptr; m && m->add_hook(pre_popup_event, hook_post_pass, false)) ++n;
    // its flow's own update (R19: the arrows' copies are made there; the popup is up while it runs)
    Method* upd = own_method(tdb->find_type("app.UIFlowCharacterDetailSetting.Wait"), "OnUpdate");
    const bool flow = upd && upd->get_function_raw() && upd->add_hook(hook_pass, post_popup_update, false);
    g_ck[1].hook = flow && SF6CORE_HAS(A(), ui_clone_control);
    logf("rto select: Training popup followed from %d of its part's methods%s; its arrows %s", n,
         flow ? " and app.UIFlowCharacterDetailSetting.Wait.OnUpdate" : " (its flow's update not hooked)",
         g_ck[1].hook ? "in the rows' left margin" : "before the titles");
}

bool popup_busy() { return g_pop.up || g_s[kPop].on || g_pw.on || (g_pop_event && g_frame - g_pop_event < 30); }

void popup_tick(bool announced) {
    if (announced) g_pop_event = g_frame;
    if (g_off || !A() || !C() || !SF6CORE_HAS(A(), ui_agents)) return;
    try {
        if (const uint32_t hf = g_hook_faults.exchange(0)) logf("rto select: %u exceptions in the hooks", hf);
        color_mod_check();                          // once (every plugin is loaded before the first frame)
        claims_tick();
        pause_tick();
        void* buf[4] = {};
        const uint32_t na = A()->ui_agents(kPopupAgent, buf, 4);
        Obj* agent = na ? reinterpret_cast<Obj*>(buf[0]) : nullptr;   // the first one (the Color Mod's rule)
        Obj* cm = control_main(agent);
        bool ok = false;
        const bool shown = cm && call_bool(cm, "get_ActualVisible", {}, &ok) && ok;
        Obj* flow = shown ? param_of(agent) : nullptr;
        Obj* mfs = flow ? get_obj(flow, "MatchingFighterSetting") : nullptr;
        if (!shown || !alive(flow) || !alive(mfs)) {
            // a Confirm seen as the popup went (hidden the frame it was decided): still a Confirm
            bool d = false;
            if (g_pop.up && !g_pop.decided && !g_pop.pending.empty() && alive(g_pop.flow) && get_bool(g_pop.flow, "IsDecide", d) && d)
                popup_confirm(popup_same(get_obj(g_pop.flow, "MatchingFighterSetting")));
            popup_end("the popup closed");             // its mode (if on) left: the rows put back while it lives
            return;
        }
        if (agent != g_pop.agent || flow != g_pop.flow) {   // a new visit
            popup_end("the popup was rebuilt");
            g_pop.agent = agent; g_pop.flow = flow; g_pop.up = true; g_pop.since = g_frame;
            int32_t t = 0;
            g_pop.target = get_i32(flow, "SettingTargetPlayerIndex", t) && t == 1 ? 2 : 1;
            g_s[kPop] = follow(kPop, Side{});
            g_guide_logged = false;
            read_keys();
            logf("rto select: Training popup up (P%d's settings): costume options %s", g_pop.target,
                 popup_cm_ok() ? "offered on outfits that have them" : "off (see the Color Mod line above)");
        }
        Side& st = g_s[kPop];
        if (!g_pop.base && !st.on) {                // what it opened with (its getters are ours while the options show)
            int32_t f = -1;
            const auto c = ret_i32(mfs, "GetCostumeId"), n = ret_i32(mfs, "GetColorId");
            if (get_i32(mfs, "FighterId", f) && c && n) { g_pop.base = true; g_pop.bf = f; g_pop.bc = *c; g_pop.bn = *n; }
        }
        // Confirm: the flow's IsDecide (the Color Mod's colour commit reads it too); the changes saved and worn
        bool dec = false;
        get_bool(flow, "IsDecide", dec);
        if (dec && !g_pop.decided) {
            if (st.on) leave(kPop, "the popup was confirmed");
            popup_confirm(popup_same(mfs));
        }
        g_pop.decided = dec;
        // the pending options' files: told once they are all ready (counted every 10 frames)
        if (!g_pop.pending.empty() && g_frame % 10 == 0) {
            int left = 0, total = 0;
            for (auto& [mod, p] : g_pop.pending) {
                if (!p.slot || same_set(p.chosen, p.shown)) continue;
                int t = 0;
                left += options_files_left(*p.slot, p.chosen, &t);
                total += t;
            }
            if (left == 0 && g_pop.left != 0 && total > 0)
                logf("rto select: popup: the %d files of the pending options are ready (Confirm swaps them whole)", total);
            g_pop.left = left;
        }
        if (st.on) {
            if (!alive(st.agent) || !alive(st.param)) { drop(kPop, "the popup is gone"); return; }
            if (mfs != st.param) { leave(kPop, "the popup's part was rebuilt"); return; }
        } else {
            st.agent = agent; st.param = mfs;
        }
        const bool down = trigger_down(0);
        const bool press = down && !st.key_was;
        st.key_was = down;
        if (st.on) { run(st, kPop, press, g_pop.target); return; }

        // not on: the entry and the key
        int32_t f = -1;
        get_i32(mfs, "FighterId", f);
        const Slot* slot = outfit_slot(f, ret_i32(mfs, "GetCostumeId").value_or(-1));
        const bool settled = g_frame - g_pop.since > kSettleFrames;
        if (!g_guide_logged && g_frame - g_pop.since > 60) log_game_guide("in the Training popup");
        const int ts = g_pop.target;
        const char* why = nullptr;
        if (!popup_cm_ok()) why = "the Color Mod does not share the popup (or is not loaded)";
        else if (!settled) why = "the popup is not settled";
        else if (!slot) why = "this outfit has no options";
        else if (g_owner[ts].load() == kColorMod || di_on(ts)) why = "the Color Mod's DI mode holds the popup";
        else if (SF6CORE_HAS(A(), opt_window_open) && A()->opt_window_open()) why = "an option window is open";
        const bool on_row = settled && focused_row(agent).rfind("p_Spin_", 0) == 0;
        if (!why && !on_row) why = "the popup is not focused";
        publish(ts, !why);
        if (!press) return;
        if (!why) {
            const char* err = nullptr;
            if (!enter(kPop, &err)) logf("rto select: popup costume options not opened: %s", err ? err : "?");
            return;
        }
        logf("rto select: popup %s pressed: %s", g_keys.pad_name.c_str(), why);
    } catch (...) {
        ++g_faults;
        logf("rto select: EXCEPTION in the Training popup (%d)", g_faults);
        drop(kPop, "exception");
        if (g_faults >= 5) { g_off = true; logf("rto select: 5 faults, the costume options mode is off for this session"); }
    }
}

bool popup_guard() {
    if (g_pop_on.load()) return true;
    const uint32_t left = g_pop_left.load();
    return left && g_frame - left <= kPopGuard;
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

// The owner of a side's panel (side 1 / 2), the one writer rule between the Slots Loader's costume options mode (who 1)
// and the Color Mod's DI mode (who 2), which borrow the same rows: a side's panel of the character select, and the
// Training popup under the claim of the side it sets (SettingTargetPlayerIndex + 1; the select is never up then). op 1:
// take it, 1 = granted (it was free, or already who's), 0 = the other one holds it; op 0: give it back (only who's), 1;
// op -1: the owner now (0 free, 1, 2). Cheap, any thread, no game call.
extern "C" __declspec(dllexport) int SF6Slots_SelectPanelClaim(int side, int who, int op) {
    try { return slots::rto::select::claim(side, who, op); } catch (...) { return 0; }
}

// The guide entry of a side (1 / 2) for the Color Mod, which writes that guide alone and adds this after its own ("L1
// Edit Drive Colors"): on the character select that side's corner, in the Training popup the popup's guide (side = the
// side the popup sets). "<ICON BtnR1> Costume options" (pad != 0) or the keyboard key's glyph ("<ICON KeyC> Costume
// options"), UTF-8, NUL-terminated, copied into buf; its length, or 0 (buf empty) when that outfit has no options, the
// panel is not ready or held by the DI mode, or cap is too small. Empty too while the panel shows the options (the key
// does nothing then; Decide / Back leave). Cheap, any thread, no game call.
extern "C" __declspec(dllexport) int SF6Slots_SelectGuideEntry(int side, int pad, char* buf, int cap) {
    try { return slots::rto::select::guide_entry(side, pad, buf, cap); } catch (...) { return 0; }
}

// 1 while the Training popup's costume options mode is on, and for 15 frames after it was left, else 0: the Color Mod's
// Esc guard (its InputCancel / CloseMenu hooks of the pause menu) holds meanwhile, so the Back that leaves the mode
// neither closes the popup nor the pause menu. Cheap, any thread, no game call.
extern "C" __declspec(dllexport) int SF6Slots_PopupGuard() {
    try { return slots::rto::select::popup_guard() ? 1 : 0; } catch (...) { return 0; }
}
