# Runtime costume options (design, in progress)

Status: lot A (this document + the loader part) is built behind a setting that is **off by default**. The plugin
part (menu, runtime swaps) is not built yet. Nothing changes for a user who does not set `RuntimeOptions`.

## Goal

Costume mods made for Fluffy Mod Manager often come as one archive with groups of options: body types, skins,
dress textures, SFW / NSFW variants, extra jiggle, a tan, "hide ponytail" for hair mods... Today the Slots Loader
turns such a *modular mod* into up to 24 outfits (one per combination of the options that change meshes) and keeps
only the first option of every other group. Example, "Mini Dress Mai C1 Swap" v1.5: 43 options in 6 groups, 10
outfits, 32 options never reachable.

Target:

- **One outfit per modular mod.** Its options are chosen in game, in **Battle Settings**, like the Color Mod's
  colour editor: the `F` hint shows only on a costume that has options, the preview shows the result live.
- Applied **at runtime**, without relaunching, **per player**: P1 and P2 can wear the same outfit with other options.
- Every character, every modular mod: all its options are detected and listed.
- Fluffy vs menu: the options checked in Fluffy (or the loader's default combination) apply until the user changes
  something in our menu; from then on, the menu's choice wins.

## Proven in game (probes, 19/10)

On a fighter's model (Training) and on the Battle Settings preview, per model:

- `via.render.Mesh`: `setMesh(MeshResourceHolder)`, `set_Material(MeshMaterialResourceHolder)`,
  `setMaterialTexture(material, texture, TextureResourceHolder)`; `via.motion.Chain`: `set_ChainAsset` + restart.
- The change takes effect, holds, survives colour changes (`PlayerColorController.SetColor`) and position resets.
  P2 in the same slot is untouched as long as the write goes to the model, **never to the shared costume manifest**
  (`FighterVisualHolder._Setting`: one manifest serves P1 and P2).
- A resource that is not resident yet is not ready on the first frames (body invisible after `setMesh`, black after
  `set_Material`, for 30 frames and more): options must be **preloaded** and applied once ready. Already loaded,
  the swap is ready at frame 0.
- A body mesh and its material file belong together: swap them as a pair when an option ships both.
- Open anomaly: after a texture swap and its restore, both P1 and P2 dresses looked different. A texture binding may
  live in a material instance shared by both models. See [Textures](#textures-and-the-shared-material-question).

## Settings

`reframework\costume_mods\settings.ini`:

| `RuntimeOptions=` | Outfits of a modular mod | Options stored + `costume_options.json` |
|---|---|---|
| `off` (default, also when missing) | one per combination, as before | no (the manifest is deleted) |
| `on` | **one** (the mod's first combination) | yes |
| `variants` | one per combination, as before | yes |

`on` is the target layout. `variants` is the fallback: the options are available in game while the old outfits stay.
The default becomes `on` in the release that ships the plugin part. `off` builds exactly what the loader built
before (same pak bytes, same `outfits.json`, same registry).

## Data model

A modular mod is the tree `build_modular` already reads: options (folders with `modinfo.ini`) whose `AddonFor`
names a group, groups whose `AddonFor` names the root.

- **Group**: the options sharing one `AddonFor`, in the order `build_modular` lays them over each other (sorted
  group names; the files of a later group replace those of an earlier one at the same path).
- **Option**: `id` (8 hex digits, a hash of the group and the option's folder name: stable when the archive is
  updated or renamed), `name` (from `modinfo.ini`), `folder`, its files, and a `mode`:
  - `base`: always worn, not a choice (the root group's main files, the option `build_modular` always keeps);
  - `pick`: one option of the group at a time (a group of 2 or more options);
  - `toggle`: on or off (another option of the root group such as "hide ponytail", an option or a group whose name
    says "optional", a group of one option).
- An option is **a set of files: (original path -> stored copy)**. Each file has a **kind**: `mesh`, `material`
  (`.mdf2`), `texture`, `chain` (all four swapped on the model at runtime), `colour` (colour files, for the Color
  Mod) or `other` (user files, cloth, scenes: only at the next launch). Per option, `apply` sums it up: `live`,
  `colour` or `relaunch`.
- Options of different groups can ship the same file (Mai: the NSFW, SFW and Skimpy groups all ship the body mesh).
  They exclude each other: the menu turns off any chosen option that ships a file of the option just chosen.
- **Defaults** of an outfit = the options its combination is made of, minus those whose every file a later option
  replaces (they are not worn).

## Storage in the costume pak

For every slot of a modular mod, every option except `base` ones is stored **as that slot would use it**, under the
slot's folder:

```
natives/stm/product/model/esf/<fighter>/<slot folder>/rto/<option id>/<path in the slot folder>
natives/stm/product/model/esf/<fighter>/<slot folder>/rto/<option id>/_/<path in the fighter folder>
```

- *Target* = the file the slot's model loads in place of the original (`esf028/001/01/esf028_001_01.mesh` becomes
  `esf028/006/01/esf028_006_01.mesh` for the slot in folder 006; a shared file such as `esf028/mini/mini1_atos.tex`
  keeps its path). The loader records every file it relocates for the slot and uses that; a file the slot did not
  relocate keeps its original path (the slot loads the game's file there).
- Materials are processed like the slot's own: texture references moved with the slot, files made for an older
  material layout rebuilt on the game's, and the references to textures **the option itself ships** pointed at the
  option's copies (new `mdf2_rebind_textures`, in `patch.cpp`; the path is appended to the file, as
  `modernize_mdf2` already does). Meshes, textures, chains and colour files are the mod's bytes. A texture gets its
  streaming twin like the slot's textures.
- **Base copies** (`rto/base/`): for every file of an option the outfit is made of, what that file is without the
  option: the mod's base files, else the game's file (patched like the slot's). They let the runtime go back to "no
  option of this group" exactly as Fluffy would show it.
- The pak builder already stores identical data once: the copies of a file shared by the slot and by its options,
  or by several slots, cost an entry (48 bytes), not their size.

Mai mini dress, measured offline (game base pak, mod from its archive):

| | Pak | Entries | Build |
|---|---|---|---|
| `off` (= before) | 374.6 MB | 3841 | 1.6 s |
| `variants`: 10 outfits + options | 732.5 MB | 4481 | 5.3 s |
| `on`: 1 outfit + options | 725.3 MB | 3374 | 1.2 s |

The options of one outfit are 42 options, 64 files (5 base copies). The pak grows by the options that were never
installed before (13 body meshes, 12 dress textures...).

**Fingerprint.** `off` leaves the registry fingerprint as it was (no rebuild for existing users). `on` / `variants`
add `;options=sf6-costume-options-1:<layout>`: switching the setting rebuilds once. A missing
`costume_options.json` also forces a rebuild.

## The options manifest: `reframework/data/SF6_Costumes_Data/costume_options.json`

Written by the loader with the pak (same generation; `pak` names it), read by the plugin. Resource names are as the
game names resources: lowercase, without `natives/stm/` and without the version suffix.

```json
{
  "format": "sf6-costume-options-1",
  "generated": "2026-10-19 21:44", "loader": "<build id>", "pak": "re_chunk_000.pak.patch_001.pak",
  "layout": "one",
  "mods": [
    {"key": "esf028/mini dress mai c1 swap", "name": "mini dress Mai C1 Swap", "source": "Mai/Mini Dress Mai C1 swap v1.5",
     "defaults_from": "loader", "groups": [
      {"name": "(mini dress Mai C1 Swap)", "options": [
        {"id": "0e0a734f", "name": "1) Main Files c1", "folder": "...", "mode": "base", "apply": "relaunch"},
        {"id": "2407ae42", "name": "5) optional - more jiggle and wobble", "folder": "...", "mode": "toggle", "apply": "live"},
        {"id": "b741b9f2", "name": "5) optional - Tan skin tone", "folder": "...", "mode": "toggle", "apply": "colour"},
        {"id": "b1e9e85c", "name": "6) hide ponytail - to work with hair mods", "folder": "...", "mode": "toggle", "apply": "live"}]},
      {"name": "2! Skin type options c1", "options": [
        {"id": "b104ad5a", "name": "damaged skin", "folder": "...", "mode": "pick", "apply": "live"}, "..."]},
      "..."],
     "variants": [
      {"label": "mini dress Mai C1 Swap - Regular - Barefoot + pubic hair",
       "options": ["0e0a734f", "b104ad5a", "a7125dd3", "d5be40d1", "77b84100"]}, "..."]}],
  "slots": [
    {"fighter": 28, "fighter_dir": "esf028", "costume_no": 5, "folder": "006", "name": "Outfit I",
     "label": "mini dress Mai C1 Swap", "mod": "esf028/mini dress mai c1 swap",
     "defaults": ["0e0a734f", "b104ad5a", "a7125dd3", "d5be40d1", "77b84100"],
     "options": {
       "2407ae42": [{"kind": "chain", "target": "product/model/esf/esf028/006/esf028_001_01_chain.chain",
                     "path": "product/model/esf/esf028/006/rto/2407ae42/esf028_001_01_chain.chain"}],
       "a7125dd3": [{"kind": "texture", "target": "product/model/esf/esf028/mini/mini1_atos.tex",
                     "path": "product/model/esf/esf028/006/rto/a7125dd3/_/mini/mini1_atos.tex"}],
       "...": []},
     "base": [{"kind": "mesh", "target": "product/model/esf/esf028/006/01/esf028_006_01.mesh",
               "path": "product/model/esf/esf028/006/rto/base/01/esf028_006_01.mesh"}, "..."],
     "former": [{"costume_no": 6, "folder": "007", "label": "mini dress Mai C1 Swap - Regular - Barefoot",
                 "options": ["0e0a734f", "b104ad5a", "a7125dd3", "d5be40d1", "4fe85878"]}, "..."]}]
}
```

- `mods[].key` = `<fighter folder>/<root name, lowercase>`: stable across mod updates; the choices file uses it.
- `mods[].variants` = the combinations the loader makes as variant slots: ready-made presets.
- `slots[].options` lists only options that have files for that slot (a multi-costume mod's other folders belong to
  its other slots).
- `slots[].former`: see [Saved choices and slot numbers](#saved-choices-and-slot-numbers).
- `format` changes when a field changes meaning; the plugin ignores a manifest of another format, or one whose
  `pak` is not the costume pak of `outfits.json` (stale file).

## The user's choices: `reframework/data/SF6_CostumeSlots_data/costume_choices.json` (plugin, lot B)

Written by the plugin only, read at runtime. Per mod key, per player side:

```json
{
  "format": "sf6-costume-choices-1",
  "mods": {
    "esf028/mini dress mai c1 swap": {
      "p1": {"changed": true, "options": ["0e0a734f", "3d8bf169", "4989efb2", "cc13098f"], "names": ["..."]},
      "p2": {"changed": false}
    }
  }
}
```

- `changed: false` (or no entry): the slot's `defaults` apply (Fluffy's checked options, or the loader's
  combination). As soon as the user changes an option of that side in our menu, `changed` becomes true and the list
  wins, until "Restore defaults".
- Ids plus names: an option renamed or removed by a mod update is dropped (log line), the others stay.
- Keyed by mod, not by slot number: the choices survive slot renumbering.
- "Side" = the player side of the local user in Battle Settings (P1 / P2 there), the side seated in a fight.

## Runtime (plugin, lot B)

Effective file of a target, for one model: among the chosen options in layering order, the last one that ships it;
none -> the `base` copy if listed; else the slot's own file (nothing to do).

1. **Load**: read `costume_options.json` and the choices once at start (background thread), again when the loader's
   generation changes.
2. **Preload**: when a costume with options is about to be shown (Battle Settings preview, character select
   decided, fight load), `create_resource` + holder for every file the chosen options need, and wait until each
   is ready. A file never ready after N frames is skipped (log line), never applied half loaded.
3. **Apply per model** (each fighter's own GameObjects, the preview's own): materials first (`set_Material`, mesh +
   material as a pair when both change), then meshes, then textures (`setMaterialTexture` on the bindings whose
   path is the target), then chains (`set_ChainAsset` + restart). Never `FighterVisualHolder._Setting`. Game
   objects only from `LateUpdateBehavior`, kept objects checked with `is_managed_object` (Slots Loader rules).
4. **When**: on every rebuild of a model (preview costume / colour change, character select, fight start, round
   reload, replay, training reset): hook the moment the model's meshes are set, re-apply. After
   `PlayerColorController.SetColor` the probe showed the swaps hold; re-check textures there.
5. **Colours** (`colour` kind, the tan): the Color Mod applies them (see [Tan](#tan)); the loader already stores
   the files.
6. **Online**: options are visual and local: the opponent sees the default outfit (no network data), as for colours.

### Textures and the shared material question

The probe saw a texture change leak to the other player (or not revert). Before relying on `setMaterialTexture`:
test whether each model has its own material instance (swap on P1 only, read P2's binding back). If materials are
shared, the fallback is per-model materials: the loader writes, per slot, a material copy per texture option with
its binding pointed at the option's texture (`mdf2_rebind_textures`, already used for options shipping their own
textures), and the plugin swaps that material per model (`set_Material` is proven per model). Skin options that are
materials themselves then combine with a texture option only through such copies; the loader would build them for
the pairs actually chosen (lot B decides).

## Menu (Battle Settings, lot B)

- Through the core's option service (`opt_group`, `opt_entry`, `opt_open_menu`, `opt_navigate` with PUSH,
  `opt_rebuild`; see `core/docs/OPTIONS.md`): one page per costume, one choice row per `pick` group, one on/off row
  per `toggle` option, a "Restore defaults" button, presets from `variants`. The rows follow the costume shown
  (the page is rebuilt when it changes).
- `F` hint on the costume row only when the costume has options (`costume_options.json` lists its slot).
- Live preview: each change is applied to the preview model at once (steps 2-3), and saved.
- No new core capability is expected. If the option service cannot give a page whose rows change with the costume
  without closing it, that is a core change and needs an ADR (Proposed, then accepted by the user) before code.

## Saved choices and slot numbers

With `on`, the single outfit of a modular mod is its first combination: same files and same id as the first
variant slot, hence **the same costume number, folder, record and colours** as that slot had (checked offline: slot
`esf028/006`, costume 5, identical files). The other variants' numbers are freed; the loader already gives every
unused number the character's Outfit 1 scene, so a save or a replay naming one shows Outfit 1 and never stalls.

To keep what the user chose for the other variants, `costume_options.json` lists them in the kept slot's `former`
(previous costume number and folder, label, the options that variant was made of) **on the launch that changes the
layout**. On that launch the plugin (lot B):

- moves a Battle Settings choice (`SF6_CostumeSlots_data\state.json`) that names a former number to the kept slot
  and writes that variant's options into the choices file (`changed: true`) for that side;
- Color Mod colours stored per costume folder: the kept slot keeps its folder; colours made for a former folder are
  offered as-is on the kept slot (lot B checks the Color Mod's keys).

`former` is only known while the old registry is read: if the game does not reach the plugin on that launch, the
next rebuild no longer lists it (improvement: carry `former` over from the previous manifest).

## Presets and "Save as"

- The `variants` of a mod are built-in presets.
- "Save as" from the menu: a named set of options in the choices file (`presets` per mod key), offered in the menu
  and shown on the select screen through the same outfit. A real extra slot would need a relaunch (the costume
  table and scenes are built at launch); if wanted later, the loader can read the saved presets and build one slot
  per preset at the next launch (the variant code already builds a slot from a set of options).

## Tan

The tan option ships colour files whose only change is the skin entry (entry 0 of clusters `esf_Body00` and
`esf_Head00`), its own shade per colour number. It can therefore mix with any colour: the Color Mod (later) applies
it as an overlay per colour number on top of the chosen colour, from the stored `colour` files of the option.

## Not covered yet

- **Fluffy installs**: a mod read from Fluffy's archive loads only the options checked in Fluffy, so only those
  are stored. Lot B: load the unchecked options too, as options only (no outfit), with the checked ones as
  defaults (`defaults_from: "fluffy"`).
- **Separate add-on archives** for the same costume (e.g. a face accessories mod applied to any Mai outfit): listed
  as extra option groups of every slot whose costume uses the parts they replace. Lot B.
- `other` files (cloth, physics user files) cannot be swapped at runtime: their options apply at the next launch
  only (or not at all with `on`, the menu says so).
- Slotsloader Manager compatibility (`variants.json` / `variant_choices.json`): later, as its own task.

## Phases

- **A (done, setting off by default)**: options stored per slot, base copies, material rebinding,
  `costume_options.json` (options, defaults, presets, former slots), `RuntimeOptions=off|on|variants`.
- **B**: plugin runtime (load, preload, apply per model, rebuild moments), the texture-sharing test and its
  fallback, choices file, Battle Settings menu, migration of `state.json`, Fluffy unchecked options, add-on
  archives as option groups.
- **C**: tan and colour options through the Color Mod; presets and "Save as"; default `RuntimeOptions=on`.
