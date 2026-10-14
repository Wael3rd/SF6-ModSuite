// ui::dialogs — the game's own dialogs driven by the DLL (phase P4, owner "ui"): the text input dialog (slot names, Save
// loadout as, Rename), the Yes / No message box, the ticker strip, the Community dialog of the Manager page. The interface
// the Manager page (phase P5) calls.
//
// Behaviour of the Lua it replaces (RecordingManagerTab.lua RMT, NativeTicker.lua TK):
//  * text_input: app.UITextInputDialog.OpenTextInputDialog(no delegate, RequestType 16 "ReciperMemo", VisualType 1 FG,
//    initial, max_bytes) (RMT:1426-1438); the game's "Maximum 15 characters" line reads T("press_enter"); the pause menu's
//    input and its tab moves are blocked while it is up. `done(ok, text)`: ok = something to apply (a decide, even with the
//    initial text; or a close without a decide after the text changed); ok = false for a cancel, a close with nothing
//    typed, or a refusal (another text dialog or a message box is up, the dialog could not open, the UI is off). The text
//    is handed over as typed: max_bytes is the field's maximum length (the game counts characters), the 64-byte cut of a
//    slot name on a UTF-8 boundary is the model's (files layer). `title` names the request in the journal (the game's
//    text dialog shows no title).
//  * confirm: app.UIFlowDialog.MessageBox.Start(text, title, 1, 1, YesNo, no icon, FightingGround), polled while the
//    menu is paused; done(true) on the first button (Yes). Only one box at a time: a second request gets done(false). When
//    the box cannot be created the action runs without confirming: done(true) (RMT:80-87).
//  * ticker: the game's ticker strip, 3 s, category 6 (RMT:79, TK:24-41).
//  * community_open / community_share: the Manager's "Download Loadouts" and "Share loadout" (RMT:396-429).
//
// All run their work on the game thread (frame callback); 'done' is called there too.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace ui::dialogs {
// All run their work on the game thread (frame callback); 'done' is called there too.
void text_input(const std::string& title, const std::string& initial, size_t max_bytes,
                std::function<void(bool ok, const std::string& text)> done);
void confirm(const std::string& text, std::function<void(bool yes)> done);
void community_open();    // Manager "Download Loadouts"
void community_share();   // Manager "Share loadout"
void ticker(const std::string& text);

// ---- beyond the minimum interface (P4) ----
// The message box with its title: the Manager's boxes are titled ("Save loadout", "Save loadout as", "Reset loadout",
// "Erase loadout", RMT:116-141). The two-argument confirm above shows no title.
void confirm(const std::string& text, const std::string& title, std::function<void(bool yes)> done);
// A text dialog or a message box is up, or the Community dialog: the Lua's guards (`if naming or confirm_box then
// return end`, RMT:1459, 1464; T / G / R ignored, RMT:1762) for the keys of the Manager page (T = rename).
bool busy();
}
