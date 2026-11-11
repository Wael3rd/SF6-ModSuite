// SF6_ModSorter -- drop any downloaded mod on it: it says what the mod is and copies it into the right folder of the game.
//
//   SF6_ModSorter.exe                        the window
//   SF6_ModSorter.exe --classify <path>...   no window: one JSON line per path (category, character, reason, destination...)
//   SF6_ModSorter.exe --shot <out.png> [--items <path>...] [--lang <code>] [--select N | --select-sub N] [--install]
//                     [--nsfw N] [--replace N] [--size WxH] [--page installed|options] [--lang-step N] [--category N] [--running 0|1|2]
//                     [--move] [--maximized] [--update check|checking|installing|install|downloading|self-downloading]
//                     [--manifest <file>]
//                                            renders the window offscreen at 100 % into a PNG and exits: no window, no focus
//                                            (--page installed: the Installed tab, --category N its selected category;
//                                            --page options: the Options tab, --lang-step N its Language row pressed N
//                                            times to the right (N < 0: to the left), remembered as in the window;
//                                            --running: as if the game were not running, running, or being started;
//                                            --move: N pressed on the selected picture of the Installed tab first (it moves
//                                            the mod, test folders only); --maximized: the header as in a maximised window;
//                                            --update: the Options tab's update rows after the look at the latest release,
//                                            as if looking, as if installing, after the suite's install (test folders
//                                            only), or 42 % into a download (the suite's, the sorter's own);
//                                            --select 1 selects the sorter's row, 2 the suite's; --manifest: the release's
//                                            manifest from a file)
//   SF6_ModSorter.exe --check-updates [--game <folder>] [--manifest <file>]
//                                            no window: the suite's packages in the game folder against GitHub's latest
//                                            release, one JSON line (state, latest, lines old -> new), then one for the
//                                            sorter itself (sorter, current, latest)
//   SF6_ModSorter.exe --install-update --game <folder>
//                                            tests: the same, then what is offered is installed (refused while the game runs)
//   SF6_ModSorter.exe --install-self --manifest <file> --assets <folder>
//                                            tests, on a COPY of the exe: the sorter's own update from a manifest file and
//                                            a folder holding the zip it names; the copy replaces itself (not started again)
//   SF6_ModSorter.exe --version              no window: the sorter's version, one line
//   SF6_ModSorter.exe --manifest <file> --assets <folder>
//                                            tests, on a COPY of the exe: the window, the release read from that file; the
//                                            sorter's own update is then installed from the folder (F on its row) and the
//                                            copy starts again; the suite's packages are never installed from a file
//   --game <folder>                          the game folder for this run (tests); otherwise the remembered one, the exe's own
//                                            folder or Steam's
//   --lang <code>                            the language: en fr ja zh-Hans it de es es-419 ru pl pt ko zh-Hant ar
//                                            (default: the one chosen on the Options tab, else Windows' display
//                                            language, English when it is none of them)
//   --list <archive>                         tests: the names the archive holds, one per line
//   --unpack <archive> <folder>              tests: unpacks every entry (common/archive.cpp), nothing is run
//   --classify ... --verbose                 tests: each line also has the costume loader's own log lines
//   --move-nsfw <mod> --game <folder>        tests: the Installed page's N without the window: <mod> (a mod of the game
//                                            folder's costume_mods or costume_mods_nsfw) moved into the other one; one line
//
// See classify.cpp for what goes where, probes.hpp for the loaders' code it runs.

#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

#include "archive.hpp"
#include "sorter.hpp"
#include "suite_update.hpp"
#include "ui.hpp"
#include "version.hpp"

namespace {

HANDLE g_out = INVALID_HANDLE_VALUE;   // where --classify writes (the console or the pipe it was started with)

void out_line(const std::string& s) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    const std::string l = s + "\n";
    DWORD w = 0;
    WriteFile(g_out, l.data(), (DWORD)l.size(), &w, nullptr);
}

void open_output() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &g_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    // started from a console without a redirection: a GUI program has none of its own
    if (AttachConsole(ATTACH_PARENT_PROCESS))
        g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

