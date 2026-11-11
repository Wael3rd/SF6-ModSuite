# SF6 Stage Slots

Street Fighter 6 stage mods replace a stage: install a Training Room mod and the original Training Room is
gone until you uninstall it, and two mods for the same stage cannot live together. Stage Slots keeps the
original and every mod side by side: on the stage select screen, **UP / DOWN** cycles the focused stage
through its original look and each mod installed for it. The name, the preview image and the stage's
thumbnail in the list follow, the VS screen shows the choice, and the battle loads it.

Nothing is installed over the game's files: each mod's files are copied into a patch pak under names of
their own, and served in place of the original files only while that mod is selected.

## Install and use

Stage Slots is part of [SF6 Slots Loader](../README.md): it ships in the same `amd_ags_x64.dll` as the
costume slots (`build\amd_ags_x64.dll`, see *How it works*), plus the native plugin. It is in the releases since
1.9.0 as an **experimental** feature: the release archive puts every file below in place. You need REFramework
in `dinput8.dll` (without it the stage pak is built but nothing can be selected).

| File | Where |
|---|---|
| `build/amd_ags_x64.dll` (costume slots + stage slots) | game folder |
| `build/SF6_CostumeSlotsNative.dll` (native plugin) | `reframework/plugins/` |
| `stages/data/loader/stage_paths.txt` | `reframework/data/SF6_StageSlots_Data/loader/` |

Put stage mods **as downloaded** in `reframework/stage_mods/`: the `.zip`, `.7z` or `.rar` file itself, a
folder with a `natives` tree, or a `.pak`. Launch the game.

- Each option of a mod becomes one variant of the stage it changes. A pack covering many stages (Stage
  Lighting Overhaul: 24 options) gives one variant per stage and option.
- On the stage select screen, a stage that has variants shows its name between the UP / DOWN symbols of
  the device in use (keys on a keyboard, directions on a pad), the way the game's BGM selector frames its
  value. UP / DOWN changes the variant; the choice is kept per stage
  (`reframework/data/SF6_StageSlots_Data/state.json`).
- Names come from the mod's `modinfo.ini` (`name=`). A variant named like the stage itself (a lighting pack
  names its options "Bather's Beach") shows the pack name (`nameAsBundle=`) instead.
- Previews are made from the mod's screenshot (`screenshot=` in `modinfo.ini`, or the first image of the
  folder): a 4:1 band at 60% of the height, 2048x512, BC1 like the game's own. A mod without an image keeps
  the stage's original preview.
- The stage's thumbnail in the list at the bottom of the screen is the centre of that band (1:2, 128x256, as the
  game's own thumbnails). Every stage's thumbnail shows its selected variant, focused or not; the original look
  gives the game's thumbnail back. It needs SF6 Native Core 0.8.1 or newer: without it, the list keeps the game's
  thumbnails.
- Variants can change from one battle to the next without restarting the game.
- UP / DOWN works on the stage select screen of Fighting Ground > Versus, and only changes the look of the focused
  entry. A tile (see below) is an entry of its own after the game's stages: an arrow in the corner beside the last cell
  says there are more, RIGHT at that cell moves what the cells show, and the mirrored arrow beside the first cell says
  there are more on that side. Tiles over one stage whose names start with the same word are one entry with several
  looks, cycled with UP / DOWN. Random (R) draws among every entry, the extra stages included, then among that entry's
  looks; the cursor goes there and that look stays chosen. After a fight, the list opens on the stage fought.
- To remove a mod, delete its archive or folder: the stage pak is rebuilt at the next launch. Archives are
  unpacked once into `stage_mods\.cache` (as much disk space as their unpacked size, 120 MB for the lighting
  pack); that folder can be deleted at any time.
- Without stage mods, no stage is changed and nothing is hooked.

### Tiles and variations (Battle Settings, Training and Replays), in test

Since 1.20.0 stage mods also show in the stage menus of **Battle Settings > Other > Stage Settings**,
**Training pause > Environment Settings > Change Stage** and **Replays > Settings > Stage Settings**. The classic
stage select screen (Fighting Ground > Versus) keeps its UP / DOWN, unchanged.

