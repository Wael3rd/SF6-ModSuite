#include "community.hpp"

#include <windows.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <random>

#include "auth.hpp"
#include "client.hpp"
#include "common.hpp"
#include "identity.hpp"
#include "model/luajson.hpp"
#include "steam_ticket.hpp"

namespace community {
namespace {

constexpr const char* kConfigPath = "reframework/data/SF6_Community_data/config.json";
constexpr const char* kResponsePath = "reframework/data/SF6_Community_data/_native_response.json";
constexpr const char* kOldBridgeDll = "SF6_CommunityBridge.dll";
// The probe switch: present = method A once, ~10 s after the first frame (the title screen), logged, the ticket never sent
constexpr const char* kProbeFile = "reframework/data/SF6_CRSMNative_data/steam_ticket_probe.txt";
constexpr const char* kSteamLog = "reframework/data/SF6_CRSMNative_data/steam_ticket.log";   // this session's Steam lines
constexpr uint64_t kProbeDelayFrames = 600;

Client g_client;
bool g_on = false;                     // owner native and initialised
bool g_old_bridge = false;
steam::Engine* g_steam = nullptr;      // never deleted: Steam may be gone when the process exits
bool g_probe_file = false;
uint64_t g_probe_at = 0;
std::atomic<bool> g_steam_log_open{false};

// steam_ticket.log: the Steam steps (game thread) and the Worker's "auth:" lines (its thread), this session only
void steam_file(const std::string& s) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    std::string line = std::string("[") + b + "] " + s + "\n";
    if (!g_steam_log_open.exchange(true)) cmn::bgwriter::write(kSteamLog, std::move(line));   // a new session: a new file
    else cmn::bgwriter::append(kSteamLog, std::move(line));
}

void steam_log(const std::string& s) {
    cmn::journal("steam ticket: %s", s.c_str());
    steam_file(s);
}

// the answer of the request the Lua shim is waiting for
std::mutex g_slot_mx;
enum class Slot { Idle, Pending, Ready, TimedOut } g_slot = Slot::Idle;
bool g_slot_ok = false;

// identity: resolved again at every call (dialog open, share, vote: the CFN can change); only the anon_ number and the last
// banner CFN are kept for the session (IdentitySession). config.json is re-read only when its write time changes.
std::mutex g_id_mx;
IdentitySession g_id_session;
nlohmann::json g_cfg;
FILETIME g_cfg_time{};
bool g_cfg_read = false;

thread_local std::string t_ret[2];
const char* arg_str(const SF6CoreValue* a, uint32_t n, uint32_t i) {
    if (i >= n) return "";
    if (a[i].type == SF6CORE_VAL_STRING && a[i].s) return a[i].s;
    return "";
}
void ret_str(SF6CoreValue* r, int i, std::string s) {
    t_ret[i] = std::move(s);
    r[i].type = SF6CORE_VAL_STRING;
    r[i].s = t_ret[i].c_str();
}

bool build_command(const SF6CoreValue* a, uint32_t n, nlohmann::json* cmd) {
    const std::string name = arg_str(a, n, 0);
    nlohmann::json c = nlohmann::json::object();
    c["cmd"] = name;
    if (name == "list") {
        c["char"] = arg_str(a, n, 1); c["sort"] = arg_str(a, n, 2); c["page"] = arg_str(a, n, 3);
        c["voter_cfn"] = arg_str(a, n, 4); c["per_page"] = arg_str(a, n, 5);
    } else if (name == "download") {
        c["id"] = arg_str(a, n, 1);
    } else if (name == "vote") {
        c["id"] = arg_str(a, n, 1); c["cfn"] = arg_str(a, n, 2);
        const std::string v = arg_str(a, n, 3);
        c["value"] = (v == "-1") ? -1 : 1;                 // a number: the Worker checks value !== 1
    } else if (name == "delete") {
        c["id"] = arg_str(a, n, 1); c["cfn"] = arg_str(a, n, 2);
    } else if (name == "share") {
        c["char"] = arg_str(a, n, 1); c["name"] = arg_str(a, n, 2); c["cfn"] = arg_str(a, n, 3);
        c["player"] = arg_str(a, n, 4); c["description"] = arg_str(a, n, 5);
        // the pack travels as a path relative to reframework/data; the worker reads it
        c["file_path"] = std::string("reframework/data/") + arg_str(a, n, 6);
    } else {
        return false;
    }
    *cmd = std::move(c);
    return true;
}

int32_t SF6CORE_CALL lua_com_request(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    r[0].type = SF6CORE_VAL_BOOL; r[0].b = 0;
    if (!g_on) return 1;
    nlohmann::json cmd;
    if (!build_command(a, n, &cmd)) return 1;
    g_client.abandon();                                    // one request at a time: the shim already refused a second one ("busy")
    { std::lock_guard<std::mutex> lk(g_slot_mx); g_slot = Slot::Pending; g_slot_ok = false; }
    const bool sent = g_client.send(std::move(cmd), [](bool ok, const nlohmann::json& data) {
        std::lock_guard<std::mutex> lk(g_slot_mx);
        const bool timeout = !ok && data.is_object() && data.contains("error") && data["error"] == "timeout";
        g_slot = timeout ? Slot::TimedOut : Slot::Ready;
        g_slot_ok = ok;
    });
    r[0].b = sent ? 1 : 0;
    return 1;
} catch (...) { return -1; }

int32_t SF6CORE_CALL lua_com_poll(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 2) return 0;
    const char* state = "idle";
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(g_slot_mx);
        switch (g_slot) {
            case Slot::Idle: state = "idle"; break;
            case Slot::Pending: state = "pending"; break;
            case Slot::Ready: state = "ready"; ok = g_slot_ok; g_slot = Slot::Idle; break;        // consumed
            case Slot::TimedOut: state = "timeout"; g_slot = Slot::Idle; break;
        }
    }
    ret_str(r, 0, state);
    r[1].type = SF6CORE_VAL_BOOL; r[1].b = ok ? 1 : 0;
    return 2;
} catch (...) { return -1; }

