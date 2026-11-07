# Report a bug (report_mod.dll)

`reframework/plugins/report_mod.dll`, a small plugin on the SF6 Native Core, shipped in the Slots Loader's package and the all-in-one. Without the core it does nothing and says so
in REFramework's log. It owns the guide entry, the menu, the picture and the upload; it needs neither the Slots Loader nor the Recording Manager to run.

## Layout

```
src/
  host.cpp        the plugin's entry; what the sources it shares expect of their host (see report_mod.hpp)
  bug_report.cpp  the screen: the guide entry, the menu, the status flow, the description, Ctrl+V
  report/         the picture (D3D12 readback, WIC), what is read from the disk, the guards' pure logic
data/             SF6_ReportMod_data/locale.json, the texts in 14 languages
tests/            offline benches, no game and no user data (run_report.ps1); report_e2e_test.exe, a whole report to a server on this machine
build.bat         builds build\report_mod\report_mod.dll (also called by the root build.bat)
```

**Shared, not copied.** `crsm/src/community/` (HTTP, Steam ticket, token), the text dialog `crsm/src/ui/dialogs.cpp`, `ui/gx.cpp`, `ui/lang.cpp`, `ui/logic.cpp`, `ui/details_logic.cpp`,
`mech/hooks.cpp` (the code check), `game/gm.cpp` and `model/` are compiled from the Recording Manager's sources, so the two cannot drift. `host.cpp` stands in for the few functions of
the Recording Manager they call; a new call in them is a link error here, never a silent difference.

**One token, two DLLs.** `reframework/data/SF6_Community_data/identity.json` is the Recording Manager's file and this DLL's. It is written whole or not at all (a temporary file, then a
rename). A 401 reads the file again before anything else: the other DLL may hold a newer token. Getting a token is one at a time across the DLLs, under a mutex named for the process:
the second one waits, finds the first one's token and asks Steam for nothing.

**Beside a Recording Manager 2.2.8**, which has its own Report a bug: at the start the DLL looks for the literal only that version holds in the Recording Manager's loaded image, stays
off, and says so in the log, so R never gives two entries. 2.2.9 has no report.

**No key.** The server's `/auth/steam/ticket`, `/api/report` and `/api/report/status` need no API key: the token, verified by Steam, says who is calling. `config.json` holds the
address alone, written by `packaging/make_fluffy_package.py` from the local git-ignored `crsm/package/config.json`; the address is in no tracked file.

## What it does


On Battle Settings > Character (Fighter Settings), Character tab, R on the keyboard or X on the pad (the entry "Report a bug" in the guide
bar) takes a picture of the game and opens a menu on the side of the screen, the fighter still in sight. **No report leaves your PC until you
choose Send on that menu.** Back sends nothing and keeps nothing: the picture is only ever in memory, no file is written.

When the menu opens it asks the server one question, with your Steam-verified token: your report on this costume, if any. Only the costume's key
goes with it, no picture, no log, no text. The answer is the report's status and your own text. If the server cannot be reached the menu says so, and
OK and BUG still work.

The first row is STATUS. On OK, the default, nothing else shows. On BUG: Describe the problem and Send, and under them the character, the outfit and your
description, which moves by itself when it is too long for its place, with a bar beside it. A message on the guide's line says what you should know: with the focus
on Send, that your Steam ID, your logs and your mod list go too. After Send, or when you already have a report on this costume, STATUS shows WAITING FOR FIX, which only
the author's action can change; your text comes back and Send becomes Update, which replaces what was sent and does nothing until your text differs from it. When the
author marks it fixed, STATUS shows TO TEST: choose OK and press MARK AS FIXED if it is fixed, or choose BUG if it is still broken. You can never see anyone else's report.

What Send sends, over HTTPS to the same Community server, with your Steam-verified token (see above):

- **A picture of the game**, from the moment you pressed the key: the game's own image (its back buffer), never your desktop or another
  window. A JPEG of 1080p at most, 450 KB at most. HDR is converted to SDR.
- **Your text**, as you wrote it (1000 characters at most).
- **The character, outfit and colour** on the screen, and **the outfit's record** (its lines in `outfits.json` and `registry.json`).
- **Versions**: this plugin's, the Native Core's, the Color Mod's, the Recording Manager's when it is installed, and the Slots Loader's; the size and the start of the SHA-256 of our
  plugins, of `amd_ags_x64.dll` and of `dinput8.dll` (no other DLL is looked at); `modinfo.ini`'s name and version.
- **The list of your installed mods**: the names in `outfits.json`, `reframework\stage_mods` and `reframework\color_mods` (96 KB at most).
- **Two logs, and only these**: `SF6_CostumeLoader.log` and `reframework\data\SF6_CostumeSlots_data\native_log.txt`, their start and end, 64 KB
  each at most. Before they leave, any path under a user's folder and your Windows user and PC names become `<user>`, the game's folder
  `<game>`, and whatever looks like a platform ID (a SteamID in any form, a Steam profile address, any lone number of 9 digits or more)
  `<id>`. The same path and name cleaning goes over the mods' names and the record.
- **Your Steam ID: the plugin does not send it.** The server takes it from your verified token and keeps it with the report, so that the author
  can reach you. Other players never see a report.

The server keeps **one report per Steam account per costume** (a new one on the same costume replaces the old one; a costume is identified by
its mod's id, so a mod's update allows a new report), 100 MB per account at most, for **90 days** after its last update, and only the admin can read it. The plugin keeps no
copy. If the picture, a log or the mods' data cannot be read, the report goes without it and says so.

Cost: asleep until this screen runs, in menus, Training and fights alike: no poll, nothing per frame. A hook on the screen's own update, which the game
calls only while the screen is up, wakes it; a few reads a frame while the screen is up; when the screen is left, it goes back to sleep. What remains asleep: one atomic load per
frame presented and a key test per window message (REFramework has no way to take these callbacks back), one load in a hook on the option dialog's update, which runs only while an option
dialog is up, and the text dialog's hooks, which fire only on their events and pass through at once. The press costs one frame's copy of the image, about a millisecond;
the encoding, the reading of the files and the requests run off the game thread.
