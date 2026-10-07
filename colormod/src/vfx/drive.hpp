// vfx::drive - the pure part of SF6_DriveRecolor.lua's battle side (phase 2 step 3, sub-steps C-D): the spots (the
// Drive palette's colours found in the loaded .efx resources), their revalidation (spot_ok, remember), the scan request
// and result texts exchanged with vfx::engine, _spots.json, and the relay text (write_handoff). No REFramework
// dependency: tools/vfx_harness compares it with the shipped Lua. vfx::tick does the game side.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vfx::drive {

struct Spot { uint64_t addr = 0; int r = 0, g = 0, b = 0; std::string file; std::vector<uint32_t> written; };
struct Base { std::string file; uint64_t arr = 0; uint32_t count = 0; };

void remember(Spot& sp, uint32_t col);                   // our last 8 written values, no duplicate
// cands = the c1 colours of the current settings (validate_hook); cur = nullopt when unreadable
bool spot_ok(std::optional<uint32_t> cur, int r, int g, int b, const std::vector<uint32_t>& written, const std::vector<uint32_t>& cands);
std::string famille(const std::string& file);            // "parry" / "rush" / "special" / "" (nil)
std::string palette_sig();                               // "<files>/<colours>" of the Drive palette
std::string bases_sig(const std::vector<Base>& bases);   // "<n>:<sum hex>"

std::string scan_request(uint64_t seq, const std::vector<Base>& bases);   // "R" / "P" / "F" / "END"
// the engine's result for request seq: false while absent / incomplete / for another request
bool scan_result(const std::string& body, uint64_t seq, std::vector<Spot>& found, int& nodes, int& ms);

// _spots.json (the Lua's format; a 6th element carries our written values) and _spots_sig.txt
std::vector<Spot> load_spots_file(const std::string& path, size_t& total);
bool save_spots_file(const std::string& path, const std::vector<Spot>& spots);

// relay text (write_handoff). Colours per side 1..2: parry, rush, special (c1, c2; nullopt = keep the game's).
struct ColorPair { std::optional<uint32_t> c1, c2; };
struct Tint { uint64_t addr; uint32_t cur; uint64_t obj, ti; };      // tint_addresses
struct DiBlock { bool present = false; int container = -1; std::vector<std::string> lines; };   // di_lines / di_container
struct HandoffIn {
    uint64_t mgr[3] = {};                                 // [1], [2]
    ColorPair col[3][3];                                  // [side][0 parry, 1 rush, 2 special]
    DiBlock di[3][2];                                     // [side][0 impact, 1 reversal]
    std::vector<Tint> tints;
};
// live = spots_live (indices into spots); remember() is applied to them like the Lua does
std::string handoff(const HandoffIn& in, std::vector<Spot>& spots, const std::vector<size_t>& live, size_t& nlines);
std::string w_line(uint64_t addr, uint32_t val, uint32_t orig);                       // "W %X %08X %08X"
std::string w_line(uint64_t addr, uint32_t val, uint32_t orig, uint64_t obj, uint64_t ti);

}  // namespace vfx::drive
