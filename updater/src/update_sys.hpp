// SF6 Mod Suite updater: Windows' side. The Smart App Control check of a staged program, the game folder, and the restart (the game
// closed as by its window's close button, started again through Steam by cmd.exe once this process is gone).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf6up {

// The folder of the running exe (the game's)
std::wstring exe_folder();

// ---- Smart App Control: the file mapped as a program without running any of it (LoadLibraryExW, DONT_RESOLVE_DLL_REFERENCES: Windows'
// code integrity judges it as it would at a real load; DllMain is not called, nothing is resolved). The file is checked under a name of
// its own (renamed, then back: the same file, so the same verdict), so that a DLL of the same name already loaded is never returned. ----
enum class Sac { Ok, Blocked, Bad };
struct SacResult {
    Sac verdict = Sac::Bad;
    unsigned long error = 0;      // GetLastError of the failed load (4551 = ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION)
};
SacResult sac_check(const std::wstring& path);
bool is_program(const std::string& rel);    // .dll / .exe

// ---- the restart ----
// cmd.exe's command line that waits (2 minutes at most) for process `pid` named `image` to end, then, 8 s later, runs `go` (a cmd
// command). The game: its own pid and exe name, go = start "" "steam://rungameid/1364780" (start_waiter).
std::wstring waiter_command(const std::wstring& system_dir, unsigned long pid, const std::wstring& image, const std::wstring& go);
// Started detached (no window, out of the game's job when Windows allows it): a Windows-signed cmd.exe, no program of ours
bool start_waiter_with(unsigned long pid, const std::wstring& image, const std::wstring& go, std::string* why);
bool start_waiter(unsigned long pid, const std::wstring& image, std::string* why);   // go: Steam starts the game (kSteamRun)
// The game's main window asked to close (WM_CLOSE), as its close button does
bool close_game_window();
constexpr const wchar_t* kSteamRun = L"steam://rungameid/1364780";

}  // namespace sf6up
