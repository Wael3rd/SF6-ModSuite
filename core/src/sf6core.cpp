// sf6core.cpp -- SF6 Native UI Core implementation
//
// Single-file implementation of sf6core_native.dll.
// Build: cl /nologo /std:c++20 /EHa /O2 /LD /W4 /DSF6CORE_EXPORTS ...
//
// ADRs: ADR-0005 (single commit point), ADR-0008 (fault isolation)
//
// Key invariants:
//   - sf6core_get_api returns a pointer to a STATIC struct (never moves)
//   - register_client works before the core's own init (queued)
//   - Callbacks fire only after core init, on the game thread
//   - Every client callback is SEH-wrapped (__try/__except)
//   - Dispatch iterates a sorted vector copy (deterministic, lock-free)
//   - Zero per-frame allocation in steady state

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "sf6core_internal.h"
#include <timeapi.h>   // timeBeginPeriod (winmm)

#pragma warning(push, 0)
#include <reframework/API.hpp>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perf.hpp"

// ========================================================================
// Constants
// ========================================================================

static constexpr const char* CORE_VERSION = "0.9.4";
static constexpr const char* TAG          = "[sf6core] ";
// Circuit breaker per ADR-0008: MAX_CONSECUTIVE_FAULTS faults in a row ->
// disabled for BREAKER_COOLDOWN_FRAMES, then half-open (one retry: success
// closes the breaker, a fault re-opens it immediately for another cooldown).
static constexpr int      MAX_CONSECUTIVE_FAULTS  = 5;
static constexpr uint64_t BREAKER_COOLDOWN_FRAMES = 300; // ~5 s at 60 fps
static constexpr int  BREADCRUMB_COUNT    = 64;
// Increment 2: widget + element hiding
static constexpr int  WIDGET_MAX_RETRIES  = 300;  // ~5 s at 60 fps
static constexpr int  WIDGET_PATH_MAX     = 256;
static constexpr int  WIDGET_TEXT_MAX     = 512;
static constexpr int  WIDGET_SEG_MAX      = 16;   // max path segments
static constexpr int  ELEM_OWNER_MAX      = 128;
static constexpr int  ELEM_PATH_MAX       = 256;
static constexpr int  NAME_CACHE_BUCKETS  = 256;  // power of 2

// ========================================================================
// Widget internal state (increment 2)
// ========================================================================

struct SF6CoreWidget {
    SF6CoreClient*     owner;
    SF6CoreWidgetState state;
    bool               active;

    // Descriptor (copied at creation)
    char               prefab_path[WIDGET_PATH_MAX];
    int32_t            flow_ids[32];
    uint32_t           n_flow_ids;
    float              pos_x, pos_y;
    bool               client_visible;

    // Engine handles
    void*              resource;
    void*              holder;
    void*              prefab_mo;
    void*              game_object;
    void*              ui_agent;
    bool               ref_held;
    int                retry_count;

    // Text element cache (path -> control)
    struct TextCache { char path[WIDGET_PATH_MAX]; void* control; };
    TextCache          tcache[32];
    int                tcache_n;

    // Pending text writes (consumed each commit)
    struct PendText { char path[WIDGET_PATH_MAX]; char text[WIDGET_TEXT_MAX]; };
    PendText           ptexts[16];
    int                ptext_n;

    // Pending ops
    bool               pos_dirty;
    float              pend_x, pend_y;
    bool               vis_dirty;
    bool               pend_destroy;
    bool               was_shown;       // last applied agent-state

    // Warm-up after instantiation: no agent-state / text / position writes
    // until the agent has started (it auto-registers with UIAgentManager on
    // its first update). Forcing _StateManager._Next in the SAME frame as
    // instantiate crashed the game in-engine (2026-10-01 18:43, RIP
    // exe+0x22F0330, agent in RBX/R13/R14); the working probes always forced
    // Show at least one frame later.
    bool               warming;
    uint64_t           warm_until;      // earliest frame to go LIVE
};

static constexpr uint64_t WIDGET_WARMUP_FRAMES  = 3;
static constexpr uint64_t WIDGET_WARMUP_TIMEOUT = 180; // frames after warm_until

// ========================================================================
// Element-hiding rule (increment 2)
// ========================================================================

struct ElementHideRule {
    SF6CoreClient*     owner;           // set to nullptr on unregister (never deref after that)
    char               client_id[CLIENT_ID_MAX + 1];  // by-value copy for safe access
    char               gui_owner[ELEM_OWNER_MAX];
    char               segments[WIDGET_SEG_MAX][ELEM_PATH_MAX];
    int                n_segments;
    bool               hidden;          // desired ForceInvisible state
    bool               restore_pending; // unregister: restore on next commit, then deactivate
};

// Name cache for the draw callback (direct-mapped, no alloc)
struct NameCacheSlot {
    uintptr_t addr;
    uintptr_t owner_ptr;
    char      name[ELEM_OWNER_MAX];
    bool      valid;
};

// ========================================================================
// Breadcrumb types
// ========================================================================

enum BreadcrumbKind : uint8_t {
    BC_INIT = 0, BC_REGISTER, BC_UNREGISTER,
    BC_FLOW_CHANGE, BC_FAULT, BC_BREAKER_TRIP,
    BC_WIDGET_CREATE, BC_WIDGET_LIVE, BC_WIDGET_FAIL, BC_WIDGET_DESTROY,
    BC_ELEM_HIDE
};

static const char* bc_kind_str(BreadcrumbKind k) {
    switch (k) {
    case BC_INIT:         return "INIT";
    case BC_REGISTER:     return "REGISTER";
    case BC_UNREGISTER:   return "UNREGISTER";
    case BC_FLOW_CHANGE:  return "FLOW_CHANGE";
    case BC_FAULT:        return "FAULT";
    case BC_BREAKER_TRIP:   return "BREAKER_TRIP";
    case BC_WIDGET_CREATE:  return "WIDGET_CREATE";
    case BC_WIDGET_LIVE:    return "WIDGET_LIVE";
    case BC_WIDGET_FAIL:    return "WIDGET_FAIL";
    case BC_WIDGET_DESTROY: return "WIDGET_DESTROY";
    case BC_ELEM_HIDE:      return "ELEM_HIDE";
    default:                return "?";
    }
}

struct Breadcrumb {
    uint64_t       frame;
    BreadcrumbKind kind;
    char           client_id[CLIENT_ID_MAX + 1];
    char           detail[128];
};

// ========================================================================
// Global state (all file-static)
// ========================================================================

static std::mutex                    s_registry_mutex;
static std::vector<SF6CoreClient*>   s_clients;     // sorted by (priority, id)
static std::vector<SF6CoreClient*>   s_pending;     // pre-init registrations

std::atomic<bool> s_initialized{false};

// Kill switch: lazily checked, then cached
static std::atomic<int>  s_kill_switch{0}; // 0=unchecked, 1=ok, 2=disabled

// Closing guard: set by the window message callback on WM_CLOSE / WM_DESTROY / etc.
std::atomic<bool> s_closing{false};
static uint64_t   s_close_frame = 0;    // frame at which closing started
static bool       s_close_final = false; // WM_DESTROY / WM_NCDESTROY / WM_ENDSESSION: not cancellable

// REFramework SDK pointer (set at init)
// Shared with display.cpp (not static)
const REFrameworkSDKData* s_sdk = nullptr;

// Flow detection
static int32_t  s_flow_id         = -1;
static bool     s_flow_feature_ok = true; // false if type/field not found

// Frame counter (shared with display.cpp)
uint64_t s_frame = 0;

// Logging
static FILE*    s_log_file = nullptr;
static char     s_data_dir[MAX_PATH] = {};

// Breadcrumbs
static Breadcrumb s_breadcrumbs[BREADCRUMB_COUNT] = {};
static uint32_t   s_bc_write = 0;

// Increment 2: widgets and element hiding
static std::vector<SF6CoreWidget*> s_widgets;
static std::vector<ElementHideRule> s_elem_hides;

// Thread model for element hiding (see docs/ABI.md):
// s_elem_hides and the sighting table are shared between the game thread
// (API calls, commit phase) and the RENDERING thread (on_pre_gui_draw_element,
// REFramework Hooks.cpp gui_draw_hook_internal). Every access to them goes
// through s_hide_mutex. The rendering thread only try_lock()s it (never
// stalls a frame) and never calls engine code while holding it; the commit
// phase copies what it needs under the lock and does all engine work after
// releasing it. Lock helpers live in functions WITHOUT __try (C2712).
static std::mutex             s_hide_mutex;
static std::atomic<int>       s_elem_count_pub{0};    // rule count, for early-out
static std::atomic<bool>      s_name_cache_flush{false};
static std::atomic<uint64_t>  s_frame_pub{0};         // s_frame mirror for the render thread
uint64_t core_frame_pub() { return s_frame_pub.load(std::memory_order_relaxed); }
static NameCacheSlot s_name_cache[NAME_CACHE_BUCKETS] = {};
static bool s_elem_draw_registered = false;

// Type/method cache (resolved lazily on first use)
static struct TypeCache {
    bool resolved;
    bool failed;
    REFrameworkTypeDefinitionHandle t_prefab;
    REFrameworkTypeDefinitionHandle t_vec3;
    REFrameworkTypeDefinitionHandle t_game_object;
    REFrameworkTypeDefinitionHandle t_ui_agent;
    REFrameworkTypeDefinitionHandle t_state_show;
    REFrameworkTypeDefinitionHandle t_state_hide;
    REFrameworkTypeDefinitionHandle t_play_object;
    REFrameworkMethodHandle m_prefab_set_path;
    REFrameworkMethodHandle m_prefab_instantiate;
    REFrameworkMethodHandle m_go_get_component;
    REFrameworkMethodHandle m_go_destroy;
    REFrameworkMethodHandle m_go_get_name;
    REFrameworkMethodHandle m_po_get_child;
    REFrameworkMethodHandle m_po_get_next;
    REFrameworkMethodHandle m_po_get_name;
    REFrameworkMethodHandle m_po_get_position;
    REFrameworkMethodHandle m_po_set_position;
    REFrameworkMethodHandle m_po_set_force_invisible;
    REFrameworkMethodHandle m_po_get_force_invisible;
    REFrameworkMethodHandle m_text_get_message;
    REFrameworkMethodHandle m_text_set_message;
    REFrameworkMethodHandle m_agent_get_control_main;
    REFrameworkMethodHandle m_uiam_add;   // app.UIAgentManager.Add(app.UIAgent)
    REFrameworkManagedObjectHandle rt_ui_agent;
} s_tc = {};

// ========================================================================
// Path resolution
// ========================================================================

static void resolve_game_root(char* out, size_t out_size) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* sep = strrchr(exe, '\\');
    if (sep) *sep = '\0';
    strncpy(out, exe, out_size - 1);
    out[out_size - 1] = '\0';
}

// ========================================================================
// Kill switch (lazy, threadsafe)
// ========================================================================

bool kill_switch_active() {
    int state = s_kill_switch.load(std::memory_order_acquire);
    if (state != 0) return state == 2;

    char root[MAX_PATH];
    resolve_game_root(root, sizeof(root));
    char path[MAX_PATH];
    snprintf(path, sizeof(path),
             "%s\\reframework\\data\\sf6core\\disabled.txt", root);
    bool exists = (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    s_kill_switch.store(exists ? 2 : 1, std::memory_order_release);
    return exists;
}

// ========================================================================
// Logging
// ========================================================================

static void ensure_data_dir() {
    if (s_data_dir[0]) return;
    char root[MAX_PATH];
    resolve_game_root(root, sizeof(root));
    char d1[MAX_PATH];
    snprintf(d1, sizeof(d1), "%s\\reframework\\data", root);
    CreateDirectoryA(d1, nullptr);
    snprintf(s_data_dir, sizeof(s_data_dir),
             "%s\\reframework\\data\\sf6core", root);
    CreateDirectoryA(s_data_dir, nullptr);
}

static void open_log() {
    ensure_data_dir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\sf6core.log", s_data_dir);
    // One previous log is kept: past 8 MB the log becomes sf6core.old.log (it had reached 53 MB by 07/10)
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) &&
        (fa.nFileSizeHigh != 0 || fa.nFileSizeLow > 8u * 1024 * 1024)) {
        char old[MAX_PATH];
        snprintf(old, sizeof(old), "%s\\sf6core.old.log", s_data_dir);
        MoveFileExA(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    s_log_file = fopen(path, "a");
    if (!s_log_file) return;

    time_t now = time(nullptr);
    struct tm tb;
    localtime_s(&tb, &now);
    fprintf(s_log_file,
            "\n=== sf6core %s  ABI %d  %04d-%02d-%02d %02d:%02d:%02d ===\n",
            CORE_VERSION, SF6CORE_ABI_VERSION,
            tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday,
            tb.tm_hour, tb.tm_min, tb.tm_sec);
    fflush(s_log_file);
}

// Lines logged before the core's own initialize: the clients' DLL names sort before ours, so they register (and may
// log) first, when neither the file nor REFramework's API is ready. Kept here, written once the log is open.
// An SRWLOCK and a heap buffer: no destructor runs at process exit, where a client may still log (kill switch on,
// core never initialized).
struct EarlyLine { SF6CoreLogLevel level; std::string client_id; std::string text; };
static constexpr size_t        EARLY_LOG_MAX = 128;
static SRWLOCK                 s_early_lock = SRWLOCK_INIT;
static std::vector<EarlyLine>* s_early = nullptr;
static uint32_t                s_early_dropped = 0;
static std::atomic<bool>       s_log_ready{false};

static void write_log_line(SF6CoreLogLevel level, const char* client_id, const char* buf);

void core_log(SF6CoreLogLevel level, const char* client_id,
              const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (!s_log_ready.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&s_early_lock);
        const bool kept = !s_log_ready.load(std::memory_order_relaxed);
        if (kept) {
            if (!s_early) s_early = new std::vector<EarlyLine>();
            if (s_early->size() < EARLY_LOG_MAX)
                s_early->push_back({level, client_id ? client_id : "", buf});
            else
                ++s_early_dropped;
        }
        ReleaseSRWLockExclusive(&s_early_lock);
        if (kept) return;
    }
    write_log_line(level, client_id, buf);
}

