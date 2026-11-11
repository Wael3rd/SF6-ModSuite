// SF6_ModSorter -- the tool's texts, in the game's 14 languages. They live in tools/sorter/locale.json; texts_table.inc is
// generated from it (tools/localisation/gen_sorter_texts.py). The language is --lang's, else the one chosen on the
// window's Options page (settings.ini), else the Windows display language's; a text a language has not is shown in English. {0} {1} {2}: arguments.
#pragma once

#include <string>
#include <vector>

#include "texts_table.inc"

// A text with its arguments, rendered in the language of the moment
struct Msg {
    TextId id = T__COUNT;
    std::vector<std::string> args;
};

// code: one of kLangCodes ("en", "fr", "ja", "zh-Hans", ...), letters of any case; false (and nothing changed) for another
bool set_language(const std::string& code);
// The language of a Windows language id (GetUserDefaultUILanguage): an index in kLangCodes, English for any other language
int language_from_windows(unsigned langid);
void set_language_index(int index);
const char* language();                                             // the code of the language of the moment
int language_index();                                               // its index in kLangCodes / kLangNames
const wchar_t* language_locale();                                   // its Windows locale name: fonts, capitals
bool language_rtl();                                                // written right to left (Arabic)
std::string tr(TextId id);                                         // UTF-8
std::string tr(TextId id, const std::vector<std::string>& args);
inline std::string tr(const Msg& m) { return m.id == T__COUNT ? std::string() : tr(m.id, m.args); }
std::string tr_en(const Msg& m);                                    // English, for the JSON lines
