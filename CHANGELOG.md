# Changelog

## Unreleased

- Runtime options: a mod alone in its own sub-folder of a character folder is always its own outfit (no retouch, subset, merge or add-on placement on either side); logged and listed in the merge audit.

### SF6 Slots Loader (fixes ported from ZZtaii's Slotsloader fork, MIT)
- A slot check reads only the textures a material actually binds, not every `.tex` string of a material file: leftover
  paths no material uses (the "Leon Requiem On Luke" mods keep some) no longer count as "texture missing" (fork 1.9.9).
- Archives and folder mods whose paths pass MAX_PATH: the options of an archive, its `modinfo.ini` and the cache
  fingerprint are read with the wide API and the long-path prefix (fork 1.9.7).
- An antivirus briefly locking a freshly unpacked archive no longer makes the loader skip it: the cache folder rename
  is retried for about 2 s (fork 1.10.4).
- Modular mods with nested Fluffy menus ("03 Outfit" > "Lower Neckline" > option) are made as Fluffy installs them:
  the menu folders (a `modinfo.ini` only) are read, every option under one top-level group is one choice, the main
  files are merged first (a root not named `(...)` could overwrite the options), a group whose name or description
  says "optional" is left out, outfits are named after the mod; a modular mod may make 50 combinations (24 before)
  (fork 1.9.7-custom).
