// Probes of phase P0: questions the port needs answered by the game, asked by
// the DLL itself. Off by default. On when reframework/data/SF6_CRSMNative_data/probes_on.txt exists.
//
//   P1   path A = path B for every fighter: GetFighterIndex(fid) == fid and FighterDataList[i].FighterID == i, Record and
//        Reversal lists. Once per game run, in training.
//   P2   thread of each hook family: pass-through hooks, the thread ids seen per hook against the core's frame thread.
//   P11  does TrainingManager.get_IsMenuOpening() / OtherSetting.IsMenuOpend lead the pause bit at menu open: edge log.
//   C4e  call counters on the tiny setters and the four constant TrainingMenuFunc methods (a rate far above the
//        screen's activity = a folded body, R18); one log line every 10 s; duplicate method addresses at install.
//
// The hooks are installed at the start only when the file exists then (no cost otherwise); the frame probes (P1,
// P11, the reports) look at the file every 300 frames. Results: reframework/data/SF6_CRSMNative_data/probes.log
// (plain text, appended) and the journal. Hooks are pass-through (CALL_ORIGINAL), count only, under SEH.
#pragma once

#include <cstdint>
#include <string>

namespace probes {

bool requested();                 // probes_on.txt exists
void install();                   // hooks, only when requested(); call once after the core is connected
void on_frame(uint64_t frame);    // from the core's frame callback (game thread)
std::string status();             // one line for status.json

}  // namespace probes
