# Runtime costume options (design, in progress)

Status: lot A (the loader part) and lot B (the plugin: runtime swaps, the Battle Settings menu, the `former`
migration) are built, behind a setting that is **off by default**. Lot B is not tested in game yet. Nothing changes
for a user who does not set `RuntimeOptions` (no manifest: the plugin's runtime-options code stays idle).

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
- Texture bindings are per model: during the probe's texture phases on P1, P2's binding kept its own path in every
  sample (`rto2_result_p1.json`, `other.tex`). The visual anomaly seen after a swap and its restore (P2's dress
  looked different) is not explained by the bindings; see [Textures](#textures-and-the-shared-material-question).

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
  When an option is chosen, the menu turns off the chosen options of other groups that come **later** in the
  layering and ship one of its files (they would hide it). Earlier ones stay: the new option wins the shared file and
  their other files still apply, as in the variants the loader builds (Mai "Regular - Barefoot" = an SFW option for
  its dress texture + a Skimpy option for the body mesh). (Lot B narrowed the first rule, "any option sharing a
  file", which turned off the SFW option's texture and left a set no variant has.)
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
  A mesh gets a base copy only if it has every joint of the slot's mesh (its name table, materials aside): the
  model keeps its chains and other parts while the mesh is swapped, and they move those joints. Swimsuit C. Viper
  ships no body in its main files: the game's Outfit 1 body, without the 18 joints the swimsuit's chain moves,
  stopped the game in a chain job half a second after the swap (19/10).
  A base copy is made **only when it fits** what the model may wear with it (20/10, loader `2026-10-20-rtofit`):
  - a material file must define every material of each mesh the model may wear at its part (the meshes the mod's
    options ship there, else the game's) and every texture it names must exist; the **game's** material file only
    where the mod keeps the game's mesh for that part. Mai mini dress ships the body's materials in its skin
    options only: the game's Outfit 1 material file lacked 11 of the 14 materials of the dress's body and "None"
    drew the body black. C. Viper swimsuit keeps the game's material names, but the game's file still laid the
    game's textures on the mod's mesh (34 of the 83 textures of its skin materials are the mod's).
  - a mesh: every material it uses must be in each material file an option ships for its part; the **game's** mesh
    only where the mod keeps the game's material file (C. Viper lace lingerie: Outfit 2's body, refused).
  - a texture from the game (none of the mod's base files ships it): only with the proportions of the slot's own.
  - the mod's base files (main files) are used when they fit.
  The loader logs each refusal (`runtime options: no base copy of ... (why)`).
- **Required groups**: a pick group one of whose options ships a file the slot wears with no base copy, and no pick
  of another group ships it, is listed in the slot's `required`: the menu offers it **without None** (the group
  always wears one of its options). On Wael's install: Mai's skin group, both groups of each C. Viper mod. Mai's
  dress group keeps None: its base copy is the main files' `mini1_atos.tex` (the same file as its "side stripes").
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
     "base": [{"kind": "texture", "target": "product/model/esf/esf028/mini/mini1_atos.tex",
               "path": "product/model/esf/esf028/006/rto/base/_/mini/mini1_atos.tex"}, "..."],
     "required": [1],
     "former": [{"costume_no": 6, "folder": "007", "label": "mini dress Mai C1 Swap - Regular - Barefoot",
                 "options": ["0e0a734f", "b104ad5a", "a7125dd3", "d5be40d1", "4fe85878"]}, "..."]}]
}
```

- `mods[].key` = `<fighter folder>/<root name, lowercase>`: stable across mod updates; the choices file uses it.
- `mods[].variants` = the combinations the loader makes as variant slots: ready-made presets.
- `slots[].options` lists only options that have files for that slot (a multi-costume mod's other folders belong to
  its other slots).
- `slots[].former`: see [Saved choices and slot numbers](#saved-choices-and-slot-numbers).
- `slots[].required`: pick groups (index in the mod's `groups`) offered without None (see Base copies). A side's
  choices that hold no option of such a group (a None saved before) wear its default: the slot's default option in
  that group, else its first offered option (`complete()` in `rto_data.cpp`, also after every menu change).
- `format` changes when a field changes meaning; the plugin ignores a manifest of another format, or one whose
  `pak` is not the costume pak of `outfits.json` (stale file).

## The user's choices: `reframework/data/SF6_CostumeSlots_data/costume_choices.json` (plugin, lot B)

Written by the plugin only, read at runtime. Per mod key, per player side:

```json
{
  "format": "sf6-costume-choices-1",
  "former_done": "2026-10-19 21:45 re_chunk_000.pak.patch_001.pak",
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
- "Side" = the menu's Player side row (P1 by default): offline, the fighter seated on that side wears that side's
  options (Training dummy, Versus); online, the player's own fighter wears P1's (the opponent never wears a slot).
- `former_done`: the manifest (`generated` + `pak`) whose `former` slots were migrated.

## Runtime (plugin, lot B)

Effective file of a target, for one model: among the chosen options in layering order, the last one that ships it;
none -> the `base` copy if listed; else the slot's own file (nothing to do). A required group always has one of its
options in the set (see `slots[].required`).

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

### Built (lot B): `plugin/src/rto.cpp`, `rto_data.cpp`

- **Load**: at plugin start (synchronously, before the costume module: the `former` migration needs it; Mai's
  manifest is 20 KB). A manifest of another format, or one whose `pak` is not `outfits.json`'s, is ignored (one log
  line). The loader now also renames the pak in `costume_options.json` when it only moves the pak (same "pak" key as
  `outfits.json`), else the plugin would take the manifest for stale.
- **Models**: the preview (`CaptureRoomFighter.FighterObj` once `IsSetUpEnd`) and each fighter (`PlayerBehavior`,
  `_PlayerID` 0 / 1, once `IsAppliedResource`). Its components are walked once (transform tree, 400 nodes at most):
  `via.render.Mesh`, `via.motion.Chain`, `app.PlayerColorController`. Its **slot is found from the paths its
  meshes load** (`product/model/esf/<fighter>/<folder>/`), so the online alias (a slot over its carrier) needs
  nothing more. Game objects are never `add_ref`'d (R21): checked alive (`is_managed_object` + type) before use.
- **Pass** (per model, what differs from the manifest's desired state): verify the parts swapped on the frame
  before; meshes and materials (a pair in the same frame when both change); `SetColor(ColorNum)` on the model's
  colour controllers after a material change; textures on parts whose material is settled (the bindings of a
  material that name a target are found once per material file); chains (`set_ChainAsset` + `restart`).
- **Never half loaded**: every file is created ahead (`create_resource` + holder, kept by the plugin) and given to a
  model only 30 frames later (textures 60); then the model's own flags must say drawable **in the same frame**
  (`MeshReady`; `MaterialReady` and `MaterialNum` > 0) and again **on the next frame**, else the part goes back at
  once to what it wore (a holder of that file made before the swap) and is tried again later (1 s, 2 s, 4 s...;
  left out after 6 tries, one log line). The probe's not-resident body stayed `MeshReady` 0 for 50+ frames.
- **Chains**: the body's chain is not loaded from the slot's folder (Mai: `esf028/002/esf028_002_01_chain.chain`),
  so an option's `..._01_chain.chain` stands for the chain of the part with the same number (the GameObject whose
  mesh is `..._01.mesh`); the way back is the file that part wore when first seen.
- **When**: the preview while Battle Settings is up (every 10 frames: a menu screen; a model the game rebuilt, e.g.
  outfit or colour change, is found by its paths and done again). Fighters only after an event: an outfit mounted
  for a fight (`FighterVisualHolder..ctor`) or a model built (`PlayerBehavior.SetupAppearance`) opens a 10 s watch
  (every 10 frames), then nothing: no read in a fight (R38). If `SetupAppearance` kept the watch open for 30 s it is
  ignored for the session (not a build event). Nothing while the character select is open.
- **Ahead**: 30 s after start, the files of the choices made in the menu (both sides, 64 at most) are loaded and
  kept, so a fight shows them at once; other holders are given back 10 s after their last use.
- 5 exceptions: the runtime options stop for the session (one log line).

### Textures and the shared material question

The probe saw a texture change leak to the other player (or not revert). Before relying on `setMaterialTexture`:
test whether each model has its own material instance (swap on P1 only, read P2's binding back). If materials are
shared, the fallback is per-model materials: the loader writes, per slot, a material copy per texture option with
its binding pointed at the option's texture (`mdf2_rebind_textures`, already used for options shipping their own
textures), and the plugin swaps that material per model (`set_Material` is proven per model). Skin options that are
materials themselves then combine with a texture option only through such copies; the loader would build them for
the pairs actually chosen (lot B decides).

Lot B, from the probe's results: P2's binding (`getMaterialTexture` path, same slot, same material) stayed
`mini/mini1_ATOS.tex` through P1's swap to `royalb1_ATOS.tex`, its hold and its restore, and the screenshot during
the swap shows P2 unchanged (`results_v2/p1_tex_atos_2on.png`). **Bindings are per model**: lot B uses
`setMaterialTexture` per model and does not build the material copies. The difference seen after the restore is not
explained by the bindings (the probe restored through a fresh holder of the original path). Lot B never keeps the
game's holders (R21) either: a way back is a holder the plugin creates for that file. **To check in game**: P1 and
P2 in the same outfit, a dress option on P2 only: P1's dress must not change, before and after going back. If it
does, the fallback above is the next step.

## Menu (Battle Settings, lot B)

- Through the core's option service (`opt_group`, `opt_entry`, `opt_open_menu`, `opt_navigate` with PUSH,
  `opt_rebuild`; see `core/docs/OPTIONS.md`): one page per costume, one choice row per `pick` group, one on/off row
  per `toggle` option, a "Restore defaults" button (no preset row: the `variants` only serve the migration of a former
  variant slot). The rows follow the costume shown
  (the page is rebuilt when it changes).
