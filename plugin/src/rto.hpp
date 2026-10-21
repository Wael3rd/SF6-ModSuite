// SF6 Slots — runtime costume options (docs/RUNTIME_OPTIONS.md), the game side.
//
// rto.cpp: the manifest and the choices, the models (the Battle Settings preview, each fighter), the
// resources each one needs (created ahead, applied once ready), when to apply.
// rto_menu.cpp: the F hint on the costume row of Fighter Settings and the options menu (the core's
// option service). rto_select.cpp: the same options in a side's panel of the character select and in the Training
// pause menu's character popup.
//
// Rules (slots.hpp): game objects only from LateUpdateBehavior; nothing while the character select
// is open; in a fight, work only after an event (a fighter's model built), never per frame. Models,
// components and holders the game gave are never add_ref'd (R21): they are found again and checked
// alive (is_managed_object + type) before each use. The holders this module creates are its own.
#pragma once

#include "slots.hpp"
#include "rto_data.hpp"

namespace slots::rto {

// ---- module (main.cpp) ----
void init();                          // manifest + choices, before costumes::init (former migration)
void install_hooks();                 // first frame
bool busy();
void tick(uint32_t events);

// ---- former variant slots (costumes.cpp, load_state) ----
// A Battle Settings choice naming a costume number the registry no longer has: when the manifest lists
// it as a former variant slot (the launch that turned RuntimeOptions on), the kept slot's number, and
// that variant's options become the P1 choice of the mod. -1 otherwise. game: the kept outfit is a game
// outfit (a retouch's former outfit: the retouch is an option of the game outfit it was made from).
int migrate_former(int fighter, int costume_no, bool* game = nullptr);
void migration_done();                // after load_state: the choices file is written once

// ---- shared with rto_menu.cpp ----
bool on();                            // a valid manifest is loaded
bool nsfw_options();                  // the manifest has NSFW options (costumes.cpp then reads display_nsfw)
const Manifest& manifest();
Choices& choices();
void save_choices();                  // written at once (small file)
void choices_changed();               // the models re-apply (the preview now, fighters at their next build)
// A fighters watch started now (10 s, as after a model build): the fighters (PlayerBehavior, _PlayerID 0 / 1) wear the
// choices of their side again, without a rebuild (the Training popup confirmed with the same costume and colour: the
// game rebuilds nothing). Nothing on the character select. hold_side 0 / 1: that fighter keeps what it wears (its
// files loading meanwhile) until fighters_hold_end
void fighters_refresh(const char* why, int hold_side = -1);
// The held swap released (the Training restart's black screen, or no restart): made now, in one pass when every file is
// ready, else each part as soon as ready
void fighters_hold_end(const char* why);
// app.training.TrainingManager._MenuState (0 NONE, 1 READY, 2 SETUPED, 3 CLOSING, 4 FIRST, 11 CHARA_SELECT) and
// _TrainingState (0 NONE, 1 NOW, 2 REFRESH_START, 3 REFRESH_WAIT); -1 without a TrainingManager
int training_menu_state();
int training_state();
// The frame a fighter's model (side 0 P1, 1 P2: a new GameObject) was last found by a fighters watch, 0 never
uint32_t fighter_built_at(int side);
// PlayerBehavior.SetupAppearance calls so far (a model built), 0 when that event is ignored (called all the time)
uint32_t model_builds();
std::string fade_fields();            // the fade controller singleton's fields, for the log
// The Training popup's options mode: the files of every option of these outfits asked for ahead and kept loaded (the
// Battle Settings preview's rule); an empty list lets them go. How many files are kept.
size_t popup_preload(const std::vector<const Slot*>& slots);
// How many files a set of options needs that are not ready yet (asked for now); total: how many it needs
int options_files_left(const Slot& s, const std::vector<std::string>& chosen, int* total);
// The slot of a fighter's outfit (costume number) when it has option rows (the menus offer it), nullptr otherwise
const Slot* outfit_slot(int fighter, int costume_no);
const Slot* preview_slot();           // the slot the Battle Settings preview wears, nullptr = none with options
int preview_since();                  // the frame the preview was last identified (-1: no slot with options)
// The fighter of the last Battle Settings preview model (its name, "esf001v00" = 1), -1 none; at: the frame it was seen
int preview_fighter(uint32_t* at = nullptr);
int frame_now();
// The outfit a side's model on the character select wears (side 1 / 2) when it has options, nullptr otherwise;
// since: the frame the model was last identified as wearing it
const Slot* select_slot(int side, uint32_t* since);

namespace menu {
void install();                       // the core's option service (first frame)
bool service();                       // the core accepted our option callbacks (rto_menu.cpp owns them)
bool active();                        // the options window is ours and open
int side();                           // the player side the menu edits (0 P1, 1 P2)
bool busy();
// fs_visible: Fighter Settings (Battle Settings > character) is on screen
void tick(bool fs_visible);
// The game's keyboard key of its UI tab action (next: UITabR, the keyboard twin of R1; else UITabL): its virtual key,
// and its glyph ("<ICON KeyE>" on an AZERTY keyboard)
int tab_key(bool next, std::string* sym);
}

// rto_select.cpp: the costume options on the character select (R1 / C in a side's panel: its four rows show the
// outfit's options, the Color Mod's DI mode technique) and in the Training pause menu's character popup (its six
// rows, the changes kept until the popup is confirmed); one owner per panel with the Color Mod's DI mode
namespace select {
void tick();                          // each frame while the character select is up (rto::tick)
// The panels given back (when still alive), state reset: the select's (the screen left) and the popup's (its pending
// changes dropped)
void leave_all(const char* why);
void install_popup_hooks();           // first frame: what tells the popup is up (EV_POPUP), its flow's update
bool popup_busy();                    // the popup is up (or was just announced): popup_tick each frame
void popup_tick(bool announced);      // each frame while popup_busy() or EV_POPUP (rto::tick, not on the select)
bool popup_guard();                   // SF6Slots_PopupGuard
bool active(int side);                // side 1 / 2 shows the options
int claim(int side, int who, int op); // SF6Slots_SelectPanelClaim
int guide_entry(int side, int pad, char* buf, int cap);   // SF6Slots_SelectGuideEntry
}

// rto_camera.cpp: the preview's turn / zoom / pan and the Move / Zoom hints while the menu is open, the same as
// the Color Mod's colour editor on Fighter Settings (driven by menu::tick)
namespace camera {
void open(Obj* fs_agent, bool pad);   // idempotent: every 5 frames while the menu is open on Fighter Settings
void close();                         // camera position restored (if zoomed / panned), hints hidden; safe when off
bool active();
void tick();                          // input and transforms, each frame while open (LateUpdateBehavior)
void post_frame();                    // hint paint, after every LateUpdateBehavior pre callback (main.cpp)
}

} // namespace slots::rto
