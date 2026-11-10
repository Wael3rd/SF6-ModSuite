// SF6 Slots -- which game outfits the player really owns, for the Random outfit's pool (true_owner.hpp)
#include "true_owner.hpp"
#include "slots.hpp"

#include <windows.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slots::trueown {
namespace {

// ---- the game's tables (DlcManager's fields, the costume inventory's records), read once ----
bool g_read = false, g_read_failed = false;
int g_costume_cat = -1;                                      // app.network.api.Enum.ItemCategory of the costume inventory
std::map<uint32_t, std::vector<int>> g_item_dlcs;            // costume record -> the DLCs that give it
std::map<int, uint32_t> g_dlc_app;                           // DLC id -> its Steam app id (0: none, not on Steam)
std::map<std::pair<int, int>, std::vector<uint32_t>> g_fc_rids;   // (fighter, costume) -> its records
std::set<uint32_t> g_default_rids;

Obj* array_item(Obj* arr, int i) {
    Obj* x = call_obj(arr, "Get", { arg_i32(i) });
    return x ? x : call_obj(arr, "get_Item", { arg_i32(i) });
}
int array_length(Obj* arr) {
    if (!arr) return 0;
    Ret r = call(arr, "get_Length");
    return r.exception_thrown ? 0 : static_cast<int32_t>(r.dword);
}
uint32_t u32_of(Obj* o, const char* name) {
    const void* p = field_ptr(o, name);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

bool read_tables() {
    if (g_read) return true;
    if (g_read_failed) return false;
    auto* dm = api().get_managed_singleton("app.DlcManager");
    auto* im = api().get_managed_singleton("app.InventoryManager");
    Obj* ci = im ? get_obj(im, "_Costume") : nullptr;
    bool init = false;
    if (!dm || !ci || !get_bool(dm, "IsInitialized", init) || !init) return false;   // asked again later
    Obj* main = get_obj(dm, "DlcDataMain");
    Obj* main_arr = main ? get_obj(main, "DataArray") : nullptr;
    for (int i = 0, n = array_length(main_arr); i < n; ++i) {
        Obj* r = array_item(main_arr, i);
        if (!r) continue;
        const int id = static_cast<int32_t>(u32_of(r, "id"));
        g_dlc_app[id] = static_cast<uint32_t>(strtoul(str(get_obj(r, "SteamAppId")).c_str(), nullptr, 10));
    }
    Obj* cos = get_obj(dm, "DlcItemCostume");
    Obj* cos_arr = cos ? get_obj(cos, "DataArray") : nullptr;
    for (int i = 0, n = array_length(cos_arr); i < n; ++i) {
        Obj* r = array_item(cos_arr, i);
        if (r) g_item_dlcs[u32_of(r, "Costume")].push_back(static_cast<int32_t>(u32_of(r, "Dlc")));
    }
    Ret rc = call(ci, "get_Category");
    if (!rc.exception_thrown) g_costume_cat = static_cast<int32_t>(rc.dword);
    Method* m_rec = method_of(ci->get_type_definition(), "GetMasterDataRecord", 1);
    // the costume records, 1.. until 300 empty in a row (read_carriers' way)
    for (uint32_t rid = 1, last = 0; m_rec && rid - last <= 300 && rid < 20000; ++rid) {
        Obj* rec = call_obj(m_rec, ci, { arg_i32(static_cast<int32_t>(rid)) });
        int32_t fid = 0, cn = -1;
        if (!rec || !get_i32(rec, "fighterId", fid) || !get_i32(rec, "costumeNo", cn)) continue;
        last = rid;
        g_fc_rids[{ fid, cn }].push_back(rid);
        bool def = false;
        if (get_bool(rec, "isDefault", def) && def) g_default_rids.insert(rid);
    }
    if (g_dlc_app.empty() || g_item_dlcs.empty() || g_fc_rids.empty() || g_costume_cat < 0) {
        g_read_failed = true;
        logf("true owner: the game's DLC / costume tables not read (DLCs %zu, DLC costumes %zu, records %zu, category %d): Random's "
             "pool keeps the game's list", g_dlc_app.size(), g_item_dlcs.size(), g_fc_rids.size(), g_costume_cat);
        return false;
    }
    g_read = true;
    logf("true owner: %zu DLCs, %zu costumes given by DLCs, %zu costume records", g_dlc_app.size(), g_item_dlcs.size(), g_fc_rids.size());
    return true;
}

// ---- Steam (the game's own steam_api64.dll, its flat API) ----
using AppsFn = void*(__cdecl*)();
using AskFn = bool(__cdecl*)(void*, uint32_t);
AppsFn g_apps = nullptr;
AskFn g_subscribed = nullptr, g_dlc_installed = nullptr;
bool g_steam_tried = false;
std::mutex g_own_mx;                                         // the hook asks from any thread
std::map<uint32_t, bool> g_app_owned;

bool steam_ok() {
    if (!g_steam_tried) {
        g_steam_tried = true;
        if (HMODULE h = GetModuleHandleA("steam_api64.dll")) {
            g_apps = reinterpret_cast<AppsFn>(GetProcAddress(h, "SteamAPI_SteamApps_v008"));
            g_subscribed = reinterpret_cast<AskFn>(GetProcAddress(h, "SteamAPI_ISteamApps_BIsSubscribedApp"));
            g_dlc_installed = reinterpret_cast<AskFn>(GetProcAddress(h, "SteamAPI_ISteamApps_BIsDlcInstalled"));
        }
        if (!g_apps || !g_subscribed) logf("true owner: no Steam (steam_api64.dll's SteamApps): Random's pool keeps the game's list");
    }
    return g_apps && g_subscribed && g_apps();
}
// The account owns that app (cached for the session)
bool app_owned(uint32_t app) {
    if (!app) return false;
    std::lock_guard lk(g_own_mx);
    auto it = g_app_owned.find(app);
    if (it != g_app_owned.end()) return it->second;
    void* apps = g_apps ? g_apps() : nullptr;
    const bool owned = apps && (g_subscribed(apps, app) || (g_dlc_installed && g_dlc_installed(apps, app)));
    g_app_owned[app] = owned;
    logf("true owner: Steam app %u %s", app, owned ? "owned" : "NOT owned");
    return owned;
}
// A DLC gives this costume record and the account owns that DLC on Steam
bool item_owned(uint32_t rid) {
    auto it = g_item_dlcs.find(rid);
    if (it == g_item_dlcs.end()) return false;
    for (int d : it->second) {
        auto a = g_dlc_app.find(d);
        if (a != g_dlc_app.end() && app_owned(a->second)) return true;
    }
    return false;
}

// ---- our hook of DlcManager.IsValidDlcItem(category, item): acts only on the thread of an Honest ----
Method* g_m_valid = nullptr;
unsigned int g_hook_id = 0;
bool g_hooked = false;
int g_arg0 = 2;                                              // argv index of its first parameter (1 static, 2 instance)
thread_local bool tl_honest = false;
struct Call { int cat; uint32_t item; };
thread_local std::vector<Call> tl_calls;                     // pre -> post, nested calls included

int pre_valid(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (tl_honest && argc > g_arg0 + 1)
        tl_calls.push_back({ static_cast<int>(reinterpret_cast<uintptr_t>(argv[g_arg0]) & 0xFFFFFFFFu),
                             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[g_arg0 + 1]) & 0xFFFFFFFFu) });
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_valid(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_honest || tl_calls.empty()) return;
    const Call c = tl_calls.back();
    tl_calls.pop_back();
    if (!ret_val || c.cat != g_costume_cat) return;
    *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(item_owned(c.item) ? 1u : 0u));
}
bool add_hook() {
    if (!g_m_valid) {
        TD* td = api().tdb()->find_type("app.DlcManager");
        g_m_valid = td ? method_of(td, "IsValidDlcItem", 2) : nullptr;
        if (!g_m_valid) { logf("true owner: DlcManager.IsValidDlcItem not found"); return false; }
        g_arg0 = g_m_valid->is_static() ? 1 : 2;
    }
    if (g_hooked) g_m_valid->remove_hook(g_hook_id);
    g_hook_id = g_m_valid->add_hook(pre_valid, post_valid, false);
    g_hooked = true;
    return true;
}
// Our answer is the one the game gets: asked about a costume Steam says the player does not have, the game hears no (none such: true)
bool hook_wins() {
    uint32_t probe = 0;
    for (auto& [rid, dlcs] : g_item_dlcs)
        if (!g_default_rids.count(rid) && !item_owned(rid)) { probe = rid; break; }
    if (!probe) return true;
    auto* dm = api().get_managed_singleton("app.DlcManager");
    std::vector<void*> args{ arg_i32(g_costume_cat), arg_i32(static_cast<int32_t>(probe)) };
    Ret r = g_m_valid->is_static() ? call(g_m_valid, nullptr, args) : call(g_m_valid, dm, args);
    return !r.exception_thrown && (r.byte & 1) == 0;
}

