// perf: see perf.hpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "perf.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace perf {
namespace {

constexpr const char* kOnFile  = "reframework/data/perf_on.txt";
constexpr const char* kLogFile = "reframework/data/sf6core/perf_live.log";
constexpr uint64_t kCheckEvery = 120;           // frames between two looks at perf_on.txt
constexpr double kSlowFrameMs = 20.0;           // a SLOW frame is longer than this and than kSlowFactor x the average
constexpr double kSlowFactor  = 1.5;
constexpr double kSlowOursMs  = 2.0;            // or the DLL alone took this long in it

const char* const kFixedNames[kCount] = {
    "OnLateUpdate", "DrawElement", "OptionsPump", "OptHooks", "GuidHook", "Api",
    "PollFlow", "UpdateBreakers", "FlowCallbacks", "OptionsFrame", "FrameCallbacks",
    "CommitPhase", "CommitWidgets", "CommitElemHides", "CommitZenHides",
    "Client0", "Client1", "Client2", "Client3",
    "Client4", "Client5", "Client6", "Client7",
};

// per-client name table: "Client:<client_id>" for the Client0..Client7 slots
constexpr int kMaxClients = 8;
char g_client_names[kMaxClients][80] = {};
int  g_client_count = 0;
std::mutex g_client_mutex;

const char* name_of(int id) {
    if (id >= Client0 && id < Client0 + kMaxClients && g_client_names[id - Client0][0])
        return g_client_names[id - Client0];
    return kFixedNames[id];
}

// ---- background file writer (same design as the Color Mod's bgwriter) ----
namespace bgw {
struct Item { std::string content; };
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Item> g_queue;
std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};

void worker() {
    FILE* f = nullptr;
    while (!g_stop.load(std::memory_order_relaxed)) {
        Item item;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait_for(lk, std::chrono::milliseconds(200),
                          [] { return !g_queue.empty() || g_stop.load(std::memory_order_relaxed); });
            if (g_queue.empty()) continue;
            item = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (!f) {
            CreateDirectoryA("reframework/data/sf6core", nullptr);
            f = fopen(kLogFile, "ab");
        }
        if (f) { fwrite(item.content.data(), 1, item.content.size(), f); fflush(f); }
    }
    if (f) fclose(f);
}

void ensure_started() {
    if (g_started.exchange(true, std::memory_order_acq_rel)) return;
    std::thread(worker).detach();
}

void append(const std::string& text) {
    ensure_started();
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back(Item{text});
    g_cv.notify_one();
}
}  // namespace bgw

std::atomic<bool> g_on{false};
double g_ms_per_tick = 0;

struct Acc {                                    // since the last frame(): written by any thread
    std::atomic<int64_t> ticks{0}, max{0};
    std::atomic<int32_t> calls{0};
};
Acc g_acc[kCount];

struct Win {                                    // the current second: frame() only
    double ms = 0, max_call = 0;
    int64_t calls = 0;
};
Win g_win[kCount];
uint64_t g_frames = 0, g_win_frames = 0;
int64_t  g_last = 0, g_win_start = 0;
double   g_win_sum = 0, g_win_worst = 0, g_win_ours = 0, g_win_ours_worst = 0, g_avg = 16.7;
int      g_win_slow = 0;

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
    bgw::append(line + "\n");
}

void set_on(bool v) {
    if (v == g_on.load(std::memory_order_relaxed)) return;
    if (v) {
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
    }
    g_on.store(v, std::memory_order_relaxed);
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

Id client_id(const char* name) {
    std::lock_guard<std::mutex> lk(g_client_mutex);
    for (int i = 0; i < g_client_count; ++i)
        if (strstr(g_client_names[i], name)) return (Id)(Client0 + i);
    if (g_client_count >= kMaxClients) return FrameCallbacks;   // overflow: aggregate
    int slot = g_client_count++;
    snprintf(g_client_names[slot], sizeof(g_client_names[slot]), "Client:%s", name);
    return (Id)(Client0 + slot);
}

void frame() {
    ++g_frames;
    if (g_frames % kCheckEvery == 1) {
        const DWORD a = GetFileAttributesA(kOnFile);
        set_on(a != INVALID_FILE_ATTRIBUTES);
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
        part_ms[i]   = (double)g_acc[i].ticks.exchange(0, std::memory_order_relaxed) * g_ms_per_tick;
        part_calls[i] = g_acc[i].calls.exchange(0, std::memory_order_relaxed);
        part_max[i]  = (double)g_acc[i].max.exchange(0, std::memory_order_relaxed) * g_ms_per_tick;
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
                s += " " + std::string(name_of(i)) + " " + fmt_ms(ms) + "/" + std::to_string(part_calls[i]);
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
            s += " " + std::string(name_of(i)) + " " + fmt_ms(ms) + "/" + std::to_string(g_win[i].calls) + " max " + fmt_ms(g_win[i].max_call);
        }
        write_line(s);
        for (auto& w : g_win) w = Win{};
        g_win_start = t;
        g_win_frames = 0; g_win_sum = g_win_worst = g_win_ours = g_win_ours_worst = 0; g_win_slow = 0;
    }
}

}  // namespace perf
