// SF6 Mod Suite updater: what touches files (no game, no network). Detection of the installed packages, the downloaded zips checked,
// their extraction into a staging tree (every entry checked first: one refused entry and nothing is written), the swap into the game
// folder (all or nothing, with its journal for a crash in the middle) and the clean-up of the old copies at the next start.
//
// Every path given here is a game folder or a folder inside it (the staging tree is in reframework/data/SF6_Update_data, the same
// volume: a move is a rename). Wide paths throughout; zip names are UTF-8.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "update_logic.hpp"

namespace sf6up {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::wstring join(const std::wstring& root, const std::string& rel);   // rel '/' separated
bool exists(const std::wstring& p);
bool make_dirs(const std::wstring& dir);                               // with its parents; true when it is there
bool read_file(const std::wstring& p, std::string* out, size_t max_bytes);
bool write_file(const std::wstring& p, const std::string& data);       // written beside, flushed, then moved over the old one
int64_t file_size(const std::wstring& p);                               // -1: none
bool same_content(const std::wstring& a, const std::wstring& b);
std::string sha256_file(const std::wstring& p);                         // lowercase hex, "" when unreadable
void remove_tree(const std::wstring& dir);

// ---- installed packages ----
// What a package's main DLL reports (in game: its sf6suite_package_version export, update_game.cpp); "" = nothing
using VersionReader = std::function<std::string(const KnownPackage&)>;
std::vector<Installed> detect_installed(const std::wstring& root, const VersionReader& version_of);
// The Slotsloader Manager installed the Slots Loader (common/manager_mode.hpp, read as the plugin reads it). why: what decided, for
// a log line ("" when there is no list)
bool manager_mode(const std::wstring& root, std::string* why = nullptr);
// The Manager's list holds `rel` (relative to the game folder) in an enabled mod, at the size the file has on disk and, when the entry
// gives a sha256, with that SHA-256: the Manager installed that file, whatever the package (the same test as manager_mode's, for any
// file)
bool manager_lists(const std::wstring& root, const std::string& rel);

// ---- a downloaded zip: its size and sha256 as the manifest says ----
bool verify_file(const std::wstring& p, int64_t size, const std::string& sha256, std::string* why);

// ---- extraction ----
struct StagedFile {
    std::string rel;              // the game path ('/' separated, as the zip names it)
    Entry kind = Entry::File;     // File or KeepIfPresent
    std::string package;          // its package id
    std::string from;             // that package's installed version (the old copy's name)
};
struct Staged {
    std::vector<StagedFile> files;
    std::vector<std::string> dirs;
};
// Every entry classified first (classify_entry): any Refuse, a name twice, an encrypted entry, a non-UTF-8 name, an entry over 64 MiB or
// 256 MiB in all: false and nothing written. Then the files go into tree/<rel>.
bool extract_zip(const std::wstring& zip, const std::wstring& tree, const std::string& package, const std::string& from, Staged* out,
                 std::string* why);

// One file of a zip's root, by its exact name (case apart), written to `dest`: a tool's program (the manifest's "tools"), which
// goes beside the running one, not into the game's folders. The other entries are not looked at and never written.
bool extract_one(const std::wstring& zip, const std::string& name, const std::wstring& dest, std::string* why);

// ---- the swap ----
struct SwapResult {
    bool ok = false;
    std::string why;
    std::vector<std::string> backups;   // the old copies kept (relative paths): pending_cleanup
    int replaced = 0, added = 0, kept = 0, same = 0, dirs = 0;
};
// For each staged file: kept (KeepIfPresent and the game has one), left (same bytes), or the game's file renamed to
// <name>.old-<from> and the staged one moved in its place. Any failure: every step undone, in reverse. The journal is written first and
// removed at the end, so that a crash in the middle is undone at the next start (recover_journal). fail_at: tests only, the step that
// fails (-1: none).
SwapResult swap_in(const std::wstring& root, const std::wstring& tree, const Staged& staged, const std::wstring& journal, int fail_at = -1);
// A journal left by a swap that did not finish: undone. True when there was one.
bool recover_journal(const std::wstring& root, const std::wstring& journal, std::string* log);
// The old copies of the last update, once the new version has run: deleted. Returns the ones still there (to try again).
std::vector<std::string> cleanup_backups(const std::wstring& root, const std::vector<std::string>& list, std::string* log);

}  // namespace sf6up
