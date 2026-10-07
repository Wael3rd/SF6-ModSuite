# Development notes

State on 2026-09-30. Game version Ver.2.0401.010.

## Status

Verified in game:

- UP / DOWN cycles Training Room through Aokigahara, Aokigahara with NPCs, NULSPACE and the five Training
  Room colours of Stage Lighting Overhaul; Metro City Downtown and Carrier Byron Taylor through their lighting
  variant. Name, preview and UP / DOWN hints follow; stages without variants are left alone.
- The battle loads the selected variant (NULSPACE, Aokigahara with NPCs, Training Room Red then Yellow in the
  same session: the stage is reloaded each battle, the resource cache does not get in the way).
- The VS screen shows the variant's name and image.
- Second launch with nothing changed: stage pass in a few ms. Stage Lighting Overhaul (RAR, 120 MB, 187
  files) unpacks and builds in about 5.6 s the first time.

Not verified: the pad symbols of the hints (no pad connected; the API returns `<ICON DirU>` / `<ICON DirD>`
for the GamePad mode), and whether `InputGuideManager._Modes[0]` switches when the player changes device.

Known bad mod: *Aokigahara - no NPC* hangs on the VS screen, also as a plain Fluffy pak without the
redirection. It shares its bridge and World Tour props (`wtc0202`) with the NPC version, which loads.
Cause (2026-10-07): of its 10 files, 9 are byte-identical to the NPC version's; the 10th, the Training Room level
scene `level/scene/ess/ess0000_00/ess0000_00_level.scn.20` (the game's own is an empty 128-byte scene), places four
World Tour `om_setter`s (`Product/OM/common_wt/setter/om_setter.pfb`, gimmicks gm_017011, gm_047001 to 003, with
`WTCityOMSetter`, mission and save-data settings). Without that file the variant loads (tested in Training). Not a
type layout mismatch: `Om047000SettingParam` in the mod has CRC be5044be, as in the game's `wtc0202_01.scn.20`
(a REasy RSZ dump of May 2026 gives 2c6d6ebc and fails to parse the scene).

## Open items

1. Stage mods installed through Fluffy (their paks in the patch chain): read them as variants and restore the
   vanilla files under them.
2. Done 30/09: the costume loader leaves the pak carrying the stage marker out of its scan and fingerprint
   (a change of stage mods no longer rebuilds the costume pak) and moves its own pak when only the stage
   pak's presence changed.
3. Done 2026-10-07: the Aokigahara (no NPC) hang traced to its level scene (see Known bad mod). Tracing stays
   available: `debug.txt` in `SF6_StageSlots_Data` with `trace=<text>` logs every hashed path holding the text.
4. Done 30/09: merged into SF6-SlotsLoader (formerly SF6-CostumeSlotsLoader), under `stages/`, history kept.
5. Optional: preview framing (a 4:1 band of a 16:9 screenshot looks zoomed next to the game's own key art;
   blurred side fill was proposed, not wanted for now).
6. Done 2026-10-07: the watcher (`stage_redirect.cpp`) published an empty selection when state.json could not be
   read (the plugin replaces it by a rename) and never read it again (same file time): a quick UP / DOWN could
   leave the battle on the original stage. An unreadable or incomplete file is now read again every 50 ms (2 s
   at most, the previous selection kept meanwhile).

## Engine facts

### path_to_hash

- `uint64 path_to_hash(const wchar_t* path)` at exe+0x6fe9ca0. Upper 32 bits = murmur3 of the upper-case
  UTF-16 path, lower 32 bits = murmur3 of the lower-case one, seed 0xFFFFFFFF (same as the pak entries).
- Paths arrive absolute: `C:\...\Street Fighter 6/natives/STM/Product/...`, mixed case.
- Found by its tail, unique in the executable: `41 B8 FF FF FF FF 4C 8D 4C 24 24 48 8B CB E8 ?? ?? ?? ??
  8B 44 24 20 8B 4C 24 24 48 C1 E0 20 48 0B C1`, callee checked for the murmur3 constant 0xCC9E2D51, entry
  found walking back to the int3 padding (`40 55 53 41 56 48 8D AC 24`, or `E9` when already hooked).
- HARD READ (`hardread_core.dll`, loaded as `amd_ags_x64_chain.dll`) hooks it first, so the entry is a jump;
  REFramework's own LooseFileLoader scan then fails ("Failed to find path_to_hash candidate"). Our hook
  chains behind that jump.
- MinHook fails late in the session (MH_ERROR_MEMORY_ALLOC): no free memory within 1 GB of the 600 MB
  executable. Hence the block reserved from DllMain and the hand-written 5-byte hook (atomic 8-byte
  exchange on the 16-byte aligned entry).

### Stages

- Stage id = `essNNNN_MM` -> NNNN*100 + MM (Training Room ess0000_00 = 0, Old Town Market ess0100_00 = 10000).
- Swapping two vanilla stages does not work: a stage loads data tied to the id the game thinks it loads
  (ess0000-only textures missing, the SST streaming data of the other stage never prepared -> endless
  loading). Mods made for the stage's own id work, which is the only case needed.
- Stage lists: `app.battle.bBattleStageSelectFlow.mStageSelect` (`app.menu.UIFlowStageSelect.Param`):
  `get_StageIdList`, `GetSelectIndex`, `text0` (stage name, `c_stage/e_txt_country`), `texture0` (preview,
  `c_image/e_tex_image`, control 1920x480), `PreviewTexCahceDataList` (StageId + TextureResourceHolder per
  stage, vanilla previews `Product/GUI/data/Area_Image/ess/<ess>/tex_StageImage_<ess>_IM.tex`, 2048x512
  BC1_UNORM_SRGB, flags 0x800, 1 mip).
- VS screen: agent `VSInfoOffline`, `c_bg/e_text_stagename` and `c_bg/e_texture_bg`.

### GUI

- A `TextureResource` created by the script must finish loading before it is handed to a `via.gui.Texture`:
  `setTexture` on a resource still loading showed white, then crashed the game's LateUpdate. The script
  creates previews when a stage is focused and uses them 60 frames later.
- `getTexture` returns a new holder each call: compare textures by the native resource at holder+0x10.
- Input: `app.UIAgent.InputUp / InputDown(app.InputDigitalFlag)` on the focused agent (key configuration
  applied; flag bit 2 = trigger, 8 = repeat). Hooked read-only, filtered on the StageSelect agent.
- Input symbols: `app.InputGuideManager.GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode,
  Int32, app.EConfigInputType)`; UISelectU = 65, UISelectD = 66; mode 1 GamePad, 2 KeyboardAndMouse;
  current mode in `_Modes[0]`. Icon names: `<ICON KeyZ>`, `<ICON DirU>`, `<ICON DirV>`, `<ICON KeyArrowU>`...
  (full table in `_InputSymbolTextData`). Icons render inline in `set_Message` texts.

## Driving the game for tests

`reframework/agent/sf6.py` (REFramework WebSocket build): `launch`, `shot`, `run <lua> --wait <name>`,
`reset`, `logs`. Keys through `sf6.tap` (AZERTY scancodes). Path to the stage select: title (Enter or
Space; R/F taps are sometimes ignored there) -> main menu, F on Fighting Ground -> VERSUS One on One F ->
Next F -> sides F -> stage select (Q/D move, Z/S = UP/DOWN). From a battle: Esc -> Return to Stage Select,
or the result menu's Change Stage.

## Deploying

- Game folder `amd_ags_x64.dll` = root `build.bat` output `build\amd_ags_x64.dll` (costume + stage slots).
  Copy it with the game closed. `build\costumes_only\` is the release build without stage slots (no hook).
- Also deployed: `build/SF6_CostumeSlotsNative.dll` to `reframework/plugins/` (the stage select part is in
  `plugin/src/stages.cpp` since 1.9.0; the Lua script is gone),
  `stages/data/loader/stage_paths.txt` to `reframework/data/SF6_StageSlots_Data/loader/`. A git checkout
  may turn `stage_paths.txt` to CRLF: the loader reads both, but its size is part of the stage fingerprint,
  so copying a checkout with other line endings rebuilds the stage pak once (a few seconds).
- Backups in the game folder: `amd_ags_x64.dll.bak_20260930_prestage` (costume slots only, before stage
  slots), `amd_ags_x64.dll.bak_20260930_premerge` (last build of the standalone stage repo).
