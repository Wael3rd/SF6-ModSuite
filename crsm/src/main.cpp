// SF6_CRSMNative - CRSM (Community Recording Slot Manager) as one DLL, client of sf6core_native.dll.
//
// Phase P0: the skeleton. Registers with the Native Core as client
// "SF6_CRSMNative", keeps a journal, status.json and settings.json, draws nothing. The probes (probes.cpp) are off
// unless probes_on.txt exists. Phase P3 (mech/): the files, replay, reversal and record layers, each "lua" by default
// (settings.json): the DLL installs nothing and touches no user data until a layer is switched to "native".
// Phase P4 (ui/): the screens (owner "ui", needs every layer native), started when the Lua loader hands the UI over.
// Phase P5 (ui/): the Manager page of the Recording strip on the core's training pause-menu service (same owner), asked
// of the core at that same hand-over (ui_claim).
// Phase P6 (standalone.hpp, ui/ui.cpp): with every owner native, settings.json "standalone": "auto" and no CRSM Lua script
// installed, the DLL makes that hand-over itself, 2 s into the first training session (the Native package ships no Lua).
//
// Build: crsm\build.bat (MSVC, /std:c++20 /EHa). Output: build\crsm\SF6_CRSMNative.dll (never copied into the game).
// Kill switch: reframework/data/_crsm_native_off.txt makes the DLL inert at the next start.
// Files: reframework/data/SF6_CRSMNative_data/{settings.json (owners), status.json, debug_log.json, probes.log,
//        probes_on.txt (switch)}

#include <reframework/API.hpp>

#include "common.hpp"
#include "mech/mech.hpp"
#include "probes.hpp"
#include "ui/ui.hpp"
#include "ui/manager_page.hpp"

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    // 1.10, as the core and the other native mods: nothing past it is used (the public REFramework 1.5.8 refuses more)
    v->major = 1; v->minor = 10; v->patch = 0; v->game_name = nullptr;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions) return false;
    try { reframework::API::initialize(param); } catch (...) { return false; }
    try {
        cmn::g_probe_frame = probes::on_frame;
        cmn::g_probe_status = probes::status;
        mech::init();                         // P3 layers: declared, bridge functions and frame hooks registered
        ui::init();                           // P4 UI (owner "ui"): declared, its bridge function and frame hook
        ui::manager::init();                  // P5: the Manager page (owner ui, lua by default)
        const bool ok = cmn::init();
        // The probes' hooks only when the DLL is active (core found, no kill file) and probes_on.txt exists.
        if (cmn::active()) {
            probes::install();
            ui::manager::start();             // P5's needs (core 0.8.0+ pm service, files native), else ui declined: before the hooks
            ui::start();                      // P4: locale.json; the screens wait for the Lua loader's claim (the page is asked then)
            mech::start();                    // hooks of the layers settings.json hands to the DLL (none by default), the UI's too
            cmn::refresh_status();
        }
        return ok;
    } catch (...) { return false; }
}
