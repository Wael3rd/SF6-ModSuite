// display.cpp -- SF6 Native UI Core: Display API (increment 3)
//
// Immediate-mode UI write primitives. Every call goes through the guard chain:
//   1. kill switch / breaker  -> ERR_DISABLED
//   2. proof-of-life          -> ERR_TYPE
//   3. quarantine             -> ERR_FAULTED
//   4. compare-before-write   -> OK (skipped_identical++)
//   5. engine call under SEH  -> ERR_FAULTED on fault
//   6. managed ref fields     -> add_ref/exchange/release (R34)
//
// Threading: ui_* writes require the game thread (ERR_WRONG_THREAD).
//            obj_*/list_*/dict_* are allowed on any thread a hook runs on.
//
// Build: compiled into sf6core_native.dll alongside sf6core.cpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "sf6core_internal.h"
#include "perf.hpp"

#pragma warning(push, 0)
#include <reframework/API.hpp>
#pragma warning(pop)

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ========================================================================
// Game thread ID management
// ========================================================================

static std::atomic<DWORD> s_game_thread_id{0};
static bool               s_game_thread_ever_set = false;

void display_set_game_thread(DWORD tid) {
    s_game_thread_id.store(tid, std::memory_order_release);
    s_game_thread_ever_set = true;
}

bool is_game_thread() {
    DWORD expected = s_game_thread_id.load(std::memory_order_acquire);
    if (expected == 0) return true; // not yet recorded -> accept
    return GetCurrentThreadId() == expected;
}

bool on_recorded_game_thread() {
    DWORD expected = s_game_thread_id.load(std::memory_order_acquire);
    return expected != 0 && GetCurrentThreadId() == expected;
}

// ========================================================================
// Per-client display stats
// ========================================================================

struct DisplayStatsInternal {
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> skipped_identical{0};
    std::atomic<uint64_t> refused{0};
    std::atomic<uint64_t> faults{0};
    std::atomic<uint64_t> quarantined{0};
};

// Attached to SF6CoreClient in a side table (no struct change)
static constexpr int MAX_CLIENTS = 64;
static DisplayStatsInternal s_dstats[MAX_CLIENTS];
static SF6CoreClient*       s_dstats_clients[MAX_CLIENTS] = {};
static int                  s_dstats_count = 0;
static std::mutex           s_dstats_mutex;

static DisplayStatsInternal* get_stats(SF6CoreClient* c) {
    // Fast path: check existing
    for (int i = 0; i < s_dstats_count; i++)
        if (s_dstats_clients[i] == c) return &s_dstats[i];
    // Register
    std::lock_guard<std::mutex> lk(s_dstats_mutex);
    // Re-check under lock
    for (int i = 0; i < s_dstats_count; i++)
        if (s_dstats_clients[i] == c) return &s_dstats[i];
    if (s_dstats_count >= MAX_CLIENTS) return &s_dstats[0]; // overflow safety
    int idx = s_dstats_count++;
    s_dstats_clients[idx] = c;
    return &s_dstats[idx];
}

// ========================================================================
// Quarantine (object, operation) -> never retry after a fault
// ========================================================================

static constexpr int QUARANTINE_SLOTS = 512;
struct QuarantineEntry {
    uintptr_t obj_addr;
    uint32_t  op_id;       // hash of the operation name
    bool      active;
};
static QuarantineEntry s_quarantine[QUARANTINE_SLOTS] = {};
static int             s_quarantine_count = 0;
static std::mutex      s_quarantine_mutex;

static uint32_t op_hash(const char* s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x01000193u;
    return h;
}

static bool is_quarantined(void* obj, const char* op) {
    uintptr_t addr = (uintptr_t)obj;
    uint32_t  h    = op_hash(op);
    for (int i = 0; i < s_quarantine_count; i++) {
        auto& e = s_quarantine[i];
        if (e.active && e.obj_addr == addr && e.op_id == h) return true;
    }
    return false;
}

static void add_quarantine(void* obj, const char* op) {
    std::lock_guard<std::mutex> lk(s_quarantine_mutex);
    if (s_quarantine_count >= QUARANTINE_SLOTS) return;
    auto& e = s_quarantine[s_quarantine_count++];
    e.obj_addr = (uintptr_t)obj;
    e.op_id    = op_hash(op);
    e.active   = true;
}

// ========================================================================
// Rate-limited logging for refused/faulted calls
// ========================================================================

static std::atomic<uint64_t> s_warn_count{0};

static void rate_log_warn(const char* client_id, const char* fmt, ...) {
    uint64_t n = s_warn_count.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || (n % 100 == 0)) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        core_log(SF6CORE_LOG_WARN, client_id, "%s", buf);
    }
}

// The off-thread advisory (see guard_prefix): logged once per client and op. It used rate_log_warn, whose shared
// counter it filled in the first frames (hundreds of thousands of calls a session), so the warnings that matter
// (refused, faulted) were then only logged one in a hundred.
static void log_off_thread_once(const char* client_id, const char* op) {
    static SRWLOCK s_lock = SRWLOCK_INIT;                           // no destructor at process exit
    static std::vector<std::string>* s_seen = new std::vector<std::string>();
    std::string key = std::string(client_id ? client_id : "") + "|" + op;
    AcquireSRWLockExclusive(&s_lock);
    bool seen = false;
    for (const auto& k : *s_seen)
        if (k == key) { seen = true; break; }
    if (!seen) s_seen->push_back(key);
    ReleaseSRWLockExclusive(&s_lock);
    if (seen) return;
    core_log(SF6CORE_LOG_WARN, client_id,
             "display op %s called off the LateUpdateBehavior thread (allowed; logged once per op)", op);
}

// ========================================================================
// Type resolution cache for display types
// ========================================================================

static struct DisplayTypeCache {
    bool resolved;
    bool failed;
    REFrameworkTypeDefinitionHandle t_text;          // via.gui.Text
    REFrameworkTypeDefinitionHandle t_message_text;  // via.gui.message.Text
    REFrameworkTypeDefinitionHandle t_control;       // via.gui.Control
    REFrameworkTypeDefinitionHandle t_play_object;   // via.gui.PlayObject
    REFrameworkTypeDefinitionHandle t_color;         // via.Color
    REFrameworkTypeDefinitionHandle t_vec3;          // via.vec3
    REFrameworkTypeDefinitionHandle t_size;          // via.Size
    REFrameworkTypeDefinitionHandle t_guid;          // System.Guid
    REFrameworkTypeDefinitionHandle t_ui_agent;      // app.UIAgent
    REFrameworkTypeDefinitionHandle t_texture;       // via.gui.Texture
    REFrameworkTypeDefinitionHandle t_game_object;   // via.GameObject
    REFrameworkTypeDefinitionHandle t_gui_material;  // via.gui.Material (ADR-0019)
    REFrameworkTypeDefinitionHandle t_tex_holder;    // via.render.TextureResourceHolder
    REFrameworkTypeDefinitionHandle t_string;        // System.String

    // Methods
    REFrameworkMethodHandle m_text_get_message;
    REFrameworkMethodHandle m_text_set_message;
    REFrameworkMethodHandle m_text_get_message_id;
    REFrameworkMethodHandle m_text_set_message_id;
    REFrameworkMethodHandle m_text_get_font_size;
    REFrameworkMethodHandle m_text_set_font_size;
    REFrameworkMethodHandle m_text_get_region_size;
    REFrameworkMethodHandle m_text_set_region_size;

    REFrameworkMethodHandle m_ctrl_get_visible;
    REFrameworkMethodHandle m_ctrl_set_visible;
    REFrameworkMethodHandle m_ctrl_set_force_invisible;
    REFrameworkMethodHandle m_ctrl_get_force_invisible;
    REFrameworkMethodHandle m_ctrl_get_position;
    REFrameworkMethodHandle m_ctrl_set_position;
    REFrameworkMethodHandle m_ctrl_get_scale;
    REFrameworkMethodHandle m_ctrl_set_scale;
    REFrameworkMethodHandle m_ctrl_get_color;
    REFrameworkMethodHandle m_ctrl_set_color;
    REFrameworkMethodHandle m_ctrl_set_color_lt;
    REFrameworkMethodHandle m_ctrl_set_color_rt;
    REFrameworkMethodHandle m_ctrl_set_color_lb;
    REFrameworkMethodHandle m_ctrl_set_color_rb;
    REFrameworkMethodHandle m_ctrl_get_size;
    REFrameworkMethodHandle m_ctrl_set_size;

    REFrameworkMethodHandle m_po_get_child;
    REFrameworkMethodHandle m_po_get_next;
    REFrameworkMethodHandle m_po_get_name;

    REFrameworkMethodHandle m_agent_set_block_input;

    REFrameworkMethodHandle m_go_get_name;
    REFrameworkMethodHandle m_go_get_component;
    REFrameworkMethodHandle m_go_duplicate;

    // MemberwiseClone: System.Object
    REFrameworkMethodHandle m_obj_memberwise_clone;

    // UIParts
    REFrameworkMethodHandle m_uiparts_create;  // UIPartsManager.Create(Control,Control)

    // Runtime type for UIAgent (for getComponent)
    REFrameworkManagedObjectHandle rt_ui_agent;
} s_dtc = {};

static REFrameworkMethodHandle find_method_ll(const REFrameworkSDKData* sdk, void* obj, const char* sig);
static std::string flm_type_name(const REFrameworkSDKData* sdk, REFrameworkTypeDefinitionHandle td);

