# Structural files: offline tests

Since Slots Loader 1.11 the loader builds the scenes, the costume table and the costume name file it ships in the
costume pak from the game's own files (`costumes/loader/structural_scene.cpp`, `structural_msg.cpp`,
`add_costume_records` in `patch.cpp`). These tests check that it writes the same bytes as the Python reference
(`costumes/tools/make_static_structural.py`, REasy-parser).

1. `python make_oracle.py <oracle dir>` (REASY_PARSER set, `zstandard` installed): the game's files in `<oracle>/in`,
   the Python tool's output in `<oracle>/expected`.
2. `build_scene_test.bat`, `build_msg_test.bat`, `build_table_test.bat`, then run each test with `<oracle dir>`:
   every file must print SAME.

Run them again after a game update that changes these files: a difference means the game changed a layout the C++
code reads; the loader then falls back to the copies shipped in `loader/static/` (log line "structural files taken
from the shipped copies").

## Vanilla costume inventory

At each pak build the loader completes its vanilla costume index (`vanilla_costume_index.tsv`) from the game's paks for
the costumes the index does not know (`costumes/loader/vanilla_inventory.cpp`, log line "vanilla index completed from
the game"). `build_inventory_test.bat` then `inventory_test.exe <game dir> <vanilla_costume_index.tsv>` inventories every
costume the TSV lists with that code and compares the rows with the TSV's, which `build_vanilla_inventory.py` +
`make_vanilla_index.py` wrote. `--python-priority` reads a file that is in a DLC pak and in re_chunk_000.pak from the DLC
pak like the Python tool does; without it the loader's order is used (re_chunk_000.pak first), and the few costumes whose
color variation list a DLC pak overrides print DIFF. A DIFF otherwise, or "instance of a type this code does not know",
means the game changed a type the table in `vanilla_inventory.cpp` describes.
