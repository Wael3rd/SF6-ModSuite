// discovery.cpp -- SF6 Native UI Core: Discovery service (increment 4, ADR-0012)
//
// Agent registry (name -> agents, lazy rebuild on version/size change),
// path-based cached lookups (ui_find_cached / ui_find_many), and
// Lua-pattern element hiding for the ZenMode port.
//
// Threading: registry and path cache protected by SRWLOCK (shared for read,
// exclusive for rebuild/insert). A separate rebuild SRWLOCK serialises
// rebuilds: only one thread rebuilds at a time; others serve the current
// map. Engine calls are NEVER made while holding s_reg_lock exclusive:
// the new map is built under s_rebuild_lock (no engine data there),
// then swapped under s_reg_lock exclusive.
//
// Build: compiled into sf6core_native.dll alongside sf6core.cpp/display.cpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "sf6core_internal.h"

#pragma warning(push, 0)
#include <reframework/API.hpp>
#pragma warning(pop)

#include <cctype>
#include <cstdio>
#include <cstring>

// ========================================================================
// Constants
// ========================================================================

static constexpr int REG_MAX_NAMES       = 128;
static constexpr int REG_MAX_PER_NAME    = 16;
static constexpr int PATH_CACHE_MAX      = 1024;
static constexpr int PATH_MOVES_MAX      = 256;
static constexpr int NAME_MAX            = 128;
static constexpr int FIND_NAME_DEPTH_MAX = 8;
static constexpr int ZEN_RULE_MAX        = 512;
static constexpr int ZEN_SEG_MAX         = 16;
static constexpr int ZEN_SEG_LEN         = 256;
static constexpr int ZEN_WALK_MAX        = 4096;

static constexpr uint64_t RESOLVE_RETRY_MS = 500;   // backoff for transient failures

// ========================================================================
// SRWLOCK helpers (no __try in these -- C2712)
// ========================================================================

static SRWLOCK s_reg_lock     = SRWLOCK_INIT;   // protects s_reg and s_path_cache
static SRWLOCK s_rebuild_lock = SRWLOCK_INIT;   // serialises rebuilds

static void reg_shared_lock()    { AcquireSRWLockShared(&s_reg_lock); }
static void reg_shared_unlock()  { ReleaseSRWLockShared(&s_reg_lock); }
static void reg_excl_lock()      { AcquireSRWLockExclusive(&s_reg_lock); }
static void reg_excl_unlock()    { ReleaseSRWLockExclusive(&s_reg_lock); }

// ========================================================================
// Agent registry: field handle cache (retry-with-backoff on transient failures)
//
// Permanent failure = TDB type/field genuinely missing (logged once).
// Transient failure = singleton not created yet, list empty, SDK not
//   ready. Retried every RESOLVE_RETRY_MS.
// ========================================================================

static struct RegFieldCache {
    bool permanent_fail;      // TDB types/fields don't exist -> never retry
    bool fully_resolved;      // all handles resolved -> done
    uint64_t last_attempt;    // GetTickCount64 of last try

    REFrameworkFieldHandle  fh_entries;    // UIAgentManager._Entries
    REFrameworkFieldHandle  fh_version;    // List._version
    REFrameworkFieldHandle  fh_size;       // List._size
    REFrameworkMethodHandle mh_get_count;  // List.get_Count
    REFrameworkMethodHandle mh_get_item;   // List.get_Item
    REFrameworkFieldHandle  fh_agent;      // Entry.Agent
    REFrameworkMethodHandle mh_get_go;     // UIAgent.get_GameObject
    REFrameworkMethodHandle mh_go_name;    // GameObject.get_Name
} s_rfc = {};

static bool resolve_reg_fields() {
    if (s_rfc.permanent_fail) return false;
    if (s_rfc.fully_resolved) return true;

    uint64_t now = GetTickCount64();
    if (now - s_rfc.last_attempt < RESOLVE_RETRY_MS) return false;
    s_rfc.last_attempt = now;

    auto* sdk = s_sdk;
    if (!sdk) return false;  // transient

    DWORD exc = 0;
    __try {
        auto tdb = sdk->functions->get_tdb();
        if (!tdb) return false;  // transient

        // --- TDB phase: types that must exist (permanent failure if missing) ---
        if (!s_rfc.fh_entries) {
            auto t = sdk->tdb->find_type(tdb, "app.UIAgentManager");
            if (!t) { s_rfc.permanent_fail = true;
                      core_log(SF6CORE_LOG_ERROR, nullptr, "registry: app.UIAgentManager type not found"); return false; }
            for (auto tt = t; tt && !s_rfc.fh_entries; tt = sdk->type_definition->get_parent_type(tt))
                s_rfc.fh_entries = sdk->type_definition->find_field(tt, "_Entries");
            if (!s_rfc.fh_entries) { s_rfc.permanent_fail = true;
                                     core_log(SF6CORE_LOG_ERROR, nullptr, "registry: _Entries field not found"); return false; }
        }
        if (!s_rfc.mh_get_go) {
            auto t = sdk->tdb->find_type(tdb, "app.UIAgent");
            if (!t) { s_rfc.permanent_fail = true;
                      core_log(SF6CORE_LOG_ERROR, nullptr, "registry: app.UIAgent type not found"); return false; }
            s_rfc.mh_get_go = sdk->type_definition->find_method(t, "get_GameObject");
            if (!s_rfc.mh_get_go) { s_rfc.permanent_fail = true;
                                     core_log(SF6CORE_LOG_ERROR, nullptr, "registry: get_GameObject not found"); return false; }
        }
        if (!s_rfc.mh_go_name) {
            auto t = sdk->tdb->find_type(tdb, "via.GameObject");
            if (!t) { s_rfc.permanent_fail = true;
                      core_log(SF6CORE_LOG_ERROR, nullptr, "registry: via.GameObject type not found"); return false; }
            s_rfc.mh_go_name = sdk->type_definition->find_method(t, "get_Name");
            if (!s_rfc.mh_go_name) { s_rfc.permanent_fail = true;
                                      core_log(SF6CORE_LOG_ERROR, nullptr, "registry: get_Name not found"); return false; }
        }
        // Entry.Agent: try TDB first, instance fallback later
        if (!s_rfc.fh_agent) {
            auto t = sdk->tdb->find_type(tdb, "app.UIAgentManager.Entry");
            if (t) {
                for (auto tt = t; tt && !s_rfc.fh_agent; tt = sdk->type_definition->get_parent_type(tt))
                    s_rfc.fh_agent = sdk->type_definition->find_field(tt, "Agent");
            }
            // If the inner type isn't found by name, we'll try the instance route below
        }

        // --- Instance phase: needs the singleton and a live list ---
        if (!s_rfc.fh_version || !s_rfc.fh_size || !s_rfc.mh_get_count || !s_rfc.mh_get_item) {
            auto* mgr = sdk->functions->get_managed_singleton("app.UIAgentManager");
            if (!mgr) return false;  // transient: not created yet

            void* raw = sdk->field->get_data_raw(s_rfc.fh_entries, mgr, false);
            if (!raw) return false;
            void* entries = *(void**)raw;
            if (!entries) return false;  // transient: list not allocated

            auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)entries);
            for (auto tt = td; tt; tt = sdk->type_definition->get_parent_type(tt)) {
                if (!s_rfc.fh_version)   s_rfc.fh_version   = sdk->type_definition->find_field(tt, "_version");
                if (!s_rfc.fh_size)      s_rfc.fh_size       = sdk->type_definition->find_field(tt, "_size");
                if (!s_rfc.mh_get_count) s_rfc.mh_get_count = sdk->type_definition->find_method(tt, "get_Count");
                if (!s_rfc.mh_get_item)  s_rfc.mh_get_item  = sdk->type_definition->find_method(tt, "get_Item");
            }

            // Entry.Agent fallback: resolve from first entry's type
            if (!s_rfc.fh_agent && s_rfc.mh_get_item) {
                RawInvokeRet ret;
                void* args0[] = { (void*)(uintptr_t)0 };
                if (invoke_c(sdk, s_rfc.mh_get_item, entries, args0, 1, &ret) && ret.ptr) {
                    td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)ret.ptr);
                    for (auto tt = td; tt && !s_rfc.fh_agent; tt = sdk->type_definition->get_parent_type(tt))
                        s_rfc.fh_agent = sdk->type_definition->find_field(tt, "Agent");
                }
                // Empty list -> no entry to resolve from. Transient; retry later.
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exc = GetExceptionCode();
        core_log(SF6CORE_LOG_WARN, nullptr, "registry field resolution SEH 0x%08X", (unsigned)exc);
        return false;  // transient
    }

    bool all_ok = s_rfc.fh_entries && s_rfc.fh_version && s_rfc.fh_size &&
                  s_rfc.mh_get_count && s_rfc.mh_get_item &&
                  s_rfc.fh_agent && s_rfc.mh_get_go && s_rfc.mh_go_name;
    if (all_ok) {
        s_rfc.fully_resolved = true;
        core_log(SF6CORE_LOG_INFO, nullptr, "registry fields fully resolved");
    }
    return all_ok;
}

