// ui_bench — offline tests of the pure logic of the DLL's own screens (phase P4, src/ui/logic.*): the windows over 30
// slots and 30 reversal rows, the cursor answers at the block edges (single cases and whole walks with a model of the game's
// list), the arrows, the slot name fit, the learnt wording, the spin names' ellipsis, the text dialog's outcome, the key
// edges, and the Community dialog (pages, sort, votes, downloads, and the cursor kept after a list refresh).
// Expected values: RecordingManagerTab.lua / ReversalSlots.lua behaviour (line numbers in logic.hpp) and the game's
// own screens. No game, no data. Exit code 0 only when every check passed.
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "model/labels.hpp"
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
    std::string title() override { return "S" + std::to_string(sort) + " p" + std::to_string(page) + "/" + std::to_string(pages()); }
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
    check(d.is_open() && p.opens == 1 && p.menus == 1, "Download Loadouts opens the dialog and fetches");
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
}

// ---- the pause menu's tab under CRSM's dialogs (L1+R1 / A+E) ------------------------------------------------------------
// A model of the game: the data's tab / cursor row and the chain an L1+R1 jump runs (the data written, then CreateViewData
// and InitTabItem, whose pre-hooks call the guard), the game's own L1 / R1 when no dialog is up, the frame callback.
struct TabSim {
    lg::TabGuard g;
    lg::TabSpot data{ 3, 4 };                    // Recording tab, the Manager's "Download Loadouts" row
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

}  // namespace

int main() {
    test_windows();
    test_record_cursor();
    test_replay_cursor();
    test_reversal_cursor();
    test_fit();
    test_naming();
    test_community();
    test_tab_guard();
    test_tab_list();
    for (const auto& g : g_order) {
        const auto& c = g_by_group[g];
        std::printf("%-20s %5d checks, %d failed\n", g.c_str(), c.first, c.second);
    }
    std::printf("UI bench (P4): %d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
