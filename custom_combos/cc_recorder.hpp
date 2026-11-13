// cc_recorder.hpp - Custom Combos, part 2: the recording slot's recorder, the slot's rows in the trial pause menu, the
// training menu inside the slot, combos recorded from a replay. Included by custom_combos.cpp inside its anonymous
// namespace, after the mode (it uses the mode's helpers and state; the mode reaches it through rec::Recorder only).
//
// What it reads, every frame of the slot's fight (game thread): P1's action id, combo counter, held inputs (pl_sw_new,
// mirrored when facing right), positions, own gauge and the combo's damage; the dummy's counter flags; the fight's clock
// (gBattle.Game.stage_timer, rule 9: one demo entry per frame of the fight). What hit comes from the judge's
// TrialOnAttack (the mode owns that hook and hands it over). The offline part is cc_take.hpp.
//
// Hooks of its own (no other mod of the suite hooks these, checked), each pass-through outside the slot / a recording:
//   UIFlowESportsPauseMenu.Param.SetupList_ComboTrial (pre) and .SetupList (pre): the slot's array of rows, given before
//       the game builds its row objects from it (rule 2); any other trial gets the game's array back;
//   static UIFlowESportsPauseMenu.DecidePauseMenuItem (pre): our Training Settings row (item type 68, unknown to the
//       game: its original skipped), "Delete Last Recording" noted (a clone of Reset Battle: the game's own decide runs);
//   UIReplayPauseMenu.Param.InitMenuList (pre): the replay menu's rows with ours (FuncType 40 / 41 / 42);
//   ReplayUICtrl.OnDecideItem (pre): our three rows (their original skipped), every other row passed through.
//
// The training menu inside the slot (rules 6, 7, 8) opens the game's Training pause menu, the very menu the core's
// pause-menu service and the Recording Manager hook. Their hooks run there: both are gated on training (flow 3 / 9 with
// _tData) or on the Recording / Reversal tabs, which our tab list leaves out (FuncType 2, 3 and 6 only), so in our menu
// they pass through. What this module writes into shared data, and how it goes back:
//   * TrainingPauseMenuUserData._MenuData (the tab list, shared with Training mode): ours only while our menu is open,
//     the game's array put back on closing, on giving up, on leaving the slot or the flow, and whenever ours is found
//     in with no menu of ours up;
//   * TrainingManager._GameMode: 1 only for the calls that build the menu, put back in the same frame;
//   * InitUIMenu: once a process; the menu objects it made are kept (rooted) and given back on later loads;
//   * PauseManager: pause 8 asked while our menu is up, given back once the trial pause menu is hidden (rule 7);
//   * the training manager's half speed (OtherSetting.Is_Speed_Setting) switched off around our menu;
//   * TrainingManager._tData: every value of its setting groups is kept before our menu first opens in a slot session
//     and put back when the slot is left (out of a held pause, rule 4), but for the six values the mode pushes and puts
//     back itself (stocks, counter, dummy stance and guard). Only when _tData is still the object kept.
#pragma once

#include "cc_take.hpp"

namespace recorder {

using namespace cc::take;

constexpr int64_t kResetJump = 60LL * 65536;   // a fighter moved this far in one frame with no combo under way: Reset Battle
constexpr int kPmTraining = 68;                // our Training Settings row (an item type the game does not know)
const char* const kWidgetCountDown = "app.training.UIWidget_TMRecordCountDown";

// ---------------------------------------------------------------- small game helpers
Obj* player(int i) {
    Obj* sp = static_obj("gBattle", "Player");
    Obj* pl = sp ? get_obj(sp, "mcPlayer") : nullptr;
    return pl && i < arr_len(pl) ? arr_get(pl, i) : nullptr;
}
std::optional<double> pos_x(Obj* p) { return num(get(get(ref(p), "pos"), "x"), "v"); }
double pos_y(Obj* p) { return num(get(get(ref(p), "pos"), "y"), "v").value_or(0); }
std::optional<double> battle_clock() { Obj* g = static_obj("gBattle", "Game"); return g ? num(g, "stage_timer") : std::nullopt; }
double combo_damage(Obj* a, Obj* b) {
    double best = 0;
    for (Obj* p : { a, b }) if (p) best = std::max(best, num(get(ref(p), "mpTeam"), "mComboDamage").value_or(0));
    return best;
}
Frame read_frame(Obj* pa, Obj* pd, bool modern) {
    Frame in;
    in.act = act_of(pa);
    in.cc = static_cast<int>(inum(pa, "combo_cnt", 0));
    in.sw = static_cast<uint32_t>(inum(pa, "pl_sw_new", 0));
    in.facing_right = flag(pa, "rl_dir");
    in.x1 = pos_x(pa);
    in.x2 = pos_x(pd);
    in.y1 = pos_y(pa);
    in.style = static_cast<int>(inum(pa, "mStyleNo", 0));
    in.dmg = combo_damage(pa, pd);
    in.counter = flag(pd, "counter_fw_flag") ? 2 : flag(pd, "counter_dm_flag") ? 1 : 0;
    in.modern = modern;
    return in;
}
Obj* tmgr() { return singleton("app.training.TrainingManager"); }
Obj* pause_mgr() { return singleton("app.PauseManager"); }
int64_t pause_bits() { Obj* pm = pause_mgr(); return pm ? inum(pm, "_CurrentPauseTypeBit", 0) : 0; }
void request_pause(bool on, int type) {
    Obj* pm = pause_mgr();
    Method* m = pm ? method_of(td_of(pm), "requestPause(System.Boolean, app.PauseManager.PauseType)") : nullptr;
    if (m) invoke(m, pm, { arg_bool(on), arg_i32(type) });
}
Obj* managed_string(const std::string& s) {
    auto fn = API::get()->sdk()->functions->create_managed_string_normal;
    return fn ? reinterpret_cast<Obj*>(fn(s.c_str())) : nullptr;
}
// sdk.create_userdata(type, path) (a game file, the same cached object the game uses), rooted
Obj* create_userdata(const char* type_name, const char* path) {
    const REFrameworkSDKData* sdk = API::get()->sdk();
    if (!sdk->resource_manager || !sdk->resource_manager->create_userdata || !sdk->functions->get_resource_manager) return nullptr;
    REFrameworkResourceManagerHandle rm = sdk->functions->get_resource_manager();
    Obj* ud = rm ? reinterpret_cast<Obj*>(sdk->resource_manager->create_userdata(rm, type_name, path)) : nullptr;
    if (ud && live(ud)) ud->add_ref();
    return ud && live(ud) ? ud : nullptr;
}
bool agent_hidden(Obj* a) {
    Obj* cur = get_obj(get_obj(a, "_StateManager"), "_Curt");
    return !cur || type_name(cur).find("Hide") != std::string::npos;
}
Obj* pause_agent() { auto a = agents("ESportsPauseMenu"); return a.empty() ? nullptr : a[0]; }

// A path from UTF-8 text (the trials' paths are kept as UTF-8)
fs::path upath(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return fs::path(w);
}

// A JSON file written whole (a temporary file renamed over it)
bool write_json(const fs::path& p, const json& j) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::path(p.wstring() + L".tmp");
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    const std::string s = j.dump(4);
    const bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) { fs::remove(tmp, ec); return false; }
    return true;
}

// ---------------------------------------------------------------- state
rec::Settings rs;                              // the slot's settings (_rec_settings.json)
struct LastStart { int chara; int64_t p1, p2; };
std::optional<LastStart> last_start;           // "As Recorded": where the fighter's last take started
SlotCore core_;
std::map<int, RecLabels> rec_labels_of;
std::optional<double> slot_clock;
uint64_t ex_poll = 0;
std::atomic<bool> want_discard{false};   // asked from the pause menu's hook, done by the slot's tick

enum class Result { None, Saved, Short, Same, Deleted };
Result result = Result::None;
int result_steps = 0;
int64_t result_damage = 0;
std::vector<json> last_steps;                  // the last take's lines (shown in the rows, previewed in the list)
int last_chara = -1;
struct LastFile { std::string path; int chara; std::optional<json> prev; };
std::optional<LastFile> last_file;             // what "Delete Last Recording" undoes

struct Shown {
    std::string key; bool has_key = false;
    int64_t check_at = 0, burst_until = 0, rebuild_at = 0;
    bool burst = false, cleared = false, example = false;
    std::optional<int> ex_done;
    std::optional<double> x1, x2;
    int rebuilds = 0;
} shown;

const RecLabels& labels_for(int chara) {
    auto it = rec_labels_of.find(chara);
    if (it != rec_labels_of.end()) return it->second;
    return rec_labels_of[chara] = rec_labels(labels_of(chara));
}

// ---------------------------------------------------------------- the slot's settings
json rs_gauges() {
    return { { "sa_stock", rs.sa == 0 ? json("max") : json(rs.sa - 1) }, { "dg_stock", rs.dg == 0 ? json("max") : json(rs.dg - 1) },
             { "sa_heal", "auto" }, { "dg_heal", "auto" } };
}
int rs_guard() { static const int kGuard[] = { 0, 2, 3, 4 }; return rs.guard_raw ? *rs.guard_raw : (rs.guard >= 0 && rs.guard < 4 ? kGuard[rs.guard] : 2); }
void rs_save() {
    if (!write_json(kData / "_rec_settings.json", rec::settings_json(rs))) logf("recorder: _rec_settings.json not written");
}
// the settings as the slot's fields (a trial like the others: the mode applies them); "As Recorded": the last take's start
void slot_fields(Trial& slot) {
    rec::slot_fields(slot, rs, cfg.rec_counter);
    if (rs.pos == 0 && last_start && last_start->chara == slot.fighter_id) slot.j["start"] = { { "p1", last_start->p1 }, { "p2", last_start->p2 } };
}
// the fighter's recording slots, one per tab
std::vector<TrialP> slots_of(int chara) {
    std::vector<TrialP> out;
    if (!sets_loaded) return out;
    auto it = all_sets.find(chara);
    if (it != all_sets.end()) for (const TrialP& t : it->second) if (t && t->rec) out.push_back(t);
    return out;
}
// the settings changed: every slot of every fighter gets them (they share the slot's settings)
void refresh_slots() {
    if (!sets_loaded) return;
    for (auto& [id, set] : all_sets) for (const TrialP& t : set) if (t && t->rec) slot_fields(*t);
}

