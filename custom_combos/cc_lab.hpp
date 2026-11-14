// cc_lab.hpp - Custom Combos: the game itself as the judge of the example builder ("lab").
//
// Combo Studio builds a trial's example (the inputs "Watch Example" plays) by trying placements of each line. Its
// judge was an outside simulator; here the game plays the tries instead, so what is kept is what the game does.
// Included by custom_combos.cpp after the recorder, inside its anonymous namespace. Everything runs on the game thread
// (the mode's frame); nothing is hooked.
//
// The channel: files in SF6_CustomTrials_data/_lab/
//   * status.json, written twice a second: { stamp (ms, system clock), file ("<Fighter>/<name>.json" of the trial
//     fought, "" when none), ready (a trial of ours is installed and the lab is idle), busy }
//   * request.json, written by Combo Studio: { id, file, runs: [ { rle: [[frames, mask]...], frames,
//     stop: { ids: [action ids], after: frame, hit: bool } } ] }
//   * result.json, written when a request is done: { id, ok, reason, runs: [ { frames: [[action, hits, damage]...] } ] }
//     one entry per frame of the fight from the start of the example (frame 0 = the first frame after it started;
//     Combo Studio measures once how far the first input lands from there).
//
// How a try is played (found in game, 14/11): the battle flow's phases (bFlowManager.m_flow_work.MainFlow._Phase)
// hold the combo trial's steps; phase 13 is the example (start / update / endComboTrialExampleReplay) and phase 4 the
// fight. Each try: phase 4, the example's input buffer (ComboTrialData.ExampleData.InputData buff / Num) written with
// the try, phase 13; then one sample per frame of the fight (gBattle.Game.stage_timer) of P1's action, P1's combo
// counter and the combo's damage, until the frames asked, or earlier when the stop rule says the answer is known. At
// the end the trial's own example is laid again and the fight goes on (phase 4).
#pragma once

