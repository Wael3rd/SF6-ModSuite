#include "dipanel.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ll_common.hpp"

#include "../common.hpp"
#include "../perf.hpp"
#include "../perf_hook.hpp"

namespace screens::dipanel {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;

// ---- constants (from DIPanelMode.lua) ----------------------------------

constexpr int kMaxMecs = 6;              // training popup has 6 rows
constexpr int kMaxNames = 12;            // row names for re-resolution (rows + hidden)
// The game's input entry points carry an app.InputDigitalFlag (Down 1, Trigger 2, Release 4, Repeat 8): a press is
// one Trigger, a held key a Repeat every 3 frames after a delay. Only Trigger and Repeat act, as in the game's own
// rows. The 12-frame hold of DIPanelMode.lua stood in for that filter and also ate quick presses (6-7 frames apart:
// one in two lost, traced 06/10). One action per side and frame stays, for the entry points without a flag (Add /
// Sub, the mouse, MoveFocus) and two paths of one press in the same frame.
constexpr uint32_t kActFlags = 2 | 8;    // app.InputDigitalFlag Trigger | Repeat
constexpr int kMaxFaults = 5;

// ---- per-mechanic display state (pushed by Lua) -------------------------

struct MecState {
    uint64_t spin_addr = 0;
    std::string title;
    std::string value;
    int32_t mn = 0, mx = 0, num = 0;
};

// ---- per-side state -----------------------------------------------------

struct SideState {
    bool active = false;
    uint64_t param_addr = 0;
    uint64_t agent_addr = 0;
    uint64_t owner_addr = 0;
    uint64_t group_addr = 0;
    int32_t saved_ci = -1, saved_co = -1;
    int32_t n_mecs = 0;
    MecState mecs[kMaxMecs];
    std::vector<std::string> row_names;
    std::vector<std::string> hide_names;
    double ref_pos_x = 0, ref_region_w = 300;
    // captured actions (consumed by pull)
    struct Action { int type; int which; int dir; };
    std::deque<Action> actions;
    // debounce
    uint64_t last_step = 0;
    uint64_t last_move = 0;
};

// ---- global state -------------------------------------------------------

std::recursive_mutex g_mx;                     // B2: recursive — game calls under lock can re-enter hooked methods
SideState g_side[3];                       // index 0 unused, 1 = P1, 2 = P2
bool g_any = false;                        // fast exit for hooks
bool g_on = false;                         // install succeeded
int g_faults = 0;

// address -> side maps (for hook dispatch)
std::unordered_map<uint64_t, int> g_by_spin;    // spin addr -> side
std::unordered_map<uint64_t, int> g_by_spin_w;  // spin addr -> which (1-based)
std::unordered_map<uint64_t, int> g_by_param;   // param/owner addr -> side
std::unordered_map<uint64_t, int> g_by_agent;   // agent addr -> side
std::unordered_map<uint64_t, int> g_by_group;   // group addr -> side

// cancel block: owner_addr -> frame until which IsCancel/IsDecide stay false
std::unordered_map<uint64_t, uint64_t> g_cancel;

// thread-local pre->post communication for return-override hooks
thread_local int64_t tl_ret = INT64_MIN;

// ---- helpers ------------------------------------------------------------

void fault(const char* where) {
    ++g_faults;
    cmn::journal("dipanel: fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("dipanel: too many faults, off (Lua takes over)");
    }
}

void rebuild_maps() {
    g_by_spin.clear(); g_by_spin_w.clear();
    g_by_param.clear(); g_by_agent.clear(); g_by_group.clear();
    bool any = false;
    for (int s = 1; s <= 2; ++s) {
        auto& st = g_side[s];
        if (!st.active) continue;
        any = true;
        g_by_param[st.owner_addr] = s;
        if (st.param_addr && st.param_addr != st.owner_addr)
            g_by_param[st.param_addr] = s;
        g_by_agent[st.agent_addr] = s;
        g_by_group[st.group_addr] = s;
        for (int i = 0; i < st.n_mecs; ++i) {
            g_by_spin[st.mecs[i].spin_addr] = s;
            g_by_spin_w[st.mecs[i].spin_addr] = i + 1;
        }
    }
    g_any = any;
}

// look up which side a spin belongs to (0 = not ours)
int spin_side(uint64_t addr) {
    auto it = g_by_spin.find(addr);
    return it != g_by_spin.end() ? it->second : 0;
}
int spin_which(uint64_t addr) {
    auto it = g_by_spin_w.find(addr);
    return it != g_by_spin_w.end() ? it->second : 0;
}
int param_side(uint64_t addr) {
    auto it = g_by_param.find(addr);
    return it != g_by_param.end() ? it->second : 0;
}
int agent_side(uint64_t addr) {
    auto it = g_by_agent.find(addr);
    return it != g_by_agent.end() ? it->second : 0;
}
int group_side(uint64_t addr) {
    auto it = g_by_group.find(addr);
    return it != g_by_group.end() ? it->second : 0;
}
bool acts(void* flag_arg) { return ((uint32_t)(reinterpret_cast<uintptr_t>(flag_arg) & 0xFFFFFFFF) & kActFlags) != 0; }
bool cancel_blocked(uint64_t addr) {
    auto it = g_cancel.find(addr);
    return it != g_cancel.end() && cmn::frame() < it->second;
}

// record a step action for the Lua to pull
void do_step(int side, int which, int dir, const char* via) {
    if (side < 1 || side > 2) return;
    auto& st = g_side[side];
    if (!st.active) return;
    if (st.last_step && cmn::frame() == st.last_step) return;      // one step per frame (see kActFlags)
    st.last_step = cmn::frame();
    st.actions.push_back({ 1, which, dir });
    cmn::journal("dipanel: step P%d row %d dir %d via %s", side, which, dir, via);   // which hook produced it (phantom steps, 03/10)
}

// record an exit action
void do_exit(int side) {
    if (side < 1 || side > 2) return;
    auto& st = g_side[side];
    if (!st.active) return;
    st.actions.push_back({ 2, 0, 0 });
}

// focused mechanic (which of our rows has the focus)
// NOTE: must not be called from inside __try (it uses std::string/vector)
int focused_which(int side) {
    auto& st = g_side[side];
    auto* agent = reinterpret_cast<MO*>(st.agent_addr);
    auto* gfi = ll::find_method_noargs(agent->get_type_definition(), "GetFocusItem");
    if (!gfi) return 1;
    auto rc = gfi->invoke(agent, std::vector<void*>{});
    if (rc.exception_thrown || !rc.ptr) return 1;
    auto* item = reinterpret_cast<MO*>(rc.ptr);
    MO* ctrl = ll::read_field_object(item, "<Control>k__BackingField");
    if (!ctrl) return 1;
    auto* gn = ll::find_method_noargs(ctrl->get_type_definition(), "get_Name");
    if (!gn) return 1;
    auto rn = gn->invoke(ctrl, std::vector<void*>{});
    if (rn.exception_thrown || !rn.ptr) return 1;
    std::string nm = ll::managed_string_to_utf8(reinterpret_cast<MO*>(rn.ptr));
    for (int i = 0; i < st.n_mecs; ++i)
        if (i < (int)st.row_names.size() && nm == st.row_names[i]) return i + 1;
    return 1;
}

// do focus move (handled by the DLL internally, not pushed to Lua)
void do_move(int side, int dir) {
    auto& st = g_side[side];
    if (!st.active) return;
    if (st.last_move && cmn::frame() == st.last_move) return;      // one move per frame (see kActFlags)
    st.last_move = cmn::frame();
    auto* api = cmn::core();
    if (!api) return;
    void* group = reinterpret_cast<void*>(st.group_addr);
    SF6CoreArg out{};
    api->obj_invoke(cmn::client(), group, "GetFocusIndex", nullptr, 0, &out);
    int32_t cur = (int32_t)out.v.i;
    int32_t n = st.n_mecs;
    int32_t target = (((cur + dir) % n) + n) % n;
    if (target != cur) {
        SF6CoreArg arg{};
        arg.type = SF6CORE_ARG_I64; arg.v.i = target;
        api->obj_invoke(cmn::client(), group, "ChangeFocusIndex", &arg, 1, nullptr);
    }
}

// ---- tick (repaint from pushed state) ------------------------------------

void tick_side(int side) {
    auto& st = g_side[side];
    if (!st.active) return;
    auto* api = cmn::core();
    if (!api) return;

    // the agent must still be alive
    auto* agent = reinterpret_cast<MO*>(st.agent_addr);
    if (!API::get()->sdk()->managed_object->is_managed_object(agent)) return;

    // resolve ControlMain
    auto* get_cm = ll::find_method_noargs(agent->get_type_definition(), "get_ControlMain");
    if (!get_cm) return;
    auto rc = get_cm->invoke(agent, std::vector<void*>{});
    if (rc.exception_thrown || !rc.ptr) return;
    void* cm = rc.ptr;

    // find all row controls + sub-elements in one walk per row
    for (int i = 0; i < st.n_mecs; ++i) {
        auto& m = st.mecs[i];
        if (i >= (int)st.row_names.size()) continue;
        const std::string& rn = st.row_names[i];

        // find the row control
        void* row = api->ui_find_child(cm, rn.c_str(), 8);
        if (!row) continue;

        // set visibility
        api->ui_set_visible(cmn::client(), row, 1);

        // find sub-elements
        void* title_e = api->ui_find_child(row, "e_text_title", 4);
        void* text_e = api->ui_find_child(row, "e_text", 4);
        void* list_h = api->ui_find_child(row, "p_TextList_color_h", 4);
        if (!list_h) list_h = api->ui_find_child(row, "p_TextList_type_h", 4);
        void* chip = api->ui_find_child(row, "c_chip", 4);
        void* arrow_l = api->ui_find_child(row, "p_MouseDecideOperable_L", 4);
        void* arrow_r = api->ui_find_child(row, "p_MouseDecideOperable_R", 4);

        // set texts (core handles R7 compare-before-write)
        if (title_e && !m.title.empty())
            api->ui_set_text(cmn::client(), title_e, m.title.c_str());
        if (text_e && !m.value.empty())
            api->ui_set_text(cmn::client(), text_e, m.value.c_str());

        // hide the colour chips (all four lines look the same)
        if (chip) api->ui_set_visible(cmn::client(), chip, 0);

        // arrows: unlock ForceInvisible and show
        for (void* a : { arrow_l, arrow_r }) {
            if (a) {
                api->ui_set_force_invisible(cmn::client(), a, 0);
                api->ui_set_visible(cmn::client(), a, 1);
            }
        }

        // align the value text holder to the reference position
        if (list_h && st.ref_pos_x != 0)
            api->ui_set_position(cmn::client(), list_h, (float)st.ref_pos_x, 0, 0);

        // align the region size
        if (text_e && st.ref_region_w > 0)
            api->ui_set_region_size(cmn::client(), text_e, (float)st.ref_region_w, 0);

        // set spin values
        void* spin = reinterpret_cast<void*>(m.spin_addr);
        SF6CoreArg arg{};
        arg.type = SF6CORE_ARG_I64;
        arg.v.i = m.mn;
        api->obj_invoke(cmn::client(), spin, "set_MinNum", &arg, 1, nullptr);
        arg.v.i = m.mx;
        api->obj_invoke(cmn::client(), spin, "set_MaxNum", &arg, 1, nullptr);
        api->obj_set_i64(cmn::client(), spin, "_Num", m.num);
    }

    // hide rows that don't belong to us
    for (auto& hn : st.hide_names) {
        void* hrow = api->ui_find_child(cm, hn.c_str(), 8);
        if (hrow) api->ui_set_visible(cmn::client(), hrow, 0);
    }

    // clamp focus: only our rows exist
    {
        void* group = reinterpret_cast<void*>(st.group_addr);
        SF6CoreArg out{};
        if (api->obj_invoke(cmn::client(), group, "GetFocusIndex", nullptr, 0, &out) == SF6CORE_OK) {
            int32_t cur = (int32_t)out.v.i;
            if (cur >= st.n_mecs) {
                SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = 0;
                api->obj_invoke(cmn::client(), group, "ChangeFocusIndex", &a, 1, nullptr);
            }
        }
    }
}

// ---- hook functions (SEH-wrapped) ----------------------------------------

// Inner functions (may use objects with destructors: lock_guard, string) called from SEH wrappers below.
int spin_step_inner(void** argv, int dir, bool flagged) {          // flagged: InputRight / InputLeft(flag)
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = spin_side(addr);
    if (!s || !g_side[s].active) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (flagged && !acts(argv[2])) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;   // held / released: nothing (our spin never moves)
    int w = spin_which(addr);
    do_step(s, w, dir, dir > 0 ? "spin add" : "spin sub");
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
int spin_dir_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = spin_side(addr);
    if (!s || !g_side[s].active) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int w = spin_which(addr);
    uint32_t d = (uint32_t)(reinterpret_cast<uintptr_t>(argv[2]) & 0xFFFFFFFF);
    if ((d & 12) && !acts(argv[3])) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;  // InputDirection(dir, flag)
    if (d & 4) { do_step(s, w, -1, "spin dir"); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    if (d & 8) { do_step(s, w, 1, "spin dir"); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int param_skip_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    return param_side(addr) ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int param_gate_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    if (param_side(addr) || cancel_blocked(addr)) { tl_ret = 0; return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    tl_ret = INT64_MIN;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int param_get_ci_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = param_side(addr);
    if (s && g_side[s].active && g_side[s].saved_ci >= 0) { tl_ret = g_side[s].saved_ci; return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    tl_ret = INT64_MIN; return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int param_get_co_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = param_side(addr);
    if (s && g_side[s].active && g_side[s].saved_co >= 0) { tl_ret = g_side[s].saved_co; return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    tl_ret = INT64_MIN; return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int agent_lr_inner(void** argv, int dir) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = agent_side(addr);
    if (!s || !g_side[s].active) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!acts(argv[2])) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;     // InputLeft / InputRight(flag)
    int w = focused_which(s);
    do_step(s, w, dir, "agent lr");
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
int agent_ud_inner(void** argv, int dir) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = agent_side(addr);
    if (!s || !g_side[s].active) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!acts(argv[2])) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;     // InputUp / InputDown(flag)
    do_move(s, dir);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
int agent_dir_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = agent_side(addr);
    if (!s || !g_side[s].active) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    uint32_t d = (uint32_t)(reinterpret_cast<uintptr_t>(argv[2]) & 0xFFFFFFFF);
    if ((d & 15) && !acts(argv[3])) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;  // InputDirection(dir, flag)
    if (d & 4) { do_step(s, focused_which(s), -1, "agent dir"); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    if (d & 8) { do_step(s, focused_which(s), 1, "agent dir"); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    if (d & 3) { do_move(s, (d & 1) ? -1 : 1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int agent_exit_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = agent_side(addr);
    if (!s) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    do_exit(s);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
int group_move_inner(void** argv) {
    uint64_t addr = reinterpret_cast<uint64_t>(argv[1]);
    std::lock_guard lk(g_mx);
    int s = group_side(addr);
    if (!s || !g_side[s].active) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    uint32_t d = (uint32_t)(reinterpret_cast<uintptr_t>(argv[3]) & 0xFFFFFFFF);
    do_move(s, (d & 1) ? -1 : 1);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}

// SEH wrappers: no objects with destructors, so __try is legal.
#pragma warning(push)
#pragma warning(disable: 4733)

int pre_spin_add(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return spin_step_inner(argv, 1, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("spin_add"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_spin_sub(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return spin_step_inner(argv, -1, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("spin_sub"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_spin_input_right(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return spin_step_inner(argv, 1, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("spin_input_right"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_spin_input_left(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return spin_step_inner(argv, -1, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("spin_input_left"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_spin_dir(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return spin_dir_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("spin_dir"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_param_skip(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return param_skip_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("param_skip"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_param_gate(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) { tl_ret = INT64_MIN; return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    __try { return param_gate_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { tl_ret = INT64_MIN; fault("param_gate"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
void post_param_gate(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_ret != INT64_MIN) *ret_val = reinterpret_cast<void*>((uintptr_t)tl_ret);
}
int pre_param_get_costume(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) { tl_ret = INT64_MIN; return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    __try { return param_get_ci_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { tl_ret = INT64_MIN; fault("getCostume"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_param_get_color(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) { tl_ret = INT64_MIN; return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    __try { return param_get_co_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { tl_ret = INT64_MIN; fault("getColor"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
void post_ret_override(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_ret != INT64_MIN) *ret_val = reinterpret_cast<void*>((uintptr_t)tl_ret);
}
int pre_agent_left(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return agent_lr_inner(argv, -1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("agent_left"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_agent_right(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return agent_lr_inner(argv, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("agent_right"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_agent_up(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return agent_ud_inner(argv, -1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("agent_up"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_agent_down(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return agent_ud_inner(argv, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("agent_down"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_agent_dir(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return agent_dir_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("agent_dir"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_agent_exit(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return agent_exit_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("agent_exit"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_group_move(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_any) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return group_move_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("group_move"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}

#pragma warning(pop)

void post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- csv helpers --------------------------------------------------------

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

// ---- hook installation --------------------------------------------------

bool hook_methods(API::TypeDefinition* td, const char* const* names, size_t n,
                  int (*pre)(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long),
                  void (*post)(void**, REFrameworkTypeDefinitionHandle, unsigned long long),
                  int& count) {
    if (!td) return false;
    for (size_t i = 0; i < n; ++i) {
        auto* m = td->find_method(names[i]);
        if (m && m->get_function_raw()) {
            if (m->add_hook(pre, post, false)) { ++count; perf::journal_hooked(names[i], m); }
        }
    }
    return true;
}

}  // namespace

// ---- public API ---------------------------------------------------------

bool install() {
    auto* tdb = API::get()->tdb();
    int total = 0;

    // UIPartsSpin step hooks
    auto* spin_td = tdb->find_type("app.UIPartsSpin");
    if (spin_td) {
        const char* n_add[] = { "Add" }; const char* n_addnum[] = { "AddNum" }; const char* n_mouseadd[] = { "MouseAdd" };
        const char* n_sub[] = { "Sub" }; const char* n_subnum[] = { "SubNum" }; const char* n_mousesub[] = { "MouseSub" };
        const char* right_name[] = { "InputRight" };
        const char* left_name[] = { "InputLeft" };
        hook_methods(spin_td, n_add, 1, perf::timed_pre<perf::HkDiSpinAdd, pre_spin_add>, post_nop, total);
        hook_methods(spin_td, n_addnum, 1, perf::timed_pre<perf::HkDiSpinAddNum, pre_spin_add>, post_nop, total);
        hook_methods(spin_td, n_mouseadd, 1, perf::timed_pre<perf::HkDiSpinMouseAdd, pre_spin_add>, post_nop, total);
        hook_methods(spin_td, n_sub, 1, perf::timed_pre<perf::HkDiSpinSub, pre_spin_sub>, post_nop, total);
        hook_methods(spin_td, n_subnum, 1, perf::timed_pre<perf::HkDiSpinSubNum, pre_spin_sub>, post_nop, total);
        hook_methods(spin_td, n_mousesub, 1, perf::timed_pre<perf::HkDiSpinMouseSub, pre_spin_sub>, post_nop, total);
        hook_methods(spin_td, right_name, 1, perf::timed_pre<perf::HkDiSpinInputRight, pre_spin_input_right>, post_nop, total);
        hook_methods(spin_td, left_name, 1, perf::timed_pre<perf::HkDiSpinInputLeft, pre_spin_input_left>, post_nop, total);
        auto* sd = spin_td->find_method("InputDirection");
        if (sd && sd->get_function_raw() && sd->add_hook(perf::timed_pre<perf::HkDiSpinInputDirection, pre_spin_dir>, post_nop, false)) { ++total; perf::journal_hooked("UIPartsSpin.InputDirection", sd); }
    }

    // Param skip hooks
    auto* param_td = tdb->find_type("app.UIFlowUI10505.Param");
    if (param_td) {
        const char* skip_names[] = {
            "SelectChangeCostume", "SelectChangeColorChip", "SelectChangeControllerType",
            "SetMessageCostume", "SetMessageColor", "SetMessagePreset", "SetMessageList",
            "setCostume", "setColor", "setCostumeAndColor"
        };
        hook_methods(param_td, skip_names, 10, perf::timed_pre<perf::HkDiParamSkip, pre_param_skip>, post_nop, total);   // 10 methods, one Id
        const char* gate_names[] = { "IsDecide", "IsCancel", "isSelectOperationType" };
        hook_methods(param_td, gate_names, 3, perf::timed_pre<perf::HkDiParamGate, pre_param_gate>, post_param_gate, total);   // 3 methods, one Id
        auto* gc = param_td->find_method("getCostume");
        if (gc && gc->get_function_raw() && gc->add_hook(perf::timed_pre<perf::HkDiParamGetCostume, pre_param_get_costume>, post_ret_override, false)) { ++total; perf::journal_hooked("UIFlowUI10505.Param.getCostume", gc); }
        auto* gcl = param_td->find_method("getColor");
        if (gcl && gcl->get_function_raw() && gcl->add_hook(perf::timed_pre<perf::HkDiParamGetColor, pre_param_get_color>, post_ret_override, false)) { ++total; perf::journal_hooked("UIFlowUI10505.Param.getColor", gcl); }
    }

    // MFS skip hooks
    auto* mfs_td = tdb->find_type("app.UIPartsMatchingFighterSetting");
    if (mfs_td) {
        const char* mfs_skip[] = {
            "SelectChangeCostume", "SelectChangeColorChip", "SelectChangeControllerType",
            "SelectChangePresetIndex", "SetMessage", "SetMessageColor", "SetupSpin",
            "SetupPresetSpin", "SetupPresetSpinCursor", "SelectionChanged",
            "SelectionChanged_Spin", "ChangeEverything"
        };
        hook_methods(mfs_td, mfs_skip, 12, perf::timed_pre<perf::HkDiMfsSkip, pre_param_skip>, post_nop, total);   // 12 methods, one Id
        // GetCostumeId / GetColorId -> return saved values (same as getCostume/getColor)
        auto* gci = mfs_td->find_method("GetCostumeId");
        if (gci && gci->get_function_raw() && gci->add_hook(perf::timed_pre<perf::HkDiMfsGetCostume, pre_param_get_costume>, post_ret_override, false)) { ++total; perf::journal_hooked("UIPartsMatchingFighterSetting.GetCostumeId", gci); }
        auto* gcol = mfs_td->find_method("GetColorId");
        if (gcol && gcol->get_function_raw() && gcol->add_hook(perf::timed_pre<perf::HkDiMfsGetColor, pre_param_get_color>, post_ret_override, false)) { ++total; perf::journal_hooked("UIPartsMatchingFighterSetting.GetColorId", gcol); }
    }

    // UIAgent hooks
    auto* agent_td = tdb->find_type("app.UIAgent");
    if (agent_td) {
        auto* il = agent_td->find_method("InputLeft");
        if (il && il->get_function_raw() && il->add_hook(perf::timed_pre<perf::HkDiAgentInputLeft, pre_agent_left>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputLeft", il); }
        auto* ir = agent_td->find_method("InputRight");
        if (ir && ir->get_function_raw() && ir->add_hook(perf::timed_pre<perf::HkDiAgentInputRight, pre_agent_right>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputRight", ir); }
        auto* iu = agent_td->find_method("InputUp");
        if (iu && iu->get_function_raw() && iu->add_hook(perf::timed_pre<perf::HkDiAgentInputUp, pre_agent_up>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputUp", iu); }
        auto* id = agent_td->find_method("InputDown");
        if (id && id->get_function_raw() && id->add_hook(perf::timed_pre<perf::HkDiAgentInputDown, pre_agent_down>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputDown", id); }
        auto* idir = agent_td->find_method("InputDirection");
        if (idir && idir->get_function_raw() && idir->add_hook(perf::timed_pre<perf::HkDiAgentInputDirection, pre_agent_dir>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputDirection", idir); }
        auto* idec = agent_td->find_method("InputDecide");
        if (idec && idec->get_function_raw() && idec->add_hook(perf::timed_pre<perf::HkDiAgentInputDecide, pre_agent_exit>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputDecide", idec); }
        auto* ican = agent_td->find_method("InputCancel");
        if (ican && ican->get_function_raw() && ican->add_hook(perf::timed_pre<perf::HkDiAgentInputCancel, pre_agent_exit>, post_nop, false)) { ++total; perf::journal_hooked("UIAgent.InputCancel", ican); }
    }

    // UIPartsGroup.MoveFocus
    auto* grp_td = tdb->find_type("app.UIPartsGroup");
    if (grp_td) {
        auto* mf = grp_td->find_method("MoveFocus");
        if (mf && mf->get_function_raw() && mf->add_hook(perf::timed_pre<perf::HkDiGroupMoveFocus, pre_group_move>, post_nop, false)) { ++total; perf::journal_hooked("UIPartsGroup.MoveFocus", mf); }
    }

    g_on = total > 0;
    cmn::journal("dipanel: %d hooks installed", total);
    return g_on;
}

bool ready() { return g_on; }

void enter(int side, uint64_t param, uint64_t agent, uint64_t owner,
           uint64_t group, int32_t ci, int32_t co, int32_t n_mecs) {
    if (side < 1 || side > 2) return;
    std::lock_guard lk(g_mx);
    auto& st = g_side[side];
    st.active = true;
    st.param_addr = param; st.agent_addr = agent;
    st.owner_addr = owner; st.group_addr = group;
    st.saved_ci = ci; st.saved_co = co;
    st.n_mecs = std::min(n_mecs, (int32_t)kMaxMecs);
    st.actions.clear();
    st.last_step = 0; st.last_move = 0;
    for (auto& m : st.mecs) m = {};
    rebuild_maps();
    cmn::journal("dipanel: enter P%d, %d mecs", side, st.n_mecs);
}

void set_spin(int side, int which, uint64_t addr, int32_t mn, int32_t mx, int32_t num) {
    if (side < 1 || side > 2 || which < 1 || which > kMaxMecs) return;
    std::lock_guard lk(g_mx);
    auto& m = g_side[side].mecs[which - 1];
    m.spin_addr = addr; m.mn = mn; m.mx = mx; m.num = num;
    // rebuild maps (spin address may have changed)
    rebuild_maps();
}

void set_text(int side, int which, const std::string& title, const std::string& value) {
    if (side < 1 || side > 2 || which < 1 || which > kMaxMecs) return;
    std::lock_guard lk(g_mx);
    auto& m = g_side[side].mecs[which - 1];
    m.title = title; m.value = value;
}

void set_names(int side, const std::string& row_csv, const std::string& hide_csv) {
    if (side < 1 || side > 2) return;
    std::lock_guard lk(g_mx);
    g_side[side].row_names = split_csv(row_csv);
    g_side[side].hide_names = split_csv(hide_csv);
}

void set_geom(int side, double ref_pos_x, double ref_region_w) {
    if (side < 1 || side > 2) return;
    std::lock_guard lk(g_mx);
    g_side[side].ref_pos_x = ref_pos_x;
    g_side[side].ref_region_w = ref_region_w;
}

void leave(int side) {
    if (side < 1 || side > 2) return;
    std::lock_guard lk(g_mx);
    // B3: set the cancel block BEFORE clearing the maps, so IsDecide/IsCancel stay blocked
    // through the transition (same order as the Lua build: cancel_block set, then S[side] = nil)
    if (g_side[side].active && g_side[side].owner_addr)
        g_cancel[g_side[side].owner_addr] = cmn::frame() + 15;
    g_side[side].active = false;
    g_side[side].actions.clear();
    rebuild_maps();
    cmn::journal("dipanel: leave P%d", side);
}

void set_cancel_block(uint64_t owner_addr, int32_t frames) {
    std::lock_guard lk(g_mx);
    g_cancel[owner_addr] = cmn::frame() + (uint64_t)frames;
}

void pull(int side, int32_t& evt, int32_t& which, int32_t& dir) {
    evt = 0; which = 0; dir = 0;
    if (side < 1 || side > 2) return;
    std::lock_guard lk(g_mx);
    auto& st = g_side[side];
    if (st.actions.empty()) return;
    auto a = st.actions.front();
    st.actions.pop_front();
    evt = a.type; which = a.which; dir = a.dir;
}

static void tick_all() {                   // separated from on_frame: objects with destructors (lock, string)
    perf::Scope timed(perf::DipanelTickAll);
    std::lock_guard lk(g_mx);
    for (int s = 1; s <= 2; ++s) tick_side(s);
}
void on_frame() {
    if (!g_on || !g_any) return;
    __try { tick_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("tick"); }
}

std::string status() {
    std::lock_guard lk(g_mx);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "dipanel: on=%d faults=%d P1=%s P2=%s",
                  g_on ? 1 : 0, g_faults,
                  g_side[1].active ? "active" : "idle",
                  g_side[2].active ? "active" : "idle");
    return buf;
}

}  // namespace screens::dipanel
