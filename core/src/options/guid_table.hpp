// dialogs::guid_text - the GUID -> text table of the native option engine, without any game dependency
// (phase 4b). Compiled as it is into the DLL (guid_text.cpp adds the hMsg.GetMessage hook) and into the offline
// harness (tools/options_harness).
//
// Ported from func/NativeOptions.lua :42-69 (guid_text table, message_guid) and :1077-1087 (message_text).
// Decision Q3 (docs/CPP_PORT_PHASE4.md section 8): the texts are dropped when the option roots are purged, which
// the Lua never did (its table only grows). The texts of the tree that stays in the game's list are kept: that
// tree is opened again without a rebuild, and its root row stays visible in the game's own Options screen.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dialogs::guid_text {

struct GuidBytes { uint64_t hi = 0, lo = 0; };

// A FRESH GUID per call, mapped to the text (:60-69; reusing one GUID per text crashed, :44-49).
GuidBytes alloc(const std::string& text);

// Change the text of an existing GUID (:1077-1087). False when the GUID is not in the table.
bool retext(const GuidBytes& gb, const std::string& text);

// The hook's lookup (:82-86). False on a miss, which is nearly every call.
bool lookup(uint64_t hi, uint64_t lo, std::string& out);

// A new unit tree starts: the GUIDs issued from now on belong to it.
void begin_tree();

// Drop the texts of the trees built before the current one (keep_current), or all of them.
void purge(bool keep_current);

size_t count();   // GUIDs issued since load (:70 guid_count, a census: never reset)
size_t live();    // texts in the table

}  // namespace dialogs::guid_text
