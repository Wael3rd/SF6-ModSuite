#include "body.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings

#include "../common.hpp"
#include "../perf.hpp"
#include "../perf_hook.hpp"
#include "data.hpp"
#include "engine.hpp"
#include "mech.hpp"

namespace vfx::body {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;

constexpr uint64_t kDriveHold = 20;          // SF6_DriveRecolor.lua DRIVE_HOLD

std::mutex g_mx;                             // pushed colours + counters (Lua bridge vs effect hooks)
bool g_write = false;                        // owner vfx_body native
bool g_on = false;
int g_faults = 0;
API::TypeDefinition* g_f4 = nullptr;         // VfxMPCFloat4, found by name on first sight (the Lua compares the name)
struct ListM { API::Method* count = nullptr; API::Method* item = nullptr; };
std::unordered_map<API::TypeDefinition*, ListM> g_list_m;

int64_t g_dr[3][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };                 // [side][parry, rush]
int64_t g_di[3][2][2] = { { { -1, -1 }, { -1, -1 } }, { { -1, -1 }, { -1, -1 } }, { { -1, -1 }, { -1, -1 } } };  // [side][impact, reversal][slot]
int64_t g_act[3] = { -1, -1, -1 };
uint64_t g_drive_until[3] = {};
uint64_t g_calls = 0, g_body = 0, g_written = 0;
std::map<std::string, uint64_t> g_cnt;       // "dr <side>/<mech>", "di <side>/<param>/<why>" (the DI module's body_note keys)
std::string g_last;

API::Field* g_pn_field = nullptr;            // VfxMPCFloat4.paramName, resolved with g_f4 (no lookup by name per call)
API::TypeDefinition* g_not_f4[16] = {};      // types seen that are not VfxMPCFloat4 (no name compare per call)
unsigned g_not_f4_n = 0;

bool is_f4(API::TypeDefinition* td) {
    if (!td) return false;
    if (td == g_f4) return true;
    for (auto* t : g_not_f4) if (t == td) return false;
    const char* n = td->get_name();
    if (n && std::strcmp(n, "VfxMPCFloat4") == 0) { g_pn_field = ll::find_field_walk(td, "paramName"); g_f4 = td; return true; }
    g_not_f4[g_not_f4_n++ & 15] = td;
    return false;
}

// paramName compared as the managed string's UTF-16 chars (System.String: length @0x10, chars @0x14, see
// ll::managed_string_to_utf8): 1 = Emit_Color, 2 = Emit_BaseColor, 0 = another parameter
int slot_of(MO* p, const char*& pn) {
    MO* s = nullptr;
    if (g_pn_field) { void* raw = g_pn_field->get_data_raw(p, false); s = raw ? *reinterpret_cast<MO**>(raw) : nullptr; }
    else s = ll::read_field_object(p, "paramName");
    if (!s) return 0;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(s);
    const int32_t n = *reinterpret_cast<const int32_t*>(b + 0x10);
    const wchar_t* w = reinterpret_cast<const wchar_t*>(b + 0x14);
    if (n == 10 && std::wmemcmp(w, L"Emit_Color", 10) == 0) { pn = "Emit_Color"; return 1; }
    if (n == 14 && std::wmemcmp(w, L"Emit_BaseColor", 14) == 0) { pn = "Emit_BaseColor"; return 2; }
    return 0;
}

void count(const std::string& k) { ++g_cnt[k]; }

// the DI mechanic on_param uses for a side: impact / reversal seen recently, else a Drive Impact action; -1 = none
int di_mech_locked(int side, const char* m) {
    if (m && std::strcmp(m, "impact") == 0) return 0;
    if (m && std::strcmp(m, "reversal") == 0) return 1;
    if (g_act[side] == 855 || g_act[side] == 857) return 0;      // DI_ACTIONS: the Drive Impact's two variants
    return -1;
}
int dr_mech(const char* m) { return !m ? -1 : (std::strcmp(m, "parry") == 0 ? 0 : (std::strcmp(m, "rush") == 0 ? 1 : -1)); }

// Whether on_param can change a parameter of this side now: a DI colour for its DI mechanic, or a recent parry / rush
// with its Drive colour. False: on_param would leave every value as it is (the hooks return before the list walk).
bool may_write_locked(int side, const char* m) {
    if (side != 1 && side != 2) return false;
    const int di = di_mech_locked(side, m);
    if (di >= 0 && (g_di[side][di][0] >= 0 || g_di[side][di][1] >= 0)) return true;
    const int dr = dr_mech(m);
    return dr >= 0 && g_dr[side][dr] >= 0;
}

// One body material parameter: DI first, then DR on what DI left (the Lua runs SF6_DIRecolor's hook first).
// side / m: the manager's side (mech::side_cached) and its recent mechanic, resolved once per hook call.
// dbg (vfx_debug_on.txt): the per-key counters and the last change of vfx_body_probe.
void on_param(int side, const char* m, MO* p, bool dbg) {
    if (!p || !is_f4(p->get_type_definition())) return;
    const char* pn = nullptr;
    const int slot = slot_of(p, pn);
    if (!slot) return;
    float* f = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(p) + 0x20);
    const float in[3] = { f[0], f[1], f[2] };
    float out[3] = { in[0], in[1], in[2] };
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_body;
    // ---- SF6_DIRecolor body_repaint ----
    if (side != 1 && side != 2) {
        if (dbg && side == 0) count(std::string("di ?/") + pn + "/pas un joueur");
    } else {
        const int mi = di_mech_locked(side, m);
        if (mi < 0) { if (dbg) count("di " + std::to_string(side) + "/" + pn + "/mecanique refusee"); }
        else if (g_di[side][mi][slot - 1] < 0) { if (dbg) count("di " + std::to_string(side) + "/" + pn + "/pas de couleur"); }
        else {
            const uint32_t c = (uint32_t)g_di[side][mi][slot - 1];
            out[0] = (float)((c & 0xFF) / 255.0); out[1] = (float)(((c >> 8) & 0xFF) / 255.0); out[2] = (float)(((c >> 16) & 0xFF) / 255.0);
            if (dbg) count("di " + std::to_string(side) + "/" + pn + "/ECRIT");
        }
    }
    // ---- SF6_DriveRecolor repaint_param (reads what DI wrote) ----
    const double x = out[0], y = out[1], z = out[2];
    if (vfx::data::is_drive_hue(x, y, z)) {
        if (side >= 1 && side <= 2) g_drive_until[side] = cmn::frame() + kDriveHold;
        const int mi = dr_mech(m);
        if (mi >= 0 && side >= 1 && side <= 2 && g_dr[side][mi] >= 0) {
            const uint32_t col = vfx::data::tint_with_d((uint32_t)g_dr[side][mi], x * 255, y * 255, z * 255);
            out[0] = (float)((col & 0xFF) / 255.0); out[1] = (float)(((col >> 8) & 0xFF) / 255.0); out[2] = (float)(((col >> 16) & 0xFF) / 255.0);
            if (dbg) count("dr " + std::to_string(side) + "/" + m);
        }
    }
    if (out[0] == in[0] && out[1] == in[1] && out[2] == in[2]) return;
    if (dbg) {
        char b[96];
        std::snprintf(b, sizeof b, "P%d %s %.3f,%.3f,%.3f -> %.3f,%.3f,%.3f", side, pn, in[0], in[1], in[2], out[0], out[1], out[2]);
        g_last = b;
    }
    if (!g_write) return;
    f[0] = out[0]; f[1] = out[1]; f[2] = out[2];
    ++g_written;
}

