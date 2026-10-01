/*
 * Vendored from: SF6_NativeUICore/include/sf6core/sf6core_api.h
 * Keep this file identical to the upstream. Do not modify.
 */

/*
 * sf6core_api.h -- SF6 Native UI Core: Public C ABI v1
 *
 * Pure C header (usable from C and C++). No C++ types, no exceptions
 * across this boundary.
 *
 * The SF6CoreAPI struct is APPEND-ONLY: future minor versions may add
 * new function pointers after the existing ones. Existing fields never
 * move, change type, or change semantics. Clients MUST NOT read past
 * struct_size bytes.
 *
 * String ownership: the core copies every string the client passes.
 * The client need not keep the string alive after the call returns.
 *
 * Threading: all callbacks are invoked on the game thread inside the
 * core's single LateUpdateBehavior callback. register_client and
 * unregister_client acquire an internal mutex and are safe to call
 * from a plugin's reframework_plugin_initialize. Callbacks never fire
 * before the core's own reframework_plugin_initialize has completed.
 *
 * Pre-init safety (design doc section 11.0): sf6core_get_api and
 * register_client are safe to call BEFORE the core's own
 * reframework_plugin_initialize runs. The function table is statically
 * allocated; registrations are queued and replayed when the core
 * initializes. Clients should call sf6core_get_api and register_client
 * in their own reframework_plugin_initialize (never in DllMain or
 * reframework_plugin_required_version).
 */

#ifndef SF6CORE_API_H
#define SF6CORE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version ---------------------------------------------------------- */

#define SF6CORE_ABI_VERSION  1

/* ---- Calling convention ----------------------------------------------- */

#define SF6CORE_CALL __cdecl

/* ---- Opaque handle ---------------------------------------------------- */

typedef struct SF6CoreClient SF6CoreClient;

/* ---- Enumerations ----------------------------------------------------- */

typedef enum SF6CoreStatus {
    SF6CORE_OK               = 0,
    SF6CORE_ERR_INVALID_ARG  = 1,
    SF6CORE_ERR_DUPLICATE_ID = 2,
    SF6CORE_ERR_DISABLED     = 3,
    SF6CORE_ERR_ABI_MISMATCH = 4,
    SF6CORE_ERR_NOT_FOUND    = 5,
    /* increment 3 (display API) */
    SF6CORE_ERR_WRONG_THREAD = 6,   /* element write requested off the game thread: refused */
    SF6CORE_ERR_FAULTED      = 7,   /* the engine call faulted (SEH or managed exception), or the
                                       element is quarantined after an earlier fault */
    SF6CORE_ERR_TYPE         = 8,   /* object is not of the expected type / not alive */
    SF6CORE_ERR_NOT_READY    = 9    /* resource not loaded yet, core not initialized yet */
} SF6CoreStatus;

typedef enum SF6CoreClientState {
    SF6CORE_CLIENT_ACTIVE              = 0,
    SF6CORE_CLIENT_DISABLED_BY_BREAKER = 1,
    SF6CORE_CLIENT_PENDING_INIT        = 2
} SF6CoreClientState;

typedef enum SF6CoreLogLevel {
    SF6CORE_LOG_ERROR = 0,
    SF6CORE_LOG_WARN  = 1,
    SF6CORE_LOG_INFO  = 2,
    SF6CORE_LOG_DEBUG = 3
} SF6CoreLogLevel;

/* ---- Callback signatures ---------------------------------------------- */

typedef void (SF6CORE_CALL *SF6CoreFlowCb)(
    int32_t old_id, int32_t new_id, void* user);

typedef void (SF6CORE_CALL *SF6CoreFrameCb)(
    uint64_t frame, void* user);

/* ---- Client descriptor ------------------------------------------------ */
/*
 * Passed to register_client. The core copies all strings.
 *
 * struct_size : sizeof(SF6CoreClientDesc).
 * client_id   : unique, non-NULL, max 63 chars. Duplicate = error.
 * client_version : informational, max 31 chars. May be NULL.
 * priority    : dispatch order (ascending). Ties broken by client_id
 *               byte-wise comparison (strcmp). Deterministic.
 */
