#ifndef NOMINMAX
#define NOMINMAX
#endif
// pausemenu - the game side of the training pause-menu engine (increment 9, ADR-0016). See pausemenu.hpp and
// docs/PAUSE_MENU.md; every decision is in pm_core.cpp.
//
// Rules carried as code (reference/04_PITFALLS_CRASHES, and the notes of the engine this one was ported from):
//   R4   the menu's update methods are never called back from their own chain: the page rebuild after a change of a
//        shown spin's labels runs 2 frames later from ANOTHER scroll group's update (never our list's own); the
//        UpdateSpinBox that our own repair's set_SelectedIndex starts repairs nothing (the residual of crash C4; the
//        game's own nested UpdateSpinBox calls are repaired). ViewUpdate(page) right after the page's own Function is
//        the game's own pattern (its record pages chain into it the same way).
//   R5   a row is ours by the GUID of its _MessageID (clones keep it); the game's DYNAMIC rows carry a delegate.
//   R17  UpdateSpinBox hooked by its full signature; rows are data (GUID texts); the one set_Message is the visible
//        option text of our own spin, compared first (R7), through the display API.
//   R18  each method is checked for machine code shared with another method before its hook (folded bodies, jump
//        thunks): shared = the hook is refused, logged with what it costs, the rest of the engine runs.
//   R26  key-guide hints appended to the game's own guide data in GetGuideData's post.
//   R29  hooks run concurrently on the game's UI job threads: per-thread state in thread_locals, the registry under
//        pm_core's lock, the cached menu param under its own lock with a reference held while used; every hook body,
//        the frame and the page builds under one engine lock, as the reference engine ran (see g_eng_mx).

#include "pausemenu.hpp"

#include "../options/ll_common.hpp"   // REFramework C++ API (warnings off), windows.h

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../sf6core_internal.h"
#include "../perf.hpp"
#include "../options/guid_table.hpp"
#include "../options/guid_text.hpp"
#include "../options/options_core.hpp"
#include "pm_core.hpp"

namespace pausemenu {
namespace {

namespace pc = ::pausemenu::core;
namespace gt = ::dialogs::guid_text;
using API = reframework::API;
using MO = API::ManagedObject;
using TD = API::TypeDefinition;

constexpr int kMaxFaults = 5;                  // the engine's hooks off after 5 faults (as the option engine's)
constexpr int32_t kTrainingFlow = 3;           // training
constexpr int32_t kHubTrainingFlow = 9;        // Battle Hub training room
constexpr uint32_t kPauseNone = 64;            // PauseManager._CurrentPauseTypeBit with no menu up
constexpr uint32_t kPauseNone2 = 2112;
constexpr int kGroupMaxDepth = 6;              // a spin part's control is at most 6 levels under its p_Group_<row>_
constexpr int kNest = 8;                       // per-thread nesting kept for UpdateSpinBox / UpdateScroll

std::atomic<bool> g_on{false};
std::atomic<int> g_faults{0};
Calls g_calls;
SF6CoreClient* g_self = nullptr;               // the engine's own client: display writes, texts
const SF6CoreAPI* g_api = nullptr;

// ---- types (resolved once) ----
std::mutex g_types_mx;
bool g_types_done = false;
TD* g_td_menu = nullptr;                       // app.training.TrainingMenuData
API::Field* g_f_msg = nullptr;                 // its _MessageID (System.Guid)
API::Field* g_f_ftype = nullptr;               // its _FuncType
TD* g_td_param = nullptr;                      // app.training.UIFlowTrainingMenu.Param
TD* g_td_spin = nullptr;                       // app.UIPartsSpin
TD* g_td_scroll = nullptr;                     // app.UIPartsScrollList (a spin's carousel part, its child 0)
TD* g_td_guide = nullptr;                      // app.InputGuideData
API::Method* g_m_viewupdate = nullptr;         // app.training.TrainingMenuFunc.ViewUpdate
// TrainingMenuData[] through the game's System.Array.CreateInstance(System.Type, System.Int32), as the reference
// engine's sdk.create_managed_array does inside REFramework. NOT API::create_managed_array: that member of the plugin
// function table came with plugin API 1.13 (REFramework, 03/2025); the REFramework of the players (1.5.8 nightly,
// 12/2024, plugin API 1.10) ends its table before it, so the call read past the table and called whatever followed:
// the "fault in frame" of every install pass on 10/10.
API::Method* g_m_array_new = nullptr;
MO* g_rt_menu = nullptr;                       // typeof(TrainingMenuData) (the VM's runtime type: never freed)

// ---- per-thread state (R29) ----
thread_local pc::Ctx tl_ctx = pc::Ctx::None;   // the row being laid out on this thread
thread_local int64_t tl_ctx_row = -1;
thread_local int64_t tl_building = -1;         // our page being built on this thread (its ViewUpdate)
thread_local MO* tl_build_param = nullptr;     // the menu param of that build
thread_local bool tl_repairing = false;        // this thread is inside the engine's own spin repair

uint64_t now() { return core_frame_pub(); }

// ---- in-game bisection: reframework/data/sf6core/pm_bisect.txt ----
// Each word found in the file turns one behaviour of the engine off, from the next check on (every 2 seconds, with the
// trace file): hints (no key-guide hints), repair (no spin repair after UpdateSpinBox), arrows (no arrow / font writes,
// no put-back), notify (no on_page_shown), barriers (the extra barrier hooks pass through), textbarrier (GetMessage
// passes through), lock (no engine lock at all). For a crash hunt in the game without a rebuild; no file = all on.
enum : uint32_t { kOffHints = 1, kOffRepair = 2, kOffArrows = 4, kOffNotify = 8, kOffBarriers = 16, kOffTextBarrier = 32,
                  kOffLock = 64 };
std::atomic<uint32_t> g_off{0};
bool off(uint32_t f) { return (g_off.load(std::memory_order_relaxed) & f) != 0; }

// ---- the engine lock: one engine at a time, as the reference engine ran ----
// The reference engine's hooks, its frame and its page builds all ran under the one lock of their Lua state: a page
// build (the game's ViewUpdate, run from the page's Function post) held it to the end, so every other thread entering
// one of the hooked menu methods (UpdateScroll of every scroll group, Init / set_Num of the parts, GetGuideData...)
// waited until the rows were rebound. These hooks run concurrently (R29), so the game's GUI jobs ran in the middle of
// our page builds: the one difference with the reference engine left when fast sub-page switching crashed the game on
// a GUI job thread (11/10, tests A2 and A3: a per-frame engine event delivered to a managed delegate already freed;
// the cause is not proven). Every hook body and the frame take this lock (recursive: the build's own nested calls go
// through), a hook with nothing to do takes it as a barrier. The wait
// is bounded (kEngWaitMs): past it the call goes on unlocked (as before this lock) and the wait is counted, so a game
// lock taken in the other order can never become a deadlock.
constexpr int kEngWaitMs = 30;
std::recursive_timed_mutex g_eng_mx;
std::atomic<uint64_t> g_eng_timeouts{0};
std::atomic<bool> g_eng_timeout_logged{false};

bool eng_lock() {
    if (off(kOffLock)) return false;
    if (g_eng_mx.try_lock()) return true;
    if (g_eng_mx.try_lock_for(std::chrono::milliseconds(kEngWaitMs))) return true;
    ++g_eng_timeouts;
    if (!g_eng_timeout_logged.exchange(true))
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] engine lock not taken within %d ms on thread %lu: the call went "
                 "on unlocked (counted in pm_status)", kEngWaitMs, (unsigned long)GetCurrentThreadId());
    return false;
}
void eng_unlock(bool locked) { if (locked) g_eng_mx.unlock(); }
void eng_barrier() { eng_unlock(eng_lock()); }   // waits for a page build of another thread to end


// ======================== diagnostics ========================

// Every thread keeps the engine's step (a short name set before each group of game calls) and the game call under
// way; the SEH filter copies both, with the exception code and address, before the stack unwinds. A fault line then
// says where it happened (the first kDetailedFaults of them).
thread_local const char* tl_step = "-";
thread_local const char* tl_op = "-";
inline void step(const char* s) { tl_step = s; tl_op = "-"; }
inline void op(const char* s) { tl_op = s; }

struct SehInfo {                               // plain data: allowed in a function with a __try
    DWORD code;
    const void* addr;                          // where the exception was raised
    int av;                                    // access violation: 0 read, 1 write, 8 execute; -1 another exception
    uintptr_t av_addr;                         // the address it touched
    const char* step;
    const char* op;
    DWORD tid;
};
int seh_filter(EXCEPTION_POINTERS* ep, SehInfo* si) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    si->code = er ? er->ExceptionCode : 0;
    si->addr = er ? er->ExceptionAddress : nullptr;
    si->av = -1;
    si->av_addr = 0;
    if (er && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        si->av = (int)er->ExceptionInformation[0];
        si->av_addr = (uintptr_t)er->ExceptionInformation[1];
    }
    si->step = tl_step;
    si->op = tl_op;
    si->tid = GetCurrentThreadId();
    return EXCEPTION_EXECUTE_HANDLER;
}

// "module+offset" of a code address.
void where_in_module(const void* a, char* out, size_t n) {
    HMODULE h = nullptr;
    char path[MAX_PATH] = {};
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(a), &h) && h && GetModuleFileNameA(h, path, MAX_PATH)) {
        const char* base = std::strrchr(path, '\\');
        std::snprintf(out, n, "%s+0x%llX", base ? base + 1 : path, (unsigned long long)((uintptr_t)a - (uintptr_t)h));
    } else {
        std::snprintf(out, n, "0x%llX", (unsigned long long)(uintptr_t)a);
    }
}

void describe(const SehInfo& si, char* out, size_t n) {
    char at[200];
    where_in_module(si.addr, at, sizeof at);
    char what[96] = {};
    if (si.code == EXCEPTION_ACCESS_VIOLATION && si.av >= 0)
        std::snprintf(what, sizeof what, ", %s of 0x%llX", si.av == 1 ? "write" : (si.av == 8 ? "execute" : "read"),
                      (unsigned long long)si.av_addr);
    else if (si.code == 0xE06D7363)
        std::snprintf(what, sizeof what, " (a C++ exception)");
    std::snprintf(out, n, "step '%s', call '%s': exception 0x%08lX at %s%s, thread %lu", si.step ? si.step : "?",
                  si.op ? si.op : "?", (unsigned long)si.code, at, what, (unsigned long)si.tid);
}

constexpr int kDetailedFaults = 8;

void fault(const char* where, const SehInfo* si) {
    const int n = ++g_faults;
    if (si && n <= kDetailedFaults) {
        char d[420];
        describe(*si, d, sizeof d);
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] fault in %s (%d): %s", where, n, d);
    } else {
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] fault in %s (%d)", where, n);
    }
    if (n >= kMaxFaults && g_on.exchange(false))
        core_log(SF6CORE_LOG_ERROR, nullptr, "[pausemenu] too many faults: hooks off, the game answers its menu alone");
}

// A file of the core's data folder (reframework/data/sf6core/<name>).
void data_path(const char* name, char* out, size_t n) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* sep = std::strrchr(exe, '\\')) *sep = '\0';
    std::snprintf(out, n, "%s\\reframework\\data\\sf6core\\%s", exe, name);
}

// ---- trace mode: reframework/data/sf6core/pm_trace.txt ----
// While the file exists: the code check, the hooks, the next install pass and the next page build are logged step by
// step (objects found, array sizes, fields read, every write refused), kTraceBudget lines at most. Re-created: another
// round. Off: nothing is formatted (the TR macro tests first).
constexpr int kTraceBudget = 500;
std::atomic<bool> g_trace_armed{false};
std::atomic<int> g_trace_left{0}, g_trace_passes{0}, g_trace_builds{0};
thread_local bool tl_tracing = false;

void trace_check() {                           // any thread; one file-system look
    char p[MAX_PATH];
    data_path("pm_trace.txt", p, sizeof p);
    const bool on = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    if (on && !g_trace_armed.exchange(true)) {
        g_trace_left = kTraceBudget;
        g_trace_passes = 1;
        g_trace_builds = 1;
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] trace on (pm_trace.txt): the code check, the hooks, the next "
                 "install pass and the next page build are logged step by step (%d lines at most)", kTraceBudget);
    } else if (!on && g_trace_armed.exchange(false)) {
        g_trace_left = 0;
        g_trace_passes = 0;
        g_trace_builds = 0;
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] trace off (pm_trace.txt removed)");
    }
}
// pm_bisect.txt (see g_off): read with the trace file, game thread.
void bisect_check() {
    char p[MAX_PATH];
    data_path("pm_bisect.txt", p, sizeof p);
    uint32_t want = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, p, "r") == 0 && f) {
        char buf[512] = {};
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        buf[n] = 0;
        for (char* c = buf; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        static const struct { const char* word; uint32_t bit; } kWords[] = {
            { "textbarrier", kOffTextBarrier }, { "barriers", kOffBarriers }, { "hints", kOffHints },
            { "repair", kOffRepair }, { "arrows", kOffArrows }, { "notify", kOffNotify }, { "lock", kOffLock } };
        for (const auto& w : kWords) {
            const char* at = std::strstr(buf, w.word);
            // "barriers" must not match inside "textbarrier(s)"
            while (at && w.bit == kOffBarriers && at > buf && at[-1] == 't') at = std::strstr(at + 1, w.word);
            if (at) want |= w.bit;
        }
    }
    const uint32_t was = g_off.exchange(want);
    if (was != want)
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] bisect (pm_bisect.txt): off =%s%s%s%s%s%s%s%s", want ? "" : " nothing",
                 (want & kOffHints) ? " hints" : "", (want & kOffRepair) ? " repair" : "", (want & kOffArrows) ? " arrows" : "",
                 (want & kOffNotify) ? " notify" : "", (want & kOffBarriers) ? " barriers" : "",
                 (want & kOffTextBarrier) ? " textbarrier" : "", (want & kOffLock) ? " lock" : "");
}
bool tracing() { return tl_tracing && g_trace_left.load(std::memory_order_relaxed) > 0; }
void tr(const char* fmt, ...) {
    if (!tracing()) return;
    const int left = --g_trace_left;
    if (left < 0) return;
    char buf[440];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] trace: %s", buf);
    if (left == 0)
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] trace: line budget spent (re-create pm_trace.txt for another round)");
}
#define TR(...) do { if (tracing()) tr(__VA_ARGS__); } while (0)

