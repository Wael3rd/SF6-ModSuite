// screens::colour — the colour screens' files, as SF6_ColorSpinExtra.lua read and wrote them (phase 5c-2):
//   state.json       the selection memory: { p1, p2, fs } = { rw, path, fighter, costume } (the entry each select
//                    side / the Fighter Settings screen last chose), an old single-side layout migrated at load
//   chip_slots.json  the reference slots of each costume ("<fighter>_<costume>": ["<cluster>/<n>", ...], learned from
//                    colour 01 or written by the editor) and the squares saved with a colour file ("<path>": [c0, c1])
// Lua tables as json (engine/luajson): files byte-identical to json.dump_file. Pure: tools/settings_harness checks
// them against the Lua.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "../engine/catalog.hpp"

namespace screens::colour {

using json = nlohmann::json;

// the main chunk's load: json.load_file or {}, the old layout migrated, p1 / p2 / fs defaulted (nothing written).
// nullopt where the Lua raised (a file holding a number or a string): the script then failed to load.
std::optional<json> load_state(const std::filesystem::path& file);
bool save_state(const std::filesystem::path& file, const json& state, std::string* err = nullptr);

// chip_cfg = json.load_file(CHIP_FILE) or {}; chip_cfg.seen = nil (nullopt where the Lua raised)
std::optional<json> load_chip_cfg(const std::filesystem::path& file);
// learn_chip_slots' write: chip_cfg[key] = { s0, s1 }; json.dump_file(CHIP_FILE, chip_cfg)
bool learn_chip_slots(json& chip_cfg, const std::string& key, const std::string& s0, const std::string& s1,
                      const std::filesystem::path& file, std::string* err = nullptr);

// Re-derives a remembered selection's rank (sel.rw) from the colour's identity (sel.path = Entry::ident), after the
// colour lists changed (load, catalogue rebuild). Returns true when `sel` was modified (the caller saves it).
//  - no path, rank > 0 (state of an older version): the rank stays, the path is filled in (migration)
//  - path found: rank = its current rank
//  - path not found (the file is not there NOW: an archive whose cache is being unpacked again, a first start after
//    .cache was deleted, or a removed mod): rank 0 (default colour) and the identity is parked in sel.path_pending
//    with its fighter / costume, out of the way of the code that rewrites path. Every later call looks for it again and
//    puts the colour back as soon as it exists. It is dropped only when the player chose another colour or fighter.
bool resolve_selection(json& sel, const engine::Catalog& cat, std::string* note = nullptr);

}  // namespace screens::colour
