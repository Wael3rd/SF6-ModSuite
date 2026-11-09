// report_mod.dll: the plugin's entry, and what the sources it shares with the Recording Manager expect of their host (see report_mod.hpp).
//
// Until the screen's own update first runs, the DLL is one hook (on that update) and one atomic load per Present. While the screen is up, also what shows the entry and reads the key:
// its core client, its texts, the keys read once a frame. The other hooks and the network go in at the first press (bug_report.cpp). No window message is taken, ever; closing is asked
// of the core.
//
// Silent and inert, with one line in REFramework's log and no hook, when: the Native Core is missing or too old; no address in its config.json (a build without the local config); or
// a Recording Manager that has its own "Report a bug" (2.2.8) is running beside it: that one keeps the key, so R never gives two entries.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

#include <reframework/API.hpp>

#include "common.hpp"
#include "community/client.hpp"
#include "community/community.hpp"
#include "community/http.hpp"
#include "community/protocol.hpp"
#include "community/steam_ticket.hpp"
#include "game/gm.hpp"
#include "model/luajson.hpp"
#include "nlohmann/json.hpp"
#include "perf.hpp"
#include "report/capture.hpp"
#include "report/image_scan.hpp"
#include "report/report_logic.hpp"
#include "report_mod.hpp"
#include "ui/drill_details.hpp"
#include "ui/dump.hpp"
#include "ui/gx.hpp"
#include "ui/internal.hpp"
#include "ui/lang.hpp"

namespace {

namespace gm = game::gm;
using API = reframework::API;
using MO = API::ManagedObject;

constexpr const char* kVersion = "1.2.0";
constexpr const char* kDataDir = "reframework/data/SF6_ReportMod_data";
constexpr const char* kConfigPath = "reframework/data/SF6_ReportMod_data/config.json";   // api_base alone: no key is needed, none ships
constexpr const char* kLocalePath = "reframework/data/SF6_ReportMod_data/locale.json";
constexpr const wchar_t* kCachePath = L"reframework/data/SF6_ReportMod_data/codecheck.txt";
constexpr const char* kJournalPath = "reframework/data/SF6_ReportMod_data/debug_log.json";
constexpr const char* kTokenPath = "reframework/data/SF6_Community_data/identity.json";  // one token for the Recording Manager and this DLL (community/auth.hpp: TokenLock)
constexpr size_t kJournalLines = 400;
constexpr int kMaxFaults = 5;

const SF6CoreAPI* g_core = nullptr;
SF6CoreClient* g_client = nullptr;
std::atomic<bool> g_awake{false}, g_tripped{false}, g_dialogs{false}, g_paused{false};
std::atomic<uint64_t> g_frame{0};
std::atomic<int> g_faults{0};
std::vector<int> g_asked;                        // the keys read each awake frame (the game thread's, as all of this)
std::array<uint32_t, 256> g_presses{};
std::array<bool, 256> g_down{};
std::atomic<void (*)()> g_present_fn{nullptr};
std::recursive_mutex g_lock;

bool g_on = false;
community::Client g_com;
community::steam::Engine* g_steam = nullptr;     // never deleted: Steam may be gone when the process exits
bool g_texts_loaded = false, g_net_started = false;

// ---- the journal: a few lines, written off the game thread ----
std::mutex g_jmx;
std::deque<std::string> g_lines;
bool g_jdirty = false;
std::atomic<bool> g_jwriting{false};

void add_line(std::string line) {
    char pre[32];
    std::snprintf(pre, sizeof pre, "[f%llu] ", (unsigned long long)g_frame.load(std::memory_order_relaxed));
    std::lock_guard lk(g_jmx);
    g_lines.push_back(pre + std::move(line));
    while (g_lines.size() > kJournalLines) g_lines.pop_front();
    g_jdirty = true;
}

void log_line(bool warn, const char* text) {
    if (const auto& a = API::get()) { if (warn) a->log_warn("%s", text); else a->log_info("%s", text); }
    add_line(text);
}

void on_present() {
    if (void (*fn)() = g_present_fn.load(std::memory_order_relaxed)) fn();
}

bool game_in_front() {
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    return front && GetWindowThreadProcessId(front, &pid) && pid == GetCurrentProcessId();
}

void read_keys() {
    for (const int vk : g_asked) {
        const SHORT st = GetAsyncKeyState(vk);
        const bool down = (st & 0x8000) != 0;
        if (report::key_press(g_down[(size_t)vk], down, (st & 1) != 0) && game_in_front()) ++g_presses[(size_t)vk];
        g_down[(size_t)vk] = down;
    }
}

// A running Recording Manager that has its own "Report a bug" (2.2.8): the literal only that one holds, the client it gave the core; gone from its sources in 2.2.9. Read once, at the
// start; every plugin DLL is loaded before REFramework initialises the first one, so the order of the two does not matter.
bool crsm_has_its_own_report() {
    HMODULE h = GetModuleHandleW(L"SF6_CRSMNative.dll");
    static constexpr char kOwn[] = "SF6_CRSMNative_BugReport";
    return h && report::image_holds(h, kOwn, sizeof(kOwn) - 1);
}

void start_module() {
    std::vector<mech::hooks::Spec> specs;
    ui::bug_report::specs(specs);                    // first: the screen's own update, the one hook in place before the first wake
    ui::dlg::specs(specs);                           // the text dialog and its tab blocks: event hooks only
    mech::hooks::install_later(specs);               // the code check of all, now, in one pass; the others hook at the first wake
    ui::bug_report::start();
}

}  // namespace

