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
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <reframework/API.hpp>

#include "nlohmann/json.hpp"
#include "community/community.hpp"
#include "owners.hpp"
#include "perf.hpp"
#include "standalone.hpp"

namespace cmn {
namespace {

constexpr const char* kVersion = "2.2.4";              // 2.2.4: descriptions shown in full (Manager help line follows the spin, Community dialog area grown, Like / Sort on the guide row); 2.2.3: the first Community list of a session links the Steam account; 2.2.2: a pad's L1 / R1 / Y / X do what A / E / T / R do in the Community dialog, with the pad's glyphs when the game's guide shows a pad's, and the dialog built before its window opens (a slow list answer left nothing open); 2.2.1: lighter game-thread work (status, switch files, training entry); 2.2.0: Arjun, packs carry the training setup, pack descriptions; 2.1.3: the UI claimed at the first frame without a CRSM Lua; 2.1.2: background threads start after the first frames; 2.1.1: a missing settings.json no longer leaves the mod inert, "Verifying with Steam..." and clear wait messages instead of "busy", source in SF6-ModSuite crsm/; 2.1.0: Community writes with a Steam-verified identity
constexpr const char* kClientId = "SF6_CRSMNative";
// The game's working directory is the game folder (convention of the other native mods).
constexpr const char* kDataDir = "reframework/data/SF6_CRSMNative_data";
constexpr const char* kOffFile = "reframework/data/_crsm_native_off.txt";
constexpr size_t kJournalLines = 400;         // same depth and format as the Color Mod's journal
constexpr uint64_t kJournalFlushEvery = 30;   // frames
constexpr uint64_t kStatusEvery = 600;        // frames (~10 s): status.json rewritten when something in it changed

// What this build implements. A layer asked "native" in settings.json but not implemented stays with the Lua
// (journaled), so a settings file ahead of the DLL never leaves a layer without an owner.
constexpr std::array<bool, (size_t)Feature::Count> kImplemented = { true, false, false, false, false, false };   // P2: community
constexpr std::array<const char*, (size_t)Feature::Count> kNames = { "community", "files", "replay", "reversal", "record", "ui" };
static_assert(owners::kCount == (size_t)Feature::Count && owners::kUi == (size_t)Feature::Ui, "owners.hpp follows cmn::Feature");

const SF6CoreAPI* g_api = nullptr;
SF6CoreClient* g_client = nullptr;
bool g_active = false;
std::array<std::atomic<bool>, (size_t)Feature::Count> g_owns{};

// ---- layers (P3): implemented by the modules of this build, asked by settings.json, faults, hand-back --------------------
constexpr int kMaxFaults = 5;
std::array<bool, (size_t)Feature::Count> g_impl{};                // set_implemented() (or kImplemented)
std::array<bool, (size_t)Feature::Count> g_req{};                 // settings.json says "native"
std::array<std::atomic<int>, (size_t)Feature::Count> g_faults{};
std::array<std::atomic<bool>, (size_t)Feature::Count> g_tripped{};
std::array<std::atomic<bool>, (size_t)Feature::Count> g_hand_back_due{};
std::array<std::vector<void (*)()>, (size_t)Feature::Count> g_hand_back{};
struct FrameFn { void (*fn)(uint64_t); int perf_id; };
std::vector<FrameFn> g_frame_fns;
std::vector<void (*)(int32_t, int32_t)> g_flow_fns;
bool g_flow_cb = false;                       // the core took our flow callback
std::vector<std::pair<std::string, std::string (*)()>> g_status_fns;
std::vector<std::pair<std::string, SF6CoreLuaFn>> g_bridge_fns;

std::mutex g_jmx;                             // journal: written from the frame callback and from game hooks later
std::deque<std::string> g_lines;
bool g_dirty = false;
std::atomic<uint64_t> g_frame{0};
std::atomic<uint32_t> g_frame_tid{0};
std::optional<std::string> g_standalone;      // settings.json "standalone" (nullopt = missing: auto, standalone.hpp)
std::atomic<bool> g_lua_called{false};        // a Lua called the bridge (ui_claim, frame_poll, owns): a CRSM Lua is running

// Closing guard: the game window got WM_CLOSE / WM_DESTROY / WM_NCDESTROY / WM_ENDSESSION. Once set, every
// per-frame entry point and every pre-hook returns immediately (a WM_CLOSE the user cancels is lifted after 30 s).
std::array<std::atomic<bool>, 256> g_keys{};      // key_down(): REFramework's m_last_keys, from the window's messages
std::atomic<bool> g_closing{false};
std::atomic<bool> g_closing_final{false};
std::atomic<int64_t> g_closing_tick{0};

std::string vformat(const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

void journal_line(const std::string& s) {
    std::lock_guard lk(g_jmx);
    if (g_lines.size() >= kJournalLines) g_lines.pop_front();
    g_lines.push_back("[f" + std::to_string(g_frame.load()) + "] " + s);
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
    bgwriter::write((std::string(kDataDir) + "/debug_log.json").c_str(), arr.dump(4, ' ', false, nlohmann::json::error_handler_t::replace));
}

std::string now_text() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

std::string g_started;

// settings.json: { "owners": { "community": "lua", ... }, "standalone": "auto" }. A missing file is written once with
// owners::default_settings() and used at once: the six "native" when no CRSM Lua entry script is in reframework/autorun
// (as the release package ships it), the six "lua" when one is; the file can then be edited by hand. "standalone" (P6,
// standalone.hpp): "auto" (default, also when missing) or "off"; it matters only with every owner native.
// A layer is the DLL's when this build implements it AND settings.json asks "native" (default "lua");
// replay / reversal / record also need files native.
void apply_owners() {
    std::array<bool, owners::kCount> impl{};
    for (size_t i = 0; i < kNames.size(); ++i) impl[i] = g_impl[i] || kImplemented[i];
    const owners::Result r = owners::resolve(g_req, impl);          // the rule (owners.hpp), tested offline
    for (const std::string& n : r.notes) journal_line(n);
    for (size_t i = 0; i < kNames.size(); ++i) g_owns[i].store(r.owns[i]);
}

void load_settings() {
    for (auto& o : g_owns) o.store(false);
    g_req.fill(false);
    const std::string path = std::string(kDataDir) + "/settings.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        const std::string lua = standalone::find_lua([](const std::string& n) {     // plugin start: as ui::start() does
            std::error_code ec;
            return std::filesystem::exists(std::filesystem::path("reframework/autorun") / n, ec);
        });
        bgwriter::write(path.c_str(), owners::default_settings(!lua.empty()).dump(4));
        g_req = owners::default_requested(!lua.empty());
        journal_line(lua.empty() ? std::string("settings.json missing, no CRSM Lua script: written with the six owners native, standalone auto")
                                 : "settings.json missing, reframework/autorun/" + lua + " installed: written with the six owners lua");
        apply_owners();
        return;
    }
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("owners") || !j["owners"].is_object()) {
        journal_line("settings.json unreadable or without owners: every layer stays lua");
        return;
    }
    if (auto sa = j.find("standalone"); sa != j.end())
        g_standalone = sa->is_string() ? sa->get<std::string>() : sa->dump();
    for (size_t i = 0; i < kNames.size(); ++i) {
        auto it = j["owners"].find(kNames[i]);
        if (it != j["owners"].end() && it->is_string() && it->get<std::string>() == "native") {
            g_req[i] = true;
            if (!kImplemented[i] && !g_impl[i])
                journal_line(std::string("settings ask ") + kNames[i] + "=native: not implemented in this build, stays lua");
        }
    }
    apply_owners();
}

