// engine/random_value.hpp: the Color row's Random value (its rank, its index in a list). Build: build_random_value_test.bat
#include <cstdio>

#include "../src/engine/random_value.hpp"

using namespace engine::random_value;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-90s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    // a list of 10 game colours (0-9), 3 entries (10-12), the Random value at 13
    check(rank_at(0, 10, 3, 13) == 0 && rank_at(9, 10, 3, 13) == 0, "a game colour: no rank");
    check(rank_at(10, 10, 3, 13) == 1 && rank_at(12, 10, 3, 13) == 3, "the entries: 1..3");
    check(rank_at(13, 10, 3, 13) == kK, "the Random value: its own rank");
    check(rank_at(14, 10, 3, 13) == 0, "past the list: nothing");
    check(index_of(1, 10, 3, 13) == 10 && index_of(3, 10, 3, 13) == 12 && index_of(kK, 10, 3, 13) == 13, "the indexes back");
    check(index_of(4, 10, 3, 13) == -1 && index_of(0, 10, 3, 13) == -1, "a rank the list has not: -1");
    // an outfit without entries: the Random value alone, right after the game's colours
    check(rank_at(8, 8, 0, 8) == kK && rank_at(7, 8, 0, 8) == 0 && index_of(kK, 8, 0, 8) == 8, "no entries: the Random value alone");
    // the Random value absent (no Slots Loader, or Opponent Outfit Settings)
    check(rank_at(13, 10, 3, -1) == 0 && index_of(kK, 10, 3, -1) == -1, "no Random value: never its rank, no index");
    check(rank_at(5, -1, 0, -1) == 0, "nothing of ours in the list");
    // a new pack moves the entries, never the Random value's rank
    check(rank_at(15, 10, 5, 15) == kK && kK > 100, "five entries now: still kK (a rank no list of entries reaches)");
    std::printf(g_fail ? "%d FAILED\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