// ========================================================================
// Agent registry: data structures
// ========================================================================

struct RegNameEntry {
    char     name[NAME_MAX];
    void*    agents[REG_MAX_PER_NAME];
    int      count;
    uint32_t gen;
    bool     occupied;
};

struct AgentRegMap {
    RegNameEntry entries[REG_MAX_NAMES];
    int          n_entries;
    uint32_t     global_gen;
    uintptr_t    list_addr;
    int32_t      cached_ver;
    int32_t      cached_size;
};

static AgentRegMap s_reg = {};

static int reg_hash(const char* name) {
    uint32_t h = 0x811c9dc5u;
    for (; *name; name++) h = (h ^ (uint8_t)*name) * 0x01000193u;
    return (int)(h & (REG_MAX_NAMES - 1));
}

static RegNameEntry* reg_find(AgentRegMap* map, const char* name) {
    int start = reg_hash(name);
    for (int i = 0; i < REG_MAX_NAMES; i++) {
        int idx = (start + i) & (REG_MAX_NAMES - 1);
        auto* e = &map->entries[idx];
        if (!e->occupied) return nullptr;
        if (strcmp(e->name, name) == 0) return e;
    }
    return nullptr;
}

static RegNameEntry* reg_find_or_insert(AgentRegMap* map, const char* name) {
    int start = reg_hash(name);
    for (int i = 0; i < REG_MAX_NAMES; i++) {
        int idx = (start + i) & (REG_MAX_NAMES - 1);
        auto* e = &map->entries[idx];
        if (!e->occupied) {
            strncpy(e->name, name, NAME_MAX - 1);
            e->name[NAME_MAX - 1] = '\0';
            e->count = 0; e->gen = 0; e->occupied = true;
            map->n_entries++;
            return e;
        }
        if (strcmp(e->name, name) == 0) return e;
    }
    return nullptr;
}

// ========================================================================
// Agent registry: rebuild (engine calls, called under s_rebuild_lock only)
// Returns true on success: new_map is valid and should be swapped in.
// Returns false on any failure: caller must NOT swap.
// ========================================================================

static bool reg_rebuild(AgentRegMap* new_map) {
    auto* sdk = s_sdk;
    if (!sdk || !resolve_reg_fields()) return false;

    memset(new_map->entries, 0, sizeof(new_map->entries));
    new_map->n_entries = 0;

    auto* mgr = sdk->functions->get_managed_singleton("app.UIAgentManager");
    if (!mgr) return false;

    void* raw = sdk->field->get_data_raw(s_rfc.fh_entries, mgr, false);
    if (!raw) return false;
    void* entries = *(void**)raw;
    if (!entries) return false;

    raw = sdk->field->get_data_raw(s_rfc.fh_version, entries, false);
    if (!raw) return false;
    new_map->cached_ver = *(int32_t*)raw;

    raw = sdk->field->get_data_raw(s_rfc.fh_size, entries, false);
    if (!raw) return false;
    new_map->cached_size = *(int32_t*)raw;
    new_map->list_addr = (uintptr_t)entries;

    if (!s_rfc.mh_get_count || !s_rfc.mh_get_item || !s_rfc.fh_agent ||
        !s_rfc.mh_get_go || !s_rfc.mh_go_name)
        return false;

    RawInvokeRet ret;
    if (!invoke_c(sdk, s_rfc.mh_get_count, entries, nullptr, 0, &ret)) return false;
    int n = (int)ret.dword;

    for (int i = 0; i < n; i++) {
        void* args[] = { (void*)(uintptr_t)i };
        if (!invoke_c(sdk, s_rfc.mh_get_item, entries, args, 1, &ret) || !ret.ptr) continue;
        void* entry = ret.ptr;

        raw = sdk->field->get_data_raw(s_rfc.fh_agent, entry, false);
        if (!raw) continue;
        void* agent = *(void**)raw;
        if (!agent) continue;

        if (!invoke_c(sdk, s_rfc.mh_get_go, agent, nullptr, 0, &ret) || !ret.ptr) continue;
        void* go = ret.ptr;
        RawInvokeRet nr;
        if (!invoke_c(sdk, s_rfc.mh_go_name, go, nullptr, 0, &nr) || !nr.ptr) continue;
        char nm[NAME_MAX];
        if (!managed_str_to_utf8(nr.ptr, nm, sizeof(nm))) continue;

        auto* e = reg_find_or_insert(new_map, nm);
        if (e && e->count < REG_MAX_PER_NAME)
            e->agents[e->count++] = agent;
    }
    return true;
}

