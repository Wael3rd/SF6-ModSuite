// wjournal: see wjournal.hpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "wjournal.hpp"
#include "perf.hpp"
#include "slots.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace wjournal {
namespace {

constexpr const char* kOnFile  = "reframework/data/write_journal_on.txt";
constexpr const char* kLogFile = "reframework/data/SF6_CostumeSlots_data/writes.log";
constexpr uint32_t kCheckEvery = 120;

std::atomic<bool> g_on{false};

}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }

void frame() {
    if (slots::g_frame % kCheckEvery != 1) return;
    bool want = GetFileAttributesA(kOnFile) != INVALID_FILE_ATTRIBUTES;
    bool was = g_on.load(std::memory_order_relaxed);
    if (want == was) return;
    g_on.store(want, std::memory_order_relaxed);
    SYSTEMTIME t; GetLocalTime(&t);
    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "%02d:%02d:%02d.%03d f%u T%lu ",
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                  slots::g_frame, GetCurrentThreadId());
    perf::bg_write(kLogFile, std::string(hdr) + (want ? "write journal ON\n" : "write journal OFF\n"));
}

void log(const char* fmt, ...) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "%02d:%02d:%02d.%03d f%u T%lu ",
                  t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                  slots::g_frame, GetCurrentThreadId());
    char body[512];
    va_list a; va_start(a, fmt);
    int n = std::vsnprintf(body, sizeof body, fmt, a);
    va_end(a);
    if (n < 0) n = 0;
    if (n >= static_cast<int>(sizeof body)) n = static_cast<int>(sizeof body) - 1;
    perf::bg_write(kLogFile, std::string(hdr) + std::string(body, static_cast<size_t>(n)) + "\n");
}

}  // namespace wjournal
