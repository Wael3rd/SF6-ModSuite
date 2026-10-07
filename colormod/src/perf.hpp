// perf: what this DLL costs the game, live. Off unless reframework/data/SF6_ColorModNative_data/perf_on.txt exists
// (checked every 2 s: it can be created or deleted while the game runs). When on, every entry point is timed with
// QueryPerformanceCounter and reframework/data/SF6_ColorModNative_data/perf_live.log gets:
//   - one line per second: frames, frame time (average / worst), what the DLL took per frame, its costliest parts;
//   - one SLOW line per game frame longer than kSlowFrameMs (or where the DLL alone took more than kSlowOursMs),
//     with every part of the DLL that ran during that frame (time, calls).
// A frame is the time between two calls of frame() (the core's frame callback, LateUpdateBehavior).
// Off, begin() is one relaxed atomic load and end() returns at once.
#pragma once

#include <cstdint>

namespace perf {

enum Id : int {
    // entry points: the game calls them, their times add up to what the DLL costs it
    CoreFrame,              // the core's frame callback (every module's per-frame work below)
    VfxEngineUpdate,        // vfx::engine, UpdateBehavior (scan requests, relay, queue)
    VfxEngineLate,          // vfx::engine, LateUpdateBehavior (relay, queue)
    VfxTickUpdate,          // vfx::tick, UpdateBehavior (scans, spots, providers)
    HookSetupColor,         // app.PlayerColorController.SetupColor pre / post
    HookSetupParts,         // SetupPartsColor pre / post
    HookGetColorList,       // ColorInventory.GetColorList pre / post
    HookSetColor,           // PlayerColorController.SetColor pre
    HookTempColor,          // GuiEsportsManager.setTempCosColData / setTempColor
    HookRequestEffect,      // vfx engine: requestEffect (Drive effects born)
    HookAddMaterial,        // vfx engine: addMaterialEffect
    HookBodyAddMaterial,    // vfx body: VfxActorEffectManager.addMaterialEffect
    HookBodyRequestMaterial,// vfx body: VfxActorEffectManager.requestMaterialEffect
    HookGuide,              // input guide hooks (screens::guide)
    kTopCount,
    // parts of the entry points above (shown, not added again)
    FrameDecide = kTopCount, FrameVfxFeed, FrameDipanel, FrameTracker, FrameGuide, FrameLocale, FrameCamera, FrameScreens,
    VfxScan, VfxApply,
    kCount
};

bool on();
int64_t begin();                  // 0 when off
void end(Id id, int64_t t0);      // no-op when t0 == 0
void frame();                     // once per game frame, from the core's frame callback

struct Scope {                    // for functions without __try (an object with a destructor is not allowed there)
    explicit Scope(Id id) : id_(id), t0_(begin()) {}
    ~Scope() { end(id_, t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Id id_;
    int64_t t0_;
};

}  // namespace perf
