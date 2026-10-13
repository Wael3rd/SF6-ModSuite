#pragma once
// Vanilla costume inventory: the list of every file one costume of the game uses (scene, prefabs,
// meshes, materials, textures, user files, chains...), found by walking the references from the
// costume's scene in the game's paks. It writes the rows costumes/tools/build_vanilla_inventory.py +
// make_vanilla_index.py write into vanilla_costume_index.tsv, so that a costume the shipped index does
// not know (a fighter or an outfit added by a game update) can be added to the index at run time.
// No Windows / REFramework dependency beyond pak.hpp.
#include "pak.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Extension -> version suffix ("mesh" -> "230110883"), the #SUFFIXES section of the index.
using SuffixMap = std::unordered_map<std::string, std::string>;

// Suffixes the Python tool starts from; any other extension is found by probing the paks.
SuffixMap vanilla_known_suffixes();

struct VanillaCostume {
    int fighter = 0;
    std::string fighter_dir;           // "esf034"
    int costume_no = 0;                // 0-9 for esfNNNv00-09, 100-109 for esfNNNa00-09
    std::string model_dir;             // folder of the costume's first mesh, empty when it has none
    std::string scene;                 // pak path of its scene
    struct File { uint64_t hash; std::string kind, path; };
    std::vector<File> files;           // only those found in the paks, in the index's order
    // Empty when the scene was read by its layout. Otherwise why that failed (an unknown type id, a layout
    // that changed): the files were then found by scanning the data for strings that look like paths.
    std::string fallback;
};

// "esf034 v00" / "esf034 a01"
std::string vanilla_costume_label(int fighter, int costume_no);

// Costume numbers a fighter has in the game: esfNNNv00..09 then esfNNNa00..09 whose scene is in the
// paks, as costume numbers 0-9 then 100-109 (the Python tool probes the same names, fighters 1-49).
std::vector<int> vanilla_costume_numbers(PakReader& pak, int fighter, const SuffixMap& suffixes);

// For tests: take the string-scanning path for every file, as if the layout were not known.
void vanilla_force_heuristic(bool on);

// Inventories one costume. suffixes is the cache of extension versions: seed it with
// vanilla_known_suffixes() and the index's own suffixes; an extension found by probing is added to it.
// A scene or user file laid out in a way this code does not know is read by scanning its data for strings
// that look like paths (VanillaCostume::fallback says so): the same files are found, a few more may be.
// False (why in *err) only when the scene is not in the paks.
bool vanilla_costume_inventory(PakReader& pak, int fighter, int costume_no, SuffixMap& suffixes,
                               VanillaCostume& out, std::string* err);

// The index rows of a costume, as the TSV writes them (no line end): the #COSTUMES row and the #FILES rows.
std::string vanilla_costume_row(const VanillaCostume& c);
std::vector<std::string> vanilla_file_rows(const VanillaCostume& c);
