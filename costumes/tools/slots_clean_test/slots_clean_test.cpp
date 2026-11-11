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

// A fixture file that can't be written is the test's own failure, and says so: plain paths stop at 259 characters, so a deep scratch
// folder would leave files out silently and the loaders would look wrong (README: keep the scratch folder short).
static void cannot_write(const std::string& path) {
    const std::string what = "cannot write a fixture file (" + std::to_string(path.size()) + " characters; keep the scratch folder short): " + path;
    check(false, what.c_str());
}

static void put(const std::string& path, const std::string& data) {
    make_dirs(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
    else cannot_write(path);
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
    put(g.root + "\\reframework\\data\\SF6_Costumes_Data\\loose_probes.tsv", "none\tnatives/stm/product/model/1.tex.241101895\n");
    put(g.root + "\\reframework\\data\\SF6_Costumes_Data\\fluffy_location.txt", g.fluffy);
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt", "shipped");
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\loader\\variants.tsv", "#fingerprint\tx\n");
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\registry.json", "{}");
    put(g.root + "\\reframework\\data\\SF6_StageSlots_Data\\state.json", "{\"selection\":{}}");
    put(g.root + "\\reframework\\data\\SF6_TitleSlots_Data\\registry.json", "{}");
    put(g.root + "\\reframework\\data\\SF6_TitleSlots_Data\\overdrive.json", "{\"game\":{}}");
    put(g.root + "\\reframework\\data\\SF6_TitleSlots_Data\\shuffle.json", "{\"left\":[]}");
    put(g.root + "\\reframework\\data\\SF6_TitleSlots_Data\\game_title.jpg", "picture");
    put(g.root + "\\reframework\\data\\SF6_UISlots_Data\\registry.json", "{}");
    put(g.root + "\\reframework\\title_mods\\Pack.zip", "title pack");
    put(g.root + "\\reframework\\ui_mods\\Pack.rar", "ui pack");
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
    "\\reframework\\data\\SF6_Costumes_Data\\loose_probes.tsv",
    "\\reframework\\data\\SF6_StageSlots_Data\\registry.json", "\\reframework\\data\\SF6_StageSlots_Data\\loader\\variants.tsv",
    "\\reframework\\data\\SF6_TitleSlots_Data\\registry.json", "\\reframework\\data\\SF6_UISlots_Data\\registry.json",
};
static const char* KEPT[] = {
    "\\re_chunk_000.pak", "\\SF6_CostumeLoader.log", "\\reframework\\costume_mods\\Ryu\\Cool.zip",
    "\\reframework\\costume_mods\\settings.ini", "\\reframework\\data\\OtherMod\\save.json",
    "\\reframework\\data\\SF6_Costumes_Data\\loader\\vanilla_costume_index.tsv",
    "\\reframework\\data\\SF6_Costumes_Data\\fluffy_location.txt",
    "\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt",
    "\\reframework\\data\\SF6_StageSlots_Data\\state.json", "\\reframework\\data\\SF6_TitleSlots_Data\\overdrive.json",
    "\\reframework\\data\\SF6_TitleSlots_Data\\shuffle.json", "\\reframework\\data\\SF6_TitleSlots_Data\\game_title.jpg",
    "\\reframework\\title_mods\\Pack.zip", "\\reframework\\ui_mods\\Pack.rar",
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
static bool g_manager_suite = false;   // --row-manager: every run in Manager mode (common/manager_mode.hpp)

static void run_loader(const char* which, const std::string& build = {}) {   // "costume" or "stage"
    const std::string& b = build.empty() ? g_build : build;
    std::string cmd = std::string(which) == "costume"
        ? "\"" + b + "\\costume_loader.exe\" \"" + g_row_game + "\" --base-pak \"" + g_base_pak + "\""
        : "\"" + b + "\\stagetest.exe\" \"" + g_row_game + "\"";
    system(("cmd /c \"" + cmd + " > \"" + g_work + "\\row_" + which + ".txt\" 2>&1\"").c_str());
}

static void expect_row(const char* step, const char* want) {
    bool unbroken;
    const std::string got = row_of(g_row_game, &unbroken);
    printf("  %-44s %s\n", step, got.c_str());
    check(unbroken, step);
    if (got != want) { printf("    expected: %s\n", want); check(false, step); }
}

// The last run's output (costume_loader.exe prints its log) has `text`, or has not
static void expect_said(const char* which, const char* text, bool want = true) {
    const bool got = has(slurp(g_work + "\\row_" + which + ".txt"), text);
    if (got != want) printf("    %s output %s \"%s\"\n", which, want ? "lacks" : "has", text);
    check(got == want, text);
}

// ---------------------------------------------------------------------------------------------
// Fixtures made of the game's own files (read from the base pak given, never written), for the fixes ported from
// ZZtaii's Slotsloader fork 1.10.5 - 1.10.9 (MIT); his tests/*.py describe the same cases on a synthetic game.
// ---------------------------------------------------------------------------------------------

static PakReader g_game_pak;   // the game's re_chunk_000.pak, read only

static std::vector<uint8_t> game_bytes(const std::string& p) {
    auto* e = g_game_pak.find(pak_path_hash(std::string_view(p)));
    return e ? g_game_pak.read(*e) : std::vector<uint8_t>{};
}

static void put_bin(const std::string& path, const std::vector<uint8_t>& d) {
    make_dirs(path);
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { if (!d.empty()) fwrite(d.data(), 1, d.size(), f); fclose(f); }
    else cannot_write(path);
}

static std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Printable UTF-16LE strings of a resource, '@' and spaces in front taken off
static std::vector<std::string> u16_strings(const std::vector<uint8_t>& d) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        std::string s;
        size_t j = i;
        while (j + 1 < d.size() && d[j + 1] == 0 && d[j] >= 0x20 && d[j] < 0x7f) { s += (char)d[j]; j += 2; }
        if (s.size() >= 4) {
            const size_t a = s.find_first_not_of("@ ");
            if (a != std::string::npos) out.push_back(s.substr(a));
        }
        i = j;
    }
    return out;
}

// Every occurrence of `from` in d (UTF-16LE) replaced by `to` (same length)
static void u16_replace(std::vector<uint8_t>& d, const std::string& from, const std::string& to) {
    std::vector<uint8_t> f, t;
    for (char c : from) { f.push_back((uint8_t)c); f.push_back(0); }
    for (char c : to) { t.push_back((uint8_t)c); t.push_back(0); }
    for (size_t i = 0; i + f.size() <= d.size(); i += 2)
        if (memcmp(d.data() + i, f.data(), f.size()) == 0) memcpy(d.data() + i, t.data(), t.size());
}

// The costume pak in the row (the real one, not a placeholder)
static std::string costume_pak_path() {
    for (int n = 1; n < 100; n++) {
        const std::string p = g_row_game + "\\" + pak_name(n);
        uint32_t e = 0;
        if (exists(p) && classify_patch_pak(p.c_str(), &e) == PatchPakKind::Costume && e > 1) return p;
    }
    return {};
}

// A file of our costume pak, or empty (absent: *found false)
static std::vector<uint8_t> costume_file(const std::string& p, bool* found = nullptr) {
    PakReader r;
    if (found) *found = false;
    if (!r.open(costume_pak_path().c_str())) return {};
    auto* e = r.find(pak_path_hash(std::string_view(lower(p))));
    if (!e) return {};
    if (found) *found = true;
    return r.read(*e);
}

// The slot the last costume run made for the given model folder ("esf001/001"): its folder and number, from the
// log line "  slot: esf001/v06 -> folder 006, ..." of the outfit made from that folder (the fixture's)
struct FixtureSlot { std::string folder, scene; };
static FixtureSlot fixture_slot(const std::string& fd) {
    FixtureSlot s;
    const std::string out = slurp(g_work + "\\row_costume.txt");
    const std::string key = "  slot: " + fd + "/v";
    for (size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + 1)) {
        const std::string v = out.substr(at + key.size(), 2);
        const size_t f = out.find("-> folder ", at);
        if (f == std::string::npos) break;
        s.folder = out.substr(f + 10, 3);
        s.scene = "natives/stm/product/charparam/esf/" + fd + "/" + fd + "v" + v + ".scn.20";
    }
    return s;
}

