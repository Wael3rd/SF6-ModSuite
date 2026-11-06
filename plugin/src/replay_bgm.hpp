// SF6 Slots — Replays > Settings: the Battle BGM row's choice (replay_outfits.json). Pure, no game access (plugin/tests). Shared
// by the tab (replay_logic.hpp) and the replay's description (replay_outfits.hpp).
//
// The row's values are the game's row's (Battle Settings > Other, read 06/11, its order: the spin's number + 1 is app.SelectBgm,
// Self 1, Opponent 2, Stage 3, Random 4). A replay has no "self": Character is P1's fighter, Opponent P2's (Wael 06/11). Those
// two play that fighter's playlist of the game's save, or its default music (app.battle.BattleFlowMap.loadBgmPrefab).
// The file: "bgm": {"mode": "character" | "opponent" | "stage"}. No "bgm", or anything else in it: Random, what a replay does by
// itself (app.battle.BattleRule.reset), and the replay's description is left as the game set it.
#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>

namespace slots::replay {

enum class BgmMode { Character = 0, Opponent, Stage, Random };   // the game's row's order
constexpr int kBgmModes = 4;

struct BgmChoice {
    BgmMode mode = BgmMode::Random;
    bool operator==(const BgmChoice& o) const { return mode == o.mode; }
};

// The row's words: the game's own messages (the player's language), English when one is not found
constexpr const char* kBgmTitleGuid = "271a7b31-b6f2-4321-bd51-c2e7b6c862f6";
constexpr const char* kBgmTitleEnglish = "Battle BGM";
inline const char* bgm_value_guid(BgmMode m) {
    switch (m) {
    case BgmMode::Character: return "b7753bc3-dc3c-41d0-942d-413078de7f87";
    case BgmMode::Opponent:  return "7a08b3b4-aa14-4857-b827-716af1438c91";
    case BgmMode::Stage:     return "300f754d-b308-4729-a60f-4783d57f2a94";
    default:                 return "fa32e0d2-996e-4f4b-a183-7074d32bc72f";
    }
}
inline const char* bgm_value_english(BgmMode m) {
    switch (m) {
    case BgmMode::Character: return "Character";
    case BgmMode::Opponent:  return "Opponent";
    case BgmMode::Stage:     return "Stage";
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

// What goes in the replay's BattleRule.BgmType (app.EBgmSet: Stage 0, Player1 1, Player2 2, Random 3); -1: nothing written
inline int bgm_set(const BgmChoice& b) {
    switch (b.mode) {
    case BgmMode::Character: return 1;
    case BgmMode::Opponent:  return 2;
    case BgmMode::Stage:     return 0;
    default:                 return -1;
    }
}

// ---- the file ------------------------------------------------------------------------------------------------------------
inline BgmChoice parse_bgm(const Json& root) {
    BgmChoice b;
    const Json* g = root.get("bgm");
    const Json* m = g && g->kind == Json::Object ? g->get("mode") : nullptr;
    if (!m || m->kind != Json::String) return b;
    if (m->s == "character") b.mode = BgmMode::Character;
    else if (m->s == "opponent") b.mode = BgmMode::Opponent;
    else if (m->s == "stage") b.mode = BgmMode::Stage;
    return b;
}
// The file's "bgm" entry; "" for the default, so a file never touched stays as it was
inline std::string bgm_json(const BgmChoice& b) {
    switch (b.mode) {
    case BgmMode::Character: return "\"bgm\": {\"mode\": \"character\"}";
    case BgmMode::Opponent:  return "\"bgm\": {\"mode\": \"opponent\"}";
    case BgmMode::Stage:     return "\"bgm\": {\"mode\": \"stage\"}";
    default:                 return std::string();
    }
}

}  // namespace slots::replay
