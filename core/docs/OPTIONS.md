# Option dialogs (increment 6, core 0.6.0)

Design decision: ADR-0013 (accepted 2026-10-06): the game's option window is a service of the core, for every mod.

The core shows its clients' groups and entries in the game's own option window (`app.Option` units started through
`UIFlowOptionBGDialog`), with texts given through a hook on `hMsg.GetMessage(System.Guid)`. The engine came from
SF6_ColorModNative 0.18.5 unchanged.

## Files (`core/src/options/`)

| File | Role |
|---|---|
| `options_core.{hpp,cpp}` | The engine's state machine (registry, dialog life cycle, value polling, events), no game header |
| `options.{hpp,cpp}` | The game side: builds the units, starts / ends the dialogs, the eight hooks, row lookups |
| `guid_text.{hpp,cpp}`, `guid_table.{hpp,cpp}` | The GUID text provider and its `GetMessage` hook, shared with the pause-menu service (increment 8) |
| `ll_common.hpp` | Low-level managed-object helpers (copy of ColorMod's `menurows/ll_common.hpp`) |
| `opt_common.hpp` | What the moved files used from the mod: journal (core log), warning, API table, client |
| `options_service.cpp` | The generic driver, the client routing and the `api_opt_*` functions |

Offline bench of `options_core.cpp` (scripted backend, no game): `core\tests\options_bench\run.bat`.

## Client model

- `opt_set_callbacks(client, cb)` once. The hooks are installed on the next frame (a core without option clients
  hooks nothing). `opt_status` starts with `options: failed` when they could not be.
- `opt_group` (one page), `opt_entry` (button, slider, choice), `opt_group_def` (title / description applied at the
  next build), `opt_remove_group`. Ids are the core's, unique across clients.
- Requests: `opt_open_menu(title, focus)`, `opt_navigate(key, flags, focus)` (`SF6CORE_OPT_NAV_REBUILD | NOW | PUSH |
  POP`), `opt_close`, `opt_rebuild`, `opt_ensure_built`.
- Callbacks (game thread, SEH-guarded, may call `opt_*`):
  - `on_value(entry, value)` and `on_button(entry, out)` for the client's own entries; `out` says what the press asks
    for (close, open a page with rebuild / push / pop);
  - `on_dialog_created`, `on_page_setup(param, rows)` (rows = the client's entries on that page), `on_page_popped`,
    `on_window_closed` for every client.

## Rows in a section of the game's Options screen (increment 11, ADR-0021)

`opt_group_place(client, group, tab, section)` turns a group's entries into rows of an existing section of the
game's own Options screen instead of a page of the core's window. `tab` = `app.Option.TabType` (General 0,
**Interface 1**, Battle 2...), `section` = `app.Option.GroupType` of the section (**290 = OthersSettings_Interface**,
Interface > Other Settings; 190 / 390 = the General / Battle ones); `-1, -1` = back to the core's window. Probe it
with `SF6CORE_HAS(api, opt_group_place)`.

- Same engine: the units are built like a page's rows (`OptionSettingUnit` -> `MakeUnitData`, `OptionValueSetting`,
  GUID texts, type ids by key); only their parent changes: the section's `ChildUnits` (and its setting's
  `ChildUnitList`) instead of a group unit. The game draws them with its own rows. No group unit and no page for a
  placed group; its buttons are not built (a section row holds a value).
- Applied at the next build. Each build replaces the rows (same TypeId, new unit); every 60 ticks the engine
  re-inserts a row the game's lists lost and removes the rows of removed / moved groups. Nothing of this runs for a
  tree that never placed a group (no game call, no extra hook).
- **No link with the game's save**: `LoadValueEvent` / `ResetEvent` skip the core's ids (as for every entry: the
  section's "Restore Default Settings" leaves the row as it is) and `OptionValueUnit.SaveEvent` (hooked on the first
  build that places a group) skips the placed rows after setting their `PrevValue` to their value. The client stores
  the value itself and passes it back as the entry's value at start.
- Values: `on_value` as for a page (choice 1-based), from the engine's full pass (every 10 ticks). A value the client
  changes (its own file edited) reaches the row at the next build: `opt_entry` value kept by the core, then
  `opt_rebuild` + `opt_ensure_built`.
- `opt_status` adds `placed sections=.. rows=.. save_hook=..` once a group was placed.

Example (a toggle "Display NSFW Outfits" in Interface > Other Settings):

