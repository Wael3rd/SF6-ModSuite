# Slots Loader 1.13.0: what changed, for tool authors

This page lists everything the SF6 Slots Loader 1.13.0 (SF6 Mod Suite 1.17.0) adds or changes compared with 1.12.0
(Mod Suite 1.16.3). It is written for authors of tools that install mods for the loader or read its files, first of
all **ZZtaii**, who maintains the Slotsloader Manager and the Slotsloader fork; several changes below are ports of his
fork's fixes (MIT) and of his Manager's NSFW filter (with his permission).

The Slots Loader is two binaries:

| Binary | Game path | Role |
|---|---|---|
| loader | `amd_ags_x64.dll` (game root); `costume_loader.exe` is the same code as a test build | builds the costume pak at launch, writes the files of `reframework/data/SF6_Costumes_Data/` |
| plugin | `reframework/plugins/SF6_CostumeSlotsNative.dll` | in game: slots in the menus, ownership, online alias, runtime options, NSFW display |

Paths below are relative to the game folder. Unless stated otherwise, every JSON file is UTF-8 without BOM.

Related pages: [RUNTIME_OPTIONS.md](RUNTIME_OPTIONS.md) (the design and every rule in detail),
[MANAGER_CONTRACT.md](MANAGER_CONTRACT.md) (the Manager contract), [FLUFFY.md](FLUFFY.md).

## Contents

