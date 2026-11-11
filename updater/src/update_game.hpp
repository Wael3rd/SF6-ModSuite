// SF6 Mod Suite updater: what it asks of the game, through REFramework's API (initialised by its host before host::init). Game thread
// only, unless said otherwise; every call guarded (a fault is caught and answered as "no").
//
//   the message box   app.UIFlowDialog.MessageBox.Start(text, title, default, cancel, style, no icon -1, FightingGround 1), polled with
//                     get_IsEnd, the answer GetSelectValue(box) = the button's place (0 Yes): the game's own box, as the Recording
//                     Manager's confirmations (crsm/src/ui/dialogs.cpp) and the Slots Loader's Options pages (plugin/src/options_page.cpp).
//                     Styles (app.DialogButtonStyle): OK 1, YesNo 4, YesNoCancel 5; the three-button box's third button gets our word in
//                     place of the game's Cancel (its text in the box's SimpleList_3: message id emptied, set_Message)
//   the screen        bFlowManager.m_flow_work._FlowMap: its _ID (the core's flow id) and its m_desc (a battle's description: a battle,
//                     a replay or training is loaded or played); app.UIFlowManager._Handles: a message box of the game's is up
//   the language      app.Option's DispLanguage (611) in its LanguageList, else via.SystemService.get_Language (as the plugin's words)
#pragma once

#include <cstdint>
#include <string>

#include "update_logic.hpp"

namespace sf6up::game {

enum class Buttons { Ok, YesNo, YesNoThird };
// third: the word of the third button (YesNoThird), after the game's Yes and No
bool box_open(const std::string& text, const std::string& title, Buttons buttons = Buttons::YesNo, const std::string& third = {});
// -1 still up, 1 Yes (OK too), 0 No (Back too: the box's cancel is its No), 2 the third button, -2 lost (unreadable: no answer)
int box_poll();
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
