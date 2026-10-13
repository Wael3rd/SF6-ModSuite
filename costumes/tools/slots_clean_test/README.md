# SF6_SlotsClean: test

`SF6_SlotsClean.exe` (`costumes/tools/slots_clean.cpp`, built by `build.bat` into `build\`) removes the Slots Loader's
runtime patch paks and state from the game folder. This test runs it on fake game folders it builds itself (synthetic paks
written with `PakWriter`: ours for the costume slots, ours for the stage slots, a foreign one). It never touches a real game.

1. `build.bat` at the repository root (builds `build\SF6_SlotsClean.exe`), then `build_slots_clean_test.bat`.
2. `slots_clean_test.exe ..\..\..\build\SF6_SlotsClean.exe <scratch folder>`: prints PASS and exits 0 when every check holds.

Cases: our paks above every foreign pak (deleted); our pak below a foreign one (replaced in place by a marker-only pak that
is still recognised as ours, second run changes nothing); stage pak below and costume pak above a foreign one; `--dry-run`
(folder identical); game running (refused, simulated with `SF6_SLOTSCLEAN_FAKE_RUNNING=1`, which the tool reads for this
purpose only); not a game folder. Every case also checks that `registry.json` / `outfits.json` / the stage slots' state
files are gone, that `reframework\costume_mods`, other mods' data, the shipped `loader\` files, the logs and Fluffy's
folder are byte for byte unchanged, and that the note about Fluffy's `ArchiveList.bin` appears only when it lists our paks.