1. [Summary](#1-summary)
2. [Identifying the loader](#2-identifying-the-loader)
3. [Folders](#3-folders)
4. [Settings](#4-settings)
5. [Files the loader writes](#5-files-the-loader-writes)
6. [Files the loader reads](#6-files-the-loader-reads)
7. [Files the plugin writes and reads](#7-files-the-plugin-writes-and-reads)
8. [When the pak is rebuilt](#8-when-the-pak-is-rebuilt)
9. [How mods become outfits and options](#9-how-mods-become-outfits-and-options)
10. [NSFW](#10-nsfw)
11. [Fixes ported from the Slotsloader fork](#11-fixes-ported-from-the-slotsloader-fork)
12. [Plugin runtime behaviour](#12-plugin-runtime-behaviour)
13. [Plugin exports](#13-plugin-exports)
14. [Log lines](#14-log-lines)
15. [What a Manager must do, and must not do](#15-what-a-manager-must-do-and-must-not-do)

## 1. Summary

- **Runtime costume options** (`RuntimeOptions=on`, off by default): a modular mod, an outfit with add-ons, versions
  of one archive... become **one** outfit whose options are chosen in game, live, per player. New files:
  `costume_options.json` (loader), `costume_choices.json` (plugin). Option files are stored in the pak under
  `<slot folder>/rto/<option id>/`.
- **NSFW outfits**: a second mods folder `reframework/costume_mods_nsfw/`, NSFW detection by keywords (the Manager
  1.0.8 list and rule), `"nsfw": true` in the json files, "Display NSFW Outfits" in game.
- **Manager contract 1**: `variants.json` written, `variant_choices.json` read, Replace-original installs read from
  `manager_installs.json` (or the Manager's `replace-state.json`), new outfits numbered after the highest in use.
- **Pak builds**: incremental build (`pak_sources.tsv`), big files in a `.spill` file, the new pak written as
  `.tmp` and renamed over the old one, a separate "metadata" id so a loader update that changes only the json files
  keeps the pak.
- **Fork fixes** 1.9.7 to 1.10.4 ported (all but the character-swap repairs).
- **Plugin**: Battle Settings and character select options menus, options worn on the select / VS / fight models,
  the outfit's name on Fighter Settings, three new exports for the SF6 Color Mod.

With `RuntimeOptions=off` (the default), no `costume_mods_nsfw` content and no NSFW keyword hit, the loader builds
the same outfits as 1.12.0, under the same numbers; the pak is rebuilt once because the loader id changed.

## 2. Identifying the loader

| What | Where | 1.13.0 value |
|---|---|---|
| Pak id (in the fingerprint and in the `"loader"` field of every json file) | `LOADER_PAK_ID`, `costumes/loader/loader_core.cpp` | `2026-10-20-physics` |
| Metadata id (only in the registry fingerprint, after `;meta=`) | `LOADER_META_ID`, same file | `2026-10-20-fastpak` |
| Manager contract | `MANAGER_CONTRACT`, same file; `SlotsloaderContract=` in the package's `modinfo.ini`; `"contract"` in `variants.json` | `1` |
| Package | `modinfo.ini` of `SF6_SlotsLoader_v1.13.0.zip` | `name=SF6 Slots Loader`, `version=1.13.0`, `author=Wael` |

1.12.0 had a single `LOADER_BUILD_ID`; 1.13.0 splits it (see [section 8](#8-when-the-pak-is-rebuilt)). Treat the
ids as opaque strings: compare them for equality only.

## 3. Folders

| Folder | Made by | Content |
|---|---|---|
| `reframework/costume_mods/<Character>/` | the loader (with a folder per fighter) and the package | as in 1.12.0 |
| `reframework/costume_mods_nsfw/<Character>/` | **new**: the loader (with a folder per fighter) and the package (with a `README.txt`) | NSFW mods, same layout and rules as `costume_mods` |
| `reframework/costume_mods/.cache/` | the loader | unpacked archives of **both** roots |
| `reframework/costume_mods/settings.ini` | the loader (template) or the user | settings of **both** roots ([section 4](#4-settings)) |

Layout of each root, as before: `<root>/<Character>/<archive | folder mod | .pak>`, or
`<root>/<Character>/<sub-folder>/<one or more of those>`. Archives are `.zip`, `.7z`, `.rar`.

**Sub-folder rules** (both roots):

- Several mods in one sub-folder of a character folder are grouped: an outfit and its add-ons put together make one
  outfit (1.12.0 behaviour). With `RuntimeOptions=on` the add-ons become options of that outfit, worn by default, and
  the outfit is named after the outfit alone (no more "+ add-on" in the name).
- **New**: a mod **alone** in its own sub-folder (`<root>/<Character>/<Sub-folder>/<that mod>`, the sub-folder being
  neither an archive nor a pak) is always its own outfit with `RuntimeOptions=on`: never a retouch, a subset or a
  version of another outfit, never merged, never given another mod's add-ons or retouches, never placed as an add-on
  on another outfit. Its own internal options stay options of its outfit. A lone pure add-on (no complete outfit)
  cannot be an outfit: it keeps the add-on rules (logged). Users can use this to keep a mod out of every merge.
- A sub-folder of `costume_mods` and one of `costume_mods_nsfw` with the same name are two sub-folders: they never
  group with each other.

## 4. Settings

### `reframework/costume_mods/settings.ini` (read by the loader)

INI lines `Key=value`; keys case-insensitive; lines starting with `;` ignored; surrounding quotes stripped; the first
line of a key wins. The loader writes a commented template listing every key when the file is missing, **but only on
a launch that looks for Fluffy** (the game folder holds a foreign patch pak or loose files); it never edits an
existing file, so an older file gets no new keys. A Manager may add or change a key line and must keep the others.

| Key | Values (default first) | Effect | In the fingerprint |
|---|---|---|---|
| `FluffyModManager` | empty / a folder | Fluffy's folder (as 1.12.0) | no (Fluffy's `installed.ini` and archives are) |
| `SlotsloaderManager` | **new**; empty / a folder | the Manager's folder (the one holding its `Data` folder); empty: searched ([section 6](#6-files-the-loader-reads)) | no (the record it finds is) |
| `RuntimeOptions` | **new**; `off` / `on` (or `1`) / `variants`; anything else = `off` | `off`: outfits as 1.12.0, no options file (`costume_options.json` is deleted). `on`: one outfit per modular mod / outfit with add-ons / merged versions, options stored. `variants`: the outfits of `off` plus the options stored | `;options=sf6-costume-options-1:one` or `:variants` (nothing for `off`) |
| `NsfwKeywords` | **new**; key absent = the built-in list; a comma list replaces it; present and empty = no detection | NSFW detection by names ([section 10](#10-nsfw)) | `;nsfwkw=<hash>` only when the key is present |
| `IncrementalPak` | **new**; `on` / `off` (or `0`) | `off`: every file is read from its source at each build | no (the pak is the same either way) |

### `reframework/data/SF6_CostumeSlots_data/settings.json` (read by the plugin)

**New** file. The plugin creates it (with the three keys below) the first time it finds an NSFW outfit or option, or
when the in-game "Display NSFW Outfits" row is used; it rewrites it keeping every key it knows.

| Key | Default | Effect |
|---|---|---|
| `display_nsfw` | `false` | `true` shows NSFW outfits and NSFW options. Re-read when the file changes, applied at the next screen that lists outfits (no relaunch). Also set by Options > Interface > Other Settings > "Display NSFW Outfits" (needs SF6 Native Core 0.9.0) |
| `select_options_button` | `"R1"` | pad button of the character select options menu: `R1`, `L1`, `R2`, `L2`, `R3`, `L3`, `off` |
| `select_options_key` | `"C"` | keyboard key of that menu: a letter or digit, `auto` (the game's keyboard key of its UI tab action, E on AZERTY), `off` |

## 5. Files the loader writes

All in `reframework/data/SF6_Costumes_Data/` unless stated otherwise.

### `registry.json` (changed)

Same layout as 1.12.0 (`version`, `fingerprint`, `slots[]`, `next_ids`, `possession[]`), plus:

- `"nsfw": true` on a `slots[]` entry and on a `possession[]` entry of an NSFW outfit (written only when true).
- `"fingerprint"` now ends with `;meta=<LOADER_META_ID>`. Emptying it still forces a rebuild at the next launch
  (contract 1). Read it as an opaque string.

### `outfits.json` (changed)

As 1.12.0 (`generated`, `loader`, `pak`, `outfits[]` with `fighter`, `fighter_dir`, `costume_no`, `name`,
`based_on`, `label`, `kind`, `source`; `left_out[]` with `fighter`, `label`, `reason`), plus:

- `"nsfw": true` on an NSFW outfit (only when true).
- `"moved"` (top level, only when something moved): outfits numbered again because they became NSFW or stopped being
  NSFW. The plugin moves a Battle Settings choice of `from` to `to` at start.

```json
"moved": [ {"fighter": 28, "from": 7, "to": 18, "label": "Mini Dress Mai C1 Swap"} ]
```

With `RuntimeOptions=on`, `label` / `name` of an outfit made of versions or of an outfit with add-ons is the outfit's
own name ("Aloha Ryu", "Ingrid - Fly In A Bucket - Drive Tech"), not the combination's.

### `costume_options.json` (new)

The options manifest, written with the pak when `RuntimeOptions` is `on` or `variants` (deleted with `off`; a missing
file with `on` / `variants` forces a rebuild). Read by the plugin; a Manager may read it but never writes it. Resource
paths are as the game names resources: lowercase, without `natives/stm/` and without the version suffix.

```json
{
  "format": "sf6-costume-options-1",
  "generated": "2026-10-20 21:44", "loader": "2026-10-20-physics", "pak": "re_chunk_000.pak.patch_001.pak",
  "layout": "one",
  "mods": [
    {"key": "esf028/mini dress mai c1 swap", "name": "mini dress Mai C1 Swap",
     "source": "Mai/Mini Dress Mai C1 swap v1.5", "defaults_from": "loader",
     "groups": [
       {"name": "2! Skin type options c1", "options": [
         {"id": "b104ad5a", "name": "damaged skin", "folder": "...", "mode": "pick", "apply": "live"},
         {"id": "4fe85878", "name": "see through", "folder": "...", "mode": "pick", "apply": "live", "nsfw": true}]}],
     "variants": [ {"label": "mini dress Mai C1 Swap - Regular - Barefoot", "options": ["0e0a734f", "b104ad5a"]} ]}],
  "slots": [
    {"fighter": 28, "fighter_dir": "esf028", "costume_no": 5, "folder": "006", "name": "Outfit I",
     "label": "mini dress Mai C1 Swap", "mod": "esf028/mini dress mai c1 swap",
     "defaults": ["0e0a734f", "b104ad5a"],
     "options": {"b104ad5a": [{"kind": "mesh", "target": "product/model/esf/esf028/006/01/esf028_006_01.mesh",
                               "path": "product/model/esf/esf028/006/rto/b104ad5a/01/esf028_006_01.mesh"}]},
     "base": [ {"kind": "texture", "target": "product/model/esf/esf028/mini/mini1_atos.tex",
                "path": "product/model/esf/esf028/006/rto/base/_/mini/mini1_atos.tex"} ],
     "required": [1],
     "former": [ {"costume_no": 6, "folder": "007", "label": "mini dress Mai C1 Swap - Regular - Barefoot",
                  "options": ["0e0a734f", "b104ad5a"]} ]}]
}
```

- `layout`: `one` (`RuntimeOptions=on`) or `variants`.
- `mods[]`: one per mod. `key` = `<fighter folder>/<root name, lowercase>`, or `<fighter folder>/nsfw/<name>` for a
  mod of `costume_mods_nsfw` (a mod NSFW by keywords keeps the plain key); `nsfw: true` on an NSFW mod. `groups[]`
  in layering order (the mod's main files first; a later group's file replaces an earlier one's at the same path).
- An option: `id` = 8 hex digits (a hash of the group key and the option's folder: stable when the archive is updated
  or renamed, changed when the option's folder is renamed; the plugin then matches saved choices by name). `mode`:
  `base` (always worn), `pick` (one of the group), `toggle` (On / Off). `apply`: `live` (swapped on the model),
  `colour` (colour files only), `relaunch` (a file that only takes effect at the next launch: physics, cloth).
  `nsfw: true`: an NSFW option.
- `variants[]`: the combinations `RuntimeOptions=off` makes as outfits (presets; used to migrate former outfits).
- `slots[]`: every outfit with options, ours, then **game outfits** given options (`"game": true`, the game's costume
  number and folder, `mod` = `<fighter folder>/game outfit N` style key, empty `defaults` / `base` / `required` /
  `former`). `nsfw: true` on an NSFW outfit. `defaults`: the options worn until the player changes something.
  `options`: per option id, the files it swaps for that outfit (`kind`: `mesh`, `material`, `texture`, `chain`,
  `colour`, `other`; `target` = the file the model loads; `path` = the stored copy). `base`: the copies that give the
  outfit back without an option. Optional per file: `unit` (part unit: files swapped together), `joints`,
  `own_joints`. `required`: indexes in the mod's `groups` of pick groups offered without None. `former`: outfits of
  the previous launch that are now this outfit with options chosen.
- The plugin ignores a manifest of another `format`, or one whose `pak` is not the costume pak named in
  `outfits.json`. When the loader only moves the pak (another pak number), it updates `pak` in both files.

### Option files in the pak (new)

```
natives/stm/product/model/esf/<fighter>/<slot folder>/rto/<option id>/<path in the slot folder>
natives/stm/product/model/esf/<fighter>/<slot folder>/rto/<option id>/_/<path in the fighter folder>
natives/stm/product/model/esf/<fighter>/<slot folder>/rto/base/...           (base copies)
```

The same under a game outfit's folder (`esf028/002/rto/<id>/...`) for game outfits' options. Nothing the game loads
by itself changes. A tool must not install files under any `rto/` folder (they are rebuilt with the pak).

### `variants.json` (new, contract 1)

Written whole (`variants.json.tmp`, then renamed over) on every launch that rebuilds the pak, and on the launches that
find no mods or make no outfit; not on an "Up to date" launch.

```json
{ "generated": "2026-10-20 21:44", "loader": "2026-10-20-physics", "contract": 1, "runtime_options": "on",
  "archives": [ { "source": "Mai/Mini Dress Mai C1 swap v1.5.zip", "fighter_dir": "esf028",
                  "variants": [ {"label": "mini dress Mai C1 Swap - Regular", "enabled": true, "slot": "Outfit I"},
                                {"label": "mini dress Mai C1 Swap - Skimpy", "enabled": false, "slot": null} ] } ] }
```

- `source`: the mod's path below `reframework/costume_mods`, with `/`, as `outfits.json`'s `source`. Only mods of
  `costume_mods` are listed (not `costume_mods_nsfw`, not Fluffy paks or Fluffy's archives, not loose game files). A
  mod NSFW by keywords but kept in `costume_mods` is listed.
- `variants[]`: every variant, on and off, in the loader's order; `label` = the `label` `RuntimeOptions=off` gives;
  `slot` = `"Outfit V"` for a built variant, else `null`. With `on`, every row left on shows the one outfit as its
  slot. Retouches and subsets (options of other outfits, [section 9](#9-how-mods-become-outfits-and-options)) have no
  row.

### `pak_sources.tsv` (new)

The incremental build's record of where the pak holds each file put in it unchanged. Written after each build that
writes a pak (`.tmp`, then renamed). Tab-separated; a header line (`sf6-pak-sources-1`, the pak id, the game paks'
fingerprint, the pak's name, size and date), then one line per source (a key naming the same bytes at the next build:
a disk file's path, size and date; a mod pak entry by its pak; a game pak entry by its pak and place; then offset,
stored size, attributes, decompressed size, signature). Internal: do not edit; deleting it only makes the next build
read everything. `SF6_SlotsClean.exe` removes it with the registry and `outfits.json`.

### `manager_location.txt` (new)

The Manager folder found by the search (one line), reused at the next launches. Delete it to force a new search.

### Pak files: `.tmp`, `.spill`, the atomic replace (changed)

- The new costume pak is written as `re_chunk_000.pak.patch_NNN.pak.tmp`, flushed to the disk, then the old paks are
  retired as before (deleted, or replaced by a marker-only pak when a foreign pak sits above them) and the `.tmp` is
  renamed to `re_chunk_000.pak.patch_NNN.pak` (`MoveFileEx` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`,
  retried for about 10 s if the pak is in use; then nothing more changes and the registry is not saved).
- Big files (1 MB and more) wait in `re_chunk_000.pak.patch_NNN.pak.spill` once memory use passes half of the free
  memory beyond 4 GB (8 GB at most); the file is deleted with the writer.
- At each launch the loader **deletes** any `re_chunk_000.pak.patch_*.pak.tmp` and `re_chunk_000.pak.patch_*.pak.spill`
  in the game folder (left by a build cut short).
- The pak itself, its marker, its numbering (top foreign pak + 1) and the marker-only placeholders are unchanged.

## 6. Files the loader reads

### `variant_choices.json` (new, contract 1; written by the Manager)

`reframework/data/SF6_Costumes_Data/variant_choices.json`, read at every launch:

```json
{ "version": 1, "archives": { "Mai/Mini Dress Mai C1 swap v1.5.zip": { "mini dress Mai C1 Swap - Skimpy": false } } }
```

- Only `false` turns a variant off (`true` or a missing label = on). Sources compare case-insensitively, `\` and `/`
  alike, leading / trailing `/` ignored; labels compare exactly.
- A variant turned off gets no outfit and no registry record (its number is free); its files stay out of the pak
  unless a variant that stays on uses them. With `on`, the one outfit wears by default the first row left on (same
  outfit number); every row off: no outfit.
- The variants turned off are part of the fingerprint (`;choices:<n>:<hash>`; nothing while every variant is on).
- A malformed file turns nothing off and logs one WARN. The loader never writes it.

### `manager_installs.json` (new; asked of the Manager)

`reframework/data/SF6_Costumes_Data/manager_installs.json`, format `sf6-manager-installs-1`: the Manager's
Replace-original installs. Format and rules: [MANAGER_CONTRACT.md](MANAGER_CONTRACT.md), "Replace original installs".
In short: `mods[]` with `name`, `enabled`, `files[]` and `paks[]` (`path`, `size`, `sha256`); keys read
case-insensitively; a loose file claims only if it exists with that size; a pak matches by name, then by size, else
by SHA-256. A claimed loose file or patch pak makes no outfit (the game still loads it).

### The Manager's `Data/replace-state.json` (new; fallback)

Read only when `manager_installs.json` is missing or unreadable, and only when its `GameRoot` is this game folder
(`Format: sf6-replace-state-v1`; `Mods[]` with `Name`, `Enabled`, `Files[]` `Path` / `Sha256` / `Size`, and
`Released[]`). The Manager folder is `SlotsloaderManager=` in `settings.ini`, else `manager_location.txt`, else a
running process whose file name contains "slotsloader", else a search of Documents, Desktop, Downloads, the game
folder and the one above it and each drive root.

Both records are read only when the game folder holds loose costume files or a foreign patch pak, and the record
used is part of the fingerprint (`manager/<file name>`): an install, uninstall or enable change rebuilds the pak.

### `reframework/costume_mods_nsfw/` (new)

Scanned like `costume_mods`, after it; its entries in the fingerprint as `nsfw:<path>` (none while it is empty).

## 7. Files the plugin writes and reads

| File | Access | Content |
|---|---|---|
| `reframework/data/SF6_CostumeSlots_data/costume_choices.json` | **new**, written by the plugin at each change | the player's options per mod key and side |
| `reframework/data/SF6_CostumeSlots_data/settings.json` | **new**, read; written by the plugin | [section 4](#4-settings) |
| `reframework/data/SF6_CostumeSlots_data/state.json` | changed: migrations | Battle Settings outfit / colour per fighter (as before); an outfit number in `outfits.json`'s `moved` or in a slot's `former` is moved at start |
| `reframework/data/SF6_Costumes_Data/costume_options.json`, `outfits.json`, `registry.json` | read | as above |
| `reframework/data/SF6_NativeLocale_data/locale.json` | read (optional) | two hint titles of the options menu's preview camera |

`costume_choices.json`:

```json
{ "format": "sf6-costume-choices-1",
  "former_done": "2026-10-20 21:44 re_chunk_000.pak.patch_001.pak",
  "mods": { "esf028/mini dress mai c1 swap": {
      "p1": {"changed": true, "options": ["0e0a734f", "3d8bf169"], "names": ["1) Main Files c1", "damaged skin"]},
      "p2": {"changed": false} } } }
```

`changed: false` (or no entry): the outfit's `defaults` apply, i.e. the options checked in Fluffy, the loader's
combination, or the Manager's variant choice. Once the player changes an option of that side, the list wins until
"Restore defaults". Keyed by mod key, not by outfit number: choices survive renumbering. `former_done` = the manifest
whose `former` outfits were migrated. A Manager should not write this file.

## 8. When the pak is rebuilt

The registry's fingerprint is compared with the one of this launch. It is made of `loader=<LOADER_PAK_ID>`, the
foreign and own mod paks (name, size, date), the files of `costume_mods` and of `costume_mods_nsfw` (`nsfw:` prefix;
path, size, date of each archive / pak / folder entry), loose game files, Fluffy's `installed.ini` and the archives it
names, the Manager's record (`manager/...`), the DLC paks, the base pak (`;base:size:date`), then `;options=...`,
`;choices:...` and `;nsfwkw=...` when they apply.

| Case | Result |
|---|---|
| same fingerprint, same `;meta=`, the pak at its number | "Up to date": nothing is built; only `"pak"` of `outfits.json` / `costume_options.json` is synced |
| same, the pak one number off (a stage pak came or went) | the pak is renamed, nothing else |
| same fingerprint, **another or no `;meta=`** (a loader update that changes only the files beside the pak) | everything is built again, but the pak in place is **kept** when the build makes as many entries (no rewrite of several GB) |
| anything else (a mod changed, `LOADER_PAK_ID` changed, `RuntimeOptions` switched...) | full build |

**Incremental build** (`IncrementalPak=on`, the default): a full build still runs every step (scan, slots, checks),
but the data of every source listed in `pak_sources.tsv` whose key still matches (same file, same size and date) is
copied from the previous pak instead of being read and decompressed again. It applies only when the previous pak is
the loader's single pak at the expected place and the tsv header matches exactly (format, pak id, game paks, pak
name, size, date); otherwise it silently reads everything. The pak comes out byte for byte the one a full build makes
(`costumes/tools/fastpak_test/`: six scenarios compared entry by entry and whole-file; 19-27 s against 44-53 s for a
full build on a 9 GB set).

A Manager that wants a rebuild empties `registry.json`'s `"fingerprint"` (contract 1) or changes a mod file; it never
needs to delete the pak.

## 9. How mods become outfits and options

With `RuntimeOptions=off` nothing below applies except the fork fixes ([section 11](#11-fixes-ported-from-the-slotsloader-fork)):
outfits are made as in 1.12.0 (one per combination of a modular mod, one per add-on...). With `variants` the outfits
are those of `off` and the options are stored too (rules A only). With `on`:

**One outfit per mod**

- **Modular mods** (Fluffy options and groups): one outfit, its first combination (same id, number, folder and colours
  as the first variant slot of `off`); every option stored. A group its author asks exactly one option of (`"2!
  Skin"`, a description saying "choose / select / pick one", the Manager's rule) is offered without None, unless its
  options share files with another group's. A series' "main files" option is never a choice on its own.
- **Outfits with add-ons** (Fluffy `AddonFor`, the same bundle, the same archive touching the same costume folder):
  one outfit, the add-ons as options (add-ons replacing a same file are one choice). The former add-on outfits move to
  it (`former`).
- **Outfit + add-ons in one sub-folder**: one outfit, the add-ons options worn by default (the combination `off` made,
  same id and number).
- **Separate add-ons that make no outfit** (a modular mod none of whose options has a body mesh; a lone add-on no
  outfit of its own mod completes): options of the outfits whose files they replace, the game's outfits included,
  where they fit (meshes: every joint of the outfit's mesh and every material in the part's material file; material
  files: every material of the mesh, every texture present; textures: the outfit loads one there; chains: the outfit
  has one there). Shared files every outfit of a fighter loads (Mai's wrinkle masks, Ingrid's props) go on every
  outfit. None is worn by default.

**Retouches**: a mod whose files are only costume meshes and textures (no material file, no chain, colour or other
file) that made an outfit makes none any more: it is an option of every outfit that wears the files it replaces, where
it fits (the game outfit it was made from and the mods' outfits made from it that keep the game's file there).
Retouches of one file across mods are one choice ("Body": None / Barefoot / Coatless...). One that fits no game outfit
it modifies stays an outfit. Its former outfit, chosen in Battle Settings, becomes the game outfit with the retouch
chosen.

**Merge rules** (decided from the files: bytes, the meshes' name tables and faces, the costume folders; names only
label the options):

| Rule | Layout | What |
|---|---|---|
| A. Same-file toggles | `on` and `variants` | On / Off options that replace a same file (in one group or several) become one pick group with None; ids unchanged |
| B. Versions of one archive | `on` | the complete options of one folder or archive for one fighter (Aloha Ryu C1 / C2 / C3) are one outfit with a required "Version" choice |
| C. Identical meshes | `on` | outfits of several archives whose every mesh is the same bytes for the same game outfit (Sagat's Tees) are one outfit with a choice |
| D. Subsets | `on` | an outfit whose every mesh is a game outfit's mesh with some materials taken off (left out, or their triangles folded), the joints in the same order, is an option of that game outfit (Ryu Outfit 1 No Cloak) |

- B and C: the **base** is the first version (archive order; the first one the Manager leaves on; the first one that is
  not NSFW): it keeps its outfit number and colours, the others are moved onto its folder and their former outfits
  move to it with their version picked. A version made for another game outfit carries what that outfit wears at the
  parts it does not ship. A version is merged only where every file fits the base.
- **Physics content check** (B, C and D): physics files are not swapped live (`_chain.user`, `_jcs.user`, `.jcns`,
  `_havok.user`, `.havokcloth`, `_aogeo.user`, `_shape.user`, model chains, the wind chain). So every physics file of
  a subset must be byte-identical to one the game outfit loads, and every physics file of a version to one the base
  loads (the files its scene names and the files its physics `.user` files name, one level), compared by content
  whatever the names; else the mod stays its own outfit. A subset that ships a changed model chain stays an outfit.
- Not merged: Fluffy installs, modular mods, retouches, outfits put with their add-ons in a sub-folder, a mod alone in
  its sub-folder.
- Each build logs a **merge audit**: per fighter, what was made one, and outfits that look related but were not, with
  the reason.

**Other effects**

- Materials an option brings that the outfit has not get colour entries appended to the outfit's colour files
  (`cmd_NNN`), enabled, with the colour the game shows; existing entries are never changed or moved.
- Part units: a part whose mesh, material file and chain change together is stored and swapped as one unit (its
  chain checked against both meshes' joints), so an add-on like a hair with its chain can be turned off live.
- Options of a part the outfit takes from another folder (the game's hair in `000/02/`) target what the outfit's scene
  loads there.
- **Numbering**: a new outfit takes the number after the highest one in use for its fighter (gaps are filled only once
  the last reserved number is taken); outfits in the registry keep their numbers. NSFW outfits come after the others
  ([section 10](#10-nsfw)). The visible name ("Outfit IV") follows the rank, the number stays.

## 10. NSFW

- **Folder**: mods of `costume_mods_nsfw` are NSFW. Both roots are read as **one folder** for every rule (add-ons,
  retouches, merges, the audit); only sub-folders stay apart. An outfit whose base is an NSFW mod's is NSFW; an option
  an NSFW mod gives an outfit that is not NSFW is an NSFW option; an outfit that wears an NSFW option by default is
  NSFW.
- **Keywords** (mods left in `costume_mods`, and Fluffy installs): a port of the Slotsloader Manager 1.0.8 filter
  (`LibraryFilters.NsfwKeywords`, `LibrarySearch.HasCompact`), `common/nsfw_keywords.hpp`. Default list: `nsfw, nude,
  naked, topless, bottomless, pubic, pubes, nipple, pasties, lewd, futa, see through, see thru, no panties, no
  underwear, erotic`. A keyword matches when its letters and digits appear as one run in the text's letters and
  digits (case, accents, spaces, dashes and punctuation dropped); a keyword of one or two characters must start a word.
  - Mod level (archive / folder / pak name, the folders around all its options, its Fluffy root menu name, every name
    of a one-option mod): the mod is handled as if it were in `costume_mods_nsfw`, but keeps its `variants.json` row
    and its options' manifest key.
  - Option level (a `modinfo.ini` name, folders below the mod's, Fluffy menu groups up to the root): an NSFW option.
    With `on`, the outfit wears by default the first combination without one. With `off` / `variants`, an outfit made
    with one is NSFW.
- **Numbers**: a fighter's first NSFW outfit goes 10 numbers after the highest in use; a new non-NSFW outfit goes
  before the NSFW ones when there is room. An outfit whose NSFW state changed is numbered again once (`outfits.json`
  `moved`).
- **Display**: the plugin hides NSFW outfits (from the game's outfit lists too, even with an unlock-all script) and
  NSFW options while `display_nsfw` is false; a Battle Settings choice of a hidden outfit goes back to Outfit 1, a
  saved choice of a hidden option is kept.
- **Markers**: `"nsfw": true` in `outfits.json`, `registry.json` (`slots[]`, `possession[]`) and
  `costume_options.json` (`mods[]`, `slots[]`, options). Written only when true: without NSFW content every file is
  as before.

## 11. Fixes ported from the Slotsloader fork

| Fork version | In 1.13.0 |
|---|---|
| 1.9.7 | archives and folder mods whose paths pass MAX_PATH: options, `modinfo.ini` and the cache fingerprint read with the wide API and the long-path prefix |
| 1.9.7-custom | nested Fluffy menus ("03 Outfit" > "Lower Neckline" > option): the menu folders (`modinfo.ini` only) are read, every option under one top-level group is one choice (group key = that group's name), the main files laid first, groups whose name or description says "optional" left out of the outfits, outfits named after the mod; a modular mod may make 50 combinations (24 before) |
| 1.9.8 | an option whose `AddonFor` names another complete option of its mod is made with it; joint constraints (`.jcns`) a mod ships are used by its outfit (its own `_jcs.user` copy, the scene following it); a physics file nothing of the outfit names gets a WARN |
| 1.9.9 | a slot check reads only the textures a material binds (leftover `.tex` strings no longer count as missing); add-ons whose `AddonFor` names nothing of their mod, beside a single complete option, are its add-ons (WARN); a mod's own scene is used whole when it is another layout of the outfit |
| 1.10.0 | `variants.json` / `variant_choices.json`; new outfits numbered after the highest in use |
| 1.10.1, 1.10.3 | a game file whose pak entry a "PAK method" install turned off is read from its loose copy; a mod's own scene used when the game's cannot be read or the mod puts the outfit on another body (only a scene made for this game version: every type signature the game's) |
| 1.10.4 | the cache folder rename retried for about 2 s (antivirus locks) |
| 1.10.4 | **not ported**: character-swap repairs; `swap_repairs.txt` is ignored |

## 12. Plugin runtime behaviour

Only with a `costume_options.json` (`RuntimeOptions` `on` or `variants`); otherwise the runtime-options code stays
idle. The NSFW display and the outfit name on Fighter Settings work with any layout.

- **Battle Settings** (Fighter Settings): while the Costume row of an outfit with options is focused, the guide bar
  shows "F Costume options"; `F`, the pad's A / Cross or a left click on the row opens the menu (the core's option
  service): a Player side row, a choice row per pick group, an On / Off row per toggle, Restore defaults; pages of 7
  rows turned with the game's tab keys, L1 / R1 or a click beside the title. Each change is saved to
  `costume_choices.json` and shown on the preview. Options that change nothing on the model (colour or physics files
  only) are not offered.
- **Character select**: R1 (pad) or C (keyboard), configurable ([section 4](#4-settings)), in a side's panel shows that
  side's outfit's options in the panel's four rows (Left / Right change, Up / Down move; B / Esc or Decide go back).
  Shares the panel with the SF6 Color Mod's Drive colour mode (one owner at a time, [section 13](#13-plugin-exports)).
- **Models**: the Battle Settings preview, each side's select model, the two VS screen models and the fighters wear
  the chosen options. A model's outfit is found from the paths its meshes load (ours first, then a game outfit), so the
  online alias needs nothing more. Every file is loaded ahead and given to a model only once ready; a part that is not
  drawable goes back at once. The shared costume manifest is never written. No read during a fight after the model is
  built.
- **Sides**: offline, each side its own options; online, the player's own fighter wears P1's options (the opponent
  sees nothing new). The character select menu online works only while one panel is active.
- **Migrations at start**: a Battle Settings choice of a former outfit (a variant, a retouch's or a merged version's
  outfit) moves to the outfit that replaced it, with its options chosen; a choice of an outfit in `moved` follows it.
- **Fighter Settings**: the outfit's name (manifest label, else `outfits.json` label, else the game's) at the right end
  of the bottom guide bar.

## 13. Plugin exports

Exported from `SF6_CostumeSlotsNative.dll` (`extern "C"`, found with `GetModuleHandleW` + `GetProcAddress`). They copy
values the plugin keeps under a lock: no game call, any thread, never throw. They exist for the SF6 Color Mod (two
writers of one game UI text crashed the game in testing, so one mod writes); a Manager has no use for them.

| Export | Meaning |
|---|---|
| `int SF6Slots_GuideEntry(char* buf, int cap)` | the Fighter Settings guide bar entry (`<ICON KeyF><ICON MouseL> Costume options`, pad `<ICON BtnD> Costume options`, UTF-8) while it belongs in the bar, else 0 and an empty string. Each call also tells the plugin a guide owner is there: while calls come, the plugin never writes the bar. Color Mod 0.22.4+ calls it every 3 frames and appends the entry |
| `int SF6Slots_SelectPanelClaim(int side, int who, int op)` | one owner per character select panel. `side` 1 / 2; `who` 1 = the plugin's options, 2 = the Color Mod's Drive colour mode; `op` 1 = take (1 granted, 0 held by the other), 0 = give back, -1 = the owner (0, 1, 2). Color Mod 0.22.5+ |
| `int SF6Slots_SelectGuideEntry(int side, int pad, char* buf, int cap)` | the select guide corner entry ("<ICON BtnR1> Costume options" with `pad` != 0, else the keyboard key's glyph) while that side's outfit has options and its panel is free, else 0 |

The plugin calls the Color Mod's `int SF6ColorMod_DiPanelActive(int side)`; a Color Mod without it (before 0.22.5)
keeps the character select options off (logged).

## 14. Log lines

`SF6_CostumeLoader.log` (game folder), stable prefixes:

- `runtime options: on (one outfit per modular mod)` / `runtime options: variants (options stored too)`;
  `runtime options: <option> does not fit this outfit (<file>: why): not offered here`;
  `runtime options: no base copy of ... (why)`; `runtime options: part <folder> as one unit (...)`;
  `runtime options: <option> ships joint constraints (...)`; `runtime options: <slot>: colour files: +N entries ...`.
- `merge audit (RuntimeOptions=on): outfits that look related`, then `    <fighter>: ...` lines (including
  `subset: <mod> stays an outfit (... its physics differ (<file>))`).
- `<mod>: alone in its sub-folder <name>: its own outfit (no merge)`.
- `costume_options.json: options of N slots, N game outfits`.
- `NSFW keywords (NsfwKeywords): <list>`; `nsfw: <mod> (<where> "<text>": "<keyword>")...`;
  `nsfw: <mod> / <option> (...)`; `nsfw: <mod>: outfit number A -> B (...)`;
  `NSFW by keywords: N mods, M options; K NSFW outfits of costume_mods`.
- `Slotsloader Manager: ...`; `N mods installed with Replace original (...): N game files and N patch paks make no
  outfit`; `<mod>: installed by the Slotsloader Manager (Replace original): no outfit`;
  `WARN: variant_choices.json cannot be read (...)`; `variant choices: N variants off in variant_choices.json`.
- Build: `  [  12.3 s] <step> (peak working set N MB, peak commit N MB)` per step (`game files read`, `mods scanned`,
  `slots assigned`, `files assembled`, `slots checked`, `colour swatches made`, `new pak written`, `pak in place`);
  `files unchanged since the last build taken from <pak>`; `N entries taken from the previous pak, not read from
  their sources (X MB)`; `IncrementalPak=off: every file read from its source`;
  `big files kept in memory up to X GB (Y GB free), then in <pak>.spill`;
  `same mods, same pak (<id>), files beside it from another loader version: they are written again`;
  `kept <pak> (N entries, the pak this build makes)`; `removed <pak> (left by a build cut short)`;
  `ERROR: could not write <pak>.tmp (disk full or access denied), costumes not updated.`;
  `Up to date (fingerprint match, <pak> exists) in N ms`.

`reframework/data/SF6_CostumeSlots_data/native_log.txt` (plugin): `rto:` (manifest, choices, models, VS screen),
`rto select:` (character select menu, keys, warnings when the game's own guide shows the configured button),
`fs name:` (outfit name), `nsfw option:`, `costumes: Display NSFW Outfits on|off (...)`.

## 15. What a Manager must do, and must not do

**Do**

- Accept the package by its contract: `SlotsloaderContract=1` (or more) in the plugin component's `modinfo.ini`
  ([MANAGER_CONTRACT.md](MANAGER_CONTRACT.md), "What the Manager needs to change"); at run time,
  `variants.json`'s `"contract"`.
- Write `manager_installs.json` after every Replace-original change (whole, temporary file then rename, UTF-8 without
  BOM), so those installs make no extra outfit.
- Read `variants.json` for the variants and their outfits; write `variant_choices.json` with `false` for the variants
  turned off, naming sources exactly as `variants.json` does.
- Read `outfits.json` / `registry.json` with the new optional fields (`nsfw`, `moved`) and ignore unknown fields.
- With `RuntimeOptions=on`, expect one outfit for several rows (the same `slot` on each row left on) and no row for
  retouches and subsets; your variant choice sets the outfit's **default** options, the player's in-game choice wins.
- To force a rebuild, empty `registry.json`'s `"fingerprint"`.
- If your NSFW keyword list changes, users can mirror it with `NsfwKeywords=` in `settings.ini`.

**Do not**

- Name your own temporary files `re_chunk_000.pak.patch_*.pak.tmp` or `*.pak.spill` in the game folder: the loader
  deletes them at launch.
- Install files under a slot's `rto/` folder, or edit `costume_options.json`, `costume_choices.json`,
  `pak_sources.tsv` or `registry.json` (other than emptying its fingerprint).
- Expect `costume_mods_nsfw` mods in `variants.json`, or turn them off with `variant_choices.json` (ignored).
- Expect `variants.json` to be rewritten on a launch that rebuilds nothing.
- Use `swap_repairs.txt`: not read by this loader.
- Replace or delete the loader's pak or its marker-only placeholders while the game runs; `SF6_SlotsClean.exe` is the
  supported clean-up for the patch day.
