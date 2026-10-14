// model::reversal — the decisions of the "reversal" layer (phase P3c), pure: the map
// of the reversal rows that play a virtual slot (the game's data says "recording, slot 8"), its hygiene on the game's
// setters, and what the trigger presents to the game. Sources: ReversalSlots.lua (RSL) 286-301 (SetReversalData /
// RemoveReversalData), 553-583 (present_rows), 586-622 (UpdateReversalCheck), 645-655 (the setters' saves).
// The game objects (the 10 / 30 / scratch arrays, the candidates) are in mech/layer_reversal.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "draw.hpp"
#include "nlohmann/json.hpp"

namespace model::reversal {

using json = nlohmann::json;

constexpr int kRecording = 4;          // app.training.ReversalType.RECORDING
constexpr int kWork = 7;               // live slot 8 (0-based): what the game's data holds for a virtual reversal
constexpr int kRowsPage = 10, kRowsTotal = 30, kSlotTotal = 30;

// SetReversalData pre (RSL:289-296): with the page's category rt and the arguments, what happens to the map and to the
// skill argument. `mapped` = map_get(rt, row) before the call.
struct SetResult {
    bool ignored = false;              // no category, or row outside 0-29: the call goes through untouched
    bool map_set = false;              // map[rt][row] = n (a virtual slot chosen in the picker)
    int n = 0;
    bool rewrite_skill = false;        // the skill argument becomes 7 (the game stores "slot 8")
    bool map_clear = false;            // the row back to the game's own choice
};
SetResult on_set(std::optional<int64_t> rt, int64_t row, int64_t type, int64_t skill, bool mapped);

// One ReversalData as the trigger reads it.
struct Row { bool present = false, valid = false, active = false; int64_t type = -1, skill = -1; };

// present_rows (RSL:553-583): when one of rows 10-29 is valid and active, a work array of 10 stands in for the game's:
// the valid and active rows (a uniform random ten of them when more), a virtual row pointed at its own slot.
struct Plan {
    bool use = false;                  // false: no extra row active (the mapped-rows path runs instead)
    std::vector<int> cands;            // the rows copied into the work array, in its order (<= 10)
    std::vector<int> virt;             // per work entry: the virtual slot n it plays (SkillIndex n - 1), 0 = as copied
    bool need_push = false;            // a virtual row is presented: the 30-slot array must be in place
};
Plan present(const Row (&big)[kRowsTotal], const std::function<std::optional<int64_t>(int row)>& map_of,
             const std::function<int64_t(int n)>& vframes, model::draw::Rng& rng);

// The mapped-rows path (RSL:603-613): the rows of the game's array that are mapped, still "recording / slot 8", and whose
// virtual slot has content. `game_row(row)` = rows:GetValue(row) (false when out of the array).
struct Change { int row; int n; };
std::vector<Change> mapped(const std::map<int64_t, json>& rt_map, const std::function<bool(int row, Row*)>& game_row,
                           const std::function<int64_t(int n)>& vframes);

}  // namespace model::reversal