typedef struct SF6CoreClientDesc {
    uint32_t    struct_size;
    const char* client_id;
    const char* client_version;
    int32_t     priority;
} SF6CoreClientDesc;

/* ---- Widget types (increment 2) ---------------------------------------- */

typedef struct SF6CoreWidget SF6CoreWidget;

typedef enum SF6CoreWidgetState {
    SF6CORE_WIDGET_PENDING  = 0,
    SF6CORE_WIDGET_LIVE     = 1,
    SF6CORE_WIDGET_FAILED   = 2
} SF6CoreWidgetState;

/*
 * Widget descriptor. Passed to create_widget.
 *
 * struct_size      : sizeof(SF6CoreWidgetDesc).
 * prefab_path      : UTF-8 path to the .pfb resource (copied by the core).
 * flow_ids         : array of flow IDs where the widget may be visible.
 *                    NULL or n_flow_ids == 0 means "visible in all flows".
 * n_flow_ids       : number of entries in flow_ids.
 * initial_x/y      : initial GUI position (EXPERIMENTAL -- see docs).
 * initially_visible: 1 = visible (subject to flow context), 0 = hidden.
 */
typedef struct SF6CoreWidgetDesc {
    uint32_t       struct_size;
    const char*    prefab_path;
    const int32_t* flow_ids;
    uint32_t       n_flow_ids;
    float          initial_x;
    float          initial_y;
    int32_t        initially_visible;
} SF6CoreWidgetDesc;

/* ---- Display API types (increment 3, ADR-0011 Proposed) ----------------- */

/* Generic argument for obj_invoke. The core converts each value to the
 * method's real parameter type (int8..int64, uint*, float, double, bool,
 * enum, System.String from str, object/pointer from ptr). */
typedef enum SF6CoreArgType {
    SF6CORE_ARG_I64  = 0,
    SF6CORE_ARG_F64  = 1,
    SF6CORE_ARG_BOOL = 2,
    SF6CORE_ARG_PTR  = 3,
    SF6CORE_ARG_STR  = 4    /* UTF-8, converted to a managed System.String */
} SF6CoreArgType;

typedef struct SF6CoreArg {
    int32_t type;           /* SF6CoreArgType */
    int32_t reserved;
    union { int64_t i; double f; void* p; const char* s; } v;
} SF6CoreArg;

/* Row clone request (P4): the full "LL recipe" for adding a row to a
 * UIPartsList-style menu. Must be called from inside the game's own update
 * hook of that screen (e.g. a post-hook of <Flow>.Main.OnUpdate), never from
 * REFramework's UpdateBehavior: the engine hands back an unfinished object
 * there (see SF6_MenuRowsNative history). */
typedef struct SF6CoreRowCloneDesc {
    uint32_t    struct_size;
    void*       template_ctrl;    /* via.gui control of the row to duplicate */
    void*       parent_ctrl;      /* via.gui control the clone is attached to (addChild) */
    void*       parts_owner;      /* UIParts list owner whose _Children receives the part */
    const char* new_name;         /* GameObject/control name of the clone, e.g. "p_Spin_drvimpact" */
    int32_t     index;            /* set_Index value */
    float       pos_y;            /* local Y position of the clone */
    int32_t     visible;          /* initial set_Visible */
    int32_t     disabled;         /* initial set_IsDisable */
    const char* title_text;       /* UTF-8 text written to the clone's e_text_title, NULL = none */
} SF6CoreRowCloneDesc;

/* Per-client display statistics (diagnostics, TRC tests). */
typedef struct SF6CoreDisplayStats {
    uint32_t struct_size;
    uint64_t writes;              /* engine writes performed */
    uint64_t skipped_identical;   /* writes skipped because the value was unchanged */
    uint64_t refused;             /* refused: wrong type, dead object, wrong thread, disabled */
    uint64_t faults;              /* engine calls that faulted (SEH or managed exception) */
    uint64_t quarantined;         /* calls refused because the element faulted before */
} SF6CoreDisplayStats;

/*
 * Check whether a function pointer is readable in the API struct.
 * Usage: if (SF6CORE_HAS(api, create_widget)) { api->create_widget(...); }
 */
