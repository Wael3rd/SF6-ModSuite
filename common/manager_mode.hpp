// Slotsloader Manager mode: is this Slots Loader the one ZZtaii's Slotsloader Manager installed?
//
// One package for everybody. When the Manager installed the loader, the loader and the plugin behave as
// the Manager's own Slotsloader fork did (docs/MANAGER_CONTRACT.md, "Manager mode"); otherwise they behave
// as the Slots Loader always has. Nobody sets anything: the Manager already writes
// reframework/data/SF6_Costumes_Data/manager_installs.json, which lists every mod it installed, the loader
// included, with each file's size and SHA-256. The loader is the Manager's when an enabled mod of that list
// holds the plugin (reframework/plugins/SF6_CostumeSlotsNative.dll) at the size the plugin has on disk and,
// when that entry gives a "sha256", with the SHA-256 the plugin has on disk:
//   - the loader installed by hand or by Fluffy Mod Manager: the plugin is not in the list, or another
//     tool wrote over it since (other size, or same size and other bytes): normal mode;
//   - the Manager removed, its last list left behind: the plugin it installed is still the one in the game,
//     so the game still runs the Manager's install, and stays in Manager mode until that install changes.
//     A byte-identical plugin put back by hand cannot be told from the Manager's own copy and still matches.
//   - a list of an older Manager without "sha256": the size alone decides, as before.
// The plugin's SHA-256 is asked for only when an entry at its size gives one (plugin_sha256 is called at most
// once). Pure logic (no file access): the loader (loader_core.cpp), the plugin (costumes.cpp), the updater
// (update_fs.cpp) and Report a bug (report_mod host.cpp) read the list with their own JSON readers and ask the
// same question here; common/file_sha256.hpp hashes the file for those that have no hasher of their own.
#pragma once

#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace manager_mode {

constexpr const char* kRecordFile   = "manager_installs.json";            // in reframework/data/SF6_Costumes_Data
constexpr const char* kRecordDir    = "reframework\\data\\SF6_Costumes_Data";
constexpr const char* kRecordFormat = "sf6-manager-installs-1";
constexpr const char* kPluginPath   = "reframework/plugins/sf6_costumeslotsnative.dll";   // normalised (norm_path)

// One file of one mod of the list
struct ListedFile {
    bool enabled = true;      // the mod's "enabled"
    std::string path;         // as written ("reframework/plugins/SF6_CostumeSlotsNative.dll", any case and slashes)
    long long size = -1;      // -1: not given
    std::string sha256;       // the entry's "sha256", lower case hex; empty: not given
};

// Lower case, '/' only, no leading '/'
inline std::string norm_path(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) out += c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    size_t lead = out.find_first_not_of('/');
    return lead == std::string::npos ? std::string() : out.substr(lead);
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// How one file on disk compares with the list's entries for its path
enum class Match {
    SizeAndHash,   // an enabled entry at its size with its SHA-256
    SizeOnly,      // an enabled entry at its size that gives no SHA-256
    HashDiffers,   // listed at its size, but the file's SHA-256 is another (or the file could not be read)
    OtherSize,     // listed, at another size
    Off,           // listed only in mods turned off in the Manager
    NotListed,
    NoFile,        // not on disk
};

// path: normalised (norm_path); size: the file's size on disk (-1: not there); sha256: the file's SHA-256 as
// lower case hex ("" when it cannot be read), asked for at most once and only when needed.
inline Match match_file(const std::vector<ListedFile>& files, const std::string& path, long long size,
                        const std::function<std::string()>& sha256) {
    if (size < 0) return Match::NoFile;
    bool listed = false, off = false, differs = false, hashed = false;
    std::string have;
    for (auto& f : files) {
        if (norm_path(f.path) != path) continue;
        if (!f.enabled) { off = true; continue; }
        listed = true;
        if (f.size != size) continue;
        if (f.sha256.empty()) return Match::SizeOnly;
        if (!hashed) { have = sha256 ? lower(sha256()) : std::string(); hashed = true; }
        if (!have.empty() && have == lower(f.sha256)) return Match::SizeAndHash;
        differs = true;
    }
    if (differs) return Match::HashDiffers;
    if (listed) return Match::OtherSize;
    return off ? Match::Off : Match::NotListed;
}

inline bool matched(Match m) { return m == Match::SizeAndHash || m == Match::SizeOnly; }

// For a log line: how the plugin compared with the list
inline const char* how(Match m) {
    switch (m) {
        case Match::SizeAndHash: return "the plugin's size and sha256 matched";
        case Match::SizeOnly:    return "the plugin's size matched, the list gives no sha256";
        case Match::HashDiffers: return "the plugin is listed at its size, but its sha256 differs";
        case Match::OtherSize:   return "the plugin is listed at another size";
        case Match::Off:         return "the plugin is listed only in a mod turned off in the Manager";
        case Match::NotListed:   return "the plugin is not listed";
        case Match::NoFile:      return "no plugin on disk";
    }
    return "";
}

struct Decision {
    bool manager = false;
    std::string why;          // for a log line: what decided
};

// format: the list's "format"; files: every file of every mod; plugin_size: the plugin's size on disk
// (-1: not found); plugin_sha256: the plugin's SHA-256 (called only when needed). Manager mode when the
// Manager installed the plugin the game runs.
inline Decision decide(const std::string& format, const std::vector<ListedFile>& files, long long plugin_size,
                       const std::function<std::string()>& plugin_sha256) {
    if (format != kRecordFormat) return { false, "the list's format is not " + std::string(kRecordFormat) };
    const Match m = match_file(files, kPluginPath, plugin_size, plugin_sha256);
    return { matched(m), how(m) };
}

inline bool installed_by_manager(const std::string& format, const std::vector<ListedFile>& files,
                                 long long plugin_size, const std::function<std::string()>& plugin_sha256) {
    return decide(format, files, plugin_size, plugin_sha256).manager;
}

} // namespace manager_mode
