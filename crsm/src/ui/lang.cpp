#include "ui/lang.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "model/locale.hpp"
#include "model/luajson.hpp"

namespace ui::lang {
namespace {

namespace gm = game::gm;
using API = reframework::API;

constexpr const char* kFile = "reframework/data/SF6_CRSM_data/locale.json";
constexpr uint64_t kProbeEvery = 60;          // NativeLocale.lua:115 (here: while the menu is up)
constexpr int32_t kDispLanguage = 611;        // app.Option.ValueType.DispLanguage (NativeLocale.lua:43)

std::mutex g_mx;                              // g_loc
model::locale::Locale g_loc;
std::atomic<uint64_t> g_version{0};
uint64_t g_last_probe = 0;
uint64_t g_events_seen = 0;
bool g_probed = false;
int g_errors = 0;

// The probe's types, resolved once (the Lua's opt_m / list_f / sys_m).
struct {
    bool tried = false;
    API::Method* opt_get = nullptr;           // app.Option.GetOptionValue(Int32), static
    API::Field* list_field = nullptr;         // app.Option.LanguageList, static via.Language[]
    API::Method* sys_get = nullptr;           // via.SystemService.get_Language(), static
} R;

// The game's text language as a via.Language value; false when nothing could be read.
bool probe_value(int32_t* out) {
    if (!R.tried) {
        R.tried = true;
        if (auto* tdb = API::get()->tdb()) {
            R.opt_get = tdb->find_method("app.Option", "GetOptionValue");
            R.list_field = tdb->find_field("app.Option", "LanguageList");
            R.sys_get = tdb->find_method("via.SystemService", "get_Language");
        }
    }
    if (R.opt_get && R.list_field) {
        const auto ridx = R.opt_get->invoke(nullptr, std::vector<void*>{ gm::arg_i32(kDispLanguage) });
        if (!ridx.exception_thrown) {
            const int32_t idx = (int32_t)ridx.dword;
            void* slot = R.list_field->get_data_raw(nullptr, false);
            gm::MO* arr = slot ? *reinterpret_cast<gm::MO**>(slot) : nullptr;
            if (arr && idx >= 0 && idx < gm::array_len(arr)) {
                reframework::InvokeRet r{};
                // the array type's own indexer (NativeLocale.lua: arr:call("get_Item", i); the Color Mod's probe too)
                if (gm::call(arr, "get_Item(System.Int32)", { gm::arg_i32(idx) }, &r) ||
                    gm::call(arr, "get_Item", { gm::arg_i32(idx) }, &r) ||
                    gm::call(arr, "Get(System.Int32)", { gm::arg_i32(idx) }, &r)) {
                    *out = (int32_t)r.dword;
                    return true;
                }
                // the boxed element (System.Array.GetValue): its value__ (P4's fallback)
                int64_t x = 0;
                if (gm::MO* boxed = gm::array_get(arr, idx); boxed && gm::get_int(boxed, "value__", &x)) {
                    *out = (int32_t)x;
                    return true;
                }
            }
        }
    }
    if (R.sys_get) {                                              // the system language: better than none
        const auto r = R.sys_get->invoke(nullptr, std::vector<void*>{});
        if (!r.exception_thrown) { *out = (int32_t)r.dword; return true; }
    }
    return false;
}

void probe() {
    int32_t v = -1;
    if (!probe_value(&v)) return;
    const char* code = model::locale::code_for_language(v);
    if (!code) return;
    std::lock_guard lk(g_mx);
    if (g_loc.lang() != code) {
        g_loc.set_lang(code);
        g_version.fetch_add(1);
    }
}

bool probe_seh() {
    __try { probe(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

}  // namespace

void load() {
    const model::locale::json file = model::luajson::load_file(kFile);
    std::lock_guard lk(g_mx);
    g_loc.load(file);
    g_version.fetch_add(1);
    if (file.is_null()) cmn::journal("ui: %s missing or unreadable: the texts are the keys", kFile);
}

void frame(uint64_t fr, bool menu_up, uint64_t events) {
    if (g_probed && events == g_events_seen && !(menu_up && fr - g_last_probe >= kProbeEvery)) return;
    g_probed = true;
    g_events_seen = events;
    g_last_probe = fr;
    if (!probe_seh() && ++g_errors <= 3) cmn::journal("ui: fault in the language probe (%d)", g_errors);
}

uint64_t version() { return g_version.load(); }

std::string lang() {
    std::lock_guard lk(g_mx);
    return g_loc.lang();
}

std::string T(const std::string& key, const std::vector<std::string>& args) {
    std::lock_guard lk(g_mx);
    return g_loc.T(key, args);
}

std::string status() {
    char b[96];
    std::snprintf(b, sizeof(b), "lang %s (v%llu, %d probe faults)", lang().c_str(), (unsigned long long)version(), g_errors);
    return b;
}

}  // namespace ui::lang
