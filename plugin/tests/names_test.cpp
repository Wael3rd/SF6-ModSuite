// Unit test of common/outfit_display.hpp: the text the game shows for an extra outfit (the loader's costume name message). Made-up
// names only. Build and run: build_names_test.bat (prints "all ok" and exits 0 when every check passes).
#include "../../common/outfit_display.hpp"
#include "../src/scroll_logic.hpp"

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    check(outfit_display_name("Aloha Ryu") == "Aloha Ryu", "a plain label stays");
    check(outfit_display_name("").empty(), "no label: nothing, the outfit keeps its Outfit VII");
    check(outfit_display_name("   ").empty(), "only spaces: nothing");
    check(outfit_display_name("<<>>").empty(), "only what is taken out: nothing");
    check(outfit_display_name("  Aloha Ryu  ") == "Aloha Ryu", "no space at its ends");
    check(outfit_display_name("<ICON KeyR>Bob") == "ICON KeyRBob", "no message tag can be formed");
    check(outfit_display_name("a<b>c") == "abc", "< and > gone");
    check(outfit_display_name("a\nb\tc\x01" "d\x7F" "e") == "abcde", "control characters gone");
    check(outfit_display_name(std::string("a\0b", 3)) == "ab", "a NUL gone");
    check(outfit_display_name("Jos\xC3\xA9") == "Jos\xC3\xA9", "accents stay");
    check(outfit_display_name("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC", "Japanese stays");
    check(outfit_display_name("\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A") == "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A", "Arabic stays");
    check(outfit_display_name("\xF0\x9F\x98\x80" "x") == "\xF0\x9F\x98\x80" "x", "an emoji stays");
    check(outfit_display_name("Mai (C1) - Regular & co. 100%") == "Mai (C1) - Regular & co. 100%", "punctuation stays");
    check(outfit_display_name("a\xE2\x80\xAE" "b") == "ab", "a bidirectional override gone");
    check(outfit_display_name("a\xE2\x80\xA8" "b") == "ab", "a line separator gone");
    check(outfit_display_name("a\xC2\x85" "b") == "ab", "a C1 control gone");
    // what add_msg_entries refuses must never reach it: it would refuse the whole file
    check(outfit_display_name("a\xFF" "b\xC3") == "ab", "broken UTF-8 gone (a Latin-1 file name)");
    check(outfit_display_name("caf\xE9") == "caf", "a Latin-1 accent gone, the rest stays");
    check(outfit_display_name("\xC0\xBC" "x") == "x", "an overlong < gone");
    check(outfit_display_name("\xE0\x80\xBC" "x") == "x", "another overlong < gone");
    check(outfit_display_name("\xED\xA0\x80" "x") == "x", "a surrogate gone");
    check(outfit_display_name("\xF4\x90\x80\x80" "x") == "x", "beyond U+10FFFF gone");
    // bounded, on a UTF-8 boundary
    check(outfit_display_name(std::string(kOutfitDisplayMaxBytes, 'x')).size() == kOutfitDisplayMaxBytes, "at the bound: whole");
    const std::string longer = outfit_display_name(std::string(300, 'x'));
    check(longer.size() == kOutfitDisplayMaxBytes && longer.compare(longer.size() - 3, 3, "...") == 0, "beyond it: cut with ...");
    std::string wide;
    for (int i = 0; i < 100; ++i) wide += "\xE6\x97\xA5";
    const std::string cut = outfit_display_name(wide);
    check(cut.size() <= kOutfitDisplayMaxBytes && (cut.size() - 3) % 3 == 0 && outfit_display_name(cut) == cut, "beyond it, never inside a UTF-8 sequence");
    check(outfit_display_name(outfit_display_name("  <a> b\x01 ")) == outfit_display_name("  <a> b\x01 "), "twice is once");

    // ---- the scrolling ----
    using namespace slots::scroll;
    check(offset_at(0, 5) == 0 && offset_at(-10, 5) == 0, "a name that fits doesn't move");
    check(offset_at(120, 0) == 0 && offset_at(120, 1.4) == 0, "it rests at its start");
    const double slide = 120 / kUnitsPerSecond;     // 2 s
    check(offset_at(120, kRestSeconds + slide / 2) == 60, "it slides, half way");
    check(offset_at(120, kRestSeconds + slide - 0.01) >= 119, "it reaches the end");
    check(offset_at(120, kRestSeconds + slide + 0.2) == 120 && offset_at(120, kRestSeconds + slide + 1.4) == 120, "it rests at its end");
    check(offset_at(120, 2 * kRestSeconds + slide + slide / 2) == 60, "it comes back");
    const double period = 2 * kRestSeconds + 2 * slide;
    check(offset_at(120, period + 0.1) == 0 && offset_at(120, period + kRestSeconds + slide / 2) == 60, "it repeats");
    check(offset_at(120, -1) == 0, "before it starts: its start");
    bool in_range = true;
    int steps = 0, last = -1;
    for (double t = 0; t < 3 * period; t += 1.0 / 60) {
        const int o = offset_at(120, t);
        if (o < 0 || o > 120) in_range = false;
        if (o != last) ++steps;
        last = o;
    }
    check(in_range, "never outside its overflow");
    check(steps < 3 * 2 * 125, "a whole unit at a time: a write only when it changes");
    check(offset_at(120, 0, true) == 120 && offset_at(120, kRestSeconds + slide + 0.2, true) == 0, "right to left: from its other end");
    check(offset_at(0.4f, 3) == 0 || offset_at(0.4f, 3) == 1, "a sliver of overflow stays sane");
    // the window stays where it is on the screen: the clip, local to the text, moves as the text does the other way
    for (const int off : { 0, 37, 120 }) {
        const Window w = window_at(500, 200, off);
        check(w.x + w.clip_left == 500 && w.x + w.clip_right == 700, "the window stays put");
    }
    const Window w0 = window_at(500, 200, 0);
    check(w0.clip_top < 0 && w0.clip_bottom > 100, "no glyph cut at the top or the bottom");
    check(left_of(412, 5, 200) == 312 && left_of(412, 4, 200) == 412 && left_of(412, 6, 200) == 212, "the place's left edge, from a centre, left or right point");
    check(left_of(412, 9, 200) == 312, "whatever its row");
    check(!overflows(200, 200) && !overflows(200.4f, 200) && overflows(201, 200), "wider than its place");
    check(!text_is_rtl("Aloha Ryu") && !text_is_rtl("") && !text_is_rtl("123 (x)"), "Latin: left to right");
    check(text_is_rtl("\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A") && text_is_rtl("12 \xD8\xB9\xD8\xB1") && text_is_rtl("\xD7\x90\xD7\x91"), "Arabic, Hebrew: right to left");
    check(!text_is_rtl("\xE6\x97\xA5\xE6\x9C\xAC") && !text_is_rtl("Jos\xC3\xA9") && !text_is_rtl("Ryu \xD8\xB9"), "CJK, accents, Latin first: left to right");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
