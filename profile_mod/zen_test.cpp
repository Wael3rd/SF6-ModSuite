// Zen Mode's pure part (zen_logic.hpp): the table, the pattern items, the walk against ZenMode.lua's own, the file, the schedule.
#include "zen_logic.hpp"

#include <cstdio>
#include <memory>
#include <set>
#include <utility>

using namespace zen;
static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-74s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

// ---- a made-up control tree ----
struct Node {
    std::string name;
    Node* first = nullptr;
    Node* next = nullptr;
};
struct Forest {
    std::vector<std::unique_ptr<Node>> all;
    Node* make(Node* parent, const std::string& name) {
        all.push_back(std::make_unique<Node>());
        Node* n = all.back().get();
        n->name = name;
        if (parent) {
            if (!parent->first) parent->first = n;
            else { Node* l = parent->first; while (l->next) l = l->next; l->next = n; }
        }
        return n;
    }
    Node* child_named(Node* parent, const std::string& name) {
        for (Node* c = parent->first; c; c = c->next) if (c->name == name) return c;
        return make(parent, name);
    }
};
struct FakeTree {
    int names_read = 0;
    Node* child(Node* n) { return n->first; }
    Node* next(Node* n) { return n->next; }
    bool name(Node* n, char* buf, size_t cap) { ++names_read; snprintf(buf, cap, "%s", n->name.c_str()); return true; }
};
// ZenMode.lua's hide_path, as it is written there: one rule at a time
static void lua_hide_path(Node* control, const Rule& r, int depth, std::set<std::pair<Node*, int>>& out, int rule) {
    if (depth >= r.n) { out.insert({ control, rule }); return; }
    for (Node* c = control->first; c; c = c->next)
        if (seg_match(c->name.c_str(), r.path[depth])) lua_hide_path(c, r, depth + 1, out, rule);
}
// A name a segment matches: its text, each %d+ and %d given digits, and something around it
static std::string name_for(const char* seg, int variant) {
    std::string s;
    for (const char* p = seg; *p;) {
        if (p[0] == '%' && p[1] == 'd') {
            if (p[2] == '+') { s += variant % 2 ? "12" : "7"; p += 3; }
            else { s += static_cast<char>('0' + variant % 10); p += 2; }
        } else s += *p++;
    }
    if (variant % 3 == 1) s += "tail";
    if (variant % 3 == 2) s = "x_" + s;
    return s;
}
// A tree for an owner: each path there once or more, with controls that match nothing beside them
static Node* tree_for(Forest& f, const Owner& o, int copies) {
    Node* view = f.make(nullptr, "view");
    f.make(view, "c_other");
    for (int r = 0; r < o.n; ++r)
        for (int k = 0; k < copies; ++k) {
            Node* at = view;
            for (int d = 0; d < o.rules[r].n; ++d) {
                // the first segments shared (as in the game), the varied names further down
                at = f.child_named(at, name_for(o.rules[r].path[d], d < 2 ? 0 : k));
                f.make(at, "e_decoy");
            }
        }
    return view;
}

