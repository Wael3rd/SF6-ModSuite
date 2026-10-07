// perf: see perf.hpp.

#include "perf.hpp"

#include <windows.h>
#include <share.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace perf {
namespace {

constexpr const char* kOnFile = "reframework/data/SF6_ColorModNative_data/perf_on.txt";
constexpr const char* kLogFile = "reframework/data/SF6_ColorModNative_data/perf_live.log";
constexpr uint64_t kCheckEvery = 120;           // frames between two looks at perf_on.txt
constexpr double kSlowFrameMs = 20.0;           // a SLOW frame is longer than this and than kSlowFactor x the average
constexpr double kSlowFactor = 1.5;
constexpr double kSlowOursMs = 2.0;             // or the DLL alone took this long in it
constexpr long kMaxLogBytes = 50L * 1024 * 1024;

const char* const kNames[kCount] = {
    "CoreFrame", "VfxEngineUpdate", "VfxEngineLate", "VfxTickUpdate", "HookSetupColor", "HookSetupParts",
    "HookGetColorList", "HookSetColor", "HookTempColor", "HookRequestEffect", "HookAddMaterial", "HookBodyAddMaterial",
    "HookBodyRequestMaterial", "HookGuide",
    "FrameDecide", "FrameVfxFeed", "FrameDipanel", "FrameTracker", "FrameGuide", "FrameLocale", "FrameCamera",
    "FrameScreens", "VfxScan", "VfxApply",
};

std::atomic<bool> g_on{ false };
double g_ms_per_tick = 0;

struct Acc {                                    // since the last frame(): written by any thread
    std::atomic<int64_t> ticks{ 0 }, max{ 0 };
    std::atomic<int32_t> calls{ 0 };
};
Acc g_acc[kCount];

struct Win {                                    // the current second: frame() only
    double ms = 0, max_call = 0;
    int64_t calls = 0;
};
Win g_win[kCount];
uint64_t g_frames = 0, g_win_frames = 0;
int64_t g_last = 0, g_win_start = 0;
double g_win_sum = 0, g_win_worst = 0, g_win_ours = 0, g_win_ours_worst = 0, g_avg = 16.7;
int g_win_slow = 0;
FILE* g_log = nullptr;

int64_t now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

std::string clock_text(bool ms) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    if (ms) std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    else std::snprintf(b, sizeof b, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return b;
}

void write_line(const std::string& line) {
    if (!g_log) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(kLogFile).parent_path(), ec);
        g_log = _fsopen(kLogFile, "ab", _SH_DENYNO);   // shared: readable while the game writes
        if (!g_log) return;
    }
    if (std::ftell(g_log) > kMaxLogBytes) {
        std::fclose(g_log);
        g_log = _fsopen(kLogFile, "wb", _SH_DENYNO);
        if (!g_log) return;
        std::fputs("(log restarted: over 50 MB)\n", g_log);
    }
    std::fputs(line.c_str(), g_log);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void set_on(bool on) {
    if (on == g_on.load(std::memory_order_relaxed)) return;
    if (on) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_ms_per_tick = 1000.0 / (double)f.QuadPart;
        for (auto& a : g_acc) { a.ticks = 0; a.max = 0; a.calls = 0; }
        for (auto& w : g_win) w = Win{};
        g_last = 0;
        g_win_start = now();
        g_win_frames = 0; g_win_sum = g_win_worst = g_win_ours = g_win_ours_worst = 0; g_win_slow = 0;
        write_line(clock_text(false) + " perf on (frame " + std::to_string(g_frames) + "): SLOW = frame over " +
                   std::to_string((int)kSlowFrameMs) + " ms and 1.5x the average, or the DLL over 2 ms in it");
    } else {
        write_line(clock_text(false) + " perf off");
        if (g_log) { std::fclose(g_log); g_log = nullptr; }
    }
    g_on.store(on, std::memory_order_relaxed);
}

std::string fmt_ms(double ms) { char b[32]; std::snprintf(b, sizeof b, "%.2f", ms); return b; }

}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }

int64_t begin() { return g_on.load(std::memory_order_relaxed) ? now() : 0; }