- The loader guesses what each mod is from the files it replaces. A **tile** is another stage built over a vanilla
  one (it replaces the stage's root prop scene, its arena mesh, ships collision, or adds 20 or more new files: the
  Suzaku Castle or Medieval Town ports over the Training Room, NULSPACE, Aokigahara). A **variation** only tweaks
  its stage (lighting, colours, a few textures). The guess and its reason are in `registry.json` (`kind`,
  `kind_reason`).
- A tile gets a cell of its own after the game's stages, with its picture and name.
- A variation belongs to its stage's cell. **X** (pad) or **R** (keyboard; the game's own key for it) previews the
  next one on the focused cell; the guide bar shows "Stage Variation" only when that stage has some (this hint needs the
  Native Core 0.8.2 or newer). **Confirm** chooses the shown variation; leaving the cell without confirming puts
  the chosen one back.
- Random Stage Settings: each cell has its own tick. When the game's random lands on a stage that has ticked cells,
  one of them is loaded.
- The VS screen shows the stage that is really loaded.
- Online is safe: only the original stage ids travel (the Training Room for a tile over it). The mod is visible on
  your PC only, so the other player sees the original stage.
- Each menu keeps its own choice: Battle Settings', Training's and Replays' are separate (Battle Settings' is used for
  online battles, Training's when Training loads its stage, Replays' for replays, the stage select screen's UP / DOWN
  for the battles it starts). Replay files only ever hold the vanilla stage id.
- Pictures: the mod's own stage-select picture and thumbnail when it ships them, else cut from its screenshot,
  else the stage's own picture with the mod's name.
- `reframework\stage_mods\stage_slots.ini` overrides the guess and the names, one section per mod (the mod's key, its
  name, its source, or its archive / folder name, with or without its extension; a later matching section wins):
  `kind=tile` or `kind=variation`, `name=the name shown in the menus`. Lines starting with `;` or `#` are comments;
  any other value for `kind` is ignored and logged. The file is written as a commented template the first time; changes
  apply at the next start without rebuilding anything. Example:

  ```ini
  [SIRN Training Room]
  kind=variation

  [Suzaku Castle Over Training Room]
  name=Suzaku Castle
  ```

## Title screen packs

Title screen mods (Fluffy-style packs: a `modinfo.ini` and a `natives` tree with the title screen's movies, often
with files that skip the boot logos and the health warning) go **as downloaded** in `reframework/title_mods/`: the
`.zip`, `.7z` or `.rar` file itself, a folder, or a `.pak` (alone, or inside the archive or folder, as Fluffy installs it).
Archives are unpacked once into `title_mods\.cache`. A pak keeps only the hashes of its paths: its files are named from the
twelve paths every title pack has (`title_slots::known_paths()`) and from the packs shipped as files; a file named by neither
is a startup file (the log counts them). Each pack gets a key (`t` + 12 hex digits) from its archive or folder name, so a choice survives a new
download of the same file; names and authors come from `modinfo.ini`.

- Three settings, in `reframework/data/SF6_CostumeSlots_data/settings.json`: `"title_pack"` (`""` or missing: the
  game's own title; a pack's key; `"random"`: a draw among the backgrounds in `"title_random_pool"`), `"skip_intro"`
  (`true` / `false`, missing: `false`) and `"title_random_pool"` (the backgrounds Random draws from; at least one
  stays ticked). A key that names no pack gives the game's own title (logged). They are set in Options > Interface >
  Title Screen Selection (needs Native Core 0.9.9) and Interface > Other Settings (`skip_intro`).
- The chosen pack's title movies (`natives/stm/streaming/product/gui/gm/ui10000/part/title_*`) are written into the
  stage pak at their real paths. With `skip_intro`, the pack's other files are written too (boot logos, health
  warning); with the game's own title, or a pack that has none, those of the first pack that has some.
