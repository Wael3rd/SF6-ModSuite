// model::vslots — the virtual slots 9-30 of a character: <char>/_virtual_slots.json (keys "9".."30"), their entries,
// weights, texts and the pack export / import. Sources: RMT:461-491 (storage, vweight, entry_frames, text),
// 2049-2104 (export_virtual, virtual_rows, set_virtual, set_virtual_entry, import_virtual), 1408-1418 (set_slot_name).
//
// An entry is the json object of a pack entry plus an `active` flag: {id, weight, name?, timeline[], empty, active?}.
// Keys of the file that are not whole numbers are not kept (Lua would keep a fractional key; the mod never writes one).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace model::vslots {

using json = nlohmann::json;

constexpr int kFirst = 9, kLast = 30;           // SLOT_TOTAL = 8 + VIRTUAL_SLOTS
constexpr size_t kNameMax = 64;                 // NAME_MAX: bytes of a slot name (cut on a character boundary)

// What a row of the pages shows for a virtual slot (RMT:2052 virtual_rows).
struct Row { int n; std::string name; bool active; double weight; int64_t frames; };

class VSlots {
public:
    // vslots_load: the decoded file (luajson::load_file). Not a table: no slot. Keys that are numbers are kept (any
    // number, as Lua; only 9-30 are used).
    void load(const json& file);
    // vslots_save: {"9": entry, ...} for every slot held. Empty: an empty table (written as null).
    json to_file() const;

    const json* get(int n) const;
    double weight(int n) const;                           // vweight: math.max(1, tonumber(weight) or 1)
    int64_t frames(int n) const;                          // entry_frames
    std::string text(int n, const std::string& empty_word = "Empty") const;   // virtual_slot_text: "123F" or the empty word
    std::string name(int n) const;                        // slot_display_name for n >= 9
    std::vector<int> checked() const;                     // virtual_checked: active and with content, sorted
    std::vector<Row> rows() const;                        // virtual_rows: slots 9-30

    // export_virtual: the 22 entries 9-30 of a pack. A slot with content is its entry with id = n, empty = false and no
    // `active`; any other is {id, empty = true, timeline = {}, weight, name = ""}.
    json export_virtual() const;
    // import_virtual: the entries with id 9-30, with content, replace the slots; a boolean `active` in the entry is kept
    // (a pack that carries the flags), otherwise the slot keeps the flag it had (a non-boolean one in the file is dropped).
    // Slots absent from `list` are emptied (an 8-slot pack empties 9-30).
    void import_virtual(const json& list);
    // set_virtual: name / active / weight of a slot, created empty ({id, empty = true, timeline = {}, weight = 1}) when missing.
    struct Fields { const std::string* name = nullptr; const bool* active = nullptr; const json* weight = nullptr; };
    void set_virtual(int n, const Fields& f);
    // set_virtual_entry: a recording put into a slot (id = n, empty = false; active kept, weight and name kept when the entry has none).
    void set_virtual_entry(int n, const json& entry);
    // set_slot_name for n >= 9: the name cut to kNameMax bytes on a character boundary; the slot is created empty when missing.
    void set_name(int n, const std::string& name);
    // vslots[n] = nil (RemoveRecord on a virtual row), vslots[n] = e (finalize of a virtual recording), and a mutable view.
    void erase(int n) { slots_.erase(n); }
    void put(int n, const json& e) { slots_[n] = e; }
    json* mut(int n) { auto it = slots_.find(n); return it == slots_.end() ? nullptr : &it->second; }

private:
    std::map<int64_t, json> slots_;
};

}  // namespace model::vslots
