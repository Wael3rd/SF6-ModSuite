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

int query(uint64_t a) {                       // lock held
    MEMORY_BASIC_INFORMATION mbi{};
    ++g_queries;
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    int st = 0;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && mbi.Protect != PAGE_NOACCESS) {
        st = 1;
        const DWORD p = mbi.Protect & 0xFF;
        if (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY) st |= 2;
    }
    const uint64_t base = (uint64_t)mbi.BaseAddress, end = base + (uint64_t)mbi.RegionSize;
    if (end > base && a >= base && a < end) {
        if (g_regions.size() >= kMaxRegions) g_regions.clear();
        g_regions[base] = Region{ end, st };
    }
    return st;
}

}  // namespace

int state(uint64_t a) {
    std::lock_guard<std::mutex> lk(g_mx);
    const uint64_t f = cmn::frame();
    const ULONGLONG now = GetTickCount64();
    if (f != g_frame || now - g_born > kMaxAgeMs) { g_regions.clear(); g_frame = f; g_born = now; }
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

}  // namespace vfx::memguard
