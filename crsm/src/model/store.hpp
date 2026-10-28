// model::store — CRSM's small data files and the rules around them: paths, the pack list (glob result filtering, sort,
// display names), _erased.json, _names.json, config.json, the legacy-file migration, the reversal map
// (_reversal_virtual.json), the extra reversal rows (_reversal_rows.json), com_state.json.
// Sources: MAIN:13-63, 269-399, 446-459; RS:29-55, 415-436, 440-470 (rows); RMT:154-186 (com_state), CB / RMT:240-262.
// Everything takes and returns json values already decoded (luajson::load_file) or to be encoded (luajson::dump_file).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace model::store {

using json = nlohmann::json;

// ---- paths (relative to the game folder, '/' separators as the Lua writes them) -----------------------------------------
constexpr const char* kDataFolder = "SF6_RecordingSlotManager_data";
constexpr const char* kCommunityFolder = "SF6_Community_data";
std::string char_folder(const std::string& char_name);                  // DATA/<char>
std::string pack_path(const std::string& char_name, const std::string& file);
std::string erased_path(const std::string& char_name);                  // _erased.json
std::string names_path(const std::string& char_name);                   // _names.json
std::string virtual_slots_path(const std::string& char_name);           // _virtual_slots.json
std::string reversal_virtual_path(const std::string& char_name);        // _reversal_virtual.json
std::string reversal_rows_path(const std::string& char_name);           // _reversal_rows.json
std::string backups_folder(const std::string& char_name);               // DATA/<char>/Backups
std::string config_path();                                              // DATA/config.json
std::string com_state_path();                                           // SF6_Community_data/com_state.json

// ---- the pack list -----------------------------------------------------------------------------------------------------
// _rsm_is_backup: "Backups" or "pre_import" anywhere in the path.
bool is_backup(const std::string& path);
// filepath:match("^.+\\(.+)$") or filepath
std::string basename_of(const std::string& path);
// refresh_file_list: from the paths the glob returned (recursive, so the backups come too): drop the backups, the names
// starting with "_" and the erased ones, sort byte-wise (table.sort on strings in the C locale).
std::vector<std::string> pack_files(const std::vector<std::string>& paths, const std::set<std::string>& erased);
// refresh_filtered_list: { "-- Select File --", display names... } for the picker, index 1 = no file.
std::vector<std::string> display_list(const std::vector<std::string>& files, const std::map<std::string, std::string>& names);
std::string display_name(const std::string& file, const std::map<std::string, std::string>& names);   // names[file] or the file without ".json"

// ---- _erased.json: a sorted array of file names (an erased pack is overwritten with an empty table: null) -----------------
std::set<std::string> erased_load(const json& file);
json erased_save(const std::set<std::string>& erased);
// The bytes of an erased pack file (json.dump_file of {}): "null".
std::string erased_pack_bytes();

// ---- _names.json: file name -> display name (the ones that differ) ----------------------------------------------------------
std::map<std::string, std::string> names_load(const json& file);
json names_save(const std::map<std::string, std::string>& names);
// set_display_name: nothing for an empty filename; an empty display or one equal to the file's stem removes the entry.
void set_display_name(std::map<std::string, std::string>& names, const std::string& filename, const std::string* display);

// ---- config.json -------------------------------------------------------------------------------------------------------------
bool config_activate_on_load(const json& cfg, bool current);            // load_settings: the flag when present
json config_with_activate_on_load(const json& cfg, bool on);            // save_settings: the file's other keys kept
// import_training_setup: false = an import leaves the dummy's reversal rows and replay settings alone even when the pack
// carries them (docs/PACK_FORMAT.md). Absent: true.
bool config_import_setup(const json& cfg);
bool config_migration_done(const json& cfg);
json config_with_migration(const json& cfg, int64_t count);             // migration_done = true, migrated_count = count

// ---- legacy migration (one-shot) ---------------------------------------------------------------------------------------------
// migrate_legacy_files: where a loose DATA/<file>.json goes. Candidate characters are tried in id order (the Lua used
// pairs(): an undefined order, which only matters for a name holding two character names).
struct Migration { std::string char_name; std::string dst_name; };
std::optional<Migration> legacy_target(const std::string& filename);

// ---- reversal map: [rtype][row] = virtual slot (0-based row) ------------------------------------------------------------------
class RevMap {
public:
    void load(const json& file);                         // map_load: numbers only, keys normalised to integers
    json to_file() const;                                // map_save: {"rt": {"row": n}} (an empty rtype is an empty table)
    std::optional<json> get(int64_t rt, int64_t row) const;
    void set(int64_t rt, int64_t row, const json& n);
    void erase(int64_t rt, int64_t row) { m_[rt].erase(row); }      // map[rt] = map[rt] or {}; map[rt][row] = nil
    const std::map<int64_t, json>* rows_of(int64_t rt) const { auto it = m_.find(rt); return it == m_.end() ? nullptr : &it->second; }
private:
    std::map<int64_t, std::map<int64_t, json>> m_;
};

// ---- extra reversal rows: [rtype 0-2][row 10-29] = the 9 fields of a ReversalData --------------------------------------------------
struct RdRow {
    int64_t Type = -1; bool IsActive = false; bool IsValid = false; bool IsFrame = false;
    int64_t Delay = 0, Count = 1, SkillIndex = -1, Level = 0, Frame = 0;
};
json rd_to_json(const RdRow& r);                          // rd_to_table
RdRow rd_from_json(const json& t, const RdRow& base);     // rd_from_table: a field absent from t keeps its value in base
constexpr int kRevTypes = 3, kRowsPage = 10, kRowsTotal = 30;
using RevRows = std::array<std::map<int, RdRow>, kRevTypes>;
json rev_rows_to_file(const RevRows& rows);               // ext_save: all 3 types x rows 10-29 (missing rows: defaults)
RevRows rev_rows_from_file(const json& file);             // ensure_ext: saved[tostring(rt)][tostring(k)] when a table

// ---- com_state.json ------------------------------------------------------------------------------------------------------------
struct ComState {
    json last_id;                                         // null = nil
    std::map<std::string, json> downloaded;               // file -> version (number), legacy `true` = v1
};
ComState com_state_load(const json& file);
json com_state_save(const ComState& s);                   // { last_id = ..., downloaded = ... }
// The pack file a community loadout is saved as: display_name without ".json", every character outside [%w-_. ] removed
// (the non-ASCII ones too), ".json" added.
std::string community_filename(const std::string& display_name);
// dl_version: the version of the downloaded copy (legacy true = 1), nullopt when never downloaded. A name ending with the
// Worker's test-row tag " [TEST]" (list only, the download gives the name without it) also finds the copy without the tag.
std::optional<json> dl_version(const ComState& s, const std::string& display_name);
// sync_downloaded_with_disk: drop the entries whose pack is erased or no longer on disk. True when something changed.
bool sync_downloaded(ComState& s, const std::set<std::string>& erased, const std::function<bool(const std::string&)>& file_exists);

}  // namespace model::store