// The game files the loader names in its log are read: SF6_COSTUME_TEST_TURNED_OFF lists those that read as
// turned off (PakReader::forget)
static void turned_off(const std::vector<std::string>& paths) {
    const std::string off = g_work + "\\turned_off.txt";
    std::string text;
    for (auto& p : paths) text += p + "\n";
    put(off, text);
    SetEnvironmentVariableA("SF6_COSTUME_TEST_TURNED_OFF", paths.empty() ? nullptr : off.c_str());
}

// A pak with rows in the given order (hash, data): a turned-off row has hash 0, as a mod manager's PAK method
// leaves it (the row and its data stay, zeros over the hash)
static void write_rows(const std::string& path, const std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& rows) {
    make_dirs(path);
    PakWriter w;
    for (auto& [h, d] : rows) w.add_uncompressed(h, d);
    w.write(path.c_str());
}

// A zip of stored (uncompressed) entries, names with '/'
static uint32_t crc32_of(const std::vector<uint8_t>& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void write_stored_zip(const std::string& path, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& entries) {
    std::vector<uint8_t> out, cd;
    auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    auto u32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i))); };
    for (auto& [name, data] : entries) {
        const uint32_t crc = crc32_of(data), size = (uint32_t)data.size(), at = (uint32_t)out.size();
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0x21);   // stored, 1980-01-01
        u32(out, crc); u32(out, size); u32(out, size); u16(out, (uint32_t)name.size()); u16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0x21);
        u32(cd, crc); u32(cd, size); u32(cd, size); u16(cd, (uint32_t)name.size()); u16(cd, 0); u16(cd, 0); u16(cd, 0);
        u16(cd, 0); u32(cd, 0); u32(cd, at);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    const uint32_t cd_at = (uint32_t)out.size();
    out.insert(out.end(), cd.begin(), cd.end());
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, (uint32_t)entries.size()); u16(out, (uint32_t)entries.size());
    u32(out, (uint32_t)cd.size()); u32(out, cd_at); u16(out, 0);
    put_bin(path, out);
}

// Manager mode on (the Slotsloader Manager's manager_installs.json lists the plugin at its size on disk, as when it
// installed the loader) or off (neither file)
static void set_manager_mode(bool on) {
    const std::string plugin = g_row_game + "\\reframework\\plugins\\SF6_CostumeSlotsNative.dll";
    const std::string list = g_row_game + "\\reframework\\data\\SF6_Costumes_Data\\manager_installs.json";
    if (!on) { DeleteFileA(list.c_str()); DeleteFileA(plugin.c_str()); return; }
    const std::string fake(4321, 'P');
    put(plugin, fake);
    put(list, "{\n  \"format\": \"sf6-manager-installs-1\",\n  \"generated\": \"2026-11-05T00:00:00Z\",\n"
              "  \"manager\": \"Slotsloader Manager 1.0.18\",\n  \"mods\": [\n    {\n      \"id\": \"test\",\n"
              "      \"name\": \"SF6 Slots Loader\",\n      \"installed_utc\": \"2026-11-05T00:00:00Z\",\n      \"enabled\": true,\n"
              "      \"files\": [\n        { \"path\": \"amd_ags_x64.dll\", \"sha256\": \"\", \"size\": 1 },\n"
              "        { \"path\": \"reframework/plugins/SF6_CostumeSlotsNative.dll\", \"sha256\": \"\", \"size\": "
              + std::to_string(fake.size()) + " }\n      ],\n      \"paks\": []\n    }\n  ]\n}\n");
}

