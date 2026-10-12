# Training pause menu (increment 8, core 0.8.0)

Design decision: ADR-0016 (accepted 2026-10-09): the game's training pause menu is a service of the core, for every
mod. Its texts are GUIDs answered by the core's one `GetMessage` hook (shared with the option dialogs).

The game builds its training pause menu (`app.training.UIFlowTrainingMenu`) from data: `TrainingManager._UIData.MenuData`
holds the primary tabs, their rows are `app.training.TrainingMenuData`, and every row answers the game through
`app.training.TrainingMenuFunc` (`GetIsActive`, `IsValueType`, `GetVisibleCase`, `IsChangedValue`, `ViewUpdate`,
`GetOptionIndex`, `GetOptionText`, `GetOptionText2`, `Function`). The core builds its clients' pages and rows as such
data, puts each page into the sub-page strip of a primary tab, answers the game for its own rows only, renders the
spins and adds the clients' key-guide hints. The engine is a port of a Lua engine that ran such a page in game.

## Files (`core/src/pausemenu/`)

| File | Role |
|---|---|
| `pm_core.{hpp,cpp}` | The state machine, no game header: registry (pages, rows, texts, data), identity (GUID key), routing of the game's questions, page placement in a strip, spin rules, timings, the install pass. Compiled as it is into an offline bench with a scripted backend |
| `pausemenu.{hpp,cpp}` | The game side: the backend (menu data, option arrays, strips, guide data), the code check of each hooked method (R18), the fifteen hooks, the spin repairs, the per-frame state |
| `pm_service.cpp` | The clients and their callbacks (SEH, breaker), the `api_pm_*` functions, the frame entry, the clean-up when a client unregisters |

## Client model

```c
SF6CorePmCallbacks cb = { sizeof cb, user, on_decide, spin_value, on_spin, on_page_shown, hints };
api->pm_set_callbacks(client, &cb);                     /* the engine is installed at the next frame */
SF6CorePmPageDesc pd = { sizeof pd, 4 /* Recording Settings */, 0 /* first */, "My page", "Guide line" };
int64_t page = api->pm_page(client, &pd);
SF6CorePmRowDesc rd = { sizeof rd, SF6CORE_PM_SPIN, "Choice", "Left / right to choose.", 64, 0, 70.0f };
int64_t spin = api->pm_row(client, page, &rd);
const char* labels[] = { "-- none --", "First", "Second" };
api->pm_set_options(client, spin, labels, 3);
```

- **Pages.** `pm_page(primary_func, strip_index, title, guide)`: a sub-page of the primary tab whose `_FuncType` is
  `primary_func` (`TrainingFuncType`). That tab's first row must be a sub-page strip: `_Type` 18 (the 2-page strip) or
  19 (the 3-page strip). The strip gets the page at `strip_index` among its pages (past the end: last; pages asking for
  the same index keep their creation order) and is retyped to the strip of its new count. Only counts of 2 and 3 pages
  have a strip in the game: a page that would make 4 is not installed (logged). Example: Recording Settings (tab 4) has
  Record and Replay; a page at index 0 gives `page > Record > Replay` and the strip's type 18 becomes 19. The title is
  the strip label, the guide the page's guide line. A page with no row is not installed.
- **Rows.** `pm_row(page, desc)`, at the end of the page, 13 rows at most (R9):
  - button: `keep_open` 1 = the menu stays open after the decide, 0 = the game closes it;
  - spin: `capacity` option slots (1..64; 64 is the largest spin run in this menu, R9 says 16 elsewhere), built once;
    `arrow_spread` > 0 moves the two arrows that much further apart (0..300) and shrinks a long option text to the room
    between them.
- **Texts in place.** `pm_set_texts(id, title, guide)` (NULL = unchanged) and `pm_set_options(row, labels, n)` change
  the texts at once (the game resolves them at each draw: a language change is a `pm_set_texts`). The count of options
  shown follows `n`, at most the capacity (the labels past it are dropped); `n == 0` shows one empty option. A change of
  labels while the page is shown refreshes the visible option at once, sizes the option array to the labels,
  brings the spin part's range and number and its carousel's count into it at once, and rebuilds the page 2 frames
  later (R4).
- **Removal.** `pm_remove(page_or_row)`: taken out of the menu at the next install pass (menu closed), texts released
  after. A client that unregisters loses its pages and rows the same way.
