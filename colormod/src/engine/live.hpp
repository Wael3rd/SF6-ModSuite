// Costume editor preview, native (feature "editor", docs/CPP_PORT_PLAN.md phase 1).
//
// The Lua editor (SF6_ColorSpinExtra.lua) keeps its UI and its working tree (edit.tree). This module holds a copy of
// that tree and does what the Lua inject()/restore() did for the preview model: on PlayerColorController.SetupColor
// of the Fighter Settings preview (side 3), the tree is written over the live app.CostumeMaterialData, and put back
// at the first SetupPartsColor / SetupColor post (reference-counted, same as the Lua). In C++ the ~3100 field writes
// and their restore cost microseconds; in Lua they cost ~3 ms per patch, on every slider step and pulse step.
//
// Since phase 5 the native editor (dialogs/editor) calls editor_share / editor_active directly.
// The bridge wrappers for editor_load/set/hl/engine_patch are removed (no Lua caller left).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rsz.hpp"

namespace engine::live {

bool install();                 // hooks; false = not installed (types missing), the feature then stays off
bool ready();                   // installed and not switched off by faults (the Lua bridge's owns("editor"))
void editor_active(bool on);
// Native editor (phase 4d, dialogs/editor): the working copy itself, not a copy of it, with the colours to pull
// towards white. The caller holds mx() while it edits the tree. Turns the preview on.
void editor_share(const rsz::NodeRef& tree, bool hl_on, double hl_k, const std::vector<const rsz::Node*>& hl);
// Owner "engine" (phase 1, step 2): the Lua still decides what each controller wears (side, entry, override) and
// hands the DLL the files from inside its SetupColor pre-hook; the DLL writes them and restores at the post.
// override_path first, entry_path when there is no override (Lua entry_tree). >= 0 = patched (0 = already on:
// same data, same frame), < 0 = not done, the Lua patches itself: -1 not a colour controller, -2 feature off,
// -3 no colour data, -4 no tree. Every refusal is journaled with its reason.
std::string status();           // one line: active, nodes, patches, average cost, faults

// offline test only (tools/rsz_harness/sync_cpp_replay.cpp): the C++ copy as a .user.2, and the highlighted nodes as
// "<node>:<class>:<Color>" sorted by node number
void debug_offline();           // accept the bridge calls without the game (no hooks installed)
std::vector<uint8_t> debug_serialize();
std::string debug_hl();

}  // namespace engine::live
