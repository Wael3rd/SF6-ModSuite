// update_test - the updater's guards, without the game and without the network: versions, the manifest read as hostile input, what is
// offered, which zip entries may be written where (zip slip), the all-or-nothing swap and its journal, the clean-up, the "ignored"
// memory, the restart's waiter. Its files only under --work (a fake game folder made there); never the real game folder.
//
// update_test.exe --work <empty or disposable folder>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../src/update_fs.hpp"
#include "../src/update_install.hpp"
#include "../src/update_logic.hpp"
#include "../src/update_sys.hpp"
#include "../src/update_text.hpp"
#include "../../common/third_party/miniz.h"

using namespace sf6up;
namespace fs = std::filesystem;

static int g_checks = 0, g_failed = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_failed; printf("FAILED: %s\n", what.c_str()); }
}
static std::wstring g_work;

static void put(const std::wstring& p, const std::string& text) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream f(fs::path(p), std::ios::binary);
    f << text;
}
static std::string get(const std::wstring& p) {
    std::string s;
    read_file(p, &s, 64u << 20);
    return s;
}
// Every file of a tree: relative path -> content
static std::map<std::string, std::string> snapshot(const std::wstring& root) {
    std::map<std::string, std::string> m;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::path(root), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string rel = narrow(fs::relative(it->path(), fs::path(root)).wstring());
        m[rel + (it->is_directory() ? "/" : "")] = it->is_directory() ? "" : get(it->path().wstring());
    }
    return m;
}

// ---- versions ----
static void test_versions() {
    check(newer("1.26.1", "1.26.0"), "1.26.1 > 1.26.0");
    check(!newer("1.26.0", "1.26.1"), "1.26.0 not > 1.26.1");
    check(!newer("1.26.1", "1.26.1"), "equal is not newer");
    check(newer("0.9.10", "0.9.9"), "0.9.10 > 0.9.9 (numbers, not text)");
    check(newer("1.10.0", "1.9.9"), "1.10.0 > 1.9.9");
    check(!newer("1.26", "1.26.0") && !newer("1.26.0", "1.26"), "1.26 == 1.26.0");
    check(newer("1.26.1", "1.26.1-beta"), "a release after its beta");
    check(!newer("1.26.1-beta", "1.26.1"), "a beta before its release");
    check(newer("1.26.1-rc2", "1.26.1-rc1"), "rc2 > rc1");
    check(!newer("2.0.0", "garbage") && !newer("garbage", "1.0"), "not a version: never newer");
    check(!parse_version("") && !parse_version("1..2") && !parse_version(".1") && !parse_version("1.") && !parse_version("v1.2"), "malformed versions");
    check(!parse_version("1.2.3.4.5"), "5 numbers");
    check(!parse_version("1234567.0"), "a number of 7 digits");
    check(!parse_version("1.0-") && !parse_version("1.0-a b") && !parse_version("1.0-../x"), "bad suffixes");
    check(compare_versions("2.2.9", "2.2.10") == -1, "2.2.9 < 2.2.10");
}

// ---- the manifest ----
static Manifest good_manifest() {
    Manifest m;
    m.format = kManifestFormat;
    m.suite = "1.30.1";
    m.tag = "v1.30.1";
    for (auto& k : known_packages()) {
        ManifestPackage p;
        p.id = k.id;
        p.name = k.name;
        p.version = "9.9.9";
        p.asset = std::string(k.id) + "_9.9.9.zip";
        p.size = 1234;
        p.sha256 = std::string(64, 'a');
        p.proof = k.proof;
        m.packages.push_back(p);
    }
    return m;
}

static bool parses(const std::string& t, std::string* why = nullptr) {
    Manifest m;
    std::string w;
    const bool ok = parse_manifest(t, &m, &w);
    if (why) *why = w;
    return ok;
}

static std::string replace_once(std::string s, const std::string& a, const std::string& b) {
    const size_t at = s.find(a);
    if (at != std::string::npos) s.replace(at, a.size(), b);
    return s;
}

