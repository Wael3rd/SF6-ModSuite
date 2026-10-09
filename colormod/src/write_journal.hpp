// write_journal: off-by-default raw-write journal for memory-corruption diagnosis.
// When reframework/data/write_journal_on.txt exists, every raw write the Color Mod makes
// into game memory is logged to an in-memory buffer and flushed to
// reframework/data/SF6_ColorModNative_data/writes.log once per second and at closing.
// Cost when off: one relaxed atomic load per write site.
#pragma once

#include <cstdint>

namespace wjournal {

bool active();                                   // relaxed load; the only call when off
void check();                                    // stat the sentinel file (call every 120 frames)
void flush();                                    // drain the buffer to disk via bgwriter::append

// Raw 32-bit write
void raw32(const char* tag, uint64_t addr, uint32_t old_val, uint32_t new_val,
           uint64_t obj = 0, uint64_t ti = 0);
// Raw 8-bit write
void raw8(const char* tag, uint64_t addr, uint8_t old_val, uint8_t new_val);
// Managed field write (ll::write_field_*)
void field(const char* tag, uint64_t obj_addr, const char* field_name,
           uint32_t old_val, uint32_t new_val);
// SetColor method invoke
void set_color(const char* tag, uint64_t ctrl_addr, uint32_t color_num);

}  // namespace wjournal
