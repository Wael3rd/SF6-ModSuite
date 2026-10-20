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

## Frame model

- LateUpdateBehavior: `options_frame()` runs after the flow callbacks and before the clients' frame callbacks
  (install on demand, then the driver's tick: events, open / build / rebuild, button queue, value pass).
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
