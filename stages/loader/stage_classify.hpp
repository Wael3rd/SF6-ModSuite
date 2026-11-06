#pragma once
// Stage slots: what a stage mod is, from the files it replaces (pure logic, no game, no Windows: stagetest --selftest).
//
// A TILE is a different stage built over a vanilla one (a Suzaku Castle port over the Training Room): it gets its own
// entry in the stage grids (Battle Settings > Other > Stage Settings, Training's Change Stage). A VARIATION only tweaks
// its stage (lighting, materials, a few textures): it is cycled on that stage's own entry. Rule, checked against the 15
// mods on PC3 on 2026-11-05 (38 variants: 8 tiles, all over the Training Room, 30 variations):
//   tile when the variant replaces the stage's root prop scene  environment/stage/scene/ess/<ess>/<ess>_env.scn.*
//                                   its arena mesh             environment/stage/resource/ess/<ess>/<ess>(_00).mesh.*
//                                   or ships collision         *.mcol.*
//                                   or adds >= 20 files the game does not have
//   variation otherwise (light / camera / skydome scenes, materials, textures, effects, env_props_* scenes, a level scene
//   alone: Stage Lighting Overhaul's Suval'hal Arena replaces one).
// reframework\stage_mods\stage_slots.ini overrides the guess per variant (kind=, name=).

#include <string>
#include <vector>

struct StageKind {
    bool tile = false;
    std::string reason;          // why, for registry.json and the log
};

// ess: the variant's stage code (ess0000_00). paths: the variant's files, lower case, '/' separators, as long as known
// (natives/stm/product/...; a pak entry the stage index does not name has no path: count it in new_files only).
// new_files: files of the variant that are not files of the game's stage index (the mod's own new files).
StageKind classify_stage_variant(const std::string& ess, const std::vector<std::string>& paths, size_t new_files);

// One section of stage_slots.ini.
struct StageOverride {
    std::string section;         // as written
    std::string kind;            // "tile", "variation" or "" (not set)
    std::string name;            // "" = not set
};

// [section] then key=value lines; ';' and '#' start comments; keys are case-insensitive; a value "tile" / "variation" only
// for kind (anything else is ignored and reported in bad, one line each).
std::vector<StageOverride> parse_stage_overrides(const std::string& text, std::vector<std::string>* bad = nullptr);

// The section that names this variant: its key, its name (the mod's), its source (relative to stage_mods), the first part
// of its source (the archive or folder in stage_mods) or that part without its extension; case-insensitive. The last
// matching section wins (a later, more specific line can refine a whole pack). nullptr: none.
const StageOverride* find_stage_override(const std::vector<StageOverride>& list, const std::string& key,
                                         const std::string& name, const std::string& source);

// The template written when stage_slots.ini does not exist yet.
extern const char* kStageOverridesTemplate;
