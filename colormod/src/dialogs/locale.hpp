// dialogs::locale - port of func/NativeLocale.lua (phase 4a, docs/CPP_PORT_PHASE4.md section 2 "4a").
//
// 14-language text service for the native menus (colour editor, DI colour menu, Auto Rematch tab). Loads
// reframework/data/SF6_NativeLocale_data/locale.json (legacy fallback: SF6_ColorSpinExtra_data/locale.json),
// follows the game's TEXT language (app.Option.GetOptionValue(DispLanguage), re-probed every 60 frames; via.Language
// -> code through CODES; a language without its own block borrows a close one (NEAR) before English).
//
// No hooks, no shadow mode (docs/CPP_PORT_PHASE4.md section 3): the DLL either serves L()/lang() or the Lua does.
// Owner "dialogs_locale" in settings.json. Rollback: "dialogs_locale": "lua", restart.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dialogs::locale {

// Loads locale.json (primary then legacy path), resolves the probe's type handles lazily. Always returns true
// (a missing/unreadable file leaves the tables empty with an "en" table present, same forgiving behaviour as the
// Lua's load_tables): the feature is still "ready" so the Lua can stand down and the DLL answers with key-as-text.
bool install();
bool ready();                // installed (tables loaded); the probe itself never disables the feature

// Frame callback: every 60 frames, re-reads the game's text language (docs/CPP_PORT_PHASE4.md timing constants).
// SEH-guarded; a crash in the probe does not disable the feature (lang just stops updating), matching the Lua's
// own pcall(probe_lang) which swallows the error the same way.
void on_frame();

// L(key, args): args already stringified by the bridge layer (common.cpp), one per %s / %d in order, same
// semantics as the Lua's L(key, ...): missing key everywhere -> the key itself; no args -> the raw template
// (even if it still has %s/%d in it); a malformed substitution (wrong count/type) -> the raw template, matching
// the Lua's pcall(string.format, ...) fallback.
std::string get(const std::string& key, const std::vector<std::string>& args);
std::string status();        // diagnostics: lang, table size, last probe frame, errors

// offline test only (tools/locale_harness/locale_cpp_dump.cpp): load an explicit file (bypasses the game's data
// dir) and set the language directly (bypasses the game probe). Mirrors the Lua's M.reload() / M.set(code).
void debug_load(const std::string& path);
void debug_set_lang(const std::string& code);

}  // namespace dialogs::locale