nlohmann::json status_json() {
    nlohmann::json j;
    j["version"] = kVersion;
    j["started"] = g_started;
    j["active"] = g_active;
    j["core"] = (g_api && g_api->get_version) ? g_api->get_version() : "absent";
    j["kill_file"] = std::filesystem::exists(kOffFile);
    nlohmann::json owners = nlohmann::json::object();
    for (size_t i = 0; i < kNames.size(); ++i) owners[kNames[i]] = g_owns[i].load() ? "native" : "lua";
    j["owners"] = owners;
    nlohmann::json faults = nlohmann::json::object();
    for (size_t i = 0; i < kNames.size(); ++i)
        if (g_faults[i].load() || g_tripped[i].load())
            faults[kNames[i]] = std::to_string(g_faults[i].load()) + (g_tripped[i].load() ? " (handed back to the Lua)" : "");
    j["faults"] = faults;
    for (auto& [name, fn] : g_status_fns) { try { j[name] = fn(); } catch (...) { j[name] = "?"; } }
    j["probes"] = g_probe_status ? g_probe_status() : std::string("off");
    j["community"] = nlohmann::json::parse(community::status(), nullptr, false);
    j["frames"] = g_frame.load();
    return j;
}

// Every kStatusEvery frames (frame callback): status.json follows the layers' counters, written (off the game thread,
// bgwriter) only when something other than the frame count changed since the last periodic write.
std::string g_status_seen;                    // frame thread: the last status checked, without "frames"
void status_tick() {
    nlohmann::json j = status_json();
    const nlohmann::json frames = j["frames"];
    j.erase("frames");
    std::string key = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (key == g_status_seen) return;
    g_status_seen = std::move(key);
    j["frames"] = frames;
    bgwriter::write((std::string(kDataDir) + "/status.json").c_str(), j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace));
}

