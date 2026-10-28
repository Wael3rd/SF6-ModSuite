# Drill pack format

A drill ("pack") is `reframework/data/SF6_RecordingSlotManager_data/<Char>/<file>.json`: what Save / Save As write,
what Open imports, what the Community stores and serves as it is. Code: `src/model/pack.*` (entries, validation, split),
`src/model/setup.*` (the `active` flags and the training block), `src/model/files.cpp` (export, import, backup).

## The array of 30 entries

A JSON array, one object per slot, ids 1 to 30 (1-8 the game's slots, 9-30 CRSM's), written as CRSM writes every file
(sorted keys, 4-space indent, CRLF, an empty table as `null`):

| Key | Meaning |
|---|---|
| `id` | the slot, 1-30 |
| `empty` | `true`: no recording |
| `timeline` | the recording, run-length lines (`"12f : 2 + MK"`); `inputs` (one value per frame) is read too |
| `weight` | the slot's weight in a random replay |
| `name` | the slot's name (absent or `""` = none) |
| `active` | **since 2.2**: the slot is checked for the normal replay (the game's IsActive for 1-8, `_virtual_slots.json`'s flag for 9-30); always `false` on an empty slot |
| `training` | **since 2.2, entry 1 only**: the training block below |

Every reader of packs (CRSM 2.1.x, the Lua versions, the Community server) requires each element of the array to be a
slot entry with a numeric id 1..30 and either `empty` or a timeline / inputs, and ignores unknown keys. A new top-level
element (an object without an id, or a non-array root) would make them reject the file. So the new data is carried as
keys of the entries: the per-slot flag on every entry, the setup of the dummy on entry 1, which is always a live slot
(1-8 go to the game, never into `_virtual_slots.json`), so the block never leaks into another file. Entry 1 is found by
its id, not by its place.

## The training block (entry 1, key `training`)

```json
"training": {
    "version": 1,
    "playback": { "InfoType": 2, "LoopType": 1, "PlayType": 0 },
    "reversal": {
        "down":   [ { "Count": 1, "Delay": 0, "Frame": 0, "IsActive": true, "IsFrame": false,
                      "IsValid": true, "Level": 0, "SkillIndex": 0, "Type": 4 }, ... ],
        "guard":  [ ... ],
        "damage": [ ... ]
    },
    "dummy": {
        "status":  { "DummyActionType": 1, "JumpType": 0 },
        "guard":   { "GuardCount": 0, "GuardOnlyType": 0, "GuardType": 2, "GuardWeight": 50, "IsGuardSwitching": true },
        "defense": { "DP_Type": 0, "DR_DelayCount": 0, "DR_DelayFrame": 0, "DR_Getup_Weight": 1, "DR_Guard_Weight": 1,
                     "DR_No_Weight": 1, "DR_Type": 0, "GD_Type": 1, "GD_Weight": 50, "QS_Type": 1, "QS_Weight": 50 },
        "counter": { "NC_TYPE": 0, "NC_Weight": 1, "NH_Weight": 1, "PC_TYPE": 0, "PC_Weight": 1 }
    }
}
```

- `version`: a reader applies the block only when it is its own version (1). Compatible additions keep 1 (a reader
  ignores keys it does not know); an incompatible change takes 2, and a 2.2 reader then leaves the block alone.
- `playback`: the replay settings of `TM_RecordSetting` (`TrainingManager._tData.RecordSetting`), the game's own enum
  values: `PlayType`, `LoopType`, `InfoType`. Each may be absent (not written). `RecordType` (which sub-page,
  Record / Replay, the menu shows), `RecordStartType` and `RecordCountDown` (how the user records) are the user's own:
  never carried.
- `reversal`: the dummy's reversal rows, `down` / `guard` / `damage` = `DownReversalDatas` / `GuardReversalDatas` /
  `DamageReversalDatas` of `TM_ReversalSetting.FighterData` (CRSM's categories 0 / 1 / 2). Each is an array: element
  *i* = row *i* (0-based), the nine `ReversalData` fields under the game's names (`Type`: -1 none, 0 normal, 1 command
  normal, 2 special, 3 SA, 4 recording, 5 common, 6 other). 10 elements (the game's rows) or 30 (+ CRSM's rows 11-30,
  `_reversal_rows.json`); a missing element is an empty row (Type -1, IsValid false, Count 1, SkillIndex -1, the rest
  0 / false), elements past 30 are ignored.
- A recording row (`Type` 4) says its slot as `SkillIndex` = slot - 1, **0-29**: a row that plays virtual slot 15 holds
  14. In the game such a row is stored as "recording, slot 8" (SkillIndex 7) plus CRSM's map
  (`_reversal_virtual.json`, `[category][row] = 15`); the export and the import translate between the two.
