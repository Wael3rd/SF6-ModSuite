// SF6 Slots — native REFramework plugin: entry points.
//
// One LateUpdateBehavior callback (REFramework offers no way to remove it). With no event pending
// and no work in progress it returns at once, having read nothing but this plugin's own memory.
#include "slots.hpp"
#include "slots_ui.hpp"

namespace slots {

std::atomic<bool> g_closing{false};

namespace {

bool g_hooks = false;
bool g_costumes_busy = true, g_stages_busy = true;   // checked on the first frames

// Closing guard state (not cancellable for WM_DESTROY / WM_NCDESTROY / WM_ENDSESSION)
uint32_t s_close_frame = 0;
bool     s_close_final = false;

bool on_message(void* /*hwnd*/, unsigned int msg,
                unsigned long long wparam, long long /*lparam*/) {
    bool final_msg = false;
    const char* name = nullptr;
    switch (msg) {
    case 0x0010: name = "WM_CLOSE";     break;
    case 0x0002: name = "WM_DESTROY";   final_msg = true; break;
    case 0x0082: name = "WM_NCDESTROY"; final_msg = true; break;
    case 0x0016:
        if (wparam == 0) return true;
        name = "WM_ENDSESSION"; final_msg = true; break;
    default: return true;
    }
    if (!g_closing.exchange(true, std::memory_order_seq_cst)) {
        s_close_frame = g_frame;
        s_close_final = final_msg;
        logf("closing: the game window got %s, the plugin stops", name);
    } else if (final_msg && !s_close_final) {
        s_close_final = true;
    }
    return true;
}

void on_late_update() {
    ++g_frame;

    // Closing guard: skip all game work. Cancel WM_CLOSE after 30 s of frames.
    if (g_closing.load(std::memory_order_relaxed)) {
        if (!s_close_final && s_close_frame > 0 &&
            (g_frame - s_close_frame) > 60u * 30) {
            g_closing.store(false, std::memory_order_relaxed);
            s_close_frame = 0;
            logf("closing: cancelled, the plugin resumes");
        } else {
            return;
        }
    }

    uint32_t ev = g_events.exchange(0, std::memory_order_acq_rel);
    if (!ev && !g_costumes_busy && !g_stages_busy) return;
    try {
        if (!g_hooks) {
            g_hooks = true;
            ui::log_path();
            costumes::install_hooks();
            stages::install_hooks();
        }
        if (ev & EV_DIAG) diag_flush();
        costumes::tick(ev);
        stages::tick(ev);
        g_costumes_busy = costumes::busy();
        g_stages_busy = stages::busy();
    } catch (const std::exception& e) {
        logf("EXCEPTION: %s", e.what());
    } catch (...) {
        logf("EXCEPTION (unknown)");
    }
}

} // namespace
} // namespace slots

extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1;
    v->minor = 10;    // API of REFramework 1.5.8
    v->patch = 0;
    v->game_name = nullptr;
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->functions->on_pre_application_entry) return false;
    try { reframework::API::initialize(param); } catch (...) { return false; }
    slots::log_open();
    slots::logf("SF6 Slots native plugin loaded");
    slots::ui::init();
    // The Lua scripts this plugin replaces, left by an update made by hand: both would handle the
    // same menus. Renamed, not deleted.
    for (const char* name : { "SF6_CostumeSlots.lua", "SF6_StageSlots.lua" }) {
        std::string p = std::string("reframework/autorun/") + name;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        bool ok = MoveFileExA(p.c_str(), (p + ".old").c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        slots::logf("old script %s %s", name, ok ? "renamed to .lua.old (replaced by this plugin)" : "could not be renamed");
    }
    try {
        slots::costumes::init();
        slots::stages::init();
    } catch (...) {
        slots::logf("EXCEPTION at init");
    }
    param->functions->on_pre_application_entry("LateUpdateBehavior", slots::on_late_update);
    // Register window message callback (closing guard)
    if (param->functions->on_message) {
        param->functions->on_message(slots::on_message);
        slots::logf("registered on_message (closing guard)");
    } else {
        slots::logf("on_message not available (closing guard disabled)");
    }
    return true;
}
