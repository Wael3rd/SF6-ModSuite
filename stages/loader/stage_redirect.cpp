// Stage slots: path_to_hash detour. See stage_redirect.hpp.

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "stage_log.hpp"
#include "stage_redirect.hpp"

namespace {

wchar_t g_state_path[MAX_PATH];
wchar_t g_debug_path[MAX_PATH];
const std::vector<StageVariant>* g_variants = nullptr;
ULONGLONG g_start_tick = 0;

// Nothing is hooked before this much time has passed since the game started: tools that hook
// path_to_hash themselves (REFramework's loose file loader) find it by walking the code from
// via.io.file.exists and give up when the entry is already a jump.
const ULONGLONG HOOK_DELAY_MS = 20000;
const int MAX_PATH_CHARS = 1024;
const long MAX_LOGGED = 5000;

std::atomic<long> g_logged{0};
std::atomic<long long> g_served{0};

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

// ---------------------------------------------------------------------------------------------
// What is served. Published as an immutable block through an atomic pointer; a block is never
// freed (the detour may still be reading it), a few KB per change of selection.
// ---------------------------------------------------------------------------------------------

struct Active {
    std::unordered_map<uint64_t, uint64_t> map;     // vanilla hash -> hash of the variant's copy
    std::vector<std::wstring> traces;               // lower case
};

std::atomic<const Active*> g_active{nullptr};
std::vector<std::unique_ptr<Active>> g_all_active;  // watcher thread only

// ---------------------------------------------------------------------------------------------
// The detour
// ---------------------------------------------------------------------------------------------

using PathToHashFn = uint64_t (*)(const wchar_t* path);
PathToHashFn g_original = nullptr;

uint64_t detour(const wchar_t* path) {
    uint64_t h = g_original(path);
    const Active* a = g_active.load(std::memory_order_acquire);
    if (a == nullptr) return h;
    if (!a->traces.empty() && path) {
        size_t n = wcsnlen(path, MAX_PATH_CHARS);
        for (const auto& t : a->traces) {
            if (contains_ci(path, n, t)) {
                if (g_logged.fetch_add(1) < MAX_LOGGED) slog("trace %s", narrow(path).c_str());
                break;
            }
        }
    }
    auto it = a->map.find(h);
    if (it == a->map.end()) return h;
    g_served.fetch_add(1, std::memory_order_relaxed);
    if (path && g_logged.fetch_add(1) < MAX_LOGGED) slog("serve %s", narrow(path).c_str());
    return it->second;
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
    g_original = (PathToHashFn)tramp;

    intptr_t rel = (intptr_t)relay - (intptr_t)(target + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { slog("relay out of reach"); return false; }
    uint64_t after = (before & 0xFFFFFF0000000000ull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t)rel << 8);

    DWORD old = 0;
    if (!VirtualProtect(entry, 8, PAGE_EXECUTE_READWRITE, &old)) { slog("VirtualProtect failed (%lu)", GetLastError()); return false; }
    bool ok = InterlockedCompareExchange64((volatile LONG64*)entry, (LONG64)after, (LONG64)before) == (LONG64)before;
    VirtualProtect(entry, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, 8);
    if (!ok) { slog("entry changed while hooking: not hooked"); return false; }
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
        for (auto& r : v->redirects) a->map[r.first] = r.second;
        slog("selection: stage %u -> %s \"%s\" (%zu files)", id, key.c_str(), v->name.c_str(), v->redirects.size());
    }
    if (a->map.empty() && a->traces.empty()) {
        g_active.store(nullptr, std::memory_order_release);
        slog("selection: vanilla everywhere");
        return;
    }
    g_active.store(a.get(), std::memory_order_release);
    g_all_active.push_back(std::move(a));
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
        if (!hooked && !gave_up && g_active.load() != nullptr) {
            ULONGLONG since = GetTickCount64() - g_start_tick;
            if (since >= HOOK_DELAY_MS) { hooked = install_hook(); gave_up = !hooked; }
            else wait = static_cast<DWORD>(HOOK_DELAY_MS - since);
        }
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
    reserve_near_block();
    if (g_near) slog("near block at 0x%llx", (unsigned long long)(uintptr_t)g_near);
    else slog("no free block near the executable: no redirection this session");
    HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else slog("cannot start the watcher thread");
}
