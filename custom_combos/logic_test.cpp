// logic_test.cpp - offline checks of Custom Combos' file and text logic (cc_labels.hpp, cc_trials.hpp, cc_rec.hpp, cc_take.hpp).
// No game, no data folder: every input is written here. build_logic_test.bat builds it into build\custom_combos\.
#include "cc_rec.hpp"
#include "cc_take.hpp"

#include <cstdio>

using namespace cc;

static int g_fail = 0, g_n = 0;
static void check(bool ok, const char* what) {
    ++g_n;
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); }
}
static void eq(const std::string& got, const std::string& want, const char* what) {
    ++g_n;
    if (got != want) { ++g_fail; std::printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str()); }
}

static TrialP trial(const char* text, Labels& L) {
    bool del = false;
    return trial_from(json::parse(text), "x/Test file.json", 1, L, &del);
}

int main() {
    // notation -> the game's tag text
    eq(text::note_text("2+MK"), "<ICON 2><ICON +><ICON mk>M", "note 2+MK");
    eq(text::note_text("236+LP"), "<CMD _236><ICON +><ICON lp>L", "note 236+LP");
    eq(text::note_text("> HP"), "<ICON next><ICON hp>H", "note target combo");
    eq(text::note_text("j.HK"), "<ICON 8><ICON hk>H", "note jump");
    eq(text::note_text("66"), "<ICON 6><ICON 6>", "note dash");
    eq(text::note_text("DRC"), "<ICON 6><ICON 6>", "note drive rush");
    eq(text::note_text("4+THROW"), "<ICON 4><ICON +><ICON lp>L<ICON lk>L", "note back throw");
    eq(text::note_text("weird"), "weird", "note unknown");
    eq(text::one_way("<ICON 5> or <ICON 6><ICON +>X"), "<ICON 5><ICON +>X", "one way");
    eq(text::one_way("<ICON 6><ICON 6> or (<ICON 4>)+X"), "<ICON 6><ICON 6>", "one way, a group after the or");
    eq(text::drop_notes("<CMD _236><ICON +><CMDBTN CmnK> (hold for a longer range)"), "<CMD _236><ICON +><CMDBTN CmnK>", "notes dropped");
    eq(text::drop_notes("(<ICON 5> or <ICON 6>)<ICON +>"), "(<ICON 5> or <ICON 6>)<ICON +>", "grouped inputs kept");

    Labels L;
    L.skills["1010110"] = { "<CMD _236><ICON +><CMDBTN CmnP>", "<CMD _236><ICON +><CMDBTN SM>" };
    L.skills["120"] = { "<CMDBTN LowP>", "" };
    L.groups = json::parse(R"({ "236+P": [900, 901], "2+MK": [640] })");
    eq(step_text(L, "236+LP", 1010110, 1, false), "<CMD _236><ICON +><ICON lp>L", "skill, light");
    eq(step_text(L, "236+PP", 1010110, 7, false), "<CMD _236><ICON +><ICON p><ICON p>", "skill, OD");
    eq(step_text(L, "236+P", 1010110, 0, true), "<CMD _236><ICON +><ICON sm>", "skill, Modern");
    eq(step_text(L, "236+P", 1010110, 7, true), "<CMD _236><ICON +><ICON auto><ICON hd><ICON +><ICON sm>", "skill, Modern OD");
    check(has_cmd(L, 120, false) && !has_cmd(L, 120, true) && has_cmd(L, 999, true), "has_cmd");
    eq(cmd_key("<CMD _236><ICON +><CMDBTN CmnK>"), "236+K", "cmd_key motion");
    eq(cmd_key("<ICON 4><ICON +><CMDBTN HighP> (note)"), "4+HP", "cmd_key level");

    // a trial file
    TrialP t = trial(R"({ "title": "REC 12/11 11:34 - 3731 dmg", "level": 1, "order": 2, "control": "modern",
        "steps": [ { "notation": "2+MK", "check_type": 0 }, { "notation": "236+P", "action_ids": ["905"], "skill_id": 1010110 } ],
        "recorded": { "signature": "M:2+MK/0>236+P/0", "damage": 3731.6 } })", L);
    check(t && t->steps.size() == 2, "trial read");
    check(t->steps[0].ids == std::vector<int64_t>{ 640 } && t->steps[0].action_id == 640, "ids from the notation's group");
    check(t->steps[1].ids == std::vector<int64_t>{ 905 }, "explicit ids win (a string number counts)");
    eq(t->steps[0].text, "<ICON 2><ICON +><ICON mk>M", "a line no skill names gets its command");
    check(t->steps[1].text.empty(), "a skill line keeps the game's text");
    eq(signature(*t), "M:2+MK/0>236+P/0", "signature");
    eq(shown_title(*t), "REC 12/11 11:34 (3731)", "title: old damage out, the take's in");
    eq(row_title(*t, false), "[Modern] REC 12/11 11:34 (3731)", "title in a Classic list");
    check(t->level == 1 && t->order == 2 && t->modern, "level, order, control");
    TrialP edited = trial(R"({ "title": "T", "steps": [ { "notation": "5LP" } ], "recorded": { "signature": "x", "damage": 9 } })", L);
    eq(shown_title(*edited), "T", "an edited recipe shows no damage");
    TrialP untitled = trial(R"({ "steps": [ { "notation": "5LP" } ] })", L);
    eq(untitled->title, "Test file", "title from the file name");
    bool del = false;
    check(!trial_from(json::parse(R"({ "deleted": true })"), "a.json", 1, L, &del) && del, "a deleted take");
    check(!trial_from(json::parse(R"({ "deleted": true, "steps": [] })"), "a.json", 1, L, &del) && !del, "not only deleted: kept on disk");

    // a recipe exported by the editor: one step per ProgressNo, every line an accepted id
    TrialP r = trial(R"({ "recipe": { "lines": [ { "ProgressNo": 1, "ExtNo": 1, "ActionNo": 12 }, { "ProgressNo": 0, "ActionNo": 5, "SkillID": 7 },
        { "ProgressNo": 1, "ExtNo": 0, "ActionNo": 11, "CheckType": 1 } ] } })", L);
    check(r && r->steps.size() == 2 && r->steps[0].ids == std::vector<int64_t>{ 5 } && r->steps[1].ids == std::vector<int64_t>{ 11, 12 } &&
          r->steps[1].check_type == 1 && r->steps[0].skill_id == 7, "steps from a raw recipe");

    // tabs: own control type first, then order, then file; caps spill into the next tab
    std::vector<TrialP> all;
    all.push_back(rec::make_slot(1, rec::Settings{}, ""));
    for (int i = 0; i < 4; ++i) {
        auto x = std::make_shared<Trial>();
        x->title = "t" + std::to_string(i);
        x->steps.resize(1);
        x->modern = i == 1;
        x->order = i == 3 ? -1 : 0;
        all.push_back(x);
    }
    for (size_t i = 0; i < all.size(); ++i) all[i]->file_order = static_cast<int>(i) + 1;
    auto by = level_sets(all, false, nullptr);
    std::string order;
    for (auto& x : by[0].rows) order += x->title + ",";
    eq(order, std::string(rec::kSlotTitle) + ",t3,t0,t2,t1,", "Classic tab order");
    const int caps[3] = { 2, 2, 9 };
    auto capped = level_sets(all, false, caps);
    check(capped[0].rows.size() == 2 && capped[1].rows.size() == 2 && capped[2].rows.size() == 1 && capped[1].rows[0]->title == "t0", "caps spill");

    // end delay: the file's, or the official trials' for the same last action / skill, whichever is longer
    const json delays = json::parse(R"({ "1": { "a": { "905": 30 }, "s": { "1010110": 329 } } })");
    t->fighter_id = 1;
    check(end_delay(*t, delays) == 329, "end delay from the skill");
    t->j["end_delay"] = 2000;
    check(end_delay(*t, delays) == 900, "end delay capped");

    // the slot
    rec::Settings rs;
    rs.sa = 2; rs.pos = 2; rs.counter = 2;
    TrialP slot = rec::make_slot(21, rs, "counter");
    check(slot->rec && slot->steps.size() == 12 && slot->steps[0].ids == std::vector<int64_t>{ rec::kPlaceholder + 1 }, "slot steps");
    check(slot->j["gauges"]["sa_stock"] == 1 && slot->j["dummy_counter"] == 2 && slot->j["start"]["p1"] == 615, "slot settings");
    check(!slot->j.contains("unique"), "no unique gauge at level 0");
    eq(shown_title(*slot), rec::kSlotTitle, "slot title");

    // ---------------------------------------------------------------- the recorder (cc_take.hpp)
    using namespace cc::take;
    Labels RL;
    RL.loaded = true;
    RL.raw = json::parse(R"({ "actions": { "640": ["2+MK", 0, 0, 0], "900": ["236+LP", 1010110, 1, 0], "901": ["236+LP", 1010110, 1, 0],
        "1205": ["", 1010110, 0, 0], "5000": ["8", 0, 0, 0], "610": ["MP", 0, 0, 0], "611": [">HP", 0, 0, 0] },
        "chains": { "MPHP": 777 }, "modern": { "640": [4220, 0] } })");
    RL.skills["1010110"] = { "<CMD _236><ICON +><CMDBTN CmnP>", "<CMD _236><ICON +><CMDBTN SM>" };
    const RecLabels R = rec_labels(RL);
    check(R.group.at("236+LP") == std::vector<int64_t>{ 900, 901 } && R.chains.at("MPHP") == 777 && R.modern.at(640).first == 4220, "label tables");
    eq(note_key("2 + MK"), "2+MK", "note key");
    eq(note_key("5236+P"), "236+P", "note key without the 5");
    eq(cmd_note("<CMD _236236><ICON +><CMDBTN CmnP>", 7).value_or("-"), "236236+PP", "a command's notation, OD");
    eq(cmd_note("<CMD _236236><ICON +><CMDBTN CmnP>", 2).value_or("-"), "236236+MP", "a command's notation, medium");
    check(!cmd_note("<CMDBTN DriveP>", 0), "no notation for a Drive button");
    check(input_mask(2 | 4 | 256, true, false) == (2 | 8 | 256) && input_mask(8, false, false) == 8 && input_mask(0x1000, false, false) == 0 &&
          input_mask(0x1000, false, true) == 0x1000, "input masks: facing right swaps, Modern keeps its high bits");

    // a take played frame by frame: rest, 2+MK that hits, 236+LP whose ball hits, then the counter back to 0
    SlotCore sc;
    Frame f;
    f.x1 = -75.0 * 65536; f.x2 = 75.0 * 65536;
    auto run = [&](int n, int64_t act, int cc, uint32_t sw, int step = 1) {
        SlotCore::Out last;
        for (int i = 0; i < n; ++i) {
            f.act = act; f.cc = cc; f.sw = sw;
            SlotCore::Out o = sc.tick(f, step, R);
            if (o.closed) last = std::move(o);
        }
        return last;
    };
    run(25, 1, 0, 0);                                  // standing still: the take starts from the rest
    run(3, 640, 0, 2 | 256);                           // 2+MK
    run(5, 640, 1, 0);                                 // it hits
    run(2, 900, 1, 2);                                 // 236+LP (two frames in one tick below)
    run(1, 900, 1, 1 | 16, 2);
    sc.push_hit(1205, true);                           // its ball hits
    run(1, 1205, 2, 0);
    f.dmg = 1234;
    run(10, 1, 2, 0);
    SlotCore::Out o = run(12, 1, 0, 0);
    check(o.closed.has_value() && o.closed->moves.size() == 2 && o.closed->hits == 2 && o.closed->max_cc == 2, "a take closes");
    if (o.closed) {
        const Take& tk = *o.closed;
        check(tk.inputs.size() == static_cast<size_t>(sc.frame - tk.start) || tk.inputs.size() > 0, "one input per fight frame");
        const auto steps = build_steps(R, tk.moves, false);
        check(steps.size() == 2, "take -> 2 steps");
        if (steps.size() == 2) {
            eq(str_of(at(steps[0].j, "notation")), "2+MK", "step 1 notation");
            eq(str_of(at(steps[1].j, "notation")), "236+LP", "step 2 notation");
            check(steps[1].j["action_ids"] == json::parse("[900, 901, 1205]"), "step 2 ids: the group and the ball");
            check(steps[1].j["check_type"] == 0 && steps[1].j.value("is_projectile_hit", false), "step 2 must hit, by its ball");
            eq(str_of(at(steps[1].j, "text")), "<CMD _236><ICON +><ICON lp>L", "step 2 text");
            eq(signature(steps, false), "2+MK/0>236+LP/0", "signature");
        }
        // the example: one entry per frame of the fight (the 2-frame tick wrote 2), from 4 frames before the first input
        TrialOpts opt;
        opt.dir = "Ryu";
        opt.gauges = json{ { "sa_stock", "max" } };
        opt.when = stamp_of(static_cast<std::time_t>(1762950000));
        Closed c = close_take(tk, R, 1, opt);
        check(c.kind == Closed::Made, "a trial is made");
        const json& demo = c.trial["demo"];
        int64_t total = 0;
        for (const json& e : demo["rle"]) total += e[0].get<int64_t>();
        check(total == demo["frames"].get<int64_t>(), "rle covers the frames");
        check(demo["rle"][0] == json::parse("[3, 258]") && demo["rle"][1] == json::parse("[5, 0]"), "rle: the take opens on 2+MK held 3 frames (a rest is never in a take)");
        bool two = false;
        for (const json& e : demo["rle"]) if (e[1] == 17 && e[0] == 2) two = true;
        check(two, "a 2-frame tick gives 2 entries");
        eq(demo["timeline"][0].get<std::string>(), "3f : 2+MK", "timeline");
        check(c.trial["recorded"]["damage"] == 1234 && c.trial["recorded"]["signature"] == "2+MK/0>236+LP/0" && c.trial["start"]["p1"] == -75,
              "recorded fields, start");
        check(!c.trial.contains("dummy") && !c.trial.contains("recipe"), "the trials' own dummy writes nothing");
        eq(c.trial["title"].get<std::string>().substr(0, 4), "REC ", "title");
        opt.min_steps = 3;
        check(close_take(tk, R, 1, opt).kind == Closed::Short, "too short");
        const std::string st = opt.when.file;
        check(st.size() == 15 && st[8] == '_', "stamp YYYYmmdd_HHMMSS");
        eq(take_file_name(st, [](const std::string&) { return false; }), "rec_" + st + ".json", "file name");
        eq(take_file_name(st, [&](const std::string& n) { return n == "rec_" + st + ".json"; }), "rec_" + st + "_2.json", "file name taken");
    }

    // target combo merged into one line when its chain is a skill
    {
        Move a; a.act = 610; a.note = "MP"; a.hit = true; a.hits = 1; a.ids = { 610 }; a.f = 1;
        Move b; b.act = 611; b.note = ">HP"; b.hit = true; b.hits = 1; b.ids = { 611 }; b.f = 9;
        const auto steps = build_steps(R, { a, b }, false);
        check(steps.size() == 1 && steps[0].j["notation"] == "MP >HP" && steps[0].j["skill_id"] == 777, "target combo: one line");
        Move w; w.act = 5; w.note = "66"; w.f = 0;
        Move miss; miss.act = 6; miss.note = "MK"; miss.label_check = 0; miss.f = 0;
        Move after; after.act = 7; after.note = "HK"; after.f = 20;
        check(build_steps(R, { w, miss, a, after }, false).size() == 1, "positioning, a whiff before and a miss after: dropped");
        check(move_label(R, a, true).first == 0 && move_label(R, Move{ 640, "2+MK" }, true).first == 4220, "Modern labels");
    }

    // carry: what was started after the last hit goes to the next take, from the neutral before its command
    {
        Take old;
        old.start = 0;
        old.inputs.assign(40, 0);
        for (int i = 1; i <= 12; ++i) old.inputs[i - 1] = 16;          // the combo
        for (int i = 25; i <= 40; ++i) old.inputs[i - 1] = 64;         // a new command
        old.x1s.assign(40, std::nullopt);
        old.x1s[20] = 1.0 * 65536;
        Move h; h.act = 640; h.note = "2+MK"; h.hit = true; h.f = 5;
        Move nx; nx.act = 900; nx.note = "236+LP"; nx.f = 30;
        old.moves = { h, nx };
        old.last_hit = 10;
        Frame now;
        now.style = 3;
        auto ct = carry_take(old, now, 40);
        check(ct && ct->inputs.size() == 20 && ct->first_input == 5 && ct->quiet && ct->moves.size() == 1 && ct->moves[0].f == 10 &&
              ct->start == 20 && ct->x1 == 65536.0 && ct->style == 3, "carry_take");
        Take quiet_old = old;
        quiet_old.moves = { h };
        for (int i = 13; i <= 40; ++i) quiet_old.inputs[i - 1] = 0;
        check(!carry_take(quiet_old, now, 40), "nothing to carry");
        check(!carry_take(old, now, 41), "a take with a rest in it is not carried");
    }

    // a replay's take cut to its combo: it starts on the ground, kReplayLead frames before
    {
        Take rt;
        rt.inputs.assign(80, 1);
        for (int i = 0; i < 80; ++i) rt.px.push_back({ 300.0 * 65536, 100.0 * 65536, (i + 1 >= 35 && i + 1 <= 40) ? 5.0 : 0.0 });
        rt.styles.assign(80, 2);
        Move walk; walk.note = "66"; walk.f = 20;
        Move first; first.act = 640; first.note = "2+MK"; first.hit = true; first.f = 40; first.first_hit = 44;
        rt.moves = { walk, first };
        rt.last_hit = 44;
        check(replay_cut(rt) && rt.moves.size() == 1 && rt.moves[0].f == 21 && rt.inputs.size() == 61 && *rt.last_hit == 25,
              "replay cut: from the ground, 14 frames before");
        check(rt.x1 && *rt.x1 == -300.0 * 65536 && *rt.x2 == -100.0 * 65536 && rt.style == 2, "replay cut: mirrored to start on the left");
    }

    // the only files the recorder deletes: its takes, right in a fighter's folder
    check(is_take_file("data/Ryu/rec_20261112_101010.json", "data") && !is_take_file("data/Ryu/trial.json", "data") &&
          !is_take_file("data/_labels/rec_x.json", "data") && !is_take_file("other/Ryu/rec_1.json", "data") &&
          !is_take_file("data/Ryu/sub/rec_1.json", "data"), "deletable files");
    eq(combos_text(1), "1 combo", "combos text");

    // the slot's settings (13/11): life, recoveries, a fighter's own gauge per fighter; their file
    {
        rec::Settings s0;
        bool any = true;
        const json t0 = rec::settings_teams(s0, &any);
        check(!any && t0 == json::parse(R"([{}, { "SGaugeStock": 0, "DGaugeStock": 6 }])"), "teams: the baseline writes nothing in a take");
        rec::Settings s1;
        s1.vp2 = 30; s1.dh2 = 2; s1.vh1 = 0; s1.sh1 = 2;
        s1.uq[1] = { 0, 1 };                                       // Ryu: Denjin Charge "Activated"
        s1.uq[16] = { 3, 0 };                                      // Juri: three Fuha stocks
        const json t1 = rec::settings_teams(s1, &any);
        check(any && t1[0] == json::parse(R"({ "VGaugeHealType": 0 })") && t1[1]["VGaugePoint"] == 30 && t1[1]["DGaugeHealType"] == 2,
              "teams: life and recoveries");
        const rec::Settings back = rec::settings_from(rec::settings_json(s1));
        check(back.vp2 == 30 && back.dh2 == 2 && back.vh1 == 0 && back.uq.size() == 2 && back.uq.at(1).t == 1 && back.uq.at(16).s == 3,
              "settings file round trip");
        check(rec::settings_from(json::parse(R"({ "vp1": 250, "unique": { "21": { "s": 9, "t": -1 } } })")).vp1 == 100 &&
              rec::settings_from(json::parse(R"({ "unique": { "21": { "s": 9, "t": -1 } } })")).uq.at(21).s == 7, "settings clamped");
        check(rec::own_gauge(s1, 1).value_or(json()) == json::parse(R"({ "type": 1, "timer": 1 })"), "own gauge: a timer alone, type 1");
        check(rec::own_gauge(s1, 16).value_or(json()) == json::parse(R"({ "type": 1, "stock": 3 })"), "own gauge: a stock");
        rec::Settings s2;
        s2.ug = 2;
        bool set = true;
        check(rec::own_gauge(s2, 16).value_or(json())["stock"] == 2 && !rec::own_gauge(s2, 1, &set) && !set, "own gauge: the old row value, stock fighters only");
        s2.uq[16] = { 0, 0 };
        check(!rec::own_gauge(s2, 16, &set) && set, "own gauge: set to nothing by the menu");
        TrialP sl = rec::make_slot(1, s1, "");
        check(sl->j["unique"] == json::parse(R"({ "type": 1, "timer": 1 })") && sl->j["gauges"]["sa_heal"] == 2 &&
              sl->j["gauges"]["dg_heal"] == "auto" && sl->j["recipe"]["teams"][1]["VGaugePoint"] == 30, "slot fields");
        sl->j["unique"] = json::parse(R"({ "type": 1, "stock": 1 })");   // a take's, for a fighter the menu never set
        TrialP sj = rec::make_slot(21, s0, "");
        sj->j["unique"] = json::parse(R"({ "type": 1, "stock": 1 })");
        rec::slot_own_gauge(*sj, s0);
        rec::slot_own_gauge(*sl, s1);
        check(sj->j["unique"]["stock"] == 1 && sl->j["unique"] == json::parse(R"({ "type": 1, "timer": 1 })"), "the menu's setting wins, a take's stays otherwise");
        rec::Settings s3 = s1;
        s3.uq[1].t = 2;
        check(rec::settings_key(s1, 1) != rec::settings_key(s3, 1) && rec::settings_key(s1, 16) == rec::settings_key(s3, 16) &&
              rec::settings_key(s1, 1) == rec::settings_key(back, 1), "the menu's change is seen for the slot's fighter");

        // the own gauge a take starts with: the slot's setting; Jamie's drinks from the fight
        TrialOpts op;
        op.own_gauge = json::parse(R"({ "type": 1, "stock": 2 })");
        check(take_own_gauge(kJamie, 3, op).value()["stock"] == 3 && take_own_gauge(kJamie, 6, op).value()["stock"] == 2 &&
              take_own_gauge(16, 4, op).value()["stock"] == 2, "take own gauge: slot, Jamie's higher level");
        op.own_gauge = json::parse(R"({ "type": 1, "stock": 1, "timer": 1 })");
        check(take_own_gauge(kJamie, 3, op).value()["stock"] == 1, "take own gauge: a timer keeps the slot's stock");
        op.own_gauge.reset();
        check(take_own_gauge(kJamie, 2, op).value()["stock"] == 2 && !take_own_gauge(16, 2, op), "take own gauge: nothing set");
        op.replay = true;
        check(take_own_gauge(kJamie, 5, op).value() == json::parse(R"({ "type": 1, "stock": 4, "timer": 1 })") && !take_own_gauge(28, 3, op),
              "take own gauge: a replay knows Jamie's drinks only");
    }

    // a take of the slot carries the slot's teams and the tab asked
    {
        Take tk;
        tk.start = 0;
        tk.inputs.assign(30, 16);
        Move a; a.act = 640; a.note = "2+MK"; a.hit = true; a.hits = 1; a.ids = { 640 }; a.f = 2; a.first_hit = 5;
        Move b; b.act = 900; b.note = "236+LP"; b.hit = true; b.hits = 1; b.ids = { 900 }; b.f = 12; b.first_hit = 16;
        tk.moves = { a, b };
        tk.hits = 2; tk.max_cc = 2; tk.last_hit = 16; tk.damage = 900; tk.style = 2; tk.first_input = 1;
        TrialOpts oq;
        oq.dir = "Jamie";
        oq.when = stamp_of(static_cast<std::time_t>(1762950000));
        rec::Settings s;
        s.vp1 = 50;
        oq.teams = rec::settings_teams(s, &oq.teams_any);
        oq.own_gauge = rec::own_gauge(s, kJamie);
        oq.level = 2;
        Closed c = close_take(tk, R, kJamie, oq);
        check(c.kind == Closed::Made && c.trial["level"] == 2 && c.trial["recipe"]["teams"][0]["VGaugePoint"] == 50 &&
              c.trial["unique"] == json::parse(R"({ "type": 1, "stock": 2 })"), "take: tab, life, Jamie's drinks");
        oq.replay = true;
        Closed rc = close_take(tk, R, kJamie, oq);
        check(!rc.trial.contains("recipe") && rc.trial["gauges"]["sa_stock"] == "max", "a replay's take: none of the slot's settings");
    }

    // the reset a changed setting leaves pending: asked as the player leaves the pause menu
    {
        using PR = rec::PendingReset;
        rec::PauseView menu;                       // the pause menu on screen, its flow there
        menu.agent = menu.shown = menu.flow = true;
        rec::PauseView stale = menu;               // isCancel still true from an earlier cancel
        stale.cancel = true;
        rec::PauseView cancel = stale;             // the cancel frame
        cancel.hide_next = true;
        rec::PauseView decided = menu;             // a row decided: same _Next, isCancel false
        decided.hide_next = true;
        rec::PauseView fading;                     // the frame after: FadeOut, no delegates
        fading.agent = true;
        rec::PauseView sub = fading;               // a sub-window: Show, no delegates
        sub.shown = true;
        PR p;
        check(p.tick(0, true, cancel) == PR::None, "nothing pending: nothing asked");
        p.arm(5);
        check(p.tick(5, true, stale) == PR::None && p.pending, "the training menu closed: the pause menu is still up");
        check(p.tick(5, true, cancel) == PR::Now && !p.pending, "asked on the cancel frame");
        p.arm(5);
        check(p.tick(5, true, menu) == PR::None && p.tick(5, true, fading) == PR::Prev, "the net: the frame after, with the flow kept");
        p.arm(5);
        p.row_decided();
        check(p.tick(5, true, decided) == PR::None && p.tick(5, true, fading) == PR::None && p.pending, "a decided row asks nothing");
        check(p.tick(6, true, menu) == PR::None && !p.pending, "ended by the next StageSetup (the row's own reset)");
        p.arm(5);
        check(p.tick(5, true, sub) == PR::None && p.tick(5, true, fading) == PR::None && p.pending, "a sub-window asks nothing");
        check(p.tick(5, true, cancel) == PR::Now, "... and the cancel after it does");
        p.arm(5);
        p.tick(5, true, menu);
        p.menu_up();
        check(p.tick(5, true, fading) == PR::None && p.pending, "the training menu up again: no flow kept");
        check(p.tick(5, false, cancel) == PR::None && !p.pending, "ended when our rows are not the menu's");
        p.arm(5);
        rec::PauseView none;
        for (int i = 0; i < 120; ++i) p.tick(5, true, none);
        check(p.pending, "no pause menu for 120 frames: still pending");
        p.tick(5, true, none);
        check(!p.pending, "ended when the scene has no pause menu");
    }

    // one recording slot per tab; a take goes to its slot's tab
    {
        std::vector<TrialP> set;
        for (int lv = 2; lv >= 0; --lv) set.insert(set.begin(), rec::make_slot(1, rec::Settings{}, "", lv));
        auto f1 = std::make_shared<Trial>(); f1->title = "a"; f1->level = 1; f1->steps.resize(1); f1->path = "x/a.json";
        auto f2 = std::make_shared<Trial>(); f2->title = "b"; f2->level = 1; f2->steps.resize(1); f2->path = "x/b.json"; f2->order = -5;
        set.push_back(f1);
        set.push_back(f2);
        for (size_t i = 0; i < set.size(); ++i) set[i]->file_order = static_cast<int>(i) + 1;
        auto tabs = level_sets(set, false, nullptr);
        check(tabs[0].rows.size() == 1 && tabs[1].rows.size() == 3 && tabs[2].rows.size() == 1 && tabs[0].rows[0]->rec &&
              tabs[1].rows[0]->rec && tabs[1].rows[0]->level == 1 && tabs[2].rows[0]->rec && tabs[1].rows[1] == f2,
              "a recording slot first in every tab");
        // Move: the file's level, the tabs sorted again
        f2->level = 2;
        tabs = level_sets(set, false, nullptr);
        check(tabs[1].rows.size() == 2 && tabs[2].rows.size() == 2 && tabs[2].rows[0]->rec && tabs[2].rows[1] == f2, "moved: in the other tab, after its slot");
        TrialOpts o1;
        o1.dir = "Ryu";
        o1.level = tabs[1].rows[0]->level;
        Take tk;
        tk.inputs.assign(10, 16);
        Move a; a.act = 640; a.note = "2+MK"; a.hit = true; a.hits = 1; a.ids = { 640 }; a.f = 1;
        Move b; b.act = 900; b.note = "236+LP"; b.hit = true; b.hits = 1; b.ids = { 900 }; b.f = 5;
        tk.moves = { a, b };
        tk.hits = 2;
        tk.last_hit = 6;
        check(close_take(tk, R, 1, o1).trial["level"] == 1, "a take gets its slot's tab");
    }

    // the sub-menu of a trial: its buttons per tab, the names, the trash
    {
        auto titles = [](int lv) { std::string s; for (const auto& b : manage::buttons(lv)) s += b.title + "|"; return s; };
        eq(titles(0), "Rename|Move to Intermediate|Move to Advanced|Delete|Share Trial|Download Trial|", "buttons, Beginner tab");
        eq(titles(1), "Rename|Move to Beginner|Move to Advanced|Delete|Share Trial|Download Trial|", "buttons, Intermediate tab");
        eq(titles(2), "Rename|Move to Beginner|Move to Intermediate|Delete|Share Trial|Download Trial|", "buttons, Advanced tab");
        const auto b1 = manage::buttons(1);
        check(b1.size() == 6 && b1[1].act == manage::Act::Move && b1[1].level == 0 && b1[2].level == 2 && b1[3].act == manage::Act::Delete &&
              b1[4].act == manage::Act::Share && b1[5].act == manage::Act::Download, "buttons: actions and levels");
        check(manage::buttons(7).size() == 6 && manage::buttons(7)[1].level == 1, "buttons: a level out of range is Beginner");
        auto own = std::make_shared<Trial>(); own->path = "x/a.json";
        auto rslot = rec::make_slot(1, rec::Settings{}, "", 1);
        auto bare = std::make_shared<Trial>();
        check(manage::can_manage(*own) && !manage::can_manage(*rslot) && !manage::can_manage(*bare), "the sub-menu: our files only");
        eq(manage::clean_name("  My combo  ", 40), "My combo", "name trimmed");
        eq(manage::clean_name("   ", 40), "", "an empty name changes nothing");
        eq(manage::clean_name(std::string("Ab\xC3\xA9", 4), 3), "Ab", "name cut on a character boundary");
        const fs::path tp = manage::trash_path("data", "Ryu", "data/Ryu/rec_20261113_080000.json", "20261113_091500");
        eq(u8(tp), u8(fs::path("data") / "_trash" / "Ryu" / "rec_20261113_080000_20261113_091500.json"), "trash path");
        check(manage::deleted_marker() == json::parse(R"({ "deleted": true })"), "the marker");
        bool mdel = false;
        check(!trial_from(manage::deleted_marker(), "a.json", 1, L, &mdel) && mdel, "the marker is a deleted take for the loader");
        check(!manage::share_trial(*own).empty() && !manage::download_trial(1).empty(), "share / download: inert, they say so");
    }

    // the Replay Menu's rows and when they are written again
    {
        const auto idle = replay_rows(false, 0);
        check(idle.size() == 1 && idle[0].ft == kFtStart && idle[0].text == "Start Combo Recording", "replay rows: idle");
        const auto rec2 = replay_rows(true, 2);
        check(rec2.size() == 4 && rec2[0].ft == kFtSave && rec2[1].ft == kFtSaveMid && rec2[2].ft == kFtSaveAdv && rec2[3].ft == kFtCancel,
              "replay rows: recording");
        eq(rec2[1].text, "Save as Intermediate (2 combos held)", "replay rows: a save row's text");
        eq(replay_rows(true, 1)[2].text, "Save as Advanced (1 combo held)", "replay rows: one combo");
        check(replay_rows_state(false, true, 3) == "off" && replay_rows_state(true, false, 3) == "idle" &&
              replay_rows_state(true, true, 3) == "rec 3" && replay_rows_state(true, true, 3) != replay_rows_state(true, true, 4),
              "replay rows: states");
        check(replay_save_level(kFtSave) == 0 && replay_save_level(kFtSaveMid) == 1 && replay_save_level(kFtSaveAdv) == 2 &&
              replay_save_level(kFtCancel) == -1 && replay_save_level(kFtStart) == -1, "replay rows: save levels");
        check(replay_ft_ours(40) && replay_ft_ours(44) && !replay_ft_ours(39) && !replay_ft_ours(45) && !replay_ft_ours(8), "replay rows: ours");
    }

    std::printf("%d check(s), %d failure(s)\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
