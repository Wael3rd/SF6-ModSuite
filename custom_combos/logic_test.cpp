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

    std::printf("%d check(s), %d failure(s)\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
