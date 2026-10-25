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
- **Training pause menu**: clients' pages (a sub-page of a primary tab's strip) with buttons and spins, answered by one
  engine for every client, with key-guide hints, see `docs/PAUSE_MENU.md`.
- **Fault isolation**: a client that faults is skipped after five consecutive faults (circuit breaker), the others keep running.
- A small **Lua bridge** (`SF6Core` in REFramework's Lua state) for scripts and diagnostics.
- A log: `reframework/data/sf6core/sf6core.log` (one previous log kept as `sf6core.old.log` past 8 MB).

## Timer resolution

The game's 60 fps limiter needs a 1 ms Windows timer, which the game asks for itself. Windows 11 can ignore a
process's timer requests, and in some sessions it ignored the game's (and any other request made at start) with the
game in the foreground: the limiter then slept on the 15.6 ms default tick and the whole session ran at 45-48 fps. A
request Windows ignored stays ignored; a new one made later is honoured. During the game's first minute the timer
requests of the game, Media Foundation (the movies) and the audio made it ignored again several times.

At start the Core asks for 1 ms (`timeBeginPeriod(1)`, and Windows' opt-out `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`
in `ControlMask`, `StateMask` 0, which alone did not stop those sessions). Then a background thread measures every
second how long `Sleep(1)` takes in the game (the shortest of three in a row after a first one, which ends on a timer
tick: about 1-2 ms when the 1 ms timer is honoured, 15.6 ms when not); only when it is not honoured, it makes the
kernel request again (`NtSetTimerResolution` released, then set). The global timer
(`NtQueryTimerResolution`) does not tell: another program holding 1 ms sets it without the game getting it.
`sf6core.log` gives the first measurement (`timer: Sleep(1) takes 1.9 ms at start (...)`) and each new request (`timer:
Windows was ignoring the game's 1 ms timer (...): asked again, now 1.1 ms`, the first 20). An empty
`reframework/data/sf6core/timer_off.txt` turns all of this off.

## For mod authors

The C ABI is `include/sf6core/sf6core_api.h` (pure C, append-only): get it with `sf6core_get_api(SF6CORE_ABI_VERSION)`,
register with `register_client`, test optional functions with `SF6CORE_HAS(api, fn)`. Registration works before the
core's own initialization (it is queued). Reference: `docs/ABI.md`.

## Build

`core\build.bat` (Visual Studio 2022 or Build Tools, C++ workload) builds `build\core\sf6core_native.dll` and two test
clients. Third-party code: Lua 5.4.3 (`third_party/lua`, MIT), REFramework plugin headers (`third_party/reframework`, MIT).
Nothing is copied into the game folder.
