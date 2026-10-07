// lua_bridge.cpp -- SF6 Native UI Core: Lua bridge for REFramework scripts
//
// Registers _G.SF6Core with functions that mirror the C API.
// Uses Lua 5.4.3 statically linked (same ABI as REFramework's embedded Lua).
// Raw Lua C API only (no sol2).
//
// Lifecycle: on_lua_state_created -> register global table.
//            on_lua_state_destroyed -> forget the Lua state pointer.
//
// Bridge functions return integer status codes for engine problems;
// argument type errors use luaL_argerror (Lua error).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "sf6core_internal.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <cstring>
#include <mutex>

// ========================================================================
// State
// ========================================================================

static std::atomic<lua_State*> s_lua_state{nullptr};
static const REFrameworkPluginFunctions* s_ref_functions = nullptr;
static std::mutex s_lua_mutex;

// ========================================================================
// Client table bound to a core client via upvalues
// The upvalue at index 1 is a lightuserdata pointing to SF6CoreClient*.
// ========================================================================

static SF6CoreClient* check_client(lua_State* L) {
    auto* c = (SF6CoreClient*)lua_touserdata(L, lua_upvalueindex(1));
    if (!c) luaL_error(L, "SF6Core client handle is invalid");
    return c;
}

// Helper: push an address as a Lua integer
static void push_addr(lua_State* L, void* p) {
    lua_pushinteger(L, (lua_Integer)(uintptr_t)p);
}

// Helper: read an address from a Lua integer argument
static void* check_addr(lua_State* L, int idx) {
    return (void*)(uintptr_t)luaL_checkinteger(L, idx);
}

// ========================================================================
// SF6Core.version() / SF6Core.abi()
// ========================================================================

static int l_version(lua_State* L) {
    auto* api = sf6core_api_ptr();
    lua_pushstring(L, api ? api->get_version() : "unknown");
    return 1;
}

static int l_abi(lua_State* L) {
    lua_pushinteger(L, SF6CORE_ABI_VERSION);
    return 1;
}

// ========================================================================
// Per-client closures (upvalue 1 = SF6CoreClient lightuserdata)
// ========================================================================

static int l_ui_set_text(lua_State* L) {
    auto* c = check_client(L);
    void* text = check_addr(L, 1);
    const char* utf8 = luaL_checkstring(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_text(c, text, utf8));
    return 1;
}

static int l_ui_set_message_id(lua_State* L) {
    auto* c = check_client(L);
    void* text = check_addr(L, 1);
    size_t len = 0;
    const char* guid = luaL_checklstring(L, 2, &len);
    if (len != 16) return luaL_argerror(L, 2, "GUID must be exactly 16 bytes");
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_message_id(c, text, (const uint8_t*)guid));
    return 1;
}

static int l_ui_get_message_id(lua_State* L) {
    void* text = check_addr(L, 1);
    uint8_t guid[16] = {};
    auto* api = sf6core_api_ptr();
    auto st = api->ui_get_message_id(text, guid);
    if (st != SF6CORE_OK) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char*)guid, 16);
    return 1;
}

static int l_ui_set_font_size(lua_State* L) {
    auto* c = check_client(L);
    void* text = check_addr(L, 1);
    float w = (float)luaL_checknumber(L, 2);
    float h = (float)luaL_checknumber(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_font_size(c, text, w, h));
    return 1;
}

static int l_ui_set_region_size(lua_State* L) {
    auto* c = check_client(L);
    void* text = check_addr(L, 1);
    float w = (float)luaL_checknumber(L, 2);
    float h = (float)luaL_checknumber(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_region_size(c, text, w, h));
    return 1;
}

static int l_ui_set_color(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    uint32_t rgba = (uint32_t)luaL_checkinteger(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_color(c, ctrl, rgba));
    return 1;
}

