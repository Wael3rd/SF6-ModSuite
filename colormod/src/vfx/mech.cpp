#include "mech.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings

#include "../common.hpp"
#include "../perf.hpp"
#include "data.hpp"

namespace vfx::mech {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;

constexpr uint64_t kMechHold = 150;          // SF6_DriveRecolor.lua MECH_HOLD [1021]: a whole Drive Impact fits in it

std::mutex g_mx;                             // the effect hooks may run on several threads
bool g_installed = false;
API::Method* g_go_get_component = nullptr;   // via.GameObject.getComponent(System.Type)
MO* g_pb_type = nullptr;                     // System.Type of app.PlayerBehavior
MO* g_mgr_type = nullptr;                    // System.Type of app.vfx.VfxActorEffectManager
API::Method* g_scene_current = nullptr;      // via.SceneManager.get_CurrentScene
API::Method* g_find_components = nullptr;    // via.Scene.findComponents(System.Type)
API::Method* g_arr_len = nullptr;            // System.Array.GetLength(Int32)
API::Method* g_arr_get = nullptr;            // System.Array.GetValue(Int32)
std::unordered_map<API::TypeDefinition*, API::Method*> g_get_go;   // per manager type: get_GameObject

std::unordered_map<uint64_t, int> g_mgr_side;   // Lua mgr_vus + mgr_addr: address -> side (0 = seen, not a player)
uint64_t g_mgr_addr[3] = {};
int64_t g_di_cont[3] = { -1, -1, -1 };          // Lua di_cont (-1 = nil)
const char* g_last_mech[3] = {};
uint64_t g_mech_at[3] = {};
uint64_t g_req_all = 0, g_req_n = 0;
std::string g_req_lab;
std::map<std::string, uint64_t> g_seen[3];      // Lua req_seen[side] (vfx_debug_on.txt only)
constexpr uint64_t kSideTtl = 120;              // side_cached: a player's manager is resolved again after 2 s,
constexpr uint64_t kNoSideTtl = 10;             // any other manager (or one not resolvable yet) after 10 frames
constexpr size_t kSideMax = 256;
struct SideAt { int side; uint64_t at; };
std::unordered_map<uint64_t, SideAt> g_body_side;   // side_cached

int side_of_manager(MO* mgr) {                  // Lua: this:get_GameObject() -> side_of(go)
    auto* td = mgr->get_type_definition();
    auto it = g_get_go.find(td);
    if (it == g_get_go.end()) it = g_get_go.emplace(td, ll::find_method_noargs(td, "get_GameObject")).first;
    if (!it->second) return 0;
    auto rg = it->second->invoke(mgr, std::vector<void*>{});
    MO* go = rg.exception_thrown ? nullptr : reinterpret_cast<MO*>(rg.ptr);
    if (!go) return 0;
    auto rp = g_go_get_component->invoke(go, std::vector<void*>{ (void*)g_pb_type });
    MO* pb = rp.exception_thrown ? nullptr : reinterpret_cast<MO*>(rp.ptr);
    int32_t id = -1;
    if (!pb || !ll::read_field_i32(pb, "_PlayerID", id)) return 0;
    return id + 1;
}

}  // namespace

bool install() {
    auto* tdb = API::get()->tdb();
    g_go_get_component = tdb->find_method("via.GameObject", "getComponent(System.Type)");
    auto* pb_td = tdb->find_type("app.PlayerBehavior");
    g_pb_type = pb_td ? pb_td->get_runtime_type() : nullptr;
    auto* mgr_td = tdb->find_type("app.vfx.VfxActorEffectManager");
    g_mgr_type = mgr_td ? mgr_td->get_runtime_type() : nullptr;
    g_scene_current = tdb->find_method("via.SceneManager", "get_CurrentScene");
    g_find_components = tdb->find_method("via.Scene", "findComponents(System.Type)");
    g_arr_len = tdb->find_method("System.Array", "GetLength(System.Int32)");
    g_arr_get = tdb->find_method("System.Array", "GetValue(System.Int32)");
    g_installed = g_go_get_component && g_pb_type;
    if (!g_installed) cmn::journal("vfx mech: getComponent / app.PlayerBehavior missing, off");
    else cmn::journal("vfx mech: on (vfx_body native: replaces SF6_DriveRecolor's requestEffect hook; lua: shadow, vfx_mech_probe)");
    return g_installed;
}

void on_request(uint64_t mgr, uint32_t cat, uint32_t cont, uint32_t el) try {
    if (!g_installed) return;
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_req_all;                                // Lua req_all (every call of the hooked overloads)
    int side = (mgr && mgr == g_mgr_addr[1]) ? 1 : ((mgr && mgr == g_mgr_addr[2]) ? 2 : 0);
    if (!side) {
        auto it = g_mgr_side.find(mgr);
        if (it != g_mgr_side.end()) return;     // seen, not a player
        MO* m = reinterpret_cast<MO*>(mgr);
        side = (mgr && API::get()->sdk()->managed_object->is_managed_object(m)) ? side_of_manager(m) : 0;
        g_mgr_side[mgr] = side;
        if (side != 1 && side != 2) return;
        g_mgr_addr[side] = mgr;                 // first effect of the side: its manager is known from now on
    }
    ++g_req_n;
    if (perf::vfx_debug()) {                    // the per-label counters of vfx_mech_probe: a string map, debug only
        char lab[48];
        std::snprintf(lab, sizeof lab, "%u/%u/%u", cont, cat, el);
        g_req_lab = lab;
        ++g_seen[side][lab];
    }
    const char* m = vfx::data::label_mech(cont, cat, el, g_di_cont[side]);
    if (m) { g_last_mech[side] = m; g_mech_at[side] = cmn::frame(); }
} catch (...) {}                                // never reaches the engine's SEH guard (it would blacklist the manager)

