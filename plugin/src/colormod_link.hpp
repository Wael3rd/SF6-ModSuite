// SF6 Slots — the Color Mod's colour entries (RW / MC) and requests: the contract colormod/include/sf6colormod_entries.h,
// resolved with GetModuleHandleW + GetProcAddress (the suite's DLL-to-DLL pattern), looked for again every 10 s while
// missing. Without a Color Mod, or an older one (no SF6ColorMod_EntriesVersion, or its colour engine off), entries() is
// empty and requests do nothing: the Replays > Settings tab then lists the game's own colours only.
#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../../colormod/include/sf6colormod_entries.h"

namespace slots::colormod {

struct Fns {
    SF6ColorMod_EntriesVersionFn version = nullptr;
    SF6ColorMod_EntriesFn entries = nullptr;
    SF6ColorMod_RequestFn request = nullptr;
    SF6ColorMod_ClearRequestsFn clear = nullptr;
};

inline Fns link() {
    static std::mutex mx;
    static Fns f;
    static DWORD tried_at = 0;
    static bool tried = false;
    std::lock_guard lk(mx);
    if (!f.request) {
        const DWORD now = GetTickCount();
        if (!tried || now - tried_at > 10000) {
            tried = true;
            tried_at = now;
            if (HMODULE h = GetModuleHandleW(L"SF6_ColorModNative.dll")) {
                Fns n;
                n.version = reinterpret_cast<SF6ColorMod_EntriesVersionFn>(GetProcAddress(h, "SF6ColorMod_EntriesVersion"));
                n.entries = reinterpret_cast<SF6ColorMod_EntriesFn>(GetProcAddress(h, "SF6ColorMod_Entries"));
                n.request = reinterpret_cast<SF6ColorMod_RequestFn>(GetProcAddress(h, "SF6ColorMod_Request"));
                n.clear = reinterpret_cast<SF6ColorMod_ClearRequestsFn>(GetProcAddress(h, "SF6ColorMod_ClearRequests"));
                if (n.version && n.entries && n.request && n.clear) f = n;
            }
        }
    }
    return f;
}

// The Color Mod is there, knows this contract and its colour engine runs
inline bool available() {
    const Fns f = link();
    return f.version && f.version() == SF6CM_ENTRIES_VERSION;
}

struct Entry { std::string label, id; };

// A fighter / costume's RW / MC entries, the Color Mod's order and labels (empty without the Color Mod)
inline std::vector<Entry> entries(int fighter, int costume) {
    std::vector<Entry> out;
    const Fns f = link();
    if (!f.entries || !available()) return out;
    const int n = f.entries(fighter, costume, nullptr, 0);
    if (n <= 0) return out;
    std::vector<SF6CMEntry> buf(static_cast<size_t>(n));
    const int got = f.entries(fighter, costume, buf.data(), n);
    for (int i = 0; i < got && i < n; ++i) {
        buf[static_cast<size_t>(i)].label[sizeof(buf[0].label) - 1] = 0;
        buf[static_cast<size_t>(i)].id[sizeof(buf[0].id) - 1] = 0;
        out.push_back({ buf[static_cast<size_t>(i)].label, buf[static_cast<size_t>(i)].id });
    }
    return out;
}

// What one fighter / costume wears (scope SF6CM_SCOPE_PREVIEW / SF6CM_SCOPE_REPLAY): an entry id, "" = the game's own colours
inline bool request(int scope, int fighter, int costume, const std::string& id, void* battle = nullptr) {
    const Fns f = link();
    return f.request && available() && f.request(scope, fighter, costume, id.c_str(), battle) == 1;
}
inline void clear(int scope) {
    const Fns f = link();
    if (f.clear) f.clear(scope);
}

// How many entries are ticked for an outfit on Opponent Outfit Settings: 0 with no Color Mod or an older one
inline int opponent_ticks(int fighter, int costume) {
    HMODULE h = GetModuleHandleW(L"SF6_ColorModNative.dll");
    auto* fn = h ? reinterpret_cast<SF6ColorMod_OpponentTicksFn>(GetProcAddress(h, "SF6ColorMod_OpponentTicks")) : nullptr;
    return fn ? fn(fighter, costume) : 0;
}

}  // namespace slots::colormod