// ---------------------------------------------------------------- saving a take
struct Found { std::string path; double damage; std::string title; int level; double order; };
std::optional<Found> find_recorded(int chara, const std::string& sig) {
    for (const TrialP& t : trial_set(chara)) {
        const json* r = obj(&t->j, "recorded");
        if (t->path.empty() || !r || !is_str(at(r, "signature"), sig.c_str())) continue;
        return Found{ t->path, num(at(r, "damage")).value_or(0), t->title, t->level, t->order };
    }
    return std::nullopt;
}

// A saved take joins its fighter's set; the slot keeps its example and start (Watch Example replays it, Reset Battle
// goes back to where it began). The lists are rebuilt by the mode on its next look.
void on_saved(int chara, const json& trial, const std::string& path) {
    bool del = false;
    if (TrialP t = trial_from(trial, upath(path), chara, labels_of(chara), &del)) mode::on_saved(chara, t);
    for (const TrialP& slot : slots_of(chara)) {          // the three slots share the fighter's last take
        if (const json* d = at(trial, "demo")) slot->j["demo"] = *d;
        if (const json* u = at(trial, "unique")) slot->j["unique"] = *u; else slot->j.erase("unique");
        rec::slot_own_gauge(*slot, rs);          // (what the training menu set for this fighter stays)
        if (!rs.pos) {
            if (const json* s = at(trial, "start")) slot->j["start"] = *s; else slot->j.erase("start");
        }
        Obj* si = get_obj(get_obj(mission(), "mpStageAsset"), "Root");
        if (si && asset_ready(si) && slot_trial == slot) try { apply_position(si, *slot); } catch (...) {}
    }
    logf("recorded: %s (%zu steps) -> %s", str_of(at(trial, "title")).c_str(), arr(&trial, "steps") ? arr(&trial, "steps")->size() : 0, path.c_str());
}

std::string new_take_path(const std::string& dir, const std::string& stamp, const std::set<std::string>& also_taken = {}) {
    const fs::path folder = kData / upath(dir);
    const std::string name = take_file_name(stamp, [&](const std::string& n) {
        std::error_code ec;
        const std::string p = u8(folder / upath(n));
        return fs::exists(folder / upath(n), ec) || also_taken.count(p) > 0;
    });
    return u8(folder / upath(name));
}

void finish_slot_take(const Take& t, int chara) {
    shown.cleared = false;
    if (t.hits == 0) return;
    last_file.reset();                           // "Delete Last Recording" is about this take
    const RecLabels& L = labels_for(chara);
    const char* dir = fighter_dir(chara);
    TrialOpts o;
    o.min_steps = cfg.rec_min_steps;
    o.gauges = rs_gauges();
    o.dummy_state = rs.state; o.dummy_guard = rs_guard();
    o.teams = rec::settings_teams(rs, &o.teams_any);
    o.own_gauge = rec::own_gauge(rs, chara);
    o.level = slot_trial && slot_trial->rec ? slot_trial->level : 0;   // the tab of the slot the take was played in
    o.dir = dir ? dir : "Unknown";
    o.when = stamp_of(std::time(nullptr));
    Closed c = close_take(t, L, chara, o);
    if (c.kind == Closed::None) return;
    if (c.kind == Closed::Short) { result = Result::Short; logf("recorder: not kept (%zu step(s), %d needed)", c.steps.size(), o.min_steps); return; }
    // the same combo recorded again replaces its file (the better take stays)
    const auto replaced = find_recorded(chara, c.sig);
    if (replaced && replaced->damage > t.damage) {
        result = Result::Same;
        last_steps.clear();
        for (const OutStep& s : c.steps) last_steps.push_back(s.j);
        last_chara = chara;
        logf("recorder: same combo already saved with more damage (%.0f)", replaced->damage);
        return;
    }
    std::string path = replaced ? replaced->path : new_take_path(o.dir, o.when.file);
    if (replaced) {
        c.trial["title"] = replaced->title;      // (its level does not follow: the slot's tab wins)
        c.trial["order"] = replaced->order;
    }
    LastFile lf{ path, chara, std::nullopt };
    if (replaced) { json prev = read_json(upath(path)); if (prev.is_object()) lf.prev = std::move(prev); }
    if (!write_json(upath(path), c.trial)) { logf("recorder: the take could not be written (%s)", path.c_str()); return; }
    last_file = std::move(lf);
    if (const json* s = obj(&c.trial, "start")) last_start = LastStart{ chara, inum(at(s, "p1"), 0), inum(at(s, "p2"), 0) };
    last_steps.clear();
    for (const OutStep& s : c.steps) last_steps.push_back(s.j);
    last_chara = chara;
    result = Result::Saved;
    result_steps = static_cast<int>(c.steps.size());
    result_damage = static_cast<int64_t>(std::floor(t.damage));
    on_saved(chara, c.trial, path);
}

// "Delete Last Recording": that take's file goes (only it: a rec_*.json in a fighter's folder, written this session),
// or the file it replaced comes back
void discard_last() {
    std::optional<LastFile> lf = std::move(last_file);
    last_file.reset();
    if (!lf) return;
    if (lf->prev) {
        if (write_json(upath(lf->path), *lf->prev)) {
            bool del = false;
            if (TrialP t = trial_from(*lf->prev, upath(lf->path), lf->chara, labels_of(lf->chara), &del)) mode::on_saved(lf->chara, t);
        }
    } else if (is_take_file(upath(lf->path), kData)) {
        std::error_code ec;
        fs::remove(upath(lf->path), ec);
        mode::on_discard(lf->chara, lf->path);
    }
    for (const TrialP& slot : slots_of(lf->chara)) slot->j.erase("demo");
    last_steps.clear();
    result = Result::Deleted;
    shown.cleared = false;
    shown.has_key = false;
    logf("recorder: last recording deleted (%s)", lf->path.c_str());
}

// ---------------------------------------------------------------- the live recipe
// The slot's recipe has 12 lines whose judge never clears. Their texts are ours: the moves of the take as they come
// (the window sliding once the list is full), the cursor (InfoPtr) on the move under way; the header says the state.
std::vector<std::string> live_texts(const RecLabels& L) {
    std::vector<std::string> out;
    if (core_.live()) {
        for (const Move& mv : core_.take->moves) out.push_back(move_text(L, mv, core_.take->modern));
    } else if (!last_steps.empty() && !shown.cleared) {
        for (const json& st : last_steps) {
            const json* t = at(st, "text");
            out.push_back(t && t->is_string() ? t->get<std::string>()
                                              : (L.L ? step_text(*L.L, str_of(at(st, "notation")), inum(at(st, "skill_id"), 0), static_cast<int>(inum(at(st, "strength_type"), 0)), false) : std::string{}));
        }
    }
    return out;
}

std::string title_text() {
    char b[160];
    if (core_.live()) { std::snprintf(b, sizeof(b), "REC  -  Recording: %zu move(s), %d hit(s)", core_.take->moves.size(), core_.take->hits); return b; }
    const Result k = shown.cleared ? Result::None : result;
    switch (k) {
    case Result::Saved: std::snprintf(b, sizeof(b), "REC  -  Saved: %d step(s), %lld damage", result_steps, static_cast<long long>(result_damage)); return b;
    case Result::Short: std::snprintf(b, sizeof(b), "REC  -  Not kept (%d step(s) needed)", cfg.rec_min_steps); return b;
    case Result::Same: return "REC  -  Already saved";
    case Result::Deleted: return "REC  -  Last recording deleted";
    default: return "REC  -  Do a combo: it becomes a trial";
    }
}

// ControlMain / p_ScrollList_0_v / c_item | itemN / c_inner / e_txt_item, and ControlMain / c_head / e_txt_title: looked
// up again on every use, never kept (the game builds a new recipe on a restart)
bool view(std::map<int, Obj*>& rows, Obj*& title) {
    Obj* ctrl = recipe_ctrl();
    if (!ctrl) return false;
    title = nullptr;
    for (Obj* c = child(ctrl); c; c = next(c)) {
        const std::string nm = name_of(c);
        if (nm == "c_head") title = find_child(c, "e_txt_title");
        else if (nm.rfind("p_ScrollList", 0) == 0)
            for (Obj* it = child(c); it; it = next(it)) {
                const std::string inm = name_of(it);
                const int r = inm == "c_item" ? 0 : is_item_name(inm) ? std::stoi(inm.substr(4)) : -1;
                if (r < 0) continue;
                Obj* inner = find_child(it, "c_inner");
                if (Obj* txt = inner ? find_child(inner, "e_txt_item") : nullptr) rows[r] = txt;
            }
    }
    return true;
}

std::pair<int, std::string> live_key() {
    if (core_.live()) {
        const int n = static_cast<int>(core_.take->moves.size());
        return { n, std::to_string(n * 1000 + core_.take->hits) };
    }
    if (shown.cleared) return { 0, "cleared" };
    const int n = static_cast<int>(last_steps.size());
    std::string k = std::to_string(static_cast<int>(result)) + ":" + std::to_string(n);
    if (shown.ex_done) k += "@" + std::to_string(*shown.ex_done);   // an example under way: how far it is
    return { n, k };
}

