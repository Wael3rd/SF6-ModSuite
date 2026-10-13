#pragma once
#include <map>
#include <string>

// The fighters' names as the game spells them, read from the game's own message file
// (natives/stm/product/message/cmn/001/wordlist/fighternamemessage.msg.21, entries
// "FighterNameMessage<id>"). A fighter added by a game update is therefore named with no code change.

// {fighter id: English name} from <game_dir>\re_chunk_000.pak and its official patch paks (newest
// wins; paks written by the loaders are skipped). Empty on any failure; never throws. Needs
// structural_msg.cpp, pak.cpp and the zstd / miniz objects.
std::map<int, std::string> game_fighter_names(const std::string& game_dir);

// A Windows-safe folder name in the Slots Loader's style: spaces and inner dots are kept
// ("C. Viper", "E. Honda"), a trailing dot is dropped ("A.K.I." -> "A.K.I"), characters that are
// invalid in file names are removed, and the ends are trimmed. Empty if nothing is left.
std::string fighter_folder_name(const std::string& english);
