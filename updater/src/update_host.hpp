// SF6 Mod Suite updater: the in-game host. Compiled into the Native Core (sf6core_native.dll) and the Slots Loader's plugin
// (SF6_CostumeSlotsNative.dll); exactly one of them runs it per launch.
//
// Why those two: every package but the Slots Loader needs the Native Core, and the Slots Loader's plugin works without it. So whatever
// the player installed, one of them is there, and no package needs a new one. Which one: each exports sf6suite_updater_rank(); at its
// first frame each copy compares its rank with the other's (the newest updater code first, then the core) and only the highest runs.
// An older DLL without the export never runs it.
//
// Once per launch, off the game thread: a swap left half done is undone, the manifest of the latest release is read from GitHub, the
// installed packages are compared with it. Something newer: at the main menu (a screen held 5 s, no battle loaded, no message box of
// the game's up), the game's own Yes / No box offers it. Yes: download, checks, Smart App Control, swap, all off the game thread; then
// a box offers the restart (the game closed as by its close button, started again through Steam by cmd.exe). No: that release is never
// offered again. Once the game reaches the main menu, the old copies of the last update are deleted.
//
// Off: "check_for_updates": false in reframework/data/SF6_Update_data/state.json (nothing is fetched; a half-done swap is still undone
// and old copies still removed).
#pragma once

#include <cstdint>

namespace sf6up::host {

constexpr uint32_t kUpdaterVersion = 3;          // raise when the updater's behaviour changes: the newest copy runs
constexpr uint32_t kRoleCore = 2, kRolePlugin = 1;

struct HostInfo {
    uint32_t role = 0;                           // kRoleCore or kRolePlugin
    void (*log)(const char* line) = nullptr;     // the host's log (game thread)
};
// From reframework_plugin_initialize, once REFramework's API is initialised
void init(const HostInfo& info);
// From the host's frame callback (LateUpdateBehavior, game thread), while the game is not closing
void frame();

}  // namespace sf6up::host

// The rank of this copy: (kUpdaterVersion << 8) | role, 0 before init
extern "C" __declspec(dllexport) uint32_t sf6suite_updater_rank(void);
