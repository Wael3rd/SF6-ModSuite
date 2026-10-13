#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// One message to add to a game .msg.21 file (RE Engine GMSG, version 21).
struct MsgEntry {
    std::string guid;   // "a1b2c3d4-0001-4000-8000-000000005000" (hyphenated or 32 hex digits)
    std::string name;   // entry name, ASCII ("FighterCostumeMessage6")
    std::string text;   // UTF-8, written as UTF-16 and used for every language of the file
};

// Appends the entries the way REasy-parser's MsgHandler.add_entry() + rebuild() does, and
// reproduces its output byte for byte: entries sorted by lower-cased name, name hashes
// recomputed, string pool rewritten (one copy per reference) and re-encrypted.
// Returns false and leaves `msg` untouched on any layout it does not recognise; `err` (may be
// null) says why. Only the encrypted by-hash versions (21) without user parameters are accepted.
bool add_msg_entries(std::vector<uint8_t>& msg, const std::vector<MsgEntry>& entries,
                     std::string* err);

// Reads one language of a game .msg.21 file: entry name (ASCII) -> text (UTF-8). `language` is the
// via.Language id (0 = Japanese, 1 = English); the column is found through the file's language table.
// Returns false (and an empty `out`) on any layout it does not recognise, or when the file has no
// such language; `err` (may be null) says why. Read-only: the file is not modified.
bool read_msg_texts(const std::vector<uint8_t>& msg, uint32_t language,
                    std::map<std::string, std::string>& out, std::string* err = nullptr);