// Called once, right after open_log(): the lines kept before, then the log is live.
static void flush_early_log() {
    AcquireSRWLockExclusive(&s_early_lock);
    if (s_early)
        for (const auto& e : *s_early)
            write_log_line(e.level, e.client_id.empty() ? nullptr : e.client_id.c_str(), e.text.c_str());
    if (s_early_dropped) {
        char b[96];
        snprintf(b, sizeof(b), "%u line(s) logged before init were dropped", s_early_dropped);
        write_log_line(SF6CORE_LOG_WARN, nullptr, b);
    }
    delete s_early;
    s_early = nullptr;
    s_log_ready.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&s_early_lock);
}

static void write_log_line(SF6CoreLogLevel level, const char* client_id, const char* buf) {
    // REFramework log
    try {
        auto& api = reframework::API::get();
        if (api) {
            switch (level) {
            case SF6CORE_LOG_ERROR: api->log_error("%s%s", TAG, buf); break;
            case SF6CORE_LOG_WARN:  api->log_warn("%s%s", TAG, buf);  break;
            default:                api->log_info("%s%s", TAG, buf);   break;
            }
        }
    } catch (...) {
        // API not yet initialized -- ignore
    }

    // File log
    if (s_log_file) {
        const char* lvl_str = "INFO";
        switch (level) {
        case SF6CORE_LOG_ERROR: lvl_str = "ERROR"; break;
        case SF6CORE_LOG_WARN:  lvl_str = "WARN";  break;
        case SF6CORE_LOG_DEBUG: lvl_str = "DEBUG"; break;
        default: break;
        }
        if (client_id && client_id[0])
            fprintf(s_log_file, "[%llu] %s [%s] %s\n",
                    (unsigned long long)s_frame, lvl_str, client_id, buf);
        else
            fprintf(s_log_file, "[%llu] %s %s\n",
                    (unsigned long long)s_frame, lvl_str, buf);
        fflush(s_log_file);
    }
}

// ========================================================================
// Breadcrumbs
// ========================================================================

static void push_breadcrumb(BreadcrumbKind kind, const char* client_id,
                            const char* detail) {
    uint32_t idx = s_bc_write % BREADCRUMB_COUNT;
    Breadcrumb* bc = &s_breadcrumbs[idx];
    bc->frame = s_frame;
    bc->kind  = kind;
    if (client_id)
        strncpy(bc->client_id, client_id, CLIENT_ID_MAX);
    else
        bc->client_id[0] = '\0';
    bc->client_id[CLIENT_ID_MAX] = '\0';
    if (detail)
        strncpy(bc->detail, detail, sizeof(bc->detail) - 1);
    else
        bc->detail[0] = '\0';
    bc->detail[sizeof(bc->detail) - 1] = '\0';
    s_bc_write++;
}

static void dump_breadcrumbs() {
    ensure_data_dir();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\breadcrumbs.txt", s_data_dir);
    FILE* f = fopen(path, "w");
    if (!f) return;

    uint32_t total = s_bc_write;
    uint32_t start = (total > BREADCRUMB_COUNT)
                   ? (total - BREADCRUMB_COUNT) : 0;
    for (uint32_t i = start; i < total; i++) {
        const Breadcrumb* bc = &s_breadcrumbs[i % BREADCRUMB_COUNT];
        fprintf(f, "frame=%llu kind=%s client=%s detail=%s\n",
                (unsigned long long)bc->frame,
                bc_kind_str(bc->kind),
                bc->client_id[0] ? bc->client_id : "-",
                bc->detail[0] ? bc->detail : "-");
    }
    fclose(f);
}

// ========================================================================
// Field-reading helpers (C SDK, safe inside __try)
// ========================================================================

void* read_field_ref(const REFrameworkSDKData* sdk,
                     void* obj, const char* name) {
    if (!obj || !name) return nullptr;
    auto td = sdk->managed_object->get_type_definition(
        (REFrameworkManagedObjectHandle)obj);
    while (td) {
        auto fh = sdk->type_definition->find_field(td, name);
        if (fh) {
            void* raw = sdk->field->get_data_raw(fh, obj, false);
            if (!raw) return nullptr;
            return *(void**)raw;
        }
        td = sdk->type_definition->get_parent_type(td);
    }
    return nullptr;
}

static bool read_field_i32(const REFrameworkSDKData* sdk,
                           void* obj, const char* name, int32_t* out) {
    if (!obj || !name) return false;
    auto td = sdk->managed_object->get_type_definition(
        (REFrameworkManagedObjectHandle)obj);
    while (td) {
        auto fh = sdk->type_definition->find_field(td, name);
        if (fh) {
            void* raw = sdk->field->get_data_raw(fh, obj, false);
            if (!raw) return false;
            *out = *(int32_t*)raw;
            return true;
        }
        td = sdk->type_definition->get_parent_type(td);
    }
    return false;
}

// ========================================================================
// Flow polling (SEH-guarded, every frame)
// Cached field handles: resolved once, re-resolved only on failure.
// Per-frame cost: get_managed_singleton + 3 get_data_raw (pointer loads).
// ========================================================================

static struct FlowFieldCache {
    bool resolved;
    bool failed;
    REFrameworkFieldHandle fh_flow_work;
    REFrameworkFieldHandle fh_flow_map;
    REFrameworkFieldHandle fh_id;
} s_ffc = {};

static bool resolve_flow_fields() {
    if (s_ffc.resolved) return !s_ffc.failed;
    s_ffc.resolved = true;
    auto* sdk = s_sdk;
    if (!sdk) { s_ffc.failed = true; return false; }

    auto* fm = sdk->functions->get_managed_singleton("app.bFlowManager");
    if (!fm) { s_ffc.failed = true; return false; }

    // m_flow_work field on bFlowManager
    auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)fm);
    for (auto t = td; t && !s_ffc.fh_flow_work; t = sdk->type_definition->get_parent_type(t))
        s_ffc.fh_flow_work = sdk->type_definition->find_field(t, "m_flow_work");
    if (!s_ffc.fh_flow_work) { s_ffc.failed = true; return false; }

    void* raw = sdk->field->get_data_raw(s_ffc.fh_flow_work, fm, false);
    if (!raw) { s_ffc.failed = true; return false; }
    void* fw = *(void**)raw;
    if (!fw) { s_ffc.failed = true; return false; }

    // _FlowMap field on the flow_work object
    td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)fw);
    for (auto t = td; t && !s_ffc.fh_flow_map; t = sdk->type_definition->get_parent_type(t))
        s_ffc.fh_flow_map = sdk->type_definition->find_field(t, "_FlowMap");
    if (!s_ffc.fh_flow_map) { s_ffc.failed = true; return false; }

    raw = sdk->field->get_data_raw(s_ffc.fh_flow_map, fw, false);
    if (!raw) { s_ffc.failed = true; return false; }
    void* fmap = *(void**)raw;
    if (!fmap) { s_ffc.failed = true; return false; }

    // _ID field on the FlowMap object
    td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)fmap);
    for (auto t = td; t && !s_ffc.fh_id; t = sdk->type_definition->get_parent_type(t))
        s_ffc.fh_id = sdk->type_definition->find_field(t, "_ID");
    if (!s_ffc.fh_id) { s_ffc.failed = true; return false; }

    core_log(SF6CORE_LOG_INFO, nullptr, "flow field handles cached");
    return true;
}

static int32_t poll_flow_id_inner() {
    auto* sdk = s_sdk;
    if (!sdk) return -1;

    // The singleton lives for the whole session: look it up by name only
    // every 60 frames (or while it is missing), not every frame.
    static void* s_fm = nullptr;
    static uint32_t s_fm_age = 0;
    if (!s_fm || ++s_fm_age >= 60) {
        s_fm = sdk->functions->get_managed_singleton("app.bFlowManager");
        s_fm_age = 0;
    }
    auto* fm = s_fm;
    if (!fm) return -1;

    if (resolve_flow_fields()) {
        // Fast path: cached field handles, just pointer loads
        void* raw = sdk->field->get_data_raw(s_ffc.fh_flow_work, fm, false);
        if (!raw) goto slow;
        void* fw = *(void**)raw;
        if (!fw) goto slow;
        raw = sdk->field->get_data_raw(s_ffc.fh_flow_map, fw, false);
        if (!raw) goto slow;
        void* fmap = *(void**)raw;
        if (!fmap) goto slow;
        raw = sdk->field->get_data_raw(s_ffc.fh_id, fmap, false);
        if (!raw) goto slow;
        return *(int32_t*)raw;
    }

slow:
    // Fallback: re-resolve on next frame
    s_ffc.resolved = false;
    s_ffc.failed = false;
    s_fm = nullptr;

    // Original slow path
    void* fw = read_field_ref(sdk, fm, "m_flow_work");
    if (!fw) return -1;
    void* fmap = read_field_ref(sdk, fw, "_FlowMap");
    if (!fmap) return -1;
    int32_t id = -1;
    if (!read_field_i32(sdk, fmap, "_ID", &id)) return -1;
    return id;
}

static int32_t poll_flow_id() {
    if (!s_flow_feature_ok) return s_flow_id;

    int32_t id = -1;
    DWORD exc = 0;
    __try {
        id = poll_flow_id_inner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }
    if (exc) {
        // Type or field missing -- disable flow feature, don't crash
        s_flow_feature_ok = false;
        core_log(SF6CORE_LOG_WARN, nullptr,
                 "flow polling failed (SEH 0x%08X), feature disabled", exc);
        return s_flow_id;
    }
    return id;
}

// ========================================================================
// Client sorting (deterministic: priority ascending, then id byte-wise)
// ========================================================================

static bool client_less(const SF6CoreClient* a, const SF6CoreClient* b) {
    if (a->priority != b->priority) return a->priority < b->priority;
    return strcmp(a->client_id, b->client_id) < 0;
}

static void sort_clients() {
    std::sort(s_clients.begin(), s_clients.end(), client_less);
}

// ========================================================================
// SEH wrappers for client callbacks
// (POD args only inside __try -- no C++ objects needing unwinding)
// ========================================================================

struct FlowCbArgs {
    SF6CoreFlowCb cb;
    int32_t old_id, new_id;
    void* user;
};

static bool invoke_flow_seh(const FlowCbArgs* a, DWORD* out_exc) {
    __try {
        a->cb(a->old_id, a->new_id, a->user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out_exc = GetExceptionCode();
        return false;
    }
}

struct FrameCbArgs {
    SF6CoreFrameCb cb;
    uint64_t frame;
    void* user;
};

static bool invoke_frame_seh(const FrameCbArgs* a, DWORD* out_exc) {
    __try {
        a->cb(a->frame, a->user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out_exc = GetExceptionCode();
        return false;
    }
}

// ========================================================================
// Fault handling for a client
// ========================================================================

void handle_fault(SF6CoreClient* c, DWORD exc_code,
                  const char* phase) {
    c->fault_count++;
    c->total_faults++;

    char detail[128];
    snprintf(detail, sizeof(detail), "SEH 0x%08X in %s (consecutive=%d, total=%d)",
             (unsigned)exc_code, phase, c->fault_count, c->total_faults);

    push_breadcrumb(BC_FAULT, c->client_id, detail);
    core_log(SF6CORE_LOG_ERROR, c->client_id, "%s", detail);
    dump_breadcrumbs();

    const bool retry_failed = c->half_open;
    if (retry_failed || c->fault_count >= MAX_CONSECUTIVE_FAULTS) {
        c->state          = SF6CORE_CLIENT_DISABLED_BY_BREAKER;
        c->half_open      = false;
        c->disabled_until = s_frame + BREAKER_COOLDOWN_FRAMES;
        push_breadcrumb(BC_BREAKER_TRIP, c->client_id,
                        retry_failed ? "half-open retry failed: re-opened"
                                     : "circuit breaker opened");
        core_log(SF6CORE_LOG_ERROR, c->client_id,
                 "circuit breaker %s -- callbacks skipped for %llu frames, "
                 "then one half-open retry",
                 retry_failed ? "re-opened (half-open retry failed)"
                              : "opened after consecutive faults",
                 (unsigned long long)BREAKER_COOLDOWN_FRAMES);
        dump_breadcrumbs();
    }
}

void handle_success(SF6CoreClient* c) {
    if (c->half_open) {
        c->half_open = false;
        push_breadcrumb(BC_BREAKER_TRIP, c->client_id,
                        "half-open retry succeeded: breaker closed");
        core_log(SF6CORE_LOG_INFO, c->client_id,
                 "circuit breaker closed (half-open retry succeeded)");
    }
    c->fault_count = 0;
}

// Called once per frame before dispatch: move cooled-down clients to half-open.
static void update_breakers(const std::vector<SF6CoreClient*>& snapshot) {
    for (SF6CoreClient* c : snapshot) {
        if (!c->active) continue;
        if (c->state != SF6CORE_CLIENT_DISABLED_BY_BREAKER) continue;
        if (s_frame < c->disabled_until) continue;
        c->state       = SF6CORE_CLIENT_ACTIVE;
        c->half_open   = true;
        c->fault_count = 0;
        core_log(SF6CORE_LOG_WARN, c->client_id,
                 "circuit breaker half-open: retrying callbacks");
    }
}

// ========================================================================
// Engine invoke helpers (increment 2 -- POD-only, safe inside __try)
// ========================================================================

bool invoke_c(const REFrameworkSDKData* sdk, REFrameworkMethodHandle mh,
              void* thisptr, void** args, unsigned int argc,
              RawInvokeRet* out) {
    if (!mh) return false;
    memset(out, 0, sizeof(*out));
    auto r = sdk->method->invoke(mh, thisptr, args,
                                 argc * (unsigned)sizeof(void*),
                                 out, sizeof(*out));
    return r == REFRAMEWORK_ERROR_NONE && !out->exception_thrown;
}

bool managed_str_to_utf8(void* str_obj, char* buf, int buf_size) {
    if (!str_obj || buf_size < 1) return false;
    buf[0] = '\0';
    int32_t len = *(int32_t*)((uint8_t*)str_obj + 0x10);
    if (len <= 0) return true;
    const wchar_t* data = (const wchar_t*)((uint8_t*)str_obj + 0x14);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, data, len,
                                    buf, buf_size - 1, nullptr, nullptr);
    if (bytes <= 0) return false;
    buf[bytes] = '\0';
    return true;
}

// Assign a managed-object reference field with the engine's own semantics:
// add_ref(new) -> atomic exchange -> release(old). This is exactly what
// REFramework's Lua set_field does ("This is what Capcom does for all field
// assignments", REFramework/src/mods/bindings/Sdk.cpp:1181-1195).
//
// A plain `*(void**)raw = value` (the previous implementation) leaves the new
// object without the field's reference: the engine may free it before it is
// read, and the next release of the field underflows. That crashed the game
// 3/3 when writing UIAgent._StateManager._Next (2026-10-01, see
// docs/results/WIDGET_CRASH_RCA_2026-10-01.md). The field now owns exactly one
// reference; the engine releases it when it overwrites/clears the field, so
// the core must NOT release it again later.
//
// Exactly as the game's own stores, which test the TARGET first (its compiled
// field stores, e.g. in TrainingMenuFunc.ViewUpdate): a target whose reference
// count is negative is a local object (not refcounted yet, swept with its frame)
// and gets a plain store, no add_ref, no release: its fields own no reference.
// The previous version released the old value of such a field too, a reference
// the field never held: the training pause menu's spin wrote the Data of the
// ViewData the game hands to Function at a page build (a local object) and
// released the option it pointed at once per build, until that option was
// freed while the menu's lists still pointed at it (crashes of 11/10 under fast
// sub-page switching).
bool write_field_managed(const REFrameworkSDKData* sdk, void* obj,
                         const char* name, void* value) {
    if (!obj || !name) return false;
    auto td = sdk->managed_object->get_type_definition(
        (REFrameworkManagedObjectHandle)obj);
    while (td) {
        auto fh = sdk->type_definition->find_field(td, name);
        if (fh) {
            void* raw = sdk->field->get_data_raw(fh, obj, false);
            if (!raw) return false;
            void* volatile* field = (void* volatile*)raw;
            if (*field == value) return true;
            if (*(volatile int32_t*)((uint8_t*)obj + 8) < 0) {    // a local target: the game's plain store
                _InterlockedExchangePointer((PVOID volatile*)field, value);
                return true;
            }
            if (value)
                sdk->managed_object->add_ref((REFrameworkManagedObjectHandle)value);
            void* old = *field;
            while (_InterlockedCompareExchangePointer((PVOID volatile*)field, value, old) != old)
                old = *field;
            if (old)
                sdk->managed_object->release((REFrameworkManagedObjectHandle)old);
            return true;
        }
        td = sdk->type_definition->get_parent_type(td);
    }
    return false;
}

REFrameworkMethodHandle find_method_on(const REFrameworkSDKData* sdk,
                                       void* obj, const char* name) {
    if (!obj || !name) return nullptr;
    auto td = sdk->managed_object->get_type_definition(
        (REFrameworkManagedObjectHandle)obj);
    while (td) {
        auto mh = sdk->type_definition->find_method(td, name);
        if (mh) return mh;
        td = sdk->type_definition->get_parent_type(td);
    }
    return nullptr;
}

// ========================================================================
// Glob matching (simple: * = zero or more characters)
// ========================================================================

static bool glob_match(const char* pat, const char* txt) {
    while (*pat && *txt) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            while (*txt) { if (glob_match(pat, txt)) return true; txt++; }
            return glob_match(pat, txt);
        }
        if (*pat != *txt) return false;
        pat++; txt++;
    }
    while (*pat == '*') pat++;
    return *pat == '\0' && *txt == '\0';
}

