# SF6 Recording Manager (CRSM)

30 recording slots per character in Training (1-8 are the game's own, 9-30 are extra), drills, slot names, a reversal
picker and Community sharing of drills, all in the game's training pause menu (Recording Settings > Manager / Record
/ Replay). One native REFramework plugin, `reframework/plugins/SF6_CRSMNative.dll`, client of the SF6 Native Core
(0.9.5 or newer: Confirm on spin rows, plus its training pause-menu service, [core/docs/PAUSE_MENU.md](../core/docs/PAUSE_MENU.md)). No Lua
script. Release package: `SF6_CRSM_<version>.zip`; its `README.txt` ([package/README.txt](package/README.txt)) is the
user guide.

## Layout

```
src/            the plugin (C++20)
  model/        pure model of the files and the slots (no game): drills, virtual slots, packs, timelines, locale
  game/         the few game accesses (fields, arrays, references) on REFramework's C++ API
  mech/         the four layers files / replay / reversal / record, their hooks and the code check of each hook target
  ui/           the screens on the game's own menus, the Manager page on the core's pause-menu service, the dialogs
  community/    the Community client: WinHTTP, protocol, identity, the Steam ticket
  report/       "Report a bug": the picture (D3D12 readback, WIC), what is read from the disk, the guards' pure logic (screen: ui/bug_report.cpp)
  owners.hpp    which layer the plugin runs (settings.json); standalone.hpp: when it takes the menu without a Lua script
data/           SF6_CRSM_data/locale.json, the texts in 14 languages (shipped as reframework/data/SF6_CRSM_data/)
package/        the package's README.txt, modinfo.ini, settings.json and the Community config template
docs/           PACK_FORMAT.md: the drill file (30 entries, `active` flags, the reversal / replay block on entry 1)
tests/          offline benches, no game and no user data (run.ps1, run_community.ps1)
build.bat       builds build\crsm\SF6_CRSMNative.dll (also called by the root build.bat)
```

## Building and testing

`build.bat` at the repository root (or `crsm\build.bat`): MSVC (Visual Studio 2022 or Build Tools), C++20, `/EHa`,
`/W4` with 0 warnings. The headers come from the rest of the repository: `core/include/sf6core` (the core's C API),
`core/third_party/reframework/include` (REFramework plugin API) and `colormod/third_party` (nlohmann/json). The plugin
asks for REFramework plugin API 1.10 (REFramework 1.5.8) and uses nothing newer.

`crsm\tests\run.ps1` builds and runs the benches of the model, the layers, the screens' logic, the Manager page and the
integration rules; `crsm\tests\run_community.ps1` the Community client against a stub server in the same process and a
fake Steam API; `crsm\tests\run_report.ps1` "Report a bug": its logic, and the picture on a D3D12 swap chain of its own. None
reaches the network.

Packaging: `packaging\make_suite_packages.py` builds `SF6_CRSM_<version>.zip` with the other packages. It needs the
real Community config, `crsm/package/config.json`: copy `config.example.json` to that name and put the API key in it.
Git ignores that file; never commit it. Without it, use `--without-crsm` (the plugin still builds; its Community then
answers "api_base not set in config.json").

## Settings

`reframework/data/SF6_CRSMNative_data/settings.json`: six owners (`community`, `files`, `replay`, `reversal`,
`record`, `ui`), each `"native"` (the plugin runs that part) or `"lua"` (left to a Lua version of the mod), and
`"standalone": "auto"` (the plugin takes the menu itself 2 s into Training when no Lua version is installed) or
`"off"`. The package ships every owner `"native"` and `"auto"`. A missing file is written again: as the package ships
it when no Lua version of the mod is in `reframework/autorun`, every owner `"lua"` when one is. While a Lua version is
installed (`SF6_RecordingManager.lua`, `SF6_RecordingSlotManager.lua`, `CRSM_REFrameworkUI.lua`, or the SF6 Training
Suite's `Training_ScriptManager.lua`), the plugin leaves the menu to it.

Emergency stop: an empty file `reframework/data/_crsm_native_off.txt` (the plugin does nothing at the next start).

## What the Community sends

Only when you open the Community dialog, download, share or like. Every request goes over HTTPS to the `api_base` of
`reframework/data/SF6_Community_data/config.json`, with the shared `x-api-key` of that file (the same in every package:
it identifies the client, not you) and `x-client-version`.

- **List**: the character, the sort, the page. **Download**: the drill's id.
- **Share**: the character, the drill's name and description, your CFN name as read from the game, and the drill
  file (its slots: names, weights, recorded inputs and which are checked; the dummy's reversal rows when one is in use,
  the dummy's settings, the replay settings and the loaded pack's description, [docs/PACK_FORMAT.md](docs/PACK_FORMAT.md)).
  **Like**: the drill's id, the value and your CFN name.
- **Verification** (once, before the first share or like): a Steam Web API ticket that your running Steam client makes
  for `sf6-community`, your CFN name and the plugin's version. The server checks the ticket with Steam and answers a
  personal token, kept in `reframework/data/SF6_Community_data/identity.json` and sent as `Authorization: Bearer` with
  later requests. Delete that file to forget it on this PC; never share it.

The plugin never sends your SteamID or Steam name, and a name that looks like an id is never sent as a CFN. The server
learns your Steam account id from Steam's answer to the ticket and keeps it for moderation; lists show only the CFN
name, with `[V]` for a verified share. The Community server itself is not part of this repository.

## Report a bug

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
- **Versions**: the Recording Manager's, the Native Core's, the Color Mod's and the Slots Loader's; the size and the start of the SHA-256 of our
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
calls only while the screen is up, wakes it; a few reads a frame while the screen is up; when the screen is left, it goes back to sleep. One atomic load per
frame presented remains (REFramework has no way to take a present callback back). The press costs one frame's copy of the image, about a millisecond;
the encoding, the reading of the files and the requests run off the game thread.