bool g_told_off = false;

}  // namespace

Honest::Honest() {
    if (!read_tables() || !steam_ok()) return;
    if (!g_hooked && !add_hook()) return;
    tl_honest = true;
    tl_calls.clear();
    ok_ = hook_wins();
    if (!ok_ && add_hook()) {                                // another hook (an unlock-all script reloaded) answers after ours
        ok_ = hook_wins();
        logf("true owner: our DlcManager.IsValidDlcItem hook added again (last): %s", ok_ ? "it answers now" : "still overridden");
    }
    if (!ok_) {
        tl_honest = false;
        if (!g_told_off) {
            g_told_off = true;
            logf("true owner: the game's list cannot be asked with Steam's DLC answer: Random leaves out every outfit given only by "
                 "DLCs Steam does not count as owned");
        }
    }
}

Honest::~Honest() {
    tl_honest = false;
    tl_calls.clear();
}

bool dlc_only_unowned(int fid, int cos) {
    if (!read_tables() || !steam_ok()) return false;
    auto it = g_fc_rids.find({ fid, cos });
    if (it == g_fc_rids.end()) return false;
    bool dlc = false;
    for (uint32_t rid : it->second) {
        if (g_default_rids.count(rid) || item_owned(rid)) return false;
        dlc |= g_item_dlcs.count(rid) != 0;
    }
    return dlc;
}

}  // namespace slots::trueown
