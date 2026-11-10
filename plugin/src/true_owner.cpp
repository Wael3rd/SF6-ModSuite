// SF6 Slots -- which game outfits the player really owns, for the Random outfit in ranked matches (true_owner.hpp)
#include "true_owner.hpp"
#include "slots.hpp"

#include <windows.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slots::trueown {
namespace {

// ---- the game's tables (DlcManager's fields, the costume inventory's records), read once ----
bool g_read = false, g_read_failed = false;
std::map<uint32_t, std::vector<int>> g_item_dlcs;            // costume record -> the DLCs that give it
std::map<int, uint32_t> g_dlc_app;                           // DLC id -> its Steam app id (0: none, not on Steam)
std::map<std::pair<int, int>, std::vector<uint32_t>> g_fc_rids;   // (fighter, costume) -> its records
std::set<uint32_t> g_default_rids;

Obj* costume_inventory() {
    auto* im = api().get_managed_singleton("app.InventoryManager");
    return im ? get_obj(im, "_Costume") : nullptr;
}
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
    Obj* ci = costume_inventory();
    bool init = false;
    if (!dm || !ci || !get_bool(dm, "IsInitialized", init) || !init) return false;   // asked again later
    Obj* main = get_obj(dm, "DlcDataMain");
    Obj* main_arr = main ? get_obj(main, "DataArray") : nullptr;
    for (int i = 0, n = array_length(main_arr); i < n; ++i) {
        Obj* r = array_item(main_arr, i);
        if (!r) continue;
        g_dlc_app[static_cast<int32_t>(u32_of(r, "id"))] = static_cast<uint32_t>(strtoul(str(get_obj(r, "SteamAppId")).c_str(), nullptr, 10));
    }
    Obj* cos = get_obj(dm, "DlcItemCostume");
    Obj* cos_arr = cos ? get_obj(cos, "DataArray") : nullptr;
    for (int i = 0, n = array_length(cos_arr); i < n; ++i) {
        Obj* r = array_item(cos_arr, i);
        if (r) g_item_dlcs[u32_of(r, "Costume")].push_back(static_cast<int32_t>(u32_of(r, "Dlc")));
    }
    Method* m_rec = method_of(ci->get_type_definition(), "GetMasterDataRecord", 1);
    for (uint32_t rid = 1, last = 0; m_rec && rid - last <= 300 && rid < 20000; ++rid) {   // read_carriers' way: until 300 empty
        Obj* rec = call_obj(m_rec, ci, { arg_i32(static_cast<int32_t>(rid)) });
        int32_t fid = 0, cn = -1;
        if (!rec || !get_i32(rec, "fighterId", fid) || !get_i32(rec, "costumeNo", cn)) continue;
        last = rid;
        g_fc_rids[{ fid, cn }].push_back(rid);
        bool def = false;
        if (get_bool(rec, "isDefault", def) && def) g_default_rids.insert(rid);
    }
    if (g_dlc_app.empty() || g_item_dlcs.empty() || g_fc_rids.empty()) {
        g_read_failed = true;
        logf("true owner: the game's DLC / costume tables not read (DLCs %zu, DLC costumes %zu, records %zu): Random in ranked "
             "matches keeps the game's list", g_dlc_app.size(), g_item_dlcs.size(), g_fc_rids.size());
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
std::map<uint32_t, bool> g_app_owned;

bool steam_ok() {
    if (!g_steam_tried) {
        g_steam_tried = true;
        if (HMODULE h = GetModuleHandleA("steam_api64.dll")) {
            g_apps = reinterpret_cast<AppsFn>(GetProcAddress(h, "SteamAPI_SteamApps_v008"));
            g_subscribed = reinterpret_cast<AskFn>(GetProcAddress(h, "SteamAPI_ISteamApps_BIsSubscribedApp"));
            g_dlc_installed = reinterpret_cast<AskFn>(GetProcAddress(h, "SteamAPI_ISteamApps_BIsDlcInstalled"));
        }
        if (!g_apps || !g_subscribed) logf("true owner: no Steam (steam_api64.dll's SteamApps): Random in ranked matches keeps the game's list");
    }
    return g_apps && g_subscribed && g_apps();
}
// The account owns that app (cached for the session)
bool app_owned(uint32_t app) {
    if (!app) return false;
    auto it = g_app_owned.find(app);
    if (it != g_app_owned.end()) return it->second;
    void* apps = g_apps ? g_apps() : nullptr;
    const bool owned = apps && (g_subscribed(apps, app) || (g_dlc_installed && g_dlc_installed(apps, app)));
    g_app_owned[app] = owned;
    logf("true owner: Steam app %u %s", app, owned ? "owned" : "NOT owned");
    return owned;
}

}  // namespace

bool owned(int fid, int cos) {
    if (!read_tables() || !steam_ok()) return true;
    auto it = g_fc_rids.find({ fid, cos });
    if (it == g_fc_rids.end()) return true;                  // not a game outfit the tables know: the game's answer
    Obj* ci = costume_inventory();
    static Method* m_save = ci ? method_of(ci->get_type_definition(), "ExistsSaveData", 1) : nullptr;
    bool dlc = false;
    for (uint32_t rid : it->second) {
        if (g_default_rids.count(rid)) return true;
        if (m_save && ci) {
            Ret r = call(m_save, ci, { arg_i32(static_cast<int32_t>(rid)) });
            if (!r.exception_thrown && (r.byte & 1)) return true;   // bought (in the save)
        }
        auto d = g_item_dlcs.find(rid);
        if (d == g_item_dlcs.end()) continue;
        dlc = true;
        for (int id : d->second) {
            auto a = g_dlc_app.find(id);
            if (a != g_dlc_app.end() && app_owned(a->second)) return true;
        }
    }
    return !dlc;                                             // no DLC gives it: the game's list counts it on other grounds
}

}  // namespace slots::trueown
