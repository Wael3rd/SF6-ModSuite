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
