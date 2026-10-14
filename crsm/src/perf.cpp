// perf: see perf.hpp.
#include "perf.hpp"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common.hpp"

namespace perf {

std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_gm[GmCount];
std::atomic<uint32_t> g_core[CoreCount];

namespace {

constexpr const char* kOnFile  = "reframework/data/perf_on.txt";
constexpr const char* kDir     = "reframework/data/SF6_CRSMNative_data";
constexpr const char* kLogFile = "reframework/data/SF6_CRSMNative_data/perf_live.log";
constexpr const char* kOldFile = "reframework/data/SF6_CRSMNative_data/perf_live.old.log";
constexpr uint64_t kCheckEvery = 120;           // frames between two looks at perf_on.txt
constexpr double kSlowFrameMs = 20.0;           // a SLOW frame is longer than this and than kSlowFactor x the average
constexpr double kSlowFactor  = 1.5;
constexpr double kSlowOursMs  = 2.0;            // or the DLL alone took this long in it
constexpr size_t kRotateAt = 8u * 1024u * 1024u;
constexpr int kShown = 10;
constexpr double kAllEveryMs = 10000.0;         // the "ALL:" line: every entry that ran, every 10 s

const char* const kGmNames[GmCount] = {
    "type", "field", "method", "method_n", "singleton", "is_managed",
    "ref", "get_int", "set_int", "set_ref", "is_float", "invoke", "new_object",
};

// ---- the entry table ----------------------------------------------------------------------------------------------
struct Entry { char name[72]; bool top; };
Entry g_entries[kMaxEntries];
std::atomic<int> g_count{0};
std::mutex g_reg_mutex;

// ---- background file writer (same design as the core's) ----
namespace bgw {
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<std::string> g_queue;
std::atomic<bool> g_started{false};

void worker() {
    FILE* f = nullptr;
    size_t written = 0;
    for (;;) {
        std::string item;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_queue.empty(); });
            if (g_queue.empty()) continue;
            item = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (!f) {
            CreateDirectoryA(kDir, nullptr);
            f = fopen(kLogFile, "ab");
            if (f) { fseek(f, 0, SEEK_END); written = (size_t)ftell(f); }
        }
        if (f && written > kRotateAt) {                // bounded: perf_live.log -> perf_live.old.log
            fclose(f);
            DeleteFileA(kOldFile);
            MoveFileA(kLogFile, kOldFile);
            f = fopen(kLogFile, "ab");
            written = 0;
        }
        if (f) { fwrite(item.data(), 1, item.size(), f); fflush(f); written += item.size(); }
    }
}

void append(std::string text) {
    if (!g_started.exchange(true, std::memory_order_acq_rel)) std::thread(worker).detach();
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back(std::move(text));
    g_cv.notify_one();
}
}  // namespace bgw

struct Acc {                                    // since the last frame(): written by any thread
    std::atomic<int64_t> ticks{0}, max{0};
    std::atomic<int32_t> calls{0};
};
Acc g_acc[kMaxEntries];

struct Win {                                    // the current second: frame() only
    double ms = 0, max_call = 0;
    int64_t calls = 0;
};
Win g_win[kMaxEntries];
uint64_t g_frames = 0, g_win_frames = 0;
double   g_ms_per_tick = 0;
int64_t  g_last = 0, g_win_start = 0;
double   g_win_sum = 0, g_win_worst = 0, g_win_ours = 0, g_win_ours_worst = 0, g_avg = 16.7;
int      g_win_slow = 0;
uint64_t g_win_gm[GmCount] = {};
uint64_t g_win_core = 0;
// the 10-second window of the "ALL:" line (the per-second windows added up)
Win g_all[kMaxEntries];
uint64_t g_all_frames = 0;
uint64_t g_all_gm[GmCount] = {};
uint64_t g_all_core = 0;
int64_t g_all_start = 0;

int64_t now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }

std::string clock_text(bool ms) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    if (ms) std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    else std::snprintf(b, sizeof b, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return b;
}

void write_line(const std::string& line) { bgw::append(line + "\n"); }

void reset_window() {
    for (auto& w : g_win) w = Win{};
    g_win_frames = 0; g_win_sum = g_win_worst = g_win_ours = g_win_ours_worst = 0; g_win_slow = 0;
    for (auto& v : g_win_gm) v = 0;
    g_win_core = 0;
}

