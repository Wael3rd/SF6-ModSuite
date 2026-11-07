// Unit test of common/outfit_display.hpp: the text the game shows for an extra outfit (the loader's costume name message). Made-up
// names only. Build and run: build_names_test.bat (prints "all ok" and exits 0 when every check passes).
#include "../../common/outfit_display.hpp"

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

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
