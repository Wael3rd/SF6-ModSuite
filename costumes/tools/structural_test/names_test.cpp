// Checks game_fighter_names() / fighter_folder_name() (common/fighter_names.cpp) and the Color Mod's
// fighters::name() (colormod/src/fighters.cpp) against the game's own message file.
// Usage: names_test <game folder> [oracle.tsv]
//   oracle.tsv = "<id>\t<English name>" per line, written by an independent reader of the same file.
#include "fighter_names.hpp"
#include "fighters.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: names_test <game folder> [oracle.tsv]\n"); return 2; }
    const std::string game = argv[1];

    auto t0 = std::chrono::steady_clock::now();
    const std::map<int, std::string> names = game_fighter_names(game);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("game_fighter_names: %zu names in %.0f ms\n", names.size(), ms);
    check(!names.empty(), "names read");
    check(game_fighter_names(game + "\\no_such_folder").empty(), "missing folder -> empty, no throw");

    std::map<int, std::string> oracle;
    if (argc > 2) {
        std::ifstream in(argv[2]);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t tab = line.find('\t');
            if (tab != std::string::npos) oracle[std::atoi(line.c_str())] = line.substr(tab + 1);
        }
        check(!oracle.empty(), "oracle file read");
    }

    std::printf("\n%4s  %-16s %-8s %-16s %-14s %s\n", "id", "English", "oracle", "folder name", "Color Mod", "esf fallback");
    fighters::set_game_dir_for_test(game);
    int same = 0, diff = 0;
    for (const auto& [id, en] : names) {
        const char* cmp = "n/a";
        if (!oracle.empty()) {
            auto it = oracle.find(id);
            if (it != oracle.end() && it->second == en) { cmp = "SAME"; ++same; } else { cmp = "DIFF"; ++diff; }
        }
        std::printf("%4d  %-16s %-8s %-16s %-14s %s\n", id, en.c_str(), cmp, fighter_folder_name(en).c_str(),
                    fighters::name_or_empty(id).c_str(), fighters::name(id).c_str());
    }
    if (!oracle.empty()) {
        std::printf("\noracle: %d SAME, %d DIFF, oracle ids %zu, ours %zu\n", same, diff, oracle.size(), names.size());
        check(diff == 0 && oracle.size() == names.size(), "every id equals the oracle");
    }

    // The Color Mod's spellings are in file names on users' disks: they must not change.
    static const char* const kFixed[] = { nullptr, "Ryu", "Luke", "Kimberly", "ChunLi", "Manon", "Zangief", "JP", "Dhalsim", "Cammy", "Ken",
        "DeeJay", "Lily", "AKI", "Rashid", "Blanka", "Juri", "Marisa", "Guile", "Ed", "EHonda", "Jamie", "Akuma", nullptr, nullptr, "Sagat",
        "Bison", "Terry", "Mai", "Elena", "CViper", "Alex", "Ingrid", "Yasmine", "Arjun" };
    bool fixed_ok = true;
    for (int id = 1; id < (int)(sizeof(kFixed) / sizeof(kFixed[0])); ++id)
        if (kFixed[id] && fighters::name(id) != kFixed[id]) { fixed_ok = false; std::printf("  id %d: %s\n", id, fighters::name(id).c_str()); }
    check(fixed_ok, "Color Mod: ids 1-22 and 25-34 keep their spellings");
    check(fighters::name(23) == "esf023" && fighters::name(24) == "esf024", "Color Mod: ids with no name -> esfNNN");
    check(fighters::name(250) == "esf250", "Color Mod: out-of-range id -> esfNNN");

    // Sanitising
    check(fighter_folder_name("C. Viper") == "C. Viper", "folder: 'C. Viper' kept");
    check(fighter_folder_name("A.K.I.") == "A.K.I", "folder: trailing dot dropped");
    check(fighter_folder_name("E. Honda") == "E. Honda", "folder: 'E. Honda' kept");
    check(fighter_folder_name("Bad<>:\"/\\|?*Name ") == "BadName", "folder: invalid characters removed");
    check(fighter_folder_name("...") == "", "folder: nothing left -> empty");
    check(fighters::ascii_name("C. Viper") == "CViper" && fighters::ascii_name("Chun-Li") == "ChunLi" &&
          fighters::ascii_name("Dee Jay") == "DeeJay" && fighters::ascii_name("A.K.I.") == "AKI" &&
          fighters::ascii_name("\xC3\x89lise") == "lise" && fighters::ascii_name("???") == "",
          "Color Mod: reduced to [A-Za-z0-9]");

    // A hypothetical future fighter: unknown in the fixed table, so it takes the game's name.
    std::printf("\nfighters::all_known(): %zu names\n", fighters::all_known().size());
    std::printf(fails ? "\nFAILED (%d)\n" : "\nALL OK\n", fails);
    return fails ? 1 : 0;
}
