// perf: what this DLL costs the game, live (same switch and line format as the Native Core's perf log, so logs compare).
// Off unless reframework/data/perf_on.txt exists (looked at every 120 frames with GetFileAttributesA). When on, every entry
// point is timed with QueryPerformanceCounter and reframework/data/SF6_CRSMNative_data/perf_live.log gets:
//   - one line per second: frames, frame time (average / worst between two frame callbacks), what the DLL took per frame,
//     game calls per second (gm_calls, core_calls, per wrapper), the costliest entry points as "Name total_ms/count max ms";
//   - one SLOW line for a frame where the DLL alone took more than 2 ms (or the frame is over 20 ms and 1.5x the average);
//   - every 10 s, one "ALL:" line: EVERY entry that ran in those 10 s (name, calls, total ms, worst call), by calls, and the
//     game calls of the 10 s (gm_calls per wrapper, core_calls, per frame): cheap but frequent hooks show there.
// The file is bounded: rotated to perf_live.old.log past 8 MB.
// Off, begin() is one relaxed atomic load and end() returns at once; the counters are one relaxed load and a branch.
// Entry points are "top" (the game or the Lua calls them: hook bodies, the frame callback, window messages, bridge
// functions; their times add up to the DLL's cost) or "sub" (a part of the frame callback: shown, not added again).
// Accumulation is per entry in relaxed atomics: safe from any thread, no locks in hook bodies.
#pragma once

#include <atomic>
#include <cstdint>

namespace perf {

constexpr int kMaxEntries = 384;

int reg(const char* name, bool top);          // an entry point id (same name = same id); at start / install time, any thread
bool on();
int64_t begin();                              // 0 when off
void end(int id, int64_t t0);                 // no-op when t0 == 0 or id < 0
void frame();                                 // once per game frame, first thing of the frame callback

// Scope: for functions without __try (an object with a destructor is not allowed there; use begin / end in locals there)
struct Scope {
    explicit Scope(int id) : id_(id), t0_(begin()) {}
    ~Scope() { end(id_, t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    int id_;
    int64_t t0_;
};
// A Scope whose id is registered once (a part of the frame callback / an entry point)
#define PERF_SUB(var, name) static const int var##_id = ::perf::reg(name, false); ::perf::Scope var(var##_id)
#define PERF_TOP(var, name) static const int var##_id = ::perf::reg(name, true); ::perf::Scope var(var##_id)

// ---- game calls ----------------------------------------------------------------------------------------------------
enum Gm : int {
    GmType, GmField, GmMethod, GmMethodN, GmSingleton, GmIsManaged,
    GmRef, GmGetInt, GmSetInt, GmSetRef, GmIsFloat,
    GmInvoke, GmNewObject,
    GmCount
};
enum Core : int { CoreCall, CoreCount };

extern std::atomic<bool> g_on;
extern std::atomic<uint32_t> g_gm[GmCount];
extern std::atomic<uint32_t> g_core[CoreCount];
inline void gm(Gm w) { if (g_on.load(std::memory_order_relaxed)) g_gm[w].fetch_add(1, std::memory_order_relaxed); }
inline void core_call() { if (g_on.load(std::memory_order_relaxed)) g_core[CoreCall].fetch_add(1, std::memory_order_relaxed); }

// a core API pointer used for one call: perf::counted(api)->fn(...) counts it (core_calls/s)
template <class T> inline T* counted(T* p) { core_call(); return p; }

}  // namespace perf
