# SF6 Recording Manager (CRSM)

30 recording slots per character in Training (1-8 are the game's own, 9-30 are extra), loadouts, slot names, a reversal
picker and Community sharing of loadouts, all in the game's training pause menu (Recording Settings > Manager / Record
/ Replay). One native REFramework plugin, `reframework/plugins/SF6_CRSMNative.dll`, client of the SF6 Native Core
(0.8.0 or newer: its training pause-menu service, [core/docs/PAUSE_MENU.md](../core/docs/PAUSE_MENU.md)). No Lua
script. Release package: `SF6_CRSM_<version>.zip`; its `README.txt` ([package/README.txt](package/README.txt)) is the
user guide.

## Layout

```
src/            the plugin (C++20)
  model/        pure model of the files and the slots (no game): loadouts, virtual slots, packs, timelines, locale
  game/         the few game accesses (fields, arrays, references) on REFramework's C++ API
  mech/         the four layers files / replay / reversal / record, their hooks and the code check of each hook target
  ui/           the screens on the game's own menus, the Manager page on the core's pause-menu service, the dialogs
  community/    the Community client: WinHTTP, protocol, identity, the Steam ticket
  owners.hpp    which layer the plugin runs (settings.json); standalone.hpp: when it takes the menu without a Lua script
data/           SF6_CRSM_data/locale.json, the texts in 14 languages (shipped as reframework/data/SF6_CRSM_data/)
package/        the package's README.txt, modinfo.ini, settings.json and the Community config template
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
fake Steam API. Neither reaches the network.

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

- **List**: the character, the sort, the page. **Download**: the loadout's id.
- **Share**: the character, the loadout's name and description, your CFN name as read from the game, and the loadout
  file (its slots: names, weights and recorded inputs). **Like**: the loadout's id, the value and your CFN name.
- **Verification** (once, before the first share or like): a Steam Web API ticket that your running Steam client makes
  for `sf6-community`, your CFN name and the plugin's version. The server checks the ticket with Steam and answers a
  personal token, kept in `reframework/data/SF6_Community_data/identity.json` and sent as `Authorization: Bearer` with
  later requests. Delete that file to forget it on this PC; never share it.

The plugin never sends your SteamID or Steam name, and a name that looks like an id is never sent as a CFN. The server
learns your Steam account id from Steam's answer to the ticket and keeps it for moderation; lists show only the CFN
name, with `[V]` for a verified share. The Community server itself is not part of this repository.
