# Runtime costume options (design, in progress)

Status: lot A (the loader part) and lot B (the plugin: runtime swaps, the Battle Settings menu, the `former`
migration) are built, behind a setting that is **off by default**. Lot B is not tested in game yet. Nothing changes
for a user who does not set `RuntimeOptions` (no manifest: the plugin's runtime-options code stays idle).

## Goal

Costume mods made for Fluffy Mod Manager often come as one archive with groups of options: body types, skins,
dress textures, SFW / NSFW variants, extra jiggle, a tan, "hide ponytail" for hair mods... Today the Slots Loader
turns such a *modular mod* into up to 50 outfits (one per combination of the options that change meshes) and keeps
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
names a group, groups whose `AddonFor` names the root. The Fluffy menu entries (folders holding only a
`modinfo.ini`, usually `DummyMod=True`) are read too, with their descriptions (ported from ZZtaii's Slotsloader
fork 1.9.7-custom, MIT).

- **Group**: the options sharing one `AddonFor`. When options hang from sub-groups of the menu ("03 Outfit" >
  "Lower Neckline" > option), every option under one top-level group is ONE group (one choice), keyed by that
  top-level group's **name** (`menu:<name>`, not its place in the menu, so saved choices keep their ids); the main
  files of such a mod form the group `(root)`. Groups are listed in the order their files are laid over each
  other: the **root group (main files) first**, then the others (sorted group names; nested: menu folder order). The
  files of a later group replace those of an earlier one at the same path. (Before the port, a root whose name did
  not start with `(` could be merged last, over the options.)
- **Option**: `id` (8 hex digits, a hash of the group key and the option's folder name: stable when the archive is
  updated or renamed; unchanged for mods without nested menus), `name` (from `modinfo.ini`), `folder`, its files,
  and a `mode`:
  - `base`: always worn, not a choice (the root group's main files: its first plain option whose name does not say
    "addon" / "add-on");
  - `pick`: one option of the group at a time, in a **choice** group: its author asks for one ("2! Skin": a name
    numbered with `!`, or a description saying select / choose / pick (only) one, as ZZtaii's Slotsloader Manager
    reads it), or every option of the group ships one same file (two of them on would hide each other);
  - `toggle`: on or off: another option of the root group ("hide ponytail"), an option or a group whose name or
    description says "optional", a lone option, every option of a group that is not a choice.
- **Exactly one** (no None): a choice group its author asks one option of, unless its name or description says
  "optional", and unless its options share a file with options of another group (Mai mini dress: "4! NSFW",
  "4! SFW" and "4! Skimpy" all ship the body mesh, "3! Dress" and "4! SFW" the dress texture: picking in one
  group turns the other's option off, a required group would put it back over the new choice). Listed in each
  slot's `required` (with the groups that have no base copy, see Storage).
- An option is **a set of files: (original path -> stored copy)**. Each file has a **kind**: `mesh`, `material`
  (`.mdf2`), `texture`, `chain` (all four swapped on the model at runtime), `colour` (colour files, for the Color
  Mod) or `other` (user files, cloth, joint constraints `.jcns` / `_jcs.user`, scenes: only at the next launch;
  the loader logs `runtime options: <option> ships joint constraints (...)`). Per option, `apply` sums it up: `live`,
  `colour` or `relaunch`.
- Options of different groups can ship the same file (Mai: the NSFW, SFW and Skimpy groups all ship the body mesh).
  When an option is chosen, the menu turns off the chosen options of other groups that come **later** in the
  layering and ship one of its files (they would hide it). Earlier ones stay: the new option wins the shared file and
  their other files still apply, as in the variants the loader builds (Mai "Regular - Barefoot" = an SFW option for
  its dress texture + a Skimpy option for the body mesh). (Lot B narrowed the first rule, "any option sharing a
  file", which turned off the SFW option's texture and left a set no variant has.)
- **Defaults** of an outfit = the options its combination is made of, minus those whose every file a later option
  replaces (they are not worn).

### Outfits with add-ons (not modular)

A complete option and the options of its mod made for it (Fluffy `AddonFor` naming it, the same bundle, the same
archive touching its costume folder; a complete option whose `AddonFor` names another complete option, ported from
the fork 1.9.8; add-ons whose `AddonFor` names nothing of their mod beside a single complete option, fork 1.9.9,
with a WARN) are also one mod:

- `RuntimeOptions=off`: as before, the outfit plus one outfit per add-on (and one with all of them when they replace
  no same file; the outfit alone past three add-ons). "Leon Requiem On Luke C1": Leon as Luke, + More muscles,
  + Leon Hair, + Trimmed Hair (More muscles was an outfit of its own without the main files before the port).
- `on`: **one outfit**, the outfit alone (its own id: its slot as before), every add-on an option, any number of them:
  the outfit is `base`, each add-on a `toggle` in the group "Add-ons", add-ons that replace a same file one choice
  group (`pick`, with None) named after the word their names share ("Hair": Leon Hair / Trimmed Hair). The former
  add-on outfits are its `variants` (saved choices of their slots move to it, `former`), the complete add-on's
  own former outfit too. `variants`: the outfits of `off`, with the options stored.
- Add-ons the user put with the outfit in one sub-folder (`Ken\Ken SFV\` holding KenSFV and Ken SFV Hair V2; the
  separate add-on archives of Ingrid's Fly In A Bucket): `off` makes the outfit with them only, never alone. `on`:
  the same one outfit with its add-ons as options, worn by default (`defaults`): the combination `off` makes (the
  one with the most add-ons of the sub-folder), whose id it keeps (its slot as before); the outfit alone and the
  other combinations are its `variants`. Option ids as above (the add-on's folder). Its name (`outfits.json`, the
  manifest's `label`, the menu's title) is the outfit's alone ("Ingrid - Fly In A Bucket - Drive Tech"; before,
  "... + Ingrid - True... meme book + Ingrid - Maxwell the cat (monad)"); the variants keep the names `off` gives.
- Not when an add-on brings files for another costume folder than the outfit's (the runtime swaps the slot's own
  folders only; Cammy's Belly_cammy02 + BellyHair01_Cammy, hair made for `001/02/`): `off` rules.

An **add-on mod** (a modular mod none of whose options has a body mesh: "Mai face accessories", whose options
replace the shared head `000/00/` or Outfit 2's head) makes no outfit: with `on` / `variants` its options go on the
outfits whose files they replace (next section); with `off` it is not used (WARN).

### Add-ons on other outfits (20/10, loader `2026-10-20-rto-addons`)

A separate add-on that makes no outfit of its own becomes runtime options of the outfits whose files it replaces
(`attach_addons` in the loader): a modular mod none of whose combinations is a complete outfit ("Mai face accessories
C2 and swap test"), or a lone add-on no outfit of its own mod completes (A.K.I No Sleeves: a material file of
Outfit 1; Cammy C4 pale skin: textures of Outfit 4). With `off` nothing changes (such add-ons are not used). A
retouch that made an outfit of its own (C. Viper C2 Glasses Removed, "applied to the original outfit it modifies") is
an option too since the retouch rule (next section).

Where each group of the add-on goes (a group = a series of its menu: `!C2 Mai face accessories`):

- **Shared files only** (files of the fighter's `000/` that every game costume of the fighter loads, or that no
  costume scene names: the props of the option prefabs every costume scene names): an option of **every outfit of
  the fighter**, ours and the game's. Mai's "3) addon - remove wrinkles" (`000/00/..._facialblend.tex`, loaded by all
  four game costumes); Ingrid's book (`000/32`, `000/34`) and cat (`000/30`) when they are not in a sub-folder with
  an outfit.
- Else, a **mesh or material file a mod's outfit ships too**: an option of those outfits. The "C1 swap" series
  (glasses / piercings on the head `000/00/` that "Mini Dress Mai C1 Swap" ships) goes on the mini dress. The same
  texture paths alone do not count: that is a re-skin of the game's outfit (Cammy C4 pale skin, next to outfits made
  for Cammy's Outfit 4 slot that ship their own textures at those paths).
- Else, the **game outfits** that load its files (a costume folder `NNN/`: the game outfit of that folder; a `000/`
  file: the costumes whose scene or material files name it, from the vanilla index): options of that game outfit.
  The "C2" series (`002/00/`) goes on Mai's Outfit 2.
- An add-on put with an outfit in one sub-folder is still that outfit's option only (the folder wins, as before); a
  copy of an add-on an outfit already has (same paths, same sizes) is not put on it twice.

The options: **none is worn by default** (the outfit stays as installed; the slot's own files, or the game's, are
the way back: no base copy). A series with a "main files" option (its name says "main file") is one choice with None:
each other option **with** the main files (the main files alone are not offered) (what Fluffy installs: "1) Main files" + one of
"2) Glasses + piercings / Glasses only / Piercings only"); in another series, options that replace a same file are one
choice (named after the word their names share), the others On / Off. An option with colour or other files only is
not one (nothing to swap on a model: "CVS Shin Akuma Inspired Color"). The menu shows names without the prefixes
mods use for Fluffy's sort order ("!C2 Mai face accessories" -> "C2 Mai face accessories", "3) addon - remove mais
facial wrinkles" -> "Remove mais facial wrinkles"); the manifest keeps the mod's names.

**Fit** (`addon_fit`, per slot and per game outfit): an option is stored for an outfit only if each of its files fits
what that outfit wears there; else the log says `runtime options: <option> does not fit this outfit (<file>: why):
not offered here`.

- a mesh: the outfit wears a mesh at that path; the add-on's mesh has every joint of it (the model keeps its chains;
  blend shapes, `BS_<mesh>.<joint>_...` in the same name table, do not count: no chain moves them); every material of
  the add-on's mesh is in the part's material file (the option's own, else the one the outfit loads for that part:
  a slot's own copy when it relocated it, C.Viper lace lingerie's head material file);
- a material file: every material of the part's mesh (the option's, else the outfit's) is in it, and every texture it
  names exists (the option's own textures count);
- a texture: the outfit loads a texture at that path (any size: "remove wrinkles" is an 8x8 over a 512x256 mask);
- a chain: the outfit has one at that path.

### Retouches (20/10, loader `2026-10-20-rto-retouch`)

With `on` (not `variants`, which keeps the outfits of `off`), a **retouch** makes no outfit of its own: a mod (its
whole folder or archive) whose every file is a mesh or a texture of costumes (streaming twins included), **no
material file**, no chain, colour or other file, that made an outfit (one with a body mesh, or a lone option applied
to the outfit it modifies). It keeps that outfit's look: it is an option of every outfit that wears the files it
replaces, where it fits. A mod that ships its own `.mdf2` stays an outfit. Why no chain: a chain swapped on a game
outfit has no way back that keeps the chain safe (part units exist for a slot's base copies only), so a retouch with
a chain stays an outfit. Colour and other files cannot be swapped on a live model (they would be lost).

- **Where**: the game outfits that load its files (a file of their folder, a `000/` file they load), and the mods'
  outfits made from those costumes, except where that outfit ships one of the option's files itself (it replaces the
  game's files: Bayonetta C2 ships its own `002/00` head, so "Glasses Removed" is not offered there).
- **One series per fighter**: retouches of different mods that replace a same file are one choice named after the
  part ("Body": part `01`, "Head": `00`, else "Part NN"), None = the outfit's own file; the others On / Off each,
  named as their outfit was. A modular retouch keeps its own groups (add-on rules). Option ids: `retouch/<option
  folder>` (stable when another retouch is added).
- **A file the outfit does not load is left out** of that outfit's option, not a reason to refuse it: no material file
  of the outfit names it (C. Viper Barefoot C1 ships four `access*` textures Outfit 1's material file does not name).
- **Kept as an outfit** when it does not fit the game outfit it was made from (checked while the mods are read,
  against the game's files): Ken "VEGETA MAJIN" (its hair lacks 95 joints of the game's hair) is still an outfit.
- An add-on mod that never made an outfit ("Mai face accessories": no body mesh) keeps the add-on rules above.
- **Former outfits**: the game outfit's `former` lists each retouch's slot of the launch before (its costume number and
  folder, its option); a Battle Settings choice of it moves to the game outfit with that option chosen (see Saved
  choices). The other outfits keep their numbers (registry); only their names ("Outfit IV") follow their rank.

Offline (Wael's mods): C. Viper Barefoot C1, coat no mesh C1,
Coatless C1 (Outfit 1's body; Barefoot also 3 body textures) -> game Outfit 1, one "Body" choice; C2 Glasses
Removed (Outfit 2's head mesh) -> game Outfit 2, C.Viper Lace lingerie C2 (its own head material file, the game's
mesh), Battlesuit C2.1, Trench Coat (On / Off each); Cammy "Escape from Shadaloo" (Outfit 2's body mesh) -> game
Outfit 2 (B Style Bunny, Camo Cammy and Belly ship their own body); Akuma "BARECHEST CLASSIC" (Outfit 2's body
mesh) -> game Outfit 2. Six outfits fewer (76 -> 70).

### Merges (20/10, loader `2026-10-20-rto-merge`)

With `on` only (`off` and `variants` build what they built). Every rule is decided from the files: their bytes, the
meshes' name tables (materials, joints) and faces, the costume folders. Names only label the options. Each build logs
a short **merge audit** (`merge audit (RuntimeOptions=on): outfits that look related`): per fighter, what was made one,
and the outfits that look related but were not (same archive, an identical mesh, a mesh made from a game outfit's)
with the reason.

**On / Off options of a same file are one choice** (every outfit with options, `variants` too): two toggles that
replace a same file, in one group or in several (of the mod or of add-ons put on the outfit), become one pick group
with None, placed where the later of them was, named after the word their names share, else the part ("Body") or the
texture ("Jewels"); their ids do not change. Swimsuit Viper C1: "hide accessories (bracelets and leg chain)" + "hide
leg chain" both ship `jewels_atos.tex` (both on, the second one's file brought the bracelets back): "Chain" (None /
hide accessories / hide leg chain); "hide earings only / hide earings and necklace / hide necklace": "Necklace". The
plugin is defensive (a manifest of an older loader): a toggle turned on turns off every other chosen toggle that ships
one of its targets, and a set holding two picks of one group keeps the last one (both logged, `rto: menu: ...`).

**Versions: several outfits made one** (`merge_versions`). Outfits of one fighter that are
- versions of one archive: the complete options of one folder or archive (Ryu Aloha Bundle: C1 / C2 / C3 for
  Outfits 1 / 2 / 3; Lily Jun Kazama Cosplay: No Shawl / Jun's Outfit; a mod with a costume per game outfit), or
- outfits of several archives whose every mesh is the same bytes for the same game outfit (Sagat's Tees: one body
  mesh, other textures and colours; Viper's Lace / Battlesuit / Trench Coat ship other meshes: not merged)

become **one outfit** with a required choice ("Version", or the word their names end with: "Tee"). The **base** is
the first of them (archive order; the first one the Slotsloader Manager leaves on): it keeps its slot, number and
colours; the others are options of it and their slots move to it with their version picked (`former`). Each other
version is moved onto the base's folder (`esf001/002/01/...` -> `esf001/001/01/...`, its material files' texture
references follow, `RtoOption::origin`); a version made for another game outfit **carries what that outfit wears at
the parts it does not ship** (C2 on the C1 base: Outfit 2's head and part 02, mesh and material file, at the paths the
base loads; not their chains: the base's chains stay). A file the same as what the base wears there is left out (the
identical body mesh of the Aloha versions, the Tees' mesh and material file), physics files (`other`) too. The base's
own pick holds the base's files wherever a version ships another one (going back to it; worn by default, so nothing
is swapped). A version is merged **only where every file fits the base** (`version_fit`, the add-on fit checks against
the base's files and its game outfit's); else it stays an outfit (Aloha C3: its body chain, the C1 outfit has no chain
there; AoD replace Ingrid: a texture its material names is missing). A version whose files are all the same as the
base's is merged without an option; one that differs only in colours stays an outfit (colours are not swapped live).
Their add-ons are made with it (the same add-on of several versions once). The outfit's name: what the versions' names
share ("Aloha Ryu"), else the archive's name without its download numbers ("Lily Jun Kazama Cosplay"), else the
base's; option labels: the words that tell their names apart ("C1" / "C2"; "Muay Thai" / "Tiger Photo" / "Tiger" from
the archives' names when two names are alike). Not merged: Fluffy installs, modular mods, retouches, outfits put with
their add-ons in a sub-folder.

**Colour entries of an option's materials** (`on` only, `add_slot_files`, `cmd_add_missing_clusters` in
`patch.cpp`). Colour files are not swapped live, so the slot's own ones colour every option: a material no entry
names keeps its material file's default (Aloha C2 on the C1 slot: Outfit 2's head brings `esf_Eyebrow` and
`esf_Beard`, which neither the slot's `cmd_000` (game Outfit 1's) nor C1's `cmd_001..010` name: uncoloured). A
material of an option's material file that **no material file of the slot's scene has** gets its entries (the
cluster and its tree, per part type) appended to each of the slot's colour files `cmd_NNN`, taken from the option's
`cmd_NNN`, else its `cmd_000`, else the game outfit's it was made for (the folder its moved files come from), same
number then `cmd_000`. Existing entries are never changed, removed or moved (a new cluster goes at the end of its
part, a new part at the end of the parts: the Color Mod lays its colour trees over the live data by position and
name); a material the slot has is never added, so the slot looks as before. Each file is read back with the same
parser (entry count). Log: `runtime options: esf001/v05: colour files: +22 entries (esf_Beard, esf_Eyebrow) from C2,
in 11 files`; materials no source names are listed (Ingrid's book and cat props: none, their defaults). Offline on
Wael's mods only Aloha C2 needs it. Not done for game outfits' options (their colour files are the game's).

**Subsets: a game outfit with parts taken off** (`game_subset`). An outfit (a lone option of its folder or archive)
whose every mesh is the mesh a game outfit of the fighter wears at that part, either the same bytes or the same
materials and joints with some materials taken off: left out of the mesh, or every triangle of theirs folded to
nothing (two corners on one vertex; the other materials keep their triangles as they were), at least one part with
something taken off. Ryu "Outfit 1 No Cloak - Drive Tech" is the game's Outfit 1 body with `esf_Costume00`, `esf_Ring`
and `esf_Threads` folded; Sagat "Outfit 1 Shirtless" ships Outfit 1's head as it is and its body with the fur vest
folded; Ken "Outfit 2 Shirtless", Alex "Outfit 1 No Vest", Yasmine "Outfit 1 No Jacket" alike. It makes no outfit: its
files are moved to the paths that game outfit loads (Drive Tech's `005/01` -> Outfit 1's `001/01`), the ones the same
as the game's there left out, physics and colour files too (the game outfit's own stay: the subset mesh has all its
joints), and it is an option of that game outfit and of the mods' outfits that wear those files, where it fits (as a
retouch: one series per fighter, a choice with retouches of a same file, `former` moves its slot to the game outfit
with the option chosen). It is named as its outfit was. **Chains**: a subset that ships a changed chain stays an
outfit. A part unit (stop the chain, set the new chain, the mesh, restart) needs a way back that the loader checked as
a whole (a base copy of the part); a game outfit's options have no base copy (the game's files are the way back, and
the plugin goes back to them file by file), so a chain changed on a game outfit could be left bound to joints of the
other mesh. None of Wael's subsets ships a model chain (their `_chain.user` and joint constraints are physics files,
left out). A mesh made from a game outfit's with nothing taken off (an edited copy) stays an outfit (audit).

Offline (Wael's mods, `on`): 70 outfits -> 59. Ryu: Aloha Ryu (Version C1 / C2, add-ons No Gloves / No Headband),
Aloha Ryu C3 stays (its body chain), No Cloak -> an option of game Outfit 1; Sagat: one Tee outfit (Muay Thai / Tiger
Photo / Tiger), Shirtless -> game Outfit 1; Cammy: Zani Costume (Horn removed / Outfit01) and Mai Outfit 2 PRO
(PRO / PROmc: its own chain); Lily: Jun Kazama Cosplay (No Shawl / Jun's Outfit); Ken Outfit 2 Shirtless, Alex No
Vest, Yasmine No Jacket -> their game outfits. The scan takes about 12 s more (meshes read for the subset test, hashed
for identical meshes).

**Game outfits** in the manifest: a `slots[]` entry with `"game": true`, the game's costume number (Outfit 2 = 1) and
folder (`002`), name and label "Outfit 1".."Outfit 4" / "Drive Tech", mod key `<fighter>/game outfit 2` (its groups
are the add-ons' series), `defaults`, `base`, `required` and `former` empty. Options are stored under the game's
folder: `esf028/002/rto/<id>/00/esf028_002_00.mesh`, `esf028/002/rto/<id>/_/000/00/...`; nothing the game loads
changes. The plugin finds a model's outfit from the paths it loads, **one of our slots first**, then a game outfit
(`Manifest::slot_of_paths`: a slot of ours may keep a part in its game outfit's folder); menu, F hint and runtime are
those of a slot. Online, an opponent wearing that game outfit shows P1's options on this screen only (as a
replacement mod would).

**Shared files, checked in the game's files (20/10)**: every Ingrid costume scene (`esf032v00`, `v01`, `v02`, `v04`)
names the same option prefabs `Product/Event/FGM/esf/esf032/option/prefab/esf032_Option_01.pfb` (cat `000/30`),
`esf032_Option_02.pfb` (book `000/34` and the `000/32` joint constraints) and `esf032_Continue_00/01.pfb` (`000/31`,
`000/30` with its chain); no costume scene names `000/30-34` itself (the vanilla index lists them for no costume). So
the props are one set for every outfit of the fighter: a book or cat add-on made of them is an option of every Ingrid
outfit, ours and the game's (an outfit that relocated them, Fly In A Bucket, gets them at its own copies). Mai's
`000/00/` is not one set: its head mesh is loaded by Outfits 1, 3 and Drive Tech (Outfit 2 has `002/00/`), its
wrinkle masks by all four.

Offline (Wael's mods, `RuntimeOptions=on`): Mai's Outfit 1, 3 and Drive Tech get "remove wrinkles", Outfit 2 the
"C2" series (one choice: None / Glasses + piercings / Glasses only / Piercings only, each with the main files)
and "remove wrinkles", the mini dress the "C1 swap" series and "remove wrinkles"; A.K.I's Outfit 1 "No Sleeves";
Cammy's Outfit 4 the pale skin. Ingrid's book and cat put loose in `Ingrid\` (a test copy) go on every Ingrid outfit
where they fit; in Wael's sub-folders they stay options of the Fly In A Bucket outfits only.

Options of a part the slot does not have in its own folder (Leon as Luke wears the game's hair, `000/02/`; its hair
add-ons ship `001/02/` and a scene pointing there) target what the slot's scene loads for that part: the mesh and
material files the scene names for it, the chain of the same name in that folder. Before, they targeted the
original folder's path, which no model loads (the option did nothing).

### A mod alone in its sub-folder (20/10)

A mod (archive or folder mod, all its options counting as one) that is the only mod of its own sub-folder of the
character folder (`costume_mods/<Character>/<Sub-folder>/<that mod>`; the sub-folder is neither an archive nor a pak) is
**always its own outfit** with `RuntimeOptions=on`. It is left out of every rule above, on both sides: never a retouch,
a subset or a version of another outfit, never merged with another outfit (rules B, C, D), never an option of another
outfit's add-on placement, and no other mod's add-ons or retouches are placed on it. Its own internal options (a modular
mod) stay runtime options of that outfit. Several mods in one sub-folder keep the grouped-outfit rule. A lone mod that is
a pure add-on (no complete outfit) cannot be an outfit: it keeps the add-on rules, and the log says so. The log has one
line per mod kept apart (`<mod>: alone in its sub-folder <name>: its own outfit (no merge)`), repeated in the merge audit.
`RuntimeOptions=variants` and `off` are unchanged.

### NSFW outfits: `costume_mods_nsfw` (20/10)

A second root, `reframework/costume_mods_nsfw/<Character>/`, read like `costume_mods` (same layout, archives in the
same `costume_mods\.cache`, same `settings.ini`), after every other source, its units marked `nsfw`. The plugin hides
its outfits while "Display NSFW Outfits" is off (README, NSFW outfits). No mixing, every layout:

- An NSFW mod is merged (versions, identical meshes), grouped (one sub-folder) or given add-ons (`AddonFor`, bundle)
  with NSFW mods only; a mod alone in its sub-folder has its own key per root (`nsfw:<char>/<sub>`).
- An NSFW retouch or subset stays an outfit (a retouch is an option of the game's outfits, which are shown whatever
  the setting); the loader logs `... from costume_mods_nsfw: stays an outfit`.
- An NSFW add-on that makes no outfit (`attach_addons`) is an option of NSFW outfits only, never of a game outfit;
  the add-ons of `costume_mods` never go on an NSFW outfit. So no NSFW option is ever offered on, or worn by, an
  outfit that stays shown while NSFW outfits are hidden: the runtime options need no filter.
- Manifests: `outfits.json` and the registry (`slots[]`, `possession[]`) carry `"nsfw": true` on NSFW outfits;
  `costume_options.json` on their `slots[]` and `mods[]`, whose key is `<fighter folder>/nsfw/<name>` (never the
  choices of a mod of the same name in `costume_mods`). Written only when true: without `costume_mods_nsfw` every
  file is byte-identical to before. `variants.json` does not list them (the Slotsloader Manager names sources in
  `costume_mods` only).
- Numbers (`assign_slots`): a new NSFW outfit goes after every number of the fighter in use, its first one
  `kNsfwGap` (10) numbers further; a new outfit of `costume_mods` goes after the fighter's other outfits and before
  its NSFW ones (a free number before them, else after them, logged). Numbers in use are kept; a mod moved between
  the roots is numbered again (the registry's `nsfw` no longer matches).
- Fingerprint: the root's entries as `nsfw:<path>` (none without the folder).

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
  - a chain: not when a mesh of its part (same folder) has no base copy: the slot's own mesh stays, its own chain
    stays with it (Ken SFV Hair V2: the game's hair chain under the mod's hair; loader `2026-10-20-rto-grouped-addons`).
    A part whose mesh, material file and chain change together is a **part unit** instead (below).
  The loader logs each refusal (`runtime options: no base copy of ... (why)`).
- **Part units** (20/10, loader `2026-10-20-rto-units`): a model part that every option touching it ships whole
  (its mesh, its material files and, when it is the only mesh of its folder, the chain of that folder) gets its base
  copy as one unit: every file from one source (the mod's base files, else the game's), all or nothing, the per-file
  checks above replaced by checks of the whole. Ken SFV Hair V2 ships the hair's mesh, material file and chain; the
  per-file rules refused the game's hair mesh (it lacks the 22 joints of the add-on's chain) and the game's material
  file (the mod ships its own mesh), so "Off" changed nothing on the model. Checks of a unit:
  - its material files fit its mesh (`material_fit`: materials defined, textures present);
  - without a chain in the unit: the base mesh has every joint of the slot's mesh (the model's chain there stays);
  - with a chain: for every two (mesh, chain) the model may wear there (the base, each option shipping the part),
    the chain of one names no joint that only the mesh of the other has (`_end` joints by UTF-16 name, collision
    joints by name hash: murmur3, seed 0xFFFFFFFF, UTF-16LE), and each chain names at least one joint of its own
    mesh (else it is not read this way: refused). The runtime sets the new chain before the new mesh (see Runtime),
    so no joint that disappears with the old mesh is ever bound to the chain. Ken: the two hairs share 40 joints,
    the game's has 73 of its own (none named by the add-on's chain), the add-on's 22 (none named by the game's).
  The manifest marks every file of a unit (base copies and the options' copies) with `unit` (the target of its
  chain, else of its mesh) and its meshes with `joints` (logs). Logged: `runtime options: part <folder> as one unit
  (N files[, its chain]) from the game's files | the mod's base files, J joints`, or with a chain
  `... is not one unit (why): its files are checked one by one`. Ingrid's Fly In A Bucket props (book 000/32 + 34,
  cat 000/30: mesh + material each) are units too: "Off" gives back the game's props.
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
- File entries may also carry `unit` (the file belongs to a part unit, named by the target of its chain, else of
  its mesh: swapped together), `joints` (a unit's mesh: its joint count) and `own_joints` (a unit's base mesh: the
  joint count of the slot's own mesh at that target). Older plugins ignore them.
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
- **Part units** (mesh and/or material and the chain of the same GameObject all change; 20/10): the chain is
  stopped (`via.Component.set_Enabled(false)`, only if it was enabled), and on a later frame: `set_ChainAsset(new
  chain)` + `restart` on the mesh still worn, then `setMesh` / `set_Material`, then `restart` again (the chain set
  up on the mesh now worn); the next frame, if the part is drawable the chain runs again, else the old unit goes
  back the same way (chain first) and the chain runs again a frame later. Why this order: the 19/10 crash came from
  joints that went away with a mesh while a chain still moved them; a chain set on a mesh lacking some of its joints
  leaves them out (the game itself sets Mai's `esf028_001_30_Chain` on four prop meshes that each lack most of its
  joints), so the old chain is released while all its joints are there, and the new chain, set first, is never
  bound to a joint that disappears (the loader's check above). No frame ever has a running chain on a part made of
  two sources; between two frames a part wears a whole unit, so a model reset (or a fault) starts a stopped chain
  again at once; a stopped chain is started again after 2 s at most. A unit's chain never changes alone (step 4
  skips it), and a unit's mesh whose chain is not on the model is left as is (one log line).
- **Toggles Off cannot change**: an option worn by default none of whose files has a base copy (Off would show the
  same model) is not offered; a slot left without any row gets no menu and no F hint. Logged once at start:
  `rto: <fighter>/<folder>: "<option>" cannot be turned off live (no copy of the outfit without it): not in the menu`.
  With a manifest written before part units, Ken SFV Hair V2 and Ingrid's book / cat are hidden that way.
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
  (`p_Spin_costume`) is focused, the preview wears a slot with options and our menu is closed, the bottom input
  guide bar gets an extra entry `<ICON KeyF><ICON MouseL> Costume options` (a left click on the row's body, arrows excluded, opens the menu like F) (`<ICON BtnD>` after a pad input), like the Color
  Mod's `<ICON KeyF><ICON MouseL> Edit Color` on its Color row. The label is English only (the plugin has no locale
  system).
- **One writer of the bar** (the bar's `e_text_0_g_*`, else its first `e_text_0_d_*`). Who writes depends on the
  Color Mod (`rto_menu.cpp`, `guide_mode`; the log says `guide bar entry: ...` once per visit):
  - **Color Mod 0.22.4 or later** (the normal case): the plugin never writes the bar. It exports
    `int SF6Slots_GuideEntry(char* buf, int cap)` from `SF6_CostumeSlotsNative.dll`: the entry text (UTF-8, icon
    included, chosen by the plugin's device detection, which follows the Color Mod's rule) while it belongs in the
    bar, else 0 / empty (also when the plugin stopped publishing for more than 500 ms). It only copies a string the
    plugin's LateUpdateBehavior code keeps under a lock: no game call, any thread. The Color Mod resolves it with
    `GetModuleHandleW` + `GetProcAddress` (looked for again every 600 passes while absent), calls it on every
    screens pass (every 3 frames, `colormod/src/screens/colour_track.cpp`, `slots_entry_poll`) and, while Fighter
    Settings is up, appends `"      " + entry` (its own six-space separator) to the text it writes for the left
    bar, after its own entry. When the entry comes or goes it refreshes the bar at once instead of waiting for its
    15-frame refresh: the entry shows / leaves within about 4 frames of the row change. Each call also tells the
    plugin a guide owner is there; while calls come (within 60 of its frames) the plugin writes nothing.
    Without the plugin (or without that export) the Color Mod writes exactly what 0.22.3 wrote.
  - **No Color Mod loaded**: the plugin writes the bar itself (appends `"      " + entry`, takes out only its exact
    entry, never an identical write); the game is the only other writer. More than 12 writes in 120 frames = the
    text is left alone for that visit (never a fight).
  - **An older Color Mod** (loaded, never calls the export): the plugin never writes the bar. The key icon goes at
    the end of the costume row's value instead (`  <ICON KeyF>` / `  <ICON BtnD>` while the outfit has options),
    the kit B behaviour that ran 100 Costume / Color round trips without a crash; same 12-in-120 guard.
- **Why one writer (crash of 20/10)**: the Color Mod writes its stored copy of that text from its post-hook of
  `app.UIPartsInputGuideList.OnLateUpdate` (a game UI job thread) on every frame the text differs, and refreshed
  the copy only every 15 frames. The first build (51dde84) also wrote the text from the plugin, on any frame: the
  Color Mod took it back from the job thread, the plugin wrote it again, up to 14 pairs of `set_Message` on one text
  per row change. Moving S / Z between the Costume and Color rows every 0.4 s (Mai Outfit I) crashed the game 2 / 2
  within 40 round trips, same RIP (`StreetFighter6.exe+0x4654383`, a managed object with a zeroed header, i.e.
  freed, read during a nested coroutine walk of the LateUpdateBehavior entry); the build without guide writes ran
  100 round trips. 1f503bf then wrote only on the frame phase where a write stayed (learned from the Color Mod's
  refresh): no crash in 100 round trips, but after fast row changes it gave up ("16 writes in a row taken back")
  and the entry stayed gone. Replaced by the single writer above.
- **Key**: `F` (or the pad's A / Cross) with the costume row focused opens the menu. The Color Mod's keys are
  elsewhere: F on the Color row (its editor), X (Drive colours), C / V inside its editor; its editor and this menu
  never open together (both need the option window closed).
- **Title**: the outfit's name in capitals and the side (`MINI DRESS MAI C1 SWAP  -  P1`); a name longer than 40
  characters (UTF-8) is cut and ends with `...`, so the side stays readable. The costume row shows the game's
  "Outfit VII"; the outfit's name is also at the right of the guide bar ("Outfit name on Fighter Settings" below).
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
  callback of the frame) and shows a block again on any frame it finds hidden, so nothing blinks. Checked against
  the guide bar crash (20/10): not the same case. REFramework runs the pre callbacks, the LateUpdateBehavior entry
  and the post callbacks on one thread, one after the other, so the Color Mod's hide (pre, core callback) and our
  paint (post) never overlap; no job-thread hook writes `c_00` / `c_02` (the Color Mod's camera writes them only
  while its editor is open, never with our menu), and the only back and forth is one hide and one show per block
  every 30 frames (texts written only when they differ).

### Outfit name on Fighter Settings: `plugin/src/fs_name.cpp` (no new core capability)

- **What**: on Fighter Settings, the name of the outfit on the preview, right-aligned at the right end of the bottom
  input guide bar's line (the bar's left margin mirrored). One of our outfits: its label (`outfits.json`, the mod's
  name, e.g. "Aloha Ryu"), read from the screen's `app.UIPartsMatchingFighterSetting` (`FighterId`,
  `GetCostumeId()`); a game outfit (or that part not found): the costume row's own text, i.e. the game's name. Cut at
  40 characters ("..."). Works with `RuntimeOptions` off too (its own module, not the options menu).
- **Shown** while Fighter Settings is on screen and no option window is open; **hidden** under our options menu and
  the Color Mod's colour editor (their own guide bar and camera hints are there), off that screen, and when it would
  come within 48 px of the bar's own text (its entries change with the focused row).
- **How**: never the bar's text (one writer, see above). A copy of the bar's item template `e_text_0` (the element
  the game duplicates for each entry: same font, size, colour) named `e_text_slots_outfit`, added under
  `c_guideBtn`'s parent with the core's `ui_clone_control` (core 0.8.2+). R19: the copy is made inside a post-hook of
  `app.UIPartsInputGuideList.OnLateUpdate`, that list only (its template and parent found from `this`), hidden; the
  frame only asks. One copy per bar (it lives and dies with the bar's tree), no new copy after 3 refusals in a
  session. Everything else from LateUpdateBehavior, by the plugin only, through the core (R1, R35: found again by
  name under its live parent): text (tags shown as typed), ControlPoint = the bar text's row at its right point,
  AutoRegionFit, scale, position, visibility. Logs: `fs name: ...` in `native_log.txt` (source of the name, geometry
  once per change, shown / hidden and why).

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
`costume_choices.json` are written, `former_done` notes the manifest. A **game outfit's** `former` (a retouch's slot):
the choice goes to that game outfit, no slot of ours: the save's `MatchingFighterCostume` gets the game's costume
number (and the colour, when the carrier has it) the first time the save is reached (`safety_net`); until then
`state.json` keeps the former number, so a session that ends first does it again. Layout `variants` never migrates (the variant
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
- **Fluffy installs**: a mod read from Fluffy's archive loads only the options checked in Fluffy, so only those
  are stored. Lot B: load the unchecked options too, as options only (no outfit), with the checked ones as
  defaults (`defaults_from: "fluffy"`).
- Separate add-ons: done (see [Add-ons on other outfits](#add-ons-on-other-outfits-2010-loader-2026-10-20-rto-addons)),
  and retouches (an add-on applied to the game outfit it modifies included). Not done: a retouch's former outfit
  migrates for a fighter that still has a slot of ours (the plugin reaches the save entries of those only), a subset's
  too; the Slotsloader Manager does not see retouches and subsets (no outfit, no `variants.json` row).
- `other` files (cloth, physics user files) cannot be swapped at runtime: their options apply at the next launch
  only (or not at all with `on`, the menu says so).
- Slotsloader Manager compatibility (`variants.json` / `variant_choices.json`): done, see
  [MANAGER_CONTRACT.md](MANAGER_CONTRACT.md) (with `on`, the Manager's variant choice sets the outfit's defaults).

## Phases

- **A (done, setting off by default)**: options stored per slot, base copies, material rebinding,
  `costume_options.json` (options, defaults, presets, former slots), `RuntimeOptions=off|on|variants`.
- **B (built, to test in game)**: plugin runtime (load, preload, apply per model, rebuild moments), the
  texture-sharing check (offline: per model; the fallback is not built), choices file, Battle Settings menu,
  migration of `state.json`. Left for later: Fluffy unchecked options.
- **C**: tan and colour options through the Color Mod; presets and "Save as"; default `RuntimeOptions=on`.
