// SF6 Slots -- long names scroll inside their place instead of being squeezed or cut (name_scroll.cpp, scroll_logic.hpp).
#pragma once

#include <cstdint>
#include <string>

namespace slots::namescroll {

// Every LateUpdateBehavior: the Costume rows and the Opponent Outfit summary, on their screens. A look every 15 frames while none is up:
// registry reads only.
void frame();

// After LateUpdateBehavior: a Costume row's sliding name back where it was slid, if the game's focus animation moved it this frame, before it is
// drawn. Looks at nothing while no row's name slides.
void post_frame();

// One text, found this frame (R35): measured, and slid when it is wider than its place; left as the game has it when it fits. `width`: its
// place's width, 0 its region's. `poll`: frames between looks while it isn't moving (1 for a text the game writes itself).
// `allowed` false, or the text hidden: put back as it was found. From LateUpdateBehavior only, as all of this.
void drive(void* text, float width = 0, uint32_t poll = 1, bool allowed = true);
// The same for the value of a spin row (an Options row: the arrows p_MouseDecideOperable_L / _R under `row`): its place runs between
// its arrows, as on a Costume row
void drive_row(void* text, void* row, bool allowed = true);
// Back as it was found, and measured again next time (before moving it or changing its text by other means)
void release(void* text);
// The same, unless it already holds this text
void release_unless(void* text, const std::string& message);
// The scroller holds it: its region, alignment and place are not to be written meanwhile.
bool busy(void* text);
// The same, for a text placed by its left point, whose place may change: `x`, `y` is where it should be now.
bool busy(void* text, float x, float y);
// What this control measured for this text; -1 not yet. 0: it could not be measured.
float known_width(void* text, const std::string& message);

}  // namespace slots::namescroll
