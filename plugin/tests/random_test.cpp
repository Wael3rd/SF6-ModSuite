// Unit test of plugin/src/random_logic.hpp (the Random outfit: rounds without repeats, a new round once every outfit was given,
// the NSFW filter at the draw, the colour rounds per outfit with the Color Mod's entries, a Random colour of one outfit, the
// online set and the save, state.json's "random").
// Build and run: build_random_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/random_logic.hpp"

#include <cstdio>
#include <random>
#include <set>

using namespace slots;
using namespace slots::rnd;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-80s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static Rng seeded(uint32_t seed) {
    auto mt = std::make_shared<std::mt19937>(seed);
    return [mt]() { return static_cast<uint32_t>((*mt)()); };
}

int main() {
    std::printf("random_logic\n");
    const std::vector<int> outfits = { 0, 1, 2, 5, 7 };

    {   // one round gives every outfit once, then a new round starts, never with the last one drawn
        bool rounds_ok = true, no_back_to_back = true;
        for (uint32_t seed = 1; seed <= 200; ++seed) {
            Pool p;
            Rng rng = seeded(seed);
            int prev = -1;
            for (int round = 0; round < 6; ++round) {
                std::set<int> got;
                for (size_t i = 0; i < outfits.size(); ++i) {
                    const int v = draw(p, outfits, nullptr, rng);
                    if (v == prev) no_back_to_back = false;
                    prev = v;
                    got.insert(v);
                }
                if (got.size() != outfits.size()) rounds_ok = false;
            }
        }
        check(rounds_ok, "each round of 5 draws gives the 5 outfits once (200 seeds, 6 rounds)");
        check(no_back_to_back, "never the same outfit twice in a row, across rounds too");
    }
    {   // a single outfit: always that one; nothing allowed: -1 and the pool untouched
        Pool p;
        Rng rng = seeded(7);
        bool same = true;
        for (int i = 0; i < 5; ++i) same &= draw(p, { 3 }, nullptr, rng) == 3;
        check(same, "a pool of one outfit gives it at every draw");
        Pool q;
        q.drawn = { 1 };
        q.last = 1;
        check(draw(q, { 1, 2 }, [](int) { return false; }, rng) == -1 && q.drawn.size() == 1 && q.last == 1,
              "nothing allowed: -1, the pool left as it was");
        check(draw(q, {}, nullptr, rng) == -1, "no candidate: -1");
    }
    {   // NSFW outfits hidden: checked at each draw, whatever the round holds
        const std::set<int> nsfw = { 5, 7 };
        bool hidden = false;
        Pool p;
        Rng rng = seeded(11);
        for (int i = 0; i < 300; ++i) {
            const bool show = i >= 150;                       // "Display NSFW Outfits" turned on half way
            const int v = draw(p, outfits, [&](int c) { return show || !nsfw.count(c); }, rng);
            if (!show && nsfw.count(v)) hidden = true;
        }
        check(!hidden, "an NSFW outfit is never drawn while NSFW outfits are hidden");
        Pool q;
        q.drawn = { 0, 1, 2 };                               // the hidden ones left: a new round of what is allowed
        const int v = draw(q, outfits, [&](int c) { return !nsfw.count(c); }, rng);
        check(v >= 0 && !nsfw.count(v) && q.drawn.size() == 1, "every allowed outfit given: a new round of allowed ones only");
    }
    {   // an outfit gone from the list (its mod removed) is forgotten by the round; a new one joins it
        Pool p;
        p.drawn = { 0, 9 };
        p.last = 9;
        Rng rng = seeded(3);
        const int v = draw(p, { 0, 1 }, nullptr, rng);
        check(v == 1 && p.drawn == std::vector<int>({ 0, 1 }), "an outfit no longer listed leaves the round, the one left is drawn");
        const int w = draw(p, { 0, 1, 4 }, nullptr, rng);
        check(w == 4, "a new outfit listed is drawn before the round ends");
    }
    {   // the colour: a round per outfit, colour 0 when none is known
        Fighter f;
        Rng rng = seeded(5);
        auto colours = [](int cos) { return cos == 1 ? std::vector<int>{ 0, 1, 2 } : std::vector<int>{}; };
        std::set<int> c1;
        bool zero = true;
        for (int i = 0; i < 6; ++i) {
            const Draw d = draw_outfit(f, { 1, 8 }, nullptr, colours, rng);
            if (d.costume == 1) c1.insert(d.color);
            else zero &= d.color == 0;
        }
        check(c1.size() == 3, "outfit 1 drawn 3 times: its 3 colours, one round");
        check(zero, "an outfit without known colours: colour 0");
        check(!draw_outfit(f, { 1 }, [](int) { return false; }, colours, rng).valid(), "no outfit allowed: no draw");
    }
    {   // the Color Mod's entries: a number of their own per file, riding on colour 02
        const int rw = entry_value("..\\color_mods\\Ryu\\pack\\esf001_001_cmd_003.user.2");
        const int mc = entry_value("..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_001_cmd_001.user.2");
        check(is_entry(rw) && is_entry(mc) && rw != mc, "two entry files: two entry numbers, both entries");
        check(rw == entry_value("..\\color_mods\\Ryu\\pack\\esf001_001_cmd_003.user.2"), "the same file: the same number (a new pack moves no round)");
        check(!is_entry(0) && !is_entry(9) && !is_entry(207), "the game's colours (01-10, EX, DX) are no entries");
        check(worn_colour(rw) == 1 && worn_colour(4) == 4 && worn_colour(201) == 201, "an entry is worn on colour 02, a game colour as it is");
        // a round among the game's colours and the entries: each once, never twice in a row
        Fighter f;
        Rng rng = seeded(21);
        const std::vector<int> cs = { 0, 1, 2, rw, mc };
        std::set<int> got;
        int prev = -1;
        bool back_to_back = false;
        for (int i = 0; i < 5; ++i) {
            const int v = draw_colour(f, 3, cs, nullptr, rng);
            got.insert(v);
            back_to_back |= v == prev;
            prev = v;
        }
        check(got.size() == 5 && !back_to_back, "a round of 5 colours (3 of the game's, 2 entries): each once");
        check(f.colours.count(3) && f.colours[3].drawn.size() == 5, "the round is that outfit's (shared with the Random outfit's draws of it)");
        const int v = draw_colour(f, 3, cs, [&](int c) { return worn_colour(c) != 1; }, rng);
        check(v == 0 || v == 2, "a mirror on colour 02: neither 02 nor an entry (they ride on it)");
        check(draw_colour(f, 4, {}, nullptr, rng) == 0, "nothing known: colour 0");
        // the Random outfit's colour: the entries too, the mirror filter by outfit
        Fighter g;
        const Draw d = draw_outfit(g, { 5 }, nullptr, [&](int) { return std::vector<int>{ 1, rw }; }, rng,
                                   [](int cos, int c) { return cos != 5 || worn_colour(c) != 1; });
        check(d.costume == 5 && d.color == 0, "every colour of the outfit drawn ridden on 02 by a mirror: colour 0");
    }
    {   // online: the set and the save
        const Draw game{ 2, 4 }, ours{ 12, 1 };
        check(save_holds(game, false) == game, "a game outfit drawn: the save holds it (the profile sends it)");
        const int rw = entry_value("rw");
        check(save_holds(Draw{ 2, rw }, false) == Draw({ 2, 1 }), "a game outfit and an entry drawn: the save holds the outfit on 02");
        Bind e = bind_set(Draw{ 2, rw }, false, 2, 1);
        check(e.consumed && !e.local && e.set == Draw({ 2, rw }), "the match holds that outfit on 02: the entry is worn (on this PC)");
        e = bind_set(Draw{ 2, rw }, false, 2, 5);
        check(e.consumed && e.set == Draw({ 2, 5 }), "the match holds another colour: that colour (what the opponent sees)");
        e = bind_set(Draw{ 12, rw }, true, 0, 3);
        check(e.consumed && e.local && e.set == Draw({ 12, rw }), "a Slots Loader outfit and an entry: the alias wears both");
        // Battle Settings' Random colour of a game outfit
        Bind c = bind_colour(Draw{ 3, 6 }, 3, 6);
        check(c.consumed && c.set == Draw({ 3, 6 }), "Random colour: the match holds the colour drawn, the set wears it");
        c = bind_colour(Draw{ 3, rw }, 3, 1);
        check(c.consumed && c.set == Draw({ 3, rw }), "Random colour, an entry: the match holds 02, the entry is worn");
        c = bind_colour(Draw{ 3, 6 }, 3, 2);
        check(!c.consumed && c.set == Draw({ 3, 2 }), "no profile since the draw (another colour): the set wears it, the draw waits");
        c = bind_colour(Draw{ 3, 6 }, 4, 6);
        check(!c.consumed && c.set == Draw({ 4, 6 }), "another outfit in the match: what it holds, the draw waits");
        check(!bind_colour(Draw{}, 3, 6).consumed, "no Random colour: nothing taken");
        check(!save_holds(ours, true).valid(), "a Slots Loader outfit drawn: the save holds the carrier");
        Bind b = bind_set(ours, true, 0, 3);
        check(b.consumed && b.local && b.set == ours, "Slots Loader outfit: worn on this PC over what the match holds, the draw taken");
        b = bind_set(game, false, 2, 4);
        check(b.consumed && !b.local && b.set == Draw({ 2, 4 }), "game outfit the match holds: the game wears it, the draw taken");
        b = bind_set(game, false, 2, 6);
        check(b.consumed && !b.local && b.set == Draw({ 2, 6 }), "game outfit, another colour in the match: the match's colour (what the opponent sees)");
        b = bind_set(game, false, 0, 1);
        check(b.consumed && b.local && b.set == game, "game outfit the match does not hold (no profile since): worn on this PC, the draw taken");
        b = bind_set(Draw{ 0, 2 }, false, 4, 1);
        check(!b.consumed && !b.local && b.set == Draw({ 4, 1 }), "Outfit 1 drawn, the match holds another: worn as the match holds, the draw waits");
        b = bind_set(Draw{ 0, 2 }, false, 0, 5);
        check(b.consumed && !b.local && b.set == Draw({ 0, 5 }), "Outfit 1 drawn and held: the game wears it");
        b = bind_set(Draw{}, false, 3, 0);
        check(!b.consumed && !b.local && b.set == Draw({ 3, 0 }), "no draw yet: what the match holds");
    }
    {   // state.json "random": written, read back; unreadable parts left out
        std::map<int, Fighter> all;
        all[1].outfits = { { 7, 0 }, 0 };
        all[1].next = { 7, 3 };
        all[1].colours[7] = { { 3 }, 3 };
        all[1].colours[0] = { {}, 5 };
        all[2];                                              // nothing to keep: left out
        all[5].next_colour = { 2, entry_value("mc1") };      // a Random colour alone (no Random outfit draw)
        const std::string text = "{ \"random\": " + state_json(all, 4) + " }";
        Json j;
        const bool parsed = json_parse(text, j);
        check(parsed, "state_json is valid JSON");
        auto back = parse_state(j.get("random"));
        check(back.size() == 2 && back.count(1) && back.count(5), "an empty character is not written");
        check(back[5].next_colour == Draw({ 2, entry_value("mc1") }) && !back[5].next.valid() && !back[1].next_colour.valid(),
              "the Random colour's next one read back (an entry's number whole)");
        const Fighter& f = back[1];
        check(f.outfits.drawn == std::vector<int>({ 7, 0 }) && f.outfits.last == 0, "the outfit round read back");
        check(f.next == Draw({ 7, 3 }), "the next draw read back");
        check(f.colours.size() == 2 && f.colours.at(7).drawn == std::vector<int>({ 3 }) && f.colours.at(0).last == 5,
              "the colour rounds read back (outfit 0 included)");
        Json bad;
        json_parse("{ \"random\": { \"x\": 1, \"3\": { \"next\": [1], \"outfits\": 4 }, \"4\": [] } }", bad);
        auto b2 = parse_state(bad.get("random"));
        check(b2.size() == 1 && b2.count(3) && !b2[3].next.valid() && b2[3].outfits.drawn.empty(), "unreadable parts left out");
        check(parse_state(nullptr).empty(), "no \"random\": nothing");
        check(state_json({}, 4) == "{}", "nothing to keep: {}");
    }

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
