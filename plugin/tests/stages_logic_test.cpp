// Unit test of plugin/src/stages_logic.hpp (state.json, the stage grids' tiles, the variation cycle, label, key and hint,
// the chosen mark) and
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
    std::printf("variations, the variation key and its hint\n");
    check(cycle(0, 7, 1) == 1 && cycle(7, 7, 1) == 0 && cycle(0, 7, -1) == 7, "cycle wraps over the vanilla look and the variations");
    check(cycle(0, 0, 1) == 0, "no variation: the vanilla look");
    check(cycle(0, 1, 1) == 1 && cycle(1, 1, 1) == 0, "one key press: next variation, wrapping to the vanilla look");
    // one name line, never "stage · variation"
    check(variation_label("Dhalsimer Temple", "Dhalsimer Temple Banner", "") == "Dhalsimer Temple Banner", "label: the variation's own name");
    check(variation_label("Dhalsimer Temple", "dhalsimer temple ", "Stage Lighting Overhaul") == "Stage Lighting Overhaul",
          "label: a variation named like its stage shows its bundle");
    check(variation_label("Dhalsimer Temple", "Dhalsimer Temple", "") == "Dhalsimer Temple", "label: no bundle, the name");
    check(variation_label("Training Room", "", "x") == "Training Room", "label: the vanilla look, the stage's name");
    check(variation_label("Training Room", "Training Room Red", "Stage Lighting Overhaul").find("\xC2\xB7") == std::string::npos,
          "label: no middle dot, one name");
    // the hint
    check(kVariationKeyId == 341, "the key: UIOpponentCosSettingToggleEXDXColors (341)");
    check(variation_hint(true, false, 2, true), "hint: focused stage with variations");
    check(!variation_hint(true, false, 0, true), "no hint: a stage without variations");
    check(!variation_hint(true, true, 0, true) && !variation_hint(true, true, 3, true), "no hint: a tile");
    check(!variation_hint(false, false, 2, true), "no hint: the grid has not the focus");
    check(!variation_hint(true, false, 2, false), "no hint: no key symbol");
    check(variation_hint_text("<ICON KeyR>") == "<ICON KeyR> Stage Variation", "hint text: the game's icon, then the label");
    check(guide_hint_text("<ICON KeyR>", "Costume options") == "<ICON KeyR> Costume options", "hint text: another entry on the same key");
    // the keyboard key of a symbol (QWERTY places)
    check(qwerty_scan("<ICON KeyR>") == 0x13 && qwerty_scan("<ICON KeyT>") == 0x14 && qwerty_scan("<ICON KeyA>") == 0x1E &&
          qwerty_scan("<ICON KeyM>") == 0x32 && qwerty_scan("<ICON Key1>") == 0x02 && qwerty_scan("<ICON Key0>") == 0x0B,
          "scan codes of letters and digits");
    check(qwerty_scan("<ICON KeyEsc>") == 0 && qwerty_scan("<ICON BtnL>") == 0 && qwerty_scan("") == 0, "no letter: no key");
}

static void test_chosen_mark() {
    std::printf("the chosen mark\n");
    std::vector<uint32_t> list = { 40000, 70000, 0, 999999 };
    std::vector<TileRef> tiles = { { 0, "tA" }, { 0, "tB" } };
    size_t pos = 0;
    std::vector<uint32_t> ins;
    auto slots = plan_slots(list, tiles, pos, ins);   // 0:40000 1:70000 2:0 3:tA 4:tB 5:Random
    check(slots.size() == 6 && slots[3].key == "tA" && slots[4].key == "tB", "grid: Training Room, then its two tiles");
    // the game marks the cell ConvertStageIdToGridIndex answers: one entry, never every entry of the stage
    check(entry_index(slots, 0, "tB") == 4 && entry_index(slots, 0, "") == 2 && entry_index(slots, 0, "variation") == 2,
          "the mark: the chosen tile only, else the stage's own entry");
    int marked = 0;
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) marked += i == entry_index(slots, 0, "tB") ? 1 : 0;
    check(marked == 1, "one marked entry among the stage's entries");
    // Decide compares with the choice from before it
    check(decide_marks_now(slots, 0, "", 4), "a tile confirmed over its own stage: marked at once");
    check(decide_marks_now(slots, 0, "tA", 4), "another tile of the same stage: marked at once");
    check(decide_marks_now(slots, 0, "tB", 2), "the stage's own entry after one of its tiles: marked at once");
    check(!decide_marks_now(slots, 0, "tB", 4), "the tile chosen again: already chosen (nothing to do)");
    check(!decide_marks_now(slots, 0, "", 2), "the stage chosen again: already chosen");
}

