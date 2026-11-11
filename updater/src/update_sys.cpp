// SF6 Mod Suite updater: Windows' side (update_sys.hpp).
#include "update_sys.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "update_logic.hpp"

#pragma comment(lib, "user32.lib")

namespace sf6up {

std::wstring exe_folder() {
    std::wstring p(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size());
    p.resize(n);
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

bool is_program(const std::string& rel) {
    const std::string l = lower(rel);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, ".dll") == 0 || l.compare(l.size() - 4, 4, ".exe") == 0);
}

SacResult sac_check(const std::wstring& path) {
    SacResult r;
    const std::wstring probe = path + L".sacchk.dll";
    if (!MoveFileExW(path.c_str(), probe.c_str(), 0)) { r.error = GetLastError(); return r; }
    SetLastError(0);
    HMODULE h = LoadLibraryExW(probe.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const DWORD e = GetLastError();
    if (h) FreeLibrary(h);
    const bool back = MoveFileExW(probe.c_str(), path.c_str(), 0) != 0;
    if (h) r.verdict = back ? Sac::Ok : Sac::Bad;
    else {
        r.error = e;
        r.verdict = e == 4551 /* ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION */ ? Sac::Blocked : Sac::Bad;
    }
    return r;
}

std::wstring waiter_command(const std::wstring& system_dir, unsigned long pid, const std::wstring& image, const std::wstring& go_cmd) {
    const std::wstring sys = system_dir;
    const std::wstring id = std::to_wstring(pid);
    // for 120 rounds of about a second: the process gone (tasklist lists it no more) = wait 8 s (Steam sees the game closed), run `go`,
    // end; else wait 1 s.
    // Past the 120 rounds (the game was not closed): it ends, nothing is opened.
    std::wstring check = L"(\"" + sys + L"\\tasklist.exe\" /nh /fi \"PID eq " + id + L"\" /fi \"IMAGENAME eq " + image + L"\" | \"" + sys +
                         L"\\find.exe\" /i \"" + image + L"\" >nul)";
    std::wstring go = L"(\"" + sys + L"\\PING.EXE\" -n 9 127.0.0.1 >nul & " + go_cmd + L" & exit)";
    std::wstring wait = L"\"" + sys + L"\\PING.EXE\" -n 2 127.0.0.1 >nul";
    return L"\"" + sys + L"\\cmd.exe\" /d /s /c \"for /l %i in (1,1,120) do @((" + check + L" || " + go + L") & " + wait + L")\"";
}

bool start_waiter_with(unsigned long pid, const std::wstring& image, const std::wstring& go, std::string* why) {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) { if (why) *why = "no system folder"; return false; }
    const std::wstring app = std::wstring(sys) + L"\\cmd.exe";
    std::wstring cmd = waiter_command(sys, pid, image, go);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, sys, &si, &pi);
    if (!ok) {
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;           // a job that does not allow it: the waiter stays in it
        cmd = waiter_command(sys, pid, image, go);
        ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, sys, &si, &pi);
    }
    if (!ok) {
        if (why) { char b[64]; std::snprintf(b, sizeof b, "cmd.exe did not start (error %lu)", GetLastError()); *why = b; }
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool start_waiter(unsigned long pid, const std::wstring& image, std::string* why) {
    return start_waiter_with(pid, image, std::wstring(L"start \"\" \"") + kSteamRun + L"\"", why);
}

namespace {
// The game's window: a visible top-level window of this process, the RE Engine's class ("via") first, never a console
struct Found { HWND via = nullptr, other = nullptr; };
BOOL CALLBACK find_main(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    auto* f = reinterpret_cast<Found*>(lp);
    if (!wcscmp(cls, L"via")) { f->via = h; return FALSE; }
    if (wcscmp(cls, L"ConsoleWindowClass") && !f->other) f->other = h;
    return TRUE;
}
}  // namespace

bool close_game_window() {
    Found f;
    EnumWindows(find_main, reinterpret_cast<LPARAM>(&f));
    HWND main = f.via ? f.via : f.other;
    return main && PostMessageW(main, WM_CLOSE, 0, 0);
}

}  // namespace sf6up
