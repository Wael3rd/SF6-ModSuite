// Unit test of plugin/src/stages_logic.hpp (state.json, the stage grids' tiles, the variation cycle, the keys) and
// plugin/src/codecheck.hpp (the R18 cache of several methods).
// Build and run: build_stages_logic_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/codecheck.hpp"
#include "../src/stages_logic.hpp"

#include <cstdio>

using namespace slots::stagelogic;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-80s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

// The loader's own scan of state.json (stages/loader/stage_redirect.cpp parse_state), copied: what it serves
static std::vector<std::pair<uint32_t, std::string>> loader_parse(const std::string& js) {
    std::vector<std::pair<uint32_t, std::string>> out;
    size_t sel = js.find("\"selected\"");
    if (sel == std::string::npos) return out;
    size_t open = js.find('{', sel), close = js.find('}', sel);
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    size_t i = open + 1;
    while (i < close) {
        size_t q1 = js.find('"', i);
        if (q1 == std::string::npos || q1 >= close) break;
        size_t q2 = js.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        size_t colon = js.find(':', q2);
        if (colon == std::string::npos) break;
        size_t v1 = js.find('"', colon);
        if (v1 == std::string::npos) break;
        size_t v2 = js.find('"', v1 + 1);
        if (v2 == std::string::npos || v2 > close) break;
        std::string id = js.substr(q1 + 1, q2 - q1 - 1), key = js.substr(v1 + 1, v2 - v1 - 1);
        if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos && !key.empty())
            out.push_back({ static_cast<uint32_t>(std::stoul(id)), key });
        i = v2 + 1;
    }
    return out;
}

static void test_state() {
    std::printf("state.json\n");
    State s;
    s.menu[kClassic][70000] = "saaa932d3422d";
    s.menu[kBattle][0] = "s685432d1343e";
    s.menu[kTraining][0] = "sd6f5bdab19f0";
    s.menu[kReplay][10000] = "s2ebef05422a3";
    s.menu[kReplay][20000] = "";                    // the vanilla look is not written
    s.active = s.menu[kBattle];
    const std::string js = state_json(s);
    auto served = loader_parse(js);
    check(served.size() == 1 && served[0].first == 0 && served[0].second == "s685432d1343e", "the loader's scan reads the served selection only");
    State back;
    check(parse_state(js, back), "our own file parses");
    check(back.active == s.active && back.menu[kClassic] == s.menu[kClassic] && back.menu[kBattle] == s.menu[kBattle] &&
          back.menu[kTraining] == s.menu[kTraining], "round trip of every menu");
    check(back.menu[kReplay].size() == 1 && back.menu[kReplay].count(10000), "an empty key is dropped");
    State old;
    check(parse_state("{\n    \"selected\": {\n        \"70000\": \"saaa932d3422d\"\n    }\n}\n", old) &&
          old.menu[kClassic].size() == 1 && old.menu[kClassic][70000] == "saaa932d3422d" && old.active == old.menu[kClassic] &&
          old.menu[kBattle] == old.active && old.menu[kTraining] == old.active && old.menu[kReplay] == old.active,
          "an older file: every menu starts from its selection (nothing changes)");
    State empty;
    const std::string ej = state_json(empty);
    check(loader_parse(ej).empty() && parse_state(ej, back) && back.active.empty(), "nothing chosen: vanilla everywhere");
    check(!parse_state("not json", back), "garbage refused");
    State esc;
    esc.menu[kClassic][1] = "a\"b";
    State esc2;
    check(parse_state(state_json(esc), esc2) && esc2.menu[kClassic][1] == "a\"b", "keys are escaped");
}

