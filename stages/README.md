# SF6 Stage Slots

Street Fighter 6 stage mods replace a stage: install a Training Room mod and the original Training Room is
gone until you uninstall it, and two mods for the same stage cannot live together. Stage Slots keeps the
original and every mod side by side: on the stage select screen, **UP / DOWN** cycles the focused stage
through its original look and each mod installed for it. The name and the preview image follow, the VS
screen shows the choice, and the battle loads it.

Nothing is installed over the game's files: each mod's files are copied into a patch pak under names of
their own, and served in place of the original files only while that mod is selected.

## Install and use

Stage Slots is part of [SF6 Slots Loader](../README.md): it ships in the same `amd_ags_x64.dll` as the
costume slots (`build\amd_ags_x64.dll`, see *How it works*), plus the native plugin. It is in the releases since
1.9.0 as an **experimental** feature: the release archive puts every file below in place. You need REFramework
in `dinput8.dll` (without it the stage pak is built but nothing can be selected).

| File | Where |
|---|---|
| `build/amd_ags_x64.dll` (costume slots + stage slots) | game folder |
| `build/SF6_CostumeSlotsNative.dll` (native plugin) | `reframework/plugins/` |
| `stages/data/loader/stage_paths.txt` | `reframework/data/SF6_StageSlots_Data/loader/` |

Put stage mods **as downloaded** in `reframework/stage_mods/`: the `.zip`, `.7z` or `.rar` file itself, a
folder with a `natives` tree, or a `.pak`. Launch the game.

- Each option of a mod becomes one variant of the stage it changes. A pack covering many stages (Stage
  Lighting Overhaul: 24 options) gives one variant per stage and option.
- On the stage select screen, a stage that has variants shows its name between the UP / DOWN symbols of
  the device in use (keys on a keyboard, directions on a pad), the way the game's BGM selector frames its
  value. UP / DOWN changes the variant; the choice is kept per stage
  (`reframework/data/SF6_StageSlots_Data/state.json`).
- Names come from the mod's `modinfo.ini` (`name=`). A variant named like the stage itself (a lighting pack
  names its options "Bather's Beach") shows the pack name (`nameAsBundle=`) instead.
- Previews are made from the mod's screenshot (`screenshot=` in `modinfo.ini`, or the first image of the
  folder): a 4:1 band at 60% of the height, 2048x512, BC1 like the game's own. A mod without an image keeps
  the stage's original preview.
- Variants can change from one battle to the next without restarting the game.
- UP / DOWN works on the stage select screen of Fighting Ground > Versus.
- To remove a mod, delete its archive or folder: the stage pak is rebuilt at the next launch. Archives are
  unpacked once into `stage_mods\.cache` (as much disk space as their unpacked size, 120 MB for the lighting
  pack); that folder can be deleted at any time.
- Without stage mods, no stage is changed and nothing is hooked.

## Limits

- **The choice is local, and it applies to every load of that stage**: training, arcade, replays, and online
  too (a random stage in ranked included), since the redirection is native and does not look at the mode. The
  game only knows the vanilla stage id, so another player sees their own version of the stage. To get the
  original back, select it again on the stage select screen. **Online play has not been tested.**
- Only the stage select screen of Fighting Ground > Versus was tested. The other places where a stage is
  chosen (rooms, Battle Hub, matchmaking settings) were not checked and probably show no UP / DOWN hint.
- Files of a mod outside the stage's own folder (shared props: the bridge and World Tour area of Aokigahara,
  the lamps of the lighting pack) are replaced wherever the game loads them while the variant is selected. In
  theory; not seen.
- The UP / DOWN hints were tested with a keyboard only.
- The preview is a 4:1 band cut from the mod's screenshot, more zoomed in than the game's own pictures.
- **Stage mods installed through Fluffy Mod Manager are not variants yet**: their pak replaces the stage for
  everyone, the loader does not restore the original under it.