// ========================================================================
// Parse slash-separated path into segments
// ========================================================================

static int parse_path(const char* path, char out[][ELEM_PATH_MAX], int max_segs) {
    int n = 0;
    const char* p = path;
    while (*p && n < max_segs) {
        const char* sep = strchr(p, '/');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0 && len < ELEM_PATH_MAX) {
            memcpy(out[n], p, len);
            out[n][len] = '\0';
            n++;
        }
        if (!sep) break;
        p = sep + 1;
    }
    return n;
}

// ========================================================================
// Type/method cache resolution (lazy, once)
// ========================================================================

static bool resolve_type_cache() {
    if (s_tc.resolved) return !s_tc.failed;
    if (s_tc.failed) return false;
    s_tc.resolved = true;

    auto* sdk = s_sdk;
    if (!sdk) { s_tc.failed = true; return false; }

    auto tdb = sdk->functions->get_tdb();
    if (!tdb) { s_tc.failed = true; return false; }

    DWORD exc = 0;
    __try {
        s_tc.t_prefab = sdk->tdb->find_type(tdb, "via.Prefab");
        s_tc.t_vec3   = sdk->tdb->find_type(tdb, "via.vec3");
        s_tc.t_game_object = sdk->tdb->find_type(tdb, "via.GameObject");
        s_tc.t_ui_agent    = sdk->tdb->find_type(tdb, "app.UIAgent");
        s_tc.t_state_show  = sdk->tdb->find_type(tdb, "app.UIAgent.State.Show");
        s_tc.t_state_hide  = sdk->tdb->find_type(tdb, "app.UIAgent.State.Hide");
        s_tc.t_play_object = sdk->tdb->find_type(tdb, "via.gui.PlayObject");

        if (s_tc.t_prefab) {
            s_tc.m_prefab_set_path = sdk->type_definition->find_method(
                s_tc.t_prefab, "set_Path");
            s_tc.m_prefab_instantiate = sdk->type_definition->find_method(
                s_tc.t_prefab, "instantiate");
        }
        if (s_tc.t_game_object) {
            s_tc.m_go_get_component = sdk->type_definition->find_method(
                s_tc.t_game_object, "getComponent(System.Type)");
            if (!s_tc.m_go_get_component)
                s_tc.m_go_get_component = sdk->type_definition->find_method(
                    s_tc.t_game_object, "getComponent");
            s_tc.m_go_destroy = sdk->type_definition->find_method(
                s_tc.t_game_object, "destroy");
            s_tc.m_go_get_name = sdk->type_definition->find_method(
                s_tc.t_game_object, "get_Name");
        }
        if (s_tc.t_play_object) {
            s_tc.m_po_get_child = sdk->type_definition->find_method(
                s_tc.t_play_object, "get_Child");
            s_tc.m_po_get_next = sdk->type_definition->find_method(
                s_tc.t_play_object, "get_Next");
            s_tc.m_po_get_name = sdk->type_definition->find_method(
                s_tc.t_play_object, "get_Name");
            s_tc.m_po_get_position = sdk->type_definition->find_method(
                s_tc.t_play_object, "get_Position");
            s_tc.m_po_set_position = sdk->type_definition->find_method(
                s_tc.t_play_object, "set_Position");
            s_tc.m_po_set_force_invisible = sdk->type_definition->find_method(
                s_tc.t_play_object, "set_ForceInvisible");
            s_tc.m_po_get_force_invisible = sdk->type_definition->find_method(
                s_tc.t_play_object, "get_ForceInvisible");
        }
        // UIAgentManager.Add(app.UIAgent): both working probes called it
        // before forcing the agent state; the two core runs that skipped it
        // crashed in-engine right after the first Show (2026-10-01).
        auto t_uiam = sdk->tdb->find_type(tdb, "app.UIAgentManager");
        if (t_uiam) {
            s_tc.m_uiam_add = sdk->type_definition->find_method(
                t_uiam, "Add(app.UIAgent)");
            if (!s_tc.m_uiam_add)
                s_tc.m_uiam_add = sdk->type_definition->find_method(t_uiam, "Add");
        }
        // Text methods -- resolve from via.gui.Text
        auto t_text = sdk->tdb->find_type(tdb, "via.gui.Text");
        if (t_text) {
            s_tc.m_text_get_message = sdk->type_definition->find_method(
                t_text, "get_Message");
            s_tc.m_text_set_message = sdk->type_definition->find_method(
                t_text, "set_Message");
        }
        // UIAgent methods
        if (s_tc.t_ui_agent) {
            s_tc.m_agent_get_control_main = sdk->type_definition->find_method(
                s_tc.t_ui_agent, "get_ControlMain");
            s_tc.rt_ui_agent = sdk->type_definition->get_runtime_type(
                s_tc.t_ui_agent);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }

    if (exc || !s_tc.t_prefab || !s_tc.m_prefab_instantiate) {
        s_tc.failed = true;
        core_log(SF6CORE_LOG_WARN, nullptr,
                 "type cache resolution %s (SEH=0x%08X)",
                 s_tc.failed ? "failed" : "partial", (unsigned)exc);
        return false;
    }

    core_log(SF6CORE_LOG_INFO, nullptr,
             "type cache resolved (prefab=%s, instantiate=%s, "
             "get_component=%s, position=%s/%s, ForceInvisible=%s/%s)",
             s_tc.t_prefab ? "ok" : "NO",
             s_tc.m_prefab_instantiate ? "ok" : "NO",
             s_tc.m_go_get_component ? "ok" : "NO",
             s_tc.m_po_get_position ? "ok" : "NO",
             s_tc.m_po_set_position ? "ok" : "NO",
             s_tc.m_po_get_force_invisible ? "ok" : "NO",
             s_tc.m_po_set_force_invisible ? "ok" : "NO");
    return true;
}

// ========================================================================
// GUI tree walk: find control by name path under a root
// ========================================================================

// Walk get_Child/get_Next finding a child whose get_Name matches seg (glob).
// Returns the first match, or NULL. All calls SEH-guarded by caller.
static void* find_child_by_name(const REFrameworkSDKData* sdk,
                                void* parent, const char* seg) {
    if (!parent || !s_tc.m_po_get_child || !s_tc.m_po_get_next ||
        !s_tc.m_po_get_name)
        return nullptr;

    RawInvokeRet ret;
    if (!invoke_c(sdk, s_tc.m_po_get_child, parent, nullptr, 0, &ret))
        return nullptr;
    void* child = ret.ptr;

    while (child) {
        // get_Name on child
        RawInvokeRet nr;
        if (invoke_c(sdk, s_tc.m_po_get_name, child, nullptr, 0, &nr) &&
            nr.ptr) {
            char name[ELEM_PATH_MAX];
            if (managed_str_to_utf8(nr.ptr, name, sizeof(name)) &&
                glob_match(seg, name)) {
                return child;
            }
        }
        // get_Next
        if (!invoke_c(sdk, s_tc.m_po_get_next, child, nullptr, 0, &ret))
            break;
        child = ret.ptr;
    }
    return nullptr;
}

// Walk all siblings matching seg, calling fn(matched_control) for each.
// Used by element hiding where a wildcard matches multiple siblings.
static void for_each_child_match(const REFrameworkSDKData* sdk,
                                 void* parent, const char* seg,
                                 void (*fn)(void* ctrl, void* ctx),
                                 void* ctx) {
    if (!parent || !s_tc.m_po_get_child) return;
    RawInvokeRet ret;
    if (!invoke_c(sdk, s_tc.m_po_get_child, parent, nullptr, 0, &ret)) return;
    void* child = ret.ptr;
    while (child) {
        RawInvokeRet nr;
        if (invoke_c(sdk, s_tc.m_po_get_name, child, nullptr, 0, &nr) &&
            nr.ptr) {
            char name[ELEM_PATH_MAX];
            if (managed_str_to_utf8(nr.ptr, name, sizeof(name)) &&
                glob_match(seg, name)) {
                fn(child, ctx);
            }
        }
        if (!invoke_c(sdk, s_tc.m_po_get_next, child, nullptr, 0, &ret)) break;
        child = ret.ptr;
    }
}

// Resolve a full name path (segments) under a root control.
// Returns the final control, or NULL.
static void* resolve_name_path(const REFrameworkSDKData* sdk,
                               void* root,
                               const char segs[][ELEM_PATH_MAX],
                               int n_segs) {
    void* cur = root;
    for (int i = 0; i < n_segs && cur; i++)
        cur = find_child_by_name(sdk, cur, segs[i]);
    return cur;
}

// ========================================================================
// Widget instantiation chain (runs in commit phase, game thread)
// ========================================================================

// Warm-up check (game thread, commit phase). Returns true when the widget
// just became LIVE. No engine writes here: only a field read under SEH.
static bool widget_warmup_tick(SF6CoreWidget* w) {
    if (s_frame < w->warm_until) return false;
    void* sm = nullptr;
    __try {
        sm = read_field_ref(s_sdk, w->ui_agent, "_StateManager");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sm = nullptr;
    }
    if (!sm) {
        if (s_frame > w->warm_until + WIDGET_WARMUP_TIMEOUT) {
            w->warming = false;
            w->state = SF6CORE_WIDGET_FAILED;
            push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "warm-up timeout");
            core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                     "widget warm-up timeout: agent _StateManager still null");
        }
        return false;
    }
    // Register explicitly with UIAgentManager before any state write, exactly
    // like the EXP-3b / EXP-2 probes did (Add(agent), thisptr = singleton).
    bool add_ok = false;
    if (s_tc.m_uiam_add) {
        __try {
            void* uiam = s_sdk->functions->get_managed_singleton("app.UIAgentManager");
            if (uiam) {
                RawInvokeRet ret;
                void* args[] = { w->ui_agent };
                add_ok = invoke_c(s_sdk, s_tc.m_uiam_add, uiam, args, 1, &ret);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            add_ok = false;
        }
    }
    if (!add_ok) {
        w->warming = false;
        w->state = SF6CORE_WIDGET_FAILED;
        push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "UIAgentManager.Add failed");
        core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                 "widget FAILED: UIAgentManager.Add %s -- no state write attempted",
                 s_tc.m_uiam_add ? "invoke failed" : "method not resolved");
        return false;
    }

    w->warming = false;
    w->state = SF6CORE_WIDGET_LIVE;
    w->vis_dirty = true; // trigger initial visibility evaluation
    push_breadcrumb(BC_WIDGET_LIVE, w->owner->client_id, w->prefab_path);
    core_log(SF6CORE_LOG_INFO, w->owner->client_id,
             "widget LIVE after warm-up (frame %llu) go=0x%p agent=0x%p",
             (unsigned long long)s_frame, w->game_object, w->ui_agent);
    return true;
}