bool on_message_cb(void*, unsigned int msg, unsigned long long wparam, long long) {
    PERF_TOP(pm, "OnMessage");
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) { if (wparam < 256) g_keys[(size_t)wparam].store(true); return true; }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) { if (wparam < 256) g_keys[(size_t)wparam].store(false); return true; }
    if (msg == WM_KILLFOCUS) { for (auto& k : g_keys) k.store(false); return true; }
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
        log_info("[CRSMNative] closing: the game window got %s, the DLL stops", name);
        journal_flush();
    } else if (final_msg && !g_closing_final.load(std::memory_order_relaxed)) {
        g_closing_final.store(true, std::memory_order_release);
    }
    return true;
}

struct PerfEndGuard { int64_t t0; int id; ~PerfEndGuard() { perf::end(id, t0); } };

void start_writer();
void SF6CORE_CALL on_frame(uint64_t, void*) {
    perf::frame();                                                   // the perf log (off: one branch every frame)
    const int64_t pt0 = perf::begin();
    static const int pid_frame = perf::reg("OnFrame", true);
    struct PerfEnd { int64_t t0; ~PerfEnd() { perf::end(pid_frame, t0); } } perf_end{ pt0 };
    const uint64_t fr = g_frame.fetch_add(1) + 1;
    g_frame_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (fr == 300) start_writer();
    if (g_closing.load(std::memory_order_acquire)) {
        if (!g_closing_final.load(std::memory_order_relaxed)) {
            const uint64_t elapsed = GetTickCount64() - (uint64_t)g_closing_tick.load(std::memory_order_relaxed);
            if (elapsed > 30000) {
                g_closing.store(false, std::memory_order_release);
                log_info("[CRSMNative] closing: cancelled, the DLL resumes");
            }
        }
        journal_flush();
        return;
    }
    if (g_probe_frame) { PERF_SUB(ps, "Frame:probes"); g_probe_frame(fr); }
    for (size_t i = 0; i < g_hand_back_due.size(); ++i) {           // a layer that went back to the Lua: its clean-up
        if (!g_hand_back_due[i].exchange(false)) continue;
        PERF_SUB(ph, "Frame:hand_back");
        for (auto fn : g_hand_back[i]) {
            try { fn(); } catch (...) { journal_line(std::string("hand-back of ") + kNames[i] + " faulted"); }
        }
        refresh_status();
    }
    for (const FrameFn& ff : g_frame_fns) {                          // each module catches its own faults
        PerfEndGuard pg{ perf::begin(), ff.perf_id };
        ff.fn(fr);
    }
    { PERF_SUB(pc, "Frame:community"); community::on_frame(fr); }    // P2 (checks its own owner)
    if (fr % kJournalFlushEvery == 0) { PERF_SUB(pj, "Frame:journal_flush"); journal_flush(); }
    if (fr % kStatusEvery == 61) { PERF_SUB(pw, "Frame:status_write"); try { status_tick(); } catch (...) {} }   // (phase: off the journal flush's frames)
}

void SF6CORE_CALL on_flow(int32_t old_id, int32_t new_id, void*) {
    PERF_TOP(pf, "OnFlow");
    journal_line("flow " + std::to_string(old_id) + " -> " + std::to_string(new_id));
    for (auto fn : g_flow_fns) { try { fn(old_id, new_id); } catch (...) {} }
}

