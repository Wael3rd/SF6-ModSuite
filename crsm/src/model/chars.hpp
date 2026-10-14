// model::chars — the character ids and folder names of the data files (MAIN:160-170, 243-245). The names are part of the
// file contract: a pack lives in <data>/<name>/<pack>.json.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace model::chars {

// Name of a fighter id (1-22, 25-33), nullptr for any other id.
const char* name_of(int id);
// get_char_name: the name, or "Unknown(<id>)".
std::string get_char_name(int id);
// Every known (id, name) pair in id order (ids 23 and 24 do not exist).
struct Entry { int id; const char* name; };
const std::vector<Entry>& all();

}  // namespace model::chars