static int l_ui_texture_load(lua_State* L) {
    auto* c = check_client(L);
    const char* path = luaL_checkstring(L, 1);
    void* holder = nullptr;
    auto* api = sf6core_api_ptr();
    auto st = api->ui_texture_load(c, path, &holder);
    if (st != SF6CORE_OK || !holder) { lua_pushnil(L); return 1; }
    push_addr(L, holder);
    return 1;
}

static int l_ui_texture_ready(lua_State* L) {
    void* holder = check_addr(L, 1);
    auto* api = sf6core_api_ptr();
    lua_pushboolean(L, api->ui_texture_ready(holder));
    return 1;
}

static int l_ui_set_texture(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    void* holder = check_addr(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_texture(c, ctrl, holder));
    return 1;
}

static int l_ui_set_visible(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    int visible = lua_toboolean(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_visible(c, ctrl, visible));
    return 1;
}

static int l_ui_set_force_invisible(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    int on = lua_toboolean(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_force_invisible(c, ctrl, on));
    return 1;
}

static int l_ui_set_position(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_position(c, ctrl, x, y, z));
    return 1;
}

static int l_ui_set_scale(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_scale(c, ctrl, x, y, z));
    return 1;
}

static int l_ui_set_size(lua_State* L) {
    auto* c = check_client(L);
    void* ctrl = check_addr(L, 1);
    float w = (float)luaL_checknumber(L, 2);
    float h = (float)luaL_checknumber(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_size(c, ctrl, w, h));
    return 1;
}

static int l_ui_set_block_input(lua_State* L) {
    auto* c = check_client(L);
    void* agent = check_addr(L, 1);
    int block = lua_toboolean(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->ui_set_block_input(c, agent, block));
    return 1;
}

static int l_obj_invoke(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    const char* sig = luaL_checkstring(L, 2);
    int nargs = lua_gettop(L) - 2;
    auto* api = sf6core_api_ptr();

    SF6CoreArg args[16] = {};
    for (int i = 0; i < nargs && i < 16; i++) {
        int idx = i + 3;
        if (lua_isinteger(L, idx)) {
            args[i].type = SF6CORE_ARG_I64;
            args[i].v.i = lua_tointeger(L, idx);
        } else if (lua_isnumber(L, idx)) {
            args[i].type = SF6CORE_ARG_F64;
            args[i].v.f = lua_tonumber(L, idx);
        } else if (lua_isboolean(L, idx)) {
            args[i].type = SF6CORE_ARG_BOOL;
            args[i].v.i = lua_toboolean(L, idx) ? 1 : 0;
        } else if (lua_isstring(L, idx)) {
            args[i].type = SF6CORE_ARG_STR;
            args[i].v.s = lua_tostring(L, idx);
        } else if (lua_istable(L, idx)) {
            lua_getfield(L, idx, "ptr");
            args[i].type = SF6CORE_ARG_PTR;
            args[i].v.p = (void*)(uintptr_t)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
        } else {
            return luaL_argerror(L, idx, "expected integer, number, boolean, string, or {ptr=addr}");
        }
    }

    SF6CoreArg out = {};
    auto st = api->obj_invoke(c, obj, sig, args, (uint32_t)nargs, &out);
    lua_pushinteger(L, st);
    if (st != SF6CORE_OK) return 1;

    // Push result based on type
    switch ((SF6CoreArgType)out.type) {
    case SF6CORE_ARG_I64:  lua_pushinteger(L, out.v.i); return 2;
    case SF6CORE_ARG_F64:  lua_pushnumber(L, out.v.f); return 2;
    case SF6CORE_ARG_BOOL: lua_pushboolean(L, out.v.i != 0); return 2;
    case SF6CORE_ARG_PTR:  push_addr(L, out.v.p); return 2;
    case SF6CORE_ARG_STR:  lua_pushstring(L, out.v.s ? out.v.s : ""); return 2;
    default:               lua_pushinteger(L, out.v.i); return 2;
    }
}

