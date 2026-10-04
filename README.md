# SF6 Slots Loader

Street Fighter 6 mods replace what they change. This project keeps the original and the mods side by side,
in one `amd_ags_x64.dll`:

| Part | What it does | State |
|---|---|---|
| **Costume slots** (`costumes/`) | every costume mod becomes an extra outfit slot | released, this page |
| **Stage slots** (`stages/`) | UP / DOWN on the stage select screen cycles a stage through its mods | experimental, in the releases since 1.9.0: [stages/README.md](stages/README.md) |

This repository was named SF6-CostumeSlotsLoader until 30/09/2026; the old links lead here.

## Costume slots

Street Fighter 6 costume mods replace an existing outfit: install a Ryu mod and Outfit 1 is gone until you
uninstall it. This loader turns every installed costume mod into an **extra** outfit slot and puts the original
outfit back, so Outfit 1 and the mod live side by side in the costume spinner.

Install it once. After that nothing changes in how you use the game: install costume mods the way you already
do, start the game, and the new outfits are there.

## Install

Grab the archive from [Releases](https://github.com/Wael3rd/SF6-SlotsLoader/releases), install it in
Fluffy Mod Manager or copy it over your Street Fighter 6 folder, then install costume mods as usual. You need
REFramework in `dinput8.dll`, official build 1.5.8 or newer.

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
  the combined outfit is made, not the costume alone as well. An add-on made for another outfit's folder than
  the costume's (a hair for Outfit 1 next to a costume of Outfit 2) still brings its parts to the costume,
  as long as the costume has none of that part.
- **Modular mods** (main files, body options, skin options, optional extras) give one outfit per body
  option with the default skin, as Fluffy installs them by default.
- A mod that only changes a part of an original outfit (glasses, earrings) gives that outfit with the
  change, in a slot of its own.
- Variants share most of their files, and the generated pak stores identical data only once.
- Broken, password-protected or unrelated archives are skipped and reported in `SF6_CostumeLoader.log`.

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
   **100 extra slots per character**, and restores the outfit the mod had overwritten from the base pak.
3. Colours, physics chains, shared body parts, weapons and the `streaming/` high resolution textures follow the
   costume to its new slot. Files made before a game update are brought up to date on the way (see below).
4. It reads every slot back the way the game will load it, repairs what cannot load, and leaves out a slot it
   cannot repair instead of letting it stall the game. Only the slots in use are declared to the game.
5. It writes a registry that the native plugin reads to make the slots selectable and, online, to show the
   other players a legal outfit. The registry also carries each slot's colour swatches.

A fingerprint of the installed mods is kept, so a launch with nothing changed costs about 20 ms. A launch after
installing or removing a mod takes a few seconds, up to about fifteen with many large archives.

## Compatibility

Mods published since 2023 were made for older versions of the game. The loader recognises the known
differences (old texture suffixes, colour files in older layouts, broken texture headers, parts whose
materials no longer match) and fixes them in its own pak, without touching the mod files.
[COMPATIBILITY.md](COMPATIBILITY.md) lists what it fixes and the mods tested; [TESTING.md](TESTING.md)
lists what still needs testing.

Online, the extra slot is aliased to a DriveTech outfit for the other players, so matchmaking stays valid.
Since 1.9.0 everything in game is done by one native REFramework plugin (no Lua script), driven by the game's
own events: menus shown or closed, an outfit folder mounted, the stage select screen. A fight triggers none of
them, so the plugin does nothing during a fight. Official REFramework stops Lua scripts during online matches,
not native plugins. See [plugin/README.md](plugin/README.md).

## Repository layout

```
build.bat            builds everything into build/
common/              shared by both loaders: KPKA pak reader and writer, archive reading, third party code
proxy/               the AGS proxy: one DllMain for the stage and costume passes
costumes/loader/     costume loader: mod detection, slot relocation, scene and material patching
costumes/tools/      generators for the data files, run against your own game installation
costumes/audit/      SF6_CostumeAudit.bat / .ps1, shipped in costume_mods: what is installed, for users
stages/              stage slots: loader pass, redirection, tools, notes
plugin/              native REFramework plugin: menus, ownership, colours, online alias, stage select, VS screen
packaging/           builds the distributable zip
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
build\costume_loader.exe, paktool.exe, loadtest.exe, stagepak.exe, stagetest.exe, textest.exe
```

`build.bat deps` also rebuilds the archive decoders. Everything is compiled with `/MT` and `/EHa`. Structured
exception handling must not be disabled: the loaders run inside `DllMain` and swallow any fault rather than
taking the game down with it.

## Data files

The loader needs two data files that are **derived from your own copy of the game** and are deliberately not
distributed here:

- `vanilla_costume_index.tsv`, an index of the original costume files, built by `costumes/tools/build_vanilla_inventory.py`.
- the static structural tables that declare the extra slots, built by `costumes/tools/make_static_structural.py`.

Both scripts read the game paks through [REasy-parser](https://github.com/seifhassine/REasy). Point the
`REASY_PARSER` environment variable at your checkout before running them.

## Installing another proxy alongside

Only one `amd_ags_x64.dll` can exist in the game folder. If another tool already uses that entry point, rename
its DLL to `amd_ags_x64_chain.dll` and keep the genuine AMD library as `amd_ags_x64_real.dll`. The loader then
loads the chained module from a separate thread and logs it in `SF6_CostumeLoader.log`. Without that file, the
loader loads nothing else.

## Requirements

REFramework in `dinput8.dll`, official build 1.5.8 or newer.

## Credits and licence

Written by Wael. Released under the MIT licence, see `LICENSE`.

Third party code is vendored under `common/third_party/`, `plugin/include/` and
`stages/loader/third_party/`: the zstd decompressor, miniz, bcdec, the LZMA SDK 7z decoder, UnRAR, the
REFramework plugin API and the stb image libraries. See `THIRD_PARTY.md` for their licences.

This repository ships no game data: the tables the loader needs are generated from your own installation,
and the release archive carries a prebuilt copy of them. Street Fighter 6 is a trademark of Capcom.