std::string type_name(MO* o) {                 // trace only
    TD* t = o ? o->get_type_definition() : nullptr;
    return t ? t->get_full_name() : std::string(o ? "?" : "null");
}

// ======================== game helpers ========================

// Method lookup, cached per (type, signature literal): a full signature first (through the parent types), then the
// bare name with that many parameters.
std::mutex g_mc_mx;
std::map<std::pair<TD*, const char*>, API::Method*> g_mc;

API::Method* meth(TD* td, const char* sig) {
    if (!td) return nullptr;
    {
        std::lock_guard lk(g_mc_mx);
        auto it = g_mc.find({ td, sig });
        if (it != g_mc.end()) return it->second;
    }
    API::Method* m = nullptr;
    const std::string_view s{ sig };
    const size_t open = s.find('(');
    if (open == std::string_view::npos) {
        m = ll::find_method_noargs(td, sig);
    } else {
        for (TD* t = td; t && !m; t = t->get_parent_type()) m = t->find_method(sig);
        if (!m) {
            const std::string_view bare = s.substr(0, open);
            uint32_t arity = 0;
            if (s.size() > open + 1 && s[open + 1] != ')') { arity = 1; for (char c : s) if (c == ',') ++arity; }
            for (TD* t = td; t && !m; t = t->get_parent_type())
                for (auto* x : t->get_methods())
                    if (x && x->get_name() && bare == x->get_name() && x->get_num_params() == arity) { m = x; break; }
        }
    }
    std::lock_guard lk(g_mc_mx);
    g_mc[{ td, sig }] = m;
    return m;
}

bool call(MO* o, const char* sig, const std::vector<void*>& args, reframework::InvokeRet& out) {
    if (!o) return false;
    op(sig);
    API::Method* m = meth(o->get_type_definition(), sig);
    if (!m) return false;
    std::vector<void*> a = args;
    out = m->invoke(o, a);
    return !out.exception_thrown;
}
MO* call_obj(MO* o, const char* sig, const std::vector<void*>& args = {}) {
    reframework::InvokeRet r;
    return call(o, sig, args, r) ? reinterpret_cast<MO*>(r.ptr) : nullptr;
}
bool call_i32(MO* o, const char* sig, int32_t& v) {
    reframework::InvokeRet r;
    if (!call(o, sig, {}, r)) return false;
    v = (int32_t)r.dword;
    return true;
}
bool call_size(MO* o, const char* sig, float& w, float& h) {     // via.Size {w, h}
    reframework::InvokeRet r;
    if (!call(o, sig, {}, r)) return false;
    std::memcpy(&w, r.bytes.data(), 4);
    std::memcpy(&h, r.bytes.data() + 4, 4);
    return true;
}
bool call_pos(MO* o, float& x, float& y, float& z) {               // via.vec3
    reframework::InvokeRet r;
    if (!call(o, "get_Position", {}, r)) return false;
    std::memcpy(&x, r.bytes.data(), 4);
    std::memcpy(&y, r.bytes.data() + 4, 4);
    std::memcpy(&z, r.bytes.data() + 8, 4);
    return true;
}

MO* field_obj(MO* o, const char* name) {
    if (!o) return nullptr;
    op(name);
    return ll::read_field_object(o, name);
}

int32_t list_count(MO* l) { int32_t n = 0; return (l && call_i32(l, "get_Count", n)) ? n : 0; }
MO* list_item(MO* l, int32_t i) { return call_obj(l, "get_Item(System.Int32)", { ll::arg_i32(i) }); }
int32_t array_length(MO* a) { int32_t n = 0; return (a && call_i32(a, "get_Length", n)) ? n : 0; }
MO* array_get(MO* a, int32_t i) { return call_obj(a, "GetValue(System.Int32)", { ll::arg_i32(i) }); }
bool array_set(MO* a, MO* v, int32_t i) {
    reframework::InvokeRet r;
    return call(a, "SetValue(System.Object, System.Int32)", { v, ll::arg_i32(i) }, r);
}

std::string name_of(MO* ctrl) {
    MO* s = call_obj(ctrl, "get_Name");
    return s ? ll::managed_string_to_utf8(s) : std::string();
}
MO* child(MO* ctrl, const char* name) {      // a direct child by name (read-only, SEH inside the core's query)
    if (!ctrl) return nullptr;
    op(name);
    return reinterpret_cast<MO*>(g_api->ui_find_child(ctrl, name, 0));
}

bool is_menu_data(MO* o) {
    if (!o || (uintptr_t)o <= 0x10000) return false;
    op("get_type_definition");
    return o->get_type_definition() == g_td_menu;
}

// The identity of a TrainingMenuData: its _MessageID (R5).
bool menu_key(MO* o, pc::Key& k) {
    if (!is_menu_data(o)) return false;
    op("_MessageID");
    const void* raw = g_f_msg->get_data_raw(o, false);
    if (!raw) return false;
    std::memcpy(&k.hi, raw, 8);
    std::memcpy(&k.lo, static_cast<const uint8_t*>(raw) + 8, 8);
    return true;
}
int32_t menu_ftype(MO* o) {
    if (!is_menu_data(o)) return -1;
    op("_FuncType");
    const void* raw = g_f_ftype->get_data_raw(o, false);
    int32_t v = -1;
    if (raw) std::memcpy(&v, raw, 4);
    return v;
}
pc::Ident ident_of(MO* o) {
    pc::Key k;
    return menu_key(o, k) ? pc::identify(k) : pc::Ident{};
}
// A list entry (ViewData): its Data, or ParentData on the secondary pages (Data = the option shown).
pc::Ident row_ident_of_entry(MO* vd) {
    pc::Ident i = ident_of(field_obj(vd, "Data"));
    if (i.what == pc::What::None) i = ident_of(field_obj(vd, "ParentData"));
    return i;
}
bool is_our_spin(const pc::Ident& i) {
    return (i.what == pc::What::Row || i.what == pc::What::Option) && !i.removed && i.kind == pc::Kind::Spin;
}

// ---- the menu's param ----
MO* view_entry(MO* param, int32_t idx) {
    MO* l = field_obj(param, "_ViewDataList");
    if (!l || idx < 0 || idx >= list_count(l)) return nullptr;   // shorter during a rebuild
    return list_item(l, idx);
}
MO* group_at(MO* param, int32_t idx) {
    MO* ch = field_obj(field_obj(param, "_SecondaryList"), "_Children");
    if (!ch || idx < 0 || idx >= list_count(ch)) return nullptr;
    return list_item(ch, idx);
}
MO* control_of(MO* part) { return call_obj(part, "get_Control"); }

// The spin part of a row: the UIPartsSpin with a SpinAction in the row's group.
MO* spin_part(MO* param, int32_t idx) {
    MO* ch = field_obj(group_at(param, idx), "_Children");
    const int32_t n = list_count(ch);
    for (int32_t j = 0; j < n; ++j) {
        MO* p = list_item(ch, j);
        if (p && p->get_type_definition() == g_td_spin && field_obj(p, "<SpinAction>k__BackingField")) return p;
    }
    return nullptr;
}
MO* option_list(MO* part) { return child(control_of(part), "p_ScrollList_0_h"); }       // the carousel
MO* option_text(MO* part) { return child(child(option_list(part), "c_selectItem"), "e_txt_0"); }   // the option shown

// The menu param seen last (a reference held while kept, released when the menu closes).
std::mutex g_param_mx;
MO* g_param = nullptr;
std::atomic<uintptr_t> g_sec_list{0};          // its _SecondaryList (our row list's scroll group)

void param_seen(MO* p) {
    if (!p || p->get_type_definition() != g_td_param) return;
    MO* sec = field_obj(p, "_SecondaryList");
    std::lock_guard lk(g_param_mx);
    if (g_param == p) return;
    p->add_ref();
    if (g_param) g_param->release();
    g_param = p;
    g_sec_list = reinterpret_cast<uintptr_t>(sec);
}
MO* param_get() {
    std::lock_guard lk(g_param_mx);
    if (g_param) g_param->add_ref();
    return g_param;
}
void param_put(MO* p) { if (p) p->release(); }
void param_drop() {
    std::lock_guard lk(g_param_mx);
    if (g_param) g_param->release();
    g_param = nullptr;
    g_sec_list = 0;
}

bool page_shown_live(MO* param, int64_t page) {
    if (tl_building == page) return true;
    const pc::Ident i = row_ident_of_entry(view_entry(param, 1));   // the page's first row (0 = the strip row)
    return (i.what == pc::What::Row || i.what == pc::What::Option) && i.page == page;
}

// ---- display writes (ADR-0011: the core's guarded primitives, for the engine's own client) ----
// (a refused write is traced: the guarded primitives never fault, they answer a status)
void set_i64(MO* o, const char* f, int64_t v) {
    op(f);
    const SF6CoreStatus st = g_api->obj_set_i64(g_self, o, f, v);
    if (st != SF6CORE_OK) TR("obj_set_i64 %p.%s = %lld: status %d", (void*)o, f, (long long)v, (int)st);
}
bool set_ref(MO* o, const char* f, void* v) {
    op(f);
    const SF6CoreStatus st = g_api->obj_set_ref(g_self, o, f, v);
    if (st != SF6CORE_OK) TR("obj_set_ref %p.%s = %p: status %d", (void*)o, f, v, (int)st);
    return st == SF6CORE_OK;
}
void set_guid(MO* o, const char* f, const pc::Key& k) {
    uint8_t g[16];
    std::memcpy(g, &k.hi, 8);
    std::memcpy(g + 8, &k.lo, 8);
    op(f);
    const SF6CoreStatus st = g_api->obj_set_guid(g_self, o, f, g);
    if (st != SF6CORE_OK) TR("obj_set_guid %p.%s: status %d", (void*)o, f, (int)st);
}
void invoke_i(MO* o, const char* sig, int64_t v, int32_t type) {
    SF6CoreArg a{};
    a.type = type;
    a.v.i = v;
    op(sig);
    const SF6CoreStatus st = g_api->obj_invoke(g_self, o, sig, &a, 1, nullptr);
    if (st != SF6CORE_OK) TR("obj_invoke %p.%s(%lld): status %d", (void*)o, sig, (long long)v, (int)st);
}

// ---- the game's training data ----
MO* training_manager() {
    op("get_managed_singleton TrainingManager");
    return API::get()->get_managed_singleton("app.training.TrainingManager");
}
MO* menu_tabs() {
    MO* ui = field_obj(training_manager(), "_UIData");
    return ui ? call_obj(ui, "get_MenuData") : nullptr;
}

// The delegate field of DynamicData (AddDynamicMenu's Action): the game's rows carry one, a mod's nil (R5).
std::mutex g_dlg_mx;
TD* g_dlg_td = nullptr;
API::Field* g_dlg_field = nullptr;

bool is_delegate_type(TD* t) {
    for (TD* p = t; p; p = p->get_parent_type()) {
        const char* ns = p->get_namespace();
        const char* nm = p->get_name();
        if (ns && nm && std::string_view{ ns } == "System" &&
            (std::string_view{ nm } == "Delegate" || std::string_view{ nm } == "MulticastDelegate")) return true;
    }
    return false;
}
int has_delegate(MO* dd) {                     // 1 yes, 0 no, -1 unknown (DynamicData has no delegate field)
    if (!dd) return 0;
    TD* td = dd->get_type_definition();
    API::Field* f = nullptr;
    {
        std::lock_guard lk(g_dlg_mx);
        if (g_dlg_td != td) {
            g_dlg_td = td;
            g_dlg_field = nullptr;
            for (TD* t = td; t && !g_dlg_field; t = t->get_parent_type())
                for (auto* x : t->get_fields())
                    if (x && !x->is_static() && x->get_type() && is_delegate_type(x->get_type())) { g_dlg_field = x; break; }
            core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] DynamicData delegate field: %s",
                     g_dlg_field && g_dlg_field->get_name() ? g_dlg_field->get_name() : "none (every unknown DYNAMIC row counts as the game's)");
        }
        f = g_dlg_field;
    }
    if (!f) return -1;
    const void* raw = f->get_data_raw(dd, false);
    return raw && *static_cast<void* const*>(raw) ? 1 : 0;
}

