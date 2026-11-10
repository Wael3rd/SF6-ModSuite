// options_bench.cpp - offline bench of the option engine's state machine (core/src/options/options_core.cpp, compiled
// as it ships) with a scripted backend: no game, no REFramework. Run: core\tests\options_bench\run.bat
//
// Covers increment 11 (ADR-0021, a group placed in a section of the game's Options screen) and checks that a tree
// without a placed group behaves as before (no placement call, its pages recorded as before). Increment 13 (a group as a
// section of a game tab's left list): the registry side (the units, the screen and the holds are options.cpp's). Increment 14
// (a section whose right side is the game's sub list of its entries): placed entries, their reads, the issued ids.
#include "../../src/options/options_core.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace oc = dialogs::options::core;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else { ++g_fail; std::printf("FAIL: %s\n", what); }
}

// ---- scripted backend ----
std::map<oc::Obj, int32_t> g_values;      // unit -> its value (get_value)
int g_next_obj = 1;
int g_sync_calls = 0, g_sync_keep = 0, g_sync_drop = 0;
oc::Obj new_obj() { return reinterpret_cast<oc::Obj>(static_cast<uintptr_t>(0x1000 * g_next_obj++)); }

// What be_build_units does with the registry, without the game: a unit per group (none for a placed one, nothing for a
// section) and per entry (buttons of a placed group skipped; a section with rows: its entries as a placed group's), type ids
// by key.
bool be_build() {
    oc::build_set_root(new_obj());
    for (auto& gp : oc::groups()) {
        if (gp->section_tab >= 0 && !gp->section_rows) { oc::build_set_group(*gp, nullptr); continue; }
        const bool placed = gp->place_tab >= 0 || gp->section_rows;
        oc::build_set_group(*gp, placed ? nullptr : new_obj());
        for (auto& e : gp->entries) {
            if (placed && e->kind == oc::Kind::Button) continue;
            const int32_t tid = oc::type_id_for_key("e:" + oc::id_key_of(*gp) + "/" + e->key);
            oc::Obj u = new_obj();
            g_values[u] = (int32_t)e->value;
            oc::build_set_entry(e, tid, u);
        }
    }
    return true;
}
bool be_get_value(oc::Obj u, int32_t& out) { auto it = g_values.find(u); if (it == g_values.end()) return false; out = it->second; return true; }
void be_set_value(oc::Obj u, int32_t v, bool) { g_values[u] = v; }
int32_t be_base() { return 100000; }
void be_sync(bool keep_live) { ++g_sync_calls; if (keep_live) ++g_sync_keep; else ++g_sync_drop; }
void be_purge(bool) {}
bool be_agent(const char*) { return false; }

std::vector<std::string> events() {
    std::vector<std::string> out;
    std::string s = oc::pull_events(), cur;
    for (char c : s) { if (c == '\x1e') { out.push_back(cur); cur.clear(); } else cur += (c == '\x1f' ? ' ' : c); }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
bool has_event(const std::vector<std::string>& ev, const std::string& want) {
    for (auto& e : ev) if (e == want) return true;
    return false;
}

void run_ticks(int64_t& tick, int n) { for (int i = 0; i < n; ++i) { ++tick; oc::tick_a(tick, false, false); oc::tick_a2(); oc::tick_b(); } }

}  // namespace

