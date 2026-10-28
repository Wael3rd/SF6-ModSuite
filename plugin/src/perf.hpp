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
    RtoTick,            // rto::tick (runtime costume options), all of it
    RtoWatch,           // its fighters watch (watch_tick)
    RtoVs,              // its VS screen models (vs_tick)
    RtoVsScan,          // the VS screen's scene search inside vs_tick
    RtoSelect,          // its character select models (select::tick + select_tick)
    RtoPreview,         // the Battle Settings preview and its menu (menu::tick + preview_tick)
    RtoWalk,            // a model's walk (its GameObjects and components), inside the parts above
    RtoPass,            // a model's swap pass (pass), inside the parts above
    RtoWant,            // what a pass wants (chosen_of + desired), inside RtoPass
    RtoBind,            // a part's material bindings search, inside RtoPass
    RtoPopup,           // the Training popup (select::popup_tick)
    RtoKeys,            // the options key read from settings.json (read_keys), inside RtoPopup / RtoSelect
    RtoGuide,           // the game's guide read for the log (log_game_guide), inside RtoPopup / RtoSelect
    RtoMem,             // the "rto mem:" lines (mem_tick)
    StgScreen,          // stages: the stage select's name / preview / UP-DOWN work (screen_work)
    StgStrip,           // stages: the strip cells (strip_work)
    StgStripScan,       // stages: the strip's cells looked for (strip_scan), inside StgStrip
    StgCheck,           // stages: the stage select's visibility check (find_agent)
    StgVs,              // stages: the VS screen's stage name and image (vs_work)
    FsNameTick,         // fsname::tick
    kCount
};

bool on();
int64_t begin();                  // 0 when off
void end(Id id, int64_t t0);      // no-op when t0 == 0
void frame();                     // once per game frame, from on_late_update

// background writer: queues text for a file, written on a shared thread (no I/O on the caller)
void bg_write(const char* path, const std::string& text);
// waits (at most timeout_ms) until everything queued before the call is written
void bg_flush(unsigned timeout_ms);

struct Scope {
    explicit Scope(Id id) : id_(id), t0_(begin()) {}
    ~Scope() { end(id_, t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Id id_;
    int64_t t0_;
};

}  // namespace perf