static int l_obj_new(lua_State* L) {
    auto* c = check_client(L);
    const char* type_name = luaL_checkstring(L, 1);
    auto* api = sf6core_api_ptr();
    void* obj = api->obj_new(c, type_name);
    if (!obj) { lua_pushnil(L); return 1; }
    push_addr(L, obj);
    return 1;
}

static int l_obj_clone(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    auto* api = sf6core_api_ptr();
    void* clone = api->obj_clone(c, obj);
    if (!clone) { lua_pushnil(L); return 1; }
    push_addr(L, clone);
    return 1;
}

static int l_obj_set_i64(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    const char* field = luaL_checkstring(L, 2);
    int64_t v = luaL_checkinteger(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->obj_set_i64(c, obj, field, v));
    return 1;
}

static int l_obj_set_f64(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    const char* field = luaL_checkstring(L, 2);
    double v = luaL_checknumber(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->obj_set_f64(c, obj, field, v));
    return 1;
}

static int l_obj_set_string(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    const char* field = luaL_checkstring(L, 2);
    const char* utf8 = luaL_checkstring(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->obj_set_string(c, obj, field, utf8));
    return 1;
}

static int l_obj_set_ref(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    const char* field = luaL_checkstring(L, 2);
    void* val = check_addr(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->obj_set_ref(c, obj, field, val));
    return 1;
}

static int l_obj_set_color(lua_State* L) {
    auto* c = check_client(L);
    void* obj = check_addr(L, 1);
    const char* field = luaL_checkstring(L, 2);
    uint32_t rgba = (uint32_t)luaL_checkinteger(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->obj_set_color(c, obj, field, rgba));
    return 1;
}

static int l_list_add(lua_State* L) {
    auto* c = check_client(L);
    void* list = check_addr(L, 1);
    void* item = check_addr(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->list_add(c, list, item));
    return 1;
}

static int l_list_set_item(lua_State* L) {
    auto* c = check_client(L);
    void* list = check_addr(L, 1);
    int32_t index = (int32_t)luaL_checkinteger(L, 2);
    void* item = check_addr(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->list_set_item(c, list, index, item));
    return 1;
}

static int l_list_get_range(lua_State* L) {
    auto* c = check_client(L);
    void* list = check_addr(L, 1);
    int32_t start = (int32_t)luaL_checkinteger(L, 2);
    int32_t count = (int32_t)luaL_checkinteger(L, 3);
    auto* api = sf6core_api_ptr();
    void* result = api->list_get_range(c, list, start, count);
    if (!result) { lua_pushnil(L); return 1; }
    push_addr(L, result);
    return 1;
}

static int l_dict_add(lua_State* L) {
    auto* c = check_client(L);
    void* dict = check_addr(L, 1);
    int64_t key = luaL_checkinteger(L, 2);
    void* val = check_addr(L, 3);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->dict_add(c, dict, key, val));
    return 1;
}

static int l_dict_contains(lua_State* L) {
    void* dict = check_addr(L, 1);
    int64_t key = luaL_checkinteger(L, 2);
    auto* api = sf6core_api_ptr();
    lua_pushboolean(L, api->dict_contains(dict, key));
    return 1;
}

