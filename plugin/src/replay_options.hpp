// SF6 Slots — Replays > Settings: the costume options saved for a fighter (replay_outfits.json). Pure, no game access
// (plugin/tests). Shared by the tab (replay_logic.hpp) and the replay's description (replay_outfits.hpp).
//
// Under a fighter's entry: "options": {"<outfit: its mod key in costume_options.json>": ["<option id>", ...]}, the set of
// options that outfit wears (an outfit not named wears its defaults). Saved per outfit: changing the outfit keeps the
// other outfit's. A fighter set to Off keeps them too: {"mode": "off", "options": {...}} (not forced: nothing is worn).
// Never the player's own choices (costume_choices.json).
#pragma once

#include "json.hpp"

#include <map>
#include <string>
#include <vector>

namespace slots::replay {

using OutfitOptions = std::map<std::string, std::vector<std::string>>;   // outfit (mod key) -> option ids

inline void parse_outfit_options(const Json& fighter, OutfitOptions& out) {
    out.clear();
    const Json* os = fighter.get("options");
    if (!os || os->kind != Json::Object) return;
    for (const auto& [mod, ids] : os->obj) {
        if (ids.kind != Json::Array || mod.empty()) continue;
        std::vector<std::string>& v = out[mod];
        for (const Json& id : ids.arr) if (id.kind == Json::String && !id.s.empty()) v.push_back(id.s);
    }
}

// `"options": {...}`
inline std::string outfit_options_json(const OutfitOptions& o) {
    std::string s = "\"options\": {";
    bool first_mod = true;
    for (const auto& [mod, ids] : o) {
        s += (first_mod ? "\"" : ", \"") + json_escape(mod) + "\": [";
        first_mod = false;
        for (size_t i = 0; i < ids.size(); ++i) s += (i ? ", \"" : "\"") + json_escape(ids[i]) + "\"";
        s += "]";
    }
    return s + "}";
}

}  // namespace slots::replay
