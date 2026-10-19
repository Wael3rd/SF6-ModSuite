// slots_ui.hpp -- the plugin's ONLY interface for the ported display operations.
//
// Each function tries the sf6core display API when the core is present, then
// falls back to the legacy path (compiled only with SLOTS_LEGACY_UI_FALLBACK).
//
// Removing the legacy fallback later:
//   1. delete slots_ui_legacy.cpp
//   2. delete the SLOTS_LEGACY_UI_FALLBACK define in build.bat
//   3. delete the #ifdef SLOTS_LEGACY_UI_FALLBACK branches in slots_ui.cpp
// Nothing else.
#pragma once

#include "slots.hpp"
#include <sf6core/sf6core_api.h>

namespace slots::ui {

// ---- lifecycle ----
// Call init() from reframework_plugin_initialize (safe before core init:
// registrations are queued, ADR-0002/0007).  log_path() logs once which
// display path is active on the first LateUpdateBehavior frame.
void init();
void log_path();
// The core and this plugin's client, for the modules that use its services directly (the option
// service: rto_menu.cpp). nullptr without the core.
const SF6CoreAPI* core();
SF6CoreClient* client();

// ---- P1: data injection (SL-07, SL-08, SL-09) ----
// clone: MemberwiseClone.  Returns an object with one caller-owned ref
// (core) or add_ref'd (legacy).  Caller must NOT call add_ref again.
Obj* clone(Obj* src);
// set_i64: any integer / bool / enum field, regardless of width.
bool set_i64(Obj* obj, const char* field, int64_t v);
// set_ref: managed reference assignment (core: add_ref new, exchange, release
// old; legacy: raw pointer write -- the caller has already add_ref'd via clone).
bool set_ref(Obj* obj, const char* field, Obj* value);
// set_color: write a via.Color value field (RGBA, R in the low byte).
bool set_color(Obj* obj, const char* field, uint32_t rgba);
// dict_contains: Dictionary ContainsKey.  *ok set when the call succeeded.
bool dict_contains(Obj* dict, int64_t key, bool* ok = nullptr);
// dict_add: Dictionary Add(key, value).
bool dict_add(Obj* dict, int64_t key, Obj* value);
// list_range: List GetRange.  Returns a new list with one caller-owned ref.
Obj* list_range(Obj* list, int32_t start, int32_t count);
// list_set: List set_Item.
bool list_set(Obj* list, int32_t index, Obj* item);
// list_add: List Add.
bool list_add(Obj* list, Obj* item);

// ---- P2 + P3: text and texture (SL-13, SL-14, SL-15) ----
// set_text: set_Message with compare-before-write.
bool set_text(Obj* text_obj, const char* utf8);
// tex_load: create a TextureResourceHolder for a resource path.
// Returns a holder with one caller-owned ref.
Obj* tex_load(const char* resource_path);
// tex_ready: true once the texture has finished loading (>= 60 frames).
bool tex_ready(Obj* holder);
// set_tex: setTexture with compare-before-write.
bool set_tex(Obj* tex_obj, Obj* holder);

// ---- effective selection (core 0.7.0+; nothing to do without the core) ----
// set_effective: the fighter the game shows in `shown_costume` really wears
// `real_costume`, colour `real_colour` (-1 = the colour the game shows), for
// the other mods that read its costume / colour; real_costume < 0 clears.
void set_effective(int fighter, int shown_costume, int real_costume, int real_colour);

// ---- GUI material texture (core 0.8.1+, ADR-0019; nothing without it) ----
// has_mat_tex: the core has ui_get/set_material_texture.
bool has_mat_tex();
// get_mat_tex: the holder the material's texture variable `param` shows now,
// with one reference owned by the caller (release() it); nullptr if none
// (*status: the core's SF6CoreStatus, -1 without the op).
Obj* get_mat_tex(Obj* material, const char* param, int* status = nullptr);
// set_mat_tex: the variable shows `holder` (from tex_load and ready, or from
// get_mat_tex); compare-before-write in the core.
bool set_mat_tex(Obj* material, const char* param, Obj* holder);

} // namespace slots::ui

// ---- legacy declarations (compiled only with SLOTS_LEGACY_UI_FALLBACK) ----
#ifdef SLOTS_LEGACY_UI_FALLBACK
namespace slots::ui::legacy {
    Obj* clone(Obj* src);
    bool set_i64(Obj* obj, const char* field, int64_t v);
    bool set_ref(Obj* obj, const char* field, Obj* value);
    bool set_color(Obj* obj, const char* field, uint32_t rgba);
    bool dict_contains(Obj* dict, int64_t key, bool* ok);
    bool dict_add(Obj* dict, int64_t key, Obj* value);
    Obj* list_range(Obj* list, int32_t start, int32_t count);
    bool list_set(Obj* list, int32_t index, Obj* item);
    bool list_add(Obj* list, Obj* item);
    bool set_text(Obj* text_obj, const char* utf8);
    Obj* tex_load(const char* resource_path);
    bool tex_ready(Obj* holder);
    bool set_tex(Obj* tex_obj, Obj* holder);
} // namespace slots::ui::legacy
#endif
