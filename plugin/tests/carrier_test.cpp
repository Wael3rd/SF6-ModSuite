// Unit test of plugin/src/carrier.hpp: the carrier of the online alias, the mirror rule, a stale carrier.
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
    const int RYU = 1, ARJUN = 34;
    const int O1 = 0, O2 = 1, DT = 4;

    // choose: from the costume numbers of the game's costume table
    check(choose({0, 1, 2, 3, 4}) == DT, "DriveTech when the character has it");
    check(choose({0, 1, 2, 4}) == DT, "DriveTech without an Outfit 4");
    check(choose({0, 1}) == O1, "Outfit 1 without DriveTech (Arjun)");
    check(choose({0}) == O1, "Outfit 1 alone");
    check(choose({1, 2}) == -1, "neither: no carrier");
    check(choose({}) == -1, "no record: no carrier");

    // mirror: two fighters of the battle in the same character's carrier
    check(mirror({{ARJUN, O1}, {ARJUN, O1}}, ARJUN, O1), "Arjun Outfit 1 vs Arjun Outfit 1: mirror");
    check(!mirror({{ARJUN, O1}, {ARJUN, O2}}, ARJUN, O1), "Arjun Outfit 1 vs Arjun Outfit 2: no mirror");
    check(!mirror({{ARJUN, O1}, {RYU, O1}}, ARJUN, O1), "Arjun vs Ryu, both Outfit 1: no mirror");
    check(mirror({{RYU, DT}, {RYU, DT}}, RYU, DT), "Ryu DriveTech vs Ryu DriveTech: mirror");
    check(!mirror({{RYU, DT}, {RYU, O1}}, RYU, DT), "Ryu DriveTech vs Ryu Outfit 1: no mirror");
    check(!mirror({{RYU, O1}, {RYU, O1}}, RYU, DT), "a mirror off the carrier does not count");
    check(!mirror({{ARJUN, -1}, {ARJUN, -1}}, ARJUN, -1), "no carrier: never a mirror");
    check(!mirror({{ARJUN, O1}}, ARJUN, O1), "one fighter: no mirror");

    // stale: the save still holds a former carrier
    check(!stale(DT, DT, DT, true, true), "DriveTech then and now: nothing moves");
    check(!stale(DT, DT, O1, false, true), "DriveTech carrier, Outfit 1 chosen: the player's");
    check(stale(DT, O1, DT, true, false), "DriveTech written for Arjun, slot intent: to Outfit 1");
    check(stale(DT, O1, DT, false, false), "DriveTech written for Arjun, no intent: to Outfit 1");
    check(!stale(DT, O1, O1, true, true), "Arjun's save on Outfit 1 already: nothing moves");
    check(stale(O1, DT, O1, true, true), "Arjun gets DriveTech, slot intent: to DriveTech");
    check(!stale(O1, DT, O1, false, true), "Arjun gets DriveTech, Outfit 1 chosen: the player's");
    check(!stale(O1, DT, O2, true, true), "another outfit in the save: not a carrier, left");
    check(!stale(DT, -1, DT, true, false), "no carrier now: nothing moves");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
