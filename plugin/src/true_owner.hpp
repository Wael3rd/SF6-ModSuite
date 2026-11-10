// SF6 Slots -- which game outfits the player really owns, for the Random outfit in ranked matches (Wael 10/11: "never an outfit we do
// not have, even with an unlock-all script ... only for ranked"; replays, local modes and the other online modes draw from the whole
// Costume row, unlock-all included).
//
// The game's answer (CostumeInventory.GetCostumeNoList, ownership.hpp) counts an outfit a DLC gives when DlcManager.IsValidDlcItem
// says yes; unlock_all.lua forces that (and IsValidDlc, IsValidPackageDlc, IsNoPackageDlc, IsSubscribed, IsPlayGoMode) to true. A
// native hook of ours answering instead was tried (10/11): REFramework runs the Lua post-hook after ours whatever the order they were
// added in, so the game's own logic cannot be asked honestly. What is read instead (probes g98 - g102, 10/11):
//   - DlcManager.DlcItemCostume's records (fields): costume record -> a DLC that gives it; DlcDataMain's: DLC id -> SteamAppId
//     ("" off Steam: Ryu's Outfit 2 and 3 have DLC 8 alone, a PlayStation pack; on Steam they are bought, never given).
//   - Steam (steam_api64.dll's flat API: SteamApps_v008, BIsSubscribedApp / BIsDlcInstalled): the DLCs the account owns. Native: a
//     Lua hook cannot change it.
//   - The costume inventory's own getters (not DlcManager's): a record's isDefault, ExistsSaveData (an outfit bought, or a slot the
//     loader added, is in the save).
// An outfit is the player's: a default one, one in the save, one no DLC gives (the game's list counts it on other grounds the
// unlock-all script does not touch), or one a DLC the account owns on Steam gives.
#pragma once

namespace slots::trueown {

// Outfit `cos` of fighter `fid` is the player's (rules above). Not known (the tables not read, no Steam): true, the game's answer
// stands. Game thread.
bool owned(int fid, int cos);

}  // namespace slots::trueown
