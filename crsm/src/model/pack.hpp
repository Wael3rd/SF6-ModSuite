// model::pack — loadouts ("packs"): the 30-entry array of a pack file, its validation, the split between live slots
// (1-8, the game's) and virtual slots (9-30, CRSM's), the entry of a live slot, file names and backups.
// Sources: MAIN:551-564 (validate), 580-599 (split), 962-1004 (serialize), 1070-1134 (save names), 566-577 (backup),
// 1159-1167 (export_as); RMT:2049-2051 (export of 9-30).
//
// A pack entry is a json object {id, weight, name?, timeline[], empty, active?} (+ whatever else a file carries: kept);
// entry 1 may carry the "training" block. `active` and the block: model::setup, docs/PACK_FORMAT.md.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace model::pack {

using json = nlohmann::json;

constexpr int kLiveSlots = 8;          // slots 1-8: the game's
constexpr int kSlotTotal = 30;         // 8 live + 22 virtual (RMT: SLOT_TOTAL)

// validate_slot_data: a non-empty table of entries with a numeric id in 1..30, each either `empty` or carrying a table
// `timeline` or `inputs`. `err` is the Lua message ("empty or invalid slot data", "invalid slot entry (bad id)",
// "slot N has no timeline/inputs").
bool validate(const json& data, std::string* err);

// info.description of a pack (the entry whose id is 1, docs/PACK_FORMAT.md): "" when none
std::string description(const json& data);

// apply_data_to_character's split of a pack: entries with id <= 8 go to the live slots, the others to the virtual ones
// (in file order). Call after validate(). An id that is not an integer is compared as a number (3.5 <= 8: live).
struct Split { json live = json::array(); json virt = json::array(); };
Split split(const json& data);

// serialize_slots_data, one live slot: `values` = the frames of the recording (the buffer's first `frames` values).
// {id, weight, name? (only when non-empty), timeline, empty}; an invalid slot or 0 frames gives the empty entry
// {id, weight, timeline = {}, empty = true}.
json live_entry(int id, int64_t weight, const std::string& name, bool is_valid, int64_t frames, const uint16_t* values);

// A full pack: the 8 live entries followed by the 22 virtual ones (vslots::export_virtual). 30 entries.
json assemble(const json& live_entries, const json& virtual_entries);

// ---- names -------------------------------------------------------------------------------------------------------------
// save_custom_file_text: the file name for a typed name: ".json" added when missing, the character name put in front
// ("<Char>_") when the normalised name does not contain the normalised character name.
std::string save_filename(const std::string& typed, const std::string& char_name);

// export_as: what the user typed in the name dialog -> the display name (trimmed, chevrons and the like kept) and the
// file name (the characters \ / : * ? " < > | removed, trimmed, ".json" added). file is empty when nothing is left.
struct TypedName { std::string display; std::string file; };
TypedName typed_name(const std::string& typed);

// "<Char>_pre_import_<YYYYMMDD_HHMMSS>.json" (in <Char>/Backups/)
std::string backup_filename(const std::string& char_name, const std::tm& when);

// _rsm_last_backup_clock: a backup is skipped when the previous one for the same fighter is less than 5 s old.
struct BackupGate {
    bool allow(int fighter_id, double now_seconds);
private:
    struct Slot { int id; double at; };
    std::vector<Slot> slots_;
};

}  // namespace model::pack
