# Slotsloader Manager contract

ZZtaii's Slotsloader Manager installs costume mods for the Slots Loader and reads what
the loader wrote at the last launch. This page says what our loader (`amd_ags_x64.dll`,
`costumes/loader/loader_core.cpp`) provides to the Manager, under which **contract number**, and what the Manager
needs to change to accept our loader. The variant code below comes from ZZtaii's Slotsloader fork (1.10.0, MIT);
the Replace-original record follows the Manager's own `replace-state.json` (ZZtaii).

## Contract number

| Where | Field | Value |
|---|---|---|
| `modinfo.ini` of `SF6_SlotsLoader_v*.zip` (the component that ships `reframework/plugins/SF6_CostumeSlotsNative.dll`) | `SlotsloaderContract=` | `2` |
| `modinfo.ini` of `SF6_ModSuite_AllInOne_v*.zip` (it ships the same plugin; `make_bundle.py` copies the Slots Loader's value) | `SlotsloaderContract=` | `2` |
| `reframework/data/SF6_Costumes_Data/variants.json` (written by the loader) | `"contract"` | `2` |

All three come from `MANAGER_CONTRACT` in `loader_core.cpp` (the packaging scripts read it there). The number goes up
only when the loader changes something the Manager relies on; a loader with contract `N` keeps every promise of
contract `N - 1` unless this page says otherwise.

### Contract 1 promises

1. **`variants.json`** in `SF6_Costumes_Data`, written whole (temporary file, then renamed over) at every launch
   that rebuilds the costume pak, and on the launches that find no mods:
   `{ "generated", "loader", "contract": 1, "runtime_options": "off"|"variants"|"on", "archives": [ { "source",
   "fighter_dir", "variants": [ { "label", "enabled", "slot" } ] } ] }`. `source` is the mod's path below
   `reframework/costume_mods` with `/`, as in `outfits.json`; every variant is listed, on and off, in the loader's
   order; `slot` is `"Outfit V"` for a built variant, else `null`. Only mods put in `costume_mods` are listed
   (not Fluffy paks, not mods read from Fluffy's own archives, not the game's loose files).
2. **`variant_choices.json`** (written by the Manager) is read at every launch:
   `{ "version": 1, "archives": { "<source>": { "<label>": false } } }`. Only `false` turns a variant off;
   sources compare case-insensitively with `\` and `/` alike, labels exactly. A variant turned off gets no outfit
   and no registry record (its number is free again); its files stay out of the pak unless a variant that stays on
   uses them. The variants turned off are part of the rebuild fingerprint (changing them rebuilds the pak at the
   next launch; with every variant on the fingerprint is the one of a loader without choices). A malformed file
   turns nothing off and logs one WARN; the loader never writes it.
3. **New outfits are numbered after the highest number in use** for their fighter (a later install is listed
   last instead of filling a gap a removed mod left); gaps are filled only once the last reserved number is taken.
   An outfit already in `registry.json` keeps its number (saved choices, colours, replays name outfits by number).
   **What the game shows** (loader `2026-11-07-outfit-labels`): the text of an extra outfit in the game's own lists is its mod's
   label (`outfits.json`'s `label`, without control characters and `<` `>`), through one name message per slot in the pak's
   `fightercostumemessage.msg.21`; "Outfit V" when it has none. Only that text changes: `slot` here, `outfits.json`'s `name`,
   `registry.json` and the numbers stay as they are, and a slot still travels as its number.
4. Unchanged files the Manager already reads: `outfits.json` (`source`, `name`, `label`, `left_out`, `pak`) and
   `registry.json`'s `"fingerprint"` (emptied by the Manager to force a rebuild).
5. **`outfits.json`'s `"loose_files_kept": true`** (added after Slots Loader 1.16.3; the key, place and meaning of
   ZZtaii's Slotsloader fork 1.10.9: top level, right after `"loader"`). Written by every build: the costume pak
   never covers a loose game file with an outfit's copy (a mod-only file an extra outfit shares at a path outside its
   slot, which a Replace-original install also put in the game's `natives` folder, is left to that file), and a
   replacement installed, changed or removed at a path the pak depends on rebuilds it at the next launch (the
   fingerprint walks `natives/stm/product/model/esf`; the other paths are listed in `loose_probes.tsv` beside the
   registry). A launch that rebuilds nothing leaves `outfits.json` as the last build wrote it, so a pak an older
   loader built is never reported this way. A missing key means `false`; no contract change.

### Contract 2 promises

Everything of contract 1, and:

6. **Manager mode** (`common/manager_mode.hpp`): when the Slotsloader Manager installed this loader, the loader and the
   plugin behave as the Manager's own Slotsloader fork (1.10.9) does. The loader knows it from
   `reframework/data/SF6_Costumes_Data/manager_installs.json` (written by the Manager since 1.0.9): an enabled mod of
   that list holds `reframework/plugins/SF6_CostumeSlotsNative.dll` at the size the plugin has on disk. Installed any
   other way (by hand, by Fluffy Mod Manager, or over the Manager's install), the plugin is not listed at its size and
   nothing of this applies. In Manager mode:
   - outfits come from `reframework/costume_mods` only: no Fluffy Mod Manager discovery, no loose `natives` files and no
     other patch paks as outfits (they are still read as the game's files). `reframework/costume_mods_nsfw` is neither
     read nor made, as under the fork, which had no such folder; mods someone put there give no outfit, and the log says
     how many. So no outfit is NSFW in Manager mode, and the plugin never adds the "Display NSFW Outfits" row;
   - one outfit per variant of a modular mod (`variants.json`'s `"runtime_options"` is `"off"`), each switched by
     `variant_choices.json`; no in-game costume options (a `costume_options.json` a build in normal mode left is removed:
     it names the same pak, so the plugin would use it);
   - more than three add-ons of one outfit, each a complete outfit (its own body mesh) naming it in `AddonFor` and all
     replacing the same files, are one variant each ("main + add-on"), however many there are (fork 1.10.5, MIT; Ingrid
     C1 Barefoot Stockings v2: the main and five outfits); other add-on sets keep the limit of three ("too many to
     combine": the main alone);
   - a costume put in one sub-folder with add-ons that replace no same file (each its own archive or folder) gives one
     outfit per add-on and one with all of them, up to three add-ons, each its row in `variants.json`; "only the complete
     outfit", and "handled together", belong to the runtime options;
   - no NSFW detection by keywords;
   - a modular mod with one menu level ("01 Main files" and its options) keeps the fork's variant labels, made from its path
     below `costume_mods` (`Ryu/Sakura as Ryu C1.zip/01 Main files - Sakura as Ryu C1 - Combined hair`, not `Sakura as Ryu C1 -
     Combined hair`): `variant_choices.json` names a variant by its label, and one turned off under the fork stays off;
   - the game checks for no update of the suite (`updater/README.md`, The Slotsloader Manager's installs): nothing is fetched
     from inside the game unless `state.json` says `"check_in_manager_mode": true`;
   - character-swap repairs (ported from fork 1.10.4, MIT, `costumes/loader/swap_repairs.cpp`) while an empty
     `swap_repairs.txt` is in `SF6_Costumes_Data`. The Manager has no switch for it since 1.0.15: it writes the file at
     its start, at every refresh and when it installs the loader, as soon as that folder exists. Its coming or going
     rebuilds the pak;
   - plugin: slots made from Outfit 1 or DriveTech are always shown, and the owned-outfit check is off while the
     Manager's opt-out marker is in `reframework/data/SF6_CostumeSlots_data` (`plugin/src/manager_marker.hpp`).
   A change of mode rebuilds the costume pak once.

   - the costume pak is built again after a game update that the base pak's size and date do not show (ported from fork
     1.9.5, MIT): the fingerprint also holds a digest of the game's executable (size, date, first 4 KiB), of
     `re_chunk_000.pak`'s index (its path-hash fields left out: a mod manager turns rows off and on there), of
     `vanilla_costume_index.tsv`, `static_meta.json` and the static files it names, and each DLC pak's date beside its size;
   - each of the plugin's additions to the game's menus can be left out, by a key of its own set to `false` in
     `reframework/data/SF6_CostumeSlots_data/settings.json` (read at start; a key that is not there means shown):
     - `menu_outfit_names`: the mods' names in the costume lists, and their scrolling. The loader reads the same key and keeps
       each slot's "Outfit V" name in place of its mod's label (a change rebuilds the costume pak once);
     - `menu_fighter_settings_name`: the outfit's name on Fighter Settings;
     - `menu_online_outfit_row`: Options > Interface > Other Settings, "Outfit Others See";
     - `menu_title_screen`: the "Title Screen Selection" section and the "Skip intro screens" row;
     - `menu_interface_packs`: the "Interface Packs" section and its editor;
     - `menu_replay_settings`: the Replays menu's settings tab.

     `"menu_additions": false` leaves out every one of them whose own key is not in the file. The "Display NSFW Outfits"
     row of that place has no key: it is never added in Manager mode. The stage select is not
     concerned; `online_outfit` is still read from that file (as without the Native Core), `title_pack`
     and `skip_intro` by the loader at the next launch, so the Manager can set them from outside the game. Not checked in the
     game yet: the interface packs' choices (`ui_choices`) with the section left out.

   - nothing of the game is written back at its own path (fork 1.9.4, MIT): no original is "restored" into the costume pak,
     and neither a mod-only dependency nor the texture repair writes at a path of the game's own index, even when a
     replacement turned that entry off in the base pak. The pak holds the slots' own files, the mod-only files at paths the
     game does not have, and the tables the loader builds. A replacement installed later is never covered by it;
   - a loose game file the build depended on is watched by content as well as by size and date (fork 1.10.9's digest, kept in
     `loose_probes.tsv`), the files below `model\esf` and the loose colour files a slot took included: a replacement put back
     with the size and date it had still rebuilds the pak;
   - a mod's own scene that is another layout of the outfit (another fighter's body, another number of model parts) is used
     whole even when it names a file found nowhere, as the fork takes it (1.10.3): Swimsuit Sakura as Ryu C2 names
     `sakura_001_01_havok.user`, which it does not ship, and was sent back to the game's scene, on Ryu's skeleton. The
     game-version check of such a scene is unchanged;
   - Report a bug (`report_mod.dll`, when it is installed) stays off: `"report_a_bug": true` in the same `settings.json` turns
     it on (read at start).

   Outside Manager mode none of the above applies: the fingerprint, the menus, the updater and Report a bug are what they were.

   The fork's other loader fixes of 1.10.6 - 1.10.9 (a scene or colour file read from its turned-off entry, an outfit's
   own colour list, donor colour files following the slot, promise 5) and its Opponent Outfit Settings summary (done
   in the plugin, no Lua script; with or without SF6 Native Core) apply in every mode, Manager mode included.

What the Manager must keep doing for Manager mode: write `manager_installs.json` (with the loader's own files and
sizes), `swap_repairs.txt` and its opt-out marker where it does now.

### What the Manager checks in the install (Manager 1.0.17, the download of 2026-11-06, source 0f2c7b1)

| The Manager | This loader |
|---|---|
| accepts a package whose plugin component's `modinfo.ini` has `SlotsloaderContract=` ≥ 2 (`KnownContract`) | `2` in the Slots Loader and all-in-one packages |
| skips a program at the package's top when the game folder does not have it, and refuses the package when it does (`SkippedProgram`, `ValidateDestination`) | the package ships `SF6_SlotsClean.exe`: Install stays off while a copy is in the game folder (one put there by hand; Fluffy Mod Manager removes its own) |
| counts the loader as installed and rebuilding (`Rebuilds`, `ToolStatus`): `amd_ags_x64.dll` over 200 KB, `amd_ags_x64_real.dll`, the plugin, `SF6_Costumes_Data\loader` | the package ships all four |
| recognises the costume pak by `natives/stm/sf6_costume_slots.marker` without the stage marker; one entry = placeholder | the stage slots have a pak of their own; placeholders as described in contract 1 |
| refuses to replace what the pak writes for itself: `esf.scn.20`, `fightercostumeuserdata.user.2`, `fightercostumemessage.msg.21`, `version_update/ver_NN_NNNN/esf.scn.20` (`IsSlotTable`), `charparam/esf/esfNNN/*.scn.20`, the model folders of `registry.json`'s `slots` (`fighter_dir`, `new_folder`) | exactly these; anything else in the pak is an outfit's copy of a shared path, left to a loose file (promise 5) |
| trusts the pak to leave loose files alone (`KeepsLooseFiles`): the installed version ≥ 1.10.9, else `outfits.json`'s `"loose_files_kept": true` | every Slots Loader from 1.16.4 does, and writes the key; the Manager refuses the older ones (contract 1) anyway |
| reads `outfits.json`, `variants.json`, `registry.json`, the run lines of `SF6_CostumeLoader.log` (history) | unchanged formats |

### Not in contract 2

- `variants.json` is not rewritten on a launch that rebuilds nothing (same as the fork).

## Replace original installs

With **Replace original**, the Manager writes a mod's files loose under the game's `natives/` (the PAK method
also turns off the matching rows of the base pak) and installs a mod's own `.pak` as the next
`re_chunk_000.pak.patch_NNN.pak`. Such a mod replaces the game's outfit: it is not an extra one. The Manager's
installs have priority, so the loader makes **no outfit** from them:

- a loose game file an enabled Manager mod installed (and still the same size on disk) is left out of the
  "natives" source, the way files Fluffy Mod Manager installed are;
- a patch pak an enabled Manager mod installed (same file name, and same size, else same SHA-256, when the
  record gives one: patch numbers get reused) is not a source of outfits; the game still loads it and the loader
  still reads it as a game pak;
- disabled mods, files missing from the game, files another tool wrote over since, and files the Manager released
  ("Keep current file", `Released[]`) claim nothing;
- the same mod also put in `reframework/costume_mods` stays an extra outfit (the user put it there on purpose).

The log says where the record came from, how many mods / files / paks it claims, and for each source left out
"`<mod>`: installed by the Slotsloader Manager (Replace original): no outfit". The record file is part of the
rebuild fingerprint: an install, uninstall or enable change in the Manager rebuilds the pak at the next launch.

### What the loader reads

1. **`reframework/data/SF6_Costumes_Data/manager_installs.json`** (requested from the Manager, format below). When
   it is there and readable it is the record, and nothing else is looked for. An unknown `format` or a file that
   does not parse logs one WARN and the loader falls back to 2.
2. Until the Manager writes it: the Manager's own **`Data/replace-state.json`** (`"Format": "sf6-replace-state-v1"`,
   ZZtaii's format, read as the Manager writes it), when its `GameRoot` is this game folder. The Manager is portable
   and leaves no pointer, so the folder is found the way the loader finds Fluffy: `SlotsloaderManager=` in
   `reframework/costume_mods/settings.ini`, else the folder found before (`manager_location.txt` in
   `SF6_Costumes_Data`), else a running Manager, else a search of Documents, Desktop, Downloads, the game folder and
   the one above it and each drive root (and their sub-folders named like a tool). From it the loader takes
   `Mods[]` (`Name`, `Enabled`, `Files[]` `Path` / `Sha256` / `Size`) and `Released[]`; `Paks[]` (the base pak
   stamps) is not used.

Neither file is ever written by the loader. Both are read only when the game folder has loose costume files or a
foreign patch pak (the only things a Replace-original install leaves).

### `manager_installs.json` (asked of the Manager)

Write it whole after every install, uninstall, enable / disable and priority change of a Replace-original mod
(temporary file in the same folder, then renamed over it), UTF-8 without BOM. Delete it, or write an empty `mods`,
when no Replace-original mod is installed.

```json
{
  "format": "sf6-manager-installs-1",
  "generated": "2026-10-20T21:00:00Z",
  "manager": "Slotsloader Manager 1.0.8",
  "mods": [
    {
      "id": "4f1c...",
      "name": "Gym Cammy",
      "installed_utc": "2026-10-20T20:58:12Z",
      "enabled": true,
      "files": [
        { "path": "natives/stm/product/model/esf/esf017/001/01/esf017_001_01.mesh.230110883",
          "sha256": "9b2e...", "size": 1843216 }
      ],
      "paks": [
        { "path": "re_chunk_000.pak.patch_004.pak", "sha256": "77aa...", "size": 3386324 }
      ]
    }
  ]
}
```

- `path`: relative to the game folder, `/` or `\`, any case (the same paths as `replace-state.json`'s
  `Files[].Path`). `files` holds the loose files, `paks` the patch paks the mod added (a patch pak listed under
  `files` counts the same).
- `size` (bytes) is what the loader checks; `sha256` is used for a pak only when `size` is missing. Both optional.
- `enabled: false`: the mod claims nothing. Mods in priority order (first = the one whose file is in the game).
- `generated`, `manager`, `id`, `installed_utc` are for logs and support; the loader does not need them.
- A later, incompatible layout gets a new `format` string; fields may be added to this one freely.

## RuntimeOptions and what the Manager sees

**Change (after Slots Loader 1.15.1): the `RuntimeOptions=` setting is removed; the loader always behaves as `on`.** A
`RuntimeOptions=` line in `settings.ini` is ignored (the loader logs it once), so a Manager must not write it and cannot
select `off` or `variants`; `variants.json`'s `"runtime_options"` is always `"on"`. The table below describes the
layouts as they were when the setting existed; only the `on` row applies now. Nothing else of the contract changes.

`reframework/costume_mods/settings.ini`, `RuntimeOptions` (`docs/RUNTIME_OPTIONS.md`):

| Layout | Outfits | Variants listed | A variant turned off |
|---|---|---|---|
| `off` (the default of 1.13.0) | one per variant (as the fork) | one row per outfit, label = `outfits.json`'s `label` | its outfit is not built |
| `variants` | as `off`, options stored too | as `off` | as `off` |
| `on` (the default since 1.13.1) | one per modular mod / outfit with add-ons | the outfits `off` would make, with `off`'s labels; every row left on shows the one outfit as its slot | the one outfit wears by default the first row left on (same outfit number); every row off: no outfit |

With `on`, the Manager's choice only sets the outfit's **default options** (`costume_options.json`, `defaults`,
the files built into the pak). What the player picked in the in-game options menu (`costume_choices.json`) wins
over the default, as it always does. Because the labels are the same in the three layouts, choices made under one
layout carry over to the others. A mod made of a single outfit is one row (the Manager shows no toggle for it).
With `on`, a retouch (meshes and textures only, no material file: `RUNTIME_OPTIONS.md`, Retouches) makes no outfit:
it is an option of the outfits that wear its files and has no row in `variants.json`; a subset (a game outfit with
parts taken off: `RUNTIME_OPTIONS.md`, Merges) too. Outfits made one (`RUNTIME_OPTIONS.md`, Merges: versions of one
archive, outfits of several archives with the same meshes) keep their rows, each under its own archive's `source` and
with `off`'s label; every row left on shows the one outfit as its slot. The one outfit wears by default the first of
them (archive order) whose row is left on; every row off: no outfit.

Mods in `reframework/costume_mods_nsfw` (NSFW outfits, shown in game only while "Display NSFW Outfits" is on:
`RUNTIME_OPTIONS.md`, NSFW outfits) have no row in `variants.json` and are not turned off by `variant_choices.json`:
the Manager names sources below `costume_mods` only. Without that folder nothing changes.

## What the Manager needs to change

Accept this loader's package instead of refusing it as upstream (`LoaderCostumePak.IsUpstreamBuild`, used by
`ReplacePlanner.Prepare`) when its plugin component's `modinfo.ini` says `SlotsloaderContract=` 2 or more:

```csharp
// The loader says which Manager contract it implements (modinfo.ini SlotsloaderContract=N).
internal const int KnownContract = 2;
internal static bool HonoursContract(ModComponent c)
{
    string v; int n;
    return c != null && c.Metadata != null && c.Metadata.TryGetValue("SlotsloaderContract", out v)
        && Int32.TryParse((v ?? "").Trim(), out n) && n >= KnownContract;
}
// ReplacePlanner.Prepare: refuse only when !HonoursContract(plugin.Component) && IsUpstreamBuild(...)
```

Then, to give every Manager user this loader, the Manager's "Install Slotsloader" fetches this loader's release zip
(its pin) instead of the fork's.

At run time, `variants.json`'s `"contract"` tells which contract the installed loader honours (a missing field = a
loader before the contract). `VariantChoices.ReadReport` can keep ignoring it.

Second, for Replace original: write `reframework/data/SF6_Costumes_Data/manager_installs.json` as above. Until then
the loader reads `Data/replace-state.json` wherever it can find the Manager; a Manager folder it cannot find
(unusual place, no `SlotsloaderManager=` in `settings.ini`) means its Replace-original installs show up twice again.