static void test_manifest() {
    const std::string good = manifest_json(good_manifest());
    Manifest m;
    std::string why;
    check(parse_manifest(good, &m, &why) && m.packages.size() == 5 && m.suite == "1.30.1", "a good manifest: " + why);
    // malformed
    check(!parses(""), "empty");
    check(!parses("not json"), "not JSON");
    check(!parses("[]"), "an array");
    check(!parses(good.substr(0, good.size() / 2)), "cut in half");
    check(!parses(std::string(70000, ' ') + "{}"), "over 64 KiB");
    check(!parses(std::string(2000, '[') + std::string(2000, ']')), "nested 2000 deep");
    {
        std::string deep = "{\"format\":\"sf6suite-manifest-1\",\"suite\":\"1.0\",\"tag\":\"v1.0\",\"packages\":[],\"x\":" + std::string(9, '[') + std::string(9, ']') + "}";
        check(!parses(deep), "nested 11 deep");
    }
    // wrong types
    check(!parses(replace_once(good, "\"size\": 1234", "\"size\": \"1234\"")), "size as a string");
    check(!parses(replace_once(good, "\"size\": 1234", "\"size\": 1234.5")), "size not an integer");
    check(!parses(replace_once(good, "\"size\": 1234", "\"size\": -1")), "negative size");
    check(!parses(replace_once(good, "\"size\": 1234", "\"size\": 99999999999")), "size over 64 MiB");
    check(!parses(replace_once(good, "\"version\": \"9.9.9\"", "\"version\": 9")), "version as a number");
    check(!parses(replace_once(good, "\"version\": \"9.9.9\"", "\"version\": \"latest\"")), "version not a version");
    check(!parses(replace_once(good, "\"suite\": \"1.30.1\"", "\"suite\": [\"1.30.1\"]")), "suite as an array");
    check(!parses(replace_once(good, "\"packages\": [", "\"packages\": {\"a\": [") + "}"), "packages as an object");
    check(!parses(replace_once(good, "\"format\": \"sf6suite-manifest-1\"", "\"format\": \"sf6suite-manifest-2\"")), "an unknown format");
    check(!parses(replace_once(good, "\"tag\": \"v1.30.1\"", "\"tag\": \"v1.30.1/../../x\"")), "a tag that is not v<suite>");
    check(!parses(replace_once(good, "\"sha256\": \"" + std::string(64, 'a'), "\"sha256\": \"" + std::string(63, 'a') + "Z")), "sha256 not hex");
    // paths in the asset
    for (const char* bad : { "../evil.zip", "..\\\\evil.zip", "C:\\\\x.zip", "/etc/x.zip", "a/b.zip", "x.zip/", "x.dll", "x..zip", ".zip", "x:y.zip", "%2e%2e.zip" })
        check(!parses(replace_once(good, "\"asset\": \"slots_loader_9.9.9.zip\"", std::string("\"asset\": \"") + bad + "\"")), std::string("asset ") + bad);
    // the proof must be the one this code knows
    check(!parses(replace_once(good, "\"proof\": \"reframework/plugins/SF6_ColorModNative.dll\"", "\"proof\": \"amd_ags_x64.dll\"")), "another proof file");
    check(!parses(replace_once(good, "\"proof\": \"reframework/plugins/SF6_ColorModNative.dll\"", "\"proof\": \"C:/Windows/System32/x.dll\"")), "an absolute proof");
    check(!parses(replace_once(good, "\"proof\": \"reframework/plugins/SF6_ColorModNative.dll\"", "\"proof\": \"reframework/plugins/../../x.dll\"")), "a proof with ..");
    // a package twice, an asset twice
    {
        Manifest d = good_manifest();
        d.packages.push_back(d.packages[0]);
        check(!parses(manifest_json(d)), "a package twice");
        Manifest e = good_manifest();
        e.packages[1].asset = e.packages[0].asset;
        check(!parses(manifest_json(e)), "an asset twice");
    }
    // unknown packages: skipped, the rest kept
    {
        std::string t = replace_once(good, "\"packages\": [", "\"packages\": [{\"id\": \"future_mod\", \"whatever\": [1, 2]},");
        check(parse_manifest(t, &m, &why) && m.packages.size() == 5 && m.skipped.size() == 1 && m.skipped[0] == "future_mod", "an unknown package skipped: " + why);
        check(!parses(replace_once(good, "\"packages\": [", "\"packages\": [{\"id\": \"../x\"},")), "an unknown id that is not a name");
        check(!parses(replace_once(good, "\"packages\": [", "\"packages\": [7,")), "a package that is not an object");
    }
    {
        Manifest many = good_manifest();
        for (int i = 0; i < 20; ++i) { ManifestPackage p; p.id = "x" + std::to_string(i); many.packages.push_back(p); }
        check(!parses(manifest_json(many)), "17 packages and more");
    }
}

