// Colour mods' file rules (the Color Mod's reframework/color_mods, colormod/src/engine/color_mods.cpp), shared with
// SF6_ModSorter (tools/sorter) so that both read a colour file the same way. Header only, no Windows API.
//   - a colour file is named esfFFF_CCC_cmd_KKK.user.2, letters of any case (FFF the fighter, CCC the costume folder,
//     KKK the colour number); the catalogue (catalog.cpp) also skips an emptied file
//   - the <Character> folder of a fighter: the same list and spelling as the costume_mods folders (Windows drops a
//     trailing dot: "A.K.I"), with the esf number
#pragma once

#include <string>

namespace colour_files {

struct Character { int id; const char* name; };
inline const Character kCharacters[] = {
    { 1, "Ryu" }, { 2, "Luke" }, { 3, "Kimberly" }, { 4, "Chun-Li" }, { 5, "Manon" }, { 6, "Zangief" }, { 7, "JP" },
    { 8, "Dhalsim" }, { 9, "Cammy" }, { 10, "Ken" }, { 11, "Dee Jay" }, { 12, "Lily" }, { 13, "A.K.I" }, { 14, "Rashid" },
    { 15, "Blanka" }, { 16, "Juri" }, { 17, "Marisa" }, { 18, "Guile" }, { 19, "Ed" }, { 20, "E. Honda" }, { 21, "Jamie" },
    { 22, "Akuma" }, { 25, "Sagat" }, { 26, "M. Bison" }, { 27, "Terry" }, { 28, "Mai" }, { 29, "Elena" },
    { 30, "C. Viper" }, { 31, "Alex" }, { 32, "Ingrid" }, { 33, "Yasmine" }, { 34, "Arjun" },
};

// The <Character> folder of a fighter id (esf001 -> "Ryu"); "" for an id not in the list.
inline std::string character_folder(int fighter_id) {
    for (const Character& c : kCharacters) if (c.id == fighter_id) return c.name;
    return {};
}

// esfFFF_CCC_cmd_KKK.user.2 (letters of any case); n_in: a file name, no folder
inline bool is_colour_name(const std::string& n_in) {
    std::string n = n_in;
    for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    static const char* tail = ".user.2";
    if (n.size() != 3 + 3 + 1 + 3 + 1 + 3 + 1 + 3 + 7) return false;
    auto dig = [&](size_t a, size_t b) { for (size_t i = a; i < b; ++i) if (n[i] < '0' || n[i] > '9') return false; return true; };
    return n.compare(0, 3, "esf") == 0 && dig(3, 6) && n[6] == '_' && dig(7, 10) && n[10] == '_' &&
           n.compare(11, 4, "cmd_") == 0 && dig(15, 18) && n.compare(18, 7, tail) == 0;
}

}  // namespace colour_files
