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
reframework/data/SF6_ColorSpinExtra_data/      settings and colour packs (colors/<Name>_esfNNN/<pack>/)
reframework/data/SF6_DIRecolor_data/, SF6_DriveRecolor_data/, SF6_ColorModNative_data/   Drive settings, caches, log
```

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
