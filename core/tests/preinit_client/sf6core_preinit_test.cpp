// sf6core_preinit_test.cpp -- Pre-init path test client
//
// DLL stem "SF6core_preinit_test" sorts BEFORE "sf6core_native" in
// REFramework's std::map order (uppercase 'S' < lowercase 's').
// This client registers during its own reframework_plugin_initialize
// (before the core's initialize) to prove the pending queue:
//   - register_client succeeds (state = PENDING_INIT)
//   - after the core's init, the first frame callback fires
//   - the client logs that it received its first callback
//
// Build: cl /nologo /std:c++20 /EHa /O2 /LD /W4
//            /I include /I third_party\reframework\include
//            sf6core_preinit_test.cpp
// Output: SF6core_preinit_test.dll (uppercase stem!)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <sf6core/sf6core_api.h>

#pragma warning(push, 0)
#include <reframework/API.hpp>
#pragma warning(pop)

#include <cstdio>

// ========================================================================
// State
// ========================================================================

static constexpr const char* TAG             = "[preinit_test] ";
static constexpr const char* CLIENT_ID       = "sf6core_preinit_test";
static constexpr const char* CLIENT_VERSION  = "0.2.0";
static constexpr int         CLIENT_PRIORITY = 10; // low = early

static const SF6CoreAPI* s_api    = nullptr;
static SF6CoreClient*    s_client = nullptr;
static bool              s_first_frame_logged = false;

// ========================================================================
// Callbacks
// ========================================================================

static void SF6CORE_CALL on_frame(uint64_t frame, void* /*user*/) {
    if (!s_first_frame_logged) {
        s_first_frame_logged = true;
        if (s_api && s_client) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "FIRST FRAME CALLBACK received (frame=%llu) -- "
                     "pre-init queue PASS",
                     (unsigned long long)frame);
            s_api->log(s_client, SF6CORE_LOG_INFO, msg);
        }
    }
}

// ========================================================================
// REFramework plugin entry points
// ========================================================================

extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major     = 1;
    v->minor     = 10;
    v->patch     = 0;
    v->game_name = nullptr;
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->sdk) return false;

    try { reframework::API::initialize(param); } catch (...) { return false; }

    // Discover the core (should succeed: DLL loaded in pass 1)
    HMODULE core = GetModuleHandleW(L"sf6core_native.dll");
    if (!core) {
        param->functions->log_info(
            "%ssf6core_native.dll not loaded -- staying inert", TAG);
        return true;
    }

    auto get_api = (SF6CoreGetApiFn)GetProcAddress(core, "sf6core_get_api");
    if (!get_api) {
        param->functions->log_info(
            "%ssf6core_get_api not found -- core too old", TAG);
        return true;
    }

    const SF6CoreAPI* api = get_api(SF6CORE_ABI_VERSION);
    if (!api) {
        param->functions->log_info(
            "%ssf6core ABI mismatch or disabled", TAG);
        return true;
    }
    s_api = api;

    // The core may NOT have initialized yet (this DLL sorts first).
    // register_client should succeed with state = PENDING_INIT.
    SF6CoreClientDesc desc = {};
    desc.struct_size    = sizeof(SF6CoreClientDesc);
    desc.client_id      = CLIENT_ID;
    desc.client_version = CLIENT_VERSION;
    desc.priority       = CLIENT_PRIORITY;

    SF6CoreStatus status = api->register_client(&desc, &s_client);
    if (status != SF6CORE_OK) {
        param->functions->log_warn(
            "%sregister_client failed (status=%d)", TAG, (int)status);
        s_api = nullptr;
        return true;
    }

    // Log registration state (should be PENDING_INIT if core hasn't init'd)
    SF6CoreClientState st = api->get_client_state(s_client);
    const char* st_str = "unknown";
    switch (st) {
    case SF6CORE_CLIENT_ACTIVE:              st_str = "active"; break;
    case SF6CORE_CLIENT_DISABLED_BY_BREAKER: st_str = "disabled"; break;
    case SF6CORE_CLIENT_PENDING_INIT:        st_str = "pending_init"; break;
    }
    param->functions->log_info(
        "%sregistered (state=%s) -- waiting for first frame callback", TAG, st_str);

    // Set frame callback
    api->set_frame_callback(s_client, on_frame, nullptr);

    return true;
}

// ========================================================================
// DllMain
// ========================================================================

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
