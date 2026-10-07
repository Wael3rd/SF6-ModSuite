// sf6core_internal.h -- Shared definitions between sf6core.cpp and display.cpp
// Not part of the public API. Only used within the core DLL.

#ifndef SF6CORE_INTERNAL_H
#define SF6CORE_INTERNAL_H

#include <windows.h>
#include <sf6core/sf6core_api.h>
#include <reframework/API.h>
#include <atomic>

// ========================================================================
// RawInvokeRet (POD for __try -- no C++ objects needing unwinding)
// ========================================================================

#pragma pack(push, 1)
struct RawInvokeRet {
    union {
        uint8_t  bytes[128];
        uint32_t dword;
        uint64_t qword;
        float    f;
        void*    ptr;
    };
    bool exception_thrown;
};
#pragma pack(pop)

// ========================================================================
// Internal client definition (opaque to callers via SF6CoreClient*)
// ========================================================================

static constexpr int CLIENT_ID_MAX  = 63;
static constexpr int CLIENT_VER_MAX = 31;

struct SF6CoreClient {
    char              client_id[CLIENT_ID_MAX + 1];
    char              client_version[CLIENT_VER_MAX + 1];
    int32_t           priority;
    SF6CoreClientState state;

    SF6CoreFlowCb     flow_cb;
    void*             flow_user;
    SF6CoreFrameCb    frame_cb;
    void*             frame_user;

    int               fault_count;     // consecutive faults (reset on success)
    int               total_faults;    // lifetime counter (health)
    uint64_t          disabled_until;  // frame at which the breaker goes half-open
    bool              half_open;       // next callback is the retry
    bool              active;          // false after unregister
    bool              rules_suspended; // set_element_rules_suspended: skip all hide rules
};

// ========================================================================
// Shared globals (defined in sf6core.cpp)
// ========================================================================

extern const REFrameworkSDKData* s_sdk;
extern std::atomic<bool>         s_initialized;
extern uint64_t                  s_frame;

// ========================================================================
// Shared functions (defined in sf6core.cpp)
// ========================================================================

void core_log(SF6CoreLogLevel level, const char* client_id,
              const char* fmt, ...);

bool kill_switch_active();

void handle_fault(SF6CoreClient* c, DWORD exc_code, const char* phase);
void handle_success(SF6CoreClient* c);

bool invoke_c(const REFrameworkSDKData* sdk, REFrameworkMethodHandle mh,
              void* thisptr, void** args, unsigned int argc,
              RawInvokeRet* out);

bool write_field_managed(const REFrameworkSDKData* sdk, void* obj,
                         const char* name, void* value);

bool managed_str_to_utf8(void* str_obj, char* buf, int buf_size);

void* read_field_ref(const REFrameworkSDKData* sdk, void* obj, const char* name);

REFrameworkMethodHandle find_method_on(const REFrameworkSDKData* sdk,
                                       void* obj, const char* name);

// Accessor for the static API struct (defined in sf6core.cpp)
const SF6CoreAPI* sf6core_api_ptr();

// ========================================================================
// Functions defined in display.cpp / lua_bridge.cpp
// ========================================================================

void display_set_game_thread(DWORD tid);
bool is_game_thread();
void display_log_init();
void lua_bridge_init(const REFrameworkPluginFunctions* functions);

// ========================================================================
// Functions defined in discovery.cpp (increment 4, ADR-0012)
// ========================================================================

void discovery_log_init();

// Agent registry: first agent by name (used by api_ui_find_agent)
void* discovery_find_first_agent(const char* name);

// ZenMode element hiding
void discovery_commit_zen_hides(uint64_t current_frame,
                                 const char (*sighted_owners)[128],
                                 const void** sighted_views,
                                 const uint64_t* sighted_frames,
                                 int n_sighted);
bool discovery_zen_owner_declared(const char* owner_name);
void discovery_restore_client_zen(SF6CoreClient* client);

// Lua pattern matcher (shared with element hiding)
bool lua_pat_match(const char* subject, const char* pattern);

// Zen rule count (for draw callback early-out)
extern std::atomic<int> s_zen_rule_count_pub;

// API implementations (discovery.cpp)
uint32_t SF6CORE_CALL api_ui_agents(const char* name, void** out, uint32_t max);
uint32_t SF6CORE_CALL api_ui_agents_gen(const char* name);
uint32_t SF6CORE_CALL api_ui_registry_gen(void);
void*    SF6CORE_CALL api_ui_find_cached(void* root, const char* name);
uint32_t SF6CORE_CALL api_ui_find_many(void* root, const char* const* names,
                                        uint32_t n, void** out);
SF6CoreStatus SF6CORE_CALL api_set_element_hidden_zen(
    SF6CoreClient* client, const char* gui_owner,
    const char* const* pattern_segments, uint32_t n_segments, int32_t hidden);

// Increment 5: bridge extensions
int32_t       SF6CORE_CALL api_client_state_by_id(const char* client_id);
SF6CoreStatus SF6CORE_CALL api_lua_register_fn(SF6CoreClient* client, const char* name,
                                                SF6CoreLuaFn fn, void* user);
SF6CoreStatus SF6CORE_CALL api_set_element_rules_suspended(SF6CoreClient* client, int32_t suspended);

// Access to the native-fn registry from sf6core.cpp (for lua_register_fn / unregister cleanup)
void bridge_register_native_fn(SF6CoreClient* client, const char* name, SF6CoreLuaFn fn, void* user);
void bridge_unregister_client_fns(const char* client_id);

// Safe client state query by id (reads under s_registry_mutex, no dangling pointer).
// Returns the client state, or -1 if not found/not active.
int32_t query_client_state_by_id(const char* client_id);

// Safe combined query: state + rules_suspended, under registry mutex.
void query_client_state_and_suspended(const char* client_id,
                                       int32_t* out_state, bool* out_suspended);

// Safe handle_fault by client id (re-looks up the client under s_registry_mutex).
void handle_fault_by_id(const char* client_id, DWORD exc_code, const char* phase);

#endif // SF6CORE_INTERNAL_H
