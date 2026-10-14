// p3_tests — pure tests of the P3 layers (no game, no data): replay (P3b), reversal (P3c), record (P3d).
// Each engine runs against a fake port that behaves like the game objects the Lua touches; the expectations are the
// Lua's behaviour (RecordingManagerTab.lua / ReversalSlots.lua, line numbers in the engines' headers).
#include "p3_tests.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "model/draw.hpp"
#include "model/luajson.hpp"
#include "model/record.hpp"
#include "model/replay.hpp"
#include "model/reversal.hpp"
#include "model/store.hpp"
#include "model/timeline.hpp"
#include "model/vslots.hpp"

namespace p3 {
namespace {

using json = nlohmann::json;

json entry(int id, int frames, uint16_t base, const char* name = nullptr, int weight = 1) {
    std::vector<uint16_t> v;
    for (int f = 0; f < frames; ++f) v.push_back((uint16_t)((base + f / 4) & 0x3FF));
    json e;
    e["id"] = id;
    e["empty"] = frames == 0;
    json tl = json::array();
    for (auto& s : model::timeline::compress(v.data(), v.size())) tl.push_back(s);
    e["timeline"] = tl;
    e["weight"] = weight;
    if (name) e["name"] = name;
    return e;
}

// ---- replay: the fake port ---------------------------------------------------------------------------------------------
// Live slots carry an RSM entry as their content (what read_slot would give); write8 behaves as MAIN's apply of one entry
// into slot 8 (content replaced, IsValid from the content, IsActive false, Weight when the entry has one).
struct FakeReplay final : model::replay::Port {
    struct S { bool active = false, valid = false; int64_t weight = 1; json content; };
    S live_[8];
    bool installed_ = false;
    std::map<int, std::pair<bool, int64_t>> entries;       // the installed array's 22 entries
    const model::vslots::VSlots* vs = nullptr;
    int installs = 0, uninstalls = 0, syncs = 0, writes8 = 0;