// Every DynamicData of the primary tabs (their DynamicChildData): fn(dynamic_data, its ChildData).
template <class Fn> void each_dynamic(Fn&& fn) {
    MO* arr = menu_tabs();
    const int32_t n = array_length(arr);
    for (int32_t t = 0; t < n; ++t) {
        MO* dl = field_obj(array_get(arr, t), "DynamicChildData");
        const int32_t c = list_count(dl);
        for (int32_t i = 0; i < c; ++i) {
            MO* dd = list_item(dl, i);
            if (dd && fn(dd, field_obj(dd, "ChildData"))) return;
        }
    }
}

bool game_owns(MO* d) {                        // this data is a game DYNAMIC row (its delegate runs on decide)
    bool owned = false;
    each_dynamic([&](MO* dd, MO* cd) {
        if (cd != d) return false;
        owned = has_delegate(dd) != 0;         // unknown field: treated as the game's (passes, as an unknown row)
        return true;
    });
    return owned;
}

void scan_game_rows() {
    bool present = false;
    each_dynamic([&](MO* dd, MO* cd) {
        if (menu_ftype(cd) != pc::kDynamic) return false;
        const int dl = has_delegate(dd);
        if (dl > 0 || (dl < 0 && ident_of(cd).what == pc::What::None)) { present = true; return true; }
        return false;
    });
    if (present != pc::game_rows())
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] game DYNAMIC rows in the menu: %s", present ? "yes" : "no");
    pc::set_game_rows(present);
}

// ======================== backend of the state machine ========================

bool keep_owner(gt::Owner o) { return o && static_cast<const SF6CoreClient*>(o)->active; }

bool be_text_new(const std::string& t, pc::Key& out) {
    gt::GuidBytes gb;
    if (!g_self || !gt::alloc_owned(g_self, t, keep_owner, gb, nullptr)) return false;
    out.hi = gb.hi;
    out.lo = gb.lo;
    return true;
}
bool be_text_set(const pc::Key& k, const std::string& t) {
    gt::GuidBytes gb;
    gb.hi = k.hi; gb.lo = k.lo;
    return g_self && gt::retext_owned(g_self, gb, t);
}
void be_text_release(const pc::Key& k) {
    gt::GuidBytes gb;
    gb.hi = k.hi; gb.lo = k.lo;
    if (g_self) gt::release_owned(g_self, gb);
}

std::string key_hex(const pc::Key& k) {      // trace only
    char b[40];
    std::snprintf(b, sizeof b, "%016llX%016llX", (unsigned long long)k.hi, (unsigned long long)k.lo);
    return b;
}

MO* new_menu_data(int32_t item_type) {
    op("TrainingMenuData create_instance");
    MO* d = g_td_menu->create_instance();
    if (!d) { TR("create_instance(TrainingMenuData) returned null"); return nullptr; }
    op("add_ref");
    d->add_ref();                                        // held: the game keeps it in its arrays (R28)
    set_i64(d, "_Type", item_type);
    set_i64(d, "_FuncType", pc::kDynamic);
    set_i64(d, "IsEnabled", 1);
    set_i64(d, "_Interval", 1);
    set_i64(d, "_Interval_Option", 1);
    set_i64(d, "_GuidIcon", -1);
    set_i64(d, "_GuidAddIcon", -1);
    set_i64(d, "VisibleCase", 0);
    return d;
}
pc::Obj be_new_row_data(int32_t item_type, const pc::Key& title, const pc::Key& guide) {
    step("new_row_data");
    MO* d = new_menu_data(item_type);
    if (!d) return nullptr;
    set_guid(d, "_MessageID", title);
    set_guid(d, "_GuideMessage", guide);
    TR("row data %p: _Type %d, _MessageID %s", (void*)d, item_type, key_hex(title).c_str());
    return d;
}
pc::Obj be_new_option_data(const pc::Key& text) {
    step("new_option_data");
    MO* d = new_menu_data(pc::kItemTextOnly);
    if (d) set_guid(d, "_MessageID", text);
    return d;
}
// An empty TrainingMenuData[n] (see g_m_array_new), or null.
MO* new_menu_array(uint32_t n) {
    if (!g_m_array_new || !g_rt_menu) return nullptr;
    op("System.Array.CreateInstance");
    const reframework::InvokeRet r = g_m_array_new->invoke(nullptr, std::vector<void*>{ g_rt_menu, ll::arg_i32((int32_t)n) });
    if (r.exception_thrown || !r.ptr) { TR("System.Array.CreateInstance(TrainingMenuData, %u) failed", n); return nullptr; }
    MO* a = reinterpret_cast<MO*>(r.ptr);
    op("is_managed_object");
    if (!a->is_managed_object()) { TR("System.Array.CreateInstance gave %p: not a managed object", (void*)a); return nullptr; }
    return a;
}
pc::Obj be_new_array(const std::vector<pc::Obj>& items) {
    step("new_array");
    MO* a = new_menu_array((uint32_t)items.size());
    if (!a) return nullptr;
    op("add_ref");
    a->add_ref();
    for (size_t i = 0; i < items.size(); ++i)
        if (!array_set(a, reinterpret_cast<MO*>(items[i]), (int32_t)i)) {
            TR("SetValue [%zu] of %p refused", i, (void*)a);
            a->release();
            return nullptr;
        }
    TR("array %p (%s): %zu item(s), get_Length %d", (void*)a, type_name(a).c_str(), items.size(), array_length(a));
    return a;
}
bool be_set_children(pc::Obj data, pc::Obj arr) {
    step("set_children");
    return set_ref(reinterpret_cast<MO*>(data), "_ChildData", arr);
}

// A value field copied as it is, a reference field assigned (R34).
void copy_field(MO* dst, MO* src, const char* name) {
    op(name);
    API::Field* f = ll::find_field_walk(src->get_type_definition(), name);
    TD* ft = f ? f->get_type() : nullptr;
    if (!ft) { TR("copy %s: no such field", name); return; }
    void* s = f->get_data_raw(src, false);
    void* d = f->get_data_raw(dst, false);
    if (!s || !d) return;
    if (ft->is_valuetype()) {
        const uint32_t n = ft->get_valuetype_size();
        if (n && n <= 64) std::memcpy(d, s, n);
        TR("copy %s (%s): %u byte(s)%s", name, ft->get_full_name().c_str(), n, (n && n <= 64) ? "" : ", NOT copied");
    } else {
        set_ref(dst, name, *static_cast<void**>(s));
        TR("copy %s (%s): reference %p", name, ft->get_full_name().c_str(), *static_cast<void**>(s));
    }
}
pc::Obj be_new_page_data(pc::Obj template_page, const pc::Key& title, const pc::Key& guide, pc::Obj rows) {
    step("new_page_data");
    MO* tp = reinterpret_cast<MO*>(template_page);
    int32_t type = 0;
    op("_Type");
    if (!tp || !ll::read_field_i32(tp, "_Type", type)) { TR("template page %p: no _Type", (void*)tp); return nullptr; }
    MO* d = new_menu_data(type);                         // typed like the strip's own pages (TAB_ITEM)
    if (!d) return nullptr;
    copy_field(d, tp, "NextButton");
    copy_field(d, tp, "PrevButton");
    set_guid(d, "_MessageID", title);
    set_guid(d, "_GuideMessage", guide);
    set_ref(d, "_ChildData", rows);
    MO* dyn = field_obj(tp, "DynamicChildData");
    MO* empty = call_obj(dyn, "GetRange(System.Int32, System.Int32)", { ll::arg_i32(0), ll::arg_i32(0) });
    if (empty) {                                         // an empty List<DynamicData> of the right type
        op("add_ref");
        empty->add_ref();
        set_ref(d, "DynamicChildData", empty);
    }
    TR("page data %p: _Type %d (template %p), rows %p, DynamicChildData %p -> %p (%s)", (void*)d, type, (void*)tp, rows,
       (void*)dyn, (void*)empty, type_name(empty).c_str());
    return d;
}
pc::Obj be_new_hint(int32_t button, const pc::Key& text) {
    if (!g_td_guide) return nullptr;
    step("new_hint");
    op("InputGuideData create_instance");
    MO* g = g_td_guide->create_instance();
    if (!g) return nullptr;
    op("add_ref");
    g->add_ref();
    set_i64(g, "<Type>k__BackingField", 0);              // InputGuideDataType.DigitalConfig: the glyph of the device in use
    set_i64(g, "<DigitalId>k__BackingField", -1);
    set_i64(g, "<DigitalConfigId>k__BackingField", button);
    set_guid(g, "<MessageId>k__BackingField", text);
    return g;
}
void be_release(pc::Obj o) {
    if (!o) return;
    op("release");
    reinterpret_cast<MO*>(o)->release();
}

bool be_strip_read(int32_t primary_func, pc::Strip& s) {
    step("strip_read: the menu tabs");
    MO* arr = menu_tabs();
    const int32_t n = array_length(arr);
    TR("strip_read(%d): TrainingManager %p, _UIData.MenuData %p (%s), %d tab(s)", primary_func, (void*)training_manager(),
       (void*)arr, type_name(arr).c_str(), n);
    for (int32_t t = 0; t < n; ++t) {
        step("strip_read: a tab");
        MO* tab = array_get(arr, t);
        const int32_t ft = menu_ftype(tab);
        TR("  tab %d: %p, _FuncType %d", t, (void*)tab, ft);
        if (ft != primary_func) continue;
        step("strip_read: the strip row");
        MO* kids = field_obj(tab, "_ChildData");
        const int32_t nk = array_length(kids);
        MO* row = nk > 0 ? array_get(kids, 0) : nullptr;   // the strip is the tab's first row
        int32_t type = 0;
        op("_Type");
        const bool typed = row && ll::read_field_i32(row, "_Type", type);
        TR("  tab %d _ChildData %p: %d row(s); row 0 = %p, _Type %d", t, (void*)kids, nk, (void*)row, typed ? type : -1);
        if (!typed || (type != pc::kStripTwo && type != pc::kStripThree)) return false;
        MO* pages = field_obj(row, "_ChildData");
        if (!pages) return false;
        s.row = row;
        s.type = type;
        s.array = pages;
        step("strip_read: the pages");
        const int32_t np = array_length(pages);
        for (int32_t i = 0; i < np; ++i) {
            MO* p = array_get(pages, i);
            pc::Key k;
            menu_key(p, k);
            s.pages.push_back(p);
            s.keys.push_back(k);
            if (tracing()) {
                int32_t pt = -1;
                ll::read_field_i32(p, "_Type", pt);
                tr("  page %d: %p, _FuncType %d, _Type %d, _MessageID %s", i, (void*)p, menu_ftype(p), pt, key_hex(k).c_str());
            }
        }
        return true;
    }
    return false;
}
std::vector<MO*> g_strip_arrays;                         // the strip arrays we wrote (held for the session)
bool be_strip_write(const pc::Strip& s, const std::vector<pc::Obj>& pages, int32_t type) {
    MO* arr = reinterpret_cast<MO*>(be_new_array(pages));
    step("strip_write");
    if (!arr) return false;
    MO* row = reinterpret_cast<MO*>(s.row);
    if (!set_ref(row, "_ChildData", arr)) { arr->release(); return false; }
    set_i64(row, "_Type", type);
    g_strip_arrays.push_back(arr);
    return true;
}
void be_log(const char* line) { core_log(SF6CORE_LOG_INFO, nullptr, "%s", line); }
// diagnostics: the first install pass after the trace file appeared is traced (game thread)
void be_pass_begin() {
    step("install pass");
    if (g_trace_passes.load(std::memory_order_relaxed) > 0 && g_trace_passes.fetch_sub(1) > 0) {
        tl_tracing = true;
        TR("install pass begins (frame %llu, game thread %lu)", (unsigned long long)now(), (unsigned long)GetCurrentThreadId());
    }
}
void be_pass_end() {
    TR("install pass ends");
    tl_tracing = false;
}
bool be_tracing() { return tracing(); }
void be_trace(const char* line) { tr("%s", line); }

// ======================== R18: one hook per machine code ========================

// The code a function entry runs: its jump thunks followed (jmp rel32 / rel8 / jmp [rip+disp32]), as REFramework's
// add_hook does (ignore_jmp = false).
uintptr_t code_of(uintptr_t p) {
    __try {
        for (int hop = 0; hop < 4 && p; ++hop) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            if (b[0] == 0xE9) p = p + 5 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 1);
            else if (b[0] == 0xEB) p = p + 2 + (intptr_t) * reinterpret_cast<const int8_t*>(b + 1);
            else if (b[0] == 0xFF && b[1] == 0x25) p = *reinterpret_cast<const uintptr_t*>(p + 6 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 2));
            else break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return p;
}

struct FoldTarget {
    API::Method* m = nullptr;
    uint32_t index = 0;
    uintptr_t raw = 0, code = 0;
    int shared = 0;                                       // other methods with the same entry or code
    API::Method* other = nullptr;                          // one of them, for the log
    std::vector<uint32_t> sharers;                         // their method indexes (the first 16)
};

