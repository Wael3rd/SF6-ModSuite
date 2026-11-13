// SF6 Mod Suite updater: the in-game host (update_host.hpp).
#include "update_host.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "update_game.hpp"
#include "update_install.hpp"
#include "update_sys.hpp"
#include "update_text.hpp"

namespace sf6up::host {

namespace {

enum class St : int { Idle, Dormant, Checking, Offer, AskInstall, Installing, AskRestart, WaitRestart, AskFailed, WaitFailed, Done };

constexpr uint64_t kPollEvery = 30;              // frames between two looks at the screen
constexpr uint64_t kStableFrames = 300;          // a screen held 5 s: the player is in a menu

std::atomic<bool> g_ready{false};
std::atomic<int> g_state{(int)St::Idle};
HostInfo g_info;
std::unique_ptr<Paths> g_paths;
std::mutex g_mx;                                 // what follows, between the game thread and the workers
Plan g_plan;
InstallResult g_result;
std::deque<std::string> g_lines;                 // for the host's log, written on the game thread
std::atomic<int> g_pending{0};                   // lines waiting in g_lines

uint64_t g_frames = 0;
int32_t g_flow = -2;
uint64_t g_flow_since = 0;
bool g_menu_idle = false, g_menu_seen = false;
std::atomic<bool> g_recovered{false};

St state() { return (St)g_state.load(); }
void set_state(St s) { g_state.store((int)s); }

// ---- the log: the host's (on the game thread) and update.log (any thread) ----
void log(const std::string& line) {
    {
        std::lock_guard lk(g_mx);
        if (g_lines.size() < 200) { g_lines.push_back(line); g_pending.store((int)g_lines.size()); }
    }
    if (g_paths) append_log(*g_paths, line);
}

void flush_log() {
    if (g_pending.load(std::memory_order_relaxed) == 0) return;
    std::deque<std::string> lines;
    {
        std::lock_guard lk(g_mx);
        lines.swap(g_lines);
        g_pending.store(0);
    }
    if (g_info.log)
        for (auto& l : lines) g_info.log(l.c_str());
}

void spawn(void (*fn)()) {
    try {
        std::thread([fn] {
            try { fn(); } catch (...) { log("a worker stopped on an exception"); }
        }).detach();
    } catch (...) {
        log("a worker could not start");
        set_state(St::Done);
    }
}

// ---- the election: the highest rank of the loaded copies runs the updater ----
bool elected() {
    const uint32_t mine = sf6suite_updater_rank();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&sf6suite_updater_rank), &self);
    for (const wchar_t* name : { L"sf6core_native.dll", L"SF6_CostumeSlotsNative.dll" }) {
        HMODULE h = GetModuleHandleW(name);
        if (!h || h == self) continue;
        using RankFn = uint32_t (*)();
        auto fn = reinterpret_cast<RankFn>(GetProcAddress(h, "sf6suite_updater_rank"));
        if (fn && fn() > mine) return false;
    }
    return mine != 0;
}

// ---- the workers ----
void check_worker() {
    const Paths& p = *g_paths;
    recover(p, log);
    g_recovered.store(true);
    const State s = load_state(p);
    // the off switch, and the Slotsloader Manager's installs (check_allowed): nothing leaves the game
    std::string mode_why;
    const bool managed = manager_mode(p.root, &mode_why);
    if (!mode_why.empty())
        log(std::string("manager_installs.json: ") + mode_why + (managed ? ": installed by the Slotsloader Manager" : ": normal mode (not the Slotsloader Manager's install)"));
    if (std::string off; !check_allowed(s, managed, &off)) { log(off); set_state(St::Done); return; }
    std::string text, why;
    if (!fetch_manifest(&text, &why)) { log("no manifest from the latest release: " + why); set_state(St::Done); return; }
    const std::wstring root = p.root;
    CheckResult r = run_check(p, [root](const KnownPackage& k) { return game::package_version(k, root); }, text, s.ignored);
    if (!r.ok) { log("the latest release's manifest was refused: " + r.why); set_state(St::Done); return; }
    for (auto& n : r.plan.notes) log(n);
    if (r.plan.items.empty()) { log("nothing newer to offer (latest release " + r.plan.suite + ")"); set_state(St::Done); return; }
    std::string offer = "release " + r.plan.suite + " offers:";
    for (auto& it : r.plan.items) offer += " " + it.pkg.id + " " + it.from + " -> " + it.pkg.version + ";";
    log(offer);
    {
        std::lock_guard lk(g_mx);
        g_plan = r.plan;
    }
    set_state(St::Offer);
}