static void process_pending_widget(SF6CoreWidget* w) {
    auto* sdk = s_sdk;
    if (!sdk || !resolve_type_cache()) {
        w->state = SF6CORE_WIDGET_FAILED;
        return;
    }
    if (w->warming) {            // already instantiated, waiting for start
        widget_warmup_tick(w);
        return;
    }
    DWORD exc = 0;

    // A1: create resource
    if (!w->resource) {
        __try {
            auto rm = sdk->functions->get_resource_manager();
            w->resource = sdk->resource_manager->create_resource(
                rm, "via.PrefabResource", w->prefab_path);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
        if (!w->resource) {
            core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                     "widget A1 create_resource failed (SEH=0x%08X)", (unsigned)exc);
            w->state = SF6CORE_WIDGET_FAILED;
            push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "A1");
            return;
        }
    }

    // A2: create holder
    if (!w->holder) {
        exc = 0;
        __try {
            w->holder = (void*)sdk->resource->create_holder(
                (REFrameworkResourceHandle)w->resource,
                "via.PrefabResourceHolder");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
        if (!w->holder) {
            core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                     "widget A2 create_holder failed (SEH=0x%08X)", (unsigned)exc);
            w->state = SF6CORE_WIDGET_FAILED;
            push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "A2");
            return;
        }
    }

    // A4+A5: create Prefab instance, set Path
    if (!w->prefab_mo) {
        exc = 0;
        __try {
            w->prefab_mo = (void*)sdk->type_definition->create_instance(
                s_tc.t_prefab, 0);
            if (w->prefab_mo && s_tc.m_prefab_set_path) {
                // get_ResourcePath from holder, set on prefab
                auto get_rp = sdk->type_definition->find_method(
                    (REFrameworkTypeDefinitionHandle)
                    sdk->managed_object->get_type_definition(
                        (REFrameworkManagedObjectHandle)w->holder),
                    "get_ResourcePath");
                void* rp_val = nullptr;
                if (get_rp) {
                    RawInvokeRet ret;
                    if (invoke_c(sdk, get_rp, w->holder, nullptr, 0, &ret))
                        rp_val = ret.ptr;
                }
                if (rp_val) {
                    RawInvokeRet ret;
                    void* args[] = { rp_val };
                    invoke_c(sdk, s_tc.m_prefab_set_path, w->prefab_mo,
                             args, 1, &ret);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
        if (!w->prefab_mo) {
            core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                     "widget A4/A5 failed (SEH=0x%08X)", (unsigned)exc);
            w->state = SF6CORE_WIDGET_FAILED;
            push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "A4");
            return;
        }
    }

    // A7: instantiate (may return null if resource not loaded; retry)
    if (!w->game_object) {
        exc = 0;
        __try {
            static struct { float x, y, z; } s_v3 = {};
            void* v3arg = (void*)&s_v3;
            // Try create_instance for vec3
            if (s_tc.t_vec3) {
                auto mo = sdk->type_definition->create_instance(s_tc.t_vec3, 0);
                if (mo) v3arg = (void*)mo;
            }
            RawInvokeRet ret;
            void* args[] = { v3arg };
            if (invoke_c(sdk, s_tc.m_prefab_instantiate, w->prefab_mo,
                         args, 1, &ret))
                w->game_object = ret.ptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
        if (!w->game_object) {
            w->retry_count++;
            if (w->retry_count >= WIDGET_MAX_RETRIES) {
                core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                         "widget A7 instantiate failed after %d retries",
                         w->retry_count);
                w->state = SF6CORE_WIDGET_FAILED;
                push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "A7-timeout");
            }
            return; // retry next frame
        }
    }

    // A8: add_ref
    if (!w->ref_held) {
        exc = 0;
        __try {
            sdk->managed_object->add_ref(
                (REFrameworkManagedObjectHandle)w->game_object);
            w->ref_held = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
    }

    // A9: release resource
    if (w->resource) {
        __try {
            sdk->resource->release((REFrameworkResourceHandle)w->resource);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        w->resource = nullptr;
    }

    // A10: getComponent(app.UIAgent)
    if (!w->ui_agent && s_tc.m_go_get_component && s_tc.rt_ui_agent) {
        exc = 0;
        __try {
            RawInvokeRet ret;
            void* args[] = { (void*)s_tc.rt_ui_agent };
            if (invoke_c(sdk, s_tc.m_go_get_component, w->game_object,
                         args, 1, &ret))
                w->ui_agent = ret.ptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
        }
        if (!w->ui_agent) {
            core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                     "widget A10 getComponent(UIAgent) null");
            w->state = SF6CORE_WIDGET_FAILED;
            push_breadcrumb(BC_WIDGET_FAIL, w->owner->client_id, "A10");
            return;
        }
    }

    // Instantiated: warm up for a few frames before any write (see struct).
    w->warming    = true;
    w->warm_until = s_frame + WIDGET_WARMUP_FRAMES;
    core_log(SF6CORE_LOG_INFO, w->owner->client_id,
             "widget instantiated go=0x%p agent=0x%p path=%s -- warming up %llu frames",
             w->game_object, w->ui_agent, w->prefab_path,
             (unsigned long long)WIDGET_WARMUP_FRAMES);
}

// ========================================================================
// Widget commit: agent state (Show/Hide via _StateManager._Next)
// ========================================================================

static void widget_apply_agent_state(SF6CoreWidget* w, bool show) {
    if (!w->ui_agent) return;
    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        void* sm = read_field_ref(sdk, w->ui_agent, "_StateManager");
        if (!sm) return;
        auto td = show ? s_tc.t_state_show : s_tc.t_state_hide;
        if (!td) return;
        auto inst = sdk->type_definition->create_instance(td, 0);
        if (inst)
            write_field_managed(sdk, sm, "_Next", (void*)inst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                 "widget agent state SEH 0x%08X", (unsigned)exc);
    }
    w->was_shown = show;
}

// Evaluate effective visibility: client_visible AND flow context
static bool widget_effective_visible(SF6CoreWidget* w) {
    if (!w->client_visible) return false;
    if (w->n_flow_ids == 0) return true; // all flows
    for (uint32_t i = 0; i < w->n_flow_ids; i++)
        if (w->flow_ids[i] == s_flow_id) return true;
    return false;
}

// ========================================================================
// Widget commit: text (compare-before-write, R7)
// ========================================================================