// Per frame this only compares a key. The controls are walked when something changed, and otherwise once a second
// while nothing is recorded (the game may have rebuilt the list); never during a take, whose rows the game leaves alone.
void show_rows(Obj* m, const RecLabels& L, Trial* T) {
    const auto [n, key] = live_key();
    const int64_t f = core_.frame;
    const bool changed = !shown.has_key || key != shown.key;
    if (!changed && f < shown.check_at) return;
    shown.check_at = f + (core_.live() ? 600 : (f < shown.burst_until ? 4 : 60));
    std::map<int, Obj*> rows;
    Obj* title = nullptr;
    if (!view(rows, title)) return;
    if (title) set_message(title, title_text());
    // a slot opened before its lines were ours (or whose lines gave no row) is rebuilt
    int nrows = 0;
    for (int i = 0; i < rec::kRows; ++i) if (rows.count(i)) ++nrows;
    if (nrows >= rec::kRows) shown.rebuilds = 0;
    if (nrows < rec::kRows && T && f >= shown.rebuild_at && shown.rebuilds < 4) {
        ++shown.rebuilds;
        shown.rebuild_at = f + 90;
        mode::rebuild_recipe(m, *T);
        return;
    }
    const std::vector<std::string> texts = live_texts(L);
    int off = std::max(0, n - rec::kRows);
    const std::optional<int> exd = !core_.live() ? shown.ex_done : std::nullopt;
    if (exd) off = std::max(0, std::min(n - rec::kRows, *exd - (rec::kRows - 4)));   // a long take: the window follows
    for (int i = 0; i < rec::kRows; ++i) {
        auto it = rows.find(i);
        const size_t k = static_cast<size_t>(off + i);
        if (it != rows.end()) set_message(it->second, k < texts.size() ? texts[k] : std::string{});
    }
    shown.key = key;
    shown.has_key = true;
    // the cursor: on the move under way while recording; after a save the take's lines stay, all cleared
    int ptr = 0;
    if (n > 0) {
        int done = n;
        if (core_.live() && !core_.take->moves.back().hit) done = n - 1;
        if (exd) done = std::min(n, *exd);
        ptr = std::max(0, std::min(done - off, rec::kRows - 1));
    }
    set(m, "InfoPtr", ptr);
}

// ---------------------------------------------------------------- the trial's training data
// _tData is per mode (measured: Training's own settings are another object), and every trial start copies its asset
// into it (StageSetup, rule 13), so nothing of it is kept and put back but the one value our menu switches off and
// no start rewrites: the training manager's half speed (OtherSetting.Is_Speed_Setting), put back as a field when the
// slot is left (no apply: it only acts once a menu applies it). Only into the object it was read from.
struct SnapVal { std::vector<std::string> path; double v; };
struct TSnap { Obj* td = nullptr; std::vector<SnapVal> vals; bool pending = false; } tsnap;

void tdata_keep(Obj* tm) {
    if (tsnap.td) return;
    Obj* td = get_obj(tm, "_tData");
    if (!td) return;
    tsnap.vals.clear();
    if (auto v = num(get(ref(td), "OtherSetting"), "Is_Speed_Setting")) tsnap.vals.push_back({ { "OtherSetting", "Is_Speed_Setting" }, *v });
    td->add_ref();                               // the object compared on restore (its address never reused meanwhile)
    tsnap.td = td;
}

void tdata_restore(bool live_fight) {
    (void)live_fight;
    tsnap.pending = false;
    Obj* td0 = tsnap.td;
    if (!td0) return;
    tsnap.td = nullptr;
    Obj* tm = tmgr();
    Obj* td = tm ? get_obj(tm, "_tData") : nullptr;
    if (td == td0)
        for (const SnapVal& s : tsnap.vals) {
            Ref r = ref(td);
            for (size_t i = 0; i + 1 < s.path.size() && r; ++i) r = get(r, s.path[i].c_str());
            if (r) set(r, s.path.back().c_str(), s.v);
        }
    td0->release();
    tsnap.vals.clear();
}
void tdata_drop() {
    tsnap.pending = false;
    if (tsnap.td) { tsnap.td->release(); tsnap.td = nullptr; }
    tsnap.vals.clear();
}

// A fighter's own gauge in the trial's training data (_tData.ParameterSetting.UniqueData.stock_0_NNN / timer_0_NNN,
// NNN the fighter's id). The game copies the asset's values there at every stage setup for every fighter but C.
// Viper's timer (measured): written here from the StageSetup hook for the fighter of the trial (where the game copies,
// it writes the same numbers over ours), and back to zero for the others (the fields apply to whichever side plays
// that fighter). Never into Training's data (_GameMode 1: there they are the player's own settings).
std::set<int> setup_uq;                        // the fighters whose fields we wrote
bool unique_tdata(int id, const json* u) {
    Obj* tm = tmgr();
    if (!tm || id <= 0 || inum(tm, "_GameMode", -1) == 1) return false;
    Ref ud = get(get(get(ref(tm), "_tData"), "ParameterSetting"), "UniqueData");
    if (!ud) return false;
    char sn[24], tn[24];
    std::snprintf(sn, sizeof(sn), "stock_0_%03d", id);
    std::snprintf(tn, sizeof(tn), "timer_0_%03d", id);
    const double s = u ? std::floor(num(at(u, "stock")).value_or(0)) : 0, t = u ? std::floor(num(at(u, "timer")).value_or(0)) : 0;
    if (field_of(ud.td, sn)) set(ud, sn, s);
    if (field_of(ud.td, tn)) set(ud, tn, t);
    return true;
}
// the StageSetup hook, a trial of ours starting: its fighter's values in, every other fighter's we wrote back to zero
void stage_own_gauge(int fighter, const json* unique) {
    if (fighter <= 0) return;
    for (auto it = setup_uq.begin(); it != setup_uq.end();) {
        if (*it != fighter) { unique_tdata(*it, nullptr); it = setup_uq.erase(it); } else ++it;
    }
    setup_uq.insert(fighter);
    unique_tdata(fighter, unique);
}
// the StageSetup hook, an official trial starting: what we wrote back to zero before the game writes its own
void own_gauge_clear_all() {
    for (int id : setup_uq) unique_tdata(id, nullptr);
    setup_uq.clear();
}

// ---------------------------------------------------------------- the training menu inside the slot
struct TMState {
    bool want = false, open = false, seen = false, prepare = false, guard = false;
    int want_n = 0, closed_for = 0, unblock_in = -1, asked_in = 0, guard_n = 0, hidden_for = 0;
    enum class Prep { None, Destroy, Settle } prep = Prep::None;
    int prep_n = 0;
    std::vector<std::pair<std::string, Obj*>> objs; bool objs_made = false;   // InitUIMenu's objects, once a process
    Obj* ud = nullptr; Obj* sc = nullptr;                                     // the menu's user data (rooted)
    Obj* tabs_ui = nullptr; Obj* game_tabs = nullptr; Obj* our_tabs = nullptr;
    bool swapped = false;                        // our tab list put in, the game's not back yet
    std::string snap;                            // the slot's settings when the menu opened (rec::settings_key)

    std::optional<int64_t> start_loc;
    bool blocked = false;
} TM;
const char* const kTmObjs[] = { "_UITrainingMenu", "_UITrainingMenu_TextList", "_UITrainingMenu_SpinList", "_UITrainingMenu_All",
                                "_UITrainingMenu_Reversal", "_UITrainingDialog", "_UITrainingImageDialog" };
bool tm_tab_kept(int64_t ft) { return ft == 2 || ft == 3 || ft == 6; }          // Environment, Dummy, Parameter
bool tm_env_row_kept(int64_t ft) { return ft == 33 || ft == 189; }              // Start Position, Character-Specific Settings
int tm_start_to_pos(int64_t loc) { switch (loc) { case 0: return 1; case 1: return 2; case 2: return 3; case 3: return 0; default: return -1; } }
int64_t tm_pos_to_start(int pos) { switch (pos) { case 2: return 1; case 3: return 2; default: return 0; } }   // "As Recorded" shows Center

void tm_block(bool on) {
    Obj* a = pause_agent();
    if (a && core->ui_set_block_input) core->ui_set_block_input(client, a, on ? 1 : 0);
    TM.blocked = on;
}
void tm_hold_pause() {
    if (!pause_mgr()) return;
    --TM.asked_in;
    if (!(pause_bits() & 256) && TM.asked_in <= 0) {   // BATTLE_MENU_PAUSE, the pause menu's own
        request_pause(true, 8);
        TM.asked_in = 4;                             // (the bit follows a frame later)
        TM.guard = true;
    }
}
// A pause we asked for is ours to give back once the pause menu is gone
void tm_guard() {
    if (++TM.guard_n % 4 != 0) return;
    Obj* a = pause_agent();
    if (a && !agent_hidden(a)) { TM.hidden_for = 0; return; }
    if (++TM.hidden_for < 2) return;
    if ((pause_bits() & 256) && TM.hidden_for < 30) request_pause(false, 8);
    else { TM.guard = false; TM.hidden_for = 0; }
}
// the training manager's own half speed off (it pauses every other frame with type 11 and takes pause 8 away)
void tm_speed_off(Obj* tm) {
    Ref os = get(get(ref(tm), "_tData"), "OtherSetting");
    if (os && inum(os, "Is_Speed_Setting", 0) != 0) {
        set(os, "Is_Speed_Setting", 0);
        call_ok(tm, "ForceGameSpeedSettingApply()");
    }
    if (pause_bits() & 2048) request_pause(false, 11);
}

// The menu objects and the user data, given with _GameMode 1 for the calls (the manager of a combo trial has none)
void tm_give(Obj* tm) {
    const int64_t mode = inum(tm, "_GameMode", 5);
    set(tm, "_GameMode", 1);
    try {
        if (!get_obj(tm, "_UITrainingMenu")) {
            if (TM.objs_made) {
                for (const auto& [name, o] : TM.objs) set_ref(tm, name.c_str(), o);
            } else {
                call_ok(tm, "InitUIMenu()");
                TM.objs_made = true;
                for (const char* name : kTmObjs)
                    if (Obj* o = get_obj(tm, name)) { o->add_ref(); TM.objs.push_back({ name, o }); }
                logf("training menu: InitUIMenu (once a process), %zu menu object(s) kept", TM.objs.size());
            }
        }
        if (!get_obj(tm, "_UIData")) {
            if (!TM.ud) TM.ud = create_userdata("app.training.TrainingPauseMenuUserData", "Product/GUI/gm/ui11200/TrainingData_ES.user");
            if (TM.ud) call_ok(tm, "set_UIData(app.training.TrainingPauseMenuUserData)", { TM.ud });
        }
        if (!get_obj(tm, "_UIShortCutData")) {
            if (!TM.sc) TM.sc = create_userdata("app.training.TrainingShortCutUserData", "Product/GUI/gm/ui11200/ShortCutData_ES.user");
            if (TM.sc) set_ref(tm, "_UIShortCutData", TM.sc);
        }
        call_ok(tm, "SetupSkillData()");
        call_ok(tm, "InitDynamicMenu()");
    } catch (...) { logf("training menu: a call failed while it was prepared"); }
    set(tm, "_GameMode", static_cast<double>(mode));
}

