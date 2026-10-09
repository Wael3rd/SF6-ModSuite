// perf: what this DLL costs the game, live. Off unless reframework/data/perf_on.txt exists
// (checked every 120 frames). When on, every entry point is timed and
// reframework/data/SF6_CostumeSlots_data/perf_live.log gets one line per second and SLOW lines.
// A frame is the time between two calls of frame() (on_late_update, LateUpdateBehavior).
// Off, begin() is one relaxed atomic load and end() returns at once.
#pragma once

#include <cstdint>
#include <string>

namespace perf {

enum Id : int {
    // entry points: the game calls them, their times add up to what the DLL costs it
    OnLateUpdate,       // the whole LateUpdateBehavior callback
    // one Id per hooked game method (costumes.cpp, then stages.cpp)
    HkCosHolderCtor,
    HkCosMenuCreatedObject,
    HkCosMenuShowedObject,
    HkCosMenuHidObject,
    HkCosMenuOnEnd,
    HkCosFlowStart,
    HkCosFlowOnDestroy,
    HkCosUI10501Start,
    HkCosUI10501End,
    HkCosTitleOnDestroy,
    HkStgShown_GotObject,
    HkStgShown_BeforeShowObject,
    HkStgShown_ShowedObject,
    HkStgHidden_BeforeHideObject,
    HkStgHidden_OnEnd,
    HkStgDirty_SelectionChangedWithArgs_List,
    HkStgDirty_SetPreview,
    HkStgDirty_ChangePreview,
    HkAgentInputUp,
    HkAgentInputDown,
    HkVsActivate,
    HkVsSetDispInfo,
    HkVsSetTexture,
    kTopCount,
    // sub-parts of OnLateUpdate (shown, not added again)
    CostumesTick = kTopCount,
    StagesTick,
    StateWrite,         // write_file (state.json)
    kCount
};

bool on();
int64_t begin();                  // 0 when off
void end(Id id, int64_t t0);      // no-op when t0 == 0
void frame();                     // once per game frame, from on_late_update

// background writer: queues text for a file, written on a shared thread (no I/O on the caller)
void bg_write(const char* path, const std::string& text);

struct Scope {
    explicit Scope(Id id) : id_(id), t0_(begin()) {}
    ~Scope() { end(id_, t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Id id_;
    int64_t t0_;
};

}  // namespace perf