// ---- Lua bridge (core 0.5.0+): SF6Core.clients["SF6_CRSMNative"].<name>(...) ---------------------------------------
thread_local std::string t_ret;                      // returned strings must outlive the call (the bridge copies them)
const char* arg_str(const SF6CoreValue* a, uint32_t n, uint32_t i) { return (i < n && a[i].type == SF6CORE_VAL_STRING && a[i].s) ? a[i].s : ""; }

// What the Lua will ask in the coming phases: does the DLL run that layer (and is it ready)? In P0 the answer is
// always no, the Lua keeps everything. Every bridge function catches everything: an exception reaching the core's
// SEH guard would count as a fault of this client (breaker).
int32_t SF6CORE_CALL lua_owns(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) try {
    note_lua_call();
    if (max < 1) return 0;
    bool on = false;
    const std::string name = arg_str(a, n, 0);
    for (size_t i = 0; i < kNames.size(); ++i) if (name == kNames[i]) on = owns((Feature)i);
    r[0].type = SF6CORE_VAL_BOOL; r[0].b = on ? 1 : 0;
    return 1;
} catch (...) { return -1; }

// owners(): the layers the DLL runs now as a bitmask (1 << Feature), read by the Lua once per frame (crsm/CrsmNative.lua).
int32_t SF6CORE_CALL lua_owners(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    r[0].type = SF6CORE_VAL_INT; r[0].i = (int64_t)owners_mask();
    return 1;
} catch (...) { return -1; }

int32_t SF6CORE_CALL lua_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = status_json().dump();
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }

// Every Lua bridge function goes through this wrapper (its context holds the real function): timed as "Lua:<name>".
struct BridgeCtx { SF6CoreLuaFn fn; int perf_id; };
std::deque<BridgeCtx> g_bridge_ctx;
int32_t SF6CORE_CALL bridge_thunk(void* ctx, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) {
    const BridgeCtx* c = static_cast<const BridgeCtx*>(ctx);
    const int64_t t0 = perf::begin();
    const int32_t rc = c->fn(nullptr, a, n, r, max);
    perf::end(c->perf_id, t0);
    return rc;
}
SF6CoreStatus register_timed(const char* name, SF6CoreLuaFn fn) {
    g_bridge_ctx.push_back({ fn, perf::reg((std::string("Lua:") + name).c_str(), true) });
    return g_api->lua_register_fn(g_client, name, bridge_thunk, &g_bridge_ctx.back());
}

void register_bridge() {
    if (!SF6CORE_HAS(g_api, lua_register_fn)) { journal_line("core without lua_register_fn (needs 0.5.0+): no Lua bridge"); return; }
    struct Reg { const char* name; SF6CoreLuaFn fn; };
    const Reg fns[] = { { "owns", lua_owns }, { "status", lua_status }, { "owners", lua_owners } };
    for (const Reg& f : fns) {
        const SF6CoreStatus st = register_timed(f.name, f.fn);
        if (st != SF6CORE_OK) journal_line(std::string("lua_register_fn ") + f.name + " failed: " + std::to_string((int)st));
    }
    for (auto& [name, fn] : g_bridge_fns) {                         // the layer modules' functions
        const SF6CoreStatus st = register_timed(name.c_str(), fn);
        if (st != SF6CORE_OK) journal_line("lua_register_fn " + name + " failed: " + std::to_string((int)st));
    }
}

bool connect_core() {
    // REFramework maps every plugin before initializing any: the core module is there even if its own initialize
    // runs later (registrations are queued by the core).
    HMODULE h = GetModuleHandleA("sf6core_native.dll");
    if (!h) { log_warn("[CRSMNative] sf6core_native.dll not loaded: inert"); return false; }
    auto get_api = reinterpret_cast<SF6CoreGetApiFn>(GetProcAddress(h, "sf6core_get_api"));
    if (!get_api) { log_warn("[CRSMNative] sf6core_get_api missing: inert"); return false; }
    g_api = get_api(SF6CORE_ABI_VERSION);
    if (!g_api || !g_api->register_client || !g_api->set_frame_callback) {
        log_warn("[CRSMNative] core API unusable (ABI %d): inert", SF6CORE_ABI_VERSION);
        g_api = nullptr;
        return false;
    }
    SF6CoreClientDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.client_id = kClientId;
    desc.client_version = kVersion;
    desc.priority = 100;
    if (g_api->register_client(&desc, &g_client) != SF6CORE_OK) {
        log_warn("[CRSMNative] register_client refused: inert");
        g_api = nullptr;
        return false;
    }
    g_api->set_frame_callback(g_client, on_frame, nullptr);
    if (g_api->set_flow_callback) g_flow_cb = g_api->set_flow_callback(g_client, on_flow, nullptr) == SF6CORE_OK;
    register_bridge();
    return true;
}

}  // namespace

