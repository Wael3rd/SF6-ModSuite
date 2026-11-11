// SF6 Mod Suite updater: what it asks of the game, through REFramework's API (initialised by its host before host::init). Game thread
// only, unless said otherwise; every call guarded (a fault is caught and answered as "no").
//
//   the message box   app.UIFlowDialog.MessageBox.Start(text, title, 1, 1, YesNo 4, no icon -1, FightingGround 1), polled with get_IsEnd,
//                     the answer GetSelectValue(box) == 0 for Yes: the game's own Yes / No box, as the Recording Manager's confirmations
//                     (crsm/src/ui/dialogs.cpp) and the Slots Loader's Options pages (plugin/src/options_page.cpp)
//   the screen        bFlowManager.m_flow_work._FlowMap: its _ID (the core's flow id) and its m_desc (a battle's description: a battle,
//                     a replay or training is loaded or played); app.UIFlowManager._Handles: a message box of the game's is up
//   the language      app.Option's DispLanguage (611) in its LanguageList, else via.SystemService.get_Language (as the plugin's words)
#pragma once

#include <cstdint>
#include <string>

#include "update_logic.hpp"

namespace sf6up::game {

bool box_open(const std::string& text, const std::string& title);
int box_poll();                   // -1 still up, 1 Yes, 0 No (or the box lost)
void box_drop();
bool box_is_open();

int32_t flow_id();                // -1 unknown
bool in_battle();                 // a battle description is loaded (battle, replay, training)
bool game_dialog_up();            // a message box of the game's own is open (false when it cannot be read)
std::string language();           // "en", "fr", "ja"...

// Any thread: what a package's main DLL, loaded in this process from the game folder, reports. Its export
// sf6suite_package_version(); the Native Core also through sf6core_get_api()->get_version (every core has it). "" = nothing.
std::string package_version(const KnownPackage& k, const std::wstring& root);

}  // namespace sf6up::game