// One pass over every method of the game: who else enters the same code. Reads only (the type database, the methods'
// entries, their first bytes): run on the check's own thread. False when the game closed meanwhile (not finished).
bool fold_scan(std::vector<FoldTarget>& ts) {
    auto* tdb = API::get()->tdb();
    const uint32_t n = tdb->get_num_methods();
    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 0xFFFF) == 0 && core_closing()) return false;
        API::Method* m = tdb->get_method(i);
        if (!m) continue;
        const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
        if (!fn) continue;
        uintptr_t code = 0;
        bool code_done = false;
        for (auto& t : ts) {
            if (!t.raw) continue;
            bool same = fn == t.raw || fn == t.code;
            if (!same) {
                if (!code_done) { code = code_of(fn); code_done = true; }
                same = code && code == t.code;
            }
            const uint32_t mi = m->get_index();
            if (!same || mi == t.index) continue;
            ++t.shared;
            if (!t.other) t.other = m;
            if (t.sharers.size() < 16) t.sharers.push_back(mi);
        }
    }
    return true;
}

std::string method_label(API::Method* m) {
    if (!m) return "?";
    TD* t = m->get_declaring_type();
    return (t ? t->get_full_name() : std::string("?")) + "." + (m->get_name() ? m->get_name() : "?");
}

// ======================== spin repairs ========================

int32_t value_of(const pc::RowInfo& r) {      // the spin's option: the client's, else the one the core kept
    int32_t v = 0;
    if (g_calls.spin_value && g_calls.spin_value(r.owner, r.id, v)) {
        v = pc::clamp_value(r.id, v);
        pc::store_value(r.id, v);
        return v;
    }
    if (!pc::stored_value(r.id, v)) v = 0;
    return pc::clamp_value(r.id, v);
}

// The carousel's own selected index: a press scrolls from it, and a page build leaves it at 0. The carousel is never
// grown (set_ItemCount): a scroll list grown after its build piles its items up unlaid (the game's own count follows
// the option array at each build).
void enforce_index(MO* part, int32_t target) {
    MO* sl = option_list(part);
    int32_t cur = 0;
    if (!sl || !call_i32(sl, "get_SelectedIndex", cur) || cur == target) return;
    invoke_i(sl, "set_SelectedIndex(System.Int32)", target, SF6CORE_ARG_I64);
    if (tracing() && call_i32(sl, "get_SelectedIndex", cur) && cur != target)
        tr("carousel %p: set_SelectedIndex(%d) did not take (%d)", (void*)sl, target, cur);
}

// The visible option text: ours while the page is shown, compared first (R7).
void enforce_text(MO* part, const std::string& want) {
    MO* t = option_text(part);
    if (!t) return;
    MO* cur = call_obj(t, "get_Message");
    if (cur && ll::managed_string_to_utf8(cur) == want) return;
    invoke_i(t, "set_DisableMessageTag(System.Boolean)", 1, SF6CORE_ARG_BOOL);   // names shown literally ("<x>" is no tag)
    g_api->ui_set_text(g_self, t, want.c_str());
}

// Pooled groups whose arrows / option font we changed: put back when they show something else.
struct Moved { bool font = false; float w = 0, h = 0; };
std::mutex g_mv_mx;
std::map<int32_t, Moved> g_moved;
std::atomic<bool> g_any_moved{false};

void mark_moved(int32_t idx, bool font, float w, float h) {
    std::lock_guard lk(g_mv_mx);
    Moved& m = g_moved[idx];
    if (font && !m.font) { m.font = true; m.w = w; m.h = h; }
    g_any_moved = true;
}
bool take_moved(int32_t idx, Moved& out) {
    std::lock_guard lk(g_mv_mx);
    auto it = g_moved.find(idx);
    if (it == g_moved.end()) return false;
    out = it->second;
    g_moved.erase(it);
    g_any_moved = !g_moved.empty();
    return true;
}
std::vector<int32_t> moved_list() {
    std::lock_guard lk(g_mv_mx);
    std::vector<int32_t> v;
    for (auto& [idx, m] : g_moved) v.push_back(idx);
    return v;
}

bool place(MO* el, float x) {
    float px = 0, py = 0, pz = 0;
    if (!el || !call_pos(el, px, py, pz) || std::fabs(px - x) <= pc::kEpsilon) return false;
    g_api->ui_set_position(g_self, el, x, py, pz);
    return true;
}
// The two arrows of the spin part(s) of a pooled group: apart by `spread`, or the game's layout (0).
void arrows(MO* param, int32_t idx, float spread) {
    MO* gc = control_of(group_at(param, idx));
    for (MO* c = call_obj(gc, "get_Child"); c; c = call_obj(c, "get_Next")) {
        if (name_of(c).rfind("p_Spin", 0) != 0) continue;
        place(child(c, "p_MouseDecideOperable_L"), pc::arrow_x(false, spread));
        place(child(c, "p_MouseDecideOperable_R"), pc::arrow_x(true, spread));
    }
    if (spread > 0) mark_moved(idx, false, 0, 0);
}

// A long option text shrunk to the room between the arrows (pm_core::fit: stable, rate-limited).
void fit_row(int64_t row, MO* part, int32_t idx) {
    MO* t = option_text(part);
    float cw = 0, ch = 0, dw = 0, dh = 0;
    if (!t || !call_size(t, "get_FontSize", cw, ch) || !call_size(t, "get_DisplaySize", dw, dh)) return;
    const pc::Fit f = pc::fit(row, cw, ch, dw);
    if (!f.write) return;
    float bw = 0, bh = 0;
    if (pc::base_font(row, bw, bh)) mark_moved(idx, true, bw, bh);   // the game's own font, put back off the page
    g_api->ui_set_font_size(g_self, t, f.w, f.h);
}

void restore_group(MO* param, int32_t idx) {
    Moved m;
    if (!take_moved(idx, m)) return;
    arrows(param, idx, 0.0f);
    if (!m.font) return;
    MO* part = spin_part(param, idx);
    MO* t = part ? option_text(part) : nullptr;
    float cw = 0, ch = 0;
    if (t && call_size(t, "get_FontSize", cw, ch) && std::fabs(cw - m.w) > pc::kEpsilon)
        g_api->ui_set_font_size(g_self, t, m.w, m.h);
}

// The spin part's own range and number (R17: MaxNum, _Num; the Color Mod edits its DI panel spins in place the same
// way). A build sets them (UIPartsSpin.Init, from the option array); a change of the labels while the page is shown did
// not: the deferred ViewUpdate rebuilds the rows' data, not the part. Test A 11/10: 5 -> 7 labels on a shown page, the
// presses still looped over 5 options until the page was left; a client's list shrinking on the shown page (10 -> 9)
// left _Num past the new count and the presses fell on option 0. The range never goes past the option array the game reads
// (min(labels, array) - 1); _Num is set only inside it. Compared first: a build's own values are never rewritten.
void enforce_range(MO* part, const pc::RowInfo& r, int32_t v) {
    const int32_t n = std::min(r.labels_n, r.opt_n);
    if (n < 1) return;
    int32_t mx = 0, num = 0;
    if (call_i32(part, "get_MaxNum", mx) && mx != n - 1) invoke_i(part, "set_MaxNum(System.Int32)", n - 1, SF6CORE_ARG_I64);
    if (v >= 0 && v < n && call_i32(part, "get_Num", num) && num != v) set_i64(part, "_Num", v);
}

// The spin's carousel part (app.UIPartsScrollList): the spin part's child 0. The game's own spin layout (ViewUpdate ->
// the training menu's spin setup) runs UIPartsSpin.Init(0, count - 1, num, action, true), sets the part's CheckAdd /
// CheckSub, then initialises this list with InitList(count, num, -1), count = the row's option array length. A label
// change on the shown page left the list at the count of the last build: after a grow (11 options, list of 10) the
// presses stopped at the last option instead of wrapping and ran on past it unseen (test B step 6, Save As).
MO* carousel_part(MO* part) {
    MO* c = call_obj(part, "GetChild(System.Int32)", { ll::arg_i32(0) });
    return c && g_td_scroll && c->get_type_definition() == g_td_scroll ? c : nullptr;
}
// At a label change on the shown page: the list initialised as the layout does, with the options the array holds now
// (compared first: nothing when the list already has that count). `before` / `after`: its count, -1 = no list.
void init_carousel(MO* part, int32_t n, int32_t v, int32_t& before, int32_t& after) {
    before = after = -1;
    MO* sl = carousel_part(part);
    if (!sl || !call_i32(sl, "get_ItemMax", before)) return;
    after = before;
    if (n < 1 || before == n) return;
    SF6CoreArg a[3]{};
    a[0].type = a[1].type = a[2].type = SF6CORE_ARG_I64;
    a[0].v.i = n;
    a[1].v.i = v < 0 ? 0 : (v >= n ? n - 1 : v);
    a[2].v.i = -1;
    op("UIPartsScrollList.InitList");
    const SF6CoreStatus st = g_api->obj_invoke(g_self, sl, "InitList(System.Int32, System.Int32, System.Int32)", a, 3, nullptr);
    if (st != SF6CORE_OK) TR("InitList on %p: status %d", (void*)sl, (int)st);
    call_i32(sl, "get_ItemMax", after);
}

// After a label change on the shown page, the next states of the spin (its number and range, its list's count and
// index) are logged when they change, 16 lines at most: what the presses did (bounded, INFO).
std::atomic<int> g_range_trace{0};
struct RangeSeen { int64_t row = -1; int32_t v = -2, num = -2, mx = -2, cnt = -2, sel = -2; };
RangeSeen g_range_seen;                        // under the engine lock (repairs run under it)
void trace_range(MO* part, const pc::RowInfo& r, int32_t v) {
    RangeSeen s;
    s.row = r.id;
    s.v = v;
    call_i32(part, "get_Num", s.num);
    call_i32(part, "get_MaxNum", s.mx);
    if (MO* sl = carousel_part(part)) { call_i32(sl, "get_ItemMax", s.cnt); call_i32(sl, "get_SelectedIndex", s.sel); }
    const RangeSeen& o = g_range_seen;
    if (o.row == s.row && o.v == s.v && o.num == s.num && o.mx == s.mx && o.cnt == s.cnt && o.sel == s.sel) return;
    g_range_seen = s;
    if (g_range_trace.fetch_sub(1) <= 0) { g_range_trace = 0; return; }
    core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] row %lld after its label change: option %d of %d, part number %d of 0..%d, "
             "list %d of %d", (long long)r.id, v, r.labels_n, s.num, s.mx, s.sel, s.cnt);
}

// The row's entry in the list is still that row.
bool entry_is(MO* param, int32_t idx, int64_t row) {
    const pc::Ident i = row_ident_of_entry(view_entry(param, idx));
    return (i.what == pc::What::Row || i.what == pc::What::Option) && i.id == row;
}

// A spin row shown with its option, after the game laid the row out, as the reference engine: the carousel's index,
// the visible text (compared first, R7), the font. tl_repairing marks the writes: set_SelectedIndex re-enters
// UpdateSpinBox, whose post then repairs nothing (R4).
void repair_spin(MO* part, const pc::RowInfo& r, int32_t idx) {
    const int32_t v = value_of(r);
    tl_repairing = true;
    step("spin repair: carousel index");
    enforce_index(part, v);
    step("spin repair: option text");
    enforce_text(part, pc::label(r.id, v));
    step("spin repair: range");
    enforce_range(part, r, v);
    if (g_range_trace.load(std::memory_order_relaxed) > 0) { step("spin repair: range trace"); trace_range(part, r, v); }
    tl_repairing = false;
    step("spin repair: font fit");
    if (r.spread > 0) fit_row(r.id, part, idx);
    TR("spin repair row %lld (list index %d): part %p, value %d of %d", (long long)r.id, idx, (void*)part, v, r.labels_n);
}

void notify(const std::vector<pc::Edge>& edges) {
    if (!g_calls.page_shown || off(kOffNotify)) return;
    for (const auto& e : edges) g_calls.page_shown(e.owner, e.page, e.shown);
}

// ======================== hooks ========================

#pragma warning(push)
#pragma warning(disable: 4733)

int32_t arg_i32(void* a) { return static_cast<int32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFF); }

void set_ctx(MO* data) {                      // a TrainingMenuData laid out on this thread
    step("ViewUpdate: row context");
    if (!is_menu_data(data)) return;
    if (menu_ftype(data) != pc::kDynamic) { tl_ctx = pc::Ctx::None; tl_ctx_row = -1; return; }
    const pc::Ident i = ident_of(data);
    if (i.what == pc::What::None) { tl_ctx = pc::Ctx::Foreign; tl_ctx_row = -1; return; }
    tl_ctx = pc::Ctx::Ours;
    tl_ctx_row = i.what == pc::What::Page ? -1 : i.id;
}