static int run_row_test(const char* build, const char* work, const char* game, const char* mod_archive,
                        const char* previous) {
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

    if (g_manager_suite) {
        printf("Manager mode: every run below as installed by the Slotsloader Manager (manager_installs.json)\n");
        set_manager_mode(true);
    }

    printf("[7] costume loader: a mod pak appears above ours, then goes away\n");
    foreign(1);
    run_loader("costume");  expect_row("foreign 001, loader run", "1M 2C");
    expect_said("costume", "Manager mode: installed by the Slotsloader Manager", g_manager_suite);
    foreign(3);
    run_loader("costume");  expect_row("foreign 003 added, loader run", "1M 2c 3M 4C");
    run_loader("costume");  expect_row("loader run again (up to date)", "1M 2c 3M 4C");
    expect_said("costume", "Up to date (fingerprint match");   // nothing filled, nothing rebuilt
    expect_said("costume", "filled a hole", false);
    expect_said("costume", "variants.json missing", false);
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

    printf("[12] a pak removed in the middle of the row by another tool: the hole gets a placeholder\n");
    foreign(4); foreign(5);
    run_loader("stage"); run_loader("costume");
    expect_row("foreign 004 and 005 added", "1M 2s 3c 4M 5M 6S 7C");
    drop(4);
    run_loader("stage"); run_loader("costume");
    expect_row("foreign 004 removed: hole filled", "1M 2s 3c 4c 5M 6S 7C");
    expect_said("costume", "filled a hole in the patch pak row: patch_004 (marker only)");
    run_loader("stage"); run_loader("costume");
    expect_row("both run again: nothing changes", "1M 2s 3c 4c 5M 6S 7C");
    expect_said("costume", "Up to date (fingerprint match");
    expect_said("costume", "filled a hole", false);
    drop(5);
    run_loader("stage"); run_loader("costume");
    expect_row("foreign 005 removed: placeholders go", "1M 2S 3C");
    drop(2);   // only ours above the hole: not filled, the costume pak moves down
    run_loader("costume");  expect_row("stage pak removed, costume run", "1M 2C");
    expect_said("costume", "filled a hole", false);
    run_loader("stage"); run_loader("costume");
    expect_row("stage then costume", "1M 2S 3C");

    printf("[13] a pak that cannot be read (locked) counts as another mod's\n");
    foreign(4);
    HANDLE lock = CreateFileA((g_row_game + "\\" + pak_name(4)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "pak 004 locked");
    run_loader("costume");  expect_row("locked 004 above ours, costume run", "1M 2S 3c 4M 5C");
    expect_said("costume", "patch_004: cannot be read");
    CloseHandle(lock);
    check(classify_patch_pak((g_row_game + "\\" + pak_name(4)).c_str()) == PatchPakKind::Mod, "locked pak left as it was");
    run_loader("costume");  expect_row("unlocked, costume run", "1M 2S 3c 4M 5C");
    drop(4);
    run_loader("stage"); run_loader("costume");
    expect_row("foreign 004 removed", "1M 2S 3C");

    printf("[14] variants.json deleted while the pak is up to date\n");
    const std::string variants = g_row_game + "\\reframework\\data\\SF6_Costumes_Data\\variants.json";
    const std::string pak_before = slurp(g_row_game + "\\" + pak_name(3));
    DeleteFileA(variants.c_str());
    run_loader("costume");  expect_row("costume run", "1M 2S 3C");
    expect_said("costume", "variants.json missing: rebuilding");
    expect_said("costume", "the pak this build makes");   // kept, not written again
    check(exists(variants), "variants.json written again");
    check(!pak_before.empty() && slurp(g_row_game + "\\" + pak_name(3)) == pak_before, "costume pak unchanged");
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");

    printf("[15] structural files from the game: version scenes past ver_02, one that cannot be read, a fighter in none\n");
    const std::string dlc = g_row_game + "\\dlc\\test.pak";
    {
        make_dirs(dlc);
        PakWriter w;   // an empty entry: found in the paks, read as nothing
        w.add_uncompressed(pak_path_hash(std::string_view("natives/stm/product/flow/version_update/ver_03_0100/esf.scn.20")), {});
        w.write(dlc.c_str());
    }
    run_loader("costume");  expect_row("costume run", "1M 2S 3C");
    expect_said("costume", "structural files taken from the shipped copies (natives/stm/product/flow/version_update/ver_03_0100/esf.scn.20: cannot be read)");
    DeleteFileA(dlc.c_str());
    const std::string registry = g_row_game + "\\reframework\\data\\SF6_Costumes_Data\\registry.json";
    const std::string off = g_work + "\\turned_off.txt";
    put(off, "natives/stm/product/flow/version_update/ver_02_0500/esf.scn.20\n");
    SetEnvironmentVariableA("SF6_COSTUME_TEST_TURNED_OFF", off.c_str());
    DeleteFileA(registry.c_str());   // same mods: forces a build
    run_loader("costume");  expect_row("ver_02_0500 turned off, costume run", "1M 2S 3C");
    expect_said("costume", "fighter 34 is in the costume table but in no fighter scene: no slots for it");
    expect_said("costume", "structural files built from the game");
    SetEnvironmentVariableA("SF6_COSTUME_TEST_TURNED_OFF", nullptr);
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("back to normal, costume run", "1M 2S 3C");
    expect_said("costume", "structural files built from the game");
    expect_said("costume", "no fighter scene", false);

    const std::string data_dir = g_row_game + "\\reframework\\data\\SF6_Costumes_Data";
    const std::string probes = data_dir + "\\loose_probes.tsv";
    auto win = [](std::string p) { for (auto& c : p) if (c == '/') c = '\\'; return p; };
    auto remove_tree = [](const std::string& dir) { system(("cmd /c rmdir /s /q \"" + dir + "\" 2>nul").c_str()); };
    auto is_ref = [](const std::string& l, const char* ext) {
        const size_t n = strlen(ext);
        return l.size() > n && l.compare(l.size() - n, n, ext) == 0;
    };

    printf("[16] turned-off rows (zeros over the hash): the candidates of a hash are the rows between its neighbours\n");
    {
        const std::string r1p = g_work + "\\rows\\sorted.pak", r2p = g_work + "\\rows\\unsorted.pak";
        const std::string r3p = g_work + "\\rows\\two.pak", r4p = g_work + "\\rows\\under.pak";
        const uint64_t a = 0x1000, b = 0x2000, c = 0x3000;
        write_rows(r1p, {{a, bytes_of("A")}, {0, bytes_of("turned off")}, {c, bytes_of("C")}});
        write_rows(r2p, {{c, bytes_of("C")}, {0, bytes_of("turned off")}, {a, bytes_of("A")}});
        write_rows(r3p, {{a, bytes_of("A")}, {0, bytes_of("one")}, {0, bytes_of("two")}, {c, bytes_of("C")}});
        write_rows(r4p, {{a, bytes_of("A")}});
        PakReader r1, r2, r3, r4;
        check(r1.open(r1p.c_str()) && r2.open(r2p.c_str()) && r3.open(r3p.c_str()) && r4.open(r4p.c_str()), "row paks read");
        auto c1 = r1.turned_off_candidates(b);
        check(c1.size() == 1 && r1.read(*c1[0]) == bytes_of("turned off"), "a hash between the rows around it: that row, read");
        check(r1.turned_off_candidates(a - 1).empty() && r1.turned_off_candidates(c + 1).empty(), "a hash outside that gap: none");
        check(r1.turned_off_candidates(a).empty(), "a hash the pak has: none");
        check(r2.turned_off_candidates(b).empty(), "a table not sorted: none");
        check(r3.turned_off_candidates(b).size() == 2, "two turned-off rows in the gap: both");
        r4.add_overlay(&r1);
        auto c4 = r4.turned_off_candidates(b);
        check(c4.size() == 1 && r4.read(*c4[0]) == bytes_of("turned off"), "a turned-off row of a pak read over it: read through the pak");
    }

    printf("[17] an extra outfit whose mod ships its own colour list (fork 1.10.6); mod-only files at a shared path and at model/ (fork 1.10.9)\n");
    check(g_game_pak.open(g_base_pak.c_str()), "the game's pak read");
    const std::string mroot = g_row_game + "\\reframework\\costume_mods\\Ryu\\ZZ Colour Fixture\\";
    auto mod_file = [&](const std::string& p, const std::vector<uint8_t>& d) { put_bin(mroot + win(p), d); };
    const std::string c1_scene = "natives/stm/product/charparam/esf/esf001/esf001v00.scn.20";
    const std::string c2_scene = "natives/stm/product/charparam/esf/esf001/esf001v01.scn.20";
    // The body mesh and material file a scene names in a model folder ("esf001/001/01/")
    auto body_of = [&](const std::vector<uint8_t>& scene, const std::string& part, std::string& mesh_p, std::string& mdf_p) {
        for (auto& s : u16_strings(scene)) {
            const std::string l = lower(s);
            if (l.find("product/model/esf/" + part) != 0) continue;
            if (mesh_p.empty() && is_ref(l, ".mesh")) mesh_p = "natives/stm/" + l + ".230110883";
            if (mdf_p.empty() && is_ref(l, ".mdf2")) mdf_p = "natives/stm/" + l + ".31";
        }
    };
    const auto scene0 = game_bytes(c1_scene);
    std::string mesh_p, mdf_p;
    body_of(scene0, "esf001/001/01/", mesh_p, mdf_p);
    auto mesh = game_bytes(mesh_p);
    const auto mdf = game_bytes(mdf_p);
    check(!scene0.empty() && !mesh.empty() && !mdf.empty(), "Ryu's Outfit 1 scene, body mesh and material file read from the game");
    if (!mesh.empty()) mesh.back() ^= 0x5A;   // not the game's own mesh
    const std::string list_p = "natives/stm/product/model/esf/esf001/001/esf001_001_ccvd.user.2";
    const auto list0 = game_bytes(list_p);
    std::vector<std::string> cmd_names;   // the colours the game's list names: "esf001_001_cmd_000.user"
    for (auto& s : u16_strings(list0)) {
        const std::string l = lower(s);
        if (l.find("_cmd_") != std::string::npos && is_ref(l, ".user")) cmd_names.push_back(l.substr(l.rfind('/') + 1));
    }
    // The mod's colours: Outfit 2's under Outfit 1's names (other bytes, same layout); the second one with
    // MaterialData's signature of before the game update that changed it (0x4D532A61)
    std::vector<std::pair<std::string, std::vector<uint8_t>>> mod_cmds;
    for (size_t k = 0; k < cmd_names.size() && mod_cmds.size() < 2; k++) {
        std::string c2 = cmd_names[k];
        c2.replace(0, 11, "esf001_002_");
        auto d = game_bytes("natives/stm/product/model/esf/esf001/002/" + c2 + ".2");
        if (!d.empty() && d != game_bytes("natives/stm/product/model/esf/esf001/001/" + cmd_names[k] + ".2"))
            mod_cmds.push_back({cmd_names[k], d});
    }
    check(!list0.empty() && mod_cmds.size() == 2, "the outfit's colour list and two colours of Outfit 2 read from the game");
    bool stale = false;
    if (mod_cmds.size() == 2) {
        auto& d = mod_cmds[1].second;
        const uint8_t type[4] = {0xF4, 0xF1, 0x32, 0x9E};
        uint8_t now[4] = {};
        for (size_t i = 0; i + 8 <= d.size() && !stale; i++)
            if (memcmp(d.data() + i, type, 4) == 0) { memcpy(now, d.data() + i + 4, 4); stale = true; }
        const uint8_t old[4] = {0x61, 0x2A, 0x53, 0x4D};
        if (stale && memcmp(now, old, 4) != 0) {
            for (size_t i = 0; i + 8 <= d.size(); i++)
                if (memcmp(d.data() + i, type, 4) == 0 && memcmp(d.data() + i + 4, now, 4) == 0) memcpy(d.data() + i + 4, old, 4);
        } else stale = false;
    }
    check(stale, "a colour file of the mod given the old MaterialData signature");
    const std::string dep = "natives/stm/product/model/esf/esf001/002/01/zz_slotsloader_shared.tex.241101895";
    const std::string rootdep = "natives/stm/product/model/zz_slotsloader_root.tex.241101895";
    const auto dep_mod = bytes_of("SF6 Slots Loader test: a mod-only file at a shared path (the outfit's copy)");
    const auto root_mod = bytes_of("SF6 Slots Loader test: a mod-only file in product/model/ (the outfit's copy)");
    mod_file(mesh_p, mesh);
    mod_file(mdf_p, mdf);
    mod_file(list_p, list0);
    for (auto& [n, d] : mod_cmds) mod_file("natives/stm/product/model/esf/esf001/001/" + n + ".2", d);
    mod_file(dep, dep_mod);
    mod_file(rootdep, root_mod);
    run_loader("costume");  expect_row("fixture added, costume run", "1M 2S 3C");
    expect_said("costume", "slots checked, 0 left out");
    FixtureSlot slot = fixture_slot("esf001");
    check(!slot.folder.empty(), "the fixture outfit has a slot");
    std::string sdir = "esf001/" + slot.folder + "/";
    {
        bool found = false;
        const auto scene = costume_file(slot.scene, &found);
        std::string list_ref;
        int lists = 0, own = 0;
        for (auto& s : u16_strings(scene)) {
            const std::string l = lower(s);
            if (l.find("ccvd.user") == std::string::npos) continue;
            lists++;
            if (l.find("/" + sdir + "esf001_001_ccvd.user") != std::string::npos) { own++; list_ref = l; }
        }
        check(found && lists > 0 && own == lists, "the slot's scene names the colour list pointed at the slot's colours");
        const auto list = costume_file("natives/stm/" + list_ref + ".2", &found);
        int cmds = 0, in_slot = 0;
        for (auto& s : u16_strings(list)) {
            const std::string l = lower(s);
            if (l.find("_cmd_") == std::string::npos) continue;
            cmds++;
            if (l.find("/" + sdir) != std::string::npos) in_slot++;
        }
        check(found && cmds > 0 && in_slot == cmds, "that list names the slot's colours");
        if (!mod_cmds.empty())
            check(costume_file("natives/stm/product/model/esf/" + sdir + mod_cmds[0].first + ".2") == mod_cmds[0].second,
                  "the slot's colour holds the mod's bytes");
    }
    expect_said("costume", "mod files brought to the current type signatures");
    expect_said("costume", "  colour files: the game's", false);
    check(costume_file(dep) == dep_mod, "no loose file: the shared mod-only file is the outfit's copy in the pak");
    check(costume_file(rootdep) == root_mod, "no loose file: the model/ mod-only file is the outfit's copy in the pak");
    check(has(slurp(probes), ("none\t" + rootdep + "\n").c_str()), "the model/ file is watched (none)");
    check(!has(slurp(probes), dep.c_str()), "the shared file is not (the fingerprint walks model/esf)");
    check(has(slurp(data_dir + "\\outfits.json"), "\"loader\": \"") && has(slurp(data_dir + "\\outfits.json"), "\",\n  \"loose_files_kept\": true,\n  \"pak\": "),
          "outfits.json says loose_files_kept, after loader");
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");

    printf("[18] a replacement puts a loose file at those paths, changes it, removes it (fork 1.10.9)\n");
    const std::string loose_dep = g_row_game + "\\" + win(dep), loose_root = g_row_game + "\\" + win(rootdep);
    bool in_pak = false;
    put(loose_dep, "LOOSE REPLACEMENT, FIRST");
    run_loader("costume");  expect_row("loose file at the shared path", "1M 2S 3C");
    expect_said("costume", "Up to date (fingerprint match", false);
    expect_said("costume", ("mod-only dependency left to the game folder's loose file: " + dep).c_str());
    costume_file(dep, &in_pak);
    check(!in_pak, "the shared path is left to the loose file");
    check(costume_file(rootdep) == root_mod, "the model/ file is still the outfit's");
    expect_said("costume", "slots checked, 0 left out");
    check(slurp(loose_dep) == "LOOSE REPLACEMENT, FIRST", "the loose file is not touched");
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");
    put(loose_dep, "LOOSE REPLACEMENT, SECOND VERSION");
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match", false);
    costume_file(dep, &in_pak);
    check(!in_pak, "replacement changed: still left to it");
    DeleteFileA(loose_dep.c_str());
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match", false);
    check(costume_file(dep) == dep_mod, "replacement removed: the outfit's copy is back");
    put(loose_root, "LOOSE REPLACEMENT AT MODEL/");
    run_loader("costume");  expect_row("loose file at the model/ path", "1M 2S 3C");
    expect_said("costume", "Up to date (fingerprint match", false);
    expect_said("costume", ("mod-only dependency left to the game folder's loose file: " + rootdep).c_str());
    costume_file(rootdep, &in_pak);
    check(!in_pak, "the model/ path is left to the loose file");
    check(has(slurp(probes), ("loose:" + std::to_string(slurp(loose_root).size()) + ":").c_str()), "its probe says loose");
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");
    DeleteFileA(loose_root.c_str());
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match", false);
    check(costume_file(rootdep) == root_mod, "removed: the outfit's copy is back");
    check(has(slurp(probes), ("none\t" + rootdep + "\n").c_str()), "its probe says none again");
    remove_tree(g_row_game + "\\natives");   // empty folders: nothing of the fingerprint
    run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");

    printf("[19] the game colour file the signatures come from, turned off by a replacement (fork 1.10.8)\n");
    const std::string ref = "natives/stm/product/model/esf/esf001/001/esf001_001_cmd_000.user.2";
    const uint64_t rh = pak_path_hash(std::string_view(ref));
    turned_off({ref});
    write_rows(dlc, {{rh - 1, bytes_of("before")}, {0, game_bytes(ref)}, {rh + 1, bytes_of("after")}});
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("turned off, its row kept", "1M 2S 3C");
    expect_said("costume", ("colour files: the game's " + ref + " is turned off in the pak (a replacement installed over it): "
                            "read from its turned-off entry").c_str());
    expect_said("costume", "mod files brought to the current type signatures");
    DeleteFileA(dlc.c_str());
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("gone", "1M 2S 3C");
    expect_said("costume", ("colour files: the game's " + ref + " is missing (a replacement installed over it): signatures read from").c_str());
    expect_said("costume", "mod files brought to the current type signatures");
    {
        std::vector<std::string> every;
        for (int f = 0; f < 100; f++) {
            char p[96];
            snprintf(p, sizeof p, "natives/stm/product/model/esf/esf%03d/001/esf%03d_001_cmd_000.user.2", f, f);
            every.push_back(p);
        }
        turned_off(every);
    }
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("no fighter's colour file", "1M 2S 3C");
    expect_said("costume", "WARN: no game colour file could be read");
    expect_said("costume", "mod files brought to the current type signatures", false);
    turned_off({});
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_said("costume", "  colour files: the game's", false);

    printf("[20] the outfit's scene, turned off by a replacement: read from its turned-off row (fork 1.10.6)\n");
    const uint64_t sh = pak_path_hash(std::string_view(c1_scene));
    const auto scene1 = game_bytes(c2_scene);
    check(!scene1.empty(), "Ryu's Outfit 2 scene read from the game");
    turned_off({c1_scene});
    write_rows(dlc, {{sh - 1, bytes_of("before")}, {0, scene0}, {sh + 1, bytes_of("after")}});
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("scene turned off, its row kept", "1M 2S 3C");
    expect_said("costume", ("scene: the game's " + c1_scene + " is turned off in the pak (a replacement installed over it): "
                            "read from its turned-off entry").c_str());
    expect_said("costume", "slots checked, 0 left out");
    slot = fixture_slot("esf001");
    costume_file(slot.scene, &in_pak);
    check(!slot.folder.empty() && in_pak, "the slot has its scene");
    write_rows(dlc, {{sh - 1, bytes_of("before")}, {0, scene1}, {sh + 1, bytes_of("after")}});
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("another outfit's scene in that row", "1M 2S 3C");
    expect_said("costume", "read from its turned-off entry", false);
    expect_said("costume", "slot scene missing");
    write_rows(dlc, {{sh - 1, bytes_of("before")}, {0, scene0}, {0, scene0}, {sh + 1, bytes_of("after")}});
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("two rows of that outfit's folder", "1M 2S 3C");
    expect_said("costume", "read from its turned-off entry", false);
    expect_said("costume", "slot scene missing");
    write_rows(dlc, {{sh + 1, bytes_of("after")}, {0, scene0}, {sh - 1, bytes_of("before")}});
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_row("a table not sorted", "1M 2S 3C");
    expect_said("costume", "read from its turned-off entry", false);
    expect_said("costume", "slot scene missing");
    turned_off({});
    DeleteFileA(dlc.c_str());
    DeleteFileA(registry.c_str());
    run_loader("costume");  expect_said("costume", "slots checked, 0 left out");

    printf("[21] a donor's colour files in the outfit's folder follow the slot (fork 1.10.7)\n");
    {
        auto donor = game_bytes("natives/stm/product/model/esf/esf002/001/esf002_001_ccvd.user.2");
        std::vector<std::string> names;   // "esf002_001_cmd_000.user"
        for (auto& s : u16_strings(donor)) {
            const std::string l = lower(s);
            if (l.find("_cmd_") != std::string::npos && is_ref(l, ".user")) names.push_back(l.substr(l.rfind('/') + 1));
        }
        u16_replace(donor, "esf002/001/", "esf001/001/");   // in the host's folder, as Female Ryu C1 ships it
        int moved = 0;
        for (auto& s : u16_strings(donor)) if (lower(s).find("esf001/001/esf002_001_cmd_") != std::string::npos) moved++;
        check(!names.empty() && moved > 0, "a donor's colour list (esf002 Outfit 1) read from the game, put in Ryu's folder");
        mod_file("natives/stm/product/model/esf/esf001/001/esf002_001_ccvd.user.2", donor);
        for (auto& n : names)
            mod_file("natives/stm/product/model/esf/esf001/001/" + n + ".2", game_bytes("natives/stm/product/model/esf/esf002/001/" + n + ".2"));
        run_loader("costume");  expect_row("donor files added", "1M 2S 3C");
        expect_said("costume", "slots checked, 0 left out");
        slot = fixture_slot("esf001");
        sdir = "esf001/" + slot.folder + "/";
        bool found = false;
        const auto list = costume_file("natives/stm/product/model/esf/" + sdir + "esf002_001_ccvd.user.2", &found);
        int cmds = 0, in_slot = 0, there = 0;
        for (auto& s : u16_strings(list)) {
            const std::string l = lower(s);
            if (l.find("_cmd_") == std::string::npos) continue;
            cmds++;
            if (l.find("/" + sdir) == std::string::npos) continue;
            in_slot++;
            bool f = false;
            costume_file("natives/stm/" + l + ".2", &f);
            if (f) there++;
        }
        check(found && cmds > 0 && in_slot == cmds, "the slot's copy of the donor's list names the slot's copies of its colours");
        check(there == cmds, "those copies are in the pak");
        // The host's list as the mod ships it, copied under the slot's name: the colours the mod ships follow too
        const auto host = costume_file("natives/stm/product/model/esf/" + sdir + "esf001_" + slot.folder + "_ccvd.user.2", &found);
        int followed = 0;
        for (auto& [n, d] : mod_cmds) {
            std::string sn = n;
            sn.replace(0, 11, "esf001_" + slot.folder + "_");
            for (auto& s : u16_strings(host)) if (lower(s).find(sdir + sn) != std::string::npos) { followed++; break; }
        }
        check(found && followed == (int)mod_cmds.size(), "with a donor's list, the host's colours the mod ships follow the slot too");
        DeleteFileA((mroot + win("natives/stm/product/model/esf/esf001/001/esf002_001_ccvd.user.2")).c_str());
        for (auto& n : names) DeleteFileA((mroot + win("natives/stm/product/model/esf/esf001/001/" + n + ".2")).c_str());
        run_loader("costume");  expect_said("costume", "slots checked, 0 left out");
    }

    printf("[22] a main and five add-ons that each replace its body mesh (fork 1.10.5): options of one outfit here\n");
    {
        const std::string oroot = g_row_game + "\\reframework\\costume_mods\\Ryu\\ZZ Overrides\\";
        std::string m2, d2;
        body_of(game_bytes(c2_scene), "esf001/002/01/", m2, d2);
        auto mesh2 = game_bytes(m2);
        const auto mdf2 = game_bytes(d2);
        check(!mesh2.empty() && !mdf2.empty(), "Ryu's Outfit 2 body mesh and material file read from the game");
        mesh2.back() ^= 0x11;
        put(oroot + "00 Main\\modinfo.ini", "name=Fixture Main\r\n");
        put_bin(oroot + "00 Main\\" + win(m2), mesh2);
        put_bin(oroot + "00 Main\\" + win(d2), mdf2);
        for (int k = 1; k <= 5; k++) {
            const std::string dir = oroot + "0" + std::to_string(k) + " Option " + std::to_string(k) + "\\";
            put(dir + "modinfo.ini", "name=Option " + std::to_string(k) + "\r\nAddonFor=Fixture Main\r\n");
            auto m = mesh2;
            m[m.size() - 2] ^= uint8_t(k);
            put_bin(dir + win(m2), m);
        }
        run_loader("costume");  expect_row("overrides added", "1M 2S 3C");
        if (!g_manager_suite) {
            expect_said("costume", "Fixture Main: 5 add-ons as runtime options");
        } else {   // one sub-folder of separate mods (user_grouped): the combined outfits only, one per add-on (case 24)
            expect_said("costume", "Fixture Main: 5 add-ons that each replace the same files: one variant each");
            expect_said("costume", "Fixture Main: put with its add-on(s) in one sub-folder, only the combined outfit is made");
            for (int k = 1; k <= 5; k++)
                expect_said("costume", ("  variant: Fixture Main + Option " + std::to_string(k)).c_str());
        }
        expect_said("costume", "too many to combine", false);
        remove_tree(oroot.substr(0, oroot.size() - 1));
    }

    if (g_manager_suite) {
        // The sub-folder rule (g_group_sources) belongs to the runtime options: in Manager mode a sub-folder stays as the fork has it
        printf("[25] Manager mode: several mods in one sub-folder of a character folder are not handled together\n");
        const std::string bison = g_row_game + "\\reframework\\costume_mods\\M. Bison\\";
        const auto bscene = game_bytes("natives/stm/product/charparam/esf/esf026/esf026v00.scn.20");
        std::string body_p, hair_p, unused;
        body_of(bscene, "esf026/001/01/", body_p, unused);
        body_of(bscene, "esf026/001/02/", hair_p, unused);
        auto body = game_bytes(body_p), hair = game_bytes(hair_p);
        check(!body.empty() && !hair.empty(), "M. Bison's Outfit 1 body and hair read from the game");
        if (!body.empty()) body.back() ^= 0x21;
        if (!hair.empty()) hair.back() ^= 0x21;
        put(bison + "ZZ Group\\Body\\modinfo.ini", "name=ZZ Body\r\n");
        put_bin(bison + "ZZ Group\\Body\\" + win(body_p), body);
        put(bison + "ZZ Group\\Hair\\modinfo.ini", "name=ZZ Hair\r\n");
        put_bin(bison + "ZZ Group\\Hair\\" + win(hair_p), hair);
        run_loader("costume");
        expect_said("costume", "handled together", false);
        expect_said("costume", "sub-folder ZZ Group", false);
        expect_said("costume", "retouch M. Bison", false);
        expect_said("costume", "slots checked, 0 left out");
        remove_tree(bison.substr(0, bison.size() - 1));

        // A costume and two add-ons that replace no same file, each its own archive in one sub-folder (Ingrid's FlyIn_DT_Book): one
        // outfit per add-on and one with both, as the fork lists them; "only the complete outfit" is the runtime options' rule
        std::string head_p;
        body_of(bscene, "esf026/000/00/", head_p, unused);
        auto head = game_bytes(head_p);
        check(!head.empty(), "M. Bison's head read from the game");
        if (!head.empty()) head.back() ^= 0x21;
        const std::string source = "M. Bison/ZZ Three", choices = data_dir + "\\variant_choices.json";
        write_stored_zip(bison + "ZZ Three\\ZZ Body.zip", {{"ZZ Body/modinfo.ini", bytes_of("name=ZZ Body\r\n")}, {"ZZ Body/" + body_p, body}});
        write_stored_zip(bison + "ZZ Three\\ZZ Hair.zip", {{"ZZ Hair/modinfo.ini", bytes_of("name=ZZ Hair\r\n")}, {"ZZ Hair/" + hair_p, hair}});
        write_stored_zip(bison + "ZZ Three\\ZZ Head.zip", {{"ZZ Head/modinfo.ini", bytes_of("name=ZZ Head\r\n")}, {"ZZ Head/" + head_p, head}});
        const std::string rows[3] = {"ZZ Body + ZZ Hair", "ZZ Body + ZZ Head", "ZZ Body + ZZ Hair + ZZ Head"};
        auto slot_of_row = [&](const std::string& vj, const std::string& label) {   // "" when off or not listed
            const std::string key = "{\"label\": \"" + label + "\", \"enabled\": true, \"slot\": \"";
            const size_t at = vj.find(key);
            return at == std::string::npos ? std::string() : vj.substr(at + key.size(), vj.find('"', at + key.size()) - at - key.size());
        };
        run_loader("costume");
        expect_said("costume", "ZZ Body: put with its add-on(s) in one sub-folder, only the combined outfit is made");
        expect_said("costume", "only the complete outfit (all of them) is made", false);
        for (auto& r : rows) expect_said("costume", ("  variant: " + r).c_str());
        expect_said("costume", "slots checked, 0 left out");
        // Its outfit number, -1 when it has none. ("Outfit V" in variants.json is a place in the fighter's list: it moves when an
        // outfit before it goes. The number stays while the outfit is there; a variant turned on again is a new outfit.)
        auto number_of = [&](const std::string& label) {
            const std::string oj = slurp(data_dir + "\\outfits.json");
            const size_t at = oj.find("\"label\": \"" + label + "\"");
            const size_t no = at == std::string::npos ? std::string::npos : oj.rfind("\"costume_no\": ", at);
            return no == std::string::npos ? -1 : atoi(oj.c_str() + no + 14);
        };
        std::string vj = slurp(data_dir + "\\variants.json");
        int numbers[3];
        for (int k = 0; k < 3; k++) {
            numbers[k] = number_of(rows[k]);
            check(numbers[k] > 0 && !slot_of_row(vj, rows[k]).empty(), "a row with its outfit for each add-on and for both");
        }
        check(has(vj, ("{\"source\": \"" + source + "\"").c_str()) && !has(vj, "{\"label\": \"ZZ Body\""), "variants.json: under the sub-folder, no row for the costume alone");
        check(!has(slurp(data_dir + "\\outfits.json"), "\"label\": \"ZZ Body\""), "outfits.json: no outfit for the costume alone");
        run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");
        // Each row turned off by itself, the way the Manager writes it: its outfit goes, the others keep their numbers
        for (int k = 0; k < 3; k++) {
            put(choices, "{ \"version\": 1, \"archives\": { \"" + source + "\": { \"" + rows[k] + "\": false } } }\n");
            run_loader("costume");
            expect_said("costume", "Up to date (fingerprint match", false);
            expect_said("costume", "  variants off (choice): 1 of 3 for M. Bison/ZZ Three");
            expect_said("costume", "slots checked, 0 left out");
            vj = slurp(data_dir + "\\variants.json");
            check(has(vj, ("{\"label\": \"" + rows[k] + "\", \"enabled\": false, \"slot\": null}").c_str()), "variants.json: the row turned off has no outfit");
            check(number_of(rows[k]) == -1, "outfits.json: the outfit turned off is gone");
            numbers[k] = -1;
            for (int o = 0; o < 3; o++) {
                if (o == k) continue;
                const int now = number_of(rows[o]);
                check(now > 0 && !slot_of_row(vj, rows[o]).empty(), "the other rows have their outfits");
                check(numbers[o] < 0 || now == numbers[o], "an outfit that stays keeps its number");
                numbers[o] = now;
            }
        }
        DeleteFileA(choices.c_str());
        run_loader("costume");
        expect_said("costume", "Up to date (fingerprint match", false);
        vj = slurp(data_dir + "\\variants.json");
        for (int k = 0; k < 3; k++)
            check(number_of(rows[k]) > 0 && !slot_of_row(vj, rows[k]).empty() && (numbers[k] < 0 || number_of(rows[k]) == numbers[k]),
                  "every row on again, with its outfit");
        remove_tree(bison.substr(0, bison.size() - 1));
        run_loader("costume");  expect_row("M. Bison's fixtures removed", "1M 2S 3C");
    } else {
        printf("[25] several mods in one sub-folder of a character folder: handled together, retouches included\n");
        const std::string mods = g_row_game + "\\reframework\\costume_mods\\", bison = mods + "M. Bison\\";
        const std::string nsfw_root = g_row_game + "\\reframework\\costume_mods_nsfw\\";
        const std::string zdir = g_work + "\\zz_zips\\";
        // Retouches of M. Bison, as RE4R Wesker and Krauser Hair are: meshes only, made of the game's own (not its bytes)
        const auto bscene = game_bytes("natives/stm/product/charparam/esf/esf026/esf026v00.scn.20");
        const auto b2scene = game_bytes("natives/stm/product/charparam/esf/esf026/esf026v01.scn.20");
        std::string body_p, hair_p, head_p, body2_p, unused;
        body_of(bscene, "esf026/001/01/", body_p, unused);
        body_of(bscene, "esf026/001/02/", hair_p, unused);
        body_of(bscene, "esf026/000/00/", head_p, unused);
        body_of(b2scene, "esf026/002/01/", body2_p, unused);
        const auto body0 = game_bytes(body_p), hair0 = game_bytes(hair_p), head0 = game_bytes(head_p), body20 = game_bytes(body2_p);
        check(!body0.empty() && !hair0.empty() && !head0.empty() && !body20.empty(),
              "M. Bison's Outfit 1 body and hair, his head, his Outfit 2 body read from the game");
        using Files = std::vector<std::pair<std::string, std::vector<uint8_t>>>;
        auto changed = [](std::vector<uint8_t> d, uint8_t x) { if (!d.empty()) d.back() ^= x; return d; };
        const Files body = {{body_p, changed(body0, 0x21)}};
        const Files body_b = {{body_p, changed(body0, 0x42)}};
        const Files both = {{body_p, changed(body0, 0x63)}, {body2_p, changed(body20, 0x63)}};
        const Files hair = {{hair_p, changed(hair0, 0x21)}, {head_p, changed(head0, 0x21)}};
        const Files hair_b = {{hair_p, changed(hair0, 0x42)}};
        const Files head = {{head_p, changed(head0, 0x42)}};
        auto put_mod = [&](const std::string& dir, const std::string& name, const Files& files, const std::string& ini = {}) {
            put(dir + "\\modinfo.ini", "name=" + name + "\r\n" + ini);
            for (auto& [p, d] : files) put_bin(dir + "\\" + win(p), d);
        };
        // A .zip of a mod folder (its folder at the top of the archive, as these mods ship), by Windows' own tar
        auto zip_of = [&](const std::string& zip, const std::string& name) {
            make_dirs(zip);
            system(("cmd /c \"\"%SystemRoot%\\System32\\tar.exe\" -a -cf \"" + zip + "\" -C \"" + zdir.substr(0, zdir.size() - 1) + "\" \"" + name + "\"\"").c_str());
            check(exists(zip), "a test archive made");
            remove_tree(zdir.substr(0, zdir.size() - 1));
        };
        auto put_zip = [&](const std::string& zip, const std::string& name, const Files& files) {
            put_mod(zdir + name, name, files);
            zip_of(zip, name);
        };
        int layout = 0;
        auto clean = [&]() {   // (each layout's log is kept: row_25_<n>.txt)
            CopyFileA((g_work + "\\row_costume.txt").c_str(), (g_work + "\\row_25_" + std::to_string(layout++) + ".txt").c_str(), FALSE);
            remove_tree(bison.substr(0, bison.size() - 1));
            remove_tree(nsfw_root + "M. Bison");
        };
        auto options = [&]() { return slurp(data_dir + "\\costume_options.json"); };
        auto outfits = [&]() { return slurp(data_dir + "\\outfits.json"); };
        auto no_game_options = [&](const char* what) {
            run_loader("costume");
            expect_said("costume", "Up to date (fingerprint match", false);
            expect_said("costume", "slots checked, 0 left out");
            expect_said("costume", "retouch M. Bison", false);
            const bool none = !has(options(), "esf026/game ");
            if (!none) printf("    %s: something of the group is on a game outfit of M. Bison\n", what);
            check(none, what);
        };
        auto game_options = [&](const char* what) {
            const bool got = has(options(), "esf026/game outfit 1");
            if (!got) printf("    %s: no option on M. Bison's Outfit 1\n", what);
            check(got, what);
        };
        const char* together = "2 mods, handled together";

        clean();
        put_zip(bison + "ZZ Group\\ZZ Hair.zip", "ZZ Hair", hair);
        put_zip(bison + "ZZ Group\\ZZ Body.zip", "ZZ Body", body);
        no_game_options("two archives, one with a body mesh: nothing on a game outfit");
        expect_said("costume", "sub-folder ZZ Group: 2 mods, handled together");
        expect_said("costume", "ZZ Body: 1 add-on as runtime options (ZZ Hair)");
        expect_said("costume", "put with its add-ons in one sub-folder: worn by default");
        check(has(outfits(), "\"label\": \"ZZ Body\"") && !has(outfits(), "\"label\": \"ZZ Hair\""), "one outfit, the body's; none for the hair");
        check(has(options(), "\"ZZ Hair\""), "the hair is an option");

        clean();
        put_mod(bison + "ZZ Group\\Body", "ZZ Body", body);
        put_mod(bison + "ZZ Group\\Hair", "ZZ Hair", hair);
        no_game_options("two folder mods, one with a body mesh: nothing on a game outfit");
        expect_said("costume", together);
        expect_said("costume", "ZZ Body: 1 add-on as runtime options (ZZ Hair)");

        clean();
        put_mod(bison + "ZZ Two\\A", "ZZ Body A", body);
        put_mod(bison + "ZZ Two\\B", "ZZ Body B", body_b);
        no_game_options("two mods with a body mesh: nothing on a game outfit");
        expect_said("costume", together);
        expect_said("costume", "versions of one archive");
        check(has(options(), "\"ZZ Body B\""), "the second one is an option of the first (Version)");

        clean();
        put(zdir + "ZZ Modular\\Menu\\modinfo.ini", "name=ZZ Menu\r\nDummyMod=True\r\n");
        put_mod(zdir + "ZZ Modular\\A", "ZZ Pick A", body, "AddonFor=ZZ Menu\r\n");
        put_mod(zdir + "ZZ Modular\\B", "ZZ Pick B", body_b, "AddonFor=ZZ Menu\r\n");
        zip_of(bison + "ZZ Modular\\ZZ Modular.zip", "ZZ Modular");
        put_zip(bison + "ZZ Modular\\ZZ Hair.zip", "ZZ Hair", hair);
        no_game_options("a modular mod and an add-on: the add-on stays in the sub-folder");
        expect_said("costume", "sub-folder ZZ Modular: 2 mods, handled together");
        expect_said("costume", "for esf026 Outfit", false);
        check(has(options(), "\"ZZ Hair\""), "the add-on is an option of the sub-folder's outfit");

        clean();
        put_mod(bison + "ZZ Both\\Both", "ZZ Both", both);
        put_mod(bison + "ZZ Both\\Head", "ZZ Head", head);
        no_game_options("an outfit of two costumes and an add-on of neither of their folders: nothing on a game outfit");
        expect_said("costume", together);
        expect_said("costume", "applied to the original outfit it modifies", false);

        clean();
        put_mod(bison + "ZZ Addons\\A", "ZZ Hair A", hair);
        put_mod(bison + "ZZ Addons\\B", "ZZ Hair B", hair_b);
        run_loader("costume");
        expect_said("costume", "handled together", false);
        expect_said("costume", "add-on M. Bison/ZZ Addons/A: applied to the original outfit it modifies");
        expect_said("costume", "add-on M. Bison/ZZ Addons/B: applied to the original outfit it modifies");

        clean();
        put_mod(bison + "ZZ Body", "ZZ Body", body);
        put_mod(bison + "ZZ Hair", "ZZ Hair", hair);
        run_loader("costume");
        expect_said("costume", "handled together", false);
        expect_said("costume", "retouch M. Bison/ZZ Body: meshes / textures only");
        expect_said("costume", "retouch M. Bison/ZZ Hair: meshes / textures only");
        game_options("two folder mods loose at character level: still options of the game outfits");
        check(!has(outfits(), "\"label\": \"ZZ Body\""), "and no outfit");

        clean();
        put_zip(bison + "ZZ Body.zip", "ZZ Body", body);
        put_zip(bison + "ZZ Hair.zip", "ZZ Hair", hair);
        run_loader("costume");
        expect_said("costume", "handled together", false);
        expect_said("costume", "retouch M. Bison/ZZ Body.zip/ZZ Body: meshes / textures only");
        expect_said("costume", "retouch M. Bison/ZZ Hair.zip/ZZ Hair: meshes / textures only");
        game_options("two archives loose at character level: still options of the game outfits");

        clean();
        put_zip(bison + "ZZ Hair.zip", "ZZ Hair", hair);
        put_zip(bison + "ZZ Lone\\ZZ Body.zip", "ZZ Body", body);
        run_loader("costume");
        expect_said("costume", "ZZ Body: alone in its sub-folder ZZ Lone: its own outfit (no merge)");
        expect_said("costume", "handled together", false);
        expect_said("costume", "retouch M. Bison/ZZ Hair.zip/ZZ Hair: meshes / textures only");
        check(has(outfits(), "\"label\": \"ZZ Body\""), "alone in its sub-folder: its own outfit");
        game_options("the loose hair: still an option of the game outfits");
        expect_said("costume", "ZZ Body (ZZ Hair)", false);

        clean();   // the same layout gives three outfits in Manager mode (the other side of this case)
        put_zip(bison + "ZZ Three\\ZZ Body.zip", "ZZ Body", body);
        put_zip(bison + "ZZ Three\\ZZ Hair.zip", "ZZ Hair", hair_b);
        put_zip(bison + "ZZ Three\\ZZ Head.zip", "ZZ Head", head);
        no_game_options("a body and two add-ons that replace no same file: nothing on a game outfit");
        expect_said("costume", "sub-folder ZZ Three: 3 mods, handled together");
        expect_said("costume", "ZZ Body: 2 add-ons as runtime options");
        check(has(outfits(), "\"label\": \"ZZ Body\"") && !has(outfits(), "\"label\": \"ZZ Body + "), "one outfit, its add-ons are options");

        clean();
        put_mod(bison + "ZZ Bundle\\A", "ZZ Bundle A", body, "NameAsBundle=ZZ Bundle\r\n");
        put_mod(bison + "ZZ Bundle\\B", "ZZ Bundle B", body_b, "NameAsBundle=ZZ Bundle\r\n");
        run_loader("costume");
        expect_said("costume", "handled together", false);
        expect_said("costume", "retouch M. Bison/ZZ Bundle/A: meshes / textures only");
        game_options("the options of one unpacked bundle: still a retouch");

        clean();
        put_mod(nsfw_root + "M. Bison\\ZZ Group\\Body", "ZZ Body", body);
        put_mod(nsfw_root + "M. Bison\\ZZ Group\\Hair", "ZZ Hair", hair);
        no_game_options("a sub-folder of costume_mods_nsfw: nothing on a game outfit");
        expect_said("costume", together);
        check(has(outfits(), "\"label\": \"ZZ Body\""), "the NSFW sub-folder's outfit");

        clean();
        put_mod(bison + "ZZ Roots\\Body", "ZZ Body", body);
        put_mod(nsfw_root + "M. Bison\\ZZ Roots\\Hair", "ZZ Hair", hair);
        run_loader("costume");
        expect_said("costume", "handled together", false);
        expect_said("costume", "ZZ Body: alone in its sub-folder ZZ Roots: its own outfit (no merge)");
        expect_said("costume", "ZZ Hair: alone in its sub-folder ZZ Roots, but a pure add-on");

        clean();
        run_loader("costume");  expect_row("M. Bison's fixtures removed", "1M 2S 3C");
    }

    if (previous && !g_manager_suite) {
        printf("[23] an install built by the previous build: this one says Up to date; built afresh, the same pak byte for byte\n");
        remove_tree(mroot.substr(0, mroot.size() - 1));
        DeleteFileA(registry.c_str());
        DeleteFileA(probes.c_str());
        run_loader("costume", previous);  expect_row("previous build", "1M 2S 3C");
        const std::string before = slurp(costume_pak_path());
        run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");
        DeleteFileA(registry.c_str());
        DeleteFileA((data_dir + "\\pak_sources.tsv").c_str());
        run_loader("costume");  expect_row("this build, from scratch", "1M 2S 3C");
        check(!before.empty() && slurp(costume_pak_path()) == before, "the same pak byte for byte");
        check(has(slurp(data_dir + "\\outfits.json"), "\"loose_files_kept\": true"), "this build says loose_files_kept");
    }

    printf("[24] Manager mode (one outfit per variant): a main and five add-ons that each replace its body mesh, in one "
           "archive (fork 1.10.5)\n");
    {
        const std::string options_file = data_dir + "\\costume_options.json";
        check(exists(options_file) != g_manager_suite, "before this case: an options manifest in normal mode, none in Manager mode");
        if (!g_manager_suite) set_manager_mode(true);
        const auto s2 = game_bytes(c2_scene);
        std::string body_m, body_d, hair_m, hair_d;
        body_of(s2, "esf001/002/01/", body_m, body_d);
        for (int part = 2; part <= 9 && hair_m.empty(); part++) {
            char pp[32];
            snprintf(pp, sizeof pp, "esf001/002/%02d/", part);
            body_of(s2, pp, hair_m, hair_d);
        }
        auto body = game_bytes(body_m), hair = game_bytes(hair_m);
        const auto mdf = game_bytes(body_d);
        check(!body.empty() && !mdf.empty() && !hair.empty(),
              "Ryu's Outfit 2 body mesh, its material file and another part's mesh read from the game");
        if (!body.empty()) body.back() ^= 0x22;
        if (!hair.empty()) hair.back() ^= 0x22;
        auto changed = [](std::vector<uint8_t> d, int k) { if (d.size() > 1) d[d.size() - 2] ^= uint8_t(k); return d; };
        const std::string over = g_row_game + "\\reframework\\costume_mods\\Ryu\\ZZ Overrides.zip";
        const std::string mixed = g_row_game + "\\reframework\\costume_mods\\Ryu\\ZZ Mixed.zip";
        std::vector<std::pair<std::string, std::vector<uint8_t>>> z = {
            {"00 Main/modinfo.ini", bytes_of("name=Fixture Main\r\n")}, {"00 Main/" + body_m, body}, {"00 Main/" + body_d, mdf}};
        for (int k = 1; k <= 5; k++) {
            const std::string dir = "0" + std::to_string(k) + " Option " + std::to_string(k) + "/";
            z.push_back({dir + "modinfo.ini", bytes_of("name=Option " + std::to_string(k) + "\r\nAddonFor=Fixture Main\r\n")});
            z.push_back({dir + body_m, changed(body, k)});
        }
        write_stored_zip(over, z);
        // Control: four add-ons that do not all replace the same files (two of the body, two of another part)
        const auto mbody = changed(body, 0x40), mhair = changed(hair, 0x40);
        z = {{"00 Main/modinfo.ini", bytes_of("name=Mixed Main\r\n")}, {"00 Main/" + body_m, mbody}, {"00 Main/" + body_d, mdf},
             {"00 Main/" + hair_m, mhair}};
        for (int k = 1; k <= 4; k++) {
            const std::string dir = "0" + std::to_string(k) + " Mixed " + std::to_string(k) + "/";
            z.push_back({dir + "modinfo.ini", bytes_of("name=Mixed " + std::to_string(k) + "\r\nAddonFor=Mixed Main\r\n")});
            z.push_back({dir + (k <= 2 ? body_m : hair_m), changed(k <= 2 ? mbody : mhair, k)});
        }
        write_stored_zip(mixed, z);
        run_loader("costume");  expect_row("Manager mode, both archives added", "1M 2S 3C");
        expect_said("costume", "Manager mode: installed by the Slotsloader Manager");
        expect_said("costume", "runtime options: off (Manager mode: one outfit per variant)");
        check(!exists(options_file), "Manager mode: the options manifest of the normal mode is gone (the same pak name: the plugin would use it)");
        expect_said("costume", "Fixture Main: 5 add-ons that each replace the same files: one variant each");
        for (int k = 1; k <= 5; k++)
            expect_said("costume", ("  variant: Fixture Main + Option " + std::to_string(k)).c_str());
        expect_said("costume", "Fixture Main: 5 add-ons, too many to combine", false);
        expect_said("costume", "Mixed Main: 4 add-ons, too many to combine; the base alone is installed");
        expect_said("costume", "  variant: Mixed Main +", false);
        expect_said("costume", "slots checked, 0 left out");
        const std::string vj = slurp(data_dir + "\\variants.json");
        check(has(vj, "\"runtime_options\": \"off\""), "variants.json: runtime options off");
        int own = has(vj, "{\"label\": \"Fixture Main\", \"enabled\": true, \"slot\": \"Outfit ") ? 1 : 0;
        for (int k = 1; k <= 5; k++)
            if (has(vj, ("{\"label\": \"Fixture Main + Option " + std::to_string(k) + "\", \"enabled\": true, \"slot\": \"Outfit ").c_str()))
                own++;
        check(own == 6, "variants.json: the main and main + each add-on, each with its outfit");
        check(has(vj, "{\"label\": \"Mixed Main\", \"enabled\": true, \"slot\": \"Outfit ") && !has(vj, "\"Mixed Main +"),
              "variants.json: Mixed Main alone");
        run_loader("costume");  expect_said("costume", "Up to date (fingerprint match");
        if (!g_manager_suite) {   // installed any other way: runtime options, as before (case 22)
            set_manager_mode(false);
            run_loader("costume");  expect_row("not the Manager's install any more", "1M 2S 3C");
            expect_said("costume", "Up to date (fingerprint match", false);   // a change of mode rebuilds
            expect_said("costume", "Manager mode: installed by the Slotsloader Manager", false);
            expect_said("costume", "runtime options: on (one outfit per modular mod)");
            check(exists(options_file), "back in normal mode: the options manifest is written again");
            expect_said("costume", "that each replace the same files: one variant each", false);
            expect_said("costume", "too many to combine", false);
        }
        DeleteFileA(over.c_str());
        DeleteFileA(mixed.c_str());
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 6 && (std::string(argv[1]) == "--row" || std::string(argv[1]) == "--row-manager")) {
        g_manager_suite = std::string(argv[1]) == "--row-manager";
        return run_row_test(argv[2], argv[3], argv[4], argv[5], argc >= 7 ? argv[6] : nullptr);
    }
    if (argc >= 3 && argv[1][0] != '-') return run_clean_tests(argv[1], argv[2]);
    printf("usage: slots_clean_test <SF6_SlotsClean.exe> <work dir>\n"
           "       slots_clean_test --row <build dir> <work dir> <game dir (read only)> <costume mod archive>"
           " [<previous build dir>]\n"
           "       slots_clean_test --row-manager <build dir> <work dir> <game dir (read only)> <costume mod archive>\n");
    return 2;
}
