// model::files — the "files" layer of the port (phase P3a): every loadout action of
// SF6_RecordingSlotManager.lua (MAIN) and the virtual-slot storage of RecordingManagerTab.lua (RMT), on the P1 model,
// with the same files as the Lua byte for byte.
//
// Pure C++: the game is behind `LiveSlots` (the 8 live slots of the dummy, path A as MAIN's get_slots_access) and the
// clock behind `Clock`, so the bench drives the same engine on copies of the user's data with fakes. The DLL plugs the
// game adapter in (mech/files.cpp).
//
// Sources (line numbers of the shipped Lua): MAIN:190-441 (lists, names, erased), 547-744 (validate, backup, import),
// 871-954 (ALLOC / WRITE_DATA), 960-1044 (serialize, slot API), 1069-1221 (save, open, export as, erase, rename, reset);
// RMT:461-481 (vslots storage), 1409-1418 (slot names), 2049-2104 (export / import of 9-30), 1525-1536 (Delete of a
// virtual slot).
//
// Rules kept from the Lua, on purpose:
//  * a file is written only where the Lua writes it, from the state the Lua would hold (never reload-and-rewrite: the
//    null members would go, P1);
//  * MAIN uses path A for the live slots (RMT / RSL path B): LiveSlots::access is path A;
//  * the two fixes Wael decided: names are cut on a character boundary (lutil::utf8_cut), and nothing else.
#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "pack.hpp"
#include "setup.hpp"
#include "vslots.hpp"

namespace model::files {

using json = nlohmann::json;

constexpr int64_t kRecordBufferLen = 3600;      // UInt16 entries a game recording allocates per slot (60 s)

// ---- the game, as the files layer sees it ---------------------------------------------------------------------------
// One live slot read for serialize_slots_data: Frame, Weight (the json number the Lua would hold), IsValid, and the
// first `frames` buffer values when valid and frames > 0.
struct LiveRead {
    bool valid = false;
    bool active = false;                     // IsActive (the pack's `active` flag)
    int64_t frames = 0;
    json weight = 0;
    std::vector<uint16_t> values;
};

class LiveSlots {
public:
    virtual ~LiveSlots() = default;
    // get_slots_access(fid) (MAIN:248, path A): "" when the slot array is reachable, else the Lua's error text
    // ("Unknown Character ID", "No TrainingManager", "No RecFunc", "No Fighter List", "Data not found for ID <n>").
    virtual std::string access(int fid) = 0;
    // One slot (k0 = 0..7). 0 = read; a read the Lua raises on: 1 = no InputData, 2 = no buffer or frames past its end.
    virtual int read(int fid, int k0, LiveRead* out) = 0;
    // InputData.buff length (0 = no InputData / no buffer).
    virtual int64_t capacity(int fid, int k0) = 0;
    // The direct allocation of MAIN:629-640: UInt16[max(3600, needed)] into InputData.buff, Cursor = 0, and the length
    // checked. false = the Lua's pcall failed there (no InputData...): the ALLOC fallback takes the slot.
    virtual bool allocate(int fid, int k0, int64_t needed) = 0;
    // The writes of MAIN:673-716.
    virtual bool set_weight(int fid, int k0, const json& weight) = 0;          // Weight (an integer field)
    virtual bool write_empty(int fid, int k0) = 0;                             // IsValid false, Frame 0, IsActive false, Num 0
                                                                               // (false = no InputData: the three fields only)
    virtual bool write_values(int fid, int k0, const uint16_t* v, size_t n) = 0;   // buffer[0..n): false past its length
    virtual bool finish(int fid, int k0, int64_t needed) = 0;                  // IsValid true, Frame needed, IsActive false, Num needed
    virtual bool activate_valid(int fid) = 0;                                  // activate_on_load (MAIN:729-735)
    // IsActive of one slot (a pack's `active` flag); not pure so that a game without it (older fakes) keeps compiling.
    virtual bool set_active(int, int, bool) { return false; }
    // The two game calls of fill_record are on a RecordData the caller holds (not here).
};

// The pack's "training" block in the game (model::setup): read at an export or a backup, written at an import. The DLL's
// adapter (mech/layer_files.cpp) reaches the reversal rows through the reversal layer (its map and rows 11-30).
class SetupPort {
public:
    virtual ~SetupPort() = default;
    // The dummy's setup now; false = not reachable (the pack then carries no block). A part the port cannot read is
    // left out (nullopt).
    virtual bool read(int fid, model::setup::Setup* out) = 0;
    // The parts the setup holds replace the game's; `report` says what was written or skipped (for the journal).
    virtual bool write(int fid, const model::setup::Setup& s, std::string* report) = 0;
};

class Clock {
public:
    virtual ~Clock() = default;
    virtual double seconds() = 0;            // os.clock(): the 5 s backup gate
    virtual std::tm local_now() = 0;         // os.date("%Y%m%d_%H%M%S")
};

// ---- ALLOC / WRITE_DATA (MAIN:646-669, 871-954) --------------------------------------------------------------------
// The queue of the import's fallback when a slot has no memory: a real 10-frame recording per slot, then the import
// again. Pure state; the game steps run through AllocEnv on the DLL's frame callback, never while paused.
struct AllocAction {
    enum class Type { Alloc, WriteData } type = Type::Alloc;
    enum class Step { Init, Waiting, Stop1, Stop2 } step = Step::Init;
    int slot = 0;                            // ALLOC: 0-based live slot
    int timer = 0;
    int target_id = -1;                      // WRITE_DATA
    json data;
    std::string name;
    bool already_retried = false;
};

class AllocEnv {
public:
    virtual ~AllocEnv() = default;
    virtual bool start_recording(int slot) = 0;   // ChangeRecordStartSetting(0), SetStartRecord(16, slot), ReleaseDummyData,
                                                  // CopyDummyData, TrainingManager.ChangeState(3), ForceApply. false = no RecordFunc
    virtual void stop_phase1() = 0;               // TrainingManager.ChangeState(4)
    virtual void stop_phase2() = 0;               // StopRecord, ChangeRecordStartSetting(1), ForceApply
};

// ---- the engine --------------------------------------------------------------------------------------------------------
class Engine {
public:
    // root = the folder that holds SF6_RecordingSlotManager_data (the game's reframework/data in the DLL).
    Engine(std::filesystem::path root, LiveSlots* live, Clock* clock);
    // The training-block port (nullptr = none: packs are written and imported without the block, as before).
    void set_setup(SetupPort* setup) { setup_ = setup; }