// ---- TrainingMenuFunc.ViewUpdate(BaseParam, TrainingMenuData, Int32): the row being laid out ----
int pre_view_update(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    SehInfo si{};
    __try { set_ctx(reinterpret_cast<MO*>(argv[3])); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("ViewUpdate", &si); }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_none(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- the four FuncType-only methods and GetOptionIndex(ftype, wanted, option ftype) ----
thread_local int64_t tl_ret[5];
thread_local bool tl_ret_set[5];

int const_inner(int m, void** argv) {
    step("TrainingMenuFunc (FuncType-only)");
    const pc::Route r = pc::route(arg_i32(argv[2]), tl_ctx, pc::game_rows());
    if (r == pc::Route::Pass) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    tl_ret[m] = pc::const_answer(static_cast<pc::Method>(m), r, m == pc::kGetOptionIndex ? arg_i32(argv[3]) : 0);
    tl_ret_set[m] = true;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
template <int M> int pre_const(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_ret_set[M] = false;
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (arg_i32(argv[2]) != pc::kDynamic) { eng_barrier(); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    SehInfo si{};
    __try { rc = const_inner(M, argv); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("TrainingMenuFunc (FuncType-only)", &si); }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
    return rc;
}
template <int M> void post_const(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_ret_set[M]) return;
    tl_ret_set[M] = false;
    *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(tl_ret[M]));
}

// ---- GetOptionText(ftype) / GetOptionText2(ftype) ----
thread_local std::string tl_text[2];
thread_local bool tl_text_set[2];

int text_inner(int which, void** argv) {
    step("GetOptionText");
    const pc::Route r = pc::route(arg_i32(argv[2]), tl_ctx, pc::game_rows());
    if (r == pc::Route::Pass) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::string s;
    pc::RowInfo ri;
    if (which == 0 && r == pc::Route::Ours && tl_ctx_row >= 0 && pc::row_info(tl_ctx_row, ri) && ri.kind == pc::Kind::Spin)
        s = pc::label(ri.id, value_of(ri));
    tl_text[which] = std::move(s);
    tl_text_set[which] = true;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
void text_out(int which, void** ret_val) {
    step("GetOptionText post");
    op("create_utf8_string");
    MO* s = ll::create_utf8_string(tl_text[which]);       // a fresh string, one extra reference for the game (R6)
    op("add_ref");
    if (s) s->add_ref();
    *ret_val = s;
}
template <int W> int pre_text(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_text_set[W] = false;
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (arg_i32(argv[2]) != pc::kDynamic) { eng_barrier(); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    SehInfo si{};
    __try { rc = text_inner(W, argv); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("GetOptionText", &si); }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
    return rc;
}
template <int W> void post_text(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_text_set[W]) return;
    tl_text_set[W] = false;
    const bool lk = eng_lock();
    SehInfo si{};
    __try { text_out(W, ret_val); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("GetOptionText post", &si); }
    eng_unlock(lk);
}

// ---- Function(ftype, BaseParam, ViewData, value) ----
struct FnState { bool set; int64_t ret; int64_t page; MO* param; };
thread_local FnState tl_fn;

int64_t spin_function(const pc::FnPlan& plan, MO* param, MO* vd, int32_t idx) {
    step("Function: spin");
    pc::RowInfo r;
    if (!pc::row_info(plan.id, r)) return 0;
    int32_t cur = value_of(r);
    step("Function: spin");                   // (value_of may run a client callback)
    if (!plan.at_build) {                     // after a press the part's _Num is the option the game moved to
        MO* part = spin_part(param, idx);
        int32_t num = 0;
        if (part && call_i32(part, "get_Num", num)) {
            const int32_t w = pc::wrap(r.id, num);
            if (w != cur) {
                pc::store_value(r.id, w);
                if (g_calls.on_spin) g_calls.on_spin(r.owner, r.id, w);
            }
        }
        cur = value_of(r);
    }
    if (vd) {
        set_i64(vd, "SliderValue", cur);
        set_i64(vd, "Index", cur);
    }
    if (plan.at_build) {
        // As the reference engine, at the build only: the row text is the option object the ViewData points at (at
        // the build it points at option 0), and the list entry the flow renders from gets the option when its Data is
        // the row itself. No other write into the game's view data (the entry's Data and a press's entry were written
        // since 0.8.0's second test build, a difference with the reference engine: taken out after the third crash).
        pc::Obj o = vd ? pc::option_data(r.id, cur) : nullptr;
        if (o) set_ref(vd, "Data", o);
        MO* e = view_entry(param, idx);
        pc::Key k;
        const bool same = e && menu_key(field_obj(e, "Data"), k) && k == r.key;
        if (same) {
            set_i64(e, "Index", cur);
            set_i64(e, "SliderValue", cur);
        }
        TR("spin row %lld at build (list index %d): value %d, option data %p, ViewData %p (refcount %d), list entry %p "
           "(refcount %d)%s", (long long)r.id, idx, cur, o, (void*)vd, vd ? *reinterpret_cast<const int32_t*>(
           reinterpret_cast<const uint8_t*>(vd) + 8) : 0, (void*)e, e ? *reinterpret_cast<const int32_t*>(
           reinterpret_cast<const uint8_t*>(e) + 8) : 0, same ? "" : " (not this row: not written)");
    }
    return 0;
}

int function_inner(void** argv) {
    step("Function");
    const int32_t ft = arg_i32(argv[2]);
    MO* param = reinterpret_cast<MO*>(argv[3]);
    MO* vd = reinterpret_cast<MO*>(argv[4]);
    const int32_t idx = arg_i32(argv[5]);
    if (ft != pc::kDynamic) {
        // a game page of a strip we use was selected: our pages there are no longer shown
        if (pc::any_shown()) {
            MO* gd = field_obj(vd, "Data");
            if (gd && pc::is_game_page(gd)) notify(pc::game_page_built(gd));
        }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    MO* d = field_obj(vd, "Data");
    pc::FnInput in;
    in.ftype = ft;
    in.frame = now();
    in.row_index = idx;
    in.ctx = tl_ctx;
    in.ctx_row = tl_ctx_row;
    in.data_null = d == nullptr;
    in.data = ident_of(d);
    if (in.data.what == pc::What::None && d) in.parent = ident_of(field_obj(vd, "ParentData"));
    if (d && in.data.what == pc::What::None && in.parent.what == pc::What::None) in.game_owned = game_owns(d);
    const pc::FnPlan plan = pc::plan_function(in);
    TR("Function(345, list index %d): data %p (ours: %d), plan %d, id %lld, at build %d", idx, (void*)d,
       (int)in.data.what, (int)plan.act, (long long)plan.id, plan.at_build ? 1 : 0);
    switch (plan.act) {
    case pc::FnAct::Pass:
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;          // the game's rows and unknown rows (ADR-0016 point 2)
    case pc::FnAct::Retired:
        tl_fn = { true, 0, -1, nullptr };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    case pc::FnAct::Page:                              // our page selected in its strip: built in the post-hook
        param_seen(param);
        tl_ctx = pc::Ctx::Ours; tl_ctx_row = -1;
        tl_fn = { true, 0, plan.id, param };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    case pc::FnAct::Button: {
        tl_ctx = pc::Ctx::Ours; tl_ctx_row = plan.id;
        int64_t ret = 0;
        if (!plan.at_build) {                          // a decide: synchronous, on this thread (ADR-0016 point 4)
            step("Function: decide (client callback)");
            if (g_calls.decide) g_calls.decide(plan.owner, plan.id);
            ret = plan.keep_open ? 0 : 1;              // 1 = the game closes the menu
        }
        tl_fn = { true, ret, -1, nullptr };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    case pc::FnAct::Spin:
        tl_ctx = pc::Ctx::Ours; tl_ctx_row = plan.id;
        param_seen(param);
        tl_fn = { true, spin_function(plan, param, vd, idx), -1, nullptr };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_function(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_fn = FnState{ false, 0, -1, nullptr };
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    SehInfo si{};
    __try { rc = function_inner(argv); }
    __except (seh_filter(GetExceptionInformation(), &si)) {
        fault("Function", &si);
        rc = tl_fn.set ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
    return rc;
}

// Our page in its strip: shown, rows marked "at build", option arrays sized, then the rows built by the game's
// ViewUpdate(page) on this same thread, as its own record pages do after their Function.
void view_update(MO* param, int64_t page) {
    MO* pd = reinterpret_cast<MO*>(pc::page_data(page));
    if (!pd || !g_m_viewupdate || !param) return;
    step("page build: ViewUpdate(page)");
    op("TrainingMenuFunc.ViewUpdate");
    TR("page %lld: ViewUpdate(param %p, page data %p, 0) on thread %lu", (long long)page, (void*)param, (void*)pd,
       (unsigned long)GetCurrentThreadId());
    tl_building = page;
    tl_build_param = param;
    const auto r = g_m_viewupdate->invoke(nullptr, std::vector<void*>{ param, pd, ll::arg_i32(0) });
    tl_building = -1;
    tl_build_param = nullptr;
    if (r.exception_thrown) core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] ViewUpdate of page %lld threw", (long long)page);
    TR("page %lld: ViewUpdate done%s", (long long)page, r.exception_thrown ? " (threw)" : "");
}
void page_build(int64_t page, MO* param) {
    step("page build: shown state");
    pc::building(page);                        // labels the owner sets from on_page_shown need no rebuild
    notify(pc::set_shown(page));
    step("page build: option arrays");
    pc::page_built(page, now());
    view_update(param, page);
}
void post_function(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_fn.set) return;
    const FnState s = tl_fn;
    tl_fn = FnState{ false, 0, -1, nullptr };
    *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(s.ret));
    if (s.page < 0 || !g_on.load(std::memory_order_relaxed) || core_closing()) return;
    const bool lk = eng_lock();                // held to the end of the build: no hooked menu work of another thread
    int64_t pt = perf::begin();                //  runs while the game rebinds the rows (the reference engine's model)
    // the first page build after the trace file appeared is traced (this thread, the game's layout included)
    if (g_trace_builds.load(std::memory_order_relaxed) > 0 && g_trace_builds.fetch_sub(1) > 0) tl_tracing = true;
    SehInfo si{};
    __try { page_build(s.page, s.param); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("Function post (page build)", &si); }
    tl_building = -1;
    tl_build_param = nullptr;
    tl_repairing = false;
    tl_tracing = false;
    // REFramework keeps one return slot per thread and hook: the rows' own Function calls of the build, nested in this
    // post (REFramework logs "(Post) Recursive hook detected" once per thread for them: expected), wrote theirs into
    // it. The page's answer is written again, after them.
    *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(s.ret));
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
}

// ---- UIFlowTrainingMenu.Param.UpdateSpinBox(Int32, Boolean, Boolean): the spin repairs after the game's layout ----
struct SbFrame { int64_t row; MO* param; int32_t idx; };
thread_local SbFrame tl_sb[kNest];
thread_local int tl_sb_n = 0;

void spinbox_pre(int d, MO* param, int32_t idx) {
    step("UpdateSpinBox");
    tl_sb[d].param = param;
    tl_sb[d].idx = idx;
    op("get_type_definition");
    if (!pc::any_shown() || !param || param->get_type_definition() != g_td_param) return;
    // the row at that index must be OUR data: the row list is shared by every page of the menu
    const pc::Ident i = row_ident_of_entry(view_entry(param, idx));
    if (!is_our_spin(i)) return;
    tl_sb[d].row = i.id;
    tl_ctx = pc::Ctx::Ours;
    tl_ctx_row = i.id;
    pc::set_row_index(i.id, idx);
    param_seen(param);
}
int pre_spinbox(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (tl_sb_n < 0 || tl_sb_n >= kNest) tl_sb_n = 0;   // posts that never ran (a call left by an exception) never pile up
    const int d = tl_sb_n++;
    tl_sb[d] = SbFrame{ -1, nullptr, -1 };
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!pc::any_shown()) { eng_barrier(); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    SehInfo si{};
    __try { spinbox_pre(d, reinterpret_cast<MO*>(argv[1]), arg_i32(argv[2])); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("UpdateSpinBox", &si); }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// Every post of our row repairs it, nested or not: the game nests this method itself (REFramework's "(Overall)
// UpdateSpinBox appears to be calling itself" while no page of ours was shown, 11/10, and at presses), and a press laid
// out in a nested call got no repair there. Only the re-entry from our own repair (set_SelectedIndex) is skipped
// (tl_repairing, R4).
void spinbox_post(const SbFrame f) {
    step("UpdateSpinBox post: spin part");
    pc::RowInfo r;
    MO* part = pc::row_info(f.row, r) ? spin_part(f.param, f.idx) : nullptr;
    if (!part) { TR("spin repair row %lld (list index %d): no spin part", (long long)f.row, f.idx); return; }
    repair_spin(part, r, f.idx);
}
void post_spinbox(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int d = --tl_sb_n;
    if (d < 0) { tl_sb_n = 0; return; }
    if (d >= kNest || !tl_sb[d].param || !g_on.load(std::memory_order_relaxed) || core_closing()) return;
    if (tl_repairing) return;                 // our own set_SelectedIndex re-entered it (R4): the repair under way goes on
    if (tl_sb[d].row < 0 || off(kOffRepair)) { eng_barrier(); return; }   // a game row: never touched here (put-backs run
                                                                          // from the row list's frame, as the reference engine)
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    SehInfo si{};
    __try { spinbox_post(tl_sb[d]); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("UpdateSpinBox post", &si); }
    tl_repairing = false;
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
}

// ---- UIPartsSpin.Init(min, max, num, action[, bool]) / set_Num(num): the option shown after a build or a focus ----
// The game initialises the spin part with its own num (0 for ours) at every build and focus, then calls set_Num with
// it: num replaced by the row's option, and set_Num held for kForceFrames after Init.
bool row_of_part(MO* part, int32_t& idx, MO*& gctrl) {
    MO* c = control_of(part);
    for (int i = 0; i < kGroupMaxDepth && c; ++i) {
        const std::string nm = name_of(c);
        if (nm.rfind("p_Group_", 0) == 0) {           // p_Group_<row>_...
            size_t p = 8;
            int32_t v = 0;
            while (p < nm.size() && nm[p] >= '0' && nm[p] <= '9') v = v * 10 + (nm[p++] - '0');
            if (p == 8 || p >= nm.size() || nm[p] != '_') return false;
            idx = v;
            gctrl = c;
            return true;
        }
        c = call_obj(c, "get_Parent");
    }
    return false;
}
int64_t spin_row_at(int32_t idx, MO* gctrl) {
    pc::RowInfo r;
    if (tl_building >= 0) {                   // our page's build on this thread: the index Function recorded
        int64_t row = pc::row_at(idx);
        if (row < 0 && tl_build_param) {      // the part's Init came before the row's Function (first build of a menu):
            const pc::Ident i = row_ident_of_entry(view_entry(tl_build_param, idx));   // its list entry says
            if (is_our_spin(i)) { row = i.id; pc::set_row_index(row, idx); }
        }
        return row >= 0 && pc::row_info(row, r) && r.page == tl_building && r.kind == pc::Kind::Spin && !r.removed ? row : -1;
    }
    MO* param = param_get();
    if (!param) return -1;
    int64_t out = -1;
    if (control_of(group_at(param, idx)) == gctrl) {   // this part really is in row idx of the menu's list
        const pc::Ident i = row_ident_of_entry(view_entry(param, idx));
        if (is_our_spin(i)) out = i.id;
    }
    param_put(param);
    return out;
}
void num_inner(void** argv, int slot, bool init) {
    step(init ? "UIPartsSpin.Init" : "UIPartsSpin.set_Num");
    MO* part = reinterpret_cast<MO*>(argv[1]);
    op("get_type_definition");
    if (!part || part->get_type_definition() != g_td_spin) return;
    int32_t idx = -1;
    MO* gctrl = nullptr;
    if (!row_of_part(part, idx, gctrl)) return;
    const int64_t row = spin_row_at(idx, gctrl);
    if (row < 0) return;
    const uint64_t f = now();
    if (!init && !pc::forcing(row, f)) return;
    pc::RowInfo r;
    if (!pc::row_info(row, r)) return;
    const int32_t target = value_of(r);
    if (init) pc::force(row, f);
    if (arg_i32(argv[slot]) != target) argv[slot] = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(target)));
}
template <int Slot, bool Init> int pre_num(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!pc::any_shown()) { eng_barrier(); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    SehInfo si{};
    __try { num_inner(argv, Slot, Init); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault(Init ? "UIPartsSpin.Init" : "UIPartsSpin.set_Num", &si); }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- TrainingManager.GetGuideData(focused data, ref List<InputGuideData>, ref List<String>): hints (R26) ----
thread_local void* tl_guide_focus = nullptr;
thread_local void** tl_guide_slot = nullptr;

int pre_guide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_guide_slot = nullptr;
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    eng_barrier();
    if (!g_calls.any_hints || !g_calls.any_hints() || off(kOffHints)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    tl_guide_focus = argv[2];
    tl_guide_slot = reinterpret_cast<void**>(argv[3]);    // the lists are created inside the call: read in the post
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void guide_post(void* focus, void** slot) {
    step("GetGuideData post: the list");
    op("the guide list slot");
    MO* list = slot ? reinterpret_cast<MO*>(*slot) : nullptr;
    if (!list) return;
    std::vector<Hint> hints;
    step("GetGuideData post: hints (client callbacks)");
    g_calls.hints(focus, hints);
    step("GetGuideData post: add");
    for (const Hint& h : hints)
        if (pc::Obj o = pc::hint_obj(h.owner, h.button, h.text)) { op("list_add"); g_api->list_add(g_self, list, o); }
}
void post_guide(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_guide_slot) return;
    void** slot = tl_guide_slot;
    void* focus = tl_guide_focus;
    tl_guide_slot = nullptr;
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return;
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    SehInfo si{};
    __try { guide_post(focus, slot); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("GetGuideData post", &si); }
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
}

// ---- UIPartsGroupScroll.UpdateScroll: per frame on the menu's thread ----
thread_local uintptr_t tl_scroll[kNest];
thread_local int tl_scroll_n = 0;

void rebuild(MO* param, int64_t page) {
    step("UpdateScroll post: rebuild");
    pc::page_built(page, now());              // rows "at build" (no decide, no change), arrays sized to the labels
    view_update(param, page);
}
void per_frame(MO* param, int64_t page) {
    step("UpdateScroll post: retext");
    for (int64_t rid : pc::take_retext(page)) {           // labels changed: the visible text at once
        pc::RowInfo r;
        if (!pc::row_info(rid, r) || r.kind != pc::Kind::Spin || r.row_index < 0 || !entry_is(param, r.row_index, rid)) continue;
        MO* part = spin_part(param, r.row_index);
        if (!part) continue;
        int32_t mx0 = -1, num0 = -1, mx1 = -1, num1 = -1;
        call_i32(part, "get_MaxNum", mx0);
        call_i32(part, "get_Num", num0);
        const int32_t arr0 = r.opt_n;
        // the range first brought inside what both the labels and the current array allow, then the array sized to the
        // labels (the reference engine's prepare_page at a list change) and the range with it: the part never points
        // past the array the game reads
        repair_spin(part, r, r.row_index);
        if (pc::size_options(rid) && pc::row_info(rid, r)) repair_spin(part, r, r.row_index);
        // the carousel list as the layout initialises it (InitList(count, num, -1)), then the repair once more (its index,
        // text and range follow the list)
        int32_t cnt0 = -1, cnt1 = -1;
        tl_repairing = true;
        init_carousel(part, std::min(r.labels_n, r.opt_n), value_of(r), cnt0, cnt1);
        tl_repairing = false;
        if (cnt0 != cnt1) repair_spin(part, r, r.row_index);
        call_i32(part, "get_MaxNum", mx1);
        call_i32(part, "get_Num", num1);
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] row %lld: labels changed on the shown page: %d option(s), array %d -> %d, "
                 "part range 0..%d -> 0..%d, number %d -> %d, list count %d -> %d", (long long)rid, r.labels_n, arr0, r.opt_n, mx0,
                 mx1, num0, num1, cnt0, cnt1);
        g_range_seen = RangeSeen{};
        g_range_trace = 16;
    }
    if (off(kOffArrows)) return;
    std::vector<int32_t> kept;
    step("UpdateScroll post: font fit and arrows");
    for (int64_t rid : pc::page_rows(page)) {
        pc::RowInfo r;
        if (!pc::row_info(rid, r) || r.kind != pc::Kind::Spin || r.spread <= 0 || r.row_index < 0) continue;
        if (!entry_is(param, r.row_index, rid)) continue;
        MO* part = spin_part(param, r.row_index);
        if (!part) continue;
        fit_row(rid, part, r.row_index);      // the game's focus state puts its own font back: checked per frame
        arrows(param, r.row_index, r.spread);
        kept.push_back(r.row_index);
    }
    step("UpdateScroll post: restore");
    for (int32_t idx : moved_list())
        if (std::find(kept.begin(), kept.end(), idx) == kept.end()) restore_group(param, idx);
}
void scroll_post(bool our_list) {
    step("UpdateScroll post");
    MO* param = param_get();
    if (!param) return;
    if (!our_list) {
        // R4: the rebuild after a label change runs here, in ANOTHER group's update, 2 frames after the change, held
        // while an option window is up, dropped if the page is no longer shown
        const int64_t p = pc::take_rebuild(now(), dialogs::options::core::window_open());
        if (p >= 0 && page_shown_live(param, p)) rebuild(param, p);
    } else {
        const int64_t p = pc::shown_page();
        if (p >= 0 && page_shown_live(param, p)) per_frame(param, p);
        else if (!off(kOffArrows)) for (int32_t idx : moved_list()) restore_group(param, idx);
    }
    param_put(param);
}
int pre_scroll(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (tl_scroll_n < 0 || tl_scroll_n >= kNest) tl_scroll_n = 0;   // posts that never ran never pile up
    const int d = tl_scroll_n++;
    tl_scroll[d] = reinterpret_cast<uintptr_t>(argv[1]);
    if (g_on.load(std::memory_order_relaxed) && !core_closing()) eng_barrier();   // no scroll group moves during a build
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_scroll(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int d = --tl_scroll_n;
    if (d < 0) { tl_scroll_n = 0; return; }
    if (d >= kNest || !g_on.load(std::memory_order_relaxed) || core_closing()) return;
    const uintptr_t ours = g_sec_list.load(std::memory_order_relaxed);
    const bool our_list = ours && tl_scroll[d] == ours;
    if (our_list ? (!pc::any_shown() && !g_any_moved.load(std::memory_order_relaxed)) : !pc::rebuild_pending()) {
        eng_barrier();
        return;
    }
    const bool lk = eng_lock();
    int64_t pt = perf::begin();
    SehInfo si{};
    __try { scroll_post(our_list); }
    __except (seh_filter(GetExceptionInformation(), &si)) { fault("UpdateScroll post", &si); }
    tl_building = -1;
    tl_build_param = nullptr;
    tl_repairing = false;
    perf::end(perf::PmHooks, pt);
    eng_unlock(lk);
}

// ---- barrier hooks: the menu's own navigation waits for a page build of another thread ----
// The reference engine also hooked the training menu's navigation (tab and sub-tab inputs, the page index, the list
// updates, the record / replay row updaters, the cursor, the list's next / previous row, a spin's left / right) and the
// game's text lookup: each such call entered its Lua lock, so none of them ran while a page of ours was being built,
// and its page builds never overlapped the game's own page switch. These hooks do the same: the pre waits for the
// engine lock and gives it back at once (a hook with nothing else to do). R18 checks each one like the others.
int pre_barrier(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_on.load(std::memory_order_relaxed) && !core_closing() && !off(kOffBarriers)) eng_barrier();
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

#pragma warning(pop)

// ======================== frame (game thread) ========================

uint64_t g_training_checked = 0;
bool g_training = false;
bool g_menu_up = false;

bool pause_up() {
    op("get_managed_singleton PauseManager");
    MO* pm = API::get()->get_managed_singleton("app.PauseManager");
    uint32_t b = kPauseNone;
    op("_CurrentPauseTypeBit");
    return pm && ll::read_field_u32(pm, "_CurrentPauseTypeBit", b) && b != kPauseNone && b != kPauseNone2;
}

void frame_inner(uint64_t f) {
    step("frame: flow");
    const int32_t flow = g_api->get_flow_id();
    const bool training_flow = flow == kTrainingFlow || flow == kHubTrainingFlow;
    if (f % 120 == 7) { step("frame: trace and bisect files"); trace_check(); bisect_check(); }
    if (!training_flow) g_training = false;
    else if (f - g_training_checked >= 60 || g_training_checked == 0) {
        step("frame: training check");
        MO* tm = training_manager();
        g_training = tm && field_obj(tm, "_tData");
        g_training_checked = f;
    }
    step("frame: pause check");
    const bool paused = g_training && (pause_up() || dialogs::options::core::window_open());
    if (g_menu_up && !paused) {               // the menu closed: nothing shown, the menu's param let go
        step("frame: menu closed");
        notify(pc::menu_closed());
        param_drop();
    }
    g_menu_up = paused;
    step("frame: state machine");             // the install pass sets its own steps (pass_begin and the backend)
    pc::tick(f, g_training, paused);
    if (g_training && !paused && f % 60 == 20) { step("frame: game rows"); scan_game_rows(); }
}
bool frame_seh(uint64_t f, SehInfo* si) {
    __try { frame_inner(f); }
    __except (seh_filter(GetExceptionInformation(), si)) { return false; }
    return true;
}

// ======================== install ========================

struct Spec {
    const char* type;
    const char* sig;                          // full signature, or the name (the first method so called: as the
                                              // reference engine's get_method)
    REFPreHookFn pre;
    REFPostHookFn post;
    bool required;
    const char* without;                      // what the engine loses when the hook is refused
    API::Method* m = nullptr;
    bool hooked = false, refused = false;
    int shared = 0;
    std::string other;
    uintptr_t raw = 0, code = 0;
};
std::mutex g_specs_mx;
std::vector<Spec> g_specs;                    // made once (make_specs), before the code check's thread starts
std::string g_refused;

std::string spec_label(const Spec& s) { return std::string(s.type) + "." + s.sig; }

constexpr const char* kBarrierLoss = "that navigation step does not wait for a page build of another thread";

void make_specs() {                           // under g_specs_mx
    if (!g_specs.empty()) return;
    const char* tmf = "app.training.TrainingMenuFunc";
    g_specs = {
        { tmf, "GetIsActive", pre_const<pc::kGetIsActive>, post_const<pc::kGetIsActive>, false,
          "the game's own answer for the clients' rows (as for its own DYNAMIC rows)" },
        { tmf, "IsValueType", pre_const<pc::kIsValueType>, post_const<pc::kIsValueType>, false,
          "the game's own answer for the clients' rows" },
        { tmf, "GetVisibleCase", pre_const<pc::kGetVisibleCase>, post_const<pc::kGetVisibleCase>, false,
          "the game's own answer for the clients' rows" },
        { tmf, "IsChangedValue", pre_const<pc::kIsChangedValue>, post_const<pc::kIsChangedValue>, false,
          "the game's own answer for the clients' rows" },
        { tmf, "ViewUpdate", pre_view_update, post_none, false,
          "no row context from the layout (UpdateSpinBox and Function still give it)" },
        { tmf, "GetOptionIndex", pre_const<pc::kGetOptionIndex>, post_const<pc::kGetOptionIndex>, false,
          "the spins' display probes answered by the game" },
        { tmf, "GetOptionText", pre_text<0>, post_text<0>, false,
          "the game's option text for the clients' spins (the visible text is still repaired after UpdateSpinBox)" },
        { tmf, "GetOptionText2", pre_text<1>, post_text<1>, false, "the game's second option text for the clients' rows" },
        { tmf, "Function", pre_function, post_function, true, "no page, no decide: the engine is off" },
        { "app.training.UIFlowTrainingMenu.Param", "UpdateSpinBox(System.Int32, System.Boolean, System.Boolean)",
          pre_spinbox, post_spinbox, false, "no spin repair after the game's layout (carousel index, option text, font)" },
        { "app.UIPartsSpin", "Init(System.Int32, System.Int32, System.Int32, System.Action)", pre_num<4, true>,
          post_none, false, "a spin shows option 0 after a build or a focus until the next layout repair" },
        { "app.UIPartsSpin", "Init(System.Int32, System.Int32, System.Int32, System.Action, System.Boolean)",
          pre_num<4, true>, post_none, false, "a spin shows option 0 after a build or a focus until the next layout repair" },
        { "app.UIPartsSpin", "set_Num(System.Int32)", pre_num<2, false>, post_none, false,
          "the flow's own set_Num right after Init is not held: a spin may start a press from option 0 after a build or "
          "a focus (its text is repaired at the next layout)" },
        { "app.training.TrainingManager", "GetGuideData", pre_guide, post_guide, false, "no key-guide hints" },
        { "app.UIPartsGroupScroll", "UpdateScroll", pre_scroll, post_scroll, false,
          "no per-frame font fit / arrow reset, no page rebuild after a change of a shown spin's labels (the page shows "
          "them at its next build)" },
        // barrier hooks (pre_barrier): the methods the reference engine also hooked on the menu's navigation path
        { "app.UIAgent", "InputTab", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIAgent", "InputTabNext", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIAgent", "InputTabBack", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIAgent", "InputChildTab", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIAgent", "InputChildTabNext", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIAgent", "InputChildTabBack", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "OnChangedTab", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "ChangeTabIndex", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "UpdatePrimaryList", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "UpdateSecondaryList", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "CreateViewData", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "set_SelectChildIndex(System.Int32)", pre_barrier, post_none, false,
          kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "UpdateRecordSlotItem(app.UIPartsGroup, System.Int32)", pre_barrier,
          post_none, false, kBarrierLoss },
        { "app.training.UIFlowTrainingMenu.Param", "UpdatePlaySlotItem(app.UIPartsGroup, System.Int32)", pre_barrier,
          post_none, false, kBarrierLoss },
        { "app.UIPartsPageIndex", "set_PageIndex", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIPartsPageIndex", "Prev", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIPartsPageIndex", "Next", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIPartsListBase", "GetNextIndex", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIPartsListBase", "GetPrevIndex", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIPartsSpin", "InputRight(app.InputDigitalFlag)", pre_barrier, post_none, false, kBarrierLoss },
        { "app.UIPartsSpin", "InputLeft(app.InputDigitalFlag)", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.TrainingManager", "ChangeMainMenuFlow", pre_barrier, post_none, false, kBarrierLoss },
        { "app.training.UITrainingMenu", "ChangeFlow", pre_barrier, post_none, false, kBarrierLoss },
    };
}

// ---- R18: the code check, on a thread of its own, and its cache ----
// The type database and the game's code are static and the check only reads them; the REFramework lookups it uses take
// their own locks (find_type, get_method, get_function, get_full_name: shared_mutex each). So it runs once on a thread
// of its own, started by the first pm_set_callbacks (6.5 s over every method on 10/10, the game frozen meanwhile when
// it ran on the game thread); the game thread only polls an atomic, and installs the hooks from the frame after the
// check ended, one per frame. The result is kept in reframework/data/sf6core/pm_codecheck.txt, keyed by the game's
// executable (PE header, file size and time) and its method count: the next sessions read it instead of walking every
// method again (REFramework then writes none of its "Encoded offset is 0" lines either: 345k of them on 10/10).
struct Check {
    std::vector<FoldTarget> targets;          // one per spec
    bool scanned = false;                     // every method compared, or a cache that matches this game
    bool cache_hit = false, cache_written = false, faulted = false;
    uint32_t methods = 0;
    uint64_t ms = 0;
    DWORD tid = 0;
    char key[200] = {};
    char cache_note[160] = {};                // why the cache was not used
    SehInfo fault{};
};
Check g_check;                                // written by the check's thread, then g_check_state = 2 (release)
std::atomic<int> g_check_state{0};            // 0 not started, 1 running, 2 done

constexpr const char* kCacheFile = "pm_codecheck.txt";
constexpr const char* kCacheHeader = "sf6core pause-menu code check 1";

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
long long rva(uintptr_t a) { return a ? (long long)(a - exe_base()) : 0; }

void exe_key(char* out, size_t n, uint32_t methods) {
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    unsigned long long size = 0, time = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        time = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    std::snprintf(out, n, "exe %08lX %08lX %08lX size %llu time %llX methods %u",
                  (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage,
                  (unsigned long)nt->OptionalHeader.CheckSum, size, time, methods);
}

void chomp(char* s) {
    size_t n = std::strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

// The cached sharing of every target, when the file is of this game and each target is where it was (entry and code).
bool cache_read() {
    char path[MAX_PATH];
    data_path(kCacheFile, path, sizeof path);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) { std::snprintf(g_check.cache_note, sizeof g_check.cache_note, "no cache file yet"); return false; }
    std::vector<FoldTarget> got = g_check.targets;
    char line[1024];
    const char* why = nullptr;
    if (!std::fgets(line, sizeof line, f)) why = "empty cache file";
    else { chomp(line); if (std::strcmp(line, kCacheHeader) != 0) why = "another cache format"; }
    if (!why) {
        if (!std::fgets(line, sizeof line, f)) why = "no key";
        else { chomp(line); if (std::strncmp(line, "key ", 4) != 0 || std::strcmp(line + 4, g_check.key) != 0) why = "another game executable"; }
    }
    for (size_t i = 0; !why && i < g_specs.size(); ++i) {
        if (!std::fgets(line, sizeof line, f)) { why = "targets missing"; break; }
        chomp(line);
        char* bar = std::strchr(line, '|');
        if (!bar) { why = "unreadable target line"; break; }
        *bar = 0;
        if (spec_label(g_specs[i]) != bar + 1) { why = "the hooked methods changed"; break; }
        int found = 0, shared = 0, nsh = 0, pos = 0;
        unsigned index = 0;
        long long raw = 0, code = 0, other = -1;
        if (sscanf_s(line, "t %d %u %lld %lld %d %lld %d%n", &found, &index, &raw, &code, &shared, &other, &nsh, &pos) != 7 ||
            nsh < 0 || nsh > 16) { why = "unreadable target line"; break; }
        FoldTarget& t = got[i];
        if ((found != 0) != (t.m != nullptr) || index != t.index || raw != rva(t.raw) || code != rva(t.code)) {
            why = "a hooked method's entry or code moved";
            break;
        }
        t.shared = shared;
        t.sharers.clear();
        const char* p = line + pos;
        for (int k = 0; k < nsh; ++k) {
            char* e = nullptr;
            const unsigned long v = std::strtoul(p, &e, 10);
            if (e == p) break;
            t.sharers.push_back((uint32_t)v);
            p = e;
        }
        if ((int)t.sharers.size() != nsh) { why = "unreadable target line"; break; }
        t.other = other >= 0 ? API::get()->tdb()->get_method((uint32_t)other) : nullptr;
    }
    std::fclose(f);
    if (why) { std::snprintf(g_check.cache_note, sizeof g_check.cache_note, "%s", why); return false; }
    g_check.targets = std::move(got);
    return true;
}

bool cache_write() {
    char path[MAX_PATH], tmp[MAX_PATH];
    data_path(kCacheFile, path, sizeof path);
    std::snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "w") != 0 || !f) return false;
    std::fprintf(f, "%s\nkey %s\n", kCacheHeader, g_check.key);
    for (size_t i = 0; i < g_specs.size() && i < g_check.targets.size(); ++i) {
        const FoldTarget& t = g_check.targets[i];
        const long long other = t.other ? (long long)t.other->get_index() : -1;
        std::fprintf(f, "t %d %u %lld %lld %d %lld %d", t.m ? 1 : 0, t.index, rva(t.raw), rva(t.code), t.shared, other,
                     (int)t.sharers.size());
        for (uint32_t s : t.sharers) std::fprintf(f, " %u", s);
        std::fprintf(f, "|%s\n", spec_label(g_specs[i]).c_str());
    }
    const bool ok = std::fclose(f) == 0;
    return ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}

void check_run() {
    tl_tracing = g_trace_armed.load();
    const uint64_t t0 = GetTickCount64();
    g_check.tid = GetCurrentThreadId();
    step("code check: the hooked methods");
    auto* tdb = API::get()->tdb();
    g_check.methods = tdb ? tdb->get_num_methods() : 0;
    g_check.targets.assign(g_specs.size(), FoldTarget{});
    for (size_t i = 0; tdb && i < g_specs.size(); ++i) {
        const Spec& s = g_specs[i];
        op(s.sig);
        TD* td = tdb->find_type(s.type);
        API::Method* m = td ? td->find_method(s.sig) : nullptr;
        FoldTarget& t = g_check.targets[i];
        if (m) {
            t.m = m;
            t.index = m->get_index();
            t.raw = reinterpret_cast<uintptr_t>(m->get_function_raw());
            t.code = t.raw ? code_of(t.raw) : 0;
        }
        TR("check target %zu %s.%s: method %p, index %u, entry 0x%llX, code 0x%llX", i, s.type, s.sig, (void*)m, t.index,
           (unsigned long long)t.raw, (unsigned long long)t.code);
    }
    if (tdb) {
        step("code check: cache");
        exe_key(g_check.key, sizeof g_check.key, g_check.methods);
        TR("check key: %s", g_check.key);
        if (cache_read()) {
            g_check.cache_hit = true;
            g_check.scanned = true;
        } else {
            TR("cache not used: %s", g_check.cache_note);
            step("code check: every method");
            g_check.scanned = fold_scan(g_check.targets);
            if (g_check.scanned) {
                step("code check: cache write");
                g_check.cache_written = cache_write();
            }
        }
    }
    g_check.ms = GetTickCount64() - t0;
    tl_tracing = false;
}
bool check_seh(SehInfo* si) {
    __try { check_run(); }
    __except (seh_filter(GetExceptionInformation(), si)) { return false; }
    return true;
}
DWORD WINAPI check_thread(LPVOID) {
    SehInfo si{};
    if (!check_seh(&si)) {
        g_check.faulted = true;
        g_check.fault = si;
        g_check.scanned = false;
    }
    tl_tracing = false;
    g_check_state.store(2, std::memory_order_release);
    return 0;
}

void check_start_impl() {
    if (g_check_state.load(std::memory_order_acquire) != 0) return;
    if (!s_initialized.load(std::memory_order_acquire)) return;   // REFramework's API ready (the core's initialize ran)
    {
        std::lock_guard lk(g_specs_mx);
        make_specs();
    }
    int expected = 0;
    if (!g_check_state.compare_exchange_strong(expected, 1)) return;
    trace_check();
    HANDLE h = CreateThread(nullptr, 0, check_thread, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] code check (R18) started on a background thread: the hooks "
                 "follow when it ends, the game does not wait for it");
    } else {
        std::snprintf(g_check.cache_note, sizeof g_check.cache_note, "its thread was not created (error %lu)", GetLastError());
        g_check_state.store(2, std::memory_order_release);
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] code check thread not created: no hook (R18: never hooked blind)");
    }
}

std::string check_text() {
    const int st = g_check_state.load(std::memory_order_acquire);
    if (st != 2) return st ? "running" : "not started";
    char b[200];
    if (g_check.cache_hit)
        std::snprintf(b, sizeof b, "%llu ms, from the cache", (unsigned long long)g_check.ms);
    else if (g_check.scanned)
        std::snprintf(b, sizeof b, "%llu ms over %u methods on thread %lu, cache %s", (unsigned long long)g_check.ms,
                      g_check.methods, (unsigned long)g_check.tid, g_check.cache_written ? "written" : "not written");
    else
        std::snprintf(b, sizeof b, "failed");
    return b;
}

// ---- the install on the game thread: types and backend, then (check done) the verdicts, then one hook per frame ----
bool g_prepared = false, g_wait_logged = false, g_verdicts_done = false, g_required_ok = true;
std::vector<pc::Verdict> g_verdicts;
size_t g_next_hook = 0;
int g_hooked = 0;

bool prepare(const Calls& calls, SF6CoreClient* engine_client) {
    g_calls = calls;
    g_self = engine_client;
    g_api = sf6core_api_ptr();
    {
        std::lock_guard lk(g_types_mx);
        if (!g_types_done) {
            auto* tdb = API::get()->tdb();
            if (!tdb) return false;
            g_types_done = true;
            g_td_menu = tdb->find_type("app.training.TrainingMenuData");
            if (g_td_menu) { g_f_msg = ll::find_field_walk(g_td_menu, "_MessageID"); g_f_ftype = ll::find_field_walk(g_td_menu, "_FuncType"); }
            g_td_param = tdb->find_type("app.training.UIFlowTrainingMenu.Param");
            g_td_spin = tdb->find_type("app.UIPartsSpin");
            g_td_scroll = tdb->find_type("app.UIPartsScrollList");
            g_td_guide = tdb->find_type("app.InputGuideData");
            if (TD* tmf = tdb->find_type("app.training.TrainingMenuFunc")) g_m_viewupdate = tmf->find_method("ViewUpdate");
            if (TD* arr = tdb->find_type("System.Array")) {
                g_m_array_new = arr->find_method("CreateInstance(System.Type, System.Int32)");
                if (!g_m_array_new) {                    // REFramework's own pick for sdk.create_managed_array
                    API::Method* m = arr->find_method("CreateInstance");
                    if (m && m->get_num_params() == 2) g_m_array_new = m;
                }
            }
            if (g_td_menu) g_rt_menu = g_td_menu->get_runtime_type();
        }
    }
    if (!g_self || !g_td_menu || !g_f_msg || !g_f_ftype || !g_td_param || !g_m_viewupdate || !g_m_array_new || !g_rt_menu) {
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] game types not found (TrainingMenuData %s, Param %s, ViewUpdate %s, "
                 "System.Array.CreateInstance %s, its runtime type %s): off", g_td_menu ? "ok" : "no", g_td_param ? "ok" : "no",
                 g_m_viewupdate ? "ok" : "no", g_m_array_new ? "ok" : "no", g_rt_menu ? "ok" : "no");
        return false;
    }

    pc::Backend be;
    be.text_new = be_text_new; be.text_set = be_text_set; be.text_release = be_text_release;
    be.new_row_data = be_new_row_data; be.new_option_data = be_new_option_data; be.new_array = be_new_array;
    be.set_children = be_set_children; be.new_page_data = be_new_page_data; be.new_hint = be_new_hint;
    be.release = be_release; be.strip_read = be_strip_read; be.strip_write = be_strip_write; be.log = be_log;
    be.pass_begin = be_pass_begin; be.pass_end = be_pass_end; be.tracing = be_tracing; be.trace = be_trace;
    pc::set_backend(be);
    return true;
}

