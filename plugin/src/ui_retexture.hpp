// SF6 Slots -- live texture overrides of the game's interface (ui_retexture.cpp; the pure part: ui_retexture_logic.hpp).
//
// An element on screen (via.gui.Texture) that shows a file an override replaces is given the override's texture at once, and
// the elements that appear later too; an override dropped gives the game's own texture back. For the files the game loads once
// and keeps for the session (the character select's icons), which the loader's redirect cannot change after boot.
#pragma once

#include <string>

namespace slots::ui_retexture {

// The overrides now: lines "<real resource path>\t<resource path to show instead>\n" (what the loader's SF6UISlots_Overrides
// gives); "" no override (every element we changed gets the game's own texture back). Paths are matched in lower case, without
// "natives/stm/" nor a version number. Any thread: kept, and applied from the next LateUpdateBehavior on.
void set_overrides(const std::string& text);

// The loader's overrides now (amd_ags_x64.dll's SF6UISlots_Overrides(char* buf, int cap): its text's length, a length >= cap
// asking for more room, < 0 when the UI slots are not running), then set_overrides. Without that export: nothing (logged once).
// Any thread.
void refresh_from_loader();

// Every LateUpdateBehavior (main.cpp). Without any override nor anything of ours left on screen: one load of this module's own
// memory, nothing else.
void frame();

}  // namespace slots::ui_retexture