int32_t SF6CORE_CALL lua_com_busy(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    r[0].type = SF6CORE_VAL_BOOL; r[0].b = g_client.busy() ? 1 : 0;
    return 1;
} catch (...) { return -1; }

int32_t SF6CORE_CALL lua_com_abandon(void*, const SF6CoreValue*, uint32_t, SF6CoreValue*, uint32_t) try {
    g_client.abandon();
    std::lock_guard<std::mutex> lk(g_slot_mx);
    g_slot = Slot::Idle;
    return 0;
} catch (...) { return -1; }

Identity identity_now(const std::string& banner) {
    std::lock_guard<std::mutex> lk(g_id_mx);
    // a stat per call, the (small) file is parsed again only when it changed: nothing heavy on the frame that asks
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool have = GetFileAttributesExA(kConfigPath, GetFileExInfoStandard, &fa) != 0;
    if (!g_cfg_read || !have || CompareFileTime(&fa.ftLastWriteTime, &g_cfg_time) != 0) {
        g_cfg = model::luajson::load_file(kConfigPath);
        g_cfg_time = have ? fa.ftLastWriteTime : FILETIME{};
        g_cfg_read = true;
    }
    static std::mt19937 rng{(unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId()};
    return resolve_identity_session(g_id_session, banner, g_cfg, [] { return (int)(100000 + rng() % 900000); });
}

int32_t SF6CORE_CALL lua_com_identity(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max) try {
    if (max < 2) return 0;
    const Identity id = identity_now(arg_str(a, n, 0));
    ret_str(r, 0, id.cfn);
    ret_str(r, 1, id.name);
    return 2;
} catch (...) { return -1; }

}  // namespace

