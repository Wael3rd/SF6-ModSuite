# SF6 Mod Suite

Street Fighter 6 mods by Wael, built and released together. Each release carries four Fluffy Mod Manager
packages:

| Package | What it does | Needs |
|---|---|---|
| **SF6 Slots Loader** | costume mods become **extra** outfit slots, the original outfits stay; stage mods become variants of their stage (experimental) | REFramework |
| **SF6 Color Mod** | extra costume colours, a colour editor, Drive colours per character and per player, all from the game's own menus: [colormod/README.md](colormod/README.md) | REFramework, SF6 Native Core |
| **SF6 Native Core** | the shared native-UI core the mods display through: [core/README.md](core/README.md) | REFramework |
| **SF6 Recording Manager** | (CRSM) 30 recording slots per character in Training, loadouts, a reversal picker and online sharing of recordings, all in the game's training pause menu: [crsm/README.md](crsm/README.md) | REFramework, SF6 Native Core |

Install what you want: the Slots Loader works on its own; the Color Mod and the Recording Manager need the Native Core of the same
release. Each release also carries `SF6_ModSuite_AllInOne_v<version>.zip`, the four packages in one: install either it or
the separate packages, never both (two packages shipping the same files conflict in Fluffy Mod Manager).

This repository was named SF6-SlotsLoader until 07/10/2026 and SF6-CostumeSlotsLoader until 30/09/2026; the old
links lead here.

## Recording Manager

`SF6_CRSM_<version>.zip` (the Recording Manager, CRSM) adds slots 9-30 to the game's 8 recording slots in Training,
loadouts, slot names, a reversal picker and Community sharing of loadouts, all in the training pause menu (Recording
Settings > Manager / Record / Replay). Since 2.0.0 it is one native plugin, `reframework/plugins/SF6_CRSMNative.dll`,
with no Lua script.

