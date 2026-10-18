#include "color_mods.hpp"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <thread>

#include "archive.hpp"

namespace engine::color_mods {
namespace fs = std::filesystem;

namespace {

constexpr const char* kCacheDir = ".cache";
constexpr const char* kComplete = ".complete";

// Same list and spelling as the costume_mods folders (Windows drops a trailing dot: "A.K.I").
const char* const kCharacters[] = {
    "Ryu", "Luke", "Kimberly", "Chun-Li", "Manon", "Zangief", "JP", "Dhalsim", "Cammy", "Ken", "Dee Jay", "Lily",
    "A.K.I", "Rashid", "Blanka", "Juri", "Marisa", "Guile", "Ed", "E. Honda", "Jamie", "Akuma", "Sagat", "M. Bison",
    "Terry", "Mai", "Elena", "C. Viper", "Alex", "Ingrid", "Yasmine", "Arjun",
};

bool is_dot(const fs::path& p) {
    const std::wstring n = p.filename().wstring();
    return !n.empty() && n[0] == L'.';
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool ext_is_archive(const fs::path& p) {
    const std::string e = lower(p.extension().string());
    return e == ".zip" || e == ".7z" || e == ".rar";
}

uint32_t fnv1a32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

// esfFFF_CCC_cmd_KKK.user.2 (letters of any case)
bool is_colour_name(const std::string& n_in) {
    const std::string n = lower(n_in);
    static const char* tail = ".user.2";
    if (n.size() != 3 + 3 + 1 + 3 + 1 + 3 + 1 + 3 + 7) return false;
    auto dig = [&](size_t a, size_t b) { for (size_t i = a; i < b; ++i) if (n[i] < '0' || n[i] > '9') return false; return true; };
    return n.compare(0, 3, "esf") == 0 && dig(3, 6) && n[6] == '_' && dig(7, 10) && n[10] == '_' &&
           n.compare(11, 4, "cmd_") == 0 && dig(15, 18) && n.compare(18, 7, tail) == 0;
}

bool keep_entry(const std::wstring& entry) {
    const size_t i = entry.find_last_of(L'/');
    std::string n;
    for (wchar_t c : (i == std::wstring::npos ? entry : entry.substr(i + 1))) n += c < 128 ? (char)c : '?';
    return lower(n) == "modinfo.ini" || is_colour_name(n);
}

bool has(const fs::path& p) { std::error_code ec; return fs::exists(p, ec); }

// Plain walk of a folder: regular files, no dot folders / dot files.
template <class F> void walk_visible(const fs::path& dir, F&& fn) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (is_dot(it->path())) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec)) fn(it->path());
    }
}

std::string describe_error(const std::string& what) { return what.empty() ? std::string("unknown error") : what; }

std::atomic<bool> g_started{false};
std::atomic<bool> g_changed{false};

}  // namespace

fs::path root_of(const fs::path& data_dir) { return data_dir / ".." / "color_mods"; }

const char* readme_text() {
    return R"README(Drop colour mods here, exactly as downloaded from Nexus:
  reframework/color_mods/<Character>/<the .zip, .7z or .rar file>
or unpacked:
  reframework/color_mods/<Character>/<Mod folder>/
or the loose files (esfNNN_CCC_cmd_KKK.user.2) in any folder below this one.

The character folder is only for your own tidiness: the fighter and the costume come from the
file names (esf001 = Ryu, 001 = Outfit 1...). Files placed directly in this folder work too.

Archives are unpacked once, in the background, a few seconds after the game starts, into the
hidden folder .cache (only the colour files are kept). The new colours appear in the colour list
of the character as soon as you are out of a fight, the character select and the colour editor;
otherwise at the next launch. Delete the archive or the folder to remove its colours.

Colours from this folder are listed as RW n, after the colours of the Color Mod's own colour
folder. The colour editor can edit them (SAVE keeps your change apart, RESET goes back to the
original) but never changes or deletes a file in this folder.
Folders and files whose name starts with a dot are ignored.
Messages are in reframework/data/SF6_ColorModNative_data/debug_log.json ("color_mods:").
)README";
}

std::vector<fs::path> list_archives(const fs::path& root) {
    std::vector<fs::path> out;
    walk_visible(root, [&](const fs::path& p) { if (ext_is_archive(p)) out.push_back(p); });
    return out;
}