// true once the manager has its menu. InitUIMenu makes a new HUD widget set nothing destroys: the HUD is destroyed first
bool tm_prepare(Obj* tm) {
    if (TM.prep == TMState::Prep::None) {
        if (get_obj(tm, "_UIData") && get_obj(tm, "_UITrainingMenu")) return true;
        if (TM.objs_made) { tm_give(tm); TM.prep = TMState::Prep::Settle; TM.prep_n = 0; return false; }   // a later load
        call_ok(tm, "RequestAllDestroyHUD()");
        TM.prep = TMState::Prep::Destroy;
        TM.prep_n = 0;
        logf("training menu: preparing");
        return false;
    }
    ++TM.prep_n;
    if (TM.prep == TMState::Prep::Destroy) {
        Obj* dict = get_obj(tm, "_ViewUIWigetDict");
        if (call_i32(dict, "get_Count").value_or(0) == 0 || TM.prep_n > 180) { tm_give(tm); TM.prep = TMState::Prep::Settle; TM.prep_n = 0; }
        return false;
    }
    if (TM.prep_n < 20) return false;            // the menu's own flow comes up
    TM.prep = TMState::Prep::None;
    logf("training menu: ready");
    return true;
}

// our list of tabs in, the game's kept; and back (the user data is shared with Training mode)
// Measured: while our menu opens, something (the game's own dynamic tab, most likely) may replace the list with a copy of
// ours plus a tab of its own ([2, 3, 6, 345]): what is put back is the game's array whatever the list holds then, and
// the game's array is never taken again while ours (or a copy of it) is in.
void tm_tabs(Obj* tm, bool ours) {
    Obj* ui = ours ? get_obj(tm, "_UIData") : TM.tabs_ui;
    Obj* arr_ = ui ? get_obj(ui, "_MenuData") : nullptr;
    if (!ours) {
        if (ui && TM.game_tabs && TM.swapped && arr_ != TM.game_tabs) set_ref(ui, "_MenuData", TM.game_tabs);
        TM.swapped = false;
        return;
    }
    if (!arr_) return;
    if (TM.swapped && ui == TM.tabs_ui) {        // ours already in (or the copy made of it): ours again only if the game's came back
        if (arr_ == TM.game_tabs) set_ref(ui, "_MenuData", TM.our_tabs);
        return;
    }
    if (!TM.our_tabs) {
        std::vector<Obj*> tabs;
        TD* mdt = type("app.training.TrainingMenuData");
        for (int32_t i = 0, n = arr_len(arr_); i < n; ++i) {
            Obj* t = arr_get(arr_, i);
            const int64_t ft = inum(t, "_FuncType", -1);
            if (!t || !tm_tab_kept(ft)) continue;
            if (ft == 2) {                           // Environment: a copy with the rows kept
                Obj* ch = get_obj(t, "_ChildData");
                std::vector<Obj*> rows;
                for (int32_t k = 0, m = arr_len(ch); k < m; ++k)
                    if (Obj* r = arr_get(ch, k); r && tm_env_row_kept(inum(r, "_FuncType", -1))) rows.push_back(r);
                Obj* c = clone(t);
                Obj* na = c ? new_array(mdt, static_cast<int32_t>(rows.size())) : nullptr;
                if (!na) continue;
                for (size_t k = 0; k < rows.size(); ++k) arr_set(na, static_cast<int32_t>(k), rows[k]);
                set_ref(c, "_ChildData", na);
                t = c;
            }
            tabs.push_back(t);
        }
        if (tabs.empty()) return;
        Obj* na = new_array(mdt, static_cast<int32_t>(tabs.size()));
        if (!na) return;
        for (size_t k = 0; k < tabs.size(); ++k) arr_set(na, static_cast<int32_t>(k), tabs[k]);
        TM.our_tabs = na;
    }
    if (arr_ != TM.our_tabs) {
        arr_->add_ref();
        if (TM.game_tabs && TM.game_tabs != arr_) TM.game_tabs->release();
        TM.game_tabs = arr_;
        if (ui != TM.tabs_ui) { ui->add_ref(); if (TM.tabs_ui) TM.tabs_ui->release(); TM.tabs_ui = ui; }
        if (set_ref(ui, "_MenuData", TM.our_tabs)) TM.swapped = true;
    }
}
bool tm_tabs_in() { return TM.swapped; }

// The Character-Specific Settings page writes _tData.ParameterSetting.UniqueData.stock_0_NNN / timer_0_NNN (a fighter
// has one, both or none): the object, and the names of the two fields that exist for that fighter
Ref tm_unique(Obj* tm, int id, std::string& sn, std::string& tn) {
    sn.clear();
    tn.clear();
    Ref ud = get(get(get(ref(tm), "_tData"), "ParameterSetting"), "UniqueData");
    if (!ud || id <= 0) return {};
    char b[24];
    std::snprintf(b, sizeof(b), "stock_0_%03d", id);
    if (field_of(ud.td, b)) sn = b;
    std::snprintf(b, sizeof(b), "timer_0_%03d", id);
    if (field_of(ud.td, b)) tn = b;
    return ud;
}
// the page shows what the slot keeps for this fighter
void tm_unique_show(Obj* tm) {
    const int id = slot_trial ? slot_trial->fighter_id : 0;
    auto it = rs.uq.find(id);
    if (it == rs.uq.end()) return;
    std::string sn, tn;
    Ref ud = tm_unique(tm, id, sn, tn);
    if (!ud) return;
    if (!sn.empty()) set(ud, sn.c_str(), it->second.s);
    if (!tn.empty()) set(ud, tn.c_str(), it->second.t);
}

// Reset Battle without the player (rule 15): the game's own decision for that row (item type 10: it closes the menu
// and resets the battle), static UIFlowESportsPauseMenu.DecidePauseMenuItem(10, flow), from the game thread, with the
// pause menu's flow. (BattleFlow.DoSoftReset() crashes the game; Param.isDecide, set_IsReqRefresh / IsReqRestart and
// a StageSetup by hand do nothing.) When: as the player leaves the pause menu (rec::PendingReset).
bool pm_ours_on();                             // (the trial pause menu's rows, below)
std::atomic<uint64_t> setup_calls{0};          // StageSetup calls, any trial (the pending reset ends at the next one)
std::atomic<bool> pend_on{false}, pend_decided{false};   // read / set by the decide hook (another thread)
rec::PendingReset pend;
Obj* pend_flow = nullptr;                      // the pause menu's flow of the frame before (rooted while kept)

void pend_keep(Obj* flow) {
    if (flow == pend_flow) return;
    if (flow) flow->add_ref();
    if (pend_flow) pend_flow->release();
    pend_flow = flow;
}
void pend_arm() {
    pend.arm(setup_calls.load());
    pend_decided = false;
    pend_keep(nullptr);
    pend_on = true;
}
const char* reset_with(Obj* flow) {
    TD* ft = type("app.esports.UIFlowESportsPauseMenu");
    if (!flow || !live(flow) || !get_obj(flow, "_Param")) return "no flow";
    Method* m = method_of(ft, "DecidePauseMenuItem", 2);
    if (!m) return "no decide method";
    return invoke(m, nullptr, { arg_i32(10), flow }).exception_thrown ? "the call failed" : "asked";
}
// game thread, every frame while a reset is pending
void pend_tick() {
    if (pend_decided.exchange(false)) pend.row_decided();
    if (TM.want || TM.open) { pend.menu_up(); pend_keep(nullptr); return; }   // nothing is watched while the training menu is up
    rec::PauseView v;
    Obj* flow = nullptr;
    Obj* a = pause_agent();
    v.agent = a != nullptr;
    if (a) {
        Obj* cur = get_obj(get_obj(a, "_StateManager"), "_Curt");
        v.shown = cur && type_name(cur).find("Show") != std::string::npos;
        if (v.shown) {
            flow = get_obj(get_obj(a, "<InputDelegates>k__BackingField"), "<Flow>k__BackingField");
            Obj* param = get_obj(flow, "_Param");
            Obj* nx = get_obj(get_obj(flow, "_Manager"), "_Next");
            v.flow = param != nullptr;
            v.hide_next = nx && type_name(nx).find("HideObject") != std::string::npos;
            v.cancel = param && flag(param, "isCancel");
        }
    }
    const rec::PendingReset::Act act = pend.tick(setup_calls.load(), pm_ours_on(), v);
    if (act == rec::PendingReset::Now) logf("reset as the pause menu is left (on the cancel): %s", reset_with(flow));
    else if (act == rec::PendingReset::Prev) logf("reset as the pause menu is left (the frame after): %s", reset_with(pend_flow));
    pend_keep(pend.have_prev ? flow : nullptr);
    pend_on = pend.pending;
}