- Install the SF6 Native Core of the same release with it.
- **Remove the Lua version (1.x) first**: delete `SF6_RecordingManager.lua`, `SF6_RecordingSlotManager.lua` and the
  `crsm` folder from `reframework\autorun\` (and `CRSM_REFrameworkUI.lua` if you added it). While one of them is there
  the plugin leaves the menu to it.
- Your 1.x recordings, loadouts and names are kept: they stay in `reframework\data\SF6_RecordingSlotManager_data\`,
  the same folder as 1.x, and no package overwrites it.
- **Community** (since 2.1.0): sharing and liking loadouts need an identity verified by Steam, obtained by the plugin
  in the background (nothing to click). The server keeps your Steam account id for moderation; lists only show your
  CFN name, with `[V]` for a verified share. Version 2.0.x can browse and download, not share or like.
- The zip's `README.txt` has the details (settings, logs, the emergency stop). The source is in `crsm/`;
  [crsm/README.md](crsm/README.md) also says what the plugin sends to the Community server.

The rest of this page is about the Slots Loader.

## Costume slots

Street Fighter 6 costume mods replace an existing outfit: install a Ryu mod and Outfit 1 is gone until you
uninstall it. This loader turns every installed costume mod into an **extra** outfit slot and puts the original
outfit back, so Outfit 1 and the mod live side by side in the costume spinner.

Install it once. After that nothing changes in how you use the game: install costume mods the way you already
do, start the game, and the new outfits are there.

**Only the outfits you own get extra slots** (since 1.11.0). A mod made for a fighter's Outfit 2, 3 or 4 shows only
when the game counts that outfit as yours: a default outfit, one bought with in-game currency or in the shop, or one
given by a DLC. Mods made for Outfit 1 or the Drive Tech outfit always show. Whatever makes the game say yes counts,
an unlock-all script included. The check is made offline and kept until the game closes, so nothing changes during an
online session. `reframework\data\SF6_CostumeSlots_data\native_log.txt` has one line per hidden slot.

**Game updates** (since 1.11.0): when an update adds a fighter or an outfit, the loader picks it up from the game by
itself at the next launch; no new Slots Loader release is needed. Run the clean-up below first (Patch day).

## Install

Grab `SF6_SlotsLoader_v<version>.zip` from [Releases](https://github.com/Wael3rd/SF6-ModSuite/releases), install it in
Fluffy Mod Manager or copy it over your Street Fighter 6 folder, then install costume mods as usual. You need
REFramework in `dinput8.dll`, official build 1.5.8 or newer. For the Color Mod or the Recording Manager, install
`SF6_NativeCore_<version>.zip` and `SF6_ColorMod_<version>.zip` / `SF6_CRSM_<version>.zip` from the same release. Or
take `SF6_ModSuite_AllInOne_v<version>.zip` alone: it holds the four packages.

Costume mods can also go straight into `reframework\costume_mods\<Character>\`, **as downloaded**: the
`.zip`, `.7z` or `.rar` file itself, or an unpacked folder with a `natives` tree or a `.pak`. Nothing to
unpack, nothing to convert.

**Stage mods** (experimental, since 1.9.0) go into `reframework\stage_mods\`, as downloaded too. On the stage
select screen of Fighting Ground > Versus, UP / DOWN cycles a stage through its original look and each mod
made for it. Without stage mods, no stage is changed and nothing is hooked. See
[stages/README.md](stages/README.md).

Updating in Fluffy Mod Manager: disable or remove the previous version first. Since 1.9.0 the mod is named
SF6 Slots Loader.

### Costume mods installed with Fluffy Mod Manager

Each costume mod ticked in Fluffy gets an outfit slot of its own, even when two of them replace the same
outfit. Fluffy copies the mods it installs into the game (loose files in `natives\` when REFramework's
loose file loader is on, a pak otherwise), so two mods of one outfit end up as one. The loader reads
Fluffy's list of installed mods instead and takes each mod from its archive in Fluffy's
`Games\SF6\Mods` folder. Unticking a mod removes its slot at the next launch.

The loader looks for Fluffy in Documents, Desktop, Downloads, the game folder and the root of each drive.
If yours is elsewhere, give its folder (the one with `Modmanager.exe`) in
`reframework\costume_mods\settings.ini`:

```ini
FluffyModManager=D:\Tools\FluffyModManager
```

The outfit a Fluffy mod replaces still shows that mod (the last one installed for it): the game reads
Fluffy's copy over everything else. To keep the original outfit as well, put the mod in
`reframework\costume_mods\<Character>\` instead of ticking it in Fluffy.

More in [docs/FLUFFY.md](docs/FLUFFY.md): checking it worked, log lines, what to send when reporting.

## Patch day (after a game update)

The slots exist as patch paks the loader writes into the game folder at launch. If the loader is turned off
before Fluffy Mod Manager re-reads the game archives, those paks stay and Fluffy records them as game files
(the slots then show even with every mod off). So, in this order:

1. The game updated: **before** letting Fluffy re-read the archives ("EXE changed") and before turning mods off,
   close the game and run `SF6_SlotsClean.exe` (double-click, in the game folder; `--dry-run` only lists). It removes
   the loader's runtime paks and state files, and nothing else: `reframework\costume_mods`, other mods' data and
   Fluffy's files are not touched.
2. Let Fluffy re-read the game archives.
3. Launch the game: the loader rebuilds the slots from `reframework\costume_mods`.

## Checking what is installed

Double-click `reframework\costume_mods\SF6_CostumeAudit.bat`. It changes nothing: it checks that the loader,
REFramework and the native plugin are in place, says what the last launch did, lists the
costume mods it finds (Fluffy Mod Manager paks in the game folder, archives and folders in `costume_mods`)
with the outfits each one gave, then the extra outfits by character. The report is saved as
`SF6_CostumeAudit.txt` next to it: send it with `SF6_CostumeLoader.log` when reporting a problem.

## Archives, bundles and add-ons

- **Archives** are recognised by their content, not their name, and unpacked once into the hidden
  `costume_mods\.cache` folder, then reused until the archive changes. Only what the game needs is
  kept. The first launch after adding a large archive takes a few extra seconds; the next ones don't.
- **Bundles**: an archive or folder holding several options gives one outfit slot per option.
- **Add-ons**: an option that only changes a part (no gloves, other hair, a weapon) is combined with the
  outfit it completes, the way Fluffy Mod Manager installs them together. The loader reads Fluffy's
  `addonfor` and bundle names; without them it matches options of the same archive that touch the same
  outfit. Each outfit gets one variant per add-on, plus one with all of them.
- **Combining two downloads**: a costume and an add-on published separately (a hair, a weapon) are combined
  when both archives sit in one sub-folder of the character, `costume_mods\<Character>\<any name>\`. Only
  the combined outfit is made, not the costume alone as well. Several add-ons downloaded separately (a cat
  and a book, one archive each) in that sub-folder give one outfit wearing all of them, as long as they do not
  replace the same files (otherwise one variant per add-on). An add-on made for another outfit's folder than
  the costume's (a hair for Outfit 1 next to a costume of Outfit 2) still brings its parts to the costume,
  as long as the costume has none of that part.
- **Modular mods** (main files, body options, skin options, optional extras) give one outfit per body
  option with the default skin, as Fluffy installs them by default.
- A mod that only changes a part of an original outfit (glasses, earrings) gives that outfit with the
  change, in a slot of its own.
- Variants share most of their files, and the generated pak stores identical data only once.
- Broken, password-protected or unrelated archives are skipped and reported in `SF6_CostumeLoader.log`.

## NSFW outfits

NSFW costume mods go in a folder of their own, `reframework\costume_mods_nsfw\<Character>\`, instead of
`reframework\costume_mods\<Character>\`. The loader makes that folder (with a folder per fighter) next to
`costume_mods`; the Slots Loader package ships it too. It takes the same things, with the same rules: mod folders,
archives, sub-folders, paks, modular mods and runtime options (`costume_mods\settings.ini` applies to both folders).
Left empty, it changes nothing.

Mods left in `costume_mods` are looked at too, by name (creators tag their mods, or put "nsfw" in the names):

- **A mod whose name says it is NSFW** (its archive, folder or pak name, the folder around all its options, its
  Fluffy menu name, the `modinfo.ini` name of a one-option mod) is an NSFW mod, exactly as if it were in
  `costume_mods_nsfw` (hidden, numbered after the others, never mixed).
- **An option whose name says so** (its `modinfo.ini` name, its folders, the Fluffy menu groups it hangs from, such as
  "4! NSFW style options") is an NSFW option of an outfit that stays shown: while NSFW outfits are hidden, it is not
  offered in the costume options menu and not worn; your choice of it is kept and comes back when they are shown. The
  outfit never wears one by default when its group can do without it (else the outfit itself is NSFW). With
  `RuntimeOptions=off` or `variants`, an outfit made with an NSFW option is an NSFW outfit.
- **The words** (Slotsloader Manager 1.0.8's NSFW filter, by ZZtaii): `nsfw, nude, naked, topless, bottomless, pubic,
  pubes, nipple, pasties, lewd, futa, see through, see thru, no panties, no underwear, erotic`. Case, spaces and dashes
  do not matter ("See-Through" = "seethrough"), but a word must appear as one run ("no panties" is not found in
  "Normal panties"); clothing words (bikini, lingerie, thong) are not in the list. To change it, add a line to
  `costume_mods\settings.ini`: `NsfwKeywords=nsfw, nude, lingerie` replaces the list; `NsfwKeywords=` (empty) turns
  the detection off. `SF6_CostumeLoader.log` names every mod and option it caught (lines `nsfw: ...`, with the word
  and where it was found) and the count.
- No `modinfo.ini` field says a mod is for adults (Fluffy's fields are name, version, author, category, description,
  screenshot, AddonFor, NameAsBundle; Nexus's adult flag is not in the downloads), so only names count.

- **Hidden by default.** NSFW outfits are listed only while "Display NSFW Outfits" is on. For now the setting is in
  `reframework\data\SF6_CostumeSlots_data\settings.json`: `"display_nsfw": true` shows them, `false` hides them
  (the file is created, set to `false`, the first time the plugin finds an NSFW outfit or option). A change takes
  effect at the next screen that lists outfits (character select, Battle Settings), without a relaunch; the outfits
  stay installed either way. An in-game switch is not available yet.
- **Hidden** means not listed by the character select screens or Battle Settings / Fighter Settings, like an outfit
  you do not own. A fighter whose Battle Settings outfit is an NSFW one goes back to its Outfit 1 when they are
  hidden.
- **Numbers**: a fighter's NSFW outfits come after its other outfits in the costume list (ten numbers further, so
  outfits added later to `costume_mods` still come before them), and keep their numbers from one launch to the next.
  A mod moved from one folder to the other, or that becomes NSFW by its name (or stops being NSFW), is numbered again
  once; a Battle Settings choice of it follows it to its new number. Other outfits keep their numbers.
- **Kept apart**: an NSFW mod is merged with, or made an option of, other NSFW mods only. An NSFW add-on is an option
  of NSFW outfits only (never of the game's outfits); an NSFW retouch stays an outfit of its own; the add-ons of
  `costume_mods` never go on an NSFW outfit. The Slotsloader Manager does not list the mods of `costume_mods_nsfw`.

## Colour swatches (experimental)

The costume menu shows two small squares next to each colour. Capcom picks them by hand for its own outfits,
so they cannot be read from a mod. Since 1.7 the loader computes them for every modded outfit: the tints of
the outfit's two largest colour zones, taken from the mod's own colour files and colour masks. Measured on
the game's own outfits, the result looks like the outfit (vivid colours, the right family most of the time,
sometimes the other way round) but is not what Capcom would have picked. This is a trial and may be removed.
An outfit whose materials do not use the game's colour system (a full character replacement) keeps the
squares of the original outfit, as before 1.7.

## How it works

The game imports `amd_ags_x64.dll` from its own folder, so a proxy with that name is mapped into the process
before any engine code runs. That is the moment the loader does its work:

1. It scans the installed costume mods: the mods Fluffy Mod Manager installed (each one from its archive),
   the patch paks and loose files in the game folder, and the archives and folders under
   `reframework/costume_mods/`.
2. It rebuilds a patch pak that relocates each modded costume to a free slot, from 5 to 104, which is
   **100 extra slots per character**, and restores the outfit the mod had overwritten from the base pak. The game
   files that declare the slots (its costume table, version scenes and outfit names) are built from the game's own,
   so the fighters and outfits of the installed game version are always there.
3. Colours, physics chains, shared body parts, weapons and the `streaming/` high resolution textures follow the
   costume to its new slot. Files made before a game update are brought up to date on the way (see below).
4. It reads every slot back the way the game will load it, repairs what cannot load, and leaves out a slot it
   cannot repair instead of letting it stall the game. Only the slots in use are declared to the game.
5. It writes a registry that the native plugin reads to make the slots selectable (those of the outfits the player
   owns) and, online, to show the other players a legal outfit. The registry also carries each slot's colour swatches.

A fingerprint of the installed mods is kept, so a launch with nothing changed costs about 20 ms. A launch after
installing or removing a mod takes a few seconds, up to about fifteen with many large archives.

## Compatibility

Mods published since 2023 were made for older versions of the game. The loader recognises the known
differences (old texture suffixes, colour files in older layouts, broken texture headers, parts whose
materials no longer match) and fixes them in its own pak, without touching the mod files.
[COMPATIBILITY.md](COMPATIBILITY.md) lists what it fixes and the mods tested; [TESTING.md](TESTING.md)
lists what still needs testing.

Online, the extra slot is aliased to one of the character's own outfits for the other players (DriveTech, or
Outfit 1 for a character without it, such as Arjun), so matchmaking stays valid.
Since 1.9.0 everything in game is done by one native REFramework plugin (no Lua script), driven by the game's
own events: menus shown or closed, an outfit folder mounted, the stage select screen. A fight triggers none of
them, so the plugin does nothing during a fight. Official REFramework stops Lua scripts during online matches,
not native plugins. See [plugin/README.md](plugin/README.md), and
[docs/REFRAMEWORK_COMPATIBILITY.md](docs/REFRAMEWORK_COMPATIBILITY.md) for why every DLL of the suite works online
and on every REFramework build.

## Repository layout

```
build.bat            builds everything into build/
common/              shared by both loaders: KPKA pak reader and writer, archive reading, third party code
proxy/               the AGS proxy: one DllMain for the stage and costume passes
costumes/loader/     costume loader: mod detection, slot relocation, scene and material patching
costumes/tools/      generators for the data files, run against your own game installation
                     slots_clean.cpp: SF6_SlotsClean.exe, the patch-day clean-up
