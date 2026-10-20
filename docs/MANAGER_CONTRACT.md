# Slotsloader Manager contract

ZZtaii's Slotsloader Manager installs costume mods for the Slots Loader and reads what
the loader wrote at the last launch. This page says what our loader (`amd_ags_x64.dll`,
`costumes/loader/loader_core.cpp`) provides to the Manager, under which **contract number**, and what the Manager
needs to change to accept our loader. The variant code below comes from ZZtaii's Slotsloader fork (1.10.0, MIT);
the Replace-original record follows the Manager's own `replace-state.json` (ZZtaii).

## Contract number

| Where | Field | Value |
|---|---|---|
| `modinfo.ini` of `SF6_SlotsLoader_v*.zip` (the component that ships `reframework/plugins/SF6_CostumeSlotsNative.dll`) | `SlotsloaderContract=` | `1` |
| `reframework/data/SF6_Costumes_Data/variants.json` (written by the loader) | `"contract"` | `1` |

Both come from `MANAGER_CONTRACT` in `loader_core.cpp` (the packaging script reads it there). The number goes up
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
4. Unchanged files the Manager already reads: `outfits.json` (`source`, `name`, `label`, `left_out`, `pak`) and
   `registry.json`'s `"fingerprint"` (emptied by the Manager to force a rebuild).

### Not in contract 1

- **Character-swap repairs** (fork 1.10.4, `swap_repairs.txt`): not ported. The file is ignored; outfits are built
  as without it. Porting it means the fork's swap-repair module (`patch.cpp`, about 200 lines with verified file
  fingerprints) and changes in the pak read paths of every mod file, the scene USER files and one Mai model-parts
  case: not small and not isolated, left for its own task.
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

`reframework/costume_mods/settings.ini`, `RuntimeOptions` (`docs/RUNTIME_OPTIONS.md`):

| Layout | Outfits | Variants listed | A variant turned off |
|---|---|---|---|
| `off` (default) | one per variant (as the fork) | one row per outfit, label = `outfits.json`'s `label` | its outfit is not built |
| `variants` | as `off`, options stored too | as `off` | as `off` |
| `on` | one per modular mod / outfit with add-ons | the outfits `off` would make, with `off`'s labels; every row left on shows the one outfit as its slot | the one outfit wears by default the first row left on (same outfit number); every row off: no outfit |

With `on`, the Manager's choice only sets the outfit's **default options** (`costume_options.json`, `defaults`,
the files built into the pak). What the player picked in the in-game options menu (`costume_choices.json`) wins
over the default, as it always does. Because the labels are the same in the three layouts, choices made under one
layout carry over to the others. A mod made of a single outfit is one row (the Manager shows no toggle for it).

## What the Manager needs to change

The Manager 1.0.7 refuses our package before reading anything else: `LoaderCostumePak.IsUpstreamBuild` (used by
`ReplacePlanner.Prepare`) treats a package whose plugin component says `author=Wael` without a fork mark
(`name` starting with "Slotsloader", `ZZtaii` in `author`, `-custom` in `version`) as upstream and adds an error.
To accept a loader that implements the contract, check the number first, for example:

```csharp
// The loader says which Manager contract it implements (modinfo.ini SlotsloaderContract=N).
internal const int KnownContract = 1;
internal static bool HonoursContract(ModComponent c)
{
    string v; int n;
    return c != null && c.Metadata.TryGetValue("SlotsloaderContract", out v) && Int32.TryParse(v, out n) && n >= KnownContract;
}
// ReplacePlanner.Prepare: refuse only when !HonoursContract(plugin.Component) && IsUpstreamBuild(...)
```

At run time, `variants.json`'s `"contract"` tells which contract the installed loader honours (a missing field = a
loader before the contract). `VariantChoices.ReadReport` can keep ignoring it.

Second, for Replace original: write `reframework/data/SF6_Costumes_Data/manager_installs.json` as above. Until then
the loader reads `Data/replace-state.json` wherever it can find the Manager; a Manager folder it cannot find
(unusual place, no `SlotsloaderManager=` in `settings.ini`) means its Replace-original installs show up twice again.
