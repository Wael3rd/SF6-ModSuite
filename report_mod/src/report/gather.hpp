// report::gather - what a report reads from the disk, off the game thread (never called on it). Everything is relative to the game folder:
// nothing outside it, and only the logs of logic.hpp's kLogs. Whatever cannot be read leaves a note in the report, which still goes:
// the Recording Manager must not need the Slots Loader to be there.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "report/report_logic.hpp"

namespace report {

// What the game thread read when the key was pressed, and what the player wrote
struct Snapshot {
    int fighter = -1;                          // < 0: not found on the screen
    int costume_no = -1;                       // < 0: not read: the outfit is found by the row's text
    std::string character;                     // the English name (model::chars)
    std::string outfit_row, colour_row;        // what the screen's rows show
    std::string text;
    std::string reporter_version, core_version;   // report_mod's, the core's
    std::vector<uint8_t> jpeg;
    std::string jpeg_note;                     // why there is none
};

// The Windows user's name and the PC's, and the game folder, from this process
Scrub scrub_here(const std::filesystem::path& root);
Report gather(const Snapshot& snap, const std::filesystem::path& root, const Scrub& scrub);

}  // namespace report
