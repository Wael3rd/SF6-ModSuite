// SF6_ColorModNative - SF6 Color Mod, one DLL, client of sf6core_native.dll (core/).
//
// Phase 0: registers with the shared core, records which features it owns, keeps a journal.
// Phase 1, step 1: "editor" = the costume editor's preview patch (engine/live), when settings.json says
// "editor": "native". The Lua editor drives it through the core's Lua bridge (SF6Core.clients["SF6_ColorModNative"]).
// Phase 1, step 2: "engine" = every other colour patch written by the DLL; the Lua still decides what is worn.
// Phase 1, step 3: "decisions" = the DLL also decides (engine/decide): sides, wanted entry, the GetColorList
// entries, the mirror rule, the battle pass, squares. The Lua screens push / pull through the bridge.
// Phase 2, step 2: "vfx_engine" = SF6_MemWriterNative absorbed (vfx/engine); relay, queue and scan through the bridge.
//
// Build: build.bat (MSVC, /std:c++20 /EHa). Output: reframework/plugins/SF6_ColorModNative.dll
// Kill switch: reframework/data/_colormod_off.txt makes the DLL inert at the next start.
// Files: reframework/data/SF6_ColorModNative_data/{settings.json (owners), status.json, debug_log.json}

#include <reframework/API.hpp>

#include "common.hpp"
#include "fighters.hpp"
#include "dialogs/locale.hpp"
#include "engine/decide.hpp"
#include "engine/live.hpp"
#include "screens/rows.hpp"
#include "screens/dipanel.hpp"
#include "screens/tracker.hpp"
#include "screens/guide.hpp"
#include "dialogs/camera.hpp"
#include "dialogs/editor.hpp"
#include "dialogs/options_driver.hpp"
#include "screens/colour_track.hpp"
#include "screens/drive_bridge.hpp"
#include "vfx/drive_cfg.hpp"
#include "vfx/feed.hpp"
#include "vfx/engine.hpp"
#include "vfx/body.hpp"
#include "vfx/mech.hpp"
#include "vfx/tick.hpp"

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    // 1.10, as the core and the Slots Loader: nothing past it is used (07/10: 1.15, copied from older plugins, kept
    // the mod from loading on the public REFramework 1.5.8)
    v->major = 1; v->minor = 10; v->patch = 0; v->game_name = nullptr;
}