- **Queries.** `pm_page_shown(page)` (live: the menu's first row is one of the page's), `pm_row_part(row)` (the row's
  `app.UIPartsGroup` while its page is shown, for decoration with the display API), `pm_status` (one line).
- **Structural changes are deferred, texts are not.** `pm_page`, `pm_row` and `pm_remove` record what the next install
  pass does; they may be called from any thread, before the core's init too. Ids are the core's (>= 0), unique across
  clients and kinds; -1 on failure (the reason in the log).

## Callbacks

| Callback | When | Thread |
|---|---|---|
| `on_decide(row)` | a button of the client was pressed | the menu's job thread, inside the game's `Function` |
| `spin_value(row)` | the spin's current option, 0-based (clamped by the core) | the menu's job threads (layout, `Function`, spin init) |
| `on_spin(row, index)` | the player moved a spin | the menu's job thread, inside `Function` |
| `on_page_shown(page, shown)` | 1: the page was built (its strip selected it); 0: another page of its strip was built, the menu closed or the page was removed | the menu's job thread (1), the game thread (0 on close) |
| `hints(focused, out, max)` | each frame of the menu, for the focused row (any row, the game's included) | the menu's job thread, in `GetGuideData` |

- **Synchronous** (ADR-0016 point 4): the menu closes or stays open at once after a decide, and a decide may run game
  calls that must happen on the menu's thread. The game runs its UI jobs on several threads at once: **the client must
  be thread-safe** (R29).
- **Never call the menu's own update methods back** (`ViewUpdate`, `UpdateSpinBox`, `UpdateScroll`...: R4). Every
  `pm_*` function may be called from a callback; nothing is locked while it runs.
- **Faults.** Each call is SEH-guarded and a fault counts on the client's breaker (ADR-0008). A client disabled by its
  breaker or unregistered is skipped: its spins show the value the core kept, its buttons do nothing.
- **No `spin_value`?** The core keeps the value itself (the last `on_spin`).
- **Hints.** Up to 8 per client and focused row; `button` is the game's digital config id (decide 90, cancel 91, X 92,
  Y 93, LB 94, RB 95, LT 96, RT 97: `app.InputAssign.Digital.ConfigId` `UI_*`), the text is copied before the callback
  returns. The core appends them to the game's own guide data in `GetGuideData`'s post-hook (R26); the game lays the bar
  out, merges entries that share a button and shows the glyph of the device in use. One cached guide object per
  (client, button, text), 256 at most.

## Frame model

- `pm_set_callbacks` (the first one) starts the code check of the hooked methods on a thread of its own (R18, below).
- `LateUpdateBehavior`, after the option dialogs and before the clients' frame callbacks: install on demand (from the
  first frame after a `pm_set_callbacks`: the engine's client and types, then, once the code check has ended, one hook
  per frame; the frame never waits for the check), then the state: training (flow 3 or 9 and `TrainingManager._tData`, checked every
  60 frames), menu up (`PauseManager._CurrentPauseTypeBit` other than 64 / 2112, or an option window), the menu-close
  edge (pages "not shown", the menu's param let go), the **install pass** (every 60 frames at phase 33, once training
  has run 300 frames, menu closed: rule 10) and the scan for the game's own DYNAMIC rows (every 60 frames, menu closed).
