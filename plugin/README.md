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
| UP / DOWN on the stage select agent (`app.UIAgent.InputUp` / `InputDown`) | next / previous look of the focused entry, `state.json` for the loader; with tiles in the list the game's own UP / DOWN is skipped (it would turn the list's pages) |
| the stage select's list built (`app.menu.UIFlowStageSelect.Param.InitStageList`, post) | the tiles' entries after the game's stages, `InitList` again with the longer count (the cursor's place in the view: the entry last fought on screen), the two arrows copied from the music selector's |
| Random on that screen (its `Param.IsRandom`, seen by the frame) | the game's draw (its first cells only) replaced in the same frame: an entry among all of them, one of its looks, the cursor moved with `UIPartsScrollList.SetSelectedIndex` as the game does |
| VS screen activated or rewritten (`app.esports.VSInfoOffline`) | the variant's name and image |
| Opponent Outfit Settings' summary drawn (`app.UIFlowOtherMatchingFighterSetting.UIParam.UpdateCosColorTextList`, pre and post) | at once in the hook: the outfit list shortened for the draw, put back after it (below) |

Without stage mods, the stage hooks are not installed; without costume slots, the costume ones are not. A
fight triggers none of these events: during a fight the plugin reads nothing but its own memory.

## The online alias

A slot chosen in Battle Settings is not a costume the other players have: the save data (and the network) keep one
of the character's own outfits, the carrier, and the plugin dresses the player's own fighter in the slot on the
player's screen only. The carrier is the player's choice, **Outfit 1** (costume 0, the default since Slots Loader
1.15) or **DriveTech** (costume 4; Outfit 1 for a character without it, Arjun): Options > Interface > Other Settings >
"Outfit Others See" (core 0.9.0+), or `"online_outfit": "Outfit 1"` / `"DriveTech"` in
`SF6_CostumeSlots_data\settings.json`. A change applies without a restart: outside Battle Settings the carriers are
decided again and the saves that hold the former one for a slot are moved to the new one (from the next battle; a
fighter on screen keeps what it was built from). The outfits each character has, and both carriers' colours, are read
at boot in the game's costume table (`CostumeInventory.GetMasterDataRecord`: `fighterId`, `costumeNo`; if the table
gives no answer for 30 s, the scene's folders `fighterNNN/esfNNNv00` / `v04`); a character with neither is left out
of the alias (one log line).

When `reframework\data\SF6_TitleSlots_Data\registry.json` (written by the loader at game start) lists title packs, Interface > Other Settings gets **Skip intro screens** (`"skip_intro"`), and Options > Interface gets a section **Title Screen Selection** in the left list, just above "Other Settings" (it needs Native Core 0.9.9; without it there is no title page and everything else works). Confirm on the section gives its page the input. The page is laid out like Battle Settings > Other > Stage Settings: a banner with the background's name, a 3-column grid of the backgrounds (the game's own title first, each pack, then Random with the game's globe picture) and a scroll bar. A chooses (yellow corner marker on the chosen one; `"title_pack"` in `settings.json`), Y opens **Random Intro Settings** (tick which backgrounds Random draws from, at least one stays ticked; `"title_random_pool"`; Y there ticks all / unticks all but the focused one), X opens the title editor of the focused background without choosing it, B goes back. The choice changes the title at the next return to the title screen, no relaunch needed.

The title editor is full screen: the background's picture with the "OVERDRIVE EDITION" line and PRESS ANY BUTTON as the title screen shows them. X shows / hides the line, the directions move it, L3 / R3 centre it horizontally / vertically, LT / RT change its size, LB / RB go to the previous / next background, Y applies this background's setting to every background after the game's own Yes / No box. Place and size are shown in % of the screen. Each background keeps its own setting in `reframework\data\SF6_TitleSlots_Data\overdrive.json`, and the real title screen follows it. A background never set shows no line, except the game's own title, which shows it by default. Random's draw (without repeats) is kept in `shuffle.json` in the same folder. The pictures are each pack's modinfo screenshot, and `game_title.jpg` (same folder, shipped) for the game's own title; Random's editor picture is a live mosaic of the ticked backgrounds. The pack names come from `registry.json` (written by the loader at game start); a name too long for its place slides in it, never cut. `"title_pack"`, `"skip_intro"` and `"title_random_pool"` are in `settings.json`.

