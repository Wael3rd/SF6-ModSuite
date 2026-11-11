// SF6_ModSorter -- placement: a COPY into the game folder (the original stays where it was), never a silent overwrite.
// The copy is written beside its destination under a temporary name starting with a dot, then renamed into place; Replace
// moves the old copy aside first and deletes it only once the new one is in place. An installed costume moved between the
// two costume folders (move_costume_root) is renamed, never copied nor written over.

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <vector>

#include "sorter.hpp"

std::wstring destination_rel(const Classification& c, bool nsfw) {
    const std::wstring& dir = (nsfw && !c.dest_dir_nsfw.empty()) ? c.dest_dir_nsfw : c.dest_dir;
    return dir + L"\\" + c.dest_name;
}

bool destination_exists(const std::wstring& game_dir, const Classification& c, bool nsfw) {
    if (game_dir.empty() || c.cat == Category::Unknown) return false;
    const DWORD a = GetFileAttributesW(long_path_w(game_dir + L"\\" + destination_rel(c, nsfw)).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

namespace {

struct Copier {
    CopyProgress progress = nullptr;
    void* ctx = nullptr;
    uint64_t total = 0, done = 0, file_base = 0;
    DWORD error = 0;
};

DWORD CALLBACK on_progress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                           HANDLE, HANDLE, LPVOID data) {
    Copier* cp = static_cast<Copier*>(data);
    cp->done = cp->file_base + (uint64_t)transferred.QuadPart;
    if (cp->progress) cp->progress(cp->ctx, cp->done, cp->total);
    return PROGRESS_CONTINUE;
}

uint64_t size_of_tree(const std::wstring& src) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(long_path_w(src).c_str(), GetFileExInfoStandard, &fa)) return 0;
    if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    uint64_t n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(long_path_w(src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        n += size_of_tree(src + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

bool copy_file(const std::wstring& src, const std::wstring& dst, Copier& cp) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(long_path_w(src).c_str(), GetFileExInfoStandard, &fa);
    BOOL cancel = FALSE;
    if (!CopyFileExW(long_path_w(src).c_str(), long_path_w(dst).c_str(), on_progress, &cp, &cancel, COPY_FILE_FAIL_IF_EXISTS)) {
        cp.error = GetLastError();
        return false;
    }
    cp.file_base += (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    // a copy of a read-only original stays writable in the game folder (so it can be replaced or deleted later)
    const DWORD a = GetFileAttributesW(long_path_w(dst).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(long_path_w(dst).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    return true;
}

bool copy_tree(const std::wstring& src, const std::wstring& dst, Copier& cp) {
    if (!CreateDirectoryW(long_path_w(dst).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { cp.error = GetLastError(); return false; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(long_path_w(src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;   // an empty folder
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // links are not followed
        const std::wstring s = src + L"\\" + fd.cFileName, d = dst + L"\\" + fd.cFileName;
        ok = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? copy_tree(s, d, cp) : copy_file(s, d, cp);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool make_dirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(long_path_w(dir.substr(0, i)).c_str(), nullptr);
    return dir_exists_w(dir);
}

std::string win_error(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring w = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    char code[24];
    snprintf(code, sizeof code, "%lu", (unsigned long)e);
    return w.empty() ? std::string(code) : to_utf8(w) + " (" + code + ")";
}

void remove_any(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(long_path_w(p).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) { delete_tree(p); return; }
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(long_path_w(p).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    DeleteFileW(long_path_w(p).c_str());
}

}  // namespace

PlaceResult place_item(const std::wstring& game_dir, const Classification& c, bool nsfw, bool replace,
                       CopyProgress progress, void* progress_ctx) {
    PlaceResult r;
    if (game_dir.empty()) { r.error = {E_NO_GAME, {}}; return r; }
    if (c.cat == Category::Unknown) return r;
    const std::wstring rel = destination_rel(c, nsfw);
    r.dest = rel;
    const DWORD sa = GetFileAttributesW(long_path_w(c.path).c_str());
    if (sa == INVALID_FILE_ATTRIBUTES) { r.error = {E_SOURCE_GONE, {}}; return r; }
    const std::wstring dir = game_dir + L"\\" + ((nsfw && !c.dest_dir_nsfw.empty()) ? c.dest_dir_nsfw : c.dest_dir);
    const std::wstring target = game_dir + L"\\" + rel;
    const bool exists = GetFileAttributesW(long_path_w(target).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists && !replace) { r.state = PlaceResult::Skipped; return r; }
    if (!make_dirs(dir)) { r.error = {E_COPY, {win_error(GetLastError())}}; return r; }

    Copier cp;
    cp.progress = progress;
    cp.ctx = progress_ctx;
    cp.total = size_of_tree(c.path);
    // names starting with a dot: every loader skips them, so a copy cut short is never read as a mod
    const std::wstring tmp = dir + L"\\.sf6sorter_tmp_" + c.dest_name, old = dir + L"\\.sf6sorter_old_" + c.dest_name;
    remove_any(tmp);
    bool ok;
    if (sa & FILE_ATTRIBUTE_DIRECTORY) {
        ok = copy_tree(c.path, tmp, cp);
    } else {
        ok = copy_file(c.path, tmp, cp);
    }
    if (!ok) {
        r.error = {E_COPY, {win_error(cp.error)}};
        remove_any(tmp);
        return r;
    }
    if (exists) {
        remove_any(old);
        if (!MoveFileExW(long_path_w(target).c_str(), long_path_w(old).c_str(), 0)) {
            r.error = {E_REPLACE, {win_error(GetLastError())}};
            remove_any(tmp);
            return r;
        }
    }
    if (!MoveFileExW(long_path_w(tmp).c_str(), long_path_w(target).c_str(), 0)) {
        const DWORD e = GetLastError();
        if (exists) MoveFileExW(long_path_w(old).c_str(), long_path_w(target).c_str(), 0);   // the old copy back
        remove_any(tmp);
        r.error = {E_COPY, {win_error(e)}};
        return r;
    }
    if (exists) remove_any(old);
    r.state = PlaceResult::Installed;
    return r;
}

// A rename inside the game folder (one volume): nothing is copied, the mod is never in two places nor half moved
MoveResult move_costume_root(const std::wstring& game_dir, bool from_nsfw, const std::wstring& folder, const std::wstring& name) {
    MoveResult r;
    const std::wstring to_root = std::wstring(L"reframework\\") + (from_nsfw ? L"costume_mods" : L"costume_mods_nsfw");
    const std::wstring sub = folder.empty() ? std::wstring() : L"\\" + folder;
    r.dest = to_root + sub + L"\\" + name;
    if (game_dir.empty()) { r.error = {E_NO_GAME, {}}; return r; }
    const std::wstring from_dir = game_dir + L"\\reframework\\" + (from_nsfw ? L"costume_mods_nsfw" : L"costume_mods") + sub;
    const std::wstring root = game_dir + L"\\" + to_root, dir = root + sub;
    const std::wstring src = from_dir + L"\\" + name, dst = dir + L"\\" + name;
    if (GetFileAttributesW(long_path_w(dst).c_str()) != INVALID_FILE_ATTRIBUTES) {
        r.error = {T_NOTE_MOVE_EXISTS, {to_utf8(to_root + sub)}};
        return r;
    }
    const bool had_root = dir_exists_w(root), had_dir = dir_exists_w(dir);
    // the folders a failed move made go again (RemoveDirectoryW: only while they are empty)
    auto undo_dirs = [&]() {
        if (!had_dir && !folder.empty()) RemoveDirectoryW(long_path_w(dir).c_str());
        if (!had_root) RemoveDirectoryW(long_path_w(root).c_str());
    };
    if (!make_dirs(dir)) {
        r.error = {T_NOTE_MOVE_FAILED, {win_error(GetLastError())}};
        undo_dirs();
        return r;
    }
    // no MOVEFILE_REPLACE_EXISTING: a name that appeared meanwhile is not overwritten either
    if (!MoveFileExW(long_path_w(src).c_str(), long_path_w(dst).c_str(), 0)) {
        r.error = {T_NOTE_MOVE_FAILED, {win_error(GetLastError())}};
        undo_dirs();
        return r;
    }
    // the character folder it left, when nothing else is in it (RemoveDirectoryW never removes a folder that holds anything)
    if (!folder.empty()) RemoveDirectoryW(long_path_w(from_dir).c_str());
    r.ok = true;
    return r;
}