    // config.json's activate_on_load (MAIN:452-459) and import_training_setup, read once at the start as the Lua does at load.
    void load_settings();
    bool import_setup() const { return import_setup_; }
    // A fighter named after the game added it (Arjun, 34) kept its data in DATA/Unknown(<id>) until then: that folder
    // is merged into DATA/<name> (renamed when <name> does not exist; otherwise every file <name> lacks is moved, and
    // the old folder removed once empty). Returns one line per folder handled, for the log.
    std::vector<std::string> migrate_unknown_folders();

    // ---- character (MAIN:202-241 hook, MAIN:755-775 ensure_files) ----
    int fighter() const { return fid_; }
    std::optional<std::string> char_name() const;            // current_char_name: nil when -1
    void fighter_changed(int new_id);                        // the UpdateGameInfo hook's reset on a new dummy
    bool ensure(int select_menu_id);                         // ensure_files with the id read from SelectMenu (-1: unknown)
    void invalidate() { files_char_.reset(); }
    // The cached loadout list was read for the held dummy: ensure(fighter()) would not read the folder again.
    bool list_fresh() const { return files_char_ && *files_char_ == fid_; }

    // ---- the picker (MAIN:776-784, 1174-1176) ----
    const std::vector<std::string>& dropdown() const { return display_list_; }
    int dropdown_index() const { return dropdown_index_; }
    void set_dropdown_index(const json& idx);
    std::string selected_name() const;
    std::string selected_file() const { return custom_file_input_; }
    std::string loaded_name() const;
    uint64_t list_version() const { return list_version_; }

    // ---- loadout actions: the result string the Lua returns; `ticker` = the ticker MAIN itself shows ("<x> File Saved")
    std::string import_selected();
    std::string export_as(const json& name, std::string* ticker);
    std::string export_selected(std::string* ticker);
    std::string erase_selected();
    std::string rename_selected(const json& new_name, std::string* ticker);
    std::string reopen_loaded();
    void set_native_action(bool on) { native_action_ = on; }

    // ---- slot API for the virtual slots (MAIN:1007-1044) ----
    json slot_name(int idx0) const;                          // "" when none
    void set_slot_name(int idx0, const json& name);          // names of slots 1-8 (memory only)
    std::optional<json> read_slot(int idx0, std::string* err);   // serialize_slots_data(...)[idx0 + 1]
    std::string write_slot(const json& entry);               // apply as "virtual slot" (silent, no backup, no split)

