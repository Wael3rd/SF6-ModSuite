# Slotsloader Manager contract

ZZtaii's Slotsloader Manager installs costume mods for the Slots Loader and reads what
the loader wrote at the last launch. This page says what our loader (`amd_ags_x64.dll` + `costume_loader.exe`,
`costumes/loader/loader_core.cpp`) provides to the Manager, under which **contract number**, and what the Manager
needs to change to accept our loader. The variant code below comes from ZZtaii's Slotsloader fork (1.10.0, MIT).

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
- **Replace original installs**: the loader still makes an outfit of its own from loose game files and from
  foreign patch paks, so an outfit the Manager installed with "Replace original" can also appear as an extra
  outfit. Not covered yet.
- `variants.json` is not rewritten on a launch that rebuilds nothing (same as the fork).

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
With `on`, a retouch (meshes and textures only, no material file: `RUNTIME_OPTIONS.md`, Retouches) makes no outfit:
it is an option of the outfits that wear its files and has no row in `variants.json`.

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