int main() {
    // ---- the pattern items ----
    check(seg_match("c_main", "c_main"), "plain text: the same name");
    check(seg_match("p_FightersProfileBanner_0", "p_FightersProfileBanner_"), "plain text: not anchored at the end");
    check(seg_match("xp_SmallRankIcon_3", "p_SmallRankIcon_"), "plain text: not anchored at the start");
    check(!seg_match("c_mai", "c_main"), "plain text: shorter: no");
    check(!seg_match("C_MAIN", "c_main"), "plain text: case matters");
    check(seg_match("item0", "item%d") && seg_match("item12", "item%d"), "%d: a digit, more may follow");
    check(!seg_match("item", "item%d") && !seg_match("itemA", "item%d"), "%d: no digit: no");
    check(seg_match("item12", "item%d+") && seg_match("item7", "item%d+"), "%d+: one digit or more");
    check(!seg_match("item", "item%d+") && !seg_match("item_1", "item%d+"), "%d+: none right there: no");
    check(seg_match("p_VsInfoTop_player1_v", "p_VsInfoTop_player%d_v"), "%d in the middle");
    check(!seg_match("p_VsInfoTop_player12_v", "p_VsInfoTop_player%d_v"), "%d in the middle: two digits: no");
    check(!seg_match("p_VsInfoTop_player_v", "p_VsInfoTop_player%d_v"), "%d in the middle: no digit: no");
    check(seg_match("p_ReplayBannerPlayerInfoShort_2P", "p_ReplayBannerPlayerInfoShort_%dP"), "%d before a letter");
    check(seg_match("p_BattleFighterInfo_1p_", "p_BattleFighterInfo_%dp_"), "%d between texts");
    check(seg_match("a123b", "a%d+b") && seg_match("a123b", "a%d+3b") && !seg_match("a123b", "a%d+4b"), "%d+ gives digits back");
    check(seg_match("c_item_0", "c_item_%d") && seg_match("zzc_item_9z", "c_item_%d"), "%d: found anywhere in the name");
    check(seg_match("", "") && seg_match("abc", "") && !seg_match("", "a"), "the empty cases");

    // ---- the table ----
    check(kCatCount == 13 && kOwnerCount == 32 && kRuleCount == 89, "13 categories, 32 owners, 89 rules");
    {
        bool sorted = true, found = true, cats = true, depth = true, masks = true;
        int rules = 0;
        for (int i = 0; i < kOwnerCount; ++i) {
            if (i && strcmp(kOwners[i - 1].name, kOwners[i].name) >= 0) sorted = false;
            if (owner_index(kOwners[i].name) != i) found = false;
            if (kOwners[i].n < 1 || kOwners[i].n > 32) masks = false;
            for (int r = 0; r < kOwners[i].n; ++r, ++rules) {
                if (kOwners[i].rules[r].cat >= kCatCount) cats = false;
                if (kOwners[i].rules[r].n < 1 || strcmp(kOwners[i].rules[r].path[0], "c_main") != 0) depth = false;
            }
        }
        check(sorted && found, "the owners sorted by name, each found by its name");
        check(rules == kRuleCount && cats && masks, "every rule has a category; no owner has more than 32");
        check(depth, "every path starts at c_main");
        check(owner_index("BattleHud") < 0 && owner_index("") < 0 && owner_index("zzz") < 0 && owner_index("AAA") < 0, "another name: no owner");
    }
    {
        const Owner& vs = kOwners[owner_index("VSInfoOffline")];
        check(vs.n == 9 && vs.rules[3].cat == cat_index("National flag") && vs.rules[4].cat == cat_index("Player name"), "VSInfoOffline: nine rules, the flag, the name");
        check(vs.rules[0].cat == cat_index("Rank icon") && vs.rules[1].cat == cat_index("Rank-up chance") && vs.rules[2].cat == cat_index("Win streak"), "VSInfoOffline: rank icon, chance, streak");
        check(vs.rules[5].cat == cat_index("Player title") && vs.rules[6].cat == cat_index("Team / club") && vs.rules[7].cat == cat_index("Platform icon") && vs.rules[8].cat == cat_index("Control type"), "VSInfoOffline: title, club, platform, control type");
        check(kOwners[owner_index("BattleHud_WinCount")].rules[0].cat == cat_index("Win streak"), "BattleHud_WinCount: win streak, by its name");
        check(kOwners[owner_index("RankGauge_2P")].rules[0].cat == cat_index("Rank gauge") && kOwners[owner_index("RankUp")].rules[0].cat == cat_index("Rank-up animation"), "the rank gauge and the rank-up animation, by their names");
        check(cat_index("c_rank") < 0 && cat_index("c_RankInfo") < 0 && cat_index("c_CircleInfo") < 0, "no category made of a container's name");
        const Owner& ob = kOwners[owner_index("OnlineBannerUI")];
        check(ob.n == 2 && ob.rules[0].cat == cat_index("Rank icon") && ob.rules[1].cat == cat_index("League points"), "the main menu's banner: the icon and the points apart");
        const Owner& ms = kOwners[owner_index("MatchingSetting")];
        check(ms.n == 4 && ms.rules[0].cat == cat_index("Rank icon") && ms.rules[1].cat == cat_index("Rank icon") && ms.rules[2].cat == cat_index("League points") && ms.rules[3].cat == cat_index("Master rate"), "Battle Settings: the icon, the placement matches, LP and MR apart (cat = in the rules)");
        const Owner& ai = kOwners[owner_index("BattleHud_AccountInfo")];
        bool icons = ai.n == 8;
        for (int r = 0; icons && r < 6; ++r) icons = ai.rules[r].cat == cat_index("Rank icon");
        for (int r = 0; icons && r < 5; ++r) icons = ai.rules[r].n == 7 && !strcmp(ai.rules[r].path[4], "c_rank");
        check(icons && ai.rules[6].cat == cat_index("League points") && ai.rules[7].cat == cat_index("Team / club"), "the fight's HUD: the icon (what is inside c_rank's panels, never c_rank), the points, the club");
        const Owner& wn = kOwners[owner_index("BattleHud_MatchWonNumber")];
        check(wn.n == 1 && wn.rules[0].n == 5 && !strcmp(wn.rules[0].path[4], "e_") && wn.rules[0].cat == cat_index("Player title") && !whole_owner(wn), "the fight's HUD: the title under the round counter, by its elements");
        const Owner& rl = kOwners[owner_index("CFNReplayChildList")];
        check(rl.rules[0].cat == cat_index("League points") && rl.rules[1].cat == cat_index("Rank icon") && rl.rules[3].cat == cat_index("Rank icon"), "the replay rows: c_league is the rank icon");
        int whole = 0;
        for (int i = 0; i < kOwnerCount; ++i) whole += whole_owner(kOwners[i]) ? 1 : 0;
        check(whole == 7 && whole_owner(kOwners[owner_index("BattleHud_InputTypeIcon")]) && whole_owner(kOwners[owner_index("RankGauge_1P")]) && whole_owner(kOwners[owner_index("RankUp")])
              && !whole_owner(ai) && !whole_owner(kOwners[owner_index("ResultCounter")]), "the seven elements hidden as a whole");
        check(kOwners[owner_index("CFNFighterProfileChildPlayTab3")].rules[1].cat == cat_index("Master rate"), "the Lua's own oddity kept: the LP of the character list counted as Master rate");
    }
    {
        bool one = true;
        int in_groups = 0;
        for (int c = 0; c < kCatCount; ++c) if (group_of(c) < 0) one = false;
        for (int g = 0; g < kGroupCount; ++g)
            for (const char* const* c = kGroupCats[g]; *c; ++c) { ++in_groups; if (cat_index(*c) < 0 || group_of(cat_index(*c)) != g) one = false; }
        check(one && in_groups == kCatCount, "every category in one group, every group's names are categories");
    }

    // ---- the walk gives what the Lua's gives ----
    {
        bool same = true, fewer = true;
        size_t targets = 0;
        for (int copies = 1; copies <= 3; ++copies)
            for (int i = 0; i < kOwnerCount; ++i) {
                Forest f;
                Node* view = tree_for(f, kOwners[i], copies);
                std::set<std::pair<Node*, int>> want, got;
                for (int r = 0; r < kOwners[i].n; ++r) lua_hide_path(view, kOwners[i].rules[r], 0, want, r);
                FakeTree t;
                int budget = 100000;
                auto sink = [&](Node* n, int r) { got.insert({ n, r }); };
                walk(t, view, kOwners[i], all_rules(kOwners[i]), 0, sink, budget);
                if (want != got || want.size() < kOwners[i].n) same = false;
                if (t.names_read > static_cast<int>(f.all.size())) fewer = false;
                targets += got.size();
            }
        check(same, "every owner, one to three copies of each path: the same controls as hide_path");
        check(fewer, "a control's name is read once at most");
        check(targets > 300, "enough controls in those trees");
    }
    {
        Forest f;
        Node* view = f.make(nullptr, "view");
        Node* main = f.make(view, "c_main");
        Node* list = f.make(f.make(f.make(main, "p_Group_base_h"), "p_Group_communication_v"), "p_ScrollList_member_v");
        Node* a = f.make(f.make(list, "item0"), "p_LargeRankIcon_0");
        Node* b = f.make(f.make(list, "item12"), "p_LargeRankIcon_0");
        Node* c = f.make(f.make(list, "p_CustomRoomMemberBanner_"), "p_LargeRankIcon_0");
        f.make(f.make(list, "itemX"), "p_LargeRankIcon_0");
        f.make(f.make(list, "item3"), "p_SmallRankIcon_0");
        const Owner& o = kOwners[owner_index("CustomRoomRoom")];
        std::set<Node*> got;
        FakeTree t;
        int budget = 1000;
        auto sink = [&](Node* n, int) { got.insert(n); };
        walk(t, view, o, all_rules(o), 0, sink, budget);
        check(got == std::set<Node*>{ a, b, c }, "a custom room's member rows: every row's rank icon, nothing else");
        got.clear();
        budget = 3;
        walk(t, view, o, all_rules(o), 0, sink, budget);
        check(got.empty() && budget == 0, "a budget: the walk stops");
        const Owner& hud = kOwners[owner_index("BattleHud_WinCount")];
        Forest g;
        Node* v2 = g.make(nullptr, "view");
        Node* m1 = g.make(v2, "c_main");
        g.make(m1, "c_inner");
        got.clear();
        budget = 1000;
        walk(t, v2, hud, all_rules(hud), 0, sink, budget);
        check(got == std::set<Node*>{ m1 }, "a whole element: its c_main");
    }

    // ---- the settings ----
    {
        State s;
        bool all = true;
        for (bool h : s.hidden) all = all && h;
        check(!s.enabled && all, "at first: off, every category hidden once on");
        check(!wants_hidden(s, 0), "off: nothing is to be hidden");
        s.enabled = true;
        check(wants_hidden(s, 0) && wants_hidden(s, kCatCount - 1) && !wants_hidden(s, kCatCount) && !wants_hidden(s, -1), "on: every category of the table");
        check(group_value(s, kRanks) == GroupValue::Hidden, "a group: Hidden");
        step_group(s, kRanks);
        check(group_value(s, kRanks) == GroupValue::Shown && !s.hidden[cat_index("Rank icon")] && !s.hidden[cat_index("Rank-up chance")] && s.hidden[cat_index("League points")], "a step: the whole group Shown, the others as they were");
        check(!wants_hidden(s, cat_index("Rank gauge")) && wants_hidden(s, cat_index("Win streak")), "what is hidden follows");
        step_group(s, kRanks);
        check(group_value(s, kRanks) == GroupValue::Hidden, "another step: Hidden again");
        s.hidden[cat_index("Player title")] = false;
        check(group_value(s, kNames) == GroupValue::Mixed, "a group whose categories differ: Mixed");
        step_group(s, kNames);
        check(group_value(s, kNames) == GroupValue::Hidden, "a step on Mixed: Hidden");
    }
    {
        State s;
        s.enabled = true;
        s.hidden[cat_index("National flag")] = false;
        s.hidden[cat_index("Team / club")] = false;
        State back;
        bool had = false;
        check(parse_state(state_json(s), back, &had) && back == s && had, "the file written is read back the same");
        State d;
        check(parse_state(state_json(State()), d, &had) && d == State() && had, "the first settings too");
        State t;
        check(parse_state("\xEF\xBB\xBF{ \"other\": [1, {\"a\": \"}\"}], \"hidden\": { \"Win streak\": false, \"Not a category\": false }, \"enabled\": true, \"x\": null }", t, &had)
              && t.enabled && !t.hidden[cat_index("Win streak")] && t.hidden[cat_index("Rank icon")] && had, "a BOM, other keys, an unknown category: read");
        State u;
        u.enabled = true;
        check(parse_state("{}", u, &had) && u.enabled && !had, "an empty object: nothing changed, no `enabled` seen");
        State v;
        v.hidden[0] = false;
        State before = v;
        check(!parse_state("", v) && !parse_state("not json", v) && !parse_state("{\"hidden\": {\"Win streak\": fal", v) && !parse_state("[true]", v) && v == before, "not such a file: refused, the settings untouched");
        State w;
        check(parse_state("{\"hidden\": {\"Win streak\": null, \"Rank icon\": 0, \"Rank gauge\": \"no\", \"c_rank\": false}}", w) && w.hidden[cat_index("Win streak")] && w.hidden[cat_index("Rank icon")] && w.hidden[cat_index("Rank gauge")], "as Lua reads it: null is missing, 0 and a text are true");
    }
    {
        State a = first_state(false, "");
        check(a == State(), "the first start, no Lua file: off");
        State b = first_state(true, "{\"hidden\":{\"Player name\":false,\"Rank icon\":true}}");
        check(b.enabled && !b.hidden[cat_index("Player name")] && b.hidden[cat_index("Rank icon")] && b.hidden[cat_index("Win streak")], "the Lua's file: on, its categories kept, a missing one hidden");
        State c = first_state(true, "garbage");
        check(c == State(), "the Lua's file unreadable: as if there were none");
    }

    // ---- the schedule ----
    {
        check(battle_owner("BattleHud_WinCount") && battle_owner("RankGauge_1P") && !battle_owner("VSInfoOffline") && !battle_owner("RankUp") && !battle_owner("ResultCounter"), "the fight's owners: the HUD and the rank gauges");
        Sched s;
        check(s.dormant, "never seen: nothing due");
        sched_appear(s, 100);
        check(!s.dormant && s.due == 100, "seen: due at once");
        std::vector<uint32_t> at;
        for (uint32_t f = 100; f < 1100; ++f)
            if (s.due <= f) { at.push_back(f); sched_applied(s, f, true, false, false, true); }
        check(at == std::vector<uint32_t>{ 100, 102, 110, 130, 190 } && s.due == kNever, "the fight itself: what appears is looked at 0, 2, 10, 30, 90 frames after, then never");
        uint32_t applied = 0;
        sched_appear(s, 2000);
        for (uint32_t f = 2000; f < 2000 + 600; ++f)
            if (s.due <= f) { ++applied; sched_applied(s, f, true, false); }
        check(applied == 600 && s.due == 2600, "around the fight: the HUD looked at every frame");
        sched_phase(s, 7000, true);
        sched_applied(s, 7000, true, true, false, true);
        check(s.due == 7002, "the fight itself: something came back: it settles again");
        uint32_t again = 0;
        for (uint32_t f = 7001; f < 7001 + 36000; ++f)
            if (s.due <= f) { ++again; sched_applied(s, f, true, true, false, true); }
        check(again == 4 && s.due == kNever, "the fight itself: what the game undoes each time is given up after 4 more looks");
        sched_appear(s, 100);
        sched_applied(s, 104, true, false, false, true);
        check(s.due == 110, "the fight itself: a late look skips what is past");
        sched_gone(s);
        sched_now(s, 500);
        check(s.dormant && s.due == 110, "gone: a settings change waits for it to be seen again");
        sched_appear(s, 600);
        sched_applied(s, 600, true, false, false, true);
        sched_applied(s, 602, true, false, false, true);
        sched_now(s, 605);
        check(s.due == 605, "on screen: a settings change is applied at once");
    }
    {
        check(quiet_phase(phase_of(kFightNow, false)) && !quiet_phase(phase_of(kFightNow, true)) && !quiet_phase(phase_of(3, false)) && !quiet_phase(phase_of(5, false))
              && !quiet_phase(kNoFight) && phase_of(0, false) != kNoFight && phase_of(4, true) != phase_of(4, false), "the fight itself: its state NOW, not paused, its HUD drawn");
        Sched s;
        sched_appear(s, 100);
        uint32_t looks = 0;
        for (uint32_t f = 100; f < 100 + 36000; ++f)
            if (s.due <= f) { ++looks; sched_applied(s, f, true, false, false, true); }
        check(looks == 5 && s.due == kNever, "the fight itself: what appears settles, then nothing more for ten minutes");
        sched_phase(s, 50000, false);
        looks = 0;
        for (uint32_t f = 50000; f < 50100; ++f)
            if (s.due <= f) { ++looks; sched_applied(s, f, true, false, false, false); }
        check(looks == 100 && s.due == 50100, "a round ends: the HUD is looked at every frame until the next one starts");
        sched_phase(s, 50100, true);
        looks = 0;
        for (uint32_t f = 50100; f < 50100 + 36000; ++f)
            if (s.due <= f) { ++looks; sched_applied(s, f, true, false, false, true); }
        check(looks == 1 && s.due == kNever, "the fight starts: one look, then nothing");
        sched_phase(s, 90000, true);
        sched_applied(s, 90000, true, true, false, true);
        check(s.due == 90002, "the fight starts and something had come back: it settles again");
        Sched m;
        sched_appear(m, 10);
        for (uint32_t f = 10; f < 400; ++f)
            if (m.due <= f) sched_applied(m, f, false, false, false, false);
        sched_phase(m, 400, true);
        check(m.due == 400, "a menu's owner drawn under the fight: one look when the fight starts");
        sched_applied(m, 400, false, false, false, true);
        check(m.due == kNever, "and nothing after");
        sched_phase(m, 900, false);
        sched_applied(m, 900, false, false, false, false);
        check(m.due == 901 && m.dense_until == 960, "the fight ends or pauses: a menu's owner is looked at as in a menu again");
        Sched w;
        sched_appear(w, 5);
        sched_applied(w, 5, true, true, true, false);
        check(w.due == kNever, "a whole element: one look when it appears");
        sched_now(w, 77);
        check(w.due == 77, "a whole element: the settings wake it");
        Sched g;
        sched_phase(g, 5, false);
        check(g.dormant && g.due == 0, "not on screen: a phase changes nothing");
    }
    {
        Sched s;
        sched_appear(s, 1000);
        bool all = true;
        for (uint32_t f = 1000; f < 1060; ++f) all = all && walk_due(s, f, f - 1);
        check(all, "an owner just there: its whole tree walked at each look");
        check(!walk_due(s, 1061, 1060) && !walk_due(s, 1063, 1060) && walk_due(s, 1064, 1060), "an owner the game writes over every frame: its tree walked every 4 frames");
        check(walk_due(s, 5000, 0), "the settings changed: walked");
    }
    {
        Sched s;
        sched_appear(s, 10);
        uint32_t dense = 0, slow = 0;
        for (uint32_t f = 10; f < 10 + 300; ++f)
            if (s.due <= f) { (f < 70 ? dense : slow)++; sched_applied(s, f, false, false); }
        check(dense == 60 && slow == 60, "a menu: every frame for 60 frames, then every 4");
        sched_applied(s, 1000, false, false);
        check(s.due == 1004, "a menu, settled: 4 frames later");
        sched_applied(s, 1004, false, true);
        check(s.due == 1005 && s.dense_until == 1064, "a menu: something had to be hidden: every frame again for 60");
    }

    // ---- the words ----
    {
        bool full = true;
        for (const Words& w : kWords) {
            for (const char* t : { w.lang, w.tab, w.on, w.off, w.hidden, w.shown, w.mixed, w.help_master }) if (!t || !*t) full = false;
            for (int g = 0; g < kGroupCount; ++g) if (!w.group[g] || !*w.group[g] || !w.help_group[g] || !*w.help_group[g]) full = false;
        }
        check(full, "every language has every word");
        check(!strcmp(words_of("fr").tab, "Mode Zen") && !strcmp(words_of("xx").tab, "Zen Mode"), "French; an unknown language: English");
        bool all = true;
        for (const char* l : { "en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar" }) all = all && !strcmp(words_of(l).lang, l);
        check(all, "the game's fourteen languages");
    }

    {
        auto same = [](Act a, bool write, bool value, Mark mark) { return a.write == write && (!write || a.value == value) && a.mark == mark; };
        // Zen Mode off, or the group on Shown, since the screen came: nothing is written, whatever the game did
        check(same(decide(Mark::None, false, false, false), false, false, Mark::None), "shown by the game, not to hide: left alone");
        check(same(decide(Mark::None, true, false, false), false, false, Mark::Game), "hidden by the game (the rank-up notice of who has none), not to hide: left hidden");
        check(same(decide(Mark::Game, true, false, false), false, false, Mark::Game), "still the game's at the next look");
        // to hide
        check(same(decide(Mark::None, false, true, false), true, true, Mark::Ours), "shown, to hide: hidden, ours");
        check(same(decide(Mark::None, true, true, false), false, true, Mark::Game), "to hide, found hidden: the game's, nothing written");
        check(same(decide(Mark::Ours, true, true, false), false, true, Mark::Ours), "ours, still hidden: nothing written");
        check(same(decide(Mark::Ours, false, true, false), true, true, Mark::Ours), "ours, put back by the game (the custom room): hidden again");
        check(same(decide(Mark::Game, false, true, false), true, true, Mark::Ours), "the game showed what it hid: hidden, ours from now on");
        // no longer to hide: Off, or the group on Shown
        check(same(decide(Mark::Ours, true, false, false), true, false, Mark::None), "ours, no longer to hide: shown again");
        check(same(decide(Mark::Ours, false, false, false), false, false, Mark::None), "ours, already put back by the game: forgotten");
        check(same(decide(Mark::Game, true, false, true), false, false, Mark::Game), "the game's stays the game's, whatever its rule");
        // a list's new row: a copy of a row we hid
        check(same(decide(Mark::None, true, true, true), false, true, Mark::Ours), "a copy of ours, to hide: ours, nothing written");
        check(same(decide(Mark::None, true, false, true), true, false, Mark::None), "a copy of ours, no longer to hide: shown");
        check(copy_rule(0b0100, 0, 2) && !copy_rule(0b0100, 0, 1), "a copy: only under a rule with controls of ours");
        check(!copy_rule(0b0100, 0b0100, 2), "a rule where the game hides too: found hidden is the game's");
        check(!copy_rule(0xFFFFFFFFu, 0, 32) && !copy_rule(0xFFFFFFFFu, 0, -1), "a rule out of the bits: no copy");
        // the versus screen seen on 11/11: the notice of player 1 (a chance) shown by the game, of player 2 (none) hidden by it
        Mark p1 = Mark::None, p2 = Mark::None;
        Act a1 = decide(p1, false, true, copy_rule(0, 0, 1)), a2 = decide(p2, true, true, copy_rule(0, 0, 1));
        p1 = a1.mark; p2 = a2.mark;
        check(a1.write && a1.value && !a2.write && p1 == Mark::Ours && p2 == Mark::Game, "Zen Mode on: player 1's notice hidden, player 2's the game's");
        a1 = decide(p1, true, false, copy_rule(0b10, 0b10, 1)); a2 = decide(p2, true, false, copy_rule(0b10, 0b10, 1));
        check(a1.write && !a1.value && !a2.write && a2.mark == Mark::Game, "Zen Mode off: player 1's notice back, player 2's still hidden");
    }

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