std::string cache_key(const fs::path& archive) {
    std::error_code ec;
    const uint64_t sz = fs::file_size(archive, ec);
    const auto mt = fs::last_write_time(archive, ec).time_since_epoch().count();
    const std::string name = archive.filename().string();
    std::string safe;
    for (unsigned char c : name) {
        if (safe.size() >= 24) break;
        safe += (std::isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    char key[64];
    std::snprintf(key, sizeof(key), "_%08x", fnv1a32("v1:" + name + ":" + std::to_string(sz) + ":" + std::to_string((long long)mt)));
    return safe + key;
}

std::vector<fs::path> valid_cache_dirs(const fs::path& root) {
    std::vector<fs::path> out;
    const fs::path cache = root / kCacheDir;
    if (!has(cache)) return out;
    for (const fs::path& a : list_archives(root)) {
        const fs::path d = cache / cache_key(a);
        if (has(d / kComplete)) out.push_back(d);
    }
    return out;
}

std::vector<fs::path> candidate_files(const fs::path& root) {
    std::vector<fs::path> out;
    auto take = [&](const fs::path& p) {
        const std::string n = lower(p.filename().string());
        if (n.size() > 7 && n.compare(n.size() - 7, 7, ".user.2") == 0) out.push_back(p);
    };
    walk_visible(root, take);
    for (const fs::path& d : valid_cache_dirs(root)) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) take(it->path());
        }
    }
    return out;
}

void ensure_layout(const fs::path& root) {
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const char* c : kCharacters) fs::create_directories(root / c, ec);
    const fs::path readme = root / "README.txt";
    if (!has(readme)) {
        std::ofstream f(readme, std::ios::binary);
        if (f) {
            std::string t = readme_text(), crlf;
            for (char c : t) { if (c == '\n') crlf += '\r'; crlf += c; }
            f << crlf;
        }
    }
}

SyncReport sync(const fs::path& root, void (*log)(const std::string&)) {
    SyncReport rep;
    auto say = [&](const std::string& s) { if (log) log(s); };
    std::error_code ec;
    const fs::path cache = root / kCacheDir;
    std::set<std::string> used;
    for (const fs::path& a : list_archives(root)) {
        const std::string fname = a.filename().string();
        const std::string key = cache_key(a);
        const fs::path fin = cache / key, part = cache / (key + ".partial");
        if (has(fin / kComplete)) { used.insert(key); ++rep.already; continue; }
        const ArchiveKind kind = archive_sniff(a.wstring());
        if (kind == ArchiveKind::None) {
            say("color_mods: archive " + fname + " is not a zip, 7z or rar file, ignored");
            ++rep.failed;
            continue;
        }
        fs::create_directories(cache, ec);
        SetFileAttributesW(cache.c_str(), FILE_ATTRIBUTE_HIDDEN);
        fs::remove_all(part, ec);
        fs::remove_all(fin, ec);
        const auto t0 = std::chrono::steady_clock::now();
        const ArchiveResult r = archive_extract(a.wstring(), part.wstring(), keep_entry);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r.ok) {
            say("color_mods: archive " + fname + " (" + archive_kind_name(kind) + ") CANNOT be used: " + describe_error(r.error));
            fs::remove_all(part, ec);
            ++rep.failed;
            continue;
        }
        { std::ofstream m(part / kComplete, std::ios::binary); }
        fs::rename(part, fin, ec);
        if (ec) {
            say("color_mods: archive " + fname + " extracted but its cache folder could not be finalised: " + ec.message());
            fs::remove_all(part, ec);
            ++rep.failed;
            continue;
        }
        used.insert(key);
        ++rep.extracted;
        char buf[160];
        std::snprintf(buf, sizeof(buf), ") extracted in %.0f ms: %d files kept, %d skipped", ms, r.files_written, r.files_skipped);
        say("color_mods: archive " + fname + " (" + archive_kind_name(kind) + buf);
    }
    // cache folders of archives that are gone or changed, and interrupted extractions
    std::vector<fs::path> stale;
    for (auto it = fs::directory_iterator(cache, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !used.count(it->path().filename().string())) stale.push_back(it->path());
    }
    for (const fs::path& s : stale) {
        fs::remove_all(s, ec);
        ++rep.purged;
        say("color_mods: cache " + s.filename().string() + " removed (archive no longer installed or changed)");
    }
    return rep;
}

void start_worker(const fs::path& root, void (*log)(const std::string&)) {
    if (g_started.exchange(true)) return;
    std::thread([root, log] {
        try {
            ensure_layout(root);
            const SyncReport r = sync(root, log);
            if (log && (r.extracted || r.failed || r.purged))
                log("color_mods: " + std::to_string(r.extracted) + " extracted, " + std::to_string(r.already) + " cached, " +
                    std::to_string(r.failed) + " unusable, " + std::to_string(r.purged) + " cache folders removed");
            if (r.changed()) g_changed.store(true, std::memory_order_release);
        } catch (...) {
            if (log) log("color_mods: worker failed (exception)");
        }
    }).detach();
}

bool take_changed() { return g_changed.exchange(false, std::memory_order_acq_rel); }

}  // namespace engine::color_mods