void reset_all() {
    for (auto& w : g_all) w = Win{};
    g_all_frames = 0;
    for (auto& v : g_all_gm) v = 0;
    g_all_core = 0;
}

void set_on(bool v) {
    if (v == g_on.load(std::memory_order_relaxed)) return;
    if (v) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_ms_per_tick = 1000.0 / (double)f.QuadPart;
        for (auto& a : g_acc) { a.ticks = 0; a.max = 0; a.calls = 0; }
        for (auto& c : g_gm) c = 0;
        for (auto& c : g_core) c = 0;
        reset_window();
        reset_all();
        g_last = 0;
        g_win_start = now();
        g_all_start = g_win_start;
        write_line(clock_text(false) + " SF6_CRSMNative " + cmn::version() + " perf on (frame " + std::to_string(g_frames) +
                   "): SLOW = frame over " + std::to_string((int)kSlowFrameMs) + " ms and 1.5x the average, or the DLL over 2 ms in it");
    } else {
        write_line(clock_text(false) + " perf off");
    }
    g_on.store(v, std::memory_order_relaxed);
}

std::string fmt_ms(double ms) { char b[32]; std::snprintf(b, sizeof b, "%.2f", ms); return b; }

}  // namespace

int reg(const char* name, bool top) {
    std::lock_guard<std::mutex> lk(g_reg_mutex);
    const int n = g_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(g_entries[i].name, name) == 0) return i;
    if (n >= kMaxEntries) return -1;
    std::snprintf(g_entries[n].name, sizeof g_entries[n].name, "%s", name);
    g_entries[n].top = top;
    g_count.store(n + 1, std::memory_order_release);
    return n;
}

bool on() { return g_on.load(std::memory_order_relaxed); }

int64_t begin() { return g_on.load(std::memory_order_relaxed) ? now() : 0; }

