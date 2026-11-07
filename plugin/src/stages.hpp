// SF6 Slots — the stage slots' entries and per-menu choices, for the other modules of this plugin (replay_tab.cpp's stage
// picker). The game side is stages.cpp; the rules are stages_logic.hpp.
//
// Each stage mod is a variant of one vanilla stage (the loader serves its files in place of the stage's own, so the game,
// the opponent online and the replay files only ever hold the vanilla id). The loader guesses what each one is
// (registry.json "kind", stage_slots.ini corrects it):
//   - a TILE (another stage over a vanilla one, a Suzaku Castle port over the Training Room): its own entry;
//   - a VARIATION (lighting...): cycled on its stage's own entry.
// Each menu keeps its own choice per vanilla stage (state.json "menus"), applied when that menu's stage loads:
//   kClassic   the full-screen Stage Select (UP / DOWN: a stage's variations; its tiles are entries past the last cell), applied at once
//   kBattle    Battle Settings > Other > Stage Settings: applied when an online battle is set up, on this PC only
//   kTraining  Training's Change Stage: applied when Training sets its stage (and on the choice itself)
//   kReplay    the replay picker: applied when a replay's battle description is set up (ReplayFlowMap.setupDesc)
//
// Use (game thread: LateUpdateBehavior, e.g. replaytab::frame()):
//   auto list = stages::entries();                   // vanilla stages (the game's order), then tiles; variations last
//   stages::choose(stages::kReplay, e.base_id, e.key);   // e from the list; key "" = the vanilla look
//   stages::choose_replay_stage(base_id);            // optional: play the next replays on another vanilla stage
//   stages::chosen(stages::kReplay, base_id);        // the key chosen for a stage ("" vanilla)
//   if (stages::variation_key_down()) ...            // the variation key (edge-detect it: one press, one step)
//   stages::replay_variation_hint();                 // each frame the focused stage has variations
// Banner and thumbnail are texture resource paths for slots::ui::tex_load (wait for tex_ready, about 60 frames, before
// handing one to the GUI: a texture still loading crashed the game). They are always set (a variant without its own
// pictures gets its stage's).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace slots::stages {

enum MenuId : int { kClassic = 0, kBattle = 1, kTraining = 2, kReplay = 3 };

struct Entry {
    enum Kind : int { Vanilla = 0, Tile = 1, Variation = 2 };
    uint32_t base_id = 0;         // the vanilla stage id (what the game, the opponent and the replay files hold)
    std::string key;              // "" for a vanilla stage, else the variant's key (registry.json)
    Kind kind = Vanilla;
    std::string name;             // display name (a variation: its own name; the stage's is that of its Vanilla entry)
    std::string bundle;           // the variant's pack ("Stage Lighting Overhaul"; stagelogic::variation_label), "" none
    std::string banner;           // 4:1 picture (2048x512), texture resource path
    std::string thumb;            // 1:2 picture (128x256)
    bool has_own_picture = false; // banner / thumb are the variant's own, not its stage's
};

// Vanilla stages in the game's stage-select order (with their names once the game has them), then the tiles, then the
// variations (grouped by stage). Without the game's list yet (before the first frame of a menu): the stages that have
// variants only.
std::vector<Entry> entries();
// The key a menu has chosen for a stage ("" = vanilla look).
std::string chosen(int menu, uint32_t base_id);
// A menu's choice for a stage: key "" or a variant of that stage (anything else is refused, false). Saved at once
// (state.json); applied when that menu's stage loads (kClassic: at once).
bool choose(int menu, uint32_t base_id, const std::string& key);
// The next replays are played on this vanilla stage (-1: the recorded one), with kReplay's choice for it. Only the stage
// id changes: width, position and the inputs are the recorded ones.
void choose_replay_stage(int32_t base_id);
int32_t replay_stage();
// The variation key: the game's binding of the pad's X button (app.InputAssign.Digital.Id 341, Opponent Outfit Settings'
// "EX/DX Colors On/Off": its keyboard key at its QWERTY place, else the merged pad's X), held down now. False while the game
// window is not in front. Game thread. One press = the next variation, wrapping (stagelogic::cycle(c, n, 1)).
bool variation_key_down();
// The guide bar's "<key> Stage Variation" (the game's icon for that key, then the label: a copy of the bar's own item
// template after its entries), for the replay picker: call it every frame its stage grid has the focus on a stage with
// variations (game thread); it goes away when the calls stop.
void replay_variation_hint();
// The same key and the same entry for another use of Replays > Settings (its character page's Costume options), with or
// without stage slots: the key held down now; "<key> <label>" in the guide bar, called every frame it is wanted.
bool replay_key_down();
void replay_guide_hint(const char* label);
// Battle Settings' key of its "Character Music" entry (app.InputAssign.Digital.Id 295: T, the pad's Y), for the Replay Music row
// of Replays > Settings: held down now; "<key> <label>" in the guide bar, called every frame it is wanted.
bool replay_music_key_down();
void replay_music_hint(const char* label);

}  // namespace slots::stages
