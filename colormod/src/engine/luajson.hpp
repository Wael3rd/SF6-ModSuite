// engine::luajson — Lua tables held as nlohmann::json, and REFramework's json module at both ends (phase 5: the files
// the Lua wrote with json.dump_file are written by the DLL, byte for byte). A Lua value is a json: null = nil,
// object / array = a table (an empty table stays an empty container in memory, a null inside an array is a hole).
//   decode    json.load_file's side: a null member is not stored (t[k] = nil), {} and [] give an empty (truthy) table
//   encode    json.dump_file's side: an empty table is written as null, a sequence with a hole as an object
//   dump_file encode + dump(4) + the text-mode LF -> CRLF, written through a temporary file
// (REFramework src/mods/bindings/Json.cpp, nlohmann 3.9.1 on both sides.) Lua semantics helpers: truthiness, t.k (with
// the errors Lua raises), t.k = v, #t. Used by vfx/settings (5b) and screens/colour (5c-2).
#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "nlohmann/json.hpp"

namespace engine::luajson {

using json = nlohmann::json;

// a Lua run-time error (indexing a number, pairs on a string...): the Lua code raised there
struct LuaError : std::runtime_error { using std::runtime_error::runtime_error; };

json decode(const json& j);
json encode(const json& v);
json load_file(const std::filesystem::path& p);                 // missing or invalid file: null (nil)
bool write_text(const std::filesystem::path& p, const std::string& lf, std::string* err);
bool dump_file(const std::filesystem::path& p, const json& v, std::string* err);
// the exact bytes dump_file writes (encode + dump(4) + CRLF), for a writer off the game thread; false + err on failure
bool dump_bytes(const json& v, std::string& out, std::string* err);

bool truthy(const json& v);
bool is_table(const json& v);
json index(const json& t, const char* k);                       // t.k (throws LuaError where Lua raises)
void set(json& t, const char* k, json v);                       // t.k = v (nil erases)
size_t len(const json& t);                                      // #t

}  // namespace engine::luajson
