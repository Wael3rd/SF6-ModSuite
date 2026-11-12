// SF6 Mod Suite updater: the texts of its message boxes, in the game's language (updater/locale/locale.json, compiled in as
// update_words.inc by tools/localisation/gen_updater_words.py). Pure: the host gives the language code.
#pragma once

#include <initializer_list>
#include <string>

#include "update_install.hpp"
#include "update_logic.hpp"

namespace sf6up::text {

#include "update_words.inc"

// The language's text, English when the language or the word is missing
const char* tr(const std::string& lang, Word w);
// %s slots filled in order (%% = a percent sign); a slot without its argument stays empty
std::string format(const char* tmpl, std::initializer_list<std::string> args);

std::string title(const std::string& lang);
std::string offer(const std::string& lang, const Plan& plan);            // the release, one line per package (old -> new)
std::string ignore(const std::string& lang);                             // the offer's third button: never ask again for this release
std::string done(const std::string& lang);                               // installed: restart now?
std::string show_zip(const std::string& lang);                           // the done box's third button: the kept zip in Explorer
// The game's ticker strip while an update installs: the release and the download's percentage, then the release alone
std::string downloading(const std::string& lang, const std::string& suite, int percent);
std::string installing(const std::string& lang, const std::string& suite);
std::string failed(const std::string& lang, Fail why, const std::string& what);   // an OK box; what: the file Smart App Control blocks

}  // namespace sf6up::text