// ---- detection and the plan ----
static void test_plan() {
    const std::wstring root = g_work + L"\\plan_game";
    remove_tree(root);
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "plugin");
    put(join(root, "reframework/plugins/SF6_ColorModNative.dll"), "cm");
    put(join(root, "reframework/plugins/sf6core_native.dll"), "core");
    put(join(root, "reframework/plugins/SF6_CRSMNative.dll"), "crsm");      // installed, reports no version
    std::map<std::string, std::string> reports = { { "slots_loader", "1.26.0" }, { "color_mod", "0.26.0" }, { "native_core", "0.9.10" }, { "profile_mod", "1.0.0" } };
    auto reader = [&](const KnownPackage& k) { auto it = reports.find(k.id); return it == reports.end() ? std::string() : it->second; };
    const auto inst = detect_installed(root, reader);
    check(inst.size() == 5, "five packages looked at");
    int present = 0;
    for (auto& i : inst) present += i.present;
    check(present == 4, "four present (profile_mod's DLL is not there, whatever it would report)");
    Manifest m = good_manifest();
    for (auto& p : m.packages) {
        if (p.id == "slots_loader") p.version = "1.26.1";
        if (p.id == "color_mod") p.version = "0.26.1";
        if (p.id == "native_core") p.version = "0.9.10";
        if (p.id == "recording_manager") p.version = "2.3.0";
        if (p.id == "profile_mod") p.version = "1.1.0";
    }
    Plan plan = make_plan(m, inst, false, "");
    check(plan.items.size() == 2, "two offered (slots 1.26.0 -> 1.26.1, colour 0.26.0 -> 0.26.1): " + std::to_string(plan.items.size()));
    if (plan.items.size() == 2)
        check(plan.items[0].pkg.id == "slots_loader" && plan.items[0].from == "1.26.0" && plan.items[1].pkg.id == "color_mod", "the right two");
    bool crsm_note = false;
    for (auto& n : plan.notes) crsm_note |= n.find("recording_manager") != std::string::npos;
    check(crsm_note, "the Recording Manager without a version: noted, not offered");
    // older or equal never offered (a developer's newer build is never nagged)
    reports["slots_loader"] = "1.27.0-dev";
    reports["color_mod"] = "0.26.1";
    plan = make_plan(m, detect_installed(root, reader), false, "");
    check(plan.items.empty(), "a newer local build and an equal one: nothing");
    reports["slots_loader"] = "1.26.0";
    reports["color_mod"] = "0.26.0";
    // the Slotsloader Manager owns the Slots Loader
    plan = make_plan(m, detect_installed(root, reader), true, "");
    check(plan.items.size() == 1 && plan.items[0].pkg.id == "color_mod", "manager mode: the Slots Loader left out");
    // ignored
    plan = make_plan(m, detect_installed(root, reader), false, "1.30.1");
    check(plan.items.empty(), "the ignored release: nothing");
    plan = make_plan(m, detect_installed(root, reader), false, "1.30.0");
    check(plan.items.size() == 2, "a later release than the ignored one: asked again");

    // manager mode read from the Manager's list, as the plugin reads it
    check(!manager_mode(root), "no list: not manager mode");
    const std::string list = "{\"format\": \"sf6-manager-installs-1\", \"mods\": [{\"enabled\": true, \"files\": [{\"path\": \"reframework\\\\plugins\\\\SF6_CostumeSlotsNative.dll\", \"size\": 6}]}]}";
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), list);
    check(manager_mode(root), "the Manager's list holds the plugin at its size: manager mode");
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "plugin, replaced since");
    check(!manager_mode(root), "the plugin replaced since: not manager mode");
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), "{ broken");
    check(!manager_mode(root), "a broken list: not manager mode");
}

