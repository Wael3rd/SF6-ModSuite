# SF6 Native Core

`reframework/plugins/sf6core_native.dll`: the shared native-UI core of the Street Fighter 6 mods in this repository.
Mods are separate REFramework plugins that register with it as clients and display through it; none of them writes the
game's UI directly.

## What it provides

- **Display primitives** for every client: texts, message ids, textures, colours, positions, visibility, row cloning,
  element hiding. Each call is guarded (compare before write, liveness checks, SEH) and counted per client.
- **Screen discovery**: the game's UI agents and views by name, refreshed when the game's registry changes.
- **Option dialogs**: the game's own option window as a service (groups, sliders, choices, buttons), see `docs/OPTIONS.md`.
- **Effective selection**: a client can declare what a fighter really wears (the Slots Loader's online alias), another
  reads it (the Color Mod's Drive squares).
- **Client texts**: message GUIDs that the game resolves to a client's own texts in data-driven UI (training
  pause-menu rows, input guides), answered by the core's single `GetMessage` hook, see `docs/ABI.md` (increment 8).
- **Training pause menu**: clients' pages (a sub-page of a primary tab's strip) with buttons and spins, answered by one
  engine for every client, with key-guide hints, see `docs/PAUSE_MENU.md`.
- **Fault isolation**: a client that faults is skipped after five consecutive faults (circuit breaker), the others keep running.
- A small **Lua bridge** (`SF6Core` in REFramework's Lua state) for scripts and diagnostics.
- A log: `reframework/data/sf6core/sf6core.log` (one previous log kept as `sf6core.old.log` past 8 MB).

## For mod authors

The C ABI is `include/sf6core/sf6core_api.h` (pure C, append-only): get it with `sf6core_get_api(SF6CORE_ABI_VERSION)`,
register with `register_client`, test optional functions with `SF6CORE_HAS(api, fn)`. Registration works before the
core's own initialization (it is queued). Reference: `docs/ABI.md`.

## Build

`core\build.bat` (Visual Studio 2022 or Build Tools, C++ workload) builds `build\core\sf6core_native.dll` and two test
clients. Third-party code: Lua 5.4.3 (`third_party/lua`, MIT), REFramework plugin headers (`third_party/reframework`, MIT).
Nothing is copied into the game folder.
