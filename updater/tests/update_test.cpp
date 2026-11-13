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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <functional>
#include <vector>

#include "../src/update_fs.hpp"
#include "../src/update_install.hpp"
#include "../src/update_logic.hpp"
#include "../src/update_pe.hpp"
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
    // the tools (the Mod Sorter): optional, each entry on its own
    {
        auto tool = [] {
            ManifestTool t;
            t.id = "mod_sorter"; t.name = "SF6 Mod Sorter"; t.version = "1.1.0"; t.asset = "SF6_ModSorter_1.1.0.zip";
            t.size = 4321; t.sha256 = std::string(64, 'b'); t.file = "SF6_ModSorter.exe";
            return t;
        };
        Manifest w = good_manifest();
        w.tools.push_back(tool());
        const std::string with = manifest_json(w);
        Manifest r;
        check(parse_manifest(with, &r, &why) && r.packages.size() == 5 && r.tools.size() == 1 && r.tool_notes.empty(), "a manifest with a tool: " + why);
        const ManifestTool* t = find_tool(r, "mod_sorter");
        check(t && t->version == "1.1.0" && t->asset == "SF6_ModSorter_1.1.0.zip" && t->size == 4321 && t->file == "SF6_ModSorter.exe" &&
              t->sha256 == std::string(64, 'b'), "the tool read back");
        check(!find_tool(r, "other_tool") && !find_tool(m, "mod_sorter"), "a tool that is not there");
        check(manifest_json(r) == with, "a manifest with a tool: written back the same");
        check(manifest_json(good_manifest()).find("\"tools\"") == std::string::npos, "no tool: no tools key");
        // one bad entry: left out with a note, the packages kept
        auto left_out = [&](const std::string& what, const std::function<void(ManifestTool&)>& spoil) {
            Manifest b = good_manifest();
            b.tools.push_back(tool());
            spoil(b.tools[0]);
            Manifest g;
            const bool ok = parse_manifest(manifest_json(b), &g, &why);
            check(ok && g.packages.size() == 5 && g.tools.empty() && g.tool_notes.size() == 1, "a tool " + what + ": left out, the manifest kept");
        };
        left_out("whose file is a path", [](ManifestTool& x) { x.file = "..\\SF6_ModSorter.exe"; });
        left_out("whose file is in a folder", [](ManifestTool& x) { x.file = "bin/SF6_ModSorter.exe"; });
        left_out("whose file is no program", [](ManifestTool& x) { x.file = "SF6_ModSorter.dll"; });
        left_out("without a version", [](ManifestTool& x) { x.version = "latest"; });
        left_out("whose asset is a path", [](ManifestTool& x) { x.asset = "../x.zip"; });
        left_out("whose asset is a package's", [](ManifestTool& x) { x.asset = "slots_loader_9.9.9.zip"; });
        left_out("whose sha256 is not hex", [](ManifestTool& x) { x.sha256 = std::string(64, 'z'); });
        left_out("of no size", [](ManifestTool& x) { x.size = 0; });
        left_out("over 64 MiB", [](ManifestTool& x) { x.size = kAssetMaxBytes + 1; });
        left_out("whose id is not a name", [](ManifestTool& x) { x.id = "../x"; });
        {
            Manifest two = good_manifest();
            two.tools = { tool(), tool() };
            Manifest g;
            check(parse_manifest(manifest_json(two), &g, &why) && g.tools.size() == 1 && g.tool_notes.size() == 1, "a tool twice: the first kept");
        }
        Manifest g;
        check(parse_manifest(replace_once(good, "\"packages\": [", "\"tools\": {\"id\": \"mod_sorter\"}, \"packages\": ["), &g, &why) &&
              g.packages.size() == 5 && g.tools.empty() && g.tool_notes.size() == 1, "tools that is no list: left out");
        check(parse_manifest(replace_once(good, "\"packages\": [", "\"tools\": [7, \"x\"], \"packages\": ["), &g, &why) &&
              g.packages.size() == 5 && g.tools.empty() && g.tool_notes.size() == 2, "tools that are no objects: left out");
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

    // any package the Manager installed is the Manager's (the Native Core beside a loader it did not install)
    for (auto& p : m.packages)
        if (p.id == "native_core") p.version = "0.9.11";
    plan = make_plan(m, detect_installed(root, reader), manager_mode(root), "");
    check(plan.items.size() == 3, "no list: the core offered with the two others: " + std::to_string(plan.items.size()));
    const std::string core_rel = "reframework/plugins/sf6core_native.dll";
    const std::string core_list = "{\"format\": \"sf6-manager-installs-1\", \"mods\": [{\"enabled\": true, \"files\": [{\"path\": \"Reframework\\\\Plugins\\\\SF6Core_Native.dll\", \"size\": 4}]}]}";
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), core_list);
    check(manager_lists(root, core_rel) && !manager_mode(root), "the Manager's list holds the core at its size, not the plugin");
    plan = make_plan(m, detect_installed(root, reader), manager_mode(root), "");
    check(plan.items.size() == 3, "the Manager's core beside a loader it did not install: not manager mode, offered as always: " + std::to_string(plan.items.size()));
    // manager mode (the list holds the plugin too): the Manager's core is left out with the loader
    const std::string both_list = "{\"format\": \"sf6-manager-installs-1\", \"mods\": [{\"enabled\": true, \"files\": [{\"path\": \"reframework/plugins/SF6_CostumeSlotsNative.dll\", \"size\": 6}, "
                                  "{\"path\": \"Reframework\\\\Plugins\\\\SF6Core_Native.dll\", \"size\": 4}]}]}";
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), both_list);
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "plugin");      // the size the list gives again (replaced above)
    check(manager_mode(root) && manager_lists(root, core_rel), "the Manager's list holds the plugin and the core");
    plan = make_plan(m, detect_installed(root, reader), manager_mode(root), "");
    bool core_offered = false, core_note = false;
    for (auto& it : plan.items) core_offered |= it.pkg.id == "native_core";
    for (auto& n : plan.notes) core_note |= n.find("native_core: installed by the Slotsloader Manager") != std::string::npos;
    check(plan.items.size() == 1 && plan.items[0].pkg.id == "color_mod" && !core_offered && core_note, "manager mode: the Manager's core left out and noted with the loader, the Color Mod offered");
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), core_list);
    put(join(root, core_rel), "core, replaced since");
    check(!manager_lists(root, core_rel), "the core replaced since: no longer the Manager's");
    put(join(root, core_rel), "core");
    std::string off_list = core_list;
    off_list.replace(off_list.find("\"enabled\": true"), 15, "\"enabled\": false");
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), off_list);
    check(!manager_lists(root, core_rel), "a mod turned off in the Manager claims nothing");
    check(!manager_lists(root, "reframework/plugins/not_there.dll"), "a file that is not there: not the Manager's");

    // the list gives each file's sha256 (the Manager writes it; older Managers did not): size and sha256 must both match
    const std::string plugin_sha = "5e689e2b01672bf33996e75d5e372ff60c536ce1599a1458e867cd8f4bef5160";   // of "plugin"
    const std::string core_sha = "0d45f5fd462b8c70bffb10021ac1bcff3f58f29b1faf7568595095427d42812c";     // of "core"
    const std::string sha_list = "{\"format\": \"sf6-manager-installs-1\", \"mods\": [{\"enabled\": true, \"files\": ["
                                 "{\"path\": \"reframework/plugins/SF6_CostumeSlotsNative.dll\", \"size\": 6, \"sha256\": \"" + plugin_sha + "\"}, "
                                 "{\"path\": \"reframework/plugins/sf6core_native.dll\", \"size\": 4, \"sha256\": \"" + core_sha + "\"}]}]}";
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), sha_list);
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "plugin");
    std::string why;
    check(manager_mode(root, &why) && why.find("size and sha256 matched") != std::string::npos, "sha256 in the list, the same plugin: manager mode (" + why + ")");
    check(manager_lists(root, core_rel), "sha256 in the list, the same core: the Manager's");
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "PLUGIN");   // same size, other bytes: put back by hand or by Fluffy
    check(!manager_mode(root, &why) && why.find("sha256 differs") != std::string::npos, "same size, other sha256: not manager mode (" + why + ")");
    plan = make_plan(m, detect_installed(root, reader), manager_mode(root), "");
    bool loader_offered = false;
    for (auto& it : plan.items) loader_offered |= it.pkg.id == "slots_loader";
    check(loader_offered, "same size, other sha256: the Slots Loader offered as always");
    put(join(root, core_rel), "CORE");
    check(!manager_lists(root, core_rel), "the core at the listed size with other bytes: not the Manager's");
    put(join(root, core_rel), "core");
    std::string upper_list = sha_list;
    for (size_t at = upper_list.find(plugin_sha), i = 0; i < plugin_sha.size(); ++i) upper_list[at + i] = (char)std::toupper((unsigned char)upper_list[at + i]);
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), upper_list);
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "plugin");
    check(manager_mode(root), "a sha256 in capitals: still the same file");
    put(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json"), both_list);   // no sha256: the size alone, as before
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "PLUGIN");
    check(manager_mode(root, &why) && why.find("no sha256") != std::string::npos, "no sha256 in the list: the size alone decides, as before (" + why + ")");
    check(manager_lists(root, core_rel), "no sha256 in the list: the core at its size is the Manager's, as before");
    std::filesystem::remove(std::filesystem::path(join(root, "reframework/data/SF6_Costumes_Data/manager_installs.json")));
    check(!manager_mode(root, &why) && why.empty(), "no list: nothing to log");
    put(join(root, "reframework/plugins/SF6_CostumeSlotsNative.dll"), "plugin");
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
    // one file of a zip's root (a tool's program)
    {
        const std::wstring z = make_zip(dir + L"\\tool.zip", { { "modinfo.ini", "name=x" }, { "README.txt", "readme" },
                                                                 { "sub/SF6_ModSorter.exe", "in a folder" }, { "SF6_ModSorter.exe", "the program" } });
        const std::wstring out = dir + L"\\one\\SF6_ModSorter.exe.new";
        fs::create_directories(fs::path(out).parent_path());
        check(extract_one(z, "SF6_ModSorter.exe", out, &why) && get(out) == "the program", "one file taken out of a zip: " + why);
        check(extract_one(z, "sf6_modsorter.EXE", out, &why) && get(out) == "the program", "its name whatever the case");
        check(snapshot(dir + L"\\one").size() == 1, "nothing else written");
        check(!extract_one(z, "Other.exe", dir + L"\\one\\o.exe", &why) && !exists(dir + L"\\one\\o.exe"), "a file the zip does not hold");
        check(!extract_one(dir + L"\\none.zip", "SF6_ModSorter.exe", dir + L"\\one\\o.exe", &why), "no zip");
        put(dir + L"\\junk.zip", "this is no zip");
        check(!extract_one(dir + L"\\junk.zip", "SF6_ModSorter.exe", dir + L"\\one\\o.exe", &why), "not a zip");
        const std::wstring twice = make_zip(dir + L"\\twice.zip", { { "A.exe", "one" }, { "a.exe", "two" } });
        check(!extract_one(twice, "A.exe", dir + L"\\one\\o.exe", &why) && !exists(dir + L"\\one\\o.exe"), "the same name twice: refused");
        const std::wstring empty = make_zip(dir + L"\\empty.zip", { { "A.exe", "" } });
        check(!extract_one(empty, "A.exe", dir + L"\\one\\o.exe", &why), "an empty program: refused");
    }
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

    // the Slotsloader Manager's installs: nothing is fetched from the game unless the player asks for it
    std::string why;
    check(check_allowed(parse_state(""), false, &why) && why.empty(), "not manager mode: checked");
    check(!check_allowed(parse_state(""), true, &why) && why.find("Slotsloader Manager") != std::string::npos, "manager mode: off, and the log says why");
    check(check_allowed(parse_state("{\"check_in_manager_mode\": true}"), true, nullptr), "manager mode, asked for: checked");
    check(!check_allowed(parse_state("{\"check_for_updates\": false, \"check_in_manager_mode\": true}"), true, &why) && why.find("check_for_updates") != std::string::npos,
          "the off switch wins");
    check(!check_allowed(parse_state("{\"check_for_updates\": false}"), false, nullptr), "the off switch, not manager mode: off as before");
    check(!check_allowed(parse_state("{\"check_in_manager_mode\": \"yes\"}"), true, nullptr), "a wrong type: the default (off in manager mode)");
    s = parse_state("{\"check_in_manager_mode\": true}");
    s.ignored = "1.31.0";
    check(parse_state(state_json(s)).check_managed, "the player's key kept when the module writes");
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

