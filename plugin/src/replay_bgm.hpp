// SF6 Slots — Replays > Settings: the Replay Music row's choice (replay_outfits.json). Pure, no game access (plugin/tests). Shared
// by the tab (replay_logic.hpp) and the replay's description (replay_outfits.hpp).
//
// The row's first values are the game's Battle BGM row's (Battle Settings > Other, read 06/11, its order: the spin's number + 1 is
// app.SelectBgm, Self 1, Opponent 2, Stage 3, Random 4). A replay has no "self": Character is P1's fighter, Opponent P2's (Wael
// 06/11). Those two play that fighter's playlist of the game's save, or its default music (app.battle.BattleFlowMap.loadBgmPrefab).
// Then a value of ours, Custom: a list of tracks of our own, filled in the game's playlist dialog; the replay plays one of them,
// picked at random by the game. An empty list behaves as Random.
// The file: "bgm": {"mode": "character" | "opponent" | "stage" | "random" | "custom", "list": [<BgmUiDataUserDataRecord.ManageId>, ...]}.
// The list is kept whatever the mode, so leaving Custom does not lose it. No "bgm", or anything else in it: Random, what a replay
// does by itself (app.battle.BattleRule.reset), and the replay's description is left as the game set it.
#pragma once

#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace slots::replay {

enum class BgmMode { Character = 0, Opponent, Stage, Random, Custom };   // the game's row's order, then ours
constexpr int kBgmModes = 5;
constexpr size_t kBgmListMax = 30;           // what the game's dialog takes ("Music: 0 / 30")

struct BgmChoice {
    BgmMode mode = BgmMode::Random;
    std::vector<uint32_t> list;              // Custom's tracks
    bool operator==(const BgmChoice& o) const { return mode == o.mode && list == o.list; }
};

// The game's values: its own messages (the player's language), English when one is not found. Custom's word: ours (words.hpp)
inline const char* bgm_value_guid(BgmMode m) {
    switch (m) {
    case BgmMode::Character: return "b7753bc3-dc3c-41d0-942d-413078de7f87";
    case BgmMode::Opponent:  return "7a08b3b4-aa14-4857-b827-716af1438c91";
    case BgmMode::Stage:     return "300f754d-b308-4729-a60f-4783d57f2a94";
    case BgmMode::Custom:    return nullptr;
    default:                 return "fa32e0d2-996e-4f4b-a183-7074d32bc72f";
    }
}
inline const char* bgm_value_english(BgmMode m) {
    switch (m) {
    case BgmMode::Character: return "Character";
    case BgmMode::Opponent:  return "Opponent";
    case BgmMode::Stage:     return "Stage";
    case BgmMode::Custom:    return "Custom";
    default:                 return "Random";
    }
}

// A System.Guid's text to its bytes in memory (the first three groups little-endian); false when it is not one
inline bool parse_guid(const char* text, uint8_t out[16]) {
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (!text) return false;
    int at = 0;
    for (int i = 0; i < 16; ++i) {
        if (at == 8 || at == 13 || at == 18 || at == 23) { if (text[at] != '-') return false; ++at; }
        const int hi = hex(text[at]);
        if (hi < 0) return false;
        const int lo = hex(text[at + 1]);
        if (lo < 0) return false;
        out[order[i]] = static_cast<uint8_t>(hi * 16 + lo);
        at += 2;
    }
    return text[at] == 0;
}

// Left / Right on the row: its values in order, wrapping
inline BgmMode bgm_cycle(BgmMode m, int dir) {
    return static_cast<BgmMode>(((static_cast<int>(m) + (dir >= 0 ? 1 : -1)) % kBgmModes + kBgmModes) % kBgmModes);
}

// What goes in the replay's BattleRule.BgmType (app.EBgmSet: Stage 0, Player1 1, Player2 2, Random 3); -1: nothing written.
// Custom: Player1, whose list the replay's load gets replaced (replay_music.cpp), with `list` as it stands for this replay
inline int bgm_set(const BgmChoice& b) {
    switch (b.mode) {
    case BgmMode::Character: return 1;
    case BgmMode::Opponent:  return 2;
    case BgmMode::Stage:     return 0;
    case BgmMode::Custom:    return b.list.empty() ? -1 : 1;
    default:                 return -1;
    }
}

// The tracks still owned, in their order, each once, at most what the dialog takes. Nothing known of what is owned (the game's
// list empty or not read): as they are, a list is not emptied for that.
inline std::vector<uint32_t> bgm_keep_owned(const std::vector<uint32_t>& list, const std::vector<uint32_t>& owned) {
    if (owned.empty()) return list;
    std::vector<uint32_t> out;
    for (uint32_t id : list) {
        if (out.size() >= kBgmListMax) break;
        if (std::find(owned.begin(), owned.end(), id) != owned.end() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

// The summary's line of the Custom value: "Custom (3)", in the player's words
inline std::string bgm_custom_line(const std::string& word, size_t count) { return word + " (" + std::to_string(count) + ")"; }

// The list swap at a replay's load: only ever on the copy the game's getter made, never on the list of the save
inline bool bgm_swap_allowed(const void* returned, const void* saved) { return returned && returned != saved; }

// ---- the file ------------------------------------------------------------------------------------------------------------
inline BgmChoice parse_bgm(const Json& root) {
    BgmChoice b;
    const Json* g = root.get("bgm");
    if (!g || g->kind != Json::Object) return b;
    if (const Json* m = g->get("mode"); m && m->kind == Json::String) {
        if (m->s == "character") b.mode = BgmMode::Character;
        else if (m->s == "opponent") b.mode = BgmMode::Opponent;
        else if (m->s == "stage") b.mode = BgmMode::Stage;
        else if (m->s == "custom") b.mode = BgmMode::Custom;
    }
    if (const Json* l = g->get("list"); l && l->kind == Json::Array)
        for (const Json& e : l->arr) {
            if (b.list.size() >= kBgmListMax) break;
            const int64_t id = e.as_int(0);
            if (id <= 0 || id > 0xFFFFFFFFll) continue;
            const uint32_t u = static_cast<uint32_t>(id);
            if (std::find(b.list.begin(), b.list.end(), u) == b.list.end()) b.list.push_back(u);
        }
    return b;
}
// The file's "bgm" entry; "" for the default (Random, no list), so a file never touched stays as it was
inline std::string bgm_json(const BgmChoice& b) {
    if (b.mode == BgmMode::Random && b.list.empty()) return std::string();
    const char* mode = b.mode == BgmMode::Character ? "character" : b.mode == BgmMode::Opponent ? "opponent" : b.mode == BgmMode::Stage ? "stage"
                     : b.mode == BgmMode::Custom ? "custom" : "random";
    std::string o = std::string("\"bgm\": {\"mode\": \"") + mode + "\"";
    if (!b.list.empty()) {
        o += ", \"list\": [";
        for (size_t i = 0; i < b.list.size(); ++i) o += (i ? ", " : "") + std::to_string(b.list[i]);
        o += "]";
    }
    return o + "}";
}

}  // namespace slots::replay
