# SF6 Mod Suite updater

When a newer release of the suite is out, the game says so once per launch, at the main menu, in its own Yes / No message box, and
installs it on Yes: download, checks, swap of the files, then the game closes and Steam starts it again. No new DLL, no new EXE: the code
here is compiled into the **Native Core** (`sf6core_native.dll`) and the **Slots Loader's plugin** (`SF6_CostumeSlotsNative.dll`).

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

## What happens

1. At start (a worker thread): a swap that stopped in the middle is undone (`swap_journal.json`), the manifest is fetched, the installed
   packages (their main DLL in the game folder) are compared with it. The Slots Loader is left out when ZZtaii's Slotsloader Manager
   installed it (`common/manager_mode.hpp`: the Manager owns those files, and a plugin of another size would end its Manager mode).
2. Something newer, at the main menu (a screen held 5 s, no battle, replay or training loaded, no message box of the game's up): the box
   lists the release and each package `old -> new`. **Yes** installs; **No** never offers that release again (`ignored_suite_version`);
   a later release asks again.
3. The install, off the game thread: each zip downloaded to `staging/zips`, its size and sha256 checked (a mismatch stops everything,
   nothing changed); extracted to `staging/tree`, every entry checked first (`classify_entry`: one refused entry and nothing is
   written); every `.dll` / `.exe` checked against Smart App Control (`LoadLibraryExW(DONT_RESOLVE_DLL_REFERENCES)` on the staged file,
   under a name of its own: error 4551 = blocked, the update stops and says which file); then the swap: each changed file of the game
   folder renamed `<name>.old-<installed version>`, the new one moved in its place (same volume: renames), all or nothing.
4. A box offers the restart. Yes: `cmd.exe` (Windows' own) is started, waiting for the game's process to end (2 minutes at most), then
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

## Trying the popup in the game without publishing a release

The address is fixed: there is no file that points the updater elsewhere. The latest release on GitHub needs a `manifest.json`
(`packaging/make_manifest.py --release build\release_<x> --version <x>`, uploaded to that release with its zips). Then install the core
and the plugin built from this source, the plugin built with an older `kPackageVersion` in `plugin/src/version.hpp` (for instance
`1.26.0` while the manifest says `1.26.1`): at the main menu the box offers `SF6 Slots Loader 1.26.0 -> 1.26.1`. `update.log` and
REFramework's log (`[update]` / `update:` lines) say what it did. To ask again after No: delete `ignored_suite_version` from
`state.json`.
