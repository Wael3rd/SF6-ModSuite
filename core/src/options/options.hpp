// dialogs::options - the native option-dialog engine (phase 4b): the game side.
// Ported from: func/NativeOptions.lua. Design: docs/CPP_PORT_PHASE4.md section 4b.
//
// The engine itself (registry, dialog life cycle, value polling, events) is options_core.{hpp,cpp}, free of any
// game header so the offline harness compiles the very file that ships. This file is what the core cannot do
// without the game: building the app.Option units, starting and ending the dialogs, the eight hooks
// (FlowEvent_ResetCurrentUnits, GetFocusDecideEventType, CreatedObject, SetupDispUnits, CheckCanSelect,
// LoadValueEvent, ResetEvent, TrainingManager.OpenMenu; GetMessage is guid_text.cpp) and the row lookups.
//
// The Lua callers (the costume editor until phase 4d, the DI colour menu until 4e) go through the native block
// of func/NativeOptions.lua, which drives the core's ticks and pulls its events.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dialogs::options {

bool install();              // Backend + hooks; false on a hard failure
bool ready();                // Hooks in place and not switched off by faults
void note_fault(const char* where);   // a bridge call raised: counted like a hook fault
void set_purge_pause_rows(bool on);   // :267 the next builds remove (or keep) the dynamic rows of the training pause menu

// core::add_entry with the kind as the Lua names it ("toggle", "choice", "slider", "button")
int64_t add_entry(int64_t group_id, const std::string& kind, const std::string& key);

// ---- queries that read the game -------------------------------------------------------------------------------
int64_t focused_entry();                                    // id of the entry under the cursor, -1 when not ours (:1029-1038)
uint64_t row_control(int64_t entry_id, uint64_t param_addr); // via.gui.Control of the row; param 0 = the current page (:1058-1073)
uint64_t row_part(int64_t entry_id);                        // UIPartsOptionUnit of the row on the current page (:1040-1055)
bool set_option_text(int64_t entry_id, int index, const std::string& text);   // retext one option in place (:1102-1111)

std::string status();

}  // namespace dialogs::options
