// model::transfer — Lua tables to and from the DLL through the core's Lua bridge (strings only), phase P3.
//
// The Lua sends a table as json.dump_string(t) (REFramework: encode_any(t).dump(-1)); an empty Lua table comes out as
// null there, and a table never holds nil, so every null of that text is an empty table. The DLL sends a value as text
// that json.load_string turns into exactly what json.load_file gives from the file the DLL writes.
#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace model::transfer {

using json = nlohmann::json;

// json.dump_string(t) text -> the Lua table as the model holds it (nulls back to empty tables). discarded() on bad text.
json from_lua(const std::string& text);
// A model value -> the text the Lua decodes with json.load_string (the luajson encoding, invalid UTF-8 replaced).
std::string to_lua(const json& v);
// The text json.dump_file(path, t) writes (before its LF -> CRLF), from the text json.dump_string(t) gave: the parsed
// tree as it is, dump(4) (an empty table stays null). luajson::write_text adds the CRLF. false when the text does not parse.
bool file_text(const std::string& lua_text, std::string* lf_out);

}  // namespace model::transfer
