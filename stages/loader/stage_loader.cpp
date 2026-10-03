// Stage slots: startup pass. See stage_loader.hpp.

#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "archive.hpp"
#include "pak.hpp"
#include "patch.hpp"          // texture_mip_check, shared with the costume loader
#include "preview_tex.hpp"
#include "stage_loader.hpp"
#include "stage_log.hpp"

namespace {

const char* STAGE_MARKER = STAGE_SLOTS_MARKER;
const char* COSTUME_MARKER = COSTUME_SLOTS_MARKER;
// Part of the fingerprint: bump it when the pak layout, the preview conversion or the checks of the
// mods' files change.
const char* FORMAT_VERSION = "stageslots-4";
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

bool load_stage_index(const std::wstring& path, std::unordered_map<uint64_t, std::string>& out) {
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
        if (!line.empty() && ess_code_in(line, code)) out[pak_path_hash(std::string_view(line))] = code;
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------------------------
// Mods: sources (what sits in stage_mods) and options (what the game would install)
// ---------------------------------------------------------------------------------------------

struct OptFile { uint64_t hash; std::vector<uint8_t> data; int64_t attrib; int64_t dsize; };
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

void add_natives(const std::wstring& dir, const std::string& rel, Option& opt) {
    for (auto& e : list_dir(dir)) {
        std::wstring full = dir + L"\\" + e.name;
        std::string r = rel + "/" + lower_a(narrow(e.name));
        if (e.dir) { add_natives(full, r, opt); continue; }
        OptFile f;
        if (!read_file(full, f.data)) { slog("  cannot read %s", narrow(full).c_str()); continue; }
        f.hash = pak_path_hash(std::string_view(r));
        f.attrib = 0;
        f.dsize = (int64_t)f.data.size();
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
void collect_options(const std::wstring& dir, const std::string& rel, const std::string& default_name,
                     bool is_root, int depth, std::vector<Option>& out) {
    auto entries = list_dir(dir);
    bool has_pak = false, has_natives = false;
    for (auto& e : entries) {
        if (!e.dir && ends_with(e.name, L".pak")) has_pak = true;
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
            if (!e.dir && ends_with(e.name, L".pak")) add_pak(dir + L"\\" + e.name, opt);
            if (e.dir && _wcsicmp(e.name.c_str(), L"natives") == 0) add_natives(dir + L"\\" + e.name, "natives", opt);
        }
        out.push_back(std::move(opt));
        return;
    }
    if (depth >= 6) return;
    for (auto& e : entries) {
        if (!e.dir || e.name[0] == L'.') continue;
        collect_options(dir + L"\\" + e.name, rel + "/" + narrow(e.name), default_name, false, depth + 1, out);
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

std::string compute_fingerprint(const std::vector<Source>& sources, const std::wstring& index_path) {
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

bool save_outputs(const std::wstring& data_dir, const std::string& fingerprint, size_t pak_entries, const std::vector<StageVariant>& vars) {
    std::string tsv = "#fingerprint\t" + fingerprint + "\n#pak\t" + std::to_string(pak_entries) + "\n";
    for (auto& v : vars) {
        tsv += "V\t" + v.key + "\t" + std::to_string(v.stage_id) + "\t" + v.ess + "\t" + (v.has_preview ? "1" : "0") + "\t" +
               tsv_clean(v.name) + "\t" + tsv_clean(v.author) + "\t" + tsv_clean(v.source) + "\t" + tsv_clean(v.bundle) + "\n";
        for (auto& r : v.redirects) tsv += "R\t" + v.key + "\t" + hex16(r.first) + "\t" + hex16(r.second) + "\n";
    }
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
                  (v->has_preview ? stage_preview_resource(v->key) : std::string()) + "\", \"files\": " +
                  std::to_string(v->redirects.size()) + " }";
        }
        js += "\n    ] }";
    }
    js += "\n  ]\n}\n";
    bool ok = write_text(data_dir + L"\\loader\\variants.tsv", tsv);
    ok = write_text(data_dir + L"\\registry.json", js) && ok;
    return ok;
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
    const std::string fp = compute_fingerprint(sources, index_path);

    auto paks = classify_patch_paks(game_dir);
    int max_mod = 0;
    std::vector<PatchPak> stage_paks;
    for (auto& p : paks) {
        if (p.kind == PakKind::Mod) max_mod = std::max(max_mod, p.num);
        if (p.kind == PakKind::Stage) stage_paks.push_back(p);
    }
    const int target = max_mod + 1;
    const std::wstring target_path = patch_path(game_dir, target);
    slog("%zu source(s) in stage_mods, mod paks up to patch_%03d, stage pak goes to patch_%03d", sources.size(), max_mod, target);

    // Nothing changed: the saved table is the answer. The pak must be the one the table was saved
    // with (its entry count): a pak rewritten since for other mods, under a table put back, held
    // none of the variants' files and the stage never finished loading (03/10).
    {
        std::string saved_fp;
        size_t saved_entries = 0;
        std::vector<StageVariant> saved;
        if (load_table(table_path, saved_fp, saved_entries, saved) && saved_fp == fp) {
            bool pak_ok = saved.empty() ? stage_paks.empty()
                                        : (stage_paks.size() == 1 && stage_paks[0].num == target
                                           && stage_paks[0].entries == saved_entries);
            if (pak_ok) {
                out = std::move(saved);
                slog("up to date: %zu variant(s), %.1f ms", out.size(), ms());
                return;
            }
        }
    }

    std::unordered_map<uint64_t, std::string> index;
    if (!load_stage_index(index_path, index)) {
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

    // One variant per option and per stage it changes
    PakWriter writer;
    std::vector<StageVariant> vars;
    for (auto& opt : options) {
        check_textures(opt);
        std::map<std::string, std::vector<size_t>> by_ess;
        std::vector<size_t> shared;
        for (size_t i = 0; i < opt.files.size(); ++i) {
            auto it = index.find(opt.files[i].hash);
            if (it != index.end()) by_ess[it->second].push_back(i);
            else shared.push_back(i);
        }
        if (by_ess.empty()) {
            slog("  %s: no stage file among %zu, skipped", opt.rel.c_str(), opt.files.size());
            continue;
        }
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
            for (size_t i : all) {
                const OptFile& f = opt.files[i];
                uint64_t moved = pak_path_hash(std::string_view("natives/stm/_stageslots/" + v.key + "/" + hex16(f.hash)));
                writer.add_raw(moved, f.data, f.attrib, f.dsize);
                v.redirects.push_back({ f.hash, moved });
            }
            if (!opt.screenshot.empty()) {
                std::vector<uint8_t> tex;
                std::string err;
                if (make_stage_preview_tex(lp(opt.screenshot), tex, err)) {
                    std::string p = "natives/stm/_stageslots/" + v.key + "/preview" + TEX_SUFFIX;
                    writer.add_uncompressed(pak_path_hash(std::string_view(p)), std::move(tex));
                    v.has_preview = true;
                } else {
                    slog("  %s: no preview (%s)", opt.rel.c_str(), err.c_str());
                }
            }
            slog("  variant %s: \"%s\" -> %s (stage %u), %zu files (%zu of the stage)%s", v.key.c_str(), v.name.c_str(),
                 ess.c_str(), v.stage_id, all.size(), idx.size(), v.has_preview ? ", preview" : "");
            vars.push_back(std::move(v));
        }
    }
    std::stable_sort(vars.begin(), vars.end(), [](const StageVariant& a, const StageVariant& b) {
        if (a.stage_id != b.stage_id) return a.stage_id < b.stage_id;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    // Our old pak goes; the costume loader's pak moves up when it sits where ours goes (it is
    // rebuilt right after us anyway, its mod list having changed).
    for (auto& p : stage_paks) {
        if (!delete_with_retry(p.path)) {
            slog("ERROR: %s is in use (another Street Fighter 6 still running?): stages not updated", narrow(p.path).c_str());
            return;
        }
        slog("  removed old pak %s", narrow(leaf(p.path)).c_str());
    }
    size_t pak_entries = 0;
    if (!vars.empty()) {
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
    if (!save_outputs(data_dir, fp, pak_entries, vars)) slog("ERROR: cannot write the registry");
    out = std::move(vars);
    slog("built: %zu variant(s) from %zu option(s), %.1f ms", out.size(), options.size(), ms());
}

} // namespace

std::string stage_preview_resource(const std::string& key) {
    return "_stageslots/" + key + "/preview.tex";
}

void stage_loader_run(const wchar_t* game_dir, std::vector<StageVariant>& out) {
    slog("=== SF6 Stage Slots: loader ===");
    run(game_dir, out);
}