// what the training menu left in the training data becomes the slot's settings
void tm_sync(Obj* tm) {
    {   // the Character-Specific Settings page: the slot's fighter's two values (kept per fighter)
        const int id = slot_trial ? slot_trial->fighter_id : 0;
        std::string sn, tn;
        Ref ud = tm_unique(tm, id, sn, tn);
        if (ud && (!sn.empty() || !tn.empty())) {
            const int64_t sv = sn.empty() ? 0 : inum(ud, sn.c_str(), 0), tv = tn.empty() ? 0 : inum(ud, tn.c_str(), 0);
            if (rs.uq.count(id) || sv > 0 || tv > 0)
                rs.uq[id] = rec::OwnGauge{ static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(7, sv))),
                                           static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(2, tv))) };
        }
    }
    Ref ds = tdata(tm, "DummyStatus"), gs = tdata(tm, "GuardSetting"), cs = tdata(tm, "CounterSetting");
    if (ds) { const int64_t a = inum(ds, "DummyActionType", 0); rs.state = a >= 0 && a <= 2 ? static_cast<int>(a) : 0; }
    if (gs) rs.guard_raw = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(5, inum(gs, "GuardType", 2))));
    if (cs) rs.counter = inum(cs, "PC_TYPE", 0) == 1 ? 2 : inum(cs, "NC_TYPE", 0) == 1 ? 1 : 0;
    Obj* pl = get_obj(get(get(ref(tm), "_tData"), "ParameterSetting"), "PlayerDatas");
    if (arr_len(pl) >= 2) {
        Obj* a = arr_get(pl, 0);
        Obj* b = arr_get(pl, 1);
        const int64_t sa = inum(a, "SA_Stock", 3), dg = inum(a, "DG_Stock", 6);
        rs.sa = sa >= 3 ? 0 : static_cast<int>(sa + 1);
        rs.dg = dg >= 6 ? 0 : static_cast<int>(dg + 1);
        rs.sa2 = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(3, inum(b, "SA_Stock", 0))));
        rs.dg2 = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(6, inum(b, "DG_Stock", 6))));
        // life (percent) and the recoveries: the menu's Refill / Fixed / Standard are flags of the player data
        // (Refill = Is_<x>_Recovery_Timer, Fixed = Is_<x>_Infinity, Standard = neither)
        auto heal = [](Obj* pd, const char* what) {
            if (flag(pd, (std::string("Is_") + what + "_Recovery_Timer").c_str())) return 1;
            if (flag(pd, (std::string("Is_") + what + "_Infinity").c_str())) return 2;
            return 0;
        };
        Obj* pds[2] = { a, b };
        int* vp[2] = { &rs.vp1, &rs.vp2 };
        int* vh[2] = { &rs.vh1, &rs.vh2 };
        int* sh[2] = { &rs.sh1, &rs.sh2 };
        int* dh[2] = { &rs.dh1, &rs.dh2 };
        for (int i = 0; i < 2; ++i) {
            *vp[i] = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100, inum(pds[i], "Vital_Point", 100))));
            *vh[i] = heal(pds[i], "Vital");
            *sh[i] = heal(pds[i], "SA");
            *dh[i] = heal(pds[i], "DG");
        }
    }
    // the start position only when the menu changed it ("As Recorded" is no choice of that list)
    if (auto loc = num(get(get(ref(tm), "_tData"), "SelectMenu"), "StartLocation")) {
        const int p = tm_start_to_pos(static_cast<int64_t>(*loc));
        if (static_cast<int64_t>(*loc) != TM.start_loc.value_or(-1) && p >= 0) rs.pos = p;
    }
    rs_save();
    refresh_slots();
    if (slot_trial && slot_trial->rec) mode::apply_settings(*slot_trial, true);
    // a setting changed: the battle will be reset when the player leaves the pause menu (the start, life and a started
    // timer only show at the start of a battle; not while the pause menu is still up); config.json "rec_menu_reset":
    // false leaves it to the player
    if (cfg.rec_menu_reset && !TM.snap.empty() && TM.snap != rec::settings_key(rs, slot_trial ? slot_trial->fighter_id : 0)) pend_arm();
    logf("training menu closed: state %d guard %d counter %d, P1 sa %d dg %d, dummy sa %d dg %d, life %d / %d%s", rs.state,
         rs.guard_raw.value_or(-1), rs.counter.value_or(-1), rs.sa, rs.dg, rs.sa2, rs.dg2, rs.vp1, rs.vp2, pend.pending ? ", reset pending" : "");
}

// Our menu given up or cut short: everything of it back at once (tabs, input, half speed); the pause guard stays on
// to give pause 8 back once the pause menu is hidden
void tm_abort(const char* why) {
    if (!(TM.want || TM.open || TM.blocked || tm_tabs_in())) return;
    try {
        if (Obj* tm = tmgr()) { tm_tabs(tm, false); if (TM.open) tm_speed_off(tm); }
        tm_block(false);
    } catch (...) {}
    TM.want = TM.open = false;
    TM.want_n = 0;
    TM.unblock_in = -1;
    logf("training menu: %s", why);
}

// game thread, every frame while the menu is asked for, open, being prepared or its pause given back
void tm_tick() {
    Obj* tm = tmgr();
    if (!tm) { tm_abort("no training manager"); return; }
    if (TM.prepare && !TM.want && !TM.open) {    // the slot's pause menu is up: get ready before it is asked for
        if (tm_prepare(tm)) TM.prepare = false;
        return;
    }
    if (TM.want) {
        if (TM.want_n == 0) tdata_keep(tm);
        tm_block(true);
        tm_hold_pause();
        if (++TM.want_n > 600) { tm_abort("did not open"); return; }   // ten seconds: the pause menu takes over again
        if (!tm_prepare(tm)) return;
        TM.prepare = false;
        if (TM.want_n % 6 != 1) return;          // asked every 6 frames until it answers true
        tm_tabs(tm, true);
        tm_speed_off(tm);
        Ref td = get(ref(tm), "_tData");
        if (td) {
            set(td, "SelectIndex", 0);
            set(td, "SelectChildIndex", 0);
            Ref sm = get(td, "SelectMenu");     // the Start Position row shows the slot's choice
            if (sm && set(sm, "StartLocation", static_cast<double>(tm_pos_to_start(rs.pos)))) TM.start_loc = inum(sm, "StartLocation", 0);
        }
        tm_unique_show(tm);
        TM.snap = rec::settings_key(rs, slot_trial ? slot_trial->fighter_id : 0);
        Method* om = method_of(td_of(tm), "OpenMenu(app.training.TrainingManager.MenuType, app.training.BaseParam)");
        auto r = invoke(om, tm, { arg_i32(0), nullptr });
        if (r.exception_thrown || !r.byte) return;
        logf("training menu: open (asked %d frame(s))", TM.want_n);
        TM.want = false;
        TM.want_n = 0;
        TM.open = true;
        TM.seen = false;
        TM.closed_for = 0;
        TM.unblock_in = -1;
        return;
    }
    if (!TM.open) return;
    tm_hold_pause();
    if (TM.unblock_in >= 0) {                    // closed: the pause menu takes its input back a moment later
        if (--TM.unblock_in < 0) {
            tm_block(false);
            TM.open = false;
            logf("training menu: pause menu back%s", pend.pending ? " (a reset is pending)" : "");
        }
        return;
    }
    const int64_t st = inum(tm, "_MenuState", 0);   // 4 and above: a page of the menu is up; 3: closing / closed
    if (st >= 4) { TM.seen = true; TM.closed_for = 0; return; }
    if ((TM.seen && ++TM.closed_for >= 3) || (!TM.seen && ++TM.closed_for >= 120)) {
        tm_tabs(tm, false);
        tm_sync(tm);
        tm_speed_off(tm);
        TM.unblock_in = 8;
    }
}

// ---------------------------------------------------------------- the slot's rows in the trial pause menu
// The combo trial pause menu is built from data: Param.PauseMenuListData.Datas (app.PauseMenuListData.PauseMenuItemData[])
// read when the menu opens. While the recording slot is the trial under way the array is ours: the game's 15 rows where
// the game put them, Training Settings in the place of Open Matchmaking Settings (8) and Delete Last Recording in the
// place of Command List (6). Rows are clones of the game's Reset Battle row (rooted), texts are GUIDs of ours.
// SetupList_ComboTrial runs on a thread that is not the game thread (measured): the arrays are made on the game thread
// (pm_prepare, every frame of the slot); the hook takes no lock of the mode, runs no managed code
// and swaps one reference when the menu holds the game's full list, row for row as expected (rule 17: the menu's
// array read from memory, its layout checked on our own arrays beforehand, fields read, one reference stored).
struct PMState {
    std::map<int64_t, Obj*> by;                  // the controller's rows by item type (read once a process)
    std::vector<int64_t> order;
    bool have = false;
    uint64_t scan_at = 0;
    Obj* training = nullptr; Obj* discard = nullptr; Obj* discard_off = nullptr;
    std::vector<int64_t> types;                  // the game's full list, item type by row (fixed once ready)
    int discard_rows[2] = { -1, -1 };
    std::atomic<Obj*> arrays[3] = {};            // plain, delete, the game's own rows
    std::atomic<bool> ready{false}, want{false}, ours_on{false};
    std::atomic<int> layout{0}, discard_at{-1};
    std::mutex orig_mx;                          // the game's array of the list object we last wrote (pointers only)
    Obj* orig_key = nullptr; Obj* orig_arr = nullptr;
} PM;
std::atomic<bool> pm_tm_want{false}, pm_tm_prepare{false};   // asked from the hooks, done by the frame
bool pm_ours_on() { return PM.ours_on.load(); }