void end(Id id, int64_t t0) {
    if (!t0 || (unsigned)id >= (unsigned)kCount) return;
    const int64_t d = now() - t0;
    Acc& a = g_acc[id];
    a.ticks.fetch_add(d, std::memory_order_relaxed);
    a.calls.fetch_add(1, std::memory_order_relaxed);
    int64_t m = a.max.load(std::memory_order_relaxed);
    while (d > m && !a.max.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
}

void frame() {
    ++g_frames;
    if (g_frames % kCheckEvery == 1) {
        std::error_code ec;
        set_on(std::filesystem::exists(kOnFile, ec));
    }
    if (!g_on.load(std::memory_order_relaxed)) return;
    const int64_t t = now();
    const double dt = g_last ? (double)(t - g_last) * g_ms_per_tick : 0;
    g_last = t;
    // what ran since the last frame
    double part_ms[kCount], part_max[kCount];
    int32_t part_calls[kCount];
    double ours = 0;
    for (int i = 0; i < kCount; ++i) {
        part_ms[i] = (double)g_acc[i].ticks.exchange(0, std::memory_order_relaxed) * g_ms_per_tick;
        part_calls[i] = g_acc[i].calls.exchange(0, std::memory_order_relaxed);
        part_max[i] = (double)g_acc[i].max.exchange(0, std::memory_order_relaxed) * g_ms_per_tick;
        if (i < kTopCount) ours += part_ms[i];
        Win& w = g_win[i];
        w.ms += part_ms[i]; w.calls += part_calls[i]; w.max_call = std::max(w.max_call, part_max[i]);
    }
    if (dt > 0) {
        ++g_win_frames;
        g_win_sum += dt;
        g_win_worst = std::max(g_win_worst, dt);
        g_win_ours += ours;
        g_win_ours_worst = std::max(g_win_ours_worst, ours);
        if ((dt > kSlowFrameMs && dt > kSlowFactor * g_avg) || ours > kSlowOursMs) {
            ++g_win_slow;
            std::vector<std::pair<double, int>> parts;
            for (int i = 0; i < kCount; ++i) if (part_calls[i]) parts.emplace_back(part_ms[i], i);
            std::sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.first > b.first; });
            std::string s = clock_text(true) + " f" + std::to_string(g_frames) + " SLOW " + fmt_ms(dt) + " ms (avg " + fmt_ms(g_avg) +
                            ") DLL " + fmt_ms(ours) + " ms:";
            for (auto& [ms, i] : parts) {
                s += " " + std::string(kNames[i]) + " " + fmt_ms(ms) + "/" + std::to_string(part_calls[i]);
                if (part_calls[i] > 1) s += " max " + fmt_ms(part_max[i]);
            }
            write_line(s);
        }
        g_avg = g_avg * 0.95 + dt * 0.05;
    }
    // once a second: the summary
    const double win_ms = (double)(t - g_win_start) * g_ms_per_tick;
    if (win_ms >= 1000.0 && g_win_frames) {
        std::vector<std::pair<double, int>> parts;
        for (int i = 0; i < kCount; ++i) if (g_win[i].calls) parts.emplace_back(g_win[i].ms, i);
        std::sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string s = clock_text(false) + " f" + std::to_string(g_frames) + " " + std::to_string(g_win_frames) + " frames, avg " +
                        fmt_ms(g_win_sum / (double)g_win_frames) + " worst " + fmt_ms(g_win_worst) + " ms, slow " +
                        std::to_string(g_win_slow) + " | DLL avg " + fmt_ms(g_win_ours / (double)g_win_frames) + " worst " +
                        fmt_ms(g_win_ours_worst) + " ms/frame |";
        int shown = 0;
        for (auto& [ms, i] : parts) {
            if (shown++ == 8) break;
            s += " " + std::string(kNames[i]) + " " + fmt_ms(ms) + "/" + std::to_string(g_win[i].calls) + " max " + fmt_ms(g_win[i].max_call);
        }
        write_line(s);
        for (auto& w : g_win) w = Win{};
        g_win_start = t;
        g_win_frames = 0; g_win_sum = g_win_worst = g_win_ours = g_win_ours_worst = 0; g_win_slow = 0;
    }
}

}  // namespace perf
