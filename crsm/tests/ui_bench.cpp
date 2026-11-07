// ui_bench — offline tests of the pure logic of the DLL's own screens (phase P4, src/ui/logic.*): the windows over 30
// slots and 30 reversal rows, the cursor answers at the block edges (single cases and whole walks with a model of the game's
// list), the arrows, the slot name fit, the learnt wording, the spin names' ellipsis, the text dialog's outcome, the key
// edges, and the Community dialog (pages, sort, votes, downloads, and the cursor kept after a list refresh); the drill
// details dialog (src/ui/details_logic.*: slot pages, texts, the drillcodes author label, the guide entries, the wrapped
// lines from baselines, the window, the thumb, the hold-repeat, the mouse wheel's client -> UI mapping, area and notches)
// and the pack readers it shows (model::pack author / source / slot_summaries).
// Expected values: RecordingManagerTab.lua / ReversalSlots.lua behaviour (line numbers in logic.hpp) and the game's
// own screens. No game, no data. Exit code 0 only when every check passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "model/labels.hpp"
#include "model/pack.hpp"
#include "ui/details_logic.hpp"
#include "ui/logic.hpp"

namespace lg = ui::logic;

namespace {

int g_checks = 0, g_fails = 0;
std::string g_group;
std::map<std::string, std::pair<int, int>> g_by_group;
std::vector<std::string> g_order;

void group(const std::string& g) { g_group = g; if (!g_by_group.count(g)) g_order.push_back(g); }
void check(bool ok, const std::string& what) {
    ++g_checks;
    auto& c = g_by_group[g_group];
    ++c.first;
    if (ok) return;
    ++g_fails;
    ++c.second;
    if (c.second <= 12) std::printf("  FAIL [%s] %s\n", g_group.c_str(), what.c_str());
}
template <class A, class B> void eq(const A& a, const B& b, const std::string& what) {
    check(a == b, what);
}

// ---- windows and arrows --------------------------------------------------------------------------------------------------
void test_windows() {
    group("window");
    eq(lg::clamp_win(-3), 0, "clamp below");
    eq(lg::clamp_win(0), 0, "clamp 0");
    eq(lg::clamp_win(22), 22, "clamp 22");
    eq(lg::clamp_win(40), 22, "clamp above");
    for (int n = 1; n <= 30; ++n) {
        eq(lg::record_slot_id(n), n <= 8 ? n - 1 : 7, "record _SlotID of slot " + std::to_string(n) + " (R37: 9-30 through slot 8)");
        eq(lg::replay_slot_id(n, true), n - 1, "replay _SlotID with the 30-array, slot " + std::to_string(n));
        eq(lg::replay_slot_id(n, false), n <= 8 ? n - 1 : 7, "replay _SlotID without the 30-array, slot " + std::to_string(n));
    }
    for (int win = 0; win <= 22; ++win)
        for (int k = 1; k <= 8; ++k) {
            std::string want;
            if (k == 1 && win > 0) want = "\xE2\x96\xB2";
            else if (k == 8 && win < 22) want = "\xE2\x96\xBC";
            eq(std::string(lg::window_arrow(k, win)), want, "arrow row " + std::to_string(k) + " win " + std::to_string(win));
        }
    group("reversal window");
    for (int win = 0; win <= 20; ++win)
        for (int idx = 0; idx <= 11; ++idx) {
            std::string want;
            if (idx == 1 && win > 0) want = "\xE2\x96\xB2";
            else if (idx == 10 && win < 20) want = "\xE2\x96\xBC";
            eq(std::string(lg::reversal_arrow(idx, win)), want, "reversal arrow idx " + std::to_string(idx) + " win " + std::to_string(win));
        }
}

// ---- the Record cursor ---------------------------------------------------------------------------------------------------
// The game's list on the Record page: 0 strip, 1 header (not focusable), 2-9 slot rows,
// 10 Recording Trigger, 11 Countdown. Its own next / previous index (wrap at the ends).
int rec_game_next(int cur) { return cur == 11 ? 2 : (cur == 0 ? 2 : cur + 1); }
int rec_game_prev(int cur) { return cur == 2 ? 0 : (cur == 0 ? 11 : cur - 1); }

struct RecSim {
    int cur = 0, win = 0;
    // one press: what the hooks do (pre: the claim; post: the answer)
    void press(int dir) {
        const int nxt = dir > 0 ? rec_game_next(cur) : rec_game_prev(cur);
        const bool in_block = cur >= lg::kFirstRow && cur <= lg::kLastRow;
        const bool outside_claim = !in_block && cur <= lg::kLastRow + 3 && cur >= lg::kFirstRow - 2;
        if (in_block || outside_claim) {
            const lg::CursorAnswer a = lg::record_cursor(cur, nxt, dir, win, outside_claim);
            if (a.move) win = a.win;
            cur = a.replace ? a.index : nxt;
        } else {
            cur = nxt;
        }
    }
    int slot() const { return (cur >= 2 && cur <= 9) ? win + cur - 1 : 0; }
};

void test_record_cursor() {
    group("record cursor");
    // single cases
    auto a = lg::record_cursor(5, 5, 1, 0, false);
    check(!a.move && !a.replace, "nxt == cur: the game's answer");
    a = lg::record_cursor(5, 6, 1, 3, false);
    check(!a.move && !a.replace, "inside the block: the game's answer");
    a = lg::record_cursor(9, 10, 1, 3, false);
    check(a.move && a.win == 4 && a.replace && a.index == 9, "down past the last row: the window slides, the cursor stays");
    a = lg::record_cursor(9, 10, 1, 22, false);
    check(!a.move && !a.replace, "down past slot 30: the cursor leaves to Recording Trigger");
    a = lg::record_cursor(2, 0, -1, 5, false);
    check(a.move && a.win == 4 && a.replace && a.index == 2, "up past the first row: the window slides back");
    a = lg::record_cursor(2, 0, -1, 0, false);
    check(!a.move && !a.replace, "up from slot 1: the game's answer (the strip)");
    a = lg::record_cursor(0, 2, 1, 7, true);
    check(a.move && a.win == 0 && a.replace && a.index == 2, "from the strip down: slot 1, window at the top");
    a = lg::record_cursor(10, 9, -1, 0, true);
    check(a.move && a.win == 22 && a.replace && a.index == 9, "up from Recording Trigger: slot 30, window at the bottom");
    a = lg::record_cursor(11, 2, 1, 15, true);
    check(a.move && a.win == 0 && a.replace && a.index == 2, "down from Countdown wraps to slot 1");
    a = lg::record_cursor(10, 11, 1, 4, true);
    check(!a.move && !a.replace, "Trigger -> Countdown: the game's answer");
    // as the game numbers them, read in game 07/11: Trigger 11, Countdown 12, the row between them and the slots not focusable
    a = lg::record_cursor(12, 11, -1, 22, true);
    check(!a.move && !a.replace, "up from row 12: the game's answer, row 11");
    a = lg::record_cursor(11, 9, -1, 0, true);
    check(a.move && a.win == 22 && a.replace && a.index == 9, "up from row 11: slot 30");
    a = lg::record_cursor(12, 2, 1, 22, true);
    check(a.move && a.win == 0 && a.replace && a.index == 2, "down from row 12: slot 1");
    // Wael, 07/11: up from Countdown jumped to slot 30 over Recording Trigger
    for (const int win : { 0, 7, 22 }) {
        a = lg::record_cursor(11, 10, -1, win, true);
        check(!a.move && !a.replace, "up from Countdown: the game's answer, Recording Trigger, at any window");
    }
    {
        RecSim r;
        r.cur = 9; r.win = 22;
        r.press(1);
        check(r.cur == 10, "slot 30 down: Recording Trigger");
        r.press(1);
        check(r.cur == 11, "down: Countdown");
        r.press(-1);
        check(r.cur == 10 && r.win == 22, "up: Recording Trigger, not slot 30");
        r.press(-1);
        check(r.cur == 9 && r.win == 22 && r.slot() == 30, "up: slot 30");
        r.cur = 11; r.win = 7;
        r.press(-1);
        check(r.cur == 10 && r.win == 7, "from a window in the middle too: Recording Trigger, the window kept");
        r.press(-1);
        check(r.cur == 9 && r.win == 22 && r.slot() == 30, "then slot 30, as before");
        r.cur = 11; r.win = 7;
        r.press(1);
        check(r.cur == 2 && r.win == 0 && r.slot() == 1, "the wrap, down from the last row: slot 1");
        r.press(-1);
        check(r.cur == 0, "up from slot 1 at the top: the strip");
        r.press(-1);
        check(r.cur == 11, "up from the strip: the last row");
        r.press(-1);
        check(r.cur == 10, "and up again: Recording Trigger");
    }
    // whole walks
    RecSim s;
    std::vector<int> slots;
    for (int i = 0; i < 31; ++i) { s.press(1); slots.push_back(s.slot()); }
    bool ok = true;
    for (int i = 0; i < 30; ++i) ok = ok && slots[(size_t)i] == i + 1;
    check(ok, "30 presses down from the strip show slots 1..30 under the cursor in order");
    check(s.cur == 10 && s.win == 22, "the 31st press leaves the block to Recording Trigger, window at the bottom");
    s.press(1);
    check(s.cur == 11, "then Countdown");
    s.press(1);
    check(s.cur == 2 && s.win == 0 && s.slot() == 1, "then the wrap back to slot 1");
    RecSim u; u.cur = 10; u.win = 0;
    std::vector<int> up;
    for (int i = 0; i < 30; ++i) { u.press(-1); up.push_back(u.slot()); }
    ok = true;
    for (int i = 0; i < 30; ++i) ok = ok && up[(size_t)i] == 30 - i;
    check(ok, "30 presses up from Recording Trigger show slots 30..1");
    u.press(-1);
    check(u.cur == 0, "then the strip");
    // the window never leaves 0..22 and the cursor never sits outside the list
    RecSim r;
    unsigned seed = 12345;
    bool bounded = true;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 1103515245u + 12345u;
        r.press((seed >> 16) & 1 ? 1 : -1);
        bounded = bounded && r.win >= 0 && r.win <= 22 && r.cur >= 0 && r.cur <= 11 && r.cur != 1;
    }
    check(bounded, "5000 random presses: window within 0..22, cursor on a focusable row");
}

// ---- the Replay cursor ---------------------------------------------------------------------------------------------------
void test_replay_cursor() {
    const auto all_empty_first = [](int) { return true; };
    group("replay cursor");
    const std::set<int> valid = { 1, 5, 12, 30 };
    auto disabled = [&](int n) { return !valid.count(n); };
    // inside the block: the game's own move stands
    auto a = lg::replay_cursor(2, 6, 1, 0, false, disabled);
    check(!a.move && !a.replace, "inside the block: the game's answer");
    // slot 5 (row 6), down: slots 6-8 empty, the game leaves the block -> the next valid slot (12) brought under row 6
    a = lg::replay_cursor(6, 10, 1, 0, false, disabled);
    check(a.move && a.win == 7 && a.replace && a.index == 6, "down from slot 5 jumps to slot 12 under the same row");
    // from slot 30 down: nothing valid that way -> the list scrolls to its end, the game's answer (Replay Info Display)
    a = lg::replay_cursor(9, 10, 1, 22, false, disabled);
    check(a.move && a.win == 22 && !a.replace, "down from slot 30: the cursor leaves the block");
    // up from slot 12 (win 7, row 6) -> slot 5 under row 6 (window 0... clamped: 5 - 5 = 0)
    a = lg::replay_cursor(6, 1, -1, 7, false, disabled);
    check(a.move && a.win == 0 && a.replace && a.index == 6, "up from slot 12 jumps to slot 5");
    // up from slot 1: nothing above -> window to 0, the game's answer
    a = lg::replay_cursor(2, 0, -1, 0, false, disabled);
    check(a.move && a.win == 0 && !a.replace, "up from slot 1: the cursor leaves to the strip");
    // up from Replay Info Display (row 10): the last valid slot of the whole list (30), window at the bottom
    a = lg::replay_cursor(10, 9, -1, 0, true, disabled);
    check(a.move && a.win == 22 && a.replace && a.index == 9, "up from Replay Info Display: slot 30");
    // Start Replay (row 12) down, the game wraps to the top: the first valid slot
    a = lg::replay_cursor(12, 2, 1, 15, true, disabled);
    check(a.move && a.win == 0 && a.replace && a.index == 2, "Start Replay down: slot 1");
    // from the strip down (row 0): the first valid slot
    a = lg::replay_cursor(0, 2, 1, 9, true, disabled);
    check(a.move && a.win == 0 && a.replace && a.index == 2, "from the strip down: slot 1");
    // outside, moving away from the block (Replay Info -> Repeat Replay): the game's answer
    a = lg::replay_cursor(10, 11, 1, 0, true, disabled);
    check(!a.move && !a.replace, "Replay Info Display down: the game's answer");
    // read in game 07/11: the window's 8 rows empty, the game answers the header (row 1) from row 11: the valid slots further on still come first
    a = lg::replay_cursor(11, 1, -1, 0, true, disabled);
    check(a.move && a.replace && a.index >= lg::kFirstRow && a.index <= lg::kLastRow, "up from row 11 over an empty window: the last valid slot");
    a = lg::replay_cursor(11, 1, -1, 0, true, all_empty_first);
    check(!a.move && !a.replace, "nothing valid at all: the game's answer");
    a = lg::replay_cursor(12, 11, -1, 22, true, disabled);
    check(!a.move && !a.replace, "up from row 12: the game's answer, row 11");
    // Wael, 07/11: up from the second row under the block jumped to the last slot
    for (const int cur : { 11, 12 }) for (const int rwin : { 0, 7, 22 }) {
        a = lg::replay_cursor(cur, cur - 1, -1, rwin, true, disabled);
        check(!a.move && !a.replace, "up inside the rows under the block: the game's answer, at any window");
    }
    // everything empty: no jump at all
    auto all_empty = [](int) { return true; };
    a = lg::replay_cursor(0, 10, 1, 0, true, all_empty);
    check(!a.move && !a.replace, "no valid slot, from outside: the game's answer");
    a = lg::replay_cursor(9, 10, 1, 3, false, all_empty);
    check(a.move && a.win == 22 && !a.replace, "no valid slot, from inside: the list scrolls to its end");
    // the answer always lands on a valid slot row, the window in range
    bool ok = true;
    for (int rwin = 0; rwin <= 22; ++rwin)
        for (int cur = 2; cur <= 9; ++cur)
            for (int dir : { -1, 1 }) {
                const int nxt = dir > 0 ? 10 : 0;
                const auto b = lg::replay_cursor(cur, nxt, dir, rwin, false, disabled);
                if (b.replace) ok = ok && b.win >= 0 && b.win <= 22 && b.index >= 2 && b.index <= 9 && valid.count(b.win + b.index - 1);
            }
    check(ok, "every jump lands on a valid slot under a slot row");
}

