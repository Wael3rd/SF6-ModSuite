// vfx::engine - SF6_MemWriterNative absorbed (owner "vfx_engine", phase 2 step 2): see engine.cpp.
// The Lua (SF6_DriveRecolor, EfxMeshColor) hands over through the core's Lua bridge the texts it wrote to files.
#pragma once

#include <string>

namespace vfx::engine {

bool install();                              // hooks + update callbacks; false = refused (MemWriter loaded, API missing)
bool ready();                                // installed
void relay(const std::string& text);         // was _drivecolors.txt (re-read at the next Update / LateUpdate)
void relay_native(const std::string& text);  // the same from vfx::tick (owner vfx): no manager reset on an empty relay
void queue(const std::string& text);         // was _memwrite.txt (appended; applied at the next Update / LateUpdate)
void scan_request(const std::string& text);  // was _drivescan.txt (run at the next UpdateBehavior)
std::string scan_result();                   // was _drivespots.txt (the last result, "" before the first)
std::string status();

}  // namespace vfx::engine