// The check's result turned into one verdict per hook (pm_core::hook_verdicts, checked offline).
void verdicts_compute() {
    const Check& c = g_check;
    if (c.faulted) {
        char d[420];
        describe(c.fault, d, sizeof d);
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] code check (R18) failed: %s", d);
    }
    if (c.cache_hit)
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] code check (R18): read from %s (same game executable), %llu ms on "
                 "thread %lu (background)", kCacheFile, (unsigned long long)c.ms, (unsigned long)c.tid);
    else if (c.scanned)
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] code check (R18): %u methods compared in %llu ms on thread %lu "
                 "(background: the game did not wait); cache %s (%s)", c.methods, (unsigned long long)c.ms,
                 (unsigned long)c.tid, c.cache_written ? "written" : "NOT written", c.cache_note);
    else
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] code check (R18) not done (%s): every hook refused",
                 c.cache_note[0] ? c.cache_note : (c.faulted ? "a fault" : "the game closed"));
    static const FoldTarget kNone;
    std::vector<pc::HookCode> codes;
    for (size_t i = 0; i < g_specs.size(); ++i) {
        const FoldTarget& t = i < c.targets.size() ? c.targets[i] : kNone;
        Spec& s = g_specs[i];
        s.m = t.m;
        s.raw = t.raw;
        s.code = t.code;
        s.shared = c.scanned ? t.shared : -1;
        s.other = t.other ? method_label(t.other) : std::string();
        pc::HookCode h;
        h.found = t.m != nullptr;
        h.index = t.index;
        h.raw = t.raw;
        h.code = t.code;
        h.shared = s.shared;
        h.sharers = t.sharers;
        h.group = (int)i;                    // a hook group = the same pre / post functions
        for (size_t j = 0; j < i; ++j)
            if (g_specs[j].pre == s.pre && g_specs[j].post == s.post) { h.group = codes[j].group; break; }
        codes.push_back(h);
    }
    g_verdicts = pc::hook_verdicts(codes);
    g_refused.clear();
    g_next_hook = 0;
    g_hooked = 0;
    g_required_ok = true;
}