// ---- what the shared sources expect ----------------------------------------------------------------------------------------------------------------
namespace perf {
std::atomic<bool> g_on{false};
std::atomic<uint32_t> g_gm[GmCount];
std::atomic<uint32_t> g_core[CoreCount];
int reg(const char*, bool) { return -1; }
bool on() { return false; }
int64_t begin() { return 0; }
void end(int, int64_t) {}
void frame() {}
}  // namespace perf

namespace cmn {
const char* feature_name(Feature) { return "report"; }
bool owns(Feature) { return g_core && !g_tripped.load(std::memory_order_relaxed); }
bool active() { return g_core != nullptr; }
// The core's own closing guard (it takes the window messages; its frame callbacks stop then too), asked only while awake: asleep nothing here touches the game. set_element_hidden
// answers ERR_DISABLED while the core is closing, and ERR_INVALID_ARG for these empty names otherwise, touching nothing (a rule of report_test.cpp reads that order in the core).
bool closing() {
    return g_awake.load(std::memory_order_relaxed) && g_core && g_client && g_core->set_element_hidden(g_client, "", "", 0) == SF6CORE_ERR_DISABLED;
}
uint64_t frame() { return g_frame.load(std::memory_order_relaxed); }
const SF6CoreAPI* core() { return g_core; }
SF6CoreClient* client() { return g_client; }
const char* version() { return kVersion; }
const char* locale_file() { return kLocalePath; }
const wchar_t* code_cache_file() { return kCachePath; }
uint32_t key_presses(int vk) {
    if (vk <= 0 || vk >= 256) return 0;
    if (std::find(g_asked.begin(), g_asked.end(), vk) == g_asked.end()) {        // first asked: what it did before does not count
        g_asked.push_back(vk);
        g_down[(size_t)vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    return g_presses[(size_t)vk];
}
void set_present_fn(void (*fn)()) { g_present_fn.store(fn, std::memory_order_relaxed); }

static void format_line(bool log, bool warn, const char* fmt, va_list ap) {
    char b[1024];
    std::vsnprintf(b, sizeof b, fmt, ap);
    if (log) log_line(warn, b); else add_line(b);
}
void log_info(const char* fmt, ...) { va_list ap; va_start(ap, fmt); format_line(true, false, fmt, ap); va_end(ap); }
void log_warn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); format_line(true, true, fmt, ap); va_end(ap); }
void journal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); format_line(false, false, fmt, ap); va_end(ap); }
void fault(Feature, const char* where) {
    add_line(std::string("fault: ") + where);
    if (g_faults.fetch_add(1) + 1 >= kMaxFaults && !g_tripped.exchange(true)) log_line(true, "report_mod: too many faults: off for this session");
}
}  // namespace cmn

