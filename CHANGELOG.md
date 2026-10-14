# Changelog

## Unreleased

### SF6 Slots Loader
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
### SF6 Native Core 0.8.0
- A client-texts API (`txt_new`, `txt_set`, `txt_release`, `obj_set_guid`, `txt_status`) existed in earlier 0.8.0 builds as
  increment 8; it had no client and was removed before release. The pause menu is increment 8 and its functions moved up
  (`pm_set_callbacks` is now the 77th member of the table): **a client built against an earlier 0.8.0 header must be
  rebuilt.** Increments 1-7 did not move.
- **Training pause menu** (ABI increment 8, appended): `pm_set_callbacks`, `pm_page`, `pm_row`, `pm_set_texts`,
  `pm_set_options`, `pm_remove`, `pm_page_shown`, `pm_row_part`, `pm_status`, `pm_row_of` (the client row a
  `hints` callback's focused menu data belongs to, so a hint can show on a mod's own rows only). A mod adds its own pages to the training
  pause menu (a sub-page next to the game's ones, e.g. in Recording Settings) with buttons and spins; the core builds
  them as the game's own menu data, answers the game for the mod's rows only (the game's rows are left to the game),
  renders the spins (up to 64 options, labels changed in place, the arrows set apart and long labels shrunk on request)
  and adds the mods' key-guide hints to the bar at the bottom. One engine for every mod; nothing is hooked until a mod
  uses it, and every method is checked first for machine code it shares with other methods of the game: such a method
  is not hooked (logged), so the service cannot slow the game down the way a shared hook does. The check runs on a
  thread of its own (the game never waits for it) and its result is kept per game version
  (`reframework/data/sf6core/pm_codecheck.txt`): the next sessions skip it. See
  [core/docs/PAUSE_MENU.md](core/docs/PAUSE_MENU.md).
- **The game's own dynamic rows of the training pause menu are kept**, such as "End Training Session" in the Battle Hub
  training room: the clean-up at each build of the option window removed every row of that kind; it now removes only
  rows the core built and no longer uses.
- `docs/ABI.md` lists the functions of increments 6 (option dialogs) and 7 (effective selection) too.
- Tested in the game: the first test of the training pause menu found two faults, both fixed: its
  pages were never built (the core called a function of REFramework's plugin interface that the REFramework most
  players run does not have; the arrays are now made through the game itself), and the game froze about 6 seconds
  when the first mod used the service (the method check, see above). The service's fault lines now say where they
  happened (step, game call, address, thread), and `reframework/data/sf6core/pm_trace.txt` turns on a step-by-step
  log of its next install and page build. The second test showed the test page, but its spin kept showing its first
  option while its value changed, and the game crashed a few page switches later. Fixed since: the spin is repaired
  after each of the game's layouts of it (nested ones included); it starts from its value at the first build of a menu
  too; the page's answer to the game is no longer
  overwritten by its rows' answers; a short option text keeps the game's font (written only to shrink a long one); the
  option carousel is never grown. The third test (spin fine) crashed the game again while switching the Recording
  Settings sub-pages very fast, and again with one hook at a time; never with the Lua engine this service was ported
  from, nor without a page of ours. Since then the service also waits, as that engine did, on the menu's own navigation
  and on the game's text lookup while a page is being built, and writes nothing into the game's view data beyond what
  that engine wrote. `reframework/data/sf6core/pm_bisect.txt` turns its behaviours off one by one in the game.
  The likely cause, found after the fifth test: the core's reference writes (`obj_set_ref`) always released the field's old
  value, but the game writes a local object (not refcounted yet) with a plain store, its fields own no reference. The
  spin's option written at each page build into such an object (the ViewData the game hands to `Function`) released
  the option shown before, a reference it never held, until that option was freed while the menu still pointed at it.
  `obj_set_ref` now follows the target's reference count as the game does (this also applies to the other mods' writes
  through it). Confirmed by the sixth test: the view data is such a local object, and the test page went through
  several hundred very fast sub-page switches, spin changes, label and text changes, a deliberate fault in its
  callback (caught, the game went on) and its removal without a crash.
- **REFramework 1.5.8 (plugin API 1.10)**: element hiding (`set_element_hidden`) needs `on_pre_gui_draw_element`, which
  only exists from plugin API 1.14 on. The core registered it on any REFramework, reading past an older one's
  function table; it now checks the API version first and keeps element hiding off (one log line) below 1.14. No mod
  of this suite uses element hiding.
- **Method lookup on types with more than 64 methods** (bug): the core's by-name method lookup (used by every display
  write: `get_Message`, `set_Message`, the style setters of `via.gui.Text`, `ui_clone_row`...) passed REFramework's
  `get_methods` a buffer size in elements where it expects bytes, so room for 64 handles only. A type with more
  methods answered "too small" and that level of the type chain was skipped silently: a lookup that depended on such
  a level failed. The buffer is now sized from the type's method count, results (also "not found") are cached per
  object type and signature, and the log says once per type and signature when a method is found only thanks to the
  fix (`[lookup] ... found by name in a type with N methods`) or not found at all (`[lookup] ... no method ... by
  name`).

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