void test_reversal_cursor() {
    group("reversal cursor");
    auto a = lg::reversal_cursor(5, 6, 1, 0);
    check(!a.move && !a.replace, "inside, forward: the game's answer");
    a = lg::reversal_cursor(10, 1, 1, 0);
    check(a.move && a.win == 1 && a.replace && a.index == 10, "row 10 down (the game wraps): the window slides, the cursor stays");
    a = lg::reversal_cursor(10, 11, 1, 3);
    check(a.move && a.win == 4 && a.replace && a.index == 10, "row 10 down (the game leaves): the window slides");
    a = lg::reversal_cursor(10, 1, 1, 20);
    check(a.move && a.win == 0 && !a.replace, "slot 30 down (the game wraps to row 1): the window to the top, slot 1");
    a = lg::reversal_cursor(10, 0, 1, 20);
    check(a.move && a.win == 0 && !a.replace, "slot 30 down (the game wraps to the strip): the window to the top");
    a = lg::reversal_cursor(10, 11, 1, 20);
    check(!a.move && !a.replace, "row 10 at the last window leaving forward: the game's answer");
    a = lg::reversal_cursor(10, 10, 1, 20);
    check(!a.move && !a.replace, "the game keeps the cursor: nothing");
    a = lg::reversal_cursor(1, 10, -1, 5);
    check(a.move && a.win == 4 && a.replace && a.index == 1, "row 1 up: the window slides back");
    a = lg::reversal_cursor(1, 0, -1, 0);
    check(!a.move && !a.replace, "row 1 at the first window: the game's answer (the strip)");
    a = lg::reversal_cursor(1, 10, -1, 0);
    check(a.move && a.win == 20 && !a.replace, "slot 1 up (the game wraps to row 10): the window to the bottom, slot 30");
    a = lg::reversal_cursor(0, 10, -1, 0);
    check(a.move && a.win == 20 && !a.replace, "strip up (the game wraps to row 10): slot 30");
    a = lg::reversal_cursor(0, 1, 1, 7);
    check(a.move && a.win == 0 && !a.replace, "strip down: slot 1 (window at the top)");
    a = lg::reversal_cursor(0, 1, 1, 0);
    check(!a.move && !a.replace, "strip down, window at the top: the game's answer");
    // slot 30, then Down: slot 1 (Wael, test B step 6: it showed slot 21)
    {
        int c = 10, w = 20;
        const auto b = lg::reversal_cursor(c, 1, 1, w);
        if (b.move) w = b.win;
        c = b.replace ? b.index : 1;
        check(w + c == 1, "slot 30 Down shows slot 1");
        const auto u = lg::reversal_cursor(c, 10, -1, w);   // and Up again: slot 30
        if (u.move) w = u.win;
        c = u.replace ? u.index : 10;
        check(w + c == 30, "then Up (the game wraps) shows slot 30");
    }
    // a walk down over the 30 rows
    int cur = 1, win = 0;
    std::vector<int> rows;
    for (int i = 0; i < 29; ++i) {
        const int nxt = cur == 10 ? 1 : cur + 1;     // the game wraps inside its 10 rows
        const auto b = lg::reversal_cursor(cur, nxt, 1, win);
        if (b.move) win = b.win;
        cur = b.replace ? b.index : nxt;
        rows.push_back(win + cur);
    }
    bool ok = true;
    for (int i = 0; i < 29; ++i) ok = ok && rows[(size_t)i] == i + 2;
    check(ok, "29 presses down from row 1 show rows 2..30");
}

// ---- fit, wording, ellipsis ----------------------------------------------------------------------------------------------
void test_fit() {
    group("name fit");
    lg::FitIn in;
    in.has_name = true; in.name_w = 100; in.has_frames = true; in.frames_w = 60;
    in.own_w = 200; in.font_w = 30; in.base_w = 30; in.base_h = 30; in.pos_x = -180;
    // left = -420 + 100 + 26 = -294; right = -30 - 26 = -56; avail = 238 * 0.92 = 218.96; centre = -175
    auto o = lg::fit_name(in);
    check(!o.set_font && o.font_w == 30.0f, "a name narrower than the room keeps the base font");
    check(o.set_x && o.x == -175.0f, "centred in the free span");
    in.own_w = 300;                                  // w_base 300 > 218.96: scale 0.7299
    o = lg::fit_name(in);
    check(o.set_font && o.font_w > 21.8f && o.font_w < 22.0f && o.font_h == o.font_w, "a wide name shrinks to the room");
    in.own_w = 2000;                                 // scale floor 0.5
    o = lg::fit_name(in);
    check(o.set_font && o.font_w == 15.0f, "never below half the base font");
    in.own_w = 300; in.font_w = 15;                  // already shrunk: the natural width is own * base / cur = 600
    o = lg::fit_name(in);
    check(!o.set_font && o.font_w == 15.0f, "the natural width (at the base font) decides: stable, nothing rewritten");
    in.has_name = false; in.has_frames = false; in.own_w = 0; in.font_w = 30; in.pos_x = -420;
    // left = -420 + 60 + 26 = -334; right = -20 - 26 = -46; centre = -190
    o = lg::fit_name(in);
    check(!o.set_font && o.set_x && o.x == -190.0f, "absent texts: the Lua's widths 60 / 40");
    in.has_name = true; in.name_w = 500; in.has_frames = true; in.frames_w = 400; in.own_w = 200; in.pos_x = 0;
    o = lg::fit_name(in);                            // room negative: 60 at least
    check(o.set_font && o.font_w == 15.0f, "no room: the 60-unit floor, then the half font");

    group("wording");
    std::string p, s;
    check(lg::learn_name_format("Slot 1", &p, &s) && p == "Slot " && s.empty(), "Slot 1");
    check(lg::learn_name_format("\xE3\x82\xB9\xE3\x83\xAD\xE3\x83\x83\xE3\x83\x88""1", &p, &s) && p == "\xE3\x82\xB9\xE3\x83\xAD\xE3\x83\x83\xE3\x83\x88" && s.empty(), "a Japanese prefix");
    check(lg::learn_name_format("1 slot", &p, &s) && p.empty() && s == " slot", "a suffix");
    check(lg::learn_name_format("Slot 12 b", &p, &s) && p == "Slot " && s == " b", "the first run of digits");
    check(!lg::learn_name_format("Slot", &p, &s), "no digit: not learnt");
    eq(lg::slot_text(false, "", "", 14), std::string("Slot 14"), "the default wording");
    eq(lg::slot_text(true, "Emplacement ", "", 14), std::string("Emplacement 14"), "the learnt wording");
    check(lg::is_frames_text("59F") && lg::is_frames_text("59 F") && lg::is_frames_text("1200  F"), "frames texts");
    check(!lg::is_frames_text("F") && !lg::is_frames_text("Empty") && !lg::is_frames_text("59Fx") && !lg::is_frames_text(" 59F"), "not frames texts");
    check(lg::learnable_empty_word("Vide") && !lg::learnable_empty_word("") && !lg::learnable_empty_word("nil") && !lg::learnable_empty_word("78F"), "the empty word");
    check(lg::learnable_start_text("Start Recording") && !lg::learnable_start_text("\xE2\x96\xB2") && !lg::learnable_start_text("\xE2\x96\xBC") && !lg::learnable_start_text("nil") && !lg::learnable_start_text(""), "the start text");

    group("spin names");
    const std::string e64(64, 'e'), e65(65, 'e');
    auto l = model::labels::short_list({ e64, e65, "Ryu_Base" });
    check(l[0] == e64, "64 bytes: kept");
    check(l[1] == std::string(63, 'e') + "\xE2\x80\xA6", "65 bytes: 63 + ellipsis");
    check(l[2] == "Ryu_Base", "short: kept");
    std::string acc;                                 // 32 x "é" (2 bytes) = 64, + "€" (3) = 67 bytes: 63 would split an "é"
    for (int i = 0; i < 32; ++i) acc += "\xC3\xA9";
    acc += "\xE2\x82\xAC";
    l = model::labels::short_list({ acc });
    check(l[0].size() <= 66 && l[0].substr(l[0].size() - 3) == "\xE2\x80\xA6" && (unsigned char)l[0][l[0].size() - 4] != 0xC3, "cut on a character boundary (fix 7.4)");
}

// ---- the Community rows fitted to their width, the descriptions ------------------------------------------------------------
using json = nlohmann::json;
const std::string kEll = "\xE2\x80\xA6";

json com_item(const std::string& name, const std::string& by, const std::string& date, int slots, int score, int dls) {
    return json{ {"id", "x"}, {"display_name", name}, {"uploader_name", by}, {"created_at", date + " 16:07:06"}, {"updated_at", ""},
                 {"version", 1}, {"slot_count", slots}, {"score", score}, {"downloads", dls} };
}
std::string com_full(const json& it, const json& dv = json()) { std::string l; model::labels::community_label(it, dv, &l); return l; }
std::string com_fit(const json& it, const json& dv = json(), double em = lg::kComRowEm) { return lg::com_row_fit(it, dv, com_full(it, dv), em); }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

// the Community words in another language, the width of non-Latin text, the capitals of the details' headers
void test_localised_words() {
    group("localised words");
    namespace dt = ui::details;
    // ---- the width of a text: a UTF-8 sequence is one character, never its bytes
    const std::string cjk = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";               // ni-hon-go (3 characters, 9 bytes)
    const std::string hangul = "\xED\x95\x9C\xEA\xB8\x80";                         // han-geul (2 characters)
    const std::string cyr = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";     // Privet (6 letters, 12 bytes)
    const std::string e_acute = "\xC3\xA9", emoji = "\xF0\x9F\x98\x80", fullwidth_a = "\xEF\xBC\xA1", rad = "\xE2\xBA\x80";
    eq(lg::text_em(cjk), 3.0, "CJK: 1 em a character (3 characters, not 9 bytes)");
    eq(lg::text_em(hangul), 2.0, "Hangul: 1 em a character");
    eq(lg::text_em(fullwidth_a), 1.0, "a full-width letter: 1 em");
    eq(lg::text_em(rad), 1.0, "a CJK radical (U+2E80): 1 em");
    eq(lg::text_em(emoji), 1.0, "a character past U+FFFF: 1 em");
    check(std::fabs(lg::text_em(cyr) - 6 * 0.6) < 1e-9 && lg::text_em(cyr) >= 6 * 0.55, "Cyrillic: 0.6 em a letter (>= 0.55), 6 letters not 12 bytes");
    check(std::fabs(lg::text_em(cyr + cyr) - 2 * lg::text_em(cyr)) < 1e-9, "Cyrillic: the width adds up");
    eq(lg::text_em(e_acute), 0.6, "an accented Latin letter: one character of 0.6 em");
    eq(lg::text_em("\xE9"), 0.6, "a stray byte: one character of 0.6 em, never 0");
    const std::string lead_only = "a\xE6";                                                  // a cut sequence at the end
    check(std::fabs(lg::text_em(lead_only) - (lg::text_em("a") + 0.6)) < 1e-9, "a cut sequence: one character");
    const std::string mixed = "A" + cjk + cyr + "z";
    check(std::fabs(lg::text_em(mixed) - (lg::text_em("A") + 3.0 + lg::text_em(cyr) + lg::text_em("z"))) < 1e-9, "mixed text adds up");
    // cut_em: on a character boundary, fits, ends with the ellipsis
    for (double em : { 2.5, 3.5, 4.5, 6.0 }) {
        const std::string cj = lg::cut_em(cjk + cjk + cjk, em);
        check(valid_utf8(cj) && lg::text_em(cj) <= em + 1e-9 && cj.size() >= kEll.size() && cj.substr(cj.size() - kEll.size()) == kEll,
              "cut_em of CJK to " + std::to_string(em) + " em: valid, fits, ellipsis");
        const std::string cy = lg::cut_em(cyr + cyr + cyr, em);
        check(valid_utf8(cy) && lg::text_em(cy) <= em + 1e-9 && cy.substr(cy.size() - kEll.size()) == kEll,
              "cut_em of Cyrillic to " + std::to_string(em) + " em: valid, fits, ellipsis");
    }
    eq(lg::cut_em(cjk, 3.0), cjk, "CJK that fits is kept whole");

    // ---- the Community row and title in another language (words given as templates)
    model::labels::ComWords w;
    w.by = "%s \xE3\x81\xAB\xE3\x82\x88\xE3\x82\x8B";               // "<author> ni-yoru"
    w.slots = "%d \xE3\x82\xB9\xE3\x83\xAD\xE3\x83\x83\xE3\x83\x88";   // "<n> surotto"
    w.downloads = "DL %d";
    w.mark_upd = "NEW";
    w.mark_dl = "OK";
    w.window_title = "%s / %s";
    w.page = "%d of %d";
    const json it = com_item("Combo", "Ann", "2026-10-24", 4, 3, 7);
    std::string lab;
    check(model::labels::community_label(it, json(), &lab, w), "the label is built with the words");
    eq(lab, std::string("Combo v1  Ann \xE3\x81\xAB\xE3\x82\x88\xE3\x82\x8B  2026-10-24  |  4 \xE3\x82\xB9\xE3\x83\xAD\xE3\x83\x83\xE3\x83\x88  +3  DL 7"), "the row in the words' order");
    std::string en_lab;
    model::labels::community_label(it, json(), &en_lab);
    eq(en_lab, std::string("Combo v1  by Ann  2026-10-24  |  4 slots  +3  7DL"), "the default words are the English ones");
    json dv = 1;
    json up = it; up["version"] = 2;
    model::labels::community_label(up, dv, &lab, w);
    check(lab.rfind("<color FFAA00>NEW</color> ", 0) == 0, "the UPD mark's word: " + lab);
    model::labels::community_label(it, dv, &lab, w);
    check(lab.rfind("<color 00FF00>OK</color> ", 0) == 0, "the DL mark's word: " + lab);
    model::labels::ComWords bad;
    bad.by = "%d by";                                                                         // cannot be filled with a name
    bad.slots = "%q";
    bad.downloads = "%s %s";
    model::labels::community_label(it, json(), &lab, bad);
    eq(lab, en_lab, "a template that cannot be filled: the English one");
    eq(model::labels::community_title("New", "Ryu", 2, 5, false, w), std::string("New / Ryu\n<ICON KeyA>   2 of 5   <ICON KeyE>"), "title with the words");
    eq(model::labels::community_title("New", "Ryu", 2, 5, true, w), std::string("New / Ryu\n<ICON BtnL1>   2 of 5   <ICON BtnR1>"), "title with the words, pad glyphs");
    eq(model::labels::community_title("New", "Ryu", 1, 1), std::string("New  Ryu Drills\n<ICON KeyA>   Page 1/1   <ICON KeyE>"), "title: the English default");
    // the fit works on the words too: a label in the words of another language still fits its row
    const json longit = com_item("Post-throw slop simulator (updated) [TEST]", "Chris_F (drillcodes)", "2026-10-24", 4, 0, 0);
    std::string full;
    model::labels::community_label(longit, json(), &full, w);
    const std::string fitted = lg::com_row_fit(longit, json(), full, lg::kComRowEm, w);
    check(lg::text_em(full) > lg::kComRowEm && lg::text_em(fitted) <= lg::kComRowEm && has(fitted, "DL 4") == false && has(fitted, "DL 0"), "the fit with the words: it fits, the words stay: " + fitted);
    check(valid_utf8(fitted), "the fitted label is valid UTF-8");
    // a long Japanese author: cut on a character, valid
    const json jp = com_item("Combo", cjk + cjk + cjk + cjk + cjk + cjk + cjk + cjk + cjk + cjk, "2026-10-24", 4, 0, 0);
    std::string jfull;
    model::labels::community_label(jp, json(), &jfull, w);
    const std::string jfit = lg::com_row_fit(jp, json(), jfull, 30.0, w);
    check(valid_utf8(jfit) && lg::text_em(jfit) <= 30.0 + 1e-9 && lg::text_em(jfull) > 30.0, "a long CJK author: cut on a character, fits: " + jfit);

    // ---- the slot wording when the game's is not learnt
    eq(lg::slot_text(false, "", "", 14, "Emplacement 14"), std::string("Emplacement 14"), "not learnt: the locale's wording");
    eq(lg::slot_text(true, "S ", "", 14, "Emplacement 14"), std::string("S 14"), "learnt: the game's wording first");
    // the loading word of a port
    struct P : lg::ComPort {
        void page_open() override {} std::string title(bool) override { return ""; } bool loading() override { return true; }
        int row_count() override { return 0; } std::string row_label(int) override { return ""; } std::string row_desc(int) override { return ""; }
        bool data_changed() override { return false; } void mark_built() override {} int64_t version() override { return 0; }
        bool prev_page() override { return false; } bool next_page() override { return false; } void cycle_sort() override {}
        void vote(int) override {} bool download_prompt(int, std::string*) override { return false; } void download_confirmed() override {}
        void show(const std::string&, const std::vector<std::string>& l, const std::vector<std::string>&) override { shown = l; }
        void open_menu() override {} void navigate(int) override {} int focused_row() override { return 0; }
        void confirm(const std::string&, std::function<void(bool)>) override {}
        std::string loading_word() override { return "..."; }
        std::vector<std::string> shown;
    } port;
    lg::ComDialog dlg(port);
    dlg.open();
    check(!port.shown.empty() && port.shown[0] == "...", "the dialog shows the port's loading word");

    // ---- capitals of the details' headers (UTF-8)
    eq(dt::upper_utf8("page 1/2"), std::string("PAGE 1/2"), "upper: ASCII");
    eq(dt::upper_utf8("p\xC3\xA1gina \xC3\xA7 \xC3\xBC \xC3\xB1 \xC3\xA0 \xC3\xAA"), std::string("P\xC3\x81GINA \xC3\x87 \xC3\x9C \xC3\x91 \xC3\x80 \xC3\x8A"), "upper: Latin-1 letters");
    eq(dt::upper_utf8("\xC3\xBF \xC3\xB7 \xC3\x9F"), std::string("\xC5\xB8 \xC3\xB7 \xC3\x9F"), "upper: y-diaeresis to Y-diaeresis, the division sign and sharp s unchanged");
    eq(dt::upper_utf8("\xC5\x82\xC3\xB3""d\xC5\xBA \xC4\x85\xC4\x99 \xC5\x9B\xC4\x87 \xC5\xBC \xC5\x84"), std::string("\xC5\x81\xC3\x93""D\xC5\xB9 \xC4\x84\xC4\x98 \xC5\x9A\xC4\x86 \xC5\xBB \xC5\x83"), "upper: Polish letters (Latin Extended-A)");
    eq(dt::upper_utf8("\xC5\x9F \xC4\x9F \xC4\xB1 \xC4\xB0 \xC5\xA1 \xC5\xBE \xC4\x8D \xC4\x9B"), std::string("\xC5\x9E \xC4\x9E I \xC4\xB0 \xC5\xA0 \xC5\xBD \xC4\x8C \xC4\x9A"), "upper: Turkish and Czech letters");
    eq(dt::upper_utf8("\xD1\x81\xD1\x82\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x86\xD0\xB0 \xD1\x91 \xD1\x8F"), std::string("\xD0\xA1\xD0\xA2\xD0\xA0\xD0\x90\xD0\x9D\xD0\x98\xD0\xA6\xD0\x90 \xD0\x81 \xD0\xAF"), "upper: Cyrillic");
    eq(dt::upper_utf8(cjk + hangul + "\xD8\xB5\xD9\x81\xD8\xAD\xD8\xA9"), cjk + hangul + "\xD8\xB5\xD9\x81\xD8\xAD\xD8\xA9", "upper: CJK, Hangul, Arabic unchanged");
    eq(dt::upper_utf8(emoji + "a"), emoji + "A", "upper: a 4-byte character kept, the rest raised");
    eq(dt::upper_utf8("\xE9"), std::string("\xE9"), "upper: a stray byte kept");
    eq(dt::upper_utf8(""), std::string(""), "upper: empty");
    const dt::Tr fr_t = [](const std::string& key, const std::vector<std::string>&) {
        return key == "details_page" ? std::string("p\xC3\xA1gina 1/2") : key == "details_saved_one" ? std::string("1 espacio guardado \xC3\xA9") : std::string("\xD1\x81\xD0\xBB\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2 %d");
    };
    eq(dt::header_text(fr_t, 0, 2), std::string("P\xC3\x81GINA 1/2"), "header_text raises accented letters");
    eq(dt::saved_text(fr_t, 1), std::string("1 ESPACIO GUARDADO \xC3\x89"), "saved_text raises accented letters");
    check(dt::saved_text(fr_t, 5).rfind("\xD0\xA1\xD0\x9B\xD0\x9E\xD0\xA2\xD0\x9E\xD0\x92", 0) == 0, "saved_text raises Cyrillic");
}