static void widget_apply_texts(SF6CoreWidget* w) {
    if (w->ptext_n == 0 || !w->ui_agent) return;
    auto* sdk = s_sdk;

    // Get root control
    void* root = nullptr;
    DWORD exc = 0;
    __try {
        if (s_tc.m_agent_get_control_main) {
            RawInvokeRet ret;
            if (invoke_c(sdk, s_tc.m_agent_get_control_main, w->ui_agent,
                         nullptr, 0, &ret))
                root = ret.ptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }
    if (!root) { w->ptext_n = 0; return; }

    for (int i = 0; i < w->ptext_n; i++) {
        auto& pt = w->ptexts[i];
        // Check cache
        void* ctrl = nullptr;
        for (int j = 0; j < w->tcache_n; j++) {
            if (strcmp(w->tcache[j].path, pt.path) == 0) {
                ctrl = w->tcache[j].control;
                break;
            }
        }
        // Resolve if not cached
        if (!ctrl) {
            char segs[WIDGET_SEG_MAX][ELEM_PATH_MAX];
            int ns = parse_path(pt.path, segs, WIDGET_SEG_MAX);
            exc = 0;
            __try { ctrl = resolve_name_path(sdk, root, segs, ns); }
            __except (EXCEPTION_EXECUTE_HANDLER) { exc = GetExceptionCode(); }
            if (ctrl && w->tcache_n < 32) {
                strncpy(w->tcache[w->tcache_n].path, pt.path, WIDGET_PATH_MAX - 1);
                w->tcache[w->tcache_n].path[WIDGET_PATH_MAX - 1] = '\0';
                w->tcache[w->tcache_n].control = ctrl;
                w->tcache_n++;
            }
        }
        if (!ctrl) continue;

        // R7: compare-before-write
        exc = 0;
        __try {
            RawInvokeRet ret;
            char cur[WIDGET_TEXT_MAX] = {};
            if (s_tc.m_text_get_message &&
                invoke_c(sdk, s_tc.m_text_get_message, ctrl, nullptr, 0, &ret)
                && ret.ptr) {
                managed_str_to_utf8(ret.ptr, cur, sizeof(cur));
            }
            if (strcmp(cur, pt.text) != 0) {
                auto* str = sdk->functions->create_managed_string_normal(pt.text);
                if (str && s_tc.m_text_set_message) {
                    void* args[] = { (void*)str };
                    invoke_c(sdk, s_tc.m_text_set_message, ctrl, args, 1, &ret);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
            core_log(SF6CORE_LOG_ERROR, w->owner->client_id,
                     "widget text SEH 0x%08X path=%s", (unsigned)exc, pt.path);
        }
    }
    w->ptext_n = 0;
}

// ========================================================================
// Widget commit: position (EXPERIMENTAL -- via.gui get/set_Position)
// ========================================================================

static void widget_apply_position(SF6CoreWidget* w) {
    if (!w->pos_dirty || !w->ui_agent) return;
    w->pos_dirty = false;
    if (!s_tc.m_po_get_position || !s_tc.m_po_set_position) {
        core_log(SF6CORE_LOG_WARN, w->owner->client_id,
                 "widget_set_position: Position methods not resolved "
                 "(EXPERIMENTAL feature)");
        return;
    }
    auto* sdk = s_sdk;
    void* root = nullptr;
    DWORD exc = 0;
    __try {
        if (s_tc.m_agent_get_control_main) {
            RawInvokeRet ret;
            if (invoke_c(sdk, s_tc.m_agent_get_control_main, w->ui_agent,
                         nullptr, 0, &ret))
                root = ret.ptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { exc = GetExceptionCode(); }
    if (!root) return;

    __try {
        RawInvokeRet pos;
        if (invoke_c(sdk, s_tc.m_po_get_position, root, nullptr, 0, &pos)) {
            float* px = (float*)pos.bytes;
            float* py = (float*)(pos.bytes + 4);
            if (*px != w->pend_x || *py != w->pend_y) {
                *px = w->pend_x;
                *py = w->pend_y;
                RawInvokeRet dummy;
                void* args[] = { (void*)pos.bytes };
                invoke_c(sdk, s_tc.m_po_set_position, root, args, 1, &dummy);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        core_log(SF6CORE_LOG_WARN, w->owner->client_id,
                 "widget position SEH 0x%08X (EXPERIMENTAL)", (unsigned)exc);
    }
}

// ========================================================================
// Widget destruction
// ========================================================================

static void do_destroy_widget(SF6CoreWidget* w) {
    auto* sdk = s_sdk;
    if (!sdk) return;
    DWORD exc = 0;

    // Release managed ref
    if (w->game_object && w->ref_held) {
        __try {
            sdk->managed_object->release(
                (REFrameworkManagedObjectHandle)w->game_object);
        } __except (EXCEPTION_EXECUTE_HANDLER) { exc = GetExceptionCode(); }
        w->ref_held = false;
    }
    // via.GameObject.destroy (auto-unregisters from UIAgentManager)
    if (w->game_object && s_tc.m_go_destroy) {
        __try {
            RawInvokeRet ret;
            void* args[] = { w->game_object };
            invoke_c(sdk, s_tc.m_go_destroy, nullptr, args, 1, &ret);
        } __except (EXCEPTION_EXECUTE_HANDLER) { exc = GetExceptionCode(); }
    }
    // Release resource if still held
    if (w->resource) {
        __try {
            sdk->resource->release((REFrameworkResourceHandle)w->resource);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    push_breadcrumb(BC_WIDGET_DESTROY, w->owner->client_id, w->prefab_path);
    core_log(SF6CORE_LOG_INFO, w->owner->client_id, "widget destroyed");
    w->active = false;
}

// ========================================================================
// Element hiding: draw callback = read-only observer (rendering thread)
// Commit phase (LateUpdateBehavior) = all writes (game thread)
//
// Threading: the draw callback runs on the rendering thread (Hooks.cpp:827
// gui_draw_hook_internal, hooked on GUI::draw). It must NOT write GUI
// state (R1). It only resolves owner names and records sightings.
// ========================================================================

// Sighting table: draw callback writes, commit phase reads.
// Fixed-size, lock-free: the draw callback writes atomically per slot;
// the commit phase reads after the draw callback has returned (same frame
// boundary guaranteed by the engine's update/render pipeline ordering:
// LateUpdateBehavior runs BEFORE the render pass that calls gui_draw).
// Because LateUpdateBehavior runs first, the sightings read in the commit
// phase are from the PREVIOUS frame. That is correct: we see an element
// drawn last frame, then apply our state before the engine draws again.
static constexpr int SIGHT_SLOTS = 256;
struct Sighting {
    char     owner[ELEM_OWNER_MAX];
    void*    view;         // via.gui.View* seen last draw
    uint64_t frame_seen;   // s_frame at which the draw callback recorded it
};
static Sighting s_sightings[SIGHT_SLOTS] = {};
static std::atomic<int> s_sight_count{0}; // how many valid entries

// Per-rule resolved-element state (managed in commit phase only).
struct ResolvedElem {
    void*     control;       // resolved via.gui control
    bool      orig_invisible; // original ForceInvisible before we touched it
    bool      applied;        // we wrote set_ForceInvisible
    uint64_t  resolved_frame; // when we resolved this
};
static constexpr int RESOLVED_MAX = 128;
static ResolvedElem s_resolved[RESOLVED_MAX] = {};
static int s_resolved_n = 0;

// Track when we last did a full re-resolve (frame number).
static uint64_t s_elem_last_resolve = 0;
static constexpr int ELEM_REVALIDATE_INTERVAL = 20; // frames

// ---- Lock helpers (no __try in these functions) ----

// Rendering thread: is any rule declared for this owner? try_lock only.
static bool render_owner_declared(const char* owner_name) {
    // Check zen rules first (uses its own SRWLOCK)
    if (discovery_zen_owner_declared(owner_name)) return true;
    std::unique_lock<std::mutex> lk(s_hide_mutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;   // skip this draw, never stall
    for (const auto& r : s_elem_hides)
        if (strcmp(r.gui_owner, owner_name) == 0) return true;
    return false;
}

// Rendering thread: upsert a sighting keyed by (owner name, view address).
// Multiple drawn instances of the same owner name each get their own slot.
// On overflow, the oldest sighting is dropped. try_lock only.
static void render_record_sighting(const char* owner_name, void* view) {
    std::unique_lock<std::mutex> lk(s_hide_mutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    const uint64_t f = s_frame_pub.load(std::memory_order_relaxed);
    int cnt = s_sight_count.load(std::memory_order_relaxed);
    // Update existing slot with same (owner, view)
    for (int i = 0; i < cnt; i++) {
        if (s_sightings[i].view == view &&
            strcmp(s_sightings[i].owner, owner_name) == 0) {
            s_sightings[i].frame_seen = f;
            return;
        }
    }
    // New slot
    if (cnt < SIGHT_SLOTS) {
        auto& s = s_sightings[cnt];
        strncpy(s.owner, owner_name, ELEM_OWNER_MAX - 1);
        s.owner[ELEM_OWNER_MAX - 1] = '\0';
        s.view = view;
        s.frame_seen = f;
        s_sight_count.store(cnt + 1, std::memory_order_relaxed);
    } else {
        // Full: drop the oldest sighting (lowest frame_seen)
        int oldest = 0;
        for (int i = 1; i < cnt; i++)
            if (s_sightings[i].frame_seen < s_sightings[oldest].frame_seen) oldest = i;
        auto& s = s_sightings[oldest];
        strncpy(s.owner, owner_name, ELEM_OWNER_MAX - 1);
        s.owner[ELEM_OWNER_MAX - 1] = '\0';
        s.view = view;
        s.frame_seen = f;
    }
}

// Game thread (commit phase): copy rules + sightings under the lock so that
// all engine work afterwards runs on private snapshots, lock released.
struct SightSnap {
    char     owner[ELEM_OWNER_MAX];
    void*    view;
    uint64_t frame_seen;
};
static std::vector<ElementHideRule> s_rules_snap;      // capacity reused
static SightSnap                    s_sight_snap[SIGHT_SLOTS] = {};
static int                          s_sight_snap_n = 0;

static void snapshot_hide_state() {
    std::lock_guard<std::mutex> lk(s_hide_mutex);
    s_rules_snap = s_elem_hides;   // reuses capacity after the first copy
    int cnt = s_sight_count.load(std::memory_order_relaxed);
    for (int i = 0; i < cnt; i++) {
        memcpy(s_sight_snap[i].owner, s_sightings[i].owner, ELEM_OWNER_MAX);
        s_sight_snap[i].view       = s_sightings[i].view;
        s_sight_snap[i].frame_seen = s_sightings[i].frame_seen;
    }
    s_sight_snap_n = cnt;
}

static void clear_sightings() {
    std::lock_guard<std::mutex> lk(s_hide_mutex);
    s_sight_count.store(0, std::memory_order_relaxed);
}

// on_pre_gui_draw_element: READ-ONLY observer (rendering thread).
// Resolves the owner name, records {owner, view, frame} into sighting
// table. No GUI writes, no tree walks. Always returns true.
static bool on_pre_gui_draw_element(void* element, void* /*context*/) {
    if (s_closing.load(std::memory_order_relaxed)) return true;
    if (s_elem_count_pub.load(std::memory_order_acquire) == 0 &&
        s_zen_rule_count_pub.load(std::memory_order_acquire) == 0) return true;
    if (!s_sdk) return true;
    int64_t pt_draw = perf::begin();

    // Name cache is touched only on this thread; the game thread requests a
    // flush through an atomic flag instead of clearing it directly.
    if (s_name_cache_flush.exchange(false, std::memory_order_acq_rel))
        memset(s_name_cache, 0, sizeof(s_name_cache));

    auto* sdk = s_sdk;

    // Resolve owner name via cache
    uintptr_t addr = (uintptr_t)element;
    int slot = (int)((addr >> 4) & (NAME_CACHE_BUCKETS - 1));   // elements are 16-byte aligned: drop the always-zero bits
    auto& ce = s_name_cache[slot];
    const char* owner_name = nullptr;

    if (ce.valid && ce.addr == addr) {
        __try {
            uintptr_t cur_owner = *(uintptr_t*)((uint8_t*)element + 0x10);
            if (cur_owner == ce.owner_ptr)
                owner_name = ce.name;
            else
                ce.valid = false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ce.valid = false;
        }
    }

    if (!owner_name) {
        __try {
            auto m_getgo = find_method_on(sdk, element, "get_GameObject");
            if (m_getgo) {
                RawInvokeRet ret;
                if (invoke_c(sdk, m_getgo, element, nullptr, 0, &ret) &&
                    ret.ptr) {
                    void* go = ret.ptr;
                    if (s_tc.m_go_get_name) {
                        RawInvokeRet nr;
                        if (invoke_c(sdk, s_tc.m_go_get_name, go, nullptr, 0,
                                     &nr) && nr.ptr) {
                            managed_str_to_utf8(nr.ptr, ce.name, sizeof(ce.name));
                            ce.addr = addr;
                            ce.owner_ptr =
                                *(uintptr_t*)((uint8_t*)element + 0x10);
                            ce.valid = true;
                            owner_name = ce.name;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (!owner_name || !owner_name[0]) { perf::end(perf::DrawElement, pt_draw); return true; }

    // Check if any rule targets this owner (locked helper, try_lock)
    if (!render_owner_declared(owner_name)) { perf::end(perf::DrawElement, pt_draw); return true; }

    // Get the View (read-only: we store the pointer but don't write through it)
    void* view = nullptr;
    __try {
        auto m_get_view = find_method_on(sdk, element, "get_View");
        if (m_get_view) {
            RawInvokeRet ret;
            if (invoke_c(sdk, m_get_view, element, nullptr, 0, &ret))
                view = ret.ptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!view) { perf::end(perf::DrawElement, pt_draw); return true; }

    // Record sighting (locked helper, try_lock, bounded by SIGHT_SLOTS).
    render_record_sighting(owner_name, view);
    perf::end(perf::DrawElement, pt_draw);
    return true; // never block drawing
}

// ---------- Commit-phase element hiding (game thread, LateUpdateBehavior) ----------

// Find the snapshot sighting for a given owner (taken by snapshot_hide_state
// this frame). Returns nullptr if not seen within the last 2 frames.
static SightSnap* find_sighting(const char* owner) {
    for (int i = 0; i < s_sight_snap_n; i++) {
        if (strcmp(s_sight_snap[i].owner, owner) == 0 &&
            s_frame - s_sight_snap[i].frame_seen <= 2)
            return &s_sight_snap[i];
    }
    return nullptr;
}

// Look up or add a ResolvedElem slot by control address.
static ResolvedElem* find_or_add_resolved(void* ctrl) {
    for (int i = 0; i < s_resolved_n; i++)
        if (s_resolved[i].control == ctrl) return &s_resolved[i];
    if (s_resolved_n >= RESOLVED_MAX) return nullptr;
    auto* re = &s_resolved[s_resolved_n++];
    re->control = ctrl;
    re->orig_invisible = false;
    re->applied = false;
    re->resolved_frame = s_frame;
    return re;
}

// Apply or restore ForceInvisible on a single control. Compare-before-write.
static void apply_force_invisible(const REFrameworkSDKData* sdk,
                                  void* ctrl, bool want_hidden,
                                  ResolvedElem* re) {
    if (!s_tc.m_po_get_force_invisible || !s_tc.m_po_set_force_invisible)
        return;
    DWORD exc = 0;
    __try {
        RawInvokeRet ret;
        bool cur = false;
        if (invoke_c(sdk, s_tc.m_po_get_force_invisible, ctrl,
                     nullptr, 0, &ret))
            cur = (ret.dword != 0);

        // On first touch, save original value
        if (re && !re->applied) {
            re->orig_invisible = cur;
            re->resolved_frame = s_frame;
        }

        // Compare-before-write
        if ((bool)want_hidden != cur) {
            void* args[] = { (void*)(uintptr_t)(want_hidden ? 1 : 0) };
            invoke_c(sdk, s_tc.m_po_set_force_invisible, ctrl, args, 1, &ret);
        }
        if (re) re->applied = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        core_log(SF6CORE_LOG_ERROR, nullptr,
                 "apply_force_invisible SEH 0x%08X", (unsigned)exc);
    }
}

// Main commit-phase element-hide processing.
// Called once per frame from commit_phase().
// Design: only (re)apply when the view pointer changed, a flow change
// happened, a rule changed, or the revalidation tick fires.
static bool s_elem_rules_changed = false;
static bool s_elem_flow_changed  = false;

// Lock helper: remove restore_pending rules from s_elem_hides (no __try here).
static void compact_restored_hides() {
    std::lock_guard<std::mutex> lk(s_hide_mutex);
    auto it = s_elem_hides.begin();
    while (it != s_elem_hides.end()) {
        if (it->restore_pending)
            it = s_elem_hides.erase(it);
        else
            ++it;
    }
    s_elem_count_pub.store((int)s_elem_hides.size(), std::memory_order_release);
}

static void commit_element_hides() {
    if (s_elem_count_pub.load(std::memory_order_acquire) == 0) return;
    auto* sdk = s_sdk;
    if (!sdk || !resolve_type_cache()) return;

    bool need_resolve =
        s_elem_rules_changed ||
        s_elem_flow_changed ||
        (s_frame - s_elem_last_resolve >= ELEM_REVALIDATE_INTERVAL);

    if (!need_resolve) return;
    s_elem_rules_changed = false;
    s_elem_flow_changed  = false;
    s_elem_last_resolve  = s_frame;

    // Copy shared state under the lock; all engine work below runs on the
    // private snapshots with the lock released.
    snapshot_hide_state();

    for (auto& rule : s_rules_snap) {
        if (rule.n_segments == 0) continue;
        // Safe state check: look up client by id under registry lock
        if (!rule.restore_pending) {
            int32_t cst;
            bool suspended;
            query_client_state_and_suspended(rule.client_id, &cst, &suspended);
            if (cst == SF6CORE_CLIENT_DISABLED_BY_BREAKER || cst == -1) continue;
            if (suspended) continue;
        }

        SightSnap* sight = find_sighting(rule.gui_owner);
        if (!sight) continue; // GUI not drawn recently, skip

        void* view = sight->view;
        if (!view) continue;

        DWORD exc = 0;
        __try {
            // Resolve the path under the sighted view
            if (rule.n_segments == 1) {
                // Fan-out on the single segment
                for_each_child_match(sdk, view, rule.segments[0],
                    [](void* ctrl, void* ctx) {
                        bool hidden = *(bool*)ctx;
                        auto* re = find_or_add_resolved(ctrl);
                        apply_force_invisible(s_sdk, ctrl, hidden, re);
                    }, (void*)&rule.hidden);
            } else {
                // Walk interior segments (first match only), fan-out on last
                void* parent = view;
                for (int i = 0; i < rule.n_segments - 1 && parent; i++)
                    parent = find_child_by_name(sdk, parent, rule.segments[i]);
                if (parent) {
                    for_each_child_match(sdk, parent,
                        rule.segments[rule.n_segments - 1],
                        [](void* ctrl, void* ctx) {
                            bool hidden = *(bool*)ctx;
                            auto* re = find_or_add_resolved(ctrl);
                            apply_force_invisible(s_sdk, ctrl, hidden, re);
                        }, (void*)&rule.hidden);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            exc = GetExceptionCode();
            core_log(SF6CORE_LOG_ERROR, nullptr,
                     "commit_element_hides SEH 0x%08X owner=%s",
                     (unsigned)exc, rule.gui_owner);
        }
    }

    // Deactivate restore_pending rules (lock helper outside __try)
    compact_restored_hides();
}

// Restore all hides for a specific client: set ForceInvisible back to
// original value for each resolved control, then drop the rules.
// Called from api_unregister_client (deferred via pend_destroy for widgets;
// for element hides, we mark for restore -- actual restore runs in next
// commit_element_hides via the hidden=false + rules_changed path).
static void restore_client_hides(SF6CoreClient* client) {
    int n_marked = 0;
    {
        std::lock_guard<std::mutex> lk(s_hide_mutex);
        for (auto& r : s_elem_hides) {
            if (r.owner == client) {
                r.hidden = false;
                r.restore_pending = true;
                r.owner = nullptr;  // null out before client is freed
                n_marked++;
            }
        }
    }
    if (n_marked > 0) {
        s_elem_rules_changed = true;
        core_log(SF6CORE_LOG_INFO, client->client_id,
                 "marking %d element rule(s) for restore+deactivate", n_marked);
    }
}

// Called on flow change (game thread): invalidate sightings and resolved
// elements. The render-thread name cache is flushed via an atomic request.
static void elem_hides_on_flow_change() {
    clear_sightings();
    s_resolved_n = 0;
    s_elem_flow_changed = true;
    s_name_cache_flush.store(true, std::memory_order_release);
}

// ========================================================================
// Commit phase (runs AFTER client callbacks -- THE GUI write pass)
// ========================================================================

static void commit_phase() {
    // -- Widget instantiation --
    {
        int64_t pt_w = perf::begin();
        for (auto* w : s_widgets) {
            if (!w->active) continue;
            if (w->owner->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER) {
                // Hide breaker-disabled widgets
                if (w->state == SF6CORE_WIDGET_LIVE && w->was_shown) {
                    widget_apply_agent_state(w, false);
                }
                continue;
            }
            if (w->state == SF6CORE_WIDGET_PENDING)
                process_pending_widget(w);
            if (w->state != SF6CORE_WIDGET_LIVE) continue;

            // Apply visibility
            if (w->vis_dirty) {
                bool eff = widget_effective_visible(w);
                if (eff != w->was_shown)
                    widget_apply_agent_state(w, eff);
                w->vis_dirty = false;
            }

            // Apply position
            widget_apply_position(w);

            // Apply text
            widget_apply_texts(w);

            // Destroy if requested
            if (w->pend_destroy) {
                do_destroy_widget(w);
            }
        }
        perf::end(perf::CommitWidgets, pt_w);
    }

    // -- Element hiding (all writes here, game thread) --
    {
        int64_t pt_eh = perf::begin();
        commit_element_hides();
        perf::end(perf::CommitElemHides, pt_eh);
    }

    // -- ZenMode element hiding (per-frame, all-segment fan-out) --
    if (s_zen_rule_count_pub.load(std::memory_order_acquire) > 0) {
        int64_t pt_zh = perf::begin();
        // Pass sighting snapshot to the discovery module
        snapshot_hide_state();
        // Build flat arrays from the sight snapshot for discovery
        static const void*  zen_views[SIGHT_SLOTS];
        static uint64_t     zen_frames[SIGHT_SLOTS];
        static char         zen_owners[SIGHT_SLOTS][128];
        for (int i = 0; i < s_sight_snap_n; i++) {
            memcpy(zen_owners[i], s_sight_snap[i].owner, 128);
            zen_views[i] = s_sight_snap[i].view;
            zen_frames[i] = s_sight_snap[i].frame_seen;
        }
        discovery_commit_zen_hides(s_frame,
                                    zen_owners, zen_views, zen_frames,
                                    s_sight_snap_n);
        perf::end(perf::CommitZenHides, pt_zh);
    }
}

// Invalidate widget text caches and mark visibility dirty
// (called on flow change).
static void widgets_on_flow_change() {
    for (auto* w : s_widgets) {
        if (!w->active) continue;
        w->tcache_n = 0;
        w->vis_dirty = true;
    }
    // Element-hide name cache belongs to the rendering thread: request a
    // flush instead of clearing it from the game thread.
    s_name_cache_flush.store(true, std::memory_order_release);
}

// ========================================================================
// Dispatch (LateUpdateBehavior callback -- THE single commit point)
// ========================================================================

// Reusable dispatch list -- avoids per-frame heap allocation.
// After the first frame, operator= reuses the existing buffer
// (source size <= destination capacity => no reallocation).
static std::vector<SF6CoreClient*> s_dispatch;

// Display API implementations (defined in display.cpp)
SF6CoreStatus SF6CORE_CALL api_ui_set_text(SF6CoreClient*, void*, const char*);
SF6CoreStatus SF6CORE_CALL api_ui_set_message_id(SF6CoreClient*, void*, const uint8_t[16]);
SF6CoreStatus SF6CORE_CALL api_ui_get_message_id(void*, uint8_t[16]);
SF6CoreStatus SF6CORE_CALL api_ui_set_font_size(SF6CoreClient*, void*, float, float);
SF6CoreStatus SF6CORE_CALL api_ui_set_region_size(SF6CoreClient*, void*, float, float);
SF6CoreStatus SF6CORE_CALL api_ui_set_color(SF6CoreClient*, void*, uint32_t);
SF6CoreStatus SF6CORE_CALL api_ui_texture_load(SF6CoreClient*, const char*, void**);
int32_t       SF6CORE_CALL api_ui_texture_ready(void*);
SF6CoreStatus SF6CORE_CALL api_ui_set_texture(SF6CoreClient*, void*, void*);
SF6CoreStatus SF6CORE_CALL api_ui_set_visible_disp(SF6CoreClient*, void*, int32_t);
SF6CoreStatus SF6CORE_CALL api_ui_set_force_invisible(SF6CoreClient*, void*, int32_t);
SF6CoreStatus SF6CORE_CALL api_ui_set_position_disp(SF6CoreClient*, void*, float, float, float);
SF6CoreStatus SF6CORE_CALL api_ui_set_scale(SF6CoreClient*, void*, float, float, float);
SF6CoreStatus SF6CORE_CALL api_ui_set_size(SF6CoreClient*, void*, float, float);
SF6CoreStatus SF6CORE_CALL api_ui_set_block_input(SF6CoreClient*, void*, int32_t);
SF6CoreStatus SF6CORE_CALL api_ui_clone_row(SF6CoreClient*, const SF6CoreRowCloneDesc*, void**);
SF6CoreStatus SF6CORE_CALL api_obj_invoke(SF6CoreClient*, void*, const char*, const SF6CoreArg*, uint32_t, SF6CoreArg*);
void*         SF6CORE_CALL api_obj_new(SF6CoreClient*, const char*);
void*         SF6CORE_CALL api_obj_clone(SF6CoreClient*, void*);
SF6CoreStatus SF6CORE_CALL api_obj_set_i64(SF6CoreClient*, void*, const char*, int64_t);
SF6CoreStatus SF6CORE_CALL api_obj_set_f64(SF6CoreClient*, void*, const char*, double);
SF6CoreStatus SF6CORE_CALL api_obj_set_string(SF6CoreClient*, void*, const char*, const char*);
SF6CoreStatus SF6CORE_CALL api_obj_set_ref(SF6CoreClient*, void*, const char*, void*);
SF6CoreStatus SF6CORE_CALL api_obj_set_color(SF6CoreClient*, void*, const char*, uint32_t);
SF6CoreStatus SF6CORE_CALL api_list_add(SF6CoreClient*, void*, void*);
SF6CoreStatus SF6CORE_CALL api_list_set_item(SF6CoreClient*, void*, int32_t, void*);
void*         SF6CORE_CALL api_list_get_range(SF6CoreClient*, void*, int32_t, int32_t);
SF6CoreStatus SF6CORE_CALL api_dict_add(SF6CoreClient*, void*, int64_t, void*);
int32_t       SF6CORE_CALL api_dict_contains(void*, int64_t);
void*         SF6CORE_CALL api_ui_find_child(void*, const char*, int32_t);
void*         SF6CORE_CALL api_ui_find_agent(const char*);
SF6CoreStatus SF6CORE_CALL api_get_display_stats(SF6CoreClient*, SF6CoreDisplayStats*);
// Discovery API (defined in discovery.cpp)
uint32_t      SF6CORE_CALL api_ui_agents(const char*, void**, uint32_t);
uint32_t      SF6CORE_CALL api_ui_agents_gen(const char*);
uint32_t      SF6CORE_CALL api_ui_registry_gen(void);
void*         SF6CORE_CALL api_ui_find_cached(void*, const char*);
uint32_t      SF6CORE_CALL api_ui_find_many(void*, const char* const*, uint32_t, void**);
SF6CoreStatus SF6CORE_CALL api_set_element_hidden_zen(SF6CoreClient*, const char*,
                                                       const char* const*, uint32_t, int32_t);
// Increment 5
int32_t       SF6CORE_CALL api_client_state_by_id(const char*);
SF6CoreStatus SF6CORE_CALL api_lua_register_fn(SF6CoreClient*, const char*, SF6CoreLuaFn, void*);
SF6CoreStatus SF6CORE_CALL api_set_element_rules_suspended(SF6CoreClient*, int32_t);
// Increment 6: native option dialogs (defined in options/options_service.cpp)
void options_frame();
void options_pump();
SF6CoreStatus SF6CORE_CALL api_opt_set_callbacks(SF6CoreClient*, const SF6CoreOptCallbacks*);
int64_t       SF6CORE_CALL api_opt_group(SF6CoreClient*, const char*, const char*, const char*);
SF6CoreStatus SF6CORE_CALL api_opt_group_def(SF6CoreClient*, int64_t, const char*, const char*);
SF6CoreStatus SF6CORE_CALL api_opt_remove_group(SF6CoreClient*, int64_t);
int64_t       SF6CORE_CALL api_opt_entry(SF6CoreClient*, int64_t, const SF6CoreOptEntryDesc*);
SF6CoreStatus SF6CORE_CALL api_opt_rebuild(SF6CoreClient*);
SF6CoreStatus SF6CORE_CALL api_opt_ensure_built(SF6CoreClient*);
SF6CoreStatus SF6CORE_CALL api_opt_open_menu(SF6CoreClient*, const char*, int32_t);
SF6CoreStatus SF6CORE_CALL api_opt_navigate(SF6CoreClient*, const char*, int32_t, int32_t);
SF6CoreStatus SF6CORE_CALL api_opt_close(SF6CoreClient*);
int32_t       SF6CORE_CALL api_opt_window_open(void);
void*         SF6CORE_CALL api_opt_current_param(void);
void*         SF6CORE_CALL api_opt_parent_param(void);
int64_t       SF6CORE_CALL api_opt_focused_entry(void);
void*         SF6CORE_CALL api_opt_row_control(int64_t, void*);
void*         SF6CORE_CALL api_opt_row_part(int64_t);
SF6CoreStatus SF6CORE_CALL api_opt_set_option_text(SF6CoreClient*, int64_t, int32_t, const char*);
uint32_t      SF6CORE_CALL api_opt_status(char*, uint32_t);
// Increment 7: effective selection (defined in selection.cpp)
SF6CoreStatus SF6CORE_CALL api_sel_set_effective(SF6CoreClient*, int32_t, int32_t, int32_t, int32_t);
int32_t       SF6CORE_CALL api_sel_get_effective(int32_t, int32_t, int32_t*, int32_t*);
// Increment 8: training pause menu (defined in pausemenu/pm_service.cpp)
SF6CoreStatus SF6CORE_CALL api_pm_set_callbacks(SF6CoreClient*, const SF6CorePmCallbacks*);
int64_t       SF6CORE_CALL api_pm_page(SF6CoreClient*, const SF6CorePmPageDesc*);
int64_t       SF6CORE_CALL api_pm_row(SF6CoreClient*, int64_t, const SF6CorePmRowDesc*);
SF6CoreStatus SF6CORE_CALL api_pm_set_texts(SF6CoreClient*, int64_t, const char*, const char*);
SF6CoreStatus SF6CORE_CALL api_pm_set_options(SF6CoreClient*, int64_t, const char* const*, uint32_t);
SF6CoreStatus SF6CORE_CALL api_pm_remove(SF6CoreClient*, int64_t);
int32_t       SF6CORE_CALL api_pm_page_shown(int64_t);
void*         SF6CORE_CALL api_pm_row_part(int64_t);
uint32_t      SF6CORE_CALL api_pm_status(char*, uint32_t);
int64_t       SF6CORE_CALL api_pm_row_of(SF6CoreClient*, void*);
// Increment 9: GUI material texture (ADR-0019, display.cpp)
SF6CoreStatus SF6CORE_CALL api_ui_set_material_texture(SF6CoreClient*, void*, const char*, void*);
SF6CoreStatus SF6CORE_CALL api_ui_get_material_texture(SF6CoreClient*, void*, const char*, void**);
// Increment 10: texture rectangle, texture state, plain clone (ADR-0020 amendment 1, display.cpp)
SF6CoreStatus SF6CORE_CALL api_ui_set_texture_rect(SF6CoreClient*, void*, void*, float, float, float, float);
SF6CoreStatus SF6CORE_CALL api_ui_get_texture_state(SF6CoreClient*, void*, SF6CoreTextureState*);
SF6CoreStatus SF6CORE_CALL api_ui_clone_control(SF6CoreClient*, void*, void*, const char*, void**);
// Increment 11: option rows in a game Options section (ADR-0021, options/options_service.cpp)
SF6CoreStatus SF6CORE_CALL api_opt_group_place(SF6CoreClient*, int64_t, int32_t, int32_t);

// ========================================================================
// Closing guard: window message callback (0.7.2)
// ========================================================================

static bool on_message(void* /*hwnd*/, unsigned int msg,
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
    if (!s_closing.exchange(true, std::memory_order_seq_cst)) {
        s_close_frame = s_frame;
        s_close_final = final_msg;
        core_log(SF6CORE_LOG_INFO, nullptr,
                 "closing: the game window got %s, the core stops"
                 " (no client frames, no display)", name);
    } else if (final_msg && !s_close_final) {
        s_close_final = true;
    }
    return true;
}

static void on_late_update() {
    s_frame++;
    s_frame_pub.store(s_frame, std::memory_order_relaxed);
    perf::frame();
    int64_t pt_outer = perf::begin();

    // Closing guard: stop all game work. A WM_CLOSE that was not followed by
    // WM_DESTROY may be cancelled if frames keep coming for 30 s.
    if (s_closing.load(std::memory_order_relaxed)) {
        if (!s_close_final && s_close_frame > 0 &&
            (s_frame - s_close_frame) > 60u * 30) {
            s_closing.store(false, std::memory_order_relaxed);
            s_close_frame = 0;
            core_log(SF6CORE_LOG_INFO, nullptr,
                     "closing: cancelled, the core resumes");
        } else {
            perf::end(perf::OnLateUpdate, pt_outer);
            return;
        }
    }

    // Record the game thread ID on the first frame
    static bool s_thread_recorded = false;
    if (!s_thread_recorded) {
        display_set_game_thread(GetCurrentThreadId());
        s_thread_recorded = true;
    }

    // -- 1. Poll flow --
    int64_t pt_poll = perf::begin();
    int32_t new_flow = poll_flow_id();
    perf::end(perf::PollFlow, pt_poll);

    // -- 2. Snapshot the client list under the lock --
    {
        std::lock_guard<std::mutex> lk(s_registry_mutex);
        s_dispatch = s_clients;
    }
    // From here, NO lock held. Safe to call client code.
    const auto& snapshot = s_dispatch;
    int64_t pt_brk = perf::begin();
    update_breakers(snapshot);
    perf::end(perf::UpdateBreakers, pt_brk);


    // -- 3. Flow-change callbacks --
    if (new_flow != s_flow_id && new_flow != -1) {
        int64_t pt_flow = perf::begin();
        int32_t old_flow = s_flow_id;
        s_flow_id = new_flow;

        char detail[64];
        snprintf(detail, sizeof(detail), "%d -> %d", old_flow, new_flow);
        push_breadcrumb(BC_FLOW_CHANGE, nullptr, detail);
        core_log(SF6CORE_LOG_INFO, nullptr,
                 "flow change: %d -> %d", old_flow, new_flow);

        // Re-evaluate widget visibility and invalidate caches
        widgets_on_flow_change();
        elem_hides_on_flow_change();

        for (SF6CoreClient* c : snapshot) {
            if (!c->active) continue;
            if (c->state != SF6CORE_CLIENT_ACTIVE) continue;
            if (!c->flow_cb) continue;

            FlowCbArgs args = {c->flow_cb, old_flow, new_flow, c->flow_user};
            DWORD exc = 0;
            if (!invoke_flow_seh(&args, &exc)) {
                handle_fault(c, exc, "flow_cb");
            } else {
                handle_success(c);
            }
        }
        perf::end(perf::FlowCallbacks, pt_flow);
    }

    // -- 3b. Option dialogs (increment 6): before the clients' frames, as the
    //        mod that owned the engine ran it at the start of its frame --
    {
        int64_t pt_opt = perf::begin();
        options_frame();
        perf::end(perf::OptionsFrame, pt_opt);
    }

    // -- 3c. Training pause menu (increment 8): installed on demand, then its
    //        state (training, pause, menu close) and the install pass --
    pausemenu_frame();

    // -- 4. Frame callbacks --
    {
        int64_t pt_fc = perf::begin();
        for (SF6CoreClient* c : snapshot) {
            if (!c->active) continue;
            if (c->state != SF6CORE_CLIENT_ACTIVE) continue;
            if (!c->frame_cb) continue;

            perf::Id cid = perf::client_id(c->client_id);
            int64_t pt_c = perf::begin();
            FrameCbArgs args = {c->frame_cb, s_frame, c->frame_user};
            DWORD exc = 0;
            if (!invoke_frame_seh(&args, &exc)) {
                handle_fault(c, exc, "frame_cb");
            } else {
                handle_success(c);
            }
            perf::end(cid, pt_c);
        }
        perf::end(perf::FrameCallbacks, pt_fc);
    }

    // -- 5. Commit phase (all GUI writes) --
    {
        int64_t pt_cp = perf::begin();
        commit_phase();
        perf::end(perf::CommitPhase, pt_cp);
    }

    perf::end(perf::OnLateUpdate, pt_outer);
}

// ========================================================================
// API function implementations
// ========================================================================

static const char* SF6CORE_CALL api_get_version(void) {
    return CORE_VERSION;
}

static SF6CoreStatus SF6CORE_CALL api_register_client(
        const SF6CoreClientDesc* desc, SF6CoreClient** out) {
    if (!desc || !out) return SF6CORE_ERR_INVALID_ARG;
    if (desc->struct_size < sizeof(SF6CoreClientDesc))
        return SF6CORE_ERR_INVALID_ARG;
    if (!desc->client_id || desc->client_id[0] == '\0')
        return SF6CORE_ERR_INVALID_ARG;

    auto* c = new SF6CoreClient{};
    strncpy(c->client_id, desc->client_id, CLIENT_ID_MAX);
    c->client_id[CLIENT_ID_MAX] = '\0';
    if (desc->client_version) {
        strncpy(c->client_version, desc->client_version, CLIENT_VER_MAX);
        c->client_version[CLIENT_VER_MAX] = '\0';
    }
    c->priority    = desc->priority;
    c->flow_cb     = nullptr;
    c->flow_user   = nullptr;
    c->frame_cb    = nullptr;
    c->frame_user  = nullptr;
    c->fault_count = 0;
    c->active      = true;

    {
        std::lock_guard<std::mutex> lk(s_registry_mutex);

        // Check for duplicate ID
        auto dup_check = [&](const std::vector<SF6CoreClient*>& v) {
            for (const auto* x : v)
                if (x->active && strcmp(x->client_id, c->client_id) == 0)
                    return true;
            return false;
        };
        if (dup_check(s_clients) || dup_check(s_pending)) {
            delete c;
            return SF6CORE_ERR_DUPLICATE_ID;
        }

        if (s_initialized.load(std::memory_order_acquire)) {
            c->state = SF6CORE_CLIENT_ACTIVE;
            s_clients.push_back(c);
            sort_clients();
        } else {
            c->state = SF6CORE_CLIENT_PENDING_INIT;
            s_pending.push_back(c);
        }
    }

    push_breadcrumb(BC_REGISTER, c->client_id,
                    s_initialized.load() ? "active" : "pending");
    core_log(SF6CORE_LOG_INFO, c->client_id,
             "registered (priority=%d, version=%s, state=%s)",
             c->priority, c->client_version,
             s_initialized.load() ? "active" : "pending_init");

    *out = c;
    return SF6CORE_OK;
}

static void SF6CORE_CALL api_unregister_client(SF6CoreClient* client) {
    if (!client) return;

    // Mark client-owned widgets for deferred destruction (commit phase)
    for (auto* w : s_widgets) {
        if (w->active && w->owner == client)
            w->pend_destroy = true;
    }
    // Mark client-owned hides for deferred restore (draw callback)
    restore_client_hides(client);
    // Zen hides
    discovery_restore_client_zen(client);
    // Bridge native functions
    bridge_unregister_client_fns(client->client_id);

    {
        std::lock_guard<std::mutex> lk(s_registry_mutex);
        client->active = false;
        auto it = std::find(s_clients.begin(), s_clients.end(), client);
        if (it != s_clients.end()) s_clients.erase(it);
        auto it2 = std::find(s_pending.begin(), s_pending.end(), client);
        if (it2 != s_pending.end()) s_pending.erase(it2);
    }
    // Training pause menu (increment 8): its callbacks gone, its pages / rows out of
    // the menu at the next pass
    pausemenu_release_client(client);

    push_breadcrumb(BC_UNREGISTER, client->client_id, nullptr);
    core_log(SF6CORE_LOG_INFO, client->client_id, "unregistered");
    // Intentionally not deleted -- handle may still be in a dispatch snapshot.
    // Memory is small and one-time.
}

static SF6CoreStatus SF6CORE_CALL api_set_flow_callback(
        SF6CoreClient* client, SF6CoreFlowCb cb, void* user) {
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    client->flow_cb   = cb;
    client->flow_user = user;
    return SF6CORE_OK;
}

static SF6CoreStatus SF6CORE_CALL api_set_frame_callback(
        SF6CoreClient* client, SF6CoreFrameCb cb, void* user) {
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    client->frame_cb   = cb;
    client->frame_user = user;
    return SF6CORE_OK;
}

static int32_t SF6CORE_CALL api_get_flow_id(void) {
    return s_flow_id;
}

static void SF6CORE_CALL api_log(
        SF6CoreClient* client, SF6CoreLogLevel level, const char* msg) {
    const char* cid = (client && client->client_id[0])
                    ? client->client_id : nullptr;
    core_log(level, cid, "%s", msg ? msg : "");
}

static SF6CoreClientState SF6CORE_CALL api_get_client_state(
        SF6CoreClient* client) {
    if (!client) return SF6CORE_CLIENT_PENDING_INIT;
    return client->state;
}

// ========================================================================
// Increment 5: client_state_by_id, lua_register_fn, set_element_rules_suspended
// ========================================================================

// Safe client state + suspended query under registry mutex.
void query_client_state_and_suspended(const char* client_id,
                                       int32_t* out_state, bool* out_suspended) {
    *out_state = -1;
    *out_suspended = false;
    if (!client_id) return;
    std::lock_guard<std::mutex> lk(s_registry_mutex);
    for (auto* c : s_clients)
        if (c->active && strcmp(c->client_id, client_id) == 0) {
            *out_state = (int32_t)c->state;
            *out_suspended = c->rules_suspended;
            return;
        }
    for (auto* c : s_pending)
        if (c->active && strcmp(c->client_id, client_id) == 0) {
            *out_state = (int32_t)c->state;
            *out_suspended = c->rules_suspended;
            return;
        }
}

// Safe client state query: reads active+state under the registry mutex,
// returns -1 if not found. No pointer escapes the lock.
int32_t query_client_state_by_id(const char* client_id) {
    if (!client_id) return -1;
    std::lock_guard<std::mutex> lk(s_registry_mutex);
    for (auto* c : s_clients)
        if (c->active && strcmp(c->client_id, client_id) == 0)
            return (int32_t)c->state;
    for (auto* c : s_pending)
        if (c->active && strcmp(c->client_id, client_id) == 0)
            return (int32_t)c->state;
    return -1;
}

// Safe handle_fault by client id: re-looks up the client under the lock.
// If the client was unregistered between the call and now, skip silently.
void handle_fault_by_id(const char* client_id, DWORD exc_code, const char* phase) {
    if (!client_id) return;
    SF6CoreClient* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_registry_mutex);
        for (auto* x : s_clients)
            if (x->active && strcmp(x->client_id, client_id) == 0) { c = x; break; }
    }
    if (c) handle_fault(c, exc_code, phase);
}

int32_t SF6CORE_CALL api_client_state_by_id(const char* client_id) {
    return query_client_state_by_id(client_id);
}

SF6CoreStatus SF6CORE_CALL api_lua_register_fn(SF6CoreClient* client, const char* name,
                                                SF6CoreLuaFn fn, void* user) {
    if (!client || !name || name[0] == '\0') return SF6CORE_ERR_INVALID_ARG;
    if (strlen(name) > 63) return SF6CORE_ERR_INVALID_ARG;
    bridge_register_native_fn(client, name, fn, user);
    return SF6CORE_OK;
}

SF6CoreStatus SF6CORE_CALL api_set_element_rules_suspended(SF6CoreClient* client, int32_t suspended) {
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    if (!is_game_thread()) return SF6CORE_ERR_WRONG_THREAD;
    client->rules_suspended = (suspended != 0);
    if (!suspended) {
        // Resuming: trigger re-apply on next frame
        s_elem_rules_changed = true;
    }
    return SF6CORE_OK;
}

// ========================================================================
// Increment 2: API function implementations (widgets + element hiding)
// ========================================================================

static SF6CoreStatus SF6CORE_CALL api_create_widget(
        SF6CoreClient* client, const SF6CoreWidgetDesc* desc,
        SF6CoreWidget** out) {
    if (!client || !desc || !out) return SF6CORE_ERR_INVALID_ARG;
    if (desc->struct_size < sizeof(SF6CoreWidgetDesc))
        return SF6CORE_ERR_INVALID_ARG;
    if (!desc->prefab_path || desc->prefab_path[0] == '\0')
        return SF6CORE_ERR_INVALID_ARG;
    if (client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER)
        return SF6CORE_ERR_DISABLED;

    auto* w = new SF6CoreWidget{};
    w->owner = client;
    w->state = SF6CORE_WIDGET_PENDING;
    w->active = true;
    strncpy(w->prefab_path, desc->prefab_path, WIDGET_PATH_MAX - 1);
    w->prefab_path[WIDGET_PATH_MAX - 1] = '\0';
    uint32_t nf = desc->n_flow_ids;
    if (nf > 32) nf = 32;
    for (uint32_t i = 0; i < nf; i++) w->flow_ids[i] = desc->flow_ids[i];
    w->n_flow_ids = nf;
    w->pos_x = desc->initial_x;
    w->pos_y = desc->initial_y;
    w->pend_x = desc->initial_x;
    w->pend_y = desc->initial_y;
    w->pos_dirty = (desc->initial_x != 0.0f || desc->initial_y != 0.0f);
    w->client_visible = (desc->initially_visible != 0);
    w->vis_dirty = true;
    w->was_shown = false;
    w->tcache_n = 0;
    w->ptext_n = 0;
    w->pend_destroy = false;

    s_widgets.push_back(w);
    push_breadcrumb(BC_WIDGET_CREATE, client->client_id, desc->prefab_path);
    core_log(SF6CORE_LOG_INFO, client->client_id,
             "widget created (pending): %s", desc->prefab_path);

    *out = w;
    return SF6CORE_OK;
}

static SF6CoreWidgetState SF6CORE_CALL api_get_widget_state(
        SF6CoreWidget* widget) {
    if (!widget) return SF6CORE_WIDGET_FAILED;
    return widget->state;
}

static SF6CoreStatus SF6CORE_CALL api_widget_set_text(
        SF6CoreWidget* widget, const char* element_path,
        const char* utf8_text) {
    if (!widget || !element_path || !utf8_text) return SF6CORE_ERR_INVALID_ARG;
    if (!widget->active) return SF6CORE_ERR_DISABLED;
    if (widget->state == SF6CORE_WIDGET_FAILED) return SF6CORE_ERR_DISABLED;
    if (widget->ptext_n >= 16) return SF6CORE_ERR_INVALID_ARG; // queue full

    auto& pt = widget->ptexts[widget->ptext_n++];
    strncpy(pt.path, element_path, WIDGET_PATH_MAX - 1);
    pt.path[WIDGET_PATH_MAX - 1] = '\0';
    strncpy(pt.text, utf8_text, WIDGET_TEXT_MAX - 1);
    pt.text[WIDGET_TEXT_MAX - 1] = '\0';
    return SF6CORE_OK;
}

static SF6CoreStatus SF6CORE_CALL api_widget_set_visible(
        SF6CoreWidget* widget, int32_t visible) {
    if (!widget) return SF6CORE_ERR_INVALID_ARG;
    if (!widget->active) return SF6CORE_ERR_DISABLED;
    widget->client_visible = (visible != 0);
    widget->vis_dirty = true;
    return SF6CORE_OK;
}

static SF6CoreStatus SF6CORE_CALL api_widget_set_position(
        SF6CoreWidget* widget, float x, float y) {
    if (!widget) return SF6CORE_ERR_INVALID_ARG;
    if (!widget->active) return SF6CORE_ERR_DISABLED;
    widget->pend_x = x;
    widget->pend_y = y;
    widget->pos_dirty = true;
    return SF6CORE_OK;
}

static SF6CoreStatus SF6CORE_CALL api_destroy_widget(
        SF6CoreWidget* widget) {
    if (!widget) return SF6CORE_ERR_INVALID_ARG;
    if (!widget->active) return SF6CORE_ERR_NOT_FOUND;
    widget->pend_destroy = true;
    return SF6CORE_OK;
}

static SF6CoreStatus SF6CORE_CALL api_set_element_hidden(
        SF6CoreClient* client, const char* gui_owner_name,
        const char* element_path, int32_t hidden) {
    if (!client || !gui_owner_name || !element_path)
        return SF6CORE_ERR_INVALID_ARG;
    if (core_closing()) return SF6CORE_ERR_DISABLED;
    if (gui_owner_name[0] == '\0' || element_path[0] == '\0')
        return SF6CORE_ERR_INVALID_ARG;

    // Parse the path into segments
    char segs[WIDGET_SEG_MAX][ELEM_PATH_MAX];
    int n_segs = parse_path(element_path, segs, WIDGET_SEG_MAX);
    if (n_segs == 0) return SF6CORE_ERR_INVALID_ARG;

    {
        std::lock_guard<std::mutex> lk(s_hide_mutex);

        // Check for existing entry (same client + owner + path)
        for (auto& r : s_elem_hides) {
            if (r.owner != client) continue;
            if (strcmp(r.gui_owner, gui_owner_name) != 0) continue;
            if (r.n_segments != n_segs) continue;
            bool same = true;
            for (int i = 0; i < n_segs; i++) {
                if (strcmp(r.segments[i], segs[i]) != 0) { same = false; break; }
            }
            if (same) {
                r.hidden = (hidden != 0);
                s_elem_rules_changed = true;
                return SF6CORE_OK;
            }
        }

        // New entry
        ElementHideRule rule = {};
        rule.owner = client;
        strncpy(rule.client_id, client->client_id, CLIENT_ID_MAX);
        rule.client_id[CLIENT_ID_MAX] = '\0';
        rule.restore_pending = false;
        strncpy(rule.gui_owner, gui_owner_name, ELEM_OWNER_MAX - 1);
        rule.gui_owner[ELEM_OWNER_MAX - 1] = '\0';
        rule.n_segments = n_segs;
        for (int i = 0; i < n_segs; i++) {
            strncpy(rule.segments[i], segs[i], ELEM_PATH_MAX - 1);
            rule.segments[i][ELEM_PATH_MAX - 1] = '\0';
        }
        rule.hidden = (hidden != 0);
        s_elem_hides.push_back(rule);
        s_elem_count_pub.store((int)s_elem_hides.size(), std::memory_order_release);
    }
    s_elem_rules_changed = true;

    push_breadcrumb(BC_ELEM_HIDE, client->client_id, element_path);
    core_log(SF6CORE_LOG_INFO, client->client_id,
             "element %s: %s/%s", hidden ? "hide" : "unhide",
             gui_owner_name, element_path);
    return SF6CORE_OK;
}

// ========================================================================
// Static API table (lives for the lifetime of the DLL -- never moves)
// ========================================================================

static const SF6CoreAPI s_api = {
    SF6CORE_ABI_VERSION,
    sizeof(SF6CoreAPI),
    api_get_version,
    api_register_client,
    api_unregister_client,
    api_set_flow_callback,
    api_set_frame_callback,
    api_get_flow_id,
    api_log,
    api_get_client_state,
    // -- Increment 2 --
    api_create_widget,
    api_get_widget_state,
    api_widget_set_text,
    api_widget_set_visible,
    api_widget_set_position,
    api_destroy_widget,
    api_set_element_hidden,
    // -- Increment 3: Display API --
    api_ui_set_text,
    api_ui_set_message_id,
    api_ui_get_message_id,
    api_ui_set_font_size,
    api_ui_set_region_size,
    api_ui_set_color,
    api_ui_texture_load,
    api_ui_texture_ready,
    api_ui_set_texture,
    api_ui_set_visible_disp,
    api_ui_set_force_invisible,
    api_ui_set_position_disp,
    api_ui_set_scale,
    api_ui_set_size,
    api_ui_set_block_input,
    api_ui_clone_row,
    api_obj_invoke,
    api_obj_new,
    api_obj_clone,
    api_obj_set_i64,
    api_obj_set_f64,
    api_obj_set_string,
    api_obj_set_ref,
    api_obj_set_color,
    api_list_add,
    api_list_set_item,
    api_list_get_range,
    api_dict_add,
    api_dict_contains,
    api_ui_find_child,
    api_ui_find_agent,
    api_get_display_stats,
    // -- Increment 4: Discovery (ADR-0012) --
    api_ui_agents,
    api_ui_agents_gen,
    api_ui_registry_gen,
    api_ui_find_cached,
    api_ui_find_many,
    api_set_element_hidden_zen,
    // -- Increment 5: Bridge extensions + rules suspend --
    api_client_state_by_id,
    api_lua_register_fn,
    api_set_element_rules_suspended,
    // -- Increment 6: native option dialogs (ADR-0013) --
    api_opt_set_callbacks,
    api_opt_group,
    api_opt_group_def,
    api_opt_remove_group,
    api_opt_entry,
    api_opt_rebuild,
    api_opt_ensure_built,
    api_opt_open_menu,
    api_opt_navigate,
    api_opt_close,
    api_opt_window_open,
    api_opt_current_param,
    api_opt_parent_param,
    api_opt_focused_entry,
    api_opt_row_control,
    api_opt_row_part,
    api_opt_set_option_text,
    api_opt_status,
    // -- Increment 7: effective selection (ADR-0014) --
    api_sel_set_effective,
    api_sel_get_effective,
    // -- Increment 8: training pause menu (ADR-0016) --
    api_pm_set_callbacks,
    api_pm_page,
    api_pm_row,
    api_pm_set_texts,
    api_pm_set_options,
    api_pm_remove,
    api_pm_page_shown,
    api_pm_row_part,
    api_pm_status,
    api_pm_row_of,
    // Increment 9
    api_ui_set_material_texture,
    api_ui_get_material_texture,
    // Increment 10
    api_ui_set_texture_rect,
    api_ui_get_texture_state,
    api_ui_clone_control,
    // Increment 11
    api_opt_group_place
};

// Accessor for the static API pointer (used by lua_bridge.cpp)
const SF6CoreAPI* sf6core_api_ptr() { return &s_api; }

// ========================================================================
// Exported entry: sf6core_get_api
// ========================================================================

extern "C" __declspec(dllexport)
const SF6CoreAPI* SF6CORE_CALL sf6core_get_api(uint32_t abi_version) {
    if (abi_version != SF6CORE_ABI_VERSION) return nullptr;
    if (kill_switch_active()) {
        // Log once (best effort -- REFramework API may not be ready)
        core_log(SF6CORE_LOG_WARN, nullptr,
                 "kill switch active (disabled.txt), returning NULL");
        return nullptr;
    }
    return &s_api;
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

// The 1 ms timer the game's 60 fps limiter needs. Windows 11 can ignore a process's timer requests even with
// its window in the foreground: the sessions stuck at 45 fps were sessions where it ignored the game's (and a
// request made at start, ours included), and the limiter slept on the 15.6 ms default tick. A request Windows
// ignored stays ignored; a new kernel request made later is honoured. During the game's first minute the
// timer requests of the game, Media Foundation (the movies) and the audio made it ignored again several
// times. So a background thread checks every second how long Sleep(1) takes in this process (about 1-2 ms
// when the request is honoured, 15.6 when not) and asks the kernel again only then. Nothing released,
// Windows does it at process exit.
using NtSetTimerResolution_t = LONG (NTAPI*)(ULONG desired, BOOLEAN set, PULONG current);
static NtSetTimerResolution_t s_nt_set_timer = nullptr;

// This process's kernel request, made again: released, then set (timeBeginPeriod only counts a period the
// process already holds and does not reach the kernel)
static void renew_timer_request() {
    ULONG cur = 0;
    s_nt_set_timer(10000, FALSE, &cur);
    s_nt_set_timer(10000, TRUE, &cur);
}

// Shortest of three Sleep(1) in a row, in ms. A first Sleep(1) ends on a timer tick, so each of the three
// after it lasts a whole tick (alone, a Sleep(1) lasts anything up to 15.6 ms when the request is ignored).
static double sleep1_ms() {
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    Sleep(1);
    double best = 1e9;
    for (int i = 0; i < 3; ++i) {
        QueryPerformanceCounter(&a);
        Sleep(1);
        QueryPerformanceCounter(&b);
        double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (ms < best) best = ms;
    }
    return best;
}

static void timer_watch() {
    constexpr double kHonouredMs = 5.0;       // Sleep(1) under this: the 1 ms timer is honoured
    constexpr unsigned kMaxLogged = 20;
    unsigned renewed = 0;
    double ms = sleep1_ms();
    core_log(SF6CORE_LOG_INFO, nullptr, "timer: Sleep(1) takes %.1f ms at start (%s)", ms,
             ms < kHonouredMs ? "1 ms timer honoured" : "Windows ignores the 1 ms request");
    for (;;) {
        if (ms >= kHonouredMs) {
            renew_timer_request();
            double after = sleep1_ms();
            if (++renewed <= kMaxLogged)
                core_log(after < kHonouredMs ? SF6CORE_LOG_INFO : SF6CORE_LOG_WARN, nullptr,
                         "timer: Windows was ignoring the game's 1 ms timer (Sleep(1) took %.1f ms): asked again, "
                         "now %.1f ms%s", ms, after, renewed == kMaxLogged ? " (not logged any more)" : "");
        }
        Sleep(1000);
        ms = sleep1_ms();
    }
}

static void request_timer_resolution() {
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;

    char root[MAX_PATH];
    resolve_game_root(root, sizeof(root));
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\reframework\\data\\sf6core\\timer_off.txt", root);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        core_log(SF6CORE_LOG_INFO, nullptr, "timer: not requested (timer_off.txt present)");
        return;
    }
    // Windows' documented opt-out (always honour this process's timer requests); alone it did not stop the
    // 45 fps sessions, the check below does
    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask   = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));

    // Our count in winmm: a timeEndPeriod of the game's cannot take the period away
    MMRESULT rc = timeBeginPeriod(1);
    if (rc != TIMERR_NOERROR) {
        core_log(SF6CORE_LOG_WARN, nullptr, "timer: timeBeginPeriod(1) failed (%u)", (unsigned)rc);
        return;
    }
    core_log(SF6CORE_LOG_INFO, nullptr,
             "timer: 1 ms resolution requested, checked every second (the game's 60 fps limiter needs it; "
             "reframework/data/sf6core/timer_off.txt turns this off)");
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    s_nt_set_timer = ntdll ? (NtSetTimerResolution_t)GetProcAddress(ntdll, "NtSetTimerResolution") : nullptr;
    if (!s_nt_set_timer) {
        core_log(SF6CORE_LOG_WARN, nullptr, "timer: NtSetTimerResolution not found, no check");
        return;
    }
    std::thread(timer_watch).detach();
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->sdk) return false;

    // Kill switch check (before any real work)
    if (kill_switch_active()) {
        // Try to log via REFramework even though we're refusing to init
        if (param->functions->log_warn)
            param->functions->log_warn(
                "%skill switch active -- core disabled", TAG);
        return true; // return true so REFramework doesn't FreeLibrary us
    }

    // Initialize REFramework API wrapper
    try { reframework::API::initialize(param); } catch (...) { return false; }
    s_sdk = param->sdk;

    // Open log file, then write what was logged before (the clients' registrations)
    open_log();
    flush_early_log();
    core_log(SF6CORE_LOG_INFO, nullptr,
             "initializing (version=%s, ABI=%d)", CORE_VERSION,
             SF6CORE_ABI_VERSION);
    request_timer_resolution();

    // Replay pending registrations and flip to "initialized" in the SAME
    // critical section: a registration racing with init either lands in
    // s_pending before the replay, or sees s_initialized == true and goes
    // straight to s_clients -- it can never be lost in between.
    // (Dispatch only iterates s_clients and only starts once the
    // LateUpdateBehavior callback below is registered.)
    {
        std::lock_guard<std::mutex> lk(s_registry_mutex);
        for (auto* c : s_pending) {
            c->state = SF6CORE_CLIENT_ACTIVE;
            s_clients.push_back(c);
            core_log(SF6CORE_LOG_INFO, c->client_id,
                     "pending registration activated");
        }
        s_pending.clear();
        sort_clients();
        s_initialized.store(true, std::memory_order_release);
    }

    // Register LateUpdateBehavior callback (THE single commit point)
    param->functions->on_pre_application_entry(
        "LateUpdateBehavior", on_late_update);
    core_log(SF6CORE_LOG_INFO, nullptr,
             "registered on_pre_application_entry(LateUpdateBehavior)");

    // Register draw-element callback for element hiding (increment 2). The member only exists from plugin API
    // 1.14 on: an older REFramework (1.5.8 = 1.10) has no such entry, the pointer would be read past its table.
    const bool has_draw_cb = param->version &&
        (param->version->major > 1 || (param->version->major == 1 && param->version->minor >= 14));
    if (has_draw_cb && param->functions->on_pre_gui_draw_element) {
        param->functions->on_pre_gui_draw_element(on_pre_gui_draw_element);
        s_elem_draw_registered = true;
        core_log(SF6CORE_LOG_INFO, nullptr,
                 "registered on_pre_gui_draw_element (element hiding)");
    } else {
        core_log(SF6CORE_LOG_WARN, nullptr,
                 "on_pre_gui_draw_element needs plugin API >= 1.14 (have %d.%d): element hiding disabled",
                 param->version ? param->version->major : 0, param->version ? param->version->minor : 0);
    }

    // Register window message callback (closing guard, 0.7.2)
    if (param->functions->on_message) {
        param->functions->on_message(on_message);
        core_log(SF6CORE_LOG_INFO, nullptr,
                 "registered on_message (closing guard)");
    } else {
        core_log(SF6CORE_LOG_WARN, nullptr,
                 "on_message not available (closing guard disabled)");
    }

    // Initialize Lua bridge (increment 3)
    lua_bridge_init(param->functions);
    display_log_init();

    // Initialize discovery service (increment 4)
    discovery_log_init();

    // Option dialogs (increment 6): the event pump before the frame is drawn
    // (the hooks are installed later, on the first frame after a client asks)
    param->functions->on_pre_application_entry("BeginRendering", options_pump);

    push_breadcrumb(BC_INIT, nullptr, "core initialized");

    core_log(SF6CORE_LOG_INFO, nullptr, "init complete, %zu client(s)",
             s_clients.size());
    return true;
}

// ========================================================================
// DllMain
// ========================================================================

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        if (s_log_file) {
            fclose(s_log_file);
            s_log_file = nullptr;
        }
    }
    return TRUE;
}
