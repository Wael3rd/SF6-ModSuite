// SF6 Slots -- an interface pack of portraits per character, as a grid of characters in Options > Interface (ui_packs_grid.cpp): the
// section is the core's section with a page of ours (opt_section_place), the grid the Replays' Outfit Settings' (the characters'
// cells, "On" over a character that has a choice), the choice on a cell as on Stage Settings (R / pad X shows the next portrait on
// the cell, Confirm chooses it). The packs and their rules: ui_packs.cpp, ui_packs_logic.hpp (grid_of).
#pragma once

#include "ui_packs_logic.hpp"

#include <cstdint>
#include <vector>

namespace slots::uipacks::gridpage {

struct Section {
    int64_t group = -1;                     // the core's section
    const Pack* pack = nullptr;             // valid until the next drop()
    const Grid* grid = nullptr;
};
using TicksFn = Ticks (*)(const Pack&);                                    // the pack's ticks now
using ApplyFn = void (*)(const Pack&, const Ticks&, const char* why);       // the pack's ticks changed: written and applied

// Every frame, on the game thread (LateUpdateBehavior)
void frame(const std::vector<Section>& secs, TicksFn ticks, ApplyFn apply);
// The sections are being made again: the page let go of them (the hold given back, nothing shown)
void drop();

}  // namespace slots::uipacks::gridpage