void test_com_fit() {
    group("community row fit");
    check(lg::text_em("") == 0.0 && lg::text_em("<color 00FF00>DL</color> ") == lg::text_em("DL "), "markup tags draw nothing");
    check(lg::text_em("i") < lg::text_em("m") && lg::text_em(kEll) == 1.0 && lg::text_em("\xE3\x83\xAA") == 1.0, "glyph widths: i < m, the ellipsis and CJK 1 em");
    check(lg::text_em("a < b") > lg::text_em("a  b"), "a lone '<' is a character");
    eq(lg::cut_em("short", 10), std::string("short"), "cut_em: what fits is kept");
    const std::string c = lg::cut_em("Post-throw slop simulator", 8);
    check(lg::text_em(c) <= 8.0 && c.size() > kEll.size() && c.substr(c.size() - kEll.size()) == kEll && c[c.size() - kEll.size() - 1] != ' ', "cut_em: fits, ends with the ellipsis, no space before it");

    // Wael 24/10, seen in game: both overflowed their row on both sides
    const json a = com_item("Post-throw slop simulator (updated) [TEST]", "Chris_F (drillcodes)", "2026-10-24", 4, 0, 0);
    const json b = com_item("Throw Slop Simulator [TEST]", "Chris_F (drillcodes)", "2026-10-24", 4, 0, 0);
    eq(com_full(a), std::string("Post-throw slop simulator (updated) [TEST] v1  by Chris_F (drillcodes)  2026-10-24  |  4 slots  +0  0DL"), "the label as built (community_label)");
    check(lg::text_em(com_full(a)) > lg::kComRowEm && lg::text_em(com_full(b)) > lg::kComRowEm, "both are wider than a row");
    const std::string fa = com_fit(a), fb = com_fit(b);
    check(lg::text_em(fa) <= lg::kComRowEm && lg::text_em(fb) <= lg::kComRowEm, "both fit");
    check(!has(fa, "2026-10-24") && !has(fb, "2026-10-24"), "the date goes first");
    eq(fb, std::string("Throw Slop Simulator [TEST] v1  by Chris_F (drillcodes)  |  4 slots  +0  0DL"), "the short one: without the date it fits, its name whole");
    check(lg::text_em(fb) * lg::kComPxPerEm <= 780.0, "...within the 780 px measured in game");
    eq(com_fit(b, json(), 34.0), std::string("Throw Slop Simulator [TEST] v1  by Chris_F (drillcodes)  |  4 slots  +0  0DL"), "a little more room: without the date it fits, the name whole");
    check(has(fa, "Post-throw slop") && has(fa, kEll + " [TEST] v1  by Chris_F (drillcodes)  |  4 slots  +0  0DL"), "the long one: its name cut with \"…\" before its [TEST] tag, version, author, slots, score, DL kept");
    const json kc = com_item("Ken_Cheepz_Changed2", "Wael3rd_PC", "2026-10-12", 10, 0, 1);
    eq(com_fit(kc), std::string("Ken_Cheepz_Changed2 v1  by Wael3rd_PC  2026-10-12  |  10 slots  +0  1DL"), "734 px in game (it fitted): kept whole, date included");
    eq(com_fit(com_item("Ninja [x] tag", "Bob", "2026-10-12", 1, 0, 0)), com_full(com_item("Ninja [x] tag", "Bob", "2026-10-12", 1, 0, 0)), "a tag inside a name: nothing special");

    const json ok = com_item("Ryu combos", "Wael", "2026-10-14", 12, 3, 5);
    eq(com_fit(ok), com_full(ok), "a row that fits is left as it is (date kept)");
    const json upd = com_item("Post-throw slop simulator (updated) [TEST]", "Chris_F (drillcodes)", "2026-10-24", 30, 12, 140);
    const std::string fu = com_fit(upd, json(0));                // downloaded v0 < v1: UPD mark
    check(fu.rfind("<color FFAA00>UPD</color> ", 0) == 0 && has(fu, "30 slots  +12  140DL") && lg::text_em(fu) <= lg::kComRowEm, "the UPD mark, slots, score, downloads stay");
    json v = com_item("Throw Slop Simulator [TEST]", "A very long player name for sure <color 66DD66>[V]</color>", "2026-10-24", 4, 0, 0);
    const std::string fv = com_fit(v, json(), 30.0);
    check(has(fv, "<color 66DD66>[V]</color>") && has(fv, kEll + " <color 66DD66>[V]</color>") && lg::text_em(fv) <= 30.0, "a long author is cut last, its verified mark kept whole");
    const std::string tight = com_fit(a, json(), 10.0);
    check(has(tight, "Post-throw s") && has(tight, "|  4 slots  +0  0DL"), "no room at all: the name keeps its 12 characters, the counts stay");
    json named_json = com_item("Ken_Test.json", "Bob", "2026-10-24", 4, 0, 0);
    eq(com_fit(named_json), com_full(named_json), "the .json of a name is dropped as before");

    group("descriptions");
    const std::string drill = "Pick Ken and throw him. He will auto break it. Then, STOP THE SLOP";
    auto d = lg::desc_lines(drill, lg::kComRowEm, 2);
    check(d.size() == 1 && d[0] == drill, "a drill's description on one line");
    d = lg::desc_lines("Line one\r\n\r\n  Line\ttwo <b>  \n\n", 40, 3);
    check(d.size() == 2 && d[0] == "Line one" && d[1] == "Line two (b)", "\\r dropped, empty lines dropped, tabs as spaces, markup made plain");
    eq(lg::desc_line("a [x] <y>", 80), std::string("a (x) (y)"), "square brackets (not drawn by the game) and markup as parentheses");
    d = lg::desc_lines(drill + " " + drill + " " + drill, 20, 2);
    check(d.size() == 2 && lg::text_em(d[0]) <= 20 && lg::text_em(d[1]) <= 20 && d[1].substr(d[1].size() - kEll.size()) == kEll, "wrapped on words, the last kept line ends with \"…\"");
    check(d[0].rfind("Pick Ken", 0) == 0 && d[0].back() != ' ', "wrapped at a space");
    d = lg::desc_lines(std::string(200, 'W'), 10, 2);
    check(d.size() == 2 && lg::text_em(d[0]) <= 10 && d[1].substr(d[1].size() - kEll.size()) == kEll, "a word wider than a line is split");
    check(lg::desc_lines("", 30, 2).empty() && lg::desc_lines(" \n \n", 30, 2).empty() && lg::desc_lines("x", 30, 0).empty(), "nothing to show");

    eq(lg::desc_line("Pick Ken and throw him.\n\nThen, STOP THE SLOP", 80), std::string("Pick Ken and throw him. | Then, STOP THE SLOP"), "one line: the paragraphs joined");
    const std::string one = lg::desc_line(drill + "\n" + drill + "\n" + drill, 40);
    check(lg::text_em(one) <= 40 && one.substr(one.size() - kEll.size()) == kEll && has(one, " | "), "one line: filled, then cut with \"…\"");
    {   // the help line's pages (no "…" cut): word boundaries, a counter, no separator at a page's edge
        eq(lg::help_pages("Pick Ken.\n\nThen stop.", 80).size(), (size_t)1, "a short description: one page");
        eq(lg::help_pages("Pick Ken.\n\nThen stop.", 80)[0], std::string("Pick Ken. | Then stop."), "one page: no counter");
        check(lg::help_pages("", 80).empty() && lg::help_pages(" \n ", 80).empty(), "nothing to show: no page");
        const std::string longd = drill + "\n" + drill + "\n" + drill + "\n" + drill;
        const auto hp = lg::help_pages(longd, 40);
        bool fits = hp.size() >= 3, counted = true, edges = true;
        std::string all;
        for (size_t i = 0; i < hp.size(); ++i) {
            fits = fits && lg::text_em(hp[i]) <= 40;
            const std::string tail = "  (" + std::to_string(i + 1) + "/" + std::to_string(hp.size()) + ")";
            counted = counted && hp[i].size() > tail.size() && hp[i].substr(hp[i].size() - tail.size()) == tail;
            const std::string body = hp[i].substr(0, hp[i].size() - tail.size());
            edges = edges && body.rfind("|", 0) != 0 && body.back() != '|' && body.front() != ' ';
            all += (all.empty() ? "" : " ") + body;
        }
        check(fits, "every page within the help line (" + std::to_string(hp.size()) + " pages)");
        check(counted, "each page ends with its counter (n/N)");
        check(edges, "no page starts or ends with the separator");
        check(all.find("\xE2\x80\xA6") == std::string::npos && all.find("STOP THE SLOP") != std::string::npos, "nothing cut with an ellipsis");
    }
    eq(lg::com_guide_text({ "Full name", "Desc" }, "G"), std::string("Full name\nDesc\n\nG"), "the description area: lines, a blank line, the guide line");
    eq(lg::com_guide_text({}, "G"), std::string("G"), "no description: the guide line alone");
    eq(lg::com_guide_text({ "" }, "G"), std::string("G"), "only empty lines: the guide line alone");
    const std::vector<std::string> six{ "a", "b", "c", "d", "e", "f" };
    check(lg::com_desc_page(six, 8, 0) == six && lg::com_desc_page(six, 6, 3) == six, "one page: the lines as they are, no page mark");
    check(lg::com_desc_page(six, 4, 0) == std::vector<std::string>{ "a", "b", "c", "d  (1/2)" }, "page 1 of 2 marked on its last line");
    check(lg::com_desc_page(six, 4, 1) == std::vector<std::string>{ "e", "f  (2/2)" } && lg::com_desc_page(six, 4, 2) == lg::com_desc_page(six, 4, 0), "the last page, then the first again");
    check(lg::com_desc_page({}, 4, 5).empty() && lg::com_desc_page(six, 0, 1) == std::vector<std::string>{ "b  (2/6)" }, "no line: no page; 0 a page counts as 1");
    // the drill the 27/10 report showed (drillcodes: paragraphs, then the credit line): whole in one page now
    const std::string credit = "Drill by Chris_F, from drillcodes.com (made with Training Mode Drill Share by higorae), shared here with permission.";
    const auto dl = lg::desc_lines("Pick Ken and throw him. He will auto break it.\nThen, STOP THE SLOP\n" + credit, lg::kComDescEm, lg::kComDescMaxLines);
    check(dl.size() <= (size_t)lg::kComDescLines && dl.size() >= 3 && dl[1] == "Then, STOP THE SLOP" && dl.back().find("\xE2\x80\xA6") == std::string::npos,
          "the drill's description and credit line fit one page, nothing cut");
    for (const std::string& l : dl) check(lg::text_em(l) <= lg::kComDescEm, "each line within the area's width");
    float y = 0;
    check(lg::keep_bottom_y(8, 300, 70, 150, &y) && y == 300, "ControlPoint LeftBottom: the bottom edge is the point, it stays");
    check(lg::keep_bottom_y(0, 230, 70, 150, &y) && y == 150, "ControlPoint LeftTop: moved up by the growth");
    check(lg::keep_bottom_y(5, 265, 70, 150, &y) && y == 225, "ControlPoint CenterCenter: moved up by half the growth");
    check(!lg::keep_bottom_y(12, 0, 70, 150, &y) && !lg::keep_bottom_y(3, 0, 70, 150, &y) && !lg::keep_bottom_y(-1, 0, 70, 150, &y), "an unknown ControlPoint: refused");
    check(lg::guide_w_fits("<ICON KeyF> Confirm", 110.281, 23) && lg::guide_w_fits("<ICON KeyEsc><ICON MouseR> Back", 110.727, 23),
          "the game's two guide items as dumped (27/10) fit the estimate");
    check(!lg::guide_w_fits("<ICON KeyEsc><ICON MouseR> Back", 500, 23) && !lg::guide_w_fits("<ICON KeyF> Confirm", 20, 23) && !lg::guide_w_fits("", 100, 23),
          "an item not fitted yet (a default width) does not");
    const auto long_d = lg::desc_lines(std::string(1000, 'x') + " " + drill + " " + drill, lg::kComDescEm, lg::kComDescMaxLines);
    check(long_d.size() > (size_t)lg::kComDescLines && long_d.size() <= (size_t)lg::kComDescMaxLines && long_d.back().find("\xE2\x80\xA6") == std::string::npos,
          "a 1000-character description: several pages, nothing cut");

    group("pack info");
    json pack = json::array({ json{{"id", 2}, {"empty", true}}, json{{"id", 1}, {"empty", true}, {"info", json{{"description", "D"}, {"source", "drillcodes.com"}}}} });
    eq(model::pack::description(pack), std::string("D"), "entry 1 found by its id");
    pack[1]["info"] = json{{"source", "community"}};
    eq(model::pack::description(pack), std::string(), "an info without description");
    eq(model::pack::description(json::array({ json{{"id", 1}, {"empty", true}} })), std::string(), "no info");
    eq(model::pack::description(json::object()), std::string(), "not a pack");
}

