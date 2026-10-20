// SF6_SlotsClean.exe: removes what the Slots Loader made at runtime in the game folder, and nothing else.
//
//   SF6_SlotsClean.exe [game folder] [--dry-run]
//
// The loader writes its patch paks (re_chunk_000.pak.patch_NNN.pak holding its costume or stage marker)
// and a few state files when the game starts. They stay when the loader's package is turned off in
// Fluffy Mod Manager (nothing runs to remove them), and Fluffy's re-read of the game archives after a
// game update records them as game files. Run this BEFORE that re-read: it takes the loader's paks and
// state out of the way and touches nothing of the user's (costume_mods, other mods' data, Fluffy's files).
//
// The game reads the patch paks as an unbroken row from patch_001 (a gap gives a black screen, see the
// launch mutex in proxy/slots_proxy.cpp). So a pak of ours is deleted only when no pak that is not ours
// has a higher number; otherwise it is replaced in place by a pak holding only the marker (same name):
// the row stays unbroken, nothing of ours is loaded, and the loader finds it again as its own.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "pak.hpp"

struct PatchPak { int num; std::string name, path; PatchPakKind kind; uint32_t entries; };

static const char* GAME_EXE = "StreetFighter6.exe";

static bool file_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool dir_exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool eq_nocase(const char* a, const char* b) { return _stricmp(a, b) == 0; }

// True while a Street Fighter 6 process exists. SF6_SLOTSCLEAN_FAKE_RUNNING=1 simulates one (tests).
static bool game_running() {
    char env[8];
    if (GetEnvironmentVariableA("SF6_SLOTSCLEAN_FAKE_RUNNING", env, sizeof(env)) && env[0] == '1') return true;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
        found = eq_nocase(pe.szExeFile, GAME_EXE);
    CloseHandle(snap);
    return found;
}

// Started by a double-click: the console belongs to this process alone and closes when it exits.
static bool own_console() {
    DWORD pids[4];
    return GetConsoleProcessList(pids, 4) <= 1;
}

static int finish(int code, bool wait) {
    if (wait) { printf("\nPress Enter to close..."); fflush(stdout); getchar(); }
    return code;
}

static std::string exe_folder() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf, n);
    size_t sl = s.find_last_of("\\/");
    return sl == std::string::npos ? "." : s.substr(0, sl);
}

static std::vector<PatchPak> list_patch_paks(const std::string& dir) {
    std::vector<PatchPak> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\re_chunk_000.pak.patch_*.pak").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        size_t ps = name.find("patch_");
        if (ps == std::string::npos) continue;
        PatchPak p{ atoi(name.c_str() + ps + 6), name, dir + "\\" + name, PatchPakKind::Mod, 0 };
        p.kind = classify_patch_pak(p.path.c_str(), &p.entries);
        out.push_back(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const PatchPak& a, const PatchPak& b) { return a.num < b.num; });
    return out;
}

static std::string read_first_line(const std::string& path) {
    std::string s;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[1024]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
        s.assign(buf, n);
    }
    size_t e = s.find_first_of("\r\n");
    if (e != std::string::npos) s.resize(e);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