// ---- zip entries ----
static void test_entries() {
    struct Case { const char* name; Entry want; };
    const Case cases[] = {
        { "modinfo.ini", Entry::Skip }, { "README.txt", Entry::Skip },
        { "amd_ags_x64.dll", Entry::File }, { "amd_ags_x64_real.dll", Entry::File }, { "SF6_SlotsClean.exe", Entry::File },
        { "reframework/plugins/SF6_ColorModNative.dll", Entry::File },
        { "reframework/data/SF6_NativeLocale_data/locale.json", Entry::File },
        { "reframework/data/SF6_Costumes_Data/loader/static/esf.scn.20", Entry::File },
        { "reframework/data/SF6_CRSMNative_data/settings.json", Entry::KeepIfPresent },
        { "reframework/costume_mods/LISEZMOI.txt", Entry::File }, { "reframework/costume_mods/SF6_CostumeAudit.bat", Entry::File },
        { "reframework/costume_mods/Ryu/", Entry::Dir }, { "reframework/color_mods/all_chars/", Entry::Dir }, { "reframework/stage_mods/README.txt", Entry::File },
        { "reframework/data/SF6_Update_data/", Entry::Refuse }, { "reframework/data/SF6_Update_data/state.json", Entry::Refuse },
        { "reframework/costume_mods/settings.ini", Entry::KeepIfPresent },
        // zip slip and friends
        { "../evil.dll", Entry::Refuse }, { "..\\evil.dll", Entry::Refuse }, { "reframework/plugins/../../evil.dll", Entry::Refuse },
        { "reframework/../../evil.dll", Entry::Refuse }, { "reframework/data/x/../../../evil.dll", Entry::Refuse },
        { "/evil.dll", Entry::Refuse }, { "\\evil.dll", Entry::Refuse }, { "C:/Windows/evil.dll", Entry::Refuse }, { "C:evil.dll", Entry::Refuse },
        { "\\\\server\\share\\evil.dll", Entry::Refuse }, { "reframework/plugins/x.dll:stream", Entry::Refuse },
        { "reframework/plugins/./x.dll", Entry::Refuse }, { "reframework//plugins/x.dll", Entry::Refuse },
        { "reframework/plugins/x.dll.", Entry::Refuse }, { "reframework/plugins/x.dll ", Entry::Refuse },
        { "reframework/data/x/CON", Entry::Refuse }, { "reframework/data/x/nul.txt", Entry::Refuse }, { "reframework/data/x/COM1.json", Entry::Refuse },
        { "reframework/data/x/a\x01.txt", Entry::Refuse }, { "reframework/data/x/a?.txt", Entry::Refuse },
        // outside the known roots
        { "dinput8.dll", Entry::Refuse }, { "StreetFighter6.exe", Entry::Refuse }, { "steam_api64.dll", Entry::Refuse }, { "re_chunk_000.pak", Entry::Refuse },
        { "natives/stm/x.pak", Entry::Refuse }, { "reframework/autorun/x.lua", Entry::Refuse }, { "reframework/plugins/x.exe", Entry::Refuse },
        { "reframework/plugins/sub/x.dll", Entry::Refuse }, { "reframework/x.dll", Entry::Refuse }, { "reframework/data/x.json", Entry::Refuse },
        { "reframework/data/SF6_Community_data/identity.json", Entry::Refuse },
        { "reframework/costume_mods/Ryu/mod.zip", Entry::Refuse }, { "reframework/costume_mods/Ryu/sub/", Entry::Refuse },
        { "reframework/data/x/a.dll.old-1.0", Entry::Refuse }, { "", Entry::Refuse }, { "a/", Entry::Refuse },
    };
    for (auto& c : cases) {
        const EntryRule r = classify_entry(c.name);
        check(r.kind == c.want, std::string("entry \"") + c.name + "\": got " + std::to_string((int)r.kind) + " " + r.why);
    }
    check(classify_entry(std::string(201, 'a')).kind == Entry::Refuse, "a name over 200 characters");
    check(classify_entry("reframework/data/a/b/c/d/e/f/g.txt").kind == Entry::Refuse, "9 levels");
}