When `reframework\data\SF6_UISlots_Data\registry.json` (written by the loader at game start, from the interface mods dropped in `reframework\ui_mods`) lists packs, Options > Interface gets one section per pack in its left list, just above "Other Settings", named as the pack (its description line: its author). Its rows are the game's own rows (Native Core 0.9.10, `opt_section_place_rows`; an older core: no section, the choices still apply at launch): a group of options of which one at a time can be used is one row, **Game's own** (the game's files) then its options, cycled with Left / Right; any other option is one row Off / On; a heading that holds other rows is a header row when the core has them, else its name goes in front of its rows'. The game draws them, moves the focus and shows each row's description (the mod's own, its literal `\n` as spaces). A long title or value keeps its size and slides in its place, as on every row of the Options screen. Under the rows, when there is room in the right panel, the picture of the focused row's value (an Off / On row: its option's; Game's own: none), else of the pack. Every change is written to `"ui_choices"` in `settings.json` (`{ "<pack key>": [ "<option id>", ... ] }`, the other keys kept) and handed to the loader at once (`SF6UISlots_Apply` in `amd_ags_x64.dll`), which serves the new files from then on; the textures the game keeps in memory are swapped on screen (`ui_retexture.cpp`). A loader without that export applies the choice at the next launch.

How (since 1.15): the battle description (`BattleFlowMap.m_desc`) has one `FighterDesc` per fighter, whose `Costume`
is what the network holds; the game also keeps a local override per fighter (`SetOverwriteDesc` / `GetCostume`) and
builds the fighter's visual from it. When the game asks for the files of a character's carrier and the player
controls that character in it with a slot as the intent, the slot's files are asked for too, the same way (online
the game asks for them as managed files, which it releases itself), and the player's `FighterDesc` gets the slot and
its colour as its override at once (the VS screen builds its models from the description before the fighters are
built); just before the fighters are built it is checked again and kept only if the slot's files were asked for
for that battle (online: in that match's requests, or the last match's for a rematch, which asks for none; a reload's
description is used only by the flow map that set it up).
Each fighter is built from its own outfit: an opponent in the same character's Outfit 1 (a mirror) stays in Outfit 1.
The slot's files are unloaded with the last of the carrier's the game holds, and the override is taken off then, or
before fighters are built without it; never while the fighter built from it is on screen.

Online the network may hold another outfit than the carrier for the player's fighter (1.15.0: a character changed in
Battle Settings inside a Custom Room sent the slot's number in the fighter profile, the room gave Outfit 1): the slot
is worn over whatever outfit the network holds. While Battle Settings is open the save holds the slots (so the menu
shows them); around the fighter profile the game builds, sends and checks (`app.TemporarilyDataManager`
`CreatePostFighterProfileBody`, `UploadData`, `InvalidCheck`) the save holds the carriers, the slots are put back
after.

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

## Random outfit

The loader gives every character one more outfit, Random (`registry.json` possession `"random": true`, the last
number of the character's range: a copy of Outfit 1's scene, so its preview is Outfit 1; named with the game's own
word, `CommonMessage756`, in each language). It has one colour, which its Color row shows as Random too
(`ColorInventory.GetColorList` post-hook, code-checked, R18). It is never worn: a draw takes its place
(`src/random_logic.hpp`, unit test `tests/build_random_test.bat`).

- The pool of a character: what its Costume row lists (`GetCostumeNoList`, read offline at the screens that list
  outfits), Random left out, never an NSFW outfit while those are hidden (checked again at each draw). A draw takes
  one the round has not given; once all were given a new round starts (never with the last one). The colour: the
  same, among the drawn outfit's colours the player has (a game outfit's colour records that are default or in the
  save; ours 0-9). "Display NSFW Outfits" changed: a new round.
- Picked on a select screen (Arcade, Training, Versus...): when the game asks for the fighters' files, each
  FighterDesc in Random gets its draw (`Costume`, `Color`), and the request in flight of Random's number asks for the
  drawn outfit's files (its argument; its unload too). A new draw at each load whose description holds Random.
