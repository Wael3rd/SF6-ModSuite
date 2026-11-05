#include "color_mods.hpp"
#include "live_internal.hpp"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "archive.hpp"
#include "catalog.hpp"

namespace engine::color_mods {
namespace fs = std::filesystem;

namespace {

constexpr const char* kCacheDir = ".cache";
constexpr const char* kComplete = ".complete";

// Same list and spelling as the costume_mods folders (Windows drops a trailing dot: "A.K.I"), with the esf number.
struct Character { int id; const char* name; };
const Character kCharacters[] = {
    { 1, "Ryu" }, { 2, "Luke" }, { 3, "Kimberly" }, { 4, "Chun-Li" }, { 5, "Manon" }, { 6, "Zangief" }, { 7, "JP" },
    { 8, "Dhalsim" }, { 9, "Cammy" }, { 10, "Ken" }, { 11, "Dee Jay" }, { 12, "Lily" }, { 13, "A.K.I" }, { 14, "Rashid" },
    { 15, "Blanka" }, { 16, "Juri" }, { 17, "Marisa" }, { 18, "Guile" }, { 19, "Ed" }, { 20, "E. Honda" }, { 21, "Jamie" },
    { 22, "Akuma" }, { 25, "Sagat" }, { 26, "M. Bison" }, { 27, "Terry" }, { 28, "Mai" }, { 29, "Elena" },
    { 30, "C. Viper" }, { 31, "Alex" }, { 32, "Ingrid" }, { 33, "Yasmine" }, { 34, "Arjun" },
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

Colours from this folder are listed as RW n. The colour editor can edit them (SAVE keeps your
change apart, RESET goes back to the original) but never changes or deletes a mod's file.
The colours you make in the editor (SAVE AS, listed as MC n) are saved here too, in
<Character>/MANUALCOLORS_01, _02...; ERASE deletes them.
Colours of older versions (reframework/data/SF6_ColorSpinExtra_data/colors) were moved here
by the Color Mod; that folder is no longer read.
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
    for (const Character& c : kCharacters) fs::create_directories(root / c.name, ec);
    const fs::path readme = root / "README.txt";
    std::string t = readme_text(), crlf;
    for (char c : t) { if (c == '\n') crlf += '\r'; crlf += c; }
    std::ifstream in(readme, std::ios::binary);
    const std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (cur != crlf) {                                    // missing, or the text of an older version
        std::ofstream f(readme, std::ios::binary | std::ios::trunc);
        if (f) f << crlf;
    }
}

std::string character_folder(int fighter_id) {
    for (const Character& c : kCharacters) if (c.id == fighter_id) return c.name;
    return {};
}

namespace {

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool write_text(const fs::path& p, const std::string& s) {      // through a temporary file and a rename
    std::error_code ec;
    const fs::path tmp = fs::path(p.wstring() + L".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(s.data(), (std::streamsize)s.size());
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    live::files_changed();
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    return read_text(a) == read_text(b);
}

// "a\\b" as it is written inside a JSON string
std::string json_escaped(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '\\') out += "\\\\"; else out += c; }
    return out;
}

// Every quoted occurrence of an old path (as JSON writes it, or with forward slashes) -> the new path.
int replace_paths(std::string& text, const std::vector<std::pair<std::string, std::string>>& moves) {
    int n = 0;
    for (const auto& [from, to] : moves) {
        std::string fwd = from;
        for (char& c : fwd) if (c == '\\') c = '/';
        for (const std::string& f : { json_escaped(from), fwd }) {
            const std::string needle = "\"" + f + "\"", repl = "\"" + json_escaped(to) + "\"";
            for (size_t i = text.find(needle); i != std::string::npos; i = text.find(needle, i + repl.size())) {
                text.replace(i, needle.size(), repl);
                ++n;
            }
        }
    }
    return n;
}

// The override key of a colour file at `rel` (catalog.cpp: "<fighter>_<costume id>_file_<sanitised identity>").
std::string override_key(const std::string& rel, const std::string& fname) {
    if (!is_colour_name(fname)) return {};
    const int f = std::atoi(fname.substr(3, 3).c_str()), c = std::atoi(fname.substr(7, 3).c_str());
    return std::to_string(f) + "_" + std::to_string(c - 1) + "_file_" + Catalog::sanitize(Catalog::ident_of(rel));
}

}  // namespace

MigrateReport migrate_legacy(const fs::path& data_dir, void (*log)(const std::string&)) {
    MigrateReport rep;
    auto say = [&](const std::string& s) { if (log) log(s); };
    std::error_code ec;
    const fs::path legacy = data_dir / "SF6_ColorSpinExtra_data" / "colors";
    if (!fs::is_directory(legacy, ec)) return rep;
    const fs::path root = root_of(data_dir);
    const fs::path overrides = data_dir / "SF6_ColorSpinExtra_data" / "overrides";
    std::vector<std::pair<std::string, std::string>> moves;           // relative to data_dir, backslashes
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(legacy, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { say("color_mods: former colour folder not fully read (" + ec.message() + "): the rest moves at a later start"); break; }
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    for (const fs::path& src : files) {
        const fs::path rel_in = fs::relative(src, legacy, ec);         // <Name>_esfNNN\<pack>\...\file
        auto part = rel_in.begin();
        if (part == rel_in.end()) continue;
        const std::string top = part->string();
        if (std::next(part) == rel_in.end()) {                           // a file directly in colors\: kept at the top
            if (lower(top) == "readme.txt") { fs::remove(src, ec); continue; }
        } else if (lower(src.filename().string()) == "readme.txt" && std::next(std::next(part)) == rel_in.end()) {
            fs::remove(src, ec);                                         // <Name>_esfNNN\README.txt: the old drop note
            continue;
        }
        // <Name>_esfNNN -> the color_mods folder of fighter NNN (else <Name>, as it was)
        fs::path dest_dir = root;
        fs::path rest;
        if (std::next(part) != rel_in.end()) {
            std::string folder = top;
            const size_t at = lower(top).rfind("_esf");
            if (at != std::string::npos && top.size() == at + 7) {
                const std::string name = character_folder(std::atoi(top.substr(at + 4).c_str()));
                folder = name.empty() ? top.substr(0, at) : name;
            }
            dest_dir = root / folder;
            for (auto p = std::next(part); p != rel_in.end(); ++p) rest /= *p;
        } else {
            rest = rel_in;
        }
        fs::path dest = dest_dir / rest;
        if (has(dest)) {
            if (same_bytes(src, dest)) {                                 // already there (an earlier, cut-short run)
                fs::remove(src, ec);
                ++rep.duplicates;
            } else {
                dest = dest_dir / "_from_old_colors_folder" / rest;      // never overwrite a file of color_mods
            }
        }
        if (has(src)) {
            fs::create_directories(dest.parent_path(), ec);
            fs::rename(src, dest, ec);
            live::files_changed();
            if (ec) { say("color_mods: cannot move " + src.string() + ": " + ec.message()); ++rep.failed; continue; }
            ++rep.moved;
        }
        std::string from = fs::relative(src, data_dir, ec).string(), to = "..\\color_mods\\" + fs::relative(dest, root, ec).string();
        for (char& c : from) if (c == '/') c = '\\';
        for (char& c : to) if (c == '/') c = '\\';
        moves.emplace_back(from, to);
        // an edited copy of that colour: its file name carries the colour's path
        const std::string fname = dest.filename().string(), ok_from = override_key(from, fname), ok_to = override_key(to, fname);
        if (!ok_from.empty() && ok_from != ok_to) {
            const fs::path a = overrides / (ok_from + ".user.2"), b = overrides / (ok_to + ".user.2");
            if (has(a) && !has(b)) { fs::rename(a, b, ec); live::files_changed(); if (!ec) ++rep.renamed_overrides; }
        }
    }
    // what names a colour by its path: the remembered selections, the squares of the editor's colours
    for (const char* name : { "state.json", "chip_slots.json" }) {
        const fs::path p = data_dir / "SF6_ColorSpinExtra_data" / name;
        if (!has(p)) continue;
        std::string text = read_text(p);
        const int n = replace_paths(text, moves);
        if (n > 0 && !write_text(p, text)) say(std::string("color_mods: cannot rewrite ") + name + " after the move");
    }
    // the former folder goes once nothing is left in it (empty folders only)
    size_t left = 0;
    std::error_code wec;
    for (auto it = fs::recursive_directory_iterator(legacy, fs::directory_options::skip_permission_denied, wec);
         it != fs::recursive_directory_iterator(); it.increment(wec)) {
        if (wec) break;
        if (!it->is_directory(wec)) ++left;
    }
    if (left == 0 && !wec) { fs::remove_all(legacy, ec); rep.removed = !has(legacy); }
    say("color_mods: former colour folder SF6_ColorSpinExtra_data\\colors: " + std::to_string(rep.moved) + " files moved to color_mods, " +
        std::to_string(rep.duplicates) + " already there, " + std::to_string(rep.failed) + " failed, " +
        std::to_string(rep.renamed_overrides) + " edited copies renamed; folder " + (rep.removed ? "removed" : "KEPT (files left in it)"));
    return rep;
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
        live::files_changed();
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
