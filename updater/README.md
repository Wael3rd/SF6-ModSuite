# SF6 Mod Suite updater

When a newer release of the suite is out, the game says so once per launch, at the main menu, in its own Yes / No message box, and
installs it on Yes: download, checks, swap of the files, then the game closes and Steam starts it again. No new DLL, no new EXE: the code
here is compiled into the **Native Core** (`sf6core_native.dll`) and the **Slots Loader's plugin** (`SF6_CostumeSlotsNative.dll`).

## Outside the game: the Mod Sorter

`SF6_ModSorter.exe` (`tools/sorter/suite_update.cpp`) drives the same code with the game closed: the same manifest, checks, swap,
`state.json` and `update.log` (its lines start with `mod sorter:`). Two differences. The installed versions are read from the DLLs'
files (`update_pe.cpp`: nothing is loaded or run). And nothing is installed while the game runs: the game's own box does that. A
look at the latest release writes nothing where no package of the suite is installed. Its zip is no package: the game never installs
it. `make_manifest.py` lists it apart, under `"tools"` (see The manifest), and the sorter (1.1.0 and later) updates itself from that
entry: its zip downloaded, size and sha256 checked, the exe extracted next to the running one, checked against Smart App Control, then
two renames (`SF6_ModSorter.exe` -> `.old`, the new one in its place; a running exe can be renamed) and the tool starts again; the
`.old` copy is deleted at the next start. Its version is `tools/sorter/version.hpp` (`--version` prints it).

## Who runs it

Every package but the Slots Loader needs the Native Core; the Slots Loader's plugin works without it. So in any combination one of the
two is there, and no package needs a new one. Both export `sf6suite_updater_rank()` (`(kUpdaterVersion << 8) | role`, 0 before their
init); at its first frame each copy compares its rank with the other's and only the highest runs the updater: the newest updater code,
then the core. A DLL of a release before the updater has no export and never runs it.

## The version each package reports

