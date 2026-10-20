// SF6 Slots — runtime costume options (docs/RUNTIME_OPTIONS.md), the game side.
//
// rto.cpp: the manifest and the choices, the models (the Battle Settings preview, each fighter), the
// resources each one needs (created ahead, applied once ready), when to apply.
// rto_menu.cpp: the F hint on the costume row of Fighter Settings and the options menu (the core's
// option service).
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
// that variant's options become the P1 choice of the mod. -1 otherwise.
int migrate_former(int fighter, int costume_no);
void migration_done();                // after load_state: the choices file is written once

// ---- shared with rto_menu.cpp ----
bool on();                            // a valid manifest is loaded
const Manifest& manifest();
Choices& choices();
void save_choices();                  // written at once (small file)
void choices_changed();               // the models re-apply (the preview now, fighters at their next build)
const Slot* preview_slot();           // the slot the Battle Settings preview wears, nullptr = none with options
int preview_since();                  // the frame the preview was last identified (-1: no slot with options)
int frame_now();

namespace menu {
void install();                       // the core's option service (first frame)
bool active();                        // the options window is ours and open
int side();                           // the player side the menu edits (0 P1, 1 P2)
bool busy();
// fs_visible: Fighter Settings (Battle Settings > character) is on screen
void tick(bool fs_visible);
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
