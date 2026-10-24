// Stage slots: path_to_hash detour. See stage_redirect.hpp.

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "stage_log.hpp"
#include "stage_redirect.hpp"

namespace {

wchar_t g_state_path[MAX_PATH];
wchar_t g_debug_path[MAX_PATH];
wchar_t g_perf_path[MAX_PATH];
wchar_t g_hook_off_path[MAX_PATH];
bool g_forced_off = false;                          // watcher thread only
const std::vector<StageVariant>* g_variants = nullptr;
ULONGLONG g_start_tick = 0;

// Nothing is hooked before this much time has passed since the game started: tools that hook
// path_to_hash themselves (REFramework's loose file loader) find it by walking the code from
// via.io.file.exists and give up when the entry is already a jump.
const ULONGLONG HOOK_DELAY_MS = 20000;
const int MAX_PATH_CHARS = 1024;
const long MAX_LOGGED = 5000;

std::atomic<long> g_logged{0};                      // trace / serve lines written (slow path only)

inline wchar_t lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c + 32) : c; }

bool contains_ci(const wchar_t* s, size_t n, const std::wstring& lo) {
    if (lo.empty() || lo.size() > n) return false;
    for (size_t i = 0; i + lo.size() <= n; ++i) {
        size_t k = 0;
        while (k < lo.size() && lower(s[i + k]) == lo[k]) ++k;
        if (k == lo.size()) return true;
    }
    return false;
}

// At most one line per slow-path call until MAX_LOGGED. A plain load first: once the cap is
// reached, a file hashed every frame no longer costs a locked add on a line shared by all threads.
bool may_log() {
    return g_logged.load(std::memory_order_relaxed) < MAX_LOGGED &&
           g_logged.fetch_add(1, std::memory_order_relaxed) < MAX_LOGGED;
}

// ---------------------------------------------------------------------------------------------
// What is served. Published as an immutable block through an atomic pointer; a block is never
// freed (the detour may still be reading it), a few KB per change of selection.
//
// The engine hashes every path it opens or looks up, on many threads, so the detour must cost
// next to nothing for the paths it does not serve (all of them during a fight). `filter` is a
// 4096-bit set of the low 12 bits of each served vanilla hash (murmur3 output, evenly spread):
// with a few dozen redirected files about 99% of the calls end on one load and one bit test.
// A trace sets every bit, so every path then goes through the slow path.
// ---------------------------------------------------------------------------------------------

struct Active {
    uint64_t filter[64] = {};                               // bit (h & 4095) set: h may be served
    std::vector<std::pair<uint64_t, uint64_t>> map;         // vanilla hash -> copy's hash, sorted
    std::vector<std::wstring> traces;                       // lower case
};

inline bool filter_hit(const Active* a, uint64_t h) { return (a->filter[(h >> 6) & 63] >> (h & 63)) & 1; }

using PathToHashFn = uint64_t (*)(const wchar_t* path);

// Everything a call of the detour reads, alone in one cache line: read on every call by every
// thread, written only by the watcher (hook installed, selection changed, perf switch).
struct alignas(64) Hot {
    PathToHashFn original = nullptr;                        // set once, before the entry is patched
    std::atomic<const Active*> active{nullptr};
    std::atomic<bool> perf{false};
};
Hot g_hot;

std::vector<std::unique_ptr<Active>> g_all_active;  // watcher thread only

// ---------------------------------------------------------------------------------------------
// Optional measurements. reframework\data\SF6_StageSlots_Data\perf_on.txt present: every 5 s the
// watcher logs what the detour saw (calls per second, threads, served) and sampled cycles (one call
// in 1024 per thread, __rdtsc around the original and around our own work). Absent: one predictable
// branch on the line the detour reads anyway. Counters live in one block per thread (its own cache
// line, plain stores by that thread only, no locked instruction); the watcher only reads them.
// ---------------------------------------------------------------------------------------------