- `F` entry in the bottom input guide bar, only on the costume row of a costume that has options (`costume_options.json`
  lists its slot).
- Live preview: each change is applied to the preview model at once (steps 2-3), and saved.
- No new core capability is expected. If the option service cannot give a page whose rows change with the costume
  without closing it, that is a core change and needs an ADR (Proposed, then accepted by the user) before code.

### Built (lot B): `plugin/src/rto_menu.cpp` (no new core capability)

- **Where**: Battle Settings > character (Fighter Settings, agent `MatchingFighterSetting`). While the costume row
  (`p_Spin_costume`) is focused and the preview wears a slot with options, the bottom input guide bar gets an extra
  entry `<ICON KeyF> Costume options` (`<ICON BtnD>` after a pad input), like the Color Mod's `<ICON KeyF><ICON
  MouseL> Edit Color` on its Color row. The costume row's own text is no longer touched (it is only read, to see
  when the outfit changes). The label is English only (the plugin has no locale system).
- **Coexistence with the Color Mod** (same text object, the bar's `e_text_0_g_*` else first `e_text_0_d_*`): the
  Color Mod strips everything from the first `"      <ICON"` (six spaces) of the game's text and appends its entry
  after it, only on its Color row; its stored copy is written back every frame and refreshed every 5 frames from
  what it reads. Our entry starts with FOUR spaces, so its strip does not match: it keeps ours as game text (no
  rewrite) and its own entry lands after ours. We remove only our exact entry (either icon) wherever it sits, and
  add it only on the costume row of an outfit with options; elsewhere (Color row, other rows, window closed, menu
  open) it is removed and the rest of the text stays. No identical write; a fight of more than 60 writes in 120
  frames (the Color Mod's stale copy right after a change) is logged once and the text is still rewritten.
- **Key**: `F` (or the pad's A / Cross) with the costume row focused opens the menu. The Color Mod's keys are
  elsewhere: F on the Color row (its editor), X (Drive colours), C / V inside its editor; its editor and this menu
  never open together (both need the option window closed).
- **Pages** (7 rows at most, the dialog's pool): page 1 = Player side (P1 / P2), the first option rows, More options (pushes page 2), Restore defaults; later pages = option rows (+
  More options). A pick group = one choice row: None + its options (15 labels at most a row, R9: a bigger group is
  split in rows "Group (1/2)"...); a required group has no None (split in rows, its first label is "(other row)":
  picked on the row that holds the choice, it goes back to the group's default). A toggle = an Off / On row. Options that change nothing on the model (colour or
  cloth files only, e.g. the tan) are not offered; one that also ships such files is marked `*` (its description
  says so). Texts are English only for now.
- **A change**: saved at once (`costume_choices.json`), applied to the preview by the runtime (a not-yet-loaded
  file shows within a second); the page is rebuilt on the next frame when other values changed (an option turned
  off), a page under it is rebuilt when it comes back on top. The window docks left (x -430, as the
  Color Mod's editor on this screen) so the preview stays visible. Esc goes back / closes, as the game's dialogs.
- **Preview camera** (`plugin/src/rto_camera.cpp`, a port of the Color Mod's `colormod/src/dialogs/camera.cpp`
  with the same constants, limits and input; the Color Mod is unchanged): while the menu is open on Fighter
  Settings, exactly what the colour editor gives. Mouse right of the docked dialog (x > 1040 at 1920 wide): left
  drag = turn (`app.FighterSpinCtrl`'s `Rot` yaw + `mAngle`), middle drag = pan, wheel = zoom (`ess_DefaultCamera`
  local position, eased, clamped); PageUp / PageDown = zoom anywhere; pad: right stick = turn, L3 + right stick =
  pan, R2 / L2 = zoom. The cursor is read only while the game window has the focus. The screen's hint blocks
  `c_00` (Move: `<ICON MouseM>` or `<ICON BtnL3>＋<ICON StickR>`) and `c_02` (Zoom: wheel or L2 / R2 glyphs) are
  shown at the editor's positions, glyph size from the game's Rotation block `c_01`, titles `hint.move` /
  `hint.zoom` from the shared `SF6_NativeLocale_data/locale.json` in the game's text language (English when the
  file is absent). The device (pad or keyboard / mouse) follows the Color Mod's rule (a pad button; a menu key, a
  mouse button or a cursor move), also for the guide bar entry. On close: the camera position goes back to what
  it was before the first zoom / pan (the turn stays, as in the editor) and both blocks are hidden; this happens
  on the frame after the window closed (never from the core's callback). Input is only read, never swallowed.
  The Color Mod hides `c_00` / `c_02` every 30 of its frames while Fighter Settings is up and its editor is closed
  (so also under our menu): the plugin paints them from a post-LateUpdateBehavior callback (after every pre
  callback of the frame) and shows a block again on any frame it finds hidden, so nothing blinks.

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

Built (lot B): `costumes.cpp` `load_state` asks `rto::migrate_former` for every Battle Settings choice whose costume
number the registry no longer has; with layout `one` and the number in a slot's `former`, the choice moves to the kept
slot (its colour kept) and that variant's options become the mod's P1 choice (`changed: true`); `state.json` and
`costume_choices.json` are written, `former_done` notes the manifest. Layout `variants` never migrates (the variant
slots still exist). Not done: Color Mod colours made for a former folder.

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

- Lot B limits: a model the game rebuilds in a fight without one of the two events (round reload, replay seek...) is
  not seen until the next one (the probes saw no rebuild on Training resets); the Training pause menu's Character
  Settings popup has no menu (its preview is not handled); menu texts are English only; no mouse click on the
  costume row; whether the pad's A on the costume row does something in the game was not checked.
- A modular mod that is an add-on (no body mesh, e.g. "Mai face accessories": the loader logs "partial option ... no
  body mesh in part 01") makes no outfit, so no slot lists its options: its stored options wait for the add-on
  archives item below.

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
- **B (built, to test in game)**: plugin runtime (load, preload, apply per model, rebuild moments), the
  texture-sharing check (offline: per model; the fallback is not built), choices file, Battle Settings menu,
  migration of `state.json`. Left for later: Fluffy unchecked options, add-on archives as option groups.
- **C**: tan and colour options through the Color Mod; presets and "Save as"; default `RuntimeOptions=on`.
