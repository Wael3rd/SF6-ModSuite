// The default presets' names (vfx/data.cpp, vfx/settings.cpp): the player's file keeps them in English, the screens show them
// through locale.json. Any other name is the player's own and stays as it is.
#pragma once

#include <string>

namespace vfx {

inline const char* preset_key(const std::string& name) {
    static const struct { const char* name; const char* key; } kNames[] = {
        { "SP Red", "vfx.preset.red" }, { "SP Orange", "vfx.preset.orange" }, { "SP Yellow", "vfx.preset.yellow" },
        { "SP Lemon Green", "vfx.preset.lemon" }, { "SP Green", "vfx.preset.green" }, { "SP Cyan", "vfx.preset.cyan" },
        { "SP Purple", "vfx.preset.purple" }, { "SP Magenta", "vfx.preset.magenta" }, { "SP Pink", "vfx.preset.pink" },
        { "SP White", "vfx.preset.white" },
    };
    for (const auto& n : kNames)
        if (name == n.name) return n.key;
    return nullptr;
}

}  // namespace vfx
