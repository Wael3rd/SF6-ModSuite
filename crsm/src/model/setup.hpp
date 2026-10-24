// model::setup — what a pack carries besides its recordings so that a loadout plays as its author set it up: each slot's
// `active` flag (the slots a normal replay draws from) and, on the entry with id 1, the "training" block: the dummy's
// reversal rows and the replay settings. Format and rules: crsm/docs/PACK_FORMAT.md.
//
// Why on entry 1 and not a new array element: every reader of packs (CRSM 2.1.x, the Lua versions, the Community server)
// requires each array element to be a slot entry with an id 1..30; unknown keys of an entry are ignored by all of them, and
// entry 1 is a live slot, never stored in _virtual_slots.json, so the block never leaks into another file.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "store.hpp"

namespace model::setup {

using json = nlohmann::json;
using model::store::RdRow;

constexpr const char* kKey = "training";       // the block's key on the entry with id 1
constexpr const char* kInfoKey = "info";        // the loadout's metadata (description, author, source), entry 1 too
constexpr int kVersion = 1;                     // a reader applies a block of its own version only (additions keep 1)
constexpr int kCats = 3, kRowsPage = 10, kRowsTotal = 30, kSlotTotal = 30;
constexpr int kRecording = 4, kWork = 7;        // ReversalType.RECORDING; live slot 8 (0-based), what a virtual row stores
constexpr int kTypeMin = -1, kTypeMax = 6;      // ReversalType NONE .. OTHER
// The reversal layer's category order (rt): DownReversalDatas, GuardReversalDatas, DamageReversalDatas.
constexpr const char* kCatNames[kCats] = { "down", "guard", "damage" };

// TM_RecordSetting's replay settings. RecordType (the Record / Replay sub-page), RecordStartType and RecordCountDown
// (how the user records) are the user's own and never carried.
struct Playback { std::optional<int64_t> PlayType, LoopType, InfoType; };

// The dummy's settings: the pages of the game's Dummy Settings tab, TrainingManager._tData.<object>.DummyData, carried
// field by field under the game's names in the block's "dummy" part, one sub-object per page (`part`). A value outside
// [min, max] (the game's enum, a percentage, a plausible count) is not taken; the weights of a random choice (group > 0)
// are taken together or not at all, and never all zero (the game draws among them). CpuLevel and the counter's StyleNo
// are left out (their ranges are not known).
struct DummyField {
    const char* part;                           // the key in "dummy"
    const char* object;                         // the _tData field whose DummyData holds it
    const char* name;                           // the game's field
    bool is_bool;
    int64_t min, max;
    int group;                                  // 0, or the random weights it belongs to
};
inline constexpr DummyField kDummyFields[] = {
    { "status", "DummyStatus", "DummyActionType", false, 0, 5, 0 },     // TM_DS_DUMMYTYPE STAND .. CPU
    { "status", "DummyStatus", "JumpType", false, 0, 3, 0 },            // TM_DS_JUMPTYPE VIRTICAL, FRONT, BACK, RANDOM
    { "status", "DummyStatus", "JumpWeight_Front", false, 0, 65535, 1 },
    { "status", "DummyStatus", "JumpWeight_Virtical", false, 0, 65535, 1 },
    { "status", "DummyStatus", "JumpWeight_Back", false, 0, 65535, 1 },
    { "guard", "GuardSetting", "GuardType", false, 0, 5, 0 },           // TM_GS_GUARD_TYPE NO_GUARD .. COUNT_GUARD
    { "guard", "GuardSetting", "GuardWeight", false, 0, 100, 0 },
    { "guard", "GuardSetting", "GuardCount", false, 0, 9999, 0 },
    { "guard", "GuardSetting", "IsGuardSwitching", true, 0, 1, 0 },
    { "guard", "GuardSetting", "GuardOnlyType", false, 0, 3, 0 },       // TM_GS_ONLY_TYPE NONE, STAND, CROUCH, RANDOM
    { "defense", "DefenseSystem", "QS_Type", false, 0, 2, 0 },          // recovery, TM_DS_QS_TYPE STAY, BACK, RANDOM
    { "defense", "DefenseSystem", "QS_Weight", false, 0, 100, 0 },
    { "defense", "DefenseSystem", "GD_Type", false, 0, 2, 0 },          // throw escape, TM_COMMON_TYPE OFF, ON, RANDOM
    { "defense", "DefenseSystem", "GD_Weight", false, 0, 100, 0 },
    { "defense", "DefenseSystem", "DR_Type", false, 0, 3, 0 },          // DRIVE_REVERSAL_TYPE OFF, GUARD, GET_UP, RANDOM
    { "defense", "DefenseSystem", "DR_No_Weight", false, 0, 65535, 2 },
    { "defense", "DefenseSystem", "DR_Guard_Weight", false, 0, 65535, 2 },
    { "defense", "DefenseSystem", "DR_Getup_Weight", false, 0, 65535, 2 },
    { "defense", "DefenseSystem", "DR_DelayFrame", false, 0, 9999, 0 },
    { "defense", "DefenseSystem", "DR_DelayCount", false, 0, 9999, 0 },
    { "defense", "DefenseSystem", "DP_Type", false, 0, 3, 0 },          // drive parry, TM_DP_TYPE OFF, PARRY, JUST, RANDOM
    { "counter", "CounterSetting", "NC_TYPE", false, 0, 2, 0 },         // TM_COMMON_TYPE OFF, ON, RANDOM
    { "counter", "CounterSetting", "PC_TYPE", false, 0, 2, 0 },
    { "counter", "CounterSetting", "NH_Weight", false, 0, 65535, 3 },
    { "counter", "CounterSetting", "NC_Weight", false, 0, 65535, 3 },
    { "counter", "CounterSetting", "PC_Weight", false, 0, 65535, 3 },
};
constexpr int kDummyCount = (int)(sizeof(kDummyFields) / sizeof(kDummyFields[0]));

// v[i] = kDummyFields[i]'s value (a bool as 0 / 1); nullopt = not carried (the game keeps the user's).
struct Dummy {
    std::array<std::optional<int64_t>, kDummyCount> v{};
    bool any() const;
};
// Drops what a reader does not take: values out of range, a weight group incomplete or all zero.
void sanitize(Dummy& d);

// rows[rt][row] in pack terms: a RECORDING row's SkillIndex is its slot - 1 (0-29), virtual slots included. 10 rows (the
// game's) or 30 (+ CRSM's rows 11-30); a missing row is a default (empty) row.
using Rows = std::array<std::vector<RdRow>, kCats>;

struct Setup {
    std::optional<Rows> reversal;
    std::optional<Playback> playback;
    std::optional<Dummy> dummy;                 // set only when it holds a value
};

// {"version": 1, "reversal"?: {"down": [rows], "guard": [...], "damage": [...]}, "playback"?: {...},
//  "dummy"?: {"status"?: {...}, "guard"?: {...}, "defense"?: {...}, "counter"?: {...}}}
json to_json(const Setup& s);
// A version-1 block: nullopt (why says it) when absent, not an object or of another version. A part that is not an object
// is absent; a category that is not an array is all default rows; rows past 30 are ignored.
std::optional<Setup> from_json(const json& block, std::string* why);

// The entry whose id is 1 (numerically), nullptr when none.
const json* entry_one(const json& pack);
json* entry_one(json& pack);
std::optional<Setup> of_pack(const json& pack, std::string* why);
bool stamp(json& pack, const Setup& s);         // the block onto entry 1; false when the pack has none

// `active` of every entry: on[id - 1] and the entry has content (an empty slot is never active).
void stamp_active(json& pack, const std::array<bool, kSlotTotal>& on);
// The boolean `active` of an entry; nullopt when absent or not a boolean (a pack without the flags: today's behaviour).
std::optional<bool> active_of(const json& entry);

bool is_default(const RdRow& r);
bool rows_in_use(const Rows& r);                // a row valid somewhere: worth carrying (an unused reversal setup is not)
void trim(Rows& r);                             // every category to 10 rows when all of 11-30 are default, else 30

// Between pack terms and the game's data: the game holds a row that plays virtual slot n (9-30) as RECORDING / slot 8
// (SkillIndex 7) with _reversal_virtual.json's map[rt][row] = n.
RdRow from_game(const RdRow& game, std::optional<int64_t> mapped);   // the mapped n when the row is still RECORDING / 7
struct GameRow { RdRow row; int virt = 0; };    // virt: the n to map (9-30), 0 = no map entry for the row
// A row the game cannot play (Type outside -1..6, a RECORDING row whose slot is outside 1-30) becomes a default row: the
// game indexes its slot array with SkillIndex.
GameRow to_game(const RdRow& pack);

}  // namespace model::setup
