// Slotsloader Manager mode: is this Slots Loader the one ZZtaii's Slotsloader Manager installed?
//
// One package for everybody. When the Manager installed the loader, the loader and the plugin behave as
// the Manager's own Slotsloader fork did (docs/MANAGER_CONTRACT.md, "Manager mode"); otherwise they behave
// as the Slots Loader always has. Nobody sets anything: the Manager already writes
// reframework/data/SF6_Costumes_Data/manager_installs.json, which lists every mod it installed, the loader
// included, with each file's size. The loader is the Manager's when an enabled mod of that list holds the
// plugin (reframework/plugins/SF6_CostumeSlotsNative.dll) at the size the plugin has on disk:
//   - the loader installed by hand or by Fluffy Mod Manager: the plugin is not in the list, or another
//     tool wrote over it since (other size): normal mode;
//   - the Manager removed, its last list left behind: the plugin it installed is still the one in the game,
//     so the game still runs the Manager's install, and stays in Manager mode until that install changes.
// Pure logic (no file access): the loader (loader_core.cpp) and the plugin (costumes.cpp) read the list with
// their own JSON readers and ask the same question here.
#pragma once

#include <cctype>
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
};

// Lower case, '/' only, no leading '/'
inline std::string norm_path(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) out += c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
    size_t lead = out.find_first_not_of('/');
    return lead == std::string::npos ? std::string() : out.substr(lead);
}

// format: the list's "format"; files: every file of every mod; plugin_size: the plugin's size on disk
// (-1: not found). True when the Manager installed the plugin the game runs.
inline bool installed_by_manager(const std::string& format, const std::vector<ListedFile>& files,
                                 long long plugin_size) {
    if (format != kRecordFormat || plugin_size < 0) return false;
    for (auto& f : files)
        if (f.enabled && f.size == plugin_size && norm_path(f.path) == kPluginPath) return true;
    return false;
}

} // namespace manager_mode