static int l_ui_find_child(lua_State* L) {
    auto* c = check_client(L);
    (void)c;
    void* root = check_addr(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int32_t depth = (int32_t)luaL_optinteger(L, 3, 8);
    auto* api = sf6core_api_ptr();
    void* result = api->ui_find_child(root, name, depth);
    if (!result) { lua_pushnil(L); return 1; }
    push_addr(L, result);
    return 1;
}

static int l_ui_find_agent(lua_State* L) {
    auto* c = check_client(L);
    (void)c;
    const char* name = luaL_checkstring(L, 1);
    auto* api = sf6core_api_ptr();
    void* result = api->ui_find_agent(name);
    if (!result) { lua_pushnil(L); return 1; }
    push_addr(L, result);
    return 1;
}

static int l_stats(lua_State* L) {
    auto* c = check_client(L);
    auto* api = sf6core_api_ptr();
    SF6CoreDisplayStats stats = {};
    stats.struct_size = sizeof(stats);
    api->get_display_stats(c, &stats);
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)stats.writes);           lua_setfield(L, -2, "writes");
    lua_pushinteger(L, (lua_Integer)stats.skipped_identical); lua_setfield(L, -2, "skipped_identical");
    lua_pushinteger(L, (lua_Integer)stats.refused);          lua_setfield(L, -2, "refused");
    lua_pushinteger(L, (lua_Integer)stats.faults);           lua_setfield(L, -2, "faults");
    lua_pushinteger(L, (lua_Integer)stats.quarantined);      lua_setfield(L, -2, "quarantined");
    return 1;
}

static int l_state(lua_State* L) {
    auto* c = check_client(L);
    auto* api = sf6core_api_ptr();
    lua_pushinteger(L, api->get_client_state(c));
    return 1;
}

// ========================================================================
// SF6Core.client(id) -- returns a table of closures bound to a client
// ========================================================================

// Registry key for tracking existing clients by ID -> SF6CoreClient*
static const char* const CLIENT_REGISTRY_KEY = "SF6Core_clients";

