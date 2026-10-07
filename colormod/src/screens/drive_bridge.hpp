// screens::drive_bridge — the Lua entry points of the Drive screens' native model (phase 5c-1), see drive_bridge.cpp.
#pragma once

#include <vector>

#include "../common.hpp"

namespace screens::drive_bridge {

const std::vector<cmn::BridgeDialogEditor::Fn>& fns();   // registered by main.cpp with the editor relay's functions

}  // namespace screens::drive_bridge