namespace ui {
bool paused() { return g_paused.load(std::memory_order_relaxed); }
uint64_t tick() { return cmn::frame(); }
bool active() { return cmn::owns(cmn::Feature::Ui); }
bool dialogs_on() { return g_dialogs.load(std::memory_order_relaxed) && cmn::owns(cmn::Feature::Ui); }
void report_needs_dialogs(bool on) { g_dialogs.store(on, std::memory_order_relaxed); }
std::recursive_mutex& lock() { return g_lock; }
std::string T(const std::string& key, const std::vector<std::string>& args) { return ui::lang::T(key, args); }
void spec(std::vector<mech::hooks::Spec>& out, const char* type, const char* sig, REFPreHookFn pre, REFPostHookFn post, const char* without, bool* ok) {
    mech::hooks::Spec s{ cmn::Feature::Ui, type, sig, pre, post, without, false };
    s.installed = ok;
    out.push_back(s);
}
void fault(const char* where) { cmn::fault(cmn::Feature::Ui, where); }

namespace details { bool is_open() { return false; } }                           // no drill details here
namespace dump { bool tab_switch() { return false; } }
namespace ticker { void push(const std::string&) {} }
namespace com_api { void open() {} void share() {} }                             // no Community dialog here
namespace com {
bool open() { return false; }
std::optional<bool> guide_mode_pad() {                                           // the game's guide mode, as the Community dialog reads it
    static API::Method* m_get = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (gx::TD* td = gm::type("app.InputGuideManager")) m_get = gm::method(td, "GetMode(System.Int32)");
    }
    if (!m_get || !gm::singleton("app.InputGuideManager")) return std::nullopt;
    reframework::InvokeRet r{};
    if (!gm::call(nullptr, m_get, { gm::arg_i32(0) }, &r)) return std::nullopt;
    if (r.dword == 1) return true;
    if (r.dword == 2) return false;
    return std::nullopt;
}
}  // namespace com
namespace windows {
namespace { std::vector<MO*> g_pads; bool g_pads_held = false; }
void drop_pads() { for (MO* p : g_pads) p->release(); g_pads.clear(); g_pads_held = false; }
// X on a connected pad, read every frame while the screen is up (a press must be seen on its frame), as the Recording Manager's pad_held: the devices are listed 1 frame in 30 and held
// meanwhile, so a frame between two lists is one call per connected pad, none without a pad. A device that is not a managed object is not held: the list is then read every frame.
PadHeld pad_held(bool) {
    static void* pad = nullptr;
    static API::Method* devices_m = nullptr;
    static uint32_t x_bit = 64;                                                  // via.hid.GamePadButton.RLeft
    static bool looked = false;
    static uint64_t next_list = 0;
    if (!looked) {
        looked = true;
        if (gx::TD* pb = gm::type("via.hid.GamePadButton"))
            if (API::Field* f = pb->find_field("RLeft")) { const int32_t v = f->get_data<int32_t>(nullptr); if (v) x_bit = (uint32_t)v; }
        if (gx::TD* gt = gm::type("via.hid.GamePad")) devices_m = gt->find_method("get_ConnectingDevices");
    }
    auto buttons_of = [](MO* p) -> uint32_t {
        reframework::InvokeRet rb{};
        return (p && gm::call(p, "get_Button", {}, &rb)) ? rb.dword : 0;
    };
    PadHeld h;
    uint32_t buttons = 0;
    if (g_pads_held && cmn::frame() < next_list) {
        for (MO* p : g_pads) buttons |= buttons_of(p);
    } else {
        next_list = cmn::frame() + 30;
        drop_pads();
        if (!pad) pad = API::get()->get_native_singleton("via.hid.GamePad");
        reframework::InvokeRet r{};
        if (!pad || !devices_m || !gm::call(reinterpret_cast<MO*>(pad), devices_m, {}, &r) || !r.ptr) return h;
        MO* devices = static_cast<MO*>(r.ptr);
        bool can_hold = true;
        const int32_t n = gm::list_count(devices);
        for (int32_t i = 0; i < n; ++i) {
            MO* p = gm::list_item(devices, i);
            if (!p) continue;
            buttons |= buttons_of(p);
            if (can_hold && gm::is_managed(p)) { p->add_ref(); g_pads.push_back(p); }
            else can_hold = false;
        }
        if (can_hold) g_pads_held = true; else drop_pads();
    }
    h.x = (buttons & x_bit) != 0;
    return h;
}
}  // namespace windows
}  // namespace ui

namespace community {
bool on() { return g_on; }
Client& client() { return g_com; }
}  // namespace community

