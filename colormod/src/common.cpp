#include "common.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <reframework/API.hpp>

#include "nlohmann/json.hpp"
#include "fighters.hpp"
#include "perf.hpp"
#include "write_journal.hpp"
#include "engine/color_mods.hpp"

namespace cmn {
namespace {

constexpr const char* kVersion = "0.23.0";
constexpr const char* kClientId = "SF6_ColorModNative";
// The game's working directory is the game folder (same convention as SF6_MenuRowsNative / SF6_MemWriterNative).
constexpr const char* kDataDir = "reframework/data/SF6_ColorModNative_data";
constexpr const char* kOffFile = "reframework/data/_colormod_off.txt";
constexpr size_t kJournalLines = 400;      // same depth as the Lua journal (SF6_ColorSpinExtra.lua dlog)
constexpr uint64_t kJournalFlushEvery = 30;   // frames, same cadence as the Lua journal

// What this build actually implements. A feature asked "native" in settings.json but not implemented yet stays
// with Lua (journaled), so a settings file ahead of the DLL never leaves a feature without an owner.
constexpr std::array<bool, (size_t)Feature::Count> kImplemented = { true, true, false, false, true, true, true, true, true, true, true, true, true, true, true, true, true };
constexpr std::array<const char*, (size_t)Feature::Count> kNames = { "engine", "vfx", "screens", "dialogs", "editor", "decisions", "vfx_engine", "vfx_body",
                                                                     "screens_rows", "screens_dipanel", "screens_tracker", "screens_guide", "dialogs_locale", "dialogs_camera",
                                                                     "dialogs_options", "dialogs_editor", "screens_colour" };

const SF6CoreAPI* g_api = nullptr;
SF6CoreClient* g_client = nullptr;
bool g_active = false;
std::array<bool, (size_t)Feature::Count> g_owns{};

std::mutex g_jmx;                            // journal: written from the frame callback and from game hooks later
std::deque<std::string> g_lines;
bool g_dirty = false;
uint64_t g_frame = 0;

// Closing guard: the game window got WM_CLOSE / WM_DESTROY / WM_NCDESTROY / WM_ENDSESSION.
// Once set, every per-frame entry point and every pre-hook returns immediately.
std::atomic<bool> g_closing{false};
std::atomic<bool> g_closing_final{false};    // true = WM_DESTROY / WM_NCDESTROY / WM_ENDSESSION (not cancellable)
std::atomic<int64_t> g_closing_tick{0};      // GetTickCount64 when g_closing was set

std::string vformat(const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

void journal_line(const std::string& s) {
    std::lock_guard lk(g_jmx);
    if (g_lines.size() >= kJournalLines) g_lines.pop_front();
    g_lines.push_back("[f" + std::to_string(g_frame) + "] " + s);
    g_dirty = true;
}

void journal_flush() {
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard lk(g_jmx);
        if (!g_dirty) return;
        g_dirty = false;
        for (auto& l : g_lines) arr.push_back(l);
    }
    bgwriter::write((std::string(kDataDir) + "/debug_log.json").c_str(), arr.dump(4));
}

std::string now_text() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

// Phase 5 (docs/CPP_PORT_PHASE5.md): the Lua copies of the native features are deleted, so this DLL owns every
// feature it implements whatever settings.json says; an owner set to "lua" is journaled and ignored. Rollback =
// redeploying the previous build (scripts + DLL), not an owner.
void load_settings() {
    g_owns.fill(false);
    for (size_t i = 0; i < kNames.size(); ++i) g_owns[i] = kImplemented[i];
    std::ifstream f(std::string(kDataDir) + "/settings.json", std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("owners") || !j["owners"].is_object()) return;
    for (size_t i = 0; i < kNames.size(); ++i) {
        auto it = j["owners"].find(kNames[i]);
        if (it != j["owners"].end() && it->is_string() && it->get<std::string>() == "lua" && kImplemented[i])
            journal_line(std::string("settings ask ") + kNames[i] + "=lua: its Lua copy is gone since phase 5, stays native");
    }
}

// status.json: what this DLL decided at start (read by humans and, from phase 1, by the Lua side).
void write_status() {
    nlohmann::json j;
    j["version"] = kVersion;
    j["started"] = now_text();
    j["active"] = g_active;
    j["core"] = (g_api && g_api->get_version) ? g_api->get_version() : "absent";
    j["kill_file"] = std::filesystem::exists(kOffFile);
    nlohmann::json owners;
    for (size_t i = 0; i < kNames.size(); ++i) owners[kNames[i]] = g_owns[i] ? "native" : "lua";
    j["owners"] = owners;
    bgwriter::write((std::string(kDataDir) + "/status.json").c_str(), j.dump(4));
}

}  // namespace
void (*g_screens_frame)() = nullptr;
bool g_light_fight = true;                              // see light_fight()
namespace { bool light_fight_flag() { return g_light_fight; } void set_light_fight(bool v) { g_light_fight = v; } }
namespace {
void timed(perf::Id id, void (*fn)()) { const int64_t t0 = perf::begin(); fn(); perf::end(id, t0); }
bool on_message_cb(void*, unsigned int msg, unsigned long long wparam, long long) {
    const char* name = nullptr;
    bool final_msg = false;
    if (msg == WM_CLOSE) name = "WM_CLOSE";
    else if (msg == WM_DESTROY) { name = "WM_DESTROY"; final_msg = true; }
    else if (msg == WM_NCDESTROY) { name = "WM_NCDESTROY"; final_msg = true; }
    else if (msg == WM_ENDSESSION && wparam != 0) { name = "WM_ENDSESSION"; final_msg = true; }
    if (!name) return true;
    if (!g_closing.load(std::memory_order_acquire)) {
        g_closing_tick.store((int64_t)GetTickCount64(), std::memory_order_relaxed);
        g_closing_final.store(final_msg, std::memory_order_relaxed);
        g_closing.store(true, std::memory_order_release);
        log_info("[ColorModNative] closing: the game window got %s, the Color Mod stops", name);
        journal_flush();
    } else if (final_msg && !g_closing_final.load(std::memory_order_relaxed)) {
        g_closing_final.store(true, std::memory_order_release);
    }
    return true;
}
// Threads start only after the game's first frames (REFramework's startup suspends every thread while it holds the
// loader lock: a thread of ours caught holding the heap / CRT lock there froze a tester's game). Until then the file
// writes queue up (bgwriter) and are written when the writer starts. Triggered by the first of REFramework's present
// callback (works without the core: kill switch, core missing) and the core's frame callback.
constexpr uint64_t kThreadsAfterFrames = 300;
std::atomic<bool> g_threads_started{false};
std::atomic<uint64_t> g_present_count{0};
void cm_log(const std::string& s) { journal("%s", s.c_str()); }
void start_threads() {
    if (g_threads_started.exchange(true)) return;
    bgwriter::start();
    if (g_active) {
        fighters::preload_async();                                   // the game's fighter names, off the game's frames
        engine::color_mods::start_worker(engine::color_mods::root_of("reframework/data"), cm_log);
    }
}
void on_present_cb() {
    if (g_present_count.fetch_add(1, std::memory_order_relaxed) + 1 == kThreadsAfterFrames) start_threads();
}

void SF6CORE_CALL on_frame(uint64_t, void*) {
    ++g_frame;
    if (g_frame == kThreadsAfterFrames) start_threads();
    if (g_closing.load(std::memory_order_acquire)) {
        if (!g_closing_final.load(std::memory_order_relaxed)) {
            const uint64_t elapsed = GetTickCount64() - (uint64_t)g_closing_tick.load(std::memory_order_relaxed);
            if (elapsed > 30000) {
                g_closing.store(false, std::memory_order_release);
                log_info("[ColorModNative] closing: cancelled, the Color Mod resumes");
            }
        }
        wjournal::flush();
        journal_flush();
        return;
    }
    perf::frame();                                     // the game frame that just ended (perf_live.log when on)
    const int64_t t0 = perf::begin();
    if (g_bridge_decide.frame) timed(perf::FrameDecide, g_bridge_decide.frame);
    if (g_bridge_vfx.feed_frame) timed(perf::FrameVfxFeed, g_bridge_vfx.feed_frame);
    if (g_bridge_dipanel.on_frame) timed(perf::FrameDipanel, g_bridge_dipanel.on_frame);
    if (g_bridge_tracker.on_frame) timed(perf::FrameTracker, g_bridge_tracker.on_frame);
    if (g_bridge_guide.on_frame) timed(perf::FrameGuide, g_bridge_guide.on_frame);
    if (g_bridge_locale.on_frame) timed(perf::FrameLocale, g_bridge_locale.on_frame);
    if (g_bridge_camera.on_frame) timed(perf::FrameCamera, g_bridge_camera.on_frame);
    if (g_screens_frame) {
        const int64_t ts = perf::begin();
        try { g_screens_frame(); } catch (...) { journal_line("screens frame: exception"); }
        perf::end(perf::FrameScreens, ts);
    }
    if (g_frame % kJournalFlushEvery == 0) { const int64_t tj = perf::begin(); journal_flush(); perf::end(perf::CoreJournalFlush, tj); }
    // write journal: sentinel check every 120 frames, flush every 60 frames when on
    if (g_frame % 120 == 2) { const int64_t tw = perf::begin(); wjournal::check(); perf::end(perf::CoreFileCheck, tw); }
    if (wjournal::active() && g_frame % 60 == 0) wjournal::flush();
    if (g_frame % 300 == 1) {                          // live_fight.txt: the light fight mode off
        const int64_t tf = perf::begin();
        const DWORD attr = GetFileAttributesA((std::string(kDataDir) + "/live_fight.txt").c_str());
        const bool light = attr == INVALID_FILE_ATTRIBUTES;
        if (light != light_fight_flag()) { set_light_fight(light); journal_line(light ? "fight mode: light (colours written once per fight)" : "fight mode: live (live_fight.txt: per-effect painting, periodic checks)"); }
        perf::end(perf::CoreFileCheck, tf);
    }
    perf::end(perf::CoreFrame, t0);
}

void SF6CORE_CALL on_flow(int32_t old_id, int32_t new_id, void*) {
    journal_line("flow " + std::to_string(old_id) + " -> " + std::to_string(new_id));
}

}  // namespace

// set by main.cpp (the editor module lives in engine/live, the decisions in engine/decide): keeps common free of
// engine includes
BridgeEditor g_bridge_editor{};
BridgeDecide g_bridge_decide{};
BridgeVfx g_bridge_vfx{};
BridgeRows g_bridge_rows{};
BridgeDipanel g_bridge_dipanel{};
BridgeTracker g_bridge_tracker{};
BridgeGuide g_bridge_guide{};
BridgeLocale g_bridge_locale{};
BridgeCamera g_bridge_camera{};
BridgeOptions g_bridge_options{};
BridgeDialogEditor g_bridge_dialog_editor{};

namespace {

// ---- Lua bridge (core 0.5.0+): SF6Core.clients["SF6_ColorModNative"].<name>(...) -------------------------------
thread_local std::string t_ret;                      // returned strings must outlive the call (the bridge copies them)
const char* arg_str(const SF6CoreValue* a, uint32_t n, uint32_t i) { return (i < n && a[i].type == SF6CORE_VAL_STRING && a[i].s) ? a[i].s : ""; }
int64_t arg_int(const SF6CoreValue* a, uint32_t n, uint32_t i, int64_t dflt) {
    if (i >= n) return dflt;
    if (a[i].type == SF6CORE_VAL_INT) return a[i].i;
    if (a[i].type == SF6CORE_VAL_DOUBLE) return (int64_t)a[i].d;
    return dflt;
}
void ret_int(SF6CoreValue& r, int64_t v) { r.type = SF6CORE_VAL_INT; r.i = v; }

// Every bridge function catches everything: an exception reaching the core's SEH guard would count as a fault of
// this client (breaker). A negative return makes the bridge raise a Lua error (the Lua side calls through pcall).
}  // namespace

// what the Lua's edit.native_bridge(name) answers: the DLL runs that feature and it is ready
bool bridge_owns(const std::string& name) {
    if (!g_active) return false;
    bool on = false;
    for (size_t i = 0; i < kNames.size(); ++i) if (name == kNames[i]) on = owns((Feature)i);
    // the colour patch (editor preview, engine) also needs its hooks in place and not switched off by faults
    if (on && (name == "editor" || name == "engine" || name == "decisions")) on = g_bridge_editor.ready && g_bridge_editor.ready();
    // the decisions: their own hooks too, and the engine's patch (load_settings already requires engine native)
    if (on && name == "decisions") on = owns(Feature::Engine) && g_bridge_decide.ready && g_bridge_decide.ready();
    // the VFX write engine: installed (refused while SF6_MemWriterNative.dll is loaded)
    if (on && name == "vfx_engine") on = g_bridge_vfx.ready && g_bridge_vfx.ready();
    // the body glow and the battle loop: their own hooks / callback in place (else the Lua keeps the job)
    if (on && name == "vfx_body") on = g_bridge_vfx.ready && g_bridge_vfx.ready() && g_bridge_vfx.body_ready && g_bridge_vfx.body_ready();
    if (on && name == "vfx") on = g_bridge_vfx.tick_ready && g_bridge_vfx.tick_ready();
    // screens_rows: hooks installed and not switched off by faults
    if (on && name == "screens_rows") on = g_bridge_rows.ready && g_bridge_rows.ready();
    if (on && name == "screens_dipanel") on = g_bridge_dipanel.ready && g_bridge_dipanel.ready();
    if (on && name == "screens_tracker") on = g_bridge_tracker.ready && g_bridge_tracker.ready();
    if (on && name == "screens_guide") on = g_bridge_guide.ready && g_bridge_guide.ready();
    if (on && name == "dialogs_locale") on = g_bridge_locale.ready && g_bridge_locale.ready();
    if (on && name == "dialogs_camera") on = g_bridge_camera.ready && g_bridge_camera.ready();
    if (on && name == "dialogs_options") on = g_bridge_options.ready && g_bridge_options.ready();
    // the editor needs what it is built on: the option engine, the locale, the camera gate, the guide bar, the preview
    if (on && name == "dialogs_editor") on = g_bridge_dialog_editor.ready && g_bridge_dialog_editor.ready() && owns(Feature::DialogsOptions) && g_bridge_options.ready && g_bridge_options.ready()
        && owns(Feature::DialogsLocale) && owns(Feature::DialogsCamera) && owns(Feature::ScreensGuide) && owns(Feature::Editor)
        && g_bridge_editor.ready && g_bridge_editor.ready();
    return on;
}

namespace {
int32_t SF6CORE_CALL lua_owns(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    const bool on = bridge_owns(arg_str(a, n, 0));
    r[0].type = SF6CORE_VAL_BOOL; r[0].b = on ? 1 : 0;
    return 1;
} catch (...) { return -1; }

int32_t SF6CORE_CALL lua_squares(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) try {
    uint32_t c0 = 0, c1 = 0;
    if (!g_bridge_decide.squares || max < 2 || !g_bridge_decide.squares((int)arg_int(a, n, 0, -1), (int)arg_int(a, n, 1, 0), c0, c1)) return 0;
    ret_int(r[0], c0);
    ret_int(r[1], c1);
    return 2;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_follow_debug(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_decide.follow_debug ? g_bridge_decide.follow_debug((int)arg_int(a, n, 0, -1)) : std::string("no decisions module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_vfx_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_vfx.status ? g_bridge_vfx.status() : std::string("no vfx engine");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_vfx_mech_probe(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_vfx.mech_probe ? g_bridge_vfx.mech_probe() : std::string("no vfx mech");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_vfx_body_probe(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_vfx.body_probe ? g_bridge_vfx.body_probe() : std::string("no vfx body");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_vfx_tick_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_vfx.tick_status ? g_bridge_vfx.tick_status() : std::string("no vfx tick");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_dipanel_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_dipanel.status ? g_bridge_dipanel.status() : std::string("no dipanel module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_tracker_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_tracker.status ? g_bridge_tracker.status() : std::string("no tracker module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_guide_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_guide.status ? g_bridge_guide.status() : std::string("no guide module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_locale_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_locale.status ? g_bridge_locale.status() : std::string("no locale module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_camera_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_camera.status ? g_bridge_camera.status() : std::string("no camera module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_opt_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_options.status ? g_bridge_options.status() : std::string("no options module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_decide_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_decide.status ? g_bridge_decide.status() : std::string("no decisions module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }
int32_t SF6CORE_CALL lua_rows_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_rows.status ? g_bridge_rows.status() : std::string("no rows module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }

int32_t SF6CORE_CALL lua_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = g_bridge_editor.status ? g_bridge_editor.status() : std::string("no editor module");
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }

void register_bridge() {
    if (!SF6CORE_HAS(g_api, lua_register_fn)) { journal_line("core without lua_register_fn (needs 0.5.0+): no Lua bridge"); return; }
    // only diagnostics / status functions remain; the Lua push/pull/relay paths are removed since phase 5
    struct { const char* name; SF6CoreLuaFn fn; } fns[] = {
        { "owns", lua_owns }, { "status", lua_status },
        { "squares", lua_squares }, { "follow_debug", lua_follow_debug }, { "decide_status", lua_decide_status },
        { "vfx_status", lua_vfx_status }, { "vfx_mech_probe", lua_vfx_mech_probe }, { "vfx_body_probe", lua_vfx_body_probe },
        { "vfx_tick_status", lua_vfx_tick_status },
        { "rows_status", lua_rows_status }, { "dipanel_status", lua_dipanel_status },
        { "tracker_status", lua_tracker_status }, { "guide_status", lua_guide_status },
        { "locale_status", lua_locale_status }, { "camera_status", lua_camera_status },
        { "opt_status", lua_opt_status },
    };
    for (auto& f : fns) {
        const SF6CoreStatus st = g_api->lua_register_fn(g_client, f.name, f.fn, nullptr);
        if (st != SF6CORE_OK) journal_line(std::string("lua_register_fn ") + f.name + " failed: " + std::to_string((int)st));
    }
    for (auto& f : g_bridge_dialog_editor.fns) {                 // phase 4d + 4e: the editor relay's entry points
        const SF6CoreStatus st = g_api->lua_register_fn(g_client, f.name, f.fn, nullptr);
        if (st != SF6CORE_OK) journal_line(std::string("lua_register_fn ") + f.name + " failed: " + std::to_string((int)st));
    }
}

bool connect_core() {
    // REFramework maps every plugin before initializing any: the core module is there even if its own
    // initialize runs later (registrations are queued by the core).
    HMODULE h = GetModuleHandleA("sf6core_native.dll");
    if (!h) { log_warn("[ColorModNative] sf6core_native.dll not loaded: inert"); return false; }
    auto get_api = reinterpret_cast<SF6CoreGetApiFn>(GetProcAddress(h, "sf6core_get_api"));
    if (!get_api) { log_warn("[ColorModNative] sf6core_get_api missing: inert"); return false; }
    g_api = get_api(SF6CORE_ABI_VERSION);
    // 0.4.0 at least: the discovery service (ADR-0012) is what every screen of this DLL will use.
    if (!g_api || !SF6CORE_HAS(g_api, ui_find_many)) {
        log_warn("[ColorModNative] core without the discovery API (needs sf6core 0.4.0+): inert");
        g_api = nullptr;
        return false;
    }
    SF6CoreClientDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.client_id = kClientId;
    desc.client_version = kVersion;
    desc.priority = 100;
    if (g_api->register_client(&desc, &g_client) != SF6CORE_OK) {
        log_warn("[ColorModNative] register_client refused: inert");
        g_api = nullptr;
        return false;
    }
    g_api->set_frame_callback(g_client, on_frame, nullptr);
    g_api->set_flow_callback(g_client, on_flow, nullptr);
    register_bridge();
    return true;
}

}  // namespace

const char* feature_name(Feature f) { return kNames[(size_t)f]; }
bool owns(Feature f) { return g_active && g_owns[(size_t)f]; }
bool active() { return g_active; }
bool closing() { return g_closing.load(std::memory_order_acquire); }
uint64_t frame() { return g_frame; }
bool light_fight() { return g_light_fight; }
const SF6CoreAPI* core() { return g_api; }
SF6CoreClient* client() { return g_client; }

void log_info(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); std::string s = vformat(fmt, ap); va_end(ap);
    reframework::API::get()->log_info("%s", s.c_str());
    journal_line(s);
}
void log_warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); std::string s = vformat(fmt, ap); va_end(ap);
    reframework::API::get()->log_warn("%s", s.c_str());
    journal_line(s);
}
void journal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); std::string s = vformat(fmt, ap); va_end(ap);
    journal_line(s);
}

bool init() {
    {
        auto* fns = reframework::API::get()->param()->functions;
        if (fns->on_present) fns->on_present(on_present_cb);   // (the core's frame callback starts them too)
        if (fns->on_message) fns->on_message(on_message_cb);
        else log_info("[ColorModNative] on_message not available: no closing guard");
    }
    if (std::filesystem::exists(kOffFile)) {
        log_info("[ColorModNative] %s present: inert (everything stays with Lua)", kOffFile);
        write_status();
        journal_flush();
        return true;
    }
    g_active = connect_core();
    if (g_active) load_settings();
    write_status();
    std::string owners;
    for (size_t i = 0; i < kNames.size(); ++i) owners += std::string(i ? ", " : "") + kNames[i] + "=" + (g_owns[i] ? "native" : "lua");
    log_info("[ColorModNative] %s loaded, core %s, owners: %s", kVersion,
             g_api && g_api->get_version ? g_api->get_version() : "absent", owners.c_str());
    journal_flush();
    return true;
}

// ---- background file writer -----------------------------------------------------------------------------------------
namespace bgwriter {
namespace {
struct Item { std::string path; std::string content; bool is_append; };
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Item> g_queue;
std::atomic<bool> g_stop{false};

void worker() {
    while (!g_stop.load(std::memory_order_relaxed)) {
        Item item;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_queue.empty() || g_stop.load(std::memory_order_relaxed); });
            if (g_queue.empty()) continue;
            item = std::move(g_queue.front());
            g_queue.pop_front();
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(item.path).parent_path(), ec);
        std::ofstream f(item.path, std::ios::binary | (item.is_append ? std::ios::app : std::ios::trunc));
        if (f) f << item.content;
    }
}
}  // namespace

void start() {
    std::thread(worker).detach();
}
void write(const char* path, std::string content) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back(Item{path, std::move(content), false});
    g_cv.notify_one();
}
void append(const char* path, std::string content) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back(Item{path, std::move(content), true});
    g_cv.notify_one();
}
}  // namespace bgwriter

}  // namespace cmn
