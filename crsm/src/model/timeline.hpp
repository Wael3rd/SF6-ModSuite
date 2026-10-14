// model::timeline — the numpad <-> input mask codec and the run-length lines "Nf : 5 + LP" of a recorded slot
// (MAIN:509-538 codec, 962-1004 serialize, 700-701 / 1027-1035 expand, RMT:480-483 entry_frames). Pure C++, no game type.
// Every function mirrors the Lua one it comes from, quirks included (see the notes at each function).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace model::timeline {

// MASKS of MAIN:507, absolute (no facing): the dummy is always P2.
enum Mask : uint32_t { UP = 1, DOWN = 2, RIGHT = 4, LEFT = 8, LP = 16, MP = 32, HP = 64, LK = 128, MK = 256, HK = 512 };

// decode_to_numpad: "6", "2 + LP + HK"... The direction is a numpad digit (UP wins over DOWN, LEFT over RIGHT, as in
// the Lua), then the buttons in the order LP MP HP LK MK HK, joined with " + ".
std::string decode_to_numpad(uint32_t val);

// encode_from_numpad: tokens split on whitespace and '+' ("[^%s+]+"), a digit 1-9 ORs its direction (5 = none), a
// button name ORs its mask, anything else is ignored. The result is not limited to 16 bits (the Lua integer).
uint32_t encode_from_numpad(std::string_view str);

// The compressed shape of a recording: one "<n>f : <numpad>" line per run of equal raw values (the runs are cut on
// the raw value, not on its text: two raw values with the same text give two lines). frames == 0 gives no line.
std::vector<std::string> compress(const uint16_t* values, size_t frames);

// string.match(e, "(%d+)f") summed over the lines (MAIN:~640 "needed"): the first digits followed by 'f' anywhere in
// the line, a line without one counts 0. Saturates at kMaxFrames.
constexpr int64_t kMaxFrames = 100000000;
int64_t needed_frames(const std::vector<std::string>& lines);

// string.match(e, "^(%d+)f") summed over the lines (entry_frames, fill_record): anchored, a line without it counts 0.
int64_t entry_frames(const std::vector<std::string>& lines);

// One line "(%d+)f : (.+)" (first match, unanchored, as string.match). Returns false when the line has none.
struct Run { int64_t frames = 0; std::string inputs; };
bool parse_run(std::string_view line, Run* out);

// The WRITE DATA expansion (MAIN:697-702): each matching line gives `frames` copies of the encoded value. A line that
// does not match is the Lua error "'for' limit must be a number": ok = false and the lines before it are kept.
struct Expanded { std::vector<uint16_t> values; bool ok = true; };
Expanded expand(const std::vector<std::string>& lines);

// fill_record's variant (MAIN:1027-1035): anchored "^(%d+)f : (.+)", a line that does not match is skipped.
std::vector<uint16_t> expand_anchored(const std::vector<std::string>& lines);

}  // namespace model::timeline
