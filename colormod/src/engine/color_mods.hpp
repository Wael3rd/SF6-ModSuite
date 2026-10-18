// reframework/color_mods: colour mods dropped as downloaded, handled like the costume_mods folder.
//   color_mods/<Character>/<mod .zip / .7z / .rar>       (or an unpacked folder, or loose esfNNN_CCC_cmd_KKK.user.2 files)
// Archives are unpacked once, off the game thread, into color_mods/.cache/<name>_<hash>/ (only the colour files and
// modinfo.ini are kept). The catalogue (catalog.cpp) reads the folders and the complete cache folders of the archives
// that are still installed. Nothing here touches a game object; no function runs inside a hook or a fight frame.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine::color_mods {

// Where the colour mods live, from the game's reframework/data folder.
std::filesystem::path root_of(const std::filesystem::path& data_dir);

// The text of color_mods/README.txt (English).
const char* readme_text();

// Archive files (.zip / .7z / .rar by extension, checked by signature when extracted) under root, skipping the
// folders and files whose name starts with '.'.
std::vector<std::filesystem::path> list_archives(const std::filesystem::path& root);

// Cache folder name of an archive: "<name, 24 safe chars>_<fnv1a32 of name + size + mtime>".
std::string cache_key(const std::filesystem::path& archive);

// The COMPLETE cache folders whose archive is still installed (cheap: one directory listing and a few stats, safe
// at startup).
std::vector<std::filesystem::path> valid_cache_dirs(const std::filesystem::path& root);

// Every regular *.user.2 file the catalogue should look at: under root outside dot folders, plus under the valid cache
// folders. The catalogue applies the file name rule and the size rule.
std::vector<std::filesystem::path> candidate_files(const std::filesystem::path& root);

// Creates color_mods, the <Character> folders and README.txt when missing (worker thread).
void ensure_layout(const std::filesystem::path& root);

struct SyncReport {
    int extracted = 0, failed = 0, purged = 0, already = 0;
    bool changed() const { return extracted > 0 || purged > 0; }
};
// Extracts the archives that have no complete cache folder, removes the cache folders of archives that are gone or
// changed and the leftovers of an interrupted extraction. `log` gets one line per event (called from this thread).
SyncReport sync(const std::filesystem::path& root, void (*log)(const std::string&));

// ---- background worker (started once, after the game's first frames) -------------------------------------------------
void start_worker(const std::filesystem::path& root, void (*log)(const std::string&));   // idempotent
bool take_changed();                                    // true once after the worker added or removed cache folders

}  // namespace engine::color_mods