// ---- a package's version read from its DLL's file (the mod sorter: the game closed, nothing loaded) ----
// A small PE32+ image: one section at 0x1000 (file 0x200) with the export directory, the export's code and its string
static std::string pe_image(const std::string& export_name, const std::string& version, int shape) {
    std::string b(0x400, '\0');
    auto u16 = [&](size_t o, uint16_t v) { std::memcpy(&b[o], &v, 2); };
    auto u32 = [&](size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); };
    auto raw = [](uint32_t rva) { return size_t(rva - 0x1000 + 0x200); };
    u16(0, 0x5A4D); u32(0x3C, 0x40); u32(0x40, 0x00004550);
    u16(0x44, 0x8664); u16(0x46, 1); u16(0x54, 240);                    // machine, one section, the optional header's size
    u16(0x58, 0x20B); u32(0x58 + 108, 16); u32(0x58 + 112, 0x1000); u32(0x58 + 116, 40);
    const size_t row = 0x58 + 240;
    u32(row + 8, 0x200); u32(row + 12, 0x1000); u32(row + 16, 0x200); u32(row + 20, 0x200);
    u32(raw(0x1000) + 20, 1); u32(raw(0x1000) + 24, 1); u32(raw(0x1000) + 28, 0x1040); u32(raw(0x1000) + 32, 0x1044); u32(raw(0x1000) + 36, 0x1048);
    u32(raw(0x1040), shape == 1 ? 0x1070 : 0x1080);                     // shape 1: the export is a jmp to the code
    u32(raw(0x1044), 0x1050);
    std::memcpy(&b[raw(0x1050)], export_name.c_str(), export_name.size() + 1);
    const unsigned char jmp[5] = { 0xE9, 0x0B, 0, 0, 0 };               // 0x1070 + 5 + 0x0B = 0x1080
    std::memcpy(&b[raw(0x1070)], jmp, 5);
    const unsigned char lea[8] = { 0x48, 0x8D, 0x05, 0x19, 0, 0, 0, 0xC3 };   // 0x1080 + 7 + 0x19 = 0x10A0
    std::memcpy(&b[raw(0x1080)], lea, 8);
    if (shape == 2) b[raw(0x1080) + 1] = char(0x8B);                    // shape 2: a mov, not the lea
    std::memcpy(&b[raw(0x10A0)], version.data(), version.size());       // shape 3: no end within 32 characters
    if (shape == 3) std::memset(&b[raw(0x10A0)], '1', 0x60);
    return b;
}

