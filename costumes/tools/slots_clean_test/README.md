# SF6_SlotsClean: test

`SF6_SlotsClean.exe` (`costumes/tools/slots_clean.cpp`, built by `build.bat` into `build\`) removes the Slots Loader's
runtime patch paks and state from the game folder (the costume, stage, title pack and UI pack registries; the
player's choices, `state.json`, `overdrive.json`, `shuffle.json` and `game_title.*`, stay). This test runs it on fake game folders it builds itself (synthetic paks
written with `PakWriter`: ours for the costume slots, ours for the stage slots, a foreign one). It never touches a real game.

1. `build.bat` at the repository root (builds `build\SF6_SlotsClean.exe`), then `build_slots_clean_test.bat`.
2. `slots_clean_test.exe ..\..\..\build\SF6_SlotsClean.exe <scratch folder>`: prints PASS and exits 0 when every check holds.

Cases: our paks above every foreign pak (deleted); our pak below a foreign one (replaced in place by a marker-only pak that
is still recognised as ours, second run changes nothing); stage pak below and costume pak above a foreign one; `--dry-run`
(folder identical); game running (refused, simulated with `SF6_SLOTSCLEAN_FAKE_RUNNING=1`, which the tool reads for this
purpose only); not a game folder. Every case also checks that `registry.json` / `outfits.json` / the stage slots' state
files are gone, that `reframework\costume_mods`, other mods' data, the shipped `loader\` files, the logs and Fluffy's
folder are byte for byte unchanged, and that the note about Fluffy's `ArchiveList.bin` appears only when it lists our paks.

## Row test (the loaders themselves)

`slots_clean_test.exe --row <build dir> <scratch folder> <game folder> <costume mod archive>` runs the real
`costume_loader.exe` and `stagetest.exe` (both from `build\`) on a fake game folder in the scratch folder. The game folder
is only read: its `re_chunk_000.pak` (passed as `--base-pak`), the shipped loader data and the stage path index are used,
nothing is written there. Any small costume mod archive (.zip, .7z, .rar) will do. Keep the scratch folder short, about 60
characters at most: the test writes its fixture files with plain paths, which stop at 259 characters, and a file it can't write
is a FAIL that says so. A deep folder fails cases 17 on for that reason alone. Mod paks (synthetic) appear and
disappear above the loaders' paks, and after every run the patch paks must form an unbroken row from 001. A loader pak
that has to stay below a mod pak becomes a marker-only placeholder (shown `c` / `s` in the output, `C` / `S` are the real
paks) and is deleted once no mod pak is above it. A full run takes a few minutes.

Cases 16-22 check the fixes ported from ZZtaii's Slotsloader fork 1.10.5 - 1.10.9 (MIT; his `tests/*.py` describe the
same cases on a synthetic game). Their fixtures are made of the game's own files, read from the base pak: an extra outfit
"ZZ Colour Fixture" (Ryu's Outfit 1 body, its colour list, two colours of Outfit 2 under Outfit 1's names, one of them
with MaterialData's signature of before the game update that changed it, a mod-only file in Outfit 2's folder and one in
`product/model/`). Turned-off pak entries are simulated with `SF6_COSTUME_TEST_TURNED_OFF` (the entry reads as absent) and
a `dlc\test.pak` holding rows with zeros over the hash, as a mod manager's PAK method leaves them.

- 16: `PakReader::turned_off_candidates`: the row between the neighbours of a hash, none outside, none for a table not
  sorted, both of two rows, a row of a pak read over another.
- 17: the outfit's scene names the colour list written for the slot, which names the slot's colours, which hold the mod's
  bytes; the mod-only files are the outfit's copies; the `product/model/` one is in `loose_probes.tsv`; `outfits.json` says
  `"loose_files_kept": true` after `"loader"`; a second run is Up to date.
- 18: a loose file at the shared path (as a replacement installs it), changed, removed; then at the `product/model/` path:
  each change rebuilds, the path is left to the loose file while it is there, the outfit's copy comes back after.
- 19: Ryu's `esf001_001_cmd_000.user.2` turned off with its row kept, gone, every fighter's gone: where the current
  signatures come from, and the old colour file brought up to date (not when there is none).
- 20: the outfit's scene turned off: read from its row; another outfit's scene in the row, two rows, a table not sorted:
  left out ("slot scene missing").
- 21: a donor's colour list and colours (esf002 Outfit 1's) in Ryu's folder: the slot's copy names the slot's copies.
- 22: a main and five add-ons that each replace its body mesh: one outfit with runtime options, not "too many to combine".
- 25: several mods in one sub-folder of a character folder (docs/RUNTIME_OPTIONS.md, "Several mods in one sub-folder"). The
  fixtures are retouches of M. Bison, meshes only, made of the game's own (archives by Windows' `tar.exe`). Handled together,
  nothing on a game outfit: two archives, one with a body mesh; the same as folders; two with a body mesh (a "Version" choice);
  a modular mod and an add-on; an outfit of two costumes and an add-on of neither of their folders; a sub-folder of
  `costume_mods_nsfw`. As before: all pure add-ons; the same two loose in the character folder, as folders and as archives; one
  alone in a sub-folder beside a loose hair; the options of one unpacked bundle; the same sub-folder name in both roots.

`slots_clean_test.exe --row ... <previous build dir>` (a folder with the previous release's `costume_loader.exe` and
`stagetest.exe`) adds case 23: an install built by the previous build is Up to date for this one, and this one built from
scratch makes the same pak byte for byte (the archive given must not be a case a fix changes). The executables must be
moved there, not copied, on a PC where Smart App Control is on.

Case 24 (after 23) checks fork 1.10.5 in Manager mode (`docs/MANAGER_CONTRACT.md`, promise 6: the Slotsloader Manager's
`manager_installs.json` lists a stand-in plugin at its size, as when the Manager installed the loader). Two archives
(stored zips the test writes): a main and five add-ons naming it in `AddonFor`, each a copy of the main's body mesh: the
main and five "main + add-on" outfits, each its own row in `variants.json`; a main with four add-ons, two of its body and
two of another part: "too many to combine", the main alone. `costume_options.json` of the normal mode is gone in Manager
mode (it names the same pak, the plugin would use it). Then the list is removed: runtime options again, rebuilt, the
manifest back.

`slots_clean_test.exe --row-manager <build dir> <scratch folder> <game folder> <costume mod archive>` runs cases 7-22, 25 and
24 with Manager mode on from the start (the first run must say so): the fork's fixes of cases 16-21 must hold there too.
Case 22 then gives the five combined outfits only (one sub-folder of separate mods: the main alone is not made). Case 25 is
different there: two mods in one sub-folder are not handled together (that rule belongs to the runtime options), and a costume
with two add-ons that replace no same file, three archives in one sub-folder (Ingrid's FlyIn_DT_Book), gives one outfit per
add-on and one with both, as the fork lists them. Each of the three rows of `variants.json` is then turned off by itself with
`variant_choices.json`: its outfit goes, the outfits that stay keep their numbers (the "Outfit V" of a row is its place in the
fighter's list, which moves; a variant turned on again is a new outfit, as promise 2 of the contract says).