// One spec: logged; true when add_hook was called (the next one waits for the next frame).
bool install_one(size_t i) {
    Spec& s = g_specs[i];
    const std::string label = spec_label(s);
    bool called = false;
    switch (g_verdicts[i]) {
    case pc::Verdict::Missing:
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] hook %s: not found (%s)", label.c_str(), s.without);
        break;
    case pc::Verdict::Covered:
        core_log(SF6CORE_LOG_INFO, nullptr,
                 "[pausemenu] hook %s at 0x%llX (code 0x%llX): shares its code only with a method of the same hook (%s): covered by it",
                 label.c_str(), (unsigned long long)s.raw, (unsigned long long)s.code, s.other.c_str());
        break;
    case pc::Verdict::Refused: {
        s.refused = true;
        const std::string why = s.shared >= 0 ? "shares its code with " + std::to_string(s.shared) + " other method(s), e.g. " + s.other
                                              : std::string("the code check failed");
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] hook %s at 0x%llX (code 0x%llX, declared by %s): %s: REFUSED (R18): %s",
                 label.c_str(), (unsigned long long)s.raw, (unsigned long long)s.code, method_label(s.m).c_str(),
                 why.c_str(), s.without);
        g_refused += (g_refused.empty() ? "" : ", ") + std::string(s.sig);
        break;
    }
    case pc::Verdict::Hook: {
        LARGE_INTEGER q0{}, q1{}, fq{};
        QueryPerformanceCounter(&q0);
        const unsigned int id = s.m->add_hook(s.pre, s.post, false);
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&fq);
        const double ms = fq.QuadPart ? (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0;
        called = true;
        if (id) {
            s.hooked = true;
            ++g_hooked;
            core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] hook %s at 0x%llX (code 0x%llX, declared by %s): %s, hooked (%.1f ms)",
                     label.c_str(), (unsigned long long)s.raw, (unsigned long long)s.code, method_label(s.m).c_str(),
                     s.shared ? "its code shared only within this hook" : "own code", ms);
        } else {
            core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] hook %s: add_hook failed (%s)", label.c_str(), s.without);
        }
        break;
    }
    }
    if (s.required && !s.hooked) g_required_ok = false;
    return called;
}

}  // namespace

