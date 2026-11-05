// Unit test of plugin/src/replay_logic.hpp: the Replay Outfits tab's row cycling, colour cycling, file round trip.
// Build and run: build_replay_logic_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/replay_logic.hpp"

#include <cstdio>

using namespace slots::replay;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static Fighter ryu() {
    Fighter f;
    f.fid = 1;
    f.name = "Ryu";
    f.outfits = {
        { 9, "abc123", "zeta Ryu", { 0, 1, 2 } },
        { 1, "", "Outfit 2", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
        { 0, "", "Outfit 1", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 200, 201 } },
        { 5, "def456", "Aloha Ryu", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
        { 7, "nocol", "No colours", {} },
    };
    sort_outfits(f.outfits);
    return f;
}

int main() {
    const Fighter f = ryu();
    check(f.outfits.size() == 4, "an outfit without colours is dropped");
    check(f.outfits[0].costume == 0 && f.outfits[1].costume == 1, "game outfits first, by costume number");
    check(f.outfits[2].name == "Aloha Ryu" && f.outfits[3].name == "zeta Ryu", "then extra outfits by name, case-insensitive");

    // outfit cycling
    Choice c;
    check(row_text(f, c) == "Default", "no entry: Default");
    c = cycle_outfit(f, c, +1);
    check(c.forced && c.costume == 0 && c.mod_id.empty() && c.color == 0, "Right from Default: Outfit 1, colour 0");
    check(row_text(f, c) == "Outfit 1 - Color 1", "row text of Outfit 1 colour 0");
    c.color = 200;
    check(row_text(f, c) == "Outfit 1 - Color 11", "special colour 200 shows as Color 11");
    c = cycle_outfit(f, c, +1);
    check(c.costume == 1 && c.color == 0, "Outfit 2 has no colour 200: its first colour");
    c.color = 5;
    c = cycle_outfit(f, c, +1);
    check(c.costume == 5 && c.mod_id == "def456" && c.color == 5, "Aloha Ryu keeps colour 5");
    c = cycle_outfit(f, c, +1);
    check(c.costume == 9 && c.mod_id == "abc123" && c.color == 0, "zeta Ryu has no colour 5: colour 0");
    c = cycle_outfit(f, c, +1);
    check(!c.forced && row_text(f, c) == "Default", "Right from the last outfit: back to Default");
    c = cycle_outfit(f, c, -1);
    check(c.forced && c.costume == 9 && c.mod_id == "abc123", "Left from Default: the last outfit");
    c = cycle_outfit(f, c, -1);
    check(c.costume == 5 && c.mod_id == "def456", "Left again: the previous outfit");
    Fighter empty;
    empty.fid = 99;
    check(!cycle_outfit(empty, Choice{}, +1).forced, "a fighter without outfits stays Default");

    // colour cycling
    Choice d;
    check(cycle_color(f, d, +1) == d, "colour key on Default: nothing");
    Choice z = cycle_outfit(f, Choice{}, -1);          // zeta Ryu, colours 0 1 2
    z = cycle_color(f, z, +1);
    check(z.color == 1, "colour key: next colour");
    z = cycle_color(f, z, +1);
    z = cycle_color(f, z, +1);
    check(z.color == 0, "colour key wraps to the first colour");
    z = cycle_color(f, z, -1);
    check(z.color == 2, "colour key backwards wraps to the last colour");
    Choice o1 = cycle_outfit(f, Choice{}, +1);
    o1.color = 9;
    o1 = cycle_color(f, o1, +1);
    check(o1.color == 200, "Outfit 1: after colour 9 comes the special colour 200");
    Choice gone;
    gone.forced = true; gone.costume = 42; gone.mod_id = "removed";
    check(row_text(f, gone) == "Default" && outfit_index(f, gone) == -1, "an outfit no longer listed shows Default");
    check(cycle_outfit(f, gone, +1).costume == 0, "Right from a vanished outfit: the first outfit");

    // file round trip
    Choices cs;
    cs[1] = cycle_outfit(f, Choice{}, +1);
    Choice m = cycle_outfit(f, cycle_outfit(f, cycle_outfit(f, Choice{}, +1), +1), +1);   // Aloha Ryu
    m.color = 7;
    cs[30] = m;
    cs[2] = Choice{};                                   // Default: never written
    const std::string text = to_json(cs);
    Choices back;
    check(from_json(text, back), "written file parses");
    check(back.size() == 2 && back.count(1) && back.count(30) && !back.count(2), "two forced fighters, Default left out");
    check(back[1] == cs[1] && back[30] == cs[30], "values survive the round trip");
    check(back[1].mod_id.empty() && back[30].mod_id == "def456" && back[30].color == 7, "mod_id only for the extra outfit");
    check(text.find("\"mod_id\"") == text.rfind("\"mod_id\""), "one mod_id key in the file");
    Choices none;
    Choices back2;
    check(from_json(to_json(none), back2) && back2.empty(), "no forced fighter: an empty fighters object");
    check(!from_json("{\"version\":2,\"fighters\":{}}", back2), "another version is refused");
    check(!from_json("not json", back2), "garbage is refused");
    check(from_json("{\"version\":1,\"fighters\":{\"7\":{\"mode\":\"auto\",\"costume\":1},\"x\":{\"mode\":\"forced\",\"costume\":1},"
                    "\"8\":{\"mode\":\"forced\",\"costume\":2,\"color\":3}}}", back2) &&
          back2.size() == 1 && back2[8].costume == 2 && back2[8].color == 3,
          "entries not forced, or with a bad id, are skipped");

    // cursor and window
    check(move_cursor(0, -1, 32) == 31 && move_cursor(31, 1, 32) == 0 && move_cursor(4, 1, 32) == 5, "Up / Down wrap");
    check(window_top(0, 0, 32, 14) == 0 && window_top(0, 14, 32, 14) == 1 && window_top(10, 3, 32, 14) == 3, "window follows the cursor");
    check(window_top(0, 31, 32, 14) == 18 && window_top(5, 2, 10, 14) == 0, "window clamped at the end, none when all fit");

    // fighter order and names
    std::vector<Fighter> fs(3);
    fs[0].fid = 99; fs[1].fid = 21; fs[2].fid = 1;
    order_fighters(fs);
    check(fs[0].fid == 1 && fs[1].fid == 21 && fs[2].fid == 99, "grid order, unknown ids last");
    check(std::string(fighter_name(30)) == "C. Viper" && fighter_name(23) == nullptr, "fighter names");
    check(game_outfit_name(0) == "Outfit 1" && game_outfit_name(3) == "Outfit 4" && game_outfit_name(4) == "DriveTech",
          "game outfit names");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