```cpp
static void SF6CORE_CALL on_value(void*, int64_t entry, int64_t value) {
    if (entry == g_nsfw_entry) save_display_nsfw(value == 2);        // choice: 1 = Off, 2 = On
}

SF6CoreOptCallbacks cb{ sizeof cb };
cb.on_value = on_value;
api->opt_set_callbacks(client, &cb);
const int64_t g = api->opt_group(client, "slots.interface", "Slots Loader", "");
const char* labels[] = { "Off", "On" };
SF6CoreOptEntryDesc d{ sizeof d };
d.kind = SF6CORE_OPT_CHOICE; d.key = "display_nsfw"; d.title = "Display NSFW Outfits";
d.desc = "Show the outfits marked NSFW on the character select.";
d.options = labels; d.n_options = 2; d.value = load_display_nsfw() ? 2 : 1;
g_nsfw_entry = api->opt_entry(client, g, &d);
if (SF6CORE_HAS(api, opt_group_place)) api->opt_group_place(client, g, 1, 290);   // Interface > Other Settings
api->opt_rebuild(client); api->opt_ensure_built(client);
```

## A section of a game tab's left list (increment 13, core 0.9.9)

`opt_section_place(client, group, tab, before_section)` adds the group as one more section of the left list of a game
Options tab, just above the game's section `before_section` (e.g. 1 / 290: Interface, above Other Settings; -1 = at the end
of the tab). Its right side is the client's page: the core shows no row there, it reports the screen's state and blocks
the screen's input on demand; the client draws (copies made from the screen's own update, R19) and reads its keys. Probe
with `SF6CORE_HAS(api, opt_section_place)`.

- **Texts**: the group's title is the left list's line, its description the screen's description line while the section
  is focused. Both are GUID texts owned by the sections (`guid_text::alloc_owned`: the engine's purge never blanks them);
  `opt_group_def` changes them in place (next frame), no build needed. The group's entries are ignored (no row).
- **The unit** (`options.cpp`, made once per section and held): an `app.Option.OptionGroupUnit` whose setting is DataType
  Group, InputType Button_Type1, EventType and ExtraEventType Invalid, empty ValueMessageList / ChildUnitList, TypeId from
  the core's range (= its GroupType), SupportModeType / SupportPlatform / IsOnlineSetting copied from `before_section`
  (else the tab's first group). No child: nothing for the game to load, reset or save; Confirm on it opens nothing.
- **Where**: `OptionManager.UnitLists[tab]`, just before the unit whose `get_GroupType()` is `before_section`. Checked at
  each build and every 60 frames: re-inserted after a rebuild of the game's lists, stale copies (a moved or removed
  section) dropped by TypeId. `OptionMenuParam.CurrentMainUnitLists` (the screen's copy, to which its left list's parts are
  aligned) is never touched: the section shows from the next time the game fills the tab (entering it, opening Options).
- **State** (`opt_section_state`, read once per core frame, before the clients' frames, while a section exists; every 15
  frames while the screen is not up): the screen = the agent "OptionSettingMenu" drawn, its `<InputDelegates>.<Flow>
  .get_MenuParam()` an `OptionMenuParam` with `IsShowed`. `shown` = the screen is up on the section's tab and
  `CurrentMainUnitLists` holds the section; `focused` = shown, `MenuState` MAIN (0), `ListState` MainList (0) and
  `GetMainListFocusUnit()` is the section; `item` = the left list's part whose UnitData is the section, its control
  (`p_OptionUnit_N_switch_show`), only while shown; `right_panel` = `mCtrlBGRight` (c_bg_right); `area` = c_bg_right/e_bg's
  global place (top left) and size, read every 30 frames (912, 145, 910 x 833 read 08/11, also the fallback); every
  pointer is this frame's only. For a group whose EventType is not OpenSubMenu the game hides its own sub list and radio
  group itself (ForceInvisible, read 08/11): the client must not write those flags.
- **Hold** (`opt_section_hold`): 1 = the screen's agent is blocked (`ui_set_block_input`, the core's `sf6core.options`
  client) and H2 swallows the left list's Back meanwhile; only while shown. 0 = given back: the client calls it once every
  key its page read is up (or the game gets that key: a Back would leave the Options screen). The core gives a hold back
  by itself when the screen goes (or another one comes), when the section leaves the left list, when the section or its
  client goes, and when the sections are switched off. The client must not block the agent itself as well.
