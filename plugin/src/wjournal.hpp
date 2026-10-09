// wjournal: every game-object write the plugin makes, for crash investigation.
// Off unless reframework/data/write_journal_on.txt exists (checked every 120 frames).
// When on, writes to reframework/data/SF6_CostumeSlots_data/writes.log through the
// shared background writer (perf.cpp).  When off, on() is one relaxed atomic load.
#pragma once

namespace wjournal {
bool on();
void frame();                      // call once per LateUpdateBehavior frame
void log(const char* fmt, ...);    // HH:MM:SS.mmm f<frame> T<tid> prefix added
}  // namespace wjournal