Its main DLL's `sf6suite_package_version()` export: `kVersion` (Color Mod `colormod/src/common.cpp`, Recording Manager
`crsm/src/common.cpp`, Profile Mod `profile_mod/profile_mod.cpp`), `CORE_VERSION` (core), `kPackageVersion` (`plugin/src/version.hpp`,
the Slots Loader package's version, checked by `make_suite_packages.py` against `--version`). The host asks the DLL loaded in the game,
from the game folder, never its bytes. A Native Core without the export answers through `sf6core_get_api()->get_version`. A DLL that
reports nothing (built before the updater) is never offered an update: **the first release with the updater must rebuild every package's
DLL**, unchanged ones included, or those stay out of the updater until their next release.

"Newer" is a version compare (`1.26.1 > 1.26.0`, `0.9.10 > 0.9.9`, a `-suffix` before its release), never a hash: a developer's own
build (an equal or higher version) is never offered anything.

## The manifest

`manifest.json`, an asset of each GitHub release, written by `packaging/make_manifest.py` (called by `make_bundle.py`). The game reads
`https://github.com/Wael3rd/SF6-ModSuite/releases/latest/download/manifest.json` and the zips from
`https://github.com/Wael3rd/SF6-ModSuite/releases/download/<tag>/<asset>`: fixed addresses, HTTPS (WinHTTP, the system's certificate
checks), redirects followed by hand only to `github.com`, `release-assets.githubusercontent.com` and `objects.githubusercontent.com`. No
Authorization header, no cookie, nothing about the player is sent.

```json
{ "format": "sf6suite-manifest-1", "suite": "1.30.1", "tag": "v1.30.1",
  "packages": [ { "id": "color_mod", "name": "SF6 Color Mod", "version": "0.26.1", "asset": "SF6_ColorMod_0.26.1.zip",
                  "size": 1418590, "sha256": "13c2eba9...", "proof": "reframework/plugins/SF6_ColorModNative.dll" } ] }
```

Read as hostile input (`update_logic.cpp`): 64 KiB and 8 levels at most, every field of its type, a known id's `proof` must be the file
this code knows, an asset is a bare `.zip` name, a size 64 MiB at most; an unknown id is skipped (a later package), anything else refuses
the manifest.

A tool's zip (the Mod Sorter) is listed apart, never among the packages:

```json
  "tools": [ { "id": "mod_sorter", "name": "SF6 Mod Sorter", "version": "1.1.0", "asset": "SF6_ModSorter_1.1.0.zip",
               "size": 1012345, "sha256": "5f0c...", "file": "SF6_ModSorter.exe" } ]
```

The game reads the key and installs nothing from it; an updater older than 1.34.0 does not know it and ignores it.

## What happens

1. At start (a worker thread): a swap that stopped in the middle is undone (`swap_journal.json`), the manifest is fetched, the installed
   packages (their main DLL in the game folder) are compared with it. The Slots Loader is left out when ZZtaii's Slotsloader Manager
   installed it (`common/manager_mode.hpp`: the Manager owns those files, and a plugin of another size or other bytes would end its Manager mode).
2. Something newer, at the main menu (a screen held 5 s, no battle, replay or training loaded, no message box of the game's up): the box
   lists the release and each package `old -> new`, with three buttons (the game's Yes / No / Cancel box, the third button's word
   replaced by ours). **Yes** installs; **No** (Back too) does nothing, the next launch asks again; **Ignore** never offers that
   release again (`ignored_suite_version`); a later release asks again. A failed install says why in a box with one OK button and
   is offered again at the next launch.
3. The install, off the game thread: each zip downloaded to `staging/zips`, its size and sha256 checked (a mismatch stops everything,
   nothing changed); extracted to `staging/tree`, every entry checked first (`classify_entry`: one refused entry and nothing is
   written); every `.dll` / `.exe` checked against Smart App Control (`LoadLibraryExW(DONT_RESOLVE_DLL_REFERENCES)` on the staged file,
   under a name of its own: error 4551 = blocked, the update stops and says which file); then the swap: each changed file of the game
   folder renamed `<name>.old-<installed version>`, the new one moved in its place (same volume: renames), all or nothing.
   While it lasts, the game's own ticker (the strip on the left side of the screen, the one of the Recording Manager's drill
   downloads) says where it is: `Downloading SF6 Mod Suite 1.34.0: 42%`, then the install's line. One strip, kept on screen the whole
   time: each frame the host puts the request's `ElapsedSecond` back to zero (the game takes a strip away at `DisplaySecond`) and
   rewrites the text of the strip's `via.gui.Text` when the percentage changes (`update_game.cpp`, `ticker_keep`); the strip leaves by
   itself 3 s after the install. All of it on the game thread: the same calls from the render thread crash the game. A ticker that
   cannot be used is logged and the install goes on without it. Since updater version 2 (suite 1.34.0): the update TO 1.34.0 is run
   by the installed updater, without the strip.
   Once the swap is done, the zips are moved to `downloads/` (`keep_downloads`), for the players who install with Fluffy Mod Manager
   (it keeps its own copy of each zip, and puts the old version back when the mod is re-enabled): the last zip of each package
   there, the one before it moved to `downloads/previous_version/` (in place of an older one: two versions at most; a package's zips
   are told by their name without the version, `asset_family`). Keeping them never fails an install. Since updater version 3
   (suite 1.36.0).
4. A box offers the restart, with a third button, **Show zip**: Explorer on `downloads/`, the newest zip selected
   (`show_downloads`), then the box comes back. Yes: `cmd.exe` (Windows' own) is started, waiting for the game's process to end (2 minutes at most), then
   runs `start "" "steam://rungameid/1364780"`; the game window gets `WM_CLOSE`, as from its close button. No: the new version applies at
   the next start.
5. At the next start, once the game reaches the main menu (the new version runs), the `.old-*` copies are deleted.

What a package zip may write: the loader's three files at the game's root, `reframework/plugins/*.dll`, files under
`reframework/data/<folder>/`, and only the top level of the mod folders (their README, the audit tool, the empty character folders,
which are made when missing and never emptied). Not written: the zip's own `modinfo.ini` and `README.txt` (Fluffy Mod Manager's), a
player's token (`identity.json`), anything under `reframework/data/SF6_Update_data/`. Kept when the player has one (never replaced):
`reframework/data/SF6_CRSMNative_data/settings.json` (the Recording Manager's owners), `reframework/costume_mods/settings.ini`. Files
a package does not ship are never touched, and files a new version no longer ships are not deleted.

## Files

`reframework/data/SF6_Update_data/` of the game folder: `state.json` (`check_for_updates`, `ignored_suite_version`,
`pending_cleanup`; other keys are kept), `update.log`, and only during an install `staging/` and `swap_journal.json`.
**The Slotsloader Manager's installs**: when ZZtaii's Slotsloader Manager installed the Slots Loader (`common/manager_mode.hpp`), the
game fetches nothing: its players update in the Manager. The Manager installed it when its `manager_installs.json` lists the
plugin at the size it has on disk and, when the entry gives a `sha256` (the Manager writes one), with the plugin's SHA-256: a loader
installed again by hand or by Fluffy Mod Manager over the Manager's is the player's again, unless it is byte for byte the same file.
`update.log` says in one line what decided whenever the list is there (`manager_installs.json: the plugin's size and sha256 matched:
installed by the Slotsloader Manager`, or `...: normal mode`). `"check_in_manager_mode": true` in `state.json` turns the check on for
them; the module never writes that key. With the check on in that mode, a package whose main DLL the Manager's
`manager_installs.json` lists at the size (and `sha256`, when given) it has on disk (the Native Core installed by the Manager, say)
is left out like the Slots Loader: replacing it would leave the Manager's record of the file wrong. The Mod Sorter, outside the game, still looks
at the latest release, with the same rule for the Manager's packages. Outside Manager mode nothing of this applies: the check
and the offers are what they were.

**Off switch**: `"check_for_updates": false` in `state.json` (create the file with `{ "check_for_updates": false }`): nothing is
fetched; a half-done swap is still undone and old copies still deleted.

## Fluffy Mod Manager

Fluffy keeps its own copy of each package zip and lists the files it installed (`Games\SF6\installed.ini`: the mod's name and its
files' paths, no sizes or hashes). Replacing those files in place does not break its uninstall (it deletes those paths and puts its
`_FMMBACKUP` files back). But re-enabling the mod in Fluffy writes the old zip's files back, and the updater then offers the update
again; the success box says to replace the zip in Fluffy too. A file a new version adds is not in Fluffy's list and stays after
Fluffy uninstalls the mod.

## Tests

`updater\tests\build_update_tests.bat` then:

- `build\update_tests\update_test.exe --work <folder>`: versions, the manifest as hostile input, the plan, zip slip, the swap failing
  at each step, a locked file, the journal of a crash, the clean-up, the ignored release, the off switch, the restart's waiter run on a
  real short process.
- `python updater\tests\run_e2e.py --old build\release_<a> --new build\release_<b> --work <folder> [--fetch]`: a fresh install of
  release a in a fake game folder, updated by the updater's own code from local copies of release b's zips, compared with a fresh
  install of b; `--fetch`: one real download of a release asset, checked against the digest the GitHub API gives.

To watch the strip on a fast line: an empty `slow_download.txt` in `reframework/data/SF6_Update_data/` holds the download at about
250 KiB/s (a test knob; delete it afterwards).

## Trying the popup in the game without publishing a release

The address is fixed: no file points the updater elsewhere, so the test uses the real latest release.

1. Give the latest release a manifest: `python packaging\make_manifest.py --release build\release_1.31.0 --version 1.31.0`
   and upload that `manifest.json` to the v1.31.0 release (with its zips already there). Players' current DLLs never read it.
2. Build the core and, with `kPackageVersion` set to `1.26.0` in `plugin/src/version.hpp` (a local change, not committed), the plugin:
   `build.bat core`, `build.bat plugin`; check both against Smart App Control.
3. Game closed: put `build\core\sf6core_native.dll` and `build\SF6_CostumeSlotsNative.dll` in `reframework\plugins\` (keep the
   current ones aside).
4. Start the game; 5 s after the main menu shows, the game's box offers `SF6 Slots Loader 1.26.0 -> 1.26.1` (the core runs the
   updater: `[update]` lines in `reframework\data\sf6core\sf6core.log`, and `reframework\data\SF6_Update_data\update.log`).
5. **No**: nothing is written, the next launch asks again. **Ignore**: `state.json` gets `"ignored_suite_version": "1.31.0"`; the
   next launch offers nothing. Empty that key to be asked again.
6. **Yes**: the release's `SF6_SlotsLoader_v1.26.1.zip` is downloaded and installed (`SF6_CostumeSlotsNative.dll.old-1.26.0` kept),
   then the restart box: Yes closes the game and Steam starts it again about 8 s after it is gone. At the main menu of that launch the
   `.old-1.26.0` copy is deleted, and nothing is offered (the released 1.26.1 plugin reports no version, the core 0.9.10 is current).
   Done on 2026-11-11 with the v1.31.0 release (Yes, the restart, the clean-up, Ignore).
7. The plugin as the host: the same with the test plugin and no `sf6core_native.dll` (`update:` lines in `reframework\data\SF6_CostumeSlots_data\native_log.txt`).
