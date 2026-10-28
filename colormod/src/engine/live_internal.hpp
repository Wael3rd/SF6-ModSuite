// engine::live pieces shared with engine::decide (same DLL, not a public interface): the patch routine, the lock,
// and the small reflection helpers both modules need. Everything here must be called with mx() held.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <reframework/API.hpp>

#include "rsz.hpp"

namespace engine::live {

using API = reframework::API;
using MO = API::ManagedObject;

std::recursive_mutex& mx();      // the colour engine's one lock (hooks, frame tick, Lua bridge)

// trees read from the files the decisions name (Lua read_tree): re-read when size / write time change, null when
// the file is missing, empty or unreadable. path = relative to the game folder ("reframework/data/...").
rsz::NodeRef file_tree(const std::string& path);
// a colour file was written, moved or deleted by this DLL (editor, catalogue, state, color_mods): what file_tree
// remembered of this frame's file checks is asked again (any thread)
void files_changed();

// System.Array the way REFramework's Lua reads it (GetLength / GetValue(Int32)): the findComponents quirk of 02/10
uint32_t arr_size(MO* a);
MO* arr_at(MO* a, uint32_t i);

// live field offsets of a serialized bool / u32 / f32 field (Lua OFFSETS): plain field, backing field, `_Value*` cache
const std::vector<uint32_t>& offsets_of(const rsz::ClassSpec* cls, size_t fi);

MO* invoke_obj(MO* obj, API::Method* m);         // no-argument call returning an object (null on exception)
std::string go_name(MO* comp);                   // Lua ctrl_go_name: the component's GameObject name
int side_from_parents(MO* ctrl);                 // Lua side_from_parents: 1, 2, 3 (Fighter Settings preview), 0 = nil
MO* color_data(MO* ctrl, int32_t id);            // Lua color_data: ColorData.Colors[ColorId == id].Data
bool is_color_ctrl(MO* obj);                     // app.PlayerColorController or a derived class
// Lua live_to_tree(live, depth): a live app.CostumeMaterialData (or one of its parts) read into a tree of the file's
// shape; bool / u32 / f32 from the LAST of the field's offsets (the `_Value*` cache: the effective value). Null for a
// class the RSZ table does not know, or past depth 12.
rsz::NodeRef live_tree(MO* live, int depth = 0);

bool editor_on();                                // the costume editor's working copy owns the side-3 preview
int editor_patch(MO* ctrl);                      // patch it there (Lua inject k == -1): fields, 0 shared, < 0 none
// owner "engine" patch of a decided tree: same data in the same frame = one more user (0), else a fresh patch
// (restores the previous one first). Returns the fields written.
int engine_apply(MO* data, const rsz::Node* tree);
void abort_patch();                              // after a fault mid-patch: put back what was written
void write_tree(MO* live, const rsz::Node* tree);  // Lua apply_tree with a throw-away saved list: written for good

}  // namespace engine::live
