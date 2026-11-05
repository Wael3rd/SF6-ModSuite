// Unit test of plugin/src/opp_summary.hpp: the list length while the summary is drawn, the code check's cache.
// Build and run: build_opp_summary_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/opp_summary.hpp"

#include <cstdio>

using namespace slots::oppsum;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    // draw_length: the summary's widgets are 6
    check(draw_length(20, 6) == 6, "Cammy, 5 outfits + 15 slots: the first 6 while it draws");
    check(draw_length(7, 6) == 6, "one outfit too many: 6");
    check(draw_length(6, 6) == -1, "as many outfits as labels: left alone");
    check(draw_length(4, 6) == -1, "fewer outfits than labels: left alone");
    check(draw_length(20, 0) == -1, "no widget read: left alone");
    check(draw_length(0, 6) == -1, "no outfit: left alone");

    // the code check's cache
    const std::string key = "exe 6A97881D 2618F000 25BB1081 size 632956832 time 1DD5AD806B56D43 methods 1455453";
    const std::string label = "app.UIFlowOtherMatchingFighterSetting.UIParam.UpdateCosColorTextList";
    CodeCheck now;
    now.index = 194895;
    now.raw = 0x422B070;
    now.code = 0x422B070;
    CodeCheck found = now;
    found.shared = 0;
    const std::string text = cache_text(key, label, found);
    CodeCheck out;
    const char* why = nullptr;
    check(cache_hit(text, key, label, now, out, &why) && out.shared == 0 && out.other == -1, "written then read: a hit, unique");

    CodeCheck shared = now;
    shared.shared = 3;
    shared.other = 77;
    check(cache_hit(cache_text(key, label, shared), key, label, now, out, &why) && out.shared == 3 && out.other == 77,
          "a shared verdict is kept with one of the sharers");
    check(!cache_hit(text, key + "x", label, now, out, &why), "another game executable: no hit");
    check(!cache_hit(text, key, label + "x", now, out, &why), "another method: no hit");
    CodeCheck moved = now;
    moved.raw += 16;
    check(!cache_hit(text, key, label, moved, out, &why), "the method's entry moved: no hit");
    moved = now;
    moved.code = -1;
    check(!cache_hit(text, key, label, moved, out, &why), "its code now another module's hook: no hit");
    moved = now;
    moved.index += 1;
    check(!cache_hit(text, key, label, moved, out, &why), "another method index: no hit");
    check(!cache_hit(text.substr(0, text.size() - 1), key, label, now, out, &why), "the last line not ended (half file): no hit");
    check(!cache_hit("", key, label, now, out, &why) && std::string(why) == "no cache file yet", "no file: no hit, said so");
    check(!cache_hit("garbage\n", key, label, now, out, &why), "another format: no hit");
    CodeCheck unknown = now;   // shared -1: the check never finished, never written as a result
    check(!cache_hit(cache_text(key, label, unknown), key, label, now, out, &why), "an unknown verdict: no hit");
    std::string crlf;
    for (char c : text) { if (c == '\n') crlf += '\r'; crlf += c; }
    check(cache_hit(crlf, key, label, now, out, &why), "CRLF line ends (file edited by hand): still a hit");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
