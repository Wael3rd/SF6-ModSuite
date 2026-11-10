// Unit test of plugin/src/ui_retexture_logic.hpp (the live texture overrides: paths as matched, the overrides' text, the loader's
// export read with a retry, the bookkeeping of the elements changed and of the holders kept, the GUIs new or newly shown, the
// references given back later).
// Build and run: build_ui_retexture_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/ui_retexture_logic.hpp"

#include <cstdio>

using namespace slots::retex;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-96s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static void test_paths() {
    std::printf("paths\n");
    check(clean_path("natives/stm/Product/GUI/data/x.tex.241101895") == "Product/GUI/data/x.tex", "natives/stm/ and the version number off, case kept");
    check(clean_path("  natives\\STM\\product\\gui\\x.tex  ") == "product/gui/x.tex", "backslashes, spaces, NATIVES in capitals");
    check(clean_path("natives/x64/a/b.tex.10") == "a/b.tex", "another platform folder");
    check(clean_path("/product/gui/x.tex") == "product/gui/x.tex", "a leading slash");
    check(clean_path("product/gui/x.tex") == "product/gui/x.tex", "a path already clean");
    check(clean_path("gui/file.123") == "gui/file.123", "a single extension of digits is kept (not a version)");
    check(clean_path("gui/v1.2/file") == "gui/v1.2/file", "a dot in a folder is not a version");
    check(clean_path("natives/") == "natives/", "\"natives/\" alone is left (no platform folder)");
    check(key_of("Product/GUI/data/Character_Image/001/caracterselect/tex_Esf001_face_L_s_IAM.tex") ==
              "product/gui/data/character_image/001/caracterselect/tex_esf001_face_l_s_iam.tex", "the key: lower case");
    check(key_of("natives/stm/product/gui/x.tex.241101895") == key_of("Product\\GUI\\X.TEX"), "the loader's spelling and the game's match");
}

static void test_parse() {
    std::printf("the overrides' text\n");
    const Parsed e = parse("");
    check(e.map.empty() && e.paths.empty() && e.lines == 0 && e.bad == 0, "empty text: no override");
    const std::string text =
        "# a note\r\n"
        "natives/stm/product/gui/A.tex.241101895\tnatives/stm/_uislots/u1/n1/product/gui/A.tex.241101895\r\n"
        "\n"
        "product/gui/B.tex\t_uislots/u1/n1/product/gui/B.tex\n"
        "no tab here\n"
        "product/gui/C.tex\t\n"
        "\t_uislots/x.tex\n"
        "product/gui/D.tex\tProduct/GUI/D.tex\n"
        "product/gui/B.tex\t_uislots/u1/n2/product/gui/B.tex\n"
        "PRODUCT/GUI/E.tex\t_uislots/u1/n1/product/gui/A.tex";       // no newline at the end
    const Parsed p = parse(text);
    check(p.lines == 8, "8 lines counted (blank and '#' lines are not)");
    check(p.bad == 3, "3 not read: no tab, an empty side each");
    check(p.same == 1, "1 file shown instead of itself, skipped");
    check(p.twice == 1, "1 real file listed twice");
    check(p.map.size() == 3, "3 overrides");
    check(p.map.count("product/gui/a.tex") && p.map.at("product/gui/a.tex") == "_uislots/u1/n1/product/gui/a.tex", "A: matched by key");
    check(p.map.at("product/gui/b.tex") == "_uislots/u1/n2/product/gui/b.tex", "B: the last line counts");
    check(p.map.at("product/gui/e.tex") == "_uislots/u1/n1/product/gui/a.tex", "E: the last line without a newline, two reals one target");
    check(p.paths.size() == 2, "2 targets (B's first one, no longer named, dropped)");
    check(p.paths.count("_uislots/u1/n1/product/gui/a.tex") && p.paths.at("_uislots/u1/n1/product/gui/a.tex") == "_uislots/u1/n1/product/gui/A.tex",
          "a target's path keeps its case, cleaned");
}

