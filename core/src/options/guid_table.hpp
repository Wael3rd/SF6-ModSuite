// dialogs::guid_text - the GUID -> text table of the native option engine, without any game dependency
// (phase 4b). Compiled as it is into the DLL (guid_text.cpp adds the hMsg.GetMessage hook) and into the offline
// harness (tools/options_harness).
//
// Ported from func/NativeOptions.lua :42-69 (guid_text table, message_guid) and :1077-1087 (message_text).
// Decision Q3 (docs/CPP_PORT_PHASE4.md section 8): the texts are dropped when the option roots are purged, which
// the Lua never did (its table only grows). The texts of the tree that stays in the game's list are kept: that
// tree is opened again without a rebuild, and its root row stays visible in the game's own Options screen.
//
// Owned texts (core increment 8, ADR-0016): the same table also holds texts owned by a core
// client (the pause-menu engine). The option engine's purge never touches them; only their owner releases them.
// One table, one GetMessage hook in the process.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dialogs::guid_text {

struct GuidBytes { uint64_t hi = 0, lo = 0; };

// A FRESH GUID per call, mapped to the text (:60-69; reusing one GUID per text crashed, :44-49).
GuidBytes alloc(const std::string& text);

// Change the text of an existing GUID (:1077-1087). False when the GUID is not in the table (or is a client text).
bool retext(const GuidBytes& gb, const std::string& text);

// The hook's lookup (:82-86). False on a miss, which is nearly every call.
bool lookup(uint64_t hi, uint64_t lo, std::string& out);

// A new unit tree starts: the GUIDs issued from now on belong to it.
void begin_tree();

// Drop the texts of the trees built before the current one (keep_current), or all of them. Client texts stay.
void purge(bool keep_current);

size_t count();   // GUIDs issued since load (:70 guid_count, a census: never reset)
size_t live();    // texts in the table (option engine + clients)

// ---- owned texts (the training pause-menu engine) ---------------------------------------------------------------
using Owner = const void*;            // the owning client's handle (never null for a client text)

// A fresh GUID owned by `owner`. `keep(owner)` is checked under the table's lock: false (the owner unregistered
// meanwhile) = nothing stored, returns false. *owned_after = the owner's text count after the call.
bool alloc_owned(Owner owner, const std::string& text, bool (*keep)(Owner), GuidBytes& out, size_t* owned_after);
// New text for one of the owner's GUIDs. False when the GUID is not the owner's.
bool retext_owned(Owner owner, const GuidBytes& gb, const std::string& text);
// Forget one of the owner's GUIDs. False when the GUID is not the owner's.
bool release_owned(Owner owner, const GuidBytes& gb);

struct Stats {
    size_t live = 0, owned = 0;                  // texts in the table, of which client texts
    uint64_t lookups = 0, hits = 0, owned_hits = 0;  // hook lookups, found, found among client texts
};
Stats stats();

}  // namespace dialogs::guid_text
