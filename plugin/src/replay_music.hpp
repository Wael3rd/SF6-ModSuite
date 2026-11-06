// SF6 Slots — Replays > Settings: the game's music screens, started from the Replay Music row, and the row's Custom list at a
// replay's load (replay_music.cpp).
#pragma once

#include "slots.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace slots::replaymusic {

// Every frame, from the first one: the R18 check of the hooks, then their install. Nothing once done.
void frame();

// The game's text of an input id in the guide bar, in the player's language ("" when not found). Game thread.
std::string guide_text(int input_id);

// The game's Character Music screen (app.UIFlowCharaBgmSetting), as T opens it on Battle Settings. What it changes is the game's
// own: the fighters' playlists in the save, shared with online. Game thread. False: not started (the log says why).
bool open_character_music();
// The game's playlist dialog alone (app.UIFlowCharaBgmSelect), no fighter list before it, on a list of ours: `list` marked, `title`
// as its heading. Nothing it does reaches the save: its parent screen is what writes. False: not started (the log says why).
bool open_custom_list(const std::vector<uint32_t>& list, const std::string& title);
// A screen started here is still running. Game thread, every frame while one was started.
bool up();
// The dialog just closed: what it had marked, once. False: there was no dialog (Character Music), or it is taken.
bool take_custom_list(std::vector<uint32_t>& out);
// Started state dropped, the screen under them gone. Game thread.
void forget();

// The tracks the player owns, the dialog's own list: empty when it cannot be read. Any thread.
std::vector<uint32_t> owned_tracks();

// A replay's description was just set up on this flow map, Custom with this list: while the game loads Player1's music for this
// map, the copy of the playlist it asks for gets this list instead. The save and the fighters' lists are never written.
// `map` nullptr: nothing armed. Any thread.
void arm_replay(Obj* map, std::vector<uint32_t> list);
bool can_swap();      // the hooks of the swap are in

}  // namespace slots::replaymusic