static void test_read_export() {
    std::printf("the loader's export\n");
    const std::string big(100, 'x');
    int calls = 0;
    auto loader = [&](const std::string& text) {
        return [&calls, text](char* buf, int cap) {
            ++calls;
            const int n = static_cast<int>(text.size());
            const int w = n < cap - 1 ? n : cap - 1;
            std::memcpy(buf, text.data(), static_cast<size_t>(w));
            buf[w] = 0;
            return n;
        };
    };
    std::string out;
    calls = 0;
    check(read_export(loader(big), out, 16) == 100 && out == big && calls == 2, "too small: the length needed, asked once more with the room");
    calls = 0;
    check(read_export(loader(std::string(15, 'y')), out, 16) == 15 && out.size() == 15 && calls == 1, "15 bytes in 16: one call");
    calls = 0;
    check(read_export(loader(std::string(16, 'y')), out, 16) == 16 && out.size() == 16 && calls == 2, "16 bytes in 16 (no room for its 0): two calls");
    check(read_export(loader(""), out, 16) == 0 && out.empty(), "nothing overridden: \"\"");
    check(read_export([](char*, int) { return -1; }, out) == ReadNotRunning, "a negative length: the UI slots are not running");
    check(read_export([](char*, int) { return kFault; }, out) == ReadFaulted, "the call faulted");
    int grow = 50;
    check(read_export([&](char* b, int cap) { b[0] = 0; (void)cap; grow *= 2; return grow; }, out, 16) == ReadTooLong, "grew between the two calls");
    check(read_export([](char* b, int) { std::memcpy(b, "ab\0\0", 4); return 4; }, out, 16) == 2 && out == "ab", "a length past the text's 0: cut at the 0");
}

static void test_book() {
    std::printf("bookkeeping\n");
    Book<int, int> b;
    b.set_map({ { "r1", "t1" }, { "r2", "t1" }, { "r3", "t3" } });
    check(b.needed("t1") && b.needed("t3") && !b.needed("t9"), "targets named by an override are needed");
    const std::string* to = nullptr;
    check(b.fresh("r1", &to) == Step::Apply && to && *to == "t1", "an element showing r1: given t1");
    check(b.fresh("zz", &to) == Step::None, "an element showing a file not overridden: left");
    check(b.real_key("r3") && *b.real_key("r3") == "r3" && !b.real_key("t1"), "the map's real keys");
    b.add(100, "r1", "t1", 7, 70, 700, 10);
    b.add(101, "r2", "t1", 8, 70, 700, 10);
    b.add(102, "r3", "t3", 9, 30, 300, 10);
    check(b.users["t1"] == 2 && b.users["t3"] == 1, "two elements show t1, one t3");
    check(b.recorded(b.recs[100], &to) == Step::Keep, "unchanged override: kept");
    b.set_map({ { "r1", "t2" }, { "r2", "t1" } });
    check(b.recorded(b.recs[100], &to) == Step::Swap && *to == "t2", "r1 now shows t2: swapped");
    check(b.recorded(b.recs[102], &to) == Step::Restore, "r3 no longer overridden: the game's own back");
    check(!b.map_targets.count("t3") && b.needed("t3"), "t3 named by no override, still needed: an element shows it");
    b.retarget(100, "t2", 20, 200, 20);
    check(b.users["t1"] == 1 && b.users["t2"] == 1 && b.recs[100].ours == 20 && b.recs[100].orig == 7, "retarget: users moved, the game's own kept");
    const auto gone = b.take(102);
    check(gone.orig == 9 && gone.real == "r3" && !b.needed("t3") && !b.users.count("t3"), "put back: its orig returned, t3 no longer needed");
    check(b.take(555).orig == 0, "no record: nothing taken");
    b.add(103, "", "t2", 0, 20, 200, 30);
    check(b.recorded(b.recs[103], &to) == Step::Restore && b.recs[103].orig == 0, "a copy adopted without the game's own: never put back (orig 0)");
    b.add(103, "r2", "t1", 5, 70, 700, 31);
    check(b.users["t2"] == 1 && b.users["t1"] == 2, "a second add of one element replaces its record");
    b.set_map({});
    check(b.needed("t1") && b.needed("t2"), "no override at all: the targets shown stay needed");
    b.take(100);
    b.take(101);
    b.take(103);
    check(!b.needed("t1") && !b.needed("t2") && b.recs.empty() && b.users.empty(), "every element put back: nothing needed");
}

