#include "body.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings

#include "../common.hpp"
#include "../perf.hpp"
#include "data.hpp"
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

bool is_f4(API::TypeDefinition* td) {
    if (!td) return false;
    if (td == g_f4) return true;
    const char* n = td->get_name();
    if (n && std::strcmp(n, "VfxMPCFloat4") == 0) { g_f4 = td; return true; }
    return false;
}

void count(const std::string& k) { ++g_cnt[k]; }

// One body material parameter: DI first, then DR on what DI left (the Lua runs SF6_DIRecolor's hook first).
void on_param(MO* mgr, MO* p) {
    if (!p || !is_f4(p->get_type_definition())) return;
    MO* s = ll::read_field_object(p, "paramName");
    if (!s) return;
    const std::string pn = ll::managed_string_to_utf8(s);
    const int slot = pn == "Emit_Color" ? 1 : (pn == "Emit_BaseColor" ? 2 : 0);
    if (!slot) return;
    float* f = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(p) + 0x20);
    const float in[3] = { f[0], f[1], f[2] };
    float out[3] = { in[0], in[1], in[2] };
    const int side = mech::side_now(mgr);
    const char* m = mech::recent(side);
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_body;
    // ---- SF6_DIRecolor body_repaint ----
    if (side != 1 && side != 2) {
        if (side == 0) count("di ?/" + pn + "/pas un joueur");
    } else {
        int mi = -1;
        if (m && std::strcmp(m, "impact") == 0) mi = 0;
        else if (m && std::strcmp(m, "reversal") == 0) mi = 1;
        else if (g_act[side] == 855 || g_act[side] == 857) mi = 0;   // DI_ACTIONS: the Drive Impact's two variants
        const std::string k = "di " + std::to_string(side) + "/" + pn + "/";
        if (mi < 0) count(k + "mecanique refusee");
        else if (g_di[side][mi][slot - 1] < 0) count(k + "pas de couleur");
        else {
            const uint32_t c = (uint32_t)g_di[side][mi][slot - 1];
            out[0] = (float)((c & 0xFF) / 255.0); out[1] = (float)(((c >> 8) & 0xFF) / 255.0); out[2] = (float)(((c >> 16) & 0xFF) / 255.0);
            count(k + "ECRIT");
        }
    }
    // ---- SF6_DriveRecolor repaint_param (reads what DI wrote) ----
    const double x = out[0], y = out[1], z = out[2];
    if (vfx::data::is_drive_hue(x, y, z)) {
        if (side >= 1 && side <= 2) g_drive_until[side] = cmn::frame() + kDriveHold;
        const int mi = !m ? -1 : (std::strcmp(m, "parry") == 0 ? 0 : (std::strcmp(m, "rush") == 0 ? 1 : -1));
        if (mi >= 0 && side >= 1 && side <= 2 && g_dr[side][mi] >= 0) {
            const uint32_t col = vfx::data::tint_with_d((uint32_t)g_dr[side][mi], x * 255, y * 255, z * 255);
            out[0] = (float)((col & 0xFF) / 255.0); out[1] = (float)(((col >> 8) & 0xFF) / 255.0); out[2] = (float)(((col >> 16) & 0xFF) / 255.0);
            count("dr " + std::to_string(side) + "/" + m);
        }
    }
    if (out[0] == in[0] && out[1] == in[1] && out[2] == in[2]) return;
    char b[96];
    std::snprintf(b, sizeof b, "P%d %s %.3f,%.3f,%.3f -> %.3f,%.3f,%.3f", side, pn.c_str(), in[0], in[1], in[2], out[0], out[1], out[2]);
    g_last = b;
    if (!g_write) return;
    f[0] = out[0]; f[1] = out[1]; f[2] = out[2];
    ++g_written;
}

int pre_add_inner(void** argv) {
    ++g_calls;
    on_param(reinterpret_cast<MO*>(argv[1]), reinterpret_cast<MO*>(argv[3]));
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int pre_req_inner(void** argv) {
    ++g_calls;
    MO* lst = reinterpret_cast<MO*>(argv[3]);
    if (!lst || !API::get()->sdk()->managed_object->is_managed_object(lst)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto* td = lst->get_type_definition();
    auto it = g_list_m.find(td);
    if (it == g_list_m.end()) it = g_list_m.emplace(td, ListM{ ll::find_method_noargs(td, "get_Count"), ll::find_method_full(td, "get_Item(System.Int32)") }).first;
    if (!it->second.count || !it->second.item) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    auto rc = it->second.count->invoke(lst, std::vector<void*>{});
    if (rc.exception_thrown) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int32_t n = (int32_t)rc.dword;
    for (int32_t i = 0; i < n && i < 64; ++i) {
        auto ri = it->second.item->invoke(lst, std::vector<void*>{ ll::arg_i32(i) });
        if (!ri.exception_thrown && ri.ptr) on_param(reinterpret_cast<MO*>(argv[1]), reinterpret_cast<MO*>(ri.ptr));
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
    const int64_t t0 = perf::begin();
    const int r = pre_add_seh(argc, argv);
    perf::end(perf::HookBodyAddMaterial, t0);
    return r;
}
int pre_req(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int64_t t0 = perf::begin();
    const int r = pre_req_seh(argc, argv);
    perf::end(perf::HookBodyRequestMaterial, t0);
    return r;
}
void post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

}  // namespace

bool install(bool write) {
    auto* td = API::get()->tdb()->find_type("app.vfx.VfxActorEffectManager");
    if (!td) { cmn::journal("vfx body: app.vfx.VfxActorEffectManager missing, off"); return false; }
    int n_add = 0, n_req = 0;
    for (auto* m : td->get_methods()) {
        if (!m->get_function_raw()) continue;
        const std::string name = m->get_name();
        if (name == "addMaterialEffect" && n_add == 0) { if (m->add_hook(pre_add, post_nop, false)) ++n_add; }   // the Lua's get_method: one
        else if (name == "requestMaterialEffect") { if (m->add_hook(pre_req, post_nop, false)) ++n_req; }
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
