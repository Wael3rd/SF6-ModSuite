// vfx::settings — the settings files of the Drive colours, read, migrated and written by the DLL (phase 5b,
// docs/CPP_PORT_PHASE5.md). Port of the load / migration / seeding / save of SF6_DIRecolor.lua (get_cfg, save_cfg,
// the presets block) and SF6_DriveRecolor.lua (get_cfg, migrer, upgrade, save_cfg), as they were at tag phase5-start.
//   SF6_DIRecolor_data/<Name>.json     { p1 = cfg, p2 = cfg }, cfg = { enabled, follow, c1, c2, selected, sel, rev, ... }
//   SF6_DIRecolor_data/_presets.json   { presets, presets_dr, presets_sp }
//   SF6_DriveRecolor_data/<Name>.json  { p1 = { parry, rush, special }, p2 = ... }
// Stateless: every load reads the file (the Lua scripts keep their tables, as before), every save writes it. The
// files are byte-identical to REFramework's json.dump_file (nlohmann dump(4), sorted keys, CRLF, an empty Lua table
// written as null), and a load gives the tables json.load_file + the Lua logic gave (tools/settings_harness).
// Pure: no REFramework dependency.
#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace vfx::settings {

// root = the REFramework data folder ("reframework/data" in the game). kind = "di" (SF6_DIRecolor per character),
// "presets" (SF6_DIRecolor's _presets.json, id ignored), "dr" (SF6_DriveRecolor per character).
// load: the table the Lua's get_cfg / presets block built, as compact JSON ("" + err on a Lua error); writes the file
// where the Lua wrote it (a DI file missing or in the old flat layout, presets seeded).
std::string load(const std::string& root, const std::string& kind, int id, std::string* err = nullptr);
// save: text = json.dump_string of the Lua table; written as json.dump_file wrote it. false when nothing was written.
bool save(const std::string& root, const std::string& kind, int id, const std::string& text, std::string* err = nullptr);

// The same on the tables themselves, for the DLL's own store (vfx/drive_cfg, phase 5c): load_table gives the table
// (null + err on a Lua error), save_table writes it as json.dump_file wrote the Lua table (an empty table = null).
nlohmann::json load_table(const std::string& root, const std::string& kind, int id, std::string* err = nullptr);
bool save_table(const std::string& root, const std::string& kind, int id, const nlohmann::json& table, std::string* err = nullptr);

// "Ryu", ... or "esf0NN" (the two scripts' NAMES table)
std::string char_name(int id);

}  // namespace vfx::settings
