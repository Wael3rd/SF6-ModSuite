// cc_rec.hpp - Custom Combos: the recording slot ([REC], the first Beginner row of every fighter's list) and the seam the
// recorder plugs into.
//
// This file: the slot is a trial like the others, its settings read from _rec_settings.json (written by the training
// menu inside the slot), its preview one line with a hint until a take exists.
//
// The recorder (cc_recorder.hpp: the takes, the slot's pause-menu rows, the training menu inside the slot, replay
// recording) implements the Recorder below. What the mode calls on it, and when:
//   - frame(flow)                      every frame, first thing of the mode's tick (game thread)
//   - tick(mission, trial, chara)      every frame of the fight while the slot is the trial under way (game thread)
//   - leave()                          the slot was left (another row, the menus, the flow)
//   - on_attack(mission, attacker)     the judge's TrialOnAttack, pre (the mode owns the hook: one owner per hook)
//   - training_menu_open()             the training menu is up in the slot: its values are the truth, the mode stops
//                                      pushing gauges / dummy / counter and leaves the guide line alone
//   - has_take / preview_steps         what the list previews for the slot
//   - fixed_start()                    a start position set in the pause menu stays after a take
//   - replay_tick(in_replay), replay_on()   replays (flow 10)
// What the mode lends the recorder (custom_combos.cpp, namespace cc::mode): rebuild the recipe (rebuild_recipe), the
// recipe on screen (recipe_ctrl), the settings pushed on the asset and live (apply_settings), a saved / discarded take
// joining or leaving its fighter's set (on_saved / on_discard), find_recorded, input_modern, example_playing.
//
// Coexistence notes for part 2 (the Training pause menu is hooked by the core's pause-menu service, the core's option
// service and the Recording Manager): the training menu opened inside a combo trial shares TrainingPauseMenuUserData
// with Training mode, so its tab list must be ours only while our menu is open and the game's array put back on closing;
// the half speed / pause type 8 must be given back; no hook on app.training.UIFlowTrainingMenu.Param,
// TrainingManager.OpenMenu or the Recording Manager's methods; every write into TrainingManager._tData inside a trial
// of ours only, and put back on leaving (restore_* in the mode).
#pragma once

#include "cc_trials.hpp"

