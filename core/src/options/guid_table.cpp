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

struct Text { std::string text; uint32_t tree; Owner owner; };   // owner null = the option engine's

// guid_text[hi][lo] = text, like the Lua: on a miss (nearly every call) the first lookup fails at once.
std::mutex g_mx;
std::unordered_map<uint64_t, std::unordered_map<uint64_t, Text>> g_map;
size_t g_count = 0;
std::atomic<size_t> g_live{0};                 // read without the lock by the hook (nothing registered: no lookup)
uint32_t g_tree = 0;
std::unordered_map<Owner, size_t> g_owned;     // client texts per owner (owners with none are erased)
size_t g_owned_total = 0;
uint64_t g_lookups = 0, g_hits = 0, g_owned_hits = 0;

// Under g_mx.
bool contains_locked(const GuidBytes& gb) {
    auto it = g_map.find(gb.hi);
    return it != g_map.end() && it->second.find(gb.lo) != it->second.end();
}
void owned_dec_locked(Owner owner, size_t n) {
    if (!owner || !n) return;
    auto it = g_owned.find(owner);
    if (it == g_owned.end()) return;
    it->second = it->second > n ? it->second - n : 0;
    if (!it->second) g_owned.erase(it);
    g_owned_total = g_owned_total > n ? g_owned_total - n : 0;
}

// A fresh GUID mapped to the text. `keep` (client texts) is checked under the lock; false = nothing stored.
bool insert(const std::string& text, Owner owner, bool (*keep)(Owner), GuidBytes& gb, size_t* owned_after) {
    for (int tries = 0;; ++tries) {
        UUID uuid;
        const bool ok = UuidCreate(&uuid) == RPC_S_OK;
        std::lock_guard lk(g_mx);
        if (keep && !keep(owner)) return false;
        if (ok) {
            std::memcpy(&gb.hi, &uuid, 8);
            std::memcpy(&gb.lo, reinterpret_cast<uint8_t*>(&uuid) + 8, 8);
        } else {                                                 // never seen; unique within this table all the same
            gb.hi = 0x5346364f50544e00ULL ^ (uint64_t)g_tree;
            gb.lo = g_count + 1;
        }
        // A GUID already in the table (never seen) is drawn again: one GUID never carries two texts (R6).
        if (tries < 3 && contains_locked(gb)) continue;
        auto& row = g_map[gb.hi];
        auto it = row.find(gb.lo);
        if (it == row.end()) { row.emplace(gb.lo, Text{ text, g_tree, owner }); ++g_live; }
        else { owned_dec_locked(it->second.owner, 1); it->second = Text{ text, g_tree, owner }; }
        if (owner) { ++g_owned[owner]; ++g_owned_total; }
        if (owned_after) *owned_after = owner ? g_owned[owner] : 0;
        ++g_count;
        return true;
    }
}

// Under g_mx: the entry of gb when it belongs to `owner` (null = the option engine), else nullptr.
Text* find_locked(const GuidBytes& gb, Owner owner) {
    auto it = g_map.find(gb.hi);
    if (it == g_map.end()) return nullptr;
    auto it2 = it->second.find(gb.lo);
    if (it2 == it->second.end() || it2->second.owner != owner) return nullptr;
    return &it2->second;
}

}  // namespace

GuidBytes alloc(const std::string& text) {
    GuidBytes gb;
    insert(text, nullptr, nullptr, gb, nullptr);
    return gb;
}

bool retext(const GuidBytes& gb, const std::string& text) {
    std::lock_guard lk(g_mx);
    Text* t = find_locked(gb, nullptr);              // never a client's text
    if (!t) return false;
    t->text = text;
    return true;
}

bool lookup(uint64_t hi, uint64_t lo, std::string& out) {
    std::lock_guard lk(g_mx);
    ++g_lookups;
    auto it = g_map.find(hi);
    if (it == g_map.end()) return false;
    auto it2 = it->second.find(lo);
    if (it2 == it->second.end()) return false;
    out = it2->second.text;
    ++g_hits;
    if (it2->second.owner) ++g_owned_hits;
    return true;
}

void begin_tree() { std::lock_guard lk(g_mx); ++g_tree; }

void purge(bool keep_current) {
    std::lock_guard lk(g_mx);
    if (!keep_current && !g_owned_total) { g_map.clear(); g_live = 0; return; }
    for (auto it = g_map.begin(); it != g_map.end();) {
        for (auto it2 = it->second.begin(); it2 != it->second.end();) {
            const Text& t = it2->second;
            if (!t.owner && (!keep_current || t.tree != g_tree)) { it2 = it->second.erase(it2); --g_live; } else ++it2;
        }
        if (it->second.empty()) it = g_map.erase(it); else ++it;
    }
}

size_t count() { std::lock_guard lk(g_mx); return g_count; }
size_t live() { return g_live.load(std::memory_order_relaxed); }

// ---- client texts (increment 8) -----------------------------------------------------------------------------------

bool alloc_owned(Owner owner, const std::string& text, bool (*keep)(Owner), GuidBytes& out, size_t* owned_after) {
    if (!owner) return false;
    return insert(text, owner, keep, out, owned_after);
}

bool retext_owned(Owner owner, const GuidBytes& gb, const std::string& text) {
    if (!owner) return false;
    std::lock_guard lk(g_mx);
    Text* t = find_locked(gb, owner);
    if (!t) return false;
    if (t->text != text) t->text = text;
    return true;
}

bool release_owned(Owner owner, const GuidBytes& gb) {
    if (!owner) return false;
    std::lock_guard lk(g_mx);
    auto it = g_map.find(gb.hi);
    if (it == g_map.end()) return false;
    auto it2 = it->second.find(gb.lo);
    if (it2 == it->second.end() || it2->second.owner != owner) return false;
    it->second.erase(it2);
    if (it->second.empty()) g_map.erase(it);
    --g_live;
    owned_dec_locked(owner, 1);
    return true;
}

size_t release_owner(Owner owner) {
    if (!owner) return 0;
    std::lock_guard lk(g_mx);
    if (g_owned.find(owner) == g_owned.end()) return 0;
    size_t n = 0;
    for (auto it = g_map.begin(); it != g_map.end();) {
        for (auto it2 = it->second.begin(); it2 != it->second.end();) {
            if (it2->second.owner == owner) { it2 = it->second.erase(it2); --g_live; ++n; } else ++it2;
        }
        if (it->second.empty()) it = g_map.erase(it); else ++it;
    }
    owned_dec_locked(owner, n);
    g_owned.erase(owner);
    return n;
}

Stats stats() {
    std::lock_guard lk(g_mx);
    Stats s;
    s.live = g_live.load(std::memory_order_relaxed);
    s.owned = g_owned_total;
    s.lookups = g_lookups;
    s.hits = g_hits;
    s.owned_hits = g_owned_hits;
    return s;
}

void owners(std::vector<std::pair<Owner, size_t>>& out) {
    std::lock_guard lk(g_mx);
    out.assign(g_owned.begin(), g_owned.end());
}

}  // namespace dialogs::guid_text
