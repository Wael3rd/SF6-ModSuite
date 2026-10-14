// model::replay — the "replay" layer of the port (phase P3b): the weighted draw over
// slots 1-30 inside the game's SettingRecodData, the slot 8 discipline at the menu edges, the 30-entry SlotData array
// the native Replay page reads while the menu is open. Sources: RecordingManagerTab.lua (RMT) 1045-1116 (virtual_checked,
// full_pick, write_slot8, load_virtual, unload_virtual, on_menu_open / on_menu_close), 1246-1275 (the SettingRecodData
// hook), 1023-1037 (uninstall_big's read-back), 1525-1537 (Delete of a virtual slot), 2101-2102 (after an import).
//
// Pure: the game is behind `Port` (the dummy's live slots on the array in place = path B, slot 8 through the files layer,
// the 30-entry array); the DLL plugs the game in (mech/layer_replay.cpp), the bench a fake (tests/p3_tests.cpp).
//
// Invariant kept from the Lua: slot 8 holds a virtual slot only while the menu is closed (it gets its own content back
// at each menu open, before the rows are built). Quirks kept on purpose (Wael's rule: no other difference):
//  * an import clears `loaded` but keeps the snapshot of slot 8 (RMT:2101);
//  * a virtual written into slot 8 names slot 8 with its own name until it is unloaded (MAIN's apply names slot 8).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

#include "draw.hpp"
#include "nlohmann/json.hpp"
#include "vslots.hpp"

namespace model::replay {

using json = nlohmann::json;

constexpr int kWorkSlot = 7;                       // live slot 8 (0-based)

struct Live { bool present = false; bool active = false; bool valid = false; int64_t weight = 1; };

class Port {
public:
    virtual ~Port() = default;
    // live_slot(k0) of the array in place (the game's 8, or our 30 while installed: entries 0-7 are the game's objects)
    virtual bool live(int k0, Live* out) = 0;
    virtual void set_active(int k0, bool on) = 0;
    // slot 8 through the files layer: read_slot(7) (nullopt = the Lua's nil), write_slot(entry) (silent import)
    virtual std::optional<json> read8() = 0;
    virtual void write8(const json& entry) = 0;
    // the 30-entry array (install_big / uninstall_big), the cached virtual entries (bigc)
    virtual bool install() = 0;                    // menu open: false = no fighter / no 8-array
    virtual bool installed() = 0;
    virtual bool entry(int n, bool* active, int64_t* weight) = 0;   // installed array's entry of virtual slot n
    virtual void entry_set_active(int n, bool on) = 0;
    virtual void uninstall() = 0;                  // the game's 8 back (the objects the game put into ours kept)
    virtual void sync(int n) = 0;                  // sync_big_entry(n) (nothing while no array was built)
};

// What the SettingRecodData pre-hook leaves for its post-hook (per call: thread-local in the DLL, R29).
struct Draw {
    bool active = false;
    int n = 0;                                     // our draw (1-30)
    bool saved[8]{};
    std::map<int, bool> saved_vs;                  // the 22 entries while the menu is open (Start Replay)
    bool has_vs = false;
};

class Engine {
public:
    // on_menu_open (after the other layers' menu-open work): slot 8's own content back, forced8 off, the 30-array in place
    void menu_open(Port& p, const model::vslots::VSlots& v);
    // on_menu_close: the read-back of the 22 entries (`apply_patch` gets {"n": [active, weight]}, the files layer
    // compares and writes), the game's 8 back, then the longest checked virtual into slot 8 and slot 8 forced active
    // when no live slot 1-7 is
    void menu_close(Port& p, const std::function<const model::vslots::VSlots&()>& vslots,
                    const std::function<void(const json&)>& apply_patch);
    // the SettingRecodData hook: our draw among the checked slots, only the winner left active (a virtual rides slot 8).
    // Unlike the Lua (RMT:1116), slot 8's own recording is drawn too when the player checked it (13/10)
    bool pre_draw(Port& p, const model::vslots::VSlots& v, model::draw::Rng& rng, Draw* d);
    void post_draw(Port& p, const Draw& d);
    // RemoveRecord on virtual row n (after the files layer dropped it): the cached entry follows, slot 8 forgets it
    void forget(Port& p, int n);
    // an import replaced slots 9-30 (RMT:2101-2102)
    void imported(Port& p);
    // the record layer's arm: slot 8's own content back before its snapshot (RMT:514)
    void unload_if_loaded(Port& p);
    // the record layer wrote slot 8's own content back with the game running (a virtual recording that ended on its own):
    // no virtual rides slot 8, nothing kept aside (forced8 kept); the next draw of a virtual loads it again. Before (Wael
    // 14/10): the virtual the menu close had loaded stayed "loaded", so its draw played slot 8's own recording as "Slot n"
    void own8_back();
    // a new character (vslots_load's vplay reset)
    void char_changed();
    // the REC / replay HUD: the virtual riding slot 8 while the game replays slot 8
    int label(bool replay_now, int64_t slot_id) const { return (loaded_ && replay_now && slot_id == kWorkSlot) ? loaded_ : 0; }

    int loaded() const { return loaded_; }
    bool forced8() const { return forced8_; }
    bool has_snapshot() const { return snap_ == Snap::Entry; }
    const json& snapshot() const { return snapshot_; }

    // hand-back: everything this layer left in the game undone (slot 8, forced8, the array)
    void hand_back(Port& p);

private:
    void write8_keep(Port& p, const json& entry);          // write_slot8: slot 8's IsActive preserved
    void load_virtual(Port& p, const model::vslots::VSlots& v, int n);
    void unload_virtual(Port& p);
    bool own8_recorded(bool valid_now, double weight_now, double* w) const;   // slot 8's own content is a recording (its weight)

    int loaded_ = 0;                                       // vplay.loaded (0 = nil)
    enum class Snap { None, Failed, Entry } snap_ = Snap::None;   // vplay.snapshot: nil / false / the entry
    json snapshot_;
    bool forced8_ = false;
};

// fill_virtual's content signature of an entry (RMT:953): "0", or frames|#timeline|first|middle|last.
std::string fill_signature(const json* entry);

}  // namespace model::replay
