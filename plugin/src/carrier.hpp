// SF6 Slots — the carrier of the online alias. Pure logic, no game access (plugin/tests).
//
// While a fighter wears a slot chosen in Battle Settings, the save data and the network see one of
// the game's own outfits, the carrier, and the plugin mounts the slot over it on the player's screen
// only. The carrier is DriveTech (costume 4) when the game has it for the fighter, else Outfit 1
// (costume 0, every fighter's default): Arjun (fighter 34, 13/10 game update) has no DriveTech. It is
// decided at boot from the game's costume table, so a DriveTech added by a later update is taken by
// itself.
#pragma once

#include <set>
#include <utility>
#include <vector>

namespace slots::carrier {

constexpr int kOutfit1 = 0, kDriveTech = 4;

// The fighter's carrier from the costume numbers the game's costume table has for it; -1 when it has
// neither (the fighter is left out of the alias)
inline int choose(const std::set<int>& costumes) {
    if (costumes.count(kDriveTech)) return kDriveTech;
    if (costumes.count(kOutfit1)) return kOutfit1;
    return -1;
}

// A mirror on the carrier: another fighter of the battle is the same character in the same carrier.
// Both use the carrier's folder: the manifest overwritten could be the opponent's (or both
// fighters'), who would wear the player's slot on the player's screen. That character is not
// aliased in that battle. `worn`: (fighter, costume) of every fighter of the battle.
inline bool mirror(const std::vector<std::pair<int, int>>& worn, int fighter, int carrier) {
    if (carrier < 0) return false;
    int n = 0;
    for (auto& [f, c] : worn) if (f == fighter && c == carrier) ++n;
    return n >= 2;
}

// The save data holds a former carrier: it goes to the carrier. `saved` is the carrier the save was
// given in an earlier session (state.json; DriveTech when it names none: Slots Loader 1.11.1 and
// before knew no other). Moved only when a slot is the intent (the plugin put that costume there),
// or when the fighter does not have that costume at all (DriveTech, written for Arjun by 1.11.1 and
// before); otherwise the costume is the player's own choice.
inline bool stale(int saved, int carrier, int save_cos, bool intent, bool fighter_has_it) {
    return carrier >= 0 && saved != carrier && save_cos == saved && (intent || !fighter_has_it);
}

} // namespace slots::carrier
