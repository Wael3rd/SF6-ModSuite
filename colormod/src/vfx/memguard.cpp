// vfx::memguard - see memguard.hpp.

#include "memguard.hpp"

#include <windows.h>

#include <map>
#include <mutex>

#include "../common.hpp"

namespace vfx::memguard {
namespace {

constexpr ULONGLONG kMaxAgeMs = 50;
constexpr size_t kMaxRegions = 4096;

struct Region { uint64_t end; int st; };
std::mutex g_mx;                              // the effect hooks may run on other threads than the passes
std::map<uint64_t, Region> g_regions;         // base -> region, valid for g_frame / g_born
uint64_t g_frame = UINT64_MAX;
ULONGLONG g_born = 0;
uint64_t g_queries = 0;

// one VirtualQuery: the state of a's page and its region [base, end) (end = 0: no region to keep)
int query_raw(uint64_t a, uint64_t& base, uint64_t& end) {
    MEMORY_BASIC_INFORMATION mbi{};
    base = end = 0;
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    int st = 0;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && mbi.Protect != PAGE_NOACCESS) {
        st = 1;
        const DWORD p = mbi.Protect & 0xFF;
        if (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY) st |= 2;
    }
    const uint64_t b = (uint64_t)mbi.BaseAddress, e = b + (uint64_t)mbi.RegionSize;
    if (e > b && a >= b && a < e) { base = b; end = e; }
    return st;
}

int query(uint64_t a) {                       // lock held
    ++g_queries;
    uint64_t base, end;
    const int st = query_raw(a, base, end);
    if (end) {
        if (g_regions.size() >= kMaxRegions) g_regions.clear();
        g_regions[base] = Region{ end, st };
    }
    return st;
}

}  // namespace

int state(uint64_t a) {
    std::lock_guard<std::mutex> lk(g_mx);
    const uint64_t f = cmn::frame();
    // fast path: same frame as last check, the cache is valid (skip GetTickCount64)
    if (f != g_frame) {
        const ULONGLONG now = GetTickCount64();
        g_regions.clear(); g_frame = f; g_born = now;
    } else if (g_queries & 63) {
        // only check wall time every 64 queries within the same frame (the 50 ms guard is for loading-thread hooks
        // that run without frame ticks, not for per-frame passes)
    } else {
        const ULONGLONG now = GetTickCount64();
        if (now - g_born > kMaxAgeMs) { g_regions.clear(); g_born = now; }
    }
    auto it = g_regions.upper_bound(a);
    if (it != g_regions.begin()) {
        --it;
        if (a < it->second.end) return it->second.st;
    }
    return query(a);
}

bool readable(uint64_t a, size_t n) {
    if (n == 0) n = 1;
    for (uint64_t p = a & ~0xFFFULL; p < a + n; p += 0x1000) if (!(state(p) & 1)) return false;
    return true;
}

uint64_t queries() { std::lock_guard<std::mutex> lk(g_mx); return g_queries; }

int Local::state(uint64_t a) {
    if ((n_++ & 63) == 0) {                   // wall time looked at every 64 checks, as state() does within a frame
        const ULONGLONG now = GetTickCount64();
        if (now - born_ > kMaxAgeMs) { regions_.clear(); born_ = now; }
    }
    auto it = regions_.upper_bound(a);
    if (it != regions_.begin()) {
        --it;
        if (a < it->second.end) return it->second.st;
    }
    uint64_t base, end;
    const int st = query_raw(a, base, end);
    if (end) {
        if (regions_.size() >= kMaxRegions) regions_.clear();
        regions_[base] = Region{ end, st };
    }
    return st;
}

}  // namespace vfx::memguard
