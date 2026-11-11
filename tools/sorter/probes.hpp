// SF6_ModSorter -- what the suite's own loaders make of one dropped item. Each probe runs the loader's own code
// (probe_costume.cpp: the costume loader, loader_core.cpp; probe_stage.cpp: the stage loader, stage_loader.cpp, with its
// title packs and UI packs; colour files by common/colour_files.hpp), never a copy of its rules. Single-threaded: the
// loaders keep their state in globals, so one thread probes at a time (the sorter's worker).
#pragma once

#include <set>
#include <string>
#include <vector>

namespace probe {

// ---- costumes (the Slots Loader's costume pass) ------------------------------------------------------------------

// Loads what the costume loader reads before it scans any mod: the vanilla costume index of the game folder
// (reframework\data\SF6_Costumes_Data\loader\vanilla_costume_index.tsv), completed from the game's paks like the loader
// does after a game update, the NSFW keywords (costume_mods\settings.ini NsfwKeywords=, else the default list; none in
// the Slotsloader Manager's mode). cache_dir: where nested archives are unpacked (the loader's costume_mods\.cache).
// False (why in note) when the index is missing: costumes cannot be recognised then.
bool costume_init(const std::wstring& game_dir, const std::wstring& cache_dir, std::string& note);
bool costume_ready();

struct CostumeOutfit {
    int fighter = 0;
    int costume_no = 0;          // the game's costume number (0 = Outfit 1, 4 = Drive Tech...)
    std::string outfit;          // "Outfit 1", "Drive Tech" (the loader's game_outfit_name)
    std::string option;          // the option (unit) it comes from, relative to the item
    bool full = true;            // a whole outfit (a body mesh); false: parts only, an option of the game's outfit
};

struct CostumeProbe {
    int units = 0;               // natives trees and paks found (Fluffy menu entries left out)
    int loaded = 0;              // units with costume files
    std::vector<CostumeOutfit> outfits;
    std::set<int> fighters;      // every fighter its costume files are for
    int shared_only = 0;         // units with costume files of shared folders only (000)
    size_t files = 0;            // costume files the loader kept, every unit
    size_t colour_files = 0;     // of which colour files (esfFFF_CCC_cmd_KKK.user.2): a colour pack's are only those
    bool nsfw = false;           // the loader's keyword verdict on the mod's name (as if it were in costume_mods_nsfw)
    std::string nsfw_why;        // the loader's line for it
    std::vector<std::string> nsfw_options;   // options NSFW by their own name (the mod is not)
    std::string log;             // the loader's own lines for this item
};

// kind: 0 a folder (as costume_mods\<Character>\<folder>), 1 an archive's unpacked tree (as costume_mods\.cache\<key>),
// 2 a .pak (root: its folder, name: its file name). rel: the label the loader logs ("<Character>/<name>").
CostumeProbe costume_probe(const std::wstring& root, const std::string& rel, int kind, const std::wstring& pak_name = L"");

// The game's outfit name of a costume number ("Outfit 1", "Drive Tech")
std::string outfit_name(int costume_no);

// ---- stages, title packs, UI packs (the stage loader's pass) ------------------------------------------------------

// The stage index of the game folder (reframework\data\SF6_StageSlots_Data\loader\stage_paths.txt). False when missing.
bool stage_init(const std::wstring& game_dir, std::string& note);
bool stage_ready();

struct StageVariantInfo {
    std::string ess;             // stage code, ess0000_00
    std::string option;          // the option's name (modinfo name, folder or archive)
    bool tile = false;           // another stage built over this one (stage_classify.hpp), else a variation
    std::string reason;
    size_t files = 0;
};

struct StageProbe {
    std::vector<StageVariantInfo> variants;  // one per option and stage it changes (as the stage loader makes them)
    size_t options = 0;          // options found (folders holding a natives tree or a .pak)
    size_t gui_files = 0;        // GUI files of stages only (menu pictures: never redirected)
    // natives files of the item (natives trees only: a pak's entries have no path), by what they are
    size_t natives_files = 0;
    size_t title_files = 0;      // title_slots::is_title_file
    size_t pak_title_files = 0;  // entries of its paks that are title files (title_slots::known_paths, by hash)
    size_t pak_title_others = 0; // the other entries of those paks (a title pack's startup files)
    size_t ui_files = 0;         // under natives/stm/product/gui/ or natives/stm/streaming/product/gui/
    std::set<std::string> ui_fighters;   // ui_slots::fighter_of_path of those files
    size_t pak_entries = 0;      // entries of the paks found
    size_t ui_nodes = 0;         // UI pack nodes (ui_slots: every folder with a modinfo.ini or a natives tree)
    std::string first_other;     // a natives path that is none of the above, for the reason
    std::string option_name;     // the first option's name (modinfo name, else the folder's)
};

// kind as costume_probe. name: the item's name without extension (the stage loader's default option name).
StageProbe stage_probe(const std::wstring& root, const std::string& rel, const std::string& name, int kind,
                       const std::wstring& pak_name = L"");

}  // namespace probe