namespace lab {

using recorder::player;
using recorder::battle_clock;
using recorder::combo_damage;
using recorder::write_json;

const fs::path kLab = kData / "_lab";
std::atomic<bool> busy_flag{ false };            // the lab plays tries (NextStage is skipped meanwhile)
uint64_t quiet_until = 0;                        // ... and for a while after: the clear a try made shows up ~10 s later
constexpr int kPhaseSetting = 1, kPhaseFight = 4, kPhaseExample = 13;   // 1: the round set up again (1 -> 2 -> 3 -> 4)
constexpr int kMargin = 150;                   // frames recorded past the ones asked (the example starts a few frames in)

struct Run {
    std::vector<std::pair<int64_t, uint16_t>> rle;
    int frames = 0;
    std::set<int64_t> stop_ids;
    int stop_after = -1;
    bool stop_hit = false;
};
enum class St { Idle, Leave, Settle, Arm, Start, Record };
struct State {
    St st = St::Idle;
    std::string id;
    std::vector<Run> runs;
    size_t cur = 0;
    int wait = 0;
    std::vector<std::array<int64_t, 3>> rec;
    json results = json::array();
    int64_t clock0 = -1, last_clock = -1;
    int seen_at = -1, hit_at = -1, hits_at_seen = 0, started = 0;
    Obj* entry = nullptr;                      // the list entry whose example is borrowed (rooted while busy)
    Obj* orig_buff = nullptr;                  // its buffer before the lab (rooted while busy)
    double orig_num = 0;
    Obj* buf = nullptr;                        // the lab's own buffer, rooted for the process
    int32_t buf_len = 0;
} L;
std::string last_req_id;
const auto started_at = std::filesystem::file_time_type::clock::now();   // a request older than the game's start: ignored
fs::file_time_type last_req_time{};
uint64_t status_at = 0, trial_seen = 0;
const Trial* cur_trial = nullptr;              // the trial of ours fought this frame (set by tick)

Obj* main_phase() {
    Obj* bfm = singleton("app.bFlowManager");
    Obj* w = bfm ? get_obj(bfm, "m_flow_work") : nullptr;
    Obj* mf = w ? get_obj(w, "MainFlow") : nullptr;
    return mf ? get_obj(mf, "_Phase") : nullptr;
}
// The trial flow map's IsExamplePlaying: up while a try plays, as during the game's own example, so the judge does not
// take the try for the player's own (seen 14/11: a try that performed the whole recipe cleared the trial, and its
// automatic progression moved to the next one)
void example_flag(bool on) {
    Obj* bfm = singleton("app.bFlowManager");
    Obj* w = bfm ? get_obj(bfm, "m_flow_work") : nullptr;
    Obj* fm = w ? get_obj(w, "_FlowMap") : nullptr;
    if (fm && td_of(fm) && td_of(fm)->get_full_name() == "app.battle.ComboTrialFlowMap") set(fm, "IsExamplePlaying", on ? 1.0 : 0.0);
}
int phase_index() { Obj* p = main_phase(); return p ? static_cast<int>(inum(p, "_PhaseIndex", -1)) : -1; }
bool change_phase(int i) {
    Obj* p = main_phase();
    Method* m = p ? method_of(td_of(p), "changePhase(System.Int32)") : nullptr;
    return m && !invoke(m, p, { arg_i32(i) }).exception_thrown;
}

// "<Fighter>/<file>.json" of a trial's path (either separator)
std::string rel_of(const std::string& path) {
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    const size_t a = p.find_last_of('/');
    if (a == std::string::npos || a == 0) return "";
    const size_t b = p.find_last_of('/', a - 1);
    return p.substr(b == std::string::npos ? 0 : b + 1);
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void write_status() {
    const bool fought = cur_trial && frame - trial_seen <= 5;
    json s = { { "stamp", now_ms() }, { "file", fought ? rel_of(cur_trial->path) : std::string() },
               { "ready", fought && L.st == St::Idle }, { "busy", L.st != St::Idle } };
    write_json(kLab / "status.json", s);
}

void stop_player();

void finish(bool ok, const std::string& reason) {
    json r = { { "id", L.id }, { "ok", ok }, { "reason", reason }, { "runs", L.results } };
    write_json(kLab / "result.json", r);
    if (L.entry) {                             // the trial's own example back
        if (Obj* recd = get_obj(get_obj(L.entry, "ExampleData"), "InputData")) {
            if (L.orig_buff) set_ref(recd, "buff", L.orig_buff);
            set(recd, "Num", L.orig_num);
            set(recd, "Cursor", 0);
        }
        if (cur_trial) try { apply_demo(L.entry, *cur_trial); } catch (...) {}
        L.entry->release();
    }
    if (L.orig_buff) L.orig_buff->release();
    stop_player();
    example_flag(false);
    change_phase(kPhaseSetting);               // a fresh round: positions, gauges and the judge as the trial starts
    quiet_until = frame + 900;
    logf("lab: request %s done (%s), %zu run(s)%s%s", L.id.c_str(), ok ? "ok" : "failed", L.results.size(),
         reason.empty() ? "" : ": ", reason.c_str());
    Obj* keep = L.buf;
    const int32_t keep_len = L.buf_len;
    L = State{};
    L.buf = keep;
    L.buf_len = keep_len;
}

// The try's inputs into the example's buffer (frames total: the inputs, then nothing)
bool lay(const Run& r) {
    Obj* recd = get_obj(get_obj(L.entry, "ExampleData"), "InputData");
    if (!recd) return false;
    // exactly the game's size (TrainingInput.BUFF_SIZE, 3600): when the example starts the game copies it into its own
    // 3600-entry buffer, and a longer one is not copied at all (seen 14/11: Num copied, the inputs all 0)
    const int32_t need = 3600;
    if (!L.buf || L.buf_len != need) {
        Obj* b = new_array(type("System.UInt16"), need);      // rooted by new_array: the game reads it long after
        if (!b) return false;
        L.buf = b;
        L.buf_len = need;
    }
    uint16_t* data = u16_data(L.buf, L.buf_len);
    if (!data) return false;
    std::fill(data, data + L.buf_len, static_cast<uint16_t>(0));
    int64_t head = 0;
    for (const auto& [n, mask] : r.rle) {
        for (int64_t k = head; k < head + n && k < L.buf_len; ++k) data[k] = mask;
        head += std::max<int64_t>(0, n);
    }
    set_ref(recd, "buff", L.buf);
    set(recd, "Num", static_cast<double>(std::min<int64_t>(L.buf_len, r.frames + kMargin)));     // (frames capped at 3450 by the request)
    set(recd, "Cursor", 0);
    return true;
}

// The example player (gBattle.Training, app.TrainingInput): RecordData[0] is the record the example plays from. The
// start of the example phase does not always copy the trial's example into it (seen 14/11: Num 0, or Num copied and
// the inputs not), so the try is written into it directly, on the first frame of the example phase.
bool lay_player(const Run& r) {
    Obj* ti = static_obj("gBattle", "Training");
    Obj* rd = ti ? get_obj(ti, "RecordData") : nullptr;
    Obj* rec = rd && arr_len(rd) > 0 ? arr_get(rd, 0) : nullptr;
    Obj* buff = rec ? get_obj(rec, "buff") : nullptr;
    const int32_t len = buff ? arr_len(buff) : 0;
    uint16_t* data = len > 0 ? u16_data(buff, len) : nullptr;
    if (!data) return false;
    std::fill(data, data + len, static_cast<uint16_t>(0));
    int64_t head = 0;
    for (const auto& [n, mask] : r.rle) {
        for (int64_t k = head; k < head + n && k < len; ++k) data[k] = mask;
        head += std::max<int64_t>(0, n);
    }
    set(rec, "Num", static_cast<double>(std::min<int64_t>(len, r.frames + kMargin)));
    set(rec, "Cursor", 0);
    set(rec, "State", 3);                      // TrainingInput.PLAY_NOW
    return true;
}

// The example player stopped: nothing of a try is played into the fight once the example phase is left (seen 14/11:
// a try went on in the fight, cleared the trial for real and the trial list moved to the next one)
void stop_player() {
    Obj* ti = static_obj("gBattle", "Training");
    Obj* rd = ti ? get_obj(ti, "RecordData") : nullptr;
    Obj* rec = rd && arr_len(rd) > 0 ? arr_get(rd, 0) : nullptr;
    if (!rec) return;
    set(rec, "Num", 0);
    set(rec, "Cursor", 0);
    set(rec, "State", 0);                      // TrainingInput.NON_ACTIVE
}

bool read_request(const Trial& t) {
    const fs::path p = kLab / "request.json";
    std::error_code ec;
    const auto when = fs::last_write_time(p, ec);
    if (ec || when == last_req_time) return false;
    last_req_time = when;
    if (when < started_at) return false;
    const json q = read_json(p);
    if (!q.is_object()) return false;
    const std::string id = str_of(at(&q, "id"));
    if (id.empty() || id == last_req_id) return false;
    last_req_id = id;
    L = State{ St::Idle, id, {}, 0, 0, {}, json::array(), -1, -1, -1, -1, 0, 0, nullptr, nullptr, 0, L.buf, L.buf_len };
    const std::string want = str_of(at(&q, "file"));
    if (t.rec || t.path.empty() || rel_of(t.path) != want) {
        finish(false, "the game is not in that trial (" + (t.rec ? std::string("the recording slot") : rel_of(t.path)) + ")");
        return false;
    }
    if (const json* runs = arr(&q, "runs")) {
        for (const json& j : *runs) {
            Run r;
            if (const json* rle = arr(&j, "rle"))
                for (const json& e : *rle)
                    if (e.is_array() && e.size() >= 2) r.rle.push_back({ inum(&e[0], 0), static_cast<uint16_t>(inum(&e[1], 0)) });
            r.frames = static_cast<int>(std::clamp<int64_t>(inum(at(&j, "frames"), 300), 10, 3600 - kMargin));
            if (const json* s = at(&j, "stop"); s && s->is_object()) {
                if (const json* ids = arr(s, "ids")) for (const json& i : *ids) r.stop_ids.insert(inum(&i, -1));
                r.stop_after = static_cast<int>(inum(at(s, "after"), -1));
                r.stop_hit = at(s, "hit") && at(s, "hit")->is_boolean() && at(s, "hit")->get<bool>();
            }
            L.runs.push_back(std::move(r));
        }
    }
    if (L.runs.empty()) { finish(false, "no run asked"); return false; }
    Obj* inst = ctm();
    Obj* entry = inst ? call_obj(inst, "GetComboTrialData") : nullptr;
    Obj* recd = entry ? get_obj(get_obj(entry, "ExampleData"), "InputData") : nullptr;
    if (!recd) { finish(false, "no example data on the trial"); return false; }
    entry->add_ref();
    L.entry = entry;
    if (Obj* b = get_obj(recd, "buff")) { b->add_ref(); L.orig_buff = b; }
    L.orig_num = num(recd, "Num").value_or(0);
    L.st = St::Leave;
    logf("lab: request %s, %zu run(s) on %s", id.c_str(), L.runs.size(), want.c_str());
    return true;
}

void sample(int64_t act, int64_t hits, int64_t dmg) {
    const int raw = static_cast<int>(L.rec.size());
    L.rec.push_back({ act, hits, dmg });
    const Run& r = L.runs[L.cur];
    if (r.stop_ids.empty()) return;
    if (L.seen_at < 0 && raw > r.stop_after && r.stop_ids.count(act)) { L.seen_at = raw; L.hits_at_seen = static_cast<int>(hits); }
    if (L.seen_at >= 0 && r.stop_hit && L.hit_at < 0 && hits > L.hits_at_seen) L.hit_at = raw;
}
bool run_over() {
    const Run& r = L.runs[L.cur];
    const int raw = static_cast<int>(L.rec.size()) - 1;
    if (raw + 1 >= r.frames + kMargin) return true;
    if (r.stop_ids.empty() || L.seen_at < 0) return false;
    if (!r.stop_hit) return raw >= L.seen_at + 3;
    if (L.hit_at >= 0) return raw >= L.hit_at + 3;
    if (L.hits_at_seen > 0 && L.rec.back()[1] == 0) return true;            // the combo dropped
    return raw >= L.seen_at + 300;                                          // a hit that never comes
}

void tick(Obj* /*m*/, const Trial& t) {
    cur_trial = &t;
    trial_seen = frame;
    busy_flag.store(L.st != St::Idle || frame < quiet_until);
    if (L.st == St::Idle && frame < quiet_until) {           // a clear a try made: back to the fight, not to its result
        if (const int ph = phase_index(); ph == 11 || ph == 12) change_phase(kPhaseFight);
    }
    switch (L.st) {
    case St::Idle:
        if (frame % 10 == 0) read_request(t);
        return;
    case St::Leave:                            // the fight first: the example starts again from its first frame
        stop_player();
        // a fresh round for every try (seen 14/11: after a try that cleared the trial, nothing played any more until
        // the round was set up again; phase 1 does it in a few frames: 1 -> 2 -> 3 -> 4)
        change_phase(kPhaseSetting);
        L.st = St::Settle;
        L.wait = 0;
        L.started = 0;
        return;
    case St::Settle: {                         // both fighters back to neutral (a super's cinematic, a juggle: seen
                                               // 14/11, a try started inside the last one's super), 10 frames in a row
        Obj* pa = player(0);
        Obj* pd = player(1);
        const bool calm = phase_index() == kPhaseFight && pa && pd && act_of(pa) < 100 && inum(pa, "combo_cnt", 0) == 0 &&
                          inum(pd, "combo_cnt", 0) == 0;
        L.wait = calm ? L.wait + 1 : 0;
        if (L.wait >= 10 || ++L.started > 900) { L.st = St::Arm; L.wait = 3; }
        return;
    }
    case St::Arm:
        if (--L.wait > 0) return;
        if (!lay(L.runs[L.cur])) { finish(false, "the example buffer could not be written"); return; }
        example_flag(true);
        if (!change_phase(kPhaseExample)) { finish(false, "the example could not be started"); return; }
        L.st = St::Start;
        L.started = 0;
        return;
    case St::Start: {
        if (phase_index() != kPhaseExample) { if (++L.started > 120) finish(false, "the example did not start"); return; }
        if (!lay_player(L.runs[L.cur])) { finish(false, "the example player could not be written"); return; }
        const auto c = battle_clock();
        if (!c) return;
        L.clock0 = L.last_clock = static_cast<int64_t>(*c);
        L.rec.clear();
        L.seen_at = L.hit_at = -1;
        L.st = St::Record;
        return;
    }
    case St::Record: {
        const auto c = battle_clock();
        if (!c) return;
        const int64_t clock = static_cast<int64_t>(*c);
        if (clock < L.last_clock || phase_index() != kPhaseExample) {   // the example ended or started over
            L.results.push_back({ { "frames", L.rec } });
        } else {
            Obj* pa = player(0);
            Obj* pd = player(1);
            const int64_t act = pa ? act_of(pa) : -1;
            const int64_t hits = pa ? inum(pa, "combo_cnt", 0) : 0;
            const int64_t dmg = static_cast<int64_t>(combo_damage(pa, pd));
            for (int64_t k = L.last_clock; k < clock && !run_over(); ++k) sample(act, hits, dmg);
            L.last_clock = clock;
            if (!run_over()) return;
            L.results.push_back({ { "frames", L.rec } });
        }
        if (++L.cur >= L.runs.size()) { finish(true, ""); return; }
        L.st = St::Leave;
        return;
    }
    }
}

// ComboTrialManager.NextStage (pre): skipped while the lab plays tries. A try that performs the whole recipe clears the
// trial as if the player had (seen 14/11, IsExamplePlaying up or not), and the automatic progression then moved to the
// next trial, out from under the lab. Outside the lab's work: passed through untouched.
int pre_next_stage(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    return busy_flag.load() ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_next_stage(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (busy_flag.load() && ret) *ret = nullptr;              // (skipped: "no next stage")
}

// Every frame, from the mode: the status file, twice a second
void status_tick() {
    if (frame < status_at) return;
    status_at = frame + 30;
    write_status();
}

}  // namespace lab
