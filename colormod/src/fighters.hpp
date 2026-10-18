// Fighter id -> name, one function for the whole mod. These names end up in file and folder names on users' disks
// (SF6_DIRecolor_data/<Name>.json, ...), so the spellings of today's fighters are fixed here.
// A fighter a game update adds is named from the game's own message file (its English name reduced to [A-Za-z0-9]),
// so no code change is needed; the file is read once, in the background from the plugin's start (preload_async).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fighters {

// The fixed name, else the game's English name reduced to [A-Za-z0-9], else "" (nothing known).
std::string name_or_empty(int id);
// The same, else "esf%03d".
std::string name(int id);
// Every id with a name (fixed + the game's once read, ids 1..99), in id order. Never reads: it runs on the game's
// frame (the read takes ~70 ms).
std::vector<std::pair<int, std::string>> all_known();
// Reads the game's names on a thread of its own (plugin start), so that no frame waits for it.
void preload_async();
// The game's names were read (or could not be): all_known() is complete.
bool ready();

// Pure part, exposed for the offline test: "C. Viper" -> "CViper"; "" when nothing is left.
std::string ascii_name(const std::string& english);
// Test hook: use this folder instead of the running exe's, and forget what was read.
void set_game_dir_for_test(const std::string& dir);

}  // namespace fighters
