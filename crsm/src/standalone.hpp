// standalone — the DLL's UI without any CRSM Lua (phase P6), pure (no game,
// no core), so the offline bench tests it (tests/integ_bench.cpp). ui/ui.cpp applies it on the frame callback.
//
// With every owner native (settings.json), the DLL's screens start when the Lua loader hands the UI over (ui_claim, at the
// loader's load). The Native package ships no Lua at all: nobody would ask. settings.json "standalone":
//   "auto" (default, also when the key is missing): the DLL claims the UI itself, once, when ALL of these hold:
//     * the owner ui is the DLL's (the five layers native, the core's pause-menu service present) and not claimed yet;
//     * no CRSM Lua entry script (kLuaEntries) was found in reframework/autorun at the plugin's start, and no Lua ever
//       called the DLL's bridge (ui_claim, frame_poll, owns): a CRSM Lua present asks for itself: one engine, never two;
//     * the pause menu and every option window closed: the UI's hooks start between two menu opens, never inside one.
//   So without a CRSM Lua (every install of the native package: it ships no Lua) the claim comes at the first frame
//   callback, at the title screen, like the Lua loader's claim at its own load: the Manager page and the hooks are in
//   place before the first training (until 2.1.2 it waited kGraceFrames, then kTrainingFrames in training: 2 to 3 s of
//   training without the Recording Manager's pages).
//   "off": never (with no Lua the game's own Record / Replay pages stay, the layers have no screen). Anything else: off,
//   with one journal line.
//
// A CRSM Lua entry script present (v1.x, the SF6 Training Suite's copy, the REFramework display switch) is given time to
// call the DLL: kGraceFrames frame callbacks since the first one (600 = 10 s at 60 fps: REFramework runs the autorun scripts
// within the first frames) and kTrainingFrames in a row in training (120 = 2 s; leaving training starts the count
// again). One that still never called the DLL then is a Lua engine that does not drive it: the DLL gives its hooked
// layers back for the session (Decision::Foreign), so only one engine runs the game's hooks. Only in "auto"; "off" keeps
// the behaviour of 0.2.0 exactly.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace standalone {

enum class Mode { Auto, Off };

// settings.json "standalone": missing = auto; "auto" / "off"; anything else = off, *note says why.
inline Mode parse_mode(const std::optional<std::string>& v, std::string* note) {
    if (!v) return Mode::Auto;
    if (*v == "auto") return Mode::Auto;
    if (*v == "off") return Mode::Off;
    if (note) *note = "settings.json standalone = \"" + *v + "\" (not auto / off): off";
    return Mode::Off;
}
inline const char* mode_name(Mode m) { return m == Mode::Auto ? "auto" : "off"; }

// The top-level reframework/autorun scripts that load a CRSM Lua engine (REFramework runs only the top level of autorun).
constexpr std::array<const char*, 4> kLuaEntries = {
    "SF6_RecordingManager.lua",      // the loader (v1.x and the hybrid Lua + DLL installs)
    "SF6_RecordingSlotManager.lua",  // MAIN (v1.x and the hybrid Lua + DLL installs)
    "CRSM_REFrameworkUI.lua",        // the REFramework display switch (not shipped)
    "Training_ScriptManager.lua",    // the SF6 Training Suite: it loads its own copy of the CRSM UI modules (func/)
};

// The first entry script that exists (exists("SF6_RecordingManager.lua")...), "" when none.
template <class Exists>
std::string find_lua(Exists exists) {
    for (const char* n : kLuaEntries) if (exists(std::string(n))) return n;
    return "";
}

constexpr uint64_t kGraceFrames = 600;
constexpr uint64_t kTrainingFrames = 120;

struct Params {
    uint64_t grace_frames = kGraceFrames;
    uint64_t training_frames = kTrainingFrames;
};

struct Inputs {
    Mode mode = Mode::Auto;
    bool ui_owned = false;        // cmn::owns(ui) and not tripped (the five layers native, core pm service, no fault stop)
    bool claimed = false;         // the UI was handed over already (by the loader, or by the DLL itself)
    bool lua_files = false;       // a kLuaEntries script was in reframework/autorun at the plugin's start
    bool lua_called = false;      // a Lua called the DLL's bridge (ui_claim, frame_poll, owns) at least once
    uint64_t frames = 0;          // frame callbacks since the first one
    uint64_t training_run = 0;    // frame callbacks in a row in training (0 outside)
    bool menu_up = false;         // the pause menu or an option window is up
};

enum class Decision {
    Off,          // mode off (terminal)
    NotNative,    // the owner ui is not the DLL's (terminal for this session: owners are read once)
    Claimed,      // already handed over (terminal)
    LuaCalled,    // a CRSM Lua talks to the DLL: it is the loader's to claim (terminal)
    Wait,         // no Lua: waiting for a closed menu
    WaitLua,      // a CRSM Lua script is installed: waiting for it to call the DLL
    Claim,        // claim the UI now (terminal once done)
    Foreign,      // a CRSM Lua script is installed and never called the DLL: give the hooked layers back (terminal)
};

inline const char* decision_name(Decision d) {
    switch (d) {
        case Decision::Off: return "off";
        case Decision::NotNative: return "not native";
        case Decision::Claimed: return "claimed";
        case Decision::LuaCalled: return "lua called";
        case Decision::Wait: return "wait";
        case Decision::WaitLua: return "wait lua";
        case Decision::Claim: return "claim";
        case Decision::Foreign: return "foreign";
    }
    return "?";
}

inline bool terminal(Decision d) { return d != Decision::Wait && d != Decision::WaitLua; }

inline Decision decide(const Inputs& in, const Params& p = {}) {
    if (in.claimed) return Decision::Claimed;
    if (in.mode != Mode::Auto) return Decision::Off;
    if (!in.ui_owned) return Decision::NotNative;
    if (in.lua_called) return Decision::LuaCalled;
    if (in.lua_files) {
        const bool due = in.frames >= p.grace_frames && in.training_run >= p.training_frames && !in.menu_up;
        return due ? Decision::Foreign : Decision::WaitLua;
    }
    return in.menu_up ? Decision::Wait : Decision::Claim;
}

// decide() with in.menu_up read only when it can change the answer: the menu matters only once the claim (or the foreign
// verdict) would be due with the menu closed. menu_up() is the game read (the pause bit, an option window), so waiting costs
// no game read per frame. Same answer as decide() with the real menu_up, always (integ_bench checks it).
template <class MenuUp>
Decision decide_lazy(Inputs in, MenuUp menu_up, const Params& p = {}) {
    in.menu_up = false;
    const Decision d = decide(in, p);
    if (d != Decision::Claim && d != Decision::Foreign) return d;
    in.menu_up = menu_up();
    return decide(in, p);
}

// The training run: +1 per frame callback in training, back to 0 outside.
inline uint64_t next_training_run(uint64_t run, bool training) { return training ? run + 1 : 0; }

}  // namespace standalone
