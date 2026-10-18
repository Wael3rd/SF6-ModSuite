// Offline test of reframework/color_mods: scan, archive extraction + cache, stale purge, catalogue roots and order.
// Build and run with build_color_mods_test.bat (needs build\archive_deps.lib from build.bat archive_deps).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/engine/catalog.hpp"
#include "../src/engine/color_mods.hpp"
#include "../src/screens/colour_state.hpp"
#include "miniz.h"

namespace fs = std::filesystem;
using namespace engine;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::vector<std::string> g_log;
static void logfn(const std::string& s) { g_log.push_back(s); std::printf("    log: %s\n", s.c_str()); }
static bool logged(const char* part) { for (auto& l : g_log) if (l.find(part) != std::string::npos) return true; return false; }

static void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

static void make_zip(const fs::path& out, const std::vector<std::pair<std::string, std::string>>& entries, bool encrypt = false) {
    fs::create_directories(out.parent_path());
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, out.string().c_str(), 0)) { std::printf("zip init failed\n"); std::exit(2); }
    for (auto& [n, d] : entries) mz_zip_writer_add_mem(&z, n.c_str(), d.data(), d.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    if (encrypt) {                                   // set the "encrypted" bit of every local and central header
        std::ifstream in(out, std::ios::binary);
        std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        for (size_t i = 0; i + 10 < b.size(); ++i) {
            if (b.compare(i, 4, "PK\x03\x04", 4) == 0) b[i + 6] = (char)(b[i + 6] | 1);
            if (b.compare(i, 4, "PK\x01\x02", 4) == 0) b[i + 8] = (char)(b[i + 8] | 1);
        }
        std::ofstream(out, std::ios::binary | std::ios::trunc) << b;
    }
}