// ---- the text dialog -----------------------------------------------------------------------------------------------------
void test_naming() {
    group("text dialog");
    lg::NameJob j;
    j.initial = "Combo";
    j.cancelled = true; j.decided = true; j.raw = "x";
    check(!lg::naming_outcome(j, std::string("y")), "cancel: nothing");
    j = lg::NameJob{};
    check(!lg::naming_outcome(j, std::nullopt), "an unnamed slot, closed with nothing typed, no decide: nothing");
    j.initial = "Combo";
    check(!lg::naming_outcome(j, std::string("Combo")), "closed, the field still the initial text: nothing");
    auto q = lg::naming_outcome(j, std::nullopt);
    check(q && q->empty(), "closed with the field unreadable: \"\" applied (the Lua's `... or \"\"`, kept)");
    auto o = lg::naming_outcome(j, std::string("Combo B"));
    check(o && *o == "Combo B", "closed after typing, no decide: applied as typed");
    j.decided = true;
    o = lg::naming_outcome(j, std::nullopt);
    check(o && o->empty(), "a decide with an empty field: the empty text (a slot name is cleared)");
    j.raw = "Combo";
    o = lg::naming_outcome(j, std::string("zzz"));
    check(o && *o == "Combo", "a decide with the initial text still applies");
    j = lg::NameJob{}; j.initial = "a"; j.decided = true; j.raw = std::string(); j.result = "b";
    o = lg::naming_outcome(j, std::string("c"));
    check(o && o->empty(), "raw \"\" wins over the result (Lua `or`: an empty string is a value)");
    j.raw.reset();
    o = lg::naming_outcome(j, std::string("c"));
    check(o && *o == "b", "then the result");
    j.result.reset();
    o = lg::naming_outcome(j, std::string("c"));
    check(o && *o == "c", "then the field");
    lg::NameJob d; d.initial = "x";
    lg::naming_decide(d, std::string("typed"));
    check(d.decided && d.raw && *d.raw == "typed", "a decide takes the field when raw is absent");
    d.raw = std::string("first");
    lg::naming_decide(d, std::string("second"));
    check(*d.raw == "first", "a decide keeps a non-empty raw");
    d.raw = std::string();
    lg::naming_decide(d, std::string("third"));
    check(*d.raw == "third", "a decide replaces an empty raw");
    lg::NameJob e; e.initial = "x";
    lg::naming_decide(e, std::nullopt);
    check(e.decided && !e.raw, "a decide with an empty field leaves raw absent");

    group("keys");
    lg::KeyEdge k;
    check(k.press(true) && !k.press(true) && !k.press(false) && k.press(true), "one press per down edge");
    k.reset();
    check(k.press(true), "after a reset a held key fires again (as the Lua's flags reset at the dialog close)");
}

// ---- the Community dialog ------------------------------------------------------------------------------------------------
// A model of community::Page and of the option window, enough to drive ComDialog.
struct FakePort final : lg::ComPort {
    // page
    std::vector<std::string> items;                  // all items (labels)
    std::set<int> downloaded;                        // item indexes downloaded (0-based)
    int page = 1, sort = 1;
    bool loading_ = false;
    int64_t data_ver = 0, built_ver = 0;
    std::vector<std::string> rows_;                  // the rows of the current page
    int votes = 0, last_vote_row = -1, prompts = 0, downloads = 0, opens = 0, fetches = 0;
    bool busy = false;
    std::vector<std::string> tickers;
    // window
    bool window_open = false;
    std::vector<std::string> shown_labels;
    std::string shown_title;
    int focus = 1;                                   // the cursor row (1-based) the game holds
    std::vector<int> navigations;                    // focus0 of each rebuild
    int menus = 0;
    std::function<void(bool)> pending_confirm;

    void rebuild() {
        ++data_ver;
        rows_.clear();
        for (int i = 0; i < 7; ++i) { const size_t at = (size_t)(page - 1) * 7 + (size_t)i; if (at < items.size()) rows_.push_back(items[at]); }
    }
    int pages() const { return std::max(1, (int)((items.size() + 6) / 7)); }
    void answer(std::vector<std::string> list) { loading_ = false; items = std::move(list); page = 1; rebuild(); }

    void page_open() override { ++opens; sort = 1; page = 1; ++fetches; loading_ = true; }
    std::string title(bool pad) override { return std::string(pad ? "P" : "S") + std::to_string(sort) + " p" + std::to_string(page) + "/" + std::to_string(pages()); }
    bool loading() override { return loading_; }
    int row_count() override { return (int)rows_.size(); }
    std::string row_label(int row) override { return rows_[(size_t)row - 1]; }
    std::string row_desc(int row) override { return "F: Download  |  " + rows_[(size_t)row - 1]; }
    bool data_changed() override { return data_ver != built_ver && !loading_; }
    void mark_built() override { built_ver = data_ver; }
    int64_t version() override { return data_ver; }
    bool prev_page() override { if (page <= 1) return false; --page; rebuild(); built_ver = data_ver; return true; }
    bool next_page() override { if (page >= pages()) return false; ++page; rebuild(); built_ver = data_ver; return true; }
    void cycle_sort() override { sort = sort % 3 + 1; page = 1; if (!loading_) { loading_ = true; ++fetches; } }
    void vote(int row) override {
        if (row < 1 || row > (int)rows_.size()) { tickers.push_back("No item"); return; }
        ++votes; last_vote_row = row;
    }
    void vote_answer() { rebuild(); built_ver = data_ver; tickers.push_back("Liked"); }   // Page::vote's callback
    bool download_prompt(int row, std::string* message) override {
        if (row < 1 || row > (int)rows_.size()) return false;
        if (busy) { tickers.push_back("Busy, try again"); return false; }
        ++prompts;
        *message = "Download \"" + rows_[(size_t)row - 1] + "\"?";
        return true;
    }
    void download_confirmed() override { ++downloads; ++fetches; loading_ = true; }
    void show(const std::string& t, const std::vector<std::string>& labels, const std::vector<std::string>&) override { shown_title = t; shown_labels = labels; }
    void open_menu() override { ++menus; window_open = true; focus = 1; }
    void navigate(int focus0) override { if (!window_open) return; navigations.push_back(focus0); focus = focus0 + 1; }
    int focused_row() override { return window_open ? focus : 0; }
    void confirm(const std::string&, std::function<void(bool)> done) override { pending_confirm = std::move(done); }
};

std::vector<std::string> make_items(int n, const char* tag) {
    std::vector<std::string> v;
    for (int i = 1; i <= n; ++i) v.push_back(std::string(tag) + std::to_string(i));
    return v;
}

void test_community() {
    group("community dialog");
    FakePort p;
    lg::ComDialog d(p);
    // a previous session's rows stay visible until the answer (as v1.6)
    d.open();
    check(d.is_open() && p.opens == 1 && p.menus == 1, "Download Drills opens the dialog and fetches");
    bool all_loading = p.shown_labels.size() == 7;
    for (const auto& s : p.shown_labels) all_loading = all_loading && s == "Loading...";
    check(all_loading, "no rows yet, loading: every row reads Loading... (RMT:300-303)");
    d.tick(false, false, false, false);
    check(p.navigations.empty(), "nothing to rebuild while loading");
    // the user moves the cursor to row 4 while the list loads; the answer arrives
    p.focus = 4;
    p.answer(make_items(20, "A"));
    d.tick(false, false, false, false);
    check(p.navigations.size() == 1 && p.navigations.back() == 3, "the answer rebuilds in place and KEEPS the cursor on row 4 (the fix; the Lua went to row 1)");
    check(p.shown_labels[0] == "A1" && p.shown_labels[6] == "A7", "page 1 rows");
    check(p.shown_title == "S1 p1/3", "title: sort, page 1 of ceil(20/7) = 3");
    d.tick(false, false, false, false);
    check(p.navigations.size() == 1, "no second rebuild without new data");
    // E: next page, cursor to row 1
    d.tick(false, true, false, false);
    check(p.page == 2 && p.navigations.back() == 0 && p.shown_labels[0] == "A8", "E: page 2, cursor to row 1");
    d.tick(false, true, false, false);
    check(p.page == 2, "E held: one page per press");
    d.tick(false, false, false, false);
    d.tick(false, true, false, false);
    d.tick(false, false, false, false);
    d.tick(false, true, false, false);
    check(p.page == 3, "no page past the last");
    check(p.shown_labels[5] == "A20" && p.shown_labels[6] == "--", "the last page's empty rows read --");
    // A + E together: E ignored while A is held
    d.tick(false, false, false, false);
    d.tick(true, true, false, false);
    check(p.page == 2, "A with E held: one page back only");
    d.tick(false, false, false, false);
    d.tick(true, false, false, false);
    check(p.page == 1, "A: back to page 1");
    d.tick(false, false, false, false);
    d.tick(true, false, false, false);
    check(p.page == 1, "no page before the first");
    // R: next sort, page 1, refetch, cursor to row 1 at once; the old rows stay until the answer
    p.focus = 5;
    const size_t navs = p.navigations.size();
    d.tick(false, false, false, true);
    check(p.sort == 2 && p.page == 1 && p.loading_ && p.navigations.size() == navs + 1 && p.navigations.back() == 0, "R: sort Popular, page 1, cursor to row 1");
    check(p.shown_labels[0] == "A1", "R: the previous sort's rows stay visible until the answer");
    p.focus = 6;                                     // the user moved while it loads
    p.answer(make_items(9, "B"));
    d.tick(false, false, false, false);
    check(p.navigations.back() == 5 && p.shown_labels[0] == "B1", "the sort's answer rebuilds and keeps the cursor on row 6");
    // T: like the focused row; the answer rebuilds on the voted row
    p.focus = 2;
    d.tick(false, false, true, false);
    check(p.votes == 1 && p.last_vote_row == 2, "T votes for the focused row");
    p.focus = 3;                                     // moved before the answer
    p.vote_answer();
    d.tick(false, false, false, false);
    check(p.navigations.back() == 1, "the vote's answer rebuilds on the voted row (RMT:388)");
    d.tick(false, false, false, false);
    const size_t n2 = p.navigations.size();
    d.tick(false, false, false, false);
    check(p.navigations.size() == n2, "no rebuild after a vote's rebuild");
    // F: confirm then download; the refetch's answer keeps the cursor
    p.focus = 2;
    d.button(2);
    check(p.prompts == 1 && p.pending_confirm != nullptr, "F asks with the game's box");
    p.pending_confirm(false);
    check(p.downloads == 0, "No: nothing downloaded");
    d.button(2);
    p.pending_confirm(true);
    check(p.downloads == 1 && p.loading_, "Yes: the download, then the list again");
    p.focus = 7;
    p.answer(make_items(9, "B"));
    d.tick(false, false, false, false);
    check(p.navigations.back() == 6, "after a download the refreshed list keeps the cursor (row 7)");
    d.button(9);
    check(p.prompts == 2, "F on an empty row: nothing");
    p.busy = true;
    d.button(1);
    check(p.tickers.back() == "Busy, try again" && p.prompts == 2, "F while a request is in flight: Busy");
    p.busy = false;
    // close: keys reset, no work
    d.closed();
    check(!d.is_open(), "closed");
    const size_t n3 = p.navigations.size();
    d.tick(false, true, false, true);
    check(p.navigations.size() == n3 && p.page == 1, "closed: keys do nothing");
    // reopen with A held: the edge fires (the Lua's flags were reset at the close)
    p.answer(make_items(30, "C"));
    p.next_page();                                   // page 2 before the open (open resets to 1)
    d.open();
    check(p.page == 1 && p.sort == 1, "reopen: sort New, page 1");
    p.answer(make_items(30, "C"));
    p.focus = 1;
    d.tick(false, false, false, false);
    d.tick(false, true, false, false);
    check(p.page == 2, "reopen: E works");
    // the pad's glyphs: a change of device rebuilds the title in place on the cursor's row, and only then
    p.answer(make_items(20, "D"));
    d.tick(false, false, false, false);
    check(p.shown_title.rfind("S", 0) == 0, "keyboard: the title keeps its keys");
    p.focus = 4;
    const size_t n4 = p.navigations.size();
    d.tick(false, false, false, false, true);
    check(p.shown_title.rfind("P", 0) == 0 && p.navigations.size() == n4 + 1 && p.navigations.back() == 3, "pad: the title's glyphs follow, cursor kept on row 4");
    d.tick(false, false, false, false, true);
    check(p.navigations.size() == n4 + 1, "pad: no rewrite while the device stays");
    d.tick(false, false, false, false, false);
    check(p.shown_title.rfind("S", 0) == 0 && p.navigations.size() == n4 + 2, "back to the keyboard: the keys again");
    // opened on a pad: the first render has the pad's glyphs, so the first tick rebuilds nothing (in game that rebuild
    // replaced the dialog the open had just shown, and 2.2.2's first build lost it, stuck on "Loading...")
    d.closed();
    d.open(true);
    check(p.shown_title.rfind("P", 0) == 0, "opened on a pad: the pad's glyphs from the first render");
    const size_t n5 = p.navigations.size();
    d.tick(false, false, false, false, true);
    check(p.navigations.size() == n5, "opened on a pad: no rebuild at the first tick");
    eq(model::labels::community_title("New", "Ken", 2, 5), std::string("New  Ken Drills\n<ICON KeyA>   Page 2/5   <ICON KeyE>"), "title, keyboard glyphs");
    eq(model::labels::community_title("New", "Ken", 2, 5, true), std::string("New  Ken Drills\n<ICON BtnL1>   Page 2/5   <ICON BtnR1>"), "title, pad glyphs");
    eq(model::labels::community_guide(false), std::string("<ICON KeyT> Like   <ICON KeyR> Sort"), "guide line, keyboard glyphs");
    eq(model::labels::community_guide(true), std::string("<ICON BtnU> Like   <ICON BtnL> Sort"), "guide line, pad glyphs (Y / X)");
}

