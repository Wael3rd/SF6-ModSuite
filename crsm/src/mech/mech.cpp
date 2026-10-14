#include "mech.hpp"

#include <atomic>
#include <cstdio>
#include <deque>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "mech/hooks.hpp"
#include "model/luajson.hpp"
#include "model/transfer.hpp"
#include "ui/logic.hpp"
#include "ui/ui.hpp"
#include "perf.hpp"

namespace mech {
namespace {

namespace gm = game::gm;

std::recursive_mutex g_lock;
std::mutex g_state_mx;
State g_state;
std::deque<std::string> g_tickers;                    // under g_state_mx
int (*g_label_fns[2])() = { nullptr, nullptr };        // 0 = record, 1 = replay
std::atomic<uint64_t> g_last_poll{0};                  // core frame of the last frame_poll

// ---- the state gate (frame thread, except the atomics) ----
constexpr int32_t kNoFlow = INT32_MIN;
constexpr uint64_t kTdataEvery = 60;                    // _tData looked for every 60 frames after a flow change, until there
constexpr int kMenuWatch = 120;                         // frames of pause polls after menu_event()
std::atomic<int32_t> g_flow{kNoFlow};                   // the core's flow (its flow callback); kNoFlow = none seen yet
std::atomic<uint64_t> g_flow_changes{0};                // +1 per flow callback
std::atomic<bool> g_menu_evt{false};                    // menu_event()
std::atomic<uint64_t> g_events{0};
uint64_t g_flow_seen = 0;
bool g_tdata = false;                                   // _tData seen since the flow entered 3 / 9
uint64_t g_tdata_next = 0;                              // the frame of the next look
int g_watch = 0;                                        // pause polls left after a menu event
bool g_prev_up = false, g_prev_training = false;
uint64_t g_pause_frame = UINT64_MAX;                    // the frame of g_pause (this frame's poll or live read)
Pause g_pause;
std::atomic<uint64_t> g_polls{0}, g_live_reads{0}, g_tdata_reads{0};   // status

void on_flow_change(int32_t, int32_t now) {
    g_flow.store(now);
    g_flow_changes.fetch_add(1);
}

// The pause bit read in place: no game call a frame while something waits on the pause (the menu up: 0 calls a frame).
// app.PauseManager is asked once and held (add_ref), _CurrentPauseTypeBit's address found once (gm::int_field), then the
// bit is read from memory. The singleton is asked again 1 frame in 30 and at each mech event (a scene change could
// replace it: a stale one is read at most 30 frames). The core's option window only when the bit is clear: up() = bit ||
// window, the same answer (State.window is read nowhere else).
constexpr uint64_t kPmCheckEvery = 30;
gm::MO* g_pm = nullptr;                                 // app.PauseManager, held
gm::IntField g_pm_bit;                                  // its _CurrentPauseTypeBit
uint64_t g_pm_next = 0;                                 // the frame of the next look at the singleton
uint64_t g_pm_events = UINT64_MAX;
std::atomic<uint64_t> g_pm_checks{0};                   // status

void pm_refresh() {
    g_pm_checks.fetch_add(1, std::memory_order_relaxed);
    gm::MO* pm = gm::singleton("app.PauseManager");
    if (pm && pm == g_pm && g_pm_bit.p) return;
    if (g_pm) g_pm->release();
    g_pm = nullptr;
    g_pm_bit = {};
    if (pm && gm::int_field(pm, "_CurrentPauseTypeBit", &g_pm_bit)) { pm->add_ref(); g_pm = pm; }
}

// The pause bit (GameState.in_pause_menu: not 64 / 2112) and the core's option window, read now.
Pause read_pause() {
    Pause p;
    const uint64_t fr = cmn::frame(), ev = g_events.load();
    if (fr >= g_pm_next || ev != g_pm_events) {
        g_pm_next = fr + kPmCheckEvery;
        g_pm_events = ev;
        pm_refresh();
    }
    int64_t bit = 64;
    if (g_pm && gm::read_int(g_pm_bit, &bit)) p.bit = bit != 64 && bit != 2112;
    if (!p.bit) {
        const SF6CoreAPI* api = cmn::core();
        p.window = api && SF6CORE_HAS(api, opt_window_open) && perf::counted(api)->opt_window_open() != 0;
    }
    return p;
}

// Who waits on the pause now (a menu edge in progress: the UI's screens, the 30-entry arrays installed or a hand-back
// waiting for the unpause): the poll runs only then. (The record watch and the import's allocation read it themselves,
// pause_now(), on the frames they use it.)
bool pause_needed() {
    if (ui::needs_pause()) return true;
    std::lock_guard lk(g_lock);
    return replay::needs_pause() || reversal::needs_pause();
}

// A layer settings.json handed to the DLL at the start (owned now, or handed back after faults: its deferred clean-up
// still needs the pause state). None: no work at all.
bool any_layer() {
    for (cmn::Feature f : { cmn::Feature::Files, cmn::Feature::Replay, cmn::Feature::Reversal, cmn::Feature::Record })
        if (cmn::owns(f) || (cmn::requested(f) && cmn::tripped(f))) return true;
    return false;
}

// The loader's TrainingModeActive (flow 3 / 9 and _tData) and GameState's pause (bit not 64 / 2112, + the option window):
// see State in mech.hpp for when each is read.
void refresh_state(uint64_t frame) {
    State s;
    s.frame = frame;
    // the flow: the core's flow callback (one read of get_flow_id before the first one, or every frame with a core that
    // did not take the callback)
    const SF6CoreAPI* api = cmn::core();
    int32_t flow = g_flow.load();
    if (flow == kNoFlow || !cmn::flow_events()) {
        const int32_t now = (api && api->get_flow_id) ? perf::counted(api)->get_flow_id() : 0;
        if (!cmn::flow_events()) {
            if (now != flow) on_flow_change(flow, now);
        } else {
            int32_t expected = kNoFlow;                        // (a flow callback that came meanwhile wins)
            g_flow.compare_exchange_strong(expected, now);
        }
        flow = g_flow.load();
    }
    bool event = false;
    if (const uint64_t fc = g_flow_changes.load(); fc != g_flow_seen) {
        g_flow_seen = fc;
        g_tdata = false;
        g_tdata_next = frame;                                  // looked for at once
        event = true;
    }
    const bool tflow = flow == 3 || flow == 9;
    if (tflow && !g_tdata && frame >= g_tdata_next) {
        g_tdata_reads.fetch_add(1, std::memory_order_relaxed);
        gm::MO* tm = gm::singleton("app.training.TrainingManager");
        g_tdata = tm && gm::ref(tm, "_tData");
        if (!g_tdata) g_tdata_next = frame + kTdataEvery;
    }
    s.training = tflow && g_tdata;
    if (s.training != g_prev_training) { g_prev_training = s.training; event = true; }
    // the pause: only while someone waits on it
    if (g_menu_evt.exchange(false)) g_watch = kMenuWatch;
    const bool need = g_watch > 0 || pause_needed();
    if (g_watch > 0) --g_watch;
    if (need) {
        const Pause p = pause_now();
        g_polls.fetch_add(1, std::memory_order_relaxed);
        s.paused = p.bit;
        s.window = p.window;
        s.polled = true;
        if (g_prev_up && !p.up()) event = true;                // a menu close
        g_prev_up = p.up();
    } else {
        g_prev_up = false;
    }
    if (event) g_events.fetch_add(1);
    std::lock_guard lk(g_state_mx);
    g_state = s;
}

void frame_fn(uint64_t frame) {
    if (!any_layer()) return;                         // R38: nothing at all while every layer is the Lua's
    try { refresh_state(frame); } catch (...) {}
}

// frame_poll(): owners mask, HUD slot label (0 = none), one pending ticker (nil = none). The Lua calls it once per frame.
int32_t SF6CORE_CALL lua_frame_poll(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    cmn::note_lua_call();                             // a CRSM Lua runs (P6: the DLL never claims the UI by itself then)
    g_last_poll.store(cmn::frame());
    br::Out o{ r, max };
    o.i64((int64_t)cmn::owners_mask());
    int label = 0;
    for (auto fn : g_label_fns) if (!label && fn) label = fn();
    o.i64(label);
    std::string t;
    if (ui::logic::ticker_sink(ui::active(), true) == ui::logic::TickerSink::Lua) {   // else the DLL's own UI shows them (P4)
        std::lock_guard lk(g_state_mx);
        if (!g_tickers.empty()) { t = g_tickers.front(); g_tickers.pop_front(); }
    }
    if (t.empty()) o.nil(); else o.str(t);
    return o.done();
} catch (...) { return -1; }

}  // namespace

std::recursive_mutex& lock() { return g_lock; }

State state() {
    std::lock_guard lk(g_state_mx);
    return g_state;
}

Pause pause_now() {
    const uint64_t fr = cmn::frame();
    if (g_pause_frame != fr) {
        g_pause = read_pause();
        g_pause_frame = fr;
        g_live_reads.fetch_add(1, std::memory_order_relaxed);
    }
    return g_pause;
}

void menu_event() { g_menu_evt.store(true); }
uint64_t events() { return g_events.load(); }

namespace {
std::string state_status() {
    const State s = state();
    char b[240];
    std::snprintf(b, sizeof(b), "training %d (flow %d), pause reads %llu (in place; PauseManager looks %llu), frames polled %llu, _tData looks %llu, events %llu",
                  s.training ? 1 : 0, (int)g_flow.load(), (unsigned long long)g_live_reads.load(), (unsigned long long)g_pm_checks.load(),
                  (unsigned long long)g_polls.load(), (unsigned long long)g_tdata_reads.load(), (unsigned long long)g_events.load());
    return b;
}
}  // namespace

void push_ticker(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard lk(g_state_mx);
    if (g_tickers.size() < 20) g_tickers.push_back(text);
}

bool pop_ticker(std::string* text) {
    std::lock_guard lk(g_state_mx);
    if (g_tickers.empty()) return false;
    *text = g_tickers.front();
    g_tickers.pop_front();
    return true;
}

bool lua_alive() { return ui::active() || cmn::frame() - g_last_poll.load() < 120; }

int hud_label() {
    int label = 0;
    for (auto fn : g_label_fns) if (!label && fn) label = fn();
    return label;
}

void set_label_source(int which, int (*fn)()) { if (which >= 0 && which < 2) g_label_fns[which] = fn; }

void init() {
    cmn::add_frame_fn(frame_fn, "mech_state");                     // first: the state the layers read in their own frame hooks
    cmn::add_flow_fn(on_flow_change);
    cmn::bridge_fn("frame_poll", lua_frame_poll);
    files::init();
    replay::init();
    reversal::init();
    record::init();
}

void start() {
    files::start();
    replay::start();
    reversal::start();
    record::start();
    std::vector<hooks::Spec> specs;                   // the game hooks of the native layers, R18-checked in one pass
    replay::hook_specs(specs);
    reversal::hook_specs(specs);
    ui::hook_specs(specs);                            // P4: the UI's hooks (owner "ui" native only), same R18 pass
    hooks::install(specs);
    cmn::add_status("hooks", hooks::status);
    cmn::add_status("refs", gm::ref_status);           // reference writes; "into a local target" stays 0 (gm::set_ref)
    cmn::add_status("state", state_status);
}

// ---- bridge helpers ---------------------------------------------------------------------------------------------------
namespace br {

const char* str(const SF6CoreValue* a, uint32_t n, uint32_t i) {
    return (i < n && a[i].type == SF6CORE_VAL_STRING && a[i].s) ? a[i].s : "";
}
bool is_str(const SF6CoreValue* a, uint32_t n, uint32_t i) { return i < n && a[i].type == SF6CORE_VAL_STRING && a[i].s; }

int64_t i64(const SF6CoreValue* a, uint32_t n, uint32_t i, int64_t def) {
    if (i >= n) return def;
    if (a[i].type == SF6CORE_VAL_INT) return a[i].i;
    if (a[i].type == SF6CORE_VAL_DOUBLE && a[i].d == (double)(int64_t)a[i].d) return (int64_t)a[i].d;
    return def;
}

bool boolean(const SF6CoreValue* a, uint32_t n, uint32_t i) {
    if (i >= n) return false;
    if (a[i].type == SF6CORE_VAL_NIL) return false;
    if (a[i].type == SF6CORE_VAL_BOOL) return a[i].b != 0;
    return true;
}

json value(const SF6CoreValue* a, uint32_t n, uint32_t i) {
    if (i >= n) return json();
    switch (a[i].type) {
        case SF6CORE_VAL_BOOL: return json(a[i].b != 0);
        case SF6CORE_VAL_INT: return json(a[i].i);
        case SF6CORE_VAL_DOUBLE: return json(a[i].d);
        case SF6CORE_VAL_STRING: return json(std::string(a[i].s ? a[i].s : ""));
        default: return json();
    }
}

json from_lua(const char* text) { return model::transfer::from_lua(text ? text : ""); }
std::string to_lua(const json& v) { return model::transfer::to_lua(v); }

void Out::nil() { if (n < max) { r[n] = SF6CoreValue{}; r[n].type = SF6CORE_VAL_NIL; ++n; } }
void Out::boolean(bool v) { if (n < max) { r[n] = SF6CoreValue{}; r[n].type = SF6CORE_VAL_BOOL; r[n].b = v ? 1 : 0; ++n; } }
void Out::i64(int64_t v) { if (n < max) { r[n] = SF6CoreValue{}; r[n].type = SF6CORE_VAL_INT; r[n].i = v; ++n; } }
void Out::str(const std::string& v) {
    thread_local std::string bufs[16];
    if (n >= max || n >= 16) return;
    bufs[n] = v;
    r[n] = SF6CoreValue{};
    r[n].type = SF6CORE_VAL_STRING;
    r[n].s = bufs[n].c_str();
    ++n;
}
void Out::val(const json& v) {
    if (v.is_null()) nil();
    else if (v.is_boolean()) boolean(v.get<bool>());
    else if (v.is_number_integer()) i64(v.get<int64_t>());
    else if (v.is_number_float()) { if (n < max) { r[n] = SF6CoreValue{}; r[n].type = SF6CORE_VAL_DOUBLE; r[n].d = v.get<double>(); ++n; } }
    else if (v.is_string()) str(v.get<std::string>());
    else str(to_lua(v));
}

}  // namespace br
}  // namespace mech