static int count_files(const fs::path& d) {
    int n = 0;
    std::error_code ec;
    if (!fs::exists(d, ec)) return 0;
    for (auto& e : fs::recursive_directory_iterator(d, ec)) if (e.is_regular_file()) ++n;
    return n;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "color_mods_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    const fs::path data = tmp / "reframework" / "data";
    const fs::path root = color_mods::root_of(data);
    const std::string body = "USR2-not-parsed-here";

    // the Color Mod's former colour folder: one pack, one editor colour, its READMEs; what names them by path
    const std::string old_mc = "SF6_ColorSpinExtra_data\\colors\\Ryu_esf001\\MANUALCOLORS\\esf001_001_cmd_001.user.2";
    const std::string new_mc = "..\\color_mods\\Ryu\\MANUALCOLORS\\esf001_001_cmd_001.user.2";
    put(data / "SF6_ColorSpinExtra_data/colors/Ryu_esf001/packA/esf001_001_cmd_001.user.2", body);
    put(data / "SF6_ColorSpinExtra_data/colors/Ryu_esf001/MANUALCOLORS/esf001_001_cmd_001.user.2", body);
    put(data / "SF6_ColorSpinExtra_data/colors/Ryu_esf001/README.txt", "Drop Ryu colour files here");
    put(data / "SF6_ColorSpinExtra_data/colors/Ken_esf010/README.txt", "Drop Ken colour files here");
    put(data / "SF6_ColorSpinExtra_data/colors/Zed_esf099/p/esf099_001_cmd_001.user.2", body);   // an id not in the list
    put(data / "SF6_ColorSpinExtra_data/state.json",
        "{\n    \"p1\": {\n        \"costume\": 0,\n        \"fighter\": 1,\n        \"path\": \"SF6_ColorSpinExtra_data\\\\colors\\\\Ryu_esf001\\\\MANUALCOLORS\\\\esf001_001_cmd_001.user.2\",\n        \"rw\": 2\n    }\n}");
    put(data / "SF6_ColorSpinExtra_data/chip_slots.json",
        "{\"SF6_ColorSpinExtra_data\\\\colors\\\\Ryu_esf001\\\\MANUALCOLORS\\\\esf001_001_cmd_001.user.2\":[1,2],\"1_0\":{}}");
    const std::string old_okey = "1_0_file_" + Catalog::sanitize("SF6_ColorSpinExtra_data\\colors\\Ryu_esf001\\packA\\esf001_001_cmd_001.user.2");
    const std::string new_okey = "1_0_file_" + Catalog::sanitize("..\\color_mods\\Ryu\\packA\\esf001_001_cmd_001.user.2");
    put(data / "SF6_ColorSpinExtra_data/overrides" / (old_okey + ".user.2"), body);

    // 0. the former folder moves into color_mods, once; the paths that named its files follow them
    {
        auto m = color_mods::migrate_legacy(data, logfn);
        CHECK(m.moved == 3 && m.failed == 0 && m.duplicates == 0 && m.renamed_overrides == 1 && m.removed);
        CHECK(!fs::exists(data / "SF6_ColorSpinExtra_data/colors"));
        CHECK(fs::exists(root / "Ryu/packA/esf001_001_cmd_001.user.2") && fs::exists(root / "Ryu/MANUALCOLORS/esf001_001_cmd_001.user.2"));
        CHECK(fs::exists(root / "Zed/p/esf099_001_cmd_001.user.2"));                 // unknown id: the old folder's name
        CHECK(!fs::exists(root / "Ryu/README.txt"));                                  // the old drop notes are not carried over
        std::ifstream s(data / "SF6_ColorSpinExtra_data/state.json", std::ios::binary);
        const std::string st((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
        s.close();
        CHECK(st.find("..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS\\\\esf001_001_cmd_001.user.2") != std::string::npos && st.find("colors") == std::string::npos);
        std::ifstream c(data / "SF6_ColorSpinExtra_data/chip_slots.json", std::ios::binary);
        const std::string ch((std::istreambuf_iterator<char>(c)), std::istreambuf_iterator<char>());
        c.close();
        CHECK(ch == "{\"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS\\\\esf001_001_cmd_001.user.2\":[1,2],\"1_0\":{}}");
        CHECK(!fs::exists(data / "SF6_ColorSpinExtra_data/overrides" / (old_okey + ".user.2")));
        CHECK(fs::exists(data / "SF6_ColorSpinExtra_data/overrides" / (new_okey + ".user.2")));
        CHECK(logged("files moved to color_mods") && logged("folder removed"));
        // nothing there any more: a second run does nothing
        auto again = color_mods::migrate_legacy(data, logfn);
        CHECK(again.moved == 0 && !again.removed);
        fs::remove_all(root / "Zed");
        (void)new_mc;
    }

    // color_mods: nested natives layout (Nexus), flat layout, garbage, encrypted, unpacked folder, loose file, dot folder
    make_zip(root / "Ryu/NexusMod.zip", { { "NexusMod/modinfo.ini", "name=x" },
                                          { "NexusMod/preview.png", "png" },
                                          { "NexusMod/natives/stm/product/model/esf/esf001/001/esf001_001_cmd_001.user.2", body },
                                          { "NexusMod/natives/stm/product/model/esf/esf001/001/esf001_001_cmd_002.user.2", body },
                                          { "NexusMod/natives/stm/product/model/esf/esf001/001/esf001_001_cmd_000.user.2", "" } });
    make_zip(root / "Luke/Flat.zip", { { "esf002_001_cmd_001.user.2", body }, { "readme.txt", "hello" } });
    put(root / "Ken/Garbage.zip", "this is not an archive at all");
    make_zip(root / "Ken/Locked.zip", { { "esf010_001_cmd_001.user.2", body } }, true);
    put(root / "Ken/Unpacked/esf010_001_cmd_005.user.2", body);
    put(root / "esf011_001_cmd_001.user.2", body);                       // loose, directly in color_mods
    put(root / ".hidden/esf003_001_cmd_001.user.2", body);               // dot folder: ignored
    put(root / "Ryu/.esf001_001_cmd_009.user.2", body);                  // dot file: ignored
    put(root / ".cache/old_deadbeef/.complete", "");                     // cache of no archive
    put(root / ".cache/half_00000000.partial/esf001_001_cmd_007.user.2", body);   // interrupted extraction

    // 1. cold start (nothing extracted yet): loose files only, dot things ignored
    {
        auto cand = color_mods::candidate_files(root);
        CHECK(cand.size() == 4);                                         // packA + MANUALCOLORS (moved), Unpacked, loose
        CHECK(color_mods::valid_cache_dirs(root).empty());
        CHECK(color_mods::list_archives(root).size() == 4);              // NexusMod, Flat, Garbage, Locked
    }

    // 2. sync
    auto rep = color_mods::sync(root, logfn);
    CHECK(rep.extracted == 2);
    CHECK(rep.failed == 2);                                              // garbage + encrypted
    CHECK(rep.purged == 2);                                              // the orphan cache and the .partial
    CHECK(rep.changed());
    CHECK(logged("NexusMod.zip (zip) extracted in"));
    CHECK(logged("Garbage.zip is not a zip, 7z or rar file"));
    CHECK(logged("Locked.zip") && logged("CANNOT be used"));
    CHECK(!fs::exists(root / ".cache/old_deadbeef"));
    CHECK(!fs::exists(root / ".cache/half_00000000.partial"));
    {
        auto dirs = color_mods::valid_cache_dirs(root);
        CHECK(dirs.size() == 2);
        CHECK(count_files(root / ".cache") == 7);                        // Nexus: modinfo + cmd 000/001/002 + .complete; Flat: cmd + .complete (no png, no readme)
    }

    // 3. catalogue: the packs by folder (the archive cache, then Ryu\packA), the editor colour last
    Catalog cat;
    cat.build(data);
    {
        auto* ryu = cat.entries(1, 0);
        CHECK(ryu && ryu->size() == 4);                                  // Nexus 001, Nexus 002 (000 is empty), packA, MANUAL
        if (ryu && ryu->size() == 4) {
            CHECK((*ryu)[0].label == "RW 1" && (*ryu)[0].n == 1 && !(*ryu)[0].manual);
            CHECK((*ryu)[1].label == "RW 2" && (*ryu)[1].n == 2);
            CHECK((*ryu)[2].label == "RW 3" && (*ryu)[2].path == "..\\color_mods\\Ryu\\packA\\esf001_001_cmd_001.user.2");
            CHECK((*ryu)[3].label == "MC 1" && (*ryu)[3].manual && (*ryu)[3].path == new_mc);
            CHECK((*ryu)[0].path.rfind("..\\color_mods\\.cache\\", 0) == 0);
            CHECK(!(*ryu)[0].okey.empty());
            CHECK((*ryu)[2].okey == new_okey);                           // the edited copy renamed by the move is found
            CHECK(fs::exists(data / (*ryu)[0].path));                    // the stored path resolves from the data folder
        }
        CHECK(cat.entries(2, 0) && cat.entries(2, 0)->size() == 1);      // Luke: esf002_001 -> costume id 0 ... (001 - 1)
        auto* ken = cat.entries(10, 0);
        CHECK(ken && ken->size() == 1);                                  // Unpacked 005 (garbage/locked give nothing)
        CHECK(cat.entries(11, 0) && cat.entries(11, 0)->size() == 1);    // loose file in color_mods itself
        CHECK(!cat.entries(3, 0));                                       // the dot folder
    }

    // 4. warm start: a second sync extracts nothing, the catalogue is identical
    g_log.clear();
    rep = color_mods::sync(root, logfn);
    CHECK(rep.extracted == 0 && rep.already == 2 && rep.failed == 2 && rep.purged == 0 && !rep.changed());

    // 5. stale cache: the archive is deleted -> its cache goes, its colours too
    fs::remove(root / "Ryu/NexusMod.zip");
    {
        Catalog warm;                                                    // before any purge: the cache of a deleted archive is not read
        warm.build(data);
        auto* r = warm.entries(1, 0);
        CHECK(r && r->size() == 2);                                      // packA + MANUAL only
    }
    rep = color_mods::sync(root, logfn);
    CHECK(rep.purged == 1 && rep.changed());
    CHECK(color_mods::valid_cache_dirs(root).size() == 1);

    const std::string luke_okey = cat.entries(2, 0)->front().okey, luke_ident = cat.entries(2, 0)->front().ident;

    // 6. a changed archive (new content/size) gets a new cache folder, the old one is purged
    make_zip(root / "Luke/Flat.zip", { { "esf002_001_cmd_001.user.2", body + body }, { "esf002_001_cmd_002.user.2", body } });
    rep = color_mods::sync(root, logfn);
    CHECK(rep.extracted == 1 && rep.purged == 1);
    cat.build(data);
    CHECK(cat.entries(2, 0) && cat.entries(2, 0)->size() == 2);
    // the archive changed: its cache folder (and path) changed, its identity and its override key did not
    CHECK(cat.entries(2, 0)->front().okey == luke_okey && cat.entries(2, 0)->front().ident == luke_ident);
    CHECK(cat.entries(2, 0)->front().path != luke_ident);
    CHECK(luke_ident.find(".cache\\Flat_zip\\esf002") != std::string::npos && luke_ident.find("60edb32d") == std::string::npos);
    CHECK(cat.find_ident(2, 0, luke_ident) == 1 && cat.find_ident(2, 0, "nope") == 0);

    // 6b. remembered selections follow the colour, not its rank (state.json keeps the identity next to the rank)
    {
        const fs::path d2 = tmp / "r2" / "reframework" / "data";
        put(color_mods::root_of(d2) / "Ken/MANUALCOLORS/esf010_001_cmd_001.user.2", body);   // MC 1
        put(color_mods::root_of(d2) / "Ken/packA/esf010_001_cmd_001.user.2", body);           // RW 1
        Catalog a;
        a.build(d2);
        const std::string mc_ident = a.entries(10, 0)->at(1).ident;
        CHECK(a.entries(10, 0)->at(1).label == "MC 1" && a.find_ident(10, 0, mc_ident) == 2);   // remembered: rank 2 + identity
        const std::string rw_ident = a.entries(10, 0)->at(0).ident;
        put(color_mods::root_of(d2) / "Ken/Mod/esf010_001_cmd_001.user.2", body);              // a colour mod is added
        Catalog b;
        b.build(d2);
        CHECK(b.entries(10, 0)->size() == 3);
        CHECK(b.entries(10, 0)->at(2).label == "MC 1");                                       // MC 1 moved from rank 2 to 3 ...
        CHECK(b.find_ident(10, 0, mc_ident) == 3 && b.find_ident(10, 0, rw_ident) == 2);        // ... and the identities find them
        CHECK(b.entries(10, 0)->at(0).label == "RW 1" && b.entries(10, 0)->at(0).ident.find("Ken\\Mod\\") != std::string::npos);   // the rank 1 of a legacy state would now be the mod
        fs::remove(color_mods::root_of(d2) / "Ken/Mod/esf010_001_cmd_001.user.2");
        fs::remove(color_mods::root_of(d2) / "Ken/packA/esf010_001_cmd_001.user.2");
        Catalog c;
        c.build(d2);
        CHECK(c.find_ident(10, 0, rw_ident) == 0 && c.find_ident(10, 0, mc_ident) == 1);        // a colour whose file is gone: 0 = default
    }

    // 6c. a remembered colour of an archive that is updated: rank 0 at boot but the identity is kept, the colour comes back
    {
        using screens::colour::json;
        const fs::path d3 = tmp / "r3" / "reframework" / "data";
        const fs::path r3 = color_mods::root_of(d3);
        put(r3 / "Ryu/packA/esf001_001_cmd_001.user.2", body);                                    // a pack, so the catalogue is not empty
        make_zip(r3 / "Luke/Upd.zip", { { "esf002_001_cmd_001.user.2", body }, { "esf002_001_cmd_002.user.2", body } });
        color_mods::sync(r3, logfn);
        Catalog a;
        a.build(d3);
        const std::string ident = a.entries(2, 0)->at(1).ident;
        json sel = { { "rw", 2 }, { "path", ident }, { "fighter", 2 }, { "costume", 0 } };         // remembered: colour 2 of the archive
        CHECK(!screens::colour::resolve_selection(sel, a));                                       // consistent: nothing to change
        // the archive is updated (date changes -> new cache hash): at boot its old cache is not read
        fs::last_write_time(r3 / "Luke/Upd.zip", fs::last_write_time(r3 / "Luke/Upd.zip") + std::chrono::hours(1));
        Catalog boot;
        boot.build(d3);
        CHECK(!boot.entries(2, 0));
        std::string note;
        CHECK(screens::colour::resolve_selection(sel, boot, &note));
        CHECK(sel["rw"] == 0 && sel.value("path_pending", json::object()).value("path", "") == ident && !sel.contains("path"));
        CHECK(note.find("kept for later") != std::string::npos);
        CHECK(!screens::colour::resolve_selection(sel, boot));                                    // a second look changes nothing
        CHECK(sel["path_pending"]["path"] == ident);
        // a legacy-style save in between (fighter / costume rewritten) must not lose it either
        sel["fighter"] = 2;
        // the worker extracts the new version, the catalogue is rebuilt: the colour is back at its rank
        color_mods::sync(r3, logfn);
        Catalog after;
        after.build(d3);
        CHECK(screens::colour::resolve_selection(sel, after, &note));
        CHECK(sel["rw"] == 2 && sel["path"] == ident && !sel.contains("path_pending"));
        // the player picks another colour while the first is parked -> parked identity dropped
        json sel2 = { { "rw", 0 }, { "fighter", 2 }, { "costume", 0 },
                      { "path_pending", { { "path", ident }, { "fighter", 2 }, { "costume", 0 } } } };
        sel2["rw"] = 1; sel2["path"] = after.entries(2, 0)->at(0).ident;
        CHECK(screens::colour::resolve_selection(sel2, after) && !sel2.contains("path_pending") && sel2["rw"] == 1);
        // another fighter: dropped too
        json sel3 = { { "rw", 0 }, { "fighter", 1 }, { "costume", 0 }, { "path_pending", { { "path", ident }, { "fighter", 2 }, { "costume", 0 } } } };
        CHECK(screens::colour::resolve_selection(sel3, after) && !sel3.contains("path_pending"));
        // a colour that never comes back stays parked (no loss, no harm)
        json sel4 = { { "rw", 0 }, { "fighter", 2 }, { "costume", 0 }, { "path_pending", { { "path", "..\\color_mods\\gone.user.2" }, { "fighter", 2 }, { "costume", 0 } } } };
        CHECK(!screens::colour::resolve_selection(sel4, after) && sel4.contains("path_pending"));
        // legacy state (rank only): the rank is kept, the identity is added
        json legacy = { { "rw", 1 }, { "fighter", 2 }, { "costume", 0 } };
        CHECK(screens::colour::resolve_selection(legacy, after) && legacy["rw"] == 1 && legacy["path"] == after.entries(2, 0)->at(0).ident);
        // a selection of "no colour" stays so
        json none = { { "rw", 0 }, { "fighter", 2 }, { "costume", 0 } };
        CHECK(!screens::colour::resolve_selection(none, after));
    }
    // 7. layout and README
    color_mods::ensure_layout(root);
    CHECK(fs::is_directory(root / "A.K.I") && fs::is_directory(root / "Chun-Li") && fs::is_directory(root / "Arjun"));
    {
        std::ifstream f(root / "README.txt", std::ios::binary);
        std::string shipped((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string r; for (char c : shipped) if (c != '\r') r += c;
        std::ifstream g(fs::path("..") / "data" / "color_mods_README.txt", std::ios::binary);
        std::string packed((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
        std::string p; for (char c : packed) if (c != '\r') p += c;
        CHECK(!r.empty() && r == p);                                     // the DLL's text = the file the packages ship
    }

    fs::remove_all(tmp, ec);
    std::printf(g_fail ? "FAILED (%d)\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
