// SF6 Slots — native REFramework plugin, shared declarations.
//
// The game-side part of the slots, in one plugin: costume menus, save data, ownership, colours and
// the select screen UI, the online alias of a slot on DriveTech (costumes.cpp), the stage select
// screen and the VS screen of the stage slots (stages.cpp). It replaces the two Lua scripts.
//
// Driven by events, never by polling. Hooks on the game's own methods (a menu shown or closed, the
// select screen started, a stage focused, an outfit folder mounted) record what happened in
// atomics; the work is done on the next LateUpdateBehavior, on the game thread, and stops when it is
// done. Nothing in a fight triggers anything, so a fight costs one check of this plugin's own memory
// per frame: no read of the game. The only work not started by an event is at boot, once.
//
// Rules this code follows (each one cost a crash once):
//   - Game objects are touched only from LateUpdateBehavior. Hooks may be called on any thread:
//     they only store numbers.
//   - Objects kept across frames are checked with is_managed_object before use.
//   - A GUI text is written only when it differs, a texture only once it is loaded.
#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <reframework/API.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace slots {

using API    = reframework::API;
using Obj    = API::ManagedObject;
using TD     = API::TypeDefinition;
using Method = API::Method;
using Ret    = reframework::InvokeRet;

inline API& api() { return *API::get(); }

// ---- log: reframework/data/SF6_CostumeSlots_data/native_log.txt ----
void log_open();
void logf(const char* fmt, ...);

// ---- frame counter (LateUpdateBehavior calls) ----
extern uint32_t g_frame;

// ---- events: set by hooks (any thread), taken on the game thread ----
enum Event : uint32_t {
    EV_MENU_OPEN     = 1u << 0,   // Battle Settings shown
    EV_MENU_CLOSE    = 1u << 1,   // Battle Settings closed
    EV_SELECT_START  = 1u << 2,   // character select screen started
    EV_SELECT_END    = 1u << 3,   // character select screen ended
    EV_HOLDER        = 1u << 4,   // an outfit's visual manifest was created: the game mounted its folder
    EV_STAGE_SHOW    = 1u << 5,   // stage select screen shown
    EV_STAGE_HIDE    = 1u << 6,   // stage select screen hidden
    EV_STAGE_DIRTY   = 1u << 7,   // focus changed, or the game rewrote the name / the preview
    EV_STAGE_INPUT   = 1u << 8,   // UP / DOWN on the stage select screen
    EV_VS            = 1u << 9,   // VS screen shown or rewritten by the game
    EV_TITLE_END     = 1u << 10,  // title screen left (the game clears DLC ownership at login)
    EV_DIAG          = 1u << 31,  // a method watched for the log was called
};
// Methods watched for the log only: which of them the game calls, and when
void diag_watch(TD* td, const char* method);
void diag_flush();
extern std::atomic<uint32_t> g_events;
inline void post(uint32_t e) { g_events.fetch_or(e, std::memory_order_release); }

// ---- fields ----
API::Field* field_of(TD* td, const char* name);            // walks the parents
void* field_ptr(Obj* o, const char* name);                   // address of the field in the object
Obj* get_obj(Obj* o, const char* name);
bool get_i32(Obj* o, const char* name, int32_t& out);
bool get_u32(Obj* o, const char* name, uint32_t& out);
bool get_bool(Obj* o, const char* name, bool& out);
bool set_i32(Obj* o, const char* name, int32_t v);
bool set_u32(Obj* o, const char* name, uint32_t v);
bool set_bool(Obj* o, const char* name, bool v);
bool set_obj(Obj* o, const char* name, Obj* v);
inline bool alive(Obj* o) { return o && o->is_managed_object(); }

// ---- methods ----
Method* method_of(TD* td, const char* name, int nparams = -1);   // exact signature, else bare name
Ret call(Method* m, Obj* o, std::vector<void*> args = {});
Ret call(Obj* o, const char* name, std::vector<void*> args = {}); // looked up on the object's type
Obj* call_obj(Method* m, Obj* o, std::vector<void*> args = {});
Obj* call_obj(Obj* o, const char* name, std::vector<void*> args = {});
bool call_bool(Obj* o, const char* name, std::vector<void*> args = {}, bool* ok = nullptr);
inline void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }
inline void* arg_bool(bool v)    { return reinterpret_cast<void*>(static_cast<uintptr_t>(v ? 1u : 0u)); }
inline void* arg_obj(Obj* o)     { return reinterpret_cast<void*>(o); }

// ---- collections ----
int list_count(Obj* list);
Obj* list_item(Obj* list, int i);
// The i-th element of a list whose elements may be objects or structs (a struct comes back by value)
struct Elem {
    Obj* obj = nullptr;
    TD* vt = nullptr;                    // set when the element is a struct
    Ret ret{};
    bool ok = false;
    void* field(const char* name) const;
    Obj* obj_field(const char* name) const { void* p = field(name); return p ? *reinterpret_cast<Obj**>(p) : nullptr; }
    bool u32_field(const char* name, uint32_t& out) const { void* p = field(name); if (!p) return false; memcpy(&out, p, 4); return true; }
};
Elem list_elem(Obj* list, int i);

// ---- strings ----
std::string str(Obj* s);                          // System.String -> UTF-8
Obj* new_string(const std::string& utf8);         // UTF-8 -> System.String
std::string type_name(Obj* o);

// ---- scene ----
Obj* current_scene();
Obj* find_components(TD* type);                   // scene:findComponents(System.Type)

// ---- hooks (REFramework plugin API) ----
int  hook_pass(int argc, void** argv, REFrameworkTypeDefinitionHandle* arg_tys, unsigned long long ret_addr);
void hook_post_pass(void** ret_val, REFrameworkTypeDefinitionHandle ret_ty, unsigned long long ret_addr);
// every method of the type (and its parents) with this name gets the hook; returns how many
int hook_all(TD* td, const char* method, REFPreHookFn pre, REFPostHookFn post);

// ---- files ----
std::string read_file(const char* path, size_t max_size = 4u << 20);
bool write_file(const char* path, const std::string& data);   // through a temporary file

// ---- JSON (the files the loader and this plugin write) ----
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* key) const;
    int64_t as_int(int64_t def = 0) const { return kind == Number ? static_cast<int64_t>(num) : def; }
    const std::string& as_str() const { static const std::string e; return kind == String ? s : e; }
};
bool json_parse(const std::string& text, Json& out);
std::string json_escape(const std::string& s);

// ---- modules: install their hooks once, then work on events ----
namespace costumes {
    void init();                      // registry and state files, before the first frame
    void install_hooks();             // first frame (the type database is ready)
    bool busy();                      // work left without a new event (boot, a mount in progress)
    void tick(uint32_t events);
}
namespace stages {
    void init();
    void install_hooks();
    bool busy();
    void tick(uint32_t events);
}

} // namespace slots
