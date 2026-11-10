// SF6 Slots -- which game outfits the player really owns, for the Random outfit's pool (Wael 10/11: "never an outfit we do not have,
// even with an unlock-all script").
//
// The game's answer (CostumeInventory.GetCostumeNoList, ownership.hpp) counts an outfit a DLC gives when DlcManager.IsValidDlcItem
// says yes; unlock_all.lua forces that (and IsValidDlc, IsValidPackageDlc, IsNoPackageDlc, IsSubscribed, IsPlayGoMode) to true, and
// ValidPackageDlcList then holds every DLC (read 10/11: 17 of 17). The slots keep that answer on purpose (ownership.hpp); Random must not.
// What is true (read-only probes g98 - g102, 10/11):
//   - DlcManager.DlcItemCostume's records: costume record id -> a DLC that gives it (78 records; Ryu's Outfit 2 and 3: DLC 8 alone, a
//     PlayStation pack, so on Steam they are bought, never given); DlcDataMain's records: DLC id -> SteamAppId ("" off Steam).
//   - Steam answers which DLCs the account owns (steam_api64.dll's flat API: SteamApps_v008, BIsSubscribedApp / BIsDlcInstalled),
//     native: a Lua hook cannot change it.
//   - Purchases (Fighter Coins) come from the server and are not readable: the game's own list must still decide those.
// So, while an Honest lives on this thread, our own post-hook of IsValidDlcItem answers from Steam for this thread's calls; the game's
// list asked meanwhile is the game's own logic with a true DLC answer. Our hook must run after the Lua one (REFramework runs a method's
// callbacks in the order they were added): it is added at the first need (after autorun scripts), and every Honest checks it on an item
// Steam says the player does not have; answered yes, the hook is added again (last), and checked once more. When nothing makes that
// work (no Steam, the tables not read, the check failing), ok() is false and the caller leaves out every outfit given only by DLCs
// Steam does not count as owned (dlc_only_unowned): no purchase seen then, but never an outfit the player does not have.
#pragma once

#include <cstdint>

namespace slots::trueown {

class Honest {
public:
    Honest();                       // game thread
    ~Honest();
    bool ok() const { return ok_; }
    Honest(const Honest&) = delete;
    Honest& operator=(const Honest&) = delete;
private:
    bool ok_ = false;
};

// Outfit `cos` of fighter `fid` is given only by DLCs (it has DLC records) none of which the player owns on Steam, and it is not a
// default outfit. False when not known (no tables, no Steam). Game thread.
bool dlc_only_unowned(int fid, int cos);

}  // namespace slots::trueown