void init() {
    if (!cmn::owns(cmn::Feature::Community)) return;
    set_log([](const std::string& s) { cmn::journal("%s", s.c_str()); if (s.rfind("auth:", 0) == 0) steam_file(s); });
    g_old_bridge = GetModuleHandleA(kOldBridgeDll) != nullptr;
    if (g_old_bridge)
        cmn::log_warn("[CRSMNative] %s is loaded: not needed with community=native (it serves a request file nobody writes any more); remove it from reframework\\plugins", kOldBridgeDll);
    // 2.1.0: no Steam fill of config.json any more (player_id / player_name); the identity is the token of identity.json
    set_client_version(cmn::version());
    g_steam = new steam::Engine(steam::resolve, steam_log, [] { return (uint64_t)GetTickCount64(); });
    g_client.worker().set_tickets(g_steam);
    std::error_code ec;
    g_probe_file = std::filesystem::exists(kProbeFile, ec);
    if (g_probe_file) cmn::journal("community: %s present: Steam ticket probe ~10 s after the first frame (logged, never sent)", kProbeFile);
    g_client.configure(kConfigPath, kResponsePath);
    // (the worker thread starts at frame 300, see on_frame: no thread of ours alive while REFramework starts up;
    // requests sent before that wait in its queue)
    g_on = true;
    cmn::journal("community: owner native, WinHTTP client configured (worker starts after the first frames)");
}

void on_frame(uint64_t frame) {
    if (!g_on) return;
    static bool worker_started = false;
    if (!worker_started && frame >= 300) { worker_started = true; g_client.start(); }
    if (g_steam && !cmn::closing()) {
        if (g_probe_file) {
            if (!g_probe_at) g_probe_at = frame + kProbeDelayFrames;
            else if (frame >= g_probe_at) { g_probe_file = false; g_steam->probe("switch file steam_ticket_probe.txt"); }
        }
        g_steam->frame();                            // the Steam calls of a ticket (nothing to do: two atomic loads)
    }
    g_client.poll();
    if (frame % 300 == 0) {                          // status.json follows the counters (a few bytes, written off the game thread)
        const Client::Counters c = g_client.counters();
        const uint64_t sum = c.sent + c.answered + c.timeouts + c.busy_refusals;
        static uint64_t last = 0;
        if (sum != last) { last = sum; cmn::refresh_status(); }
    }
}

void register_lua(const SF6CoreAPI* api, SF6CoreClient* client) {
    if (!g_on || !api || !SF6CORE_HAS(api, lua_register_fn)) return;
    struct Reg { const char* name; SF6CoreLuaFn fn; };
    const Reg fns[] = { { "com_request", lua_com_request }, { "com_poll", lua_com_poll }, { "com_busy", lua_com_busy },
                        { "com_abandon", lua_com_abandon }, { "com_identity", lua_com_identity } };
    for (const Reg& f : fns) {
        (void)client;
        const SF6CoreStatus st = cmn::lua_register_timed(f.name, f.fn);
        if (st != SF6CORE_OK) cmn::journal("lua_register_fn %s failed: %d", f.name, (int)st);
    }
}

bool on() { return g_on; }
Client& client() { return g_client; }
Identity identity(const std::string& banner_cfn) { return identity_now(banner_cfn); }

std::string status() {
    nlohmann::json j = nlohmann::json::object();
    j["on"] = g_on;
    j["old_bridge_dll"] = g_old_bridge;
    if (g_on) {
        const Client::Counters c = g_client.counters();
        j["sent"] = c.sent; j["answered"] = c.answered; j["timeouts"] = c.timeouts; j["busy"] = c.busy_refusals; j["failures"] = c.failures;
        const int ts = g_client.worker().token_state();
        j["token"] = ts < 0 ? "not read yet" : ts ? "yes (identity.json)" : "none";
        if (g_steam) j["steam_ticket"] = g_steam->status();
    }
    return j.dump();
}

}  // namespace community