void (*g_probe_frame)(uint64_t) = nullptr;
std::string (*g_probe_status)() = nullptr;

const char* feature_name(Feature f) { return kNames[(size_t)f]; }
bool owns(Feature f) { return g_active && g_owns[(size_t)f].load(std::memory_order_acquire); }

void set_implemented(Feature f) { g_impl[(size_t)f] = true; }
bool requested(Feature f) { return g_req[(size_t)f]; }
bool tripped(Feature f) { return g_tripped[(size_t)f].load(); }
void on_hand_back(Feature f, void (*fn)()) { if (fn) g_hand_back[(size_t)f].push_back(fn); }
void add_frame_fn(void (*fn)(uint64_t), const char* perf_name) {
    g_frame_fns.push_back({ fn, perf::reg(perf_name ? (std::string("Frame:") + perf_name).c_str() : "Frame:module", false) });
}
void add_flow_fn(void (*fn)(int32_t, int32_t)) { if (fn) g_flow_fns.push_back(fn); }
bool flow_events() { return g_flow_cb; }
void add_status(const char* name, std::string (*fn)()) { g_status_fns.emplace_back(name, fn); }
void bridge_fn(const char* name, SF6CoreLuaFn fn) { g_bridge_fns.emplace_back(name, fn); }
SF6CoreStatus lua_register_timed(const char* name, SF6CoreLuaFn fn) { return register_timed(name, fn); }

uint32_t owners_mask() {
    uint32_t m = 0;
    for (size_t i = 0; i < kNames.size(); ++i) if (owns((Feature)i)) m |= 1u << i;
    return m;
}

void fault(Feature f, const char* where) {
    const size_t i = (size_t)f;
    const int n = g_faults[i].fetch_add(1) + 1;
    journal_line(std::string("fault in ") + kNames[i] + " (" + (where ? where : "?") + "), " + std::to_string(n) + "/" +
                 std::to_string(kMaxFaults));
    if (n < kMaxFaults || g_tripped[i].exchange(true)) return;
    const bool was_owned = g_owns[i].exchange(false);
    log_warn("[CRSMNative] %s: %d faults, the layer goes back to the Lua", kNames[i], n);
    if (was_owned) g_hand_back_due[i].store(true);
    if (f == Feature::Files) {                                      // the layers that live on the DLL's files model follow
        for (Feature d : { Feature::Replay, Feature::Reversal, Feature::Record }) {
            const size_t k = (size_t)d;
            if (g_tripped[k].exchange(true)) continue;
            if (g_owns[k].exchange(false)) {
                g_hand_back_due[k].store(true);
                log_warn("[CRSMNative] %s goes back to the Lua with files", kNames[k]);
            }
        }
    }
    // The DLL's own UI (P4) drives every layer; with ui native the Lua UI is not loaded, so a layer that stops leaves the
    // screens without their mechanics: the UI stops too (the game's own menus, cleaned up at the unpause).
    const size_t u = (size_t)Feature::Ui;
    if (f != Feature::Ui && !g_tripped[u].load() && g_owns[u].load()) {
        g_tripped[u].store(true);
        if (g_owns[u].exchange(false)) {
            g_hand_back_due[u].store(true);
            log_warn("[CRSMNative] ui stops with %s (the game's own menus from now on)", kNames[i]);
        }
    }
}
void decline(Feature f, const char* why) {
    const size_t i = (size_t)f;
    if (!g_owns[i].exchange(false)) return;
    g_hand_back_due[i].store(true);
    log_warn("[CRSMNative] %s: stays with the Lua (%s)", kNames[i], why ? why : "?");
}
bool active() { return g_active; }
std::optional<std::string> setting_standalone() { return g_standalone; }
void note_lua_call() { g_lua_called.store(true, std::memory_order_relaxed); }
bool lua_called() { return g_lua_called.load(std::memory_order_relaxed); }
bool closing() { return g_closing.load(std::memory_order_acquire); }
uint64_t frame() { return g_frame.load(std::memory_order_relaxed); }
uint32_t frame_thread() { return g_frame_tid.load(std::memory_order_relaxed); }
const SF6CoreAPI* core() { return g_api; }
SF6CoreClient* client() { return g_client; }
const char* version() { return kVersion; }

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
void append_file(const char* path, std::string text) { bgwriter::append(path, std::move(text)); }
bool key_down(int vk) { return vk >= 0 && vk < 256 && g_keys[(size_t)vk].load(std::memory_order_relaxed); }

