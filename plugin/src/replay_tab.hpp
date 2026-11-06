// SF6 Slots — "Replay Outfits": a 6th tab of the Replays menu, after Search (replay_tab.cpp; logic: replay_logic.hpp).
#pragma once

#include <cstdint>

namespace slots::replaytab {

// Every LateUpdateBehavior after the closing guard, before the plugin's event gate: its own gate (one core counter read
// every 15 frames while the Replays screen is not up, nothing else), installs its hooks on the first call.
void frame();

}  // namespace slots::replaytab