Progress g_progress;                             // the install's, read by the game thread for the ticker

void install_worker() {
    Plan plan;
    {
        std::lock_guard lk(g_mx);
        plan = g_plan;
    }
    const std::string tag = plan.tag;
    g_progress.reset();
    // tests: an empty slow_download.txt in the updater's data folder holds the download at about 250 KiB a second, long enough
    // to read the percentage on a fast line
    const bool slow = exists(join(g_paths->data, "slow_download.txt"));
    if (slow) log("slow_download.txt: the download is held at about 250 KiB a second");
    InstallResult r = run_install(*g_paths, plan, [tag, slow](const PlanItem& it, const std::wstring& dest, const BytesFn& bytes, std::string* why) {
        if (!slow) return fetch_from_github(tag, it, dest, bytes, why);
        int64_t last = 0;
        return fetch_from_github(tag, it, dest, [&](int64_t n) {
            if (bytes) bytes(n);
            if (n > last) Sleep((DWORD)((n - last) / 256));
            last = n;
        }, why);
    }, log, -1, &g_progress);
    log("ticker: " + game::ticker_stats());
    {
        std::lock_guard lk(g_mx);
        g_result = r;
    }
    set_state(r.ok ? St::AskRestart : St::AskFailed);
}

void ignore_worker() {
    std::string suite;
    {
        std::lock_guard lk(g_mx);
        suite = g_plan.suite;
    }
    const bool ok = update_state(*g_paths, [&](State& s) { s.ignored = suite; });
    log("release " + suite + (ok ? " ignored: not offered again" : " ignored, but state.json could not be written"));
}

void show_zip_worker() {
    log(show_downloads(*g_paths) ? "the kept zip shown in Explorer" : "Explorer could not be started");
}

void cleanup_worker() {
    for (int i = 0; i < 600 && !g_recovered.load(); ++i) Sleep(100);
    cleanup(*g_paths, log);
}

void restart() {
    std::wstring exe(32768, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size()));
    const std::wstring image = exe.substr(exe.find_last_of(L"\\/") + 1);
    std::string why;
    if (!start_waiter(GetCurrentProcessId(), image, &why)) {
        log("restart: " + why + ": the new version applies at the next start");
        return;
    }
    log(close_game_window() ? "restart: the game is closing, Steam starts it again once it is gone"
                            : "restart: the game's window was not found: the new version applies at the next start");
}

// ---- the screen: a menu held kStableFrames, no battle, no box of the game's ----
// The flow id (bFlowManager's flow map _ID) is -1 on the title screen and changes with each screen family (48 = the menus after the
// title in Wael's log of 10/11, 0 = a loading, 3 / 9 = training). The first flow held 5 s without a battle description after the title
// is the main menu ("home"); afterwards a box shows in that flow, or in any other one without a battle description (a battle's
// description may stay on a flow map after the battle: home alone never depends on it).
int32_t g_home = -1;
int g_flow_lines = 0;
void watch_screen() {
    if (g_frames % kPollEvery != 0) return;
    const int32_t id = game::flow_id();
    if (id != g_flow) {
        if (g_flow_lines < 40 && state() != St::Done) { ++g_flow_lines; log("screen: flow " + std::to_string(id)); }
        g_flow = id;
        g_flow_since = g_frames;
    }
    g_menu_idle = false;
    if (id < 0 || g_frames - g_flow_since < kStableFrames) return;
    const bool battle = game::in_battle();
    if (g_home < 0) {
        if (battle) return;
        g_home = id;
        log("the main menu: flow " + std::to_string(id));
    }
    if (id != g_home && battle) return;
    g_menu_idle = !game::game_dialog_up();
}

// ---- the install on screen: one strip of the game's ticker, kept up while the install lasts, saying the line of the moment ----
constexpr uint64_t kStripMs = 3000;              // a strip's own time: it leaves that long after the install; the least between two requests
std::string g_lang = "en";                       // the offer's language: the strip's too
uint64_t g_strip_at = 0;                         // the last request (0: none yet)
bool g_strip_failed = false;

void show_progress() {
    if (g_strip_failed) return;
    const int step = g_progress.step.load();
    if (step == Progress::Idle) return;
    std::string suite;
    {
        std::lock_guard lk(g_mx);
        suite = g_plan.suite;
    }
    const std::string line = step == Progress::Download ? text::downloading(g_lang, suite, g_progress.percent()) : text::installing(g_lang, suite);
    const uint64_t now = GetTickCount64();
    // the strip the game still has is kept (shown, or waiting behind another one); a new one only when it is gone
    if (g_strip_at != 0 && (game::ticker_keep(line) != game::Kept::Gone || now - g_strip_at < kStripMs)) return;
    g_strip_at = now;
    if (!game::ticker(line, kStripMs / 1000.0)) {
        g_strip_failed = true;
        log("the game's ticker could not be used (" + game::ticker_why() + "): the install goes on without its progress on screen");
    }
}

