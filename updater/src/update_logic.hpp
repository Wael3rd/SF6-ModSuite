// SF6 Mod Suite updater: the pure part (no game, no network, no file). Versions, the release manifest, what to offer, which zip
// entries may be written where, the module's own state. Compiled into its two hosts (the Native Core and the Slots Loader's plugin)
// and into its tests (updater/tests).
//
// The manifest (manifest.json, an asset of every GitHub release since the updater, written by packaging/make_manifest.py):
//   { "format": "sf6suite-manifest-1", "suite": "1.30.1", "tag": "v1.30.1",
//     "packages": [ { "id": "color_mod", "name": "SF6 Color Mod", "version": "0.26.1", "asset": "SF6_ColorMod_0.26.1.zip",
//                     "size": 1418590, "sha256": "<64 hex>", "proof": "reframework/plugins/SF6_ColorModNative.dll" }, ... ],
//     "tools": [ { "id": "mod_sorter", "name": "SF6 Mod Sorter", "version": "1.1.0", "asset": "SF6_ModSorter_1.1.0.zip",
//                  "size": 974145, "sha256": "<64 hex>", "file": "SF6_ModSorter.exe" } ] }
// "tools" (optional, since suite 1.34.0): the programs of the release that are no package and update themselves (the Mod Sorter
// reads its own entry; the game never installs one). The updaters before it ignore the key.
// Read as hostile: 64 KiB at most, nesting 8 at most, every field of the expected type, a known id's proof must be the one this code
// knows (the manifest cannot point the updater at another file), an unknown id is skipped (a later package an older updater does not
// know), an asset is a bare file name, its size 64 MiB at most. Anything else: the whole manifest is refused. A tool's entry that is
// not as expected is left out alone (tool_notes): a tool never stops the packages' update.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sf6up {

// ---- versions: "1.26.1", "0.9.10", "2.2.9-beta" (1 to 4 numbers, an optional -suffix that sorts before the release) ----
struct Version {
    std::vector<uint32_t> n;
    std::string pre;              // "" = a release
};
std::optional<Version> parse_version(const std::string& s);
// -1 a < b, 0 equal, 1 a > b; nullopt when either is not a version
std::optional<int> compare_versions(const std::string& a, const std::string& b);
// a strictly newer than b (false when either is not a version: never offered on a doubt)
bool newer(const std::string& a, const std::string& b);

// ---- the packages this code knows: their id, their name, the file that proves one is installed (its main DLL) ----
struct KnownPackage {
    const char* id;
    const char* name;
    const char* proof;            // relative to the game folder, '/' separated
};
const std::vector<KnownPackage>& known_packages();
const KnownPackage* find_known(const std::string& id);

// ---- the manifest ----
constexpr const char* kManifestFormat = "sf6suite-manifest-1";
constexpr size_t kManifestMaxBytes = 64 * 1024;
constexpr int64_t kAssetMaxBytes = 64ll * 1024 * 1024;
constexpr size_t kMaxPackages = 16;
constexpr size_t kMaxTools = 8;

struct ManifestPackage {
    std::string id, name, version, asset, sha256, proof;
    int64_t size = 0;
};
struct ManifestTool {
    std::string id, name, version, asset, sha256;
    std::string file;                        // the program at the zip's root: a bare .exe name
    int64_t size = 0;
};
struct Manifest {
    std::string format, suite, tag;
    std::vector<ManifestPackage> packages;   // the known ids only
    std::vector<std::string> skipped;        // ids it does not know (logged)
    std::vector<ManifestTool> tools;         // the entries as expected only
    std::vector<std::string> tool_notes;     // why a tool's entry was left out (logged)
};
bool parse_manifest(const std::string& text, Manifest* out, std::string* why);
const ManifestTool* find_tool(const Manifest& m, const std::string& id);
// The text make_manifest.py writes, for the tests' round trip
std::string manifest_json(const Manifest& m);

