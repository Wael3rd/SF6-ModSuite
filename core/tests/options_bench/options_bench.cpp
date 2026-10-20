// options_bench.cpp - offline bench of the option engine's state machine (core/src/options/options_core.cpp, compiled
// as it ships) with a scripted backend: no game, no REFramework. Run: core\tests\options_bench\run.bat
//
// Covers increment 11 (ADR-0021, a group placed in a section of the game's Options screen) and checks that a tree
// without a placed group behaves as before (no placement call, its pages recorded as before).
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

// What be_build_units does with the registry, without the game: a unit per group (none for a placed one) and per
// entry (buttons of a placed group skipped), type ids by key.
bool be_build() {
    oc::build_set_root(new_obj());
    for (auto& gp : oc::groups()) {
        const bool placed = gp->place_tab >= 0;
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

    std::printf("options_bench: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
