# Working online, on any REFramework

How the suite's DLLs (Native Core, Color Mod, Slots Loader) keep working in online matches, and why they load on
every REFramework build, from the public 1.5.8 release to the latest nightly.

**Short version:**

1. Everything that must work online is written as a **native plugin** (C++ DLL), never as a Lua script.
2. Every DLL asks REFramework for **plugin API 1.10** only (the API of REFramework 1.5.8). Anything newer is used
   only after checking the version at runtime.

---

## 1. Why Lua is not enough online

REFramework pauses **all Lua scripts** during an online match. It is a deliberate fair-play rule in REFramework
itself, not something a script can opt out of.

- **What counts as an online match:** Ranked, Casual, Battle Hub cabinets, Custom Rooms and online training (the
  game's network game mode).
- **What stops:**
  - Lua hooks still let the game's code run, but the script's function is skipped;
  - `re.on_frame` and the `re.on_pre_application_entry` callbacks are skipped;
  - draw callbacks are skipped;
  - config saves are skipped.
- **When it resumes:** the scripts start again after the match. In the meantime they have missed everything that
  happened.

**Native plugins are not paused.** Their callbacks (`on_pre_application_entry`, `on_present`, hooks added with
`add_hook`...) have no online check. A DLL keeps running through the whole match.

**So the rule is:** a feature that must work online lives in a DLL.

- The Slots Loader's online costume alias (showing your Battle Settings outfit in Ranked or the Battle Hub) is only
  possible because the Slots Loader is a DLL.
- The core's Lua bridge (`SF6Core`) is a convenience for Lua scripts. It goes quiet online with them, and nothing in
  the core depends on it.

---

## 2. Why the REFramework version matters

A native plugin talks to REFramework through a **table of function pointers** (the plugin API, `API.h`). Each
REFramework release may **add** functions at the end of that table, and bumps the API version when it does.

| REFramework | Plugin API |
|---|---|
| 1.5.8 (public release) | **1.10** |
| recent nightlies | 1.13 to 1.15 |

Two things can go wrong.

### a) The DLL refuses to load

At startup, REFramework calls the DLL's `reframework_plugin_required_version`. If the DLL asks for a newer minor
version than REFramework has, REFramework **does not load it**. The log then says "Requires a newer minor version".

This happened to the Color Mod: it copied `1.15` from an older plugin, and so it did not load at all on 1.5.8.

### b) The DLL loads, then calls a function that does not exist

Suppose a DLL is compiled against a recent `API.h` (1.15) and runs on 1.5.8 (1.10). Anything added after 1.10 sits
past the end of REFramework's real table. Reading it gives a **garbage pointer**, and calling it means a crash, a
freeze or random faults.

**The compiler cannot warn you about this.** The header knows the function, but the user's REFramework does not.

Functions we have run into:

| Function | Added in API | What we do instead on 1.10 |
|---|---|---|
| `get_module` | 1.11 | not used |
| `create_managed_array` | 1.13 | call the game's own `System.Array.CreateInstance(System.Type, System.Int32)` |
| `on_pre_gui_draw_element` | 1.14 | registered only on 1.14+; below that, element hiding is off and logged |

---

## 3. The recipe

### Ask for 1.10, nothing more

Every DLL of the suite exports this:

```cpp
extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1;
    v->minor = 10;    // API of REFramework 1.5.8
    v->patch = 0;
    v->game_name = nullptr;
}
```

Newer REFramework builds load it as well: a plugin may ask for an *older* minor version.

### Use only what 1.10 has

The `API.h` bundled in the repo is recent (1.15), so it shows more than 1.10 offers. Before you use a member, check
when it was added (`git log -S<name> -- include/reframework/API.h` in REFramework's repository).

### If you really need something newer, check the version first

Read the real version from `param->version` in `reframework_plugin_initialize`. Turn the feature off, with one log
line, when the version is too old:

```cpp
const bool has_draw_cb = param->version &&
    (param->version->major > 1 || (param->version->major == 1 && param->version->minor >= 14));
if (has_draw_cb && param->functions->on_pre_gui_draw_element) {
    param->functions->on_pre_gui_draw_element(on_pre_gui_draw_element);
} else {
    log_warn("on_pre_gui_draw_element needs plugin API >= 1.14: element hiding disabled");
}
```

**Testing for a null pointer alone is not enough.** On an older table the slot holds whatever comes next in memory,
which is rarely null. Check the version first, then the pointer.

### Prefer the game over REFramework

When the game itself can do the job (create an array, find a type, call a method through reflection), go through the
game's own methods. They exist whatever the REFramework version.

---

## 4. Being a good citizen online

Online, the game runs rollback netcode, so a slow frame costs more than offline. Our DLLs follow these habits:

- **Work on events, not every frame.** Read the game when something happens: a screen opens, a flow changes, a hook
  fires. Do not poll every frame. The goal is zero game reads during a fight.
- **Never hook code the game shares between many methods.** One hooked function can be called hundreds of thousands
  of times per second and slow the whole game. A hook on `BeforeHideObject` once did exactly that in fights.
- **Measure it.** Put an empty file `reframework/data/perf_on.txt` in the game folder, and each DLL writes a
  per-second performance log (time per frame, calls into the game).

---

## 5. Checklist before a release

- [ ] `reframework_plugin_required_version` says **1.10**.
- [ ] No API member newer than 1.10 is called without a `param->version` check.
- [ ] Every feature that must work online lives in the DLL, not in a Lua script.
- [ ] Tested on **REFramework 1.5.8** (the public release), not only on a nightly.
- [ ] An online match played with the perf log on: nothing heavy runs during the fight.
