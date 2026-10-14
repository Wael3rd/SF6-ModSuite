// ui/lang — the menus' texts in the game's text language, for the native UI (owner "ui"): NativeLocale.lua in the DLL.
// The file reframework/data/SF6_CRSM_data/locale.json (14 languages, keys "rsm.*") is read once (model::locale: lookup,
// fallback chain, string.format); the game's text language (Options > DispLanguage index into app.Option.LanguageList,
// fallback via.SystemService.get_Language) is probed on the core's frame callback (NativeLocale.lua:47-84): at the first
// frame, after each mech event (flow change, training edge, menu close) and every 60 frames while the pause menu or an option
// window is up (Options > Language is changed there; NativeLocale.lua:112-116 probed every 60 frames everywhere).
//
// The one language module of the native UI: P4's ui::T forwards here, P5's Manager page retexts on version(). load() runs
// once from ui::start() (owner "ui" native), frame() from ui.cpp's frame callback once the loader handed the UI over.
//
// Threads: frame() on the game thread only (game calls); T(), lang() and version() from any thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui::lang {

void load();                                  // reads locale.json (once; missing file: the keys themselves)
// game thread: the language probe (the first at once; then when `events` changed, or every 60 frames while `menu_up`)
void frame(uint64_t frame, bool menu_up, uint64_t events);
uint64_t version();                           // +1 each time the language changes (and at load)
std::string lang();                           // "en", "fr", ...
// RecordingManagerTab's T(key, ...): L("rsm." .. key, ...), the bare key when the text is missing everywhere.
std::string T(const std::string& key, const std::vector<std::string>& args = {});
std::string status();

}  // namespace ui::lang