static void push_client_func(lua_State* L, void* client, lua_CFunction fn, const char* name) {
    lua_pushlightuserdata(L, client);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

// Clients created through the bridge, kept across Lua state resets (process lifetime, like the clients).
struct LuaKnownClient { char id[64]; SF6CoreClient* client; };
static LuaKnownClient s_known_clients[32];
static int s_known_count = 0;
static SF6CoreClient* known_lua_client(const char* id) {
    for (int i = 0; i < s_known_count; ++i)
        if (strcmp(s_known_clients[i].id, id) == 0) return s_known_clients[i].client;
    return nullptr;
}
static void remember_lua_client(const char* id, SF6CoreClient* c) {
    if (s_known_count >= 32 || known_lua_client(id)) return;
    strncpy_s(s_known_clients[s_known_count].id, sizeof(s_known_clients[0].id), id, _TRUNCATE);
    s_known_clients[s_known_count].client = c;
    ++s_known_count;
}

static int l_client(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    auto* api = sf6core_api_ptr();
    if (!api) return luaL_error(L, "SF6Core API not available");

    // Check registry for existing client with this ID
    lua_getfield(L, LUA_REGISTRYINDEX, CLIENT_REGISTRY_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, CLIENT_REGISTRY_KEY);
    }

    lua_getfield(L, -1, id);
    if (!lua_islightuserdata(L, -1)) {
        // A REFramework script reset recreates the Lua state (empty registry) but the core keeps the
        // client: re-bind to it instead of registering again (register would fail with DUPLICATE_ID).
        SF6CoreClient* known = known_lua_client(id);
        if (known) {
            lua_pop(L, 1);
            lua_pushlightuserdata(L, known);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, id);
        }
    }
    if (lua_islightuserdata(L, -1)) {
        // Existing client: build a new table bound to the same handle
        void* existing = lua_touserdata(L, -1);
        lua_pop(L, 2); // pop lightuserdata + registry table

        lua_newtable(L);
        push_client_func(L, existing, l_ui_set_text, "ui_set_text");
        push_client_func(L, existing, l_ui_set_message_id, "ui_set_message_id");
        push_client_func(L, existing, l_ui_get_message_id, "ui_get_message_id");
        push_client_func(L, existing, l_ui_set_font_size, "ui_set_font_size");
        push_client_func(L, existing, l_ui_set_region_size, "ui_set_region_size");
        push_client_func(L, existing, l_ui_set_color, "ui_set_color");
        push_client_func(L, existing, l_ui_texture_load, "ui_texture_load");
        push_client_func(L, existing, l_ui_texture_ready, "ui_texture_ready");
        push_client_func(L, existing, l_ui_set_texture, "ui_set_texture");
        push_client_func(L, existing, l_ui_set_visible, "ui_set_visible");
        push_client_func(L, existing, l_ui_set_force_invisible, "ui_set_force_invisible");
        push_client_func(L, existing, l_ui_set_position, "ui_set_position");
        push_client_func(L, existing, l_ui_set_scale, "ui_set_scale");
        push_client_func(L, existing, l_ui_set_size, "ui_set_size");
        push_client_func(L, existing, l_ui_set_block_input, "ui_set_block_input");
        push_client_func(L, existing, l_obj_invoke, "obj_invoke");
        push_client_func(L, existing, l_obj_new, "obj_new");
        push_client_func(L, existing, l_obj_clone, "obj_clone");
        push_client_func(L, existing, l_obj_set_i64, "obj_set_i64");
        push_client_func(L, existing, l_obj_set_f64, "obj_set_f64");
        push_client_func(L, existing, l_obj_set_string, "obj_set_string");
        push_client_func(L, existing, l_obj_set_ref, "obj_set_ref");
        push_client_func(L, existing, l_obj_set_color, "obj_set_color");
        push_client_func(L, existing, l_list_add, "list_add");
        push_client_func(L, existing, l_list_set_item, "list_set_item");
        push_client_func(L, existing, l_list_get_range, "list_get_range");
        push_client_func(L, existing, l_dict_add, "dict_add");
        push_client_func(L, existing, l_dict_contains, "dict_contains");
        push_client_func(L, existing, l_ui_find_child, "ui_find_child");
        push_client_func(L, existing, l_ui_find_agent, "ui_find_agent");
        push_client_func(L, existing, l_stats, "stats");
        push_client_func(L, existing, l_state, "state");
        return 1;
    }
    lua_pop(L, 1); // pop nil

    // Register new client
    SF6CoreClientDesc desc = {};
    desc.struct_size = sizeof(desc);
    desc.client_id = id;
    desc.client_version = "lua-1.0";
    desc.priority = 100;

    SF6CoreClient* client = nullptr;
    auto st = api->register_client(&desc, &client);
    if (st != SF6CORE_OK) {
        lua_pop(L, 1); // pop registry table
        lua_pushnil(L);
        return 1;
    }

    remember_lua_client(id, client);
    // Store in registry
    lua_pushlightuserdata(L, client);
    lua_setfield(L, -2, id);
    lua_pop(L, 1); // pop registry table

    // Build the table
    lua_newtable(L);
    push_client_func(L, client, l_ui_set_text, "ui_set_text");
    push_client_func(L, client, l_ui_set_message_id, "ui_set_message_id");
    push_client_func(L, client, l_ui_get_message_id, "ui_get_message_id");
    push_client_func(L, client, l_ui_set_font_size, "ui_set_font_size");
    push_client_func(L, client, l_ui_set_region_size, "ui_set_region_size");
    push_client_func(L, client, l_ui_set_color, "ui_set_color");
    push_client_func(L, client, l_ui_texture_load, "ui_texture_load");
    push_client_func(L, client, l_ui_texture_ready, "ui_texture_ready");
    push_client_func(L, client, l_ui_set_texture, "ui_set_texture");
    push_client_func(L, client, l_ui_set_visible, "ui_set_visible");
    push_client_func(L, client, l_ui_set_force_invisible, "ui_set_force_invisible");
    push_client_func(L, client, l_ui_set_position, "ui_set_position");
    push_client_func(L, client, l_ui_set_scale, "ui_set_scale");
    push_client_func(L, client, l_ui_set_size, "ui_set_size");
    push_client_func(L, client, l_ui_set_block_input, "ui_set_block_input");
    push_client_func(L, client, l_obj_invoke, "obj_invoke");
    push_client_func(L, client, l_obj_new, "obj_new");
    push_client_func(L, client, l_obj_clone, "obj_clone");
    push_client_func(L, client, l_obj_set_i64, "obj_set_i64");
    push_client_func(L, client, l_obj_set_f64, "obj_set_f64");
    push_client_func(L, client, l_obj_set_string, "obj_set_string");
    push_client_func(L, client, l_obj_set_ref, "obj_set_ref");
    push_client_func(L, client, l_obj_set_color, "obj_set_color");
    push_client_func(L, client, l_list_add, "list_add");
    push_client_func(L, client, l_list_set_item, "list_set_item");
    push_client_func(L, client, l_list_get_range, "list_get_range");
    push_client_func(L, client, l_dict_add, "dict_add");
    push_client_func(L, client, l_dict_contains, "dict_contains");
    push_client_func(L, client, l_ui_find_child, "ui_find_child");
    push_client_func(L, client, l_ui_find_agent, "ui_find_agent");
    push_client_func(L, client, l_stats, "stats");
    push_client_func(L, client, l_state, "state");
    return 1;
}