- An option whose `AddonFor` names another complete option of its mod is made with that option ("Leon as Luke +
  Optional - More muscles"), not as an outfit of its own without the option's files (fork 1.9.8).
- Add-ons whose `AddonFor` names nothing of their mod, beside a single complete option, are add-ons of that option
  (with a WARN: fix the mod's `AddonFor`); they were lost in a modular mod that made nothing of them (fork 1.9.9).
- Runtime costume options (`RuntimeOptions=on`; ours, with the Slotsloader Manager's "choose one" rule): an outfit
  with add-ons (Leon Requiem On Luke, Aloha Ryu, Lily's DMC5 Nico...) is one outfit, its add-ons chosen in game
  (add-ons replacing a same file are one choice: Leon Hair / Trimmed Hair); a group its author asks exactly one option of ("2! Skin", "Choose one") is offered without None
  (unless its options share files with another group's); an option for a part the outfit takes from another folder
  (Leon's hair add-ons, the game's hair in `000/02/`) now replaces that part. A modular mod none of whose options
  has a body mesh (Mai face accessories) still makes no outfit, with a WARN instead of nothing. An outfit the user
  put with its add-on archives in one sub-folder (`Ken\Ken SFV\` with KenSFV and Ken SFV Hair V2) is one outfit
  too, its add-ons worn by default (what the sub-folder made before, same slot) and chosen in game; before, it made
  the combined outfit without options. A chain is no longer put back under a mesh that stays (a part's mesh without
  a way back keeps its own chain).
- Runtime costume options: an add-on that brings a part's mesh, material and chain together (Ken SFV Hair V2) can be
  turned off live: the part without it (the game's hair) is stored as one unit and swapped as one, the chain stopped
  for the swap and set before the mesh; Ingrid's Fly In A Bucket book / cat add-ons give back the game's props. An
  option worn by default that turning off would not change is no longer offered.
- Joint constraints a mod ships (`.jcns`) are used by its outfit, as a replacement install uses them: the outfit gets
  its own copy of the game's `_jcs.user` naming them (or the mod's own, repointed), and its scene follows it. Before,
  the outfit drove the mod's mesh with the game's constraints (A.K.I. bare-legs mods with added toe bones). A
  constraint or physics file of the mod that nothing the outfit loads names is reported with a WARN (fork 1.9.8). A
  runtime option that ships joint constraints is applied at the next launch (logged).
- A mod's own scene of the outfit is used whole when it is another layout of the outfit (another number of model
  parts than the game's: SF5 Sakura over Ryu) or puts it on another fighter's body (another prefab / body shape), and
  when the game's scene cannot be read (its pak entry turned off by a mod manager's "PAK method"); before, the outfit
  lost its own hair and cloth physics, came out on the wrong skeleton, or was left out ("slot scene missing") (fork
  1.9.9, 1.10.1, 1.10.3). Unlike the fork, only a scene made for this game version (every type signature the game's)
  is taken; another is left as before, with its reason in the log.
- A game file whose pak entry a mod manager's "PAK method" turned off is read from its loose copy in the game's
  `natives` folder: outfits that use such a texture are no longer left out as "texture missing"; runtime options
  check their base copies against the same files (our pak, other tools' patch paks, the game's paks with the DLC,
  loose copies) (fork 1.10.1).
- Slotsloader Manager contract 1 (`docs/MANAGER_CONTRACT.md`): the loader writes `variants.json` (every variant of
  every mod in `costume_mods`, on and off, with its outfit) and reads the Manager's `variant_choices.json` at launch:
  a variant turned off gets no outfit and its files stay out of the pak; with `RuntimeOptions=on` the variants are
  the outfits `off` would make, the one outfit wears the first one left on by default (same outfit number) and is
  not built when every one is off. The contract number is in `variants.json` (`"contract": 1`) and in the
  package's `modinfo.ini` (`SlotsloaderContract=1`) (fork 1.10.0).
- Mods the Slotsloader Manager installed with "Replace original" (loose files, its patch paks) no longer also make an
  extra outfit: read from the Manager's record (`manager_installs.json`, else its `replace-state.json`; ZZtaii's
  format) (`docs/MANAGER_CONTRACT.md`).
- A new outfit takes the number after the highest one in use for its fighter instead of a gap a removed mod left
  (listed last); outfits already installed keep their numbers (fork 1.10.0).
- Runtime costume options: an outfit put with its add-ons in one sub-folder is named after the outfit alone ("Ingrid -
  Fly In A Bucket - Drive Tech", no more "+ Book + Cat": they are options); the menu's title cuts a name past 40
  characters ("...").
- Runtime costume options: a separate add-on that makes no outfit (Mai face accessories, A.K.I No Sleeves, Cammy C4
  pale skin) becomes options of the outfits whose files it replaces: a series made of files every outfit of the
  fighter loads (Mai's "remove wrinkles") on every outfit, ours and the game's; a series replacing a mesh or material
  file a mod's outfit ships (the "C1 swap" glasses) on that outfit; else on the game outfits that load its files (the
  "C2" glasses on Mai's Outfit 2, the game's files as the way back). Stored only where it fits (joints, materials,
  textures), none worn by default; a series' "main files" go with each of its other options. The plugin finds a game
  outfit with options from the paths its model loads (our slots first); a game outfit whose options replace a mesh
  or material is worn only when its model loads that file (A.K.I's Outfit 4 shares Outfit 1's folder, not its mdf2).
- Runtime costume options: a series' "main files" alone are no longer a choice (the mod always pairs them with one
  of its other options); the menu shows names without the prefixes mods use for Fluffy's order ("!", "1)", "addon -").
- Runtime costume options (`on`): a retouch (a mod of meshes and textures only, no material file: C. Viper Barefoot,
  Coatless, coat no mesh, C2 Glasses Removed) makes no outfit of its own any more: it is an option of every outfit
  that wears the files it replaces (the game outfit it was made from, the mods' outfits made from it that keep the
  game's file there) where it fits; retouches of one file are one choice ("Body": None / Barefoot / Coat no mesh /
  Coatless). One that fits no game outfit it modifies stays an outfit. Its former outfit, chosen in Battle Settings,
  becomes the game outfit with the retouch chosen; the other outfits keep their numbers.
- Runtime costume options: On / Off options that replace a same file are one choice (Swimsuit Viper C1: "hide
  accessories" and "hide leg chain" both on brought the bracelets back); the menu never keeps two of them on.
- Runtime costume options (`on`), decided from the files only: versions of one archive (Aloha Ryu C1 / C2, Lily Jun
  Kazama No Shawl / Jun's Outfit) and outfits of several archives with the same meshes (Sagat's three Tees) are one
  outfit with a choice, where each one fits (a version made for another game outfit brings that outfit's head and
  parts); an outfit that is a game outfit with parts taken off (Ryu Outfit 1 No Cloak, Sagat Outfit 1 Shirtless) is an
  option of that game outfit. Their former outfits move there with their option chosen. Each build logs a short merge
  audit (what was merged, what looks related and why it was not).
- Fighter Settings shows the name of the outfit on the preview (our outfits: the mod's name; the game's: its own)
  at the right end of the bottom input guide bar.
- Runtime costume options (`on`): materials an option brings that the outfit itself has not get their colour entries
  in the outfit's colour files, each colour slot as the game shows it (Aloha Ryu C2's head: dark eyebrows and beard
  as on the game's Outfit 2, no longer light grey); nothing else changes.
- Not ported: the fork's character-swap repairs (1.10.4, `swap_repairs.txt` is ignored).

## 1.16.3 (2026-10-19) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Color Mod | 0.22.3 | colour grid: the sliders follow, the focused square stands out; SAVE AS closes the editor |
| SF6 Native Core | 0.8.2 | unchanged |
| SF6 Slots Loader | 1.12.0 | unchanged |
| SF6 Recording Manager | 2.1.2 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. Needs SF6 Native Core 0.8.2.

### SF6 Color Mod 0.22.3
- Colour editor, colour grid: picking a square moves the Hue, Saturation and Brightness sliders to that colour (they
  kept their old values before).
- The square under the cursor is drawn larger and above its neighbours, so the game's focus frame around it is easy
  to see (it was very thin at the grid's size).
- SAVE AS: the editor closes and the screen ends on the new colour (`MC n`). It no longer opens the editor again.

### Known issues
- The pencil-like outline around the fighter during Drive Impact, Drive Reversal (red) and Drive Rush (green) keeps
  the game's colours: it is drawn by the game's camera filter, not by an effect.
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.16.2 (2026-10-18) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Color Mod | 0.22.2 | Drive Reversal colours, Drive Impact / Reversal effects no longer follow Drive Rush |
| SF6 Native Core | 0.8.2 | unchanged |
| SF6 Slots Loader | 1.12.0 | unchanged |
| SF6 Recording Manager | 2.1.2 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. Needs SF6 Native Core 0.8.2.

### SF6 Color Mod 0.22.2
- Drive Reversal keeps its own colours. The game plays one more effect after both Drive Impact and Drive Reversal;
  it was painted with the Drive Impact colours, so a Drive Reversal took the Drive Impact colours, even when set to Off.
- The ring behind the fighter during a Drive Reversal takes the Drive Reversal colour, and the common Drive Impact
  effects the Drive Impact colour. Both took the Drive Rush colour before.

### Known issues
- The pencil-like outline around the fighter during Drive Impact, Drive Reversal (red) and Drive Rush (green) keeps
  the game's colours: it is drawn by the game's camera filter, not by an effect.
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.16.1 (2026-10-18) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Color Mod | 0.22.1 | Drive rows in Battle Hub, Drive colours when seated as P2 online |
| SF6 Native Core | 0.8.2 | unchanged |
| SF6 Slots Loader | 1.12.0 | unchanged |
| SF6 Recording Manager | 2.1.2 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. Needs SF6 Native Core 0.8.2.

### SF6 Color Mod 0.22.1
- Battle Hub, Fighter Settings: the Drive rows are made once. The game updates two setting screens at the same time
  there, on two threads; both made the rows (two Drive Impact rows on top of each other, the second Drive Reversal
  row did not respond).
- Online and Battle Hub, seated as P2: your Drive colours are the ones of your own page (P1, the one Fighter Settings
  edits) and your opponent's the P2 page, as when you are seated as P1. Before, the sides followed the seat.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.16.0 (2026-10-18) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Color Mod | 0.22.0 | colour mods folder `reframework/color_mods`, lighter fights, select-screen fixes |
| SF6 Native Core | 0.8.2 | unchanged |
| SF6 Slots Loader | 1.12.0 | unchanged |
| SF6 Recording Manager | 2.1.2 | background threads start after the game's first frames |

Updating in Fluffy Mod Manager: disable or remove the previous version first. Needs SF6 Native Core 0.8.2.

### SF6 Color Mod 0.22.0
- **Colour mods folder**: `reframework/color_mods/<Character>/` takes colour mods as downloaded (`.zip`, `.7z`,
  `.rar`, an unpacked folder or the loose `esfNNN_CCC_cmd_KKK.user.2` files), like the costume mods folder. Archives
  are unpacked once in the background after the first seconds of the game; their colours appear as `RW n`. The folders
  and a `README.txt` come with the package. A mod's file is never changed or deleted.
- **One colour folder**: the colours of the former folder `reframework/data/SF6_ColorSpinExtra_data/colors/` move to
  `reframework/color_mods/<Character>/` at the first start, with your selections, squares and edits; the old folder is
  then removed and no longer read. Colours made in the editor (SAVE AS, `MC n`) are saved in
  `color_mods/<Character>/MANUALCOLORS_01`, `_02`...
- Remembered colour selections store the colour's identity (its file): adding or removing colours never moves a
  player to another colour. Edits of a colour from an archive survive a new version of the archive.
- **Fights**: the Drive effect recolouring does less work during a fight (its scans run on a background thread, the
  material hooks no longer look anything up per call).
- **Startup**: the background threads (file writer, fighter names, colour mods) start after the game's first frames
  instead of at plugin load. REFramework's nightly 1.5.9.1 suspends every thread at startup while holding the loader
  lock; a thread caught holding the heap lock froze one tester's game. Writes made before are queued, not lost.
- Character select: the colour row of each player reads that player's own colour list. Before, the other player's
  list could make a game colour (JP's EX colours) show as `RW 1`.
- Character select, DI mode: the Drive Reversal row no longer shows the selected costume colour (`MC 1`) and its value
  changes again.
- Character select: going from an `RW` / `MC` colour to a game colour no longer leaves the old label (`MC 23` on
  colour 01).

### SF6 Recording Manager 2.1.2
- The background threads (file writer, Community client) start after the game's first frames instead of at plugin
  load (same cause as the Color Mod's startup change). Requests made before wait in the queue.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.15.0 (2026-10-16) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Color Mod | 0.21.0 | a colour grid in the colour editor and the Drive colour pages, one colour per page |
| SF6 Native Core | 0.8.2 | unchanged |
| SF6 Slots Loader | 1.12.0 | unchanged |
| SF6 Recording Manager | 2.1.1 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. Needs SF6 Native Core 0.8.2.

### SF6 Color Mod 0.21.0
- **Colour grid**: under the Hue, Saturation and Brightness sliders, a 10 x 5 grid of the game's own cells with the
  avatar editor's 50 colours. The arrows move the game's focus glow from cell to cell (the colour does not change
  while moving); the decide key (F on keyboard, A on a pad) applies the cell; the game's check marks the cell equal
  to the current colour; Backspace puts the saved colour back.
- **One colour per page** in the colour editor (its three sliders and its grid). The Drive colour pages (Drive
  Impact, Reversal, Parry, Rush) get the same grid.
- The Hue rainbow is drawn above the slider's track (brighter than in 0.20.0).
- Rows that edit one colour show one square in their title.
- Opened from the character select, the Drive colour pages have no grid (the cells are copied from the Fighter
  Settings screen): their sliders work as before.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.14.0 (2026-10-16) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Color Mod | 0.20.0 | every Hue slider is drawn on the game's own rainbow |
| SF6 Native Core | 0.8.2 | three display functions for it |
| SF6 Slots Loader | 1.12.0 | unchanged |
| SF6 Recording Manager | 2.1.1 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. The Color Mod's rainbow needs Native
Core 0.8.2: install both.

### SF6 Color Mod 0.20.0
- **Every Hue slider shows the game's rainbow** on its track, the one of the avatar editor's colour window: the colour
  editor's Hue rows, Quick Edit's family rows, the squares and the Drive colours. Saturation and Brightness keep their
  look, values and keys are unchanged. With an older Native Core the sliders keep the option window's look.
- The rainbow is the game's own picture: its hues are not evenly spaced (around the middle of the bar the colour drawn
  can be up to about 25° away from the slider's value).

### SF6 Native Core 0.8.2
- `ui_set_texture_rect`, `ui_get_texture_state` and `ui_clone_control`, appended at the end of the API: a part of a
  texture on a GUI texture control, its state read back, and a plain copy of a GUI element added under a parent. See
  [core/docs/ABI.md](core/docs/ABI.md). Older clients are not affected.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.13.0 (2026-10-16) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.12.0 | slots for Arjun; stage thumbnails follow the variants; a stage mod no longer replaces menu pictures |
| SF6 Native Core | 0.8.1 | two functions for the Slots Loader's stage thumbnails |
| SF6 Color Mod | 0.19.12 | unchanged |
| SF6 Recording Manager | 2.1.1 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. The stage pak is built again once at
the first start (the stage loader's format changed).

### SF6 Slots Loader 1.12.0
- **Costume slots work for Arjun.** A slot chosen in Battle Settings is shown over one of the character's own
  outfits, the one the save and the other players keep (the carrier). It was always DriveTech, which Arjun does not
  have. The carrier is now read per character from the game's costume table: DriveTech when the character has it,
  else Outfit 1. A save written with DriveTech for Arjun by 1.11.1 is moved to Outfit 1 by itself. A character with
  neither is left out of the alias, the others go on. See [plugin/README.md](plugin/README.md).
- **Stage thumbnails follow the variants.** On the stage select screen, each stage's thumbnail in the list at the
  bottom shows the variant selected for it, focused or not; the original look gives the game's thumbnail back. The
  thumbnail is the centre of the variant's preview. Needs SF6 Native Core 0.8.1: with an older core the list keeps
  the game's thumbnails.
- **A stage mod no longer replaces menu pictures.** A mod's files under `natives/stm/product/gui/` are not served
  any more: a Dotonbori mod that shipped one replaced the Training Room's picture on the stage select screen.
- The UP / DOWN hints of the stage select screen name the keys of the keyboard layout in use (Z / S on AZERTY).

### SF6 Native Core 0.8.1
- `ui_get_material_texture` / `ui_set_material_texture`, appended at the end of the API: read and replace the texture
  of a named texture variable of a GUI material (game thread). Older clients are not affected; see
  [core/docs/ABI.md](core/docs/ABI.md).

### Build
- `build.bat` compiles in parallel (about 2 min for everything instead of 8) and builds one module on demand:
  `build.bat plugin`, `core`, `loader`, `colormod` or `crsm`.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.12.0 (2026-10-14) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.11.1 | a slot's chain and cloth files follow the mod (contribution by ZZtaii) |
| SF6 Recording Manager | 2.1.1 | its source is now in this repository; a deleted settings.json no longer turns it off; "Verifying with Steam..." |
| SF6 Native Core | 0.8.0 | unchanged |
| SF6 Color Mod | 0.19.12 | unchanged |

Updating in Fluffy Mod Manager: disable or remove the previous version first. The costume pak is built again once at
the first start (the loader's build changed).

### SF6 Slots Loader 1.11.1
- **A slot's physics files follow the mod** (pull request #1 by ZZtaii): when a mod ships its own `chain.user` /
  `havok.user`, the slot's copies named the original outfit's chain and cloth, so the slot moved with the game's
  physics on the mod's mesh. They now point at the slot's own copies.
- Wind and pose chain references of a slot (`Weather/Wind/...`, the challenger pose) are no longer renamed to files that
  exist nowhere; they stay on the game's file. Outfits without their own physics files give the same pak as before.

### SF6 Recording Manager 2.1.1
- **The Recording Manager's source is now in this repository**, in `crsm/` (MIT, as the rest of the suite): the plugin
  `SF6_CRSMNative.dll`, its locale table, its package texts and its offline tests. `build.bat` builds it and
  `packaging/make_suite_packages.py` builds its zip with the three others. The Community server is not part of it.
  [crsm/README.md](crsm/README.md) says what the plugin sends to the Community server.
- A deleted `reframework/data/SF6_CRSMNative_data/settings.json` no longer turns the mod off: when no Lua version of the
  mod is installed it is written again as the package ships it (every part native, the menu taken 2 s into Training)
  and used at once. Before, the mod did nothing until the package was reinstalled.
- Community: the first share or like of a player shows "Verifying with Steam..." while the Steam check runs (a few
  seconds); another request in that time says "Steam verification in progress, try again in a moment", and any other
  request while one is running says "Community: please wait, a request is in progress", instead of "busy".

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.11.2 (2026-10-14) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Recording Manager | 2.1.0 | Community shares and likes verified by Steam |
| SF6 Slots Loader | 1.11.0 | unchanged |
| SF6 Native Core | 0.8.0 | unchanged |
| SF6 Color Mod | 0.19.12 | unchanged |

Only the Recording Manager zip and `SF6_ModSuite_AllInOne_v1.11.2.zip` changed. Updating in Fluffy Mod Manager: disable or
remove the previous version first.

### SF6 Recording Manager 2.1.0
- **Community: who shares or likes a loadout is verified by Steam.** The plugin asks the Steam client, which runs with
  the game, for a ticket made for the Community server; the server checks it with Steam. No login, no password, nothing
  to click. Until now the author's name was declared by the game itself and could be faked.
- Lists show the author's CFN name with a green `[V]` for a verified share. Shares made before this version have no mark.
- Your Steam account id stays on the server for moderation: it is never shown to other players, never in the lists.
  The plugin no longer writes your SteamID or Steam name into `config.json`, and never sends a SteamID as a name.
- One like per Steam account and loadout. Deleting a shared loadout is done by the admin.
- Your verification is kept in `reframework/data/SF6_Community_data/identity.json`: never share that file.
- **Versions 2.0.x can still browse and download loadouts, but no longer share or like** (the server answers "Update SF6
  Recording Manager to 2.1.0 to share or like.").

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.11.1 (2026-10-14) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Recording Manager | 2.0.1 | fixes: Replay page kept, recording keeps the slot's name and rate, menu tab under the mod's windows, Community messages |
| SF6 Slots Loader | 1.11.0 | unchanged |
| SF6 Native Core | 0.8.0 | unchanged |
| SF6 Color Mod | 0.19.12 | unchanged |

Only the Recording Manager zip and `SF6_ModSuite_AllInOne_v1.11.1.zip` changed. Updating in Fluffy Mod Manager: disable or
remove the previous version first.

### SF6 Recording Manager 2.0.1
- **The Replay page is kept**: leaving the pause menu on Replay and coming back opened Record. The page you left (Record,
  Replay or Manager) is the one you get back, also after leaving Training and coming back.
- **Recording into a slot above 8 keeps that slot's name and random rate.** With a slot above 8 ticked for replay, the
  new recording took that slot's name and slot 8's rate. Only the recording moves now; a new slot gets no name and rate 1.
- **A recording above 8 that ends on its own** (without opening the menu) no longer leaves the replay playing slot 8's
  recording under another slot's number.
- **The menu's tab no longer changes under the mod's windows**: L1 + R1 (A + E on keyboard) in the Community list, the
  slot name box or a confirmation box moved the game's tab bar behind them (the next E then jumped from the wrong tab).
- **Community messages**: a share the server refuses says why ("Already shared: nothing changed", "Loadout too large to
  share", "Share limit reached, try again later", "Share refused: <reason>") instead of "Loadout shared"; an answer with
  no file or no list gives a clear message; un-liking moves the score down and shows "Unliked"; your CFN name is read
  again at each list, like and share.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.11.0 (2026-10-14) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.11.0 | extra slots only for the outfits you own; a clean-up tool for patch day; a game update adding a fighter or an outfit no longer needs a new release |
| SF6 Color Mod | 0.19.12 | a fighter added by a game update is named from the game |
| SF6 Native Core | 0.8.0 | the training pause menu for mods; runs on REFramework 1.5.8 |
| SF6 Recording Manager | 2.0.0 | new in the suite: 30 recording slots per character, loadouts, reversal picker and sharing in the training pause menu |

Updating in Fluffy Mod Manager: disable or remove the previous version of each package first. The costume pak is
built again once at the first start of the game (the loader's build changed).

**New: `SF6_ModSuite_AllInOne_v1.11.0.zip`**, the four packages in one Fluffy package (`packaging/make_bundle.py`).
Install either it or the separate packages, never both: two packages shipping the same files conflict in Fluffy Mod
Manager.

### SF6 Slots Loader 1.11.0
- **Extra slots only for the outfits you own.** A slot made from a fighter's Outfit 2, 3 or 4 shows only when the game
  lists that outfit as yours (default outfit, bought with in-game currency or in the shop, or given by a DLC the game
  accepts). Slots made from Outfit 1 or the Drive Tech outfit always show. Whatever makes the game say yes counts, an
  unlock-all script included: with one, every slot shows as before. The check is made offline and kept until the game
  closes; nothing changes during an online session. A hidden slot stays in the costume pak: replays and saved settings
  that name it still load. The plugin's log (`reframework\data\SF6_CostumeSlots_data\native_log.txt`) has one line per
  hidden slot, e.g. `[F1] slot v05 (Outfit I, <mod>) hidden: made for Outfit 2, which the player does not own`.
- **A game update that adds a fighter or an outfit no longer needs a new Slots Loader release.** The costume tables, the
  version scenes and the outfit names of the costume pak are built from the game's own files at each pak build (1.10.6
  shipped copies made before the update: they hid Arjun and C. Viper's Outfit 4). Outfits of the game missing from the
  shipped vanilla costume index are read from the game too (mods made for them are recognised), and a new fighter gets
  its `reframework\costume_mods\<Name>` folder named from the game. The shipped copies stay as a fallback (logged).
- **Add-ons downloaded separately and grouped with their costume** (one archive or folder each, in one sub-folder of the
  character) give one outfit wearing all of them, instead of one variant per add-on plus the complete one. Add-ons
  that replace the same files, and the options of one archive, keep the old behaviour. A grouped costume with more than
  three add-ons keeps the base alone again (it was dropped).

- **`SF6_SlotsClean.exe`** (package root, next to `amd_ags_x64.dll`): removes what the loader made at runtime in the game
  folder, for the patch day. After a game update Fluffy Mod Manager re-reads the game archives and records every pak
  it finds as a game file, the loader's slot paks included; with the package turned off nothing removes them, and the
  slots keep showing. Run it first. It finds the loader's paks by their marker (costume and stage slots), deletes one
  only when no pak of another mod has a higher number and otherwise replaces it in place by a pak holding only the
  marker (the game reads the patch paks as an unbroken row), then removes `registry.json` and `outfits.json` of the
  costume data and the stage slots' runtime state. `reframework\costume_mods`, other mods' data, the shipped `loader\`
  files, logs and Fluffy's files are not touched. `--dry-run` lists without changing anything; it refuses while
  Street Fighter 6 runs. If Fluffy's `ArchiveList.bin` lists one of the paks it says so.
- The loader logs a `WARN:` line at each pak build when Fluffy's `ArchiveList.bin` lists one of its paks.
- "Patch day" procedure in the README and in `docs/FLUFFY.md`.
- **No more hole in the patch pak row.** The game reads the patch paks as an unbroken row from patch_001. When a mod pak
  was added above the loader's pak, the loader deleted its old pak and wrote the new one above the mod's, leaving a hole
  (the black screen of 03/10). The costume loader and the stage loader now replace their old pak, in place, by a
  marker-only placeholder when a mod pak sits above it, and delete the placeholder once no mod pak is above it any more.
  Placeholders are never taken for the real pak (the "up to date" check and the move of the pak ignore them), and the
  audit script does not list them as mods.
- Tests: `costumes/tools/slots_clean_test/`.

### SF6 Color Mod 0.19.12
- A fighter added by a game update gets its name in the colour packs and Drive settings (`<Name>_esfNNN`,
  `<Name>.json`) from the game's own name file, read in the background at start, instead of waiting for a release.
  The names of today's fighters do not change.

### SF6 Native Core 0.8.0
- **Training pause menu for mods** (ABI increment 8, appended: `pm_set_callbacks`, `pm_page`, `pm_row`, `pm_set_texts`,
  `pm_set_options`, `pm_remove`, `pm_page_shown`, `pm_row_part`, `pm_status`, `pm_row_of`). A mod adds its own pages
  to the training pause menu (a sub-page next to the game's ones, e.g. in Recording Settings) with buttons and spins;
  the core builds them as the game's own menu data, answers the game for the mod's rows only, renders the spins (up to
  64 options, labels changed in place, long labels shrunk on request) and adds the mods' key-guide hints to the bar at
  the bottom. One engine for every mod; nothing is hooked until a mod uses it, and every method is first checked for
  machine code it shares with other methods of the game: such a method is not hooked (logged), so the service cannot
  slow the game down the way a shared hook does. The check runs on a thread of its own and its result is kept per game
  version (`reframework\data\sf6core\pm_codecheck.txt`). See [core/docs/PAUSE_MENU.md](core/docs/PAUSE_MENU.md).
- **The game's own dynamic rows of the training pause menu are kept**, such as "End Training Session" in the Battle Hub
  training room: the clean-up at each build of the option window removed every row of that kind; it now removes only
  rows the core built and no longer uses.
- **Reference writes follow the game** (crash fix): `obj_set_ref` always released the field's old value, but the game
  writes a local object (not reference-counted yet) with a plain store, and its fields own no reference. A value
  written into such an object at each page build released one it never held, until it was freed while the menu still
  pointed at it. `obj_set_ref` now follows the target's reference count as the game does, for every mod's writes.
- **REFramework 1.5.8 (plugin API 1.10)**: element hiding (`set_element_hidden`) needs `on_pre_gui_draw_element`, which
  only exists from plugin API 1.14 on. The core registered it on any REFramework, reading past an older one's function
  table; it now checks the API version first and keeps element hiding off (one log line) below 1.14. No mod of this
  suite uses element hiding.
- **Method lookup on types with more than 64 methods** (bug): the core's by-name method lookup (used by every display
  write: `get_Message`, `set_Message`, the style setters of `via.gui.Text`, `ui_clone_row`...) passed REFramework's
  `get_methods` a buffer size in elements where it expects bytes, so room for 64 handles only. A type with more methods
  answered "too small" and that level of the type chain was skipped silently. The buffer is now sized from the type's
  method count, results (also "not found") are cached per object type and signature, and the log says once per type
  and signature when a method is found only thanks to the fix or not found at all.
- Diagnostics: the service's fault lines say where they happened (step, game call, address, thread);
  `reframework\data\sf6core\pm_trace.txt` turns on a step-by-step log of its next install and page build,
  `pm_bisect.txt` turns its behaviours off one by one. `docs/ABI.md` lists increments 6 to 8.
- Tested in the game: several hundred very fast sub-page switches, spin changes, label and text changes, a deliberate
  fault in a mod's callback (caught, the game went on) and the page's removal, without a crash.

### SF6 Recording Manager 2.0.0
- **New in the suite** (also known as CRSM, Community Recording Slot Manager): 30 recording slots per character in
  Training (1-8 are the game's own, 9-30 extra), loadouts, slot names, a reversal picker and Community sharing of
  loadouts, all in the game's own training pause menu (Recording Settings > Manager / Record / Replay).
- **One native plugin, no Lua script**: `reframework/plugins/SF6_CRSMNative.dll` replaces the Lua Recording Manager 1.x.
  Its pages are served by the SF6 Native Core's pause-menu service; at rest it makes no game call per frame.
- **Your 1.x data is kept**: slots, loadouts and names stay in `reframework/data/SF6_RecordingSlotManager_data/`, the
  Community settings in `SF6_Community_data/`, the same folders as 1.x. The package never holds them.
- **Before installing, remove the Lua version (1.x)** from `reframework/autorun/` (the zip's README lists its files):
  while one of its scripts is there the plugin stands down and leaves the menu to it.
- The game's code is checked once per game version before the plugin hooks it (a hook on code the game shares between
  several methods would fire for all of them); the result is kept in `reframework/data/SF6_CRSMNative_data/codecheck.txt`,
  so the next starts skip the check and REFramework's log stays small.
- Needs SF6 Native Core 0.8.0 (this release) and REFramework 1.5.8 or newer. Released as a binary; its source is not in
  this repository.

### Known issues
- Slots Loader: the slot names are set when the costume pak is built. When a slot is hidden because its outfit is not
  yours, the names of the others are not renumbered (you may see Outfit I, then Outfit III).
- Recording Manager: one hook is not found since the game update of 13/10: while the slot name box is open, L1 + R1
  may change the menu's tab.
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.6 (2026-10-13) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.6 | Arjun, C. Viper's Outfit 4; the costume pak is built again after a game update |
| SF6 Color Mod | 0.19.11 | Arjun's name for his colour and Drive files |
| SF6 Native Core | 0.7.3 | unchanged |

Updating the Slots Loader in Fluffy Mod Manager: disable or remove the previous version first.

### SF6 Slots Loader 1.10.6
- **Arjun works again with the Slots Loader**, and C. Viper's Outfit 4 is back. The game update of 13/10 added them to
  the game's costume tables; the costume pak replaces those tables with its own copies, which did not have them yet:
  Arjun had no outfit (selecting him acted like a broken character) and C. Viper's new outfit was hidden. The copies
  are rebuilt from the updated game, Arjun gets his 100 slots, his `costume_mods\Arjun` folder and the vanilla costume
  index knows his outfits (mods made for him are recognised).
- **The costume pak is built again after every game update** (it was kept as long as the mods did not change).
- The tools that make the shipped tables find the game's new version scenes by themselves; the tool that writes the
  vanilla costume index is back in the repository (`costumes/tools/make_vanilla_index.py`).

### SF6 Color Mod 0.19.11
- Arjun's colour packs and Drive settings use his name (`Arjun_esf034`, `Arjun.json`) like the other fighters.

### Known issues
- A game update that adds a fighter or an outfit needs a new Slots Loader release (its copies of the game's costume
  tables must be rebuilt); until then that fighter or outfit does not work with the Slots Loader installed.
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.5 (2026-10-09) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.5 | ends a slowdown in fights; a mod's shared prop on the select and continue screens |
| SF6 Color Mod | 0.19.10 | fixes the cause of memory corruption crashes; no more stutter when a fight starts |
| SF6 Native Core | 0.7.3 | performance log |

Updating the Slots Loader in Fluffy Mod Manager: disable or remove the previous version first.

### SF6 Color Mod 0.19.10
- **Fixes the cause of crashes of the game** (in a fight, when a fight loads or ends, or when the game closes with
  "corrupted thread-free list" in `re2_framework_log.txt`). The memory scan that finds the Drive colours could take
  bytes that only looked like a colour, at addresses that are not 4-byte aligned, for colour slots — one of them
  inside the game's own memory manager — and kept them in its spots file: each fight then wrote a colour there. A
  colour is now never written at an unaligned address, the scan keeps aligned slots only, and the bad slots already
  in the spots file are ignored.
- **No more stutter when a fight starts.** The fight-start work (finding the Drive effects, the memory scan, writing
  the colours) took 13 to 50 ms in single frames during the fight intro; it is now spread over the first frames, at
  most about 1 ms per frame. The Drive settings are read once when the game starts instead of when a fighter loads
  (one read took 56 ms), and the files the mod writes (journal, status, spots) are written by a background thread.
- Drive Impact painting at each effect does less work (no repeated page checks or allocations per colour).
- The performance log counts each hooked game method on its own; a write journal for crash investigations
  (`write_journal_on.txt`, see [colormod/README.md](colormod/README.md)).

### SF6 Slots Loader 1.10.5
- **Ends a slowdown in fights.** The plugin hooked a method of the stage select screen whose code the game shares
  with methods it calls everywhere: about 240,000 calls per second, in fights too, each through REFramework's hook,
  and a visibility check of the stage select screen ran every frame. That method is no longer hooked; the screen's
  end is seen by its own end event or a check every 30 frames while it is shown. Measured in ranked matches: seconds
  below 58 fps in fights went from 10.8 % to 3 % (the rest are the game's own heavy frames, e.g. Critical Arts).
- **A mod's replacement of a shared prop shows on the character select and continue screens, for that outfit
  only** (e.g. a mod that replaces Ingrid's monad at `000/30`). Those screens load props through prefabs named by the
  costume scene: the slot gets its own copies of those prefabs, pointing at its relocated files.
- Objects are checked again just before the online alias changes them.
- Performance log (`reframework/data/perf_on.txt`) and write journal (`write_journal_on.txt`), off by default.

### SF6 Native Core 0.7.3
- Performance log (`reframework/data/perf_on.txt`, off by default): every client's frame callback, the display
  commit, the option service, element hiding and the API calls timed per frame.

### Known issues
- The select-screen prop of a slot (Slots Loader 1.10.5) is built and checked in the costume pak; its display on the
  select screen has not been confirmed in game yet.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.4 (2026-10-08) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.4 | the plugin stops when the game closes |
| SF6 Color Mod | 0.19.8 | fixes the crashes of 0.19.7 in fights, stops when the game closes |
| SF6 Native Core | 0.7.2 | stops when the game closes |

Updating the Slots Loader in Fluffy Mod Manager: disable or remove the previous version first.

### SF6 Color Mod 0.19.8
- **Fixes crashes of the game in and after fights with 0.19.7.** The Drive colours written once per fight (0.19.7)
  skipped the check that the memory still holds the effect's colour, used a list of effects made before the fight
  changed, and could be written again while the fight ended: a colour could land in an effect the game had already
  freed, and the game crashed later in its own code (online, at the end of a match or when leaving a mode). The
  check is back (the same one as before 0.19.7), the effects are looked up again before each write, and nothing is
  written while a fight ends.

### All three: they stop when the game closes
- From the moment the game window gets the close message (the window's close button, Alt+F4, Windows shutting
  down), the three plugins stop touching the game: no more per-frame work, their hooks let the game run its own
  code, the core's display functions do nothing. If the game keeps running 30 s after a close message (a close that
  did not happen), they resume.

### Known issues
- The game can stop with a memory error a few seconds after its window was closed ("corrupted thread-free list" in
  `re2_framework_log.txt`). Not caused by what the plugins do while the game closes (they have stopped by then);
  being investigated.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.3 (2026-10-07) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.3 | unchanged (same files as 1.10.1) |
| SF6 Color Mod | 0.19.7 | no more stutters in fights, loads on the public REFramework 1.5.8 |
| SF6 Native Core | 0.7.1 | unchanged |

### SF6 Color Mod 0.19.7
- **No more stutters in fights.** Measured in Training (Drive Impact, Rush, Parry, Reversal on both sides): the mod
  took 7 to 27 ms in one frame every second during a fight, 2 to 5 ms at each Drive effect, and 93 + 178 ms when
  the fight loaded. Now: 0.05 to 0.13 ms per frame during a fight with no spike, about 30 ms + 8 ms at the load.
  - The colours are decided in the menus, a fight recomputes nothing: the Drive Parry / Rush colours are written
    once when the fight loads (the game shares that effect data between both players: your side's colours, or the
    other side's when yours are left to the game); the Drive Impact / Reversal colours are still painted per player
    when each effect starts (the game copies them for each effect), now in a fraction of a millisecond; no periodic
    check runs during the fight. See [colormod/README.md](colormod/README.md), *During a fight*.
  - The memory checks in front of every Drive colour write ask Windows once per memory region instead of once per
    address.
- **Loads on the public REFramework 1.5.8**: the mod asked for a newer REFramework plugin API than it uses and was
  refused there ("Requires a newer minor version").
- `perf_on.txt` (in `SF6_ColorModNative_data`) turns on a performance log of the mod, see the README.

### Known issues
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.2 (2026-10-07) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.2 | unchanged (same files as 1.10.1) |
| SF6 Color Mod | 0.19.6 | keyboard layouts in the colour editor |
| SF6 Native Core | 0.7.1 | unchanged |

### SF6 Color Mod 0.19.6
- **The colour editor turns its pages with the game's own tab keys.** It showed and read A / E on every keyboard:
  on a QWERTY keyboard the game's key is Q, and A also moved left in the menu. The editor now shows the keys the
  game names for its tabs (A / E on an AZERTY keyboard, Q / E on a QWERTY one, a remapped key too) and reads
  those.
- On a QWERTY keyboard, W (up) also switches the hints to the keyboard's.

### Known issues
- FPS drops have been seen with the Native Core and the Color Mod installed; being measured.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.1 (2026-10-07) — SF6 Mod Suite

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.1 | one Stage Slots fix |
| SF6 Color Mod | 0.19.5 | the Battle Settings known issue of 1.10.0 fixed, colours in Battle Settings matches |
| SF6 Native Core | 0.7.1 | unchanged |

Updating the Slots Loader in Fluffy Mod Manager: disable or remove the previous version first.

### Slots Loader
- **Stage Slots: the battle loads the variant selected.** After a quick UP / DOWN on the stage select, the battle
  could load the original stage: the selection was read while it was being saved, taken for "original everywhere",
  and not read again until the game restarted.

### SF6 Color Mod 0.19.5
- **Battle Settings > Character (Edit Character): a Color Mod colour stays on the model.** After leaving and
  coming back, the Color row showed it (`MC 2` for example) but the model wore the game's colour of the same
  number.
- **Matches that take the costume from Battle Settings wear its Color Mod colour** (online, Battle Hub, Combo
  Trials...): your own fighter wears the colour chosen in Edit Character, on the outfit chosen there, Slots Loader
  slots included. Only the last fighter edited there is remembered. Other players see the game's colour. Arcade,
  Training, Versus and Team Battle keep the colour picked on the character select.

### Known issues
- FPS drops have been seen with the Native Core and the Color Mod installed; being measured.
- Stage Slots: the *Aokigahara - no NPC* stage mod hangs on the VS screen, with or without the Slots Loader. The
  cause is in the mod: see [stages/README.md](stages/README.md), Limits.

## 1.10.0 (2026-10-07) — SF6 Mod Suite

The repository is now **SF6-ModSuite** (it was SF6-SlotsLoader). A release carries three Fluffy Mod Manager
packages, built and tested together:

| Package | Version | |
|---|---|---|
| SF6 Slots Loader | 1.10.0 | everything of 1.9.3 beta and beta 2 below, plus the two changes of this section |
| SF6 Color Mod | 0.19.4 | first public release, needs the Native Core |
| SF6 Native Core | 0.7.1 | first public release |

Install the Slots Loader alone as before, or add the Native Core and the Color Mod. Updating the Slots Loader in
Fluffy Mod Manager: disable or remove the previous version first.

### Slots Loader
- **Displays through the SF6 Native Core when it is installed** (character select data, stage select, VS screen):
  the same screens as before. Without the core it does it itself, as in 1.9.
- **The slot and colour a fighter really wears under the online alias are declared to the core**, so the Color
  Mod's Drive colour squares follow the slot instead of Drive Tech's.

### SF6 Color Mod 0.19.4 (new)
- Extra costume colours in the Color row of the character select and of Fighter Settings.
- A colour editor in the game's option window: Fighter Settings > Edit Character, **F** or a click on the Color row.
- Drive Impact, Drive Reversal, Drive Parry and Drive Rush colours per character and per player: **X** on the
  character select and in the Training character popup, four rows in Fighter Settings.
- One REFramework plugin, no Lua script. See [colormod/README.md](colormod/README.md).

### SF6 Native Core 0.7.1 (new)
- The shared native-UI core: display primitives, screen discovery, the game's option window as a service, the
  effective selection shared between mods, fault isolation per mod. See [core/README.md](core/README.md).

### Known issues
- FPS drops have been seen with the Native Core and the Color Mod installed; being measured.
- Color Mod, Battle Settings: after leaving and coming back, a Color Mod colour (`MC 2` for example) shows in the
  Color row but the model wears the game's colour of the same number, and an online match can load that colour.
  Selecting another colour and back shows the right one.

## 1.9.3 beta 2 (2026-10-04)

### Fixed
- **A real Drive Tech picked on a select screen is no longer dressed as your Battle Settings slot.** A slot
  chosen in Battle Settings is shown in place of Drive Tech wherever the game takes the costume from Battle
  Settings (online, Battle Hub cabinets). It was also done in Arcade, Training, Versus and Team Battle, where
  you pick the costume yourself: Sagat's Drive Tech picked in Training showed his slot.

### Added
- **A mod's own costume scene is used whole when it changes more than paths of the same length**, as long as
  it is made for the current game version (same layout as the game's scene) and every file it names exists.
  A mod can now give a mesh to a part the game's scene leaves empty (Sagat's Drive Tech, a body split in two).
- **A mod scene's joint and chain files are followed too**: a mod that takes a head or a hair from another
  outfit gets that outfit's joints and hair physics with it.

## 1.9.3 beta (2026-10-04)

A beta: the changes below are tested on one installation only.

### Added
- **Each costume mod installed with Fluffy Mod Manager gets a slot of its own**, even two mods of the same
  outfit. Fluffy copies the mods it installs into the game, so two mods of one outfit ended up as one; mods
  installed as loose files (with REFramework's loose file loader on) got no slot at all. The loader now
  finds Fluffy, reads its list of installed mods and takes each one from its archive in Fluffy's
  `Games\SF6\Mods` folder. Unticking a mod removes its slot at the next launch. If Fluffy is not found,
  its folder can be given in `reframework\costume_mods\settings.ini`. The outfit a Fluffy mod replaces
  still shows that mod. See [docs/FLUFFY.md](docs/FLUFFY.md).

### Fixed
- **A slot no longer stalls the select screen, or shows white colours, when the game's pak lacks a colour
  file of its outfit** (seen on an installation whose pak had no Mai Outfit 1 colours 2 to 5, 7, 8 and 10).
  The loader takes such a file from the game folder when it is there (where Fluffy Mod Manager puts the
  files it installs), otherwise gives the slot another colour of the outfit.
- **A mod's texture in the shared body folder keeps its own high-resolution version.** The slot got the
  mod's texture with the game's high-resolution one, and the game waited for ever when the outfit loaded.

### Changed
- The slot check reports cloth that cannot move: a mod made for an outfit with cloth simulation on an
  outfit without, and a cloth simulation that uses another character's file (`WARN` lines in
  `SF6_CostumeLoader.log`).
- Mods with exactly the same files and sizes (variants of one design) are told apart in the log.
- `SF6_CostumeAudit.bat` lists the mods ticked in Fluffy Mod Manager with the slots made from them.

## 1.9.2 (2026-10-04)

### Fixed
- **A slot shows its colours and its cloth physics in Battle Settings.** Opened from the main menu, Battle
  Settings showed a slot white and with its cloth frozen: the slot's colour list and cloth settings were
  loaded too late for that screen. A slot's scene now loads them with the scene, like the game's own outfits.
- **Slots made from Outfit 1 of JP, Dhalsim, Lily or Guile are no longer white** (since 1.9.0). The DLC paks
  hold an older colour list for these four outfits, which the game cannot read; the slots were built from it.
  The DLC paks now only complete the base pak.
- **Dhalsim's Outfit 4 keeps its own body when Dhalsim has two slots or more.** That outfit takes a part from
  a model folder the loader believed free; a slot was placed there and replaced it. Every folder the game uses
  is now taken, and a slot already placed in one moves to a free folder (it keeps its number and its name).
- **A mod's joint constraints, shapes and AO geometry are used by its slot** (`_jcs.user`, `_shape.user`,
  `_AOgeo.user`, and the `.jcns` files a scene names): the slot kept the original outfit's.
- **The online alias dresses your own fighter only.** An opponent playing, in Outfit 4, a character you had
  chosen a slot for was shown with your slot. Nothing is aliased any more while Battle Settings is open
  either (previewing Outfit 4 there mounted the slot over it).
- **Starting the game twice in a row no longer breaks the paks.** The first launch after a change of mods
  rebuilds the costume pak for about a minute without a window; a second launch during that time numbered
  the paks 003 and 004 with no 001, which the game does not read (black screen). The second one now waits.
- **A stage pak that no longer matches its table is rebuilt** instead of being trusted (a selected stage
  variant never finished loading).

### Changed
- The slot check no longer warns about a colour list a mod ships itself, and reports a material file that has
  materials its mesh lacks (`WARN` lines in `SF6_CostumeLoader.log`).
- Mod archives unpacked under very long paths are found.

The costume pak and the stage pak are rebuilt once at the first launch (about a minute: launch the game once
and wait).

## 1.9.1 (2026-10-02)

### Fixed
- **A Training Room variant of Stage Lighting Overhaul no longer hangs the game on its way to Fighting
  Ground.** Its stage pictures (the preview, 1920x480 with 9 levels) declare a wrong row size for their three
  smallest levels; the game waits forever for such a texture, and the Fighting Ground menu loads Training
  Room's pictures. The stage loader now checks every texture of a stage mod and repairs its level table the
  way the costume loader repairs costume textures; a texture it cannot repair is left out of the variant (the
  game's own is used). The stage pak is rebuilt once at the first launch.

## 1.9.0 (2026-09-30)

### Changed
- **No more Lua scripts.** Everything in game is done by the native plugin `SF6_CostumeSlotsNative.dll`: the
  costume menus, ownership, colours and select screen data, the online alias, the stage select screen and the
  VS screen. It is driven by the game's own events (a menu shown or closed, an outfit folder mounted, the stage
  select screen shown, UP / DOWN) instead of looking at the game every frame: during a fight it does nothing.
  An outfit mount is seen when the game creates the outfit's visual manifest; the online alias then checks
  for two seconds, during the loading screen, and stops. Played online on 2026-09-30.
  An update made by hand leaves the old scripts in `reframework/autorun/`: the plugin renames them to
  `.lua.old`.
- The loader's stage watcher sleeps until the stage choice file changes (a change notification) instead of
  looking at it four times a second, and the stage redirection no longer counts the files it sees.

### Added
- **Stage slots (experimental).** Stage mods dropped as downloaded in `reframework/stage_mods/` (`.zip`,
  `.7z`, `.rar`, a folder with a `natives` tree, a `.pak`) become variants of the stage they change. On the
  stage select screen of Fighting Ground > Versus, UP / DOWN cycles a stage through its original look and
  each mod; the name and a preview made from the mod's screenshot follow, the VS screen shows the choice, the
  battle loads it, and the choice is kept per stage. A pack covering many stages gives one variant per stage
  and option. The mod files go into a patch pak of their own, below the costume pak, and are served in place
  of the original files only while selected, through one hook on the engine's path hashing that is installed
  only once a variant is selected. The choice is local and applies to every load of the stage, online too;
  online play has not been tested. Details and limits: [stages/README.md](stages/README.md).
- `SF6_CostumeLoader.log` names every file of a mod that its slot does not show
  (`WARN: the slot loads ... from the game`): what to send when a part of a mod is missing.

### Repository
- **Renamed SF6-SlotsLoader** (was SF6-CostumeSlotsLoader; the old links lead here). It now also holds the
  stage slots (`stages/`): on the stage select screen, UP / DOWN cycles a stage through its mods. They ship in
  the same `amd_ags_x64.dll` (experimental, see *Added*). Layout: `costumes/`,
  `stages/`, `common/` (pak and archive code, third party), `proxy/`, one `build.bat` at the root that builds
  everything into `build/`, including the costume loader alone (`build/costumes_only/`) for the releases.

### Fixed
- **The stage slots' pak is not taken for a mod pak.** It sits right below the costume pak; adding or removing
  a stage mod rebuilt the costume pak (about 50 s with many mods). It is now recognised by its marker and left
  out, the costume pak stays above it, and when only its presence changed the costume pak is moved instead of
  rebuilt. The first launch after updating rebuilds the costumes once.
- `outfits.json` (read by the audit) follows the costume pak when it changes number.
- **A mod's own Havok cloth is used by its slot.** A slot kept the game's cloth file of the original outfit on
  the mod's body (Lily Kenyan Summer C2); it now gets its own copy pointing at the mod's cloth, as for chain
  physics. The model folder's havok references are no longer renamed into files that exist nowhere.
- **Weapons show on the character select screen.** The slot's weapon list was loaded late: the first player to
  pick the outfit had no weapons there (Lily Kenyan Summer C2). Both of the scene's references now point at the
  slot's list.
- **Parts a mod retextures without their material are shown.** A material of the outfit that the mod does not
  ship, but that uses textures the mod ships, kept the game's textures: C. Viper Lace lingerie C2's head,
  the hood of Lily Haruka Hoodie (her hair material uses the body's cloth), Oni Juri's eyes (shared `000/`
  folder). The slot now gets its own copy of that material, bound to the mod's textures. The mod's files stay
  out of the original outfit, which stays as the game made it.
- **A mod's wind settings are used by its slot** (`weather/wind/.../*_chain_BattleSetting.chain`: Swimsuit
  C. Viper, C. Viper Concept Outfit). The scene now follows every file the slot has under its own name.
- **The DLC paks are read.** The game reads them over `re_chunk_000.pak`, for the players who own them: they
  hold newer colour files of the Outfit 1 of JP, Dhalsim, Lily and Guile. The slots were built from the older
  ones; they are now built from what the game reads, and buying or removing a DLC rebuilds them.

## 1.8.1 (2026-09-30)

Found by testing with a large set of Cammy, C. Viper and Ingrid mods installed together.

### Fixed
- **A slot no longer takes files from another mod.** The colour files and the colour-variation data a mod does
  not ship, and the high-resolution (streaming) version of a texture, were looked up among the files of all
  installed mods. Several mods ship textures of the same name for the same outfit (five for Cammy's Outfit 1),
  so an outfit got the colours or the pictures of another one, and which one depended on the other mods
  installed: Escape from Shadaloo lost its green, Alt Bare Legs its jacket and glove colours. A slot now uses
  its own mod's files, else the game's, as when the mod is installed alone.
- **Texture references written with doubled slashes (`002/01//Knit_CMASK.tex`) or backslashes
  (`product\model\...`)** now follow the texture to its slot. Before, the outfit waited forever for a texture
  that had moved (Camo Cammy showed the original outfit, then froze; C. Viper Trench Coat froze the select
  screen).
- **Files placed directly in `product/model/`** (Dance Outfit for Cammy names a `1.tex` there) are taken from
  folder mods; the game keeps none there, so nothing can be overridden.
- **Material files made for an older layout** are rebuilt on the game's file for that part: when every
  material has the game's name and master material but exactly the parameters the game had minus those it
  gained since, the game's file is used, with the mod's texture bindings, rendering flags and parameter
  values (Dance Outfit for Cammy showed a dark body and opaque fabric; it shows the sheer dancer). Measured on
  41 material files of a large collection: only that one matches.
- **A part hidden on purpose stays hidden.** A mod that hides a face or hair with a reduced mesh naming a
  material no file defines (AoD replace Ingrid) no longer gets the original part back, and such an outfit is
  no longer left out.
- **A partial add-on brings its parts to a costume put in the same sub-folder**, even when it was made for
  another outfit's folder (BellyHair for Dance Outfit: hair in Outfit 1's folder, costume of Outfit 2). Only the
  combined outfit is made when both are in one sub-folder of the character; archive bundles keep the base and
  each variant. Note: Ken SFV with Ken SFV Hair in one sub-folder now gives only the combined outfit.

### Script
- The script does nothing while a fight is running (match, replay, training outside the pause) when the
  native plugin is there. The signal is the fight clock (`gBattle.Game.stage_timer`) moving; it stops in
  pause, menus and loading, and the checks come back at once. Before, the save, the costume ownership and
  the colour clean-up were polled every frame, about 3 fps on a CPU-bound laptop during a replay; now 8 us
  per frame. Never while the Battle Settings menu is open (its closing must be handled), and without the
  native plugin the script keeps its in-match role.

## 1.8.0 (2026-09-29)

- A part the mod made for an older layout of the game is taken over when the outfit's scene takes that part
  from another costume's folder (Aria for Ingrid: face and hair). The slot gets the mod's part with the
  original material file of the place it replaces; a material the part names that no file defines is
  reported in the log and kept as the mod made it. To confirm in game.
- A mod with a real costume (ten files or more) and a minor entry next to it (two files or less, no
  texture) no longer gives that entry as an outfit (Athena for Ingrid gave a second, almost original
  Drive Tech outfit).
- Other slots are unchanged.

## 1.7.0 (2026-09-29)

### Colour swatches (experimental)
- The two squares shown next to each colour in the costume menu are computed for every modded outfit: the
  tints of its two largest colour zones, from the mod's own colour files and colour masks, instead of the
  original outfit's squares. Capcom picks its squares by hand, so the result looks like the outfit without
  matching what Capcom would have chosen. This is a trial and may be removed. Outfits whose materials do
  not use the game's colour system keep the original squares.

### Compatibility
- A mod that ships its own costume scene and points a part at another folder gets that part: Mummy Dhalsim
  showed the original head. Followed only when the file exists and the mod's scene matches the game's
  (same model references, same path length); otherwise the log says it was not followed.
- Textures whose small mip levels were sized with fractional blocks are repaired as a whole (Feixue for
  Mai lost its body and face). The mip table is rewritten only when the one computed from the format
  fills the file exactly.
- A part hidden behind a stripped mesh of another part keeps that mesh, paired with a material file that
  defines its materials, instead of getting the original part back (Mai's hair on Feixue and SuiSui) or
  being left out (Changli).
- Physics files kept next to the parts, where the game keeps them, are followed: the slot's physics
  settings point at the mod's files (Chique Casual Ingrid's hair). Every mod laid out that way now uses its
  own physics.

### Building
- `build.bat` finds Visual Studio or the Build Tools wherever they are installed.
- `loader/third_party/bcdec.h` (MIT or public domain) decodes the colour masks.

## 1.6.1 (2026-09-27)

- `reframework\costume_mods\SF6_CostumeAudit.bat`: lists what is installed (loader, REFramework, script,
  plugin), what the last launch did, the costume mods found with where they come from (Fluffy pak,
  archive, folder) and the outfits each one gave. Read-only; the report is saved next to it.
- The loader writes `reframework\data\SF6_Costumes_Data\outfits.json` at each generation, the list the
  audit reads.
- Notes or other files dropped in `costume_mods` no longer make the next launch rebuild everything; only
  archives, `.pak` files and folders count.
- A mod without a name in its `modinfo.ini` is named after its archive or folder in the log.
- Documented: a costume and an add-on from two archives are combined when put in one sub-folder of the
  character.

## 1.6.0 (2026-09-27)

### Installing mods
- Costume mods are read as downloaded: `.zip`, `.7z` and `.rar` files in
  `reframework\costume_mods\<Character>\`, unpacked once into a hidden cache.
- Bundles give one outfit slot per option; add-ons (no gloves, other hair, a weapon) are combined with
  the outfit they complete: up to three add-ons, one variant each, plus one with all of them when they
  do not replace the same files.
- Identical files shared by several variants are stored once in the generated pak.

### Mods made for older versions of the game
- Colour files whose type signature predates a game update are brought up to date
  (DOA4 Christie, TFD Racer showed up white).
- Colour files in the 2023 layout are converted to the current one (Ken SFV style).
- Textures whose mip table declares a wrong row pitch are repaired (Vegeta Majin).
- Material references are renamed together with the textures they point at (white Specter Zangief).
- A part shipped without a material gets the original material of that part only (see-through Lily).
- Weapons and props listed in `model_parts` follow the slot (Lily's clubs).
- A model folder shared by two outfits is mapped to the right one (Mummy Dhalsim).

### Loading safety
- Every slot is read back the way the game will load it before the pak is written: files present,
  every mesh material defined by its material file, every texture present and consistent. Broken parts
  are rebuilt from the mod's shared part or the original outfit; a slot that still fails is left out
  and named in the log.
- Files only a mod has, stored in another outfit's folder, are kept (Juri stalled with two mods).
- Only the slots in use are declared to the game (a character on trial listed 100 empty outfits).
- The pak is really written or nothing changes: a game still closing no longer leaves an old pak
  marked up to date.
- Removing a mod no longer stalls the character select: every unused slot number shows the
  character's Outfit 1, for a choice saved by the game or a replay that still names it.

### Modular mods and variants
- Modular mods built for Fluffy are installed the way Fluffy does by default: one outfit per shape
  option, first texture option, optional parts left out (C. Viper went from 134 entries to 18).
- Options that replace the same files no longer share a slot; identical outfits are installed once.
- A character that runs out of its 100 slots is named in the log.
- A mod that only changes a part of an original outfit (glasses, earrings) gives that outfit with
  the change, in a slot of its own, instead of being ignored.

### Names
- Outfit names follow each other per character (Outfit I, II, III) after a mod is removed; each slot
  keeps its number behind the name, so saved choices and replays stay valid.

### Log
- Each slot line shows its outfit name (BrewedVFX).