// ---- the pause menu's tab under CRSM's dialogs (L1+R1 / A+E) ------------------------------------------------------------
// A model of the game: the data's tab / cursor row and the chain an L1+R1 jump runs (the data written, then CreateViewData
// and InitTabItem, whose pre-hooks call the guard), the game's own L1 / R1 when no dialog is up, the frame callback.
struct TabSim {
    lg::TabGuard g;
    lg::TabSpot data{ 3, 4 };                    // Recording tab, the Manager's "Download Drills" row
    lg::TabSpot built{ 3, 4 };                   // what the last rebuild read (the page shown)
    bool modal = false;
    int fixes = 0;
    void apply(const lg::TabFix& f) {
        if (f.tab) { data.tab = f.to.tab; ++fixes; }
        if (f.child) { data.child = f.to.child; ++fixes; }
    }
    void frame() { apply(g.check(modal, data, lg::TabAt::Frame)); }
    void rebuild(int64_t fallback = -1) {        // CreateViewData pre, the rebuild, then InitTabItem pre
        apply(g.check(modal, data, lg::TabAt::Rebuild, fallback));
        built = data;
        apply(g.check(modal, data, lg::TabAt::Items, fallback));
    }
    void jump(int64_t to) { data = lg::TabSpot{ to, 0 }; rebuild(); }   // L1+R1: the tab written, the page rebuilt
};

// The game as read in the exe (build of 13/10, logic.hpp): the primary tab list (8 tabs, a looping ScrollList whose
// selected item sits 2 places after the tab) is the real tab: L1 / R1 move it and its change handler writes the data from
// it; the L1+R1 shortcut writes the data (tab 0, row 12), then the list's setSelectedIndex(2) (through the DLL's pre-hook
// when it is in place), then the rebuild (CreateViewData, InitTabItem).
struct GameTabs {
    static constexpr int64_t kTabs = 8, kCentre = 2;
    lg::TabGuard g;
    lg::TabSpot data{ 3, 4, 5 };                 // Recording (tab 3), its list item 5
    lg::TabSpot built{ 3, 4, 5 };
    bool modal = false, hook = true, read_list = true;
    int blocked = 0;
    void select(int64_t v) {
        if (hook && lg::block_list_select(modal, true, g.holding() ? g.held().list : -1, v)) { ++blocked; return; }
        data.list = v;
    }
    void apply(const lg::TabFix& f) {
        if (f.tab) data.tab = f.to.tab;
        if (f.child) data.child = f.to.child;
        if (f.list) select(f.to.list);
    }
    void frame() {
        lg::TabSpot now = data;
        if (!read_list) now.list = -1;           // (2.0.1's first fix: the list was not read)
        apply(g.check(modal, now, lg::TabAt::Frame));
    }
    void rebuild() {
        apply(g.check(modal, lg::TabSpot{ data.tab, data.child, -1 }, lg::TabAt::Rebuild));
        built = data;
        apply(g.check(modal, lg::TabSpot{ data.tab, data.child, -1 }, lg::TabAt::Items));
    }
    void shortcut() { data.tab = 0; data.child = 12; select(kCentre); rebuild(); }
    void step(int dir) {                         // R1 / E (+1) or L1 / A (-1), the game's own (no dialog of CRSM up)
        data.list = ((data.list + dir) % kTabs + kTabs) % kTabs;
        data.tab = ((data.list - kCentre) % kTabs + kTabs) % kTabs;
        rebuild();
    }
    bool consistent() const { return ((data.list - kCentre) % kTabs + kTabs) % kTabs == data.tab; }
};

void test_tab_list() {
    group("tab list (L1+R1)");
    check(!lg::block_list_select(false, true, 5, 2), "no dialog: the list moves");
    check(!lg::block_list_select(true, false, 5, 2), "another list of the game: never blocked");
    check(lg::block_list_select(true, true, 5, 2), "the tab list under a dialog: the shortcut's move blocked");
    check(!lg::block_list_select(true, true, 5, 5), "the held selection put back: let through");
    check(lg::block_list_select(true, true, -1, 5), "not read yet: every move blocked");
    // Wael 14/10 with 2.0.1's first fix (data only): the page stayed on Recording, E after the dialog went to tab 1
    {
        GameTabs t;
        t.hook = false;
        t.read_list = false;
        t.modal = true;
        t.frame();
        t.shortcut();
        t.frame();
        check(t.built.tab == 3 && t.data.list == 2, "first fix: the page held, the list left on the first tab");
        t.modal = false;
        t.frame();
        t.step(+1);
        check(t.built.tab == 1, "first fix reproduced: E after the dialog goes to tab 1 (Environnement)");
    }
    // the fix, the list hook in place (Community dialog, text dialog, box: the same hold)
    for (int dialog = 0; dialog < 3; ++dialog) {
        GameTabs t;
        t.modal = lg::tabs_held(dialog == 1, dialog == 0, dialog == 2, true);
        t.frame();
        check(t.g.held().list == 5, "the list's selection kept at the open");
        t.shortcut();
        t.shortcut();
        t.frame();
        check(t.blocked == 2 && t.data.list == 5 && t.built.tab == 3 && t.built.child == 4, "A+E twice: the list never moves, the page held");
        t.modal = false;
        t.frame();
        t.step(+1);
        check(t.built.tab == 4 && t.consistent(), "closed, E: Reversal, the tab after Recording");
        t.step(-1);
        t.step(-1);
        check(t.built.tab == 2 && t.consistent(), "then A twice: Sparring partner (the game's own moves)");
    }
    // the list hook refused (R18) or missing: the frame puts the selection back (its own call passes the hook)
    {
        GameTabs t;
        t.hook = false;
        t.modal = true;
        t.frame();
        t.shortcut();
        check(t.data.list == 2, "no hook: the shortcut moves the list");
        t.frame();
        check(t.data.list == 5 && t.consistent(), "the next frame puts it back");
        t.modal = false;
        t.frame();
        t.step(+1);
        check(t.built.tab == 4, "closed, E: Reversal");
    }
    // no dialog: the shortcut is the game's
    {
        GameTabs t;
        t.shortcut();
        t.frame();
        check(t.blocked == 0 && t.data.list == 2 && t.built.tab == 0 && t.built.child == 12, "no dialog: L1+R1 jumps to the first tab as the game does");
        t.step(+1);
        check(t.built.tab == 1 && t.consistent(), "then E: tab 1");
    }
    // random walk, hook in place or not: under a dialog the list and the page stay; without one, the list and the data
    // always agree after a frame (E / A then go where the screen says)
    for (int hook = 0; hook < 2; ++hook) {
        GameTabs t;
        t.hook = hook == 1;
        unsigned seed = 777u + (unsigned)hook;
        bool held_ok = true, agree_ok = true;
        lg::TabSpot spot = t.data;
        for (int i = 0; i < 20000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const unsigned r = (seed >> 16) % 5;
            if (r == 0) { t.modal = !t.modal; t.frame(); if (t.modal) spot = t.data; }
            else if (r == 1) t.shortcut();
            else if (r == 2 && !t.modal) t.step((seed >> 8) & 1 ? 1 : -1);
            else t.frame();
            t.frame();                           // (a frame follows every step)
            if (t.modal) held_ok = held_ok && t.data.list == spot.list && t.built.tab == spot.tab;
            else agree_ok = agree_ok && t.consistent();
        }
        check(held_ok, hook ? "20000 random steps (hook): under a dialog the list and the page stay" : "20000 random steps (no hook): under a dialog the list and the page stay");
        check(agree_ok, hook ? "20000 random steps (hook): without a dialog the list and the data agree" : "20000 random steps (no hook): without a dialog the list and the data agree");
    }
}

void test_tab_guard() {
    group("tab under dialogs");
    // which dialogs hold the tab
    check(!lg::tabs_held(false, false, false, true), "no dialog: nothing held");
    check(lg::tabs_held(true, false, false, false) && lg::tabs_held(false, true, false, false), "text / Community dialog: held");
    check(lg::tabs_held(false, false, true, true), "a box under the pause: held");
    check(!lg::tabs_held(false, false, true, false), "a box left up with the menu closed: nothing held");

    // no dialog: the game's own tab moves are never touched
    {
        TabSim s;
        s.frame();
        s.jump(0);
        s.data = lg::TabSpot{ 2, 1 };
        s.rebuild();
        s.frame();
        check(s.fixes == 0 && s.built.tab == 2 && s.built.child == 1 && !s.g.holding(), "no dialog: L1 / R1 / L1+R1 are the game's");
    }
    // the Community dialog (Wael 14/10: A+E took the menu back to tab 0 behind it)
    {
        TabSim s;
        s.modal = true;                          // opened from the Manager
        s.frame();
        check(s.g.holding() && s.g.held().tab == 3 && s.g.held().child == 4, "Community open: the Manager's spot kept");
        s.jump(0);
        check(s.built.tab == 3 && s.built.child == 4 && s.data.tab == 3, "A+E under the Community dialog: the page rebuilt on the Recording tab, same row");
        check(s.fixes == 2, "A+E: tab and row put back once, at CreateViewData (InitTabItem finds them right)");
        s.frame();
        check(s.fixes == 2, "the frame after: nothing more");
        s.jump(7);
        check(s.built.tab == 3 && s.fixes == 4, "a second A+E (to the last tab): put back again");
        s.modal = false;                         // the dialog closes
        s.frame();
        check(!s.g.holding(), "closed: nothing held");
        s.data = lg::TabSpot{ 4, 0 };
        s.rebuild();
        check(s.built.tab == 4 && s.fixes == 4, "closed: the next R1 is the game's");
    }
    // the text dialog opened on the Record page (slot row 5)
    {
        TabSim s;
        s.data = s.built = lg::TabSpot{ 3, 5 };
        s.modal = true;
        s.frame();
        s.jump(0);
        check(s.built.tab == 3 && s.built.child == 5, "L1+R1 under the text dialog: Recording tab, same slot row");
    }
    // the data moved with no rebuild yet: the frame puts the tab back, not the row
    {
        TabSim s;
        s.modal = true;
        s.frame();
        s.data = lg::TabSpot{ 1, 2 };
        s.frame();
        check(s.data.tab == 3 && s.data.child == 2, "frame: the tab put back, the row left (read by a rebuild only)");
        s.rebuild();
        check(s.built.tab == 3 && s.built.child == 4, "then a rebuild: the row put back too");
    }
    // the first call is a rebuild (no frame since the dialog opened)
    {
        TabSim s;
        s.modal = true;
        s.data = lg::TabSpot{ 0, 0 };
        s.rebuild(3);
        check(s.built.tab == 3 && s.built.child == 0 && s.g.held().tab == 3, "text dialog, rebuild first: the Recording tab (fallback), row unknown");
        TabSim t;
        t.modal = true;
        t.data = lg::TabSpot{ 0, 0 };
        t.rebuild();
        check(t.fixes == 0 && !t.g.holding(), "no fallback, rebuild first: the game's (nothing known, InitTabItem keeps nothing)");
        t.frame();
        check(t.g.holding() && t.g.held().tab == 0, "then the frame keeps what is shown");
    }
    // unread data: nothing written, nothing kept
    {
        lg::TabGuard g;
        check(!g.check(true, lg::TabSpot{}, lg::TabAt::Frame).tab && !g.holding(), "unread at the open: nothing kept");
        g.check(true, lg::TabSpot{ 5, 2 }, lg::TabAt::Frame);
        const lg::TabFix f = g.check(true, lg::TabSpot{}, lg::TabAt::Rebuild);
        check(!f.tab && !f.child && g.held().tab == 5, "unread later: nothing written");
        g.check(false, lg::TabSpot{}, lg::TabAt::Frame);
        g.check(true, lg::TabSpot{ 6, 1 }, lg::TabAt::Frame);
        check(g.held().tab == 6 && g.held().child == 1, "a new dialog keeps its own spot");
    }
    // InitTabItem alone (a path without CreateViewData): the tab put back, not the row
    {
        lg::TabGuard g;
        g.check(true, lg::TabSpot{ 3, 4 }, lg::TabAt::Frame);
        const lg::TabFix f = g.check(true, lg::TabSpot{ 0, 0 }, lg::TabAt::Items);
        check(f.tab && f.to.tab == 3 && !f.child, "InitTabItem alone: the tab put back, not the row");
    }
    // random walk: under a dialog the page never leaves its spot; without one the guard never writes
    {
        TabSim s;
        unsigned seed = 4242;
        bool held_ok = true, free_ok = true;
        lg::TabSpot spot = s.data;
        for (int i = 0; i < 20000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const unsigned r = (seed >> 16) % 6;
            const int before = s.fixes;
            if (r == 0) { s.modal = !s.modal; if (s.modal) { s.frame(); spot = s.data; } else s.frame(); }
            else if (r == 1) s.jump((int64_t)((seed >> 8) % 8));
            else if (r == 2 && !s.modal) { s.data = lg::TabSpot{ (s.data.tab + 1) % 8, 0 }; s.rebuild(); }
            else if (r == 3 && !s.modal) { s.data.child = (int64_t)((seed >> 4) % 12); }
            else s.frame();
            if (s.modal) held_ok = held_ok && s.built.tab == spot.tab && s.data.tab == spot.tab;
            else free_ok = free_ok && s.fixes == before;
        }
        check(held_ok, "20000 random steps: under a dialog the page stays on its tab");
        check(free_ok, "20000 random steps: without a dialog the guard never writes");
    }
}

// ---- the drill details dialog -------------------------------------------------------------------------------------------
// A fake ui::T: the texts the details read, in English
std::string fake_t(const std::string& key, const std::vector<std::string>& a) {
    if (key == "details_slot") return "Slot " + (a.empty() ? std::string() : a[0]);
    if (key == "details_page") return a.size() == 2 ? "page " + a[0] + "/" + a[1] : "?";
    if (key == "details_saved") return a.size() == 1 ? a[0] + " slots saved" : "?";
    if (key == "details_saved_one") return a.empty() ? "1 slot saved" : "?";
    if (key == "details_author") return "Author: " + (a.empty() ? std::string() : a[0]);
    if (key == "details_source") return "Source: " + (a.empty() ? std::string() : a[0]);
    if (key == "source_community") return "Community";
    if (key == "source_local") return "Local file";
    if (key == "source_local_short") return "Local";
    if (key == "details_md_help") return "Markdown Help";
    if (key == "details_md_newline") return "new line";
    if (key == "details_md_heading") return "heading";
    if (key == "details_md_emphasis") return "emphasis";
    if (key == "details_md_violet") return "light violet";
    if (key == "details_md_magenta") return "magenta";
    if (key == "details_md_purple") return "dark purple";
    if (key == "details_md_blue") return "blue highlight";
    if (key == "details_md_list") return "list line";
    if (key == "details_md_note") return "Up to " + (a.empty() ? std::string("?") : a[0]) + " characters. Ctrl+V pastes. <b> [x] are ( ).";
    return key;
}

