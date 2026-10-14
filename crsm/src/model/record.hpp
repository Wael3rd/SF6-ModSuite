// model::record — the "record" layer of the port (phase P3d): a recording into a
// virtual slot n (9-30) goes through live slot 8. Armed when the game's own F lands on a Record row showing n (its
// _SlotID is 7): slot 8 is kept aside, the game records slot 8 as usual, then the result becomes slot n and slot 8 gets
// its own content back. Sources: RecordingManagerTab.lua (RMT) 497-519 (state, begin_virtual_recording), 551-588
// (virtual_recording_tick, finalize_virtual_recording), 1899-1918 (the F on a Record row: re-target / drop / arm),
// 1971 (menu opened while recording: finalized before the rows are built).
//
// Pure: slot 8 through the files layer and the virtual slots behind `Port`; the clock is the caller's frame count.
//
// Kept on purpose (Wael's rule: no other difference than the two decided fixes):
//  * the second restore of slot 8 after the unpause (`vrestore`) is processed by the watch only, and the watch runs only
//    while a recording is armed: as in the Lua it never fires after a finalize (RMT:2017 gates the tick on vrec), and a
//    new arm cancels it. Reported, not fixed.
// Fixed (Wael 14/10, "a slot > 8 I record takes the name and the random selection rate of another slot > 8"): only the
// recording moves from slot 8 into n (recorded_entry). The Lua (RMT:576-578) stored slot 8's whole entry and kept n's
// own fields only where n had them: a new n got slot 8's weight and name, an unnamed n slot 8's name. Slot 8's are not
// even its own then: the F arms with the menu open, the close that follows loads the longest checked virtual into slot 8
// (replay::Engine::menu_close) and MAIN's apply names slot 8 after it, so the game records over a slot named like it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace model::record {

using json = nlohmann::json;

constexpr int kWorkSlot = 7;                   // live slot 8 (0-based)
constexpr int kRecordNow = 3;                  // RecordState.RECORD_NOW
constexpr uint64_t kDeadline = 60 * 120;       // ticks: 2 minutes
constexpr uint64_t kRestoreDelay = 15;         // ticks after the unpause
constexpr uint64_t kTickEvery = 10;            // the watch runs every 10 ticks (also under the pause)

class Port {
public:
    virtual ~Port() = default;
    virtual std::optional<json> read8() = 0;               // read_slot(7)
    virtual void write8(const json& entry) = 0;            // write_slot (silent import into slot 8)
    virtual const json* vslot(int n) = 0;                  // vslots[n] (the files layer's model)
    virtual void store(int n, const json& entry) = 0;      // vslots[n] = e + the file
    virtual int64_t frames(const json& entry) = 0;         // entry_frames
    // finalize wrote slot 8's own content back with the game running (the recording ended on its own, or the deadline):
    // no menu open follows to unload the replay layer's virtual, so its "virtual m rides slot 8" is stale now
    virtual void own8_back() {}
};

// The entry a recording into virtual slot n stores: `rec` (slot 8 as read_slot gives it) without slot 8's own fields
// (name, weight, active), id = n, and n's own name / weight / active as they are (absent ones stay absent); no entry
// yet: a new virtual slot's defaults (weight 1, no name, not checked).
json recorded_entry(int n, const json& rec, const json* old);

class Engine {
public:
    // begin_virtual_recording (RMT:510-519): the caller has given slot 8 its own content back (the replay layer's unload)
    void arm(Port& p, int n, uint64_t now);
    bool armed() const { return armed_; }
    int slot() const { return armed_ ? n_ : 0; }
    bool seen_now() const { return seen_now_; }
    // the F on a Record row while a recording is armed but never started (RMT:1905-1907)
    void retarget(int n) { if (armed_) n_ = n; }
    void drop() { armed_ = false; seen_now_ = false; snapshot_.reset(); }
    // finalize_virtual_recording (RMT:570-588); slot 8 written back while not `paused`: Port::own8_back (fix 14/10)
    void finalize(Port& p, bool paused);
    // virtual_recording_tick (RMT:551-569); `state` = RecordFunc._gData.State (0 when unreadable). Call every kTickEvery
    // ticks while armed (the Lua's LUB gate), paused included. Returns true when it finalized.
    bool tick(Port& p, uint64_t now, int64_t state, bool paused);
    // the menu's CreateViewData: a recording that started is stored and slot 8 restored before the rows are built
    void menu_open(Port& p) { if (armed_ && seen_now_) finalize(p, true); }
    // the REC HUD: the slot being recorded
    int label() const { return armed_ ? n_ : 0; }
    bool restore_pending() const { return restore_.has_value(); }
    // Fallback: slot 8, as the user recorded it with the game's own page, stored into
    // slot n (recorded_entry, as finalize). false = slot 8 empty.
    bool capture8(Port& p, int n);
    // hand-back: an armed recording dropped, slot 8 written back from its snapshot
    void hand_back(Port& p);

private:
    void store_from8(Port& p, int n, const json& e);

    bool armed_ = false;
    int n_ = 0;
    std::optional<json> snapshot_;               // slot 8 at the arm (nullopt = read_slot gave nil)
    bool seen_now_ = false;
    uint64_t deadline_ = 0;
    std::optional<json> restore_;                // vrestore.snap
    std::optional<uint64_t> restore_at_;         // vrestore.at
};

}  // namespace model::record
