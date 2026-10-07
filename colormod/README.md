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
  game's option window: quick edit, colours per cluster, materials, squares, Save, Save As, Reset.
- **Drive colours** per character and per player: Drive Impact, Drive Reversal, Drive Parry and Drive Rush.
  - on the character select: **X** (Edit Drive Colors) switches the panel to the four Drive rows;
  - in Training: pause menu > character settings popup, **X** as well;
  - in Fighter Settings: the four Drive rows under Control Settings.
- Works with the outfits added by the Slots Loader (`../`): the colour a slot really wears is read from the core.
- Menus in the game's languages (`data/SF6_NativeLocale_data/locale.json`).

## Requirements

- REFramework (`dinput8.dll`), plugin API 1.15 or newer.
- The **SF6 Native Core** package (`sf6core_native.dll`) 0.7.0 or newer, from the same release. Without it the mod
  stays inert and says so in REFramework's log: `[ColorModNative] sf6core_native.dll not loaded: inert`.

## Files in the game folder

```
reframework/plugins/SF6_ColorModNative.dll
reframework/data/SF6_NativeLocale_data/locale.json
reframework/data/SF6_ColorSpinExtra_data/      settings and colour packs (colors/<Name>_esfNNN/<pack>/)
reframework/data/SF6_DIRecolor_data/, SF6_DriveRecolor_data/, SF6_ColorModNative_data/   Drive settings, caches, log
```

## Build

`colormod\build.bat` (Visual Studio 2022 or Build Tools, C++ workload) builds `build\colormod\SF6_ColorModNative.dll`.
It uses the core's header (`core\include\sf6core`) and the REFramework plugin headers (`core\third_party\reframework`).
Nothing is copied into the game folder.

## Credits

The REFramework helpers in `src/ll_common.hpp` and the native menu row technique are LL's work, used with his permission.
