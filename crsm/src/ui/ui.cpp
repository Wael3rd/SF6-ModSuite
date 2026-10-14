// ui — the layer glue of the DLL's own screens (phase P4): the owner, the Lua loader's claim (with the Manager page of P5),
// the standalone claim when no CRSM Lua is installed (P6, standalone.hpp), the frame callback (the LUB work of
// RecordingManagerTab.lua, NativeTicker.lua, NativeLocale.lua: the language probe of ui/lang), the faults and the hand-back.
#include "ui.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

#include "common.hpp"
#include "dialogs.hpp"
#include "dump.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "internal.hpp"
#include "lang.hpp"
#include "manager_page.hpp"
#include "mech/mech.hpp"
#include "standalone.hpp"
#include "perf.hpp"

namespace ui {
namespace {

using F = cmn::Feature;

std::recursive_mutex g_lock;
std::mutex g_claim_mx;                           // ui_claim (the loader's thread; again after a script reset)
std::atomic<bool> g_claimed{false};
std::atomic<bool> g_training{false}, g_paused{false};
std::atomic<uint64_t> g_tick{0};
bool g_hand_back_due = false;                    // windows / reversal: put back once the menu is closed
bool g_dialogs_back_due = false;                 // the dialogs and the Community dialog: at once (the menu input back)
int g_faults = 0;

// ---- standalone (P6, standalone.hpp): the DLL claims the UI itself when no CRSM Lua is installed ----------------------------
bool g_sa_started = false;                       // ui::start ran with the owner ui native (mode read, autorun scanned)
standalone::Mode g_sa_mode = standalone::Mode::Auto;
std::string g_sa_lua_file;                       // the CRSM Lua entry script found in reframework/autorun at the start ("" = none)
bool g_sa_done = false;                          // a terminal decision was taken (frame callback only)
std::atomic<int> g_sa_last{(int)standalone::Decision::Wait};
std::atomic<uint64_t> g_sa_run{0};               // frame callbacks in a row in training
std::atomic<bool> g_by_dll{false};               // the UI was claimed by the DLL itself (no Lua loader)
std::atomic<uint64_t> g_sa_frame{0};             // the frame of that claim

// The hand-over, from the loader's ui_claim (by_dll false) or from the standalone decision on the frame callback (true).
// The Manager page (P5) is asked of the core here, not before: a loader that keeps the Lua UI keeps the strip for the
// Lua's own engine. A page the core refuses = ui declined = false. Once claimed, every later call answers true.
bool do_claim(bool by_dll) {
    std::lock_guard lk(g_claim_mx);
    bool on = cmn::owns(F::Ui) && !cmn::tripped(F::Ui);
    if (on && !g_claimed.load()) {
        on = ui::manager::claim();
        if (on) {
            g_by_dll.store(by_dll);
            g_claimed.store(true);
            if (by_dll) {
                g_sa_frame.store(g_tick.load());
                cmn::log_info("[CRSMNative] ui: standalone: no CRSM Lua script, the DLL takes the UI itself (%llu frames in training)",
                              (unsigned long long)g_sa_run.load());
            } else {
                cmn::log_info("[CRSMNative] ui: the Lua loader handed the UI over (the Lua UI modules are not loaded)");
            }
        }
    }
    return on;
}

// ui_claim(): the Lua loader asks once at load whether the DLL runs the UI (SF6_RecordingManager.lua). true = it does not
// load the Lua UI modules and the DLL's screens start now (or already run: a standalone claim came first).
int32_t SF6CORE_CALL lua_ui_claim(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    cmn::note_lua_call();                        // a CRSM Lua runs: the DLL never claims by itself from now on
    if (max < 1) return 0;
    const bool on = do_claim(false);
    r[0] = SF6CoreValue{};
    r[0].type = SF6CORE_VAL_BOOL;
    r[0].b = on ? 1 : 0;
    return 1;
} catch (...) { return -1; }

void run_dialogs_hand_back() {                   // a text dialog up when the UI stopped must not leave the menu's input off
    std::lock_guard lk(g_lock);
    try { dlg::hand_back(); } catch (...) {}
    try { com::hand_back(); } catch (...) {}
}

void run_hand_back() {
    std::lock_guard lk(g_lock);
    try { reversal_ui::hand_back(); } catch (...) {}
    try { windows::hand_back(); } catch (...) {}
    try { hud::hand_back(); } catch (...) {}
    cmn::log_warn("[CRSMNative] ui: stopped; the game's own pause menu pages from now on (slot windows put back)");
}

// Before the claim, once per frame callback until a terminal decision: no Lua loader asked, and none will (standalone.hpp).
void standalone_tick(uint64_t fr) {
    if (g_sa_done || !g_sa_started) return;
    const mech::State st = mech::state();        // the layers' state, refreshed every frame (ui native needs every layer)
    const uint64_t run = standalone::next_training_run(g_sa_run.load(), st.training);
    g_sa_run.store(run);
    standalone::Inputs in;
    in.mode = g_sa_mode;
    in.ui_owned = cmn::owns(F::Ui) && !cmn::tripped(F::Ui);
    in.claimed = g_claimed.load();
    in.lua_files = !g_sa_lua_file.empty();
    in.lua_called = cmn::lua_called();
    in.frames = fr;
    in.training_run = run;
    // the menu is read only when the claim would be due otherwise (no game read per frame while waiting)
    const standalone::Decision d = standalone::decide_lazy(in, [] { return mech::pause_now().up(); });
    g_sa_last.store((int)d);
    if (!standalone::terminal(d)) return;
    g_sa_done = true;
    if (d == standalone::Decision::Claim) {
        if (!do_claim(true))
            cmn::log_warn("[CRSMNative] ui: standalone: the claim was refused (the line above says why); the game's own pause menu pages stay");
    } else if (d == standalone::Decision::Foreign) {
        cmn::log_warn("[CRSMNative] standalone: reframework/autorun/%s is installed but never called the DLL (a CRSM Lua that does not "
                      "drive SF6_CRSMNative.dll: v1.x, the Training Suite, the REFramework display switch, or no core Lua bridge). One "
                      "engine: the DLL gives its hooked layers back for this session. Remove that script, or use the Hybrid package",
                      g_sa_lua_file.c_str());
        for (F f : { F::Ui, F::Record, F::Reversal, F::Replay, F::Files }) cmn::decline(f, "a CRSM Lua that does not drive the DLL is installed");
    }
    cmn::refresh_status();
}

// The screens waiting on the pause (under g_lock): the menu seen open by the windows or the Manager page, a dialog or the
// Community dialog (dialogs::busy), the picker.
bool screens_need_pause() {
    return windows::needs_pause() || ui::manager::needs_pause() || ui::dialogs::busy() || reversal_ui::needs_pause();
}

void frame_fn(uint64_t fr) {
    g_tick.store(fr);
    if (!cmn::owns(F::Ui)) {                     // stopped by faults: the dialogs at once, the slot windows once the menu is closed
        if (g_dialogs_back_due) { g_dialogs_back_due = false; run_dialogs_hand_back(); }
        if (g_hand_back_due && !mech::pause_now().bit) { g_hand_back_due = false; run_hand_back(); }
        return;
    }
    if (!g_claimed.load() || cmn::closing()) {
        if (!g_claimed.load() && !cmn::closing()) {
            try { PERF_SUB(ps, "UI:standalone"); standalone_tick(fr); } catch (...) { g_sa_done = true; cmn::journal("ui: standalone decision faulted: no standalone claim"); }
        }
        return;
    }
    std::lock_guard lk(g_lock);
    try {
        const mech::State st = mech::state();
        // the loader's TrainingGamePaused (+ any option window, GameState.lua:74): read only while a screen waits on it
        // (mech polled it then; a screen that started waiting since gets a read now). Otherwise nothing is up for us.
        const bool paused = (st.polled || screens_need_pause()) && mech::pause_now().up();
        g_training.store(st.training);
        g_paused.store(paused);
        // the game's text language: at the start, after each flow change / training edge / menu close, and once a second
        // while the menu is up (Options > Language there; NativeLocale.lua:112-116)
        { PERF_SUB(p1, "UI:lang"); ui::lang::frame(fr, paused, mech::events()); }
        { PERF_SUB(p2, "UI:dialogs"); dlg::frame(paused); }  // finished text dialogs, queued requests, the message box poll (paused)
        { PERF_SUB(p3, "UI:ticker"); ticker::frame(); }
        { PERF_SUB(p4, "UI:community_dialog"); com::frame(); }
        { PERF_SUB(p5, "UI:windows"); windows::frame(st.training, paused); }
        { PERF_SUB(p6, "UI:reversal_ui"); reversal_ui::frame(paused); }
        { PERF_SUB(p7, "UI:hud"); hud::frame(); }
        { PERF_SUB(p8, "UI:dump"); dump::frame(paused); }                     // the probe dump (off by default: dump_on.txt)
    } catch (...) {
        fault("frame");
    }
}

void hand_back() { if (g_claimed.load()) { g_dialogs_back_due = true; g_hand_back_due = true; } }   // (cmn: the next frame callback)

std::string standalone_status() {
    if (!g_sa_started) return cmn::owns(F::Ui) ? "not started" : "n/a (ui lua)";
    if (g_by_dll.load())
        return "auto: the DLL took the UI at frame " + std::to_string(g_sa_frame.load()) + " (no CRSM Lua script)";
    if (g_claimed.load()) return std::string(standalone::mode_name(g_sa_mode)) + ": the Lua loader took the UI";
    if (g_sa_mode == standalone::Mode::Off) return "off";
    const std::string lua = "reframework/autorun/" + g_sa_lua_file;
    switch ((standalone::Decision)g_sa_last.load()) {
        case standalone::Decision::Foreign: return "auto: " + lua + " installed but never called the DLL: hooked layers given back";
        case standalone::Decision::WaitLua: return "auto: " + lua + " installed, waiting for its loader's claim";
        case standalone::Decision::LuaCalled: return "auto: a CRSM Lua calls the DLL (its loader claims)";
        case standalone::Decision::NotNative: return "auto: ui not native";
        case standalone::Decision::Claim: return "auto: the claim was refused";
        default: break;
    }
    return "auto: no CRSM Lua script; waiting (training " + std::to_string(g_sa_run.load()) + " / " +
           std::to_string(standalone::kTrainingFrames) + " frames in a row, menu closed, " + std::to_string(standalone::kGraceFrames) +
           " frames after the start at least)";
}

std::string status() {
    std::lock_guard lk(g_lock);
    std::string s = cmn::owns(F::Ui) ? (g_claimed.load() ? (g_by_dll.load() ? "native (standalone: claimed by the DLL)" : "native")
                                                         : "native, NOT claimed yet (the Lua loader's claim, or the standalone one)")
                                     : "lua";
    if (cmn::owns(F::Ui) || cmn::tripped(F::Ui)) {
        s += "; windows: " + windows::status();
        s += "; reversal: " + reversal_ui::status();
        s += "; hud: " + hud::status();
        s += "; dialogs: " + dlg::status();
        s += "; community: " + com::status();
        s += "; ticker: " + ticker::status();
        s += "; " + ui::lang::status();
    }
    return s;
}

}  // namespace

bool needs_pause() {
    if (!cmn::owns(F::Ui)) return g_hand_back_due;   // the slot windows put back at the unpause
    if (!g_claimed.load() || cmn::closing()) return false;
    std::lock_guard lk(g_lock);
    return screens_need_pause();
}

bool active() { return g_claimed.load(std::memory_order_relaxed) && cmn::owns(F::Ui); }
bool standalone() { return g_by_dll.load(std::memory_order_relaxed) && active(); }
std::recursive_mutex& lock() { return g_lock; }
bool training() { return g_training.load(); }
bool paused() { return g_paused.load(); }
uint64_t tick() { return g_tick.load(); }

std::string T(const std::string& key, const std::vector<std::string>& args) { return ui::lang::T(key, args); }

void spec(std::vector<mech::hooks::Spec>& out, const char* type, const char* sig, REFPreHookFn pre, REFPostHookFn post,
          const char* without, bool* ok) {
    mech::hooks::Spec s{ F::Ui, type, sig, pre, post, without, false };
    s.installed = ok;
    out.push_back(s);
}

void fault(const char* where) {
    ++g_faults;
    cmn::fault(F::Ui, where);
}

void init() {
    cmn::set_implemented(F::Ui);
    cmn::on_hand_back(F::Ui, hand_back);
    cmn::add_status("ui", status);
    cmn::add_status("standalone", standalone_status);
    cmn::bridge_fn("ui_claim", lua_ui_claim);
    cmn::add_frame_fn(frame_fn, "ui");
}

void start() {
    if (!cmn::owns(F::Ui)) return;
    ui::lang::load();                            // locale.json, read once here (never from a hook)
    try { dump::tab_methods(); } catch (...) {}  // the game's tab methods (off by default: tab_dump_on.txt)
    std::string note;
    g_sa_mode = standalone::parse_mode(cmn::setting_standalone(), &note);
    if (!note.empty()) cmn::journal("%s", note.c_str());
    g_sa_lua_file = standalone::find_lua([](const std::string& n) {      // plugin start: before any autorun script runs
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path("reframework/autorun") / n, ec);
    });
    g_sa_started = true;
    const char* how = g_sa_mode == standalone::Mode::Off ? "standalone off"
                    : !g_sa_lua_file.empty() ? "standalone auto, a CRSM Lua is installed, its loader claims: reframework/autorun/"
                    : "standalone auto, no CRSM Lua script: the DLL takes the UI itself 2 s into training";
    cmn::log_info("[CRSMNative] ui: native (Record / Replay / Reversal windows over 30, picker of 30, REC HUD, text dialog, message box, "
                  "tickers, Community dialog); waits for the Lua loader's claim (%s%s)", how,
                  g_sa_mode == standalone::Mode::Auto ? g_sa_lua_file.c_str() : "");
}

void hook_specs(std::vector<mech::hooks::Spec>& out) {
    if (!cmn::owns(F::Ui)) return;               // hooks only with the owner native (none by default)
    windows::specs(out);
    reversal_ui::specs(out);
    dlg::specs(out);
    ticker::specs(out);
    com::specs(out);
    hud::specs(out);
}

}  // namespace ui
