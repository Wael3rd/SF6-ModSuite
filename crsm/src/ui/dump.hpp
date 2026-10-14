// ui/dump — the read-only probe dumps (OFF by default: reframework/data/SF6_CRSMNative_data/dump_on.txt, tab_dump_on.txt),
// dump.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include <reframework/API.hpp>

namespace ui::dump {

using MO = reframework::API::ManagedObject;

bool on();                                     // dump_on.txt seen (every 2 s)
void borrowed(MO* t, MO* ref, bool after);     // windows.cpp style(): the first borrow after the switch, before / after
void slot_row(int n, bool focused, MO* N, MO* name, MO* centre, MO* sub, MO* R);   // Record row updater's post
void spin_labels(int64_t row, size_t before, size_t after);                        // manager_page.cpp: labels change, shown
void frame(bool paused);                       // frame callback (game thread, ui::lock held)
// The game's tab methods, once per start when reframework/data/SF6_CRSMNative_data/tab_dump_on.txt exists (OFF by
// default): appended to tab_methods.txt next to it. Plugin start (ui::start), read-only.
void tab_methods();
bool tab_switch();                             // tab_dump_on.txt is there (also: dialogs.cpp surveys two code addresses)

}  // namespace ui::dump
