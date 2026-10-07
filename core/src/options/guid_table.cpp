#ifndef NOMINMAX
#define NOMINMAX
#endif
// dialogs::guid_text - the GUID -> text table. See guid_table.hpp. No game header in this file.

#include "guid_table.hpp"

#include <windows.h>
#include <rpc.h>            // UuidCreate
#pragma comment(lib, "rpcrt4.lib")

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace dialogs::guid_text {
namespace {

struct Text { std::string text; uint32_t tree; };

// guid_text[hi][lo] = text, like the Lua: on a miss (nearly every call) the first lookup fails at once.
std::mutex g_mx;
std::unordered_map<uint64_t, std::unordered_map<uint64_t, Text>> g_map;
size_t g_count = 0;
std::atomic<size_t> g_live{0};                 // read without the lock by the hook (nothing registered: no lookup)
uint32_t g_tree = 0;

}  // namespace

GuidBytes alloc(const std::string& text) {
    GuidBytes gb;
    UUID uuid;
    const bool ok = UuidCreate(&uuid) == RPC_S_OK;
    std::lock_guard lk(g_mx);
    if (ok) {
        std::memcpy(&gb.hi, &uuid, 8);
        std::memcpy(&gb.lo, reinterpret_cast<uint8_t*>(&uuid) + 8, 8);
    } else {                                                     // never seen; unique within this table all the same
        gb.hi = 0x5346364f50544e00ULL ^ (uint64_t)g_tree;
        gb.lo = g_count + 1;
    }
    auto& row = g_map[gb.hi];
    if (row.find(gb.lo) == row.end()) ++g_live;
    row[gb.lo] = { text, g_tree };
    ++g_count;
    return gb;
}

bool retext(const GuidBytes& gb, const std::string& text) {
    std::lock_guard lk(g_mx);
    auto it = g_map.find(gb.hi);
    if (it == g_map.end()) return false;
    auto it2 = it->second.find(gb.lo);
    if (it2 == it->second.end()) return false;
    it2->second.text = text;
    return true;
}

bool lookup(uint64_t hi, uint64_t lo, std::string& out) {
    std::lock_guard lk(g_mx);
    auto it = g_map.find(hi);
    if (it == g_map.end()) return false;
    auto it2 = it->second.find(lo);
    if (it2 == it->second.end()) return false;
    out = it2->second.text;
    return true;
}

void begin_tree() { std::lock_guard lk(g_mx); ++g_tree; }

void purge(bool keep_current) {
    std::lock_guard lk(g_mx);
    if (!keep_current) { g_map.clear(); g_live = 0; return; }
    for (auto it = g_map.begin(); it != g_map.end();) {
        for (auto it2 = it->second.begin(); it2 != it->second.end();) {
            if (it2->second.tree != g_tree) { it2 = it->second.erase(it2); --g_live; } else ++it2;
        }
        if (it->second.empty()) it = g_map.erase(it); else ++it;
    }
}

size_t count() { std::lock_guard lk(g_mx); return g_count; }
size_t live() { return g_live.load(std::memory_order_relaxed); }

}  // namespace dialogs::guid_text