// ========================================================================
// Increment 5: SF6Core.client_active(id) -> bool
// True when a native client with that id is registered, active, state ACTIVE.
// ========================================================================

static int l_client_active(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    int32_t st = query_client_state_by_id(id);
    lua_pushboolean(L, st == SF6CORE_CLIENT_ACTIVE);
    return 1;
}

// ========================================================================
// Increment 5: Native-function registry (ADR-0002 migration bridge)
//
// C++ clients register functions via lua_register_fn(). Lua scripts call
// them through SF6Core.clients[client_id].<name>(...). The function runs
// on the calling Lua thread. The bridge calls it under SEH; on a fault it
// counts through handle_fault_by_id / breaker and raises a Lua error.
// lua_error is called ONLY from l_native_fn_call (a C function frame),
// AFTER the native call has returned; it never longjmps through C++ frames.
// ========================================================================

static constexpr int NATIVE_FN_MAX = 256;

struct NativeFnEntry {
    char client_id[64];
    char name[64];
    SF6CoreLuaFn fn;
    void* user;
};

static NativeFnEntry s_native_fns[NATIVE_FN_MAX];
static int s_native_fn_n = 0;
static std::mutex s_native_fn_mutex;

void bridge_register_native_fn(SF6CoreClient* client, const char* name,
                                SF6CoreLuaFn fn, void* user) {
    std::lock_guard<std::mutex> lk(s_native_fn_mutex);

    // Unregister (fn == NULL)
    if (!fn) {
        for (int i = 0; i < s_native_fn_n; i++) {
            if (strcmp(s_native_fns[i].client_id, client->client_id) == 0 &&
                strcmp(s_native_fns[i].name, name) == 0) {
                for (int j = i; j < s_native_fn_n - 1; j++)
                    s_native_fns[j] = s_native_fns[j + 1];
                s_native_fn_n--;
                return;
            }
        }
        return;
    }

    // Update existing
    for (int i = 0; i < s_native_fn_n; i++) {
        if (strcmp(s_native_fns[i].client_id, client->client_id) == 0 &&
            strcmp(s_native_fns[i].name, name) == 0) {
            s_native_fns[i].fn = fn;
            s_native_fns[i].user = user;
            return;
        }
    }

    // New entry
    if (s_native_fn_n >= NATIVE_FN_MAX) return;
    auto& e = s_native_fns[s_native_fn_n++];
    strncpy(e.client_id, client->client_id, sizeof(e.client_id) - 1);
    e.client_id[sizeof(e.client_id) - 1] = '\0';
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.fn = fn;
    e.user = user;
}

