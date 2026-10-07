// Lua bridge for the Drive screens: only colour_status remains (the rest moved to native calls).

#include "drive_bridge.hpp"

#include <string>

#include "colour_track.hpp"

namespace screens::drive_bridge {
namespace {

thread_local std::string t_s1;
void r_str(SF6CoreValue& r, std::string& keep, const std::string& s) {
    keep = s;
    if (keep.empty()) { r.type = SF6CORE_VAL_NIL; return; }
    r.type = SF6CORE_VAL_STRING; r.s = keep.c_str();
}

int32_t SF6CORE_CALL lua_colour_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    r_str(r[0], t_s1, colour::track::status());
    return 1;
} catch (...) { return -1; }
}  // namespace

const std::vector<cmn::BridgeDialogEditor::Fn>& fns() {
    static const std::vector<cmn::BridgeDialogEditor::Fn> v = {
        { "colour_status", lua_colour_status },
    };
    return v;
}

}  // namespace screens::drive_bridge
