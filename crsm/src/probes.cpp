#include "probes.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

#include <reframework/API.hpp>

#include "common.hpp"

namespace probes {
namespace {

using API = reframework::API;

constexpr const char* kFlagFile = "reframework/data/SF6_CRSMNative_data/probes_on.txt";
constexpr const char* kLogFile = "reframework/data/SF6_CRSMNative_data/probes.log";

// ---- the hooked methods (P2 families + C4e small setters) -------------------------------------------------------------
// Signatures are the ones the Lua hooks use (so the same overloads are measured). A method that cannot be found is
// logged and skipped.
struct Spec { const char* family; const char* type; const char* method; bool c4e; };
constexpr Spec kSpecs[] = {
    // fight hooks (draw, slot 8, reversal trigger, REC HUD label)
    { "fight",  "app.FBattleMediator",                           "UpdateGameInfo", false },
    { "fight",  "app.training.tf_RecordSetting",                 "SettingRecodData", false },
    { "fight",  "app.training.tf_ReversalSetting",               "UpdateReversalCheck", false },
    { "fight",  "app.training.UIWidget_TMRecording",             "SetText(System.Guid, System.Object[])", false },
    // setters of the game's data (delete hooks, row saves)
    { "data",   "app.training.tf_RecordSetting.FuncData",        "RemoveRecord", false },
    { "data",   "app.training.tf_ReversalSetting.FuncData",      "SetReversalData", false },
    { "data",   "app.training.tf_ReversalSetting.FuncData",      "RemoveReversalData", false },
    // pause menu: function / view update of the rows, open, guide bar
    { "menu",   "app.training.TrainingMenuFunc",                 "Function", false },
    { "menu",   "app.training.TrainingMenuFunc",                 "ViewUpdate", false },
    { "menu",   "app.training.TrainingManager",                  "OpenMenu(app.training.TrainingManager.MenuType, app.training.BaseParam)", false },
    { "menu",   "app.training.TrainingManager",                  "GetGuideData", false },
    // pause menu: the flow's param (record / replay / reversal item updates, view data)
    { "param",  "app.training.UIFlowTrainingMenu.Param",         "UpdateRecordSlotItem(app.UIPartsGroup, System.Int32)", false },
    { "param",  "app.training.UIFlowTrainingMenu.Param",         "UpdatePlaySlotItem(app.UIPartsGroup, System.Int32)", false },
    { "param",  "app.training.UIFlowTrainingMenu.Param",         "UpdateReversalItem(app.UIPartsGroup, System.Int32)", false },
    { "param",  "app.training.UIFlowTrainingMenu.Param",         "CreateViewData", false },
    { "param",  "app.training.UIFlowTrainingMenu.Param",         "UpdateSecondaryList", false },
    { "param",  "app.training.UIFlowTrainingMenu.Param",         "UpdateSpinBox(System.Int32, System.Boolean, System.Boolean)", false },
    // list cursor and scroll (type-wide)
    { "list",   "app.UIPartsListBase",                           "GetNextIndex", false },
    { "list",   "app.UIPartsListBase",                           "GetPrevIndex", false },
    { "list",   "app.UIPartsGroupScroll",                        "UpdateScroll", false },
    // name dialog
    { "dialog", "app.UITextInputDialog",                         "OpenTextInputDialog", false },
    { "dialog", "app.UITextInputDialog",                         "InvokeResultEvent", false },
    // reversal picker
    { "picker", "app.training.UIFlowTrainingMenu_Reversal_Recording.Param", "InitScrollList", false },
    { "picker", "app.training.UIFlowTrainingMenu_Reversal_Recording.Param", "UpdateScrollList", false },
    // C4e: tiny setters and the four constant TrainingMenuFunc answers (folded bodies, R18)
    { "c4e",    "app.UIPartsSpin",                               "set_Num(System.Int32)", true },
    { "c4e",    "app.UIPartsScrollList",                         "set_SelectedIndex", true },
    { "c4e",    "app.UIPartsScrollList",                         "SetSelectedIndex", true },
    { "c4e",    "app.UIPartsPageIndex",                          "set_PageIndex", true },
    { "c4e",    "app.training.UIFlowTrainingMenu.Param",         "set_SelectChildIndex(System.Int32)", true },
    { "c4e",    "app.training.TrainingMenuFunc",                 "GetIsActive", true },
    { "c4e",    "app.training.TrainingMenuFunc",                 "IsValueType", true },
    { "c4e",    "app.training.TrainingMenuFunc",                 "GetVisibleCase", true },
    { "c4e",    "app.training.TrainingMenuFunc",                 "IsChangedValue", true },
};
constexpr size_t kN = sizeof(kSpecs) / sizeof(kSpecs[0]);
constexpr uint32_t kMaxTids = 4;

struct Counter {
    std::atomic<uint64_t> calls{0};
    uint64_t reported = 0;                   // calls at the last 10 s line (frame thread only)
    std::atomic<uint32_t> ntids{0};
    std::atomic<uint32_t> tids[kMaxTids]{};
    uint32_t logged_tids = 0;                // frame thread only
    void* fn = nullptr;
    bool found = false;
    bool hooked = false;
};
Counter g_c[kN];

bool g_requested = false;                    // probes_on.txt at the start: hooks installed
bool g_on = false;                           // probes_on.txt now (looked at every 300 frames)
bool g_p1_done = false;
unsigned g_hooks = 0;
uint64_t g_last_report_ms = 0;
uint64_t g_last_summary_ms = 0;

// A line of the probes log (and the journal).
void out(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cmn::journal("%s", buf);
    cmn::append_file(kLogFile, std::string("[f") + std::to_string(cmn::frame()) + "] " + buf + "\r\n");
}

// ---- hooks: pass-through, counters only, under SEH ---------------------------------------------------------------------
// note() holds no C++ object: it can sit in a __try frame.
void note(size_t i) {
    Counter& c = g_c[i];
    c.calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t tid = GetCurrentThreadId();
    const uint32_t n = c.ntids.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < n && k < kMaxTids; ++k)
        if (c.tids[k].load(std::memory_order_relaxed) == tid) return;
    if (n < kMaxTids) {
        const uint32_t slot = c.ntids.fetch_add(1, std::memory_order_relaxed);
        if (slot < kMaxTids) c.tids[slot].store(tid, std::memory_order_relaxed);
    }
}

template <size_t I>
int pre_hook(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { note(I); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_hook(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

template <size_t... I>
constexpr std::array<REFPreHookFn, sizeof...(I)> make_pre(std::index_sequence<I...>) {
    return { &pre_hook<I>... };
}

void install_hooks() {
    constexpr auto pres = make_pre(std::make_index_sequence<kN>{});
    auto* tdb = API::get()->tdb();
    if (!tdb) { out("probes: no type database, no hook"); return; }
    for (size_t i = 0; i < kN; ++i) {
        const Spec& s = kSpecs[i];
        auto* td = tdb->find_type(s.type);
        auto* m = td ? td->find_method(s.method) : nullptr;
        if (!m) { out("probes: %s::%s not found, skipped", s.type, s.method); continue; }
        g_c[i].found = true;
        g_c[i].fn = m->get_function_raw();
        if (!g_c[i].fn) { out("probes: %s::%s has no native body, skipped", s.type, s.method); continue; }
        if (m->add_hook(pres[i], post_hook, false)) { g_c[i].hooked = true; ++g_hooks; }
        else out("probes: add_hook refused for %s::%s", s.type, s.method);
    }
    // C4e: two probed methods on one address = a folded body (a hook on one fires for the other too)
    for (size_t i = 0; i < kN; ++i) {
        if (!g_c[i].fn) continue;
        for (size_t j = i + 1; j < kN; ++j)
            if (g_c[j].fn == g_c[i].fn)
                out("C4e fold: %s::%s and %s::%s share the address %p", kSpecs[i].type, kSpecs[i].method, kSpecs[j].type, kSpecs[j].method, g_c[i].fn);
    }
    out("probes: %u of %zu hooks installed (pass-through, counters only)", g_hooks, kN);
}

// ---- small readers (game thread, frame callback): any fault is caught by the catch(...) of the caller (/EHa) and by
// the core's SEH guard around the client's frame callback -------------------------------------------------------------------
using MO = API::ManagedObject;

void* field_ref(MO* o, const char* name) {
    if (!o) return nullptr;
    void** p = o->get_field<void*>(name);
    return p ? *p : nullptr;
}

// An integer field of 1, 2, 4 or 8 bytes (an enum too); false when the field does not exist.
bool field_int(MO* o, const char* name, int64_t* v) {
    if (!o) return false;
    auto* td = o->get_type_definition();
    auto* f = td ? td->find_field(name) : nullptr;
    if (!f) return false;
    void* p = f->get_data_raw(o, false);
    if (!p) return false;
    uint32_t sz = 4;
    if (auto* ft = f->get_type()) { const uint32_t s = ft->get_valuetype_size(); if (s) sz = s; }
    switch (sz) {
        case 1: *v = *(int8_t*)p; break;
        case 2: *v = *(int16_t*)p; break;
        case 8: *v = *(int64_t*)p; break;
        default: *v = *(int32_t*)p; break;
    }
    return true;
}

reframework::InvokeRet call(MO* o, const char* method, std::span<void*> args) {
    auto* td = o ? o->get_type_definition() : nullptr;
    auto* m = td ? td->find_method(method) : nullptr;
    if (!m) { reframework::InvokeRet r{}; r.exception_thrown = true; return r; }
    return m->invoke(o, args);
}
void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }

// ---- P1: path A = path B, every fighter ----------------------------------------------------------------------------------
// Path A (RSM): TrainingManager.get_RecordFunc()._tData.RecordSetting.FighterDataList.get_Item(fid)
// Path B (RMT, RS): TrainingManager._tData.RecordSetting.GetFighterIndex(fid), then FighterDataList.get_Item(index)
// Reported: the index the game gives for each id 0..39, the FighterID of each list entry, and whether A and B reach the
// same object. The lists have 32-34 entries; ids 23 and 24 do not exist.
void p1_list(MO* tm, int which) {
    const char* name = which == 0 ? "Record" : "Reversal";
    MO* tdata = (MO*)field_ref(tm, "_tData");
    MO* setting = (MO*)field_ref(tdata, which == 0 ? "RecordSetting" : "ReversalSetting");
    MO* list = (MO*)field_ref(setting, "FighterDataList");
    if (!list) { out("P1 %s: no FighterDataList (tData %p, setting %p)", name, (void*)tdata, (void*)setting); return; }
    reframework::InvokeRet rc = call(list, "get_Count", {});
    const int count = rc.exception_thrown ? -1 : (int)rc.dword;

    // path A for the Record list: through get_RecordFunc()
    MO* list_a = nullptr;
    bool same_setting = false;
    if (which == 0) {
        reframework::InvokeRet rf = call(tm, "get_RecordFunc", {});
        MO* func = rf.exception_thrown ? nullptr : (MO*)rf.ptr;
        MO* setting_a = (MO*)field_ref((MO*)field_ref(func, "_tData"), "RecordSetting");
        same_setting = setting_a == setting;
        list_a = (MO*)field_ref(setting_a, "FighterDataList");
    }

    std::string idx_bad, id_bad, ab_bad;
    int idx_ok = 0, id_ok = 0, ab_ok = 0, ab_n = 0;
    char b[48];
    for (int fid = 0; fid < 40; ++fid) {
        void* a1[1] = { arg_i32(fid) };
        reframework::InvokeRet ri = call(setting, "GetFighterIndex", std::span<void*>(a1, 1));
        const int idx = ri.exception_thrown ? -999 : (int)ri.dword;
        if (idx == fid) ++idx_ok;
        else { std::snprintf(b, sizeof(b), " %d->%d", fid, idx); idx_bad += b; }
        if (which == 0 && list_a) {                                   // A = list[fid], B = list[idx]
            MO* item_a = nullptr; MO* item_b = nullptr;
            if (fid < count) { void* a[1] = { arg_i32(fid) }; auto r = call(list_a, "get_Item", std::span<void*>(a, 1)); item_a = r.exception_thrown ? nullptr : (MO*)r.ptr; }
            if (idx >= 0 && idx < count) { void* a[1] = { arg_i32(idx) }; auto r = call(list, "get_Item", std::span<void*>(a, 1)); item_b = r.exception_thrown ? nullptr : (MO*)r.ptr; }
            if (item_a || item_b) {
                ++ab_n;
                if (item_a == item_b) ++ab_ok;
                else { std::snprintf(b, sizeof(b), " %d(A %p, B %p)", fid, (void*)item_a, (void*)item_b); ab_bad += b; }
            }
        }
    }
    for (int i = 0; i < count && i < 40; ++i) {
        void* a[1] = { arg_i32(i) };
        auto r = call(list, "get_Item", std::span<void*>(a, 1));
        MO* item = r.exception_thrown ? nullptr : (MO*)r.ptr;
        int64_t id = -999;
        if (!item || !field_int(item, "FighterID", &id)) id = -999;
        if (id == i) ++id_ok;
        else { std::snprintf(b, sizeof(b), " [%d]=%lld", i, (long long)id); id_bad += b; }
    }
    out("P1 %s: count %d | GetFighterIndex(fid)==fid for %d of 40, others:%s | FighterDataList[i].FighterID==i for %d of %d, others:%s",
        name, count, idx_ok, idx_bad.empty() ? " none" : idx_bad.c_str(), id_ok, count < 40 ? count : 40, id_bad.empty() ? " none" : id_bad.c_str());
    if (which == 0)
        out("P1 Record path A vs B: same RecordSetting object %s; item(fid) == item(GetFighterIndex(fid)) for %d of %d ids with an item, others:%s",
            same_setting ? "yes" : "NO", ab_ok, ab_n, ab_bad.empty() ? " none" : ab_bad.c_str());
}

void p1_try() {
    MO* tm = API::get()->get_managed_singleton("app.training.TrainingManager");
    if (!tm) return;
    MO* tdata = (MO*)field_ref(tm, "_tData");
    if (!tdata) return;
    if (!field_ref(tdata, "RecordSetting") || !field_ref(tdata, "ReversalSetting")) return;
    p1_list(tm, 0);
    p1_list(tm, 1);
    g_p1_done = true;
    cmn::refresh_status();
}

// ---- P11: does the menu-opening flag lead the pause bit? Edge log, one line when any of the three values changes -------
struct P11State { int opening = -2; int opend = -2; int pause = -2; bool fields_logged = false; bool no_opend_logged = false; };
P11State g_p11;

void p11_tick() {
    MO* tm = API::get()->get_managed_singleton("app.training.TrainingManager");
    int opening = -1, opend = -1, pause = -1;
    if (tm) {
        auto r = call(tm, "get_IsMenuOpening", {});
        if (!r.exception_thrown) opening = r.byte ? 1 : 0;
        MO* tdata = (MO*)field_ref(tm, "_tData");
        MO* other = (MO*)field_ref(tdata, "OtherSetting");
        int64_t v = 0;
        if (other && field_int(other, "IsMenuOpend", &v)) opend = v ? 1 : 0;
        else if (tdata && !g_p11.fields_logged) {                       // where is IsMenuOpend? list the fields of _tData once
            g_p11.fields_logged = true;
            std::string names;
            for (auto* f : tdata->get_type_definition()->get_fields()) { names += f->get_name(); names += ' '; }
            out("P11: _tData.OtherSetting.IsMenuOpend not found (OtherSetting %p); fields of _tData: %s", (void*)other, names.c_str());
        }
    }
    if (MO* pm = API::get()->get_managed_singleton("app.PauseManager")) {
        int64_t v = 0;
        if (field_int(pm, "_CurrentPauseTypeBit", &v)) pause = (int)v;
    }
    if (opening != g_p11.opening || opend != g_p11.opend || pause != g_p11.pause) {
        out("P11 IsMenuOpening=%d IsMenuOpend=%d pauseBit=%d (was %d %d %d)", opening, opend, pause, g_p11.opening, g_p11.opend, g_p11.pause);
        g_p11.opening = opening; g_p11.opend = opend; g_p11.pause = pause;
    }
}

// ---- reports -------------------------------------------------------------------------------------------------------------
void report_threads() {
    const uint32_t ft = cmn::frame_thread();
    for (size_t i = 0; i < kN; ++i) {
        Counter& c = g_c[i];
        const uint32_t n = c.ntids.load(std::memory_order_relaxed);
        for (; c.logged_tids < n && c.logged_tids < kMaxTids; ++c.logged_tids) {
            const uint32_t tid = c.tids[c.logged_tids].load(std::memory_order_relaxed);
            if (!tid) break;
            out("P2 %s %s::%s: thread %u%s (core frame thread %u)", kSpecs[i].family, kSpecs[i].type, kSpecs[i].method, tid,
                tid == ft ? " = frame thread" : " != frame thread", ft);
        }
    }
}

// C4e: one line every 10 s with the calls of each small setter in that window
void report_c4e(double seconds) {
    std::string line;
    char b[160];
    for (size_t i = 0; i < kN; ++i) {
        if (!kSpecs[i].c4e || !g_c[i].hooked) continue;
        const uint64_t now = g_c[i].calls.load(std::memory_order_relaxed);
        const uint64_t d = now - g_c[i].reported;
        g_c[i].reported = now;
        const char* m = kSpecs[i].method;
        std::snprintf(b, sizeof(b), " %s=%llu", m, (unsigned long long)d);
        line += b;
    }
    if (!line.empty()) out("C4e %.0fs calls:%s", seconds, line.c_str());
}

// P2 summary every 60 s: per hook, calls and thread count
void report_summary() {
    std::string line;
    char b[200];
    for (size_t i = 0; i < kN; ++i) {
        if (kSpecs[i].c4e || !g_c[i].hooked) continue;
        const uint64_t n = g_c[i].calls.load(std::memory_order_relaxed);
        if (!n) continue;
        const char* m = std::strchr(kSpecs[i].method, '(') ? "" : kSpecs[i].method;
        std::snprintf(b, sizeof(b), " %s/%s=%llu(%ut)", kSpecs[i].family, m[0] ? m : "*", (unsigned long long)n, g_c[i].ntids.load(std::memory_order_relaxed));
        line += b;
    }
    if (!line.empty()) out("P2 summary (calls, threads):%s", line.c_str());
}

}  // namespace

bool requested() { return std::filesystem::exists(kFlagFile); }

void install() {
    g_requested = requested();
    g_on = g_requested;
    if (!g_requested) return;
    out("probes: %s present, hooks and frame probes on", kFlagFile);
    install_hooks();
}

void on_frame(uint64_t frame) {
    if (frame % 300 == 1) {                                     // the flag file can be created or deleted while the game runs
        const bool now = requested();
        if (now != g_on) { g_on = now; out("probes: frame probes %s (the hooks follow the file at start only)", now ? "on" : "off"); }
    }
    if (!g_on) return;
    try {
        if (!g_p1_done && frame % 120 == 5) p1_try();
        p11_tick();
        if (frame % 30 == 3) report_threads();
        const uint64_t ms = GetTickCount64();
        if (!g_last_report_ms) { g_last_report_ms = ms; g_last_summary_ms = ms; }
        if (ms - g_last_report_ms >= 10000) { report_c4e((double)(ms - g_last_report_ms) / 1000.0); g_last_report_ms = ms; }
        if (ms - g_last_summary_ms >= 60000) { report_summary(); g_last_summary_ms = ms; }
    } catch (...) {
        out("probes: exception in the frame probes, switched off until the next start");
        g_on = false;
    }
}

std::string status() {
    if (!g_requested && !g_on) return "off";
    char b[160];
    std::snprintf(b, sizeof(b), "on (hooks %u/%zu, P1 %s)", g_hooks, kN, g_p1_done ? "done" : "pending");
    return b;
}

}  // namespace probes
