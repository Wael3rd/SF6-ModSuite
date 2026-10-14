// owners — the rule that gives a layer to the DLL, pure (no game, no core), so the
// offline benches test it (tests/integ_bench.cpp). common.cpp applies it to settings.json at the start.
//
//  * every layer is "lua" unless settings.json says "native" (a key missing, a file unreadable: "lua");
//  * a MISSING settings.json is written, and used at once, by default_settings(): the six "native" and "standalone":
//    "auto" when no CRSM Lua entry script is installed (standalone.hpp kLuaEntries), as the release package ships it, so
//    a deleted settings.json never leaves the mod inert; the six "lua" when one is (the Lua drives, a hybrid install);
//  * a layer is the DLL's when this build implements it AND settings.json says "native";
//  * replay / reversal / record also need files native (P3);
//  * ui needs the five others native (P4: the DLL's screens drive every layer, the Lua UI is not loaded). On top of this
//    rule the DLL's UI runs only once the Lua loader handed it over (ui_claim, ui/ui.cpp) and the core has the pause-menu
//    service (P5, ui/manager_page.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace owners {

enum : size_t { kCommunity = 0, kFiles, kReplay, kReversal, kRecord, kUi, kCount };   // = cmn::Feature
constexpr std::array<const char*, kCount> kNames = { "community", "files", "replay", "reversal", "record", "ui" };

inline bool needs_files(size_t i) { return i == kReplay || i == kReversal || i == kRecord; }

struct Result {
    std::array<bool, kCount> owns{};
    std::vector<std::string> notes;            // one journal line per layer asked native that stays lua, and why
};

// requested[i]: settings.json says "native"; implemented[i]: this build has the layer.
inline Result resolve(const std::array<bool, kCount>& requested, const std::array<bool, kCount>& implemented) {
    Result r;
    const bool files_on = requested[kFiles] && implemented[kFiles];
    for (size_t i = 0; i < kCount; ++i) {
        bool on = requested[i] && implemented[i];
        if (on && needs_files(i) && !files_on) {
            r.notes.push_back(std::string("settings ask ") + kNames[i] + "=native without files=native: stays lua");
            on = false;
        }
        r.owns[i] = on;
    }
    if (r.owns[kUi]) {                         // ui (P4) drives every layer from the DLL's own screens
        for (size_t i = 0; i < kUi; ++i) {
            if (r.owns[i]) continue;
            r.notes.push_back(std::string("settings ask ui=native without ") + kNames[i] + "=native: ui stays lua");
            r.owns[kUi] = false;
            break;
        }
    }
    return r;
}

// The owners a missing settings.json gets (requested[i] = "native"): all six without a CRSM Lua entry script installed
// (the DLL alone runs the mod, as the release package's settings.json), none with one (the Lua drives).
inline std::array<bool, kCount> default_requested(bool crsm_lua_installed) {
    std::array<bool, kCount> r{};
    r.fill(!crsm_lua_installed);
    return r;
}

// The settings.json written when it is missing (dump(4) gives the release package's file byte for byte without a Lua).
inline nlohmann::json default_settings(bool crsm_lua_installed) {
    const std::array<bool, kCount> req = default_requested(crsm_lua_installed);
    nlohmann::json own = nlohmann::json::object();
    for (size_t i = 0; i < kCount; ++i) own[kNames[i]] = req[i] ? "native" : "lua";
    nlohmann::json j;
    j["owners"] = own;
    j["standalone"] = "auto";
    return j;
}

}  // namespace owners