- The movies of every pack are in the stage pak, which is rebuilt at the first launch of a release with new title
  files, and when the packs change. The choice applies at the **next return to the title screen**, no relaunch.
- Random is drawn **without repeats**: a shuffled list, every background shows once before any comes again, never
  the same twice in a row. The list is kept across launches in `reframework/data/SF6_TitleSlots_Data/shuffle.json`.
  A new draw is made at the launch, at each return to the title screen, and while you stay on the title screen (each
  loop cycle: the start, the loop and the transition after a key always come from the same pack).
- The pictures of the title page and editor (preview, thumb and band textures, and the check box textures) are in the
  stage pak too. A pack's picture is its modinfo screenshot.
- `reframework/data/SF6_TitleSlots_Data/registry.json` lists the packs (key, name, author, source, by name) and what
  this launch applied, for the plugin. Without a `title_mods` folder nothing is done.

## UI packs

Interface mods made for Fluffy Mod Manager (menu art, portraits, loading screens: a `modinfo.ini` and a `natives` tree
per option) go **as downloaded** in `reframework/ui_mods/`: the `.zip`, `.7z` or `.rar` file itself, or a folder.
Archives are unpacked once into `ui_mods\.cache` (a `.pak` is not read, the log says so). Every option of every mod can
then be ticked in the game, without Fluffy.

- The options form the tree Fluffy shows. Every folder with a `modinfo.ini` or a `natives` tree is a node.
  `DummyMod=True` is a title (no box). `AddonFor=<name>` hangs a node under the node of that name, also across
  archives: an add-on downloaded on its own joins its mod. A node with no `AddonFor`, or one that names no node, is a
  root. A root and everything under it is a **pack**: one row in the game.
- Under one parent, the options that replace a file in common are a **one-of group** (pick one, or none). The others
  are check boxes. Separate mods that are alternatives of each other (the 9 Loading Animations archives) are gathered
  into one pack, where they are a one-of group. That pack is named from the words their names share ("Loading").
  Alternatives replace nearly the same files: at least half of the files the two replace together are shared by
  both. Mods that only share some files stay separate packs (Round Announcement UI ships one loading screen among
  many files; Comic Art and Battlehud Portraits both have battle HUD portraits).
- Order: packs by name. Under each parent, the options by `MenuPriority` (highest first), then by name; then the sub
  groups by name.