- **Hooks**, each one only after the code check (R18) found no other method entering its code (as the pause-menu engine's
  check: every method's entry and jump target compared, on a thread of its own, started on the first frame with a
  section; result kept in `reframework/data/sf6core/opt_codecheck.txt` per game executable; one `add_hook` per frame):
  - H2 `app.UIOptionSettingMenu.Flow_MainList.InputDelegates.EventCancel()`, pre: skipped while a section is held.
    Refused or missing: a hold is the input block alone (logged once).
  - H1 `app.UIOptionSettingMenu.OptionMenuParam.InputEvent_DecideList()`, post: with the left list's cursor on a section,
    `decides` + 1 and the result Invalid (the unit's own EventType already). Refused or missing: `decides` stays 0, the
    client reads Confirm itself while `focused`.
- **Faults**: 5 faults in the sections' code, or the option hooks switched off: every section is withdrawn from its tab's
  list and every hold given back. `opt_status` adds `sections=.. live=.. hooks=check:..,H2:..,H1:.. held=.. cancels=..
  faults=..` once a section was placed.
- A group is either placed (`opt_group_place`, rows in a game section) or a section, never both (`ERR_INVALID_ARG`).
- Not done here: the mouse on the right side. A section with rows of its own on the right: increment 14.

## A section with the game's own rows on its right (increment 14, core 0.9.10)

`opt_section_place_rows(client, group, tab, before_section)` is `opt_section_place` (same place in the left list, same texts,
same `opt_section_state`, `-1, -1` = no section any more), but the section's right side is the **game's own sub list**, filled and
run by the game as for its own sections: the group's entries are its rows (choice: title, `desc` = the screen's description line
while the row is focused, `options[]` cycled by Left / Right, value 1-based; slider as a placed row; buttons are not shown).
Confirm on the section moves the cursor into its rows, Back brings it back to the left list; no hold (`opt_section_hold` 1 =
`ERR_INVALID_ARG`), no client page, no hook of its own. Probe with `SF6CORE_HAS(api, opt_section_place_rows)`.

- **What the game does with a section** (read 08/11, `sp_data` / `sp_types`): a game section with rows is an `OptionGroupUnit`
  whose setting is DataType Group, **EventType OpenSubMenu (2)**, its children (`ChildUnits`, their settings in `ChildUnitList`)
  the value units of its rows (Interface > Other Settings, 290: InputType Button_Type1, SupportModeType 55, SupportPlatform 0, not
  online). With the left list's cursor on it, the screen copies its children into `OptionMenuParam.CurrentSubUnitLists` (another
  list object; with the cursor on Battle HUD, EventType 7, the copy was empty although that group has 7 children) and shows them in
  its sub list `mSubListGroup` (`p_GroupScroll_sub_v`, a `UIPartsGroupScroll` of **25 parts** `p_OptionUnit_0 .. 24`, 58 apart,
  scrolled by the game). Confirm (OpenSubMenu) = MenuState SUB (1), ListState SubList (1), `GetSubListFocusUnit()` the row under
  the cursor.