namespace {

bool decide_squares(int fighter, int only_side, uint32_t& c0, uint32_t& c1) {
    const auto s = engine::decide::squares(fighter, only_side);
    if (!s) return false;
    c0 = s->first; c1 = s->second;
    return true;
}
}  // namespace

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions) return false;
    try { reframework::API::initialize(param); } catch (...) { return false; }
    try {
        fighters::preload_async();                              // the game's fighter names, off the game's frames
        cmn::g_bridge_editor.status = engine::live::status;
        cmn::g_bridge_editor.ready = engine::live::ready;
        cmn::g_bridge_decide.ready = engine::decide::ready;
        cmn::g_bridge_decide.frame = engine::decide::on_frame;
        cmn::g_bridge_decide.squares = decide_squares;
        cmn::g_bridge_decide.follow_debug = engine::decide::follow_debug;
        cmn::g_bridge_decide.status = engine::decide::status;
        cmn::g_bridge_vfx.ready = vfx::engine::ready;
        cmn::g_bridge_vfx.status = vfx::engine::status;
        cmn::g_bridge_vfx.mech_probe = vfx::mech::probe;
        cmn::g_bridge_vfx.body_probe = vfx::body::probe;
        cmn::g_bridge_vfx.body_ready = vfx::body::ready;
        cmn::g_bridge_rows.ready = screens::rows::ready;
        cmn::g_bridge_rows.status = screens::rows::status;
        cmn::g_bridge_vfx.tick_ready = vfx::tick::ready;
        cmn::g_bridge_vfx.tick_status = vfx::tick::status;
        {                                                       // phase 5c-1: the Drive settings and their pushes
            vfx::drive_cfg::Env env;
            env.root = "reframework/data";
            env.L = [](const std::string& k) { return dialogs::locale::get(k, {}); };
            env.squares = [](int f, int side) { return engine::decide::squares(f, side); };
            vfx::drive_cfg::set_env(env);
            vfx::drive_cfg::preload();
            cmn::g_bridge_vfx.feed_frame = vfx::feed::on_frame;
        }
        cmn::g_bridge_dipanel.ready = screens::dipanel::ready;
        cmn::g_bridge_dipanel.on_frame = screens::dipanel::on_frame;
        cmn::g_bridge_dipanel.status = screens::dipanel::status;
        cmn::g_bridge_tracker.ready = screens::tracker::ready;
        cmn::g_bridge_tracker.on_frame = screens::tracker::on_frame;
        cmn::g_bridge_tracker.status = screens::tracker::status;
        cmn::g_bridge_guide.ready = screens::guide::ready;
        cmn::g_bridge_guide.on_frame = screens::guide::on_frame;
        cmn::g_bridge_guide.status = screens::guide::status;
        cmn::g_bridge_locale.ready = dialogs::locale::ready;
        cmn::g_bridge_locale.on_frame = dialogs::locale::on_frame;
        cmn::g_bridge_locale.status = dialogs::locale::status;
        cmn::g_bridge_camera.ready = dialogs::camera::ready;
        cmn::g_bridge_camera.on_frame = dialogs::camera::on_frame;
        cmn::g_bridge_camera.status = dialogs::camera::status;
        cmn::g_bridge_dialog_editor.ready = dialogs::editor::ready;
        for (auto& f : dialogs::editor::bridge_fns()) cmn::g_bridge_dialog_editor.fns.push_back({ f.name, f.fn });
        for (auto& f : screens::drive_bridge::fns()) cmn::g_bridge_dialog_editor.fns.push_back(f);   // phase 5c-1
        {
            // since sf6core 0.6.0 the option engine is the core's: the Lua bridge's opt_* functions are gone (nothing
            // loads func/NativeOptions.lua's native block any more), only the feature's state stays
            auto& b = cmn::g_bridge_options;
            b.ready = dialogs::options::driver::ready;
            b.status = dialogs::options::driver::status;
        }
        const bool ok = cmn::init();
        const bool patch = cmn::owns(cmn::Feature::Editor) || cmn::owns(cmn::Feature::Engine);
        const bool live_ok = patch && engine::live::install();
        if (patch && !live_ok) cmn::log_warn("[ColorModNative] colour patch not installed");
        if (cmn::owns(cmn::Feature::Decisions)) {
            if (!live_ok) cmn::log_warn("[ColorModNative] decisions need the colour patch: not installed");
            else if (!engine::decide::install()) cmn::log_warn("[ColorModNative] decisions not installed");
        }
        // phase 3: screens_dipanel hooks, only when the owner is native (lua = the module stays inert, no hook)
        if (cmn::owns(cmn::Feature::ScreensDipanel) && !screens::dipanel::install()) cmn::log_warn("[ColorModNative] dipanel hooks not installed");
        if (cmn::owns(cmn::Feature::VfxEngine)) {
            if (!vfx::engine::install()) cmn::log_warn("[ColorModNative] vfx_engine not installed");
            else if (vfx::mech::install()) {
                vfx::body::install(cmn::owns(cmn::Feature::VfxBody));   // body: shadow unless vfx_body native
                // the battle loop: owner with vfx native, else shadow (compares the Lua's relays)
                if (!vfx::tick::install(cmn::owns(cmn::Feature::Vfx)) && cmn::owns(cmn::Feature::Vfx)) cmn::log_warn("[ColorModNative] vfx tick not installed");
            }
        }
        // phase 3: native screens
        if (cmn::owns(cmn::Feature::ScreensRows)) {
            if (!screens::rows::install()) cmn::log_warn("[ColorModNative] screens_rows not installed");
        }
        if (cmn::owns(cmn::Feature::ScreensTracker)) {
            if (!screens::tracker::install()) cmn::log_warn("[ColorModNative] screens_tracker not installed");
        }
        // screens_guide: hooks only when the owner is native (lua = the module stays inert, no hook)
        if (cmn::owns(cmn::Feature::ScreensGuide) && !screens::guide::install()) cmn::log_warn("[ColorModNative] screens_guide not installed");
        // phase 4a: locale (no hooks; the module only loads locale.json and probes the game language every
        // 60 frames). Only started when the owner is native (lua = NativeLocale.lua keeps doing it).
        if (cmn::owns(cmn::Feature::DialogsLocale) && !dialogs::locale::install()) cmn::log_warn("[ColorModNative] dialogs_locale not installed");
        // phase 4 sub-phase 4c: camera preview, only when the owner is native (no hook, pure polling)
        if (cmn::owns(cmn::Feature::DialogsCamera) && !dialogs::camera::install()) cmn::log_warn("[ColorModNative] dialogs_camera not installed");
        // phase 4b: dialogs_options. Since sf6core 0.6.0 the option engine and its hooks are the core's service:
        // the mod hands it its callbacks (dialogs/options_driver.cpp, the editor's relay)
        if (cmn::owns(cmn::Feature::DialogsOptions)) dialogs::options::driver::install();
        // phase 4d + 4e: the costume colour editor and the DI colour menu (no hook: driven by the Lua relay)
        if (cmn::owns(cmn::Feature::DialogsEditor) && !dialogs::editor::install()) cmn::log_warn("[ColorModNative] dialogs_editor not installed");
        // phase 5c-2: the colour screens' tracking and triggers, the Esc hooks; phase 5e: their frame and the option
        // engine's, at the end of the core's frame callback, and the engine's pump before the frame is drawn (the two
        // callbacks SF6_ColorSpinExtra.lua and func/NativeOptions.lua had)
        if (cmn::owns(cmn::Feature::ScreensColour)) {
            if (!screens::colour::track::install()) cmn::log_warn("[ColorModNative] screens_colour not installed");
            cmn::g_screens_frame = [] {
                if (!cmn::bridge_owns("screens_colour")) return;
                screens::colour::track::frame(cmn::frame());
            };
        }
        return ok;
    } catch (...) { return false; }
}