// ---- what to offer ----
struct Installed {
    std::string id;
    bool present = false;         // its proof file is in the game folder
    std::string version;          // what its main DLL reports ("" = it reports none)
    bool managed = false;         // the Slotsloader Manager's list holds its proof file at the size it has on disk: the Manager installed it
};
struct PlanItem {
    ManifestPackage pkg;
    std::string from;             // the installed version
};
struct Plan {
    std::string suite, tag;
    std::vector<PlanItem> items;
    std::vector<std::string> notes;          // why a package is not offered, for the log
};
// A package installed whose manifest version is strictly newer than what its DLL reports. The Slots Loader is left out when the
// Slotsloader Manager installed it (manager_mode): the Manager owns those files. In that mode any other package the Manager
// installed is left out too (Installed::managed: the Native Core, say): replacing its files would leave the Manager's record of
// them wrong. Outside Manager mode nothing changes. Nothing when `ignored` is the manifest's suite.
Plan make_plan(const Manifest& m, const std::vector<Installed>& installed, bool manager_mode, const std::string& ignored);

// ---- zip entries: where a package file may go ----
enum class Entry {
    Refuse,          // outside the known install roots, or an unsafe path: the whole update stops
    Skip,            // the package's own metadata (modinfo.ini, README.txt at the zip's root): Fluffy Mod Manager's, not the game's
    Dir,             // a folder the package creates (costume_mods/<Character>/): made when missing, its content never touched
    File,            // a package file: replaced
    KeepIfPresent,   // a file the player's settings live in: written only when the game folder has none
};
struct EntryRule {
    Entry kind = Entry::Refuse;
    std::string path;             // normalised: '/' separated, no leading '/', no trailing '/' (folders too)
    std::string why;              // for Refuse
};
// `raw`: the name in the zip. Safe paths only: no drive, no root, no "..", ".", empty or reserved segment (CON, NUL...), no ':' (alternate
// streams), no character Windows refuses, no trailing dot or space, 8 levels and 200 characters at most.
EntryRule classify_entry(const std::string& raw);
// The files of the packages that hold a player's settings (protected: never replaced)
const std::vector<std::string>& keep_if_present();

// ---- the module's own state: reframework/data/SF6_Update_data/state.json ----
struct State {
    bool check = true;                       // "check_for_updates": false turns the whole updater off (README)
    bool check_managed = false;              // "check_in_manager_mode": true lets the game check when the Slotsloader Manager installed the loader
    std::string ignored;                     // "ignored_suite_version": Ignore was answered for that version
    std::vector<std::string> cleanup;        // "pending_cleanup": the .old-<version> files of the last update, deleted at the next start
    std::string raw;                         // the text it was read from: unknown keys are kept when written back
};
State parse_state(const std::string& text);
std::string state_json(const State& s);
// May the game look for a newer release at this start? Not with the off switch. Not either when the Slotsloader Manager installed
// the loader (manager_mode), unless the player asked for it ("check_in_manager_mode": true): a Manager's player updates in the
// Manager, and nothing is fetched from inside the game. `why` (may be null): the line for the log when the answer is no.
bool check_allowed(const State& s, bool manager_mode, std::string* why);

// ---- the old copies kept by a swap: <name>.old-<version> ----
std::string backup_name(const std::string& file_name, const std::string& version, int n);   // n > 0: ".1", ".2"... when taken
bool is_backup_name(const std::string& file_name);

// ---- the GitHub addresses: fixed, nothing in a file changes them ----
constexpr const char* kRepo = "Wael3rd/SF6-ModSuite";
constexpr const char* kManifestUrl = "https://github.com/Wael3rd/SF6-ModSuite/releases/latest/download/manifest.json";
std::string asset_url(const std::string& tag, const std::string& asset);
// The hosts a request may reach, the redirects included (exact names, https only)
bool host_allowed(const std::string& host);

// ---- the downloaded zips kept (update_install.hpp, keep_downloads) ----
// A package's zip name without its version, lower case: "SF6_SlotsLoader_v1.27.2.zip" -> "sf6_slotsloader". The zips of one package
// share it; "" for a name that is not a zip.
std::string asset_family(const std::string& asset);

// ---- text helpers ----
bool is_sha256_hex(const std::string& s);
std::string lower(std::string s);

}  // namespace sf6up