bool game_rows() {
    if (PM.have) return true;
    const auto comps = scene_components("app.esports.ESportsPauseMenuCtrl");
    Obj* arr_ = comps.empty() ? nullptr : get_obj(get_obj(comps[0], "PauseMenuListData_ComboTrial"), "Datas");
    if (!arr_ || arr_len(arr_) < 10) return false;
    std::map<int64_t, Obj*> by;
    std::vector<int64_t> order;
    for (int32_t k = 0, n = arr_len(arr_); k < n; ++k) {
        Obj* it = arr_get(arr_, k);
        const int64_t ty = inum(it, "PauseMenuItemType", -1);
        if (it && !by.count(ty)) { it->add_ref(); by[ty] = it; order.push_back(ty); }
    }
    if (!by.count(10) || !by.count(24) || !by.count(4)) return false;
    PM.by = std::move(by);
    PM.order = std::move(order);
    PM.have = true;
    return true;
}
Obj* rows_array(const std::vector<Obj*>& rows) {
    Obj* a = new_array(type("app.PauseMenuListData.PauseMenuItemData"), static_cast<int32_t>(rows.size()));
    if (!a) return nullptr;
    for (size_t k = 0; k < rows.size(); ++k) if (!arr_set(a, static_cast<int32_t>(k), rows[k])) return nullptr;
    return a;
}
Obj* button_row(Obj* src, const char* text, const char* guide) {
    Obj* c = clone(src);
    uint8_t g1[16], g2[16];
    if (!c || !text_guid(text, g1) || !text_guid(guide, g2)) return nullptr;
    write_guid(c, "ItemMessage", g1);
    write_guid(c, "GuideMessage", g2);
    return c;
}

// game thread: the slot's arrays, made once a process from the controller's full list without the rows that come and
// go (32 ExampleReplayEnd, 30 GoNextLevel, 33 ComboTrialContinue): 15 rows, the window's height
void pm_build() {
    std::vector<int64_t> types;
    for (int64_t ty : PM.order) if (ty != 32 && ty != 30 && ty != 33) types.push_back(ty);
    static bool told = false;
    if (types.size() != 15 || !std::count(types.begin(), types.end(), 8) || !std::count(types.begin(), types.end(), 6)) {
        if (!told) { told = true; logf("trial menu: the game's list is not the one known (%zu rows): the slot's rows are off", types.size()); }
        return;
    }
    if (!PM.training) PM.training = button_row(PM.by[10], "Training Settings", "Dummy, gauges of both sides and environment: the training menu's own settings.");
    if (!PM.discard) PM.discard = button_row(PM.by[10], "Delete Last Recording", "Delete the combo that was just saved, then reset the battle.");
    if (!PM.discard_off) PM.discard_off = button_row(PM.by[10], "Delete Last Recording", "No combo was saved since the last one deleted.");
    if (!PM.training || !PM.discard || !PM.discard_off) return;
    set(PM.training, "PauseMenuItemType", kPmTraining);
    set(PM.discard_off, "Enable", 0);
    for (int layout = 0; layout < 3; ++layout) {
        std::vector<Obj*> rows;
        for (size_t k = 0; k < types.size(); ++k) {
            const int64_t ty = types[k];
            Obj* it = PM.by[ty];
            if (layout < 2 && ty == 8) it = PM.training;
            if (layout < 2 && ty == 6) {
                it = layout == 1 ? PM.discard : PM.discard_off;
                if (layout == 1) PM.discard_rows[1] = static_cast<int>(k);
            }
            rows.push_back(it);
        }
        Obj* a = rows_array(rows);
        if (!a) return;
        PM.arrays[layout] = a;
    }
    for (int layout = 0; layout < 3; ++layout)
        if (!arr_raw_ok(PM.arrays[layout].load())) {
            if (!told) { told = true; logf("trial menu: the arrays' layout is not the one known: the slot's rows are off"); }
            return;
        }
    PM.types = std::move(types);
    PM.ready.store(true, std::memory_order_release);
    logf("trial menu: the slot's rows ready");
}
void pm_prepare() {
    const bool want = cfg.rec && cfg.rec_menu && rec_live && slot_trial && slot_trial->rec;
    PM.layout = last_file ? 1 : 0;
    if (want && !PM.ready.load()) {
        if (!PM.have && frame >= PM.scan_at) { PM.scan_at = frame + 60; try { game_rows(); } catch (...) {} }
        if (PM.have) try { pm_build(); } catch (...) {}
    }
    PM.want = want && PM.ready.load();
}

