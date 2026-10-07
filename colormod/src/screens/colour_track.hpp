// screens::colour::track — the colour screens' tracking (phase 5c-2 step 2, docs/CPP_PORT_PHASE5.md section 8): the
// port of SF6_ColorSpinExtra.lua's tick() (tick_menu for the two select menus, tick_fighter_settings,
// tick_training_settings, the guide hints and corners, the DI panel / Drive rows hosts, the side hint), of its late
// steps (the training popup's refresh, the save follow-up costume +1 / -1, the restore walks), of device_tick and the
// select screens' guide part of editor_tick, and of decide_pull / decide_push (the selection memory: state.json).
//
// Since phase 5 the DLL always tracks and acts (owner "screens_colour" = "native"). The shadow
// comparison mode (shadow_tick / shadow_late / shadow_end) is removed.
#pragma once

#include <cstdint>
#include <string>

#include "../engine/rsz.hpp"

namespace screens::colour::track {


// the editor's orders that reach the screens (dialogs/editor.cpp emits them; the Lua relay applies its own copy)
void refresh_request(bool has_path, const std::string& path);             // "RL": refresh_list(path)
void catalog_rebuilt();                                                     // "CAT": build_catalog (entries start without chips)
void invalidate();                                                          // "INV": invalidate_caches
void chips_reload();                                                        // "CHIPS": chip_cfg read again

// ---- owner "screens_colour" (phase 5c-2): the DLL tracks and acts; the Lua only relays the option orders (until 5d) ----
bool install();                                                             // the two Esc hooks (InputCancel, training menu close)
void frame(uint64_t lua_frame);                                             // pull, tick, late steps, triggers, editor loop, push
bool acting();
// the editor's orders that the relay used to apply on the screens, done here while the DLL acts
void editor_refresh();                                                      // "RF"
void apply_original(int color_id, const engine::rsz::NodeRef& tree);       // "AO"
void teardown();                                                            // "TD" (agents unblocked, training menu input back)

std::string status();

}  // namespace screens::colour::track