std::string offer_text(const std::string& lang) {
    std::lock_guard lk(g_mx);
    return text::offer(lang, g_plan);
}

std::string failure_text(const std::string& lang) {
    std::lock_guard lk(g_mx);
    return text::failed(lang, g_result.fail, g_result.what);
}

bool open_box(const std::string& body, const std::string& lang, const char* what, game::Buttons buttons = game::Buttons::YesNo,
              const std::string& third = {}) {
    if (game::box_open(body, text::title(lang), buttons, third)) { log(std::string("box shown: ") + what); return true; }
    log(std::string("the game's message box could not be opened (") + what + "): nothing done");
    return false;
}

}  // namespace

void init(const HostInfo& info) {
    if (g_ready.load()) return;
    try {
        g_info = info;
        g_paths = std::make_unique<Paths>(exe_folder());
        g_ready.store(true);
    } catch (...) {
    }
}

void frame() {
    if (!g_ready.load(std::memory_order_relaxed)) return;
    St s = state();
    if (s == St::Dormant) {                          // the workers' last lines
        try { flush_log(); } catch (...) {}
        return;
    }
    try {
        ++g_frames;
        flush_log();
        if (s == St::Idle) {
            if (!elected()) { set_state(St::Dormant); return; }
            log("this copy runs the updater (rank " + std::to_string(sf6suite_updater_rank()) + ")");
            set_state(St::Checking);
            spawn(check_worker);
            return;
        }
        watch_screen();
        if (g_menu_idle && !g_menu_seen) {          // the new version reached the menu: the old copies can go
            g_menu_seen = true;
            spawn(cleanup_worker);
        }
        if (s == St::Done) {                        // nothing more to do once the clean-up is under way: no more looks at the screen
            if (g_menu_seen) set_state(St::Dormant);
            return;
        }
        switch (s) {
        case St::Offer: {
            if (!g_menu_idle) break;
            const std::string lang = game::language();
            g_lang = lang;
            set_state(open_box(offer_text(lang), lang, "offer", game::Buttons::YesNoThird, text::ignore(lang)) ? St::AskInstall : St::Done);
            break;
        }
        case St::AskInstall: {
            const int a = game::box_poll();
            if (a == -1) break;
            if (a == 1) { log("Install chosen"); set_state(St::Installing); spawn(install_worker); }
            else if (a == 2) { log("Ignore chosen"); set_state(St::Done); spawn(ignore_worker); }
            else if (a == 0) { log("No chosen: asked again at the next start"); set_state(St::Done); }
            else { log("the box went away without an answer: asked again at the next start"); set_state(St::Done); }
            break;
        }
        case St::Installing:
            show_progress();
            break;
        case St::AskRestart:
            if (!g_menu_idle) break;
        {   // the third button shows the kept zip (Fluffy Mod Manager's players replace theirs with it), then the box comes back
            const std::string lang = game::language();
            set_state(open_box(text::done(lang), lang, "installed, restart?", game::Buttons::YesNoThird, text::show_zip(lang)) ? St::WaitRestart
                                                                                                                        : St::Done);
        }
            break;
        case St::WaitRestart: {
            const int a = game::box_poll();
            if (a == -1) break;
            if (a == 2) { log("Show zip chosen"); spawn(show_zip_worker); set_state(St::AskRestart); break; }
            set_state(St::Done);
            if (a == 1) restart();
            else log("restart declined: the new version applies at the next start");
            break;
        }
        case St::AskFailed:
            if (!g_menu_idle) break;
            set_state(open_box(failure_text(game::language()), game::language(), "failed", game::Buttons::Ok) ? St::WaitFailed : St::Done);
            break;
        case St::WaitFailed: {
            const int a = game::box_poll();
            if (a == -1) break;
            set_state(St::Done);
            log("failure: asked again at the next start");
            break;
        }
        default:
            break;
        }
    } catch (...) {
        game::box_drop();
        set_state(St::Done);
        log("stopped on an exception");
    }
}

}  // namespace sf6up::host

extern "C" __declspec(dllexport) uint32_t sf6suite_updater_rank(void) {
    if (!sf6up::host::g_ready.load()) return 0;
    return (sf6up::host::kUpdaterVersion << 8) | (sf6up::host::g_info.role & 0xFF);
}