static void reg_merge_gens(AgentRegMap* old_m, AgentRegMap* new_m) {
    uint32_t base_gen = old_m->global_gen;
    bool any_changed = false;
    for (int i = 0; i < REG_MAX_NAMES; i++) {
        auto* ne = &new_m->entries[i];
        if (!ne->occupied) continue;
        auto* oe = reg_find(old_m, ne->name);
        if (oe) {
            bool same = (oe->count == ne->count);
            if (same) for (int j = 0; j < ne->count; j++)
                if (ne->agents[j] != oe->agents[j]) { same = false; break; }
            ne->gen = same ? oe->gen : oe->gen + 1;
            if (!same) any_changed = true;
        } else { ne->gen = 1; any_changed = true; }
    }
    for (int i = 0; i < REG_MAX_NAMES; i++) {
        auto* oe = &old_m->entries[i];
        if (oe->occupied && !reg_find(new_m, oe->name)) any_changed = true;
    }
    new_m->global_gen = any_changed ? base_gen + 1 : base_gen;
}

// ========================================================================
// Ensure registry is fresh
// ========================================================================

static void reg_ensure_fresh() {
    auto* sdk = s_sdk;
    if (!sdk || !resolve_reg_fields()) return;

    auto* mgr = sdk->functions->get_managed_singleton("app.UIAgentManager");
    if (!mgr) return;

    // Quick check: read cached values under SHARED lock, engine values under SEH
    bool needs_rebuild = false;
    __try {
        void* raw = sdk->field->get_data_raw(s_rfc.fh_entries, mgr, false);
        if (!raw) return;
        void* entries = *(void**)raw;
        if (!entries) return;

        uintptr_t addr = (uintptr_t)entries;
        raw = sdk->field->get_data_raw(s_rfc.fh_version, entries, false);
        if (!raw) return;
        int32_t ver = *(int32_t*)raw;
        raw = sdk->field->get_data_raw(s_rfc.fh_size, entries, false);
        if (!raw) return;
        int32_t size = *(int32_t*)raw;

        // Read cached values under shared lock (no torn reads)
        reg_shared_lock();
        bool same = (addr == s_reg.list_addr && ver == s_reg.cached_ver && size == s_reg.cached_size);
        reg_shared_unlock();
        if (!same) needs_rebuild = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;  // transient, retry next query
    }

    if (!needs_rebuild) return;

    // Only one thread rebuilds at a time; others serve the current map
    if (!TryAcquireSRWLockExclusive(&s_rebuild_lock)) return;

    // Double-check under shared lock: another thread may have rebuilt while we waited
    bool still_stale = true;
    __try {
        void* raw = sdk->field->get_data_raw(s_rfc.fh_entries, mgr, false);
        if (raw) {
            void* entries = *(void**)raw;
            if (entries) {
                uintptr_t addr = (uintptr_t)entries;
                raw = sdk->field->get_data_raw(s_rfc.fh_version, entries, false);
                int32_t ver = raw ? *(int32_t*)raw : -1;
                raw = sdk->field->get_data_raw(s_rfc.fh_size, entries, false);
                int32_t size = raw ? *(int32_t*)raw : -1;
                reg_shared_lock();
                still_stale = (addr != s_reg.list_addr || ver != s_reg.cached_ver || size != s_reg.cached_size);
                reg_shared_unlock();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReleaseSRWLockExclusive(&s_rebuild_lock);
        return;
    }
    if (!still_stale) { ReleaseSRWLockExclusive(&s_rebuild_lock); return; }

    // Build the new map while holding s_rebuild_lock (serialises rebuilds)
    static AgentRegMap s_build_buf;
    bool ok = false;
    __try {
        ok = reg_rebuild(&s_build_buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    if (ok) {
        // Swap under s_reg_lock exclusive (brief: memcpy + gen merge)
        reg_excl_lock();
        reg_merge_gens(&s_reg, &s_build_buf);
        memcpy(&s_reg, &s_build_buf, sizeof(s_reg));
        reg_excl_unlock();
    }
    // If !ok: do NOT swap, keep old map, retry next query

    ReleaseSRWLockExclusive(&s_rebuild_lock);
}

// ========================================================================
// Proof-of-life: cached UIAgent type handle
// ========================================================================

static struct AgentTypeCache {
    REFrameworkTypeDefinitionHandle t_ui_agent;
    bool resolved;
    bool failed;
    uint64_t last_attempt;
} s_atc = {};

static bool resolve_agent_type() {
    if (s_atc.failed) return false;
    if (s_atc.resolved) return true;
    uint64_t now = GetTickCount64();
    if (now - s_atc.last_attempt < RESOLVE_RETRY_MS) return false;
    s_atc.last_attempt = now;
    auto* sdk = s_sdk;
    if (!sdk) return false;
    auto tdb = sdk->functions->get_tdb();
    if (!tdb) return false;
    s_atc.t_ui_agent = sdk->tdb->find_type(tdb, "app.UIAgent");
    if (!s_atc.t_ui_agent) { s_atc.failed = true;
        core_log(SF6CORE_LOG_ERROR, nullptr, "agent_alive: app.UIAgent type not found"); return false; }
    s_atc.resolved = true;
    return true;
}

static bool agent_alive(void* agent) {
    if (!agent || !s_sdk) return false;
    if ((uintptr_t)agent < 0x10000) return false;
    if (!resolve_agent_type()) return false;
    auto* sdk = s_sdk;
    __try {
        auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)agent);
        while (td) {
            if (td == s_atc.t_ui_agent) return true;
            td = sdk->type_definition->get_parent_type(td);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// ========================================================================
// API: ui_agents, ui_agents_gen, ui_registry_gen
// ========================================================================

// Registry lookup WITHOUT the freshness check: for callers that already ran
// reg_ensure_fresh() once in the same pass (the zen commit, once per frame).
static uint32_t reg_lookup_agents(const char* name, void** out, uint32_t max) {
    if (!name || !out || max == 0) return 0;
    uint32_t count = 0;
    void* local[REG_MAX_PER_NAME];
    int local_n = 0;
    reg_shared_lock();
    auto* e = reg_find(&s_reg, name);
    if (e) {
        local_n = e->count;
        if (local_n > REG_MAX_PER_NAME) local_n = REG_MAX_PER_NAME;
        memcpy(local, e->agents, local_n * sizeof(void*));
    }
    reg_shared_unlock();
    for (int i = 0; i < local_n && count < max; i++)
        if (agent_alive(local[i])) out[count++] = local[i];
    return count;
}

uint32_t SF6CORE_CALL api_ui_agents(const char* name, void** out, uint32_t max) {
    if (!name || !out || max == 0 || core_closing()) return 0;
    reg_ensure_fresh();
    return reg_lookup_agents(name, out, max);
}

uint32_t SF6CORE_CALL api_ui_agents_gen(const char* name) {
    if (!name || core_closing()) return 0;
    reg_ensure_fresh();
    reg_shared_lock();
    auto* e = reg_find(&s_reg, name);
    uint32_t g = e ? e->gen : 0;
    reg_shared_unlock();
    return g;
}

uint32_t SF6CORE_CALL api_ui_registry_gen(void) {
    if (core_closing()) return 0;
    reg_ensure_fresh();
    reg_shared_lock();
    uint32_t g = s_reg.global_gen;
    reg_shared_unlock();
    return g;
}

void* discovery_find_first_agent(const char* name) {
    if (!name) return nullptr;
    reg_ensure_fresh();
    void* local[REG_MAX_PER_NAME];
    int local_n = 0;
    reg_shared_lock();
    auto* e = reg_find(&s_reg, name);
    if (e) {
        local_n = e->count;
        if (local_n > REG_MAX_PER_NAME) local_n = REG_MAX_PER_NAME;
        memcpy(local, e->agents, local_n * sizeof(void*));
    }
    reg_shared_unlock();
    for (int i = 0; i < local_n; i++)
        if (agent_alive(local[i])) return local[i];
    return nullptr;
}

// ========================================================================
// Path cache for ui_find_cached
// ========================================================================

struct PathEntry {
    uintptr_t root_addr;
    char      name[NAME_MAX];
    char      moves[PATH_MOVES_MAX];
    int       n_moves;
    bool      valid;
};

static PathEntry s_path_cache[PATH_CACHE_MAX] = {};
static int       s_path_cache_n = 0;

static PathEntry* path_find_locked(uintptr_t root_addr, const char* name) {
    for (int i = 0; i < s_path_cache_n; i++) {
        auto* p = &s_path_cache[i];
        if (p->valid && p->root_addr == root_addr && strcmp(p->name, name) == 0)
            return p;
    }
    return nullptr;
}

static void path_insert_locked(uintptr_t root_addr, const char* name,
                                const char* moves, int n_moves) {
    if (s_path_cache_n >= PATH_CACHE_MAX) {
        memset(s_path_cache, 0, sizeof(s_path_cache));
        s_path_cache_n = 0;
    }
    for (int i = 0; i < s_path_cache_n; i++) {
        auto* p = &s_path_cache[i];
        if (p->valid && p->root_addr == root_addr && strcmp(p->name, name) == 0) {
            memcpy(p->moves, moves, n_moves);
            p->n_moves = n_moves;
            return;
        }
    }
    auto* p = &s_path_cache[s_path_cache_n++];
    p->root_addr = root_addr;
    strncpy(p->name, name, NAME_MAX - 1);
    p->name[NAME_MAX - 1] = '\0';
    memcpy(p->moves, moves, n_moves);
    p->n_moves = n_moves;
    p->valid = true;
}

// ========================================================================
// find_name order walk (matches UIFind.lua exactly)
//
// Beyond PATH_MOVES_MAX: the walk continues (so controls further along a
// long sibling chain are found), but the path is marked uncacheable.
// ========================================================================

// get_Child is declared on via.gui.TransformObject, not on PlayObject (checked in game 02/10):
// invoke it only on nodes of that type (defined after the method caches below).
static bool invoke_child(const REFrameworkSDKData* sdk, void* node, RawInvokeRet* ret);

struct WalkState {
    const char* target;
    char moves[PATH_MOVES_MAX];
    int n_moves;
    void* result;
    bool uncacheable;   // set when the match is deeper than PATH_MOVES_MAX
};

static void walk_find_name(const REFrameworkSDKData* sdk, void* ctrl,
                           const char* target, int depth,
                           WalkState* ws, int move_idx,
                           REFrameworkMethodHandle mh_child,
                           REFrameworkMethodHandle mh_next,
                           REFrameworkMethodHandle mh_name) {
    if (!ctrl || depth > FIND_NAME_DEPTH_MAX || ws->result) return;

    RawInvokeRet nr;
    if (invoke_c(sdk, mh_name, ctrl, nullptr, 0, &nr) && nr.ptr) {
        char nm[NAME_MAX];
        if (managed_str_to_utf8(nr.ptr, nm, sizeof(nm)) && strcmp(nm, target) == 0) {
            ws->result = ctrl;
            ws->n_moves = move_idx;
            if (move_idx > PATH_MOVES_MAX) ws->uncacheable = true;
            return;
        }
    }

    // Child (depth + 1)
    RawInvokeRet ret;
    if (invoke_child(sdk, ctrl, &ret) && ret.ptr) {
        if (move_idx < PATH_MOVES_MAX)
            ws->moves[move_idx] = 'C';
        else
            ws->uncacheable = true;
        walk_find_name(sdk, ret.ptr, target, depth + 1, ws, move_idx + 1,
                      mh_child, mh_next, mh_name);
        if (ws->result) return;
    }

    // Next sibling (same depth)
    if (invoke_c(sdk, mh_next, ctrl, nullptr, 0, &ret) && ret.ptr) {
        if (move_idx < PATH_MOVES_MAX)
            ws->moves[move_idx] = 'N';
        else
            ws->uncacheable = true;
        walk_find_name(sdk, ret.ptr, target, depth, ws, move_idx + 1,
                      mh_child, mh_next, mh_name);
    }
}

static void* follow_path(const REFrameworkSDKData* sdk, void* root,
                          const char* moves, int n_moves,
                          REFrameworkMethodHandle mh_child,
                          REFrameworkMethodHandle mh_next) {
    (void)mh_child;                                         // child moves go through invoke_child (type-checked)
    void* node = root;
    for (int i = 0; i < n_moves && node; i++) {
        RawInvokeRet ret;
        if (moves[i] == 'C') {
            if (!invoke_child(sdk, node, &ret)) return nullptr;
        } else {
            if (!invoke_c(sdk, mh_next, node, nullptr, 0, &ret)) return nullptr;
        }
        node = ret.ptr;
    }
    return node;
}

// ========================================================================
// PlayObject / ForceInvisible method caches (retry-with-backoff)
// ========================================================================

static struct PlayObjMethods {
    bool resolved; bool failed; uint64_t last_attempt;
    REFrameworkMethodHandle mh_child, mh_next, mh_name;
    REFrameworkTypeDefinitionHandle td_transform;   // declaring type of get_Child
} s_po = {};

static bool resolve_po_methods() {
    if (s_po.failed) return false;
    if (s_po.resolved) return true;
    uint64_t now = GetTickCount64();
    if (now - s_po.last_attempt < RESOLVE_RETRY_MS) return false;
    s_po.last_attempt = now;
    auto* sdk = s_sdk;
    if (!sdk) return false;
    auto tdb = sdk->functions->get_tdb();
    if (!tdb) return false;
    auto t = sdk->tdb->find_type(tdb, "via.gui.PlayObject");
    auto t_tr = sdk->tdb->find_type(tdb, "via.gui.TransformObject");
    if (!t || !t_tr) { s_po.failed = true;
              core_log(SF6CORE_LOG_ERROR, nullptr, "gui type not found: PlayObject=%s TransformObject=%s",
                       t ? "ok" : "NO", t_tr ? "ok" : "NO"); return false; }
    // Declaring types (game 02/10): get_Child on via.gui.TransformObject; get_Next / get_Name on PlayObject.
    s_po.mh_child = sdk->type_definition->find_method(t_tr, "get_Child");
    s_po.mh_next  = sdk->type_definition->find_method(t, "get_Next");
    s_po.mh_name  = sdk->type_definition->find_method(t, "get_Name");
    s_po.td_transform = t_tr;
    if (!s_po.mh_child || !s_po.mh_next || !s_po.mh_name) {
        s_po.failed = true;
        core_log(SF6CORE_LOG_ERROR, nullptr, "gui methods missing: TransformObject.get_Child=%s PlayObject.get_Next=%s PlayObject.get_Name=%s",
                 s_po.mh_child ? "ok" : "NO", s_po.mh_next ? "ok" : "NO", s_po.mh_name ? "ok" : "NO");
        return false;
    }
    s_po.resolved = true;
    return true;
}

static bool invoke_child(const REFrameworkSDKData* sdk, void* node, RawInvokeRet* ret) {
    if (!node || !s_po.mh_child || !s_po.td_transform) return false;
    auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)node);
    for (int i = 0; td && i < 16; i++) {                 // leaf elements (Text, Rect...) are not TransformObjects
        if (td == s_po.td_transform) return invoke_c(sdk, s_po.mh_child, node, nullptr, 0, ret);
        td = sdk->type_definition->get_parent_type(td);
    }
    return false;
}

static struct ForceInvMethods {
    bool resolved; bool failed; uint64_t last_attempt;
    REFrameworkMethodHandle mh_get, mh_set;
} s_fi = {};

static bool resolve_fi_methods() {
    if (s_fi.failed) return false;
    if (s_fi.resolved) return true;
    uint64_t now = GetTickCount64();
    if (now - s_fi.last_attempt < RESOLVE_RETRY_MS) return false;
    s_fi.last_attempt = now;
    auto* sdk = s_sdk;
    if (!sdk) return false;
    auto tdb = sdk->functions->get_tdb();
    if (!tdb) return false;
    auto t = sdk->tdb->find_type(tdb, "via.gui.PlayObject");
    if (!t) { s_fi.failed = true; return false; }
    s_fi.mh_get = sdk->type_definition->find_method(t, "get_ForceInvisible");
    s_fi.mh_set = sdk->type_definition->find_method(t, "set_ForceInvisible");
    if (!s_fi.mh_get || !s_fi.mh_set) { s_fi.failed = true; return false; }
    s_fi.resolved = true;
    return true;
}

// ========================================================================
// API: ui_find_cached
// ========================================================================

void* SF6CORE_CALL api_ui_find_cached(void* root, const char* name) {
    if (!root || !name || core_closing()) return nullptr;
    auto* sdk = s_sdk;
    if (!sdk || !resolve_po_methods()) return nullptr;
    uintptr_t root_addr = (uintptr_t)root;

    // Try cached path
    char local_moves[PATH_MOVES_MAX];
    int local_n = 0;
    bool have_cached = false;
    reg_shared_lock();
    auto* pe = path_find_locked(root_addr, name);
    if (pe) { local_n = pe->n_moves; memcpy(local_moves, pe->moves, local_n); have_cached = true; }
    reg_shared_unlock();

    if (have_cached) {
        void* result = nullptr;
        __try {
            result = follow_path(sdk, root, local_moves, local_n, s_po.mh_child, s_po.mh_next);
            if (result) {
                RawInvokeRet nr;
                if (invoke_c(sdk, s_po.mh_name, result, nullptr, 0, &nr) && nr.ptr) {
                    char nm[NAME_MAX];
                    if (managed_str_to_utf8(nr.ptr, nm, sizeof(nm)) && strcmp(nm, name) == 0)
                        return result;
                }
                result = nullptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (result) return result;
    }

    // Full walk
    WalkState ws = {};
    ws.target = name;
    __try {
        walk_find_name(sdk, root, name, 0, &ws, 0, s_po.mh_child, s_po.mh_next, s_po.mh_name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (ws.result && !ws.uncacheable) {
        reg_excl_lock();
        path_insert_locked(root_addr, name, ws.moves, ws.n_moves);
        reg_excl_unlock();
    } else if (!ws.result && have_cached) {
        reg_excl_lock();
        auto* pe2 = path_find_locked(root_addr, name);
        if (pe2) pe2->valid = false;
        reg_excl_unlock();
    }
    return ws.result;
}

// ========================================================================
// API: ui_find_many -- one walk, first match per name (capped at 32)
// ========================================================================

struct FindManyState {
    const char* const* names;
    uint32_t n_names;
    void** out;
    bool* found;
    uint32_t remaining;
    char paths[32][PATH_MOVES_MAX];
    int  path_lens[32];
    bool uncacheable[32];
};

static void walk_find_many(const REFrameworkSDKData* sdk, void* ctrl,
                           int depth, FindManyState* ms, int move_idx,
                           char* cur_moves,
                           REFrameworkMethodHandle mh_child,
                           REFrameworkMethodHandle mh_next,
                           REFrameworkMethodHandle mh_name) {
    if (!ctrl || depth > FIND_NAME_DEPTH_MAX || ms->remaining == 0) return;

    RawInvokeRet nr;
    if (invoke_c(sdk, mh_name, ctrl, nullptr, 0, &nr) && nr.ptr) {
        char nm[NAME_MAX];
        if (managed_str_to_utf8(nr.ptr, nm, sizeof(nm))) {
            for (uint32_t i = 0; i < ms->n_names; i++) {
                if (!ms->found[i] && strcmp(nm, ms->names[i]) == 0) {
                    ms->out[i] = ctrl;
                    ms->found[i] = true;
                    ms->remaining--;
                    if (i < 32) {
                        if (move_idx <= PATH_MOVES_MAX) {
                            memcpy(ms->paths[i], cur_moves, move_idx < PATH_MOVES_MAX ? move_idx : PATH_MOVES_MAX);
                            ms->path_lens[i] = move_idx;
                        }
                        if (move_idx > PATH_MOVES_MAX)
                            ms->uncacheable[i] = true;
                    }
                    break;
                }
            }
        }
    }
    if (ms->remaining == 0) return;

    RawInvokeRet ret;
    if (invoke_child(sdk, ctrl, &ret) && ret.ptr) {
        if (move_idx < PATH_MOVES_MAX)
            cur_moves[move_idx] = 'C';
        walk_find_many(sdk, ret.ptr, depth + 1, ms, move_idx + 1,
                      cur_moves, mh_child, mh_next, mh_name);
        if (ms->remaining == 0) return;
    }

    if (invoke_c(sdk, mh_next, ctrl, nullptr, 0, &ret) && ret.ptr) {
        if (move_idx < PATH_MOVES_MAX)
            cur_moves[move_idx] = 'N';
        walk_find_many(sdk, ret.ptr, depth, ms, move_idx + 1,
                      cur_moves, mh_child, mh_next, mh_name);
    }
}

uint32_t SF6CORE_CALL api_ui_find_many(void* root, const char* const* names,
                                        uint32_t n, void** out) {
    if (!root || !names || !out || n == 0 || core_closing()) return 0;
    // Zero ALL of out[] (including beyond the 32-name cap) so callers never read garbage
    memset(out, 0, n * sizeof(void*));
    auto* sdk = s_sdk;
    if (!sdk || !resolve_po_methods()) return 0;

    uint32_t effective_n = n > 32 ? 32 : n;

    bool found_flags[32] = {};
    FindManyState ms = {};
    ms.names = names;
    ms.n_names = effective_n;
    ms.out = out;
    ms.found = found_flags;
    ms.remaining = effective_n;
    memset(ms.path_lens, 0, sizeof(ms.path_lens));
    memset(ms.uncacheable, 0, sizeof(ms.uncacheable));

    char cur_moves[PATH_MOVES_MAX];
    __try {
        walk_find_many(sdk, root, 0, &ms, 0, cur_moves,
                      s_po.mh_child, s_po.mh_next, s_po.mh_name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    uint32_t count = 0;
    uintptr_t root_addr = (uintptr_t)root;
    reg_excl_lock();
    for (uint32_t i = 0; i < effective_n; i++) {
        if (found_flags[i]) {
            count++;
            if (i < 32 && !ms.uncacheable[i] && ms.path_lens[i] <= PATH_MOVES_MAX)
                path_insert_locked(root_addr, names[i], ms.paths[i], ms.path_lens[i]);
        }
    }
    reg_excl_unlock();
    return count;
}

// ========================================================================
// Lua pattern matcher (subset: %d, %d+, literals, UNANCHORED)
// ========================================================================

static bool match_at(const char* s, const char* p) {
    while (*p) {
        if (p[0] == '%' && p[1] == 'd') {
            if (p[2] == '+') {
                if (!*s || !isdigit((unsigned char)*s)) return false;
                s++; while (*s && isdigit((unsigned char)*s)) s++;
                p += 3;
            } else {
                if (!*s || !isdigit((unsigned char)*s)) return false;
                s++; p += 2;
            }
        } else {
            if (*s != *p) return false;
            s++; p++;
        }
    }
    return true;
}

bool lua_pat_match(const char* subject, const char* pattern) {
    if (!pattern || !*pattern) return true;
    for (const char* s = subject; *s; s++)
        if (match_at(s, pattern)) return true;
    return false;
}

// ========================================================================
// ZenMode element hiding rules
// ========================================================================

struct ZenHideRule {
    SF6CoreClient* owner;           // set to nullptr on unregister
    char           client_id[64];   // by-value copy for safe lookups
    char           gui_owner[128];
    char           segments[ZEN_SEG_MAX][ZEN_SEG_LEN];
    int            n_segments;
    bool           hidden;
    bool           active;
    bool           restore_pending; // unregister: restore once, then deactivate
};

static ZenHideRule s_zen_rules[ZEN_RULE_MAX];
static int         s_zen_rule_n = 0;
static SRWLOCK     s_zen_lock = SRWLOCK_INIT;

static void zen_shared_lock()   { AcquireSRWLockShared(&s_zen_lock); }
static void zen_shared_unlock() { ReleaseSRWLockShared(&s_zen_lock); }
static void zen_excl_lock()     { AcquireSRWLockExclusive(&s_zen_lock); }
static void zen_excl_unlock()   { ReleaseSRWLockExclusive(&s_zen_lock); }

std::atomic<int> s_zen_rule_count_pub{0};

// ========================================================================
// Zen registry resolution: get views from the agent registry to avoid
// first-draw flash. Cached method handles with retry-with-backoff.
// ========================================================================

static struct ZenAgentMethods {
    bool resolved; bool failed; uint64_t last_attempt;
    REFrameworkMethodHandle mh_get_control_main;  // UIAgent.get_ControlMain
    REFrameworkMethodHandle mh_get_parent;         // PlayObject.get_Parent
    REFrameworkTypeDefinitionHandle t_view;         // via.gui.View (for type check)
} s_zam = {};

static bool resolve_zen_agent_methods() {
    if (s_zam.failed) return false;
    if (s_zam.resolved) return true;
    uint64_t now = GetTickCount64();
    if (now - s_zam.last_attempt < RESOLVE_RETRY_MS) return false;
    s_zam.last_attempt = now;
    auto* sdk = s_sdk;
    if (!sdk) return false;
    auto tdb = sdk->functions->get_tdb();
    if (!tdb) return false;
    auto t_agent = sdk->tdb->find_type(tdb, "app.UIAgent");
    if (t_agent && !s_zam.mh_get_control_main)
        s_zam.mh_get_control_main = sdk->type_definition->find_method(t_agent, "get_ControlMain");
    auto t_po = sdk->tdb->find_type(tdb, "via.gui.PlayObject");
    if (t_po && !s_zam.mh_get_parent)
        s_zam.mh_get_parent = sdk->type_definition->find_method(t_po, "get_Parent");
    if (!s_zam.t_view)
        s_zam.t_view = sdk->tdb->find_type(tdb, "via.gui.View");
    if (!s_zam.mh_get_control_main || !s_zam.mh_get_parent || !s_zam.t_view) {
        if (t_agent && t_po) { s_zam.failed = true;
            core_log(SF6CORE_LOG_ERROR, nullptr, "zen agent methods not found"); }
        return false;
    }
    s_zam.resolved = true;
    return true;
}

// Check if obj is a via.gui.View by type definition under SEH
static bool is_view_type(void* obj) {
    if (!obj || !s_sdk || !s_zam.t_view) return false;
    if ((uintptr_t)obj < 0x10000) return false;
    auto* sdk = s_sdk;
    __try {
        auto td = sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj);
        while (td) {
            if (td == s_zam.t_view) return true;
            td = sdk->type_definition->get_parent_type(td);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// Per-owner diagnostic: frames since the owner was first seen on screen (a sighting) without an agent.
// Only owners on screen count: an owner whose screen was never opened has no agent, and that is normal.
static constexpr int ZEN_DIAG_MAX = 64;
static constexpr uint64_t ZEN_DIAG_FRAMES = 600;
struct ZenDiagEntry { char name[128]; uint64_t first_frame; bool logged; };
static ZenDiagEntry s_zen_diag[ZEN_DIAG_MAX];
static int s_zen_diag_n = 0;

SF6CoreStatus SF6CORE_CALL api_set_element_hidden_zen(
        SF6CoreClient* client, const char* gui_owner,
        const char* const* pattern_segments, uint32_t n_segments,
        int32_t hidden) {
    if (!client || !gui_owner || !pattern_segments || n_segments == 0)
        return SF6CORE_ERR_INVALID_ARG;
    if (core_closing()) return SF6CORE_ERR_DISABLED;
    if (!is_game_thread()) return SF6CORE_ERR_WRONG_THREAD;
    if (gui_owner[0] == '\0') return SF6CORE_ERR_INVALID_ARG;
    if (n_segments > ZEN_SEG_MAX) return SF6CORE_ERR_INVALID_ARG;

    zen_excl_lock();
    for (int i = 0; i < s_zen_rule_n; i++) {
        auto& r = s_zen_rules[i];
        if (!r.active || r.owner != client) continue;
        if (strcmp(r.gui_owner, gui_owner) != 0) continue;
        if ((int)n_segments != r.n_segments) continue;
        bool same = true;
        for (int j = 0; j < r.n_segments; j++)
            if (strcmp(r.segments[j], pattern_segments[j]) != 0) { same = false; break; }
        if (same) { r.hidden = (hidden != 0); zen_excl_unlock(); return SF6CORE_OK; }
    }
    if (s_zen_rule_n >= ZEN_RULE_MAX) { zen_excl_unlock(); return SF6CORE_ERR_INVALID_ARG; }
    auto& nr = s_zen_rules[s_zen_rule_n++];
    nr.owner = client;
    strncpy(nr.client_id, client->client_id, sizeof(nr.client_id) - 1);
    nr.client_id[sizeof(nr.client_id) - 1] = '\0';
    nr.restore_pending = false;
    strncpy(nr.gui_owner, gui_owner, sizeof(nr.gui_owner) - 1);
    nr.gui_owner[sizeof(nr.gui_owner) - 1] = '\0';
    nr.n_segments = (int)n_segments;
    for (int i = 0; i < (int)n_segments; i++) {
        strncpy(nr.segments[i], pattern_segments[i], ZEN_SEG_LEN - 1);
        nr.segments[i][ZEN_SEG_LEN - 1] = '\0';
    }
    nr.hidden = (hidden != 0);
    nr.active = true;
    s_zen_rule_count_pub.store(s_zen_rule_n, std::memory_order_release);
    zen_excl_unlock();
    core_log(SF6CORE_LOG_INFO, client->client_id,
             "zen rule %s: %s (segs=%u)", hidden ? "hide" : "unhide", gui_owner, n_segments);
    return SF6CORE_OK;
}

// ========================================================================
// ZenMode commit phase
// ========================================================================

static void zen_apply_fi(const REFrameworkSDKData* sdk, void* ctrl, bool hidden) {
    if (!s_fi.mh_get || !s_fi.mh_set) return;
    RawInvokeRet ret;
    if (invoke_c(sdk, s_fi.mh_get, ctrl, nullptr, 0, &ret)) {
        bool cur = (ret.dword != 0);
        if (cur != hidden) {
            void* args[] = { (void*)(uintptr_t)(hidden ? 1 : 0) };
            invoke_c(sdk, s_fi.mh_set, ctrl, args, 1, &ret);
        }
    }
}

static void zen_walk_apply(const REFrameworkSDKData* sdk, void* parent,
                            const char segments[][ZEN_SEG_LEN], int seg_idx,
                            int n_segments, bool hidden, int* visited) {
    if (seg_idx >= n_segments) { zen_apply_fi(sdk, parent, hidden); return; }
    if (*visited > ZEN_WALK_MAX) return;

    RawInvokeRet ret;
    if (!invoke_child(sdk, parent, &ret) || !ret.ptr) return;
    void* child = ret.ptr;
    while (child && *visited <= ZEN_WALK_MAX) {
        (*visited)++;
        RawInvokeRet nr;
        if (invoke_c(sdk, s_po.mh_name, child, nullptr, 0, &nr) && nr.ptr) {
            char nm[ZEN_SEG_LEN];
            if (managed_str_to_utf8(nr.ptr, nm, sizeof(nm)) && lua_pat_match(nm, segments[seg_idx]))
                zen_walk_apply(sdk, child, segments, seg_idx + 1, n_segments, hidden, visited);
        }
        if (!invoke_c(sdk, s_po.mh_next, child, nullptr, 0, &ret)) break;
        child = ret.ptr;
    }
}

// Collect views for an owner from sightings + agent registry (no reg_ensure_fresh
// here -- the caller does it once for the whole pass). Reads the registry map
// under shared lock. Validates agents and resolves views outside the lock.
static int collect_views_for_owner(const REFrameworkSDKData* sdk,
                                    const char* owner_name, uint64_t current_frame,
                                    const char (*sighted_owners)[128],
                                    const void** sighted_views,
                                    const uint64_t* sighted_frames,
                                    int n_sighted,
                                    void** views, int max_views) {
    int n = 0;
    // From sightings (all views for this owner seen in the last 2 frames)
    for (int i = 0; i < n_sighted && n < max_views; i++) {
        if (current_frame - sighted_frames[i] > 2) continue;
        if (strcmp(sighted_owners[i], owner_name) != 0) continue;
        views[n++] = (void*)sighted_views[i];
    }
    const int n_seen = n;
    // From agent registry: read the map under shared lock, copy agent pointers,
    // then validate + resolve views outside the lock.
    if (resolve_zen_agent_methods()) {
        void* agents[16];
        uint32_t na = reg_lookup_agents(owner_name, agents, 16);   // reg_ensure_fresh() ran once for this pass
        for (uint32_t a = 0; a < na && n < max_views; a++) {
            void* view = nullptr;
            __try {
                RawInvokeRet ret;
                if (invoke_c(sdk, s_zam.mh_get_control_main, agents[a], nullptr, 0, &ret) && ret.ptr) {
                    RawInvokeRet pr;
                    if (invoke_c(sdk, s_zam.mh_get_parent, ret.ptr, nullptr, 0, &pr) && pr.ptr) {
                        if (is_view_type(pr.ptr))
                            view = pr.ptr;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (!view) continue;
            bool dup = false;
            for (int j = 0; j < n; j++)
                if (views[j] == view) { dup = true; break; }
            if (!dup) views[n++] = view;
        }
        // Diagnostic: log once if the owner's screen is shown but no agent matched it ~600 frames later
        if (na == 0 && n_seen > 0) {
            bool found_diag = false;
            for (int d = 0; d < s_zen_diag_n; d++) {
                if (strcmp(s_zen_diag[d].name, owner_name) == 0) {
                    found_diag = true;
                    if (!s_zen_diag[d].logged &&
                        current_frame - s_zen_diag[d].first_frame > ZEN_DIAG_FRAMES) {
                        s_zen_diag[d].logged = true;
                        core_log(SF6CORE_LOG_WARN, nullptr,
                                 "zen owner '%s' is on screen but has no agent after %llu frames (rules applied from sightings only)",
                                 owner_name, (unsigned long long)ZEN_DIAG_FRAMES);
                    }
                    break;
                }
            }
            if (!found_diag && s_zen_diag_n < ZEN_DIAG_MAX) {
                auto& de = s_zen_diag[s_zen_diag_n++];
                strncpy(de.name, owner_name, sizeof(de.name) - 1);
                de.name[sizeof(de.name) - 1] = '\0';
                de.first_frame = current_frame;
                de.logged = false;
            }
        }
    }
    return n;
}

void discovery_commit_zen_hides(uint64_t current_frame,
                                 const char (*sighted_owners)[128],
                                 const void** sighted_views,
                                 const uint64_t* sighted_frames,
                                 int n_sighted) {
    if (s_zen_rule_count_pub.load(std::memory_order_acquire) == 0) return;
    auto* sdk = s_sdk;
    if (!sdk || !resolve_po_methods() || !resolve_fi_methods()) return;

    // One reg_ensure_fresh for the whole pass
    reg_ensure_fresh();

    // Snapshot zen rules under lock
    static ZenHideRule snap[ZEN_RULE_MAX];
    int snap_n = 0;
    zen_shared_lock();
    for (int i = 0; i < s_zen_rule_n && snap_n < ZEN_RULE_MAX; i++)
        if (s_zen_rules[i].active) snap[snap_n++] = s_zen_rules[i];
    zen_shared_unlock();

    // Build distinct owner list from snapshot, check client state once per owner
    static constexpr int MAX_OWNERS = 64;
    struct OwnerEntry {
        char name[128];
        void* views[32];
        int n_views;
        bool skip;     // client disabled/suspended/gone
    };
    static OwnerEntry owners[MAX_OWNERS];
    int n_owners = 0;

    for (int r = 0; r < snap_n; r++) {
        auto& rule = snap[r];
        if (rule.n_segments == 0) continue;
        // Check if this owner is already in the list
        bool found = false;
        for (int o = 0; o < n_owners; o++) {
            if (strcmp(owners[o].name, rule.gui_owner) == 0) { found = true; break; }
        }
        if (found) continue;
        if (n_owners >= MAX_OWNERS) break;

        auto& oe = owners[n_owners];
        strncpy(oe.name, rule.gui_owner, sizeof(oe.name) - 1);
        oe.name[sizeof(oe.name) - 1] = '\0';
        oe.n_views = 0;
        oe.skip = false;

        // Check client state for this owner's rules (use first rule's client_id)
        if (!rule.restore_pending) {
            int32_t cst;
            bool suspended;
            query_client_state_and_suspended(rule.client_id, &cst, &suspended);
            if (cst == SF6CORE_CLIENT_DISABLED_BY_BREAKER || cst == -1) oe.skip = true;
            if (suspended) oe.skip = true;
        }

        if (!oe.skip) {
            oe.n_views = collect_views_for_owner(sdk, rule.gui_owner, current_frame,
                                                  sighted_owners, sighted_views, sighted_frames,
                                                  n_sighted, oe.views, 32);
        }
        n_owners++;
    }

    // Apply rules: for each rule, find its owner entry and apply to those views
    for (int r = 0; r < snap_n; r++) {
        auto& rule = snap[r];
        if (rule.n_segments == 0) continue;

        // Find owner entry
        OwnerEntry* oe = nullptr;
        for (int o = 0; o < n_owners; o++) {
            if (strcmp(owners[o].name, rule.gui_owner) == 0) { oe = &owners[o]; break; }
        }
        if (!oe || oe->skip) continue;

        // For restore_pending: override hidden to false
        bool effective_hidden = rule.restore_pending ? false : rule.hidden;

        for (int vi = 0; vi < oe->n_views; vi++) {
            __try {
                int visited = 0;
                zen_walk_apply(sdk, oe->views[vi], rule.segments, 0, rule.n_segments,
                              effective_hidden, &visited);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                core_log(SF6CORE_LOG_ERROR, nullptr,
                         "zen_walk_apply SEH owner=%s view=0x%p", rule.gui_owner, oe->views[vi]);
            }
        }
    }

    // Compact: deactivate restore_pending rules
    bool any_removed = false;
    zen_excl_lock();
    int dst = 0;
    for (int i = 0; i < s_zen_rule_n; i++) {
        if (s_zen_rules[i].active && s_zen_rules[i].restore_pending) {
            s_zen_rules[i].active = false;
            any_removed = true;
            continue;
        }
        if (dst != i) s_zen_rules[dst] = s_zen_rules[i];
        dst++;
    }
    if (any_removed) {
        s_zen_rule_n = dst;
        s_zen_rule_count_pub.store(dst, std::memory_order_release);
    }
    zen_excl_unlock();
}

bool discovery_zen_owner_declared(const char* owner_name) {
    if (s_zen_rule_count_pub.load(std::memory_order_acquire) == 0) return false;
    if (!TryAcquireSRWLockShared(&s_zen_lock)) return false;
    bool found = false;
    for (int i = 0; i < s_zen_rule_n && !found; i++)
        if (s_zen_rules[i].active && strcmp(s_zen_rules[i].gui_owner, owner_name) == 0) found = true;
    ReleaseSRWLockShared(&s_zen_lock);
    return found;
}

void discovery_restore_client_zen(SF6CoreClient* client) {
    int n = 0;
    zen_excl_lock();
    for (int i = 0; i < s_zen_rule_n; i++) {
        if (s_zen_rules[i].active && s_zen_rules[i].owner == client) {
            s_zen_rules[i].hidden = false;
            s_zen_rules[i].restore_pending = true;
            s_zen_rules[i].owner = nullptr;  // null out before client is freed
            n++;
        }
    }
    zen_excl_unlock();
    if (n > 0) core_log(SF6CORE_LOG_INFO, client->client_id,
                         "marking %d zen rule(s) for restore+deactivate", n);
}

void discovery_log_init() {
    core_log(SF6CORE_LOG_INFO, nullptr, "discovery service initialized (ADR-0012)");
}
