// fighters — see fighters.hpp.

#include "fighters.hpp"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../../common/fighter_names.hpp"

namespace fighters {

namespace {

constexpr int kMaxId = 99;   // the game's name file also holds ids such as 253 "Any" and 254 "Random"

const std::map<int, const char*>& fixed() {
    static const std::map<int, const char*> names = { { 1, "Ryu" }, { 2, "Luke" }, { 3, "Kimberly" }, { 4, "ChunLi" }, { 5, "Manon" },
        { 6, "Zangief" }, { 7, "JP" }, { 8, "Dhalsim" }, { 9, "Cammy" }, { 10, "Ken" }, { 11, "DeeJay" }, { 12, "Lily" }, { 13, "AKI" },
        { 14, "Rashid" }, { 15, "Blanka" }, { 16, "Juri" }, { 17, "Marisa" }, { 18, "Guile" }, { 19, "Ed" }, { 20, "EHonda" },
        { 21, "Jamie" }, { 22, "Akuma" }, { 25, "Sagat" }, { 26, "Bison" }, { 27, "Terry" }, { 28, "Mai" }, { 29, "Elena" },
        { 30, "CViper" }, { 31, "Alex" }, { 32, "Ingrid" }, { 33, "Yasmine" }, { 34, "Arjun" } };
    return names;
}

std::mutex g_mutex;
bool g_loaded = false;
std::atomic<bool> g_ready{ false };
std::string g_dir_override;
std::map<int, std::string> g_game;   // id -> reduced name, only ids the fixed table does not know

std::string exe_folder() {
#ifdef _WIN32
    wchar_t w[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, w, (DWORD)(sizeof(w) / sizeof(w[0])));
    if (n == 0 || n >= sizeof(w) / sizeof(w[0])) return {};
    std::string out;
    char buf[MAX_PATH * 4];
    int len = WideCharToMultiByte(CP_ACP, 0, w, (int)n, buf, (int)sizeof(buf), nullptr, nullptr);   // PakReader opens narrow paths
    if (len <= 0) return {};
    out.assign(buf, (size_t)len);
    size_t cut = out.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : out.substr(0, cut);
#else
    return {};
#endif
}

// Caller holds g_mutex.
void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    g_game.clear();
    const std::string dir = g_dir_override.empty() ? exe_folder() : g_dir_override;
    if (dir.empty()) return;
    for (const auto& [id, english] : game_fighter_names(dir)) {
        if (id < 1 || id > kMaxId || fixed().count(id)) continue;
        std::string n = ascii_name(english);
        if (!n.empty()) g_game[id] = n;
    }
    g_ready = true;
}

}  // namespace

std::string ascii_name(const std::string& english) {
    std::string s;
    for (unsigned char c : english)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s.push_back((char)c);
    return s;
}

std::string name_or_empty(int id) {
    auto it = fixed().find(id);
    if (it != fixed().end()) return it->second;
    if (id < 1 || id > kMaxId) return {};
    std::lock_guard<std::mutex> lock(g_mutex);
    load_locked();
    auto g = g_game.find(id);
    return g == g_game.end() ? std::string() : g->second;
}

std::string name(int id) {
    std::string n = name_or_empty(id);
    if (!n.empty()) return n;
    char b[16];
    std::snprintf(b, sizeof(b), "esf%03d", id);
    return b;
}

std::vector<std::pair<int, std::string>> all_known() {
    std::map<int, std::string> all;
    for (const auto& [id, n] : fixed()) all[id] = n;
    if (g_ready) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, n] : g_game) all[id] = n;
    }
    return { all.begin(), all.end() };
}

void preload_async() {
    std::thread([] {
        std::lock_guard<std::mutex> lock(g_mutex);
        load_locked();
    }).detach();
}

bool ready() { return g_ready; }

void set_game_dir_for_test(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dir_override = dir;
    g_loaded = false;
    g_ready = false;
}

}  // namespace fighters
