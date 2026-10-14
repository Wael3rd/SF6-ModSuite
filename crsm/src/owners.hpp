// owners — the rule that gives a layer to the DLL, pure (no game, no core), so the
// offline benches test it (tests/integ_bench.cpp). common.cpp applies it to settings.json at the start.
//
//  * every layer is "lua" by default (a missing settings.json is written with six "lua");
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

}  // namespace owners