    bool live(int k0, model::replay::Live* out) override {
        out->present = true; out->active = live_[k0].active; out->valid = live_[k0].valid; out->weight = live_[k0].weight;
        return true;
    }
    void set_active(int k0, bool on) override { live_[k0].active = on; }
    std::optional<json> read8() override { json c = live_[7].content; c["id"] = 8; return c; }
    void write8(const json& e) override {
        ++writes8;
        S& s = live_[7];
        s.content = e;
        s.valid = !(e.contains("empty") && e["empty"].get<bool>());
        s.active = false;
        if (e.contains("weight")) s.weight = e["weight"].get<int64_t>();
    }
    bool install() override {
        if (installed_) return true;
        installed_ = true; ++installs;
        entries.clear();
        for (int n = 9; n <= 30; ++n) {
            const json* e = vs->get(n);
            const bool act = e && e->contains("active") && (*e)["active"] == json(true) && vs->frames(n) > 0;
            entries[n] = { act, (int64_t)vs->weight(n) };
        }
        return true;
    }
    bool installed() override { return installed_; }
    bool entry(int n, bool* a, int64_t* w) override {
        auto it = entries.find(n);
        if (!installed_ || it == entries.end()) return false;
        *a = it->second.first; *w = it->second.second;
        return true;
    }
    void entry_set_active(int n, bool on) override { entries[n].first = on; }
    void uninstall() override { installed_ = false; ++uninstalls; }
    void sync(int) override { ++syncs; }
};

struct ScriptRng final : model::draw::Rng {
    std::vector<double> v; size_t i = 0;
    double next01() override { return i < v.size() ? v[i++] : 0.5; }
    int64_t below(int64_t n) override { return n; }
};

void setup(FakeReplay& p, model::vslots::VSlots& vs) {
    for (int k0 = 0; k0 < 8; ++k0) {
        p.live_[k0].content = entry(k0 + 1, 10 + k0, (uint16_t)(k0 * 3));
        p.live_[k0].valid = true;
        p.live_[k0].active = false;
        p.live_[k0].weight = 1;
    }
    json f = json::object();
    json e9 = entry(9, 30, 100, "nine", 3); e9["active"] = true;
    json e12 = entry(12, 60, 200, nullptr, 4); e12["active"] = true;
    json e15 = entry(15, 90, 300); e15["active"] = false;
    f["9"] = e9; f["12"] = e12; f["15"] = e15;
    vs.load(f);
    p.vs = &vs;
}

void replay_tests() {
    using model::replay::Engine;
    begin("P3b replay: draw over 1-30 (distribution)");
    {
        FakeReplay p; model::vslots::VSlots vs; setup(p, vs);
        p.live_[0].active = true; p.live_[0].weight = 1;            // slot 1, weight 1
        p.live_[2].active = true; p.live_[2].weight = 2;            // slot 3, weight 2
        p.live_[7].active = true;                                   // slot 8, checked by the player: drawn by its weight (13/10)
        const json own8 = p.live_[7].content;
        Engine e;
        model::draw::MtRng rng(77);
        std::map<int, int> hits;
        const int N = 60000;
        bool flags_ok = true, slot8_ok = true;
        for (int i = 0; i < N; ++i) {
            model::replay::Draw d;
            if (!e.pre_draw(p, vs, rng, &d)) { flags_ok = false; break; }
            hits[d.n]++;
            const int id = d.n > 8 ? 7 : d.n - 1;
            for (int k0 = 0; k0 < 8; ++k0) if (p.live_[k0].active != (k0 == id)) flags_ok = false;
            if (d.n > 8 && e.loaded() != d.n) slot8_ok = false;
            if (d.n > 8 && p.live_[7].content["timeline"] != vs.get(d.n)->at("timeline")) slot8_ok = false;
            if (d.n == 8 && (e.loaded() != 0 || p.live_[7].content["timeline"] != own8["timeline"])) slot8_ok = false;
            e.post_draw(p, d);
            if (!p.live_[0].active || !p.live_[2].active || !p.live_[7].active || p.live_[1].active) flags_ok = false;
        }
        const double total = 1 + 2 + 1 + 3 + 4;
        const std::map<int, double> w = { { 1, 1 }, { 3, 2 }, { 8, 1 }, { 9, 3 }, { 12, 4 } };
        for (auto& [n, x] : w) {
            const double pe = x / total, mean = N * pe, sd = std::sqrt(N * pe * (1 - pe));
            check(std::fabs(hits[n] - mean) < 5 * sd, "slot " + std::to_string(n) + " drawn by its weight", [&] { return std::to_string(hits[n]) + " vs " + std::to_string(mean); });
        }
        check(hits.size() == 5 && hits.count(8) && !hits.count(15), "only the checked slots with content, slot 8 included", [&] { return std::to_string(hits.size()); });
        check(flags_ok, "during the call only the winner is active (slot 8 for a virtual); after it the flags are back");
        check(slot8_ok, "a virtual winner rides slot 8 with its own timeline; slot 8 drawn plays its own content");
    }
    begin("P3b replay: SettingRecodData without a checked virtual slot");
    {
        FakeReplay p; model::vslots::VSlots vs; setup(p, vs);
        json f = json::object(); f["15"] = entry(15, 9, 1); vs.load(f);
        Engine e; ScriptRng r;
        model::replay::Draw d;
        check(!e.pre_draw(p, vs, r, &d) && !d.active && p.writes8 == 0, "the game's own draw: nothing touched");
    }
    begin("P3b replay: slot 8 discipline (menu close / draws / menu open)");
    {
        FakeReplay p; model::vslots::VSlots vs; setup(p, vs);
        const json own8 = p.live_[7].content;
        Engine e;
        e.menu_open(p, vs);
        check(p.installed() && p.installs == 1, "menu open: the 30-entry array in place");
        json patch_seen;
        e.menu_close(p, [&]() -> const model::vslots::VSlots& { return vs; }, [&](const json& pt) { patch_seen = pt; });
        check(!p.installed() && p.uninstalls == 1, "menu close: the game's 8 back");
        check(patch_seen.is_object() && patch_seen.contains("9") && patch_seen.contains("12") && patch_seen.contains("15") && !patch_seen.contains("10"),
              "the read-back covers the entries the file holds", [&] { return patch_seen.dump(); });
        check(e.loaded() == 15 || e.loaded() == 12, "the longest checked virtual is loaded into slot 8", [&] { return std::to_string(e.loaded()); });
        check(e.loaded() == 12, "15 is not checked: 12 (60 frames) is the longest checked", [&] { return std::to_string(e.loaded()); });
        check(e.forced8() && p.live_[7].active, "no live slot 1-7 active: slot 8 forced active");
        check(p.live_[7].content["timeline"] == vs.get(12)->at("timeline"), "slot 8 holds virtual 12 while the menu is closed");
        check(e.has_snapshot() && e.snapshot()["timeline"] == own8["timeline"], "slot 8's own content kept aside");
        ScriptRng r; r.v = { 0.0 };                                // the first candidate: virtual 9
        model::replay::Draw d;
        e.pre_draw(p, vs, r, &d);
        check(d.n == 9 && e.loaded() == 9 && p.live_[7].content["timeline"] == vs.get(9)->at("timeline"), "a draw of 9 swaps the virtual in slot 8");
        check(p.live_[7].content.contains("name") && p.live_[7].content["name"] == "nine", "the virtual's name rides along (MAIN names slot 8: kept quirk)");
        e.post_draw(p, d);
        check(p.live_[7].active, "after the call slot 8 is active again (forced)");
        const int writes_before = p.writes8;
        r.v = { 0.0 }; r.i = 0; e.pre_draw(p, vs, r, &d); e.post_draw(p, d);
        check(p.writes8 == writes_before, "the same virtual again: no rewrite of slot 8");
        e.menu_open(p, vs);
        check(e.loaded() == 0 && !e.has_snapshot() && p.live_[7].content["timeline"] == own8["timeline"], "menu open: slot 8 has its own content back");
        check(!e.forced8() && !p.live_[7].active, "menu open: forced8 removed, slot 8 inactive");
        check(p.installed(), "menu open: the array is in place again");
    }
    begin("P3b replay: slot 8 given back by the record layer (own8_back, fix 14/10)");
    {
        FakeReplay p; model::vslots::VSlots vs; setup(p, vs);
        Engine e;
        e.menu_close(p, [&]() -> const model::vslots::VSlots& { return vs; }, [](const json&) {});
        check(e.loaded() == 12 && e.forced8(), "virtual 12 in slot 8, slot 8 forced");
        json own = p.live_[7].content; own["timeline"] = entry(8, 25, 77)["timeline"];
        p.write8(own); p.live_[7].active = true;                   // the record finalize: slot 8's own content back
        e.own8_back();
        check(e.loaded() == 0 && !e.has_snapshot() && e.forced8() && e.label(true, 7) == 0, "nothing rides slot 8, nothing kept aside, forced8 kept; HUD: Slot 8");
        ScriptRng r; r.v = { 0.9 };                                // 9 (w 3) then 12 (w 4): 12
        model::replay::Draw d;
        e.pre_draw(p, vs, r, &d);
        check(d.n == 12 && e.loaded() == 12 && p.live_[7].content["timeline"] == vs.get(12)->at("timeline") && e.label(true, 7) == 12,
              "a draw of 12 loads it again: its recording plays as Slot 12", [&] { return std::to_string(d.n); });
        check(e.has_snapshot() && e.snapshot()["timeline"] == own["timeline"], "slot 8's own content (the recorded one) kept aside");
        e.post_draw(p, d);
    }
    begin("P3b replay: Start Replay with the menu open (22 entries muted)");
    {
        FakeReplay p; model::vslots::VSlots vs; setup(p, vs);
        Engine e;
        e.menu_open(p, vs);
        ScriptRng r; r.v = { 0.99 };
        model::replay::Draw d;
        e.pre_draw(p, vs, r, &d);
        bool muted = true;
        for (auto& [n, x] : p.entries) if (x.first) muted = false;
        check(d.has_vs && muted, "during the call none of the 22 entries is active");
        e.post_draw(p, d);
        check(p.entries[9].first && p.entries[12].first && !p.entries[15].first, "after it the entries are back");
    }
    begin("P3b replay: live slot 8 drawn, forget, import, record arm, hand-back, label");
    {
        FakeReplay p; model::vslots::VSlots vs; setup(p, vs);
        const json own8 = p.live_[7].content;
        p.live_[7].active = true;                                  // slot 8 is a live candidate only without a checked virtual
        Engine e;
        e.menu_close(p, [&]() -> const model::vslots::VSlots& { return vs; }, [](const json&) {});
        check(e.loaded() == 12 && !e.forced8(), "slot 8 active already: not forced");
        check(e.label(true, 7) == 12 && e.label(false, 7) == 0 && e.label(true, 3) == 0, "HUD label: the virtual while the game replays slot 8");
        {                                                          // slot 8 checked, virtual 12 riding it: 8 is drawn with its own content
            ScriptRng r8; r8.v = { 0.0 };                          // the first candidate: slot 8 (before 9, 12)
            model::replay::Draw d8;
            e.pre_draw(p, vs, r8, &d8);
            check(d8.n == 8 && e.loaded() == 0 && p.live_[7].content["timeline"] == own8["timeline"] && p.live_[7].active,
                  "slot 8 drawn while a virtual rides it: its own recording back in slot 8, active", [&] { return std::to_string(d8.n); });
            check(e.label(true, 7) == 0, "HUD label: slot 8's own recording says Slot 8");
            e.post_draw(p, d8);
            r8.v = { 0.99 }; r8.i = 0;
            e.pre_draw(p, vs, r8, &d8);
            check(d8.n == 12 && e.loaded() == 12 && e.has_snapshot() && e.snapshot()["timeline"] == own8["timeline"],
                  "then virtual 12 again: written over slot 8, slot 8's own content kept aside again", [&] { return std::to_string(d8.n); });
            e.post_draw(p, d8);
            FakeReplay q8; model::vslots::VSlots v8; setup(q8, v8);
            q8.live_[7].active = true;
            q8.live_[7].content["empty"] = true; q8.live_[7].valid = false;   // slot 8 checked but empty: never drawn
            Engine e8;
            e8.menu_close(q8, [&]() -> const model::vslots::VSlots& { return v8; }, [](const json&) {});
            bool never8 = true;
            for (double x : { 0.0, 0.3, 0.6, 0.99 }) {
                ScriptRng rx; rx.v = { x };
                model::replay::Draw dx;
                e8.pre_draw(q8, v8, rx, &dx);
                if (dx.n == 8) never8 = false;
                e8.post_draw(q8, dx);
            }
            check(never8, "slot 8 checked without a recording of its own: never drawn");
        }
        e.forget(p, 9);
        check(e.loaded() == 12 && p.syncs == 1, "forget of another slot: entry synced, slot 8 kept");
        e.forget(p, 12);
        check(e.loaded() == 0 && e.has_snapshot(), "forget of the loaded virtual: slot 8 forgets it (snapshot kept, as the Lua)");
        e.menu_open(p, vs);
        check(p.live_[7].content["timeline"] == vs.get(12)->at("timeline"), "the Lua's quirk kept: nothing loaded at the open, slot 8 not restored");
        Engine e2;
        FakeReplay q; model::vslots::VSlots v2; setup(q, v2);
        e2.menu_close(q, [&]() -> const model::vslots::VSlots& { return v2; }, [](const json&) {});
        e2.imported(q);
        check(e2.loaded() == 0 && e2.has_snapshot() && q.syncs == 22, "an import: loaded cleared, the snapshot stays (RMT:2101), 22 entries synced");
        Engine e3;
        FakeReplay s; model::vslots::VSlots v3; setup(s, v3);
        const json own = s.live_[7].content;
        e3.menu_close(s, [&]() -> const model::vslots::VSlots& { return v3; }, [](const json&) {});
        e3.unload_if_loaded(s);
        check(e3.loaded() == 0 && s.live_[7].content["timeline"] == own["timeline"], "the record arm: slot 8's own content back first");
        Engine e4;
        FakeReplay t; model::vslots::VSlots v4; setup(t, v4);
        const json own4 = t.live_[7].content;
        e4.menu_open(t, v4);
        e4.menu_close(t, [&]() -> const model::vslots::VSlots& { return v4; }, [](const json&) {});
        e4.menu_open(t, v4);
        e4.hand_back(t);
        check(!t.installed() && e4.loaded() == 0 && !e4.forced8() && t.live_[7].content["timeline"] == own4["timeline"], "hand-back with the menu open: the game's 8 back");
        Engine e5;
        FakeReplay u; model::vslots::VSlots v5; setup(u, v5);
        const json own5 = u.live_[7].content;
        e5.menu_close(u, [&]() -> const model::vslots::VSlots& { return v5; }, [](const json&) {});
        e5.hand_back(u);
        check(e5.loaded() == 0 && !e5.forced8() && !u.live_[7].active && u.live_[7].content["timeline"] == own5["timeline"], "hand-back in the fight: slot 8 restored, not forced");
        (void)own8;
    }
    begin("P3b replay: fill signature (RMT:953)");
    {
        json e = entry(9, 30, 100);
        const std::string s = model::replay::fill_signature(&e);
        const auto& tl = e["timeline"];
        const std::string want = "30|" + std::to_string(tl.size()) + "|" + tl[0].get<std::string>() + "|" + tl[std::max<size_t>(1, tl.size() / 2) - 1].get<std::string>() + "|" + tl.back().get<std::string>();
        check(s == want, "frames|count|first|middle|last", [&] { return s + " vs " + want; });
        json z = entry(9, 0, 0);
        check(model::replay::fill_signature(&z) == "0" && model::replay::fill_signature(nullptr) == "0", "no content: 0");
    }
}

// ---- reversal ---------------------------------------------------------------------------------------------------------------
void reversal_tests() {
    namespace rv = model::reversal;
    begin("P3c reversal: the picker's choice and the map hygiene (RSL:286-301)");
    {
        auto r = rv::on_set(std::nullopt, 3, 4, 9, false);
        check(r.ignored && !r.map_set && !r.rewrite_skill, "no category: untouched");
        r = rv::on_set(1, 30, 4, 9, false);
        check(r.ignored, "row 30 (31st): untouched");
        r = rv::on_set(0, 7, 4, 14, false);
        check(r.map_set && r.n == 15 && r.rewrite_skill && !r.map_clear, "recording, skill 14: map[0][7] = 15, skill stored as 7");
        r = rv::on_set(2, 29, 4, 29, true);
        check(r.map_set && r.n == 30 && r.rewrite_skill, "skill 29 = slot 30");
        r = rv::on_set(2, 4, 4, 30, true);
        check(!r.map_set && r.map_clear && !r.rewrite_skill, "skill 30 is not a slot: the row back to the game's choice");
        r = rv::on_set(1, 4, 4, 7, true);
        check(!r.map_set && r.map_clear && !r.rewrite_skill, "slot 8 picked for a mapped row: the map entry goes (a later slot 8 must not play the virtual)");
        r = rv::on_set(1, 4, 2, 9, true);
        check(r.map_clear && !r.rewrite_skill, "a special move on a mapped row: map entry cleared, skill untouched");
        r = rv::on_set(1, 4, 2, 9, false);
        check(!r.map_set && !r.map_clear && !r.ignored, "an unmapped row: nothing");
    }
    begin("P3c reversal: the map file (map_set nil keeps the category, as the Lua)");
    {
        model::store::RevMap m;
        m.load(json::parse(R"({"0": {"7": 15}, "2": {"3": 12, "4": 9}})"));
        m.erase(0, 7);
        m.erase(2, 4);
        std::string out, err;
        model::luajson::dump_string(m.to_file(), &out, &err);
        check(out == "{\r\n    \"0\": null,\r\n    \"2\": {\r\n        \"3\": 12\r\n    }\r\n}", "the emptied category is written null", [&] { return out; });
        check(!m.get(0, 7) && m.get(2, 3) && *m.get(2, 3) == json(12), "map_get after the erase");
    }
    begin("P3c reversal: rows 11-30 at the trigger (RSL:553-583)");
    {
        rv::Row rows[rv::kRowsTotal];
        for (int k = 0; k < rv::kRowsTotal; ++k) { rows[k].present = true; rows[k].valid = true; rows[k].type = 2; rows[k].skill = 1; }
        rows[2].active = true; rows[5].active = true;
        model::draw::MtRng rng(5);
        std::map<int, int64_t> map = { { 15, 20 }, { 5, 9 } };
        auto map_of = [&](int k) -> std::optional<int64_t> { auto it = map.find(k); if (it == map.end()) return std::nullopt; return it->second; };
        auto frames = [](int n) -> int64_t { return n == 20 ? 40 : 0; };
        rv::Plan p = rv::present(rows, map_of, frames, rng);
        check(!p.use, "only rows 1-10 active: the game's own array");
        rows[15].active = true; rows[15].type = 4; rows[15].skill = 7;
        rows[5].type = 4; rows[5].skill = 7;
        p = rv::present(rows, map_of, frames, rng);
        check(p.use && p.cands == std::vector<int>({ 2, 5, 15 }), "rows 3, 6 and 16 presented in row order", [&] { return std::to_string(p.cands.size()); });
        check(p.virt == std::vector<int>({ 0, 0, 20 }) && p.need_push, "row 16 plays virtual 20; row 6's slot 9 is empty: as copied");
        rows[15].valid = false;
        p = rv::present(rows, map_of, frames, rng);
        check(!p.use, "an invalid extra row does not count");
        rows[15].valid = true;
        for (int k = 10; k < 24; ++k) rows[k].active = true;
        bool ok = true;
        for (int i = 0; i < 2000; ++i) {
            p = rv::present(rows, map_of, frames, rng);
            std::set<int> u(p.cands.begin(), p.cands.end());
            if (!p.use || p.cands.size() != 10 || u.size() != 10) ok = false;
            for (int k : p.cands) if (!rows[k].active || !rows[k].valid) ok = false;
        }
        check(ok, "16 active rows: a random ten of them, distinct, each active and valid");
    }
    begin("P3c reversal: virtual rows of the game's array (RSL:603-613)");
    {
        std::map<int64_t, json> mp = { { 3, 12 }, { 5, 15 }, { 14, 9 }, { 6, json(13.0) } };
        rv::Row game[10];
        for (int k = 0; k < 10; ++k) { game[k].present = true; game[k].type = 2; game[k].skill = 0; }
        game[3].type = 4; game[3].skill = 7;
        game[5].type = 4; game[5].skill = 2;                       // the game's own slot 3 since: not virtual any more
        game[6].type = 4; game[6].skill = 7;
        auto row = [&](int r, rv::Row* out) { if (r < 0 || r >= 10) return false; *out = game[r]; return true; };
        auto frames = [](int n) -> int64_t { return (n == 12 || n == 13) ? 30 : 0; };
        const auto ch = rv::mapped(mp, row, frames);
        check(ch.size() == 2 && ch[0].row == 3 && ch[0].n == 12 && ch[1].row == 6 && ch[1].n == 13,
              "rows 4 and 7 point at slots 12 and 13; row 6 is no longer slot 8; row 15 is past the game's 10", [&] { return std::to_string(ch.size()); });
        auto none = [](int) -> int64_t { return 0; };
        check(rv::mapped(mp, row, none).empty(), "empty virtual slots: nothing changed");
    }
}

// ---- record ----------------------------------------------------------------------------------------------------------------
struct FakeRecord final : model::record::Port {
    json slot8;
    std::map<int, json> vs;
    int writes8 = 0, stores = 0, own8_backs = 0;
    bool nil8 = false;
    std::optional<json> read8() override { if (nil8) return std::nullopt; return slot8; }
    void write8(const json& e) override { ++writes8; slot8 = e; }
    const json* vslot(int n) override { auto it = vs.find(n); return it == vs.end() ? nullptr : &it->second; }
    void store(int n, const json& e) override { ++stores; vs[n] = e; }
    void own8_back() override { ++own8_backs; }
    int64_t frames(const json& e) override {
        model::vslots::VSlots v; v.put(9, e); return v.frames(9);
    }
};

void record_tests() {
    using model::record::Engine;
    using model::record::kTickEvery;
    begin("P3d record: a recording into slot 12 through slot 8 (RMT:510-588)");
    {
        FakeRecord p;
        p.slot8 = entry(8, 20, 5, "eight");
        json old = entry(12, 10, 9); old["active"] = true; old["weight"] = 3; old["name"] = "my combo";
        p.vs[12] = old;
        Engine e;
        e.arm(p, 12, 1000);
        check(e.armed() && e.slot() == 12 && e.label() == 12, "armed: the HUD says slot 12");
        uint64_t t = 1000;
        check(!e.tick(p, t += kTickEvery, 0, false), "not started yet (countdown): nothing");
        p.slot8 = entry(8, 75, 40, "eight");                       // the game records slot 8
        check(!e.tick(p, t += kTickEvery, 3, false) && e.seen_now(), "RECORD_NOW seen");
        check(e.tick(p, t += kTickEvery, 0, false), "back to NONE: finalized");
        const json& s = p.vs[12];
        check(s["id"] == 12 && s["timeline"] == entry(8, 75, 40)["timeline"], "slot 12 holds the new recording");
        check(s["active"] == true && s["weight"] == 3 && s["name"] == "my combo", "slot 12 keeps its On / Off, weight and name");
        check(p.slot8["timeline"] == entry(8, 20, 5)["timeline"] && p.slot8["id"] == 8, "slot 8 has its own content back");
        check(!e.armed() && e.label() == 0 && e.restore_pending(), "disarmed; the second restore is queued");
        check(p.own8_backs == 1, "ended on its own (no menu): the replay layer told slot 8 holds its own content (fix 14/10)");
        const int w = p.writes8;
        e.arm(p, 14, t + 100);
        check(!e.restore_pending() && p.writes8 == w, "the Lua's gate: the restore never fires between recordings, a new arm drops it");
    }
    begin("P3d record: pause, deadline, menu open, retarget / drop, slot 8's name and weight");
    {
        FakeRecord p; p.slot8 = entry(8, 20, 5);
        Engine e;
        e.arm(p, 15, 0);
        e.tick(p, 10, 3, false);
        p.slot8 = entry(8, 30, 1);
        check(e.tick(p, 20, 3, true), "the pause menu opened while recording: finalized");
        check(p.own8_backs == 0, "finalized under the pause: no own8_back (the menu open's replay unload follows)");
        check(p.vs.count(15) && p.vs[15]["timeline"] == entry(8, 30, 1)["timeline"] && !p.vs[15].contains("active"), "slot 15 created from slot 8 (no old entry: nothing kept)");
        check(p.vs[15]["id"] == 15 && p.vs[15]["weight"] == 1 && !p.vs[15].contains("name"), "a new slot 15: weight 1, no name");

        FakeRecord q; q.slot8 = entry(8, 20, 5);
        Engine d;
        d.arm(q, 16, 0);
        check(!d.tick(q, model::record::kDeadline, 0, false) && d.tick(q, model::record::kDeadline + 1, 0, false), "never started: finalized at the 2-minute deadline");
        check(!q.vs.count(16) && q.writes8 == 1 && q.own8_backs == 1, "nothing stored, slot 8 written back (own8_back: the game runs)");

        FakeRecord r; r.slot8 = entry(8, 20, 5, "slot eight name");
        json unnamed = entry(18, 6, 7, nullptr, 4);
        r.vs[18] = unnamed;
        Engine m;
        m.arm(r, 17, 0);
        m.menu_open(r);
        check(m.armed(), "menu opened before the recording started: still armed (re-targetable)");
        m.retarget(18);
        check(m.slot() == 18, "F on another virtual row: re-targeted");
        m.tick(r, 10, 3, false);
        r.slot8 = entry(8, 44, 2, "virtual twelve", 6);           // slot 8 named and weighted after the virtual riding it
        m.menu_open(r);
        check(!m.armed() && r.vs.count(18) && r.vs[18]["timeline"] == entry(8, 44, 2)["timeline"], "menu open after the start: stored before the rows are built");
        check(r.own8_backs == 0, "menu open: no own8_back");
        check(!r.vs[18].contains("name") && r.vs[18]["weight"] == 4, "slot 18 unnamed with its weight 4: slot 8's name and weight stay out (fix 14/10)",
              [&] { return r.vs[18].dump(); });

        FakeRecord u; u.slot8 = entry(8, 20, 5);
        Engine z;
        z.arm(u, 19, 0);
        z.drop();
        check(!z.armed() && u.writes8 == 0, "F on a live row: the pending recording dropped, slot 8 untouched");

        FakeRecord v; v.nil8 = true;
        Engine y;
        y.arm(v, 20, 0);
        y.tick(v, 10, 3, false);
        v.nil8 = false; v.slot8 = entry(8, 9, 3);
        y.tick(v, 20, 0, false);
        check(v.vs.count(20) && v.writes8 == 0 && !y.restore_pending() && v.own8_backs == 0, "no snapshot (read_slot nil at the arm): stored, slot 8 not written");
    }
    begin("P3d record: only the recording moves into n (recorded_entry, fix 14/10)");
    {
        using model::record::recorded_entry;
        json rec = entry(8, 30, 9, "virtual twelve", 7); rec["active"] = true;
        const json a = recorded_entry(25, rec, nullptr);
        check(a["id"] == 25 && a["timeline"] == rec["timeline"] && a["empty"] == false && a["weight"] == 1 && !a.contains("name") && !a.contains("active"),
              "no entry: the recording with a new slot's defaults", [&] { return a.dump(); });
        json old = json::object(); old["id"] = 25; old["empty"] = true; old["timeline"] = json::array(); old["name"] = ""; old["active"] = false;
        const json b = recorded_entry(25, rec, &old);
        check(b["name"] == "" && b["active"] == false && !b.contains("weight") && b["timeline"] == rec["timeline"] && b["empty"] == false,
              "an entry: its own name (empty included), On / Off and weight (absent stays absent)", [&] { return b.dump(); });
        json named = old; named["name"] = "mine"; named["weight"] = 9;
        const json c = recorded_entry(25, rec, &named);
        check(c["name"] == "mine" && c["weight"] == 9, "a named entry: its name and weight", [&] { return c.dump(); });
    }
    begin("P3d record: capture slot 8 into n (fallback), hand-back");
    {
        FakeRecord p; p.slot8 = entry(8, 33, 4);
        json old = entry(21, 5, 1); old["weight"] = 2; p.vs[21] = old;
        Engine e;
        check(e.capture8(p, 21) && p.vs[21]["timeline"] == entry(8, 33, 4)["timeline"] && p.vs[21]["weight"] == 2, "slot 8 stored into 21, weight kept");
        p.slot8 = entry(8, 0, 0);
        check(!e.capture8(p, 22) && !p.vs.count(22), "an empty slot 8: nothing");
        FakeRecord h; h.slot8 = entry(8, 20, 5);
        Engine g;
        g.arm(h, 23, 0);
        g.tick(h, 10, 3, false);
        h.slot8 = entry(8, 50, 6);
        g.hand_back(h);
        check(!g.armed() && h.vs.count(23) && h.slot8["timeline"] == entry(8, 20, 5)["timeline"], "hand-back during a recording: stored, slot 8 restored");
    }
}

}  // namespace

void run_pure_tests() {
    std::printf("[P3b] replay (pure)\n");
    replay_tests();
    std::printf("[P3c] reversal (pure)\n");
    reversal_tests();
    std::printf("[P3d] record (pure)\n");
    record_tests();
}

}  // namespace p3