int main(int argc, char** argv) {
    const bool wait = own_console();
    bool dry = false;
    std::string game;
    for (int i = 1; i < argc; i++) {
        if (eq_nocase(argv[i], "--dry-run")) dry = true;
        else if (argv[i][0] == '-') {
            printf("Unknown option %s\nUsage: SF6_SlotsClean.exe [game folder] [--dry-run]\n", argv[i]);
            return finish(2, wait);
        } else game = argv[i];
    }
    if (game.empty()) game = exe_folder();
    while (game.size() > 3 && (game.back() == '\\' || game.back() == '/')) game.pop_back();

    printf("SF6 Slots Clean%s\nGame folder: %s\n\n", dry ? " (dry run: nothing is changed)" : "", game.c_str());
    if (!dir_exists(game) || !file_exists(game + "\\re_chunk_000.pak")) {
        printf("ERROR: this is not the Street Fighter 6 folder (no re_chunk_000.pak). Put SF6_SlotsClean.exe there,\n"
               "or give the folder as the first argument.\n");
        return finish(1, wait);
    }
    if (game_running()) {
        printf("ERROR: %s is running. Close the game (check the Task Manager), then run this again.\n", GAME_EXE);
        return finish(1, wait);
    }

    int errors = 0, deleted = 0, replaced = 0, kept = 0, state_removed = 0;

    // ---- 1. Patch paks of the loader, found by their marker, never by number ----
    auto paks = list_patch_paks(game);
    int top_foreign = 0;   // highest number of a pak that is not ours (an unreadable one counts as foreign)
    for (auto& p : paks)
        if (p.kind == PatchPakKind::Mod || p.kind == PatchPakKind::Unreadable) top_foreign = (std::max)(top_foreign, p.num);
    std::vector<std::string> our_names;
    printf("Patch paks:\n");
    for (auto& p : paks) {
        if (p.kind != PatchPakKind::Costume && p.kind != PatchPakKind::Stage) {
            printf("  %s: not ours, left as it is\n", p.name.c_str());
            continue;
        }
        const char* what = p.kind == PatchPakKind::Costume ? "costume slots" : "stage slots";
        our_names.push_back(p.name);
        if (p.num > top_foreign) {
            printf("  %s: %s pak, no pak of another mod above it -> %s\n", p.name.c_str(), what, dry ? "would delete" : "delete");
            if (dry) deleted++;
            else if (DeleteFileA(p.path.c_str())) deleted++;
            else { printf("    ERROR: cannot delete it (error %lu)\n", GetLastError()); errors++; }
        } else if (p.entries == 1) {
            printf("  %s: %s pak below a pak of another mod, already marker-only -> kept\n", p.name.c_str(), what);
            kept++;
        } else {
            printf("  %s: %s pak below a pak of another mod (patch_%03d) -> %s by a marker-only pak (keeps the row unbroken)\n",
                   p.name.c_str(), what, top_foreign, dry ? "would replace" : "replace");
            if (dry) { replaced++; continue; }
            const std::string tmp = p.path + ".slotsclean.tmp";
            if (write_marker_pak(tmp.c_str(), p.kind) && MoveFileExA(tmp.c_str(), p.path.c_str(), MOVEFILE_REPLACE_EXISTING)) replaced++;
            else { printf("    ERROR: cannot replace it (error %lu)\n", GetLastError()); DeleteFileA(tmp.c_str()); errors++; }
        }
    }
    if (our_names.empty()) printf("  no pak of the loader found\n");

    // ---- 2. Runtime state of the loaders. Shipped files (loader\), costume_mods, logs and other data stay. ----
    const std::string costume_data = game + "\\reframework\\data\\SF6_Costumes_Data";
    const std::string stage_data = game + "\\reframework\\data\\SF6_StageSlots_Data";
    const std::string state[] = {
        costume_data + "\\registry.json", costume_data + "\\outfits.json", costume_data + "\\pak_sources.tsv",
        stage_data + "\\registry.json", stage_data + "\\loader\\variants.tsv",
    };
    printf("\nRuntime state:\n");
    for (auto& f : state) {
        if (!file_exists(f)) continue;
        printf("  %s -> %s\n", f.c_str() + game.size() + 1, dry ? "would delete" : "delete");
        if (dry || DeleteFileA(f.c_str())) state_removed++;
        else { printf("    ERROR: cannot delete it (error %lu)\n", GetLastError()); errors++; }
    }
    if (!state_removed) printf("  none present\n");

    // ---- 3. Fluffy's ArchiveList.bin (read only) ----
    const std::string fluffy = read_first_line(costume_data + "\\fluffy_location.txt");
    printf("\nFluffy Mod Manager:\n");
    if (fluffy.empty()) {
        printf("  its folder is not known (no fluffy_location.txt): if you use Fluffy, let it re-read the game\n"
               "  archives after this clean-up.\n");
    } else {
        auto hits = archive_list_hits(fluffy + "\\Games\\SF6\\ArchiveList.bin", our_names);
        if (hits.empty()) {
            printf("  %s\\Games\\SF6\\ArchiveList.bin does not list the loader's paks.\n", fluffy.c_str());
        } else {
            printf("  NOTE: Fluffy recorded the slots' paks as game files (ArchiveList.bin lists:");
            for (auto& h : hits) printf(" %s", h.c_str());
            printf(").\n  After this clean-up Fluffy must re-read the game archives (Fluffy's files are not modified).\n");
        }
    }

    printf("\n%s: %d pak(s) %s, %d replaced by a marker-only pak, %d already marker-only, %d state file(s) %s, %d error(s).\n",
           dry ? "Dry run" : "Done", deleted, dry ? "to delete" : "deleted", replaced, kept, state_removed,
           dry ? "to delete" : "deleted", errors);
    if (!dry && !errors)
        printf("Next: let Fluffy re-read the game archives if it asks, then launch the game: the loader rebuilds the\n"
               "slots from reframework\\costume_mods.\n");
    return finish(errors ? 1 : 0, wait);
}