namespace cc::rec {

constexpr int kRows = 12;                   // rows of the native recipe
constexpr int64_t kPlaceholder = 59900;     // action ids nobody performs: the slot's own judge never clears a row
constexpr int64_t kBlankSkill = 3820;       // a line needs a skill to get a row: a Modern-only id whose Classic command is empty
inline const char* kSlotTitle = "[REC] Record a combo";
inline const char* kSlotHint = "Play a combo in this row: it is saved as a trial.";

// The skill of a line whose text is ours: one with a name but no command in the control type on screen, so the game
// builds a row and writes nothing in it. Classic: a Modern-only id; Modern: a Classic normal.
inline int64_t blank_skill(bool modern) { return modern ? 120 : kBlankSkill; }

// What the slot starts a take with (_rec_settings.json), as the training menu inside the slot left it.
//   sa / dg: 0 = max, else bars + 1; sa2 / dg2: the dummy's bars; guard_raw: TM_GS_GUARD_TYPE (wins over `guard`);
//   vp1 / vp2: life in percent (the asset's VGaugePoint); vh / sh / dh: how life, Super and Drive come back (the
//   asset's ERecoveryType: 0 NONE = the menu's "Standard", 1 RECOVERY = "Refill", 2 LOCK = "Fixed"), 1 = player,
//   2 = dummy; uq: a fighter's own gauge per fighter id, { s: stock 0..6 / 7 infinite, t: timer 0 off / 1 started /
//   2 infinite }, the Character-Specific Settings page's two values; ug: the old pause-menu row's stock (read only
//   for a fighter with no uq entry).
struct OwnGauge { int s = 0, t = 0; };
struct Settings {
    int pos = 0, state = 0, guard = 1, sa = 0, dg = 0, ug = 0, sa2 = 0, dg2 = 6;
    int vp1 = 100, vp2 = 100, vh1 = 1, vh2 = 1, sh1 = 1, sh2 = 1, dh1 = 1, dh2 = 1;
    std::optional<int> counter, guard_raw;
    std::map<int, OwnGauge> uq;
};

inline Settings settings_from(const json& j) {
    Settings s;
    if (!j.is_object()) return s;
    auto rd = [&](const char* k, int mx, int& dst) {
        if (auto v = num(at(j, k))) dst = std::max(0, std::min(mx, static_cast<int>(std::floor(*v))));
    };
    rd("pos", 3, s.pos); rd("state", 2, s.state); rd("guard", 3, s.guard); rd("sa", 3, s.sa); rd("dg", 6, s.dg);
    rd("ug", 6, s.ug); rd("sa2", 3, s.sa2); rd("dg2", 6, s.dg2);
    rd("vp1", 100, s.vp1); rd("vp2", 100, s.vp2); rd("vh1", 2, s.vh1); rd("vh2", 2, s.vh2);
    rd("sh1", 2, s.sh1); rd("sh2", 2, s.sh2); rd("dh1", 2, s.dh1); rd("dh2", 2, s.dh2);
    if (auto v = num(at(j, "counter"))) s.counter = std::max(0, std::min(2, static_cast<int>(std::floor(*v))));
    if (auto v = num(at(j, "guard_raw"))) s.guard_raw = std::max(0, std::min(5, static_cast<int>(std::floor(*v))));
    if (const json* u = obj(&j, "unique"))
        for (auto it = u->begin(); it != u->end(); ++it) {
            const json k = it.key();
            const auto id = num(k);
            if (!id || !it->is_object()) continue;
            OwnGauge g;
            g.s = std::max(0, std::min(7, static_cast<int>(std::floor(num(at(*it, "s")).value_or(0)))));
            g.t = std::max(0, std::min(2, static_cast<int>(std::floor(num(at(*it, "t")).value_or(0)))));
            s.uq[static_cast<int>(std::floor(*id))] = g;
        }
    return s;
}
inline Settings load_settings(const fs::path& data_dir) { return settings_from(read_json(data_dir / "_rec_settings.json")); }

inline json settings_json(const Settings& s) {
    json j = { { "pos", s.pos }, { "state", s.state }, { "guard", s.guard }, { "sa", s.sa }, { "dg", s.dg }, { "ug", s.ug },
               { "sa2", s.sa2 }, { "dg2", s.dg2 }, { "vp1", s.vp1 }, { "vp2", s.vp2 }, { "vh1", s.vh1 }, { "vh2", s.vh2 },
               { "sh1", s.sh1 }, { "sh2", s.sh2 }, { "dh1", s.dh1 }, { "dh2", s.dh2 } };
    if (s.counter) j["counter"] = *s.counter;
    if (s.guard_raw) j["guard_raw"] = *s.guard_raw;
    if (!s.uq.empty()) {
        json u = json::object();
        for (const auto& [id, g] : s.uq) u[std::to_string(id)] = { { "s", g.s }, { "t", g.t } };
        j["unique"] = u;
    }
    return j;
}

// Fighters whose own gauge is a stock: only read for the old row value `ug` (the live level is cPlayer.mStyleNo for
// Jamie alone, measured)
inline bool stock_fighter(int id) {
    switch (id) { case 3: case 5: case 12: case 15: case 16: case 20: case 21: case 28: case 32: case 33: return true; default: return false; }
}

// The raw team fields the settings give: what differs from the baseline every trial starts from, and the dummy's bars
// (always). `any`: something is not the baseline.
inline json settings_teams(const Settings& rs, bool* any = nullptr) {
    json p = json::object(), d = { { "SGaugeStock", rs.sa2 }, { "DGaugeStock", rs.dg2 } };
    if (rs.vp1 != 100) p["VGaugePoint"] = rs.vp1;
    if (rs.vh1 != 1) p["VGaugeHealType"] = rs.vh1;
    if (rs.vp2 != 100) d["VGaugePoint"] = rs.vp2;
    if (rs.vh2 != 1) d["VGaugeHealType"] = rs.vh2;
    if (rs.sh2 != 1) d["SGaugeHealType"] = rs.sh2;
    if (rs.dh2 != 1) d["DGaugeHealType"] = rs.dh2;
    if (any) *any = !p.empty() || rs.sa2 != 0 || rs.dg2 != 6 || d.size() > 2;
    return json::array({ p, d });
}

// A fighter's own gauge as the asset takes it: always UGaugeType 1 (CNT_START) with the menu's two values (2 and 3
// force everything to its maximum, measured). nullopt: nothing set. `set`: the menu has an entry for this fighter.
inline std::optional<json> own_gauge(const Settings& rs, int id, bool* set = nullptr) {
    auto it = rs.uq.find(id);
    if (set) *set = it != rs.uq.end();
    const int s = it != rs.uq.end() ? it->second.s : (stock_fighter(id) ? rs.ug : 0);
    const int t = it != rs.uq.end() ? it->second.t : 0;
    if (s <= 0 && t <= 0) return std::nullopt;
    json u = { { "type", 1 } };
    if (s > 0) u["stock"] = s;
    if (t > 0) u["timer"] = t;
    return u;
}
// on a slot: what the menu set wins; a fighter the menu never set keeps what the last take left
inline void slot_own_gauge(Trial& slot, const Settings& rs) {
    bool set = false;
    if (auto u = own_gauge(rs, slot.fighter_id, &set)) slot.j["unique"] = *u;
    else if (set) slot.j.erase("unique");
}

// The settings as fields of the slot (a trial like the others: the mode applies them). `rec_counter`: config.json's
// "rec_counter" ("counter" / "punish" / empty), the default when the menu set none.
inline void slot_fields(Trial& slot, const Settings& rs, const std::string& rec_counter) {
    json& j = slot.j;
    j["gauges"] = { { "sa_stock", rs.sa == 0 ? json("max") : json(rs.sa - 1) }, { "dg_stock", rs.dg == 0 ? json("max") : json(rs.dg - 1) },
                    { "sa_heal", rs.sh1 == 1 ? json("auto") : json(rs.sh1) }, { "dg_heal", rs.dh1 == 1 ? json("auto") : json(rs.dh1) } };
    j["dummy_counter"] = rs.counter ? *rs.counter : rec_counter == "punish" ? 2 : rec_counter == "counter" ? 1 : 0;
    static const int kGuard[] = { 0, 2, 3, 4 };      // TM_GS_GUARD_TYPE: NO_GUARD, NEXT_GUARD (the trials' own), ALL_GUARD, RANDOM_GUARD
    j["dummy"] = { { "state", rs.state }, { "guard", rs.guard_raw ? *rs.guard_raw : (rs.guard >= 0 && rs.guard < 4 ? kGuard[rs.guard] : 2) } };
    j["recipe"] = { { "teams", settings_teams(rs) } };
    static const int kPos[4][2] = { { 0, 0 }, { -75, 75 }, { 615, 765 }, { -765, -615 } };   // as recorded, midscreen, corner, back to the corner
    if (rs.pos > 0) j["start"] = { { "p1", kPos[rs.pos][0] }, { "p2", kPos[rs.pos][1] } };
    else j.erase("start");                         // "As Recorded": the last take's start (the recorder knows it)
    slot_own_gauge(slot, rs);
}

// The values a closing training menu is compared on: a change resets the battle (the start, life and a started timer
// only show at the start of a battle)
inline std::string settings_key(const Settings& s, int fighter) {
    auto it = s.uq.find(fighter);
    const int v[] = { s.pos, s.state, s.guard_raw.value_or(-1), s.counter.value_or(-1), s.sa, s.dg, s.sa2, s.dg2,
                      it != s.uq.end() ? it->second.s : -1, it != s.uq.end() ? it->second.t : -1, s.vp1, s.vp2, s.vh1, s.vh2,
                      s.sh1, s.sh2, s.dh1, s.dh2 };
    std::string k;
    for (int x : v) k += std::to_string(x) + ",";
    return k;
}

// A Reset Battle left pending by the training menu (a setting changed): asked as the player LEAVES the pause menu, not
// as the training menu closes (the pause menu is still up then). Read frame by frame in game:
//   the frame of the cancel   agent Show, flow Main, its element's _Next = UIFlowCommon.HideObject, Param.isCancel true
//   one frame later           agent FadeOut, no delegates, no flow
// isCancel alone is stale (true from the last cancel until a decision clears it); a decided row closes with the same
// _Next and isCancel false; a sub-window (Control Settings) keeps the agent in Show with no delegates. The decision
// asked on the cancel frame resets the battle, and so does one asked the frame after with the flow of the frame before
// (the net for a way out not seen). It ends at the next StageSetup whoever caused it, when our rows are no longer the
// menu's, or when the scene has had no pause menu for 120 frames. Fed once a frame while no training menu is up.
struct PauseView {
    bool agent = false;          // the pause menu's agent is in the scene
    bool shown = false;          // its state is Show
    bool flow = false;           // it has delegates, a flow and the flow's Param
    bool hide_next = false;      // the flow's element goes to UIFlowCommon.HideObject next
    bool cancel = false;         // Param.isCancel
};
struct PendingReset {
    enum Act { None, Now, Prev };    // Now: with this frame's flow; Prev: with the flow kept from the frame before
    bool pending = false, decided = false, have_prev = false;
    uint64_t at = 0;                 // StageSetup calls counted when it was armed
    int dec_n = 0, gone = 0;
    void arm(uint64_t setup_calls) { *this = PendingReset{}; pending = true; at = setup_calls; }
    void row_decided() { if (pending) { decided = true; dec_n = 0; } }   // a row of the pause menu was decided
    void menu_up() { have_prev = false; }                                // the training menu is up again
    Act tick(uint64_t setup_calls, bool ours_on, const PauseView& v) {
        if (!pending) return None;
        if (setup_calls != at || !ours_on) { pending = false; have_prev = false; return None; }
        if (!v.agent) {                  // no pause menu in this scene any more: the trial was left
            have_prev = false;
            if (++gone > 120) pending = false;
            return None;
        }
        gone = 0;
        if (decided && ++dec_n > 30) { decided = false; dec_n = 0; }   // a decided row: what follows is the game's own
        Act go = None;
        if (v.shown) { if (v.flow && v.hide_next && v.cancel) go = Now; }
        else if (have_prev) go = Prev;
        have_prev = v.shown && go == None && v.flow;
        if (go == None || decided) return None;
        pending = false;
        return go;
    }
};

// One per tab (the first row of every tab): a take goes to the tab of the slot it was played in. The three share the
// slot's settings and the fighter's last take (preview, Watch Example).
inline TrialP make_slot(int chara, const Settings& rs, const std::string& rec_counter, int level = 0) {
    auto t = std::make_shared<Trial>();
    t->rec = true;
    t->title = kSlotTitle;
    t->fighter_id = chara;
    t->level = level >= 0 && level <= 2 ? level : 0;
    t->order = -1e15;
    for (int i = 1; i <= kRows; ++i) {
        Step st;
        st.blank = true;
        st.file_ids = { kPlaceholder + i };
        st.action_id = kPlaceholder + i;
        st.skill_id = kBlankSkill;
        resolve_step(st, json::object());
        t->steps.push_back(std::move(st));
    }
    slot_fields(*t, rs, rec_counter);
    return t;
}

// The recorder of part 2. Part 1 runs without one: the slot is an empty recipe with its settings.
struct Recorder {
    virtual ~Recorder() = default;
    virtual void frame(int flow) { (void)flow; }
    virtual void tick(void* mission, Trial& slot, int chara) { (void)mission; (void)slot; (void)chara; }
    virtual void leave() {}
    virtual void on_attack(void* mission, void* attacker) { (void)mission; (void)attacker; }
    virtual bool training_menu_open() { return false; }
    virtual bool has_take(int chara) { (void)chara; return false; }
    // what the list previews for the slot: the last take of the fighter; before any, one line (a hint replaces its text)
    virtual std::vector<Step> preview_steps(const Trial& slot) {
        (void)slot;
        Step st;
        st.action_id = kPlaceholder;
        st.ids = { kPlaceholder };
        st.skill_id = kBlankSkill;
        return { st };
    }
    virtual bool fixed_start() { return false; }
    virtual bool replay_on() { return false; }
    virtual void replay_tick(bool in_replay) { (void)in_replay; }
};

}  // namespace cc::rec