// A model of the game's wrap: words laid on lines of `width` characters (a word wider than a line alone on its line), the
// space at each break dropped, "\n" a new line. Glyph g (1-based: the "\n" and the dropped spaces are not glyphs) gets its
// line's baseline (23 + 32.5 a line, as the game's 1080p text). The line starts the walk must find come with it.
struct WrapModel { std::vector<float> base; std::vector<size_t> starts; };
WrapModel wrap_model(const std::string& text, size_t width) {
    WrapModel m;
    m.base.push_back(0.0f);                      // g = 0: not a glyph
    m.starts.push_back(0);
    size_t line = 0, col = 0, i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') { ++line; col = 0; ++i; m.starts.push_back(i); continue; }
        if (text[i] == ' ') {                    // kept when the next word fits after it on this line, else dropped: a break
            size_t ne = i + 1;
            while (ne < text.size() && text[ne] != ' ' && text[ne] != '\n') ++ne;
            const size_t word = ne - i - 1;
            if (col > 0 && word > 0 && col + 1 + word > width) { ++line; col = 0; ++i; m.starts.push_back(i); continue; }
            m.base.push_back(23.0f + 32.5f * (float)line);
            ++col;
            ++i;
            continue;
        }
        size_t e = i;
        while (e < text.size() && text[e] != ' ' && text[e] != '\n') ++e;
        for (size_t k = i; k < e; ++k) { m.base.push_back(23.0f + 32.5f * (float)line); ++col; }
        i = e;
    }
    return m;
}