- The install pass builds what is missing (row data, option arrays, page data typed like the strip's pages, texts),
  puts the pages into their strips, rebuilds a page's rows array after a row was added or removed, re-installs after
  the game rebuilt its menu data, takes removed pages out and releases what nothing shows any more.
- Hooks: on the game's UI job threads, concurrently (R29); per-thread state in thread-locals, the registry under the
  state machine's lock, the menu's param under its own lock with a reference held while it is used.
- **One engine at a time.** Every hook body, the frame and every page build run under one engine lock (recursive), as
  the Lua engine this one was ported from ran under the one lock of its Lua state: a page build holds it until the game
  has rebound the rows, so no other thread runs hooked menu work meanwhile (`UpdateScroll` of every scroll group,
  `Init` / `set_Num` of the parts, `GetGuideData`, the install pass...); a hook with nothing to do waits for it as a
  barrier. The menu's own navigation waits the same way, as it did in the Lua engine, which hooked it too: barrier
  hooks on the tab and sub-tab inputs (`UIAgent.InputTab*` / `InputChildTab*`), `Param.OnChangedTab` /
  `ChangeTabIndex` / `UpdatePrimaryList` / `UpdateSecondaryList` / `CreateViewData` /
  `set_SelectChildIndex` / `UpdateRecordSlotItem` / `UpdatePlaySlotItem`, `UIPartsPageIndex.set_PageIndex` / `Prev` /
  `Next`, `UIPartsListBase.GetNextIndex` / `GetPrevIndex`, `UIPartsSpin.InputRight` / `InputLeft`,
  `TrainingManager.ChangeMainMenuFlow`, `UITrainingMenu.ChangeFlow`, and the core's `GetMessage` hook while the engine
  runs. The wait is bounded (30 ms): past it the call goes on unlocked, the count is in `pm_status` and the first one
  is logged. (Fast sub-page switching crashed the game three times on a GUI job thread, 11/10, with this engine and
  never with the Lua engine nor without a page of ours: a per-frame engine event delivered to a managed delegate
  already freed; the cause is not proven.)

## How the game is answered

- **Identity (R5).** Every row, option and page the core builds is `TrainingMenuData` of FuncType 345 (`DYNAMIC`: a
  FuncType of a mod's own is not rendered) with fresh message GUIDs; the GUID of `_MessageID` names it (clones keep it).
  The game's own DYNAMIC rows carry the delegate of their `DynamicData`; anything else is unknown (another engine's).
- **The FuncType-only methods** (`GetIsActive`, `IsValueType`, `GetVisibleCase`, `IsChangedValue`, `GetOptionIndex`,
  `GetOptionText`, `GetOptionText2`) carry no row: the row being laid out on the calling thread (from `ViewUpdate`,
  `UpdateSpinBox`, `Function`) says whose it is.

  | Row on this thread | Answer |
  |---|---|
  | one of ours | ours: active, not a value type, visible case 0, not changed, the probe's own index, the spin's label |
  | another DYNAMIC row (the game's, unknown) | the game's |
  | none known, no game DYNAMIC row in the menu (offline training) | the reference engine's answer for DYNAMIC rows (as above, empty texts) |
  | none known, a game DYNAMIC row in the menu (Battle Hub) | the game's |

- **`Function`.** Our page selected in its strip: the page is marked shown (`on_page_shown(page, 1)`, where the client
  may set its labels), its rows marked "at build", the option arrays sized to the labels, then the game's own
  `ViewUpdate(page)` builds the rows on the same thread (the game's record pages chain into it the same way). The rows'
  own `Function` calls are then nested in the page's: REFramework logs `(Post) Recursive hook detected for
  'app.training.TrainingMenuFunc.Function'` once per thread (expected), and since it keeps one return slot per thread
  and hook, the page's answer is written again after the build. A row's `Function` at the build is no decide and no
  change; a button's later `Function` is the decide (returns 1 to close the menu unless `keep_open`); a spin's later
  `Function` follows a press: the spin part's `_Num` (modulo the count) is the new option (`on_spin`), then the
  ViewData handed to `Function` shows the client's value. At the build, as the Lua engine: the ViewData's `Data` is the
  option shown, and the list entry the flow draws the row from gets the value when its `Data` is the row itself; the
  core writes nothing else into the game's view data. When the ViewData handed to `Function` at the build is a local
  object (not refcounted yet, negative reference count; the trace prints it), its `Data` gets a plain store, as the
  game writes such objects (`obj_set_ref` follows the target's reference count since 0.8.0). Releasing the option it
  pointed at, as before, would free that option after a few builds while the menu's lists still point at it: the
  likely cause of the crashes of 11/10. The game's rows and unknown rows pass through.
- **Safer than the Lua engine this replaces.** Spin values are clamped to the options shown; a deferred rebuild marks
  its rows "being built" so no stray decide reaches the client; the rebuild starts only from another scroll group's
  update; the font is restored once. A FuncType-only call with no known row is answered as the table above says
  (the reference answer in offline Training, the game's in the Battle Hub).
- **Spins.** `UIPartsSpin.Init` (both overloads) gets the row's option instead of the game's own num at every build and
  focus (at the first build of a menu the part's `Init` can come before the row's `Function`: the row is then found by
  its list entry), and `set_Num` is held to it for 2 frames after `Init`. After **every** `UpdateSpinBox(Int32, Boolean,
  Boolean)` of the row (full signature, R17; the game nests this method itself, presses included) the core repairs the
  carousel's selected index and the visible option text (compared first, R7, the only `set_Message` of the engine),
  and fits the font (a game row is never touched there); only the nested `UpdateSpinBox` that the
  repair's own `set_SelectedIndex` starts is left alone (R4). The carousel is never grown (`set_ItemCount`: a scroll
  list grown after its build piles its items up). Each frame of the row list (`UpdateScroll` post on that list) the
  arrows are kept apart and a long option text is shrunk (a write at most every 15 checks); a text that fits keeps the
  game's font (the game's focus state writes it too: the core writes it only to shrink, or once to undo its own
  shrink); off the page the pooled groups get the game's arrows and font back, once. Each repair also keeps the spin
  part's own range and number (`MaxNum`, `_Num`, R17's data-only edit) inside the option array the game reads and the
  labels: a page build sets them through `Init`, a label change on the shown page did not (the deferred `ViewUpdate`
  rebuilds the rows' data, not the part: a 5 -> 7 change kept looping over 5 options, a shrinking list left `_Num` past the
  new count). Compared first: a build's own values are never rewritten. At a label change on the shown page the spin's
  carousel part (`app.UIPartsScrollList`, the spin part's child 0) is initialised as the game's layout does it,
  `InitList(count, option, -1)` (a grow left it at the old count: the presses stopped at the last option instead of
  wrapping); the next 16 changes of the spin's state are logged. The rebuild after a label change
  runs 2 frames later from **another**
  scroll group's update, never inside the list's own chain (R4), held while an option window is up, dropped if the page
  is no longer shown.

## The option-build purge

At each build the option engine purges the training pause menu's DYNAMIC rows of the primary tabs
(`DynamicChildData`). Since 0.8.0 it removes only rows the core built and no longer owns: the game's own rows (Battle
Hub "End Training Session") and unknown rows stay. It used to remove every DYNAMIC row, the game's included.

## Hooks and R18 (one hook per machine code)

Small setters can share their machine code with other methods (folded bodies, or several entries jumping to one body):
a hook on one is then a hook on all of them (the Slots Loader 1.10.5 slowdown, the 18/09 freeze). Before any hook, the
core resolves each method's entry and the code it jumps to, and compares them with every method of the game (one pass
over the type database). A method that shares its code is **not hooked**: a WARN line gives its
declaring type, address, one method that shares it and what the engine loses; the rest of the engine runs. Code shared
only with another method of the same hook (an overload of `UIPartsSpin.Init` that jumps into the other one) is hooked
once, on the body, and covers both. Each hooked method is logged with its declaring type and address.

| Hook | Without it |
|---|---|
| `TrainingMenuFunc.Function` | the engine is off (required) |
| `TrainingMenuFunc.GetIsActive`, `IsValueType`, `GetVisibleCase`, `IsChangedValue` | the game's own answer for the clients' rows |
| `TrainingMenuFunc.ViewUpdate` | no row context from the layout (`UpdateSpinBox` and `Function` still give it) |
| `TrainingMenuFunc.GetOptionIndex` | the spins' display probes answered by the game |
| `TrainingMenuFunc.GetOptionText`, `GetOptionText2` | the game's option texts for the clients' spins (the visible text is still repaired) |
| `UIFlowTrainingMenu.Param.UpdateSpinBox(Int32, Boolean, Boolean)` | no spin repair after the game's layout |
| `UIPartsSpin.Init` (two overloads) | a spin shows option 0 after a build or a focus until the next repair |
| `UIPartsSpin.set_Num(Int32)` | the flow's own `set_Num` after `Init` is not held: a press may start from option 0 after a build or a focus |
| `TrainingManager.GetGuideData` | no key-guide hints |
| `UIPartsGroupScroll.UpdateScroll` | no per-frame font fit / arrow reset, no rebuild after a change of a shown spin's labels |
| the barrier hooks (navigation, see "One engine at a time") | that step does not wait for a page build of another thread |

The `TrainingManager.OpenMenu` hook stays single (the option engine's). After 5 faults in its hooks the engine turns
them off (the game then answers its menu alone).

**Where the check runs.** The pass over every method took 6.5 s on the game thread in the first test (10/10): the game
froze. It now runs on a thread of its own, started by the first `pm_set_callbacks`: the type database and the game's
code are static and the check only reads them, through REFramework lookups that take their own locks (`find_type`,
`get_method`, `get_function`, `get_full_name`). The game thread only polls an atomic; the verdicts are computed in the
frame after the check ended, then one hook is installed per frame (each `add_hook` stops the game's threads while
REFramework patches: 1 to 6 ms; fifteen in one frame were a 23 ms frame). Each hook line gives the time its
`add_hook` took; the check's line gives its duration and thread.

**Cache.** The check's result is kept in `reframework/data/sf6core/pm_codecheck.txt`, keyed by the game's executable
(PE header, file size and time) and the number of methods; each hooked method's entry and code are compared with the
cached ones. A match: the check takes a few milliseconds and REFramework writes none of the lines it logs for every
abstract method it is asked about (345,000 lines on 10/10). A game update, or a hooked method that moved, makes the
core compare every method again (on its thread) and write the file anew. Deleting the file has the same effect.

## Diagnostics

- **Fault lines.** Each `__try` of the engine (the frame and its install pass, every hook, the code check, the clients'
  live queries) records the step it was at (a short name set before each group of game calls), the game call under
  way (method, field, object creation), the exception code, the faulting address as `module+offset`, the address
  touched for an access violation, and the thread, for the first 8 faults:
  `fault in frame (1): step 'new_array', call 'add_ref': exception 0xC0000005 at <module>+0x1234, read of 0x..., thread N`.
- **Bisection.** `reframework/data/sf6core/pm_bisect.txt` (read every 2 seconds) turns behaviours off by word, for a
  crash hunt without a rebuild: `hints`, `repair` (spin repairs), `arrows` (arrows, font, put-backs), `notify`
  (`on_page_shown`), `barriers` (the navigation barrier hooks), `textbarrier` (`GetMessage`), `lock` (the engine lock).
  Each change is logged (`bisect (pm_bisect.txt): off = ...`). No file: everything on.
- **Trace.** While `reframework/data/sf6core/pm_trace.txt` exists (looked for every 2 seconds, and when the code check
  starts), the code check, the next install pass and the next page build are logged step by step: the targets and the
  cache key, the menu tabs and their `_FuncType`, the strip row and its pages (`_Type`, `_MessageID`), every object
  built (address, type, array lengths), every guarded write refused (with its status), the page's `ViewUpdate` and the
  spin repairs of that build. 500 lines at most; deleting and re-creating the file starts another round.
- **REFramework's plugin API.** The players' REFramework (1.5.8 nightly, 12/2024) has plugin API 1.10, while the core is
  built with the 1.15 headers: a function-table member added after 1.10 (`create_managed_array`, `on_pre_gui_draw_element`,
  `module`) is read past the end of the table REFramework hands over, and calling it calls whatever follows. The
  engine's arrays are made through the game's `System.Array.CreateInstance(System.Type, System.Int32)`, as REFramework's
  own `sdk.create_managed_array` does (the 10/10 faults were the 1.13 member).

## Limits and open points

- Strips of 2 or 3 pages only (the game's two strip layouts): `pm_page` records the page, and the install pass leaves
  the game's strip untouched (one log line) when the count with the client's pages has no layout, so the page does
  not appear. One engine per strip: a Lua engine that installs pages in the same strip must not run with it.
- The strip's own memory of the sub-page to open is the game's data, read with the game's page count: a page placed
  before the game's pages shifts it. A client that wants "reopen on the last sub-page" keeps that memory itself (for
  Recording Settings: `RecordSetting.RecordType`, set before the menu's `CreateViewData`), with `on_page_shown`. The
  core does not manage that index.
- Rows in a primary tab (`AddDynamicMenu`) are not offered (a later extension of the same registry).
- The `hints` callback receives the focused `TrainingMenuData`; `pm_row_of(client, focused)` maps it to the client's
  row id (-1 for the game's rows, unknown objects, pages, removed rows and other clients' rows), so a hint can be
  shown on the client's own row only.
- `on_page_shown(page, 0)` is sent when another page of the strip is built, when the menu closes and when the page is
  removed; a change of primary tab sends nothing (use `pm_page_shown`, which reads the menu live).

## Test client (`core/tests/test_client`)

Commands in `reframework/data/sf6core_test/cmd.txt`: `pm_page` (a page `sf6core_test` at index 0 of Recording
Settings: a spin of 5 options with arrows 70 apart, a counting button, a closing button, a Y hint on its own rows
only, found with `pm_row_of`),
`pm_labels` (7 labels, one very long: count, fit, rebuild), `pm_texts` (titles changed in place), `pm_fault` (the next
decide faults once), `pm_status`, `pm_drop` (the page removed), `pm_trace` / `pm_trace_off` (the trace file created /
removed), `pm_check_reset` (the code-check cache deleted: the next session compares every method again).