int side_now(void* mgr) try {                   // resolved at every call, like the Lua's material hooks
    if (!g_installed || !mgr || !API::get()->sdk()->managed_object->is_managed_object(mgr)) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    return side_of_manager(reinterpret_cast<MO*>(mgr));
} catch (...) { return 0; }

int side_cached(void* mgr) try {
    if (!g_installed || !mgr) return 0;
    const uint64_t key = reinterpret_cast<uint64_t>(mgr), now = cmn::frame();
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_body_side.find(key);
    if (it != g_body_side.end() && now - it->second.at <= (it->second.side ? kSideTtl : kNoSideTtl)) return it->second.side;
    const int side = API::get()->sdk()->managed_object->is_managed_object(mgr) ? side_of_manager(reinterpret_cast<MO*>(mgr)) : 0;
    if (g_body_side.size() >= kSideMax) g_body_side.clear();
    g_body_side[key] = SideAt{ side, now };
    return side;
} catch (...) { return 0; }

const char* recent(int side) {
    if (side != 1 && side != 2) return nullptr;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_last_mech[side] || cmn::frame() - g_mech_at[side] > kMechHold) return nullptr;
    return g_last_mech[side];
}

uint64_t manager(int side) {
    if (side != 1 && side != 2) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    return g_mgr_addr[side];
}

int discover() try {
    if (!g_installed || !g_mgr_type || !g_scene_current || !g_find_components || !g_arr_len || !g_arr_get) return 0;
    void* sm = API::get()->get_native_singleton("via.SceneManager");
    if (!sm) return 0;
    auto rs = g_scene_current->invoke(reinterpret_cast<MO*>(sm), std::vector<void*>{});
    MO* scene = rs.exception_thrown ? nullptr : reinterpret_cast<MO*>(rs.ptr);
    if (!scene) return 0;
    auto rc = g_find_components->invoke(scene, std::vector<void*>{ (void*)g_pb_type });
    MO* comps = rc.exception_thrown ? nullptr : reinterpret_cast<MO*>(rc.ptr);
    if (!comps) return 0;
    auto rl = g_arr_len->invoke(comps, std::vector<void*>{ reinterpret_cast<void*>(static_cast<intptr_t>(0)) });
    const int32_t n = rl.exception_thrown ? 0 : (int32_t)rl.dword;
    uint64_t found[3] = {};
    int count[3] = {};
    for (int32_t i = 0; i < n; ++i) {
        auto rv = g_arr_get->invoke(comps, std::vector<void*>{ reinterpret_cast<void*>(static_cast<intptr_t>(i)) });
        MO* pb = rv.exception_thrown ? nullptr : reinterpret_cast<MO*>(rv.ptr);
        int32_t id = -1;
        if (!pb || !ll::read_field_i32(pb, "_PlayerID", id) || id < 0 || id > 1) continue;
        API::Method* get_go = ll::find_method_noargs(pb->get_type_definition(), "get_GameObject");
        auto rg = get_go ? get_go->invoke(pb, std::vector<void*>{}) : reframework::InvokeRet{};
        MO* go = (get_go && !rg.exception_thrown) ? reinterpret_cast<MO*>(rg.ptr) : nullptr;
        if (!go) continue;
        auto rm = g_go_get_component->invoke(go, std::vector<void*>{ (void*)g_mgr_type });
        MO* mgr = rm.exception_thrown ? nullptr : reinterpret_cast<MO*>(rm.ptr);
        if (!mgr) continue;
        found[id + 1] = reinterpret_cast<uint64_t>(mgr);
        ++count[id + 1];
    }
    std::lock_guard<std::mutex> lk(g_mx);
    int set = 0;
    for (int side = 1; side <= 2; ++side) {
        if (g_mgr_addr[side] || count[side] != 1) continue;          // one fighter per side, else the first effect decides
        g_mgr_addr[side] = found[side];
        g_mgr_side[found[side]] = side;
        ++set;
    }
    return set;
} catch (...) { return 0; }

void on_combat_exit() {                         // Lua combat exit: mgr_addr and mgr_vus emptied (last_mech kept)
    std::lock_guard<std::mutex> lk(g_mx);
    g_mgr_side.clear();
    g_body_side.clear();
    g_mgr_addr[1] = g_mgr_addr[2] = 0;
}

void set_di_cont(int64_t c1, int64_t c2) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_di_cont[1] = c1;
    g_di_cont[2] = c2;
}

std::string probe() {
    std::lock_guard<std::mutex> lk(g_mx);
    auto seen = [](const std::map<std::string, uint64_t>& s) {
        std::string o;
        for (auto& [k, n] : s) { if (!o.empty()) o += ","; o += k + ":" + std::to_string(n); }
        return o;
    };
    auto mech = [](int side) {
        if (!g_last_mech[side]) return std::string("nil");
        const bool recent = cmn::frame() - g_mech_at[side] <= kMechHold;
        return std::string(g_last_mech[side]) + (recent ? "" : "(old)");
    };
    char mg[64];
    std::snprintf(mg, sizeof mg, "mgr1=%llX mgr2=%llX", (unsigned long long)g_mgr_addr[1], (unsigned long long)g_mgr_addr[2]);
    return "mech1=" + mech(1) + " mech2=" + mech(2) + " " + mg + " req_all=" + std::to_string(g_req_all) + " req_n=" +
           std::to_string(g_req_n) + " lab=" + g_req_lab + " di_cont=" + std::to_string(g_di_cont[1]) + "/" + std::to_string(g_di_cont[2]) +
           " seen1=" + seen(g_seen[1]) + " seen2=" + seen(g_seen[2]);
}

}  // namespace vfx::mech