static void test_pe() {
    const std::string name = "sf6suite_package_version";
    const std::string good = pe_image(name, "1.27.0", 0);
    check(image_package_version(good) == "1.27.0", "the version of a DLL's file: " + image_package_version(good));
    check(image_package_version(pe_image(name, "0.9.11", 1)) == "0.9.11", "the export a jmp to its code");
    check(image_package_version(pe_image(name, "2.2.10-beta", 0)) == "2.2.10-beta", "a suffix");
    check(image_package_version(pe_image("sf6suite_updater_rank", "1.27.0", 0)).empty(), "no such export: nothing");
    check(image_package_version(pe_image(name + "2", "1.27.0", 0)).empty(), "a longer export name: nothing");
    check(image_package_version(pe_image(name, "hello", 0)).empty(), "not a version: nothing");
    check(image_package_version(pe_image(name, "1.27.0", 2)).empty(), "another code shape: nothing");
    check(image_package_version(pe_image(name, "1", 3)).empty(), "a string without its end: nothing");
    check(image_package_version("").empty() && image_package_version("MZ").empty(), "not an image: nothing");
    bool cut_ok = true;
    for (size_t n = 0; n < good.size(); ++n) {                          // every cut of the file: no read outside it
        const std::string v = image_package_version(good.substr(0, n));
        cut_ok = cut_ok && (v.empty() || v == "1.27.0");
    }
    check(cut_ok, "a cut file: nothing, or the version when all it needs is there");
    std::string wild = good;
    for (size_t o : { size_t(0x3C), size_t(0x58 + 112), size_t(0x58 + 240 + 12), size_t(0x58 + 240 + 20), size_t(0x21C), size_t(0x220), size_t(0x240), size_t(0x244), size_t(0x283) }) {
        std::string w = good;
        const uint32_t big = 0xFFFFFFF0u;
        std::memcpy(&w[o], &big, 4);
        check(image_package_version(w).empty(), "an offset out of the file at " + std::to_string(o) + ": nothing");
    }
    const std::wstring f = g_work + L"\\pe_test.dll";
    put(f, good);
    check(file_package_version(f) == "1.27.0", "read from a file");
    check(file_package_version(g_work + L"\\no_such.dll").empty(), "no file: nothing");
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
    check(en.find("(Ignore: never ask again for 1.31.0.)") != std::string::npos, "the release ignored is named, with the button that ignores it");
    check(fr.find("SF6 Mod Suite 1.31.0 est sortie") == 0 && fr.find("SF6 Color Mod 0.26.1 -> 0.27.0") != std::string::npos, "French offer: " + fr);
    check(text::offer("xx", plan) == en, "an unknown language: English");
    check(text::title("ja") != text::title("en") && !text::title("ja").empty(), "Japanese title");
    const std::string blocked = text::failed("en", Fail::Blocked, "SF6_ColorModNative.dll");
    check(blocked.find("Smart App Control blocks SF6_ColorModNative.dll") != std::string::npos, "the blocked file named: " + blocked);
    check(text::failed("fr", Fail::Download, "").find("un téléchargement a échoué") != std::string::npos, "French download failure");
    check(text::done("en").find("Fluffy Mod Manager") != std::string::npos, "the success text names Fluffy once");
    for (const char* l : { "en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar" }) {
        const std::string sz = text::show_zip(l), d = text::done(l);
        check(!sz.empty() && sz.size() <= 24 && d.find(sz) != std::string::npos && d.find("Fluffy Mod Manager") != std::string::npos,
              std::string("restart box in ") + l + ": its last line names the third button's word");
    }
    check(text::format("%s and %s, 100%%", { "a" }) == "a and , 100%", "format: a missing argument, a percent");
    for (const char* l : { "en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar" }) {
        const std::string t = text::offer(l, plan);
        check(t.find("1.31.0") != std::string::npos && t.find("0.26.1 -> 0.27.0") != std::string::npos && t.find("%s") == std::string::npos,
              std::string("offer in ") + l + ": every slot filled");
        const std::string ign = text::ignore(l);
        check(!ign.empty() && t.find(ign) != std::string::npos, std::string("offer in ") + l + ": its last line names the third button's word");
    }
    // the ticker's lines while an update installs, and the progress they read
    {
        Progress pr;
        check(pr.percent() == 0, "progress: nothing yet");
        pr.total = 1000; pr.done = 420;
        check(pr.percent() == 42, "progress: 42 %");
        pr.done = 999;
        check(pr.percent() == 99, "progress: never 100 before the end");
        pr.done = 5000;
        check(pr.percent() == 100, "progress: capped at 100");
        pr.total = 0;
        check(pr.percent() == 0, "progress: no total");
        pr.total = (int64_t)60 << 20; pr.done = (int64_t)59 << 20;
        check(pr.percent() == 98, "progress: large sizes");
        pr.step = Progress::Install;
        pr.reset();
        check(pr.step == Progress::Idle && pr.done == 0 && pr.total == 0, "progress: reset");
        const std::string d = text::downloading("en", "1.34.0", 42);
        check(d == "Downloading SF6 Mod Suite 1.34.0: 42 %", "English download line: " + d);
        check(text::installing("en", "1.34.0") == "Installing SF6 Mod Suite 1.34.0...", "English install line");
        for (const char* l : { "en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar" }) {
            const std::string dl = text::downloading(l, "1.34.0", 7), in = text::installing(l, "1.34.0");
            check(dl.find("1.34.0") != std::string::npos && dl.find("7") != std::string::npos && dl.find("%s") == std::string::npos &&
                  dl.find("%%") == std::string::npos && dl.find('%') != std::string::npos, std::string("download line in ") + l + ": " + dl);
            check(in.find("1.34.0") != std::string::npos && in.find("%s") == std::string::npos && in != dl, std::string("install line in ") + l + ": " + in);
        }
    }
}

