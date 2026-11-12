// SF6 Mod Suite updater: the steps, without the game. What the in-game host runs off the game thread, and what the end-to-end driver
// (updater/tests) runs on a fake game folder, the same code: the check (installed packages against the manifest) and the install
// (download, sizes and sha256, extraction into a staging tree, Smart App Control, the swap).
//
// Files (reframework/data/SF6_Update_data/ of the game folder):
//   state.json         the module's state and its one setting (update_logic.hpp, State)
//   swap_journal.json  only during a swap (update_fs.hpp)
//   staging/           only during an install: zips/ (the downloads), tree/ (their files)
//   downloads/         the zips of the installs, kept for the players who install with Fluffy Mod Manager (it keeps its own copy
//                      of each zip): the last one of each package; previous_version/ the one before it (two versions at most)
//   update.log         what it did, the last lines
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "update_fs.hpp"
#include "update_logic.hpp"
#include "update_net.hpp"

namespace sf6up {

constexpr const char* kDataDir = "reframework/data/SF6_Update_data";

struct Paths {
    std::wstring root, data, state, journal, staging, log, downloads, previous;
    explicit Paths(const std::wstring& game_root);
};

using LogFn = std::function<void(const std::string&)>;
// One dated line at the end of update.log (any thread; the file starts again once it is over 256 KiB, at a process's first line).
// The game's host and the mod sorter both write there.
void append_log(const Paths& p, const std::string& line);

// The state file, read and written under one lock (the check, the answers and the clean-up may run on different threads)
State load_state(const Paths& p);
bool update_state(const Paths& p, const std::function<void(State&)>& change);

// At start, before anything: a swap that stopped in the middle undone
void recover(const Paths& p, const LogFn& log);
// Once the new version has run: the old copies of the last update deleted
void cleanup(const Paths& p, const LogFn& log);

// ---- the check ----
struct CheckResult {
    bool ok = false;              // the manifest was read
    std::string why;
    Plan plan;                    // plan.items empty: nothing to offer
};
// manifest_text: the manifest's text, as fetched (http in game, a file in the tests)
CheckResult run_check(const Paths& p, const VersionReader& version_of, const std::string& manifest_text, const std::string& ignored);

// ---- the install ----
enum class Fail { None, Download, Check, Package, Blocked, Files };
struct InstallResult {
    bool ok = false;
    Fail fail = Fail::None;
    std::string why;              // for the log
    std::string what;             // for the player: the file Smart App Control blocks
    SwapResult swap;
};
// Where an install is, for whoever shows it (the game's ticker, the sorter's window): written by the thread that installs, read
// from any other. The download is counted in bytes over the plan's zips (their sizes are the manifest's).
struct Progress {
    enum Step : int { Idle, Download, Install };   // Install: the zips are there (checks, Smart App Control, the swap)
    std::atomic<int> step{Idle};
    std::atomic<int64_t> done{0}, total{0};
    int percent() const;                           // of the download, 0 to 100
    void reset() { step.store(Idle); done.store(0); total.store(0); }
};
// Fetch one package's zip to `dest` (in game: https from the release; the tests: a local copy), telling `bytes` what it has
// received so far. Its size and sha256 are checked after.
using Fetch = std::function<bool(const PlanItem&, const std::wstring& dest, const BytesFn& bytes, std::string* why)>;
InstallResult run_install(const Paths& p, const Plan& plan, const Fetch& fetch, const LogFn& log, int fail_at = -1,
                          Progress* progress = nullptr);
// After an install: its zips (in `from`) moved into downloads/; a zip of the same package already there (asset_family) goes to
// downloads/previous_version/, in place of the one there. Never fails the install: what could not be kept is logged.
// Returns the zips kept.
int keep_downloads(const Paths& p, const std::vector<std::string>& assets, const std::wstring& from, const LogFn& log);
// The newest zip of downloads/ ("" when there is none)
std::wstring newest_download(const Paths& p);
// Explorer on downloads/, the newest zip selected (the folder alone when there is none). False when Explorer could not start.
bool show_downloads(const Paths& p);

// The fetch the game uses: the asset of the manifest's release on GitHub
bool fetch_from_github(const std::string& tag, const PlanItem& item, const std::wstring& dest, const BytesFn& bytes, std::string* why);
// The manifest of the latest release
bool fetch_manifest(std::string* text, std::string* why);

}  // namespace sf6up
