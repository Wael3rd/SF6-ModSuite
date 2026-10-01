// SF6 Slots — native REFramework plugin: entry points.
//
// One LateUpdateBehavior callback (REFramework offers no way to remove it). With no event pending
// and no work in progress it returns at once, having read nothing but this plugin's own memory.
#include "slots.hpp"
#include "slots_ui.hpp"

namespace slots {
namespace {

bool g_hooks = false;
bool g_costumes_busy = true, g_stages_busy = true;   // checked on the first frames

void on_late_update() {
    ++g_frame;
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
    return true;
}