- Several ticked options replace the same file: when they are in two packs, the later pack by name wins it (the log
  names each pair of packs that share files, and how many). In one pack, the option deepest in the tree wins it, then
  the later in that order. A preset (FG Artwork's *Default Set*, whose banners other options also have) is a box like
  the others. A deeper option ticked with it wins its own banner.
- Keys: a pack is `u` + 12 hex digits, a node `n` + 12 hex digits, both from the archive or folder name and the
  node's folder inside it. A choice survives a new download of the same file under the same name.
- Choices: `"ui_choices"` in `reframework/data/SF6_CostumeSlots_data/settings.json`,
  `{ "<pack key>": ["<ticked node id>", ...] }`. A pack that is missing has nothing ticked: the game's own files. Ids that
  name no option are ignored (logged).
- In the stage pak:
  - every option's files, at private paths (`natives/stm/_uislots/<pack>/<node>/...`). Identical data is stored once.
  - the ticked options' files at their **real paths** too. The game loads some interface files once, at boot, and keeps
    them (the character select's portraits): written there, they show the choice from the launch on.
  - the game's own copy of each real path written that way (`_uislots/game/...`, from `re_chunk_000.pak` and its sub
    paks).
  - each node's picture, `_uislots/<pack>/<node>/preview.tex` (its modinfo screenshot at 1920x1080 BC1, 1 MB), and the
    pack's, `_uislots/<pack>/preview.tex`. Past 256 MB of pictures, the others are left out.
- Pictures (UI packs, title packs) are converted once: the result is kept in `ui_mods\.cache\_pictures` (title packs:
  `title_mods\.cache\_pictures`), named from the screenshot's path, size and last write time. A changed screenshot is
  converted again; files no pass asked for are removed after a build. The log gives the time of each part of a rebuild.
- **A choice changed in the game is served at once** (`SF6UISlots_Apply`). The redirection serves each file whose
  option changed from its private copy. It serves the game's own copy when nothing ticked replaces it any more. The new
  choice shows at the next load of the file (the Fighting Ground banners: the next time the menu opens). A file the
  game keeps in memory stays as it is until the plugin swaps it on screen (`SF6UISlots_Overrides` lists those
  textures), or until the next launch. The choices are written at the real paths, so **a choice changed since the last
  launch rebuilds the stage pak** at the next start (the log says so).
- **Per fighter.** A file under `character_image/<digits>/` belongs to that fighter (the folder decides, not the file
  name). A choice id may be `"<node id>@<fid>"` (e.g. `n13a08299e134@013`): that option for the fighter only. It is kept
  when the node is an option with files under that folder; it is dropped for an unknown fid, for a title, and when the
  whole id of the node is ticked too. The stage pak also holds the game's own copy of every fighter texture of any
  option (`_uislots/game/...`), ticked or not.
- `reframework/data/SF6_UISlots_Data/registry.json` lists the packs and their nodes (depth first, in display order:
  id, parent, name, description, title or option, one-of or box, group, files, picture, depth, source) and the options
  this launch ticked (`"applied"`). A node with files under fighter folders also has `"fighters": { "013": ["<real
  texture resource>", ...] }` (a fid with no texture: `[]`) and `"fighters_only"` (all its files are under fighters).
  The top level has `"game_textures": { "<real resource>": "<resource of the game's own copy>" }` for those textures.
  Without a `ui_mods` folder nothing is done: the stage pak is not rebuilt, and the
  registry goes.

## Limits

- **The choice is local, and it applies to every load of that stage**: training, arcade, replays, and online
  too (a random stage in ranked included), since the redirection is native and does not look at the mode. The
  game only knows the vanilla stage id, so another player sees their own version of the stage. To get the
  original back, select it again on the stage select screen. **Online play has not been tested.**
- Only the stage select screen of Fighting Ground > Versus was tested. The other places where a stage is
  chosen (rooms, Battle Hub, matchmaking settings) were not checked and probably show no UP / DOWN hint.
- Files of a mod outside the stage's own folder (shared props: the bridge and World Tour area of Aokigahara,
  the lamps of the lighting pack) are replaced wherever the game loads them while the variant is selected. In
  theory; not seen.
- The UP / DOWN hints were tested with a keyboard only.
- The preview is a 4:1 band cut from the mod's screenshot, more zoomed in than the game's own pictures.
- A mod's files under `natives/stm/product/gui/` (the game's menu pictures, such as the stage select images) are
  not served: a stage mod that ships one would replace that picture for every stage. The loader makes its own
  preview and thumbnail instead.
- **Stage mods installed through Fluffy Mod Manager are not variants yet**: their pak replaces the stage for
  everyone, the loader does not restore the original under it.
- A mod that hangs the game on its own hangs it here too (seen: *Aokigahara - no NPC* stays on the VS screen,
  also when installed as a plain Fluffy pak). Its Training Room level scene
  (`natives/stm/product/level/scene/ess/ess0000_00/ess0000_00_level.scn.20`) places four World Tour gimmicks
  (`om_setter`), which wait for World Tour systems a battle does not have. Its nine other files are the same as
  *Aokigahara with NPCs*, which loads: the mod's files without that scene, as a folder with a `natives` tree in
  `stage_mods`, load too. One cause is repaired: a texture whose level table declares
  wrong row sizes, which the game waits for forever (the Training Room pictures of Stage Lighting Overhaul,
  1.9.1). The log names each texture repaired or left out.

## How it works

The game imports `amd_ags_x64.dll` from its own folder, before any engine code runs. Its `DllMain`
(`proxy/slots_proxy.cpp`) runs the stage slots around the costume loader:

1. **Stage pass** (`stages/loader/stage_loader.cpp`, DllMain). Reads `reframework/stage_mods`, unpacks archives once
   into `stage_mods/.cache`, and recognises the stage of every file with `stage_paths.txt` (every game path
   holding a stage code `essNNNN_NN`, hashed at startup). Each file of a variant is stored in our patch pak
   under `pak_path_hash("natives/stm/_stageslots/<variant>/<vanilla hash>")`, next to its preview and thumbnail
   textures.
   The stage pak goes right above the mod paks and below the costume pak (marker
   `natives/stm/sf6_stage_slots.marker`). Every texture of a mod is checked first: a wrong level table is
   repaired (`texture_mip_check`, shared with the costume loader), a texture that cannot be repaired is left
   out. Nothing changed since last launch: the saved table is reused (a few ms).
2. **Costume pass** (`costumes/`). It recognises the stage pak by its marker and leaves it out of its scan
   and of its fingerprint, so adding or removing a stage mod does not rebuild the costume pak; its own pak
   stays above the stage pak (moved, not rebuilt, when the stage pak comes or goes).
3. **Redirection** (`stages/loader/stage_redirect.cpp`). The engine turns every file path into a pak hash through one
   function, `path_to_hash`. A small block of memory is reserved near the executable at startup; once a
   variant is selected, or a title screen or UI pack choice differs from what the launch wrote (and never in the
   first 20 s), the entry of `path_to_hash` gets a 5-byte jump to our
   detour, which swaps the hash of each vanilla file the selected variant replaces for its copy's hash. A
   tool that hooked the function first (HARD READ) stays in the chain. The selection is re-read from
   `state.json` when it changes.
4. **Native plugin** (`plugin/src/stages.cpp`, in `SF6_CostumeSlotsNative.dll`). Hooks on the stage select
   screen (shown, hidden, focus and preview changed), on UP / DOWN of its UI agent and on the VS screen: the
   variant's name between UP / DOWN hints and its preview, the thumbnail of each stage of the list (the
   `tex` texture of each cell's `e_mat_thumb` material, through the core's `ui_set_material_texture`),
   `state.json` written for the loader. Game thread only, texts written only when they differ, nothing done
   between events.

Outputs, in `reframework/data/SF6_StageSlots_Data`: `registry.json` (variants per stage, for the script),
`loader/variants.tsv` (the same with every redirection), `state.json` (selection); title packs:
`reframework/data/SF6_TitleSlots_Data/registry.json`; UI packs: `reframework/data/SF6_UISlots_Data/registry.json`. Log:
`SF6_StageSlots.log` in the game folder.

## Building

`build.bat` at the root of the repository (Visual Studio 2022 or Build Tools with the C++ workload) builds
both loaders and the tools into `build/`:

```
build\amd_ags_x64.dll   costume slots + stage slots
build\stagepak.exe, stagetest.exe, textest.exe
```

- `stagetest <folder>` runs the stage pass on a folder laid out like the game's (no game needed), and prints the
  variants and the UI packs' tree. `stagetest --selftest` runs the unit tests. `stagetest --make-base <out.pak> <path>...`
  makes a stand-in for `re_chunk_000.pak`, for the copies of the game's own files.
- `textest <image> <out.tex>` makes a preview, `textest --thumb <image> <out.tex>` a thumbnail;
  `textest --dump <in.tex> <out.png>` decodes either.
- `stages/tools/make_stage_index.py` rebuilds `stages/data/loader/stage_paths.txt` from REasy-parser's
  `SF6_STM.list` (point `REASY_PARSER` at your checkout).

Development notes (engine facts, test status, open items): [docs/DEV_NOTES.md](docs/DEV_NOTES.md).

## Third party

`stages/loader/third_party`: stb_image, stb_image_write, stb_dxt (public domain or MIT). bcdec (MIT: a mod's own menu
pictures, and `textest`) and the pak, archive and decompression code come from `common/`. See
[THIRD_PARTY.md](../THIRD_PARTY.md).
