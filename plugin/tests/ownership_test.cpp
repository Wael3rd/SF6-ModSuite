// Unit test of plugin/src/ownership.hpp: which slots the player sees from the game's answers.
// Build and run: build_ownership_test.bat (prints "all ok" and exits 0 when every check passes).
#include "../src/ownership.hpp"
#include "../../common/manager_mode.hpp"

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

    // NSFW outfits: shown only while "Display NSFW Outfits" is on
    check(slots::ownership::nsfw_shown(false, false), "other outfit, setting off: shown");
    check(!slots::ownership::nsfw_shown(true, false), "NSFW outfit, setting off: hidden");
    check(slots::ownership::nsfw_shown(true, true), "NSFW outfit, setting on: shown");
    {
        // The game's list keeps NSFW outfits whatever their ownership with an unlock-all script (C. Viper 20/10:
        // outfits 38 Lace lingerie and 39 Barefoot NSFW, the others shown): their positions, last first
        using slots::ownership::nsfw_drops;
        const std::vector<uint32_t> listed = {0, 1, 2, 3, 4, 13, 22, 25, 26, 27, 38, 39};
        check(nsfw_drops(listed, {38, 39}) == std::vector<int>({11, 10}), "NSFW outfits dropped from the list, last first");
        check(nsfw_drops(listed, {}).empty(), "no NSFW outfit: the list unchanged");
        check(nsfw_drops({0, 1, 4}, {38}).empty(), "an NSFW outfit not listed: nothing dropped");
        std::vector<uint32_t> l = listed;
        for (int i : nsfw_drops(l, {13, 39})) l.erase(l.begin() + i);
        check(l == std::vector<uint32_t>({0, 1, 2, 3, 4, 22, 25, 26, 27, 38}), "removed one at a time: the right ones go");
    }
    {
        using slots::ownership::outfit1_colour;
        const std::vector<int> o1 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        check(outfit1_colour(3, 0, o1) == 3, "back to Outfit 1: the colour chosen with the outfit");
        check(outfit1_colour(12, 2, o1) == 2, "back to Outfit 1: a colour Outfit 1 lacks -> the save's");
        check(outfit1_colour(-1, 15, {1, 2}) == 1, "back to Outfit 1: neither -> Outfit 1's first colour");
        check(outfit1_colour(3, 0, {}) == -1, "back to Outfit 1: colours not known -> left as is");
    }

    // Manager mode (the Slotsloader Manager installed the loader): its fork's rules
    {
        Book b;
        b.set_manager_rules(true);
        const std::vector<uint32_t> base_only = {0, 4};   // Outfit 1 and DriveTech only
        b.answer(RYU, O1, base_only);
        b.answer(RYU, O3, base_only);
        check(b.shown(RYU, O1) && !b.open(RYU, O1), "Manager mode: Outfit 1 slot always shown, never asked");
        check(b.answer(RYU, DT, {}) == State::Unknown && b.shown(RYU, DT) && !b.open(RYU, DT),
              "Manager mode: DriveTech slot always shown, never asked");
        check(!b.shown(RYU, O3), "Manager mode: an outfit not owned is still hidden");
    }
    {
        Book b;
        b.set_manager_rules(false);   // the Manager's opt-out marker
        b.answer(RYU, O3, {0, 4});
        check(b.shown(RYU, O3) && !b.open(RYU, O3), "Manager mode, opt-out: every slot shown, nothing asked");
    }
    {
        Book b;   // normal mode: unchanged (DriveTech asked like any outfit)
        b.answer(RYU, DT, {0, 1});
        check(!b.shown(RYU, DT), "normal mode: DriveTech not listed -> hidden, as before");
    }
    {
        using manager_mode::installed_by_manager;
        const std::string F = "sf6-manager-installs-1";
        const std::vector<manager_mode::ListedFile> list = {
            {true, "natives/stm/product/model/esf/esf001/001/x.mesh", 10},
            {true, "reframework\\Plugins\\SF6_CostumeSlotsNative.dll", 4096}};
        check(installed_by_manager(F, list, 4096), "Manager mode: plugin listed, same size");
        check(!installed_by_manager(F, list, 4097), "not Manager mode: plugin replaced since (other size)");
        check(!installed_by_manager(F, list, -1), "not Manager mode: no plugin on disk");
        check(!installed_by_manager("other", list, 4096), "not Manager mode: unknown list format");
        check(!installed_by_manager(F, {{false, "reframework/plugins/SF6_CostumeSlotsNative.dll", 4096}}, 4096),
              "not Manager mode: the loader's entry disabled in the Manager");
        check(!installed_by_manager(F, {{true, "natives/x", 4096}}, 4096), "not Manager mode: plugin not listed");
    }

    check(outfit_label(0) == "Outfit 1", "label of costume 0");
    check(outfit_label(1) == "Outfit 2", "label of costume 1");
    check(outfit_label(3) == "Outfit 4", "label of costume 3");
    check(outfit_label(4) == "DriveTech", "label of costume 4");
    check(outfit_label(7) == "costume 7", "label of another number");

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