- A row the game cannot play is written as an empty row at import: `Type` outside -1..6, a recording row whose slot is
  outside 1-30 (the game indexes its slot array with it). A move row's `SkillIndex` (the dummy's move list) is taken as
  it is: the pack is in the character's own folder.

- `dummy` (**added in 2.2, version still 1**: older 2.2 readers ignore it): the dummy's settings, the pages of the
  game's Dummy Settings tab, each field under the game's name in the `DummyData` of `TrainingManager._tData.<object>`:
  `status` = `DummyStatus` (`DummyActionType` 0 stand, 1 crouch, 2 jump, 3 short replay, 4 human, 5 CPU; `JumpType`
  0 vertical, 1 front, 2 back, 3 random; `JumpWeight_Front` / `_Virtical` / `_Back`), `guard` = `GuardSetting`
  (`GuardType` 0 no guard, 1 gap guard, 2 next guard (after the first hit), 3 all guard, 4 random, 5 count;
  `GuardWeight`; `GuardCount`; `IsGuardSwitching`; `GuardOnlyType` 0 none, 1 stand, 2 crouch, 3 random), `defense` =
  `DefenseSystem` (`QS_Type` recovery 0 stay, 1 back, 2 random; `GD_Type` throw escape 0 off, 1 on, 2 random; `DP_Type`
  drive parry 0 off, 1 parry, 2 perfect parry, 3 random; `DR_Type` drive reversal 0 off, 1 on guard, 2 on get-up,
  3 random; `DR_DelayFrame`, `DR_DelayCount`; the weights `QS_Weight`, `GD_Weight`, `DR_No/Guard/Getup_Weight`),
  `counter` = `CounterSetting` (`NC_TYPE` counter, `PC_TYPE` punish counter: 0 off, 1 on, 2 random; `NH_Weight`,
  `NC_Weight`, `PC_Weight`). The enums are the game's (`app.training.TM_DS_DUMMYTYPE`, `TM_GS_GUARD_TYPE`,
  `TM_GS_ONLY_TYPE`, `TM_DS_QS_TYPE`, `TM_COMMON_TYPE`, `DRIVE_REVERSAL_TYPE`, `TM_DP_TYPE`). Every field may be absent
  (the game keeps the user's); a reader does not take a value outside its range (the enum; 0-100 for the percentages
  `GuardWeight`, `QS_Weight`, `GD_Weight`; 0-9999 for the counts and the delays), and takes the weights of one random
  choice (the three jump weights, the three DR weights, the three counter weights) together or not at all, never all
  zero. `IsGuardSwitching` is a JSON boolean. Not carried: `CpuLevel`, the counter's `StyleNo` (ranges unknown), the
  gauges (`ParameterSetting`) and the start position (`SelectMenu.StartLocation`).

## The drill's metadata (entry 1, key `info`)

```json
"info": { "author": "Chris_F (drillcodes)", "description": "Pick Ken and throw him. He will auto break it. Then, STOP THE SLOP", "source": "community" }
```

- Carried like the training block: a key of the entry whose id is 1, ignored by every older reader.
- `description`: the drill's text for the player (what to do), free text, may hold line breaks. `""` or absent
  = none. `author`: who made it, as the source names them. `source`: where the pack came from: `"community"` (a
  Community download), `"drillcodes.com"` (a drill code converted by CRSM). Other keys may be added; a writer keeps
  the keys it does not know.
- Written by a Community download (`community::with_info`): the description of the download answer (`/api/download/:id`,
  else the list item's), the author shown in the list (`author_cfn`, else `uploader_name`), source `"community"`. An
  `info` already in the stored file keeps its other keys, and its description when the server sends none.
- Read by the screens: the Community list shows the focused drill's description in the dialog's description area,
  above the "Like / Sort" line (2 lines at most, cut with "…"); the Manager page's Drill row shows the selected
  drill's description as its help line, its paragraphs on one line joined with " | " (cut with "…"), in place of the
  row's own help. Share sends the shared pack's description to the Worker (cut to 1000 characters).
- Save / Save As (the export, `src/model/files.cpp`) build the pack from the game's slots, and carry the `info` of the
  loaded pack (the last one opened, or saved, in this session for this dummy), read from its file just before the
  write: Save over the loaded file keeps it, Save As (and Rename) copy it to the new file, Save over another selected
  file gives that file the loaded pack's `info` (the recordings are the loaded pack's). No pack loaded, or the loaded
  file without `info`: no `info`, as before.