void bridge_unregister_client_fns(const char* client_id) {
    std::lock_guard<std::mutex> lk(s_native_fn_mutex);
    int dst = 0;
    for (int i = 0; i < s_native_fn_n; i++) {
        if (strcmp(s_native_fns[i].client_id, client_id) != 0) {
            if (dst != i) s_native_fns[dst] = s_native_fns[i];
            dst++;
        }
    }
    s_native_fn_n = dst;
}

// SEH wrapper for the native call (POD only -- no C++ objects needing unwinding)
struct NativeCallArgs {
    SF6CoreLuaFn fn;
    void* user;
    const SF6CoreValue* args;
    uint32_t n_args;
    SF6CoreValue* rets;
    uint32_t max_rets;
    int32_t result;
};

static bool invoke_native_seh(NativeCallArgs* a, DWORD* out_exc) {
    __try {
        a->result = a->fn(a->user, a->args, a->n_args, a->rets, a->max_rets);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out_exc = GetExceptionCode();
        return false;
    }
}

// Upvalue 1: client_id string
// Upvalue 2: function name string
static int l_native_fn_call(lua_State* L) {
    const char* client_id = lua_tostring(L, lua_upvalueindex(1));
    const char* fn_name   = lua_tostring(L, lua_upvalueindex(2));
    if (!client_id || !fn_name)
        return luaL_error(L, "SF6Core native call: invalid upvalues");

    // Look up fn+user under the lock, copy, release
    SF6CoreLuaFn fn = nullptr;
    void* user = nullptr;
    char safe_client_id[64];
    {
        std::lock_guard<std::mutex> lk(s_native_fn_mutex);
        for (int i = 0; i < s_native_fn_n; i++) {
            if (strcmp(s_native_fns[i].client_id, client_id) == 0 &&
                strcmp(s_native_fns[i].name, fn_name) == 0) {
                fn = s_native_fns[i].fn;
                user = s_native_fns[i].user;
                strncpy(safe_client_id, s_native_fns[i].client_id,
                        sizeof(safe_client_id) - 1);
                safe_client_id[sizeof(safe_client_id) - 1] = '\0';
                break;
            }
        }
    }

    if (!fn)
        return luaL_error(L, "SF6Core: native function '%s.%s' not registered",
                          client_id, fn_name);

    // Check client state under the registry mutex (no dangling pointer)
    int32_t cstate = query_client_state_by_id(safe_client_id);
    if (cstate != SF6CORE_CLIENT_ACTIVE)
        return luaL_error(L, "SF6Core: client '%s' not active (state=%d)",
                          client_id, (int)cstate);

    // Convert Lua args -> SF6CoreValue
    int nargs = lua_gettop(L);
    if (nargs > 16) nargs = 16;
    SF6CoreValue args[16] = {};
    for (int i = 0; i < nargs; i++) {
        int idx = i + 1;
        int tp = lua_type(L, idx);
        switch (tp) {
        case LUA_TNIL:
            args[i].type = SF6CORE_VAL_NIL;
            break;
        case LUA_TBOOLEAN:
            args[i].type = SF6CORE_VAL_BOOL;
            args[i].b = lua_toboolean(L, idx);
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                args[i].type = SF6CORE_VAL_INT;
                args[i].i = lua_tointeger(L, idx);
            } else {
                args[i].type = SF6CORE_VAL_DOUBLE;
                args[i].d = lua_tonumber(L, idx);
            }
            break;
        case LUA_TSTRING:
            args[i].type = SF6CORE_VAL_STRING;
            args[i].s = lua_tostring(L, idx);
            break;
        default:
            return luaL_argerror(L, idx,
                                "expected nil, boolean, number, or string");
        }
    }

    // Call under SEH
    SF6CoreValue rets[16] = {};
    NativeCallArgs ca = { fn, user, args, (uint32_t)nargs, rets, 16, 0 };
    DWORD exc = 0;
    bool ok = invoke_native_seh(&ca, &exc);

    if (!ok) {
        // SEH fault: count through breaker by id (safe re-lookup), raise Lua error
        handle_fault_by_id(safe_client_id, exc, "lua_native_fn");
        return luaL_error(L, "SF6Core: native function '%s.%s' faulted (SEH 0x%08X)",
                          client_id, fn_name, (unsigned)exc);
    }

    if (ca.result < 0)
        return luaL_error(L, "SF6Core: native function '%s.%s' returned error %d",
                          client_id, fn_name, (int)ca.result);

    // Push return values -- strings copied into Lua at once
    int n_rets = ca.result;
    if (n_rets > 16) n_rets = 16;
    for (int i = 0; i < n_rets; i++) {
        switch ((SF6CoreValType)rets[i].type) {
        case SF6CORE_VAL_NIL:    lua_pushnil(L); break;
        case SF6CORE_VAL_BOOL:   lua_pushboolean(L, rets[i].b); break;
        case SF6CORE_VAL_INT:    lua_pushinteger(L, (lua_Integer)rets[i].i); break;
        case SF6CORE_VAL_DOUBLE: lua_pushnumber(L, rets[i].d); break;
        case SF6CORE_VAL_STRING: lua_pushstring(L, rets[i].s ? rets[i].s : ""); break;
        default:                 lua_pushnil(L); break;
        }
    }
    return n_rets;
}