void refresh_status() { bgwriter::write((std::string(kDataDir) + "/status.json").c_str(), status_json().dump(4)); }

// The writer thread starts only after the game's first frames: REFramework's startup suspends every thread while it
// holds the loader lock, and a thread of ours caught holding the heap / CRT lock there froze a tester's game. Writes
// queue up until then. Started by the first of REFramework's present callback (works with the kill switch / without
// the core) and the core's frame callback.
namespace {
constexpr uint64_t kWriterAfterFrames = 300;
std::atomic<bool> g_writer_started{false};
std::atomic<uint64_t> g_present_count{0};
void start_writer() { if (!g_writer_started.exchange(true)) bgwriter::start(); }
void on_present_cb() { if (g_present_count.fetch_add(1, std::memory_order_relaxed) + 1 == kWriterAfterFrames) start_writer(); }
}  // namespace

bool init() {
    g_started = now_text();
    {
        auto* fns = reframework::API::get()->param()->functions;
        if (fns->on_present) fns->on_present(on_present_cb);
        if (fns->on_message) fns->on_message(on_message_cb);
        else log_info("[CRSMNative] on_message not available: no closing guard");
    }
    if (std::filesystem::exists(kOffFile)) {
        log_info("[CRSMNative] %s present: inert (everything stays with the Lua)", kOffFile);
        refresh_status();
        journal_flush();
        return true;
    }
    g_active = connect_core();
    if (g_active) {
        load_settings();
        community::init();                        // owner community = native: WinHTTP client + Lua bridge functions (P2)
        community::register_lua(g_api, g_client);
    }
    refresh_status();
    std::string owners;
    for (size_t i = 0; i < kNames.size(); ++i) owners += std::string(i ? ", " : "") + kNames[i] + "=" + (g_owns[i].load() ? "native" : "lua");
    log_info("[CRSMNative] %s loaded, core %s, owners: %s", kVersion,
             g_api && g_api->get_version ? g_api->get_version() : "absent", owners.c_str());
    journal_flush();
    return true;
}

// ---- background file writer -------------------------------------------------------------------------------------------
namespace bgwriter {
namespace {
struct Item { std::string path; std::string content; bool is_append; };
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Item> g_queue;
std::atomic<bool> g_stop{false};
struct Watch { std::string path; std::atomic<bool>* flag; };
std::vector<Watch> g_watches;                 // under g_mx
constexpr uint64_t kWatchEveryMs = 1000;

void poll_watches() {
    std::vector<Watch> w;
    { std::lock_guard<std::mutex> lk(g_mx); w = g_watches; }
    for (const Watch& x : w) {
        const bool want = GetFileAttributesA(x.path.c_str()) != INVALID_FILE_ATTRIBUTES;
        x.flag->store(want, std::memory_order_relaxed);
    }
}

void worker() {
    uint64_t last_poll = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const uint64_t tk = GetTickCount64();
        if (tk - last_poll >= kWatchEveryMs) { last_poll = tk; poll_watches(); }
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

void start() { std::thread(worker).detach(); }
void watch_file(const char* path, std::atomic<bool>* flag) {
    flag->store(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES, std::memory_order_relaxed);   // once, at registration
    std::lock_guard<std::mutex> lk(g_mx);
    g_watches.push_back(Watch{path, flag});
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
