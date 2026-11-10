// Unit test of plugin/src/replay_outfits.hpp: the replay's key, the registry's mods, replay_outfits.json, the journal and
// what each side wears in Default and Forced.
// Build and run: build_replay_outfits_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/replay_outfits.hpp"

#include <cstdio>

using namespace slots::replay;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-80s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main() {
    // ---- the key ----
    ReplayFacts a;
    a.seed = 290329851; a.stage = 0; a.fighters = { 1, 31 }; a.round_seeds = { 60748, 21618, 27696, 0, 0 };
    const std::string ka = replay_key(a);
    check(ka.size() == 16 && ka.find_first_not_of("0123456789abcdef") == std::string::npos, "16 hex digits");
    check(replay_key(a) == ka, "the same facts: the same key");
    ReplayFacts b = a; b.seed ^= 1;
    check(replay_key(b) != ka, "another match seed: another key");
    b = a; b.round_seeds[1] = 21619;
    check(replay_key(b) != ka, "another round seed: another key");
    b = a; b.fighters = { 31, 1 };
    check(replay_key(b) != ka, "the sides swapped: another key");
    b = a; b.stage = 3;
    check(replay_key(b) != ka, "another stage: another key");
    b = a; b.round_seeds = { 60748, 21618, 27696 };
    check(replay_key(b) == ka, "3 rounds played, the server's copy pads to 5 with zeros: the same key");
    b = a; b.round_seeds = { 60748, 21618, 27696, 0, 0, 0, 0 };
    check(replay_key(b) == ka, "more zero padding: the same key");
    b = a; b.round_seeds = { 60748, 21618, 27696, 5 };
    check(replay_key(b) != ka, "one round more: another key");
    b = a; b.round_seeds = { 60748, 0, 27696 };
    check(replay_key(b) != ka, "a zero seed inside the rounds counts");
    b = a; b.round_seeds = { 0, 0, 0, 0, 0 };
    check(replay_key(b).empty(), "every round seed zero: no key");
    b = a; b.round_seeds = { 60748 | 0x10000u, 21618, 27696, 0, 0 };
    check(replay_key(b) == ka, "a round seed is 16 bits: higher bits ignored");
    b = a; b.round_seeds.clear();
    check(replay_key(b).empty(), "no round: no key");
    b = a; b.fighters.clear();
    check(replay_key(b).empty(), "no fighter: no key");

    // ---- the registry ----
    const std::string reg_text = R"({"version":1,"slots":[
        {"mod_id":"aaaa","fighter":1,"original_costume_no":0,"new_costume_no":5},
        {"mod_id":"bbbb","fighter":1,"original_costume_no":0,"new_costume_no":6},
        {"mod_id":"cccc","fighter":32,"original_costume_no":0,"new_costume_no":5},
        {"mod_id":"cccc","fighter":32,"original_costume_no":1,"new_costume_no":6},
        {"mod_id":"cccc","fighter":32,"original_costume_no":2,"new_costume_no":7},
        {"mod_id":"","fighter":2,"original_costume_no":0,"new_costume_no":5},
        {"fighter":2,"original_costume_no":0,"new_costume_no":6}],"possession":[]})";
    const auto reg = parse_registry(reg_text);
    check(reg.size() == 5, "registry: 5 slots with a mod (no mod_id / an empty one left out)");
    check(parse_registry("not json").empty(), "an unreadable registry: no slot");
    check(resolve_mod(reg, 1, "bbbb", 6) == 6, "mod -> its slot");
    check(resolve_mod(reg, 1, "bbbb", 9) == 6, "the slot numbered again: found by its mod");
    check(resolve_mod(reg, 1, "zzzz", 6) == -1, "the mod is gone: -1");
    check(resolve_mod(reg, 2, "bbbb", 6) == -1, "the mod is another character's: -1");
    check(resolve_mod(reg, 32, "cccc", 9, 1) == 6, "a mod with 3 slots: the one made from the same outfit");
    check(resolve_mod(reg, 32, "cccc", 7) == 7, "a mod with 3 slots, no base: the one with the same number");
    check(resolve_mod(reg, 32, "cccc", 9) == 5, "a mod with 3 slots, no base, number gone: the first");
    check(resolve_mod(reg, 32, "cccc", 7, 4) == 7, "a mod with 3 slots, base not among them: the same number");
    check(slot_of(reg, 32, 6) && slot_of(reg, 32, 6)->mod_id == "cccc" && slot_of(reg, 32, 6)->base == 1, "slot_of: its mod and base");
    check(slot_of(reg, 1, 0) == nullptr, "slot_of a game outfit: none");

    // ---- replay_outfits.json ----
    std::map<int, Forced> forced;
    check(parse_outfits("\xEF\xBB\xBF{\"version\":1,\"fighters\":{"
                        "\"1\":{\"mode\":\"forced\",\"costume\":6,\"mod_id\":\"bbbb\",\"color\":2},"
                        "\"31\":{\"mode\":\"forced\",\"costume\":3,\"color\":7},"
                        "\"2\":{\"mode\":\"default\",\"costume\":1,\"color\":1},"
                        "\"5\":{\"mode\":\"forced\"},"
                        "\"x\":{\"mode\":\"forced\",\"costume\":1}}}", forced),
          "replay_outfits.json read (with a BOM)");
    check(forced.size() == 2 && forced.count(1) && forced.count(31), "forced: F1 and F31 (default, no costume, bad id left out)");
    check(forced[1].mod_id == "bbbb" && forced[1].costume == 6 && forced[1].color == 2, "F1: an extra outfit by its mod");
    check(forced[31].mod_id.empty() && forced[31].costume == 3 && forced[31].color == 7, "F31: a game outfit");
    std::map<int, Forced> kept = forced;
    check(!parse_outfits("{\"version\":1,\"fighters\":{", kept) && kept.size() == 2, "a half-written file: refused, the last choices kept");
    std::map<int, Forced> none;
    check(parse_outfits("{\"version\":1}", none) && none.empty(), "no fighters: nothing forced");
    {
        BgmChoice bg; bg.mode = BgmMode::Stage;
        check(parse_outfits("{\"version\":1}", none, nullptr, &bg) && bg.mode == BgmMode::Random, "music: no key, Random");
        check(parse_outfits("{\"version\":1,\"bgm\":{\"mode\":\"opponent\"}}", none, nullptr, &bg) && bg.mode == BgmMode::Opponent && bgm_set(bg) == 2,
              "music: read for the replay, with no fighters too");
    }
    std::map<int, Forced> cmf;
    check(parse_outfits("{\"version\":1,\"fighters\":{\"1\":{\"mode\":\"forced\",\"costume\":8,\"mod_id\":\"8cd9\",\"color\":4,"
                        "\"colormod\":\"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_009_cmd_001.user.2\"}}}", cmf) &&
          cmf[1].colormod == "..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_009_cmd_001.user.2" && cmf[1].color == 1,
          "a Color Mod entry is read (its id; worn on colour 1, its carrier)");
    {   // a Random colour: read, carried to the plan (the playback draws it); older files: none
        std::map<int, Forced> rf;
        check(parse_outfits("{\"version\":1,\"fighters\":{\"1\":{\"mode\":\"forced\",\"costume\":2,\"color\":0,\"color_random\":true},"
                            "\"2\":{\"mode\":\"forced\",\"costume\":1,\"color\":4}}}", rf) &&
              rf[1].random_color && rf[1].color == 0 && !rf[2].random_color && rf[2].color == 4, "color_random read, absent: false");
        const auto w = plan({ Side{ 1, 0, 3 }, Side{ 2, 1, 4 } }, nullptr, rf, {});
        check(w[0].forced && w[0].random_color && w[0].costume == 2 && !w[1].random_color, "the plan says which side's colour is Random");
    }
    int stage = 7;
    {
        std::map<int, Forced> fo;
        check(parse_outfits("{\"version\":1,\"fighters\":{\"26\":{\"mode\":\"forced\",\"costume\":0,\"color\":0,"
                            "\"options\":{\"esf026/game outfit 1\":[\"wesker01\"],\"esf026/x\":[]}},"
                            "\"30\":{\"mode\":\"off\",\"options\":{\"esf030/game outfit 1\":[\"body0001\"]}}}}", fo) &&
              fo.size() == 1 && fo.count(26), "a fighter set to Off is not forced, whatever options it keeps");
        check(fo[26].options.size() == 2 && fo[26].options["esf026/game outfit 1"] == std::vector<std::string>{ "wesker01" },
              "a forced fighter's costume options are read, per outfit");
        std::map<int, Forced> fn;
        check(parse_outfits("{\"version\":1,\"fighters\":{\"2\":{\"mode\":\"forced\",\"costume\":1,\"color\":0}}}", fn) && fn[2].options.empty(),
              "a file from before them: no options");
    }
    check(parse_outfits("{\"version\":1,\"fighters\":{},\"stage\":{\"mode\":\"forced\",\"id\":40000}}", none, &stage) && stage == 40000,
          "a forced stage is read");
    check(parse_outfits("{\"version\":1,\"fighters\":{}}", none, &stage) && stage == -1, "no stage entry: the replay's own stage");
    check(parse_outfits("{\"version\":1,\"stage\":{\"mode\":\"auto\",\"id\":3}}", none, &stage) && stage == -1,
          "a stage entry that is not forced: the replay's own stage");

    // ---- the journal ----
    Entry e;
    e.key = ka; e.time = 1793862214; e.mode = 14; e.me = 0;
    e.sides = { { 1, 0, 1 }, { 1, 2, 6 } };
    e.has_slot = true; e.mod_id = "aaaa"; e.slot_costume = 5; e.slot_color = 3; e.slot_base = 0;
    const std::string line = journal_line(e);
    check(line.back() == '\n' && line.find('\n') == line.size() - 1, "one line");
    Entry e2 = e;
    e2.key = "00000000000000ff"; e2.me = 1; e2.has_slot = false;
    std::map<std::string, Entry> j;
    const size_t n = parse_journal(line + "{\"v\":1,\"key\":\"broken\n" + journal_line(e2) + "\n", j);
    check(n == 2 && j.size() == 2, "two lines read, a broken one skipped");
    const Entry& r = j[ka];
    check(r.me == 0 && r.mode == 14 && r.time == 1793862214 && r.sides.size() == 2 && r.sides[1].costume == 2 && r.sides[1].color == 6,
          "a line read back: side, mode, time, both sides");
    check(r.has_slot && r.mod_id == "aaaa" && r.slot_costume == 5 && r.slot_color == 3 && r.slot_base == 0, "a line read back: the slot worn");
    check(!j["00000000000000ff"].has_slot && j["00000000000000ff"].me == 1, "\"slot\": null read back");
    Entry e3 = e; e3.slot_color = 4;
    std::map<std::string, Entry> j2;
    parse_journal(line + journal_line(e3), j2);
    check(j2.size() == 1 && j2[ka].slot_color == 4, "the same key twice: the later line wins");
    check(line.find("short") == std::string::npos && line.find("name") == std::string::npos, "numbers only: no name, no player id");

    // ---- what each side wears ----
    const std::map<int, Forced> no_forced;
    // a Ryu mirror: the player (P1) wore slot 5 (mod aaaa) over Outfit 1, the opponent was really in Outfit 1
    const std::vector<Side> mirror = { { 1, 0, 1 }, { 1, 0, 1 } };
    Entry m = e; m.sides = mirror;
    auto w = plan(mirror, &m, no_forced, reg);
    check(w[0].change && w[0].costume == 5 && w[0].color == 3, "Default: the player's side wears the slot of the journal");
    check(!w[1].change, "Default: the opponent on the same carrier keeps what the replay holds");
    w = plan(mirror, nullptr, no_forced, reg);
    check(!w[0].change && !w[1].change, "Default, no journal line (an old replay): what it recorded");
    Entry mo = m; mo.mod_id = "zzzz";
    w = plan(mirror, &mo, no_forced, reg);
    check(!w[0].change && !w[1].change, "Default, the worn slot's mod gone: what it recorded");
    Entry mn = m; mn.has_slot = false;
    w = plan(mirror, &mn, no_forced, reg);
    check(!w[0].change && !w[1].change, "Default, the player wore no slot: what it recorded");
    Entry ms = m; ms.me = 1; ms.sides = { { 1, 0, 1 }, { 31, 0, 1 } };
    const std::vector<Side> other = { { 1, 0, 1 }, { 2, 0, 1 } };
    w = plan(other, &ms, no_forced, reg);
    check(!w[0].change && !w[1].change, "Default, the journal's side is another fighter: left alone");
    Entry mr = m; mr.slot_costume = 9;   // the slot was numbered 9 then, 5 now
    w = plan(mirror, &mr, no_forced, reg);
    check(w[0].change && w[0].costume == 5, "Default, the slot numbered again: its number now");

    // Forced: both sides of that character, the player's own included; the other character keeps Default
    std::map<int, Forced> f1 = { { 1, Forced{ 6, 2, "bbbb" } } };
    w = plan(mirror, &m, f1, reg);
    check(w[0].change && w[0].costume == 6 && w[0].color == 2 && w[1].change && w[1].costume == 6 && w[1].color == 2,
          "Forced extra outfit: both sides of a mirror, the player's own too");
    const std::vector<Side> ryu_cammy = { { 1, 0, 1 }, { 6, 2, 0 } };
    Entry rc = m; rc.sides = ryu_cammy;
    std::map<int, Forced> f6 = { { 6, Forced{ 3, 7, "" } } };
    w = plan(ryu_cammy, &rc, f6, reg);
    check(w[0].change && w[0].costume == 5 && w[1].change && w[1].costume == 3 && w[1].color == 7,
          "Forced on the opponent's character, Default (journal) on the player's");
    std::map<int, Forced> fgone = { { 1, Forced{ 6, 2, "zzzz" } } };
    w = plan(mirror, &m, fgone, reg);
    check(w[0].change && w[0].costume == 5 && !w[1].change, "Forced outfit's mod gone: Default for that character");
    std::map<int, Forced> fsame = { { 6, Forced{ 2, 0, "" } } };
    w = plan(ryu_cammy, nullptr, fsame, reg);
    check(!w[1].change, "Forced to what the replay holds already: nothing written");
    std::map<int, Forced> fcol = { { 6, Forced{ 2, 4, "" } } };
    w = plan(ryu_cammy, nullptr, fcol, reg);
    check(w[1].change && w[1].costume == 2 && w[1].color == 4, "Forced, same outfit, another colour: written");
    check(w[1].forced && w[1].colormod.empty() && !w[0].forced, "a forced side is marked (the Color Mod is asked: the game colour)");
    Forced fe{ 2, 1, "" };
    fe.colormod = "..\\color_mods\\Cammy\\MANUALCOLORS_01\\esf006_003_cmd_001.user.2";
    std::map<int, Forced> fcm = { { 6, fe } };
    w = plan(ryu_cammy, nullptr, fcm, reg);
    check(w[1].forced && w[1].colormod == fe.colormod && w[1].color == 1, "Forced Color Mod entry: colour 1 and the entry to ask for");

    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("all ok\n");
    return 0;
}
