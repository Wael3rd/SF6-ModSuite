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
 * initial_x/y      : initial GUI position (EXPERIMENTAL — see docs).
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

/* ---- Lua bridge native-function types (increment 5, ADR-0002) ----------- */

typedef enum SF6CoreValType {
    SF6CORE_VAL_NIL    = 0,
    SF6CORE_VAL_BOOL   = 1,
    SF6CORE_VAL_INT    = 2,
    SF6CORE_VAL_DOUBLE = 3,
    SF6CORE_VAL_STRING = 4
} SF6CoreValType;

typedef struct SF6CoreValue {
    int32_t     type;       /* SF6CoreValType */
    int32_t     b;          /* boolean value (when type == BOOL) */
    int64_t     i;          /* integer value (when type == INT; managed addrs travel here) */
    double      d;          /* double  value (when type == DOUBLE) */
    const char* s;          /* string  value (when type == STRING); see ownership rules */
} SF6CoreValue;

/* Native function callable from Lua. Returns the number of values placed
 * in rets[0..max_rets-1], or < 0 on error (the bridge raises a Lua error
 * with a generic message). Runs on the calling Lua thread; the function
 * must NOT longjmp. Arg strings are valid only during the call. Returned
 * strings must stay valid until the function returns (the bridge copies
 * them into Lua before unwinding); a thread_local buffer is the expected
 * pattern. Max 16 args / 16 rets. */
typedef int32_t (SF6CORE_CALL *SF6CoreLuaFn)(
    void*              user,
    const SF6CoreValue* args,
    uint32_t           n_args,
    SF6CoreValue*      rets,
    uint32_t           max_rets);

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
    void*       template_ctrl;    /* via.gui control of the row to duplicate (the template part's <Control>) */
    void*       parent_ctrl;      /* via.gui control the clone is attached to (template_ctrl.get_Parent()) */
    void*       template_part;    /* the template row's UIParts item: its get_Manager() creates the new part,
                                     its get_Layout() is copied, Manager.<OwnerParts>._Children receives it */
    const char* new_name;         /* GameObject/control name of the clone, e.g. "p_Spin_drvimpact" */
    float       pos_x, pos_y, pos_z; /* local position of the clone (via.vec3) */
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

/* ---- Option dialog types (increment 6, ADR-0013) ---------------------- */

/* The game's own option window (app.Option units shown by
 * UIFlowOptionBGDialog) filled with the clients' groups and entries. One
 * engine for every client: the core builds the tree from all the groups,
 * drives the dialogs once per frame (before the clients' frame callbacks)
 * and routes each entry's events to the client that created it. Group and
 * entry ids are the core's (>= 0), unique across clients. */
typedef enum SF6CoreOptKind {
    SF6CORE_OPT_BUTTON = 0,
    SF6CORE_OPT_SLIDER = 1,
    SF6CORE_OPT_CHOICE = 2
} SF6CoreOptKind;

typedef struct SF6CoreOptEntryDesc {
    uint32_t           struct_size;
    int32_t            kind;          /* SF6CoreOptKind */
    const char*        key;           /* the entry's key (NULL = "") */
    const char*        title;
    const char*        desc;
    const char* const* options;       /* choice: the labels */
    uint32_t           n_options;
    int64_t            min_val;       /* slider range */
    int64_t            max_val;
    int64_t            value;         /* choice: 1-based; slider: raw; button: unused */
} SF6CoreOptEntryDesc;

/* What a button press asks for, filled by the owner's on_button. */
typedef struct SF6CoreOptButtonResult {
    int32_t kind;                     /* 0 nothing, 1 close the window, 2 open the page `key` */
    int32_t rebuild;                  /* kind 2: rebuild the tree first */
    int32_t push;                     /* kind 2: the page goes on top of the current one */
    int32_t pop;                      /* kind 2: back to the parent page */
    char    key[128];
} SF6CoreOptButtonResult;