costumes/audit/      SF6_CostumeAudit.bat / .ps1, shipped in costume_mods: what is installed, for users
stages/              stage slots: loader pass, redirection, tools, notes
plugin/              native REFramework plugin: menus, ownership, colours, online alias, stage select, VS screen
core/                SF6 Native Core: sf6core_native.dll, its C API (core/include) and docs
colormod/            SF6 Color Mod: SF6_ColorModNative.dll and its locale table
crsm/                SF6 Recording Manager: SF6_CRSMNative.dll, its locale table, package texts and offline tests
packaging/           builds the distributable zips (make_suite_packages.py: the four of a release; make_bundle.py: the all-in-one)
```

The costume loader has no hooks, no user interface and no scripting. It imports `kernel32`, `bcrypt` (to hash
the mod fingerprint), and `advapi32` and `user32` for the archive decoders; the game has all four loaded
already. Every AGS export is forwarded to `amd_ags_x64_real.dll`, the genuine AMD library that ships with the
game. The stage slots add one hook, described in [stages/README.md](stages/README.md), installed only once a
stage variant is selected.

## Building

Visual Studio 2022 or Build Tools with the C++ workload, then `build.bat` at the root:

```
build\amd_ags_x64.dll                  costume slots + stage slots (the releases)
build\costumes_only\amd_ags_x64.dll    costume slots alone, no stage slots
build\SF6_CostumeSlotsNative.dll       REFramework plugin
build\core\sf6core_native.dll         SF6 Native Core (core\build.bat)
build\colormod\SF6_ColorModNative.dll SF6 Color Mod (colormod\build.bat)
build\crsm\SF6_CRSMNative.dll         SF6 Recording Manager (crsm\build.bat)
build\costume_loader.exe, paktool.exe, loadtest.exe, stagepak.exe, stagetest.exe, textest.exe
```

`build.bat deps` also rebuilds the archive decoders. Everything is compiled with `/MT` and `/EHa`. Structured
exception handling must not be disabled: the loaders run inside `DllMain` and swallow any fault rather than
taking the game down with it.

## Data files

The loader starts from two data files that are **derived from your own copy of the game** and are deliberately not
distributed here:

- `vanilla_costume_index.tsv`, an index of the original costume files, built by `costumes/tools/build_vanilla_inventory.py`
  and `costumes/tools/make_vanilla_index.py`.
- the static structural tables that declare the extra slots, built by `costumes/tools/make_static_structural.py`.

Both scripts read the game paks through [REasy-parser](https://github.com/seifhassine/REasy). Point the
`REASY_PARSER` environment variable at your checkout before running them. Since 1.11.0 the loader builds the structural
tables from the game at each pak build and adds the game's costumes missing from the index by itself: the shipped
copies are the fallback (logged) and the starting point.

## Installing another proxy alongside

Only one `amd_ags_x64.dll` can exist in the game folder. If another tool already uses that entry point, rename
its DLL to `amd_ags_x64_chain.dll` and keep the genuine AMD library as `amd_ags_x64_real.dll`. The loader then
loads the chained module from a separate thread and logs it in `SF6_CostumeLoader.log`. Without that file, the
loader loads nothing else.

## Requirements

REFramework in `dinput8.dll`, official build 1.5.8 or newer (plugin API 1.10). The Native Core, the Color Mod and the
Recording Manager are built with REFramework's plugin API 1.15 headers (`core/third_party/reframework`, REFramework
commit 76520cac) and run on REFramework 1.5.8 (the core checks the running version before using anything newer; the
Recording Manager uses nothing past plugin API 1.10).

## Credits and licence

Written by Wael. Released under the MIT licence, see `LICENSE`.

Third party code is vendored under `common/third_party/`, `plugin/include/`, `stages/loader/third_party/`,
`core/third_party/` and `colormod/third_party/`: the zstd decompressor, miniz, bcdec, the LZMA SDK 7z decoder,
UnRAR, the REFramework plugin API, the stb image libraries, Lua 5.4.3 and nlohmann/json. The REFramework
helpers and the native menu row technique used by the core and the Color Mod are LL's work, used with his
permission. See `THIRD_PARTY.md` for the licences.

This repository ships no game data: the tables the loader needs are generated from your own installation,
and the release archive carries a prebuilt copy of them. Street Fighter 6 is a trademark of Capcom.
