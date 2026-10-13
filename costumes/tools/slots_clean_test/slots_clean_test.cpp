// Tests SF6_SlotsClean.exe on fake game folders built under <work dir>; never on a real game.
//
//   slots_clean_test.exe <SF6_SlotsClean.exe> <work dir>
//
// Each case builds a folder with synthetic paks (PakWriter: ours costume, ours stage, a foreign one),
// state files, user mods and a fake Fluffy folder, runs the tool and checks the result.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "pak.hpp"

static int g_fail = 0, g_checks = 0;
static std::string g_tool, g_work;

static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) { g_fail++; printf("  FAIL: %s\n", what); }
}

static void make_dirs(const std::string& path) {   // every folder of a file path
    for (size_t i = 3; i < path.size(); i++)
        if (path[i] == '\\' || path[i] == '/') CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
}

static void put(const std::string& path, const std::string& data) {
    make_dirs(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}

static std::string slurp(const std::string& path) {
    std::string s;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char b[65536]; size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
        fclose(f);
    }
    return s;
}

static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Synthetic pak: the marker of a loader (or none) plus a few plain entries.
static void make_pak(const std::string& path, PatchPakKind kind, int extra) {
    make_dirs(path);
    PakWriter w;
    if (kind == PatchPakKind::Costume) w.add_uncompressed(pak_path_hash(std::string_view(COSTUME_SLOTS_MARKER)), {'c', 'o', 's'});
    if (kind == PatchPakKind::Stage) w.add_uncompressed(pak_path_hash(std::string_view(STAGE_SLOTS_MARKER)), {'s', 't', 'g'});
    for (int i = 0; i < extra; i++)
        w.add_uncompressed(pak_path_hash(std::string_view("natives/stm/test/file" + std::to_string(i) + "_" + path.substr(path.size() - 8))),
                           std::vector<uint8_t>(100 + i, uint8_t(i + 1)));
    w.write(path.c_str());
}

static std::string pak_name(int n) {
    char b[64]; sprintf(b, "re_chunk_000.pak.patch_%03d.pak", n);
    return b;
}

