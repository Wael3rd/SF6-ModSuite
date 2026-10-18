# SF6 Color Mod

Costume and Drive colours for Street Fighter 6, set from the game's own menus. One REFramework plugin,
`reframework/plugins/SF6_ColorModNative.dll`, built on the shared core (`sf6core_native.dll`, see `../core/`).

## What it does

- **Extra costume colours**: colour packs (`*_cmd_*.user.2` files) appear after the game's colours in the Color row
  of the character select and of Fighter Settings (labels such as `MC 7`).
  - A match that takes the costume from Battle Settings (online, Battle Hub, Combo Trials...) dresses your own
    fighter in the colour chosen in Fighter Settings > Edit Character. Only the last fighter edited there is
    remembered. Other players see the game's colour. Arcade, Training, Versus and Team Battle use the colour picked
    on the character select.
- **Colour editor**: in Fighter Settings > Edit Character, **F** (or a click) on the Color row opens an editor in the
  game's option window: quick edit, colours per cluster, materials, squares, Save, Save As, Reset. Since 0.20.0 every
  Hue slider (colours, quick edit, squares, Drive colours) is drawn on the game's own rainbow, the avatar editor's;
  it needs SF6 Native Core 0.8.2, with an older core the sliders keep the option window's look. Since 0.21.0 each
  colour has its own page: Hue, Saturation, Brightness, then a 10 x 5 grid of the game's own colour cells (the avatar
  editor's palette): the arrows move the cursor, F applies the colour under it, Backspace gives the saved colour back.
  The grid needs SF6 Native Core 0.8.2; without it the palette rows show the colour under their cursor as text.
- **Drive colours** per character and per player: Drive Impact, Drive Reversal, Drive Parry and Drive Rush.
  - on the character select: **X** (Edit Drive Colors) switches the panel to the four Drive rows;
  - in Training: pause menu > character settings popup, **X** as well;
  - in Fighter Settings: the four Drive rows under Control Settings.
- Works with the outfits added by the Slots Loader (`../`): the colour a slot really wears is read from the core.
- Menus in the game's languages (`data/SF6_NativeLocale_data/locale.json`).

## Requirements

- REFramework (`dinput8.dll`), plugin API 1.10 or newer (the public REFramework 1.5.8 included).
- The **SF6 Native Core** package (`sf6core_native.dll`) 0.7.0 or newer, from the same release. Without it the mod
  stays inert and says so in REFramework's log: `[ColorModNative] sf6core_native.dll not loaded: inert`.

## Files in the game folder

```
reframework/plugins/SF6_ColorModNative.dll
reframework/data/SF6_NativeLocale_data/locale.json
reframework/data/SF6_ColorSpinExtra_data/      settings, edited copies (overrides/), game colour backups (pristine/)
reframework/data/SF6_DIRecolor_data/, SF6_DriveRecolor_data/, SF6_ColorModNative_data/   Drive settings, caches, log
reframework/color_mods/                        colour mods dropped as downloaded and the editor's colours (see below)
```

## Colour mods folder (reframework/color_mods)

Colour mods are handled like costume mods. Drop a colour mod, exactly as downloaded, in
`reframework/color_mods/<Character>/` (or directly in `color_mods`): a `.zip`, `.7z` or `.rar` file, an
unpacked folder (Nexus layout `natives/stm/product/model/esf/esfNNN/CCC/esfNNN_CCC_cmd_KKK.user.2` or the loose
files), anywhere below the folder. The character folder is only for tidiness: the fighter and the costume come from the
file names. The mod creates the folders and a `README.txt` when they are missing.

- Archives are unpacked once, on a background thread started a few seconds after the game starts (never during the
  game's startup, a fight frame or a hook), into the hidden `color_mods/.cache/<name>_<hash>/`; only the colour files
  and `modinfo.ini` are kept. The cache follows the archive (size and date): removing or replacing the archive removes
  its cache. Folders and files whose name starts with a dot are ignored. Zip, 7z and rar are supported (password
  protected archives are refused).
- The colours show as `RW n`, then the editor's colours as `MC n`. Colours already unpacked by a previous start are there at launch. Newly unpacked ones are added to the character's colour list
  as soon as the game is out of a fight or the hub, the character select and Fighter Settings (about 1 s later);
  otherwise at the next launch.
- The colour editor can edit them: SAVE keeps the edit apart (an override), RESET goes back to the original. A mod's
  file is never written, emptied or deleted. The editor's own colours (SAVE AS, `MC n`) are saved in
  `color_mods/<Character>/MANUALCOLORS_NN/`; ERASE deletes those only.
- The former colour folder `reframework/data/SF6_ColorSpinExtra_data/colors/<Name>_esfNNN/<pack>/` (versions up to
  0.21) is moved into `color_mods/<Character>/<pack>/` at the first start, with the selections, squares and edited copies
  that named its files; then it is removed and never read again. A file dropped there later moves at the next start. Remembered colour selections (`state.json`) store the colour's identity next to its rank, so adding or removing colours
  never changes which colour a player had selected (a state saved by an older version keeps its rank and gets the identity
  at the first start). An override of a colour from an archive survives the archive being replaced.
- Messages (`color_mods: ...`, one line per archive: unpacked in N ms / cannot be used and why) are in
  `reframework/data/SF6_ColorModNative_data/debug_log.json`.
## During a fight

The colours are decided in the menus; a fight recomputes nothing (since 0.19.7):

- The Drive Parry / Rush colours are written once into the effect data when the fight loads, again only when a
  colour setting changes (the Training character popup) or the effects are found again. The game shares that data
  between both players (and the Drive Impact data in a mirror match): it shows your side's colours, or the other
  side's when yours are left to the game.
- The Drive Impact / Reversal colours and the body glow are painted when each effect starts, per player: the game
  copies those for each effect, a single write does not hold.
- Nothing is recomputed periodically during a fight (a settings comparison once a second); the costume colours are
  applied when the game dresses a model, with a check during the first 10 s of the fight only.

The previous way (every Drive effect painted per player when it starts, the effect data checked every second) comes
back with an empty file `reframework/data/SF6_ColorModNative_data/live_fight.txt` (looked at every 5 s).

When a fight starts, the work that finds and writes the Drive colours is spread over the first frames (at most about
1 ms of work per frame, since 0.19.9); the colours are in place well before anyone can use Drive.

To see what the mod costs, create `reframework/data/SF6_ColorModNative_data/perf_on.txt` (looked at every 2 s):
`perf_live.log` next to it then gets one line per second (frame time, the mod's time per frame and its costliest
parts, each hooked game method counted on its own) and one `SLOW` line per long frame with everything the mod did in
it. Delete the file to stop. `reframework/data/perf_on.txt` does the same for the Native Core and the Slots Loader
plugin (their own `perf_live.log` files).

For crash investigations, `reframework/data/write_journal_on.txt` makes the mod (and the Slots Loader plugin) log
every write it makes into the game's memory to `writes.log`. Leave it off otherwise.

## Build

`colormod\build.bat` (Visual Studio 2022 or Build Tools, C++ workload) builds `build\colormod\SF6_ColorModNative.dll`.
It uses the core's header (`core\include\sf6core`) and the REFramework plugin headers (`core\third_party\reframework`).
Nothing is copied into the game folder.

## Credits

The REFramework helpers in `src/ll_common.hpp` and the native menu row technique are LL's work, used with his permission.