void test_details() {
    namespace dt = ui::details;
    group("details pages");
    eq(dt::page_count(0), 1, "no slot: one page");
    eq(dt::page_count(1), 1, "1 slot: one page");
    eq(dt::page_count(10), 1, "10 slots: one page");
    eq(dt::page_count(11), 2, "11 slots: two pages");
    eq(dt::page_count(30), 3, "30 slots: three pages");
    eq(dt::clamp_page(5, 15), 1, "page clamped to the last");
    eq(dt::clamp_page(-1, 15), 0, "page clamped to the first");
    eq(dt::page_range(15, 0).first, 0, "15 slots, page 1 from 0");
    eq(dt::page_range(15, 0).last, 10, "15 slots, page 1 to 10");
    eq(dt::page_range(15, 1).first, 10, "15 slots, page 2 from 10");
    eq(dt::page_range(15, 1).last, 15, "15 slots, page 2 to 15");
    eq(dt::page_range(0, 0).last, 0, "no slot: nothing shown");

    group("details texts");
    eq(dt::header_text(fake_t, 0, 2), std::string("PAGE 1/2"), "header: the page only, capitals, page from 1");
    eq(dt::header_text(fake_t, 1, 2), std::string("PAGE 2/2"), "header of the second page");
    eq(dt::header_text(fake_t, 0, 0), std::string("PAGE 1/1"), "header: never fewer than one page");
    eq(dt::saved_text(fake_t, 14), std::string("14 SLOTS SAVED"), "saved count, capitals");
    eq(dt::saved_text(fake_t, 1), std::string("1 SLOT SAVED"), "a single slot: the singular");
    eq(dt::saved_text(fake_t, 0), std::string("0 SLOTS SAVED"), "no slot");
    eq(dt::row_text("Slot 7", "Fireball", 64, 0), std::string("Slot 7 - Fireball - 64F"), "row with a name");
    eq(dt::row_text("Slot 7", "", 64, 0), std::string("Slot 7 - 64F"), "row without a name");
    eq(dt::row_text("Slot 7", "  <b>x</b>  ", 3, 0), std::string("Slot 7 - (b)x(/b) - 3F"), "markup made safe, trimmed");
    {
        const std::string name = "A very long recording name that never fits the box at all";
        const std::string r = dt::row_text("Slot 12", name, 1234, 20.0);
        check(lg::text_em(r) <= 20.0, "a long row fits its em: " + r);
        check(r.rfind("Slot 12 - ", 0) == 0 && r.size() > 5 && r.substr(r.size() - 5) == "1234F", "the slot and the frames kept: " + r);
        check(r.find("\xE2\x80\xA6") != std::string::npos, "the name cut with an ellipsis: " + r);
        eq(dt::row_text("Slot 12", name, 1234, 9.5), std::string("Slot 12 - 1234F"), "no room for 3 em of the name: dropped");
    }
    eq(dt::source_label(fake_t, "community"), std::string("Community"), "source community");
    eq(dt::source_label(fake_t, "Community"), std::string("Community"), "source community, any case");
    eq(dt::source_label(fake_t, "drillcodes.com"), std::string("drillcodes.com"), "source drillcodes");
    eq(dt::source_label(fake_t, ""), std::string("Local file"), "no source: a local file");
    eq(dt::source_label(fake_t, "my <site>"), std::string("my (site)"), "another source as written");
    // the top-left box: origin, short source, author, first line
    check(dt::origin("") == dt::Origin::Local && dt::origin("my site") == dt::Origin::Local, "no source or another one: Local");
    check(dt::origin(" Community ") == dt::Origin::Community && dt::origin("DrillCodes.com") == dt::Origin::DrillCodes
          && dt::origin("drillcodes") == dt::Origin::DrillCodes, "community / drillcodes(.com), any case");
    eq(dt::short_source(fake_t, ""), std::string("Local"), "short source: Local");
    eq(dt::short_source(fake_t, "other place"), std::string("Local"), "short source of another source: Local");
    eq(dt::short_source(fake_t, "community"), std::string("Community"), "short source: Community (translated)");
    eq(dt::short_source(fake_t, "drillcodes.com"), std::string("DrillCodes"), "short source: DrillCodes (a name)");
    eq(dt::frame_author("Chris_F", "Wael3rd_PC", "community"), std::string("Chris_F"), "info.author first");
    eq(dt::frame_author(" <Chris> ", "", ""), std::string("(Chris)"), "info.author made safe");
    eq(dt::frame_author("", "Wael3rd_PC", ""), std::string("Wael3rd_PC"), "a Local drill without author: the share name");
    eq(dt::frame_author("", "Wael3rd_PC", "something"), std::string("Wael3rd_PC"), "another source counts as Local");
    eq(dt::frame_author("", "Wael3rd_PC", "community"), std::string(), "a Community drill without author: none");
    eq(dt::frame_author("", "Wael3rd_PC", "drillcodes.com"), std::string(), "a DrillCodes drill without author: none");
    eq(dt::frame_author("", "", ""), std::string(), "no share name known: none");
    // a Community import of a drill code: "<author> (drillcodes)" (the server's authorLabel) is DrillCodes, its author bare
    {
        std::string bare;
        check(dt::drillcodes_author("Chris_F (drillcodes)", &bare) && bare == "Chris_F", "the drillcodes label: the author bare: " + bare);
        check(dt::drillcodes_author("Chris_F (DrillCodes)   ", &bare) && bare == "Chris_F", "any case, spaces after it: " + bare);
        check(dt::drillcodes_author("Big  Name   (DRILLCODES)", &bare) && bare == "Big Name", "spaces inside collapsed: " + bare);
        check(dt::drillcodes_author("(drillcodes)", &bare) && bare.empty(), "the suffix alone: no author");
        check(!dt::drillcodes_author("Chris_F", &bare) && !dt::drillcodes_author("Bob(drillcodes)") && !dt::drillcodes_author("x (drillcodes) y")
              && !dt::drillcodes_author("drillcodes") && !dt::drillcodes_author(""), "no suffix, or not the label: not one");
        eq(dt::shown_author("Chris_F (drillcodes)"), std::string("Chris_F"), "shown author: the suffix dropped");
        eq(dt::shown_author(" <Chris> "), std::string("(Chris)"), "shown author: made safe as before");
        check(dt::origin("community", "Chris_F (drillcodes)") == dt::Origin::DrillCodes, "community + the label: DrillCodes");
        check(dt::origin("community", "Chris_F") == dt::Origin::Community, "community alone: still Community");
        check(dt::origin("drillcodes.com", "Chris_F") == dt::Origin::DrillCodes, "a local conversion: still DrillCodes");
        check(dt::origin("", "Me (drillcodes)") == dt::Origin::DrillCodes, "the label wins over no source");
        eq(dt::short_source(fake_t, "community", "Chris_F (drillcodes)"), std::string("DrillCodes"), "short source of the label: DrillCodes");
        eq(dt::short_source(fake_t, "community", "Chris_F"), std::string("Community"), "short source of a Community author: Community");
        eq(dt::source_label(fake_t, "community", "Chris_F (drillcodes)"), std::string("drillcodes.com"), "source line of the label: drillcodes.com");
        eq(dt::source_label(fake_t, "community", "Chris_F"), std::string("Community"), "source line of a Community author: Community");
        eq(dt::frame_author("Chris_F (drillcodes)", "Wael3rd_PC", "community"), std::string("Chris_F"), "top-left author: the label bare");
        eq(dt::frame_author("(drillcodes)", "Wael3rd_PC", "community"), std::string(), "the suffix alone: no author, never the share name");
        eq(dt::frame_text("Akuma", dt::frame_author("Chris_F (drillcodes)", "", "community"),
                          dt::short_source(fake_t, "community", "Chris_F (drillcodes)"), 0),
           std::string("Akuma - Chris_F - DrillCodes"), "Wael's line: Akuma - Chris_F - DrillCodes");
        eq(dt::frame_text("Akuma", dt::frame_author("Chris_F", "", "community"), dt::short_source(fake_t, "community", "Chris_F"), 0),
           std::string("Akuma - Chris_F - Community"), "a Community-only drill: Community");
        eq(dt::detail_text(fake_t, "Do it.", "Chris_F (drillcodes)", dt::source_label(fake_t, "community", "Chris_F (drillcodes)")),
           std::string("Do it.\n\nAuthor: Chris_F\nSource: drillcodes.com"), "the description pane's author / source lines of the label");
    }
    eq(dt::frame_text("Ryu", "Wael3rd_PC", "Local", 0), std::string("Ryu - Wael3rd_PC - Local"), "first line: character, author, source");
    eq(dt::frame_text("Ryu", "", "Community", 0), std::string("Ryu - Community"), "first line without an author");
    eq(dt::frame_text("", "", "Local", 0), std::string("Local"), "first line without a character nor an author");
    eq(dt::frame_text("", "Chris_F", "DrillCodes", 0), std::string("Chris_F - DrillCodes"), "first line without a character");
    {
        const std::string longa = "An author name so long that it never fits the top-left box at all";
        const std::string r = dt::frame_text("Ryu", longa, "Community", 22.0);
        check(lg::text_em(r) <= 22.0, "a long author: the line fits its em: " + r);
        check(r.rfind("Ryu - ", 0) == 0 && r.size() > 12 && r.substr(r.size() - 12) == " - Community", "the character and the source kept: " + r);
        check(r.find("\xE2\x80\xA6") != std::string::npos, "the author cut with an ellipsis: " + r);
        const double tight = lg::text_em("Ryu - Community") + 1.0;   // (an author under 3 em would stay)
        eq(dt::frame_text("Ryu", longa, "Community", tight), std::string("Ryu - Community"), "no room for 3 em of the author: dropped");
        const std::string all = dt::frame_text("A character name far too long", "", "Community", 8.0);
        check(lg::text_em(all) <= 8.0 && all.find("\xE2\x80\xA6") != std::string::npos, "no author and still too wide: the line cut: " + all);
        eq(dt::frame_text("Ryu", "[x]", "Local", 0), std::string("Ryu - (x) - Local"), "brackets made ( )");
    }
    eq(dt::clean_text("\r\n\nLine one\r\n\n\n\nLine\ttwo  <x> [y]\n\n", 6000), std::string("Line one\n\nLine two (x) (y)"),
       "paragraphs kept, one blank line at most, none first or last");
    eq(dt::clean_text("abc\x01\x7F d", 6000), std::string("abc d"), "control characters dropped");
    eq(dt::clean_text("\xC3\xA9\xC3\xA9\xC3\xA9", 5), std::string("\xC3\xA9\xC3\xA9"), "cut on a UTF-8 boundary");
    eq(dt::detail_text(fake_t, "Do this.\nThen that.", "Chris_F", "Community"),
       std::string("Do this.\nThen that.\n\nAuthor: Chris_F\nSource: Community"), "detail text");
    eq(dt::detail_text(fake_t, "", "", "Local file"), std::string("Source: Local file"), "detail text without description or author");
    check(std::fabs(dt::em_for(930.0f, 23.0f) - 930.0 / 23.0 * 0.98) < 1e-9, "em of a region");
    eq(dt::em_for(0.0f, 23.0f), 0.0, "em of an unknown region");

    group("details lines");
    {   // the walk against the wrap model: paragraphs, blank lines, a long text, three widths
        const std::string one = "This is a basic counter to Akuma's simple fireball spacetrap.\n\nTake a tiny step forward from "
                                "round start position and create a save state there. Akuma will throw a light fireball, then a heavy "
                                "one, then walk forward behind it.\n\nAuthor: Chris_F\nSource: Community";
        std::string text;
        for (int i = 0; i < 4; ++i) text += (i ? "\n\n" : "") + one;
        for (size_t width : { (size_t)20, (size_t)40, (size_t)55 }) {
            const WrapModel m = wrap_model(text, width);
            const dt::Lines ls = dt::line_starts(text, [&](uint32_t g) { return g < m.base.size() ? m.base[g] : 0.0f; });
            const std::string w = " (width " + std::to_string(width) + ")";
            check(ls.laid_out, "laid out" + w);
            check(ls.starts == m.starts, "line starts found from the baselines" + w + ": " + std::to_string(m.starts.size()) +
                  " lines, found " + std::to_string(ls.starts.size()));
            bool ok = true;
            for (size_t k = 0; k < ls.starts.size(); ++k) {
                const size_t e = k + 1 < ls.starts.size() ? ls.starts[k + 1] : text.size();
                const std::string l = text.substr(ls.starts[k], e - ls.starts[k]);
                if (!l.empty() && l[0] == ' ') ok = false;
                const size_t nl = l.find('\n');
                if (nl != std::string::npos && nl + 1 != l.size()) ok = false;
            }
            check(ok, "each line starts on a glyph, a line break only at its end" + w);
        }
    }
    {   // UTF-8: one glyph per character; "e-acute, a, e-acute, long, em dash, end" broken before the dash (its space dropped)
        const std::string text = "\xC3\xA9" "a" "\xC3\xA9 long \xE2\x80\x94 end";
        const std::vector<float> b = { 0, 23, 23, 23, 23, 23, 23, 23, 23, 55.5f, 55.5f, 55.5f, 55.5f, 55.5f };
        const dt::Lines ls = dt::line_starts(text, [&](uint32_t g) { return g < b.size() ? b[g] : 0.0f; });
        check(ls.starts.size() == 2 && ls.starts[1] == text.find("\xE2\x80\x94"), "a UTF-8 line starts on its character's first byte");
    }
    {   // not laid out yet: every baseline 0
        const dt::Lines ls = dt::line_starts("Some text\nmore", [](uint32_t) { return 0.0f; });
        check(!ls.laid_out && ls.starts.size() == 1 && ls.starts[0] == 0, "no baseline: not laid out, one line");
        check(!dt::line_starts("", [](uint32_t) { return 23.0f; }).laid_out, "empty text: nothing laid out");
    }
    {   // glyphs counted from 0 when baseline(0) is a glyph's
        const std::string text = "ab cd";
        const float b0[] = { 23, 23, 55.5f, 55.5f };             // a b (' ' dropped) c d
        const dt::Lines ls = dt::line_starts(text, [&](uint32_t g) { return g < 4 ? b0[g] : 0.0f; });
        check(ls.starts.size() == 2 && ls.starts[1] == 3, "glyphs from 0, the dropped space skipped");
    }

    group("details window");
    {
        const std::vector<size_t> st = { 0, 10, 20, 30, 40 };
        auto w = dt::window(st, 50, 0, 2);
        eq(w.begin, (size_t)0, "window from line 0");
        eq(w.end, (size_t)20, "window to line 2's start");
        w = dt::window(st, 50, 3, 2);
        eq(w.begin, (size_t)30, "window from line 3");
        eq(w.end, (size_t)50, "the last window to the end");
        w = dt::window(st, 50, 9, 2);
        eq(w.begin, (size_t)30, "top clamped to the last full window");
        w = dt::window(st, 50, 0, 18);
        eq(w.end, (size_t)50, "fewer lines than the window: all");
        eq(dt::max_top(43, 18), 25, "43 lines, 18 shown: 25 steps");
        eq(dt::max_top(5, 18), 0, "no scroll under 18 lines");
    }
    {
        const float H = 18 * 32.5f;
        auto t = dt::thumb(H, 43, 18, 0);
        check(std::fabs(t.h - H * 18 / 43) < 0.01f && t.y == 0, "thumb at the top, proportional");
        t = dt::thumb(H, 43, 18, 25);
        check(std::fabs(t.y + t.h - H) < 0.01f, "thumb at the bottom at the last step");
        t = dt::thumb(H, 1000, 18, 0);
        eq(t.h, dt::kThumbMin, "thumb never under 40");
        t = dt::thumb(H, 10, 18, 0);
        check(t.h == H && t.y == 0, "no scroll: the whole track");
    }

    group("details edit");
    {
        eq(dt::cut_utf16("abcdef", 4), std::string("abcd"), "ASCII cut at 4 units");
        eq(dt::cut_utf16("\xC3\xA9\xC3\xA9\xC3\xA9", 2), std::string("\xC3\xA9\xC3\xA9"), "a BMP character is one unit");
        eq(dt::cut_utf16("a\xF0\x9F\x98\x80" "b", 2), std::string("a"), "a character past U+FFFF is two units: not cut in half");
        eq(dt::cut_utf16("a\xF0\x9F\x98\x80" "b", 3), std::string("a\xF0\x9F\x98\x80"), "two units: kept whole");
        eq(dt::edit_text("First paragraph.\n\nSecond one,\nits next line.  "), std::string("First paragraph./n/nSecond one,/nits next line."),
           "each line break shown as /n, a paragraph as /n/n");
        eq(dt::edit_result("Test /n Test"), std::string("Test\nTest"), "\"Test /n Test\": a new line (each line trimmed)");
        eq(dt::edit_result("A/n/nB\\nC"), std::string("A\n\nB\nC"), "/n and a typed backslash-n are line breaks");
        eq(dt::edit_result("A/n/n/n/nB/n"), std::string("A\n\nB"), "one blank line at most, none last");
        eq(dt::edit_result("a / b, n/a, \\x"), std::string("a / b, n/a, \\x"), "a slash or a backslash alone stays");
        eq(dt::edit_result(dt::edit_text("A\n\nB <c>\nD")), std::string("A\n\nB (c)\nD"), "a round trip keeps the lines and paragraphs");
        eq(dt::edit_result("   "), std::string(), "nothing typed: an empty description");
        {
            std::string many;                                    // 600 line breaks typed: 1200 bytes of "/n" between words
            for (int i = 0; i < 600; ++i) many += "x/n";
            const std::string r = dt::edit_result(many);
            check(r.size() <= dt::kDescMaxUnits && r.size() >= dt::kDescMaxUnits - 1, "the 1000 cut after the conversion: " + std::to_string(r.size()));
        }
        std::string big;
        for (int i = 0; i < 700; ++i) big += "\xC3\xA9 ";      // 1400 units
        const std::string cut = dt::edit_result(big);
        size_t units = 0;
        for (size_t i = 0; i < cut.size();) { const unsigned char c = (unsigned char)cut[i]; i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; ++units; }
        check(units <= dt::kDescMaxUnits && units >= dt::kDescMaxUnits - 1, "a long text cut to 1000 units: " + std::to_string(units));
        check(dt::edit_text(big).size() <= big.size(), "the prefill cut too");
    }
    {
        using json = nlohmann::json;
        json e1 = { { "id", 1 }, { "empty", false }, { "timeline", { "1f : 5" } },
                    { "info", { { "author", "Chris_F" }, { "source", "community" }, { "description", "old" }, { "other", 3 } } } };
        json e2 = { { "id", 2 }, { "empty", true } };
        json out;
        check(model::pack::with_description(json::array({ e2, e1 }), "new\n\ntext", &out), "entry 1 found");
        eq(model::pack::description(out), std::string("new\n\ntext"), "the new description");
        eq(model::pack::author(out), std::string("Chris_F"), "the author kept");
        eq(model::pack::source(out), std::string("community"), "the source kept");
        check(out.is_array() && out.size() == 2 && out[1]["info"]["other"] == 3 && out[1]["timeline"].size() == 1 && out[0] == e2,
              "every other key and entry kept");
        json bare = { { "id", 1 }, { "empty", true } };
        check(model::pack::with_description(json::array({ bare }), "made", &out) && model::pack::description(out) == "made",
              "an info block made when the entry has none");
        json untouched = 7;
        check(!model::pack::with_description(json::array({ e2 }), "x", &untouched) && untouched == 7, "no entry 1: false, out untouched");
        check(!model::pack::with_description(json::object(), "x", &untouched), "not an array: false");
    }

    group("details guide");
    {
        eq(dt::leading_combo_icons("<ICON KeyA><ICON +><ICON KeyE> Go to Basic Settings"), std::string("<ICON KeyA><ICON +><ICON KeyE>"),
           "the tab keys' glyphs of the game's own entry");
        eq(dt::leading_combo_icons("<ICON KeyEsc><ICON MouseR> Back"), std::string(), "two glyphs without a '+': not a combination");
        eq(dt::leading_combo_icons("Go <ICON KeyA><ICON +><ICON KeyE>"), std::string(), "glyphs not leading: none");
        eq(dt::leading_combo_icons("<ICON KeyA"), std::string(), "a cut tag: none");
        auto gt = [](const std::string& k, const std::vector<std::string>&) {
            return k == "details_confirm" ? std::string("Confirm") : k == "details_more_slots" ? std::string("Show More Slots")
                 : k == "details_edit" ? std::string("Edit Drill Description") : k == "details_md_help" ? std::string("Markdown Help")
                 : k == "details_show_desc" ? std::string("Show Drill Description") : k;
        };
        eq(dt::guide_entries(gt, false, "<ICON KeyA><ICON +><ICON KeyE>", true),
           std::string("<ICON KeyF> Confirm   <ICON KeyA> / <ICON KeyE> Show More Slots   <ICON KeyT> Edit Drill Description   <ICON KeyR> Markdown Help"),
           "keyboard, two pages: a slash between the tab glyphs, R for the help");
        eq(dt::guide_entries(gt, true, "<ICON BtnL1><ICON +><ICON BtnR1>", false),
           std::string("<ICON BtnD> Confirm   <ICON BtnU> Edit Drill Description   <ICON BtnL> Markdown Help"),
           "pad, one page: no slot pages entry, the west button (X / Square) for the help");
        eq(dt::guide_entries(gt, false, "", true), std::string("<ICON KeyF> Confirm   <ICON KeyT> Edit Drill Description   <ICON KeyR> Markdown Help"),
           "no tab glyphs known: no entry");
        // the R entry follows the pane: the way back to the description while the Markdown help is shown
        eq(dt::guide_entries(gt, false, "<ICON KeyA><ICON +><ICON KeyE>", true, true),
           std::string("<ICON KeyF> Confirm   <ICON KeyA> / <ICON KeyE> Show More Slots   <ICON KeyT> Edit Drill Description   <ICON KeyR> Show Drill Description"),
           "keyboard, the help shown: [R] Show Drill Description");
        eq(dt::guide_entries(gt, true, "", false, true), std::string("<ICON BtnD> Confirm   <ICON BtnU> Edit Drill Description   <ICON BtnL> Show Drill Description"),
           "pad, the help shown: the same key, the description's way back");
        eq(dt::guide_entries(gt, true, "", false, false), std::string("<ICON BtnD> Confirm   <ICON BtnU> Edit Drill Description   <ICON BtnL> Markdown Help"),
           "the description shown again: Markdown Help again");
        const std::vector<float> nx = dt::guide_layout({ 100, 400, 900 }, { 110, 300, 120 }, { 23, 23, 23 });
        check(nx.size() == 3 && nx[0] == 100 && nx[1] == 233 && nx[2] == 556, "laid out as the game: x + width + font width");
        check(dt::guide_layout({}, {}, {}).empty(), "no item: nothing");
    }

    group("details markdown");
    {
        const std::string L = "<COLOR preset=\"FGM_Cmn_2\">", V = "<COLOR preset=\"FGM_Cmn_1\">", M = "<COLOR preset=\"FGM_Cmn_0\">",
                          C = "</COLOR>", B = "<HLBGN Arg0 = \"3\">", BE = "<HLEND Arg0 = \"3\">";
        eq(dt::markdown("# Setup"), L + "Setup" + C, "a heading: light violet, its '# ' dropped");
        eq(dt::markdown("#Setup"), std::string("#Setup"), "'#' without a space: no heading");
        eq(dt::markdown(dt::clean_text("   #  Plan", 6000)), L + "Plan" + C, "a heading after the line's own spaces (trimmed first)");
        eq(dt::markdown("a # b"), std::string("a # b"), "'# ' inside a line: plain");
        eq(dt::markdown("Punish the **walk** now"), "Punish the " + V + "walk" + C + " now", "**emphasis**: violet");
        eq(dt::markdown("- first\n- second"), std::string("- first\n- second"), "a bullet stays '- '");
        eq(dt::markdown("{violet}a{/} {magenta}b{/} {purple}c{/} {blue}d{/}"),
           L + "a" + C + " " + V + "b" + C + " " + M + "c" + C + " " + B + "d" + BE,
           "violet FGM_Cmn_2, magenta FGM_Cmn_1 (bright pink), purple FGM_Cmn_0 (dark), blue closed by HLEND");
        eq(dt::markdown("{red}red{/} {Violet}y"), std::string("{red}red{/} {Violet}y"), "an unknown {word} and a {/} closing nothing stay as typed");
        eq(dt::markdown("{blue}a{/}{/}"), B + "a" + BE + "{/}", "a second {/} closes nothing: as typed");
        eq(dt::markdown("Nested: {violet}outer {blue}inner{/} after{/}"), "Nested: " + L + "outer " + C + B + "inner" + BE + " after",
           "the {/} of a span a nested one closed early is dropped");
        eq(dt::markdown("{violet}a {blue}b {magenta}c{/}{/}{/}{/}"), L + "a " + C + B + "b " + BE + V + "c" + C + "{/}",
           "one dropped {/} per span closed early, the next one as typed");
        eq(dt::markdown("**a {blue}b{/}{/}"), V + "a " + C + B + "b" + BE + "{/}", "an emphasis closed early has no {/} of its own");
        eq(dt::strip_markdown("Nested: {violet}outer {blue}inner{/} after{/} {red}red{/}"), std::string("Nested: outer inner after {red}red{/}"),
           "stripped the same way");
        eq(dt::markdown("{blue}a {magenta}b{/} c"), B + "a " + BE + V + "b" + C + " c", "a span inside another closes it first");
        eq(dt::markdown("# Title **x** end"), L + "Title " + C + V + "x" + C + " end", "emphasis in a heading closes the heading");
        eq(dt::markdown("**open\nnext"), V + "open" + C + "\nnext", "an unclosed span closes at the line's end");
        eq(dt::markdown("{violet}\n# \n****"), std::string("\n\n"), "empty spans are not written");
        eq(dt::markdown("a {b c} d { e"), std::string("a {b c} d { e"), "braces that are no marker stay");
        // injection: the user's tags made ( ) by clean_text before the conversion (square brackets are not drawn by the game),
        // the only tags are the converter's
        const std::string typed = "<COLOR preset=\"FGM_Cmn_2\">must show brackets</COLOR> <HLBGN Arg0 = \"3\"> [x]";
        const std::string safe = dt::markdown(dt::clean_text(typed, 6000));
        eq(safe, std::string("(COLOR preset=\"FGM_Cmn_2\")must show brackets(/COLOR) (HLBGN Arg0 = \"3\") (x)"),
           "a typed tag comes out as (COLOR ...), visible");
        check(safe.find('<') == std::string::npos && safe.find('[') == std::string::npos, "no '<' nor '[' from the user's text");
        // a stored source (any build) with both kinds, in every path to the game: the details box, the help line, the Community area
        const std::string both = "Injection: <COLOR preset=\"FGM_Cmn_2\">must show brackets</COLOR> and [x] {blue}<b>{/}";
        const std::string box = dt::detail_text(fake_t, both, "", "Community");
        check(box.find("(COLOR preset=\"FGM_Cmn_2\")must show brackets(/COLOR) and (x) " + B + "(b)" + BE) != std::string::npos,
              "details box: < > and [ ] as ( ), the only tags the converter's: " + box);
        check(box.find('[') == std::string::npos && box.find("<COLOR preset=\"FGM_Cmn_2\">must") == std::string::npos, "details box: no [ nor the typed tag");
        const auto hl = lg::help_pages(dt::strip_markdown(both), 1e9);
        check(hl.size() == 1 && hl[0] == "Injection: (COLOR preset=\"FGM_Cmn_2\")must show brackets(/COLOR) and (x) (b)",
              "help line (whole): ( ) only: " + (hl.empty() ? std::string() : hl[0]));
        const auto ca = lg::desc_lines(dt::strip_markdown(both), 200, 3);
        check(ca.size() == 1 && !hl.empty() && ca[0] == hl[0], "Community area: the same: " + (ca.empty() ? std::string() : ca[0]));
        eq(dt::detail_text(fake_t, "# Plan\nDo **this**.", "", "Community"), L + "Plan" + C + "\nDo " + V + "this" + C + ".\n\nSource: Community",
           "the description box converted, the author / source lines plain");
        // the stripped text (help line, Community)
        eq(dt::strip_markdown("# Plan\nDo **this** {blue}now{/} {red}x{/}\n- item"), std::string("Plan\nDo this now {red}x{/}\n- item"),
           "markers dropped; an unknown {word}, a {/} closing nothing and the bullet kept");
        eq(dt::strip_markdown("  # Plan"), std::string("  Plan"), "a heading after the line's own spaces (not trimmed here)");
        eq(dt::strip_markdown(dt::edit_result("# A/n**b**")), std::string("A\nb"), "the stored source stripped");
        eq(dt::strip_markdown("# Plan\n#Plan\nx # y"), std::string("Plan\n#Plan\nx # y"), "only a line-start '# ' is a heading's");
        eq(dt::edit_text("# Plan\nDo **this**."), std::string("# Plan/nDo **this**."), "the editor shows the Markdown source");
        // the walk skips the tags (no glyph): "AB C" + emphasis "D" then a line break
        const std::string t1 = dt::markdown("AB **CD**\nEF");             // A B ' ' [tag] C D [tag] \n E F
        const float b1[] = { 0, 23, 23, 23, 23, 23, 55.5f, 55.5f };       // g1..5: A B ' ' C D, g6..7: E F
        const dt::Lines l1 = dt::line_starts(t1, [&](uint32_t g) { return g < 8 ? b1[g] : 0.0f; });
        check(l1.starts.size() == 2 && l1.starts[1] == t1.find('E'), "the walk skips the tags (a line break after a span)");
        // a wrap inside a span: the window re-opens it at its start and closes it at its end
        const std::string t2 = dt::markdown("XX **YY ZZ** W");                // X X ' ' [V] Y Y (' ' dropped) Z Z [/] ' ' W
        const float b2[] = { 0, 23, 23, 23, 23, 23, 55.5f, 55.5f, 55.5f, 55.5f };   // g1..5 line 1, g6.. line 2
        const dt::Lines l2 = dt::line_starts(t2, [&](uint32_t g) { return g < 10 ? b2[g] : 0.0f; });
        check(l2.starts.size() == 2 && l2.starts[1] == t2.find("ZZ"), "a wrap inside a span: the line starts on its glyph");
        eq(dt::window_text(t2, l2.starts, 0, 1), "XX " + V + "YY " + C, "the first window closes the span open at its end");
        eq(dt::window_text(t2, l2.starts, 1, 1), V + "ZZ" + C + " W", "the next window opens it again at its start");
        eq(dt::window_text(t2, l2.starts, 0, 18), t2, "a window of every line: the text as it is");
        const std::string t3 = dt::markdown("a {blue}b c");
        eq(dt::window_text(t3, { 0, t3.find('c') }, 1, 1), B + "c" + BE, "the blue highlight opened again, closed by HLEND");
        // the source's 1000 limit (the editor), the conversion after
        std::string big;
        for (int i = 0; i < 400; ++i) big += "**a** ";
        const std::string src = dt::edit_result(big);
        check(src.size() <= dt::kDescMaxUnits, "the source cut to 1000 before any tag");
        const std::string conv = dt::markdown(dt::clean_text(src, 6000));
        size_t opens = 0, closes = 0;
        for (size_t f = conv.find("<COLOR"); f != std::string::npos; f = conv.find("<COLOR", f + 1)) ++opens;
        for (size_t f = conv.find("</COLOR>"); f != std::string::npos; f = conv.find("</COLOR>", f + 1)) ++closes;
        check(opens == closes && opens > 0, "every span closed (" + std::to_string(opens) + " / " + std::to_string(closes) + ")");
    }

    group("details help");
    {
        const std::string L = "<COLOR preset=\"FGM_Cmn_2\">", V = "<COLOR preset=\"FGM_Cmn_1\">", M = "<COLOR preset=\"FGM_Cmn_0\">",
                          C = "</COLOR>", B = "<HLBGN Arg0 = \"3\">", BE = "<HLEND Arg0 = \"3\">";
        const std::string h = dt::help_text(fake_t);
        check(h.rfind(L + "Markdown Help" + C + "\n", 0) == 0, "the help's heading first: " + h.substr(0, 60));
        const std::string rows[] = {
            "\n/n  =  new line\n",
            "\n# Title  =  " + L + "heading" + C + "\n",
            "\n**text**  =  " + V + "emphasis" + C + "\n",
            "\n{violet}text{/}  =  " + L + "light violet" + C + "\n",
            "\n{magenta}text{/}  =  " + V + "magenta" + C + "\n",
            "\n{purple}text{/}  =  " + M + "dark purple" + C + "\n",
            "\n{blue}text{/}  =  " + B + "blue highlight" + BE + "\n",
            "\n- item  =  list line\n",
        };
        size_t at = 0;
        bool order = true;
        for (const std::string& r : rows) {
            const size_t f = h.find(r, at);
            check(f != std::string::npos, "help row: " + r.substr(1, r.size() - 2));
            if (f == std::string::npos || f < at) order = false; else at = f + 1;
        }
        check(order, "the help's rows in order");
        eq(h.substr(h.rfind("\n\n") + 2), std::string("Up to 1000 characters. Ctrl+V pastes. (b) (x) are ( )."),
           "the note last, after a blank line, its limit 1000, its brackets made ( )");
        // the only tags are the converter's verified ones, every span closed on its own line; the syntax shown as typed
        auto tags_ok = [&](const std::string& s, int* opens, int* closes) {
            *opens = *closes = 0;
            for (size_t i = s.find('<'); i != std::string::npos; i = s.find('<', i + 1)) {
                const size_t e = s.find('>', i);
                if (e == std::string::npos) return false;
                const std::string tag = s.substr(i, e - i + 1);
                if (tag == L || tag == V || tag == M || tag == B) ++*opens;
                else if (tag == C || tag == BE) ++*closes;
                else return false;
            }
            return s.find('[') == std::string::npos && s.find(']') == std::string::npos;
        };
        int o = 0, c = 0;
        check(tags_ok(h, &o, &c) && o == 7 && c == 7, "7 spans (title, heading, emphasis, 4 colours), only verified tags, no [ ]: " +
              std::to_string(o) + " / " + std::to_string(c));
        bool lines_ok = true;
        for (size_t p = 0; p <= h.size();) {
            size_t e = h.find('\n', p);
            if (e == std::string::npos) e = h.size();
            int lo = 0, lc = 0;
            if (!tags_ok(h.substr(p, e - p), &lo, &lc) || lo != lc) lines_ok = false;
            p = e + 1;
        }
        check(lines_ok, "each line of the help opens and closes its own span");
        check(h.find("**text**") != std::string::npos && h.find("{violet}text{/}") != std::string::npos && h.find("# Title") != std::string::npos,
              "the syntax shown as typed (never converted)");
        // a translation holding markers or tags makes no span of its own
        auto evil = [](const std::string& k, const std::vector<std::string>& a) {
            if (k == "details_md_newline") return std::string("**x** {blue}y{/} <COLOR preset=\"FGM_Cmn_1\">z");
            if (k == "details_md_heading") return std::string("# head [1]");
            return fake_t(k, a);
        };
        const std::string he = dt::help_text(evil);
        check(tags_ok(he, &o, &c) && o == 7 && c == 7, "a translation's markers and tags make no span: " + std::to_string(o) + " / " + std::to_string(c));
        check(he.find("\n/n  =  x bluey/ (COLOR preset=\"FGM_Cmn_1\")z\n") != std::string::npos, "its marker characters dropped, its tag shown as ( )");
        check(he.find("\n# Title  =  " + L + "head (1)" + C + "\n") != std::string::npos, "a translated '# ' dropped, the heading still one span");
        // the help through the window: each line alone valid, the walk skips its tags (one line a window)
        std::vector<size_t> st{ 0 };
        for (size_t i = 0; i < h.size(); ++i) if (h[i] == '\n') st.push_back(i + 1);
        bool win_ok = true;
        for (int k = 0; k < (int)st.size(); ++k) {
            int wo = 0, wc = 0;
            if (!tags_ok(dt::window_text(h, st, k, 1), &wo, &wc) || wo != wc) win_ok = false;
        }
        check(win_ok, "every one-line window of the help is valid on its own");
        const dt::Lines ls = dt::line_starts(h, [](uint32_t) { return 23.0f; });   // (no wrap: one baseline)
        check(ls.laid_out && ls.starts == st, "the walk finds the help's lines (tags skipped): " + std::to_string(ls.starts.size()));
    }

    group("details keys");
    {
        dt::HoldRepeat h;
        std::vector<int> steps;
        for (int f = 1; f <= 30; ++f) if (h.step(true)) steps.push_back(f);
        eq(steps.size(), (size_t)7, "30 frames held: the press and 6 repeats");
        check(steps.size() >= 2 && steps[0] == 1 && steps[1] == 15, "one at the press, the next once held over 12 frames, then every 3");
        check(!h.step(false) && h.held == 0, "released: reset");
        check(h.step(true), "pressed again: a step at once");
    }

    group("details wheel");
    {
        auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        // the client pixels in the UI's 1920 x 1080 units
        dt::GuiPoint p = dt::client_to_gui(1100, 744, 1920, 1080);
        check(p.ok && near(p.x, 1100) && near(p.y, 744), "1080p: the client pixels as they are (in game 04/11: the game's cursor too)");
        p = dt::client_to_gui(1280, 720, 2560, 1440);
        check(p.ok && near(p.x, 960) && near(p.y, 540), "1440p: scaled down by 4/3");
        p = dt::client_to_gui(3839, 2159, 3840, 2160);
        check(p.ok && near(p.x, 1919.5f) && near(p.y, 1079.5f), "2160p: halved");
        p = dt::client_to_gui(320, 0, 2560, 1080);
        check(p.ok && near(p.x, 0) && near(p.y, 0), "21:9 (2560 x 1080): bars left and right, the UI from x 320");
        p = dt::client_to_gui(2240, 1080, 2560, 1080);
        check(p.ok && near(p.x, 1920) && near(p.y, 1080), "21:9: the UI's right edge at x 2240");
        p = dt::client_to_gui(0, 60, 1920, 1200);
        check(p.ok && near(p.x, 0) && near(p.y, 0), "16:10 (1920 x 1200): bars above and below, the UI from y 60");
        p = dt::client_to_gui(640, 400, 1280, 800);
        check(p.ok && near(p.x, 960) && near(p.y, 540), "16:10 smaller (1280 x 800): scale 2/3, the centre stays the centre");
        p = dt::client_to_gui(10, 10, 1920, 1200);
        check(p.ok && p.y < 0, "in the bar above: above the UI (outside every area)");
        check(!dt::client_to_gui(5, 5, 0, 1080).ok && !dt::client_to_gui(5, 5, 1920, 0).ok, "an empty client area: no point");
        // the area: the text (930 wide) and the bar (30 further, 8 wide), 18 lines + the bar's offset high, from the region
        const dt::Area a = dt::wheel_area(740, 191);
        check(near(a.left, 740) && near(a.top, 191) && near(a.right, 740 + 930 + 30 + 8) && near(a.bottom, 191 + 6 + 18 * 32.5f),
              "the description's area at 1080p: 740, 191 - 1708, 782");
        check(dt::contains(a, dt::client_to_gui(1000, 400, 1920, 1080)), "over the text: inside");
        check(dt::contains(a, dt::client_to_gui(1704, 300, 1920, 1080)), "over the scroll bar: inside");
        check(dt::contains(a, dt::client_to_gui(740, 191, 1920, 1080)) && dt::contains(a, dt::client_to_gui(1708, 782, 1920, 1080)),
              "the edges: inside");
        check(!dt::contains(a, dt::client_to_gui(739, 400, 1920, 1080)) && !dt::contains(a, dt::client_to_gui(1709, 400, 1920, 1080))
              && !dt::contains(a, dt::client_to_gui(1000, 190, 1920, 1080)) && !dt::contains(a, dt::client_to_gui(1000, 783, 1920, 1080)),
              "beside, above, under: outside");
        check(!dt::contains(a, dt::client_to_gui(300, 300, 1920, 1080)), "the slot list (left of the text): outside");
        check(!dt::contains(a, dt::GuiPoint{ 1000, 400, false }), "a point not known: never inside");
        check(dt::contains(a, dt::client_to_gui(1333.33f, 533.33f, 2560, 1440)), "1440p: the same place on screen, inside");
        check(!dt::contains(a, dt::client_to_gui(900, 400, 2560, 1440)) && dt::contains(a, dt::client_to_gui(900, 400, 1920, 1080)),
              "the pixel (900, 400): inside at 1080p, left of the area at 1440p (675 in the UI's units)");
        check(dt::contains(a, dt::client_to_gui(320 + 1000, 400, 2560, 1080)) && !dt::contains(a, dt::client_to_gui(1000, 400, 2560, 1080)),
              "21:9: the area moved right by the bar");
        // the notches
        dt::WheelSteps w;
        eq(w.take(120), -3, "a notch forward: 3 lines up");
        eq(w.take(-120), 3, "a notch back: 3 lines down");
        eq(w.take(240), -6, "two notches at once: 6 lines");
        eq(w.take(0), 0, "no delta: nothing");
        eq(w.take(40), 0, "a third of a notch (a smooth wheel): nothing yet");
        eq(w.take(40), 0, "two thirds: nothing yet");
        eq(w.take(40), -3, "a whole notch summed: 3 lines");
        eq(w.rest, 0, "nothing left");
        eq(w.take(60), 0, "half a notch forward");
        eq(w.take(-120), 3, "then a notch back: the half forward dropped, 3 lines down");
        eq(w.take(-130), 3, "a bit more than a notch: 3 lines, the rest kept");
        eq(w.rest, -10, "the rest");
        w.reset();
        eq(w.rest, 0, "reset");
        // clamped as Up / Down: the window's top within 0 .. total - 18
        eq(std::clamp(1 + w.take(120), 0, dt::max_top(40, dt::kWindowLines)), 0, "near the top: clamped at 0");
        eq(std::clamp(20 + w.take(-120), 0, dt::max_top(40, dt::kWindowLines)), 22, "near the end: clamped at total - 18");
    }

    group("pack info");
    {
        using json = nlohmann::json;
        json e1 = { { "id", 1 }, { "empty", false }, { "timeline", { "12f : 5", "4f : 2 + LP" } }, { "name", "Jab" },
                    { "info", { { "author", "Chris_F (drillcodes)" }, { "source", "community" }, { "description", "Do it" } } } };
        json e2 = { { "id", 2 }, { "empty", true } };
        json e9 = { { "id", 9 }, { "empty", false }, { "inputs", { 0, 1, 2, 3, 4 } } };
        json e3 = { { "id", 3 }, { "empty", false } };                                    // no data: empty
        json dup = { { "id", 1 }, { "empty", true } };                                    // a second id 1: ignored
        json bad = { { "id", 31 }, { "empty", false }, { "timeline", { "1f : 5" } } };   // out of range
        json half = { { "id", 4.5 }, { "empty", false }, { "timeline", { "1f : 5" } } }; // not an integer
        const json data = json::array({ e9, e1, e2, e3, dup, bad, half });
        eq(model::pack::author(data), std::string("Chris_F (drillcodes)"), "info.author");
        eq(model::pack::source(data), std::string("community"), "info.source");
        eq(model::pack::description(data), std::string("Do it"), "info.description (unchanged)");
        eq(model::pack::author(json::array({ e2 })), std::string(), "no entry 1: no author");
        eq(model::pack::source(json::object()), std::string(), "not an array: no source");
        const auto v = model::pack::slot_summaries(data);
        eq(v.size(), (size_t)4, "slots 1, 2, 3, 9 (a duplicate, an id out of range or not an integer: dropped)");
        if (v.size() == 4) {
            eq(v[0].id, 1, "by id: 1 first");
            eq(v[0].frames, (int64_t)16, "slot 1: its timeline's runs summed");
            eq(v[0].name, std::string("Jab"), "slot 1's name");
            check(!v[0].empty, "slot 1 saved");
            check(v[1].empty && v[1].id == 2, "slot 2 empty");
            check(v[2].empty && v[2].id == 3, "slot 3 without data: empty");
            check(!v[3].empty && v[3].id == 9 && v[3].frames == 5, "slot 9: its inputs counted");
        }
        eq(model::pack::slot_summaries(json()).size(), (size_t)0, "nothing for null");
    }
}

}  // namespace

int main() {
    test_windows();
    test_record_cursor();
    test_replay_cursor();
    test_reversal_cursor();
    test_fit();
    test_com_fit();
    test_localised_words();
    test_naming();
    test_community();
    test_tab_guard();
    test_tab_list();
    test_details();
    for (const auto& g : g_order) {
        const auto& c = g_by_group[g];
        std::printf("%-20s %5d checks, %d failed\n", g.c_str(), c.first, c.second);
    }
    std::printf("UI bench (P4): %d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