static void test_fights() {
    std::printf("an element the game keeps putting back\n");
    Book<int, int> b;
    b.set_map({ { "r", "t" } });
    check(!b.put_back(1, true, 100, 50, 300, 3) && !b.put_back(1, true, 400, 350, 300, 3), "twice: not yet");
    check(b.put_back(1, true, 700, 650, 300, 3) && b.is_contested(1), "a third time within the window: contested");
    check(!b.put_back(2, true, 100, 50, 300, 3) && !b.put_back(2, false, 200, 150, 300, 3) && !b.put_back(2, true, 300, 250, 300, 3) &&
              !b.put_back(2, true, 400, 350, 300, 3), "another file in between: the run starts again");
    check(!b.put_back(3, true, 100, 50, 300, 3) && !b.put_back(3, true, 1000, 600, 300, 3) && !b.is_contested(3), "past the window: the run starts again");
    b.set_map({ { "r", "t2" } });
    check(!b.is_contested(1) && b.fights.empty(), "the overrides changed: every element gets its chance again");
}

static void test_guis() {
    std::printf("the scene's GUIs between two looks\n");
    std::unordered_map<uintptr_t, GuiSeen> seen;
    check(diff_guis(seen, { { 1, 11, true }, { 2, 22, false } }, 1, true).empty() && seen.size() == 2, "the first look: recorded only");
    auto f = diff_guis(seen, { { 1, 11, true }, { 2, 22, true }, { 3, 33, true }, { 4, 44, false } }, 2, false);
    check(f.size() == 2 && f[0] == 2 && f[1] == 3, "newly shown and new shown ones: fresh (a new hidden one is not)");
    f = diff_guis(seen, { { 1, 11, true }, { 2, 22, true }, { 3, 99, true }, { 4, 44, true } }, 3, false);
    check(f.size() == 2 && f[0] == 3 && f[1] == 4, "another View at a known address, a hidden one shown: fresh");
    f = diff_guis(seen, { { 1, 11, true } }, 4, false);
    check(f.empty() && seen.size() == 1 && seen.count(1), "the GUIs no longer listed are forgotten");
    f = diff_guis(seen, { { 1, 11, true }, { 2, 22, true } }, 5, false);
    check(f.size() == 1 && f[0] == 2, "a GUI back at a forgotten address: fresh");
}

static void test_later() {
    std::printf("references given back later\n");
    Later<int> l;
    l.add(1, 100);
    l.add(2, 200);
    l.add(0, 50);                                                    // nothing to give back
    std::vector<int> out;
    check(l.run(150, [&](int h) { out.push_back(h); }) == 1 && out.size() == 1 && out[0] == 1, "only those due");
    check(!l.empty() && l.run(200, [&](int h) { out.push_back(h); }) == 1 && l.empty(), "then the next one, at its frame");
    Later<int> w;
    w.add(3, 5u);                                                    // due just after the frame counter wrapped
    check(w.run(0xFFFFFFF0u, [](int) {}) == 0 && w.run(6u, [](int) {}) == 1, "the frame counter's wrap");
}

int main() {
    test_paths();
    test_parse();
    test_read_export();
    test_book();
    test_fights();
    test_guis();
    test_later();
    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
