// SF6 Slots — which costume slots the player sees. Pure logic, no game access (plugin/tests).
//
// A slot made from fighter F's Outfit N is shown only when the game counts that outfit as the
// player's. The game's answer is the list of costume numbers it lists for F
// (app.InventoryManager.CostumeInventory.GetCostumeNoList): the list of the character select
// screen (app.UIFlowUI10505.Param.GetCostumeId) and of Battle Settings
// (app.helper.hGUI.GetEnableFighterCostumeIdList). An outfit is listed when it is a default one,
// bought (in-game currency, shop), or given by a DLC the game counts as valid
// (app.DlcManager.IsValidDlcItem): whatever makes the game say yes counts, an unlock-all script
// that hooks the DLC checks included.
//
// Manager mode (common/manager_mode.hpp: the Slotsloader Manager installed this loader) follows the rules of
// the Manager's own Slotsloader fork instead: slots made from Outfit 1 or DriveTech are always shown and never
// asked about, and the check is off altogether while the Manager's opt-out marker is present
// (manager_marker.hpp). Outside Manager mode none of this exists: set_manager_rules is never called.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace slots::ownership {

enum class State : uint8_t { Unknown, NotOwned, Owned };

constexpr int kOutfit1   = 0;   // costume 0, every fighter's default outfit
constexpr int kDriveTech = 4;   // costume id 4 (DriveTech, C5)

class Book {
public:
    // Manager mode only (see the top): Outfit 1 and DriveTech always shown; check false: every slot shown,
    // nothing asked (the Manager's opt-out marker)
    void set_manager_rules(bool check) { manager_ = true; check_ = check; }
    bool check() const { return check_; }

    // The game listed `listed` for `fighter`; returns the state of `outfit` after that answer.
    // A list without Outfit 1 (costume 0, every fighter's default outfit) is no answer: the
    // inventory is not loaded yet, and the state stays as it was. With may_deny false (before the
    // login: shop purchases and DLC may not be known yet) only "owned" is taken.
    // Owned is kept for the session. Online, REFramework stops Lua scripts, an unlock-all script's
    // hooks with them, and the game would answer with what the player really owns: a slot shown
    // never disappears before the game is closed.
    State answer(int fighter, int outfit, const std::vector<uint32_t>& listed, bool may_deny = true) {
        if (!check_ || exempt(outfit)) return state(fighter, outfit);
        State& s = m_[{fighter, outfit}];
        if (outfit < 0 || !has(listed, 0)) return s;
        if (has(listed, static_cast<uint32_t>(outfit))) s = State::Owned;
        else if (may_deny && s != State::Owned) s = State::NotOwned;
        return s;
    }

    State state(int fighter, int outfit) const {
        auto it = m_.find({fighter, outfit});
        return it == m_.end() ? State::Unknown : it->second;
    }

    // Hidden only when the game said the outfit is not the player's. No answer yet, an outfit not
    // known (-1) or no check possible leave the slot shown, as before this check existed.
    bool shown(int fighter, int outfit) const {
        return !check_ || outfit < 0 || exempt(outfit) || state(fighter, outfit) != State::NotOwned;
    }

    // Still worth asking the game about (not owned yet)
    bool open(int fighter, int outfit) const {
        return check_ && outfit >= 0 && !exempt(outfit) && state(fighter, outfit) != State::Owned;
    }

private:
    // Manager mode: Outfit 1 and DriveTech are never asked about
    bool exempt(int outfit) const { return manager_ && (outfit == kOutfit1 || outfit == kDriveTech); }
    bool manager_ = false, check_ = true;
    static bool has(const std::vector<uint32_t>& v, uint32_t x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    }
    std::map<std::pair<int, int>, State> m_;
};

// ---- NSFW outfits (costume_mods_nsfw; the registry marks their slots "nsfw": true) ----
// Shown only while the player's "Display NSFW Outfits" setting is on (off by default). A hidden slot is not owned
// (the select screens and Battle Settings do not list it), like a slot made from an outfit the player does not own.
inline bool nsfw_shown(bool nsfw, bool display_nsfw) { return !nsfw || display_nsfw; }

// Taking the ownership back is not enough when an unlock-all script hooks app.DlcManager.IsValidDlcItem: every DLC
// record is then valid, the slots' included, and the game's list keeps them. So the list itself
// (CostumeInventory.GetCostumeNoList, read by the select screens and Battle Settings) is filtered: the positions of
// the fighter's NSFW outfits in it, last first (removed one at a time, the earlier positions stay right).
inline std::vector<int> nsfw_drops(const std::vector<uint32_t>& listed, const std::vector<uint32_t>& nsfw_outfits) {
    std::vector<int> out;
    for (int i = static_cast<int>(listed.size()) - 1; i >= 0; --i)
        if (std::find(nsfw_outfits.begin(), nsfw_outfits.end(), listed[static_cast<size_t>(i)]) != nsfw_outfits.end())
            out.push_back(i);
    return out;
}

// The colour the save gets when a hidden NSFW outfit's choice goes back to Outfit 1: the colour chosen with the
// outfit, else the one the save holds, when Outfit 1 has it; else Outfit 1's first colour. -1: leave the save's
// colour as it is (Outfit 1's colours not known).
inline int outfit1_colour(int chosen, int in_save, const std::vector<int>& outfit1_colours) {
    if (outfit1_colours.empty()) return -1;
    auto has = [&](int c) { return c >= 0 && std::find(outfit1_colours.begin(), outfit1_colours.end(), c) != outfit1_colours.end(); };
    if (has(chosen)) return chosen;
    if (has(in_save)) return in_save;
    return *std::min_element(outfit1_colours.begin(), outfit1_colours.end());
}

// The outfit's name in log lines: costume 0 -> "Outfit 1", 3 -> "Outfit 4", 4 -> "DriveTech"
inline std::string outfit_label(int costume) {
    if (costume == 4) return "DriveTech";
    if (costume >= 0 && costume < 4) return "Outfit " + std::to_string(costume + 1);
    return "costume " + std::to_string(costume);
}

} // namespace slots::ownership
