// SF6 Slots — the carrier of the online alias. Pure logic, no game access (plugin/tests).
//
// While a fighter wears a slot chosen in Battle Settings, the save data and the network see one of
// the game's own outfits, the carrier, and the plugin dresses the player's own fighter in the slot on
// the player's screen only. The carrier is Outfit 1 (costume 0, every fighter's default) for every
// fighter (since Slots Loader 1.15: DriveTech before, Outfit 1 for a fighter without it). A fighter
// with no Outfit 1 in the game's costume table is left out of the alias.
#pragma once

#include <set>

namespace slots::carrier {

constexpr int kOutfit1 = 0, kDriveTech = 4;

// The fighter's carrier from the costume numbers the game's costume table has for it; -1 when it has
// no Outfit 1 (the fighter is left out of the alias)
inline int choose(const std::set<int>& costumes) {
    return costumes.count(kOutfit1) ? kOutfit1 : -1;
}

// The save data holds a former carrier: it goes to the carrier. `saved` is the carrier the save was
// given in an earlier session (state.json; DriveTech when it names none: Slots Loader 1.14 and
// before used DriveTech for every fighter that has it). Moved only when a slot is the intent (the
// plugin put that costume there), or when the fighter does not have that costume at all; otherwise
// the costume is the player's own choice.
inline bool stale(int saved, int carrier, int save_cos, bool intent, bool fighter_has_it) {
    return carrier >= 0 && saved != carrier && save_cos == saved && (intent || !fighter_has_it);
}

} // namespace slots::carrier