// SetupList_ComboTrial (pre) and SetupList (pre): any thread
int pre_setup_list(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!PM.ready.load(std::memory_order_acquire)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;   // nothing of ours ever made
    try {
        Obj* data = get_obj(reinterpret_cast<Obj*>(argv[1]), "PauseMenuListData");
        Obj* arr_ = get_obj(data, "Datas");
        if (!arr_) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const bool mine = arr_ == PM.arrays[0].load() || arr_ == PM.arrays[1].load();
        PM.ours_on = false;
        if (!PM.want.load()) {
            if (mine) {                          // another trial: the game's rows back
                Obj* back = nullptr;
                { std::lock_guard lk(PM.orig_mx); if (PM.orig_key == data) back = PM.orig_arr; }
                if (!back) back = PM.arrays[2].load();
                if (back) set_ref(data, "Datas", back);
            }
            return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        }
        if (!mine) {
            // only the game's full list is replaced, row for row as known (after Watch Example it gives 8 rows and
            // sizes the window for 8: that menu stays the game's)
            if (arr_len_raw(arr_) != static_cast<int32_t>(PM.types.size())) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
            for (int32_t k = 0, n = arr_len_raw(arr_); k < n; ++k)
                if (inum(arr_get_raw(arr_, k), "PauseMenuItemType", -1) != PM.types[static_cast<size_t>(k)]) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
            std::lock_guard lk(PM.orig_mx);
            if (PM.orig_arr != arr_) { arr_->add_ref(); if (PM.orig_arr) PM.orig_arr->release(); PM.orig_arr = arr_; }
            PM.orig_key = data;
        }
        const int layout = PM.layout.load();
        Obj* ours = PM.arrays[layout].load();
        if (!ours) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        if (arr_ != ours) set_ref(data, "Datas", ours);
        PM.discard_at = PM.discard_rows[layout];
        PM.ours_on = true;
        pm_tm_prepare = true;
    } catch (...) { PM.ours_on = false; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// static DecidePauseMenuItem(PauseMenuItemType, UIFlow<Param>): argv[1] the item type, argv[2] the flow (any thread)
int pre_decide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int64_t ty = static_cast<int64_t>(reinterpret_cast<uintptr_t>(argv[1]) & 0xFFFFFFFF);
    if (ty == kPmTraining) {                     // our row: never the game's to decide, whatever our state
        if (PM.ours_on.load()) pm_tm_want = true;
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    if (pend_on.load()) pend_decided = true;     // (a plain store: this is not our thread)
    // Reset Battle or our clone of it (the game resets the battle either way): which row it was. GetSelectIndex() is the
    // one call left here (a getter returning the cursor's row; no field for it is known), as the reference does.
    if (ty == 10 && PM.ours_on.load() && PM.discard_at.load() >= 0)
        try {
            Obj* param = get_obj(reinterpret_cast<Obj*>(argv[2]), "_Param");
            if (call_i32(param, "GetSelectIndex()").value_or(-1) == PM.discard_at.load()) want_discard = true;
        } catch (...) {}
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---------------------------------------------------------------- combos recorded while a replay plays
// "Start Combo Recording" in the replay's pause menu; from there every finished combo of either player is held as a
// trial; "Save Combo Recording" writes them (the one under way too), "Cancel Recording" drops them; a round that ends
// writes what is held by itself. The game's record countdown widget shows REC / SAVED on screen.
struct Pending { std::string path; json trial; int chara; };
// The decide hook runs on the menu's threads (measured: four of them in a session): it only notes which row, in an
// atomic, with the controller's address; everything happens on the game thread at the next replay tick.
struct RPState {
    bool on = false;
    Side sides[2];
    std::vector<Pending> pending;
    std::atomic<int> want{0};
    std::atomic<Obj*> ctl{nullptr};              // the ReplayUICtrl that decided (an address only, checked before use)
    int level = 0;                               // the tab of the last save chosen (0 Beginner, 1 Intermediate, 2 Advanced)
    std::optional<double> clock;
    int64_t frame = 0;
    int ko_for = 0, idle = 0, saved = 0;
    struct Note { std::string a, b; int left; };
    std::optional<Note> note;
    std::string hud;
} RP;

bool replay_modern(int i) {
    Obj* m = singleton("app.BattleReplayDataManager");
    Obj* items = get_obj(get_obj(m, "_ReplayList"), "_items");
    Obj* it = items && arr_len(items) > 0 ? arr_get(items, 0) : nullptr;
    Ref info = get(get(ref(it), "ReplayInfo"), i == 0 ? "player1_info" : "player2_info");
    return info && inum(info, "battle_input_type", 0) == 1;
}

// The training REC widget is not loaded in a replay; its neighbour, the record countdown ring, is (key 0 of
// TrainingManager._ViewUIWigetDict). Looked up at each write, never kept; each line written only when it differs from
// what the control shows (rule 3: writing the same text crashed), through the String overloads by full signature.
void replay_hud(const char* text, const std::string& sub) {
    try {
        Obj* tm = tmgr();
        TD* cd = type(kWidgetCountDown);
        Obj* w = nullptr;
        for (const auto& [key, v] : dict_u32(get_obj(tm, "_ViewUIWigetDict"))) {   // Dictionary<UInt32, List<UIWidget>>
            (void)key;
            if (list_count(v) > 0) if (Obj* it = list_item(v, 0); it && td_of(it) == cd) { w = it; break; }
        }
        if (!w) { RP.hud.clear(); return; }
        Method* view_m = method_of(td_of(tm), "RequestViewHUD(app.UIWidget, System.Boolean)");
        const bool showing = call_bool(w, "isShow()").value_or(false);
        if (!text) {
            if (showing && view_m) invoke(view_m, tm, { w, arg_bool(false) });
            RP.hud.clear();
            return;
        }
        if (!showing && view_m) { invoke(view_m, tm, { w, arg_bool(true) }); RP.hud.clear(); }
        const std::string key = std::string(text) + "|" + sub;
        if (RP.hud == key) return;
        Obj* st = get_obj(w, "_SlotText");
        Obj* rt = get_obj(w, "_ReadyText");
        if (st && message(st) != text) if (Obj* s = managed_string(text)) call_ok(w, "SetText(System.String)", { s });
        if (rt && message(rt) != sub) if (Obj* s = managed_string(sub)) call_ok(w, "SetReadyText(System.String)", { s });
        RP.hud = key;
    } catch (...) {}
}

void replay_close(int chara, Take& t) {
    const RecLabels& L = labels_for(chara);
    const char* dir = fighter_dir(chara);
    TrialOpts o;
    o.min_steps = cfg.rec_min_steps;
    o.replay = true;
    o.dir = dir ? dir : "Unknown";
    o.when = stamp_of(std::time(nullptr));
    Closed c = close_take(t, L, chara, o);
    if (c.kind != Closed::Made) return;
    const auto replaced = find_recorded(chara, c.sig);
    if (replaced && replaced->damage > t.damage) return;   // the same combo is saved already, with more damage
    std::set<std::string> taken;
    for (const Pending& p : RP.pending) taken.insert(p.path);
    std::string path = replaced && !taken.count(replaced->path) ? replaced->path : new_take_path(o.dir, o.when.file, taken);
    if (replaced && path == replaced->path) {
        c.trial["title"] = replaced->title;
        c.trial["order"] = replaced->order;
    }
    RP.pending.push_back({ path, std::move(c.trial), chara });
}

void replay_drop() { for (Side& s : RP.sides) s = Side{}; }

// every held combo written, in the tab chosen (it wins over a replaced file's); the takes under way too when asked
size_t replay_flush(bool with_open) {
    if (with_open) {
        for (int i = 0; i < 2; ++i) {
            Side& s = RP.sides[i];
            Obj* pa = player(i);
            if (!pa || !s.take || s.take->hits == 0) continue;
            const int chara = static_cast<int>(inum(pa, "pl_type", 0));
            Take t = std::move(*s.take);
            s.take.reset();
            if (fighter_dir(chara) && replay_cut(t)) replay_close(chara, t);
        }
    }
    size_t n = 0;
    for (Pending& p : RP.pending) {
        p.trial["level"] = RP.level;
        if (write_json(upath(p.path), p.trial)) { on_saved(p.chara, p.trial, p.path); ++n; }
        else logf("replay recording: %s not written", p.path.c_str());
    }
    RP.pending.clear();
    replay_drop();
    RP.saved += static_cast<int>(n);
    return n;
}

void replay_side_tick(int i, Obj* pa, Obj* pd, int step) {
    const int chara = static_cast<int>(inum(pa, "pl_type", 0));
    if (!fighter_dir(chara)) return;             // (255 outside a fight)
    Side& s = RP.sides[i];
    const bool modern = s.take ? s.take->modern : replay_modern(i);
    const Frame in = read_frame(pa, pd, modern);
    if (auto closed = replay_side(s, in, step, RP.frame, labels_for(chara))) replay_close(chara, *closed);
}

// The Replay Menu's rows live in an asset the game keeps loaded (Product/GUI/menu/ui90700/data/ReplayPauseMenuData.user,
// app.ReplayPauseMenuData.MenuItems: the very object the menu's Param._MenuData points at). Its MenuItems is written
// here, on the game thread, when what our rows say changes (a replay under way, a recording on, the combos held); the
// menu finds them when it opens. Leaving the replay puts the game's own array back. Ours are clones of "Play from
// Beginning" with a FuncType of ours, before "Open Matchmaking Settings"; made once (rooted), their texts retexted.
struct RMState {
    Obj* md = nullptr;
    bool failed = false, set = false;
    std::vector<Obj*> base;                      // the game's rows (rooted)
    Obj* game = nullptr;                         // the game's own array (rooted)
    std::string state;
    std::map<int, Obj*> rows;                    // [FuncType] our row
    std::map<int, std::pair<std::array<uint8_t, 16>, std::string>> texts;   // [FuncType] its GUID and text
    Obj* arr_idle = nullptr; Obj* arr_rec = nullptr;
} RM;

Obj* replay_row(Obj* src, int ft, const std::string& text) {
    auto it = RM.rows.find(ft);
    if (it != RM.rows.end()) {                   // made already: a new text for its GUID when it changed
        auto& [g, t] = RM.texts[ft];
        if (t != text && core->text_set(client, g.data(), text.c_str()) == SF6CORE_OK) t = text;
        return it->second;
    }
    Obj* c = clone(src);
    uint8_t g[16];
    if (!c || !text_guid(text, g)) return nullptr;
    set(c, "FuncType", ft);
    write_guid(c, "ItemMsg", g);
    std::array<uint8_t, 16> ga;
    std::memcpy(ga.data(), g, 16);
    RM.texts[ft] = { ga, text };
    RM.rows[ft] = c;
    return c;
}

void replay_rows_write(bool in_replay) {
    if (RM.failed || !cfg.rec) return;
    if (!RM.md) {
        if (!in_replay) return;
        RM.md = create_userdata("app.ReplayPauseMenuData", "Product/GUI/menu/ui90700/data/ReplayPauseMenuData.user");
        if (!RM.md || !get_obj(RM.md, "MenuItems")) { RM.failed = true; logf("replay menu: its rows' asset was not found: no rows of ours"); return; }
    }
    if (RM.base.empty()) {
        Obj* arr_ = get_obj(RM.md, "MenuItems");
        for (int32_t i = 0, n = arr_len(arr_); i < n; ++i)
            if (Obj* it = arr_get(arr_, i); it && !replay_ft_ours(inum(it, "FuncType", -1))) { it->add_ref(); RM.base.push_back(it); }
        if (RM.base.empty()) { RM.failed = true; return; }
        RM.game = new_array(type("app.ReplayMenuItem"), static_cast<int32_t>(RM.base.size()));
        if (!RM.game) { RM.failed = true; return; }
        for (size_t k = 0; k < RM.base.size(); ++k) arr_set(RM.game, static_cast<int32_t>(k), RM.base[k]);
    }
    const std::string st = replay_rows_state(in_replay, RP.on, RP.pending.size());
    if (st == RM.state) return;
    RM.state = st;
    if (st == "off") {
        set_ref(RM.md, "MenuItems", RM.game);
        RM.set = false;
        return;
    }
    Obj* src = nullptr;
    size_t at_ = RM.base.size();
    for (size_t i = 0; i < RM.base.size(); ++i) {
        const int64_t ft = inum(RM.base[i], "FuncType", -1);
        if (ft == 8) src = RM.base[i];           // "Play from Beginning": a plain button of the first tab
        if (ft == 12 && at_ == RM.base.size()) at_ = i;   // ours go before "Open Matchmaking Settings"
    }
    if (!src) { RM.failed = true; logf("replay menu: no row to copy: no rows of ours"); return; }
    std::vector<Obj*> ours;
    for (const ReplayRow& r : replay_rows(RP.on, RP.pending.size())) {
        Obj* o = replay_row(src, r.ft, r.text);
        if (!o) return;
        ours.push_back(o);
    }
    Obj*& cache = RP.on ? RM.arr_rec : RM.arr_idle;
    if (!cache) {
        std::vector<Obj*> rows;
        for (size_t i = 0; i <= RM.base.size(); ++i) {
            if (i == at_) rows.insert(rows.end(), ours.begin(), ours.end());
            if (i < RM.base.size()) rows.push_back(RM.base[i]);
        }
        Obj* a = new_array(type("app.ReplayMenuItem"), static_cast<int32_t>(rows.size()));
        if (!a) return;
        for (size_t k = 0; k < rows.size(); ++k) if (!arr_set(a, static_cast<int32_t>(k), rows[k])) return;
        cache = a;
    }
    set_ref(RM.md, "MenuItems", cache);
    RM.set = true;
}

void replay_tick_impl(bool in_replay) {
    try { replay_rows_write(in_replay); } catch (...) { logf("replay menu: rows not written (an exception)"); }
    if (const int w = RP.want.exchange(0)) {      // a row of the menu was chosen: done here, not in the hook
        if (w == kFtStart) {
            RP.on = true; RP.pending.clear(); RP.clock.reset(); RP.ko_for = 0; RP.saved = 0; RP.note.reset();
            replay_drop();
            logf("replay recording: started");
        } else if (const int lv = replay_save_level(w); lv >= 0) {
            RP.level = lv;
            const size_t n = replay_flush(true);
            RP.note = RPState::Note{ "SAVED", combos_text(n), 150 };
            RP.on = false;
            logf("replay recording: saved %zu combo(s) in tab %d", n, lv);
        } else if (w == kFtCancel) {
            RP.pending.clear();
            replay_drop();
            RP.on = false;
            RP.note = RPState::Note{ "CANCELLED", "", 90 };
            logf("replay recording: cancelled");
        }
        Obj* ctl = RP.ctl.exchange(nullptr);
        TD* ct = type("app.esports.ReplayUICtrl");
        if (ctl && live(ctl) && td_of(ctl) == ct) call_ok(ctl, "ClosePauseMenu(System.Boolean)", { arg_bool(true) });
        try { replay_rows_write(in_replay); } catch (...) {}
    }
    if (!RP.on) {
        if (RP.note) {                           // the last word of a recording, for a moment, then nothing
            if (!in_replay) RP.note.reset();     // (out of the replay the widget is Training's: never touched)
            else if (--RP.note->left <= 0) { RP.note.reset(); replay_hud(nullptr, ""); }
            else if (RP.note->left % 15 == 0) replay_hud(RP.note->a.c_str(), RP.note->b);
        }
        return;
    }
    if (!in_replay) {                            // left with a recording on: what is held goes where the last choice went
        replay_flush(false);
        RP.on = false;
        logf("replay left with a recording on: what was held is written (tab %d)", RP.level);
        try { replay_rows_write(false); } catch (...) {}
        return;
    }
    Obj* a = player(0);
    Obj* b = player(1);
    const auto clock = battle_clock();
    if (!a || !b || !clock) return;
    int step = 1;
    if (RP.clock) {
        if (*clock == *RP.clock) {               // paused
            if (++RP.idle % 30 == 0) replay_hud("REC", combos_text(RP.pending.size()));
            return;
        }
        step = static_cast<int>(*clock - *RP.clock);
        if (step < 0 || step > 30) { replay_drop(); step = 1; }   // rewind, fast-forward, another round: takes start over
    }
    RP.clock = clock;
    RP.frame += step;
    try { replay_side_tick(0, a, b, step); } catch (...) { RP.sides[0] = Side{}; }
    try { replay_side_tick(1, b, a, step); } catch (...) { RP.sides[1] = Side{}; }
    if (RP.frame % 20 == 0) replay_hud("REC", combos_text(RP.pending.size()));
    // the round is over: once the last combo has closed, what is held is written and the recording stops
    const bool ko = inum(a, "vital_new", 1) <= 0 || inum(b, "vital_new", 1) <= 0;
    if (ko) {
        ++RP.ko_for;
        const bool open = (RP.sides[0].take && RP.sides[0].take->hits > 0) || (RP.sides[1].take && RP.sides[1].take->hits > 0);
        if (!open || RP.ko_for > 240) {
            const size_t n = replay_flush(true);
            RP.note = RPState::Note{ "SAVED", combos_text(n), 180 };
            RP.on = false;
            logf("replay recording: round over, %zu combo(s) saved (tab %d)", n, RP.level);
        }
    } else RP.ko_for = 0;
}

// ReplayUICtrl.OnDecideItem(EReplayFuncType, int, bool), any thread: argv[1] this, argv[2] the row's FuncType. Our
// rows: noted (a number and an address), their original skipped; every other row passed through untouched.
int pre_replay_decide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int64_t ft = static_cast<int64_t>(reinterpret_cast<uintptr_t>(argv[2]) & 0xFFFFFFFF);
    if (!replay_ft_ours(ft)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    RP.ctl = reinterpret_cast<Obj*>(argv[1]);
    RP.want = static_cast<int>(ft);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}

// ---------------------------------------------------------------- the recorder the mode calls
struct SlotRecorder final : rec::Recorder {
    int flow_prev = -1;

    // every frame, first thing of the mode's tick (game thread)
    void frame(int flow) override {
        const bool was_trial = flow_prev == kFlowComboTrial;
        flow_prev = flow;
        const bool trial = flow == kFlowComboTrial;
        // what the pause menu's hooks asked for (they run on other threads), and the slot's rows ready for them
        if (pm_tm_want.exchange(false) && trial && rec_live && !TM.open) TM.want = true;
        if (pm_tm_prepare.exchange(false) && trial && rec_live && !TM.open && !TM.want) TM.prepare = true;
        try { pm_prepare(); } catch (...) {}
        // our menu cannot outlive the slot nor the trial
        if ((TM.want || TM.open) && (!trial || !rec_live)) tm_abort("closed with the slot");
        if (!TM.want && !TM.open && tm_tabs_in()) tm_abort("our tab list found in with no menu of ours: the game's put back");
        if (TM.prepare && !trial) { TM.prepare = false; TM.prep = TMState::Prep::None; }
        if (TM.want || TM.open || TM.prepare) {
            try { tm_tick(); } catch (...) { tm_abort("an exception"); TM.prepare = false; TM.prep = TMState::Prep::None; }
        } else if (TM.guard) {
            if (trial) { try { tm_guard(); } catch (...) { TM.guard = false; } }
            else {                               // the trial is left with our pause asked: given back once, nothing kept
                try { if (was_trial && (pause_bits() & 256)) request_pause(false, 8); } catch (...) {}
                TM.guard = false;
                TM.hidden_for = 0;
            }
        }
        if (pend.pending) { try { pend_tick(); } catch (...) { pend.pending = false; pend_on = false; pend_keep(nullptr); } }
        // the training data put back (asked when the slot was left under a held pause): once the fight runs again, or on
        // the first frame out of the trial; later than that the next mode may have loaded its own: dropped
        if (tsnap.pending && !TM.want && !TM.open) {
            if (trial) { if (!in_pause) tdata_restore(true); }
            else if (was_trial) tdata_restore(false);
            else tdata_drop();
        }
    }

    void tick(void* mission_, Trial& slot, int chara) override {
        Obj* m = reinterpret_cast<Obj*>(mission_);
        if (in_pause) { shown.burst = true; return; }
        Obj* pa = player(0);
        Obj* pd = player(1);
        if (!m || !pa || !pd) return;
        const RecLabels& L = labels_for(chara);
        if (shown.burst) { shown.burst = false; shown.burst_until = core_.frame + 120; shown.has_key = false; shown.check_at = 0; }
        // asked for in the pause menu, done here: the fight is live again (rule 4)
        if (want_discard.exchange(false)) { try { discard_last(); } catch (...) { logf("recorder: delete failed"); } }
        // how many frames of the fight this tick covers: 1 nearly always, 3 or 4 when the frame rate dips
        const auto clock = battle_clock();
        int step = 1;
        if (clock && slot_clock) {
            step = static_cast<int>(*clock - *slot_clock);
            if (step < 0 || step > 30) {         // the fight started over (Reset Battle, an example): so does the take
                step = 1;
                if (core_.take) { core_.take.reset(); shown.has_key = false; }
                shown.burst = true;
            }
        }
        slot_clock = clock;
        // "Watch Example" replays the last take: nothing is recorded meanwhile; its lines clear with the take's timing
        if (++ex_poll % 10 == 0) shown.example = mode::example_playing();
        if (shown.example) {
            core_.frame += step;
            if (core_.take) { core_.take.reset(); shown.has_key = false; }
            core_.hits.clear();
            core_.last_cc = static_cast<int>(inum(pa, "combo_cnt", 0));
            if (const json* sf = arr(obj(&slot.j, "demo"), "step_frames")) {
                const double f = clock.value_or(0);
                int done = 0;
                for (size_t i = 0; i < sf->size(); ++i) if (f >= num((*sf)[i]).value_or(0) + 22) done = static_cast<int>(i) + 1;
                shown.ex_done = done;
            }
            try { show_rows(m, L, &slot); } catch (...) {}
            return;
        }
        shown.ex_done.reset();
        const Frame in = read_frame(pa, pd, mode_modern());
        // Reset Battle while the rows still show the last take: both fighters jump back to their start
        if ((!last_steps.empty() || result != Result::None) && !shown.cleared && in.cc == 0 && !core_.live()) {
            if (in.x1 && in.x2 && shown.x1 && shown.x2 &&
                (std::fabs(*in.x1 - *shown.x1) > kResetJump || std::fabs(*in.x2 - *shown.x2) > kResetJump)) {
                shown.cleared = true;
                shown.has_key = false;
            }
            shown.x1 = in.x1;
            shown.x2 = in.x2;
        } else { shown.x1.reset(); shown.x2.reset(); }
        SlotCore::Out o = core_.tick(in, step, L);
        if (o.recording) { last_steps.clear(); result = Result::None; }
        if (o.redraw) shown.has_key = false;
        if (o.closed) { try { finish_slot_take(*o.closed, chara); } catch (...) { logf("recorder: save failed"); } }
        try { show_rows(m, L, &slot); } catch (...) {}
    }

    // the slot is left (another row, the menus, the flow): nothing is kept half-recorded
    void leave() override {
        core_.drop();
        slot_clock.reset();
        if (!last_steps.empty() || result != Result::None) shown.cleared = true;
        shown.burst = true;
        shown.has_key = false;
        shown.rebuilds = 0;
        shown.rebuild_at = 0;
        want_discard = false;
        PM.ours_on = false;
        PM.want = false;
        pm_tm_want = false;
        pm_tm_prepare = false;
        tm_abort("closed with the slot");
        TM.prepare = false;
        if (tsnap.td) {
            tsnap.pending = true;
            const int f = core ? core->get_flow_id() : -1;
            if (f != kFlowComboTrial) tdata_restore(false);       // the flow is left: now, before the next mode loads its data
            else if (!in_pause) tdata_restore(true);
        }
    }

    void on_attack(void* mission_, void* attacker) override {
        (void)mission_;
        if (!core_.take || !attacker) return;
        Obj* w = reinterpret_cast<Obj*>(attacker);
        TD* work = type("nBattle.cWork");
        const bool proj = work && td_of(w) == work;
        if (proj && inum(w, "pl_no", -1) != 0) return;
        if (!proj && w != player(0)) return;     // the dummy hitting back
        core_.push_hit(act_of(w), proj);
    }

    bool training_menu_open() override { return TM.open || TM.want; }
    bool has_take(int chara) override { return !last_steps.empty() && last_chara == chara; }
    std::vector<Step> preview_steps(const Trial& slot) override {
        if (last_steps.empty() || last_chara != slot.fighter_id) return rec::Recorder::preview_steps(slot);
        std::vector<Step> out;
        const Labels& lb = labels_of(slot.fighter_id);
        for (const json& j : last_steps) { Step st = step_from(j); resolve_step(st, lb.groups); out.push_back(std::move(st)); }
        return out;
    }
    bool fixed_start() override { return rs.pos != 0; }
    bool replay_on() override { return RP.on || RP.want.load() != 0 || RM.set; }   // (RM.set: the game's rows to put back)
    void replay_tick(bool in_replay) override { replay_tick_impl(in_replay); }

    static bool mode_modern() { return input_modern(); }
};

// ---------------------------------------------------------------- set-up
void install_hooks() {
    if (!cfg.rec) { logf("recorder off (config.json \"rec\": false)"); return; }
    hook("app.esports.UIFlowESportsPauseMenu.Param", "SetupList_ComboTrial", -1, pre_setup_list, post_pass);
    hook("app.esports.UIFlowESportsPauseMenu.Param", "SetupList", -1, pre_setup_list, post_pass);
    hook("app.esports.UIFlowESportsPauseMenu", "DecidePauseMenuItem", 2, pre_decide, post_pass);
    hook("app.esports.ReplayUICtrl", "OnDecideItem", 3, pre_replay_decide, post_pass);
}

std::unique_ptr<rec::Recorder> make() {
    rs = rec::load_settings(kData);
    return std::make_unique<SlotRecorder>();
}

}  // namespace recorder
