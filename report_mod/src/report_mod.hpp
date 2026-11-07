// report_mod.dll: "Report a bug" on Fighter Settings, a small DLL on the SF6 Native Core: the guide entry, the menu, the picture and the upload.
//
// bug_report.cpp and report/ are its own. The rest is the Recording Manager's source, compiled here from crsm/src and copied nowhere, so the two cannot drift: community/ (HTTP, Steam
// ticket, token), the text dialog (ui/dialogs.cpp), ui/gx.cpp, ui/lang.cpp, ui/logic.cpp, ui/details_logic.cpp, mech/hooks.cpp (the code check), game/gm.cpp, model/. Those files call
// the Recording Manager's cmn:: and ui:: layers; host.cpp stands in for the few functions they use. A new call in them is a link error here, never a silent difference.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "mech/hooks.hpp"

namespace cmn {
// Called once at the next Present (the render thread), then it clears itself by set_present_fn(nullptr). One callback of REFramework's for the whole DLL: there is no taking one back,
// and null here costs one atomic load per Present.
void set_present_fn(void (*fn)());
}  // namespace cmn

namespace ui {
void report_needs_dialogs(bool on);              // a report is under way: the text dialog's hooks are on
}  // namespace ui

namespace ui::bug_report {
void specs(std::vector<mech::hooks::Spec>& out);
void start();
void hand_back();
std::string status();
}  // namespace ui::bug_report

namespace report_host {
// The frame callback's first thing, while the module is awake (asleep there is no frame callback): the frame count, the keys, the client's answers, the Steam calls of a ticket, the journal
void frame(uint64_t core_frame);
void flush_journal();
void start_network();                            // the first press: the Steam engine, the client's config and worker
void asleep();                                   // the frame callback was given back
SF6CoreClient* core_client();                    // made at the first wake; nullptr when the core refuses
void note_paused(bool paused);
}  // namespace report_host
