// engine/opponent_ticks.hpp: the RW / MC colours ticked on Opponent Outfit Settings, and what a pick on colour 1 means. Build: build_opponent_ticks_test.bat
#include <cstdio>
#include <map>

#include "../src/engine/opponent_ticks.hpp"

using namespace engine::opponent;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-90s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    Ticks t;
    check(!t.any(2) && t.count(2, 0) == 0 && t.of(2, 0).empty(), "nothing ticked");
    check(t.toggle(2, 0, "a"), "a tick");
    check(t.has(2, 0, "a") && !t.has(2, 0, "b") && !t.has(2, 1, "a") && !t.has(3, 0, "a"), "only that cell, that outfit, that fighter");
    check(t.toggle(2, 0, "b") && t.count(2, 0) == 2, "another: each on its own");
    check(!t.toggle(2, 0, "a") && t.has(2, 0, "b") && t.count(2, 0) == 1, "untick one: the other stays");
    t.toggle(2, 5, "c");
    check(t.costumes(2).size() == 2 && t.any(2) && !t.any(3), "the outfits with a tick");
    t.set(-1, 0, "x", true); t.set(1, -1, "x", true); t.set(1, 0, "", true);
    check(!t.any(1), "a bad fighter, outfit or id: refused");
    t.set(2, 5, "nothing", false);
    check(t.count(2, 5) == 1, "unticking what isn't ticked changes nothing");

    const std::string path = "..\\color_mods\\Luke\\a b\\esf002_001_cmd_006.user.2";
    t.set(2, 0, path, true);
    Ticks r;
    check(r.from_text(t.to_text()), "the file reads back");
    check(r.has(2, 0, path) && r.has(2, 0, "b") && r.has(2, 5, "c") && r.count(2, 0) == 2 && r.count(2, 5) == 1, "survives a re-entry, spaces and backslashes in a path too");
    check(r.to_text() == t.to_text(), "written the same again");
    Ticks crlf;
    check(crlf.from_text(std::string(kFileHeader) + "\r\n2\t0\tx\r\n") && crlf.has(2, 0, "x"), "a file with CRLF");
    Ticks bad;
    bad.set(1, 1, "kept", true);
    check(!bad.from_text("something else\n2\t0\tx\n") && !bad.any(2) && !bad.any(1), "another format: refused, nothing kept");
    Ticks odd;
    check(odd.from_text(std::string(kFileHeader) + "\nno tabs\n2\t0\n2\tx\ty\n1x\t0\ty\n2\t0\tok\n") && odd.has(2, 0, "ok") && odd.count(2, 0) == 1 && !odd.any(1),
          "lines that can't be read are left out");
    Ticks empty;
    check(empty.from_text("") && !empty.any(2), "an empty file: nothing ticked");
    Ticks none;
    check(none.to_text() == std::string(kFileHeader) + "\n", "nothing ticked: only the header");

    using V = std::vector<int>;
    check(classify(V{ 0 }, V{ 0 }, true, 1) == Change::kNone && classify(V{ 3, 0 }, V{ 0, 3 }, false, 0) == Change::kNone, "no change, in any order");
    check(classify(V{}, V{ 1 }, true, 1) == Change::kEntry && classify(V{ 1 }, V{}, true, 1) == Change::kEntry, "a tick or an untick on an entry's cell");
    check(classify(V{ 0, 1 }, V{ 0 }, true, 1) == Change::kEntry, "with another colour ticked too");
    check(classify(V{}, V{ 1 }, false, 1) == Change::kGame && classify(V{ 1 }, V{}, false, 1) == Change::kGame, "the cursor on colour 02 itself: the game's");
    check(classify(V{}, V{ 3 }, false, 3) == Change::kGame && classify(V{ 3 }, V{}, false, 3) == Change::kGame, "another colour of the game under the cursor");
    check(classify(V{}, V{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }, true, 1) == Change::kAllOn && classify(V{ 15 }, V{ 15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }, false, 2) == Change::kAllOn, "the all key turns them on");
    check(classify(V{ 0, 1, 2 }, V{}, true, 1) == Change::kAllOff && classify(V{ 0, 1, 15 }, V{ 15 }, false, 5) == Change::kAllOff, "the all key turns them off, EX / DX stay");
    check(classify(V{ 3 }, V{}, true, 1) == Change::kAllOff && classify(V{ 3 }, V{}, false, 5) == Change::kAllOff, "the all key with one tick that isn't under the cursor");
    check(classify(V{}, V{ 12, 13, 14, 15 }, false, 2) == Change::kGame && classify(V{ 12, 13 }, V{}, true, 1) == Change::kGame, "the EX / DX key isn't ours");

    using S = std::vector<std::string>;
    const std::nullopt_t no = std::nullopt;
    check(look_after_pick(false, { "rw6" }, no, no, 7) == "rw6", "RW6 ticked, colour 02 not: RW6, never 02");
    check(look_after_pick(true, {}, no, no, 7).empty(), "colour 02 ticked alone: 02, as today");
    check(look_after_pick(false, {}, no, no, 7).empty(), "nothing left to wear: plain 02, said by the caller");
    std::map<std::string, int> seen;
    for (uint32_t i = 0; i < 300; ++i) ++seen[look_after_pick(true, S{ "a", "b" }, no, no, i)];
    check(seen[""] == 100 && seen["a"] == 100 && seen["b"] == 100, "02 and two entries: each as likely");
    check(look_after_pick(false, S{ "a", "b" }, std::string("a"), no, 0) == "b" && look_after_pick(false, S{ "a", "b" }, std::string("a"), no, 1) == "b", "a mirror: not the same look as the player");
    check(look_after_pick(false, S{ "a" }, std::string("a"), no, 0) == "a", "a mirror, only that one ticked: that one, as the game keeps a single colour");
    check(look_after_pick(true, S{ "a" }, std::string(""), no, 0) == "a", "a mirror, the player on plain 02: the entry");
    check(look_after_pick(false, S{ "a", "b" }, no, std::string("a"), 0) == "b", "not twice in a row");
    check(look_after_pick(false, S{ "a" }, no, std::string("a"), 0) == "a", "a single one comes back");
    check(look_after_pick(false, S{ "a", "b" }, std::string("a"), std::string("b"), 0) == "b", "the mirror's rule first, and never nothing");

    std::printf(g_fail ? "%d FAILED\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