## Export (Save, Save As)

- `active` on all 30 entries.
- The block on entry 1: `playback` always (when RecordSetting is reachable); `reversal` only when one row of the three
  categories is valid (a user who does not use reversals does not hand his empty setup to every importer), with 10 rows
  per category when CRSM's rows 11-30 are all empty, 30 otherwise. The reversal part needs the reversal layer native
  (it owns the map and rows 11-30); a Lua reversal layer gives a block without it. `dummy` always (every field the
  game gives, as it is: the dummy's settings are part of how the author plays the drill).
- `info` on entry 1: the loaded pack's (see above).

## Import (Open, reopen, a Community download)

1. Slots 1-30 are written as before (recordings, weights, names).
2. `active`: a live slot takes the pack's flag (an empty slot is never active); with `activate_on_load` on, the pack's
   flags still win where the pack has them. Slots 9-30 take the pack's flag instead of keeping the user's own. An entry
   without `active` (any pack written before 2.2): exactly as before (live slots inactive unless `activate_on_load`,
   9-30 keep the user's flags).
3. The block, when entry 1 carries a version-1 block: its parts replace the game's. `playback`: the fields it holds.
   `reversal`: all three categories, all 30 rows (rows the pack does not hold become empty rows, CRSM's 11-30 included:
   the block is the whole reversal setup, so a stray row of the user's cannot fire inside someone's drill), and the map
   rewritten for every row. Direct field writes into the game's own `ReversalData` objects (no game setter: they need
   the reversal page's category and their hooks would map the rows again), inside the import call (the pause menu open,
   the moment the game's own reversal page writes these objects). `dummy`: the fields it holds, each a plain store into
   the game's `DummyData` objects (the menu's own data, what the Dummy Settings pages edit; never `TempPlayerData`, no
   Apply call), same moment; the training functions take them when the menu closes. A pack without the block changes
   nothing there.
4. `config.json` (`SF6_RecordingSlotManager_data/config.json`) `"import_training_setup": false` skips step 3 (absent =
   true). The flags of step 2 are slot data and always apply.

The pre-import backup (`<Char>/Backups/<Char>_pre_import_<date>.json`, written before step 1 as before) now carries the
replaced `active` flags of slots 1-8 and the replaced block (playback, all reversal rows, even unused ones, and every
dummy setting). To undo an import's reversal / replay / dummy settings, copy that file out of `Backups` into the character's folder and open it.

With the ALLOC fallback (a slot without memory), steps 2-3 run once, at the re-run that writes the slots: the block rides
on entry 1, which stays with the live entries the re-run keeps.

## Compatibility

- Older CRSM (2.1.x) and the Lua versions open a 2.2 pack as before: they ignore `active` on 1-8 and the block, and
  drop `active` on 9-30 (they keep the user's flags).
- 2.2 opens an older pack as the older version did (no flag, no block: nothing else changes).
- Community: the stored file is the pack as written, with the block; the array keeps its 30 entries.

## Sources outside CRSM

drillcodes.com codes (`SF6DRILL:v2:` base64 JSON) map onto this format: `slots[].a` = `active`;
`action_record.reversal_slots.{down,guard,damage}.overrides[]` = rows, `i` = the 1-based row of the game's 10, the other
keys the nine fields (`c` Count, `d` Delay, `f` Frame, `a` IsActive, `g` IsFrame, `v` IsValid, `l` Level, `k` SkillIndex,
`t` Type), rows not listed = empty rows; `action_record.global_settings` `p` / `l` / `i` = PlayType / LoopType / InfoType
(`t`, `s`, `c` = RecordType, RecordStartType, RecordCountDown: not carried). `training_settings` (the drill mod's
copy of the dummy pages, raw game values) = `dummy`: `ds` DummyActionType, `dg` GuardType, `bc` GuardCount, `sw`
IsGuardSwitching, `dr` DR_Type, `dl` DR_DelayFrame, `dc` DR_DelayCount, `te` GD_Type, `dv` QS_Type, `nc` NC_TYPE, `pc`
PC_TYPE. Not mapped until checked in the game: `bt` (block_type: GuardOnlyType or DP_Type), `cn` (dummy_counter), `dw`
(dummy_guard_switch, never seen in a code), the gauges `pg` / `gg` / `ps` / `gs` and the position `sp` ("Center").
The converters (`convert_drillcodes.py`, the Community's `drillcodes.js`) also write `info` on entry 1:
`{"author": <the post's author>, "description": <the post's description>, "source": "drillcodes.com"}`, each text only
when not empty.
