// dialogs::locale - port of func/NativeLocale.lua (phase 4a). See locale.hpp and docs/CPP_PORT_PHASE4.md section 2 "4a".
//
// No hooks: this module only reads a static field / calls two static methods every 60 frames (the language probe)
// and looks keys up in a table loaded once from locale.json. SEH-guarded like the other frame-tick modules (R16),
// but a probe fault does not switch the feature off (same resilience as the Lua's pcall(probe_lang): the language
// just stops updating until the next successful probe).

#include "locale.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ll_common.hpp"
#include "nlohmann/json.hpp"

#include "../common.hpp"

namespace dialogs::locale {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;

// Dossier neutre (NativeLocale.lua:14-18): partage entre l'editeur, les menus Drive et l'onglet Auto Rematch.
// L'ancien chemin reste lu en second, pour les installations faites avant ce deplacement.
constexpr const char* kFile = "reframework/data/SF6_NativeLocale_data/locale.json";
constexpr const char* kFileLegacy = "reframework/data/SF6_ColorSpinExtra_data/locale.json";
constexpr int64_t kProbeEvery = 60;         // NativeLocale.lua:115
constexpr int32_t kDispLanguage = 611;      // app.Option.ValueType.DispLanguage, NativeLocale.lua:47

// via.Language enum (checked in-game 03/09, NativeLocale.lua:20-24) -> locale.json language code.
const std::unordered_map<int32_t, const char*> kCodes = {
    { 0, "ja" }, { 1, "en" }, { 2, "fr" }, { 3, "it" }, { 4, "de" }, { 5, "es" }, { 6, "ru" }, { 7, "pl" },
    { 8, "nl" }, { 9, "pt" }, { 10, "pt" }, { 11, "ko" }, { 12, "zh-Hant" }, { 13, "zh-Hans" }, { 21, "ar" }, { 32, "es-419" },
};
// a language without its own block borrows a close one before English (NativeLocale.lua:26)
const std::unordered_map<std::string, std::string> kNear = {
    { "es-419", "es" }, { "zh-Hant", "zh-Hans" }, { "pt", "es" },
};

std::mutex g_mx;
nlohmann::json g_tables = nlohmann::json::object();     // { "<code>": { key = text } }, always has "en"
bool g_installed = false;
std::string g_lang = "en";
int64_t g_frame = 0;
int64_t g_last_probe = -1000;               // NativeLocale.lua:30 (first probe runs immediately)
int g_errors = 0;

// loads into g_tables (caller holds g_mx): primary path, legacy fallback, "en" always present.
void load_tables_locked(const char* primary, const char* legacy) {
    std::ifstream f1(primary, std::ios::binary);
    nlohmann::json data = f1 ? nlohmann::json::parse(f1, nullptr, false) : nlohmann::json(nlohmann::json::value_t::discarded);
    if (data.is_discarded() || !data.is_object()) {
        std::ifstream f2(legacy, std::ios::binary);
        data = f2 ? nlohmann::json::parse(f2, nullptr, false) : nlohmann::json(nlohmann::json::value_t::discarded);
    }
    g_tables = (!data.is_discarded() && data.is_object()) ? data : nlohmann::json::object();
    if (!g_tables.contains("en") || !g_tables["en"].is_object()) g_tables["en"] = nlohmann::json::object();
}

// ---- game language probe (NativeLocale.lua:47-84): types resolved once, like the Lua's opt_m/list_f/sys_m -------
struct {
    bool tried = false;
    API::Method* opt_get = nullptr;         // app.Option.GetOptionValue(Int32), static
    API::Field* list_field = nullptr;       // app.Option.LanguageList, static (via.Language[])
    API::Method* sys_get = nullptr;         // via.SystemService.get_Language(), static (fallback)
} R;

std::unordered_map<API::TypeDefinition*, API::Method*> g_arr_len;    // the array type's own get_Length/get_Item
std::unordered_map<API::TypeDefinition*, API::Method*> g_arr_item;   // (type-specific indexer, not System.Array's)

API::Method* arr_len_method(MO* arr) {
    auto* td = arr->get_type_definition();
    auto it = g_arr_len.find(td);
    if (it != g_arr_len.end()) return it->second;
    return g_arr_len[td] = ll::find_method_noargs(td, "get_Length");
}
API::Method* arr_item_method(MO* arr) {
    auto* td = arr->get_type_definition();
    auto it = g_arr_item.find(td);
    if (it != g_arr_item.end()) return it->second;
    return g_arr_item[td] = ll::find_method_full(td, "get_Item(System.Int32)");
}

void resolve_types() {
    if (R.tried) return;
    R.tried = true;
    auto* tdb = API::get()->tdb();
    if (!tdb) return;
    R.opt_get = tdb->find_method("app.Option", "GetOptionValue");
    R.list_field = tdb->find_field("app.Option", "LanguageList");
    R.sys_get = tdb->find_method("via.SystemService", "get_Language");
}

void probe_lang_inner() {
    resolve_types();
    int32_t val = 0;
    bool got = false;
    if (R.opt_get && R.list_field) {
        auto ridx = R.opt_get->invoke(nullptr, std::vector<void*>{ ll::arg_i32(kDispLanguage) });
        if (!ridx.exception_thrown) {
            const int32_t idx = (int32_t)ridx.dword;
            void* slot = R.list_field->get_data_raw(nullptr, false);
            MO* arr = slot ? *reinterpret_cast<MO**>(slot) : nullptr;
            if (arr && idx >= 0) {
                auto* len_m = arr_len_method(arr);
                auto* item_m = arr_item_method(arr);
                if (len_m && item_m) {
                    auto rlen = len_m->invoke(arr, std::vector<void*>{});
                    const int32_t n = rlen.exception_thrown ? 0 : (int32_t)rlen.dword;
                    if (idx < n) {
                        auto ritem = item_m->invoke(arr, std::vector<void*>{ ll::arg_i32(idx) });
                        if (!ritem.exception_thrown) { val = (int32_t)ritem.dword; got = true; }
                    }
                }
            }
        }
    }
    if (!got && R.sys_get) {                 // fallback: the system language (NativeLocale.lua:70-79)
        auto r = R.sys_get->invoke(nullptr, std::vector<void*>{});
        if (!r.exception_thrown) { val = (int32_t)r.dword; got = true; }
    }
    if (got) {
        auto it = kCodes.find(val);
        if (it != kCodes.end()) {
            std::lock_guard lk(g_mx);
            g_lang = it->second;
        }
    }
}

void probe_tick() {
    __try {
        probe_lang_inner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_errors;
        cmn::journal("[locale] fault in probe_lang (%d)", g_errors);
    }
}

// NativeLocale.lua:93-100
bool lookup_locked(const std::string& key, std::string& out) {
    auto try_lang = [&](const std::string& code) -> bool {
        auto itl = g_tables.find(code);
        if (itl == g_tables.end() || !itl->is_object()) return false;
        auto itk = itl->find(key);
        if (itk == itl->end() || !itk->is_string()) return false;
        out = itk->get<std::string>();
        return true;
    };
    if (try_lang(g_lang)) return true;
    auto itn = kNear.find(g_lang);
    if (itn != kNear.end() && try_lang(itn->second)) return true;
    return try_lang("en");
}

// Mirrors Lua's string.format(template, ...) for the %s / %d specifiers locale.json actually uses, including the
// flag/width forms it also uses (checked across every language: "%02d" in root.color_n, di.part_row, di.part_page,
// di.part_edit -- found missing by review, the bare "%[sd]" match below used to stop at '%0' and fail the whole
// substitution, silently falling back to the raw template "Color %02d" while still reporting "ok" for `?N`).
// Supports: an optional "-" (left-align) and/or "0" (zero-pad, %d only) flag, an optional decimal width, then
// "s" or "d". Any mismatch (missing arg, %d on a non-numeric arg, an unsupported specifier) returns false so the
// caller falls back to the raw template, like the Lua's pcall(string.format, ...).
bool apply_format(const std::string& tmpl, const std::vector<std::string>& args, std::string& out) {
    out.clear();
    out.reserve(tmpl.size() + 16);
    size_t ai = 0;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%') { out += tmpl[i]; continue; }
        size_t j = i + 1;
        if (j < tmpl.size() && tmpl[j] == '%') { out += '%'; i = j; continue; }
        bool left = false, zero = false;
        while (j < tmpl.size() && (tmpl[j] == '-' || tmpl[j] == '0')) {
            if (tmpl[j] == '-') left = true; else zero = true;
            ++j;
        }
        size_t width = 0;
        while (j < tmpl.size() && tmpl[j] >= '0' && tmpl[j] <= '9') { width = width * 10 + (size_t)(tmpl[j] - '0'); ++j; }
        if (j >= tmpl.size()) return false;
        const char conv = tmpl[j];
        if ((conv != 's' && conv != 'd') || ai >= args.size()) return false;

        std::string val = args[ai];
        if (conv == 'd') {
            size_t pos = 0;
            try { std::stoll(val, &pos); } catch (...) { return false; }
            if (pos != val.size()) return false;
            if (zero && !left && width > 0) {
                const bool neg = !val.empty() && val[0] == '-';
                std::string digits = neg ? val.substr(1) : val;
                const size_t signWidth = neg ? 1 : 0;
                if (digits.size() + signWidth < width) digits.insert(0, width - digits.size() - signWidth, '0');
                val = (neg ? "-" : "") + digits;
            }
        }
        if (val.size() < width && !(conv == 'd' && zero && !left)) {
            const std::string pad(width - val.size(), ' ');
            val = left ? (val + pad) : (pad + val);
        }
        out += val;
        ++ai;
        i = j;
    }
    return true;
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------------------

bool install() {
    std::lock_guard lk(g_mx);
    load_tables_locked(kFile, kFileLegacy);
    g_installed = true;
    cmn::journal("[locale] loaded, %d language table(s), lang=%s", (int)g_tables.size(), g_lang.c_str());
    return true;
}

bool ready() { return g_installed; }

void on_frame() {
    if (!g_installed) return;
    ++g_frame;
    if (g_frame - g_last_probe < kProbeEvery) return;
    g_last_probe = g_frame;
    probe_tick();
}

std::string get(const std::string& key, const std::vector<std::string>& args) {
    std::string v;
    {
        std::lock_guard lk(g_mx);
        if (!lookup_locked(key, v)) return key;      // missing everywhere: the key itself (NativeLocale.lua:104)
    }
    if (args.empty()) return v;                       // no substitution requested (NativeLocale.lua:105)
    std::string s;
    if (apply_format(v, args, s)) return s;
    return v;                                          // malformed substitution: the raw template (pcall fallback)
}

std::string status() {
    std::lock_guard lk(g_mx);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "locale: on=%d lang=%s tables=%d last_probe=f%lld errors=%d",
                  g_installed ? 1 : 0, g_lang.c_str(), (int)g_tables.size(), (long long)g_last_probe, g_errors);
    return buf;
}

void debug_load(const std::string& path) {
    std::lock_guard lk(g_mx);
    load_tables_locked(path.c_str(), path.c_str());
    g_installed = true;
}

void debug_set_lang(const std::string& code) {
    std::lock_guard lk(g_mx);
    g_lang = code;
}

}  // namespace dialogs::locale