namespace report_host {
void flush_journal() {
    std::string text;
    {
        std::lock_guard lk(g_jmx);
        if (!g_jdirty || g_jwriting.exchange(true)) return;
        g_jdirty = false;
        nlohmann::json arr = nlohmann::json::array();
        for (const std::string& l : g_lines) arr.push_back(l);
        text = arr.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    try {
        std::thread([text = std::move(text)] {
            std::error_code ec;
            std::filesystem::create_directories(kDataDir, ec);
            if (std::ofstream f(kJournalPath, std::ios::binary | std::ios::trunc); f) f << text;
            g_jwriting.store(false);
        }).detach();
    } catch (...) { g_jwriting.store(false); }
}

void note_paused(bool paused) { g_paused.store(paused, std::memory_order_relaxed); }

void frame(uint64_t core_frame) {
    g_awake.store(true, std::memory_order_relaxed);
    g_frame.store(core_frame, std::memory_order_relaxed);
    if (!g_texts_loaded) { g_texts_loaded = true; ui::lang::load(); }            // the entry's text: no table before the first opening of the screen
    read_keys();
    if (g_net_started) {
        if (g_steam) g_steam->frame();                                           // the Steam calls of a ticket, on the game thread (nothing to do: two atomic loads)
        g_com.poll();
    }
    if (core_frame % 60 == 0) flush_journal();
}

// The first press: what the upload needs and a visitor who never presses should not pay for
void start_network() {
    if (g_net_started) return;
    g_net_started = true;
    community::set_log([](const std::string& s) { cmn::journal("%s", s.c_str()); });
    community::set_client_version(std::string("report_mod ") + kVersion);
    g_steam = new community::steam::Engine(community::steam::resolve, [](const std::string& s) { cmn::journal("%s", s.c_str()); }, [] { return (uint64_t)GetTickCount64(); });
    g_com.worker().set_tickets(g_steam);
    g_com.configure(kConfigPath, "", kTokenPath);
    g_com.start();
}

void asleep() {
    g_awake.store(false, std::memory_order_relaxed);
    ui::windows::drop_pads();
}

// The DLL's one core client, made at the first wake: before that the core does not know this DLL (a client is a step of its frame loop). Refused: off for the session.
SF6CoreClient* core_client() {
    if (g_client || !g_core || g_tripped.load(std::memory_order_relaxed)) return g_client;
    SF6CoreClientDesc d{};
    d.struct_size = sizeof d;
    d.client_id = "report_mod";
    d.client_version = kVersion;
    d.priority = 100;
    if (g_core->register_client(&d, &g_client) != SF6CORE_OK) {
        g_client = nullptr;
        g_tripped.store(true);
        log_line(true, "report_mod: off (the core refused it)");
    }
    return g_client;
}
}  // namespace report_host

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1; v->minor = 10; v->patch = 0; v->game_name = nullptr;   // as the core and the other native mods
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions) return false;
    try { API::initialize(param); } catch (...) { return false; }
    try {
        auto off = [](const char* why) {             // one line in REFramework's log and in its own, then nothing: no hook, no key, no thread
            log_line(false, why);
            report_host::flush_journal();
        };
        HMODULE h = GetModuleHandleW(L"sf6core_native.dll");
        auto fn = h ? reinterpret_cast<SF6CoreGetApiFn>(GetProcAddress(h, "sf6core_get_api")) : nullptr;
        const SF6CoreAPI* a = fn ? fn(SF6CORE_ABI_VERSION) : nullptr;
        if (!a || !SF6CORE_HAS(a, ui_clone_control)) {   // (the last of the core's functions it calls: 0.8.2)
            off("report_mod: off (it needs the SF6 Native Core)");
            return true;
        }
        if (crsm_has_its_own_report()) {
            off("report_mod: off (the Recording Manager running beside it has its own Report a bug: it keeps the key until it is updated to 2.2.9 or newer)");
            return true;
        }
        const community::Config cfg = community::config_from_json(model::luajson::load_file(kConfigPath));
        if (cfg.api_base.empty()) {
            off("report_mod: off (no address in its config.json)");
            return true;
        }
        g_core = a;
        g_on = true;
        if (param->functions->on_present) param->functions->on_present(on_present);
        start_module();
        log_line(false, "report_mod: on");
    } catch (...) {
        g_core = nullptr;
        g_on = false;
        API::get()->log_warn("report_mod: off (a fault at the start)");
    }
    return true;
}
