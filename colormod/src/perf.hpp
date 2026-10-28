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
    HkDiSpinAdd,
    HkDiSpinAddNum,
    HkDiSpinMouseAdd,
    HkDiSpinSub,
    HkDiSpinSubNum,
    HkDiSpinMouseSub,
    HkDiSpinInputRight,
    HkDiSpinInputLeft,
    HkDiSpinInputDirection,
    HkDiParamSkip,
    HkDiParamGate,
    HkDiParamGetCostume,
    HkDiParamGetColor,
    HkDiMfsSkip,
    HkDiMfsGetCostume,
    HkDiMfsGetColor,
    HkDiAgentInputLeft,
    HkDiAgentInputRight,
    HkDiAgentInputUp,
    HkDiAgentInputDown,
    HkDiAgentInputDirection,
    HkDiAgentInputDecide,
    HkDiAgentInputCancel,
    HkDiGroupMoveFocus,
    HkRowsOnUpdate,
    HkRowsSpinAdd,
    HkRowsSpinAddNum,
    HkRowsSpinInputRight,
    HkRowsSpinMouseAdd,
    HkRowsSpinSub,
    HkRowsSpinSubNum,
    HkRowsSpinInputLeft,
    HkRowsSpinMouseSub,
    HkRowsSpinInputDirection,
    HkRowsAgentInputLeft,
    HkRowsAgentInputRight,
    HkRowsAgentInputDirection,
    HkRowsAgentInputDecide,
    HkTrkAgentInputCancel,
    HkTrkRequestCloseMenu,
    HkTrkCloseMenuBool,
    HkTrkCloseMenu3,
    HkGuideNewItem,
    HkGuideOnLateUpdate,
    HkGuideUpdateInputGuide,
    HkLookOnUpdate,         // dialogs/row_look: app.UIFlowOptionBGDialog.Main.OnUpdate post-hook (the hue copies)
    kTopCount,
    // parts of the entry points above (shown, not added again)
    FrameDecide = kTopCount, FrameVfxFeed, FrameDipanel, FrameTracker, FrameGuide, FrameLocale, FrameCamera, FrameScreens,
    VfxScan, VfxApply,
    // fight-start sub-scopes (what is slow during the first ~1.5 s of a fight)
    FeedDiText, FeedDrColors, FeedPartsMerge,
    EnterLoadSpots, EnterReadSig, EnterBases,
    ScanParse, ScanWalk,
    StaticValidate, StaticProviders, StaticQueue, StaticDiRelay,
    SetupDecide,
    // sub-scopes for the effect hooks (pass 3: write_block / write_validated internals)
    HookReqPages, HookReqWrites,
    // sub-scopes for previously unattributed time (pass 2)
    CoreJournalFlush, CoreFileCheck,
    ScreensDecidePull, ScreensTick, ScreensRefreshFn, ScreensFsRefresh,
    ScreensEditorTick, ScreensFsRestore, ScreensTcsRestore, ScreensDecidePush, ScreensOptApply,
    DipanelTickAll,
    FeedDiState, FeedDrCol,
    // vfx engine: relay reload slices, rolling page re-check of the relay blocks, background scan hand-off / commit
    VfxRelayReload, VfxPageRefresh, ScanCommit,
    // vfx providers: DataInstanceMap scan, the 30-frame static tick, provider walks (budgeted slices), lines / parts built
    ProvScan, ProvTick, ProvSnap, ProvBuild,
    // SetupDecide's parts: who wears what (should_inject), the colour data written (inject), the journal line
    SetupResolve, SetupInject, SetupJournal,
    // ScreensTick's parts (every 3 frames), the tracker's paints, editor_tick's guide reads, the Slots Loader claim calls
    ScrGuideFlags, ScrResolve, ScrDipanel, ScrRows, ScrFsTick, ScrTcsTick, ScrMenus, ScrGuideList,
    TrkMenus, TrkCamera, TrkDots, SlotsClaim,
    ScrSelChange, ScrSaveState,
    // VirtualQuery calls (memguard, any thread), the colour files' stat / read (live::file_tree), the wait for the
    // colour patch lock in the SetupColor hooks
    PageQuery, SetupFiles, SetupLockWait,
    kCount
};

bool on();
int64_t begin();                  // 0 when off
void end(Id id, int64_t t0);      // no-op when t0 == 0
void frame();                     // once per game frame, from the core's frame callback
// What the game shows, appended to perf_live.log lines (" | ctx menu", " | ctx fight m14", " | ctx training m2") so a
// log can be split by context. ctx: 0 menu, 1 fight, 2 training; mode = app.EGameMode (-1 = unknown). Set by vfx feed.
void set_context(int ctx, int mode);
// vfx diagnostics (the per-label / per-parameter counters of vfx_body_probe / vfx_mech_probe): off unless
// reframework/data/SF6_ColorModNative_data/vfx_debug_on.txt exists (looked at with perf_on.txt, every 2 s).
bool vfx_debug();

struct Scope {                    // for functions without __try (an object with a destructor is not allowed there)
    explicit Scope(Id id) : id_(id), t0_(begin()) {}
    ~Scope() { end(id_, t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Id id_;
    int64_t t0_;
};

}  // namespace perf