static void test_banner_name() {
    std::printf("the banner's name: never squeezed\n");
    auto near = [](float w, float real) { return w > real * 0.98f && w < real * 1.02f; };
    check(near(name_width("The Macho Ring"), 187) && near(name_width("Training Room"), 169) && near(name_width("SIRN Training Room"), 233) &&
          near(name_width("Suzaku Castle Over Training Room"), 412), "the width estimate against the names measured");
    check(!banner_name("Training Room").right && banner_name("Training Room").text == "Training Room", "a name that fits: the game's place");
    check(!banner_name("Aokigahara (no NPC, fixed)").right, "the longest name that fits today");
    auto s = banner_name("Suzaku Castle Over Training Room");
    check(s.right && s.text == "Suzaku Castle Over Training Room", "a longer one: right-aligned, whole");
    s = banner_name("Black Training stage Mod-2943-1-1759553342");
    check(s.right && s.text == "Black Training stage Mod-2943-1-1759553342", "the longest today: right-aligned, whole");
    const std::string longest(80, 'a');
    s = banner_name(longest);
    check(s.right && s.text == longest, "wider than the banner: whole, it scrolls inside it");
    s = banner_name("\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82\xE3\x81\x82");
    check(s.right && s.text.size() == 39, "13 square characters: right-aligned, whole characters");
    check(!banner_name("").right && banner_name("").text.empty(), "no name");
}

static void test_vs_stage() {
    std::printf("the VS screen\n");
    const Choice served = { { 0, "tile" }, { 70000, "banner" } };
    const std::map<uint32_t, std::string> names = { { 0, "Training Room" }, { 70000, "Dhalsimer Temple" } };
    check(vs_stage(served, names, "Training Room", -1) == 0, "the game's name stands for a served stage");
    check(vs_stage(served, names, "Dhalsimer Temple", 0) == 70000, "the name first (the stage on screen, not the one last set up)");
    check(vs_stage(served, names, "Genbu Temple", -1) == -1, "a stage nothing is served for: the game's own");
    check(vs_stage(served, names, "Genbu Temple", 20000) == -1, "the same when it is the stage set up");
    check(vs_stage(served, names, "training room!", 0) == 0, "a name read differently: the stage set up, if served");
    check(vs_stage({}, names, "Training Room", 0) == -1, "nothing served: the game's own");
    check(vs_stage({ { 0, "" } }, names, "Training Room", 0) == -1, "an empty key is its own look");
}

static void test_preview() {
    std::printf("the variation key previews, Confirm chooses\n");
    // entries: 0 Genbu (2 variations), 1 Training Room (3 variations); Training Room chosen with its variation 2
    const int chosen = 1, chosen_var = 2;
    Preview p;
    check(shown_variation(0, chosen, chosen_var, p) == 0 && shown_variation(1, chosen, chosen_var, p) == 2,
          "at rest: the chosen entry its chosen variation, another its own look");
    p = preview_next(1, chosen, chosen_var, p, 3);
    check(p.entry == 1 && p.var == 3 && shown_variation(1, chosen, chosen_var, p) == 3, "the key on the chosen entry: the next one");
    p = preview_next(1, chosen, chosen_var, p, 3);
    check(p.var == 0, "again: wraps to its own look");
    p = preview_follow(p, 1);
    check(p.entry == 1, "the focus stays: the preview stays");
    p = preview_follow(p, 0);
    check(p.entry == -1 && shown_variation(1, chosen, chosen_var, p) == 2, "the focus moves off: back to the chosen variation");
    p = preview_next(0, chosen, chosen_var, p, 2);
    check(p.entry == 0 && p.var == 1 && shown_variation(0, chosen, chosen_var, p) == 1, "another stage: its first variation");
    p = preview_follow(p, 1);
    check(shown_variation(0, chosen, chosen_var, p) == 0, "left without Confirm: its own look again");
    check(shown_variation(1, -1, 0, Preview{}) == 0, "nothing chosen in the grid: own looks");
    check(banner_entry(true, 5, 9) == 5, "the banner: the focused entry while the grid has the focus");
    check(banner_entry(false, 5, 9) == 9, "the focus on the rows: the chosen entry");
    check(banner_entry(false, 5, -1) == 5, "nothing chosen in the grid: where the cursor rests");
}

