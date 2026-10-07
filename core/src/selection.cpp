// selection.cpp - increment 7: effective selection (ADR-0014, docs/SELECTION.md).
//
// What a fighter really wears when a mod dresses it differently from what the game shows. SF6-SlotsLoader's online
// alias keeps the game (and the network) on DriveTech and mounts a slot over it, sometimes with the slot's colour
// placed under DriveTech's first colour; a mod that reads the game's costume / colour of that fighter (ColorModNative's
// Drive "Follow" squares) asks here first. Tested 2026-10-06 in Combo Trials: Lily shown as DriveTech colour 1, worn as
// slot 13 colour 4.

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

#include "sf6core_internal.h"

namespace {

struct Effective { int32_t costume = -1, colour = -1; SF6CoreClient* client = nullptr; };
std::mutex g_mx;
std::map<std::pair<int32_t, int32_t>, Effective> g_eff;    // (fighter, shown costume) -> what it wears

}  // namespace

SF6CoreStatus SF6CORE_CALL api_sel_set_effective(SF6CoreClient* client, int32_t fighter, int32_t shown_costume,
                                                 int32_t real_costume, int32_t real_colour) {
    if (!client || fighter < 0 || shown_costume < 0) return SF6CORE_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(g_mx);
    const auto key = std::make_pair(fighter, shown_costume);
    auto it = g_eff.find(key);
    if (real_costume < 0) {
        if (it != g_eff.end()) {
            g_eff.erase(it);
            core_log(SF6CORE_LOG_INFO, client->client_id, "selection: fighter %d in costume %d is itself again", fighter, shown_costume);
        }
        return SF6CORE_OK;
    }
    if (it == g_eff.end() || it->second.costume != real_costume || it->second.colour != real_colour)
        core_log(SF6CORE_LOG_INFO, client->client_id, "selection: fighter %d shown in costume %d wears costume %d, colour %d",
                 fighter, shown_costume, real_costume, real_colour);
    g_eff[key] = { real_costume, real_colour, client };
    return SF6CORE_OK;
}

// `costume` is the costume the caller sees: the shown one (the game's selection) or the real one (a model's colour
// folder already shows the slot: Lily's colour file Product/Model/esf/esf012/014/..., tested 06/10).
int32_t SF6CORE_CALL api_sel_get_effective(int32_t fighter, int32_t costume, int32_t* real_costume, int32_t* real_colour) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (const auto& [key, e] : g_eff) {
        if (key.first != fighter || !e.client || !e.client->active) continue;
        if (key.second != costume && e.costume != costume) continue;
        if (real_costume) *real_costume = e.costume;
        if (real_colour) *real_colour = e.colour;
        return 1;
    }
    return 0;
}
