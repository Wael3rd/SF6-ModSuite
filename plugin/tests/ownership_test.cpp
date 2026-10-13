// Unit test of plugin/src/ownership.hpp: which slots the player sees from the game's answers.
// Build and run: build_ownership_test.bat (prints "all ok" and exits 0 when every check passes).
#include "../src/ownership.hpp"

#include <cstdio>

using slots::ownership::Book;
using slots::ownership::State;
using slots::ownership::outfit_label;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    const int RYU = 1, KEN = 2;
    const int O1 = 0, O2 = 1, O3 = 2, DT = 4;

    {
        Book b;
        check(b.state(RYU, O2) == State::Unknown, "no answer yet: unknown");
        check(b.shown(RYU, O2), "no answer yet: shown (as before the check)");
        check(b.open(RYU, O2), "no answer yet: asked");
        check(b.shown(RYU, -1), "outfit not known (-1): shown");
        check(!b.open(RYU, -1), "outfit not known (-1): never asked");
    }
    {
        Book b;
        b.answer(RYU, O2, {});
        check(b.state(RYU, O2) == State::Unknown && b.shown(RYU, O2), "empty list (inventory not loaded): no answer, shown");
        b.answer(RYU, O2, {4, 5});
        check(b.state(RYU, O2) == State::Unknown, "list without Outfit 1: no answer");
    }
    {
        Book b;
        b.answer(RYU, O1, {0, 4});
        b.answer(RYU, O2, {0, 4});
        b.answer(RYU, DT, {0, 4});
        check(b.shown(RYU, O1) && b.state(RYU, O1) == State::Owned, "Outfit 1 listed: owned, shown");
        check(b.shown(RYU, DT) && !b.open(RYU, DT), "DriveTech listed: owned, not asked again");
        check(!b.shown(RYU, O2) && b.state(RYU, O2) == State::NotOwned, "Outfit 2 not listed: hidden");
        check(b.open(RYU, O2), "hidden outfit: asked again later");
        check(b.state(KEN, O2) == State::Unknown && b.shown(KEN, O2), "another fighter is not affected");

        b.answer(RYU, O2, {});
        check(!b.shown(RYU, O2), "no answer afterwards: stays hidden");
        b.answer(RYU, O2, {0, 1, 4});
        check(b.shown(RYU, O2) && b.state(RYU, O2) == State::Owned, "listed later (bought, DLC, unlock-all): shown");
        b.answer(RYU, O2, {0, 4});
        check(b.shown(RYU, O2), "not listed after being owned (online, Lua stopped): stays shown");
    }
    {
        Book b;
        b.answer(RYU, O3, {0, 4}, false);
        check(b.state(RYU, O3) == State::Unknown && b.shown(RYU, O3), "before the login a missing outfit is not denied");
        b.answer(RYU, O3, {0, 2, 4}, false);
        check(b.state(RYU, O3) == State::Owned, "before the login a listed outfit is owned");
        b.answer(RYU, O3, {0, 4}, true);
        check(b.shown(RYU, O3), "owned before the login stays shown");
    }
    {
        Book b;
        b.answer(RYU, 9, {0, 1, 4});
        check(!b.shown(RYU, 9), "an outfit the game does not have: hidden");
    }

    check(outfit_label(0) == "Outfit 1", "label of costume 0");
    check(outfit_label(1) == "Outfit 2", "label of costume 1");
    check(outfit_label(3) == "Outfit 4", "label of costume 3");
    check(outfit_label(4) == "DriveTech", "label of costume 4");
    check(outfit_label(7) == "costume 7", "label of another number");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
