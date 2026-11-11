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

namespace sf6up::host {

namespace {

#include "update_words.inc"

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
std::mutex g_file_mx;
bool g_file_started = false;

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
    if (!g_paths) return;
    std::lock_guard lk(g_file_mx);
    if (!g_file_started) {
        g_file_started = true;
        if (file_size(g_paths->log) > 256 * 1024) DeleteFileW(g_paths->log.c_str());
        make_dirs(g_paths->data);
    }
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(stamp, sizeof stamp, "[%Y-%m-%d %H:%M:%S] ", &tm);
    HANDLE h = CreateFileW(g_paths->log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string text = stamp + line + "\r\n";
    DWORD put = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr);
    CloseHandle(h);
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

// ---- the words ----
const char* tr(const std::string& lang, Word w) {
    int li = 0;
    for (int i = 0; i < kLangCount; ++i)
        if (lang == kLangs[i]) li = i;
    const char* t = kText[li][(int)w];
    return t && *t ? t : kText[0][(int)w];
}

std::string format(const char* tmpl, std::initializer_list<std::string> args) {
    std::string out;
    auto a = args.begin();
    for (const char* p = tmpl; *p; ++p) {
        if (*p == '%' && p[1] == 's') {
            if (a != args.end()) out += *a++;
            ++p;
        } else if (*p == '%' && p[1] == '%') {
            out += '%';
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
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
    if (!s.check) { log("off (check_for_updates is false in state.json): nothing fetched"); set_state(St::Done); return; }
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

void install_worker() {
    Plan plan;
    {
        std::lock_guard lk(g_mx);
        plan = g_plan;
    }
    const std::string tag = plan.tag;
    InstallResult r = run_install(*g_paths, plan, [tag](const PlanItem& it, const std::wstring& dest, std::string* why) {
        return fetch_from_github(tag, it, dest, why);
    }, log);
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
void watch_screen() {
    if (g_frames % kPollEvery != 0) return;
    const int32_t id = game::flow_id();
    const bool battle = id < 0 || game::in_battle();
    if (battle) { g_flow = -2; g_menu_idle = false; return; }
    if (id != g_flow) { g_flow = id; g_flow_since = g_frames; }
    g_menu_idle = g_frames - g_flow_since >= kStableFrames && !game::game_dialog_up();
}

std::string text_of_offer(const std::string& lang) {
    std::lock_guard lk(g_mx);
    std::string lines;
    for (auto& it : g_plan.items) {
        if (!lines.empty()) lines += "\n";
        lines += format(tr(lang, Word::Line), { it.pkg.name, it.from, it.pkg.version });
    }
    return format(tr(lang, Word::Offer), { g_plan.suite, lines, g_plan.suite });
}

std::string text_of_failure(const std::string& lang) {
    std::lock_guard lk(g_mx);
    Word w = Word::WhyPackage;
    switch (g_result.fail) {
    case Fail::Download: w = Word::WhyDownload; break;
    case Fail::Check: w = Word::WhyCheck; break;
    case Fail::Blocked: w = Word::WhyBlocked; break;
    case Fail::Files: w = Word::WhyFiles; break;
    default: break;
    }
    return format(tr(lang, Word::Failed), { format(tr(lang, w), { g_result.what }) });
}

bool open_box(const std::string& text, const std::string& lang, const char* what) {
    if (game::box_open(text, tr(lang, Word::Title))) { log(std::string("box shown: ") + what); return true; }
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
        case St::Offer:
            if (!g_menu_idle) break;
            if (game::box_open(text_of_offer(game::language()), tr(game::language(), Word::Title))) { log("box shown: offer"); set_state(St::AskInstall); }
            else { log("the game's message box could not be opened (offer): nothing done"); set_state(St::Done); }
            break;
        case St::AskInstall: {
            const int a = game::box_poll();
            if (a < 0) break;
            if (a == 1) { log("Install chosen"); set_state(St::Installing); spawn(install_worker); }
            else { log("Ignore chosen"); set_state(St::Done); spawn(ignore_worker); }
            break;
        }
        case St::AskRestart:
            if (!g_menu_idle) break;
            set_state(open_box(tr(game::language(), Word::Done), game::language(), "installed, restart?") ? St::WaitRestart : St::Done);
            break;
        case St::WaitRestart: {
            const int a = game::box_poll();
            if (a < 0) break;
            set_state(St::Done);
            if (a == 1) restart();
            else log("restart declined: the new version applies at the next start");
            break;
        }
        case St::AskFailed:
            if (!g_menu_idle) break;
            set_state(open_box(text_of_failure(game::language()), game::language(), "failed") ? St::WaitFailed : St::Done);
            break;
        case St::WaitFailed: {
            const int a = game::box_poll();
            if (a < 0) break;
            set_state(St::Done);
            if (a == 1) log("failure: asked again at the next start");
            else spawn(ignore_worker);
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
