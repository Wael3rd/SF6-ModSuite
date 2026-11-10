// SF6 Slots — the Color Mod's colour entries (RW / MC) and requests: the contract colormod/include/sf6colormod_entries.h,
// resolved with GetModuleHandleW + GetProcAddress (the suite's DLL-to-DLL pattern), looked for again every 10 s while
// missing. Without a Color Mod, or an older one (no SF6ColorMod_EntriesVersion, or its colour engine off), entries() is
// empty and requests do nothing: the Replays > Settings tab then lists the game's own colours only. A Color Mod with the
// Random value (SF6ColorMod_RandomChoice) also says where the player chose Random, and dresses an entry drawn for a battle
// (scopes 3 / 4): costumes.cpp's Random draws.
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
    SF6ColorMod_RandomChoiceFn random_choice = nullptr;   // a Color Mod with the Random value and the battle scopes, else null
    SF6ColorMod_FighterRandomFn fighter_random = nullptr; // one that keeps Fighter Settings' Random per fighter, else null
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
                n.random_choice = reinterpret_cast<SF6ColorMod_RandomChoiceFn>(GetProcAddress(h, "SF6ColorMod_RandomChoice"));
                n.fighter_random = reinterpret_cast<SF6ColorMod_FighterRandomFn>(GetProcAddress(h, "SF6ColorMod_FighterRandom"));
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

// ---- the Random colour (the Color Mod's Random value; costumes.cpp draws) ----
// The Color Mod can dress an entry drawn for a battle (scopes 3 / 4) and says where Random was chosen: a draw may take its
// entries. Without it (no Color Mod, or an older one) Random colours draw among the game's colours only, and none is chosen.
inline bool battle_ok() {
    const Fns f = link();
    return f.random_choice && available();
}
// Side 1 / 2 (the character select's / training popup's P1 / P2) or 3 (Fighter Settings): the fighter and outfit whose
// colour the player set to Random there, if that is the choice
inline bool random_choice(int side, int& fighter, int& costume) {
    const Fns f = link();
    int fi = -1, co = -1;
    if (!f.random_choice || !available() || f.random_choice(side, &fi, &co) != 1) return false;
    fighter = fi;
    costume = co;
    return true;
}
// Battle Settings' Random colour of a fighter (Fighter Settings, kept per fighter): the outfit it was chosen on, -1 none. An
// older Color Mod keeps one, the last fighter shown there.
inline int fighter_random(int fighter) {
    const Fns f = link();
    if (!f.random_choice || !available()) return -1;
    if (f.fighter_random) return f.fighter_random(fighter);
    int fi = -1, co = -1;
    return f.random_choice(3, &fi, &co) == 1 && fi == fighter ? co : -1;
}
// The battle fighter of `side` (1 / 2) wears that entry over colour 02 until replaced or cleared
inline bool request_battle(int side, int fighter, int costume, const std::string& id) {
    return battle_ok() && (side == 1 || side == 2) && request(SF6CM_SCOPE_BATTLE_P1 + side - 1, fighter, costume, id);
}
inline void clear_battle() {
    if (!battle_ok()) return;
    clear(SF6CM_SCOPE_BATTLE_P1);
    clear(SF6CM_SCOPE_BATTLE_P2);
}

// How many entries are ticked for an outfit on Opponent Outfit Settings: 0 with no Color Mod or an older one
inline int opponent_ticks(int fighter, int costume) {
    HMODULE h = GetModuleHandleW(L"SF6_ColorModNative.dll");
    auto* fn = h ? reinterpret_cast<SF6ColorMod_OpponentTicksFn>(GetProcAddress(h, "SF6ColorMod_OpponentTicks")) : nullptr;
    return fn ? fn(fighter, costume) : 0;
}

}  // namespace slots::colormod