static void test_ticks() {
    std::printf("Random Stage Settings: each entry its own tick\n");
    const std::vector<std::string> keys = { "", "tA", "tB" };   // Training Room's own entry and two tiles
    std::set<std::string> off;
    check(is_ticked(keys, off, true, "") && is_ticked(keys, off, true, "tB"), "nothing stored, in the game's pool: all ticked");
    check(!is_ticked(keys, off, false, "tA"), "out of the game's pool: none ticked");
    auto t = toggle_tick(keys, off, true, "tA");
    check(!t.game_toggles && t.in_pool_after && t.off == std::set<std::string>{ "tA" }, "untick one tile: only it, the game's id stays");
    check(is_ticked(keys, t.off, true, "") && !is_ticked(keys, t.off, true, "tA") && is_ticked(keys, t.off, true, "tB"),
          "the others keep their ticks");
    t = toggle_tick(keys, t.off, true, "");
    t = toggle_tick(keys, t.off, true, "tB");
    check(t.game_toggles && !t.in_pool_after, "the last ticked one unticked: the game takes its id out (its own Decide runs)");
    t = toggle_tick(keys, t.off, false, "tB");
    check(t.game_toggles && t.in_pool_after && t.off == std::set<std::string>{ "", "tA" }, "out of the pool, one ticked: only it, the id back in");
    check(is_ticked(keys, { "", "tA", "tB" }, true, "tA"), "every entry unticked but the id in the pool (changed elsewhere): all ticked");
    std::set<std::string> one = { "", "tA" };
    check(pick_ticked(keys, one, 0) == "tB" && pick_ticked(keys, one, 7) == "tB", "the random's pick: the one ticked entry");
    std::set<std::string> two = { "tA" };
    check(pick_ticked(keys, two, 0) == "" && pick_ticked(keys, two, 1) == "tB", "two ticked: either");
    // state.json keeps them per menu, the loader still reads "selected" only
    State s;
    s.menu[kBattle][0] = "tB";
    s.active = s.menu[kBattle];
    s.off[kBattle][0] = { "", "tA" };
    s.random[kBattle] = true;
    const std::string js = state_json(s);
    State back;
    check(parse_state(js, back) && back.off[kBattle][0] == s.off[kBattle][0] && back.random[kBattle] && !back.random[kTraining] &&
          back.off[kTraining].empty(), "ticks and Random per menu: round trip");
    auto served = loader_parse(js);
    check(served.size() == 1 && served[0].second == "tB", "the loader's scan still reads the served selection only");

    // the Y key: tick all, or untick all but the entry under the cursor
    std::printf("Random Stage Settings: tick all / untick all\n");
    const std::map<uint32_t, std::vector<std::string>> kof = { { 0, keys } };   // stage 0 has tiles tA, tB; 10, 20 none
    const std::vector<TickEntry> es = { { 0, "" }, { 0, "tA" }, { 0, "tB" }, { 10, "" }, { 20, "" } };
    Off o0;
    o0[0] = { "tA" };
    o0[5] = { "x" };                                                // a stage the grid does not show: left as it is
    auto a = tick_all(es, kof, { 20, 99 }, o0, 3);
    check(a.tick && a.disabled == std::vector<int32_t>{ 99 } && !a.off.count(0) && a.off.count(5), "one unticked: all ticked, other ids kept");
    check(all_ticked(es, kof, a.disabled, a.off), "then every entry shows ticked");
    auto b = tick_all(es, kof, a.disabled, a.off, 3);
    check(!b.tick && b.disabled == std::vector<int32_t>{ 99, 0, 20 } && !b.off.count(0), "all ticked: all out but the focused stage 10");
    check(entry_ticked({ 10, "" }, kof, b.disabled, b.off) && !entry_ticked({ 0, "tA" }, kof, b.disabled, b.off) &&
          !entry_ticked({ 20, "" }, kof, b.disabled, b.off), "only the focused entry stays ticked");
    auto c = tick_all(es, kof, a.disabled, a.off, 1);
    check(!c.tick && c.disabled == std::vector<int32_t>{ 99, 10, 20 } && c.off.at(0) == std::set<std::string>({ "", "tB" }),
          "the focus on a tile: its stage stays in the pool, only that tile ticked");
    check(entry_ticked({ 0, "tA" }, kof, c.disabled, c.off) && !entry_ticked({ 0, "" }, kof, c.disabled, c.off) &&
          !entry_ticked({ 0, "tB" }, kof, c.disabled, c.off), "the tile alone ticked among its stage's entries");
    auto d = tick_all(es, kof, a.disabled, a.off, -1);
    check(!d.tick && d.disabled == std::vector<int32_t>{ 99, 10, 20 } && d.off.at(0) == std::set<std::string>({ "tA", "tB" }),
          "no focus: the first entry kept");
    check(tick_all(es, kof, c.disabled, c.off, 1).tick, "after untick all, Y ticks all again");
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

static void test_classic_list() {
    std::printf("classic Stage Select's list\n");
    const std::vector<NamedTile> tiles = {
        { 0, "a1", "Forest (empty, fixed)" }, { 0, "a2", "Forest with people" }, { 0, "d", "Harbour at Night" },
        { 0, "t", "Castle" }, { 70000, "a3", "Forest Temple" }, { 0, "x1", "SF Alpha" }, { 0, "x2", "SF Zero" },
    };
    const auto g = tile_groups(tiles);
    check(g.size() == 6, "tiles sharing a first word on one stage are one entry");
    check(g[0].keys == std::vector<std::string>({ "a1", "a2" }), "its looks in registry order, the first one's entry");
    check(g[3].base == 70000 && g[3].keys.size() == 1, "the same word over another stage is another entry");
    check(g[4].keys.size() == 1 && g[5].keys.size() == 1, "a word under 4 letters groups nothing");
    check(first_word("  (New) Stage") == "new", "first word: punctuation before it skipped");

    check(!more_arrows(0, 21, 21).left && !more_arrows(0, 21, 21).right, "no extra entry: no arrow");
    check(!more_arrows(0, 21, 29).left && more_arrows(0, 21, 29).right, "list at its start: right arrow only");
    check(more_arrows(3, 21, 29).left && more_arrows(3, 21, 29).right, "in the middle: both");
    check(more_arrows(8, 21, 29).left && !more_arrows(8, 21, 29).right, "at its end: left arrow only");
    check(more_arrows(10, 21, 29).left && more_arrows(10, 21, 29).right, "wrapped round: both");

    auto tile = [](uint32_t, const std::string& k) { return k == "a1"; };
    const Choice own = { { 0, "a1" }, { 70000, "banner" } };
    check(classic_served(own, 0, "red", tile) == Choice({ { 0, "red" }, { 70000, "banner" } }), "the focused entry's look is served for its stage");
    check(classic_served(own, 0, "", tile) == Choice({ { 70000, "banner" } }), "its own look: nothing served, a tile left by an older version dropped");

    check(view_cursor(4, 21) == 4 && view_cursor(20, 21) == 20, "an entry on screen: the cursor on its own cell");
    check(view_cursor(22, 21) == 20 && view_cursor(27, 21) == 20, "past the cells: the last cell (the list scrolled to it)");
    const std::vector<int> looks = { 1, 3, 1 };
    check(random_pick(looks, 4, 2) == std::make_pair(1, 2) && random_pick(looks, 2, 7) == std::make_pair(2, 0), "Random: an entry, then one of its looks");
    check(random_pick({}, 1, 1).first == -1, "Random on an empty list: nothing");
    int seen[3] = {};
    for (uint32_t i = 0; i < 300; ++i) ++seen[random_pick(looks, i, i / 3).first];
    check(seen[0] == 100 && seen[1] == 100 && seen[2] == 100, "Random: every entry as likely, whatever its looks");

    State st;
    st.active[0] = "a2";
    st.tile_pick["a1"] = "a2";
    const std::string js = state_json(st);
    State back;
    check(parse_state(js, back) && back.tile_pick == st.tile_pick, "a tile's chosen look comes back from state.json");
    check(loader_parse(js).size() == 1 && loader_parse(js)[0].second == "a2", "the loader still reads only \"selected\"");
}

int main() {
    test_classic_list();
    test_state();
    test_grid();
    test_cycle_keys();
    test_chosen_mark();
    test_banner_name();
    test_vs_stage();
    test_preview();
    test_ticks();
    test_codecheck();
    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