// ======================== public ========================

void check_start() { check_start_impl(); }

Install install(const Calls& calls, SF6CoreClient* engine_client) {
    if (!g_prepared) {
        if (!prepare(calls, engine_client)) return Install::Failed;
        g_prepared = true;
    }
    check_start_impl();
    if (g_check_state.load(std::memory_order_acquire) != 2) {
        if (!g_wait_logged) {
            g_wait_logged = true;
            core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] waiting for the code check (background thread)");
        }
        return Install::Pending;
    }
    if (!g_verdicts_done) {                   // the verdicts this frame, the hooks from the next one
        verdicts_compute();
        g_verdicts_done = true;
        return Install::Pending;
    }
    // One add_hook per frame: REFramework's hook creation stops the game's threads while it patches (1 to 6 ms each on
    // 10/10); fifteen of them in one frame were a 23 ms frame.
    while (g_next_hook < g_specs.size())
        if (install_one(g_next_hook++)) break;
    if (g_next_hook < g_specs.size()) return Install::Pending;
    core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] %d / %zu hooks installed (code check: %s)%s", g_hooked, g_specs.size(),
             check_text().c_str(), g_required_ok ? "" : ": the engine is off");
    g_on = g_required_ok;
    return g_required_ok ? Install::Done : Install::Failed;
}

bool ready() { return g_on.load(); }

void text_barrier() {
    if (g_on.load(std::memory_order_relaxed) && !core_closing() && !off(kOffTextBarrier)) eng_barrier();
}

void frame(uint64_t f) {
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return;
    const bool lk = eng_lock();                // the install pass changes the menu data: never during a page build
    int64_t pt = perf::begin();
    SehInfo si{};
    if (!frame_seh(f, &si)) fault("frame", &si);
    tl_tracing = false;
    perf::end(perf::PmFrame, pt);
    eng_unlock(lk);
}

bool dyn_row_removable(void* dynamic_data) {
    {
        std::lock_guard lk(g_types_mx);
        if (!g_types_done) {
            auto* tdb = API::get()->tdb();
            if (!tdb) return false;
            if (!g_td_menu) g_td_menu = tdb->find_type("app.training.TrainingMenuData");
            if (g_td_menu && !g_f_msg) g_f_msg = ll::find_field_walk(g_td_menu, "_MessageID");
            if (g_td_menu && !g_f_ftype) g_f_ftype = ll::find_field_walk(g_td_menu, "_FuncType");
        }
    }
    if (!g_td_menu || !g_f_msg || !g_f_ftype) return false;
    MO* dd = reinterpret_cast<MO*>(dynamic_data);
    pc::Key k;
    if (!menu_key(field_obj(dd, "ChildData"), k)) return false;
    return pc::removable(k, has_delegate(dd) > 0);
}

namespace {
// The clients' live queries: a fault answers "no" (not counted on the engine's breaker), the first ones are logged.
std::atomic<int> g_query_faults{0};
void query_fault(const char* what, const SehInfo& si) {
    if (++g_query_faults > kDetailedFaults) return;
    char d[420];
    describe(si, d, sizeof d);
    core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] fault in %s (answered 'no'): %s", what, d);
}
int32_t page_shown_inner(int64_t page) {
    step("pm_page_shown");
    MO* param = param_get();
    if (!param) return tl_building == page ? 1 : 0;
    const bool on = page_shown_live(param, page);
    param_put(param);
    return on ? 1 : 0;
}
bool page_shown_seh(int64_t page, int32_t* out, SehInfo* si) {
    __try { *out = page_shown_inner(page); }
    __except (seh_filter(GetExceptionInformation(), si)) { return false; }
    return true;
}
void* row_part_inner(int64_t row) {
    step("pm_row_part");
    pc::RowInfo r;
    if (!pc::row_info(row, r) || r.removed || r.row_index < 0) return nullptr;
    MO* param = param_get();
    if (!param) return nullptr;
    void* out = entry_is(param, r.row_index, row) ? group_at(param, r.row_index) : nullptr;
    param_put(param);
    return out;
}
bool row_part_seh(int64_t row, void** out, SehInfo* si) {
    __try { *out = row_part_inner(row); }
    __except (seh_filter(GetExceptionInformation(), si)) { return false; }
    return true;
}
}  // namespace

int32_t page_shown(int64_t page) {
    if (!g_on.load() || core_closing()) return 0;
    int32_t v = 0;
    SehInfo si{};
    if (page_shown_seh(page, &v, &si)) return v;
    query_fault("pm_page_shown", si);
    return 0;
}

namespace {
// SEH frame: the game reads only (type check, GUID field); no C++ object here.
bool menu_key_seh(void* data, pc::Key* k, SehInfo* si) {
    __try { step("pm_row_of"); return menu_key(reinterpret_cast<MO*>(data), *k); }
    __except (seh_filter(GetExceptionInformation(), si)) { return false; }
}
}  // namespace

int64_t row_of(const void* owner, void* menu_data) {
    if (!owner || !menu_data || !g_on.load() || core_closing() || !g_f_msg) return -1;
    pc::Key k;
    SehInfo si{};
    if (!menu_key_seh(menu_data, &k, &si)) {
        if (si.code) query_fault("pm_row_of", si);
        return -1;
    }
    return pc::row_of(owner, k);              // registry lock inside
}

void* row_part(int64_t row) {
    if (!g_on.load() || core_closing()) return nullptr;
    void* p = nullptr;
    SehInfo si{};
    if (row_part_seh(row, &p, &si)) return p;
    query_fault("pm_row_part", si);
    return nullptr;
}

std::string status() {
    int hooked = 0;
    size_t n = 0;
    std::string refused;
    {
        std::lock_guard lk(g_specs_mx);       // (the table itself is made once, under this lock)
        for (const auto& s : g_specs) if (s.hooked) ++hooked;
        n = g_specs.size();
        refused = g_refused;
    }
    char b[420];
    std::snprintf(b, sizeof b, "on=%d faults=%d hooks %d/%zu (code check: %s)%s%s, engine lock waits past %d ms: %llu; ",
                  g_on.load() ? 1 : 0, g_faults.load(), hooked, n, check_text().c_str(), refused.empty() ? "" : ", refused: ",
                  refused.c_str(), kEngWaitMs, (unsigned long long)g_eng_timeouts.load());
    return std::string(b) + pc::status();
}

}  // namespace pausemenu