void end(int id, int64_t t0) {
    if (!t0 || (unsigned)id >= (unsigned)kMaxEntries) return;
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
        const DWORD a = GetFileAttributesA(kOnFile);
        set_on(a != INVALID_FILE_ATTRIBUTES);
    }
    if (!g_on.load(std::memory_order_relaxed)) return;
    const int64_t t = now();
    const double dt = g_last ? (double)(t - g_last) * g_ms_per_tick : 0;
    g_last = t;
    const int n = g_count.load(std::memory_order_acquire);
    // what ran since the last frame
    std::vector<double> part_ms((size_t)n), part_max((size_t)n);
    std::vector<int32_t> part_calls((size_t)n);
    double ours = 0;
    for (int i = 0; i < n; ++i) {
        part_ms[i]    = (double)g_acc[i].ticks.exchange(0, std::memory_order_relaxed) * g_ms_per_tick;
        part_calls[i] = g_acc[i].calls.exchange(0, std::memory_order_relaxed);
        part_max[i]   = (double)g_acc[i].max.exchange(0, std::memory_order_relaxed) * g_ms_per_tick;
        if (g_entries[i].top) ours += part_ms[i];
        Win& w = g_win[i];
        w.ms += part_ms[i]; w.calls += part_calls[i]; w.max_call = std::max(w.max_call, part_max[i]);
    }
    for (int i = 0; i < GmCount; ++i) g_win_gm[i] += g_gm[i].exchange(0, std::memory_order_relaxed);
    g_win_core += g_core[CoreCall].exchange(0, std::memory_order_relaxed);
    if (dt > 0) {
        ++g_win_frames;
        g_win_sum += dt;
        g_win_worst = std::max(g_win_worst, dt);
        g_win_ours += ours;
        g_win_ours_worst = std::max(g_win_ours_worst, ours);
        if ((dt > kSlowFrameMs && dt > kSlowFactor * g_avg) || ours > kSlowOursMs) {
            ++g_win_slow;
            std::vector<std::pair<double, int>> parts;
            for (int i = 0; i < n; ++i) if (part_calls[i]) parts.emplace_back(part_ms[i], i);
            std::sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.first > b.first; });
            std::string s = clock_text(true) + " f" + std::to_string(g_frames) + " SLOW " + fmt_ms(dt) + " ms (avg " + fmt_ms(g_avg) +
                            ") DLL " + fmt_ms(ours) + " ms:";
            for (auto& [ms, i] : parts) {
                s += " " + std::string(g_entries[i].name) + " " + fmt_ms(ms) + "/" + std::to_string(part_calls[i]);
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
        for (int i = 0; i < n; ++i) if (g_win[i].calls) parts.emplace_back(g_win[i].ms, i);
        std::sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.first > b.first; });
        const double per_s = 1000.0 / win_ms;
        uint64_t gm_total = 0;
        for (int i = 0; i < GmCount; ++i) gm_total += g_win_gm[i];
        std::string s = clock_text(false) + " f" + std::to_string(g_frames) + " " + std::to_string(g_win_frames) + " frames, avg " +
                        fmt_ms(g_win_sum / (double)g_win_frames) + " worst " + fmt_ms(g_win_worst) + " ms, slow " +
                        std::to_string(g_win_slow) + " | DLL avg " + fmt_ms(g_win_ours / (double)g_win_frames) + " worst " +
                        fmt_ms(g_win_ours_worst) + " ms/frame | gm_calls " + std::to_string((uint64_t)((double)gm_total * per_s)) + "/s (";
        bool first = true;
        for (int i = 0; i < GmCount; ++i) {
            if (!g_win_gm[i]) continue;
            s += std::string(first ? "" : " ") + kGmNames[i] + " " + std::to_string((uint64_t)((double)g_win_gm[i] * per_s));
            first = false;
        }
        s += ") core_calls " + std::to_string((uint64_t)((double)g_win_core * per_s)) + "/s |";
        int shown = 0;
        for (auto& [ms, i] : parts) {
            if (shown++ == kShown) break;
            s += " " + std::string(g_entries[i].name) + " " + fmt_ms(ms) + "/" + std::to_string(g_win[i].calls) + " max " + fmt_ms(g_win[i].max_call);
        }
        write_line(s);
        // the second added to the 10-second window; every 10 s, "ALL:": EVERY entry that ran (name count total_ms max ms,
        // by count): the cheap but frequent hooks the per-second top 10 (by time) leaves out
        for (int i = 0; i < n; ++i) {
            Win& a = g_all[i];
            a.ms += g_win[i].ms; a.calls += g_win[i].calls; a.max_call = std::max(a.max_call, g_win[i].max_call);
        }
        for (int i = 0; i < GmCount; ++i) g_all_gm[i] += g_win_gm[i];
        g_all_core += g_win_core;
        g_all_frames += g_win_frames;
        const double all_ms = (double)(t - g_all_start) * g_ms_per_tick;
        if (all_ms >= kAllEveryMs && g_all_frames) {
            std::vector<std::pair<int64_t, int>> all;
            for (int i = 0; i < n; ++i) if (g_all[i].calls) all.emplace_back(g_all[i].calls, i);
            std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            uint64_t all_gm = 0;
            for (int i = 0; i < GmCount; ++i) all_gm += g_all_gm[i];
            const double fr = (double)g_all_frames;
            char head[160];
            std::snprintf(head, sizeof head, " f%llu ALL: %.1f s, %llu frames | gm_calls %llu (%.1f/frame:", (unsigned long long)g_frames,
                          all_ms / 1000.0, (unsigned long long)g_all_frames, (unsigned long long)all_gm, (double)all_gm / fr);
            std::string a = clock_text(false) + head;
            for (int i = 0; i < GmCount; ++i) {
                if (!g_all_gm[i]) continue;
                a += std::string(" ") + kGmNames[i] + " " + std::to_string(g_all_gm[i]);
            }
            char core[96];
            std::snprintf(core, sizeof core, ") core_calls %llu (%.1f/frame) | %zu entries:", (unsigned long long)g_all_core,
                          (double)g_all_core / fr, all.size());
            a += core;
            for (auto& [calls, i] : all)
                a += " " + std::string(g_entries[i].name) + " " + std::to_string(calls) + " " + fmt_ms(g_all[i].ms) + " max " +
                     fmt_ms(g_all[i].max_call) + ",";
            if (a.back() == ',') a.pop_back();
            write_line(a);
            reset_all();
            g_all_start = t;
        }
        reset_window();
        g_win_start = t;
    }
}

}  // namespace perf
