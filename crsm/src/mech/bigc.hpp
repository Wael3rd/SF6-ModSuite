// mech::bigc — the 30-entry SlotData array of the dummy (RecordingManagerTab.lua 929-1038, 1304-1311): the game's 8
// SlotData objects followed by 22 of ours built from the virtual slots, with REAL input buffers (filled from the timelines
// when their content changes). Installed in FighterData.RecordSlots while the pause menu is open (the native Replay page
// renders SlotData[_SlotID]) and around UpdateReversalCheck (the reversal candidates read the virtual recordings); the
// game's own 8-entry array is put back after each, so the game never saves 30 entries.
//
// Kept for the session per FighterData address (bigc); entries re-synced when the files layer's virtual slots change.
// Objects created here are add_ref'd once and never released (the reversal candidates share the buffers, R28); the
// RecordSlots field is assigned add_ref -> exchange -> release (R34). Every function: mech::lock() held, game thread or
// a game hook.
#pragma once

#include <cstdint>
#include <string>

#include "game/gm.hpp"

namespace mech::bigc {

game::gm::MO* fighter_data();               // path B: RecordSetting.GetFighterIndex(fid) -> FighterDataList[i] (the files layer's fid)
bool live(int k0, bool* active, bool* valid, int64_t* weight);   // live_slot(k0) on the array in place
bool set_active(int k0, bool on);

bool install();                             // install_big (menu open)
bool installed();
game::gm::MO* installed_array();            // the 30-entry array while installed for the menu (RMT's `big.arr`)
void uninstall();                           // uninstall_big without the read-back (the replay engine reads back first)
bool push();                                // big_push (reversal trigger), true when the 30 entries are in place
void pop();                                 // big_pop
bool pushed();
void late_pop();                            // a push whose post-hook never ran (frame callback)

bool entry(int n, bool* active, int64_t* weight);   // our SlotData of virtual slot n (bigc.vs[n])
void entry_set_active(int n, bool on);
void sync(int n);                           // sync_big_entry
void check_version();                       // the virtual slots changed since the last sync: resync at the next use
// The array built ahead (frame callback, menu closed, nothing installed or pushed): what install / push build at their
// first use for a fighter (the first menu opening, the first virtual reversal of a fight), a few entries a frame, started
// some frames after a mech event or a change of the files layer's dummy / character (driven by the replay layer's frame).
void prebuild_tick(bool training);
std::string status();

}  // namespace mech::bigc
