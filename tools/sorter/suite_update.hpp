// SF6_ModSorter -- the SF6 Mod Suite's own updates, looked up and installed from the sorter (the game closed). The updater's code
// is the game's (updater/src): the same manifest of GitHub's latest release, the same checks (size, sha256, zip entries, Smart App
// Control), the same all-or-nothing swap, the same state.json and update.log in reframework\data\SF6_Update_data. What differs:
// the installed versions are read from the DLLs' files (update_pe.hpp: nothing is loaded), and nothing is installed while the
// game runs (the game's own offer, at its main menu, does that).
#pragma once

#include <memory>
#include <string>
#include <vector>

struct SuiteLine { std::string name, from, to; };   // "SF6 Color Mod", "0.26.1", "0.26.2"
struct SuitePlan;                                    // what an install needs (the updater's plan), kept by the check

struct SuiteCheck {
    enum State {
        NoGame,          // no game folder
        Off,             // check_for_updates is false in state.json and nobody asked: nothing fetched
        NotInstalled,    // none of the suite's packages is in the game folder: nothing fetched
        Failed,          // the latest release could not be read (no connection, a manifest refused): why
        Current,         // nothing newer than what is installed
        Available        // lines: what an install would change
    } state = NoGame;
    std::string latest;              // the latest release (Current, Available)
    std::vector<SuiteLine> lines;
    int installed = 0;               // packages in the game folder
    int unversioned = 0;             // of them, those whose DLL reports no version (older than 1.32.0): never offered
    bool ignored = false;            // Available, and Ignore was answered in game for that release
    std::string why;                 // Failed: the reason, in English (the log's)
    std::shared_ptr<SuitePlan> plan;
};

struct SuiteInstall {
    bool ok = false;
    bool nothing = false;            // ok, and nothing was left to install (the game did it in between)
    std::string version;             // ok: the release now installed
    std::string message;             // not ok: what to tell the player, in `lang` (the game's own sentences)
    std::string why;                 // not ok: the log's reason, in English
};

// asked: the player pressed the key (looked up even when state.json turns the automatic check off).
// manifest_file (tests): a manifest read from that file instead of GitHub's latest release; what it offers is never installed.
SuiteCheck suite_check(const std::wstring& game_dir, bool asked, const std::wstring& manifest_file = L"");
// Installs what `c` offers (looked at again first: only what is still older). Refused while the game runs (game_running: the
// caller's look at it; the game's own offer, at its main menu, is the way then).
SuiteInstall suite_install(const std::wstring& game_dir, const SuiteCheck& c, const std::string& lang, bool game_running);
// A line of an offer as the game's box writes it: "SF6 Color Mod 0.26.1 -> 0.26.2"
std::string suite_line_text(const SuiteLine& l, const std::string& lang);
// One JSON line (--check-updates)
std::string suite_check_json(const SuiteCheck& c);
