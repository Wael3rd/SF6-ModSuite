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

    // an archive's name: shown without its extension; a row of a pak built before still matches
    check(outfit_display_name("Majin Vegeta.rar") == "Majin Vegeta", ".rar gone");
    check(outfit_display_name("EliMummySim 3645 2 2026-08-07T04-29Z toYuPJU0O.zip") == "EliMummySim 3645 2 2026-08-07T04-29Z toYuPJU0O", ".zip gone");
    check(outfit_display_name("Pack.7Z") == "Pack" && outfit_display_name("Pack .RAR") == "Pack", "any case, no space left at its end");
    check(outfit_display_name(".rar") == ".rar" && outfit_display_name("x.rar.txt") == "x.rar.txt", "nothing left, or not at the end: kept");
    check(outfit_display_name("C. Viper Battlesuit C2.1") == "C. Viper Battlesuit C2.1" && outfit_display_name("Mai v1.5") == "Mai v1.5", "a dot in a name stays");
    check(outfit_display_name("Majin Vegeta.rar.zip") == "Majin Vegeta.rar", "one extension only");
    check(outfit_display_matches("Majin Vegeta.rar", "Majin Vegeta") && outfit_display_matches("Majin Vegeta.rar", "Majin Vegeta.rar"), "both rows match");
    check(!outfit_display_matches("Majin Vegeta.rar", "Majin") && !outfit_display_matches("", "") && !outfit_display_matches("<>", ""), "no other");
    check(outfit_display_matches("Aloha Ryu", "Aloha Ryu") && !outfit_display_matches("Aloha Ryu", "Aloha Ryu.rar"), "a plain label: itself only");

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
    bool goes_on = true;
    for (int k = 1; k < 120; ++k) {
        if (offset_at(120, time_at(120, k)) != k) goes_on = false;
        if (offset_at(120, time_at(120, k, true), true) != k) goes_on = false;
    }
    check(goes_on, "a new width: the slide goes on from where it is, both ways");
    check(time_at(120, 0) == 0 && time_at(120, 0, true) == kRestSeconds + 120 / kUnitsPerSecond, "from its start or its end");
    check(offset_at(100, time_at(100, 150)) == 100, "the new end is nearer than where it was: at that end");
    check(time_at(0, 50) == 0, "no overflow any more");
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
    // a Costume row's place, between its arrows (row at 348 and 700, arrows 12 wide, the text's point at 524: as read on Opponent Outfit Settings)
    const Span sp = span_between(524 + 100, 354 + 100, 694 + 100, 1.0f, 8);
    check(sp.ok && sp.l == -162 && sp.r == 162, "the place: arrow to arrow, a margin inside each, from the text's point");
    const Span spu = span_between(1000, 900, 1100, 2.0f, 8);
    check(spu.ok && spu.l == -192 && spu.r == 192, "in the units of the text's parent");
    check(!span_between(500, 490, 510, 1, 8).ok, "arrows that leave no room: not valid");
    check(fits_span(300, sp, 1) && fits_span(324, sp, 1) && !fits_span(326, sp, 1), "centred: fits the place or not");
    const Span off_centre = span_between(500, 340, 700, 1, 0);        // the text's point 40 left of the place's middle
    check(fits_span(320, off_centre, 1) && !fits_span(360, off_centre, 1), "centred, off the place's middle: what's left of its point counts");
    check(fits_span(200, off_centre, 0) && !fits_span(201, off_centre, 0), "by its left point: what's right of it counts");
    check(fits_span(160, off_centre, 2) && !fits_span(161, off_centre, 2), "by its right point: what's left of it counts");
    // a line in two parts
    const SplitPlace split = split_place(320, 260, 259);
    check(split.ok && split.offset == 60 && split.window == 199, "the value starts where it starts in the whole line; its window, from there to the place's end");
    check(!split_place(320, 320, 259).ok && !split_place(300, 310, 259).ok, "no fixed part, or a width not to be trusted: not split");
    check(!split_place(320, 140, 259).ok && split_place(320, 141, 259).ok, "a window too small to read: not split");
    check(!text_is_rtl("Aloha Ryu") && !text_is_rtl("") && !text_is_rtl("123 (x)"), "Latin: left to right");
    check(text_is_rtl("\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A") && text_is_rtl("12 \xD8\xB9\xD8\xB1") && text_is_rtl("\xD7\x90\xD7\x91"), "Arabic, Hebrew: right to left");
    check(!text_is_rtl("\xE6\x97\xA5\xE6\x9C\xAC") && !text_is_rtl("Jos\xC3\xA9") && !text_is_rtl("Ryu \xD8\xB9"), "CJK, accents, Latin first: left to right");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
