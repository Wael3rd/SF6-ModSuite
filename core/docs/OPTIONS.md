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
- Not done here: a section with rows of its own on the right (the game's sub list); the mouse on the right side.

## Frame model

- LateUpdateBehavior: `options_frame()` runs after the flow callbacks and before the clients' frame callbacks
  (install on demand, then the driver's tick: events, open / build / rebuild, button queue, value pass; then the
  sections, increment 13).
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
