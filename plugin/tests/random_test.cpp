// Unit test of plugin/src/random_logic.hpp (the Random outfit: rounds without repeats, a new round once every outfit was given,
// the NSFW filter at the draw, the colour rounds per outfit, the online set and the save, state.json's "random").
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
    {   // online: the set and the save
        const Draw game{ 2, 4 }, ours{ 12, 1 };
        check(save_holds(game, false) == game, "a game outfit drawn: the save holds it (the profile sends it)");
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
        const std::string text = "{ \"random\": " + state_json(all, 4) + " }";
        Json j;
        const bool parsed = json_parse(text, j);
        check(parsed, "state_json is valid JSON");
        auto back = parse_state(j.get("random"));
        check(back.size() == 1 && back.count(1), "an empty character is not written");
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
