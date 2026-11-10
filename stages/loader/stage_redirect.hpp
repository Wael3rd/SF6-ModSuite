#pragma once
#include <vector>

#include "stage_loader.hpp"

// Stage slots, runtime part: serves the selected variant of a stage in place of its files.
//
// The engine turns every file path into a pak hash through one function (path_to_hash). A
// detour on it swaps the hash of a vanilla file the selected variant replaces for the hash its
// copy has in our pak (stage_loader.hpp), so the game loads the variant without knowing.
//
// The selection comes from reframework\data\SF6_StageSlots_Data\state.json, written by the Lua
// script on the stage select screen and re-read when it changes:
//   { "selected": { "<stage id>": "<variant key>", ... } }
// Nothing is hooked while nothing is served (no variant selected, the title screen and the UI
// packs as this launch wrote them), and never in the first seconds of the game
// (other tools hook the same function at their first frame and scan for it before patching).
// Once installed, the hook is disarmed (original bytes back) whenever the selection is empty
// again and armed again when a variant is selected. The detour itself is one bit test for any
// path it does not serve (the engine hashes paths on many threads, all fight long).
// Optional debug.txt in the same folder: "trace=<text>" logs every hashed path holding <text>.
// Optional perf_on.txt in the same folder: a "perf:" line every 5 s (calls per second, threads,
// sampled cycles of the original and of the detour's own work); no cost while absent.
// Optional hook_off.txt (test switch): the hook stays disarmed while it exists.
// Log: SF6_StageSlots.log next to the game executable.

// Starts the watcher thread. Returns at once; safe to call from DllMain. `variants` must stay
// alive for the whole session.
void stage_slots_start(const wchar_t* game_dir, const std::vector<StageVariant>* variants);

// The selection applied now, from any thread: the plugin's per-menu choice at the moment a battle, a training stage or a
// replay is set up, ahead of the stage's first file (the watcher would read state.json a few ms later; the plugin writes it
// too, for the next start). state_json: the text of state.json ("selected" is read, the rest ignored). Returns 2 when a
// variant is served, 1 when none, 0 when the stage slots are not running. Exported by amd_ags_x64.dll as
// SF6StageSlots_Apply.
int stage_slots_apply(const char* state_json);

// The title screen's backgrounds (stage_loader_titles), before stage_slots_start. Every visit of the title reads its movies
// (title_start / title_loop / title_end, read 07/11) through path_to_hash: the chosen background's copies are served in place
// of the real files, so a change in the Options shows at the next return to the title without a new launch. Nothing is served
// while the choice is the background this launch wrote at the real paths. On "random", another background is drawn after each
// visit (title_slots_next), another than the last when there are several.
void title_slots_set(const TitleTables& tables);
// On "random": the next background drawn now (the plugin once the title screen has been left a moment; the core's title.cpp
// while the title's loop plays, nothing being prepared). 1 when one was drawn, 0 on another choice. Exported as SF6TitleSlots_Next.
int title_slots_next();
// On "random": another background for the next cycle of the visit on screen (the core's playlist: it gives the title's movies the
// copies' own paths, nothing served changes). Its key in buf (another than not_key when there are several), its length; 0 on
// another choice. Exported as SF6TitleSlots_Draw.
int title_slots_draw(const char* not_key, char* buf, int cap);
// The choice made in the game (the plugin, Options > Interface > Title Screen): "" the game's own, a pack's key, "random".
// Exported by amd_ags_x64.dll as SF6TitleSlots_Select.
void title_slots_select(const char* choice);
// The background the title shows (or showed last): "game" the game's own, else a pack's key. Its length, 0 when unknown.
// Exported as SF6TitleSlots_Shown (the core's title.cpp: the OVERDRIVE EDITION line of that background).
int title_slots_shown(char* buf, int cap);
// Random's selection changed in the game (the preview's list): comma-separated keys ("game" the game's own). Exported as
// SF6TitleSlots_Pool.
void title_slots_pool(const char* keys);

// The UI packs (stage_loader_ui), before stage_slots_start. The launch wrote its ticked options at the real paths, so nothing is
// served until the choice changes in the game; then each real file whose winner changed is served from its copy, or from the
// game's own copy when nothing ticked has it any more (ui_slots::serve). It shows at the next load of the file (the game keeps
// some for the session: the plugin swaps those on screen, ui_slots_overrides).
void ui_slots_set(const ui_slots::Tables& tables);
// The plugin's whole "ui_choices" object (pack key -> ticked node ids), applied at once. Returns the number of real files served
// from copies now, -1 when the UI slots are not running (no reframework\ui_mods at launch, or the stage slots not started).
// Exported by amd_ags_x64.dll as SF6UISlots_Apply.
int ui_slots_apply(const char* choices_json);
// The textures served now that differ from what this launch put at their real paths (what the engine already holds), one per
// line: "<real resource path>\t<resource path to show instead>\n" (ui_slots::overrides_text: no natives/stm/, no version suffix,
// in the archive's case). cap must hold the text and its terminating NUL: returns its length (0 when nothing differs), the
// length needed when cap is too small (nothing written), -1 when not running. Exported as SF6UISlots_Overrides.
int ui_slots_overrides(char* buf, int cap);
