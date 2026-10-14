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
- A client's groups are not removed when it unregisters.
