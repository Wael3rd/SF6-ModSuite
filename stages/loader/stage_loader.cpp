// Stage slots: startup pass. See stage_loader.hpp.

#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "archive.hpp"
#include "pak.hpp"
#include "patch.hpp"          // texture_mip_check, shared with the costume loader
#include "preview_tex.hpp"
#include "stage_classify.hpp"
#include "stage_loader.hpp"
#include "stage_log.hpp"
#include "title_slots.hpp"

namespace {

const char* STAGE_MARKER = STAGE_SLOTS_MARKER;
const char* COSTUME_MARKER = COSTUME_SLOTS_MARKER;
// Part of the fingerprint: bump it when the pak layout, the preview conversion or the checks of the
// mods' files change. 5: GUI files kept out of the redirection. 7: tile / variation guess and banner source in the table,
// previews from the mod's own menu pictures first.
const char* FORMAT_VERSION = "stageslots-7";
// The game's menu pictures (stage select image and thumbnail, arcade images, their prefabs...)
const char* GUI_PREFIX = "natives/stm/product/gui/";
const char* TEX_SUFFIX = ".tex.241101895";

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::wstring lower_w(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
std::string lower_a(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }

bool ends_with(const std::wstring& s, const wchar_t* suf) {
    size_t n = wcslen(suf);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

uint64_t fnv64(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string hex16(uint64_t v) {
    char b[17];
    sprintf_s(b, "%016llx", (unsigned long long)v);
    return b;
}

// Mod trees go deep (natives\stm\product\environment\props\resource\sm0x\...): under the archive
// cache their paths pass MAX_PATH, so file calls take the \\?\ form of long absolute paths.
std::wstring lp(const std::wstring& p) {
    if (p.size() < MAX_PATH - 12 || p.compare(0, 4, L"\\\\?\\") == 0 || p.size() < 3 || p[1] != L':') return p;
    std::wstring q = L"\\\\?\\" + p;
    for (auto& c : q) if (c == L'/') c = L'\\';
    return q;
}

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(lp(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(lp(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring leaf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring stem(const std::wstring& name) {
    size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos || d == 0 ? name : name.substr(0, d);
}

bool read_file(const std::wstring& p, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, lp(p).c_str(), L"rb") != 0 || !f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// Written next to the target then moved over it: a reader never sees half a file.
bool write_text(const std::wstring& p, const std::string& text) {
    std::wstring tmp = p + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

struct DirEntry { std::wstring name; bool dir; uint64_t size; uint64_t mtime; };

std::vector<DirEntry> list_dir(const std::wstring& dir) {
    std::vector<DirEntry> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(lp(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        out.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

bool delete_with_retry(const std::wstring& p) {
    for (int i = 0; i < 20; ++i) {
        if (DeleteFileW(p.c_str())) return true;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        Sleep(500);
    }
    return false;
}

// PakWriter takes a narrow path: the short form of the folder keeps it ASCII.
std::string ascii_path(const std::wstring& dir, const std::wstring& file) {
    wchar_t sh[MAX_PATH * 2];
    DWORD n = GetShortPathNameW(dir.c_str(), sh, MAX_PATH * 2);
    std::wstring d = (n > 0 && n < MAX_PATH * 2) ? std::wstring(sh) : dir;
    std::wstring full = d + L"\\" + file;
    char buf[MAX_PATH * 4];
    int m = WideCharToMultiByte(CP_ACP, 0, full.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return m > 0 ? std::string(buf) : std::string();
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string tsv_clean(std::string s) {
    for (auto& c : s) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

// ---------------------------------------------------------------------------------------------
// KPKA index (wide paths; entries are copied raw, never decompressed)
// ---------------------------------------------------------------------------------------------

struct KpkaEntry { uint64_t hash; int64_t offset, csize, dsize, attrib; };

bool kpka_index(FILE* f, std::vector<KpkaEntry>& out) {
    #pragma pack(push, 1)
    struct Header { uint32_t magic; uint8_t major, minor; int16_t feature; uint32_t count, fingerprint; } h;
    struct Entry { uint32_t lo, hi; int64_t offset, csize, dsize, attrib, checksum; };
    #pragma pack(pop)
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) return false;
    if (h.magic != 0x414B504B || h.major != 4 || h.feature != 0 || h.count > 4000000) return false;
    std::vector<Entry> raw(h.count);
    if (h.count && fread(raw.data(), sizeof(Entry), h.count, f) != h.count) return false;
    out.clear();
    out.reserve(h.count);
    for (auto& e : raw) out.push_back({ ((uint64_t)e.hi << 32) | e.lo, e.offset, e.csize, e.dsize, e.attrib });
    return true;
}

// ---------------------------------------------------------------------------------------------
// The game's patch paks: mods, ours, the costume loader's
// ---------------------------------------------------------------------------------------------

enum class PakKind { Mod, Stage, Costume };
struct PatchPak { int num; std::wstring path; PakKind kind; size_t entries; };

std::wstring patch_path(const std::wstring& game_dir, int num) {
    wchar_t b[64];
    swprintf_s(b, L"re_chunk_000.pak.patch_%03d.pak", num);
    return game_dir + L"\\" + b;
}

std::vector<PatchPak> classify_patch_paks(const std::wstring& game_dir) {
    std::vector<PatchPak> out;
    const uint64_t stage_h = pak_path_hash(std::string_view(STAGE_MARKER));
    const uint64_t costume_h = pak_path_hash(std::string_view(COSTUME_MARKER));
    for (auto& e : list_dir(game_dir)) {
        if (e.dir || _wcsnicmp(e.name.c_str(), L"re_chunk_000.pak.patch_", 23) != 0 || !ends_with(e.name, L".pak")) continue;
        int num = _wtoi(e.name.c_str() + 23);
        if (num <= 0) continue;
        std::wstring p = game_dir + L"\\" + e.name;
        PakKind kind = PakKind::Mod;
        size_t entries = 0;
        FILE* f = nullptr;
        if (_wfopen_s(&f, p.c_str(), L"rb") == 0 && f) {
            std::vector<KpkaEntry> idx;
            if (kpka_index(f, idx)) {
                entries = idx.size();
                for (auto& k : idx) {
                    if (k.hash == stage_h) { kind = PakKind::Stage; break; }
                    if (k.hash == costume_h) { kind = PakKind::Costume; break; }
                }
            }
            fclose(f);
        }
        out.push_back({ num, p, kind, entries });
    }
    std::sort(out.begin(), out.end(), [](const PatchPak& a, const PatchPak& b) { return a.num < b.num; });
    return out;
}

// ---------------------------------------------------------------------------------------------
// Stage index: every vanilla path that belongs to a stage, hashed -> its stage code
// ---------------------------------------------------------------------------------------------

bool ess_code_in(const std::string& p, std::string& code) {
    for (size_t i = 0; i + 10 <= p.size(); ++i) {
        if (p[i] != 'e' || p[i + 1] != 's' || p[i + 2] != 's') continue;
        bool ok = true;
        for (int k = 3; k < 7; ++k) ok = ok && isdigit((unsigned char)p[i + k]);
        ok = ok && p[i + 7] == '_' && isdigit((unsigned char)p[i + 8]) && isdigit((unsigned char)p[i + 9]);
        if (ok) { code = p.substr(i, 10); return true; }
    }
    return false;
}

uint32_t stage_id_of(const std::string& ess) {
    return (uint32_t)atoi(ess.substr(3, 4).c_str()) * 100 + (uint32_t)atoi(ess.substr(8, 2).c_str());
}

// gui: the paths of the index under GUI_PREFIX, by hash (a pak entry has no path of its own); paths: every path, by hash
// (lower case: the tile / variation guess of a pak's entries)
bool load_stage_index(const std::wstring& path, std::unordered_map<uint64_t, std::string>& out,
                      std::unordered_map<uint64_t, std::string>& gui, std::unordered_map<uint64_t, std::string>& paths) {
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    std::string text(data.begin(), data.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string code;
        if (line.empty() || !ess_code_in(line, code)) continue;
        const uint64_t h = pak_path_hash(std::string_view(line));
        out[h] = code;
        paths[h] = lower_a(line);
        if (lower_a(line).compare(0, strlen(GUI_PREFIX), GUI_PREFIX) == 0) gui[h] = line;
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------------------------
// Mods: sources (what sits in stage_mods) and options (what the game would install)
// ---------------------------------------------------------------------------------------------

// path: natives trees only; file: where it is on disk (natives trees only), to read it later when listed without its data
struct OptFile { uint64_t hash; std::vector<uint8_t> data; int64_t attrib; int64_t dsize; std::string path; std::wstring file; };
struct Option {
    std::string name, author, bundle, rel;     // bundle: Fluffy's nameAsBundle
    std::wstring screenshot;
    std::vector<OptFile> files;
};

bool is_image(const std::wstring& n) {
    return ends_with(n, L".png") || ends_with(n, L".jpg") || ends_with(n, L".jpeg");
}

bool archive_keep(const std::wstring& entry) {
    std::wstring l = lower_w(entry);
    return l.find(L"natives/") != std::wstring::npos || ends_with(l, L".pak") || ends_with(l, L".ini") || is_image(l);
}

void read_modinfo(const std::wstring& dir, std::string& name, std::string& author, std::string& bundle,
                  std::wstring& screenshot) {
    std::vector<uint8_t> data;
    if (!read_file(dir + L"\\modinfo.ini", data)) return;
    std::string text(data.begin(), data.end());
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = lower_a(line.substr(0, eq)), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        if (k == "name" && !v.empty()) name = v;
        else if (k == "author") author = v;
        else if (k == "nameasbundle") bundle = v;
        else if (k == "screenshot" && !v.empty()) screenshot = dir + L"\\" + widen(v);
    }
}

// read: false lists the files without reading them (dsize is the size on disk; title packs: only the chosen one is read)
void add_natives(const std::wstring& dir, const std::string& rel, Option& opt, bool read = true) {
    for (auto& e : list_dir(dir)) {
        std::wstring full = dir + L"\\" + e.name;
        std::string r = rel + "/" + lower_a(narrow(e.name));
        if (e.dir) { add_natives(full, r, opt, read); continue; }
        OptFile f;
        if (read && !read_file(full, f.data)) { slog("  cannot read %s", narrow(full).c_str()); continue; }
        f.hash = pak_path_hash(std::string_view(r));
        f.attrib = 0;
        f.dsize = read ? (int64_t)f.data.size() : (int64_t)e.size;
        f.path = r;
        f.file = full;
        opt.files.push_back(std::move(f));
    }
}

void add_pak(const std::wstring& path, Option& opt) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, lp(path).c_str(), L"rb") != 0 || !f) { slog("  cannot open %s", narrow(path).c_str()); return; }
    std::vector<KpkaEntry> idx;
    if (!kpka_index(f, idx)) { slog("  not a readable pak: %s", narrow(path).c_str()); fclose(f); return; }
    for (auto& k : idx) {
        OptFile of;
        of.hash = k.hash;
        of.attrib = k.attrib;
        of.dsize = k.dsize;
        of.data.resize((size_t)k.csize);
        if (_fseeki64(f, k.offset, SEEK_SET) != 0 || fread(of.data.data(), 1, of.data.size(), f) != of.data.size()) {
            slog("  truncated entry in %s", narrow(path).c_str());
            continue;
        }
        opt.files.push_back(std::move(of));
    }
    fclose(f);
}

// A folder holding a .pak or a natives tree is one option, as Fluffy Mod Manager installs it;
// otherwise its sub-folders are looked at (bundles: one option per sub-folder).
// read: false (title packs) lists the natives files without reading them and leaves .pak files out: a title pack's
// files are told apart by their paths, which a pak's entries do not have.
void collect_options(const std::wstring& dir, const std::string& rel, const std::string& default_name,
                     bool is_root, int depth, std::vector<Option>& out, bool read = true) {
    auto entries = list_dir(dir);
    bool has_pak = false, has_natives = false;
    for (auto& e : entries) {
        if (read && !e.dir && ends_with(e.name, L".pak")) has_pak = true;
        if (e.dir && _wcsicmp(e.name.c_str(), L"natives") == 0) has_natives = true;
    }
    if (has_pak || has_natives) {
        Option opt;
        opt.rel = rel;
        read_modinfo(dir, opt.name, opt.author, opt.bundle, opt.screenshot);
        if (opt.name.empty()) opt.name = is_root ? default_name : narrow(leaf(dir));
        if (!opt.screenshot.empty() && !file_exists(opt.screenshot)) opt.screenshot.clear();
        for (auto& e : entries) {
            if (opt.screenshot.empty() && !e.dir && is_image(e.name)) opt.screenshot = dir + L"\\" + e.name;
            if (read && !e.dir && ends_with(e.name, L".pak")) add_pak(dir + L"\\" + e.name, opt);
            if (e.dir && _wcsicmp(e.name.c_str(), L"natives") == 0) add_natives(dir + L"\\" + e.name, "natives", opt, read);
        }
        out.push_back(std::move(opt));
        return;
    }
    if (depth >= 6) return;
    for (auto& e : entries) {
        if (!e.dir || e.name[0] == L'.') continue;
        collect_options(dir + L"\\" + e.name, rel + "/" + narrow(e.name), default_name, false, depth + 1, out, read);
    }
}

// A texture whose mip table is inconsistent never finishes loading, and every screen that shows it
// waits for it forever: Stage Lighting Overhaul's Training Room previews (1920x480, 9 levels, the
// last three declared with fractional-block row pitches) hung the game on its way to the Fighting
// Ground menu, which loads Training Room's pictures, whenever one of them was selected (reproduced,
// then gone with the repair, 2026-10-02). Repaired the way the costume loader
// repairs costume textures; a texture that stays inconsistent is left out of the option, so the
// game's own file is served in its place.
void check_textures(Option& opt) {
    int repaired = 0, dropped = 0;
    for (size_t i = 0; i < opt.files.size();) {
        OptFile& f = opt.files[i];
        const int comp = int(f.attrib & 0xFF);
        // A stored file that is not a texture is not copied (a title pack's movies are tens of MB)
        if (!comp && (f.data.size() < 4 || memcmp(f.data.data(), "TEX\0", 4) != 0)) { ++i; continue; }
        std::vector<uint8_t> plain = comp ? pak_decompress(f.data, comp, f.dsize) : f.data;
        if (plain.size() < 4 || memcmp(plain.data(), "TEX\0", 4) != 0) { ++i; continue; }
        int fixed = 0;
        int bad = texture_mip_check(plain, true, &fixed);
        if (bad) {
            opt.files.erase(opt.files.begin() + i);
            ++dropped;
            continue;
        }
        if (fixed) {
            f.dsize = (int64_t)plain.size();
            f.data = std::move(plain);
            f.attrib = 0;
            ++repaired;
        }
        ++i;
    }
    if (repaired) slog("  %s: %d texture(s) with a wrong mip table, repaired", opt.rel.c_str(), repaired);
    if (dropped) slog("  %s: %d texture(s) with a mip table that cannot be repaired, left out (the game's own is used)",
                      opt.rel.c_str(), dropped);
}

struct Source { std::wstring name, path; bool dir; ArchiveKind archive; uint64_t size, mtime; };

std::vector<Source> list_sources(const std::wstring& mods_dir) {
    std::vector<Source> out;
    for (auto& e : list_dir(mods_dir)) {
        if (e.name[0] == L'.') continue;
        Source s{ e.name, mods_dir + L"\\" + e.name, e.dir, ArchiveKind::None, e.size, e.mtime };
        if (!e.dir) {
            s.archive = archive_sniff(s.path);
            if (s.archive == ArchiveKind::None && !ends_with(e.name, L".pak")) continue;   // notes, images...
        }
        out.push_back(s);
    }
    return out;
}

void fingerprint_dir(const std::wstring& dir, const std::string& rel, std::string& acc) {
    for (auto& e : list_dir(dir)) {
        std::string r = rel + "/" + narrow(e.name);
        if (e.dir) fingerprint_dir(dir + L"\\" + e.name, r, acc);
        else acc += r + "|" + std::to_string(e.size) + "|" + std::to_string(e.mtime) + "\n";
    }
}

// title: the title packs' part (scan_titles), "" without title_mods: the fingerprint is then the one it always was
std::string compute_fingerprint(const std::vector<Source>& sources, const std::wstring& index_path, const std::string& title) {
    std::string acc = FORMAT_VERSION;
    acc += "\n";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(index_path.c_str(), GetFileExInfoStandard, &fa))
        acc += "index|" + std::to_string(fa.nFileSizeLow) + "|" + std::to_string(fa.ftLastWriteTime.dwLowDateTime) + "\n";
    for (auto& s : sources) {
        std::string n = narrow(s.name);
        if (s.dir) fingerprint_dir(s.path, n, acc);
        else acc += n + "|" + std::to_string(s.size) + "|" + std::to_string(s.mtime) + "\n";
    }
    acc += title;
    return hex16(fnv64(acc));
}

// Archives are unpacked once into stage_mods\.cache\<key>\, reused while the archive is unchanged.
bool unpack_archive(const Source& s, const std::wstring& cache_root, std::wstring& out_dir) {
    std::string key = hex16(fnv64(narrow(s.name) + "|" + std::to_string(s.size) + "|" + std::to_string(s.mtime)));
    out_dir = cache_root + L"\\" + widen(key);
    std::wstring done = out_dir + L"\\.complete";
    if (file_exists(done)) return true;
    CreateDirectoryW(cache_root.c_str(), nullptr);
    SetFileAttributesW(cache_root.c_str(), FILE_ATTRIBUTE_HIDDEN);
    CreateDirectoryW(out_dir.c_str(), nullptr);
    ArchiveResult r = archive_extract(s.path, out_dir, archive_keep);
    if (!r.ok) { slog("  %s: cannot unpack (%s)", narrow(s.name).c_str(), r.error.c_str()); return false; }
    slog("  %s: unpacked %d files (%.1f MB)", narrow(s.name).c_str(), r.files_written, r.bytes_written / 1048576.0);
    write_text(done, "ok\n");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Saved table (variants.tsv)
// ---------------------------------------------------------------------------------------------

bool load_table(const std::wstring& path, std::string& fingerprint, size_t& pak_entries, std::vector<StageVariant>& out) {
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    std::string text(data.begin(), data.end());
    std::unordered_map<std::string, size_t> by_key;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) {
            size_t t = line.find('\t', a);
            f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
            if (t == std::string::npos) break;
            a = t + 1;
        }
        if (f[0] == "#fingerprint" && f.size() >= 2) fingerprint = f[1];
        else if (f[0] == "#pak" && f.size() >= 2) pak_entries = static_cast<size_t>(strtoull(f[1].c_str(), nullptr, 10));
        else if (f[0] == "V" && f.size() >= 8) {
            StageVariant v;
            v.key = f[1];
            v.stage_id = (uint32_t)strtoul(f[2].c_str(), nullptr, 10);
            v.ess = f[3];
            v.has_preview = f[4] == "1";
            v.name = f[5];
            v.author = f[6];
            v.source = f[7];
            if (f.size() >= 9) v.bundle = f[8];
            if (f.size() >= 10) v.has_thumb = f[9] == "1";
            if (f.size() >= 11) v.tile_auto = f[10] == "tile";
            if (f.size() >= 12) v.reason_auto = f[11];
            if (f.size() >= 13) v.banner = f[12];
            by_key[v.key] = out.size();
            out.push_back(v);
        } else if (f[0] == "R" && f.size() >= 4) {
            auto it = by_key.find(f[1]);
            if (it == by_key.end()) continue;
            out[it->second].redirects.push_back({ strtoull(f[2].c_str(), nullptr, 16), strtoull(f[3].c_str(), nullptr, 16) });
        }
    }
    return !fingerprint.empty();
}

// variants.tsv: the build's result (the mod's own names, the automatic tile / variation guess)
bool save_table(const std::wstring& data_dir, const std::string& fingerprint, size_t pak_entries, const std::vector<StageVariant>& vars) {
    std::string tsv = "#fingerprint\t" + fingerprint + "\n#pak\t" + std::to_string(pak_entries) + "\n";
    for (auto& v : vars) {
        tsv += "V\t" + v.key + "\t" + std::to_string(v.stage_id) + "\t" + v.ess + "\t" + (v.has_preview ? "1" : "0") + "\t" +
               tsv_clean(v.mod_name.empty() ? v.name : v.mod_name) + "\t" + tsv_clean(v.author) + "\t" + tsv_clean(v.source) + "\t" +
               tsv_clean(v.bundle) + "\t" + (v.has_thumb ? "1" : "0") + "\t" + (v.tile_auto ? "tile" : "variation") + "\t" +
               tsv_clean(v.reason_auto) + "\t" + tsv_clean(v.banner) + "\n";
        for (auto& r : v.redirects) tsv += "R\t" + v.key + "\t" + hex16(r.first) + "\t" + hex16(r.second) + "\n";
    }
    return write_text(data_dir + L"\\loader\\variants.tsv", tsv);
}

// stage_slots.ini on the variants (tile / variation, name): the automatic guess unless a section names the variant. The
// template is written when the file does not exist.
void apply_overrides(const std::wstring& mods_dir, std::vector<StageVariant>& vars) {
    const std::wstring ini = mods_dir + L"\\stage_slots.ini";
    std::vector<uint8_t> data;
    if (!read_file(ini, data)) {
        if (write_text(ini, kStageOverridesTemplate)) slog("  wrote stage_slots.ini (template: tile / variation and names per mod)");
    }
    std::vector<std::string> bad;
    const auto list = parse_stage_overrides(std::string(data.begin(), data.end()), &bad);
    for (auto& b : bad) slog("  stage_slots.ini: line not understood, ignored: %s", b.c_str());
    int tiles = 0, changed = 0;
    for (auto& v : vars) {
        if (!v.mod_name.empty()) { v.name = v.mod_name; v.mod_name.clear(); }
        v.tile = v.tile_auto;
        v.kind_reason = v.reason_auto;
        if (const StageOverride* o = find_stage_override(list, v.key, v.name, v.source)) {
            if (!o->kind.empty() && (o->kind == "tile") != v.tile_auto) {
                v.tile = o->kind == "tile";
                v.kind_reason = "stage_slots.ini [" + o->section + "] (guess: " + (v.tile_auto ? "tile, " : "variation, ") + v.reason_auto + ")";
                ++changed;
            }
            if (!o->name.empty() && o->name != v.name) {
                v.mod_name = v.name;
                v.name = o->name;
                ++changed;
            }
        }
        tiles += v.tile ? 1 : 0;
    }
    slog("  %d tile(s), %zu variation(s)%s", tiles, vars.size() - tiles,
         changed ? (", " + std::to_string(changed) + " change(s) from stage_slots.ini").c_str() : "");
}

// registry.json: what the plugin shows (stage_slots.ini applied). Written only when its text changes.
bool save_registry(const std::wstring& data_dir, const std::string& fingerprint, const std::vector<StageVariant>& vars) {
    std::map<uint32_t, std::vector<const StageVariant*>> by_stage;
    for (auto& v : vars) by_stage[v.stage_id].push_back(&v);
    std::string js = "{\n  \"version\": 1,\n  \"fingerprint\": \"" + fingerprint + "\",\n  \"stages\": [";
    bool first_stage = true;
    for (auto& [id, list] : by_stage) {
        js += first_stage ? "\n" : ",\n";
        first_stage = false;
        js += "    { \"stage_id\": " + std::to_string(id) + ", \"ess\": \"" + list[0]->ess + "\", \"variants\": [";
        for (size_t i = 0; i < list.size(); ++i) {
            auto* v = list[i];
            js += i ? ",\n" : "\n";
            js += "      { \"key\": \"" + v->key + "\", \"name\": \"" + json_escape(v->name) + "\", \"author\": \"" +
                  json_escape(v->author) + "\", \"bundle\": \"" + json_escape(v->bundle) + "\", \"source\": \"" + json_escape(v->source) + "\", \"preview\": \"" +
                  (v->has_preview ? stage_preview_resource(v->key) : std::string()) + "\", \"thumb\": \"" +
                  (v->has_thumb ? stage_thumb_resource(v->key) : std::string()) + "\", \"files\": " +
                  std::to_string(v->redirects.size()) + ", \"kind\": \"" + (v->tile ? "tile" : "variation") +
                  "\", \"kind_reason\": \"" + json_escape(v->kind_reason) + "\", \"banner\": \"" + json_escape(v->banner) +
                  "\", \"mod_name\": \"" + json_escape(v->mod_name) + "\" }";
        }
        js += "\n    ] }";
    }
    js += "\n  ]\n}\n";
    const std::wstring path = data_dir + L"\\registry.json";
    std::vector<uint8_t> old;
    if (read_file(path, old) && std::string(old.begin(), old.end()) == js) return true;
    return write_text(path, js);
}

// ---------------------------------------------------------------------------------------------
// Taking a pak of ours out of the row of patch paks
// ---------------------------------------------------------------------------------------------

// The game reads the patch paks as an unbroken row from patch_001 (a hole = black screen). A pak of
// ours that sits below a mod's pak is therefore not deleted but replaced in place by a pak holding
// only the marker (a placeholder, like the costume loader's); it goes for good once no mod's pak is
// above it. True once done; `stub` tells which.
bool retire_pak(const std::wstring& game_dir, const PatchPak& p, int max_mod, bool& stub) {
    stub = false;
    if (p.num > max_mod) return delete_with_retry(p.path);
    const std::wstring tmp = p.path + L".tmp";
    const std::string ap = ascii_path(game_dir, leaf(tmp));
    if (ap.empty() || !write_marker_pak(ap.c_str(), PatchPakKind::Stage)) { DeleteFileW(tmp.c_str()); return false; }
    for (int i = 0; i < 20; ++i) {
        if (MoveFileExW(tmp.c_str(), p.path.c_str(), MOVEFILE_REPLACE_EXISTING)) { stub = true; return true; }
        Sleep(500);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// ---------------------------------------------------------------------------------------------
// Title screen packs (title_slots.hpp): listed at every start; the chosen one's files go into the stage pak at their
// real paths (no redirection: the title screen and the boot screens are loaded once, before any menu). Every pack's
// picture (its modinfo screenshot) goes in too, for the plugin's preview on the Options screen.
// ---------------------------------------------------------------------------------------------

// Part of the title packs' fingerprint: bump it when what is written of a pack changes.
const char* TITLE_FORMAT_VERSION = "titleslots-10";      // 2: the packs' pictures; 3: at 1920x1080, and the game's own; 4: Random's mosaic;
                                                        // 5: every background's title files at private paths (the live switch);
                                                        // 6: streamed copies under natives/stm/streaming/_titleslots (the core's playlist);
                                                        // 7: their resource files beside them

struct TitlePack { title_slots::Pack info; Option opt; };   // opt: its natives files, listed, not read

struct TitleScan {
    bool present = false;              // reframework\title_mods exists: nothing is done without it
    std::vector<TitlePack> packs;      // registry order
    title_slots::Settings settings;
    title_slots::Choice choice;
    std::string fingerprint;           // added to the stage pak's ("" without title_mods)
    std::wstring game_picture;         // SF6_TitleSlots_Data\game_title.*: the game's own title's picture ("": none)
    std::wstring bag_path;             // SF6_TitleSlots_Data\shuffle.json: Random's bag
    bool game_preview = false;         // that picture is in the pak
    bool random_preview = false;       // Random's picture (the mosaic of the packs' pictures) is in the pak
};

const std::string& title_key(const TitleScan& t, int i) {
    static const std::string none;
    return i >= 0 ? t.packs[size_t(i)].info.key : none;
}

const char* title_name(const TitleScan& t, int i) {
    return i >= 0 ? t.packs[size_t(i)].info.name.c_str() : "";
}

// The packs of title_mods (archives unpacked once into title_mods\.cache, like stage mods), the settings and the choice.
TitleScan scan_titles(const std::wstring& game_dir) {
    TitleScan t;
    const std::wstring dir = game_dir + L"\\reframework\\title_mods";
    if (!dir_exists(dir)) return t;
    t.present = true;
    const auto sources = list_sources(dir);
    std::vector<Option> options;
    for (auto& s : sources) {
        const std::string n = narrow(s.name);
        if (s.dir) {
            collect_options(s.path, n, n, true, 0, options, false);
        } else if (s.archive != ArchiveKind::None) {
            std::wstring out;
            if (unpack_archive(s, dir + L"\\.cache", out)) collect_options(out, n, narrow(stem(s.name)), true, 0, options, false);
        } else {
            slog("  title_mods: %s: a .pak is not read as a title pack, ignored", n.c_str());
        }
    }
    for (auto& o : options) {
        if (o.files.empty()) {
            slog("  title_mods: %s: no file in a natives tree (a .pak inside is not read), ignored", o.rel.c_str());
            continue;
        }
        TitlePack p;
        p.info.key = title_slots::pack_key(o.rel);
        p.info.name = o.name;
        p.info.author = o.author;
        p.info.source = o.rel;
        p.info.preview = !o.screenshot.empty() && image_header_ok(lp(o.screenshot));   // the build may still find it unreadable
        for (auto& f : o.files) ++(title_slots::is_title_file(f.path) ? p.info.title_files : p.info.startup_files);
        p.opt = std::move(o);
        t.packs.push_back(std::move(p));
    }
    std::stable_sort(t.packs.begin(), t.packs.end(),
                     [](const TitlePack& a, const TitlePack& b) { return title_slots::pack_before(a.info, b.info); });
    // Random's mosaic is made from the packs' pictures: there is one when a pack has a picture (the build says so again, and a pak
    // up to date was built with the same answer: the registry written then and now agree)
    t.random_preview = std::any_of(t.packs.begin(), t.packs.end(), [](const TitlePack& p) { return p.info.preview; });

    std::vector<uint8_t> data;
    const bool have_settings = read_file(game_dir + L"\\reframework\\data\\SF6_CostumeSlots_data\\settings.json", data);
    if (have_settings) t.settings = title_slots::parse_settings(std::string(data.begin(), data.end()));
    std::vector<title_slots::Pack> infos;
    for (auto& p : t.packs) infos.push_back(p.info);
    std::mt19937_64 rng(uint64_t(std::chrono::system_clock::now().time_since_epoch().count()) ^ (uint64_t(GetCurrentProcessId()) << 32));
    t.choice = title_slots::resolve_choice(infos, t.settings, rng());
    // Random: the launch's background from the bag (no pack again before every one has shown), the bag kept for the session
    t.bag_path = game_dir + L"\\reframework\\data\\SF6_TitleSlots_Data\\shuffle.json";
    if (t.choice.random) {
        std::vector<std::string> all = { title_slots::kGamePictureKey };
        for (auto& p : t.packs) if (p.info.title_files) all.push_back(p.info.key);
        std::vector<std::string> pool = title_slots::random_selection(all, t.settings.has_pool, t.settings.pool);
        if (pool.empty()) pool = title_slots::random_selection(all, false, {});   // a selection naming no background left: every pack
        std::vector<uint8_t> raw;
        title_slots::Bag bag = read_file(t.bag_path, raw) ? title_slots::parse_bag(std::string(raw.begin(), raw.end())) : title_slots::Bag{};
        const std::string key = title_slots::bag_next(bag, pool, [&] { return rng(); });
        if (!key.empty()) {
            title_slots::Settings one = t.settings;
            one.pack = key == title_slots::kGamePictureKey ? std::string() : key;   // the game's own: no pack's title files
            t.choice = title_slots::resolve_choice(infos, one, 0);
            t.choice.random = true;
            CreateDirectoryW((game_dir + L"\\reframework\\data\\SF6_TitleSlots_Data").c_str(), nullptr);
            write_text(t.bag_path, title_slots::bag_text(bag));
            slog("  random: \"%s\" from the bag, %zu left this round", key.c_str(), bag.left.size());
        }
    }

    // What the pak would hold: the resolved choice (after the draw) and the sources, by name, size and date
    t.fingerprint = std::string(TITLE_FORMAT_VERSION) + "\npack|" + title_key(t, t.choice.title) + "\nstartup|" +
                    title_key(t, t.choice.startup) + "\nskip|" + (t.settings.skip_intro ? "1" : "0") + "\n";
    for (auto& s : sources) {
        const std::string n = narrow(s.name);
        if (s.dir) fingerprint_dir(s.path, "title_mods/" + n, t.fingerprint);
        else t.fingerprint += "title_mods/" + n + "|" + std::to_string(s.size) + "|" + std::to_string(s.mtime) + "\n";
    }
    // the game's own title's picture, when there is one
    for (const wchar_t* ext : { L".png", L".jpg", L".jpeg" }) {
        const std::wstring gp = game_dir + L"\\reframework\\data\\SF6_TitleSlots_Data\\game_title" + ext;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(lp(gp).c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        t.game_picture = gp;
        t.game_preview = image_header_ok(lp(gp));
        const uint64_t size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        const uint64_t mtime = (uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        t.fingerprint += "game_title|" + narrow(ext) + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "\n";
        slog("  the game's own title's picture: %s%s", narrow(leaf(gp)).c_str(), t.game_preview ? "" : " (not a readable PNG or JPEG: none)");
        break;
    }

    std::string names;
    for (auto& p : t.packs) names += (names.empty() ? "" : ", ") + ("\"" + p.info.name + "\"");
    slog("title_mods: %zu pack(s)%s%s", t.packs.size(), names.empty() ? "" : ": ", names.c_str());
    slog("  title_pack \"%s\", skip_intro %s%s", t.settings.pack.c_str(), t.settings.skip_intro ? "on" : "off",
         have_settings ? "" : " (no settings.json: the defaults)");
    if (t.choice.unknown) slog("  title_pack \"%s\" names no pack: the game's own title", t.settings.pack.c_str());
    if (t.choice.random)
        slog("  random: %s; the stage pak is rebuilt whenever the draw differs from the last launch's",
             t.choice.title >= 0 ? ("drew \"" + t.packs[size_t(t.choice.title)].info.name + "\" (" + title_key(t, t.choice.title) + ")").c_str()
                                 : "no pack with title files, the game's own title");
    slog("  title screen: %s; startup files (skip_intro): %s",
         t.choice.title >= 0 ? ("\"" + std::string(title_name(t, t.choice.title)) + "\" (" + title_key(t, t.choice.title) + ")").c_str()
                             : "the game's own",
         t.choice.startup >= 0 ? ("\"" + std::string(title_name(t, t.choice.startup)) + "\"").c_str() : "none");
    return t;
}

// Every pack's title files at their private paths (title_slots::private_title_path: the switch on a return to the title),
// the chosen pack's at their real paths too (this launch's title, served before any hook: the same data, stored once), and,
// with skip_intro, the chosen startup pack's startup files at their real paths. Read now, one pack at a time; their textures
// checked like a stage mod's. Then the game's own title files (re_chunk_000.pak and its sub paks) at the private paths of
// "game", to switch back to it. n_private: the copies added.
void add_title_files(TitleScan& t, PakWriter& writer, const std::wstring& game_dir, size_t& n_title, size_t& n_startup, size_t& n_private) {
    n_title = n_startup = n_private = 0;
    auto read_sel = [&](TitlePack& p, bool title_files, Option& sel) {
        sel.rel = "title_mods/" + p.opt.rel;
        for (auto& f : p.opt.files) {
            if (title_slots::is_title_file(f.path) != title_files) continue;
            OptFile g = f;   // listed: no data yet
            if (!read_file(g.file, g.data)) { slog("  cannot read %s", narrow(g.file).c_str()); continue; }
            g.dsize = (int64_t)g.data.size();
            sel.files.push_back(std::move(g));
        }
        check_textures(sel);
    };
    std::map<std::string, uint64_t> title_paths;          // every title file a pack has: the game's own copy of each
    for (size_t i = 0; i < t.packs.size(); ++i) {
        TitlePack& p = t.packs[i];
        Option sel;
        read_sel(p, true, sel);
        const bool chosen = (int)i == t.choice.title;
        for (auto& f : sel.files) {
            title_paths[f.path] = f.hash;
            const std::string src = "title:" + p.info.key + ":" + f.path;
            writer.add_raw(pak_path_hash(std::string_view(title_slots::private_title_path(p.info.key, f.path))), std::move(f.data), f.attrib, f.dsize);
            writer.note_source(src);
            ++n_private;
            if (!chosen) continue;
            if (writer.add_same_source(f.hash, src)) { ++n_title; continue; }
            std::vector<uint8_t> again;                    // not shared after all: read again for the real path
            if (read_file(f.file, again)) { writer.add_raw(f.hash, std::move(again), f.attrib, f.dsize); ++n_title; }
        }
    }
    if (t.choice.startup >= 0) {
        Option sel;
        read_sel(t.packs[size_t(t.choice.startup)], false, sel);
        for (auto& f : sel.files) {
            writer.add_raw(f.hash, std::move(f.data), f.attrib, f.dsize);
            ++n_startup;
        }
    }
    if (title_paths.empty()) return;
    PakReader base;
    std::vector<std::unique_ptr<PakReader>> subs;
    if (!base.open(ascii_path(game_dir, L"re_chunk_000.pak").c_str())) {
        slog("  the game's own title files: re_chunk_000.pak not readable, no switch back to it on a return to the title");
        return;
    }
    for (int k = 0; k < 16; ++k) {                         // the DLC paks under it
        wchar_t name[64];
        swprintf_s(name, L"re_chunk_000.pak.sub_%03d.pak", k);
        if (!file_exists(game_dir + L"\\" + name)) continue;
        auto r = std::make_unique<PakReader>();
        if (r->open(ascii_path(game_dir, name).c_str())) { base.add_overlay(r.get()); subs.push_back(std::move(r)); }
    }
    // Every background's resource file of each streamed title file (the game's own, from its pak: the same small file for all),
    // beside the private data: the core's playlist gives the movies those resources
    std::vector<std::string> keys;
    for (auto& p : t.packs) keys.push_back(p.info.key);
    keys.push_back(title_slots::kGamePictureKey);
    size_t stubs = 0;
    for (auto& [path, hash] : title_paths) {
        const std::string res = title_slots::resource_file_of(path);
        const PakEntry* e = res.empty() ? nullptr : base.find(pak_path_hash(std::string_view(res)));
        if (!e) continue;
        const std::vector<uint8_t> raw = base.read(*e);
        if (raw.empty()) continue;
        for (auto& k : keys) {
            writer.add_uncompressed(pak_path_hash(std::string_view(title_slots::private_title_path(k, res))), raw);
            ++stubs;
            ++n_private;
        }
    }
    slog("  the title movies' resource files: %zu copied beside the private copies", stubs);
    size_t found = 0;
    for (auto& [path, hash] : title_paths) {
        const PakEntry* e = base.find(hash);
        if (!e) continue;
        std::vector<uint8_t> raw = base.read_raw(*e);
        if (raw.empty()) continue;
        writer.add_raw(pak_path_hash(std::string_view(title_slots::private_title_path(title_slots::kGamePictureKey, path))), std::move(raw),
                       e->attributes, e->decompressed_size);
        ++found;
        ++n_private;
    }
    slog("  the game's own title files: %zu of %zu copied for the switch back to it", found, title_paths.size());
}

// The title tables for the redirect: every background's real -> private hashes, kept only where the stage pak holds the
// copy (an up-to-date pak was built by this same code from the same packs; a copy that could not be made is left out)
TitleTables g_titles;
void make_title_tables(const TitleScan& t, const std::wstring& pak_path) {
    g_titles = TitleTables{};
    if (!t.present || t.packs.empty()) return;
    std::unordered_map<uint64_t, bool> held;
    FILE* f = nullptr;
    if (_wfopen_s(&f, lp(pak_path).c_str(), L"rb") == 0 && f) {
        std::vector<KpkaEntry> idx;
        if (kpka_index(f, idx)) for (auto& k : idx) held[k.hash] = true;
        fclose(f);
    }
    std::map<std::string, uint64_t> title_paths;
    size_t entries = 0;
    for (auto& p : t.packs) {
        std::vector<std::pair<uint64_t, uint64_t>> r;
        for (auto& file : p.opt.files) {
            if (!title_slots::is_title_file(file.path)) continue;
            title_paths[file.path] = file.hash;
            const uint64_t h = pak_path_hash(std::string_view(title_slots::private_title_path(p.info.key, file.path)));
            if (held.count(h)) r.push_back({ file.hash, h });
            if (file.path.find("/title_start.") != std::string::npos &&
                std::find(g_titles.start_hashes.begin(), g_titles.start_hashes.end(), file.hash) == g_titles.start_hashes.end())
                g_titles.start_hashes.push_back(file.hash);
        }
        if (r.empty()) continue;
        entries += r.size();
        g_titles.random_pool.push_back(p.info.key);
        bool playable = true;                         // the core's playlist: every streamed title file whole (data + resource file)
        for (auto& file : p.opt.files) {
            if (!title_slots::is_title_file(file.path)) continue;
            const std::string res = title_slots::resource_file_of(file.path);
            if (res.empty()) continue;
            playable = playable && held.count(pak_path_hash(std::string_view(title_slots::private_title_path(p.info.key, file.path)))) &&
                       held.count(pak_path_hash(std::string_view(title_slots::private_title_path(p.info.key, res))));
        }
        if (playable) g_titles.playlist_pool.push_back(p.info.key);
        g_titles.backgrounds.push_back({ p.info.key, std::move(r) });
    }
    std::vector<std::pair<uint64_t, uint64_t>> game;
    for (auto& [path, hash] : title_paths) {
        const uint64_t h = pak_path_hash(std::string_view(title_slots::private_title_path(title_slots::kGamePictureKey, path)));
        if (held.count(h)) game.push_back({ hash, h });
    }
    entries += game.size();
    if (!game.empty()) {
        bool playable = true;                         // the game's own copies for the playlist: data + resource file
        for (auto& [path, hash] : title_paths) {
            const std::string res = title_slots::resource_file_of(path);
            if (res.empty()) continue;
            playable = playable && held.count(pak_path_hash(std::string_view(title_slots::private_title_path(title_slots::kGamePictureKey, path)))) &&
                       held.count(pak_path_hash(std::string_view(title_slots::private_title_path(title_slots::kGamePictureKey, res))));
        }
        g_titles.random_pool.insert(g_titles.random_pool.begin(), title_slots::kGamePictureKey);
        if (playable) g_titles.playlist_pool.insert(g_titles.playlist_pool.begin(), title_slots::kGamePictureKey);
        g_titles.backgrounds.push_back({ title_slots::kGamePictureKey, std::move(game) });
    }
    g_titles.has_selection = t.settings.has_pool;
    g_titles.selection = t.settings.pool;
    g_titles.launch = t.choice.title >= 0 ? t.packs[size_t(t.choice.title)].info.key : std::string(title_slots::kGamePictureKey);
    g_titles.bag_path = t.bag_path;
    g_titles.choice = t.settings.pack;
    slog("title switch on a return to the title: %zu background(s), %zu file(s); this launch: %s; the playlist: %zu background(s)",
         g_titles.backgrounds.size(), entries, g_titles.launch.c_str(), g_titles.playlist_pool.size());
}

// Every pack's picture, at natives/stm/<preview_resource>.<tex version>: 1280x720 from its screenshot. One that cannot
// be made is left out, and the registry says so (preview ""). Returns how many were added.
size_t add_title_previews(TitleScan& t, PakWriter& writer) {
    size_t n = 0;
    int listed = 0;
    for (auto& p : t.packs) listed += p.info.preview ? 1 : 0;
    const MosaicLayout lay = title_mosaic_layout(listed);
    std::vector<std::vector<uint8_t>> cells;                 // Random's mosaic: each picture made, in the packs' order
    for (auto& p : t.packs) {
        if (!p.info.preview) continue;
        std::vector<uint8_t> tex, cell, thumb, band;
        std::string err;
        if (!make_title_preview_tex(lp(p.opt.screenshot), tex, err, &cell, lay.cell_w, lay.cell_h, &thumb, &band)) {
            p.info.preview = false;
            slog("  title_mods: %s: no picture (%s)", p.opt.rel.c_str(), err.c_str());
            continue;
        }
        writer.add_uncompressed(pak_path_hash(std::string_view("natives/stm/_titleslots/" + p.info.key + "/preview" + TEX_SUFFIX)), std::move(tex));
        if (!thumb.empty())
            writer.add_uncompressed(pak_path_hash(std::string_view("natives/stm/_titleslots/" + p.info.key + "/thumb" + TEX_SUFFIX)), std::move(thumb));
        if (!band.empty())
            writer.add_uncompressed(pak_path_hash(std::string_view("natives/stm/_titleslots/" + p.info.key + "/band" + TEX_SUFFIX)), std::move(band));
        cells.push_back(std::move(cell));
        ++n;
    }
    for (bool on : { false, true }) {                       // the Random list's check boxes (only with packs: no pak for them alone)
        if (t.packs.empty()) break;
        std::vector<uint8_t> tex;
        if (make_check_box_tex(on, tex)) {
            writer.add_uncompressed(pak_path_hash(std::string_view("natives/stm/" + std::string(on ? "_titleslots/ui/check_on" : "_titleslots/ui/check_off") + TEX_SUFFIX)), std::move(tex));
            ++n;
        }
    }
    t.random_preview = false;
    if (!cells.empty()) {
        std::vector<uint8_t> tex;
        std::string err;
        if (make_title_mosaic_tex(cells, lay, tex, err)) {
            writer.add_uncompressed(pak_path_hash(std::string_view(std::string("natives/stm/_titleslots/") + title_slots::kRandomPictureKey + "/preview" + TEX_SUFFIX)),
                                    std::move(tex));
            t.random_preview = true;
            slog("  Random's picture: %zu picture(s), %d x %d places of %dx%d", cells.size(), lay.cols, lay.rows, lay.place_w, lay.place_h);
        } else {
            slog("  Random's picture: none (%s)", err.c_str());
        }
    }
    if (t.game_preview) {
        std::vector<uint8_t> tex, thumb, band;
        std::string err;
        if (make_title_preview_tex(lp(t.game_picture), tex, err, nullptr, 0, 0, &thumb, &band)) {
            writer.add_uncompressed(pak_path_hash(std::string_view(std::string("natives/stm/_titleslots/") + title_slots::kGamePictureKey + "/preview" + TEX_SUFFIX)),
                                    std::move(tex));
            if (!thumb.empty())
                writer.add_uncompressed(pak_path_hash(std::string_view(std::string("natives/stm/_titleslots/") + title_slots::kGamePictureKey + "/thumb" + TEX_SUFFIX)),
                                        std::move(thumb));
            if (!band.empty())
                writer.add_uncompressed(pak_path_hash(std::string_view(std::string("natives/stm/_titleslots/") + title_slots::kGamePictureKey + "/band" + TEX_SUFFIX)),
                                        std::move(band));
            ++n;
        } else {
            t.game_preview = false;
            slog("  the game's own title's picture: none (%s)", err.c_str());
        }
    }
    return n;
}

// reframework\data\SF6_TitleSlots_Data\registry.json: the packs, for the plugin, and what this launch applied. Written
// only when its text changes, and only when title_mods exists; title_mods gone, the file goes too (the plugin offers no pack).
bool save_title_registry(const std::wstring& game_dir, const TitleScan& t) {
    const std::wstring dir = game_dir + L"\\reframework\\data\\SF6_TitleSlots_Data";
    const std::wstring path = dir + L"\\registry.json";
    if (!t.present) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileW(path.c_str()))
            slog("title_mods is gone: the title packs' registry removed");
        return true;
    }
    std::string js = "{\n  \"version\": 1,\n  \"packs\": [";
    for (size_t i = 0; i < t.packs.size(); ++i) {
        const auto& p = t.packs[i].info;
        js += i ? ",\n" : "\n";
        js += "    { \"key\": \"" + p.key + "\", \"name\": \"" + json_escape(p.name) + "\", \"author\": \"" + json_escape(p.author) +
              "\", \"source\": \"" + json_escape(p.source) + "\", \"preview\": \"" +
              (p.preview ? title_slots::preview_resource(p.key) : std::string()) + "\", \"thumb\": \"" +
              (p.preview ? title_slots::thumb_resource(p.key) : std::string()) + "\", \"band\": \"" +
              (p.preview ? title_slots::band_resource(p.key) : std::string()) + "\" }";
    }
    js += t.packs.empty() ? "],\n" : "\n  ],\n";
    js += "  \"game_picture\": \"" + (t.game_preview ? title_slots::preview_resource(title_slots::kGamePictureKey) : std::string()) + "\",\n";
    js += "  \"random_picture\": \"" + (t.random_preview ? title_slots::preview_resource(title_slots::kRandomPictureKey) : std::string()) + "\",\n";
    js += "  \"game_thumb\": \"" + (t.game_preview ? title_slots::thumb_resource(title_slots::kGamePictureKey) : std::string()) + "\",\n";
    js += "  \"game_band\": \"" + (t.game_preview ? title_slots::band_resource(title_slots::kGamePictureKey) : std::string()) + "\",\n";
    js += "  \"check_off\": \"" + title_slots::check_resource(false) + "\", \"check_on\": \"" + title_slots::check_resource(true) + "\",\n";
    js += "  \"applied\": { \"pack\": \"" + title_key(t, t.choice.title) + "\", \"skip_intro\": " +
          (t.settings.skip_intro ? "true" : "false") + ", \"random\": " + (t.choice.random ? "true" : "false") + " }\n}\n";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<uint8_t> old;
    if (read_file(path, old) && std::string(old.begin(), old.end()) == js) return true;
    return write_text(path, js);
}

// ---------------------------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------------------------

void run(const std::wstring& game_dir, std::vector<StageVariant>& out) {
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };

    const std::wstring mods_dir = game_dir + L"\\reframework\\stage_mods";
    const std::wstring data_dir = game_dir + L"\\reframework\\data\\SF6_StageSlots_Data";
    const std::wstring index_path = data_dir + L"\\loader\\stage_paths.txt";
    const std::wstring table_path = data_dir + L"\\loader\\variants.tsv";
    CreateDirectoryW((game_dir + L"\\reframework").c_str(), nullptr);
    CreateDirectoryW(mods_dir.c_str(), nullptr);
    CreateDirectoryW((game_dir + L"\\reframework\\data").c_str(), nullptr);
    CreateDirectoryW(data_dir.c_str(), nullptr);
    CreateDirectoryW((data_dir + L"\\loader").c_str(), nullptr);

    auto sources = list_sources(mods_dir);
    TitleScan title = scan_titles(game_dir);
    const std::string fp = compute_fingerprint(sources, index_path, title.fingerprint);

    auto paks = classify_patch_paks(game_dir);
    int max_mod = 0;
    std::vector<PatchPak> stage_paks, stubs;   // stubs: marker-only placeholders, ours and the costume loader's
    for (auto& p : paks) {
        if (p.kind == PakKind::Mod) max_mod = std::max(max_mod, p.num);
        if (p.kind != PakKind::Mod && p.entries == 1) stubs.push_back(p);
        else if (p.kind == PakKind::Stage) stage_paks.push_back(p);
    }
    // A placeholder with no mod's pak above it any more is not needed (it would sit where a real pak goes)
    for (auto& p : stubs) {
        if (p.num > max_mod && delete_with_retry(p.path)) slog("  removed placeholder %s", narrow(leaf(p.path)).c_str());
    }
    const int target = max_mod + 1;
    const std::wstring target_path = patch_path(game_dir, target);
    slog("%zu source(s) in stage_mods, mod paks up to patch_%03d, stage pak goes to patch_%03d", sources.size(), max_mod, target);

    // Nothing changed: the saved table is the answer. The pak must be the one the table was saved
    // with (its entry count): a pak rewritten since for other mods, under a table put back, held
    // none of the variants' files and the stage never finished loading (03/10). A pak was written when
    // the table counts its entries: variants, or title files alone.
    {
        std::string saved_fp;
        size_t saved_entries = 0;
        std::vector<StageVariant> saved;
        if (load_table(table_path, saved_fp, saved_entries, saved) && saved_fp == fp) {
            const bool had_pak = !saved.empty() || saved_entries > 0;
            bool pak_ok = !had_pak ? stage_paks.empty()
                                   : (stage_paks.size() == 1 && stage_paks[0].num == target
                                      && stage_paks[0].entries == saved_entries);
            if (pak_ok) {
                out = std::move(saved);
                apply_overrides(mods_dir, out);
                if (!save_registry(data_dir, fp, out)) slog("ERROR: cannot write the registry");
                if (!save_title_registry(game_dir, title)) slog("ERROR: cannot write the title packs' registry");
                make_title_tables(title, target_path);
                slog("up to date: %zu variant(s), %.1f ms", out.size(), ms());
                return;
            }
        }
    }

    // The stage index is needed for stage mods only (title packs alone build without it)
    std::unordered_map<uint64_t, std::string> index, gui_index, index_paths;
    if (!load_stage_index(index_path, index, gui_index, index_paths) && !sources.empty()) {
        slog("ERROR: %s missing or empty: stage mods cannot be recognised", narrow(index_path).c_str());
        return;
    }

    // Options of every source
    std::vector<Option> options;
    for (auto& s : sources) {
        std::string n = narrow(s.name);
        if (s.dir) {
            collect_options(s.path, n, n, true, 0, options);
        } else if (s.archive != ArchiveKind::None) {
            std::wstring dir;
            if (unpack_archive(s, mods_dir + L"\\.cache", dir)) collect_options(dir, n, narrow(stem(s.name)), true, 0, options);
        } else {
            Option opt;
            opt.rel = n;
            opt.name = narrow(stem(s.name));
            add_pak(s.path, opt);
            options.push_back(std::move(opt));
        }
    }

    // One variant per option and per stage it changes.
    // GUI files (natives/stm/product/gui/**) are never redirected: the menus load them once and the
    // resource cache keeps them for the session, so a picture served while a variant was selected
    // (the Fighting Ground menu loads Training Room's at boot) stayed on the vanilla stage whatever was
    // selected later (Dotonbori At Night, 16/10). A battle loads none of them; the variant's name and
    // preview on the menus are the plugin's. Known by their path (natives trees) or, for a pak's
    // entries, by the stage index.
    PakWriter writer;
    // the title packs' movies are hundreds of MB: big data waits in a scratch file instead of memory (deleted with the writer)
    writer.spill_to(ascii_path(data_dir + L"\\loader", L"stage_spill.tmp").c_str(), 1 << 20, 64ull << 20);
    std::vector<StageVariant> vars;
    for (auto& opt : options) {
        check_textures(opt);
        std::map<std::string, std::vector<size_t>> by_ess, gui_by_ess;
        std::vector<size_t> shared, gui_shared;
        for (size_t i = 0; i < opt.files.size(); ++i) {
            const OptFile& f = opt.files[i];
            auto it = index.find(f.hash);
            const bool gui = lower_a(f.path).compare(0, strlen(GUI_PREFIX), GUI_PREFIX) == 0 || gui_index.count(f.hash);
            if (it != index.end()) (gui ? gui_by_ess : by_ess)[it->second].push_back(i);
            else (gui ? gui_shared : shared).push_back(i);
        }
        if (by_ess.empty()) {
            slog("  %s: no stage file among %zu%s, skipped", opt.rel.c_str(), opt.files.size(),
                 gui_by_ess.empty() && gui_shared.empty() ? "" : " (GUI files are not redirected)");
            continue;
        }
        auto gui_name = [&](size_t i) {
            const OptFile& f = opt.files[i];
            auto g = gui_index.find(f.hash);
            const std::string& p = !f.path.empty() ? f.path : g != gui_index.end() ? g->second : std::string();
            return p.empty() ? hex16(f.hash) : p.substr(p.find_last_of('/') + 1);
        };
        for (auto& [ess, idx] : by_ess) {
            StageVariant v;
            v.ess = ess;
            v.stage_id = stage_id_of(ess);
            v.name = by_ess.size() > 1 ? opt.name + " (" + ess + ")" : opt.name;
            v.author = opt.author;
            v.bundle = opt.bundle;
            v.source = opt.rel;
            v.key = "s" + hex16(fnv64(opt.rel + "|" + ess)).substr(0, 12);
            std::vector<size_t> all = idx;
            all.insert(all.end(), shared.begin(), shared.end());
            std::vector<std::string> kind_paths;
            for (size_t i : all) {
                const OptFile& f = opt.files[i];
                uint64_t moved = pak_path_hash(std::string_view("natives/stm/_stageslots/" + v.key + "/" + hex16(f.hash)));
                writer.add_raw(moved, f.data, f.attrib, f.dsize);
                v.redirects.push_back({ f.hash, moved });
                auto ip = index_paths.find(f.hash);
                if (!f.path.empty()) kind_paths.push_back(lower_a(f.path));
                else if (ip != index_paths.end()) kind_paths.push_back(ip->second);
            }
            const StageKind kind = classify_stage_variant(ess, kind_paths, shared.size());
            v.tile_auto = kind.tile;
            v.reason_auto = kind.reason;
            // Its menu pictures: the mod's own (tex_StageImage / tex_StageThumb of the stage, the streaming copy first: the
            // base file of a streaming texture lacks its largest levels), else cut from its screenshot (below), else none
            // (the plugin shows the vanilla stage's picture and the variant's name).
            auto own_picture = [&](const char* what, bool thumb, std::vector<uint8_t>& out_tex) {
                for (const char* root : { "natives/stm/streaming/product/", "natives/stm/product/" }) {
                    const std::string p = std::string(root) + "gui/data/area_image/ess/" + ess + "/" + what + ess + "_im" + TEX_SUFFIX;
                    const uint64_t h = pak_path_hash(std::string_view(p));
                    for (auto& f : opt.files) {
                        if (f.hash != h) continue;
                        const int comp = int(f.attrib & 0xFF);
                        std::vector<uint8_t> plain = comp ? pak_decompress(f.data, comp, f.dsize) : f.data;
                        std::string err;
                        if (make_stage_tex_from_game_tex(plain, thumb, out_tex, err)) return true;
                        slog("  %s: its %s%s cannot be used (%s)", opt.rel.c_str(), root + 11, what, err.c_str());
                    }
                }
                return false;
            };
            {
                std::vector<uint8_t> tex, thumb;
                const bool own_img = own_picture("tex_stageimage_", false, tex);
                bool own_thumb = own_picture("tex_stagethumb_", true, thumb);
                if (!own_thumb && own_img) {   // the thumbnail cut from the mod's own picture
                    own_thumb = own_picture("tex_stageimage_", true, thumb);
                }
                if (own_img) {
                    writer.add_uncompressed(pak_path_hash(std::string_view("natives/stm/_stageslots/" + v.key + "/preview" + TEX_SUFFIX)), std::move(tex));
                    v.has_preview = true;
                }
                if (own_thumb) {
                    writer.add_uncompressed(pak_path_hash(std::string_view("natives/stm/_stageslots/" + v.key + "/thumb" + TEX_SUFFIX)), std::move(thumb));
                    v.has_thumb = true;
                }
                if (own_img || own_thumb) v.banner = "mod";
            }
            if (!opt.screenshot.empty() && !(v.has_preview && v.has_thumb)) {
                std::string err;
                bool made = false;
                if (!v.has_preview) {
                    std::vector<uint8_t> tex;
                    if (make_stage_preview_tex(lp(opt.screenshot), tex, err)) {
                        std::string p = "natives/stm/_stageslots/" + v.key + "/preview" + TEX_SUFFIX;
                        writer.add_uncompressed(pak_path_hash(std::string_view(p)), std::move(tex));
                        v.has_preview = made = true;
                    } else {
                        slog("  %s: no preview (%s)", opt.rel.c_str(), err.c_str());
                    }
                }
                if (!v.has_thumb) {
                    std::vector<uint8_t> thumb;
                    if (make_stage_thumb_tex(lp(opt.screenshot), thumb, err)) {
                        std::string p = "natives/stm/_stageslots/" + v.key + "/thumb" + TEX_SUFFIX;
                        writer.add_uncompressed(pak_path_hash(std::string_view(p)), std::move(thumb));
                        v.has_thumb = made = true;
                    } else {
                        slog("  %s: no thumb (%s)", opt.rel.c_str(), err.c_str());
                    }
                }
                if (made && v.banner.empty()) v.banner = "screenshot";
            }
            slog("  variant %s: \"%s\" -> %s (stage %u), %zu files (%zu of the stage)%s%s, %s (%s)", v.key.c_str(), v.name.c_str(),
                 ess.c_str(), v.stage_id, all.size(), idx.size(), v.has_preview ? (v.has_thumb ? ", preview+thumb" : ", preview") : "",
                 v.banner.empty() ? "" : (" from " + v.banner).c_str(), v.tile_auto ? "TILE" : "variation", v.reason_auto.c_str());
            std::vector<size_t> kept = gui_shared;
            if (auto g = gui_by_ess.find(ess); g != gui_by_ess.end()) kept.insert(kept.begin(), g->second.begin(), g->second.end());
            if (!kept.empty()) {
                std::string names;
                for (size_t i : kept) names += (names.empty() ? "" : ", ") + gui_name(i);
                slog("  variant %s: %zu GUI file(s) not redirected (menus keep them for the session): %s",
                     v.key.c_str(), kept.size(), names.c_str());
            }
            vars.push_back(std::move(v));
        }
    }
    std::stable_sort(vars.begin(), vars.end(), [](const StageVariant& a, const StageVariant& b) {
        if (a.stage_id != b.stage_id) return a.stage_id < b.stage_id;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    // The title packs' files: every pack's at its private paths, the chosen one's at the real paths too
    size_t title_files = 0, startup_files = 0, title_private = 0;
    add_title_files(title, writer, game_dir, title_files, startup_files, title_private);
    const size_t title_pictures = add_title_previews(title, writer);
    if (title_pictures) slog("  title pack pictures: %zu of %zu pack(s)", title_pictures, title.packs.size());
    if (title.choice.title >= 0 || title.choice.startup >= 0)
        slog("  title pack files: %zu title file(s)%s, %zu startup file(s)%s", title_files,
             title.choice.title >= 0 ? (" of \"" + std::string(title_name(title, title.choice.title)) + "\"").c_str() : "",
             startup_files, title.choice.startup >= 0 ? (" of \"" + std::string(title_name(title, title.choice.startup)) + "\"").c_str() : "");

    // Our old pak goes; the costume loader's pak moves up when it sits where ours goes (it is
    // rebuilt right after us anyway, its mod list having changed).
    for (auto& p : stage_paks) {
        bool stub = false;
        if (!retire_pak(game_dir, p, max_mod, stub)) {
            slog("ERROR: %s is in use (another Street Fighter 6 still running?): stages not updated", narrow(p.path).c_str());
            return;
        }
        slog(stub ? "  old pak %s replaced by a marker-only pak (a mod's pak sits above it)" : "  removed old pak %s",
             narrow(leaf(p.path)).c_str());
    }
    size_t pak_entries = 0;
    if (!vars.empty() || title_files + startup_files + title_pictures + title_private > 0) {
        if (file_exists(target_path)) {
            std::wstring up = patch_path(game_dir, target + 1);
            if (file_exists(up) || !MoveFileW(target_path.c_str(), up.c_str())) {
                slog("ERROR: cannot free %s for the stage pak", narrow(leaf(target_path)).c_str());
                return;
            }
            slog("  moved %s up to %s", narrow(leaf(target_path)).c_str(), narrow(leaf(up)).c_str());
        }
        std::vector<uint8_t> marker(21);
        memcpy(marker.data(), "SF6_StageSlots v1\n\0\0\0", 21);
        writer.add_uncompressed(pak_path_hash(std::string_view(STAGE_MARKER)), std::move(marker));
        std::string ap = ascii_path(game_dir, leaf(target_path));
        if (ap.empty() || !writer.write(ap.c_str())) {
            DeleteFileW(target_path.c_str());
            slog("ERROR: cannot write %s", narrow(leaf(target_path)).c_str());
            return;
        }
        slog("  wrote %s (%zu entries)", narrow(leaf(target_path)).c_str(), writer.entry_count());
        pak_entries = writer.entry_count();
    }
    if (!save_table(data_dir, fp, pak_entries, vars)) slog("ERROR: cannot write the table");
    apply_overrides(mods_dir, vars);
    if (!save_registry(data_dir, fp, vars)) slog("ERROR: cannot write the registry");
    if (!save_title_registry(game_dir, title)) slog("ERROR: cannot write the title packs' registry");
    make_title_tables(title, target_path);
    out = std::move(vars);
    slog("built: %zu variant(s) from %zu option(s), %zu title pack file(s), %.1f ms", out.size(), options.size(),
         title_files + startup_files, ms());
}

} // namespace

const TitleTables& stage_loader_titles() { return g_titles; }

std::string stage_thumb_resource(const std::string& key) {
    return "_stageslots/" + key + "/thumb.tex";
}

std::string stage_preview_resource(const std::string& key) {
    return "_stageslots/" + key + "/preview.tex";
}

void stage_loader_run(const wchar_t* game_dir, std::vector<StageVariant>& out) {
    slog("=== SF6 Stage Slots: loader ===");
    run(game_dir, out);
}
