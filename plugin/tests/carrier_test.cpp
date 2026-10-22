// Unit test of plugin/src/carrier.hpp: the carrier of the online alias, a stale carrier.
// Build and run: build_carrier_test.bat (prints "all ok" and exits 0 when every check passes).
#include "../src/carrier.hpp"

#include <cstdio>

using namespace slots::carrier;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    const int O1 = 0, O2 = 1, DT = 4;

    // choose: from the costume numbers of the game's costume table
    check(choose({0, 1, 2, 3, 4}) == O1, "Outfit 1 for every fighter, DriveTech or not");
    check(choose({0, 1}) == O1, "Outfit 1 without DriveTech (Arjun)");
    check(choose({0}) == O1, "Outfit 1 alone");
    check(choose({1, 2, 4}) == -1, "no Outfit 1: no carrier");
    check(choose({}) == -1, "no record: no carrier");

    // stale: the save still holds a former carrier (DriveTech until 1.14)
    check(stale(DT, O1, DT, true, true), "DriveTech carrier then, slot intent: to Outfit 1");
    check(!stale(DT, O1, DT, false, true), "DriveTech chosen by the player, no intent: kept");
    check(stale(DT, O1, DT, false, false), "DriveTech written for Arjun, no intent: to Outfit 1");
    check(!stale(DT, O1, O1, true, true), "save on Outfit 1 already: nothing moves");
    check(!stale(DT, O1, O2, true, true), "another outfit in the save: not a carrier, left");
    check(!stale(O1, O1, O1, true, true), "Outfit 1 then and now: nothing moves");
    check(!stale(DT, -1, DT, true, false), "no carrier now: nothing moves");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
