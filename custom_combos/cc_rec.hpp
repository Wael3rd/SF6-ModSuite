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

// What the slot starts a take with (_rec_settings.json): indexes into the pause-menu rows' lists
struct Settings {
    int pos = 0, state = 0, guard = 1, sa = 0, dg = 0, ug = 0, sa2 = 0, dg2 = 6;
    std::optional<int> counter, guard_raw;
};

inline Settings load_settings(const fs::path& data_dir) {
    Settings s;
    const json j = read_json(data_dir / "_rec_settings.json");
    if (!j.is_object()) return s;
    auto rd = [&](const char* k, int mx, int& dst) {
        if (auto v = num(at(j, k))) dst = std::max(0, std::min(mx, static_cast<int>(std::floor(*v))));
    };
    rd("pos", 3, s.pos); rd("state", 2, s.state); rd("guard", 3, s.guard); rd("sa", 3, s.sa); rd("dg", 6, s.dg);
    rd("ug", 6, s.ug); rd("sa2", 3, s.sa2); rd("dg2", 6, s.dg2);
    if (auto v = num(at(j, "counter"))) s.counter = std::max(0, std::min(2, static_cast<int>(std::floor(*v))));
    if (auto v = num(at(j, "guard_raw"))) s.guard_raw = std::max(0, std::min(5, static_cast<int>(std::floor(*v))));
    return s;
}

// Fighters whose own gauge is a stock the game's trials preset (cPlayer.mStyleNo is the level)
inline bool stock_fighter(int id) {
    switch (id) { case 3: case 5: case 12: case 15: case 16: case 20: case 21: case 28: case 32: case 33: return true; default: return false; }
}

// The settings as fields of the slot (a trial like the others: the mode applies them). `rec_counter`: config.json's
// "rec_counter" ("counter" / "punish" / empty), the default when the pause menu set none.
inline void slot_fields(Trial& slot, const Settings& rs, const std::string& rec_counter) {
    json& j = slot.j;
    j["gauges"] = { { "sa_stock", rs.sa == 0 ? json("max") : json(rs.sa - 1) },
                    { "dg_stock", rs.dg == 0 ? json("max") : json(rs.dg - 1) }, { "sa_heal", "auto" }, { "dg_heal", "auto" } };
    j["dummy_counter"] = rs.counter ? *rs.counter : rec_counter == "punish" ? 2 : rec_counter == "counter" ? 1 : 0;
    static const int kGuard[] = { 0, 2, 3, 4 };      // TM_GS_GUARD_TYPE: NO_GUARD, NEXT_GUARD (the trials' own), ALL_GUARD, RANDOM_GUARD
    j["dummy"] = { { "state", rs.state }, { "guard", rs.guard_raw ? *rs.guard_raw : (rs.guard >= 0 && rs.guard < 4 ? kGuard[rs.guard] : 2) } };
    j["recipe"] = { { "teams", json::array({ json::object(), { { "SGaugeStock", rs.sa2 }, { "DGaugeStock", rs.dg2 } } }) } };
    static const int kPos[4][2] = { { 0, 0 }, { -75, 75 }, { 615, 765 }, { -765, -615 } };   // as recorded, midscreen, corner, back to the corner
    if (rs.pos > 0) j["start"] = { { "p1", kPos[rs.pos][0] }, { "p2", kPos[rs.pos][1] } };
    else j.erase("start");                         // "As Recorded": the last take's start (part 2 knows it)
    if (stock_fighter(slot.fighter_id) && rs.ug > 0) j["unique"] = { { "type", 1 }, { "stock", rs.ug } };
}

inline TrialP make_slot(int chara, const Settings& rs, const std::string& rec_counter) {
    auto t = std::make_shared<Trial>();
    t->rec = true;
    t->title = kSlotTitle;
    t->fighter_id = chara;
    t->level = 0;
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