- Battle Settings (online, rooms, Battle Hub): the intent is Random and its next draw is made at once. A game outfit
  drawn is what the save holds outside the menu and around the fighter profile (the opponent sees it); one of ours
  leaves the carrier there and the alias wears it. A match's first managed load of the player's fighter takes the
  draw for the set (rematches build the fighters again without asking for files: the same outfit) and makes the
  next; a game outfit drawn that the match does not hold (no profile sent since the draw) is worn on this PC only,
  through the alias, as ours are.
- `state.json` `"random"`: each character's rounds and next draw, so they go on after a restart.

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
  (and the carrier the save was given); `"random"`: the Random outfit's rounds and next draws.
- `reframework/data/SF6_StageSlots_Data/registry.json` (written by the loader) and `state.json` (the chosen variants,
  watched by the loader).
- Log: `reframework/data/SF6_CostumeSlots_data/native_log.txt` (the previous one is kept as `native_log.old.txt`).

## Build

`build.bat` at the root of the repository builds it with the rest, into `build\SF6_CostumeSlotsNative.dll`.
Copy it into `reframework\plugins\` **with the game closed**. Headers: REFramework plugin API v1.5.8
(`include/reframework`), `/std:c++20 /EHa /W4`.

## Opponent Outfit Settings with extra outfits

Battle Settings > Other > Opponent Outfit Settings > a fighter: under the colour grid, the page sums up each outfit
("Outfit 1: 1", the colours turned on). The game draws that summary (`UpdateCosColorTextList`) with one label per entry
of the page's outfit list, into the summary's widgets: 6 of them, as many as a fighter of the game has outfits. A
fighter with more outfits (our slots: Cammy with 15 slots has 20) made it write the 6 widgets, then throw: the page never
showed, the screen stayed empty and Esc did nothing. While the summary is drawn, and only for such a fighter, the list's
length is the number of widgets; the full length is put back as soon as the method returns (the method only reads the
list). Every outfit stays in the list for the rest of the page (outfit, colours, save). Idea from ZZtaii's Slotsloader fork
(MIT, `SlotsloaderOpponentOutfitSummary.lua`), done here without Lua.

The game's list shows 5 lines (the sixth widget is written and stays hidden). The lines past the fifth outfit are ours: copies of
its text under the list's parent, in its style and step, made in the guide list's update (R19), found by name each frame (R35), only
on this page. Each shows the outfit's name (`CostumeInventory.GetName`) and its count (`ValidColorIdList`, plus the Color Mod's RW / MC ticks of that outfit), with what goes round them
taken from a line the game wrote. A name too long for the place slides while the count stays. Up to 19 rows, so 24 lines; with more,
the rows follow the outfit shown on the Costume row, and a row at an end says how many are past it.

- One hook (pre and post), installed once a check over every method of the game found no other method entering its code
  (R18): on a thread of its own at the first start of a game version, then read from
  `SF6_CostumeSlots_data\codecheck.txt`. With slots only.
- The two writes (the length, the length back) go through the core's guarded field write; a draw that threw before its end
  is put back by the next draw or two frames later.
- Log: `[F9] Opponent Outfit Settings: 20 outfits, 6 summary labels: the summary shows the first 6 ...` (when the fighter
  or its counts change), `[F9] Opponent Outfit Settings: 20 outfits, 5 shown by the game, 15 lines of ours`, and the hook's line
  (`installed`, or `REFUSED (R18)`).

## Replay Outfits (`replay_outfits.cpp`, rules in `replay_outfits.hpp`)

What each character wears in a replay, never in a live battle. **Default**: what the players really wore. The replay holds
the carrier for the player's own fighter (online alias), so at the end of each online match one line goes to
`SF6_CostumeSlots_data\replay_journal.jsonl` (numbers only: the replay's key, the player's side, both sides' outfit and
colour as the replay holds them, the slot really worn, by its mod); a replay found there shows the player's side in that
slot, the opponent as recorded; a replay not found there shows what it recorded. **Forced**
(`SF6_CostumeSlots_data\replay_outfits.json`, written by the Replay Outfits tab, read again when it changed): the chosen
outfit and colour on every side that plays that character, the player's own included. An extra outfit is named by its
`mod_id` (registry.json): a mod that is gone leaves the character on Default.

- Music: the Replay Music row of the tab (`"bgm"` in the same file, `replay_bgm.hpp`), the game's values with Self as P1 and
  Opponent as P2, then Custom. A replay does not follow the online setting (the save's `MatchingSelectBgm`): its `BattleRule.BgmType` stays
  3, Random, from `BattleRule.reset` (read 06/11 in a replay with the online setting on Character), so 1 chance in 3 each for
  the stage's music, P1's and P2's, each fighter's being its playlist or else its default; the pick is seeded by the time, not
  by the replay. The row writes `BgmType` in the same post-hook (Stage 0, Player1 1, Player2 2); Random writes nothing.
  T / pad Y (input id 295) starts `app.UIFlowCharaBgmSetting` as Battle Settings does (`replay_music.cpp`).
- Custom (`"list"`: `BgmUiDataUserDataRecord.ManageId`s, the tracks not owned left out; `replay_music.cpp`). The dialog is
  `app.UIFlowCharaBgmSelect` run alone: `InitMenu`'s post-hook replaces its marks by the list, Back's result is read from it, and
  nothing reaches the save, as only its parent screen writes. In a replay: `BgmType` 1, and while `BattleFlowMap.loadBgmPrefab(1, _)`
  runs on that replay's flow map, `SystemSaveManager.GetCharaBgmIdSettingList`'s post-hook replaces the content of the new list it
  returns (refused if it were the save's own, compared by address). The game picks one track at random from it. An empty list:
  nothing written, as Random. Three hooks, each after the R18 check, cache `SF6_CostumeSlots_data\codecheck_music.txt`.
- The key: FNV-1a of the replay's `ReplayInfo` (match seed, stage, fighters, rounds' seeds without the trailing zero ones:
  the replay built at the end of a match has one round entry per round played, the server's and the local copies are padded
  to 5). Outfits and colours are not in the key. Seen equal, 05/11: the end of a match and its replay in My Replays.
- Journal: post-hook on `app.BattleReplayDataManager.CreateSaveData` (the match's replay is built), online only; the side
  is the team of `SelfSessionMemberindex`, the slot is the one the alias dressed that `FighterDesc` in.
- Replay: post-hook on `app.battle.ReplayFlowMap.setupDesc` (`FGReplayFlowMap` inherits it): the description is filled
  from the replay there, and the game builds the fighters from it later (`setupFighterAsset`) without asking for files
  through the requests the alias follows (a local replay of slots picked on a select screen loads them that way). Each
  side's `Costume` / `Color` is written there; the replay's own data is not. A replay has no player (pads -2): the alias
  leaves it alone. Opponent Outfit Settings are not applied in replays (seen 05/11).
- Log: `replay outfits: replay <key>: P1 F1 c0/1 -> c7/3 (worn (journal)), P2 F1 c2/6 (recorded)`.

## Known limits

- The alias follows the game's requests for a fighter's files (`requestNotManagedFighterAssetLoad`, online
  `requestManagedFighterAssetLoad`); fighters built without such a request (a battle loaded through a select screen)
  are left as the game built them (log: `the slot's files were not asked for before the fighters were built`).

## Runtime costume options (`rto*.cpp`, run when the loader wrote `costume_options.json`)

Runtime options are always on (the `RuntimeOptions=` setting is gone from `costume_mods\settings.ini`; a leftover line is
ignored). The loader stores every option of a modular
mod and lists them in `SF6_Costumes_Data\costume_options.json`. The plugin then shows, on the costume row of Battle
Settings > character, the key that opens the outfit's options (F / A), applies the choices to the preview at once and
to each fighter when its model is built (an outfit mounted for a fight, `PlayerBehavior.SetupAppearance`: a 10 s
watch, nothing during the fight), per player side, and saves them in `SF6_CostumeSlots_data\costume_choices.json`.
Without the manifest none of it runs. Design, rules and limits: `docs/RUNTIME_OPTIONS.md`. Offline test of the data
side: `tests\build_rto_test.bat [costume_options.json]`.
