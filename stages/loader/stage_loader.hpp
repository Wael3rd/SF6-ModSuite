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
//
// Title screen packs (title_slots.hpp) ride in the same pak: the chosen pack of reframework\title_mods has its files
// written at their real paths (no redirection), and reframework\data\SF6_TitleSlots_Data\registry.json lists the packs.
// So do the UI packs of reframework\ui_mods (ui_slots.hpp): every option at private paths, the launch's ticked options at the
// real paths too, reframework\data\SF6_UISlots_Data\registry.json lists them.

#include "ui_slots.hpp"

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

// The title screen's backgrounds, for the switch on a return to the title (stage_redirect.hpp): per background key (a pack's,
// "game" the game's own), the real title files' hashes -> its copies' in the stage pak (only those the pak holds).
struct TitleTables {
    std::vector<std::pair<std::string, std::vector<std::pair<uint64_t, uint64_t>>>> backgrounds;
    std::string launch;                 // the background this launch wrote at the real paths ("game": none written)
    std::string choice;                 // settings.json's title_pack at launch: "" the game's own, a key, "random"
    std::vector<std::string> random_pool;   // every background Random could serve on a visit ("game" + packs with copies)
    std::vector<std::string> playlist_pool; // every background the core's playlist could give the title's movies: every streamed
                                            // title file has its private data and resource file in the pak
    bool has_selection = false;             // settings.json's title_random_pool at launch (the plugin sends changes live)
    std::vector<std::string> selection;
    std::vector<uint64_t> start_hashes; // title_start.mov's real hashes: a new visit of the title (Random draws again)
    std::wstring bag_path;              // SF6_TitleSlots_Data\shuffle.json: Random's bag (title_slots::Bag), shared with the launch's draw
};

// Scans the mods, rebuilds the stage pak when they changed, fills `out` either way.
void stage_loader_run(const wchar_t* game_dir, std::vector<StageVariant>& out);
// What the last stage_loader_run found for the title screen (empty without reframework\title_mods)
const TitleTables& stage_loader_titles();
// What the last stage_loader_run found of the UI packs (not present without reframework\ui_mods)
const ui_slots::Tables& stage_loader_ui();

// Path of the preview texture of a variant as the engine's resource API takes it.
std::string stage_preview_resource(const std::string& key);
std::string stage_thumb_resource(const std::string& key);