#define SF6CORE_HAS(api, member) \
    ((api)->struct_size >= \
     (uint32_t)( (size_t)(&((const SF6CoreAPI*)0)->member) + sizeof((api)->member) ))

/* ---- Function pointer type for GetProcAddress loading ----------------- */

typedef const struct SF6CoreAPI* (SF6CORE_CALL *SF6CoreGetApiFn)(
    uint32_t abi_version);

/* ---- API function table ----------------------------------------------- */

typedef struct SF6CoreAPI {
    uint32_t abi_version;
    uint32_t struct_size;

    /* Core version string (static, e.g. "0.1.0"). */
    const char* (SF6CORE_CALL *get_version)(void);

    /* Register a client. Writes handle to *out on success.
     * Safe to call before core init (queued; state = PENDING_INIT). */
    SF6CoreStatus (SF6CORE_CALL *register_client)(
        const SF6CoreClientDesc* desc,
        SF6CoreClient**          out);

    /* Unregister a client. Handle is invalid after this call.
     * Must NOT be called from inside a callback. */
    void (SF6CORE_CALL *unregister_client)(SF6CoreClient* client);

    /* Set the flow-change callback. NULL cb clears it. */
    SF6CoreStatus (SF6CORE_CALL *set_flow_callback)(
        SF6CoreClient* client,
        SF6CoreFlowCb  cb,
        void*          user);

    /* Set the per-frame callback. NULL cb clears it. */
    SF6CoreStatus (SF6CORE_CALL *set_frame_callback)(
        SF6CoreClient* client,
        SF6CoreFrameCb cb,
        void*          user);

    /* Current flow ID from _FlowMap._ID. Returns -1 if unknown. */
    int32_t (SF6CORE_CALL *get_flow_id)(void);

    /* Log through the core (file + REFramework log). */
    void (SF6CORE_CALL *log)(
        SF6CoreClient*  client,
        SF6CoreLogLevel level,
        const char*     msg);

    /* Query the client's current state. */
    SF6CoreClientState (SF6CORE_CALL *get_client_state)(
        SF6CoreClient* client);

    /* -- Increment 2: Widgets (Tier 3, ADR-0003) -------------------------- */

    SF6CoreStatus (SF6CORE_CALL *create_widget)(
        SF6CoreClient*            client,
        const SF6CoreWidgetDesc*  desc,
        SF6CoreWidget**           out);

    SF6CoreWidgetState (SF6CORE_CALL *get_widget_state)(
        SF6CoreWidget* widget);

    SF6CoreStatus (SF6CORE_CALL *widget_set_text)(
        SF6CoreWidget* widget,
        const char*    element_path,
        const char*    utf8_text);

    SF6CoreStatus (SF6CORE_CALL *widget_set_visible)(
        SF6CoreWidget* widget,
        int32_t        visible);

    SF6CoreStatus (SF6CORE_CALL *widget_set_position)(
        SF6CoreWidget* widget,
        float          x,
        float          y);

    SF6CoreStatus (SF6CORE_CALL *destroy_widget)(
        SF6CoreWidget* widget);

    /* -- Increment 2: Element visibility (for ZenMode port) --------------- */

    SF6CoreStatus (SF6CORE_CALL *set_element_hidden)(
        SF6CoreClient* client,
        const char*    gui_owner_name,
        const char*    element_path,
        int32_t        hidden);

    /* -- Increment 3: Display API (ADR-0011, Proposed) --------------------- */

    /* P2 text */
    SF6CoreStatus (SF6CORE_CALL *ui_set_text)(SF6CoreClient* client, void* text, const char* utf8);
    SF6CoreStatus (SF6CORE_CALL *ui_set_message_id)(SF6CoreClient* client, void* text, const uint8_t guid16[16]);
    SF6CoreStatus (SF6CORE_CALL *ui_get_message_id)(void* text, uint8_t out_guid16[16]);
    SF6CoreStatus (SF6CORE_CALL *ui_set_font_size)(SF6CoreClient* client, void* text, float size);
    SF6CoreStatus (SF6CORE_CALL *ui_set_region_size)(SF6CoreClient* client, void* text, float w, float h);

    /* P3 colour / texture */
    SF6CoreStatus (SF6CORE_CALL *ui_set_color)(SF6CoreClient* client, void* ctrl, uint32_t rgba);
    SF6CoreStatus (SF6CORE_CALL *ui_texture_load)(SF6CoreClient* client, const char* resource_path, void** out_holder);
    int32_t       (SF6CORE_CALL *ui_texture_ready)(void* holder);
    SF6CoreStatus (SF6CORE_CALL *ui_set_texture)(SF6CoreClient* client, void* texture_ctrl, void* holder);

    /* P6 visibility / layout / input */
    SF6CoreStatus (SF6CORE_CALL *ui_set_visible)(SF6CoreClient* client, void* ctrl, int32_t visible);
    SF6CoreStatus (SF6CORE_CALL *ui_set_force_invisible)(SF6CoreClient* client, void* ctrl, int32_t on);
    SF6CoreStatus (SF6CORE_CALL *ui_set_position)(SF6CoreClient* client, void* ctrl, float x, float y, float z);
    SF6CoreStatus (SF6CORE_CALL *ui_set_scale)(SF6CoreClient* client, void* ctrl, float x, float y, float z);
    SF6CoreStatus (SF6CORE_CALL *ui_set_size)(SF6CoreClient* client, void* ctrl, float w, float h);
    SF6CoreStatus (SF6CORE_CALL *ui_set_block_input)(SF6CoreClient* client, void* agent, int32_t block);

    /* P4 clone a row (see SF6CoreRowCloneDesc) */
    SF6CoreStatus (SF6CORE_CALL *ui_clone_row)(SF6CoreClient* client, const SF6CoreRowCloneDesc* desc, void** out_ctrl);

    SF6CoreStatus (SF6CORE_CALL *obj_invoke)(SF6CoreClient* client, void* obj, const char* method_sig,
                                             const SF6CoreArg* args, uint32_t n_args, SF6CoreArg* out);

    /* P1 data injection into native data-driven lists */
    void*         (SF6CORE_CALL *obj_new)(SF6CoreClient* client, const char* type_name);
    void*         (SF6CORE_CALL *obj_clone)(SF6CoreClient* client, void* obj);
    SF6CoreStatus (SF6CORE_CALL *obj_set_i64)(SF6CoreClient* client, void* obj, const char* field, int64_t v);
    SF6CoreStatus (SF6CORE_CALL *obj_set_f64)(SF6CoreClient* client, void* obj, const char* field, double v);
    SF6CoreStatus (SF6CORE_CALL *obj_set_string)(SF6CoreClient* client, void* obj, const char* field, const char* utf8);
    SF6CoreStatus (SF6CORE_CALL *obj_set_ref)(SF6CoreClient* client, void* obj, const char* field, void* value);
    SF6CoreStatus (SF6CORE_CALL *obj_set_color)(SF6CoreClient* client, void* obj, const char* field, uint32_t rgba);
    SF6CoreStatus (SF6CORE_CALL *list_add)(SF6CoreClient* client, void* list, void* item);
    SF6CoreStatus (SF6CORE_CALL *list_set_item)(SF6CoreClient* client, void* list, int32_t index, void* item);
    void*         (SF6CORE_CALL *list_get_range)(SF6CoreClient* client, void* list, int32_t start, int32_t count);
    SF6CoreStatus (SF6CORE_CALL *dict_add)(SF6CoreClient* client, void* dict, int64_t key, void* value);
    int32_t       (SF6CORE_CALL *dict_contains)(void* dict, int64_t key);

    /* Read-only queries shared by all clients */
    void*         (SF6CORE_CALL *ui_find_child)(void* root, const char* name, int32_t max_depth);
    void*         (SF6CORE_CALL *ui_find_agent)(const char* gameobject_name);

    /* Diagnostics */
    SF6CoreStatus (SF6CORE_CALL *get_display_stats)(SF6CoreClient* client, SF6CoreDisplayStats* out);

} SF6CoreAPI;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SF6CORE_API_H */
