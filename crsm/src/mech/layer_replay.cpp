// mech/replay — the "replay" layer (P3b) in the game: model::replay::Engine on the dummy's slots (mech::bigc), the
// hook on tf_RecordSetting.SettingRecodData (installed only when settings.json gives the layer to the DLL, after the
// R18 check), the menu edges the Lua UI hands over (RMT's CreateViewData pre-hook = open, its pause falling edge =
// close), the HUD label, and a safety net: the 30-entry array never stays in place once training is left or the Lua
// stopped calling (a missed close would let the game save 30 entries).
#include <atomic>
#include <cstdio>
#include <ctime>

#include <windows.h>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "mech/bigc.hpp"
#include "mech/hooks.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/replay.hpp"

namespace mech::replay {
namespace {

namespace gm = game::gm;
using F = cmn::Feature;
using model::replay::Live;

class GamePort final : public model::replay::Port {
public:
    bool live(int k0, Live* out) override {
        bool a = false, v = false;
        int64_t w = 1;
        if (!bigc::live(k0, &a, &v, &w)) return false;
        out->present = true; out->active = a; out->valid = v; out->weight = w;
        return true;
    }
    void set_active(int k0, bool on) override { bigc::set_active(k0, on); }
    std::optional<json> read8() override {
        std::string err;
        return files::engine().read_slot(model::replay::kWorkSlot, &err);
    }
    void write8(const json& e) override { files::engine().write_slot(e); }
    bool install() override { return bigc::install(); }
    bool installed() override { return bigc::installed(); }
    bool entry(int n, bool* a, int64_t* w) override { return bigc::entry(n, a, w); }
    void entry_set_active(int n, bool on) override { bigc::entry_set_active(n, on); }
    void uninstall() override { bigc::uninstall(); }
    void sync(int n) override { bigc::sync(n); }
};

GamePort g_port;
model::replay::Engine g_eng;
model::draw::MtRng g_rng((uint64_t)std::time(nullptr));     // math.randomseed: the sequence differs, the distribution not
std::optional<std::string> g_char;
uint64_t g_draws = 0, g_opens = 0, g_closes = 0, g_nets = 0;
int g_label = 0;
bool g_hand_back_due = false;                              // handed back while the pause menu was up: done at the unpause
thread_local model::replay::Draw t_draw;                   // pre -> post of one SettingRecodData call (R29)

// The HUD label (frame callback): the game is asked whether it replays slot 8 (RecordFunc.IsReplayNow, _gData.SlotID) only
// while a replay can be running with a virtual in slot 8: for kPlayGrace frames after SettingRecodData (the game's draw at
// each replay start and loop: our hook), for kEventGrace frames after a mech event (flow change, training edge, menu close)
// or a change of what slot 8 holds, and as long as the last read said it replays. No virtual in slot 8, or no replay
// started: no game read at all. The RecordFunc is taken again at each (re)start of the watch, held (add_ref) meanwhile;
// while it replays, one call a frame (IsReplayNow): SlotID is read at the start of a replay or after a draw only.
constexpr int kPlayGrace = 600, kEventGrace = 120;
std::atomic<bool> g_play_arm{false};                       // SettingRecodData ran (any thread)
uint64_t g_play_events = UINT64_MAX;
int g_play_loaded = 0;
int g_play_grace = 0;
bool g_playing = false;
int64_t g_slot = -1;                                       // _gData.SlotID of the running replay (read once per draw)
gm::MO* g_rf = nullptr;                                    // TrainingManager.get_RecordFunc(), held while the watch runs
reframework::API::Method* g_is_replay = nullptr;           // its IsReplayNow (found once)
uint64_t g_label_reads = 0;

void rf_drop() {
    if (g_rf) g_rf->release();
    g_rf = nullptr;
}

bool rf_take() {
    rf_drop();
    gm::MO* rf = gm::call_obj(gm::singleton("app.training.TrainingManager"), "get_RecordFunc");
    if (!rf) return false;
    rf->add_ref();
    g_rf = rf;
    if (!g_is_replay) g_is_replay = gm::method(rf->get_type_definition(), "IsReplayNow");
    return true;
}

// The label of this frame: 0 unless the game replays slot 8 while a virtual rides it (model::replay::Engine::label).
int label_tick(bool training) {
    const uint64_t ev = mech::events();
    const int loaded = g_eng.loaded();
    const bool armed = g_play_arm.exchange(false);
    if (armed || ev != g_play_events || loaded != g_play_loaded) {
        g_slot = -1;                                       // a new draw / state: SlotID read again
        const int grace = armed ? kPlayGrace : kEventGrace;
        if (g_play_grace < grace) g_play_grace = grace;
        g_play_events = ev;
        g_play_loaded = loaded;
        if (loaded && training) rf_take();
    }
    if (!loaded || !training || (g_play_grace <= 0 && !g_playing)) {
        g_playing = false;
        if (g_play_grace > 0) --g_play_grace;
        if (g_rf && g_play_grace <= 0) rf_drop();
        return 0;
    }
    if (g_play_grace > 0) --g_play_grace;
    if (!g_rf && !rf_take()) { g_playing = false; return 0; }
    ++g_label_reads;
    bool now = false;
    int64_t slot = -1;
    reframework::InvokeRet r{};
    if (g_is_replay && gm::call(g_rf, g_is_replay, {}, &r)) now = r.byte != 0;
    if (now && (!g_playing || g_slot < 0)) {               // the slot played changes only with a draw (SettingRecodData)
        g_slot = -1;
        gm::get_int(gm::ref(g_rf, "_gData"), "SlotID", &g_slot);
    }
    if (now) slot = g_slot;
    g_playing = now;
    return g_eng.label(now, slot);
}

// vslots_load's vplay reset on a new character (RMT:469-470)
void follow_char() {
    files::engine().vslots();
    const auto c = files::engine().vslots_char();
    if (c != g_char) { if (g_char) g_eng.char_changed(); g_char = c; }
    bigc::check_version();
}

void close_now() {
    g_eng.menu_close(g_port, []() -> const model::vslots::VSlots& { return files::engine().vslots(); },
                     [](const json& patch) { files::engine().vslots_active_weight(patch); });
}

// ---- the hook: tf_RecordSetting.SettingRecodData (RMT:1246-1275) -------------------------------------------------------
void pre_body() {
    g_play_arm.store(true, std::memory_order_relaxed);      // a replay starts (or loops): the HUD label is watched
    std::lock_guard lk(mech::lock());
    try {
        t_draw = model::replay::Draw{};
        follow_char();
        if (g_eng.pre_draw(g_port, files::engine().vslots(), g_rng, &t_draw)) {
            ++g_draws;
            cmn::journal("replay: SettingRecodData: our draw %d%s", t_draw.n, t_draw.has_vs ? " (menu open)" : "");
        }
    } catch (...) {
        t_draw = model::replay::Draw{};
        cmn::fault(F::Replay, "SettingRecodData pre");
    }
}
void post_body() {
    std::lock_guard lk(mech::lock());
    try {
        g_eng.post_draw(g_port, t_draw);
    } catch (...) {
        cmn::fault(F::Replay, "SettingRecodData post");
    }
    t_draw = model::replay::Draw{};
}
int pre_hook(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !cmn::owns(F::Replay)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { pre_body(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Replay, "SettingRecodData pre (SEH)"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_hook(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing()) return;
    __try { post_body(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Replay, "SettingRecodData post (SEH)"); }
}

// ---- bridge functions (RMT's shims) ------------------------------------------------------------------------------------
#define REPLAY_FN(name) int32_t SF6CORE_CALL name(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max)
#define REPLAY_TRY std::lock_guard<std::recursive_mutex> lk_(mech::lock()); br::Out o{ r, max }; (void)a; (void)n; try {
#define REPLAY_END(what) } catch (...) { cmn::fault(F::Replay, what); return o.done(); } return o.done();

// replay_menu_open(): on_menu_open's replay part (after the other layers'); the installed array's address (0 = none)
REPLAY_FN(f_menu_open) {
    REPLAY_TRY
    follow_char();
    g_eng.menu_open(g_port, files::engine().vslots());
    ++g_opens;
    o.i64((int64_t)(uintptr_t)bigc::installed_array());
    REPLAY_END("menu_open")
}
// replay_menu_close(): on_menu_close's replay part (read-back, the game's 8 back, the longest virtual into slot 8)
REPLAY_FN(f_menu_close) {
    REPLAY_TRY
    follow_char();
    close_now();
    ++g_closes;
    cmn::journal("replay: menu closed: slot 8 holds %d, forced %d", g_eng.loaded(), g_eng.forced8() ? 1 : 0);
    REPLAY_END("menu_close")
}
// replay_forget(n): RemoveRecord on virtual row n, after files_vslot_delete
REPLAY_FN(f_forget) { REPLAY_TRY g_eng.forget(g_port, (int)br::i64(a, n, 0, 0)); REPLAY_END("forget") }
// replay_unload(): the Lua's record arm, before its snapshot of slot 8
REPLAY_FN(f_unload) { REPLAY_TRY g_eng.unload_if_loaded(g_port); REPLAY_END("unload") }

void on_import() {
    if (!cmn::owns(F::Replay)) return;
    std::lock_guard lk(mech::lock());
    try { g_eng.imported(g_port); } catch (...) { cmn::fault(F::Replay, "import"); }
}

int label() { return cmn::owns(F::Replay) ? g_label : 0; }

void frame(uint64_t) {
    if (g_hand_back_due) {                                 // (the layer is the Lua's now: this runs whatever owns() says)
        std::lock_guard lk(mech::lock());
        if (!mech::pause_now().bit) {
            g_hand_back_due = false;
            try { g_eng.hand_back(g_port); } catch (...) {}
            cmn::journal("replay: hand-back done at the unpause");
        }
        return;
    }
    if (!cmn::owns(F::Replay)) return;
    std::lock_guard lk(mech::lock());
    try {
        follow_char();
        const State st = mech::state();
        // the close never came: never 30 entries in a fight. Not while the pause menu is up: its Replay rows point past 8
        // (installed: mech polls the pause, needs_pause)
        if (bigc::installed() && (!st.training || (!mech::pause_now().bit && !mech::lua_alive()))) {
            ++g_nets;
            cmn::log_warn("[CRSMNative] replay: the 30-entry array was still in place (%s): closed by the DLL",
                          st.training ? "the Lua stopped calling" : "training left");
            close_now();
        }
        g_label = 0;                                       // (0 if the read below throws)
        g_label = label_tick(st.training);
        bigc::prebuild_tick(st.training);                  // the 30-slot array built ahead, menu closed
    } catch (...) {
        cmn::fault(F::Replay, "frame");
    }
}

void hand_back() {
    std::lock_guard lk(mech::lock());
    rf_drop();
    if (bigc::installed() && mech::pause_now().bit) {       // the menu's Replay rows point past 8: the game's 8 back at the unpause
        g_hand_back_due = true;
        cmn::log_warn("[CRSMNative] replay: back to the Lua; the game's 8 slots put back at the unpause");
        return;
    }
    try { g_eng.hand_back(g_port); } catch (...) {}
    cmn::log_warn("[CRSMNative] replay: back to the Lua (slot 8 restored, the game's 8 slots in place)");
}

std::string status() {
    std::lock_guard lk(mech::lock());
    char b[320];
    std::snprintf(b, sizeof(b), "%s, slot 8 holds %d, forced %d, draws %llu, opens %llu, closes %llu, safety closes %llu, label reads %llu; %s",
                  cmn::owns(F::Replay) ? "native" : "lua", g_eng.loaded(), g_eng.forced8() ? 1 : 0, (unsigned long long)g_draws,
                  (unsigned long long)g_opens, (unsigned long long)g_closes, (unsigned long long)g_nets,
                  (unsigned long long)g_label_reads, bigc::status().c_str());
    return b;
}

}  // namespace

void init() {
    cmn::set_implemented(F::Replay);
    cmn::on_hand_back(F::Replay, hand_back);
    cmn::add_status("replay", status);
    cmn::bridge_fn("replay_menu_open", f_menu_open);
    cmn::bridge_fn("replay_menu_close", f_menu_close);
    cmn::bridge_fn("replay_forget", f_forget);
    cmn::bridge_fn("replay_unload", f_unload);
    cmn::add_frame_fn(frame, "layer_replay");
    files::set_import_listener(on_import);
    mech::set_label_source(1, label);
}

bool needs_pause() { return bigc::installed() || g_hand_back_due; }

void* menu_open() {
    std::lock_guard lk(mech::lock());
    try {
        follow_char();
        g_eng.menu_open(g_port, files::engine().vslots());
        ++g_opens;
        return bigc::installed_array();
    } catch (...) {
        cmn::fault(F::Replay, "menu_open (ui)");
        return nullptr;
    }
}

void menu_close() {
    std::lock_guard lk(mech::lock());
    try {
        follow_char();
        close_now();
        ++g_closes;
        cmn::journal("replay: menu closed: slot 8 holds %d, forced %d", g_eng.loaded(), g_eng.forced8() ? 1 : 0);
    } catch (...) {
        cmn::fault(F::Replay, "menu_close (ui)");
    }
}

void forget(int n) {
    std::lock_guard lk(mech::lock());
    try { g_eng.forget(g_port, n); } catch (...) { cmn::fault(F::Replay, "forget (ui)"); }
}

void unload_for_record() {
    if (!cmn::owns(F::Replay)) return;
    std::lock_guard lk(mech::lock());
    try { g_eng.unload_if_loaded(g_port); } catch (...) { cmn::fault(F::Replay, "unload for record"); }
}

void own8_back() {
    if (!cmn::owns(F::Replay)) return;
    std::lock_guard lk(mech::lock());
    if (g_eng.loaded()) cmn::journal("replay: slot 8 holds its own recording again (record finalize): virtual %d no longer in it", g_eng.loaded());
    g_eng.own8_back();
}

void hook_specs(std::vector<hooks::Spec>& out) {
    out.push_back({ F::Replay, "app.training.tf_RecordSetting", "SettingRecodData", pre_hook, post_hook,
                    "no draw over slots 1-30: the game draws among its 8", true });
}

void start() {
    if (cmn::owns(F::Replay)) cmn::log_info("[CRSMNative] replay: native (draw over 1-30, slot 8 discipline, the Replay page's 30 entries)");
}

}  // namespace mech::replay
