// ui/drill_details — the drill details dialog: Confirm (keyboard F, pad A / Cross) on the Manager page's Drill row
// opens the game's own app.training.UITrainingImageDialog (GUI / UIAgent ui11281, the page of "The Frame Meter and
// You") filled with the selected drill: its name, "<Character> - <Author> - <Source>" and its saved slot count, its saved
// slots ten a page, its whole description scrolled 18 lines at a time (R / pad X: the Markdown help in its place),
// "Open drill" / "Cancel". The Manager page (ui/manager_page.cpp) gathers the drill when the core sends the Confirm (core
// 0.9.5, on_confirm) and asks here; "Open drill" runs its Open action, T / pad Y its edit of the description.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "model/pack.hpp"

namespace ui::details {

struct Drill {
    std::string title;                        // the drill's name as the spin shows it
    std::string character;                    // the dummy's character (the drills' folder)
    std::string source, author, description;  // info.source / info.author / info.description of the pack
    std::string own_name;                     // the name the Community shows for the player's shares ("" unknown): the
                                              // top-left box's author of a Local drill without info.author
    std::vector<model::pack::SlotSummary> slots;
};

// Any thread: the dialog asked, opened by the UI's next frame callback (game thread). on_open runs there when the
// player chose "Open drill" (a decide on the left button), after the dialog has ended; on_edit when T / pad Y asked to
// edit the description (the dialog stays up under the text dialog: edit_done ends it). false: the UI is off, or a dialog
// of this kind is already up or asked.
bool request(Drill drill, std::function<void()> on_open, std::function<void()> on_edit);
bool is_open();                               // asked, up, closing, or ended less than 10 frames ago: any thread
// The edit's text dialog closed (game thread): changed = the drill file now holds `description`, shown in place (its lines
// read again, from the top); the dialog takes its keys again
void edit_done(bool changed, const std::string& description);

}  // namespace ui::details