// the side of the hooked manager and its recent mechanic; false = nothing can change (debug: always walk, for the counters)
bool resolve(MO* mgr, int& side, const char*& m, bool dbg) {
    side = mech::side_cached(mgr);
    m = mech::recent(side);
    if (dbg) return true;
    std::lock_guard<std::mutex> lk(g_mx);
    return may_write_locked(side, m);
}

int pre_add_inner(void** argv) {
    ++g_calls;
    const bool dbg = perf::vfx_debug();
    int side = 0; const char* m = nullptr;
    if (!resolve(reinterpret_cast<MO*>(argv[1]), side, m, dbg)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    on_param(side, m, reinterpret_cast<MO*>(argv[3]), dbg);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int pre_req_inner(void** argv) {
    ++g_calls;
    const bool dbg = perf::vfx_debug();
    int side = 0; const char* m = nullptr;
    if (!resolve(reinterpret_cast<MO*>(argv[1]), side, m, dbg)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    MO* lst = reinterpret_cast<MO*>(argv[3]);
    if (!lst || !API::get()->sdk()->managed_object->is_managed_object(lst)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* td = lst->get_type_definition();
    auto it = g_list_m.find(td);
    if (it == g_list_m.end()) it = g_list_m.emplace(td, ListM{ ll::find_method_noargs(td, "get_Count"), ll::find_method_full(td, "get_Item(System.Int32)") }).first;
    if (!it->second.count || !it->second.item) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    // span overloads over stack slots: same invoke call, no vector allocation per call
    auto rc = it->second.count->invoke(lst, std::span<void*>{});
    if (rc.exception_thrown) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int32_t n = (int32_t)rc.dword;
    for (int32_t i = 0; i < n && i < 64; ++i) {
        void* slot[1] = { ll::arg_i32(i) };
        auto ri = it->second.item->invoke(lst, std::span<void*>{ slot });
        if (!ri.exception_thrown && ri.ptr) on_param(side, m, reinterpret_cast<MO*>(ri.ptr), dbg);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void fault(const char* where) {
    ++g_faults;
    cmn::journal("vfx body: fault caught in %s (%d)", where, g_faults);
    if (g_faults >= 5) { g_on = false; cmn::journal("vfx body: too many faults, off until the game restarts (the colours stay the game's)"); }
}

#pragma warning(push)
#pragma warning(disable: 4733)
int pre_add_seh(int argc, void** argv) {
    if (!g_on || argc < 4) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return pre_add_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("addMaterialEffect"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_req_seh(int argc, void** argv) {
    if (!g_on || argc < 4) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return pre_req_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("requestMaterialEffect"); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
#pragma warning(pop)
// timed from outside the __try functions (perf.hpp)
int pre_add(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int64_t t0 = perf::begin();
    const int r = pre_add_seh(argc, argv);
    perf::end(perf::HookBodyAddMaterial, t0);
    return r;
}
int pre_req(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int64_t t0 = perf::begin();
    const int r = pre_req_seh(argc, argv);
    perf::end(perf::HookBodyRequestMaterial, t0);
    return r;
}
void post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}
bool g_add_delegated = false;               // addMaterialEffect is dispatched by vfx::engine's hook (one REFramework dispatch)

}  // namespace

void on_add_material(int argc, void** argv) {
    if (!g_add_delegated) return;
    pre_add(argc, argv, nullptr, 0);
}

bool install(bool write) {
    auto* td = API::get()->tdb()->find_type("app.vfx.VfxActorEffectManager");
    if (!td) { cmn::journal("vfx body: app.vfx.VfxActorEffectManager missing, off"); return false; }
    int n_add = 0, n_req = 0;
    for (auto* m : td->get_methods()) {
        if (!m->get_function_raw()) continue;
        const std::string name = m->get_name();
        if (name == "addMaterialEffect" && n_add == 0 && vfx::engine::add_hooked() == static_cast<const void*>(m)) { g_add_delegated = true; ++n_add; cmn::journal("vfx body: addMaterialEffect rides on the engine hook (one dispatch)"); }
        else if (name == "addMaterialEffect" && n_add == 0) { if (m->add_hook(pre_add, post_nop, false)) { ++n_add; perf::journal_hooked("VfxActorEffectManager.addMaterialEffect (HookBodyAddMaterial)", m); } }   // the Lua's get_method: one
        else if (name == "requestMaterialEffect") { if (m->add_hook(pre_req, post_nop, false)) { ++n_req; perf::journal_hooked("VfxActorEffectManager.requestMaterialEffect (HookBodyRequestMaterial)", m); } }
    }
    g_write = write;
    g_on = n_add + n_req > 0;
    cmn::journal("vfx body: addMaterialEffect x%d, requestMaterialEffect x%d, %s", n_add, n_req,
                 write ? "writes (owner vfx_body native, the Lua hooks stand by)" : "shadow (computes, writes nothing)");
    return g_on;
}

bool ready() { return g_on; }

void set_dr(const int64_t c[4]) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_dr[1][0] = c[0]; g_dr[1][1] = c[1]; g_dr[2][0] = c[2]; g_dr[2][1] = c[3];
}

void set_di(const int64_t c[8]) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (int side = 1; side <= 2; ++side)
        for (int mi = 0; mi < 2; ++mi)
            for (int s = 0; s < 2; ++s) g_di[side][mi][s] = c[(side - 1) * 4 + mi * 2 + s];
}

void set_act(int side, int64_t act) {
    if (side != 1 && side != 2) return;
    std::lock_guard<std::mutex> lk(g_mx);
    g_act[side] = act;
}

std::string probe() {
    std::lock_guard<std::mutex> lk(g_mx);
    char h[160];
    std::snprintf(h, sizeof h, "on=%d write=%d faults=%d calls=%llu body=%llu written=%llu act=%lld/%lld drive=%d/%d", g_on ? 1 : 0, g_write ? 1 : 0,
                  g_faults, (unsigned long long)g_calls, (unsigned long long)g_body, (unsigned long long)g_written, (long long)g_act[1], (long long)g_act[2],
                  cmn::frame() < g_drive_until[1] ? 1 : 0, cmn::frame() < g_drive_until[2] ? 1 : 0);
    std::string o = h;
    o += " | ";
    bool first = true;
    for (auto& [k, n] : g_cnt) { o += (first ? "" : ";") + k + "=" + std::to_string(n); first = false; }
    return o + " | last " + g_last;
}

}  // namespace vfx::body