/* Per-client callbacks, called on the game thread from the core's frame
 * (or its BeginRendering pump), guarded like frame callbacks. A callback
 * may call the opt_* functions (re-entrant: nothing is locked meanwhile).
 * Any member may be NULL. */
typedef struct SF6CoreOptCallbacks {
    uint32_t struct_size;
    void*    user;
    /* one of the client's entries changed in the game (choice 1-based, slider raw) */
    void (SF6CORE_CALL *on_value)(void* user, int64_t entry, int64_t value);
    /* one of the client's buttons was pressed; out is zeroed before the call */
    void (SF6CORE_CALL *on_button)(void* user, int64_t entry, SF6CoreOptButtonResult* out);
    /* every client: a dialog was created / a page was set up (rows = how many
     * of this client's entries it shows) / the top page was closed by the
     * game / the window closed */
    void (SF6CORE_CALL *on_dialog_created)(void* user, void* param);
    void (SF6CORE_CALL *on_page_setup)(void* user, void* param, uint32_t rows);
    void (SF6CORE_CALL *on_page_popped)(void* user, void* param);
    void (SF6CORE_CALL *on_window_closed)(void* user);
} SF6CoreOptCallbacks;

/* opt_navigate flags */
#define SF6CORE_OPT_NAV_REBUILD 1
#define SF6CORE_OPT_NAV_NOW     2
#define SF6CORE_OPT_NAV_PUSH    4
#define SF6CORE_OPT_NAV_POP     8

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

    /* Create a widget from a prefab. The widget is queued and instantiated
     * on the game thread in the commit phase. Returns a handle immediately
     * (state = PENDING). Returns ERR_INVALID_ARG if desc or out is NULL,
     * or if prefab_path is empty. */
    SF6CoreStatus (SF6CORE_CALL *create_widget)(
        SF6CoreClient*            client,
        const SF6CoreWidgetDesc*  desc,
        SF6CoreWidget**           out);

    /* Query a widget's current state (PENDING / LIVE / FAILED). */
    SF6CoreWidgetState (SF6CORE_CALL *get_widget_state)(
        SF6CoreWidget* widget);

    /* Set text on a widget element found by name path (slash-separated,
     * e.g. "c_main/item1/e_text_left"). Compare-before-write (R7).
     * Queued, applied in the commit phase. */
    SF6CoreStatus (SF6CORE_CALL *widget_set_text)(
        SF6CoreWidget* widget,
        const char*    element_path,
        const char*    utf8_text);

    /* Set client-requested visibility. Effective visibility =
     * client flag AND (flow_ids empty OR current_flow in flow_ids).
     * Re-evaluated on every flow change. Uses agent state Show/Hide. */
    SF6CoreStatus (SF6CORE_CALL *widget_set_visible)(
        SF6CoreWidget* widget,
        int32_t        visible);

    /* Set widget position (EXPERIMENTAL). Uses get_Position/set_Position
     * on the root via.gui control (via.vec3: x, y, z=0). */
    SF6CoreStatus (SF6CORE_CALL *widget_set_position)(
        SF6CoreWidget* widget,
        float          x,
        float          y);

    /* Queue widget destruction. Releases the managed object reference
     * and calls via.GameObject.destroy. Never calls remove(). */
    SF6CoreStatus (SF6CORE_CALL *destroy_widget)(
        SF6CoreWidget* widget);

    /* -- Increment 2: Element visibility (for ZenMode port) --------------- */

    /* Declare desired visibility for a game-owned GUI element.
     * gui_owner_name: the GUI agent's GameObject name (e.g.
     *   "BattleHud_AccountInfo"). element_path: slash-separated
     *   name path from the View root (e.g. "c_main/c_hud/c_RankInfo").
     *   Supports glob wildcards: '*' matches zero or more characters
     *   within a segment (e.g. "p_BattleHudAccountInfo_*").
     *   A segment of just '*' matches any child name.
     * hidden: 1 = hide (set_ForceInvisible true), 0 = restore original
     *   visibility. Enforced at draw time via on_pre_gui_draw_element
     *   (O(1) owner lookup, no per-frame tree scan when no hides).
     *   Re-resolved on flow changes. Restored when client unregisters
     *   or is breaker-disabled. */
    SF6CoreStatus (SF6CORE_CALL *set_element_hidden)(
        SF6CoreClient* client,
        const char*    gui_owner_name,
        const char*    element_path,
        int32_t        hidden);

    /* -- Increment 3: Display API (ADR-0011, Proposed) ---------------------
     *
     * The ONLY way client mods write the game's native UI. Generic: no
     * function is specific to one mod.
     *
     * Immediate mode: each call runs synchronously in the caller's context,
     * so a mod keeps its own timing (UX unchanged). Element writes (ui_*)
     * must be called on the game thread (any REFramework frame/update
     * callback or a game method hook running on it); otherwise they return
     * SF6CORE_ERR_WRONG_THREAD and write nothing. Data operations (obj_*,
     * list_*, dict_*) may run in any thread a game hook runs on.
     *
     * Every write goes through the same guard chain:
     *   1. kill switch / client disabled by the breaker -> ERR_DISABLED
     *   2. proof-of-life: object non-null and of the expected type (type
     *      definition check; NOT is_managed_object, which rejects clones)
     *   3. quarantine: an (object, operation) that faulted once is never
     *      attempted again -> ERR_FAULTED (a REFramework-"caught" fault is a
     *      crash precursor, 2026-10-01 23:38)
     *   4. compare-before-write: the current value is read first; an
     *      identical value is not written (R7) -> SF6CORE_OK
     *   5. the engine call, under SEH, with managed-exception detection;
     *      a fault quarantines the (object, operation), counts toward the
     *      client's breaker (ADR-0008) and returns ERR_FAULTED
     *   6. managed reference fields are assigned with engine semantics:
     *      add_ref(new) -> atomic exchange -> release(old) (R34)
     *
     * Objects are raw managed-object pointers (void*), as REFramework hands
     * them out (Lua: obj:get_address()). Objects returned by obj_new,
     * obj_clone, list_get_range and ui_texture_load carry one reference owned
     * by the caller (never released by the core).
     */

    /* P2 text */
    SF6CoreStatus (SF6CORE_CALL *ui_set_text)(SF6CoreClient* client, void* text, const char* utf8);
    SF6CoreStatus (SF6CORE_CALL *ui_set_message_id)(SF6CoreClient* client, void* text, const uint8_t guid16[16]);
    SF6CoreStatus (SF6CORE_CALL *ui_get_message_id)(void* text, uint8_t out_guid16[16]);
    SF6CoreStatus (SF6CORE_CALL *ui_set_font_size)(SF6CoreClient* client, void* text, float w, float h); /* via.gui.Text.set_FontSize takes a via.Size */
    SF6CoreStatus (SF6CORE_CALL *ui_set_region_size)(SF6CoreClient* client, void* text, float w, float h);

    /* P3 colour / texture */
    SF6CoreStatus (SF6CORE_CALL *ui_set_color)(SF6CoreClient* client, void* ctrl, uint32_t rgba); /* set_Color + the 4 corner colours; rgba = raw via.Color.rgba value, unchanged */
    SF6CoreStatus (SF6CORE_CALL *ui_texture_load)(SF6CoreClient* client, const char* resource_path, void** out_holder);
    int32_t       (SF6CORE_CALL *ui_texture_ready)(void* holder);   /* 1 once loaded (>= 60 frames after creation) */
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

    /* Guarded generic invoke, for the native calls that are part of driving
     * a screen but have no dedicated primitive (owner repaints such as
     * SetMessageColor, set_IsDisable, set_MinNum...). method_sig = name or
     * full signature ("SetColor(System.Int32)"). out may be NULL. */
    SF6CoreStatus (SF6CORE_CALL *obj_invoke)(SF6CoreClient* client, void* obj, const char* method_sig,
                                             const SF6CoreArg* args, uint32_t n_args, SF6CoreArg* out);

    /* P1 data injection into native data-driven lists */
    void*         (SF6CORE_CALL *obj_new)(SF6CoreClient* client, const char* type_name);
    void*         (SF6CORE_CALL *obj_clone)(SF6CoreClient* client, void* obj);   /* MemberwiseClone */
    SF6CoreStatus (SF6CORE_CALL *obj_set_i64)(SF6CoreClient* client, void* obj, const char* field, int64_t v);   /* any integer/enum/bool field width */
    SF6CoreStatus (SF6CORE_CALL *obj_set_f64)(SF6CoreClient* client, void* obj, const char* field, double v);    /* float or double field */
    SF6CoreStatus (SF6CORE_CALL *obj_set_string)(SF6CoreClient* client, void* obj, const char* field, const char* utf8);
    SF6CoreStatus (SF6CORE_CALL *obj_set_ref)(SF6CoreClient* client, void* obj, const char* field, void* value); /* managed assignment (R34) */
    SF6CoreStatus (SF6CORE_CALL *obj_set_color)(SF6CoreClient* client, void* obj, const char* field, uint32_t rgba); /* via.Color (4-byte rgba) value field, raw value */
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

    /* -- Increment 4: Discovery service (ADR-0012, append-only) ----------- */

    /* Agent registry: all agents whose GameObject has this name, in
     * _Entries order. Registry rebuilt lazily (only when _version/_size
     * changes). Each returned pointer is proof-of-life validated. */
    uint32_t      (SF6CORE_CALL *ui_agents)(const char* gameobject_name, void** out, uint32_t max);

    /* Per-name change counter: increments when the set of agents with that
     * name changes (appears, disappears, address changes). Returns 0 for
     * an unknown name. */
    uint32_t      (SF6CORE_CALL *ui_agents_gen)(const char* gameobject_name);

    /* Global change counter: increments when any name's agent set changes. */
    uint32_t      (SF6CORE_CALL *ui_registry_gen)(void);

    /* Path-based cached lookup: on first call, walks the tree in find_name
     * order (the node, its child subtree, then its next siblings, depth
     * limit 8 on child steps) and records the move sequence (child/next).
     * On subsequent calls, follows the moves from the live root and checks
     * the name at the end; full walk only when that fails. Only objects
     * reached from the live root are touched. Bounded cache (1024 entries,
     * cleared when full). */
    void*         (SF6CORE_CALL *ui_find_cached)(void* root, const char* name);

    /* One walk returning the first match of each name in find_name order.
     * out[i] is NULL if names[i] was not found. Paths are recorded for
     * later ui_find_cached calls. Returns the number found.
     * n is capped at 32: only the first 32 names are searched. out[]
     * must have at least n entries; ALL n entries are zeroed before the
     * walk so callers never read garbage even for indices beyond 32. */
    uint32_t      (SF6CORE_CALL *ui_find_many)(void* root, const char* const* names,
                                                uint32_t n, void** out);

    /* ZenMode-style element hiding: Lua-pattern matching (%d, %d+ plus
     * literals, unanchored substring), ALL matching children at EVERY
     * segment level (fan-out), applied per-frame for owners sighted in the
     * last 2 frames. Compare-before-write makes repeated applies cheap.
     * Does NOT change the semantics of set_element_hidden.
     * pattern_segments is an array of n_segments pattern strings.
     * MUST be called on the game thread (returns ERR_WRONG_THREAD otherwise).
     * The ZenMode client queues changes from its ImGui panel and applies
     * them in its frame callback. */
    SF6CoreStatus (SF6CORE_CALL *set_element_hidden_zen)(
        SF6CoreClient* client,
        const char*    gui_owner_name,
        const char* const* pattern_segments,
        uint32_t       n_segments,
        int32_t        hidden);

    /* -- Increment 5: Bridge extensions + rules suspend (append-only) ----- */

    /* Query any client's state by id. Returns SF6CoreClientState, or -1
     * if no client with that id is registered. Read under the registry
     * mutex. */
    int32_t       (SF6CORE_CALL *client_state_by_id)(const char* client_id);

    /* Register a native C function callable from Lua as
     * SF6Core.clients[client_id].<name>(...). NULL fn unregisters that
     * name. Max name length 63 chars. See SF6CoreLuaFn doc for rules. */
    SF6CoreStatus (SF6CORE_CALL *lua_register_fn)(
        SF6CoreClient* client,
        const char*    name,
        SF6CoreLuaFn   fn,
        void*          user);

    /* Suspend / resume this client's element-hide rules (both
     * set_element_hidden and set_element_hidden_zen). While suspended,
     * the commit phase skips all of the client's rules entirely: no
     * write, no restore. Resuming re-applies on the next frame for
     * sighted owners. Mirrors ZenMode.lua doing nothing during a
     * replay.
     * MUST be called on the game thread (returns ERR_WRONG_THREAD otherwise). */
    SF6CoreStatus (SF6CORE_CALL *set_element_rules_suspended)(
        SF6CoreClient* client,
        int32_t        suspended);

    /* -- Increment 6: native option dialogs (ADR-0013, append-only) ------ */

    /* Callbacks of this client's groups and entries (copied). The engine's
     * hooks are installed on the first call of any client, on the next
     * frame. ERR when the engine could not be installed. */
    SF6CoreStatus (SF6CORE_CALL *opt_set_callbacks)(SF6CoreClient* client, const SF6CoreOptCallbacks* cb);
    /* A group = one page of the window. key NULL/"" = none (the title keys
     * the page). Returns the group id, -1 on failure. */
    int64_t       (SF6CORE_CALL *opt_group)(SF6CoreClient* client, const char* key, const char* title, const char* desc);
    /* New title / description, applied at the next build. */
    SF6CoreStatus (SF6CORE_CALL *opt_group_def)(SF6CoreClient* client, int64_t group, const char* title, const char* desc);
    SF6CoreStatus (SF6CORE_CALL *opt_remove_group)(SF6CoreClient* client, int64_t group);
    /* Returns the entry id, -1 on failure. */
    int64_t       (SF6CORE_CALL *opt_entry)(SF6CoreClient* client, int64_t group, const SF6CoreOptEntryDesc* desc);
    SF6CoreStatus (SF6CORE_CALL *opt_rebuild)(SF6CoreClient* client);
    SF6CoreStatus (SF6CORE_CALL *opt_ensure_built)(SF6CoreClient* client);
    /* Open the window on the page `title` (training menu style); focus =
     * the row to focus, -1 = default. Ignored while the window is open. */
    SF6CoreStatus (SF6CORE_CALL *opt_open_menu)(SF6CoreClient* client, const char* title, int32_t focus);
    /* Change page while the window is open (SF6CORE_OPT_NAV_* flags). */
    SF6CoreStatus (SF6CORE_CALL *opt_navigate)(SF6CoreClient* client, const char* key, int32_t flags, int32_t focus);
    SF6CoreStatus (SF6CORE_CALL *opt_close)(SF6CoreClient* client);
    /* Queries (game thread). */
    int32_t       (SF6CORE_CALL *opt_window_open)(void);
    void*         (SF6CORE_CALL *opt_current_param)(void);
    void*         (SF6CORE_CALL *opt_parent_param)(void);
    int64_t       (SF6CORE_CALL *opt_focused_entry)(void);
    void*         (SF6CORE_CALL *opt_row_control)(int64_t entry, void* param);
    void*         (SF6CORE_CALL *opt_row_part)(int64_t entry);
    SF6CoreStatus (SF6CORE_CALL *opt_set_option_text)(SF6CoreClient* client, int64_t entry, int32_t index, const char* text);
    /* One-line state, for logs. Returns the length written (truncated to cap-1). */
    uint32_t      (SF6CORE_CALL *opt_status)(char* buf, uint32_t cap);

    /* -- Increment 7: effective selection (ADR-0014, append-only) -------- */

    /* A mod that dresses a fighter differently from what the game shows
     * (SlotsLoader's alias: the game and the network see DriveTech, the
     * fighter wears a slot) declares what the fighter really wears while it
     * does: fighter shown in costume `shown_costume` really wears
     * `real_costume`, colour `real_colour` (-1 = the colour the game shows).
     * real_costume < 0 clears the declaration. One declaration per
     * (fighter, shown_costume); the last call wins. */
    SF6CoreStatus (SF6CORE_CALL *sel_set_effective)(SF6CoreClient* client, int32_t fighter, int32_t shown_costume,
                                                    int32_t real_costume, int32_t real_colour);
    /* 1 when the fighter, seen by the caller in `costume` (the shown costume
     * or the real one), is declared to wear something else: *real_costume /
     * *real_colour receive it (*real_colour -1 = the colour the game shows).
     * 0 otherwise (outputs untouched). Any thread. */
    int32_t       (SF6CORE_CALL *sel_get_effective)(int32_t fighter, int32_t costume,
                                                    int32_t* real_costume, int32_t* real_colour);

    /* -- Increment 8: client texts (ADR-0006 amendment, append-only) ---------- */

    /* Message GUIDs that the game resolves to the client's own texts through
     * app.helper.hMsg.GetMessage(System.Guid), at each draw: data-driven UI
     * that carries a message id (training pause-menu rows, input-guide
     * entries...). One table and one GetMessage hook in the process, shared
     * with the option dialogs (increment 6). The hook is installed on the
     * first txt_new of any client: at once when that call runs on the game
     * thread after the core's init (a frame or flow callback), otherwise at
     * the start of the next core frame -- create the texts before putting
     * their GUIDs into the game's data. Any thread may call txt_*.
     *
     * A client's texts are its own: the option engine never purges them;
     * they survive a breaker trip (the game may still show them) and are
     * released by txt_release or when the client unregisters. Releasing a
     * text still shown makes the game show nothing for it: release after
     * removing the data that carries it. */

    /* A fresh message GUID that the core's hMsg.GetMessage hook resolves to
     * utf8 (NULL = ""). One GUID per call, never shared by two texts (R6).
     * ERR_DISABLED: client unregistered, game closing, or the hook could not
     * be installed / was switched off by faults (nothing stored). */
    SF6CoreStatus (SF6CORE_CALL *txt_new)(SF6CoreClient* client, const char* utf8, uint8_t out_guid16[16]);
    /* New text for one of the client's GUIDs, in place (the game resolves it
     * at each draw). ERR_NOT_FOUND: not one of the client's GUIDs. */
    SF6CoreStatus (SF6CORE_CALL *txt_set)(SF6CoreClient* client, const uint8_t guid16[16], const char* utf8);
    /* Forget one of the client's GUIDs; NULL = all of them.
     * ERR_NOT_FOUND: not one of the client's GUIDs. */
    SF6CoreStatus (SF6CORE_CALL *txt_release)(SF6CoreClient* client, const uint8_t guid16[16]);
    /* Write a System.Guid value field of a managed object (guard chain of
     * obj_set_*; ERR_TYPE when the field is not a System.Guid). */
    SF6CoreStatus (SF6CORE_CALL *obj_set_guid)(SF6CoreClient* client, void* obj, const char* field, const uint8_t guid16[16]);
    /* One-line state for logs: hook, texts per client, lookups. Returns the
     * length written (truncated to cap-1). */
    uint32_t      (SF6CORE_CALL *txt_status)(char* buf, uint32_t cap);

} SF6CoreAPI;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SF6CORE_API_H */
