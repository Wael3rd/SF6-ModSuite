#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Stage slots, startup pass (DllMain, before the costume loader).
//
// Every stage mod found in reframework\stage_mods (archives as downloaded, folders, paks)
// becomes a variant of the stage it changes. Its files are copied into our own patch pak
// under hashes of their own ("natives/stm/_stageslots/<key>/<vanilla hash>"), so nothing
// replaces the vanilla files; the path_to_hash detour serves a variant's copies when that
// variant is selected (stage_redirect.cpp). The stage pak sits right above the mod paks and
// below the costume pak.
//
// Outputs, in reframework\data\SF6_StageSlots_Data:
//   registry.json        the variants per stage, for the native plugin (names, kind tile / variation, preview textures)
//   loader\variants.tsv  the same with every file redirection, reloaded when nothing changed
// Input: reframework\stage_mods\stage_slots.ini, the user's corrections of the tile / variation guess and of names
// (written as a commented template when missing), applied at every start without rebuilding the pak.

struct StageVariant {
    std::string key;            // stable id: selection state, pak folder
    uint32_t stage_id = 0;      // the game's stage id (ess0000_00 -> 0, ess0100_00 -> 10000)
    std::string ess;            // stage code, ess0000_00
    std::string name;
    std::string author;
    std::string bundle;         // Fluffy bundle name (nameAsBundle), may be empty
    std::string source;         // where it came from, relative to stage_mods
    bool has_preview = false;
    bool has_thumb = false;
    std::vector<std::pair<uint64_t, uint64_t>> redirects;   // vanilla hash -> hash in our pak
    // What it is (stage_classify.hpp): guessed from its files when the pak is built, kept in variants.tsv...
    bool tile_auto = false;
    std::string reason_auto;
    std::string banner;         // where its preview / thumbnail come from: "mod" (its own menu pictures), "screenshot", ""
    // ...then reframework\stage_mods\stage_slots.ini applied at every start (registry.json only)
    bool tile = false;
    std::string kind_reason;
    std::string mod_name;       // the mod's own name when stage_slots.ini renamed it, else ""
};

// Scans the mods, rebuilds the stage pak when they changed, fills `out` either way.
void stage_loader_run(const wchar_t* game_dir, std::vector<StageVariant>& out);

// Path of the preview texture of a variant as the engine's resource API takes it.
std::string stage_preview_resource(const std::string& key);
std::string stage_thumb_resource(const std::string& key);
