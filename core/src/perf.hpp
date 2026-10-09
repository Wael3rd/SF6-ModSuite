// perf: what this DLL costs the game, live. Off unless reframework/data/perf_on.txt exists
// (checked every 120 frames with GetFileAttributesA). When on, every entry point is timed with
// QueryPerformanceCounter and reframework/data/sf6core/perf_live.log gets:
//   - one line per second: frames, frame time (average / worst), what the DLL took per frame,
//     its costliest parts;
//   - one SLOW line per game frame longer than 20 ms (or where the DLL alone took more than 2 ms),
//     with every part of the DLL that ran during that frame (time, calls).
// A frame is the time between two calls of frame() (on_late_update, LateUpdateBehavior).
// Off, begin() is one relaxed atomic load and end() returns at once.
#pragma once

#include <cstdint>

namespace perf {

enum Id : int {
    // entry points: the game calls them, their times add up to what the DLL costs it
    OnLateUpdate,       // the whole LateUpdateBehavior callback
    DrawElement,        // on_pre_gui_draw_element (rendering thread)
    OptionsPump,        // BeginRendering: options pump
    OptHooks,           // option-dialog hooks in options.cpp
    GuidHook,           // GetMessage hook in guid_text.cpp
    Api,                // display API calls (ui_*, obj_*, list_*, dict_*)
    kTopCount,
    // sub-parts of OnLateUpdate (shown, not added again)
    PollFlow = kTopCount,
    UpdateBreakers,
    FlowCallbacks,
    OptionsFrame,
    FrameCallbacks,
    CommitPhase,
    CommitWidgets,
    CommitElemHides,
    CommitZenHides,
    // per-client frame callback slots (up to 8 clients)
    Client0, Client1, Client2, Client3,
    Client4, Client5, Client6, Client7,
    kCount
};

bool on();
int64_t begin();                  // 0 when off
void end(Id id, int64_t t0);      // no-op when t0 == 0
void frame();                     // once per game frame, from on_late_update

// per-client: returns the Id for the given client_id (allocates a slot on first use)
Id client_id(const char* name);

// Scope: for functions without __try (an object with a destructor is not allowed there)
struct Scope {
    explicit Scope(Id id) : id_(id), t0_(begin()) {}
    ~Scope() { end(id_, t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Id id_;
    int64_t t0_;
};

}  // namespace perf