    // ---- virtual slots 9-30 (RMT storage, the DLL is their single writer when it owns "files") ----
    model::vslots::VSlots& vslots();                         // loaded for the current character (vslots_load)
    std::optional<std::string> vslots_char() const { return vslots_char_; }
    uint64_t vslots_version() const { return vslots_version_; }
    uint64_t vslots_imports() const { return vslots_imports_; }
    void vslots_save();                                      // the file from the model (RMT vslots_save)
    void name_slot(int n, const json& name);                 // RMT set_slot_name: 1-8 memory, 9-30 file (cut on a character boundary)
    void vslot_delete(int n);                                // RemoveRecord on a virtual row: vslots[n] = nil + save
    void vslot_put(int n, const json& entry, bool save);     // finalize_virtual_recording: vslots[n] = e (+ save)
    // uninstall_big's read-back: { "n": [active, weight] }, written once when anything changed (RMT:1023-1033)
    void vslots_active_weight(const json& patch);

    // ---- ALLOC / WRITE_DATA ----
    bool alloc_pending() const { return !queue_.empty(); }
    // One step of MAIN's on_frame queue (call every frame; `paused` = never ALLOC under the pause). Returns the ticker to
    // show ("" = none): only the "<x> File Loaded" of a WRITE_DATA outside the Manager page's actions. `crashed` = the
    // import raised (the Lua's on_frame error: the action stays at the head and runs again next frame).
    std::string alloc_tick(bool paused, AllocEnv& env, bool* crashed = nullptr);
    const std::deque<AllocAction>& alloc_queue() const { return queue_; }

    // the last result string (status_msg), for the journal
    const std::string& status() const { return status_; }
    // what the last import did with the pack's training block ("" = the pack had none or nothing was done)
    const std::string& setup_report() const { return setup_report_; }

    // ---- paths (relative to root, '/' as the Lua writes them) ----
    std::filesystem::path abs(const std::string& rel) const;
    // json.load_file / json.dump_file under root, for the other layers' files (the reversal map and rows)
    json load_doc(const std::string& rel) const { return load_json(rel); }
    bool write_doc(const std::string& rel, const json& value) { return write_json(rel, value); }

private:
    // MAIN's helpers
    void refresh_file_list();
    void refresh_filtered_list();
    bool ensure_file_list();
    void load_erased();
    void mark_erased(const std::string& filename);
    void load_names();
    void set_display_name(const std::string& filename, const std::string* display);
    std::string display_name(const std::string& file) const;
    void reset_selection();
    std::optional<json> serialize_slots(int use_id, std::string* char_name, std::string* err, bool* crashed, bool save_site,
                                        std::array<bool, 8>* actives = nullptr);
    void stamp_pack(json& pack, int use_id, const std::array<bool, 8>& live_active, bool backup);
    void apply_flags_and_setup(int use_id, const json& data);
    void backup_live_slots(int use_id);
    std::string apply(int target_id, json data, const std::string& source_name, bool* crashed, std::string* ticker);
    std::string save_custom_file_text(std::string* ticker, bool* crashed);
    std::string import_custom_file_text(bool* crashed);
    void import_virtual(const json& list);
    bool write_json(const std::string& rel, const json& value);      // json.dump_file
    json load_json(const std::string& rel) const;                    // json.load_file
    std::vector<std::string> glob_char(const std::string& char_name) const;

    std::filesystem::path root_;
    LiveSlots* live_;
    Clock* clock_;

    int fid_ = -1;
    std::optional<int> files_char_;                 // the character the cached list was built for (nullopt = stale)
    std::vector<std::string> cached_file_list_;
    std::vector<std::string> filtered_file_list_;
    std::vector<std::string> display_list_{ "-- Select File --" };
    int dropdown_index_ = 1;
    std::string custom_file_input_;
    std::string loaded_file_name_;
    std::set<std::string> erased_;
    std::map<std::string, std::string> names_;
    std::array<json, 8> slot_names_{ "", "", "", "", "", "", "", "" };
    bool activate_on_load_ = false;
    bool import_setup_ = true;                      // config.json import_training_setup (absent = true)
    SetupPort* setup_ = nullptr;
    bool quiet_ = false;                            // _rsm_quiet
    bool native_action_ = false;                    // _rsm_native_action
    model::pack::BackupGate backup_gate_;
    std::string status_ = "Ready.";
    std::string setup_report_;
    uint64_t list_version_ = 1;

    model::vslots::VSlots vslots_;
    std::optional<std::string> vslots_char_;
    uint64_t vslots_version_ = 1;
    uint64_t vslots_imports_ = 0;

    std::deque<AllocAction> queue_;
};

// The run-length timeline of a slot entry (pack `timeline`): the strings up to the first hole (ipairs).
std::vector<std::string> timeline_lines(const json& timeline);

}  // namespace model::files
