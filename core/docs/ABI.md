# SF6 Native UI Core -- ABI Contract v1

## Version

| Field | Value |
|---|---|
| ABI version | 1 |
| Core version | 0.8.2 |
| Header | `include/sf6core/sf6core_api.h` |

## Versioning and the append-only rule

`SF6CoreAPI` starts with `abi_version` and `struct_size`. Future minor revisions **append** new function pointers after the existing ones and increment `struct_size`. Existing fields never move, change type, or change semantics.

A client compiled against ABI v1 will work with any core that supports ABI v1, even if the core has added functions beyond what the client knows about. The client MUST NOT read past `struct_size` bytes.

`sf6core_get_api(abi_version)` returns NULL if the requested version is not supported.

## Discovery

Clients discover the core at runtime (never link against it):

```c
HMODULE core = GetModuleHandleW(L"sf6core_native.dll");
if (!core) { /* core not installed */ return; }
SF6CoreGetApiFn fn = (SF6CoreGetApiFn)GetProcAddress(core, "sf6core_get_api");
if (!fn) { /* core too old */ return; }
const SF6CoreAPI* api = fn(SF6CORE_ABI_VERSION);
if (!api) { /* ABI mismatch or kill switch active */ return; }
```

Call this in the client's `reframework_plugin_initialize` -- never in `DllMain` or `reframework_plugin_required_version`.

## Pre-init behavior

REFramework loads plugins in two passes (design doc section 11.0):
1. **Pass 1**: `LoadLibraryW` on every `.dll` in `plugins/` -- DLL is loaded, exports are resolvable.
2. **Pass 2**: `reframework_plugin_initialize` is called on each plugin in `std::map<stem>` order (byte-wise lexicographic of the DLL filename stem, case-sensitive).

A client whose DLL stem sorts before `sf6core_native` will have its `reframework_plugin_initialize` called **before** the core's. At that point:
- `GetModuleHandle("sf6core_native.dll")` **succeeds** (loaded in pass 1).
- `GetProcAddress("sf6core_get_api")` returns a valid pointer.
- `sf6core_get_api(1)` returns a valid, stable `const SF6CoreAPI*` (statically allocated).
- `register_client` stores the registration as pending.
- `set_flow_callback` / `set_frame_callback` store the callback pointers.
- `get_client_state` returns `SF6CORE_CLIENT_PENDING_INIT`.
- Callbacks do NOT fire yet.

When the core's own `reframework_plugin_initialize` runs:
- All pending registrations are activated (state -> `ACTIVE`).
- The `LateUpdateBehavior` callback is registered.
- Callbacks begin firing on the next frame.

**Correctness does not depend on DLL name ordering.**

## Threading

