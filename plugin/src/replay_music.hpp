// SF6 Slots — Replays > Settings: the game's music screens, started from the Battle BGM row (replay_music.cpp).
#pragma once

namespace slots::replaymusic {

// The game's Character Music screen (app.UIFlowCharaBgmSetting), as T opens it on Battle Settings. What it changes is the game's
// own: the fighters' playlists in the save, shared with online. Game thread. False: not started (the log says why).
bool open_character_music();
// A screen started here is still running. Game thread, every frame while one was started.
bool up();

}  // namespace slots::replaymusic