static void test_grid() {
    std::printf("stage grids\n");
    // the game's list today (PC3, 05/11) with Random (Battle Settings)
    std::vector<uint32_t> list = { 40000, 40100, 110000, 50000, 90000, 80000, 100000, 60000, 120000, 20000, 30000, 70000,
                                   10000, 10100, 130000, 20100, 41000, 140000, 150000, 400000, 0, 999999 };
    std::vector<TileRef> tiles = { { 0, "s384549e7b1d9" }, { 0, "s800c479d6d25" }, { 0, "s685432d1343e" }, { 777, "sx" }, { 0, "" } };
    size_t pos = 0;
    std::vector<uint32_t> ins;
    auto slots = plan_slots(list, tiles, pos, ins);
    check(pos == 21, "tiles go before Random (Training Room stays the last stage)");
    check(ins.size() == 3 && ins[0] == 0 && ins[2] == 0, "only tiles of stages the list holds, with a key");
    check(slots.size() == list.size() + 3, "one entry per id of the grown list");
    check(slots[20].base == 0 && !slots[20].tile && slots[21].tile && slots[21].key == "s384549e7b1d9" && slots[23].key == "s685432d1343e",
          "the vanilla Training Room, then its tiles in order");
    check(slots.back().base == 999999 && !slots.back().tile, "Random stays last");
    check(entry_index(slots, 0, "s685432d1343e") == 23, "a chosen tile: its own entry");
    check(entry_index(slots, 0, "sd6f5bdab19f0") == 20, "a chosen variation: the stage's own entry");
    check(entry_index(slots, 0, "") == 20 && entry_index(slots, 70000, "") == 11, "the vanilla look: the stage's own entry");
    check(entry_index(slots, 555, "") == -1, "a stage the grid does not hold: -1");
    std::vector<uint32_t> hub = { 40000, 0, 500000, 999999 };
    slots = plan_slots(hub, tiles, pos, ins);
    check(pos == 2 && slots[2].tile && slots[5].base == 500000 && slots[6].base == 999999, "before Battle Hub too");
    std::vector<uint32_t> plain = { 40000, 0 };
    slots = plan_slots(plain, {}, pos, ins);
    check(pos == 2 && ins.empty() && slots.size() == 2, "no tiles: the list as it is");
    slots = plan_slots({ 40000 }, tiles, pos, ins);
    check(ins.empty(), "Training Room not in the list: its tiles stay out");
}

static void test_cycle_keys() {
    std::printf("variations and keys\n");
    check(cycle(0, 7, 1) == 1 && cycle(7, 7, 1) == 0 && cycle(0, 7, -1) == 7, "cycle wraps over the vanilla look and the variations");
    check(cycle(0, 0, 1) == 0, "no variation: the vanilla look");
    check(cycle_label("<ICON KeyQ>", "Training Room", "Red", "<ICON KeyE>") == "<ICON KeyQ> Training Room \xC2\xB7 Red <ICON KeyE>",
          "label: stage, middle dot, variation between the keys");
    check(cycle_label("<L>", "Training Room", "", "<R>") == "<L> Training Room <R>", "label of the vanilla look");
    check(cycle_label("", "Training Room", "Red", "") == "Training Room \xC2\xB7 Red", "no keys: the names alone");
    check(pick_keys(false, false, false, false) == kKeysChildTab, "LT / RT when free");
    check(pick_keys(true, false, false, false) == kKeysTab, "LB / RB when the screen uses LT / RT");
    check(pick_keys(false, true, false, false) == kKeysTab, "LB / RB when LT / RT are switched off");
    check(pick_keys(true, false, true, false) == kKeysNone && pick_keys(true, false, false, true) == kKeysNone, "none when both are taken");
    check(key_left_id(kKeysChildTab) == 75 && key_right_id(kKeysChildTab) == 76 && key_left_id(kKeysTab) == 73 && key_left_id(kKeysNone) == -1,
          "the digital ids of the symbols (UIChildTabL/R 75/76, UITabL/R 73/74)");
}

static void test_codecheck() {
    using namespace slots::codecheck;
    std::printf("code check cache (several methods)\n");
    std::vector<Entry> now(2);
    now[0].index = 10; now[0].raw = 0x100; now[0].code = 0x100; now[0].label = "A.f()";
    now[1].index = 11; now[1].raw = 0x200; now[1].code = -1; now[1].label = "B.g(Int32)";
    std::vector<Entry> found = now;
    found[0].shared = 0; found[1].shared = 2; found[1].other = 99;
    const std::string key = "exe 1 2 3 size 4 time 5 methods 6";
    const std::string text = cache_text(key, found);
    std::vector<Entry> out;
    const char* why = "";
    check(cache_hit(text, key, now, out, &why) && out.size() == 2 && out[0].shared == 0 && out[1].shared == 2 && out[1].other == 99,
          "a cache written for these methods answers");
    check(!cache_hit(text, "exe other", now, out, &why) && out.empty(), "another executable: no answer");
    auto moved = now;
    moved[1].raw = 0x300;
    check(!cache_hit(text, key, moved, out, &why), "a method moved: no answer");
    auto fewer = now;
    fewer.pop_back();
    check(!cache_hit(text, key, fewer, out, &why), "another list (fewer methods): no answer");
    auto more = now;
    more.push_back(now[0]);
    check(!cache_hit(text, key, more, out, &why), "another list (more methods): no answer");
    check(!cache_hit(text.substr(0, text.size() - 1), key, now, out, &why), "a half-written file: no answer");
    check(!cache_hit("", key, now, out, &why), "no file: no answer");
    std::string unknown = text;
    unknown.replace(unknown.find(" 0 -1|A.f()"), 11, " -1 -1|A.f()");
    check(!cache_hit(unknown, key, now, out, &why), "a result never computed (-1): no answer");
}

int main() {
    test_state();
    test_grid();
    test_cycle_keys();
    test_codecheck();
    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