static bool resolve_display_types() {
    if (s_dtc.resolved) return !s_dtc.failed;
    s_dtc.resolved = true;

    auto* sdk = s_sdk;
    if (!sdk) { s_dtc.failed = true; return false; }

    auto tdb = sdk->functions->get_tdb();
    if (!tdb) { s_dtc.failed = true; return false; }

    DWORD exc = 0;
    __try {
        s_dtc.t_text         = sdk->tdb->find_type(tdb, "via.gui.Text");
        s_dtc.t_message_text = sdk->tdb->find_type(tdb, "via.gui.message.Text");
        s_dtc.t_control      = sdk->tdb->find_type(tdb, "via.gui.Control");
        s_dtc.t_play_object  = sdk->tdb->find_type(tdb, "via.gui.PlayObject");
        s_dtc.t_color        = sdk->tdb->find_type(tdb, "via.Color");
        s_dtc.t_vec3         = sdk->tdb->find_type(tdb, "via.vec3");
        s_dtc.t_size         = sdk->tdb->find_type(tdb, "via.Size");
        s_dtc.t_guid         = sdk->tdb->find_type(tdb, "System.Guid");
        s_dtc.t_ui_agent     = sdk->tdb->find_type(tdb, "app.UIAgent");
        s_dtc.t_texture      = sdk->tdb->find_type(tdb, "via.gui.Texture");
        s_dtc.t_game_object  = sdk->tdb->find_type(tdb, "via.GameObject");
        s_dtc.t_gui_material = sdk->tdb->find_type(tdb, "via.gui.Material");
        s_dtc.t_tex_holder   = sdk->tdb->find_type(tdb, "via.render.TextureResourceHolder");
        s_dtc.t_string       = sdk->tdb->find_type(tdb, "System.String");

        if (s_dtc.t_text) {
            s_dtc.m_text_get_message = sdk->type_definition->find_method(s_dtc.t_text, "get_Message");
            s_dtc.m_text_set_message = sdk->type_definition->find_method(s_dtc.t_text, "set_Message");
            s_dtc.m_text_get_font_size = sdk->type_definition->find_method(s_dtc.t_text, "get_FontSize");
            s_dtc.m_text_set_font_size = sdk->type_definition->find_method(s_dtc.t_text, "set_FontSize");
            s_dtc.m_text_get_region_size = sdk->type_definition->find_method(s_dtc.t_text, "get_RegionSize");
            s_dtc.m_text_set_region_size = sdk->type_definition->find_method(s_dtc.t_text, "set_RegionSize");
        }
        // message.Text has set/get_MessageId
        if (s_dtc.t_message_text) {
            s_dtc.m_text_get_message_id = sdk->type_definition->find_method(s_dtc.t_message_text, "get_MessageId");
            s_dtc.m_text_set_message_id = sdk->type_definition->find_method(s_dtc.t_message_text, "set_MessageId");
        }

        if (s_dtc.t_play_object) {
            s_dtc.m_ctrl_get_visible = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Visible");
            s_dtc.m_ctrl_set_visible = sdk->type_definition->find_method(s_dtc.t_play_object, "set_Visible");
            s_dtc.m_ctrl_set_force_invisible = sdk->type_definition->find_method(s_dtc.t_play_object, "set_ForceInvisible");
            s_dtc.m_ctrl_get_force_invisible = sdk->type_definition->find_method(s_dtc.t_play_object, "get_ForceInvisible");
            s_dtc.m_ctrl_get_position = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Position");
            s_dtc.m_ctrl_set_position = sdk->type_definition->find_method(s_dtc.t_play_object, "set_Position");
            s_dtc.m_ctrl_get_scale    = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Scale");
            s_dtc.m_ctrl_set_scale    = sdk->type_definition->find_method(s_dtc.t_play_object, "set_Scale");
            s_dtc.m_ctrl_get_color    = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Color");
            s_dtc.m_ctrl_set_color    = sdk->type_definition->find_method(s_dtc.t_play_object, "set_Color");
            s_dtc.m_ctrl_set_color_lt = sdk->type_definition->find_method(s_dtc.t_play_object, "set_ColorLeftTop");
            s_dtc.m_ctrl_set_color_rt = sdk->type_definition->find_method(s_dtc.t_play_object, "set_ColorRightTop");
            s_dtc.m_ctrl_set_color_lb = sdk->type_definition->find_method(s_dtc.t_play_object, "set_ColorLeftBottom");
            s_dtc.m_ctrl_set_color_rb = sdk->type_definition->find_method(s_dtc.t_play_object, "set_ColorRightBottom");
            s_dtc.m_ctrl_get_size     = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Size");
            s_dtc.m_ctrl_set_size     = sdk->type_definition->find_method(s_dtc.t_play_object, "set_Size");
            s_dtc.m_po_get_child = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Child");
            s_dtc.m_po_get_next  = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Next");
            s_dtc.m_po_get_name  = sdk->type_definition->find_method(s_dtc.t_play_object, "get_Name");
        }

        if (s_dtc.t_ui_agent) {
            s_dtc.m_agent_set_block_input = sdk->type_definition->find_method(s_dtc.t_ui_agent, "set_BlockInput");
            s_dtc.rt_ui_agent = sdk->type_definition->get_runtime_type(s_dtc.t_ui_agent);
        }

        if (s_dtc.t_game_object) {
            s_dtc.m_go_get_name = sdk->type_definition->find_method(s_dtc.t_game_object, "get_Name");
            s_dtc.m_go_get_component = sdk->type_definition->find_method(s_dtc.t_game_object, "getComponent(System.Type)");
            if (!s_dtc.m_go_get_component)
                s_dtc.m_go_get_component = sdk->type_definition->find_method(s_dtc.t_game_object, "getComponent");
            s_dtc.m_go_duplicate = sdk->type_definition->find_method(s_dtc.t_game_object, "duplicate(System.String)");
        }

        // MemberwiseClone from System.Object
        auto t_obj = sdk->tdb->find_type(tdb, "System.Object");
        if (t_obj)
            s_dtc.m_obj_memberwise_clone = sdk->type_definition->find_method(t_obj, "MemberwiseClone");

        // UIPartsManager.Create
        auto t_uipm = sdk->tdb->find_type(tdb, "app.UIPartsManager");
        if (t_uipm) {
            s_dtc.m_uiparts_create = sdk->type_definition->find_method(t_uipm, "Create(via.gui.Control,via.gui.Control)");
            if (!s_dtc.m_uiparts_create)
                s_dtc.m_uiparts_create = sdk->type_definition->find_method(t_uipm, "Create");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }

    if (exc || !s_dtc.t_play_object) {
        s_dtc.failed = true;
        core_log(SF6CORE_LOG_WARN, nullptr,
                 "display type cache resolution failed (SEH=0x%08X)", (unsigned)exc);
        return false;
    }

    core_log(SF6CORE_LOG_INFO, nullptr,
             "display types resolved (gui setters resolved per object: via.gui.Text/Rect/Material each declare "
             "their own get/set_Position, get/set_Color...; MemberwiseClone=%s)",
             s_dtc.m_obj_memberwise_clone ? "ok" : "NO");
    return true;
}

// ========================================================================
// Proof-of-life: type definition check (NOT is_managed_object, R34 lesson)
// ========================================================================

// Proof-of-life (step 2). Runs BEFORE each operation's own __try, so it carries its own SEH: a stale or
// bogus pointer handed by a client must be refused, never crash the game.
static bool check_is_type(void* obj, REFrameworkTypeDefinitionHandle expected) {
    if (!obj || !expected || !s_sdk) return false;
    if ((uintptr_t)obj < 0x10000) return false;
    __try {
        auto td = s_sdk->managed_object->get_type_definition(
            (REFrameworkManagedObjectHandle)obj);
        while (td) {
            if (td == expected) return true;
            td = s_sdk->type_definition->get_parent_type(td);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static bool is_text_like(void* obj) {
    return check_is_type(obj, s_dtc.t_text) ||
           check_is_type(obj, s_dtc.t_message_text);
}

static bool is_control_like(void* obj) {
    return check_is_type(obj, s_dtc.t_play_object) ||
           check_is_type(obj, s_dtc.t_control);
}

// ========================================================================
// Common guard chain prefix (steps 1-3)
// Returns SF6CORE_OK if the call should proceed.
// ========================================================================

// perf: API-call timer started by guard_prefix, ended by on_display_success / on_display_fault
static thread_local int64_t tl_api_t0 = 0;

static SF6CoreStatus guard_prefix(SF6CoreClient* client, void* obj,
                                   const char* op, bool check_thread,
                                   DisplayStatsInternal** out_stats) {
    tl_api_t0 = perf::begin();
    if (!client || !obj) { perf::end(perf::Api, tl_api_t0); tl_api_t0 = 0; return SF6CORE_ERR_INVALID_ARG; }
    auto* st = get_stats(client);
    *out_stats = st;

    // 1. Kill switch / breaker / closing
    if (kill_switch_active() || core_closing() || client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER) {
        st->refused.fetch_add(1, std::memory_order_relaxed);
        rate_log_warn(client->client_id, "display op %s refused: client disabled", op);
        perf::end(perf::Api, tl_api_t0); tl_api_t0 = 0;
        return SF6CORE_ERR_DISABLED;
    }

    // Thread check (for ui_* calls)
    // Advisory only: the game runs UI update jobs (e.g. <Flow>.Main.OnUpdate post-hooks, where rows must be
    // cloned) on threads other than the LateUpdateBehavior one, and the clients' proven code wrote from
    // there. Refusing broke the Drive rows in-game (2026-10-02). Logged, never refused.
    if (check_thread && !is_game_thread()) {
        log_off_thread_once(client->client_id, op);
    }

    // 3. Quarantine
    if (is_quarantined(obj, op)) {
        st->quarantined.fetch_add(1, std::memory_order_relaxed);
        perf::end(perf::Api, tl_api_t0); tl_api_t0 = 0;
        return SF6CORE_ERR_FAULTED;
    }

    return SF6CORE_OK;  // timer still running: caller or on_display_success/fault ends it
}

// ========================================================================
// SEH wrapper for a single engine operation (step 5)
// ========================================================================

struct DispatchCtx {
    SF6CoreClient*        client;
    void*                 obj;
    const char*           op;
    DisplayStatsInternal* stats;
    bool                  managed_exception; // from invoke_c
};

// After a fault: quarantine + breaker accounting
static void on_display_fault(DispatchCtx* ctx, DWORD exc_code) {
    perf::end(perf::Api, tl_api_t0); tl_api_t0 = 0;
    add_quarantine(ctx->obj, ctx->op);
    ctx->stats->faults.fetch_add(1, std::memory_order_relaxed);

    // Breaker accounting: reuse the client's own fault_count/half_open
    ctx->client->fault_count++;
    ctx->client->total_faults++;
    bool retry_failed = ctx->client->half_open;
    if (retry_failed || ctx->client->fault_count >= 5) {
        ctx->client->state          = SF6CORE_CLIENT_DISABLED_BY_BREAKER;
        ctx->client->half_open      = false;
        ctx->client->disabled_until = s_frame + 300;
    }

    rate_log_warn(ctx->client->client_id,
                  "display fault: op=%s obj=0x%p SEH=0x%08X",
                  ctx->op, ctx->obj, (unsigned)exc_code);
}

// After a successful write
static void on_display_success(DispatchCtx* ctx) {
    perf::end(perf::Api, tl_api_t0); tl_api_t0 = 0;
    ctx->stats->writes.fetch_add(1, std::memory_order_relaxed);
    if (ctx->client->half_open) {
        ctx->client->half_open   = false;
        ctx->client->fault_count = 0;
    } else {
        ctx->client->fault_count = 0;
    }
}

// ========================================================================
// via.Color: RGBA packing/unpacking
// ========================================================================
// The game's via.Color is ONE 32-bit field, `rgba` (4 bytes), not 4 floats:
// ColorMod builds it with ValueType.new(via.Color):set_field("rgba", v) and
// SlotsLoader memcpy's a uint32_t into ColorDatas' Color00/Color01. The API
// value is that raw field, passed through unchanged (writing 16 bytes here
// would corrupt the neighbouring fields of the owning object).

struct ViaColor { uint32_t rgba; };
static_assert(sizeof(ViaColor) == 4, "via.Color is 4 bytes");

static ViaColor rgba_to_via_color(uint32_t rgba) { ViaColor c; c.rgba = rgba; return c; }
static uint32_t via_color_to_rgba(const ViaColor& c) { return c.rgba; }

// ========================================================================
// Texture loading state (60-frame loaded rule from SlotsLoader)
// ========================================================================

static constexpr uint64_t TEXTURE_LOAD_FRAMES = 60;
struct TextureEntry {
    void*    holder;
    uint64_t created_frame;
    bool     active;
};
static constexpr int TEXTURE_MAX = 4096;   // holders created via ui_texture_load (one per variant preview in practice)
static TextureEntry s_textures[TEXTURE_MAX] = {};
static int          s_texture_count = 0;
static std::mutex   s_texture_mutex;

static TextureEntry* find_texture(void* holder) {
    for (int i = 0; i < s_texture_count; i++)
        if (s_textures[i].active && s_textures[i].holder == holder) return &s_textures[i];
    return nullptr;
}

// ========================================================================
// API implementations
// ========================================================================

// P2: set_Message on a text control
SF6CoreStatus SF6CORE_CALL api_ui_set_text(SF6CoreClient* client, void* text, const char* utf8) {
    if (!utf8) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, text, "ui_set_text", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_text_like(text)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        // Compare-before-write (step 4): identical set_Message crashes (R7)
        RawInvokeRet ret;
        char cur[512] = {};
        if (true &&
            invoke_c(sdk, find_method_ll(sdk, text, "get_Message"), text, nullptr, 0, &ret) && ret.ptr) {
            managed_str_to_utf8(ret.ptr, cur, sizeof(cur));
        }
        if (strcmp(cur, utf8) == 0) {
            st->skipped_identical.fetch_add(1);
            return SF6CORE_OK;
        }
        // Write
        auto* str = sdk->functions->create_managed_string_normal(utf8);
        if (str && true) {
            void* args[] = { (void*)str };
            bool ok = invoke_c(sdk, find_method_ll(sdk, text, "set_Message"), text, args, 1, &ret);
            if (!ok || ret.exception_thrown) {
                DispatchCtx ctx = { client, text, "ui_set_text", st, true };
                on_display_fault(&ctx, 0);
                return SF6CORE_ERR_FAULTED;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, text, "ui_set_text", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, text, "ui_set_text", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P2: set_MessageId (System.Guid 16 bytes)
SF6CoreStatus SF6CORE_CALL api_ui_set_message_id(SF6CoreClient* client, void* text, const uint8_t guid16[16]) {
    if (!guid16) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, text, "ui_set_message_id", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_text_like(text)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {

        // Compare-before-write
        if (true) {
            RawInvokeRet cur;
            if (invoke_c(sdk, find_method_ll(sdk, text, "get_MessageId"), text, nullptr, 0, &cur)) {
                if (memcmp(cur.bytes, guid16, 16) == 0) {
                    st->skipped_identical.fetch_add(1);
                    return SF6CORE_OK;
                }
            }
        }

        // System.Guid is a 16-byte value type, passed by pointer
        RawInvokeRet ret;
        void* args[] = { (void*)guid16 };
        bool ok = invoke_c(sdk, find_method_ll(sdk, text, "set_MessageId"), text, args, 1, &ret);
        if (!ok || ret.exception_thrown) {
            DispatchCtx ctx = { client, text, "ui_set_message_id", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, text, "ui_set_message_id", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, text, "ui_set_message_id", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P2: get_MessageId
SF6CoreStatus SF6CORE_CALL api_ui_get_message_id(void* text, uint8_t out_guid16[16]) {
    if (!text || !out_guid16) return SF6CORE_ERR_INVALID_ARG;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    auto* sdk = s_sdk;
    __try {
        RawInvokeRet ret;
        if (!invoke_c(sdk, find_method_ll(sdk, text, "get_MessageId"), text, nullptr, 0, &ret))
            return SF6CORE_ERR_FAULTED;
        memcpy(out_guid16, ret.bytes, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SF6CORE_ERR_FAULTED;
    }
    return SF6CORE_OK;
}

// P2: font size
SF6CoreStatus SF6CORE_CALL api_ui_set_font_size(SF6CoreClient* client, void* text, float w, float h) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, text, "ui_set_font_size", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_text_like(text)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        // Compare-before-write
        if (true) {
            RawInvokeRet cur;
            if (invoke_c(sdk, find_method_ll(sdk, text, "get_FontSize"), text, nullptr, 0, &cur)) {
                float* cw = (float*)cur.bytes;          // via.Size {w, h}
                float* ch = (float*)(cur.bytes + 4);
                if (*cw == w && *ch == h) { st->skipped_identical.fetch_add(1); return SF6CORE_OK; }
            }
        }
        // via.Size is a value type: passed by address, like REFramework's Lua call() does for ValueTypes
        float sz[2] = { w, h };
        void* args[] = { (void*)sz };
        RawInvokeRet ret;
        bool ok = invoke_c(sdk, find_method_ll(sdk, text, "set_FontSize"), text, args, 1, &ret);
        if (!ok || ret.exception_thrown) {
            DispatchCtx ctx = { client, text, "ui_set_font_size", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, text, "ui_set_font_size", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, text, "ui_set_font_size", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P2: region size (via.Size = {w,h} as float pair)
SF6CoreStatus SF6CORE_CALL api_ui_set_region_size(SF6CoreClient* client, void* text, float w, float h) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, text, "ui_set_region_size", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_text_like(text)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        // Compare-before-write
        if (true) {
            RawInvokeRet cur;
            if (invoke_c(sdk, find_method_ll(sdk, text, "get_RegionSize"), text, nullptr, 0, &cur)) {
                float* cw = (float*)cur.bytes;
                float* ch = (float*)(cur.bytes + 4);
                if (*cw == w && *ch == h) { st->skipped_identical.fetch_add(1); return SF6CORE_OK; }
            }
        }
        // via.Size is a value type: {float w, float h}
        float sz[2] = { w, h };
        void* args[] = { (void*)sz };
        RawInvokeRet ret;
        bool ok = invoke_c(sdk, find_method_ll(sdk, text, "set_RegionSize"), text, args, 1, &ret);
        if (!ok || ret.exception_thrown) {
            DispatchCtx ctx = { client, text, "ui_set_region_size", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, text, "ui_set_region_size", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, text, "ui_set_region_size", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P3: colour -- set_Color + 4 corner colours (from ColorSpinExtra.paint_rect)
SF6CoreStatus SF6CORE_CALL api_ui_set_color(SF6CoreClient* client, void* ctrl, uint32_t rgba) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, ctrl, "ui_set_color", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_control_like(ctrl)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    ViaColor color = rgba_to_via_color(rgba);
    DWORD exc = 0;
    __try {
        // Compare-before-write: read get_Color, extract rgba field
        if (true) {
            RawInvokeRet cur;
            if (invoke_c(sdk, find_method_ll(sdk, ctrl, "get_Color"), ctrl, nullptr, 0, &cur)) {
                ViaColor cc;
                memcpy(&cc, cur.bytes, sizeof(ViaColor));
                if (via_color_to_rgba(cc) == rgba) {
                    st->skipped_identical.fetch_add(1);
                    return SF6CORE_OK;
                }
            }
        }

        // Write: set_Color + 4 corners (mirroring paint_rect from ColorSpinExtra)
        RawInvokeRet ret;
        void* args[] = { (void*)&color };
        if (true) invoke_c(sdk, find_method_ll(sdk, ctrl, "set_Color"), ctrl, args, 1, &ret);
        if (true) invoke_c(sdk, find_method_ll(sdk, ctrl, "set_ColorLeftTop"), ctrl, args, 1, &ret);
        if (true) invoke_c(sdk, find_method_ll(sdk, ctrl, "set_ColorRightTop"), ctrl, args, 1, &ret);
        if (true) invoke_c(sdk, find_method_ll(sdk, ctrl, "set_ColorLeftBottom"), ctrl, args, 1, &ret);
        if (true) invoke_c(sdk, find_method_ll(sdk, ctrl, "set_ColorRightBottom"), ctrl, args, 1, &ret);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, ctrl, "ui_set_color", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, ctrl, "ui_set_color", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// Lock helper outside __try (C2712)
static void track_texture(void* holder) {
    std::lock_guard<std::mutex> lk(s_texture_mutex);
    if (s_texture_count < TEXTURE_MAX) {
        auto& te = s_textures[s_texture_count++];
        te.holder = holder;
        te.created_frame = s_frame;
        te.active = true;
    } else {
        // untracked holders are reported ready at once by ui_texture_ready: say so loudly (never seen in practice)
        static bool warned = false;
        if (!warned) { warned = true; core_log(SF6CORE_LOG_WARN, nullptr, "texture table full (%d): 60-frame load wait no longer enforced for new holders", TEXTURE_MAX); }
    }
}

// P3: texture loading (resource + holder, 60-frame loaded rule)
SF6CoreStatus SF6CORE_CALL api_ui_texture_load(SF6CoreClient* client, const char* resource_path, void** out_holder) {
    if (!resource_path || !out_holder) return SF6CORE_ERR_INVALID_ARG;
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    *out_holder = nullptr;
    if (kill_switch_active() || core_closing() || client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER)
        return SF6CORE_ERR_DISABLED;

    auto* sdk = s_sdk;
    if (!sdk) return SF6CORE_ERR_NOT_READY;

    DWORD exc = 0;
    void* result_holder = nullptr;
    __try {
        auto* rm = sdk->functions->get_resource_manager();
        auto* res = rm ? sdk->resource_manager->create_resource(rm, "via.render.TextureResource", resource_path) : nullptr;
        if (!res) return SF6CORE_ERR_FAULTED;

        sdk->resource->add_ref((REFrameworkResourceHandle)res);
        auto holder = sdk->resource->create_holder((REFrameworkResourceHandle)res, "via.render.TextureResourceHolder");
        sdk->resource->release((REFrameworkResourceHandle)res);
        if (!holder) return SF6CORE_ERR_FAULTED;

        // add_ref for caller ownership
        sdk->managed_object->add_ref((REFrameworkManagedObjectHandle)holder);
        result_holder = (void*)holder;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        return SF6CORE_ERR_FAULTED;
    }

    if (result_holder) {
        track_texture(result_holder);
        *out_holder = result_holder;
    }
    return SF6CORE_OK;
}

// P3: texture ready check (60 frames after creation)
int32_t SF6CORE_CALL api_ui_texture_ready(void* holder) {
    if (!holder) return 0;
    auto* te = find_texture(holder);
    if (!te) return 1; // not tracked -> assume ready
    return (s_frame - te->created_frame >= TEXTURE_LOAD_FRAMES) ? 1 : 0;
}

// P3: set texture on a via.gui.Texture control
SF6CoreStatus SF6CORE_CALL api_ui_set_texture(SF6CoreClient* client, void* texture_ctrl, void* holder) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, texture_ctrl, "ui_set_texture", true, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        // Compare-before-write: compare underlying resource ptr at +0x10
        auto m_get_tex = find_method_ll(sdk, texture_ctrl, "getTexture");
        if (m_get_tex) {
            RawInvokeRet cur;
            if (invoke_c(sdk, m_get_tex, texture_ctrl, nullptr, 0, &cur) && cur.ptr) {
                // Compare resource pointers at offset +0x10
                uint64_t cur_res = *(uint64_t*)((uint8_t*)cur.ptr + 0x10);
                uint64_t new_res = holder ? *(uint64_t*)((uint8_t*)holder + 0x10) : 0;
                if (cur_res == new_res) {
                    st->skipped_identical.fetch_add(1);
                    return SF6CORE_OK;
                }
            }
        }

        // Write: setTexture
        auto m_set_tex = find_method_ll(sdk, texture_ctrl, "setTexture");
        if (!m_set_tex) return SF6CORE_ERR_NOT_READY;
        RawInvokeRet ret;
        void* args[] = { holder };
        bool ok = invoke_c(sdk, m_set_tex, texture_ctrl, args, 1, &ret);
        if (!ok || ret.exception_thrown) {
            DispatchCtx ctx = { client, texture_ctrl, "ui_set_texture", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, texture_ctrl, "ui_set_texture", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, texture_ctrl, "ui_set_texture", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// ========================================================================
// P3: texture variable of a GUI material (ADR-0019)
// ========================================================================
// A via.gui.Material keeps its textures as named material variables: getMaterialValueT(name) /
// setMaterialValueT(name, holder) (e.g. the stage strip's cells: e_mat_thumb, "Stage_Image"). Both are
// resolved by name on the object's type and their parameters checked once per method: the name a
// System.String, the texture a TextureResourceHolder type. A method that does not match is never called
// (logged once, ERR_NOT_FOUND). Game thread only (refused elsewhere, unlike the advisory of the other ops).

static bool td_derives(REFrameworkTypeDefinitionHandle td, REFrameworkTypeDefinitionHandle base) {
    for (; td; td = s_sdk->type_definition->get_parent_type(td)) if (td == base) return true;
    return false;
}

static bool holder_like(REFrameworkTypeDefinitionHandle td) {
    return td && s_dtc.t_tex_holder && (td_derives(td, s_dtc.t_tex_holder) || td_derives(s_dtc.t_tex_holder, td));
}

static SRWLOCK                                            s_mat_lock = SRWLOCK_INIT;
static std::unordered_map<REFrameworkMethodHandle, bool> s_mat_checked;   // method -> signature as expected

// Getter: (System.String) returning a holder type; setter: (System.String, holder type). Outside __try (C2712).
static bool mat_method_ok(REFrameworkMethodHandle mh, bool setter) {
    if (!mh) return false;
    AcquireSRWLockShared(&s_mat_lock);
    auto it = s_mat_checked.find(mh);
    const bool known = it != s_mat_checked.end();
    const bool cached = known && it->second;
    ReleaseSRWLockShared(&s_mat_lock);
    if (known) return cached;

    auto* sdk = s_sdk;
    const unsigned want = setter ? 2u : 1u;
    REFrameworkMethodParameter prm[4] = {};
    unsigned int n = 0;
    bool ok = sdk->method->get_num_params(mh) == want &&
              sdk->method->get_params(mh, prm, (unsigned int)sizeof prm, &n) == REFRAMEWORK_ERROR_NONE && n == want;
    REFrameworkTypeDefinitionHandle tex = ok ? (setter ? prm[1].t : sdk->method->get_return_type(mh)) : nullptr;
    const std::string name_t = (ok && prm[0].t) ? flm_type_name(sdk, prm[0].t) : std::string("-");
    ok = ok && (prm[0].t == s_dtc.t_string || name_t == "System.String") && holder_like(tex);
    const char* nm = sdk->method->get_name(mh);
    core_log(ok ? SF6CORE_LOG_INFO : SF6CORE_LOG_WARN, nullptr,
             "material texture: %s.%s(%s%s) -> %s: %s",
             flm_type_name(sdk, sdk->method->get_declaring_type(mh)).c_str(), nm ? nm : "?", name_t.c_str(),
             setter ? (std::string(", ") + (tex ? flm_type_name(sdk, tex) : "-")).c_str() : "",
             setter ? "void" : (tex ? flm_type_name(sdk, tex).c_str() : "-"),
             ok ? "used" : "not the expected signature, never called");
    AcquireSRWLockExclusive(&s_mat_lock);
    s_mat_checked[mh] = ok;
    ReleaseSRWLockExclusive(&s_mat_lock);
    return ok;
}

// -1 untracked, 0 loading, 1 loaded. Lock outside __try (C2712).
static int texture_state(void* holder) {
    std::lock_guard<std::mutex> lk(s_texture_mutex);
    auto* te = find_texture(holder);
    if (!te) return -1;
    return (s_frame - te->created_frame >= TEXTURE_LOAD_FRAMES) ? 1 : 0;
}

// A holder read from a material is on screen, so loaded: a client may give it back (ui_set_material_texture)
static void track_texture_loaded(void* holder) {
    std::lock_guard<std::mutex> lk(s_texture_mutex);
    if (find_texture(holder)) return;
    if (s_texture_count < TEXTURE_MAX) {
        auto& te = s_textures[s_texture_count++];
        te.holder = holder;
        te.created_frame = 0;
        te.active = true;
    }
}

// Proof that the variable exists: it holds a texture now (getMaterialValueT answers null for a name the
// material does not have). Shared by get / set; SEH in the callers.
static bool mat_read(const REFrameworkSDKData* sdk, REFrameworkMethodHandle m_get, void* material, void* name_str,
                     void** out) {
    RawInvokeRet ret;
    void* args[] = { name_str };
    *out = nullptr;
    if (!invoke_c(sdk, m_get, material, args, 1, &ret)) return false;
    *out = ret.ptr;
    return true;
}

static bool same_texture(void* cur, void* holder) {
    if (cur == holder) return true;
    // two holders of one resource (pointer at +0x10, as ui_set_texture compares)
    if (!check_is_type(cur, s_dtc.t_tex_holder) || !check_is_type(holder, s_dtc.t_tex_holder)) return false;
    return *(uint64_t*)((uint8_t*)cur + 0x10) == *(uint64_t*)((uint8_t*)holder + 0x10);
}

SF6CoreStatus SF6CORE_CALL api_ui_set_material_texture(SF6CoreClient* client, void* material, const char* param, void* holder) {
    if (!param || !param[0] || !holder) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, material, "ui_set_material_texture", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!on_recorded_game_thread()) { st->refused.fetch_add(1); return SF6CORE_ERR_WRONG_THREAD; }
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!check_is_type(material, s_dtc.t_gui_material)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }
    const int ts = texture_state(holder);
    if (ts < 0) {
        st->refused.fetch_add(1);
        rate_log_warn(client->client_id, "ui_set_material_texture refused: holder 0x%p is not from ui_texture_load / ui_get_material_texture", holder);
        return SF6CORE_ERR_INVALID_ARG;
    }
    if (ts == 0) return SF6CORE_ERR_NOT_READY;                    // a texture handed to the GUI while it loads crashed the game
    if (!check_is_type(holder, s_dtc.t_tex_holder)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    auto m_get = find_method_ll(sdk, material, "getMaterialValueT(System.String)");
    auto m_set = find_method_ll(sdk, material, "setMaterialValueT(System.String,via.render.TextureResourceHolder)");
    if (!mat_method_ok(m_get, false) || !mat_method_ok(m_set, true)) { st->refused.fetch_add(1); return SF6CORE_ERR_NOT_FOUND; }

    DWORD exc = 0;
    __try {
        void* name_str = (void*)sdk->functions->create_managed_string_normal(param);
        if (!name_str) return SF6CORE_ERR_FAULTED;
        // Compare-before-write; a variable that holds no texture is not proven to exist: refused
        void* cur = nullptr;
        if (!mat_read(sdk, m_get, material, name_str, &cur)) {
            DispatchCtx ctx = { client, material, "ui_set_material_texture", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
        if (!cur) {
            st->refused.fetch_add(1);
            rate_log_warn(client->client_id, "ui_set_material_texture refused: material 0x%p holds no texture '%s'", material, param);
            return SF6CORE_ERR_NOT_FOUND;
        }
        if (same_texture(cur, holder)) {
            st->skipped_identical.fetch_add(1);
            return SF6CORE_OK;
        }
        // Write: setMaterialValueT
        RawInvokeRet ret;
        void* args[] = { name_str, holder };
        if (!invoke_c(sdk, m_set, material, args, 2, &ret)) {
            DispatchCtx ctx = { client, material, "ui_set_material_texture", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, material, "ui_set_material_texture", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, material, "ui_set_material_texture", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

SF6CoreStatus SF6CORE_CALL api_ui_get_material_texture(SF6CoreClient* client, void* material, const char* param, void** out_holder) {
    if (!param || !param[0] || !out_holder) return SF6CORE_ERR_INVALID_ARG;
    *out_holder = nullptr;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, material, "ui_get_material_texture", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!on_recorded_game_thread()) { st->refused.fetch_add(1); return SF6CORE_ERR_WRONG_THREAD; }
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!check_is_type(material, s_dtc.t_gui_material)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    auto m_get = find_method_ll(sdk, material, "getMaterialValueT(System.String)");
    if (!mat_method_ok(m_get, false)) { st->refused.fetch_add(1); return SF6CORE_ERR_NOT_FOUND; }

    DWORD exc = 0;
    void* cur = nullptr;
    __try {
        void* name_str = (void*)sdk->functions->create_managed_string_normal(param);
        if (!name_str) return SF6CORE_ERR_FAULTED;
        if (!mat_read(sdk, m_get, material, name_str, &cur)) {
            DispatchCtx ctx = { client, material, "ui_get_material_texture", st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }
        if (cur) sdk->managed_object->add_ref((REFrameworkManagedObjectHandle)cur);   // the caller's reference
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, material, "ui_get_material_texture", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    perf::end(perf::Api, tl_api_t0); tl_api_t0 = 0;                // a read: not counted as a write
    if (!cur) return SF6CORE_ERR_NOT_FOUND;
    track_texture_loaded(cur);
    *out_holder = cur;
    return SF6CORE_OK;
}

// P6: set_Visible
SF6CoreStatus SF6CORE_CALL api_ui_set_visible_disp(SF6CoreClient* client, void* ctrl, int32_t visible) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, ctrl, "ui_set_visible", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_control_like(ctrl)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        // Compare-before-write
        if (true) {
            RawInvokeRet cur;
            if (invoke_c(sdk, find_method_ll(sdk, ctrl, "get_Visible"), ctrl, nullptr, 0, &cur)) {
                bool cur_vis = (cur.dword != 0);
                if (cur_vis == (visible != 0)) {
                    st->skipped_identical.fetch_add(1);
                    return SF6CORE_OK;
                }
            }
        }
        void* args[] = { (void*)(uintptr_t)(visible ? 1 : 0) };
        RawInvokeRet ret;
        invoke_c(sdk, find_method_ll(sdk, ctrl, "set_Visible"), ctrl, args, 1, &ret);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, ctrl, "ui_set_visible", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, ctrl, "ui_set_visible", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P6: set_ForceInvisible
SF6CoreStatus SF6CORE_CALL api_ui_set_force_invisible(SF6CoreClient* client, void* ctrl, int32_t on) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, ctrl, "ui_set_force_invisible", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_control_like(ctrl)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        if (true) {
            RawInvokeRet cur;
            if (invoke_c(sdk, find_method_ll(sdk, ctrl, "get_ForceInvisible"), ctrl, nullptr, 0, &cur)) {
                if ((cur.dword != 0) == (on != 0)) {
                    st->skipped_identical.fetch_add(1);
                    return SF6CORE_OK;
                }
            }
        }
        void* args[] = { (void*)(uintptr_t)(on ? 1 : 0) };
        RawInvokeRet ret;
        invoke_c(sdk, find_method_ll(sdk, ctrl, "set_ForceInvisible"), ctrl, args, 1, &ret);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, ctrl, "ui_set_force_invisible", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, ctrl, "ui_set_force_invisible", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P6: set_Position (via.vec3: x,y,z)
SF6CoreStatus SF6CORE_CALL api_ui_set_position_disp(SF6CoreClient* client, void* ctrl, float x, float y, float z) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, ctrl, "ui_set_position", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_control_like(ctrl)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        // Compare-before-write
        RawInvokeRet pos;
        if (invoke_c(sdk, find_method_ll(sdk, ctrl, "get_Position"), ctrl, nullptr, 0, &pos)) {
            float* px = (float*)pos.bytes;
            float* py = (float*)(pos.bytes + 4);
            float* pz = (float*)(pos.bytes + 8);
            if (*px == x && *py == y && *pz == z) {
                st->skipped_identical.fetch_add(1);
                return SF6CORE_OK;
            }
            *px = x; *py = y; *pz = z;
            RawInvokeRet dummy;
            void* args[] = { (void*)pos.bytes };
            invoke_c(sdk, find_method_ll(sdk, ctrl, "set_Position"), ctrl, args, 1, &dummy);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, ctrl, "ui_set_position", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, ctrl, "ui_set_position", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P6: set_Scale (via.vec3: x,y,z)
SF6CoreStatus SF6CORE_CALL api_ui_set_scale(SF6CoreClient* client, void* ctrl, float x, float y, float z) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, ctrl, "ui_set_scale", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_control_like(ctrl)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        RawInvokeRet sc;
        if (invoke_c(sdk, find_method_ll(sdk, ctrl, "get_Scale"), ctrl, nullptr, 0, &sc)) {
            float* sx = (float*)sc.bytes;
            float* sy = (float*)(sc.bytes + 4);
            float* sz = (float*)(sc.bytes + 8);
            if (*sx == x && *sy == y && *sz == z) {
                st->skipped_identical.fetch_add(1);
                return SF6CORE_OK;
            }
            *sx = x; *sy = y; *sz = z;
            RawInvokeRet dummy;
            void* args[] = { (void*)sc.bytes };
            invoke_c(sdk, find_method_ll(sdk, ctrl, "set_Scale"), ctrl, args, 1, &dummy);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, ctrl, "ui_set_scale", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, ctrl, "ui_set_scale", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P6: set_Size (via.Size: w,h)
SF6CoreStatus SF6CORE_CALL api_ui_set_size(SF6CoreClient* client, void* ctrl, float w, float h) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, ctrl, "ui_set_size", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!is_control_like(ctrl)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        RawInvokeRet sz;
        if (invoke_c(sdk, find_method_ll(sdk, ctrl, "get_Size"), ctrl, nullptr, 0, &sz)) {
            float* sw = (float*)sz.bytes;
            float* sh = (float*)(sz.bytes + 4);
            if (*sw == w && *sh == h) {
                st->skipped_identical.fetch_add(1);
                return SF6CORE_OK;
            }
            *sw = w; *sh = h;
            RawInvokeRet dummy;
            void* args[] = { (void*)sz.bytes };
            invoke_c(sdk, find_method_ll(sdk, ctrl, "set_Size"), ctrl, args, 1, &dummy);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, ctrl, "ui_set_size", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, ctrl, "ui_set_size", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P6: set_BlockInput on a UIAgent
SF6CoreStatus SF6CORE_CALL api_ui_set_block_input(SF6CoreClient* client, void* agent, int32_t block) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, agent, "ui_set_block_input", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;
    if (!check_is_type(agent, s_dtc.t_ui_agent)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        void* args[] = { (void*)(uintptr_t)(block ? 1 : 0) };
        RawInvokeRet ret;
        invoke_c(sdk, find_method_ll(sdk, agent, "set_BlockInput"), agent, args, 1, &ret);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, agent, "ui_set_block_input", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, agent, "ui_set_block_input", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// P4: ui_clone_row -- the LL recipe, ported step for step from SF6_MenuRowsNative.cpp create_rows
// (ColorMod stabilize-native-ui, proven in-game). Every step must succeed: a missing method or a
// failed/excepted invoke stops the sequence (the original threw at the same points).
// Method lookup exactly like the original (LL's method()): walk the object's type chain; on each type try the
// full signature, accepting it only if it has a native function; otherwise the unique method with the same
// name and arity that has a native function.
// REFramework's get_methods takes its buffer size in BYTES (see API.hpp's own wrapper); passing a count left room for
// only 64 handles, and a type with more methods answered "too small" (the level was skipped silently). The buffer is
// sized from get_num_methods. The lookup runs on every display write, so results (also "not found") are cached per
// (object type, sig); the logs below are written once per (type, sig) because only a cache miss reaches them.
static SRWLOCK                                                  s_flm_lock = SRWLOCK_INIT;
static std::unordered_map<std::string, REFrameworkMethodHandle> s_flm_cache;
static std::unordered_set<std::string>                          s_flm_logged;   // one-off log keys, same lock

static std::string flm_type_name(const REFrameworkSDKData* sdk, REFrameworkTypeDefinitionHandle td) {
    char buf[256] = {};
    unsigned int len = 0;
    if (td && sdk->type_definition->get_full_name(td, buf, (unsigned int)sizeof buf, &len) == REFRAMEWORK_ERROR_NONE && buf[0])
        return buf;
    const char* nm = td ? sdk->type_definition->get_name(td) : nullptr;
    return nm ? nm : "?";
}

static bool flm_log_once(const std::string& key) {
    AcquireSRWLockExclusive(&s_flm_lock);
    const bool fresh = s_flm_logged.insert(key).second;
    ReleaseSRWLockExclusive(&s_flm_lock);
    return fresh;
}

static REFrameworkMethodHandle find_method_ll_scan(const REFrameworkSDKData* sdk, REFrameworkTypeDefinitionHandle start,
                                                   const char* sig, const char* open, size_t name_len, unsigned arity) {
    for (auto td = start; td; td = sdk->type_definition->get_parent_type(td)) {
        auto exact = sdk->type_definition->find_method(td, sig);
        if (exact && sdk->method->get_function(exact)) return exact;
        const unsigned int total = sdk->type_definition->get_num_methods(td);
        if (!total) continue;
        REFrameworkMethodHandle stack_buf[256];
        std::vector<REFrameworkMethodHandle> heap_buf;
        REFrameworkMethodHandle* methods = stack_buf;
        if (total > 256) { heap_buf.resize(total); methods = heap_buf.data(); }
        const unsigned int cap = total > 256 ? total : 256;
        unsigned int n = 0;
        const auto res = sdk->type_definition->get_methods(td, methods, cap * (unsigned int)sizeof(REFrameworkMethodHandle), &n);
        if (res != REFRAMEWORK_ERROR_NONE) {
            char key[64];
            snprintf(key, sizeof key, "gm|%p", (void*)td);
            if (flm_log_once(key))
                core_log(SF6CORE_LOG_WARN, nullptr, "[lookup] %s: get_methods failed (%d) for %u methods",
                         flm_type_name(sdk, td).c_str(), (int)res, total);
            continue;
        }
        if (n > cap) n = cap;
        for (unsigned int i = 0; i < n; ++i) {
            const char* nm = sdk->method->get_name(methods[i]);
            if (!nm || strlen(nm) != name_len || strncmp(nm, sig, name_len) != 0) continue;
            if (open && sdk->method->get_num_params(methods[i]) != arity) continue;
            if (!sdk->method->get_function(methods[i])) continue;
            if (total > 64) {
                const std::string tn = flm_type_name(sdk, td);
                const std::string nmstr(sig, name_len);
                if (flm_log_once("big|" + tn + "|" + sig))
                    core_log(SF6CORE_LOG_INFO, nullptr,
                             "[lookup] %s.%s found by name in a type with %u methods (missed before: the 64-method limit)",
                             tn.c_str(), nmstr.c_str(), total);
            }
            return methods[i];
        }
    }
    return nullptr;
}

static REFrameworkMethodHandle find_method_ll(const REFrameworkSDKData* sdk, void* obj, const char* sig) {
    if (!obj || !sig) return nullptr;
    const char* open = strchr(sig, '(');
    size_t name_len = open ? (size_t)(open - sig) : strlen(sig);
    unsigned arity = 0;
    if (open && open[1] != ')') { arity = 1; for (const char* c = open; *c; ++c) if (*c == ',') ++arity; }
    auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj);
    if (!td) return nullptr;
    std::string key((const char*)&td, sizeof td);
    key += sig;
    AcquireSRWLockShared(&s_flm_lock);
    auto it = s_flm_cache.find(key);
    if (it != s_flm_cache.end()) { auto r = it->second; ReleaseSRWLockShared(&s_flm_lock); return r; }
    ReleaseSRWLockShared(&s_flm_lock);

    auto found = find_method_ll_scan(sdk, td, sig, open, name_len, arity);
    if (!found) {
        const std::string tn = flm_type_name(sdk, td);
        if (flm_log_once("nf|" + tn + "|" + sig))
            core_log(SF6CORE_LOG_WARN, nullptr, "[lookup] %s: no method '%s' by name", tn.c_str(), sig);
    }
    AcquireSRWLockExclusive(&s_flm_lock);
    s_flm_cache[key] = found;
    ReleaseSRWLockExclusive(&s_flm_lock);
    return found;
}

static bool clone_call(const REFrameworkSDKData* sdk, void* obj, const char* sig,
                       void** args, unsigned argc, RawInvokeRet* out) {
    auto mh = find_method_ll(sdk, obj, sig);
    return mh && invoke_c(sdk, mh, obj, args, argc, out);
}

SF6CoreStatus SF6CORE_CALL api_ui_clone_row(SF6CoreClient* client, const SF6CoreRowCloneDesc* desc, void** out_ctrl) {
    if (!desc || !out_ctrl) return SF6CORE_ERR_INVALID_ARG;
    if (desc->struct_size < sizeof(SF6CoreRowCloneDesc)) return SF6CORE_ERR_INVALID_ARG;
    if (!desc->template_ctrl || !desc->parent_ctrl || !desc->template_part || !desc->new_name)
        return SF6CORE_ERR_INVALID_ARG;
    *out_ctrl = nullptr;

    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, desc->template_ctrl, "ui_clone_row", true, &st);
    if (rc != SF6CORE_OK) return rc;
    if (!resolve_display_types()) return SF6CORE_ERR_NOT_READY;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    int step = 0;
    void* clone = nullptr;
    void *manager = nullptr, *group = nullptr, *children = nullptr, *name_str = nullptr, *part = nullptr;
    uint32_t layout = 0;
    int32_t index = 0;
    __try {
        RawInvokeRet ret;
        // manager = source.get_Manager(); children = manager.<OwnerParts>k__BackingField._Children
        step = 1;
        if (!clone_call(sdk, desc->template_part, "get_Manager()", nullptr, 0, &ret) || !ret.ptr) goto fail;
        manager = ret.ptr;
        group = read_field_ref(sdk, manager, "<OwnerParts>k__BackingField");
        children = group ? read_field_ref(sdk, group, "_Children") : nullptr;
        if (!children) goto fail;
        // layout = source.get_Layout()
        step = 2;
        if (!clone_call(sdk, desc->template_part, "get_Layout()", nullptr, 0, &ret)) goto fail;
        layout = ret.dword;
        // clone = src.duplicate(name)
        step = 3;
        name_str = sdk->functions->create_managed_string_normal(desc->new_name);
        if (!name_str) goto fail;
        { void* a[] = { name_str };
          if (!clone_call(sdk, desc->template_ctrl, "duplicate(System.String)", a, 1, &ret) || !ret.ptr) goto fail; }
        clone = ret.ptr;
        // parent.addChild(clone)
        step = 4;
        { void* a[] = { clone };
          if (!clone_call(sdk, desc->parent_ctrl, "addChild(via.gui.PlayObject)", a, 1, &ret)) goto fail; }
        // part = manager.Create(clone, parent)
        step = 5;
        { void* a[] = { clone, desc->parent_ctrl };
          if (!clone_call(sdk, manager, "Create(via.gui.Control,via.gui.Control)", a, 2, &ret) || !ret.ptr) goto fail; }
        part = ret.ptr;
        // part.set_Layout(layout)
        step = 6;
        { void* a[] = { (void*)(uintptr_t)layout };
          if (!clone_call(sdk, part, "set_Layout(app.UIPartsItemLayout)", a, 1, &ret)) goto fail; }
        // index = children.Count (re-read now); children.Add(part); part.set_Index(index)
        step = 7;
        if (!clone_call(sdk, children, "get_Count()", nullptr, 0, &ret)) goto fail;
        index = (int32_t)ret.dword;
        { void* a[] = { part };
          if (!clone_call(sdk, children, "Add(app.UIPartsItem)", a, 1, &ret)) goto fail; }
        step = 8;
        { void* a[] = { (void*)(intptr_t)index };
          if (!clone_call(sdk, part, "set_Index(System.Int32)", a, 1, &ret)) goto fail; }
        // part.set_IsDisable(disabled); part.set__IsLoop(true)
        step = 9;
        { void* a[] = { (void*)(uintptr_t)(desc->disabled ? 1 : 0) };
          if (!clone_call(sdk, part, "set_IsDisable(System.Boolean)", a, 1, &ret)) goto fail; }
        { void* a[] = { (void*)(uintptr_t)1 };
          if (!clone_call(sdk, part, "set__IsLoop(System.Boolean)", a, 1, &ret)) goto fail; }
        // clone.set_Position(via.vec3 {x,y,z}) -- 16-byte buffer like the original
        step = 10;
        { float pos[4] = { desc->pos_x, desc->pos_y, desc->pos_z, 0.0f };
          void* a[] = { (void*)pos };
          if (!clone_call(sdk, clone, "set_Position(via.vec3)", a, 1, &ret)) goto fail; }
        // visible
        step = 11;
        { void* a[] = { (void*)(uintptr_t)(desc->visible ? 1 : 0) };
          if (!clone_call(sdk, clone, "set_Visible(System.Boolean)", a, 1, &ret)) goto fail; }
        // title: clone.getObject("e_text_title").set_Message(title)
        step = 12;
        if (desc->title_text) {
            void* tn = sdk->functions->create_managed_string_normal("e_text_title");
            void* a[] = { tn };
            if (tn && clone_call(sdk, clone, "getObject(System.String)", a, 1, &ret) && ret.ptr) {
                void* title = ret.ptr;
                void* ts = sdk->functions->create_managed_string_normal(desc->title_text);
                if (ts) { void* b2[] = { ts }; clone_call(sdk, title, "set_Message(System.String)", b2, 1, &ret); }
            }
        }
        *out_ctrl = clone;
        step = 0;
fail:
        ;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, desc->template_ctrl, "ui_clone_row", st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    if (step != 0) {
        // a failed step: same outcome as the original's throw (no retry on this agent by the client)
        st->refused.fetch_add(1);
        core_log(SF6CORE_LOG_WARN, client->client_id, "ui_clone_row '%s' stopped at step %d%s", desc->new_name, step,
                 clone ? " (clone created, partially attached)" : "");
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, desc->template_ctrl, "ui_clone_row", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// ========================================================================
// obj_invoke -- generic guarded method invoke
// ========================================================================

SF6CoreStatus SF6CORE_CALL api_obj_invoke(SF6CoreClient* client, void* obj, const char* method_sig,
                                           const SF6CoreArg* args, uint32_t n_args, SF6CoreArg* out) {
    if (!method_sig) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, method_sig, false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    if (!sdk) return SF6CORE_ERR_NOT_READY;

    DWORD exc = 0;
    __try {
        auto mh = find_method_ll(sdk, obj, method_sig);
        if (!mh) return SF6CORE_ERR_NOT_FOUND;

        // Validate arg count
        uint32_t expected = sdk->method->get_num_params(mh);
        if (n_args != expected) return SF6CORE_ERR_INVALID_ARG;

        // Build native args array: each arg is 8 bytes (REFramework convention)
        void* native_args[16] = {};
        uint8_t arg_storage[16][16]; // temp storage for value type args
        for (uint32_t i = 0; i < n_args && i < 16; i++) {
            auto& a = args[i];
            switch ((SF6CoreArgType)a.type) {
            case SF6CORE_ARG_I64:
                native_args[i] = (void*)(uintptr_t)a.v.i;
                break;
            case SF6CORE_ARG_F64: {
                // Determine if the param wants float or double from the method definition
                // For simplicity: put a double in 8 bytes; REFramework will truncate if float
                memcpy(arg_storage[i], &a.v.f, sizeof(double));
                native_args[i] = (void*)*(uintptr_t*)arg_storage[i];
                break;
            }
            case SF6CORE_ARG_BOOL:
                native_args[i] = (void*)(uintptr_t)(a.v.i ? 1 : 0);
                break;
            case SF6CORE_ARG_PTR:
                native_args[i] = a.v.p;
                break;
            case SF6CORE_ARG_STR: {
                auto* ms = sdk->functions->create_managed_string_normal(a.v.s ? a.v.s : "");
                native_args[i] = (void*)ms;
                break;
            }
            default:
                return SF6CORE_ERR_INVALID_ARG;
            }
        }

        RawInvokeRet ret;
        bool ok = invoke_c(sdk, mh, obj, native_args, n_args, &ret);
        if (!ok || ret.exception_thrown) {
            DispatchCtx ctx = { client, obj, method_sig, st, true };
            on_display_fault(&ctx, 0);
            return SF6CORE_ERR_FAULTED;
        }

        // Fill output if requested
        if (out) {
            auto rt = sdk->method->get_return_type(mh);
            if (rt) {
                bool is_vt = sdk->type_definition->is_valuetype(rt);
                if (!is_vt && ret.ptr) {
                    out->type = SF6CORE_ARG_PTR;
                    out->v.p = ret.ptr;
                } else {
                    out->type = SF6CORE_ARG_I64;
                    out->v.i = ret.qword;
                }
            } else {
                out->type = SF6CORE_ARG_I64;
                out->v.i = ret.qword;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, method_sig, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, obj, method_sig, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// ========================================================================
// P1: data injection primitives
// ========================================================================

// obj_new: create a managed instance by type name
void* SF6CORE_CALL api_obj_new(SF6CoreClient* client, const char* type_name) {
    if (!client || !type_name) return nullptr;
    if (kill_switch_active() || core_closing() || client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER) return nullptr;
    auto* sdk = s_sdk;
    if (!sdk) return nullptr;

    DWORD exc = 0;
    void* result = nullptr;
    __try {
        auto tdb = sdk->functions->get_tdb();
        auto td = sdk->tdb->find_type(tdb, type_name);
        if (!td) return nullptr;
        auto mo = sdk->type_definition->create_instance(td, 0);
        if (mo) {
            sdk->managed_object->add_ref(mo);
            result = (void*)mo;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }
    return result;
}

// obj_clone: MemberwiseClone
void* SF6CORE_CALL api_obj_clone(SF6CoreClient* client, void* obj) {
    if (!client || !obj) return nullptr;
    if (kill_switch_active() || core_closing() || client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER) return nullptr;
    auto* sdk = s_sdk;
    if (!sdk) return nullptr;
    if (!resolve_display_types() || !s_dtc.m_obj_memberwise_clone) return nullptr;

    DWORD exc = 0;
    void* result = nullptr;
    __try {
        RawInvokeRet ret;
        if (invoke_c(sdk, s_dtc.m_obj_memberwise_clone, obj, nullptr, 0, &ret) && ret.ptr) {
            sdk->managed_object->add_ref((REFrameworkManagedObjectHandle)ret.ptr);
            result = ret.ptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
    }
    return result;
}

// obj_set_i64: write an integer/enum/bool field (any width)
SF6CoreStatus SF6CORE_CALL api_obj_set_i64(SF6CoreClient* client, void* obj, const char* field, int64_t v) {
    if (!field) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, field, false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj);
        REFrameworkFieldHandle fh = nullptr;
        while (td) {
            fh = sdk->type_definition->find_field(td, field);
            if (fh) break;
            td = sdk->type_definition->get_parent_type(td);
        }
        if (!fh) return SF6CORE_ERR_NOT_FOUND;
        void* raw = sdk->field->get_data_raw(fh, obj, false);
        if (!raw) return SF6CORE_ERR_FAULTED;

        // Determine field size from type
        auto ft = sdk->field->get_type(fh);
        uint32_t sz = ft ? sdk->type_definition->get_valuetype_size(ft) : 4;
        if (sz == 0) sz = 4;

        // Compare-before-write
        int64_t cur = 0;
        memcpy(&cur, raw, sz > 8 ? 8 : sz);
        if (cur == v) { st->skipped_identical.fetch_add(1); return SF6CORE_OK; }

        memcpy(raw, &v, sz > 8 ? 8 : sz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, field, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, obj, field, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// obj_set_f64: write a float or double field
SF6CoreStatus SF6CORE_CALL api_obj_set_f64(SF6CoreClient* client, void* obj, const char* field, double v) {
    if (!field) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, field, false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj);
        REFrameworkFieldHandle fh = nullptr;
        while (td) {
            fh = sdk->type_definition->find_field(td, field);
            if (fh) break;
            td = sdk->type_definition->get_parent_type(td);
        }
        if (!fh) return SF6CORE_ERR_NOT_FOUND;
        void* raw = sdk->field->get_data_raw(fh, obj, false);
        if (!raw) return SF6CORE_ERR_FAULTED;

        auto ft = sdk->field->get_type(fh);
        uint32_t sz = ft ? sdk->type_definition->get_valuetype_size(ft) : 4;
        if (sz <= 4) {
            float fv = (float)v;
            if (*(float*)raw == fv) { st->skipped_identical.fetch_add(1); return SF6CORE_OK; }
            *(float*)raw = fv;
        } else {
            if (*(double*)raw == v) { st->skipped_identical.fetch_add(1); return SF6CORE_OK; }
            *(double*)raw = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, field, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, obj, field, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// obj_set_string: write a string field using create_managed_string_normal
SF6CoreStatus SF6CORE_CALL api_obj_set_string(SF6CoreClient* client, void* obj, const char* field, const char* utf8) {
    if (!field) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, field, false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    bool found = false;
    __try {
        auto* str = sdk->functions->create_managed_string_normal(utf8 ? utf8 : "");
        if (!str) return SF6CORE_ERR_FAULTED;
        found = write_field_managed(sdk, obj, field, (void*)str);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, field, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    if (!found) { st->refused.fetch_add(1); return SF6CORE_ERR_NOT_FOUND; }
    DispatchCtx ctx = { client, obj, field, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// obj_set_ref: managed reference field assignment (R34)
SF6CoreStatus SF6CORE_CALL api_obj_set_ref(SF6CoreClient* client, void* obj, const char* field, void* value) {
    if (!field) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, field, false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    bool found = false;
    __try {
        found = write_field_managed(sdk, obj, field, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, field, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    if (!found) { st->refused.fetch_add(1); return SF6CORE_ERR_NOT_FOUND; }
    DispatchCtx ctx = { client, obj, field, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// obj_set_color: via.Color value field (one 4-byte rgba)
SF6CoreStatus SF6CORE_CALL api_obj_set_color(SF6CoreClient* client, void* obj, const char* field, uint32_t rgba) {
    if (!field) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, field, false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj);
        REFrameworkFieldHandle fh = nullptr;
        while (td) {
            fh = sdk->type_definition->find_field(td, field);
            if (fh) break;
            td = sdk->type_definition->get_parent_type(td);
        }
        if (!fh) return SF6CORE_ERR_NOT_FOUND;
        {   // the field must really be a 4-byte value type (via.Color): never write past it
            auto ft = sdk->field->get_type(fh);
            if (!ft || sdk->type_definition->get_valuetype_size(ft) != sizeof(ViaColor)) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }
        }
        void* raw = sdk->field->get_data_raw(fh, obj, false);
        if (!raw) return SF6CORE_ERR_FAULTED;

        ViaColor c = rgba_to_via_color(rgba);
        ViaColor cur;
        memcpy(&cur, raw, sizeof(ViaColor));
        if (via_color_to_rgba(cur) == rgba) {
            st->skipped_identical.fetch_add(1);
            return SF6CORE_OK;
        }
        memcpy(raw, &c, sizeof(ViaColor));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, field, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, obj, field, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// core_obj_set_guid (internal, used by the pause-menu engine; not in the API table): System.Guid value field (16 bytes), a text GUID
// into TrainingMenuData._MessageID. Same guard chain as obj_set_*, plus a type check: the field must be a
// System.Guid, never written past.
static bool is_guid_field_type(const REFrameworkSDKData* sdk, REFrameworkTypeDefinitionHandle ft) {
    if (!ft || sdk->type_definition->get_valuetype_size(ft) != 16) return false;
    if (s_dtc.t_guid && ft == s_dtc.t_guid) return true;
    const char* ns = sdk->type_definition->get_namespace(ft);
    const char* nm = sdk->type_definition->get_name(ft);
    return ns && nm && strcmp(ns, "System") == 0 && strcmp(nm, "Guid") == 0;
}

SF6CoreStatus core_obj_set_guid(SF6CoreClient* client, void* obj, const char* field, const uint8_t guid16[16]) {
    if (!field || !guid16) return SF6CORE_ERR_INVALID_ARG;
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, obj, field, false, &st);
    if (rc != SF6CORE_OK) return rc;
    resolve_display_types();   // System.Guid's handle; the name check covers a failed resolution

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj);
        REFrameworkFieldHandle fh = nullptr;
        while (td) {
            fh = sdk->type_definition->find_field(td, field);
            if (fh) break;
            td = sdk->type_definition->get_parent_type(td);
        }
        if (!fh) return SF6CORE_ERR_NOT_FOUND;
        if (!is_guid_field_type(sdk, sdk->field->get_type(fh))) { st->refused.fetch_add(1); return SF6CORE_ERR_TYPE; }
        void* raw = sdk->field->get_data_raw(fh, obj, false);
        if (!raw) return SF6CORE_ERR_FAULTED;

        if (memcmp(raw, guid16, 16) == 0) {
            st->skipped_identical.fetch_add(1);
            return SF6CORE_OK;
        }
        memcpy(raw, guid16, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        DispatchCtx ctx = { client, obj, field, st, false };
        on_display_fault(&ctx, exc);
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, obj, field, st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// list_add: Add(item) on a managed List
SF6CoreStatus SF6CORE_CALL api_list_add(SF6CoreClient* client, void* list, void* item) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, list, "list_add", false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    DWORD exc = 0;
    __try {
        auto m_add = find_method_ll(sdk, list, "Add");
        if (!m_add) return SF6CORE_ERR_NOT_FOUND;
        RawInvokeRet ret;
        void* args[] = { item };
        if (!invoke_c(sdk, m_add, list, args, 1, &ret) || ret.exception_thrown)
            return SF6CORE_ERR_FAULTED;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, list, "list_add", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// list_set_item: set_Item(index, item) on a managed List
SF6CoreStatus SF6CORE_CALL api_list_set_item(SF6CoreClient* client, void* list, int32_t index, void* item) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, list, "list_set_item", false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    __try {
        auto m_set = find_method_ll(sdk, list, "set_Item");
        if (!m_set) return SF6CORE_ERR_NOT_FOUND;
        RawInvokeRet ret;
        void* args[] = { (void*)(uintptr_t)index, item };
        if (!invoke_c(sdk, m_set, list, args, 2, &ret))
            return SF6CORE_ERR_FAULTED;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, list, "list_set_item", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// list_get_range: GetRange(start, count) -> new List (caller-owned ref)
void* SF6CORE_CALL api_list_get_range(SF6CoreClient* client, void* list, int32_t start, int32_t count) {
    if (!client || !list) return nullptr;
    if (kill_switch_active() || core_closing() || client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER) return nullptr;
    auto* sdk = s_sdk;
    if (!sdk) return nullptr;

    void* result = nullptr;
    __try {
        auto m_gr = find_method_ll(sdk, list, "GetRange");
        if (!m_gr) return nullptr;
        RawInvokeRet ret;
        void* args[] = { (void*)(uintptr_t)start, (void*)(uintptr_t)count };
        if (invoke_c(sdk, m_gr, list, args, 2, &ret) && ret.ptr) {
            sdk->managed_object->add_ref((REFrameworkManagedObjectHandle)ret.ptr);
            result = ret.ptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// dict_add: Dictionary.Add(key, value)
SF6CoreStatus SF6CORE_CALL api_dict_add(SF6CoreClient* client, void* dict, int64_t key, void* value) {
    DisplayStatsInternal* st = nullptr;
    auto rc = guard_prefix(client, dict, "dict_add", false, &st);
    if (rc != SF6CORE_OK) return rc;

    auto* sdk = s_sdk;
    __try {
        auto m_add = find_method_ll(sdk, dict, "Add");
        if (!m_add) return SF6CORE_ERR_NOT_FOUND;
        RawInvokeRet ret;
        void* args[] = { (void*)(uintptr_t)key, value };
        if (!invoke_c(sdk, m_add, dict, args, 2, &ret) || ret.exception_thrown)
            return SF6CORE_ERR_FAULTED;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SF6CORE_ERR_FAULTED;
    }
    DispatchCtx ctx = { client, dict, "dict_add", st, false };
    on_display_success(&ctx);
    return SF6CORE_OK;
}

// dict_contains: ContainsKey(key) -> bool
int32_t SF6CORE_CALL api_dict_contains(void* dict, int64_t key) {
    if (!dict || core_closing()) return 0;
    auto* sdk = s_sdk;
    if (!sdk) return 0;

    int32_t result = 0;
    __try {
        auto m_ck = find_method_ll(sdk, dict, "ContainsKey");
        if (!m_ck) return 0;
        RawInvokeRet ret;
        void* args[] = { (void*)(uintptr_t)key };
        if (invoke_c(sdk, m_ck, dict, args, 1, &ret))
            result = (ret.dword != 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// ========================================================================
// Read-only queries: ui_find_child, ui_find_agent
// ========================================================================

// Recursive child search by name, up to max_depth
void* SF6CORE_CALL api_ui_find_child(void* root, const char* name, int32_t max_depth) {
    if (!root || !name || max_depth < 0 || core_closing()) return nullptr;
    if (!resolve_display_types()) return nullptr;
    auto* sdk = s_sdk;
    if (!sdk) return nullptr;

    void* result = nullptr;
    __try {

        // BFS-style: try get_Child, check name, then get_Next for siblings
        // For depth > 0, recurse into children
        struct Frame { void* ctrl; int depth; };
        Frame stack[128];
        int sp = 0;
        stack[sp++] = { root, 0 };

        while (sp > 0 && !result) {
            auto [ctrl, depth] = stack[--sp];
            RawInvokeRet ret;
            if (!invoke_c(sdk, find_method_ll(sdk, ctrl, "get_Child"), ctrl, nullptr, 0, &ret) || !ret.ptr) continue;
            void* child = ret.ptr;
            while (child && !result) {
                RawInvokeRet nr;
                if (invoke_c(sdk, find_method_ll(sdk, child, "get_Name"), child, nullptr, 0, &nr) && nr.ptr) {
                    char nm[256];
                    if (managed_str_to_utf8(nr.ptr, nm, sizeof(nm)) && strcmp(nm, name) == 0) {
                        result = child;
                        break;
                    }
                }
                if (depth < max_depth && sp < 127)
                    stack[sp++] = { child, depth + 1 };
                if (!invoke_c(sdk, find_method_ll(sdk, child, "get_Next"), child, nullptr, 0, &ret)) break;
                child = ret.ptr;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}

// Find a UIAgent by its GameObject name.
// Now uses the agent registry (discovery.cpp) instead of rescanning _Entries.
void* SF6CORE_CALL api_ui_find_agent(const char* gameobject_name) {
    if (core_closing()) return nullptr;
    return discovery_find_first_agent(gameobject_name);
}

// ========================================================================
// get_display_stats
// ========================================================================

SF6CoreStatus SF6CORE_CALL api_get_display_stats(SF6CoreClient* client, SF6CoreDisplayStats* out) {
    if (!client || !out) return SF6CORE_ERR_INVALID_ARG;
    if (out->struct_size < sizeof(SF6CoreDisplayStats)) return SF6CORE_ERR_INVALID_ARG;
    auto* st = get_stats(client);
    out->writes           = st->writes.load();
    out->skipped_identical = st->skipped_identical.load();
    out->refused          = st->refused.load();
    out->faults           = st->faults.load();
    out->quarantined      = st->quarantined.load();
    return SF6CORE_OK;
}

// ========================================================================
// Init logging for display API
// ========================================================================

void display_log_init() {
    core_log(SF6CORE_LOG_INFO, nullptr,
             "display API v0.3.0 initialized, Lua bridge active");
}