// %TEMP%\SF6_ModSorter\<pid>: archives are unpacked there while they are read; removed at exit. Folders left by a sorter that
// is not running any more are removed too.
std::wstring make_work_dir() {
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    std::wstring base = (n > 0 ? std::wstring(tmp, n) : std::wstring(L".\\")) + L"SF6_ModSorter";
    CreateDirectoryW(base.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const DWORD pid = wcstoul(fd.cFileName, nullptr, 10);
            if (!pid || pid == GetCurrentProcessId()) continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            bool alive = false;
            if (p) {
                wchar_t img[MAX_PATH];
                DWORD len = MAX_PATH;
                alive = QueryFullProcessImageNameW(p, 0, img, &len) && wcsstr(img, L"SF6_ModSorter") != nullptr;
                CloseHandle(p);
            }
            if (!alive) delete_tree(base + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const std::wstring dir = base + L"\\" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring full_path(const std::wstring& p) {
    wchar_t buf[32768];
    const DWORD n = GetFullPathNameW(p.c_str(), 32768, buf, nullptr);
    return n > 0 && n < 32768 ? std::wstring(buf, n) : p;
}

// --move-nsfw: <game>\reframework\costume_mods[_nsfw]\[<folder>\]<name> moved into the other costume folder, as the
// Installed page's N does. One line, in English: "moved <where>" or "error: <why> [<where>]".
int move_nsfw(std::wstring game_dir, const std::wstring& path) {
    while (!game_dir.empty() && game_dir.back() == L'\\') game_dir.pop_back();
    const std::wstring base = lower_w(game_dir + L"\\reframework\\"), l = lower_w(path);
    const std::wstring pn = base + L"costume_mods_nsfw\\", pc = base + L"costume_mods\\";
    auto starts = [&](const std::wstring& pre) { return l.size() > pre.size() && l.compare(0, pre.size(), pre) == 0; };
    bool from_nsfw = false;
    std::wstring rest;
    if (starts(pn)) { from_nsfw = true; rest = path.substr(pn.size()); }
    else if (starts(pc)) rest = path.substr(pc.size());
    while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
    const size_t s = rest.find(L'\\');
    if (game_dir.empty() || rest.empty() || (s != std::wstring::npos && rest.find(L'\\', s + 1) != std::wstring::npos)) {
        out_line("error: not <game>\\reframework\\costume_mods[_nsfw]\\[<folder>\\]<mod>");
        return 1;
    }
    const bool root = s == std::wstring::npos;
    const MoveResult r = move_costume_root(game_dir, from_nsfw, root ? std::wstring() : rest.substr(0, s), root ? rest : rest.substr(s + 1));
    out_line(r.ok ? "moved " + to_utf8(r.dest) : "error: " + tr_en(r.error) + " [" + to_utf8(r.dest) + "]");
    return r.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    UiOptions o;
    std::wstring game, lang;
    bool classify_mode = false, game_given = false, verbose = false, move_mode = false;
    int update_mode = 0;   // 1: --check-updates, 2: --install-update, 3: --install-self
    std::wstring move_path;
    std::vector<std::wstring> classify_paths;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        auto next = [&]() -> std::wstring { return i + 1 < args.size() ? args[++i] : std::wstring(); };
        auto is_flag = [&](size_t k) { return k < args.size() && args[k].size() > 2 && args[k][0] == L'-' && args[k][1] == L'-'; };
        if (a == L"--list") {   // tests: the names an archive holds (archive_list: its headers only)
            open_output();
            std::vector<std::wstring> names;
            const bool ok = archive_list(full_path(next()), names);
            for (auto& n : names) out_line(to_utf8(n));
            if (!ok) out_line("(not a readable archive)");
            return ok ? 0 : 1;
        } else if (a == L"--unpack") {   // tests: every entry of an archive into a folder, through the archive code
            open_output();
            const std::wstring arc = full_path(next()), dest = full_path(next());
            const ArchiveResult r = archive_extract(arc, dest, [](const std::wstring&) { return true; });
            out_line(r.ok ? "ok " + std::to_string(r.files_written) + " files" : "error: " + r.error);
            return r.ok ? 0 : 1;
        } else if (a == L"--classify") {
            classify_mode = true;
            while (i + 1 < args.size() && !is_flag(i + 1)) classify_paths.push_back(full_path(args[++i]));
        } else if (a == L"--move-nsfw") { move_mode = true; move_path = full_path(next()); }
        else if (a == L"--shot") o.shot = full_path(next());
        else if (a == L"--items") { while (i + 1 < args.size() && !is_flag(i + 1)) o.items.push_back(full_path(args[++i])); }
        else if (a == L"--lang") lang = next();
        else if (a == L"--select") o.select = _wtoi(next().c_str());
        else if (a == L"--select-sub") { o.select = _wtoi(next().c_str()); o.select_sub = true; }
        else if (a == L"--install") o.install = true;
        else if (a == L"--move") o.move = true;
        else if (a == L"--maximized") o.maximized = true;
        else if (a == L"--verbose") verbose = true;
        else if (a == L"--nsfw") o.nsfw_yes.push_back(_wtoi(next().c_str()));
        else if (a == L"--replace") o.replace.push_back(_wtoi(next().c_str()));
        else if (a == L"--game") { game = full_path(next()); game_given = true; }
        else if (a == L"--page") { const std::wstring p = next(); o.page = p == L"installed" ? 1 : p == L"options" ? 2 : 0; }
        else if (a == L"--category") o.category = _wtoi(next().c_str());
        else if (a == L"--lang-step") o.lang_step = _wtoi(next().c_str());
        else if (a == L"--check-updates") update_mode = 1;
        else if (a == L"--install-update") update_mode = 2;
        else if (a == L"--install-self") update_mode = 3;
        else if (a == L"--assets") o.assets = full_path(next());
        else if (a == L"--version") { open_output(); out_line(kSorterVersion); return 0; }
        else if (a == L"--manifest") o.manifest = full_path(next());
        else if (a == L"--update") {
            const std::wstring u = next();
            o.update = u == L"check" ? 1 : u == L"checking" ? 2 : u == L"installing" ? 3 : u == L"install" ? 4 :
                       u == L"downloading" ? 5 : u == L"self-downloading" ? 6 : 0;
        }
        else if (a == L"--running") { const int v = _wtoi(next().c_str()); o.running = v < 0 ? 0 : v > 2 ? 2 : v; }
        else if (a == L"--size") {
            const std::wstring s = next();
            const size_t x = s.find_first_of(L"xX");
            if (x != std::wstring::npos) { o.width = (float)_wtof(s.substr(0, x).c_str()); o.height = (float)_wtof(s.substr(x + 1).c_str()); }
            if (o.width < 900) o.width = 900;
            if (o.height < 580) o.height = 580;
        } else if (!a.empty() && a[0] != L'-') o.items.push_back(full_path(a));   // dropped on the exe's icon
    }

    // --lang, else the language chosen on the Options tab, else Windows' display language
    if ((lang.empty() || !set_language(to_utf8(lang))) && !set_language(settings_language()))
        set_language_index(language_from_windows(GetUserDefaultUILanguage()));

    // The loaders' code prints its log lines with printf: they go nowhere (each probe keeps its own)
    open_output();
    FILE* nul = nullptr;
    _wfreopen_s(&nul, L"NUL", L"w", stdout);

    if (game_given) o.game_dir = game;
    else {
        o.game_dir = settings_game_dir();
        if (!is_game_dir(o.game_dir)) o.game_dir = detect_game_dir();
    }
    o.work_dir = make_work_dir();

    int rc = 0;
    if (move_mode) {
        rc = move_nsfw(o.game_dir, move_path);
    } else if (update_mode) {
        // what the Options tab's updates row would say; --install-update: then what F does there
        SelfCheck me;
        const SuiteCheck c = suite_check(o.game_dir, true, o.manifest, &me);
        out_line(suite_check_json(c));
        out_line(self_check_json(me));
        if (update_mode != 3 && (c.state == SuiteCheck::Failed || c.state == SuiteCheck::NoGame)) rc = 1;
        if (update_mode == 2 && c.state == SuiteCheck::Available) {
            const SuiteInstall r = suite_install(o.game_dir, c, language(), o.running >= 0 ? o.running == 1 : game_process_running());
            std::string said = r.message;
            for (char& ch : said) if (ch == '\n') ch = ' ';
            out_line(r.ok ? (r.nothing ? "nothing left to install: " : "installed: ") + r.version : "error: " + r.why + " [" + said + "]");
            rc = r.ok ? 0 : 1;
        }
        if (update_mode == 3) {   // the sorter itself, from a folder: never from GitHub here, never without --assets
            SelfInstall r;
            if (o.assets.empty() || o.manifest.empty()) r.why = "--install-self needs --manifest and --assets";
            else if (me.state != SelfCheck::Available) r.why = "no newer Mod Sorter in the manifest";
            else r = self_install(me, language(), nullptr, L"", o.assets);
            std::string said = r.message;
            for (char& ch : said) if (ch == '\n') ch = ' ';
            out_line(r.ok ? "installed: Mod Sorter " + r.version : "error: " + r.why + " [" + said + "]");
            rc = r.ok ? 0 : 1;
        }
    } else if (classify_mode) {
        ClassifyContext ctx;
        ctx.game_dir = o.game_dir;
        ctx.work_dir = o.work_dir;
        classify_init(ctx);
        int seq = 0;
        for (auto& p : classify_paths) out_line(classification_json(classify(p, ctx, ++seq), ctx, verbose));
    } else if (!o.shot.empty()) {
        rc = run_shot(o);
    } else {
        rc = run_window(o);
    }
    delete_tree(o.work_dir);
    if (g_out != INVALID_HANDLE_VALUE) CloseHandle(g_out);
    CoUninitialize();
    return rc;
}