int main() {
    oc::Backend be;
    be.build_units = be_build; be.purge_roots = be_purge; be.get_value = be_get_value; be.set_value = be_set_value;
    be.type_id_base = be_base; be.agent_present = be_agent; be.sync_placed = be_sync;
    oc::set_backend(be);
    int64_t tick = 0;

    // ---- 1. a tree without a placed group: as before ----
    oc::reset();
    const int64_t page = oc::add_group("page");
    const int64_t pe = oc::add_entry(page, oc::Kind::Choice, "spin");
    oc::set_entry_def(pe, "Spin", "", { "A", "B" }, 0, 0, 0);
    check(oc::build(), "1 build");
    run_ticks(tick, 120);
    check(g_sync_calls == 0, "1 no placement call without a placed group");
    auto e1 = oc::entry_by_id(pe);
    check(e1 && !e1->placed && !oc::placed_type(e1->type_id) && oc::known(e1->type_id), "1 page entry known, not placed");
    check(oc::on_page_setup(reinterpret_cast<oc::Obj>(0x77), { { e1->type_id, 0, reinterpret_cast<oc::Obj>(0x78) } }), "1 its page is ours");
    events();

    // ---- 2. a placed group next to it ----
    const int64_t sec = oc::add_group("nsfw");
    const int64_t te = oc::add_entry(sec, oc::Kind::Choice, "display_nsfw");          // before the placement
    const int64_t bt = oc::add_entry(sec, oc::Kind::Button, "never_shown");
    oc::set_group_place(sec, 1, 290);
    const int64_t late = oc::add_entry(sec, oc::Kind::Choice, "late");                 // after: inherits it
    oc::set_entry_def(te, "Display NSFW Outfits", "", { "Off", "On" }, 0, 0, 0);
    oc::set_entry_def(late, "Late", "", { "Off", "On" }, 0, 0, 1);
    auto t = oc::entry_by_id(te), b = oc::entry_by_id(bt), l = oc::entry_by_id(late);
    check(t && t->placed && b && b->placed && l && l->placed, "2 every entry of the group placed");
    oc::rebuild();
    check(oc::build(), "2 build");
    const auto& gs = oc::groups();
    check(gs.size() == 2 && gs[1]->unit == nullptr && gs[1]->place_tab == 1 && gs[1]->place_section == 290, "2 placed group: no unit, its section");
    check(t->unit && t->type_id && oc::placed_type(t->type_id) && oc::known(t->type_id), "2 placed entry built, known and placed");
    check(b->unit == nullptr && !oc::known(b->type_id), "2 button of a placed group not built");
    check(!oc::placed_type(e1->type_id), "2 page entry still not placed");

    // a game page showing only the placed row: not ours (the game's Restore Default row stays)
    check(!oc::on_page_setup(reinterpret_cast<oc::Obj>(0x88), { { t->type_id, 0, reinterpret_cast<oc::Obj>(0x89) } }), "2 game page with the placed row is not ours");
    // a page with both: ours, only the page entry recorded
    check(oc::on_page_setup(reinterpret_cast<oc::Obj>(0x90), { { t->type_id, 0, reinterpret_cast<oc::Obj>(0x91) }, { e1->type_id, 0, reinterpret_cast<oc::Obj>(0x92) } }), "2 mixed page ours");
    auto ev = events();
    check(has_event(ev, "S " + std::to_string(reinterpret_cast<uintptr_t>(reinterpret_cast<oc::Obj>(0x90))) + " " + std::to_string(pe) + " " + std::to_string(reinterpret_cast<uintptr_t>(reinterpret_cast<oc::Obj>(0x92)))),
          "2 mixed page records the page entry only");
    check(!oc::on_button(t->type_id), "2 a placed choice is no button");

    // ---- 3. the player changes the row in the game's Options: one V event at the next full pass ----
    g_sync_calls = 0;
    g_values[t->unit] = 1;
    run_ticks(tick, 10);
    ev = events();
    check(has_event(ev, "V " + std::to_string(te) + " 1"), "3 value change reported");
    run_ticks(tick, 10);
    ev = events();
    check(!has_event(ev, "V " + std::to_string(te) + " 1"), "3 reported once");
    // sync_placed every kPlaceCheck ticks, keep_live
    g_sync_calls = g_sync_keep = 0;
    run_ticks(tick, oc::kPlaceCheck * 3);
    check(g_sync_calls == 3 && g_sync_keep == 3, "3 placement checked every kPlaceCheck ticks");

    // ---- 4. the client sets the value: written to the unit, no echo ----
    oc::set_value(te, 0, false);
    run_ticks(tick, 20);
    ev = events();
    check(g_values[t->unit] == 0 && !has_event(ev, "V " + std::to_string(te) + " 0"), "4 client value set without echo");

    // ---- 5. rebuild: same type id, new unit ----
    const int32_t tid_before = t->type_id;
    oc::Obj unit_before = t->unit;
    oc::rebuild();
    check(oc::build(), "5 rebuild");
    check(t->type_id == tid_before && t->unit != unit_before, "5 same TypeId, new unit");

    // ---- 6. back to the core's window ----
    oc::set_group_place(sec, -1, -1);
    check(!t->placed && !l->placed, "6 entries back in the window");
    oc::rebuild();
    check(oc::build(), "6 build");
    check(gs[1]->unit != nullptr && b->unit != nullptr && !oc::placed_type(t->type_id), "6 group page again, button built");
    g_sync_calls = 0;
    run_ticks(tick, oc::kPlaceCheck);
    check(g_sync_calls == 1, "6 still checked once a group was placed (its stale rows go)");

    // ---- 7. remove a placed group: the check keeps running (removes its rows) ----
    oc::set_group_place(sec, 1, 290);
    oc::remove_group(sec);
    g_sync_calls = 0;
    run_ticks(tick, oc::kPlaceCheck);
    check(g_sync_calls == 1, "7 removed placed group: rows checked");

    // ---- 8. reset forgets everything ----
    oc::reset();
    g_sync_calls = 0;
    const int64_t g8 = oc::add_group("p8");
    oc::add_entry(g8, oc::Kind::Slider, "s");
    oc::build();
    run_ticks(tick, oc::kPlaceCheck * 2);
    check(g_sync_calls == 0, "8 after reset, no placement call without a placed group");

    // ---- 9. increment 13: a group as a section of a game tab's left list (the registry) ----
    oc::reset();
    g_sync_calls = 0;
    const int64_t s9 = oc::add_group("slots.title_section");
    const int64_t e9 = oc::add_entry(s9, oc::Kind::Choice, "ignored");
    oc::set_group_def(s9, "Title Screen Selection", "Choose the title screen.");
    check(oc::sections().empty(), "9 no section before the placement");
    check(oc::set_group_section(s9, 1, 290), "9 placed");
    check(!oc::set_group_section(9999, 1, 290), "9 unknown group refused");
    auto secs = oc::sections();
    check(secs.size() == 1 && secs[0].gid == s9 && secs[0].tab == 1 && secs[0].before == 290 && secs[0].key == "slots.title_section" &&
          secs[0].title == "Title Screen Selection" && secs[0].desc == "Choose the title screen.", "9 its definition");
    auto x9 = oc::entry_by_id(e9);
    check(x9 && !x9->placed, "9 its entries are no placed rows");
    check(oc::build() && oc::groups()[0]->unit == nullptr, "9 a section has no page unit");
    oc::set_group_section(s9, 1, -7);
    secs = oc::sections();
    check(secs.size() == 1 && secs[0].before == -1, "9 a negative before = at the end");
    oc::set_group_def(s9, "Selection de l'ecran titre", "");
    check(oc::sections()[0].title == "Selection de l'ecran titre", "9 retitled");
    run_ticks(tick, oc::kPlaceCheck * 2);
    check(g_sync_calls == 0, "9 a section is no placed group (no row sync)");
    oc::set_group_section(s9, -1, -1);
    check(oc::sections().empty(), "9 no section any more");
    oc::set_group_section(s9, 1, 290);
    oc::remove_group(s9);
    check(oc::sections().empty(), "9 removed with its group");
    oc::set_group_section(oc::add_group("other"), 1, 290);
    oc::reset();
    check(oc::sections().empty(), "9 reset forgets the sections");

    // ---- 10. increment 14: a section whose right side is the game's sub list holding its entries ----
    oc::reset();
    g_sync_calls = 0;
    const int64_t s10 = oc::add_group("slots.rows");
    const int64_t a10 = oc::add_entry(s10, oc::Kind::Choice, "a");
    const int64_t b10 = oc::add_entry(s10, oc::Kind::Button, "never_a_row");
    oc::set_entry_def(a10, "A", "", { "Off", "On" }, 0, 0, 0);
    check(oc::set_group_section(s10, 1, 290, true), "10 placed with rows");
    const int64_t c10 = oc::add_entry(s10, oc::Kind::Choice, "c");                    // after: inherits it
    oc::set_entry_def(c10, "C", "", { "1", "2", "3" }, 0, 0, 2);
    auto xa = oc::entry_by_id(a10), xb = oc::entry_by_id(b10), xc = oc::entry_by_id(c10);
    check(xa && xa->placed && xb && xb->placed && xc && xc->placed, "10 its entries are placed (never loaded, reset or saved)");
    secs = oc::sections();
    check(secs.size() == 1 && secs[0].rows && secs[0].entries.size() == 2 && secs[0].entries[0] == a10 && secs[0].entries[1] == c10,
          "10 its definition: rows, the two value entries in order, no button");
    oc::rebuild();
    check(oc::build() && oc::groups()[0]->unit == nullptr, "10 built, no page unit");
    check(xa->unit && xc->unit && oc::placed_type(xa->type_id) && oc::placed_type(xc->type_id) && !xb->unit, "10 rows built and placed, button not");
    check(!oc::on_page_setup(reinterpret_cast<oc::Obj>(0xA0), { { xa->type_id, 0, reinterpret_cast<oc::Obj>(0xA1) } }), "10 a page showing a row is not ours");
    events();
    g_values[xa->unit] = 1;                                                            // the window is closed: the full pass reads them
    run_ticks(tick, 10);
    ev = events();
    check(has_event(ev, "V " + std::to_string(a10) + " 1"), "10 a change read by the full pass with the window closed");
    g_values[xc->unit] = 0;
    oc::poll_group(s10);                                                               // the player on its rows: read at once
    ev = events();
    check(has_event(ev, "V " + std::to_string(c10) + " 0") && ev.size() == 1, "10 poll_group reports the change at once, once");
    oc::poll_group(s10);
    check(events().empty(), "10 nothing more without a change");
    check(g_sync_calls == 0, "10 a section with rows is no placed group (no game-section sync)");
    const int32_t tid10 = xa->type_id;
    check(oc::issued_type_id(tid10) && !oc::issued_type_id(290) && !oc::issued_type_id(100000), "10 issued ids: ours only");
    oc::rebuild();
    check(!oc::known(tid10) && oc::issued_type_id(tid10), "10 after a rebuild: not known, still issued (the save skips it)");
    oc::build();
    check(oc::set_group_section(s10, 1, 290, false), "10 back to a client page");
    check(!xa->placed && !xc->placed && !oc::sections()[0].rows && oc::sections()[0].entries.empty(), "10 a client page: no placed entry, no row");
    // the game's "Restore Default Settings": the value each row goes to (the game's int)
    check(oc::default_of(*xa) == 0 && oc::default_of(*xc) == 0, "10 default without one: the first value (1-based 1)");
    oc::set_entry_default(c10, 2);
    check(oc::default_of(*xc) == 2 && xc->has_default, "10 the entry's own default");
    oc::set_entry_default(c10, 7);
    check(oc::default_of(*xc) == 2, "10 a default past the values: the last one");
    const int64_t sl10 = oc::add_entry(s10, oc::Kind::Slider, "sl");
    oc::set_entry_def(sl10, "S", "", {}, 5, 20, 9);
    auto xs = oc::entry_by_id(sl10);
    check(xs && oc::default_of(*xs) == 5, "10 slider default without one: its minimum");
    oc::set_entry_default(sl10, 12);
    check(oc::default_of(*xs) == 12, "10 slider's own default");
    oc::set_group_section(s10, -1, -1, true);
    check(oc::sections().empty() && !oc::groups()[0]->section_rows && !xa->placed, "10 no section: rows off too");
    oc::reset();
    check(!oc::issued_type_id(tid10), "10 reset forgets the issued ids");

    std::printf("options_bench: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
