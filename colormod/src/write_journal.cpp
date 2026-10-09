// write_journal: see write_journal.hpp.

#include "write_journal.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "common.hpp"

namespace wjournal {
namespace {

constexpr const char* kOnFile = "reframework/data/write_journal_on.txt";
constexpr const char* kLogFile = "reframework/data/SF6_ColorModNative_data/writes.log";

std::atomic<bool> g_on{false};
std::mutex g_mx;
std::string g_buf;

std::string timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}

void append_line(const char* line) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_buf += line;
    g_buf += '\n';
}

}  // namespace

bool active() { return g_on.load(std::memory_order_relaxed); }

void check() {
    const DWORD attr = GetFileAttributesA(kOnFile);
    const bool on = attr != INVALID_FILE_ATTRIBUTES;
    const bool was = g_on.load(std::memory_order_relaxed);
    if (on != was) {
        g_on.store(on, std::memory_order_relaxed);
        if (on) cmn::journal("write journal: ON (write_journal_on.txt found)");
        else { flush(); cmn::journal("write journal: OFF"); }
    }
}

void flush() {
    std::string out;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_buf.empty()) return;
        out.swap(g_buf);
    }
    cmn::bgwriter::append(kLogFile, std::move(out));
}

void raw32(const char* tag, uint64_t addr, uint32_t old_val, uint32_t new_val, uint64_t obj, uint64_t ti) {
    char line[256];
    if (obj || ti)
        std::snprintf(line, sizeof line, "%s f%llu T%lu %s addr=%llX old=%08X new=%08X obj=%llX ti=%llX",
                      timestamp().c_str(), (unsigned long long)cmn::frame(), (unsigned long)GetCurrentThreadId(),
                      tag, (unsigned long long)addr, old_val, new_val, (unsigned long long)obj, (unsigned long long)ti);
    else
        std::snprintf(line, sizeof line, "%s f%llu T%lu %s addr=%llX old=%08X new=%08X",
                      timestamp().c_str(), (unsigned long long)cmn::frame(), (unsigned long)GetCurrentThreadId(),
                      tag, (unsigned long long)addr, old_val, new_val);
    append_line(line);
}

void raw8(const char* tag, uint64_t addr, uint8_t old_val, uint8_t new_val) {
    char line[256];
    std::snprintf(line, sizeof line, "%s f%llu T%lu %s addr=%llX old=%02X new=%02X",
                  timestamp().c_str(), (unsigned long long)cmn::frame(), (unsigned long)GetCurrentThreadId(),
                  tag, (unsigned long long)addr, old_val, new_val);
    append_line(line);
}

void field(const char* tag, uint64_t obj_addr, const char* field_name, uint32_t old_val, uint32_t new_val) {
    char line[256];
    std::snprintf(line, sizeof line, "%s f%llu T%lu %s obj=%llX field=%s old=%08X new=%08X",
                  timestamp().c_str(), (unsigned long long)cmn::frame(), (unsigned long)GetCurrentThreadId(),
                  tag, (unsigned long long)obj_addr, field_name, old_val, new_val);
    append_line(line);
}

void set_color(const char* tag, uint64_t ctrl_addr, uint32_t color_num) {
    char line[256];
    std::snprintf(line, sizeof line, "%s f%llu T%lu %s ctrl=%llX color=%u",
                  timestamp().c_str(), (unsigned long long)cmn::frame(), (unsigned long)GetCurrentThreadId(),
                  tag, (unsigned long long)ctrl_addr, color_num);
    append_line(line);
}

}  // namespace wjournal
