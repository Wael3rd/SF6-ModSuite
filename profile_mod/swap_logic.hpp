// profile_mod: the pure part of the full-profile swap and the settings (logic_test.cpp). No game access.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace psn {

// What to do with the simple profile's flow, from what is seen at each look.
//   - One swap per flow: `done` holds the handle swapped or left alone only while that handle is there. Once it is gone its address may
//     come back for a new flow, which gets its own decision (the Lua's swapped[addr] was never cleared).
//   - No swap from inside a full profile: a second one on top of the first, on the same agents. The simple one stays.
//   - No loop: a simple profile appearing right after a full profile of ours closed is the game opening it again by itself, or a very
//     quick re-open: it stays, so Back can never be trapped.
struct SwapTracker {
    static constexpr uint32_t kDelayFrames = 5;      // the simple flow finishes being created
    static constexpr uint32_t kLoopFrames = 120;     // about 2 s

    enum class Do { Nothing, Swap, LeaveAlone };

    uintptr_t pending = 0, done = 0;
    uint32_t pending_at = 0;
    bool ours = false;                               // the full profile up, or last up, came from our swap
    uint32_t ours_seen_at = 0;

    // handle: the simple flow's handle, 0 none; top: a full profile is up now; frame: a counter of frames
    Do step(uintptr_t handle, bool top, uint32_t frame) {
        if (ours) {
            if (top) ours_seen_at = frame;
            else if (frame - ours_seen_at > kLoopFrames) ours = false;
        }
        if (!handle) { pending = 0; done = 0; return Do::Nothing; }
        if (handle == done) return Do::Nothing;
        if (top || ours) { pending = 0; done = handle; return Do::LeaveAlone; }
        if (handle != pending) { pending = handle; pending_at = frame; return Do::Nothing; }
        if (frame - pending_at < kDelayFrames) return Do::Nothing;
        pending = 0;
        done = handle;
        return Do::Swap;
    }
    void swapped(uint32_t frame) { ours = true; ours_seen_at = frame; }
};

// reframework\data\profile_mod.ini: key=value lines, both 1 by default; only a 0 turns one off. Anything else is ignored.
struct Settings { bool full_profile = true, steam_name = true; };
inline Settings parse_settings(const std::string& text) {
    Settings s;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string key, val;
        bool in_val = false;
        for (size_t k = i; k < e; ++k) {
            const unsigned char c = static_cast<unsigned char>(text[k]);
            if (isspace(c)) continue;
            if (c == ';' || c == '#') break;
            if (c == '=' && !in_val) { in_val = true; continue; }
            (in_val ? val : key) += static_cast<char>(tolower(c));
        }
        if (in_val && (val == "0" || val == "1")) {
            if (key == "full_profile") s.full_profile = val == "1";
            else if (key == "steam_name") s.steam_name = val == "1";
        }
        i = e + 1;
    }
    return s;
}

}  // namespace psn
