// dialogs::editor::look - the editor's own row looks in the game's option window (0.20.0, ADR-0020 amendment 1):
// the avatar editor's rainbow under the hue sliders. Every write goes through the core's display API
// (ui_clone_control, ui_set_texture_rect, ui_set_color...). Without core 0.8.2 the rows keep the option window's look
// (one log line); when the rainbow's texture does not load, the hue sliders keep theirs.
//
// The bar lives in the dialog's own tree (under the slider's c_gage): it goes with the dialog. A row that stops being a
// hue row gets the game's look back (bar hidden, the gauge fill shown) the first time it is seen on a page.
#pragma once

#include <string>
#include <vector>

#include "editor_core.hpp"

namespace dialogs::editor::look {

using RowControlFn = void* (*)(int eid, void* param);   // the editor's entry -> its row's control on that page

void preload();              // the editor opens: the rainbow's texture starts loading (once per game session)
// every frame while the editor's window is up: the page on top (param) and its slider rows
void apply(void* param, const std::vector<core::RowLook>& rows, RowControlFn row_control);
void forget();               // the editor closed: its dialogs, and the copies in them, are gone
std::string status();

}  // namespace dialogs::editor::look