- A mod that hangs the game on its own hangs it here too (seen: *Aokigahara - no NPC* stays on the VS screen,
  also when installed as a plain Fluffy pak). One cause is repaired: a texture whose level table declares
  wrong row sizes, which the game waits for forever (the Training Room pictures of Stage Lighting Overhaul,
  1.9.1). The log names each texture repaired or left out.

## How it works

The game imports `amd_ags_x64.dll` from its own folder, before any engine code runs. Its `DllMain`
(`proxy/slots_proxy.cpp`) runs the stage slots around the costume loader:

1. **Stage pass** (`stages/loader/stage_loader.cpp`, DllMain). Reads `reframework/stage_mods`, unpacks archives once
   into `stage_mods/.cache`, and recognises the stage of every file with `stage_paths.txt` (every game path
   holding a stage code `essNNNN_NN`, hashed at startup). Each file of a variant is stored in our patch pak
   under `pak_path_hash("natives/stm/_stageslots/<variant>/<vanilla hash>")`, next to its preview texture.
   The stage pak goes right above the mod paks and below the costume pak (marker
   `natives/stm/sf6_stage_slots.marker`). Every texture of a mod is checked first: a wrong level table is
   repaired (`texture_mip_check`, shared with the costume loader), a texture that cannot be repaired is left
   out. Nothing changed since last launch: the saved table is reused (a few ms).
2. **Costume pass** (`costumes/`). It recognises the stage pak by its marker and leaves it out of its scan
   and of its fingerprint, so adding or removing a stage mod does not rebuild the costume pak; its own pak
   stays above the stage pak (moved, not rebuilt, when the stage pak comes or goes).
3. **Redirection** (`stages/loader/stage_redirect.cpp`). The engine turns every file path into a pak hash through one
   function, `path_to_hash`. A small block of memory is reserved near the executable at startup; once a
   variant is selected (and never in the first 20 s), the entry of `path_to_hash` gets a 5-byte jump to our
   detour, which swaps the hash of each vanilla file the selected variant replaces for its copy's hash. A
   tool that hooked the function first (HARD READ) stays in the chain. The selection is re-read from
   `state.json` when it changes.
4. **Native plugin** (`plugin/src/stages.cpp`, in `SF6_CostumeSlotsNative.dll`). Hooks on the stage select
   screen (shown, hidden, focus and preview changed), on UP / DOWN of its UI agent and on the VS screen: the
   variant's name between UP / DOWN hints and its preview, `state.json` written for the loader. Game thread
   only, texts written only when they differ, nothing done between events.

Outputs, in `reframework/data/SF6_StageSlots_Data`: `registry.json` (variants per stage, for the script),
`loader/variants.tsv` (the same with every redirection), `state.json` (selection). Log:
`SF6_StageSlots.log` in the game folder.

## Building

`build.bat` at the root of the repository (Visual Studio 2022 or Build Tools with the C++ workload) builds
both loaders and the tools into `build/`:

```
build\amd_ags_x64.dll   costume slots + stage slots
build\stagepak.exe, stagetest.exe, textest.exe
```

- `stagetest <folder>` runs the stage pass on a folder laid out like the game's (no game needed).
- `textest <image> <out.tex>` makes a preview; `textest --dump <in.tex> <out.png>` decodes one.
- `stages/tools/make_stage_index.py` rebuilds `stages/data/loader/stage_paths.txt` from REasy-parser's
  `SF6_STM.list` (point `REASY_PARSER` at your checkout).

Development notes (engine facts, test status, open items): [docs/DEV_NOTES.md](docs/DEV_NOTES.md).

## Third party

`stages/loader/third_party`: stb_image, stb_image_write, stb_dxt (public domain or MIT). bcdec (MIT, used by
`textest` only) and the pak, archive and decompression code come from `common/`. See
[THIRD_PARTY.md](../THIRD_PARTY.md).
