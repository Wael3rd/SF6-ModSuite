// SF6 Mod Suite updater: what touches files (update_fs.hpp).
#include "update_fs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>

#include "../../common/file_sha256.hpp"
#include "../../common/manager_mode.hpp"
#include "../../common/third_party/miniz.h"
#include "nlohmann/json.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace sf6up {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring join(const std::wstring& root, const std::string& rel) {
    std::wstring r = widen(rel);
    for (auto& c : r) if (c == L'/') c = L'\\';
    if (root.empty()) return r;
    return (root.back() == L'\\' || root.back() == L'/') ? root + r : root + L"\\" + r;
}

bool exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool make_dirs(const std::wstring& dir) {
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

namespace {

bool is_file(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring parent_of(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::string file_name_of(const std::string& rel) {
    const size_t s = rel.find_last_of('/');
    return s == std::string::npos ? rel : rel.substr(s + 1);
}

std::string dir_of(const std::string& rel) {
    const size_t s = rel.find_last_of('/');
    return s == std::string::npos ? std::string() : rel.substr(0, s);
}

std::string last_error_text(const char* what) {
    char b[96];
    std::snprintf(b, sizeof b, "%s (error %lu)", what, GetLastError());
    return b;
}

}  // namespace

bool read_file(const std::wstring& p, std::string* out, size_t max_bytes) {
    out->clear();
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && (uint64_t)sz.QuadPart <= max_bytes;
    if (ok) {
        out->resize((size_t)sz.QuadPart);
        size_t at = 0;
        while (ok && at < out->size()) {
            DWORD got = 0;
            const DWORD want = (DWORD)std::min<size_t>(out->size() - at, 1u << 20);
            ok = ReadFile(h, out->data() + at, want, &got, nullptr) && got > 0;
            at += got;
        }
    }
    CloseHandle(h);
    if (!ok) out->clear();
    return ok;
}

bool write_file(const std::wstring& p, const std::string& data) {
    make_dirs(parent_of(p));
    const std::wstring tmp = p + L".tmp-up";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

int64_t file_size(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return (int64_t)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
}

bool same_content(const std::wstring& a, const std::wstring& b) {
    const int64_t sa = file_size(a), sb = file_size(b);
    if (sa < 0 || sa != sb) return false;
    std::string x, y;
    if (!read_file(a, &x, (size_t)sa) || !read_file(b, &y, (size_t)sb)) return false;
    return x == y;
}

std::string sha256_file(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
            if (got == 0) break;
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) { ok = false; break; }
        }
        unsigned char d[32];
        if (ok && BCryptFinishHash(hash, d, sizeof d, 0) == 0) {
            static const char* hx = "0123456789abcdef";
            for (unsigned char c : d) { out += hx[c >> 4]; out += hx[c & 15]; }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

void remove_tree(const std::wstring& dir) {
    std::error_code ec;
    fs::remove_all(fs::path(dir), ec);
}

// ---- installed packages ------------------------------------------------------------------------------------------------------------
std::vector<Installed> detect_installed(const std::wstring& root, const VersionReader& version_of) {
    std::vector<Installed> out;
    for (auto& k : known_packages()) {
        Installed i;
        i.id = k.id;
        i.present = is_file(join(root, k.proof));
        if (i.present && version_of) i.version = version_of(k);
        i.managed = i.present && manager_lists(root, k.proof);
        out.push_back(i);
    }
    return out;
}

namespace {

// The Slotsloader Manager's list of what it installed (manager_installs.json), read as the plugin reads it. False: no list, or not one
bool read_manager_list(const std::wstring& root, std::string* fmt, std::vector<manager_mode::ListedFile>* out) {
    std::string text;
    const std::wstring list = join(root, "reframework/data/SF6_Costumes_Data/" + std::string(manager_mode::kRecordFile));
    if (!read_file(list, &text, 4u << 20)) return false;
    try {
        json j = json::parse(text);
        if (!j.is_object()) return false;
        std::vector<manager_mode::ListedFile>& files = *out;
        if (auto mods = j.find("mods"); mods != j.end() && mods->is_array())
            for (auto& m : *mods) {
                if (!m.is_object()) continue;
                auto en = m.find("enabled");
                const bool on = !(en != m.end() && en->is_boolean() && !en->get<bool>());
                if (auto fl = m.find("files"); fl != m.end() && fl->is_array())
                    for (auto& f : *fl) {
                        if (!f.is_object()) continue;
                        manager_mode::ListedFile lf;
                        lf.enabled = on;
                        if (auto p = f.find("path"); p != f.end() && p->is_string()) lf.path = p->get<std::string>();
                        if (auto s = f.find("size"); s != f.end() && s->is_number() && s->get<double>() >= 0) lf.size = (long long)s->get<double>();
                        if (auto h = f.find("sha256"); h != f.end() && h->is_string()) lf.sha256 = manager_mode::lower(h->get<std::string>());
                        files.push_back(lf);
                    }
            }
        if (auto f = j.find("format"); f != j.end() && f->is_string()) *fmt = f->get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool manager_mode(const std::wstring& root, std::string* why) {
    if (why) why->clear();
    std::string fmt;
    std::vector<manager_mode::ListedFile> files;
    if (!read_manager_list(root, &fmt, &files)) {
        if (why && exists(join(root, "reframework/data/SF6_Costumes_Data/" + std::string(manager_mode::kRecordFile))))
            *why = "the list could not be read";
        return false;
    }
    const std::wstring plugin = join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll");
    const manager_mode::Decision d = manager_mode::decide(fmt, files, file_size(plugin), [&] { return file_sha256::hex(plugin.c_str()); });
    if (why) *why = d.why;
    return d.manager;
}

bool manager_lists(const std::wstring& root, const std::string& rel) {
    std::string fmt;
    std::vector<manager_mode::ListedFile> files;
    if (!read_manager_list(root, &fmt, &files) || fmt != manager_mode::kRecordFormat) return false;
    const std::wstring p = join(root, rel);
    return manager_mode::matched(manager_mode::match_file(files, manager_mode::norm_path(rel), file_size(p),
                                                          [&] { return file_sha256::hex(p.c_str()); }));
}

bool verify_file(const std::wstring& p, int64_t size, const std::string& sha256, std::string* why) {
    const int64_t got = file_size(p);
    if (got != size) {
        if (why) *why = "size " + std::to_string(got) + ", the release says " + std::to_string(size);
        return false;
    }
    const std::string h = sha256_file(p);
    if (h != sha256) {
        if (why) *why = "sha256 " + (h.empty() ? std::string("unreadable") : h.substr(0, 12)) + ", the release says " + sha256.substr(0, 12);
        return false;
    }
    return true;
}

// ---- extraction --------------------------------------------------------------------------------------------------------------------
bool extract_zip(const std::wstring& zip, const std::wstring& tree, const std::string& package, const std::string& from, Staged* out,
                 std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    std::string data;
    if (!read_file(zip, &data, (size_t)kAssetMaxBytes)) return fail("the zip cannot be read");
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return fail("not a zip");
    struct Item { mz_uint index; EntryRule rule; uint64_t size; };
    std::vector<Item> items;
    std::set<std::string> seen;
    uint64_t total = 0;
    bool ok = true;
    std::string w;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    if (n > 4096) { ok = false; w = "too many entries"; }
    for (mz_uint i = 0; ok && i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) { ok = false; w = "an entry cannot be read"; break; }
        const std::string name = st.m_filename;
        bool ascii = true;
        for (unsigned char c : name) if (c >= 0x80) ascii = false;
        if (!ascii) {
            const bool utf8_flag = (st.m_bit_flag & (1u << 11)) != 0;
            if (!utf8_flag || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), (int)name.size(), nullptr, 0) <= 0) {
                ok = false; w = "an entry's name is not UTF-8"; break;
            }
        }
        EntryRule r = classify_entry(name);
        if (r.kind == Entry::Refuse) { ok = false; w = "refused entry \"" + name + "\": " + r.why; break; }
        if (r.kind == Entry::Skip) continue;
        if (st.m_is_encrypted || !st.m_is_supported) { ok = false; w = "an encrypted or unsupported entry: " + name; break; }
        const bool dir = r.kind == Entry::Dir;
        if (dir != (bool)st.m_is_directory && !(dir && st.m_uncomp_size == 0)) { ok = false; w = "a folder entry with data: " + name; break; }
        if (!seen.insert(lower(r.path) + (dir ? "/" : "")).second) { ok = false; w = "an entry twice: " + name; break; }
        if (st.m_uncomp_size > (uint64_t)kAssetMaxBytes) { ok = false; w = "an entry too large: " + name; break; }
        total += st.m_uncomp_size;
        if (total > 4ull * (uint64_t)kAssetMaxBytes) { ok = false; w = "the zip unpacks too large"; break; }
        items.push_back({ i, r, st.m_uncomp_size });
    }
    // a file and a folder of the same name
    if (ok)
        for (auto& it : items)
            if (it.rule.kind != Entry::Dir && seen.count(lower(it.rule.path) + "/")) { ok = false; w = "a file and a folder named " + it.rule.path; break; }
    Staged st_out;
    for (size_t k = 0; ok && k < items.size(); ++k) {
        auto& it = items[k];
        if (it.rule.kind == Entry::Dir) { st_out.dirs.push_back(it.rule.path); continue; }
        size_t got = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, it.index, &got, 0);
        if (!p || got != it.size) { if (p) mz_free(p); ok = false; w = "an entry does not unpack: " + it.rule.path; break; }
        const std::wstring dst = join(tree, it.rule.path);
        const bool written = make_dirs(parent_of(dst)) && write_file(dst, std::string(static_cast<const char*>(p), got));
        mz_free(p);
        if (!written) { ok = false; w = "the staging tree cannot be written: " + it.rule.path; break; }
        st_out.files.push_back({ it.rule.path, it.rule.kind, package, from });
    }
    mz_zip_reader_end(&z);
    if (!ok) return fail(w);
    out->files.insert(out->files.end(), st_out.files.begin(), st_out.files.end());
    out->dirs.insert(out->dirs.end(), st_out.dirs.begin(), st_out.dirs.end());
    return true;
}

bool extract_one(const std::wstring& zip, const std::string& name, const std::wstring& dest, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    std::string data;
    if (!read_file(zip, &data, (size_t)kAssetMaxBytes)) return fail("the zip cannot be read");
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return fail("not a zip");
    const std::string want = lower(name);
    int found = -1;
    uint64_t size = 0;
    std::string w;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    if (n > 4096) w = "too many entries";
    for (mz_uint i = 0; w.empty() && i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) { w = "an entry cannot be read"; break; }
        if (lower(st.m_filename) != want) continue;          // the exact name: an entry in a folder is another name
        if (found >= 0) { w = "an entry twice: " + name; break; }
        if (st.m_is_directory || st.m_is_encrypted || !st.m_is_supported) { w = "not a plain file: " + name; break; }
        if (st.m_uncomp_size == 0 || st.m_uncomp_size > (uint64_t)kAssetMaxBytes) { w = "an entry too large: " + name; break; }
        found = (int)i;
        size = st.m_uncomp_size;
    }
    if (w.empty() && found < 0) w = "the zip does not hold " + name;
    if (w.empty()) {
        size_t got = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, (mz_uint)found, &got, 0);
        if (!p || got != size) w = "an entry does not unpack: " + name;
        else if (!write_file(dest, std::string(static_cast<const char*>(p), got))) w = "the file cannot be written";
        if (p) mz_free(p);
    }
    mz_zip_reader_end(&z);
    return w.empty() ? true : fail(w);
}

// ---- the swap ----------------------------------------------------------------------------------------------------------------------
namespace {

struct Step {
    std::string rel, backup;      // backup "" = a file the game folder did not have
    std::wstring target, backup_w, staged;
    bool moved_old = false, moved_new = false;
};

std::string journal_text(const std::wstring& tree, const std::vector<Step>& steps) {
    json j = json::object();
    j["format"] = "sf6suite-swap-1";
    j["tree"] = narrow(tree);
    json arr = json::array();
    for (auto& s : steps) arr.push_back({ { "target", s.rel }, { "backup", s.backup } });
    j["steps"] = arr;
    return j.dump(1);
}

// Undo one step: the new file back into the staging tree (else deleted), the old one back in its place
bool undo(Step& s) {
    bool ok = true;
    if (s.moved_new) {
        if (!MoveFileExW(s.target.c_str(), s.staged.c_str(), MOVEFILE_REPLACE_EXISTING) && !DeleteFileW(s.target.c_str())) ok = false;
        else s.moved_new = false;
    }
    if (ok && s.moved_old) {
        if (!MoveFileExW(s.backup_w.c_str(), s.target.c_str(), 0)) ok = false;
        else s.moved_old = false;
    }
    return ok;
}

// The folders made for the swap, recorded so that an undo takes them away again (only when still empty)
bool make_dirs_tracked(const std::wstring& dir, std::vector<std::wstring>* made) {
    std::vector<std::wstring> missing;
    for (std::wstring d = dir; !d.empty() && !exists(d); d = parent_of(d)) missing.push_back(d);
    for (size_t k = missing.size(); k-- > 0;) {
        if (!CreateDirectoryW(missing[k].c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        made->push_back(missing[k]);
    }
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void remove_made(const std::vector<std::wstring>& made) {
    for (size_t k = made.size(); k-- > 0;) RemoveDirectoryW(made[k].c_str());
}

}  // namespace

SwapResult swap_in(const std::wstring& root, const std::wstring& tree, const Staged& staged, const std::wstring& journal, int fail_at) {
    SwapResult r;
    std::vector<Step> steps;
    std::set<std::string> taken;
    for (auto& f : staged.files) {
        Step s;
        s.rel = f.rel;
        s.target = join(root, f.rel);
        s.staged = join(tree, f.rel);
        if (!is_file(s.staged)) { r.why = "staged file missing: " + f.rel; return r; }
        const bool has = exists(s.target);
        if (has && !is_file(s.target)) { r.why = "a folder where a file goes: " + f.rel; return r; }
        if (f.kind == Entry::KeepIfPresent && has) { ++r.kept; continue; }
        if (has && same_content(s.target, s.staged)) { ++r.same; continue; }
        if (has) {
            const std::string dir = dir_of(f.rel), name = file_name_of(f.rel);
            for (int k = 0; k < 100; ++k) {
                const std::string b = (dir.empty() ? "" : dir + "/") + backup_name(name, f.from, k);
                if (!exists(join(root, b)) && !taken.count(lower(b))) { s.backup = b; break; }
            }
            if (s.backup.empty()) { r.why = "no free name for the old copy of " + f.rel; return r; }
            taken.insert(lower(s.backup));
            s.backup_w = join(root, s.backup);
        }
        steps.push_back(std::move(s));
    }
    std::vector<std::wstring> made;
    for (auto& d : staged.dirs) {
        const std::wstring p = join(root, d);
        if (exists(p)) continue;
        if (!make_dirs_tracked(p, &made)) { remove_made(made); r.why = "a folder cannot be made: " + d; return r; }
        ++r.dirs;
    }
    if (steps.empty()) { r.ok = true; return r; }
    if (!write_file(journal, journal_text(tree, steps))) { remove_made(made); r.why = "the swap journal cannot be written"; return r; }
    std::string why;
    size_t i = 0;
    for (; i < steps.size(); ++i) {
        Step& s = steps[i];
        if (!make_dirs_tracked(parent_of(s.target), &made)) { why = "a folder cannot be made for " + s.rel; break; }
        if (!s.backup.empty()) {
            if (!MoveFileExW(s.target.c_str(), s.backup_w.c_str(), 0)) { why = last_error_text(("the old " + s.rel + " cannot be renamed").c_str()); break; }
            s.moved_old = true;
        }
        if ((int)i == fail_at) { why = "test failure at step " + std::to_string(i) + " (" + s.rel + ")"; break; }
        if (!MoveFileExW(s.staged.c_str(), s.target.c_str(), 0)) { why = last_error_text(("the new " + s.rel + " cannot be moved in").c_str()); break; }
        s.moved_new = true;
    }
    if (i < steps.size()) {
        bool undone = true;
        for (size_t k = i + 1; k-- > 0;)
            if (!undo(steps[k])) undone = false;
        if (undone) {
            DeleteFileW(journal.c_str());               // else kept: the next start tries again (recover_journal)
            remove_made(made);
        }
        r.why = why + (undone ? ": nothing changed" : ": NOT all undone, the next start tries again");
        return r;
    }
    DeleteFileW(journal.c_str());
    for (auto& s : steps) {
        if (s.backup.empty()) ++r.added;
        else { ++r.replaced; r.backups.push_back(s.backup); }
    }
    r.ok = true;
    return r;
}

bool recover_journal(const std::wstring& root, const std::wstring& journal, std::string* log) {
    std::string text;
    if (!read_file(journal, &text, 4u << 20)) return false;
    std::string l = "swap journal found (an update stopped in the middle): undone";
    bool all = true;
    try {
        json j = json::parse(text);
        const std::wstring tree = widen(j.value("tree", std::string()));
        auto steps = j.at("steps");
        for (size_t k = steps.size(); k-- > 0;) {
            const std::string rel = steps[k].value("target", std::string()), backup = steps[k].value("backup", std::string());
            if (classify_entry(rel).kind == Entry::Refuse) continue;
            const std::wstring target = join(root, rel), staged = join(tree, rel);
            if (!backup.empty()) {
                const std::wstring b = join(root, backup);
                if (!is_backup_name(file_name_of(backup)) || !is_file(b)) continue;
                if (exists(target) && !MoveFileExW(target.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING) && !DeleteFileW(target.c_str())) { all = false; continue; }
                if (!MoveFileExW(b.c_str(), target.c_str(), 0)) all = false;
            } else if (is_file(target) && !is_file(staged)) {
                make_dirs(parent_of(staged));
                if (!MoveFileExW(target.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING) && !DeleteFileW(target.c_str())) all = false;
            }
        }
    } catch (...) {
        l = "swap journal unreadable: removed";
    }
    if (all) DeleteFileW(journal.c_str());
    else l += " (not all: kept for the next start)";
    if (log) *log = l;
    return true;
}

std::vector<std::string> cleanup_backups(const std::wstring& root, const std::vector<std::string>& list, std::string* log) {
    std::vector<std::string> left;
    int gone = 0;
    for (auto& rel : list) {
        // only an old copy this updater made: a backup name, a path a package may write to
        if (!is_backup_name(file_name_of(rel))) continue;
        std::string orig = rel.substr(0, lower(rel).rfind(".old-"));
        const Entry k = classify_entry(orig).kind;
        if (k != Entry::File && k != Entry::KeepIfPresent) continue;
        const std::wstring p = join(root, rel);
        if (!exists(p) || DeleteFileW(p.c_str())) { ++gone; continue; }
        left.push_back(rel);
    }
    if (log) *log = std::to_string(gone) + " old cop" + (gone == 1 ? "y" : "ies") + " removed" + (left.empty() ? "" : ", " + std::to_string(left.size()) + " still in use");
    return left;
}

}  // namespace sf6up