// ========================================================================
// Increment 5: SF6Core.clients -- lazy proxy tables via metatables
// ========================================================================

// __index metamethod for a per-client proxy
// Upvalue 1: client_id string
static int l_client_proxy_index(lua_State* L) {
    const char* fn_name = luaL_checkstring(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1));  // client_id
    lua_pushstring(L, fn_name);              // fn_name
    lua_pushcclosure(L, l_native_fn_call, 2);
    return 1;
}

// __index metamethod for SF6Core.clients table
static int l_clients_index(lua_State* L) {
    const char* client_id = luaL_checkstring(L, 2);
    lua_newtable(L);                         // proxy
    lua_newtable(L);                         // metatable
    lua_pushstring(L, client_id);            // upvalue
    lua_pushcclosure(L, l_client_proxy_index, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    // Cache: clients[id] = proxy
    lua_pushvalue(L, -1);
    lua_setfield(L, 1, client_id);
    return 1;
}

// ========================================================================
// Top-level SF6Core table
// ========================================================================

static const luaL_Reg sf6core_funcs[] = {
    {"version",       l_version},
    {"abi",           l_abi},
    {"client",        l_client},
    {"client_active", l_client_active},
    {nullptr,         nullptr}
};

// ========================================================================
// Callbacks registered with REFramework
// ========================================================================

static void on_lua_state_created(lua_State* L) {
    s_lua_state.store(L, std::memory_order_release);

    // Register SF6Core global table
    luaL_newlib(L, sf6core_funcs);

    // Increment 5: SF6Core.clients with a lazy __index metamethod
    lua_newtable(L);                         // clients table
    lua_newtable(L);                         // metatable
    lua_pushcfunction(L, l_clients_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "clients");

    lua_setglobal(L, "SF6Core");

    core_log(SF6CORE_LOG_INFO, nullptr, "Lua bridge: SF6Core registered in Lua state");
}

static void on_lua_state_destroyed(lua_State*) {
    s_lua_state.store(nullptr, std::memory_order_release);
    core_log(SF6CORE_LOG_INFO, nullptr, "Lua bridge: Lua state destroyed");
}

// ========================================================================
// Init / register with REFramework
// ========================================================================

void lua_bridge_init(const REFrameworkPluginFunctions* functions) {
    if (!functions) return;
    s_ref_functions = functions;
    functions->on_lua_state_created(on_lua_state_created);
    functions->on_lua_state_destroyed(on_lua_state_destroyed);
    core_log(SF6CORE_LOG_INFO, nullptr, "Lua bridge initialized (Lua 5.4.3 static)");
}