// ---- the zips kept for Fluffy Mod Manager's players: downloads/, previous_version/ ----
static void test_downloads() {
    check(asset_family("SF6_SlotsLoader_v1.27.2.zip") == "sf6_slotsloader", "family: _v1.27.2");
    check(asset_family("SF6_NativeCore_0.9.12.zip") == "sf6_nativecore", "family: _0.9.12");
    check(asset_family("SF6_CRSM_2.2.10.zip") == asset_family("SF6_CRSM_2.2.9.zip"), "family: two versions, one package");
    check(asset_family("SF6_ColorMod_0.26.2.zip") != asset_family("SF6_CRSM_2.2.10.zip"), "family: two packages");
    check(asset_family("SF6_ModSuite_AllInOne_v1.35.0.zip") == "sf6_modsuite_allinone", "family: the AllInOne");
    check(asset_family("Readme.ZIP") == "readme" && asset_family("notes.txt").empty() && asset_family(".zip").empty(), "family: odd names");
    check(asset_family("SF6_Thing_vX.zip") == "sf6_thing_vx", "family: no version, the whole name");

    const std::wstring root = join(g_work, "downloads_game");
    remove_tree(root);
    const Paths p(root);
    const std::wstring from = join(root, "staging_zips");
    auto stage = [&](const std::string& n) { put(join(from, n), "zip " + n); };
    auto kept = [&](const std::wstring& dir) {
        std::vector<std::string> v;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(fs::path(dir), ec))
            if (e.is_regular_file()) v.push_back(narrow(e.path().filename().wstring()));
        std::sort(v.begin(), v.end());
        std::string s;
        for (auto& x : v) s += (s.empty() ? "" : ",") + x;
        return s;
    };
    check(newest_download(p).empty(), "no download yet: nothing to show");
    // the first update: kept, nothing before it
    stage("SF6_SlotsLoader_v1.27.2.zip"); stage("SF6_NativeCore_0.9.12.zip");
    check(keep_downloads(p, { "SF6_SlotsLoader_v1.27.2.zip", "SF6_NativeCore_0.9.12.zip" }, from, nullptr) == 2, "first update: two zips kept");
    check(kept(p.downloads) == "SF6_NativeCore_0.9.12.zip,SF6_SlotsLoader_v1.27.2.zip", "first update: in downloads/ " + kept(p.downloads));
    check(!exists(p.previous), "first update: no previous_version/");
    check(get(join(p.downloads, "SF6_SlotsLoader_v1.27.2.zip")) == "zip SF6_SlotsLoader_v1.27.2.zip", "first update: the zip itself");
    check(!exists(join(from, "SF6_SlotsLoader_v1.27.2.zip")), "first update: moved, not copied");
    // the second: the Slots Loader's old zip goes to previous_version/, the Native Core's stays (not in this update)
    Sleep(20);
    stage("SF6_SlotsLoader_v1.27.3.zip");
    check(keep_downloads(p, { "SF6_SlotsLoader_v1.27.3.zip" }, from, nullptr) == 1, "second update: one zip kept");
    check(kept(p.downloads) == "SF6_NativeCore_0.9.12.zip,SF6_SlotsLoader_v1.27.3.zip", "second update: downloads/ " + kept(p.downloads));
    check(kept(p.previous) == "SF6_SlotsLoader_v1.27.2.zip", "second update: previous_version/ " + kept(p.previous));
    check(newest_download(p) == join(p.downloads, "SF6_SlotsLoader_v1.27.3.zip"), "the newest zip is the one shown");
    // the third: two versions at most, the oldest one gone
    stage("SF6_SlotsLoader_v1.27.4.zip"); stage("SF6_NativeCore_0.9.13.zip");
    check(keep_downloads(p, { "SF6_SlotsLoader_v1.27.4.zip", "SF6_NativeCore_0.9.13.zip" }, from, nullptr) == 2, "third update: two zips kept");
    check(kept(p.downloads) == "SF6_NativeCore_0.9.13.zip,SF6_SlotsLoader_v1.27.4.zip", "third update: downloads/ " + kept(p.downloads));
    check(kept(p.previous) == "SF6_NativeCore_0.9.12.zip,SF6_SlotsLoader_v1.27.3.zip", "third update: previous_version/ " + kept(p.previous));
    // the same release again (a reinstall): the new copy replaces it, previous_version/ unchanged
    put(join(from, "SF6_SlotsLoader_v1.27.4.zip"), "again");
    check(keep_downloads(p, { "SF6_SlotsLoader_v1.27.4.zip" }, from, nullptr) == 1, "the same zip again: kept");
    check(get(join(p.downloads, "SF6_SlotsLoader_v1.27.4.zip")) == "again", "the same zip again: the new copy");
    check(kept(p.previous) == "SF6_NativeCore_0.9.12.zip,SF6_SlotsLoader_v1.27.3.zip", "the same zip again: previous_version/ unchanged");
    // a zip the install did not leave (an odd name, none there): nothing kept, nothing broken
    check(keep_downloads(p, { "SF6_ColorMod_0.26.3.zip", "manifest.json" }, from, nullptr) == 0, "nothing to keep: 0");
    check(kept(p.downloads) == "SF6_NativeCore_0.9.13.zip,SF6_SlotsLoader_v1.27.4.zip", "nothing to keep: downloads/ unchanged");
    remove_tree(root);
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
    test_downloads();
    test_pe();
    test_waiter();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