// A zip of (name, data); names with "@@" are patched afterwards to start with '/' (miniz refuses to write one)
static std::wstring make_zip(const std::wstring& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    for (auto& e : entries) mz_zip_writer_add_mem(&z, e.first.c_str(), e.second.data(), e.second.size(), (mz_uint)MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
    std::string data(static_cast<const char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&z);
    for (size_t at; (at = data.find("@@")) != std::string::npos;) data.replace(at, 2, "/x");
    put(path, data);
    return path;
}

static void test_extract() {
    const std::wstring dir = g_work + L"\\extract";
    remove_tree(dir);
    const std::wstring tree = dir + L"\\tree";
    Staged st;
    std::string why;
    const std::vector<std::pair<std::string, std::string>> ok_entries = {
        { "modinfo.ini", "name=x" }, { "README.txt", "readme" }, { "amd_ags_x64.dll", "proxy" }, { "reframework/plugins/A.dll", "a" },
        { "reframework/costume_mods/Ryu/", "" }, { "reframework/data/Pkg_data/locale.json", "{}" }, { "reframework/data/SF6_CRSMNative_data/settings.json", "{}" },
        { "reframework/data/Pkg_data/caf\xc3\xa9.txt", "utf8 name" },
    };
    check(extract_zip(make_zip(dir + L"\\ok.zip", ok_entries), tree, "pkg", "1.0", &st, &why), "a good zip: " + why);
    check(st.files.size() == 5 && st.dirs.size() == 1, "5 files, 1 folder (modinfo.ini and README.txt skipped): " + std::to_string(st.files.size()));
    check(!exists(join(tree, "modinfo.ini")) && get(join(tree, "reframework/plugins/A.dll")) == "a", "the files staged");
    check(exists(join(tree, "reframework/data/Pkg_data/caf\xc3\xa9.txt")), "a UTF-8 name staged");
    bool keep = false;
    for (auto& f : st.files) keep |= f.kind == Entry::KeepIfPresent;
    check(keep, "settings.json staged as the player's");

    // hostile zips: refused, and nothing of them written
    const std::vector<std::vector<std::pair<std::string, std::string>>> bad = {
        { { "reframework/plugins/A.dll", "a" }, { "../evil.dll", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "reframework/plugins/../../../evil.dll", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "..\\..\\evil.dll", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "C:\\Windows\\evil.dll", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "@@evil.dll", "x" } },                         // "/xevil.dll": an absolute path
        { { "reframework/plugins/A.dll", "a" }, { "reframework/autorun/evil.lua", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "dinput8.dll", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "reframework/plugins/a.DLL", "x" } },          // the same file twice (case)
        { { "reframework/plugins/A.dll", "a" }, { "reframework/costume_mods/Ryu/mod.zip", "x" } },
        { { "reframework/plugins/A.dll", "a" }, { "reframework/data/SF6_Community_data/identity.json", "x" } },
    };
    int k = 0;
    for (auto& entries : bad) {
        const std::wstring t = dir + L"\\bad" + std::to_wstring(k) + L"_tree";
        Staged s2;
        const bool got = extract_zip(make_zip(dir + L"\\bad" + std::to_wstring(k) + L".zip", entries), t, "pkg", "1.0", &s2, &why);
        check(!got, "hostile zip " + std::to_string(k) + " refused");
        check(!exists(t) && s2.files.empty(), "hostile zip " + std::to_string(k) + ": nothing staged (" + why + ")");
        ++k;
    }
    check(!exists(g_work + L"\\evil.dll") && !exists(dir + L"\\evil.dll"), "no evil.dll anywhere");
    check(!extract_zip(dir + L"\\none.zip", tree, "pkg", "1.0", &st, &why), "a missing zip");
    put(dir + L"\\junk.zip", "PK not really");
    check(!extract_zip(dir + L"\\junk.zip", tree, "pkg", "1.0", &st, &why), "not a zip");
}

// ---- the swap ----
static void make_game(const std::wstring& root) {
    remove_tree(root);
    put(join(root, "amd_ags_x64.dll"), "proxy old");
    put(join(root, "reframework/plugins/A.dll"), "A old");
    put(join(root, "reframework/plugins/B.dll"), "B same");
    put(join(root, "reframework/data/Pkg_data/locale.json"), "locale old");
    put(join(root, "reframework/data/SF6_CRSMNative_data/settings.json"), "the player's settings");
    put(join(root, "reframework/data/Pkg_data/user_state.json"), "not shipped: untouched");
    put(join(root, "reframework/costume_mods/Ryu/my_mod.zip"), "the player's mod");
}
static Staged make_tree(const std::wstring& tree) {
    remove_tree(tree);
    Staged st;
    auto add = [&](const std::string& rel, const std::string& data, Entry kind = Entry::File) {
        put(join(tree, rel), data);
        st.files.push_back({ rel, kind, "pkg", "1.0.0" });
    };
    add("amd_ags_x64.dll", "proxy new");
    add("reframework/plugins/A.dll", "A new");
    add("reframework/plugins/B.dll", "B same");
    add("reframework/plugins/C.dll", "C added");
    add("reframework/data/Pkg_data/locale.json", "locale new");
    add("reframework/data/SF6_CRSMNative_data/settings.json", "shipped defaults", Entry::KeepIfPresent);
    st.dirs = { "reframework/costume_mods/Ryu", "reframework/costume_mods/Ken" };
    return st;
}

static void test_swap() {
    const std::wstring root = g_work + L"\\swap_game", tree = g_work + L"\\swap_tree", journal = g_work + L"\\swap_journal.json";
    // every step failing in turn: the game folder exactly as before
    for (int fail = 0; fail < 4; ++fail) {
        make_game(root);
        const auto before = snapshot(root);
        const Staged st = make_tree(tree);
        const SwapResult r = swap_in(root, tree, st, journal, fail);
        check(!r.ok, "failure at step " + std::to_string(fail) + ": reported");
        const auto after = snapshot(root);
        check(after == before, "failure at step " + std::to_string(fail) + ": the game folder unchanged (" + r.why + ")");
        check(!exists(journal), "failure at step " + std::to_string(fail) + ": the journal removed");
        check(get(join(tree, "reframework/plugins/A.dll")) == "A new", "failure at step " + std::to_string(fail) + ": the new file back in the staging tree");
    }
    // a file the game holds open without sharing (a file in use): the swap stops and puts everything back
    {
        make_game(root);
        const Staged st = make_tree(tree);
        HANDLE h = CreateFileW(join(root, "reframework/data/Pkg_data/locale.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto before = snapshot(root);   // (the locked file reads as empty in both snapshots)
        const SwapResult r = swap_in(root, tree, st, journal);
        const auto after = snapshot(root);
        CloseHandle(h);
        check(!r.ok && after == before, "a locked file: nothing changed (" + r.why + ")");
    }
    // success
    make_game(root);
    const Staged st = make_tree(tree);
    const SwapResult r = swap_in(root, tree, st, journal);
    check(r.ok, "the swap: " + r.why);
    check(r.replaced == 3 && r.added == 1 && r.same == 1 && r.kept == 1, "3 replaced, 1 added, 1 unchanged, 1 kept: " + std::to_string(r.replaced) + " " +
          std::to_string(r.added) + " " + std::to_string(r.same) + " " + std::to_string(r.kept));
    check(get(join(root, "amd_ags_x64.dll")) == "proxy new" && get(join(root, "reframework/plugins/A.dll")) == "A new" &&
          get(join(root, "reframework/plugins/C.dll")) == "C added" && get(join(root, "reframework/data/Pkg_data/locale.json")) == "locale new", "the new files in place");
    check(get(join(root, "reframework/plugins/A.dll.old-1.0.0")) == "A old" && get(join(root, "amd_ags_x64.dll.old-1.0.0")) == "proxy old", "the old ones kept as .old-<version>");
    check(!exists(join(root, "reframework/plugins/B.dll.old-1.0.0")), "an unchanged file not touched");
    check(get(join(root, "reframework/data/SF6_CRSMNative_data/settings.json")) == "the player's settings", "the player's settings kept");
    check(get(join(root, "reframework/data/Pkg_data/user_state.json")) == "not shipped: untouched" && get(join(root, "reframework/costume_mods/Ryu/my_mod.zip")) == "the player's mod",
          "the player's files untouched");
    check(exists(join(root, "reframework/costume_mods/Ken")), "a missing folder made");
    check(!exists(journal), "the journal removed");
    // a second update of the same file: the next free name
    {
        Staged st2 = make_tree(tree);
        put(join(tree, "reframework/plugins/A.dll"), "A newer");
        const SwapResult r2 = swap_in(root, tree, st2, journal);
        check(r2.ok && get(join(root, "reframework/plugins/A.dll.old-1.0.0.1")) == "A new", "a taken backup name: .1");
    }
    // the clean-up: only the backups named, only old copies
    {
        std::string l;
        std::vector<std::string> list = r.backups;
        list.push_back("reframework/plugins/A.dll");                   // not a backup name: never deleted
        list.push_back("dinput8.dll.old-1.0");                         // not a path a package writes: never deleted
        put(join(root, "dinput8.dll.old-1.0"), "x");
        const auto left = cleanup_backups(root, list, &l);
        check(left.empty(), "the clean-up: " + l);
        check(!exists(join(root, "reframework/plugins/A.dll.old-1.0.0")) && !exists(join(root, "amd_ags_x64.dll.old-1.0.0")), "the old copies deleted");
        check(exists(join(root, "reframework/plugins/A.dll")) && exists(join(root, "dinput8.dll.old-1.0")), "nothing else deleted");
        // a backup still in use (opened without delete sharing): kept for the next start
        put(join(root, "reframework/plugins/D.dll.old-1.0"), "in use");
        HANDLE h = CreateFileW(join(root, "reframework/plugins/D.dll.old-1.0").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto left2 = cleanup_backups(root, { "reframework/plugins/D.dll.old-1.0" }, &l);
        CloseHandle(h);
        check(left2.size() == 1, "a backup in use: kept for the next start");
    }
}

// A swap stopped in the middle (the game crashed): undone at the next start
static void test_journal() {
    const std::wstring root = g_work + L"\\journal_game", tree = g_work + L"\\journal_tree", journal = g_work + L"\\journal.json";
    make_game(root);
    const auto before = snapshot(root);
    const Staged st = make_tree(tree);
    // the steps the swap would take, done by hand up to the middle: A renamed and replaced, the proxy renamed only, C added
    std::string esc;
    for (char c : narrow(tree)) { esc += c; if (c == '\\') esc += '\\'; }
    put(journal, "{\"format\":\"sf6suite-swap-1\",\"tree\":\"" + esc + "\",\"steps\":["
                 "{\"target\":\"amd_ags_x64.dll\",\"backup\":\"amd_ags_x64.dll.old-1.0.0\"},"
                 "{\"target\":\"reframework/plugins/A.dll\",\"backup\":\"reframework/plugins/A.dll.old-1.0.0\"},"
                 "{\"target\":\"reframework/plugins/C.dll\",\"backup\":\"\"},"
                 "{\"target\":\"reframework/data/Pkg_data/locale.json\",\"backup\":\"reframework/data/Pkg_data/locale.json.old-1.0.0\"}]}");
    MoveFileW(join(root, "amd_ags_x64.dll").c_str(), join(root, "amd_ags_x64.dll.old-1.0.0").c_str());
    MoveFileW(join(root, "reframework/plugins/A.dll").c_str(), join(root, "reframework/plugins/A.dll.old-1.0.0").c_str());
    MoveFileW(join(tree, "reframework/plugins/A.dll").c_str(), join(root, "reframework/plugins/A.dll").c_str());
    MoveFileW(join(tree, "reframework/plugins/C.dll").c_str(), join(root, "reframework/plugins/C.dll").c_str());
    std::string l;
    check(recover_journal(root, journal, &l), "a journal found");
    const auto after = snapshot(root);
    check(after == before, "the half-done swap undone: " + l);
    check(!exists(journal), "the journal removed");
    check(!recover_journal(root, journal, &l), "no journal: nothing");
}

// ---- the state: the ignored release, the off switch, unknown keys kept ----
static void test_state() {
    State s = parse_state("");
    check(s.check && s.ignored.empty() && s.cleanup.empty(), "no file: on, nothing ignored");
    s = parse_state("{\"check_for_updates\": false, \"ignored_suite_version\": \"1.30.1\", \"my_note\": \"keep me\", \"pending_cleanup\": [\"a.dll.old-1\", 5]}");
    check(!s.check && s.ignored == "1.30.1" && s.cleanup.size() == 1, "read back");
    s.ignored = "1.31.0";
    const std::string out = state_json(s);
    const State t = parse_state(out);
    check(!t.check && t.ignored == "1.31.0" && out.find("keep me") != std::string::npos, "written back with the player's own keys");
    check(parse_state("{\"check_for_updates\": \"no\"}").check, "a wrong type: the default (on)");
    check(parse_state("[1,2]").check && parse_state("garbage").check, "garbage: the defaults");
    // through the file, as the host does
    const Paths p(g_work + L"\\state_game");
    remove_tree(p.root);
    check(update_state(p, [](State& st) { st.ignored = "1.30.1"; }), "state.json written");
    check(load_state(p).ignored == "1.30.1" && load_state(p).check, "the ignored release remembered");
    put(p.state, "{\"check_for_updates\": false}");
    check(update_state(p, [](State& st) { st.ignored = "1.30.2"; }) && !load_state(p).check, "the player's off switch kept when the module writes");
}

// ---- the restart's waiter: run for real against a short process, with a marker file instead of Steam ----
static void test_waiter() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring cmd = waiter_command(sys, 1234, L"StreetFighter6.exe", L"start \"\" \"steam://rungameid/1364780\"");
    check(cmd.find(L"PID eq 1234") != std::wstring::npos && cmd.find(L"IMAGENAME eq StreetFighter6.exe") != std::wstring::npos &&
          cmd.find(L"steam://rungameid/1364780") != std::wstring::npos && cmd.find(L"(1,1,120)") != std::wstring::npos, "the waiter's command line");
    // a process that lives ~3 s (ping), the marker written once it is gone
    const std::wstring marker = g_work + L"\\waiter_marker.txt";
    DeleteFileW(marker.c_str());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring pcmd = std::wstring(sys) + L"\\PING.EXE -n 4 127.0.0.1";
    if (!CreateProcessW(nullptr, pcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { check(false, "ping started"); return; }
    std::string why;
    const ULONGLONG t0 = GetTickCount64();
    check(start_waiter_with(pi.dwProcessId, L"PING.EXE", L"echo restarted> \"" + marker + L"\"", &why), "the waiter started: " + why);
    bool early = false;
    while (GetTickCount64() - t0 < 30000 && !exists(marker)) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && exists(marker)) early = true;
        Sleep(200);
    }
    const DWORD alive = WaitForSingleObject(pi.hProcess, 0);
    check(alive == WAIT_OBJECT_0, "the process ended");
    check(exists(marker) && !early, "the marker written after the process ended, not before");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// ---- the boxes' texts ----
static void test_texts() {
    Plan plan;
    plan.suite = "1.31.0";
    ManifestPackage a, b;
    a.name = "SF6 Slots Loader"; a.version = "1.27.0";
    b.name = "SF6 Color Mod"; b.version = "0.27.0";
    plan.items = { { a, "1.26.1" }, { b, "0.26.1" } };
    const std::string en = text::offer("en", plan), fr = text::offer("fr", plan);
    check(en.find("SF6 Mod Suite 1.31.0 is out") == 0, "English offer: " + en);
    check(en.find("SF6 Slots Loader 1.26.1 -> 1.27.0\nSF6 Color Mod 0.26.1 -> 0.27.0") != std::string::npos, "one line per package: " + en);
    check(en.find("never ask again for 1.31.0") != std::string::npos, "the release ignored is named");
    check(fr.find("SF6 Mod Suite 1.31.0 est sortie") == 0 && fr.find("SF6 Color Mod 0.26.1 -> 0.27.0") != std::string::npos, "French offer: " + fr);
    check(text::offer("xx", plan) == en, "an unknown language: English");
    check(text::title("ja") != text::title("en") && !text::title("ja").empty(), "Japanese title");
    const std::string blocked = text::failed("en", Fail::Blocked, "SF6_ColorModNative.dll");
    check(blocked.find("Smart App Control blocks SF6_ColorModNative.dll") != std::string::npos, "the blocked file named: " + blocked);
    check(text::failed("fr", Fail::Download, "").find("un téléchargement a échoué") != std::string::npos, "French download failure");
    check(text::done("en").find("Fluffy Mod Manager") != std::string::npos, "the success text names Fluffy once");
    check(text::format("%s and %s, 100%%", { "a" }) == "a and , 100%", "format: a missing argument, a percent");
    for (const char* l : { "en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar" }) {
        const std::string t = text::offer(l, plan);
        check(t.find("1.31.0") != std::string::npos && t.find("0.26.1 -> 0.27.0") != std::string::npos && t.find("%s") == std::string::npos,
              std::string("offer in ") + l + ": every slot filled");
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--work")) g_work = widen(argv[i + 1]);
    if (g_work.empty()) { printf("usage: update_test.exe --work <folder>\n"); return 2; }
    make_dirs(g_work);
    test_versions();
    test_manifest();
    test_plan();
    test_entries();
    test_extract();
    test_swap();
    test_journal();
    test_state();
    test_texts();
    test_waiter();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
