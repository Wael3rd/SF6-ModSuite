// dialogs::guid_text - GUID text provider for the native option engine (phase 4b): the table (guid_table.hpp)
// plus the hook on hMsg.GetMessage(System.Guid) that makes the game resolve our GUIDs to our texts.
//
// Design: docs/CPP_PORT_PHASE4.md section 4b "GUID text provider".
// Ported from: func/NativeOptions.lua :42-104 (guid_text table, message_guid, guid_lookup, managed_string, the
// GetMessage hook, set_guid).
//
// Hot path: GetMessage sees EVERY message of the whole UI, online included. Nothing registered = one atomic read;
// otherwise two qword reads and one lookup. SEH around the hook bodies; a fault counter switches the hook off.
#pragma once

#include <string>

#include "guid_table.hpp"

namespace dialogs::guid_text {

bool install();   // Install the GetMessage hook; false on a hard failure
bool ready();     // Hook in place and not switched off by faults

// Write a GuidBytes into a managed object's System.Guid field (16 bytes at the field's offset, :101-104 set_guid).
bool set_field(void* obj, const char* field_name, const GuidBytes& gb);

std::string status();

}  // namespace dialogs::guid_text