struct alignas(64) PerfBlock {
    std::atomic<uint64_t> calls{0}, served{0}, samples{0}, cyc_orig{0}, cyc_ours{0};   // owning thread
    DWORD tid = 0;
    PerfBlock* next = nullptr;
    alignas(64) uint64_t seen_calls = 0, seen_served = 0, seen_samples = 0, seen_orig = 0, seen_ours = 0;  // watcher
};

inline void bump(std::atomic<uint64_t>& c, uint64_t d) {
    c.store(c.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
}

const int MAX_PERF_BLOCKS = 256;                    // engine threads are a fixed pool; past that, one shared block
const uint64_t SAMPLE_MASK = 1023;
std::atomic<PerfBlock*> g_perf_list{nullptr};
std::atomic<int> g_perf_blocks{0};
PerfBlock g_perf_shared;                            // overflow: counted approximately, never sampled
thread_local PerfBlock* t_perf = nullptr;

__declspec(noinline) PerfBlock* perf_register() {
    PerfBlock* b = nullptr;
    if (g_perf_blocks.fetch_add(1) < MAX_PERF_BLOCKS) b = new (std::nothrow) PerfBlock();
    if (b) {
        b->tid = GetCurrentThreadId();
        PerfBlock* head = g_perf_list.load();
        do { b->next = head; } while (!g_perf_list.compare_exchange_weak(head, b));
    } else {
        b = &g_perf_shared;
    }
    t_perf = b;
    return b;
}

// ---------------------------------------------------------------------------------------------
// The detour
// ---------------------------------------------------------------------------------------------

// Paths that pass the filter (every path while a trace is set): out of line, so the hot function
// keeps a small frame (no security cookie, few saved registers).
__declspec(noinline) uint64_t detour_slow(const Active* a, const wchar_t* path, uint64_t h, PerfBlock* perf) {
    if (!a->traces.empty() && path) {
        size_t n = wcsnlen(path, MAX_PATH_CHARS);
        for (const auto& t : a->traces) {
            if (contains_ci(path, n, t)) {
                if (may_log()) slog("trace %s", narrow(path).c_str());
                break;
            }
        }
    }
    auto it = std::lower_bound(a->map.begin(), a->map.end(), h,
                               [](const std::pair<uint64_t, uint64_t>& e, uint64_t v) { return e.first < v; });
    if (it == a->map.end() || it->first != h) return h;
    if (perf && perf != &g_perf_shared) bump(perf->served, 1);
    if (path && may_log()) slog("serve %s", narrow(path).c_str());
    return it->second;
}

inline uint64_t finish(uint64_t h, const wchar_t* path, PerfBlock* perf) {
    const Active* a = g_hot.active.load(std::memory_order_acquire);
    if (a == nullptr || !filter_hit(a, h)) return h;
    return detour_slow(a, path, h, perf);
}

__declspec(noinline) uint64_t detour_perf(const wchar_t* path) {
    PerfBlock* b = t_perf;
    if (b == nullptr) b = perf_register();
    if (b == &g_perf_shared) {
        g_perf_shared.calls.fetch_add(1, std::memory_order_relaxed);
        return finish(g_hot.original(path), path, b);
    }
    const uint64_t n = b->calls.load(std::memory_order_relaxed) + 1;
    b->calls.store(n, std::memory_order_relaxed);
    if ((n & SAMPLE_MASK) != 0) return finish(g_hot.original(path), path, b);
    const uint64_t t0 = __rdtsc();
    const uint64_t h = g_hot.original(path);
    const uint64_t t1 = __rdtsc();
    const uint64_t r = finish(h, path, b);
    const uint64_t t2 = __rdtsc();
    bump(b->samples, 1);
    bump(b->cyc_orig, t1 - t0);
    bump(b->cyc_ours, t2 - t1);
    return r;
}

uint64_t detour(const wchar_t* path) {
    if (g_hot.perf.load(std::memory_order_relaxed)) return detour_perf(path);
    const uint64_t h = g_hot.original(path);
    const Active* a = g_hot.active.load(std::memory_order_acquire);
    if (a == nullptr || !filter_hit(a, h)) return h;
    return detour_slow(a, path, h, nullptr);
}

// ---------------------------------------------------------------------------------------------
// Finding path_to_hash in the game executable.
//
// Its end is unique and stable: two murmur3 calls (seed 0xFFFFFFFF, upper then lower case
// copy of the path) whose results are packed as (upper << 32) | lower:
//     41 B8 FF FF FF FF       mov r8d, 0xFFFFFFFF
//     4C 8D 4C 24 24          lea r9, [rsp+24h]
//     48 8B CB                mov rcx, rbx
//     E8 ?? ?? ?? ??          call murmur3
//     8B 44 24 20             mov eax, [rsp+20h]
//     8B 4C 24 24             mov ecx, [rsp+24h]
//     48 C1 E0 20             shl rax, 20h
//     48 0B C1                or rax, rcx
// The entry is found walking back to the int3 padding. It starts with
//     40 55 53 41 56          push rbp / push rbx / push r14
// or with a 5-byte jump when another tool already hooked it, then
//     48 8D AC 24             lea rbp, [rsp-...]
// ---------------------------------------------------------------------------------------------

const int16_t TAIL[] = {
    0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x4C, 0x24, 0x24, 0x48, 0x8B, 0xCB,
    0xE8, -1, -1, -1, -1, 0x8B, 0x44, 0x24, 0x20, 0x8B, 0x4C, 0x24, 0x24,
    0x48, 0xC1, 0xE0, 0x20, 0x48, 0x0B, 0xC1,
};
const size_t TAIL_LEN = sizeof(TAIL) / sizeof(TAIL[0]);

bool tail_at(const uint8_t* p) {
    for (size_t k = 0; k < TAIL_LEN; ++k)
        if (TAIL[k] >= 0 && p[k] != (uint8_t)TAIL[k]) return false;
    return true;
}

bool entry_at(const uint8_t* p) {
    if (((uintptr_t)p & 0xF) != 0 || p[-1] != 0xCC) return false;
    bool original = p[0] == 0x40 && p[1] == 0x55 && p[2] == 0x53 && p[3] == 0x41 && p[4] == 0x56;
    bool jumped = p[0] == 0xE9;
    return (original || jumped) && p[5] == 0x48 && p[6] == 0x8D && p[7] == 0xAC && p[8] == 0x24;
}

// murmur3's body constant 0xCC9E2D51 somewhere in the first bytes of the called function
bool is_murmur(const uint8_t* fn) {
    for (int k = 0; k < 0x100; ++k)
        if (fn[k] == 0x51 && fn[k + 1] == 0x2D && fn[k + 2] == 0x9E && fn[k + 3] == 0xCC) return true;
    return false;
}

struct ScanResult { uintptr_t entry; int tails; int entries; };

// SEH only, no C++ objects: the executable is protected and some pages may not be readable.
void scan_section(const uint8_t* begin, size_t size, ScanResult* res) {
    __try {
        const uint8_t* end = begin + size - TAIL_LEN;
        for (const uint8_t* p = begin; p < end; ++p) {
            p = (const uint8_t*)memchr(p, 0x41, end - p);
            if (!p) break;
            if (!tail_at(p)) continue;
            res->tails++;
            int32_t rel;
            memcpy(&rel, p + 15, 4);
            const uint8_t* callee = p + 19 + rel;
            if (!is_murmur(callee)) continue;
            for (int back = 0x100; back <= 0x1000; ++back) {
                const uint8_t* e = p - back;
                if (entry_at(e)) { res->entries++; res->entry = (uintptr_t)e; break; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uintptr_t find_path_to_hash() {
    auto base = (const uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    ScanResult res{0, 0, 0};
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        size_t size = sec[i].Misc.VirtualSize;
        if (size < 0x1000) continue;
        scan_section(base + sec[i].VirtualAddress, size, &res);
    }
    slog("scan: %d tail(s), %d entr%s", res.tails, res.entries, res.entries == 1 ? "y" : "ies");
    if (res.entries != 1) return 0;
    slog("path_to_hash at exe+0x%llx (first byte %02X)", (unsigned long long)(res.entry - (uintptr_t)base),
         *(const uint8_t*)res.entry);
    return res.entry;
}

// ---------------------------------------------------------------------------------------------
// The hook itself. The entry gets a 5-byte relative jump to a relay, so the relay must sit
// within 2 GB of the executable; late in a session that space is taken (the executable alone
// is 600 MB), hence a small block reserved at startup, from DllMain.
//   relay:      jmp [rip+0] -> detour
//   trampoline: what the entry did: the jump of the tool that hooked it first (absolute), or
//               the original 5 bytes (push rbp / push rbx / push r14) then back to entry+5.
// The entry is 16-byte aligned: the 5 bytes are replaced by one atomic 8-byte exchange.
// ---------------------------------------------------------------------------------------------

uint8_t* g_near = nullptr;

void reserve_near_block() {
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uintptr_t image_end = base + nt->OptionalHeader.SizeOfImage;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t reach = 0x60000000;
    const uintptr_t lowest = (uintptr_t)si.lpMinimumApplicationAddress;
    // below the image, walking down
    for (uintptr_t a = (base - gran) & ~(gran - 1); a > lowest && base - a < reach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((LPVOID)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { g_near = (uint8_t*)p; return; }
            a -= gran;
        } else {
            uintptr_t ab = (uintptr_t)mbi.AllocationBase;
            if (ab < gran) break;
            a = (ab - gran) & ~(gran - 1);
        }
    }
    // above the image, walking up
    for (uintptr_t a = (image_end + gran - 1) & ~(gran - 1); a - image_end < reach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((LPVOID)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { g_near = (uint8_t*)p; return; }
            a += gran;
        } else {
            a = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize + gran - 1) & ~(gran - 1);
        }
    }
}

void write_abs_jmp(uint8_t* at, uintptr_t dest) {
    at[0] = 0xFF; at[1] = 0x25;                 // jmp qword ptr [rip+0]
    at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &dest, 8);
}

// The entry as we found it and as we patch it: the hook is armed while a variant is selected and
// disarmed (original bytes back) otherwise, each time one atomic exchange that only succeeds when
// the entry still holds what we expect. Another tool that patched the entry after us keeps us
// armed for the session (its jump may lead to our relay).
uint8_t* g_entry = nullptr;
uint64_t g_entry_before = 0, g_entry_after = 0;
bool g_armed = false;
bool g_pinned = false;          // the entry stopped matching: leave it alone from now on

bool exchange_entry(uint64_t expected, uint64_t desired) {
    DWORD old = 0;
    if (!VirtualProtect(g_entry, 8, PAGE_EXECUTE_READWRITE, &old)) { slog("VirtualProtect failed (%lu)", GetLastError()); return false; }
    bool ok = InterlockedCompareExchange64((volatile LONG64*)g_entry, (LONG64)desired, (LONG64)expected) == (LONG64)expected;
    VirtualProtect(g_entry, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_entry, 8);
    return ok;
}

// Watcher thread only. Threads already in the relay, trampoline or detour finish normally: none of
// that memory, nor any published selection block, is ever released.
void set_armed(bool on) {
    if (!g_entry || g_pinned || g_armed == on) return;
    if (exchange_entry(on ? g_entry_before : g_entry_after, on ? g_entry_after : g_entry_before)) {
        g_armed = on;
        slog("hook %s", on ? "armed" : "disarmed: path_to_hash runs untouched");
        return;
    }
    g_pinned = true;
    slog("hook: entry of path_to_hash changed by another tool, %s for the rest of the session",
         g_armed ? "left armed" : "left unhooked (variants not served)");
}

bool install_hook() {
    uintptr_t target = find_path_to_hash();
    if (!target) { slog("path_to_hash not found: no redirection this session"); return false; }
    if (!g_near) { slog("no memory block near the executable: no redirection this session"); return false; }
    auto entry = (uint8_t*)target;

    uint64_t before = *(volatile uint64_t*)entry;
    uint8_t* relay = g_near;
    uint8_t* tramp = g_near + 32;
    write_abs_jmp(relay, (uintptr_t)&detour);
    if (entry[0] == 0xE9) {
        int32_t rel;
        memcpy(&rel, entry + 1, 4);
        uintptr_t dest = target + 5 + (intptr_t)rel;
        write_abs_jmp(tramp, dest);
        slog("entry already hooked (jump to 0x%llx): chained", (unsigned long long)dest);
    } else if (entry[0] == 0x40 && entry[1] == 0x55 && entry[2] == 0x53 && entry[3] == 0x41 && entry[4] == 0x56) {
        memcpy(tramp, entry, 5);
        write_abs_jmp(tramp + 5, target + 5);
    } else {
        slog("unexpected entry bytes %02X %02X %02X %02X %02X", entry[0], entry[1], entry[2], entry[3], entry[4]);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_near, 64);
    g_hot.original = (PathToHashFn)tramp;

    intptr_t rel = (intptr_t)relay - (intptr_t)(target + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { slog("relay out of reach"); return false; }
    uint64_t after = (before & 0xFFFFFF0000000000ull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t)rel << 8);

    g_entry = entry;
    g_entry_before = before;
    g_entry_after = after;
    if (!exchange_entry(before, after)) { g_entry = nullptr; slog("entry changed while hooking: not hooked"); return false; }
    g_armed = true;
    slog("hook installed (relay at 0x%llx)", (unsigned long long)(uintptr_t)relay);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Selection watcher
// ---------------------------------------------------------------------------------------------

bool read_text(const wchar_t* path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// "selected": { "<id>": "<key>", ... } -> id/key pairs. Tolerant scan, no JSON library.
std::vector<std::pair<uint32_t, std::string>> parse_state(const std::string& js) {
    std::vector<std::pair<uint32_t, std::string>> out;
    size_t sel = js.find("\"selected\"");
    if (sel == std::string::npos) return out;
    size_t open = js.find('{', sel), close = js.find('}', sel);
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    size_t i = open + 1;
    while (i < close) {
        size_t q1 = js.find('"', i);
        if (q1 == std::string::npos || q1 >= close) break;
        size_t q2 = js.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        size_t colon = js.find(':', q2);
        if (colon == std::string::npos) break;
        size_t v1 = js.find('"', colon);
        if (v1 == std::string::npos) break;
        size_t v2 = js.find('"', v1 + 1);
        if (v2 == std::string::npos || v2 > close) break;
        std::string id = js.substr(q1 + 1, q2 - q1 - 1), key = js.substr(v1 + 1, v2 - v1 - 1);
        if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos && !key.empty())
            out.push_back({ (uint32_t)strtoul(id.c_str(), nullptr, 10), key });
        i = v2 + 1;
    }
    return out;
}

std::vector<std::wstring> read_traces() {
    std::vector<std::wstring> out;
    std::string text;
    if (!read_text(g_debug_path, text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 6, "trace=") == 0 && line.size() > 6) {
            std::wstring w = widen(line.substr(6));
            for (auto& c : w) c = lower(c);
            out.push_back(w);
        }
    }
    return out;
}

void publish(const std::string& state, const std::vector<std::wstring>& traces) {
    auto a = std::make_unique<Active>();
    a->traces = traces;
    for (auto& [id, key] : parse_state(state)) {
        const StageVariant* v = nullptr;
        for (auto& x : *g_variants) if (x.key == key && x.stage_id == id) { v = &x; break; }
        if (!v) { slog("selection: stage %u -> %s (unknown variant, vanilla)", id, key.c_str()); continue; }
        a->map.insert(a->map.end(), v->redirects.begin(), v->redirects.end());
        slog("selection: stage %u -> %s \"%s\" (%zu files)", id, key.c_str(), v->name.c_str(), v->redirects.size());
    }
    if (a->map.empty() && a->traces.empty()) {
        g_hot.active.store(nullptr, std::memory_order_release);
        slog("selection: vanilla everywhere");
        return;
    }
    // sorted by vanilla hash, one entry per hash (the last selected stage's wins, as the map did)
    std::stable_sort(a->map.begin(), a->map.end(),
                     [](const std::pair<uint64_t, uint64_t>& x, const std::pair<uint64_t, uint64_t>& y) { return x.first < y.first; });
    std::vector<std::pair<uint64_t, uint64_t>> uniq;
    for (auto& e : a->map) {
        if (!uniq.empty() && uniq.back().first == e.first) uniq.back() = e;
        else uniq.push_back(e);
    }
    a->map.swap(uniq);
    for (auto& e : a->map) a->filter[(e.first >> 6) & 63] |= 1ull << (e.first & 63);
    if (!a->traces.empty()) memset(a->filter, 0xFF, sizeof(a->filter));
    g_hot.active.store(a.get(), std::memory_order_release);
    g_all_active.push_back(std::move(a));
}

// ---------------------------------------------------------------------------------------------
// Perf report (watcher thread). One line every PERF_REPORT_MS while perf_on.txt exists:
//   perf: 5.0 s armed, 12345 calls/s on 6 thread(s) [tid:calls/s ...], served 0/s,
//         original 2100 cyc, ours 30 cyc (412 samples) -> original 2.3 ms/s, ours 0.03 ms/s, TSC 2.90 GHz
// ---------------------------------------------------------------------------------------------

const DWORD PERF_REPORT_MS = 5000;
bool g_perf_last = false;
ULONGLONG g_perf_next = 0;
LARGE_INTEGER g_perf_qpc{};
uint64_t g_perf_tsc = 0;

void perf_snapshot_reset() {
    for (PerfBlock* b = g_perf_list.load(); b; b = b->next) {
        b->seen_calls = b->calls.load(std::memory_order_relaxed);
        b->seen_served = b->served.load(std::memory_order_relaxed);
        b->seen_samples = b->samples.load(std::memory_order_relaxed);
        b->seen_orig = b->cyc_orig.load(std::memory_order_relaxed);
        b->seen_ours = b->cyc_ours.load(std::memory_order_relaxed);
    }
    g_perf_shared.seen_calls = g_perf_shared.calls.load(std::memory_order_relaxed);
    QueryPerformanceCounter(&g_perf_qpc);
    g_perf_tsc = __rdtsc();
}

void perf_report() {
    LARGE_INTEGER qpc, freq;
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    const uint64_t tsc = __rdtsc();
    const double secs = double(qpc.QuadPart - g_perf_qpc.QuadPart) / double(freq.QuadPart);
    if (secs <= 0.0) return;
    const double tsc_hz = double(tsc - g_perf_tsc) / secs;
    uint64_t calls = 0, served = 0, samples = 0, orig = 0, ours = 0;
    int threads = 0;
    struct Top { DWORD tid; uint64_t calls; } top[4] = {};
    for (PerfBlock* b = g_perf_list.load(); b; b = b->next) {
        const uint64_t c = b->calls.load(std::memory_order_relaxed), s = b->served.load(std::memory_order_relaxed);
        const uint64_t n = b->samples.load(std::memory_order_relaxed);
        const uint64_t o = b->cyc_orig.load(std::memory_order_relaxed), u = b->cyc_ours.load(std::memory_order_relaxed);
        const uint64_t dc = c - b->seen_calls;
        calls += dc; served += s - b->seen_served; samples += n - b->seen_samples;
        orig += o - b->seen_orig; ours += u - b->seen_ours;
        b->seen_calls = c; b->seen_served = s; b->seen_samples = n; b->seen_orig = o; b->seen_ours = u;
        if (dc == 0) continue;
        ++threads;
        for (int i = 0; i < 4; ++i) {
            if (dc > top[i].calls) {
                for (int k = 3; k > i; --k) top[k] = top[k - 1];
                top[i] = { b->tid, dc };
                break;
            }
        }
    }
    const uint64_t shared_now = g_perf_shared.calls.load(std::memory_order_relaxed);
    const uint64_t shared = shared_now - g_perf_shared.seen_calls;
    g_perf_shared.seen_calls = shared_now;
    calls += shared;
    g_perf_qpc = qpc;
    g_perf_tsc = tsc;

    char tops[160] = "";
    size_t len = 0;
    for (int i = 0; i < 4 && top[i].calls; ++i)
        len += snprintf(tops + len, sizeof(tops) - len, "%s%lu:%.0f", i ? " " : "", top[i].tid, double(top[i].calls) / secs);
    const double avg_orig = samples ? double(orig) / double(samples) : 0.0;
    const double avg_ours = samples ? double(ours) / double(samples) : 0.0;
    const double cps = double(calls) / secs;
    const double ms_orig = tsc_hz > 0 ? cps * avg_orig / tsc_hz * 1000.0 : 0.0;
    const double ms_ours = tsc_hz > 0 ? cps * avg_ours / tsc_hz * 1000.0 : 0.0;
    slog("perf: %.1f s %s, %.0f calls/s on %d thread(s) [%s]%s, served %.0f/s, original %.0f cyc, ours %.0f cyc "
         "(%llu samples) -> original %.2f ms/s, ours %.3f ms/s, TSC %.2f GHz",
         secs, g_entry ? (g_armed ? "armed" : "disarmed") : "not hooked", cps, threads, tops, shared ? " +overflow" : "", double(served) / secs,
         avg_orig, avg_ours, (unsigned long long)samples, ms_orig, ms_ours, tsc_hz / 1e9);
}

// perf_on.txt checked at each wake of the watcher (its folder's change notification fires when
// the file appears or goes). Returns how long the watcher may sleep before the next report.
DWORD perf_poll() {
    const bool on = GetFileAttributesW(g_perf_path) != INVALID_FILE_ATTRIBUTES;
    const ULONGLONG now = GetTickCount64();
    if (on != g_perf_last) {
        g_perf_last = on;
        if (on) perf_snapshot_reset();
        g_hot.perf.store(on, std::memory_order_relaxed);
        g_perf_next = now + PERF_REPORT_MS;
        slog("perf: measurements %s (hook %s)", on ? "on, a line every 5 s" : "off",
             g_entry ? (g_armed ? "armed" : "disarmed") : "not installed");
    }
    if (!on) return INFINITE;
    if (now >= g_perf_next) {
        perf_report();
        g_perf_next = now + PERF_REPORT_MS;
    }
    return static_cast<DWORD>(g_perf_next - now);
}

bool mtime_of(const wchar_t* path, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return false;
    ft = fa.ftLastWriteTime;
    return true;
}

// state.json read whole: false while it cannot be opened (the plugin replaces it by a rename, a scanner may hold it
// a moment) or holds no complete object. Read then, an empty text published "vanilla everywhere" and the file's
// time, already recorded, kept it from being read again: the battle loaded the original stage (07/10).
bool read_state(std::string& out) {
    out.clear();
    if (!read_text(g_state_path, out)) return false;
    int depth = 0;
    bool any = false;
    for (char c : out) {
        if (c == '{') { ++depth; any = true; }
        else if (c == '}') --depth;
    }
    return any && depth == 0;
}

const DWORD STATE_RETRY_MS = 50;
const int STATE_RETRIES = 40;                          // 2 s, then the previous selection stays

// Sleeps until a file of SF6_StageSlots_Data changes (a change notification, no polling), or
// until the hook delay ends when a variant is selected and nothing is hooked yet.
DWORD WINAPI watcher(LPVOID) {
    FILETIME st_last{}, dbg_last{};
    bool st_seen = false, dbg_seen = false, first = true, hooked = false, gave_up = false;
    int retries = 0;
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, g_state_path);
    if (wchar_t* sl = wcsrchr(dir, L'\\')) *sl = 0;
    HANDLE change = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (change == INVALID_HANDLE_VALUE) slog("no change notification on %s: checked every second", narrow(dir).c_str());
    for (;;) {
        FILETIME st_ft{}, dbg_ft{};
        bool st_now = mtime_of(g_state_path, st_ft), dbg_now = mtime_of(g_debug_path, dbg_ft);
        bool changed = first || st_now != st_seen || dbg_now != dbg_seen ||
                       (st_now && CompareFileTime(&st_ft, &st_last) != 0) ||
                       (dbg_now && CompareFileTime(&dbg_ft, &dbg_last) != 0);
        bool retry = false;
        if (changed) {
            std::string state;
            const bool whole = !st_now || read_state(state);
            if (!whole && retries < STATE_RETRIES) {
                ++retries;                             // the time is not recorded: read again shortly
                retry = true;
            } else {
                first = false;
                st_seen = st_now; st_last = st_ft;
                dbg_seen = dbg_now; dbg_last = dbg_ft;
                if (whole) publish(st_now ? state : std::string(), read_traces());
                else slog("selection: state.json unreadable for %d ms, previous selection kept", retries * (int)STATE_RETRY_MS);
                if (whole && retries) slog("selection: state.json read after %d retries", retries);
                retries = 0;
            }
        }
        DWORD wait = INFINITE;
        const bool selected = g_hot.active.load() != nullptr;
        if (!hooked && !gave_up && selected) {
            ULONGLONG since = GetTickCount64() - g_start_tick;
            if (since >= HOOK_DELAY_MS) { hooked = install_hook(); gave_up = !hooked; }
            else wait = static_cast<DWORD>(HOOK_DELAY_MS - since);
        }
        // hook_off.txt (test switch): the hook stays disarmed while it exists, whatever the selection, so its cost
        // can be compared in one session (files a stage streams later then come from the original stage)
        const bool forced_off = GetFileAttributesW(g_hook_off_path) != INVALID_FILE_ATTRIBUTES;
        if (forced_off != g_forced_off) {
            g_forced_off = forced_off;
            slog("hook_off.txt %s", forced_off ? "present: hook kept disarmed" : "gone: hook follows the selection");
        }
        if (hooked) set_armed(selected && !forced_off);
        const DWORD perf_wait = perf_poll();
        if (perf_wait < wait) wait = perf_wait;
        if (retry) {
            Sleep(STATE_RETRY_MS < wait ? STATE_RETRY_MS : wait);
            continue;
        }
        if (change == INVALID_HANDLE_VALUE) { Sleep(wait == INFINITE ? 1000 : (wait < 1000 ? wait : 1000)); continue; }
        WaitForSingleObject(change, wait);
        FindNextChangeNotification(change);
    }
}

} // namespace

void stage_slots_start(const wchar_t* game_dir, const std::vector<StageVariant>* variants) {
    g_start_tick = GetTickCount64();
    g_variants = variants;
    wcscpy_s(g_state_path, game_dir);
    wcscat_s(g_state_path, L"\\reframework\\data\\SF6_StageSlots_Data\\state.json");
    wcscpy_s(g_debug_path, game_dir);
    wcscat_s(g_debug_path, L"\\reframework\\data\\SF6_StageSlots_Data\\debug.txt");
    wcscpy_s(g_perf_path, game_dir);
    wcscat_s(g_perf_path, L"\\reframework\\data\\SF6_StageSlots_Data\\perf_on.txt");
    wcscpy_s(g_hook_off_path, game_dir);
    wcscat_s(g_hook_off_path, L"\\reframework\\data\\SF6_StageSlots_Data\\hook_off.txt");
    reserve_near_block();
    if (g_near) slog("near block at 0x%llx", (unsigned long long)(uintptr_t)g_near);
    else slog("no free block near the executable: no redirection this session");
    HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else slog("cannot start the watcher thread");
}
