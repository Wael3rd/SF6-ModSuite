// model::locale — the menus' texts in the game's language (NativeLocale.lua: lookup, fallback chain, string.format of
// the template) from reframework/data/SF6_CRSM_data/locale.json ({ "<code>": { "<key>": "text" } }, 14 languages).
// The probe of the game's language (app.Option / via.SystemService) is a game call: it stays out of the model, only the
// mapping from a via.Language value to a code is here.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace model::locale {

using json = nlohmann::json;

// CODES: via.Language value -> code ("en", "fr", "ja", "zh-Hans", ...); nullptr for a value without one.
const char* code_for_language(int via_language);
// NEAR: a code without its own block borrows a close one ("es-419" -> "es", "zh-Hant" -> "zh-Hans", "pt" -> "es").
const char* near_of(const std::string& code);

class Locale {
public:
    // load_tables: the decoded file (luajson::load_file). Not a table: no text but the keys. "en" always exists.
    void load(const json& file);
    void set_lang(const std::string& code) { lang_ = code; }
    const std::string& lang() const { return lang_; }

    // lookup: tables[lang][key], else tables[NEAR[lang]][key], else tables.en[key]; nullopt when missing everywhere.
    std::optional<std::string> lookup(const std::string& key) const;
    // L(key, ...): the text, formatted with string.format when arguments are given (a template that cannot be formatted
    // with them is returned as it is); the key itself when the text is missing. Supported specifiers: %s %d %%.
    std::string L(const std::string& key, const std::vector<std::string>& args = {}) const;
    // T(key, ...) of RecordingManagerTab: L("rsm." .. key, ...), and the bare key when the text is missing.
    std::string T(const std::string& key, const std::vector<std::string>& args = {}) const;

private:
    json tables_ = json::object();
    std::string lang_ = "en";
};

// string.format(template, args...) for %s and %d (and %%). False where Lua raises (not enough arguments, %d with a
// non-integer, any other specifier).
bool format(const std::string& tmpl, const std::vector<std::string>& args, std::string* out);

}  // namespace model::locale