- **The section's unit** (`options.cpp`, `make_rows_section_unit`, made once and held): its setting is a **copy
  (`MemberwiseClone`) of a real section's** with rows on its right: `before_section` when it is one, else the tab's first one,
  else another tab's (never a section of ours); then its own TypeId (the core's range), its title / description (owned GUID texts,
  as increment 13) and **its own empty ValueMessageList / ChildUnitList** (the copy shares the real section's lists until they are
  replaced: the unit is refused when that fails, so the game's lists are never touched), `MakeUnitData`, an `OptionGroupUnit`
  with an empty children list of its own (checked: not the source's, empty). Placed in `UnitLists[tab]` as increment 13. The log
  gives the source section (GroupType, title, tab, row count) and every field of its setting, its first row's, ours.
- **Its rows** (`build_section_rows`, at each build): one value unit per entry, built as a placed row's (`OptionSettingUnit` ->
  `MakeUnitData`, `OptionValueSetting`, InputType SpinText / Slider, TypeId by the key `e:<group>/<entry>`), but **made once and
  kept across builds**: their texts are owned (never purged by a build) and changed in place. So the rows the screen shows (its
  copy of the children) are always the live units: values read from them, texts never blank, nothing freed under the game. Only
  another structure (another number of values or slider range: not possible through the C API, an entry's definition is fixed)
  makes a new unit; the old one, like the unit of an entry that went (its group removed, or no longer a section with rows), stays
  held and its texts are given back once no list of the screen shows it.
- **Its children** (`sec_sync_rows`, at each build, every 60 frames and when dirty): `ChildUnits` and the setting's
  `ChildUnitList` = the entries' units in entry order, rewritten only when they differ (our own lists: the screen shows its copy).
  EventType OpenSubMenu with a row, Invalid without one (Confirm then opens nothing, the right side stays hidden). **At most one
  row per part of the sub list** (25, read again from each screen's `mSubListGroup.GetChildCount()`): the entries past it are not
  shown (a warning names the count). A longer list is split into several sections by the client.
- **When changes show**: values at once (`opt_set_value` = `SetValue(v, true)` on the shown unit, then `UpdateDisp()` on the sub
  list's part showing it: SetValue alone left the row's text unchanged until the player moved its value, seen 10/11); `opt_group_def` (the section's
  texts) and `opt_set_option_text` (a row's value text) in place; entries added later become rows at the next build
  (`opt_rebuild` + `opt_ensure_built`) and **show from the game's next fill of the right list** (the left cursor coming onto the
  section, entering the tab): the core never refills the screen's list itself. Placing the group with rows (or taking them away)
  starts that build itself when the service is up; the client's own `opt_rebuild` + `opt_ensure_built` afterwards is harmless (a
  kept row is not made again).
- **Values**: `on_value` (choice 1-based) for every change the player makes: the section's entries are read **every frame while
  the cursor is in its rows** (`poll_group`), and every full pass (10 ticks) otherwise, the window closed or not.
- **The game's "Restore Default Settings"** (the game's button at the end of the rows, seen in game on 10/11, and its own
  prompt) **works on them**: the game calls `ResetEvent` on each unit it resets; the hook still skips every unit of
  ours (nothing of the game's reset code runs on them) and notes it; at the next frame each live row of a section with rows goes
  to its entry's default (`SF6CoreOptEntryDesc.has_default` / `default_value`, appended in 0.9.10, read when `struct_size`
  covers them; without one: choice 1, slider `min_val`; kept in range) with `SetValue(v, true)` and the part's `UpdateDisp()`, shown at once, and the read
  that follows calls `on_value` for each row that changed. Rows placed in a game section (increment 11), the core's own window
  and every unit of the game are not touched by this: a game section's reset runs as before. Logged: the prompt over the rows
  (`the game's reset prompt is up over its rows`), each row moved (`a -> b`), the count per section; `opt_status` `resets=..
  rows_reset=..`.
- **No link with the game's save**, as increment 11: the entries are placed (`LoadValueEvent` / `ResetEvent` skip them;
  `SaveEvent`, hooked at the first build with a placed group or a section with rows,
  sets their `PrevValue` and skips them). Since 0.9.10 the three hooks also skip a unit of ours that is **no longer known** (a
  rebuild, a removed group: the screen's copy of a list may still hold it), so a retired row is never loaded, reset (by the game or by the above) or saved either.
- **Restore Default Settings on a section with a client page** (increment 13's sections, 0.9.10): the game has no unit of it to
  reset, so H3 (`OptionMenuParam.InputEvent_ResetCurrentMainListValue`, code-checked with H1 / H2, the game's code runs on) counts the
  reset confirmed with the left list's cursor on it: `SF6CoreOptSectionState.resets` (appended after `focused_entry`); the client puts
  its page's settings back when the count moves. A row reset by the game (a section with rows) is shown again once the reset prompt is
  gone (DispSet + UpdateDisp on its part, retried for 600 frames: the reset comes while the prompt is up and the game then shows its
  parts as they were).
- **State**: `SF6CoreOptSectionState.focused_entry` (appended, `struct_size` versioning): the entry under the right list's cursor
  (MenuState SUB, ListState SubList, the right list holding the section's rows, `GetSubListFocusUnit()` one of them); 0 on the left
  list, on another section's rows, or when the screen is not up; always 0 for a section with a client page. `focused` keeps
  increment 13's meaning (the left list's cursor on the section). `held` and `decides` stay 0 (H1 / H2 serve the sections with a
  client page only, and their code check runs only while one exists).
- **Headers**: none. The game has no non-interactive title or separator unit for a sub list (`UnitInputType`: Button_Type0 - 3,
  SpinText, SpinText_OnOff, SpinText_Num, Slider; `UIPartsOptionUnit.ItemPartsType`: Button, Spin, Slider; none in any section
  read 08/11), so there is no `SF6CORE_OPT_HEADER`.
- **Log** (the core's, one run is enough): the source section and the fields copied; each row made; each rewrite of the children
  and EventType; the sub list's parts; every fill of the right list that shows (or stops showing) the section's rows, with the
  screen's states; the left cursor coming onto / leaving the section (with the right list's content); a warning when it stays 30
  frames on the section and the right list holds none of its rows (the one thing left to the game); the cursor going into /
  leaving its rows; the row under the cursor (entry, type id, title, value); each `on_value` and `opt_set_value` of a row.
  `opt_status` adds `rows: sections=.. units=.. made=.. shown=.. fills=.. retired=.. parts=.. save_skips=.. save_hook=..`.

Example (two rows in a section "Costume Options" of Options > Interface, above Other Settings):

```cpp
static void SF6CORE_CALL on_value(void*, int64_t entry, int64_t value) { save_row(entry, value); }   // choice: 1-based

SF6CoreOptCallbacks cb{ sizeof cb };
cb.on_value = on_value;
api->opt_set_callbacks(client, &cb);
const int64_t g = api->opt_group(client, "slots.costume_rows", "Costume Options", "Options of the outfits of the costume packs.");
const char* onoff[] = { "Off", "On" };
SF6CoreOptEntryDesc d{ sizeof d };
d.kind = SF6CORE_OPT_CHOICE; d.key = "nsfw"; d.title = "Display NSFW Outfits"; d.desc = "Show the outfits marked NSFW.";
d.options = onoff; d.n_options = 2; d.value = load_row("nsfw");
api->opt_entry(client, g, &d);
if (SF6CORE_HAS(api, opt_section_place_rows)) api->opt_section_place_rows(client, g, 1, 290);   // Interface, above Other Settings
api->opt_rebuild(client); api->opt_ensure_built(client);

// each frame: the picture of the focused row's value (an older core leaves focused_entry 0)
SF6CoreOptSectionState st{ sizeof st };
if (api->opt_section_state(client, g, &st) == SF6CORE_OK && st.focused_entry) show_picture(st.focused_entry);
```

## Frame model

- LateUpdateBehavior: `options_frame()` runs after the flow callbacks and before the clients' frame callbacks
  (install on demand, then the driver's tick: events, open / build / rebuild, button queue, value pass; then the
  sections, increment 13, and their rows, increment 14: the right list read, the focused row, its section's values).
- BeginRendering: `options_pump()` (the hooks' events since the tick).
- The engine's display calls are made for the core's own client `sf6core.options`.

## GUID table (shared with the pause-menu service, increment 8)

- One table maps message GUIDs to texts and one hook on `hMsg.GetMessage(System.Guid)` answers from it, for the whole
  process. The option engine fills it at each build (a fresh GUID per text, R6) and drops the texts of older trees
  (`purge`); since core 0.8.0 the same table also holds the texts of the pause-menu service (`PAUSE_MENU.md`, increment 8).
- Every entry has an owner: none for the option engine's texts, the pause-menu service for its own texts. The engine's `purge` and
  `retext` only touch the engine's own entries; the service releases its texts itself.
- `guid_text::install()` is idempotent: the option service (first frame after `opt_set_callbacks`) and the pause-menu service
  both call it, the first one installs the hook, the other finds it in place. A failure is
  final for both.
- `opt_status` reports the table as `guid_text on=.. faults=.. issued=.. live=.. clients=..` (`clients` = the pause-menu
  service's texts among the live ones).

## The training pause menu

- The option engine refuses `TrainingManager.OpenMenu` while its window is up and 20 ticks after (the Esc that closes
  a dialog also reaches the pause menu); this hook stays the only `OpenMenu` hook of the core (the pause-menu service
  of increment 8 does not hook it).
- At each build it purges the DYNAMIC rows of the pause menu's primary tabs that the core built and no longer owns
  (since 0.8.0, `pausemenu::dyn_row_removable`): the game's own rows and unknown rows stay. See `PAUSE_MENU.md`.

## Not yet

- A Lua client API: `NativeOptions.lua` (training suite) still has its own engine / driver; it must not be loaded
  together with a native option client (two drivers over the same hooks).
- A client's groups are not removed when it unregisters (placed rows included: they stay in their section).
- Placed rows: what the game's Options screen does with a foreign value unit (TypeId 100000+) and whether
  `SaveEvent` is its only save path are to be confirmed in game (ADR-0021 test plan).
