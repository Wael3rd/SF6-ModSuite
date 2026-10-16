# SF6_CostumeSlotsNative (native REFramework plugin)

The in-game part of the costume slots and of the stage slots, in C++. Since 1.9.0 it replaces the two Lua
scripts (`SF6_CostumeSlots.lua`, `SF6_StageSlots.lua`): no Lua state, no garbage collector, and it keeps
running during online matches, where official REFramework stops Lua scripts. It renames the old scripts
to `.lua.old` when it finds them in `reframework/autorun/` (an update made by hand).

The file keeps its old name so that an update overwrites it.

## Driven by events

Nothing is watched from frame to frame. Hooks on the game's own methods only record what happened (hooks
can run on any thread); the work is done on the next `LateUpdateBehavior`, on the game thread, and stops
when it is done:

| Event (hooked method) | Work |
|---|---|
| boot (first frames, once) | colour records of the slots, their select screen data and colour squares, each character's carrier (below) with its folder and colours, ownership |
| title screen destroyed (`app.menu.UIFlowTitle.onDestroy`) | ownership given back (the game clears DLC ownership at login) |
| character select started (`app.battle.bBattleFighterSelectFlow.start`, `app.UIFlowUI10501.Start`) | ownership, save safety net |
| Battle Settings shown (`app.UIFlowMatchingSetting.Param.CreatedObject` / `ShowedObject`) | the save shows the slot chosen there |
| Battle Settings closed (`HidObject`, `OnEnd`; only after an open) | the slot becomes the intent, the save gets the carrier (what other players see) |
| an outfit's visual manifest created (`app.battle.assets.FighterVisualHolder..ctor`: the game mounted its folder), outside the character select screen | for two seconds (the loading screen), for each character with an intent: once the carrier's manifest is there, the slot's folder is mounted, its manifest copied over the carrier's, the colour placed under a colour the carrier has |
| Battle Settings shown, character select started (between matches) | the colours of the last alias put back |
| stage select shown / hidden, focus or preview changed (`app.menu.UIFlowStageSelect.Param`) | the variant's name between UP / DOWN hints, its preview, each stage's thumbnail in the list (core 0.8.1+) |
| UP / DOWN on the stage select agent (`app.UIAgent.InputUp` / `InputDown`) | next / previous variant, `state.json` for the loader |
| VS screen activated or rewritten (`app.esports.VSInfoOffline`) | the variant's name and image |

Without stage mods, the stage hooks are not installed; without costume slots, the costume ones are not. A
fight triggers none of these events: during a fight the plugin reads nothing but its own memory.

## The carrier of the online alias

A slot chosen in Battle Settings is not a costume the other players have: the save data (and the network) keep one
of the character's own outfits, the carrier, and the plugin shows the slot over it on the player's screen only. The
carrier is **DriveTech** (costume 4) when the game has it for the character, else **Outfit 1** (costume 0): Arjun
(fighter 34, 13/10 update) has no DriveTech. It is read at boot from the game's costume table
(`CostumeInventory.GetMasterDataRecord`: `fighterId`, `costumeNo`), so a DriveTech added by a later update is used
by itself; if the table gives no answer for 30 s, from the scene's folders (`fighterNNN/esfNNNv04`, else `v00`).
A character without either, or whose carrier folder is not in the scene after 10 s, is left out of the alias (one
log line) and the others go on. `state.json` keeps the carrier the save was given when it is not DriveTech
(`"carrier": 0`); when it changes, the safety net moves a save still holding the former carrier (with a slot as the
intent, or a costume the character does not have) to the new one, in a colour it has.

## Slots of outfits the player does not own

A slot made from a character's Outfit 2 (or 3, 4) is shown only if the game counts that outfit as the player's.
The plugin asks the game the question its own screens ask: `app.InventoryManager.CostumeInventory.GetCostumeNoList`
(the costume numbers of a character the player can pick), which the character select screen
(`app.UIFlowUI10505.Param.GetCostumeId`) and Battle Settings (`app.helper.hGUI.GetEnableFighterCostumeIdList`) list.
An outfit is in it when it is a default one, bought (in-game currency, shop) or given by a DLC the game counts as
valid (`app.DlcManager.IsValidDlcItem`, through `SystemSaveInventory.Contains`). An unlock-all script that hooks the
DLC checks makes every outfit valid: every slot is shown then.

- Hidden means not owned: the slot's record is not given (`AddDlcSaveData`), or is taken back (`DeleteDlcSaveData`)
  if an earlier session gave it, so the game's own list leaves it out. The slot stays in the costume pak (its scene
  and files): a replay or a saved setting that names it still finds it.
- Asked at boot (an answer then can only show a slot), then each time the title screen is left and each time the
  character select screen or Battle Settings opens; not during an online battle, where REFramework stops Lua
  scripts (an unlock-all script's hooks with them) and the game would answer with what the player really owns.
  An outfit seen owned once stays so until the game is closed: such a slot never disappears during a session.
- A list without Outfit 1 (every character's default) is no answer (the inventory is not loaded yet); if the method
  is missing (a game update), every slot is shown as before.
- A Battle Settings choice of a hidden slot is kept (`state.json`) but not applied online until the outfit is the
  player's: the menu shows the carrier in its place.
- Log, one line per hidden slot: `[F1] slot v05 (Outfit I, <mod>) hidden: made for Outfit 2, which the player does
  not own`; `... shown: the player owns Outfit 2 now` if it changes.
- Probe (read-only, off by default): create `reframework\data\SF6_CostumeSlots_data\ownership_probe.txt` (empty) and
  the log gets, at up to 8 ownership passes a session, what each candidate method answers for the outfits of the
  characters that have slots (`probe F1 list=[0,1,4,...] | c1 r2: has 1 save 0 default 0 valid 1 dlc 1 ...`).

## Files

- `reframework/data/SF6_Costumes_Data/registry.json` (written by the loader): slots, record ids, colour squares.
- `reframework/data/SF6_CostumeSlots_data/state.json`: the slot and colour chosen in Battle Settings, per character
  (and the carrier the save was given, when it is not DriveTech).
- `reframework/data/SF6_StageSlots_Data/registry.json` (written by the loader) and `state.json` (the chosen variants,
  watched by the loader).
- Log: `reframework/data/SF6_CostumeSlots_data/native_log.txt` (the previous one is kept as `native_log.old.txt`).

## Build

`build.bat` at the root of the repository builds it with the rest, into `build\SF6_CostumeSlotsNative.dll`.
Copy it into `reframework\plugins\` **with the game closed**. Headers: REFramework plugin API v1.5.8
(`include/reframework`), `/std:c++20 /EHa /W4`.

## Known limits

- Mirror on the carrier: when another fighter of the battle is the same character in the same carrier, that character
  is not aliased in that battle (both use the carrier's folder: the manifest overwritten could be the opponent's, who
  would wear the slot on the player's screen); the player wears the carrier. Log:
  `[F34] alias skipped: mirror on the carrier (Outfit 1)`.
- `via.Folder.activate` / `deactivate` are not usable as events: the engine mounts folders without going through
  them (a hook on them sees only the plugin's own calls). The manifest's constructor is.
