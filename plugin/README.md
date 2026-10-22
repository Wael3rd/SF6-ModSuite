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
| boot (first frames, once) | colour records of the slots, their select screen data and colour squares, each character's carrier (below) and its colours, ownership |
| title screen destroyed (`app.menu.UIFlowTitle.onDestroy`) | ownership given back (the game clears DLC ownership at login) |
| character select started (`app.battle.bBattleFighterSelectFlow.start`, `app.UIFlowUI10501.Start`) | ownership, save safety net |
| Battle Settings shown (`app.UIFlowMatchingSetting.Param.CreatedObject` / `ShowedObject`) | the save shows the slot chosen there |
| Battle Settings closed (`HidObject`, `OnEnd`; only after an open) | the slot becomes the intent, the save gets the carrier (what other players see) |
| the game asks for / unloads a fighter's files (`app.battle.BattleFlowMap.requestNotManagedFighterAssetLoad` / `...Unload`, online `requestManagedFighterAssetLoad`), a reload (`SetupReloadBattleAsset`), the fighters built (`setupFighterAsset`) | the online alias (below), at once in the hook: these run on the flow map's own thread, where the game itself reads the battle description |
| stage select shown / hidden, focus or preview changed (`app.menu.UIFlowStageSelect.Param`) | the variant's name between UP / DOWN hints, its preview, each stage's thumbnail in the list (core 0.8.1+) |
| UP / DOWN on the stage select agent (`app.UIAgent.InputUp` / `InputDown`) | next / previous variant, `state.json` for the loader |
| VS screen activated or rewritten (`app.esports.VSInfoOffline`) | the variant's name and image |

Without stage mods, the stage hooks are not installed; without costume slots, the costume ones are not. A
fight triggers none of these events: during a fight the plugin reads nothing but its own memory.

## The online alias

A slot chosen in Battle Settings is not a costume the other players have: the save data (and the network) keep one
of the character's own outfits, the carrier, and the plugin dresses the player's own fighter in the slot on the
player's screen only. The carrier is **Outfit 1** (costume 0) for every character (Slots Loader 1.15; DriveTech
before, Outfit 1 for a character without it). It is checked at boot in the game's costume table
(`CostumeInventory.GetMasterDataRecord`: `fighterId`, `costumeNo`; if the table gives no answer for 30 s, in the
scene's folders `fighterNNN/esfNNNv00`); a character without it is left out of the alias (one log line).

How (since 1.15): the battle description (`BattleFlowMap.m_desc`) has one `FighterDesc` per fighter, whose `Costume`
is what the network holds; the game also keeps a local override per fighter (`SetOverwriteDesc` / `GetCostume`) and
builds the fighter's visual from it. When the game asks for the files of a character's carrier and the player
controls that character in it with a slot as the intent, the slot's files are asked for too, the same way (online
the game asks for them as managed files, which it releases itself), and the player's `FighterDesc` gets the slot and
its colour as its override at once (the VS screen builds its models from the description before the fighters are
built); just before the fighters are built it is checked again and kept only if the slot's files were asked for.
Each fighter is built from its own outfit: an opponent in the same character's Outfit 1 (a mirror) stays in Outfit 1.
The slot's files are unloaded with the last of the carrier's the game holds, and the override is taken off then, or
before fighters are built without it; never while the fighter built from it is on screen.

Where: online (`IsNetwork`: the fighter of our session member), and offline in a mode that takes the costume from
Battle Settings (a fighter on a pad), never in a mode that picks costumes on a select screen (Arcade, Training,
Versus, Team Battle: there a fighter in Outfit 1 really wears it). A Custom Room's training area is Training (game
mode 2) but takes the costume from Battle Settings: it is told apart by its fighters, loaded before any of them has a
pad, and the player is P1. Battles whose fighters were loaded through a select screen ask for no files and are left
as the game built them. A replay (no player) is not aliased.

`state.json` keeps the carrier the save was given (`"carrier": 0`); a save still holding a former carrier
(DriveTech, written by 1.14 and before) with a slot as the intent is moved to the new one by the safety net, in a
colour it has. For tests, `reframework\data\SF6_CostumeSlots_data\alias_everywhere.txt` present at start makes
the modes that pick costumes on a select screen aliased too (the Training shortcut of Fighting Ground loads its
fighters the way online matches do).

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
  (and the carrier the save was given).
- `reframework/data/SF6_StageSlots_Data/registry.json` (written by the loader) and `state.json` (the chosen variants,
  watched by the loader).
- Log: `reframework/data/SF6_CostumeSlots_data/native_log.txt` (the previous one is kept as `native_log.old.txt`).

## Build

`build.bat` at the root of the repository builds it with the rest, into `build\SF6_CostumeSlotsNative.dll`.
Copy it into `reframework\plugins\` **with the game closed**. Headers: REFramework plugin API v1.5.8
(`include/reframework`), `/std:c++20 /EHa /W4`.

## Known limits

- The alias follows the game's requests for a fighter's files (`requestNotManagedFighterAssetLoad`, online
  `requestManagedFighterAssetLoad`); fighters built without such a request (a battle loaded through a select screen)
  are left as the game built them (log: `the slot's files were not asked for before the fighters were built`).

## Runtime costume options (`rto*.cpp`, off unless the loader writes `costume_options.json`)

With `RuntimeOptions=on` (the default since Slots Loader 1.13.1) or `variants` in `costume_mods\settings.ini`, the loader stores every option of a modular
mod and lists them in `SF6_Costumes_Data\costume_options.json`. The plugin then shows, on the costume row of Battle
Settings > character, the key that opens the outfit's options (F / A), applies the choices to the preview at once and
to each fighter when its model is built (an outfit mounted for a fight, `PlayerBehavior.SetupAppearance`: a 10 s
watch, nothing during the fight), per player side, and saves them in `SF6_CostumeSlots_data\costume_choices.json`.
Without the manifest none of it runs. Design, rules and limits: `docs/RUNTIME_OPTIONS.md`. Offline test of the data
side: `tests\build_rto_test.bat [costume_options.json]`.
