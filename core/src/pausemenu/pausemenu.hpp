// pausemenu - the training pause-menu engine (increment 9, ADR-0016): the game side.
//
// The decisions are in pm_core.{hpp,cpp} (no game header, compiled as it is into the offline bench). This file is
// what needs the game: the backend of the state machine (TrainingMenuData rows, pages and option arrays, the sub-page
// strip of a primary tab, key-guide data), the hooks (TrainingMenuFunc x9, UIPartsSpin.Init x2 / set_Num,
// UIFlowTrainingMenu.Param.UpdateSpinBox, TrainingManager.GetGuideData, UIPartsGroupScroll.UpdateScroll), the check
// that each hooked method has its own machine code (R18), the spin repairs and the per-frame state (training, pause).
// pm_service.cpp holds the clients and their callbacks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SF6CoreClient;

namespace pausemenu {

using Owner = const void*;   // a client handle

// A hint asked for by a client (button, text), with its owner (the cache key of the hint objects).
struct Hint { Owner owner = nullptr; int32_t button = 0; std::string text; };

// The clients' callbacks, as the service runs them: SEH-guarded, counted on the client's breaker, skipped for a
// client that is disabled or unregistered. Called on the thread of the hook (the game's UI job threads).
struct Calls {
    void (*decide)(Owner owner, int64_t row) = nullptr;
    bool (*spin_value)(Owner owner, int64_t row, int32_t& out) = nullptr;   // false: no value from the client
    void (*on_spin)(Owner owner, int64_t row, int32_t index) = nullptr;
    void (*page_shown)(Owner owner, int64_t page, bool shown) = nullptr;
    void (*hints)(void* focused_menu_data, std::vector<Hint>& out) = nullptr;   // every client with a provider
    bool (*any_hints)() = nullptr;                                               // a provider exists
};

// The check of each hooked method's code (R18), on a thread of its own (the type database is static; its result is
// cached on disk per game executable). Any thread, once; a no-op before the core's initialize.
void check_start();

// Game thread, every frame until Done / Failed: types and backend, then (check ended) the verdicts, then one hook per
// frame. Failed: the engine cannot run (logged). The game thread never waits for the check.
enum class Install { Pending, Done, Failed };
Install install(const Calls& calls, SF6CoreClient* engine_client);
bool ready();                     // installed and not switched off by faults

// Game thread, every core frame: training and pause state, the menu-close edge, the install pass.
void frame(uint64_t frame);

// The option-build purge (options.cpp): may this DynamicData of a primary tab (a DYNAMIC row) be removed? Only a row
// the core built and no longer owns; the game's rows and unknown rows stay (ADR-0016 point 2).
bool dyn_row_removable(void* dynamic_data);

int32_t page_shown(int64_t page);   // live check of the menu's rows
void* row_part(int64_t row);
// The row id (of `owner`) carried by a TrainingMenuData: a row, or the option of a spin (its row). -1 for a game row,
// an unknown object, a page, a removed row or another client's row. Any thread; the registry lock is taken inside.
int64_t row_of(const void* owner, void* menu_data);
std::string status();

}  // namespace pausemenu