| Context | Safe calls |
|---|---|
| Client's `reframework_plugin_initialize` | `sf6core_get_api`, `register_client`, `set_flow_callback`, `set_frame_callback`, `get_version`, `get_flow_id`, `log`, `get_client_state`, `pm_*` (the engine comes with the first frame) |
| Inside a flow/frame callback (game thread) | `get_flow_id`, `log`, `get_client_state`, `set_flow_callback`, `set_frame_callback` |
| Any thread | `get_version`, `get_flow_id`, `log`, `get_client_state`, `pm_*` (also from the pause-menu callbacks, which run on the game's UI job threads: see increment 8) |

`register_client` and `unregister_client` acquire an internal mutex. They are safe from any thread, but `unregister_client` must NOT be called from inside a callback (deadlock-free, but the handle is in use by the dispatch loop).

All callbacks run on the **game thread**, inside the core's single `LateUpdateBehavior` callback, except the training pause-menu callbacks (increment 8), which run on the game's UI job threads inside the menu's own update. No callback ever fires before the core's init is complete.

## String ownership

The core copies every `const char*` the client passes. The client need not keep the string alive after the call returns. Returned `const char*` values (e.g. from `get_version`) are static and valid for the DLL's lifetime.

## Error codes

| Code | Meaning |
|---|---|
| `SF6CORE_OK` (0) | Success |
| `SF6CORE_ERR_INVALID_ARG` (1) | NULL pointer, empty string, or struct_size mismatch |
| `SF6CORE_ERR_DUPLICATE_ID` (2) | A client with this client_id is already registered |
| `SF6CORE_ERR_DISABLED` (3) | Core disabled (kill switch), client disabled (breaker, unregistered), game closing, or the service could not start |
| `SF6CORE_ERR_ABI_MISMATCH` (4) | ABI version not supported |
| `SF6CORE_ERR_NOT_FOUND` (5) | Client handle, field, entry or text not found |
| `SF6CORE_ERR_WRONG_THREAD` (6) | Element write requested off the game thread (increment 3) |
| `SF6CORE_ERR_FAULTED` (7) | The engine call faulted (SEH or managed exception), or the element is quarantined after an earlier fault |
| `SF6CORE_ERR_TYPE` (8) | Object or field not of the expected type, or not alive |
| `SF6CORE_ERR_NOT_READY` (9) | Resource not loaded yet, or the core not initialized yet |

## Client states

| State | Meaning |
|---|---|
| `SF6CORE_CLIENT_ACTIVE` (0) | Normal operation; callbacks fire |
| `SF6CORE_CLIENT_DISABLED_BY_BREAKER` (1) | Circuit breaker open (ADR-0008): 5 consecutive faults → callbacks skipped for 300 frames, then one half-open retry (success closes the breaker and the state returns to ACTIVE; a fault re-opens it for another 300 frames) |
| `SF6CORE_CLIENT_PENDING_INIT` (2) | Registered before core init; callbacks will fire once the core initializes |

## Fault isolation

Every client callback is invoked through an SEH wrapper (`__try/__except`). If a callback triggers an access violation or other structured exception:
1. The exception is caught (the game does not crash).
2. The client's fault counter is incremented.
3. A breadcrumb is pushed and written to `breadcrumbs.txt`.
4. If the fault counter reaches **5**, the circuit breaker trips: the client's state becomes `DISABLED_BY_BREAKER` and its callbacks are skipped for 300 frames, then retried once (half-open, see Client states).

A successful callback resets the fault counter to 0.

## Kill switch

If `reframework/data/sf6core/disabled.txt` exists, `sf6core_get_api` returns NULL. All client DLLs should handle this gracefully (log and stay inert).

## Checking for new functions (increment 2+)

When the core is updated with new function pointers (appended to `SF6CoreAPI`), `struct_size` grows. Clients compiled against the old header can safely ignore new functions. Clients compiled against a newer header must check before calling:

```c
// SF6CORE_HAS(api, member) returns true if the function pointer is within struct_size.
if (SF6CORE_HAS(api, create_widget)) {
    api->create_widget(client, &desc, &widget);
}
```

## Function list (ABI v1)

| # | Function pointer | Signature |
|---|---|---|
| 1 | `get_version` | `const char* (void)` |
| 2 | `register_client` | `SF6CoreStatus (const SF6CoreClientDesc*, SF6CoreClient**)` |
| 3 | `unregister_client` | `void (SF6CoreClient*)` |
| 4 | `set_flow_callback` | `SF6CoreStatus (SF6CoreClient*, SF6CoreFlowCb, void*)` |
| 5 | `set_frame_callback` | `SF6CoreStatus (SF6CoreClient*, SF6CoreFrameCb, void*)` |
| 6 | `get_flow_id` | `int32_t (void)` |
| 7 | `log` | `void (SF6CoreClient*, SF6CoreLogLevel, const char*)` |
| 8 | `get_client_state` | `SF6CoreClientState (SF6CoreClient*)` |

## Function list (increment 2 -- appended)

| # | Function pointer | Signature | Threading |
|---|---|---|---|
| 9 | `create_widget` | `SF6CoreStatus (SF6CoreClient*, const SF6CoreWidgetDesc*, SF6CoreWidget**)` | Frame callback or init |
| 10 | `get_widget_state` | `SF6CoreWidgetState (SF6CoreWidget*)` | Any |
| 11 | `widget_set_text` | `SF6CoreStatus (SF6CoreWidget*, const char* path, const char* text)` | Frame callback |
| 12 | `widget_set_visible` | `SF6CoreStatus (SF6CoreWidget*, int32_t)` | Frame callback |
| 13 | `widget_set_position` | `SF6CoreStatus (SF6CoreWidget*, float x, float y)` | Frame callback |
| 14 | `destroy_widget` | `SF6CoreStatus (SF6CoreWidget*)` | Frame callback |
| 15 | `set_element_hidden` | `SF6CoreStatus (SF6CoreClient*, const char* owner, const char* path, int32_t)` | Frame callback or init |

## Function list (increment 3 -- Display API, appended)

All `ui_*` functions must be called on the game thread (any REFramework frame/update callback or a game method hook running on it); they return `ERR_WRONG_THREAD` otherwise. `obj_*`, `list_*`, `dict_*` are allowed on any thread a game hook runs on.

Every write goes through the guard chain: kill switch/breaker (1), proof-of-life type check (2), quarantine (3), compare-before-write (4), engine call under SEH (5), managed reference semantics R34 (6).

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 16 | `ui_set_text` | `SF6CoreStatus (client, void* text, const char* utf8)` | Game thread | Compare-before-write; proof-of-life checks via.gui.Text family |
| 17 | `ui_set_message_id` | `SF6CoreStatus (client, void* text, const uint8_t guid16[16])` | Game thread | System.Guid 16-byte value type |
| 18 | `ui_get_message_id` | `SF6CoreStatus (void* text, uint8_t out[16])` | Any | Read-only, no client needed |
| 19 | `ui_set_font_size` | `SF6CoreStatus (client, void* text, float w, float h)` | Game thread | set_FontSize takes a via.Size |
| 20 | `ui_set_region_size` | `SF6CoreStatus (client, void* text, float w, float h)` | Game thread | via.Size value type |
| 21 | `ui_set_color` | `SF6CoreStatus (client, void* ctrl, uint32_t rgba)` | Game thread | raw via.Color.rgba value (4-byte field, passed through unchanged); sets Color + 4 corner colours |
| 22 | `ui_texture_load` | `SF6CoreStatus (client, const char* path, void** out)` | Any | Caller-owned ref; wait 60 frames before use |
| 23 | `ui_texture_ready` | `int32_t (void* holder)` | Any | 1 once loaded (>= 60 frames) |
| 24 | `ui_set_texture` | `SF6CoreStatus (client, void* ctrl, void* holder)` | Game thread | Compares underlying resource ptr |
| 25 | `ui_set_visible` | `SF6CoreStatus (client, void* ctrl, int32_t)` | Game thread | |
| 26 | `ui_set_force_invisible` | `SF6CoreStatus (client, void* ctrl, int32_t)` | Game thread | |
| 27 | `ui_set_position` | `SF6CoreStatus (client, void* ctrl, float x, y, z)` | Game thread | via.vec3 value type |
| 28 | `ui_set_scale` | `SF6CoreStatus (client, void* ctrl, float x, y, z)` | Game thread | |
| 29 | `ui_set_size` | `SF6CoreStatus (client, void* ctrl, float w, h)` | Game thread | via.Size value type |
| 30 | `ui_set_block_input` | `SF6CoreStatus (client, void* agent, int32_t)` | Game thread | app.UIAgent type check |
| 31 | `ui_clone_row` | `SF6CoreStatus (client, const SF6CoreRowCloneDesc*, void**)` | Game thread | Full LL recipe (clone, addChild, Create, layout, position, text) |
| 32 | `obj_invoke` | `SF6CoreStatus (client, void* obj, const char* sig, SF6CoreArg*, uint32_t, SF6CoreArg*)` | Any | Generic guarded invoke with arg conversion |
| 33 | `obj_new` | `void* (client, const char* type_name)` | Any | Caller-owned ref |
| 34 | `obj_clone` | `void* (client, void* obj)` | Any | MemberwiseClone; caller-owned ref |
| 35 | `obj_set_i64` | `SF6CoreStatus (client, void* obj, const char* field, int64_t)` | Any | Any integer/enum/bool width |
| 36 | `obj_set_f64` | `SF6CoreStatus (client, void* obj, const char* field, double)` | Any | float or double field |
| 37 | `obj_set_string` | `SF6CoreStatus (client, void* obj, const char* field, const char* utf8)` | Any | Managed string assignment (R34) |
| 38 | `obj_set_ref` | `SF6CoreStatus (client, void* obj, const char* field, void*)` | Any | Managed ref assignment (R34), as the game's own stores: add_ref new / exchange / release old on a refcounted target, a plain store on a local one (negative reference count; since 0.8.0) |
| 39 | `obj_set_color` | `SF6CoreStatus (client, void* obj, const char* field, uint32_t rgba)` | Any | via.Color value field (one 4-byte rgba, raw value) |
| 40 | `list_add` | `SF6CoreStatus (client, void* list, void* item)` | Any | |
| 41 | `list_set_item` | `SF6CoreStatus (client, void* list, int32_t index, void* item)` | Any | |
| 42 | `list_get_range` | `void* (client, void* list, int32_t start, int32_t count)` | Any | Caller-owned ref |
| 43 | `dict_add` | `SF6CoreStatus (client, void* dict, int64_t key, void* value)` | Any | |
| 44 | `dict_contains` | `int32_t (void* dict, int64_t key)` | Any | Read-only, no client needed |
| 45 | `ui_find_child` | `void* (void* root, const char* name, int32_t max_depth)` | Any | Recursive depth-limited name search |
| 46 | `ui_find_agent` | `void* (const char* gameobject_name)` | Any | Now uses agent registry (no full scan) |
| 47 | `get_display_stats` | `SF6CoreStatus (client, SF6CoreDisplayStats*)` | Any | Per-client write/skip/fault counters |

## Function list (increment 4 -- Discovery, ADR-0012, appended)

Agent registry rebuilt lazily: `_version`/`_size` on `UIAgentManager._Entries` are checked via cached field handles (3 pointer loads); full rebuild only when they change. Per-name and global generation counters track changes. SRWLOCK (shared for reads, exclusive for rebuild/insert). Returned agents are proof-of-life validated (type definition under SEH, not `is_managed_object`).

`ui_find_cached` and `ui_find_many` use PATH-based caching: they record the move sequence (child/next) to the first match in find_name order and follow it from the live root on the next call. Only objects reached from the live root are touched. Bounded at 1024 entries, cleared when full.

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 48 | `ui_agents` | `uint32_t (const char* name, void** out, uint32_t max)` | Any | Returns proof-of-life validated agents in _Entries order |
| 49 | `ui_agents_gen` | `uint32_t (const char* name)` | Any | Per-name change counter; 0 for unknown names |
| 50 | `ui_registry_gen` | `uint32_t (void)` | Any | Global change counter |
| 51 | `ui_find_cached` | `void* (void* root, const char* name)` | Any | Path-based cached lookup in find_name order (depth 8) |
| 52 | `ui_find_many` | `uint32_t (void* root, const char* const* names, uint32_t n, void** out)` | Any | One walk, first match per name; paths cached; **n capped at 32** (only the first 32 names are searched); out[] must be n entries wide; all n entries zeroed before walk |
| 53 | `set_element_hidden_zen` | `SF6CoreStatus (client, const char* owner, const char* const* segments, uint32_t n_segs, int32_t hidden)` | **Game thread** | ERR_WRONG_THREAD off the game thread; Lua-pattern matching (%d, %d+), all-segment fan-out, per-frame apply |

### `set_element_hidden_zen` pattern matching

Patterns support a subset of Lua `string.match` semantics (UNANCHORED substring match):
- `%d` matches exactly one decimal digit
- `%d+` matches one or more decimal digits
- All other characters match literally
- The match is unanchored: the pattern can match anywhere in the subject string

At every path segment level, ALL matching children are followed (fan-out), not just the first match. This covers list rows (`item%d+`, `player%d`) where multiple siblings must be affected. The walk is bounded at 4096 node visits per rule.

Applied every frame for owners sighted by the draw callback in the last 2 frames. Compare-before-write (`get_ForceInvisible` then `set_ForceInvisible` only if different) makes repeated applies nearly free.

Does NOT change the semantics of the existing `set_element_hidden` (glob wildcards, last-segment-only fan-out, 20-frame revalidation interval).

## Function list (increment 5 -- Bridge extensions + rules suspend, appended)

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 54 | `client_state_by_id` | `int32_t (const char* client_id)` | Any | Returns SF6CoreClientState or -1 if unknown; reads under registry mutex |
| 55 | `lua_register_fn` | `SF6CoreStatus (client, const char* name, SF6CoreLuaFn fn, void* user)` | Any | NULL fn unregisters; name max 63 chars; ADR-0002 migration bridge |
| 56 | `set_element_rules_suspended` | `SF6CoreStatus (client, int32_t suspended)` | **Game thread** | ERR_WRONG_THREAD off the game thread; suspends/resumes all of the client's element-hide rules |

### Native functions via the Lua bridge (ADR-0002 migration)

A C++ client registers functions with `lua_register_fn(client, name, fn, user)`. Lua scripts call them as `SF6Core.clients[client_id].<name>(...)` through lazy proxy tables (metatables + closures with upvalues). The registry survives Lua state resets: proxies are re-created with the bridge registration.

**Value types**: `SF6CoreValue` carries nil, bool, int (managed addresses travel as int), double, or string. Lua integers map to INT, other numbers to DOUBLE, booleans to BOOL, strings to STRING, nil to NIL. Any other Lua type raises a Lua error before the call.

**String ownership**: arg strings are valid only during the call. Returned strings must stay valid until the function returns; the bridge copies them into Lua at once. A `thread_local` buffer is the expected pattern.

**Threading**: the function runs on the calling Lua thread. Document it.

**Error handling**: `lua_error` is called ONLY from the bridge's own C function frame, after the native call has returned. It never `longjmp`s through C++ frames. The native call runs under SEH; a fault counts through `handle_fault`/breaker for that client and raises a Lua error. A negative return value raises a Lua error with a generic message.

**Limits**: max 16 args, 16 rets; name max 63 chars; max 256 registered functions across all clients.

### Rules suspend (ZenMode replay exception)

`set_element_rules_suspended(client, 1)` causes the commit phase to skip all of the client's hide rules (both `set_element_hidden` and `set_element_hidden_zen`). No writes, no restores. Resuming with `suspended=0` triggers re-apply on the next frame for sighted owners. This mirrors ZenMode.lua doing nothing during a replay (flow 10).

## Function list (increment 6 -- Option dialogs, ADR-0013, appended)

The game's own option window as a service: see `OPTIONS.md`. The engine's hooks (and the shared `GetMessage` hook)
are installed on the first frame after any client calls `opt_set_callbacks`. The `opt_*` calls are made on the game
thread: from a frame callback or from the option callbacks (re-entrant, nothing is locked while they run).

| # | Function pointer | Signature | Notes |
|---|---|---|---|
| 57 | `opt_set_callbacks` | `SF6CoreStatus (client, const SF6CoreOptCallbacks*)` | Copied; ERR_DISABLED when the engine could not be installed |
| 58 | `opt_group` | `int64_t (client, const char* key, const char* title, const char* desc)` | One page; group id, -1 on failure |
| 59 | `opt_group_def` | `SF6CoreStatus (client, int64_t group, const char* title, const char* desc)` | Applied at the next build |
| 60 | `opt_remove_group` | `SF6CoreStatus (client, int64_t group)` | |
| 61 | `opt_entry` | `int64_t (client, int64_t group, const SF6CoreOptEntryDesc*)` | Button, slider or choice; entry id, -1 on failure |
| 62 | `opt_rebuild` | `SF6CoreStatus (client)` | |
| 63 | `opt_ensure_built` | `SF6CoreStatus (client)` | |
| 64 | `opt_open_menu` | `SF6CoreStatus (client, const char* title, int32_t focus)` | Ignored while the window is open |
| 65 | `opt_navigate` | `SF6CoreStatus (client, const char* key, int32_t flags, int32_t focus)` | `SF6CORE_OPT_NAV_*` flags |
| 66 | `opt_close` | `SF6CoreStatus (client)` | |
| 67 | `opt_window_open` | `int32_t (void)` | Query |
| 68 | `opt_current_param` | `void* (void)` | Query |
| 69 | `opt_parent_param` | `void* (void)` | Query |
| 70 | `opt_focused_entry` | `int64_t (void)` | Query; -1 when none |
| 71 | `opt_row_control` | `void* (int64_t entry, void* param)` | Query |
| 72 | `opt_row_part` | `void* (int64_t entry)` | Query |
| 73 | `opt_set_option_text` | `SF6CoreStatus (client, int64_t entry, int32_t index, const char* text)` | Retext of a choice label in place |
| 74 | `opt_status` | `uint32_t (char* buf, uint32_t cap)` | One-line state; length written (truncated to cap-1) |

## Function list (increment 7 -- Effective selection, ADR-0014, appended)

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 75 | `sel_set_effective` | `SF6CoreStatus (client, int32_t fighter, int32_t shown_costume, int32_t real_costume, int32_t real_colour)` | Any | `real_costume < 0` clears; the last call wins per (fighter, shown costume) |
| 76 | `sel_get_effective` | `int32_t (int32_t fighter, int32_t costume, int32_t* real_costume, int32_t* real_colour)` | Any | 1 when the fighter seen in `costume` (shown or real) is declared to wear something else |

## Function list (increment 8 -- Training pause menu, ADR-0016, appended)

The game's training pause menu as a service: see `PAUSE_MENU.md`. The engine's hooks are installed on the first frame
after any client calls `pm_set_callbacks` (no cost without clients). Pages and rows are built and put into the menu by
the core on the game thread with the menu closed; `pm_page`, `pm_row` and `pm_remove` record what that pass does.
Clients probe the increment with `SF6CORE_HAS(api, pm_status)`.

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 77 | `pm_set_callbacks` | `SF6CoreStatus (client, const SF6CorePmCallbacks*)` | Any | Copied; ERR_DISABLED when the engine could not be installed |
| 78 | `pm_page` | `int64_t (client, const SF6CorePmPageDesc*)` | Any | A sub-page of a primary tab's strip; page id, -1 on failure |
| 79 | `pm_row` | `int64_t (client, int64_t page, const SF6CorePmRowDesc*)` | Any | Button or spin (capacity 1..64), 13 rows per page; row id, -1 on failure |
| 80 | `pm_set_texts` | `SF6CoreStatus (client, int64_t page_or_row, const char* title, const char* guide)` | Any | In place; NULL = unchanged |
| 81 | `pm_set_options` | `SF6CoreStatus (client, int64_t row, const char* const* labels, uint32_t n)` | Any | A spin's labels in place; the count shown follows `n` (at most the capacity) |
| 82 | `pm_remove` | `SF6CoreStatus (client, int64_t page_or_row)` | Any | Out of the menu at the next pass; texts released after |
| 83 | `pm_page_shown` | `int32_t (int64_t page)` | Any | 1 while the menu shows the page (live check) |
| 84 | `pm_row_part` | `void* (int64_t row)` | Any | The row's `app.UIPartsGroup` while its page is shown, NULL otherwise |
| 85 | `pm_status` | `uint32_t (char* buf, uint32_t cap)` | Any | One-line state; length written (truncated to cap-1) |
| 86 | `pm_row_of` | `int64_t (client, void* menu_data)` | Any | The caller's row id carried by a `TrainingMenuData` the core built (the focused data of a `hints` callback); a spin's option gives its row. -1: game row, unknown object, a page, a removed row, another client's row. Looked up by the object's `_MessageID` GUID under the registry lock; the one game read is SEH-guarded |

### Training pause-menu callbacks

`SF6CorePmCallbacks` (`on_decide`, `spin_value`, `on_spin`, `on_page_shown`, `hints`) run **synchronously on the game's
UI job threads**, inside the menu's own update, several threads at once: unlike every other callback of this ABI they
do not run in the core's `LateUpdateBehavior` (only `on_page_shown(page, 0)` at a menu close does). The client must be
thread-safe and must never call the menu's update methods back (R4); any `pm_*` function may be called from them.
They are SEH-guarded and a fault counts on the client's breaker; a disabled or unregistered client is skipped.

## Function list (increment 9 -- GUI material texture, ADR-0019, appended, core 0.8.1)

A `via.gui.Material` shows its textures through named material variables (the stage strip's cells: element
`e_mat_thumb`, variable `Stage_Image`). The core calls the material's own `getMaterialValueT(System.String)` /
`setMaterialValueT(System.String, holder)`, found by name on the object's type; their signatures are checked once per
method (the name a `System.String`, the texture a `via.render.TextureResourceHolder` type) and logged once
(`material texture: ... used`, or `... not the expected signature, never called` -> `ERR_NOT_FOUND`). Both run on the
**game thread only** (`ERR_WRONG_THREAD` elsewhere: refused, unlike the advisory of the other display ops). Clients
probe the increment with `SF6CORE_HAS(api, ui_get_material_texture)`.

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 87 | `ui_set_material_texture` | `SF6CoreStatus (client, void* material, const char* param, void* holder)` | **Game thread** | Display guards (kill switch, closing, breaker, quarantine, SEH), live `via.gui.Material` check. `holder`: from `ui_texture_load` and loaded (`ERR_NOT_READY` during its 60 frames), or from `ui_get_material_texture`; any other holder `ERR_INVALID_ARG`. The variable must hold a texture now (proof that it exists, `ERR_NOT_FOUND` otherwise). Same holder or same resource: nothing written, `OK` |
| 88 | `ui_get_material_texture` | `SF6CoreStatus (client, void* material, const char* param, void** out_holder)` | **Game thread** | The holder the variable shows now, one reference owned by the caller (release it); accepted back by `ui_set_material_texture` (to put the game's texture back). `ERR_NOT_FOUND`, `*out_holder` NULL: no texture |

## Function list (increment 10 -- texture rectangle, texture state, plain clone, ADR-0020 amendment 1, appended, core 0.8.2)

Probe the increment with `SF6CORE_HAS(api, ui_clone_control)`. The two texture ops run on the **game thread only**
(`ERR_WRONG_THREAD` elsewhere); they call the control's own methods, found by name on its type (a missing one is logged
once, `ERR_NOT_FOUND`). `System.Single` values travel through the invoke wrappers as doubles.

| # | Function pointer | Signature | Threading | Notes |
|---|---|---|---|---|
| 89 | `ui_set_texture_rect` | `SF6CoreStatus (client, void* texture_ctrl, void* holder, float u, float v, float w, float h)` | **Game thread** | Texture mode (`set_AssetType(1)`), `setTexture(holder)`, `set_UVType(1)` (ratio), the rectangle. `holder` from `ui_texture_load` and loaded (`ERR_NOT_READY` during its 60 frames) or from `ui_get_texture_state`, else `ERR_INVALID_ARG`. Only what differs is written; all of it equal: `OK`, nothing written. Display guards, live `via.gui.Texture` check |
| 90 | `ui_get_texture_state` | `SF6CoreStatus (client, void* texture_ctrl, SF6CoreTextureState* out)` | **Game thread** | `out->struct_size` set by the caller. Asset type, texture holder (one reference owned by the caller: release it; accepted back by `ui_set_texture_rect`), UV-sequence holder (identity only, no reference), sequence and pattern numbers, UV type and rectangle, size, colour |
| 91 | `ui_clone_control` | `SF6CoreStatus (client, void* template_ctrl, void* parent_ctrl, const char* name, void** out_ctrl)` | Advisory, as `ui_clone_row` | `duplicate(name)` of a plain element (no UIParts) added as the last child of `parent_ctrl` (drawn over its siblings). Checked before `addChild` (template's type, name asked: `ERR_NOT_READY`, nothing attached) and after (`get_Parent`: `ERR_FAULTED`). The copy is not a managed object: no reference to release, it lives with the parent's tree (find it again with `ui_find_child`). Call it from the screen's own update hook, as `ui_clone_row` (e.g. a post-hook of `app.UIFlowOptionBGDialog.Main.OnUpdate` for the option window): from REFramework's UpdateBehavior / LateUpdateBehavior the copy comes back unfinished (`ERR_NOT_READY`, "stopped at step 2" in the log) |

## Lua bridge API

The Lua bridge registers `_G.SF6Core` in REFramework's Lua state with:

- `SF6Core.version()` -> string (core version)
- `SF6Core.abi()` -> integer (ABI version number)
- `SF6Core.client(id)` -> table of closures bound to a core client
- `SF6Core.client_active(id)` -> boolean (true when client is registered, active, state ACTIVE)
- `SF6Core.clients[id].<name>(...)` -> native function call proxy (see increment 5)

`SF6Core.client(id)` is idempotent: calling it again with the same id after a Lua state reset returns a table bound to the SAME existing client (no ERR_DUPLICATE_ID). The returned table has:

| Function | Lua signature | Returns |
|---|---|---|
| `ui_set_text` | `(addr, str)` | status int |
| `ui_set_message_id` | `(addr, 16_byte_string)` | status int |
| `ui_get_message_id` | `(addr)` | 16-byte string or nil |
| `ui_set_font_size` | `(addr, w, h)` | status int |
| `ui_set_region_size` | `(addr, w, h)` | status int |
| `ui_set_color` | `(addr, rgba_int)` | status int |
| `ui_texture_load` | `(path)` | addr or nil |
| `ui_texture_ready` | `(addr)` | boolean |
| `ui_set_texture` | `(ctrl_addr, holder_addr)` | status int |
| `ui_set_visible` | `(addr, bool)` | status int |
| `ui_set_force_invisible` | `(addr, bool)` | status int |
| `ui_set_position` | `(addr, x, y, z)` | status int |
| `ui_set_scale` | `(addr, x, y, z)` | status int |
| `ui_set_size` | `(addr, w, h)` | status int |
| `ui_set_block_input` | `(addr, bool)` | status int |
| `obj_invoke` | `(addr, sig, ...)` | status, result |
| `obj_new` | `(type_name)` | addr or nil |
| `obj_clone` | `(addr)` | addr or nil |
| `obj_set_i64` | `(addr, field, int)` | status int |
| `obj_set_f64` | `(addr, field, num)` | status int |
| `obj_set_string` | `(addr, field, str)` | status int |
| `obj_set_ref` | `(addr, field, addr)` | status int |
| `obj_set_color` | `(addr, field, rgba)` | status int |
| `list_add` | `(list_addr, item_addr)` | status int |
| `list_set_item` | `(list_addr, index, item_addr)` | status int |
| `list_get_range` | `(list_addr, start, count)` | addr or nil |
| `dict_add` | `(dict_addr, key_int, val_addr)` | status int |
| `dict_contains` | `(dict_addr, key_int)` | boolean |
| `ui_find_child` | `(root_addr, name, depth?)` | addr or nil |
| `ui_find_agent` | `(go_name)` | addr or nil |
| `stats` | `()` | table {writes, skipped_identical, refused, faults, quarantined} |
| `state` | `()` | integer (client state) |

Objects are passed as Lua integers (addresses). `obj_invoke` auto-converts Lua args: integer->I64, float->F64, boolean->BOOL, string->STR; `{ptr=addr}`->PTR.

## Widget lifecycle

1. **create_widget**: client provides a `SF6CoreWidgetDesc` (prefab path, flow context, position, visibility). Returns a handle immediately with state = `PENDING`.
2. **Instantiation** (commit phase): core creates the resource, holder, prefab, and calls `instantiate(via.vec3)`. If the resource is not loaded, retries each frame (up to 300 frames = ~5s). On success: `getComponent(app.UIAgent)`, `add_ref`, state -> `LIVE`.
3. **Operations** (commit phase): `widget_set_text` uses compare-before-write (R7). `widget_set_visible` uses the agent state machine (`_StateManager._Next = Show/Hide`). `widget_set_position` uses `get_Position`/`set_Position` on the root `via.gui` control (EXPERIMENTAL).
4. **Flow visibility**: effective = `client_visible AND (flow_ids empty OR current_flow in flow_ids)`. Re-evaluated on every flow change.
5. **Destruction**: `destroy_widget` queues `release` + `via.GameObject.destroy`. Never calls `remove()`. Auto-unregisters from UIAgentManager.
6. **Breaker**: breaker-disabled clients' widgets are hidden (not destroyed). On breaker close, visibility is re-evaluated.
7. **Unregister**: all client-owned widgets are destroyed.

## Element hiding

`set_element_hidden(client, gui_owner_name, element_path, hidden)` declares a desired visibility state for game-owned GUI elements. The `gui_owner_name` is the `GameObject` name of the GUI agent (e.g. `"BattleHud_AccountInfo"`). The `element_path` is a `/`-separated name path (e.g. `"c_main/c_hud/c_RankInfo"`).

**Glob wildcards**: path segments support `*` matching zero or more characters. Examples: `"p_BattleHudAccountInfo_*"` matches `p_BattleHudAccountInfo_0`, `p_BattleHudAccountInfo_1`; `"item*"` matches `item0`, `item19`; `"*"` alone matches any child name.

**Two-phase enforcement** (R1 compliant):

1. **Draw callback** (`on_pre_gui_draw_element`, rendering thread -- `REFramework/src/mods/Hooks.cpp:827 gui_draw_hook_internal`): **read-only observer**. Resolves the element's owner name (O(1) direct-mapped cache, validated by owner pointer at +0x10). If the owner matches a declared rule, records `{owner, view_ptr, frame_seen}` into a fixed-size sighting table. No GUI writes, no tree walks. Always returns `true`.

2. **Commit phase** (`LateUpdateBehavior`, game thread): reads the sighting table. (Re)resolves and applies `set_ForceInvisible` only when a trigger fires: (a) a rule changed, (b) a flow change invalidated caches, (c) a revalidation tick (every 20 frames). Uses compare-before-write (`get_ForceInvisible` then `set_ForceInvisible` only if different).

**Original-value tracking**: on first touch, saves `get_ForceInvisible` per control address. Un-hide or client unregister restores each control to its saved original value, then drops the tracking entry.

**Interior-segment fan-out limitation**: wildcard matching fans out only on the last path segment. Interior segments use first-match-only. This covers all current ZenMode paths (documented in `docs/ports/ZENMODE_SPEC.md`).

**Thread safety**: the rule list and the sighting table are shared between the game thread (API calls, commit phase) and the rendering thread (draw callback), and every access goes through one internal mutex. The rendering thread only `try_lock`s it — if the lock is busy it simply skips recording for that draw (it never stalls a frame) — and never calls engine code while holding it. The commit phase copies the rules and sightings into private snapshots under the lock, releases it, and only then does engine work. The draw callback's owner-name cache is private to the rendering thread; the game thread requests a flush (on flow change) through an atomic flag. The early-out rule count and the frame counter used by the rendering thread are atomics.