static void recurse(const std::string& root, const std::string& rel, std::map<std::string, std::string>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + rel + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) recurse(root, rel + "\\" + n, out);
        else out[rel + "\\" + n] = slurp(root + rel + "\\" + n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static std::map<std::string, std::string> snapshot(const std::string& root) {
    std::map<std::string, std::string> m;
    recurse(root, "", m);
    return m;
}

// A fake game folder. Fluffy: ArchiveList.bin listing the given names, folder named in fluffy_location.txt.
struct Game { std::string root, fluffy; };

static Game make_game(const char* name, const std::vector<std::string>& listed) {
    Game g;
    g.root = g_work + "\\" + name + "\\game";
    g.fluffy = g_work + "\\" + name + "\\fluffy";
    put(g.root + "\\re_chunk_000.pak", "base");
    put(g.root + "\\SF6_CostumeLoader.log", "log");
    put(g.root + "\\reframework\\costume_mods\\Ryu\\Cool.zip", "user mod");
    put(g.root + "\\reframework\\costume_mods\\settings.ini", "FluffyModManager=\r\n");
    put(g.root + "\\reframework\\data\\OtherMod\\save.json", "other mod data");
    put(g.root + "\\reframework\\data\\SF6_Costumes_Data\\loader\\vanilla_costume_index.tsv", "shipped");
    put(g.root + "\\reframework\\data\\SF6_Costumes_Data\\registry.json", "{}");
    put(g.root + "\\reframework\\data\\SF6_Costumes_Data\\outfits.json", "{}");
    put(g.root + "\\reframework\\data\\SF6_Costumes_Data\\fluffy_location.txt", g.fluffy);
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt", "shipped");
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\loader\\variants.tsv", "#fingerprint\tx\n");
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\registry.json", "{}");
    std::string list("ARCH\0\0\0\0", 8);
    list += "re_chunk_000.pak";
    list.push_back('\0');
    for (auto& n : listed) { list += n; list.push_back('\0'); }
    put(g.fluffy + "\\Games\\SF6\\ArchiveList.bin", list);
    put(g.fluffy + "\\Games\\SF6\\installed.ini", "[x]");
    return g;
}

// Runs the tool with its output in a file; returns the exit code.
static int run_tool(const Game& g, const char* args, std::string& out, bool fake_running = false) {
    SetEnvironmentVariableA("SF6_SLOTSCLEAN_FAKE_RUNNING", fake_running ? "1" : nullptr);
    std::string cmd = "cmd /c \"\"" + g_tool + "\" \"" + g.root + "\" " + args + " > \"" + g_work + "\\tool_output.txt\" 2>&1\"";
    int rc = system(cmd.c_str());
    out = slurp(g_work + "\\tool_output.txt");
    return rc;
}

static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

static const char* STATE[] = {
    "\\reframework\\data\\SF6_Costumes_Data\\registry.json", "\\reframework\\data\\SF6_Costumes_Data\\outfits.json",
    "\\reframework\\data\\SF6_StageSlots_Data\\registry.json", "\\reframework\\data\\SF6_StageSlots_Data\\loader\\variants.tsv",
};
static const char* KEPT[] = {
    "\\re_chunk_000.pak", "\\SF6_CostumeLoader.log", "\\reframework\\costume_mods\\Ryu\\Cool.zip",
    "\\reframework\\costume_mods\\settings.ini", "\\reframework\\data\\OtherMod\\save.json",
    "\\reframework\\data\\SF6_Costumes_Data\\loader\\vanilla_costume_index.tsv",
    "\\reframework\\data\\SF6_Costumes_Data\\fluffy_location.txt",
    "\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt",
};

static void check_state_gone_rest_kept(const Game& g, const std::map<std::string, std::string>& before) {
    for (auto s : STATE) check(!exists(g.root + s), s);
    for (auto k : KEPT) check(slurp(g.root + k) == before.at(k), k);
}

static void check_fluffy_untouched(const Game& g, const std::map<std::string, std::string>& fb) {
    check(snapshot(g.fluffy) == fb, "Fluffy folder untouched");
}

static int run_clean_tests(const char* tool, const char* work) {
    g_tool = tool; g_work = work;
    std::string out;

    printf("[1] ours on top (stage 002, costume 003 above foreign 001): both deleted\n");
    {
        Game g = make_game("c1", { pak_name(2), pak_name(3) });
        make_pak(g.root + "\\" + pak_name(1), PatchPakKind::Mod, 3);
        make_pak(g.root + "\\" + pak_name(2), PatchPakKind::Stage, 2);
        make_pak(g.root + "\\" + pak_name(3), PatchPakKind::Costume, 5);
        auto before = snapshot(g.root); auto fb = snapshot(g.fluffy);
        int rc = run_tool(g, "", out);
        check(rc == 0, "exit code 0");
        check(!exists(g.root + "\\" + pak_name(2)) && !exists(g.root + "\\" + pak_name(3)), "our paks deleted");
        check(slurp(g.root + "\\" + pak_name(1)) == before.at("\\" + pak_name(1)), "foreign pak untouched");
        check_state_gone_rest_kept(g, before);
        check_fluffy_untouched(g, fb);
        check(has(out, "must re-read the game archives"), "ArchiveList note printed");
        check(has(out, pak_name(3).c_str()) && has(out, pak_name(2).c_str()), "note names the paks");
    }

    printf("[2] ours below a foreign pak: replaced by a marker-only pak, same name\n");
    {
        Game g = make_game("c2", {});
        make_pak(g.root + "\\" + pak_name(1), PatchPakKind::Mod, 3);
        make_pak(g.root + "\\" + pak_name(2), PatchPakKind::Costume, 6);
        make_pak(g.root + "\\" + pak_name(3), PatchPakKind::Mod, 2);
        auto before = snapshot(g.root); auto fb = snapshot(g.fluffy);
        int rc = run_tool(g, "", out);
        check(rc == 0, "exit code 0");
        const std::string p2 = g.root + "\\" + pak_name(2);
        check(exists(p2), "pak 002 still there (row unbroken)");
        check(classify_patch_pak(p2.c_str()) == PatchPakKind::Costume, "still recognised as ours");
        PakReader r;
        check(r.open(p2.c_str()) && r.entry_count() == 1, "marker only: one entry");
        check(slurp(g.root + "\\" + pak_name(1)) == before.at("\\" + pak_name(1)), "foreign 001 untouched");
        check(slurp(g.root + "\\" + pak_name(3)) == before.at("\\" + pak_name(3)), "foreign 003 untouched");
        check(!exists(p2 + ".slotsclean.tmp"), "no temp file left");
        check_state_gone_rest_kept(g, before);
        check_fluffy_untouched(g, fb);
        check(!has(out, "must re-read"), "no Fluffy note when ArchiveList does not list our paks");
        // second run: nothing more to do to it
        auto mid = snapshot(g.root);
        rc = run_tool(g, "", out);
        check(rc == 0 && snapshot(g.root) == mid && has(out, "already marker-only"), "second run changes nothing");
    }

    printf("[3] mixed: stage below foreign (replaced), costume above everything (deleted)\n");
    {
        Game g = make_game("c3", {});
        make_pak(g.root + "\\" + pak_name(1), PatchPakKind::Mod, 1);
        make_pak(g.root + "\\" + pak_name(2), PatchPakKind::Stage, 4);
        make_pak(g.root + "\\" + pak_name(3), PatchPakKind::Mod, 1);
        make_pak(g.root + "\\" + pak_name(4), PatchPakKind::Costume, 4);
        int rc = run_tool(g, "", out);
        check(rc == 0, "exit code 0");
        const std::string p2 = g.root + "\\" + pak_name(2);
        PakReader r;
        check(classify_patch_pak(p2.c_str()) == PatchPakKind::Stage && r.open(p2.c_str()) && r.entry_count() == 1, "stage pak marker-only");
        check(!exists(g.root + "\\" + pak_name(4)), "costume pak on top deleted");
        check(exists(g.root + "\\" + pak_name(1)) && exists(g.root + "\\" + pak_name(3)), "foreign paks kept");
    }

    printf("[4] dry run: lists, changes nothing\n");
    {
        Game g = make_game("c4", { pak_name(3) });
        make_pak(g.root + "\\" + pak_name(1), PatchPakKind::Costume, 3);
        make_pak(g.root + "\\" + pak_name(2), PatchPakKind::Mod, 2);
        make_pak(g.root + "\\" + pak_name(3), PatchPakKind::Stage, 2);
        auto before = snapshot(g.root); auto fb = snapshot(g.fluffy);
        int rc = run_tool(g, "--dry-run", out);
        check(rc == 0, "exit code 0");
        check(snapshot(g.root) == before, "game folder identical");
        check_fluffy_untouched(g, fb);
        check(has(out, "would replace") && has(out, "would delete"), "actions listed");
        check(has(out, "registry.json"), "state files listed");
    }

    printf("[5] game running: refused, nothing changed\n");
    {
        Game g = make_game("c5", {});
        make_pak(g.root + "\\" + pak_name(1), PatchPakKind::Costume, 3);
        auto before = snapshot(g.root);
        int rc = run_tool(g, "", out, true);
        check(rc != 0, "non-zero exit code");
        check(has(out, "is running"), "refusal message");
        check(snapshot(g.root) == before, "game folder identical");
    }

    printf("[6] not a game folder: refused\n");
    {
        Game g = make_game("c6", {});
        DeleteFileA((g.root + "\\re_chunk_000.pak").c_str());
        make_pak(g.root + "\\" + pak_name(1), PatchPakKind::Costume, 1);
        int rc = run_tool(g, "", out);
        check(rc != 0 && exists(g.root + "\\" + pak_name(1)), "refused, pak kept");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Row test: the real loaders (costume_loader.exe, stagetest.exe) on a fake game folder. The patch pak
// row must stay unbroken from 001 at every step while mod paks come and go above the loaders' paks.
// The game folder given is only read (its base pak and shipped data are used, nothing is written there).
// ---------------------------------------------------------------------------------------------

static void copy_tree(const std::string& from, const std::string& to) {
    make_dirs(to + "\\x");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_tree(from + "\\" + n, to + "\\" + n);
        else CopyFileA((from + "\\" + n).c_str(), (to + "\\" + n).c_str(), FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// The row as text: number then M mod, S stage pak, s stage placeholder, C costume pak, c costume placeholder.
// *unbroken is false when a pak exists above a missing number.
static std::string row_of(const std::string& game, bool* unbroken) {
    std::string out;
    *unbroken = true;
    for (int n = 1; n < 100; n++) {
        const std::string p = game + "\\" + pak_name(n);
        if (!exists(p)) {
            for (int k = n + 1; k < 100; k++) if (exists(game + "\\" + pak_name(k))) *unbroken = false;
            break;
        }
        uint32_t e = 0;
        PatchPakKind k = classify_patch_pak(p.c_str(), &e);
        char c = k == PatchPakKind::Stage ? (e == 1 ? 's' : 'S') : k == PatchPakKind::Costume ? (e == 1 ? 'c' : 'C') : 'M';
        out += (out.empty() ? "" : " ") + std::to_string(n) + c;
    }
    return out;
}

static std::string g_build, g_row_game, g_base_pak;

static void run_loader(const char* which) {   // "costume" or "stage"
    std::string cmd = std::string(which) == "costume"
        ? "\"" + g_build + "\\costume_loader.exe\" \"" + g_row_game + "\" --base-pak \"" + g_base_pak + "\""
        : "\"" + g_build + "\\stagetest.exe\" \"" + g_row_game + "\"";
    system(("cmd /c \"" + cmd + " > \"" + g_work + "\\row_" + which + ".txt\" 2>&1\"").c_str());
}

static void expect_row(const char* step, const char* want) {
    bool unbroken;
    const std::string got = row_of(g_row_game, &unbroken);
    printf("  %-44s %s\n", step, got.c_str());
    check(unbroken, step);
    if (got != want) { printf("    expected: %s\n", want); check(false, step); }
}

static int run_row_test(const char* build, const char* work, const char* game, const char* mod_archive) {
    g_build = build; g_work = work; g_base_pak = std::string(game) + "\\re_chunk_000.pak";
    g_row_game = g_work + "\\row\\game";
    copy_tree(std::string(game) + "\\reframework\\data\\SF6_Costumes_Data\\loader", g_row_game + "\\reframework\\data\\SF6_Costumes_Data\\loader");
    const std::string stage_idx = g_row_game + "\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt";
    make_dirs(stage_idx);
    CopyFileA((std::string(game) + "\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt").c_str(), stage_idx.c_str(), FALSE);
    const std::string zip = g_row_game + "\\reframework\\costume_mods\\Cammy\\mod.zip";
    make_dirs(zip);
    CopyFileA(mod_archive, zip.c_str(), FALSE);
    // a stage mod: one pak holding the first file of the stage path index
    std::string first = slurp(stage_idx);
    first = first.substr(0, first.find_first_of("\r\n"));
    const std::string stage_src = g_row_game + "\\reframework\\stage_mods\\test.pak";
    auto put_stage_mod = [&]() {
        make_dirs(stage_src);
        PakWriter w;
        w.add_uncompressed(pak_path_hash(std::string_view(first)), std::vector<uint8_t>(64, 7));
        w.write(stage_src.c_str());
    };
    put_stage_mod();
    auto foreign = [&](int n) { make_pak(g_row_game + "\\" + pak_name(n), PatchPakKind::Mod, 2); };
    auto drop = [&](int n) { DeleteFileA((g_row_game + "\\" + pak_name(n)).c_str()); };

    printf("[7] costume loader: a mod pak appears above ours, then goes away\n");
    foreign(1);
    run_loader("costume");  expect_row("foreign 001, loader run", "1M 2C");
    foreign(3);
    run_loader("costume");  expect_row("foreign 003 added, loader run", "1M 2c 3M 4C");
    run_loader("costume");  expect_row("loader run again (up to date)", "1M 2c 3M 4C");
    foreign(5);
    run_loader("costume");  expect_row("foreign 005 added, loader run", "1M 2c 3M 4c 5M 6C");
    drop(5); drop(3);
    run_loader("costume");  expect_row("foreign 005 and 003 removed, loader run", "1M 2C");

    printf("[8] costume loader: the stage pak appears above ours (same mods: ours moves), then goes\n");
    make_pak(g_row_game + "\\" + pak_name(3), PatchPakKind::Stage, 3);
    run_loader("costume");  expect_row("stage pak 003 added, loader run", "1M 2c 3S 4C");
    drop(3);
    run_loader("costume");  expect_row("stage pak removed, loader run", "1M 2C");

    printf("[9] costume loader: no costume mod left while a mod pak sits above ours\n");
    foreign(3);
    run_loader("costume");  expect_row("foreign 003 added, loader run", "1M 2c 3M 4C");
    DeleteFileA(zip.c_str());
    run_loader("costume");  expect_row("costume mod removed, loader run", "1M 2c 3M");
    drop(3);
    run_loader("costume");  expect_row("foreign 003 removed, loader run", "1M");
    CopyFileA(mod_archive, zip.c_str(), FALSE);
    drop(2);

    printf("[10] stage loader: a mod pak appears above the stage pak, then goes away\n");
    run_loader("stage");    expect_row("stage run", "1M 2S");
    foreign(3);
    run_loader("stage");    expect_row("foreign 003 added, stage run", "1M 2s 3M 4S");
    drop(3);
    run_loader("stage");    expect_row("foreign 003 removed, stage run", "1M 2S");
    foreign(3);
    DeleteFileA(stage_src.c_str());
    run_loader("stage");    expect_row("stage mod removed, foreign 003 above", "1M 2s 3M");
    drop(3);
    run_loader("stage");    expect_row("foreign 003 removed too", "1M");
    put_stage_mod();

    printf("[11] both loaders (stage first, as in the game), mod pak above then removed\n");
    run_loader("stage"); run_loader("costume");
    expect_row("stage then costume", "1M 2S 3C");
    foreign(4);
    run_loader("stage"); run_loader("costume");
    expect_row("foreign 004 added", "1M 2s 3c 4M 5S 6C");
    drop(4);
    run_loader("stage"); run_loader("costume");
    expect_row("foreign 004 removed", "1M 2S 3C");

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 6 && std::string(argv[1]) == "--row") return run_row_test(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 3 && argv[1][0] != '-') return run_clean_tests(argv[1], argv[2]);
    printf("usage: slots_clean_test <SF6_SlotsClean.exe> <work dir>\n"
           "       slots_clean_test --row <build dir> <work dir> <game dir (read only)> <costume mod archive>\n");
    return 2;
}
