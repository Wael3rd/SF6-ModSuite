// cc_game.hpp - Custom Combos: the game's objects through the REFramework plugin API (official REFramework, plugin API
// 1.10: no create_managed_array, see new_array).
//
// Objects are raw managed-object pointers. A field may be a reference (an object of its own) or an inline struct: Ref
// walks both, so a path such as TrainingManager._tData.CounterSetting.DummyData reads the same whichever the game's
// layout is. Values are written by their real width and type (int8..int64, enum, bool, float, double), compared first.
// Reference fields are assigned through the core (obj_set_ref: add_ref(new) / exchange / release(old), R34).
//
// Ownership (rule 1 of the handover): every object this module creates or clones and hands to the game gets one
// reference of ours that is never released (new_obj, new_array, clone): rooted for the life of the process.
#pragma once

#include "reframework/API.hpp"
#include "sf6core/sf6core_api.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cc::g {

using API = reframework::API;
using Obj = API::ManagedObject;
using TD = API::TypeDefinition;
using Method = API::Method;
using Field = API::Field;

inline const SF6CoreAPI* core = nullptr;
inline SF6CoreClient* client = nullptr;

inline std::mutex& cache_mx() { static std::mutex m; return m; }

inline TD* type(const char* name) {
    static std::map<std::string, TD*> cache;
    std::lock_guard lk(cache_mx());
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    TD* t = API::get()->tdb()->find_type(name);
    cache[name] = t;
    return t;
}

inline Field* field_of(TD* td, const char* name) {
    if (!td || !name) return nullptr;
    static std::map<std::pair<TD*, std::string>, Field*> cache;
    std::lock_guard lk(cache_mx());
    auto key = std::make_pair(td, std::string(name));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Field* f = nullptr;
    for (TD* t = td; t && !f; t = t->get_parent_type()) f = t->find_field(name);
    cache[key] = f;
    return f;
}

// A method by full signature ("SetValue(System.Object, System.Int32)") or by bare name with that many parameters
inline Method* method_of(TD* td, const char* name, int nparams = -1) {
    if (!td || !name) return nullptr;
    static std::map<std::tuple<TD*, std::string, int>, Method*> cache;
    std::lock_guard lk(cache_mx());
    auto key = std::make_tuple(td, std::string(name), nparams);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Method* found = nullptr;
    if (std::strchr(name, '(')) {
        for (TD* t = td; t && !found; t = t->get_parent_type()) found = t->find_method(name);
    } else {
        for (TD* t = td; t && !found; t = t->get_parent_type())
            for (Method* m : t->get_methods())
                if (m && m->get_name() && !std::strcmp(m->get_name(), name) &&
                    (nparams < 0 || m->get_num_params() == static_cast<uint32_t>(nparams))) { found = m; break; }
    }
    cache[key] = found;
    return found;
}

inline void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }
inline void* arg_bool(bool v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(v ? 1 : 0)); }

inline TD* td_of(Obj* o) { return o ? o->get_type_definition() : nullptr; }
inline std::string type_name(Obj* o) { TD* t = td_of(o); return t ? t->get_full_name() : std::string{}; }

inline reframework::InvokeRet invoke(Method* m, void* self, std::vector<void*> args) {
    reframework::InvokeRet bad{};
    bad.exception_thrown = true;
    if (!m) return bad;
    try { return m->invoke(reinterpret_cast<Obj*>(self), args); } catch (...) { return bad; }
}
inline reframework::InvokeRet call(Obj* o, const char* name, std::vector<void*> args = {}) {
    reframework::InvokeRet bad{};
    bad.exception_thrown = true;
    if (!o) return bad;
    return invoke(method_of(td_of(o), name, std::strchr(name, '(') ? -1 : static_cast<int>(args.size())), o, std::move(args));
}
inline Obj* call_obj(Obj* o, const char* name, std::vector<void*> args = {}) {
    auto r = call(o, name, std::move(args));
    return !r.exception_thrown && r.ptr ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}
inline std::optional<int32_t> call_i32(Obj* o, const char* name, std::vector<void*> args = {}) {
    auto r = call(o, name, std::move(args));
    if (r.exception_thrown) return std::nullopt;
    return static_cast<int32_t>(r.dword);
}
inline std::optional<bool> call_bool(Obj* o, const char* name, std::vector<void*> args = {}) {
    auto r = call(o, name, std::move(args));
    if (r.exception_thrown) return std::nullopt;
    return r.byte != 0;
}
inline bool call_ok(Obj* o, const char* name, std::vector<void*> args = {}) { return !call(o, name, std::move(args)).exception_thrown; }

inline bool live(Obj* o) {
    if (!o) return false;
    try { return o->is_managed_object(); } catch (...) { return false; }
}

// ---- strings ----
inline std::string str(Obj* s) {
    if (!s) return {};
    const auto sz = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(s) + 0x10);
    if (sz <= 0 || sz > 65536) return {};
    auto* data = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(s) + 0x14);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, data, sz, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, sz, out.data(), bytes, nullptr, nullptr);
    return out;
}
inline std::string call_str(Obj* o, const char* name) { return str(call_obj(o, name)); }
inline std::string name_of(Obj* ctrl) { return call_str(ctrl, "get_Name"); }

// ---- fields: a reference or an inline struct ----
struct Ref {
    void* p = nullptr;
    TD* td = nullptr;
    bool vt = false;          // p points at an inline struct (a field of another object), not at an object
    explicit operator bool() const { return p && td; }
    Obj* obj() const { return vt ? nullptr : reinterpret_cast<Obj*>(p); }
};
inline Ref ref(Obj* o) { return { o, td_of(o), false }; }

inline void* raw_of(const Ref& r, const char* name, Field** out_f = nullptr) {
    if (!r) return nullptr;
    Field* f = field_of(r.td, name);
    if (out_f) *out_f = f;
    if (!f) return nullptr;
    try { return f->get_data_raw(r.p, r.vt); } catch (...) { return nullptr; }
}

inline Ref get(const Ref& r, const char* name) {
    Field* f = nullptr;
    void* raw = raw_of(r, name, &f);
    if (!raw) return {};
    TD* ft = f->get_type();
    if (ft && ft->is_valuetype()) return { raw, ft, true };
    Obj* o = *reinterpret_cast<Obj**>(raw);
    return ref(o);
}
inline Ref get(Obj* o, const char* name) { return get(ref(o), name); }
inline Obj* get_obj(Obj* o, const char* name) { return get(ref(o), name).obj(); }
inline Obj* get_obj(const Ref& r, const char* name) { return get(r, name).obj(); }

enum class Kind { Int, Float, Double, Bool, None };
inline Kind kind_of(Field* f, uint32_t* size, bool* is_unsigned = nullptr) {
    TD* ft = f ? f->get_type() : nullptr;
    if (!ft) return Kind::None;
    const std::string n = ft->get_full_name();
    if (is_unsigned) *is_unsigned = n == "System.Byte" || n == "System.UInt16" || n == "System.UInt32" || n == "System.UInt64" || n == "System.Char";
    if (n == "System.Single") return Kind::Float;
    if (n == "System.Double") return Kind::Double;
    if (n == "System.Boolean") return Kind::Bool;
    if (!ft->is_valuetype()) return Kind::None;
    uint32_t sz = ft->get_valuetype_size();
    if (sz == 0) sz = 4;
    if (sz > 8) return Kind::None;
    *size = sz;
    return Kind::Int;
}

// The value of a number / enum / bool field, as the Lua's get_field read it
inline std::optional<double> num(const Ref& r, const char* name) {
    Field* f = nullptr;
    void* raw = raw_of(r, name, &f);
    if (!raw) return std::nullopt;
    uint32_t sz = 0;
    bool uns = false;
    switch (kind_of(f, &sz, &uns)) {
    case Kind::Float: return static_cast<double>(*reinterpret_cast<float*>(raw));
    case Kind::Double: return *reinterpret_cast<double*>(raw);
    case Kind::Bool: return *reinterpret_cast<uint8_t*>(raw) ? 1.0 : 0.0;
    case Kind::Int: {
        int64_t v = 0;
        std::memcpy(&v, raw, sz);
        // signed (the enums, Int16 combo_cnt...); the unsigned types as they are (Byte pl_type 255, UInt16 pl_sw_new, UInt32 stage_timer)
        if (sz < 8 && !uns) { const int sh = 64 - static_cast<int>(sz) * 8; v = (v << sh) >> sh; }
        return static_cast<double>(v);
    }
    default: return std::nullopt;
    }
}
inline std::optional<double> num(Obj* o, const char* name) { return num(ref(o), name); }
inline int64_t inum(const Ref& r, const char* name, int64_t def) { auto v = num(r, name); return v ? static_cast<int64_t>(*v) : def; }
inline int64_t inum(Obj* o, const char* name, int64_t def) { return inum(ref(o), name, def); }
inline bool flag(Obj* o, const char* name) { return inum(o, name, 0) != 0; }

// Write a number / enum / bool field by its real type; nothing written when it already holds the value
inline bool set(const Ref& r, const char* name, double v) {
    Field* f = nullptr;
    void* raw = raw_of(r, name, &f);
    if (!raw) return false;
    uint32_t sz = 0;
    switch (kind_of(f, &sz)) {
    case Kind::Float: { const float x = static_cast<float>(v); if (*reinterpret_cast<float*>(raw) != x) *reinterpret_cast<float*>(raw) = x; return true; }
    case Kind::Double: if (*reinterpret_cast<double*>(raw) != v) *reinterpret_cast<double*>(raw) = v; return true;
    case Kind::Bool: { const uint8_t b = v != 0 ? 1 : 0; if (*reinterpret_cast<uint8_t*>(raw) != b) *reinterpret_cast<uint8_t*>(raw) = b; return true; }
    case Kind::Int: {
        const int64_t x = static_cast<int64_t>(v);
        if (std::memcmp(raw, &x, sz) != 0) std::memcpy(raw, &x, sz);
        return true;
    }
    default: return false;
    }
}
inline bool set(Obj* o, const char* name, double v) { return set(ref(o), name, v); }

// A reference field, assigned the way the game's own stores do (core obj_set_ref)
inline bool set_ref(Obj* o, const char* name, Obj* v) {
    if (!o || !core || !client) return false;
    if (get_obj(o, name) == v) return true;
    return core->obj_set_ref(client, o, name, v) == SF6CORE_OK;
}

// A System.Guid field: read its 16 bytes, write 16 bytes (core obj_set_guid: type checked, compared first)
inline bool read_guid(Obj* o, const char* name, uint8_t out[16]) {
    Field* f = nullptr;
    void* raw = raw_of(ref(o), name, &f);
    if (!raw || !f->get_type() || f->get_type()->get_valuetype_size() != 16) return false;
    std::memcpy(out, raw, 16);
    return true;
}
inline bool write_guid(Obj* o, const char* name, const uint8_t g[16]) {
    return o && core && client && core->obj_set_guid(client, o, name, g) == SF6CORE_OK;
}

// ---- statics and singletons ----
inline Obj* static_obj(const char* type_name, const char* field) {
    Field* f = field_of(type(type_name), field);
    if (!f) return nullptr;
    void* raw = nullptr;
    try { raw = f->get_data_raw(nullptr, false); } catch (...) { return nullptr; }
    return raw ? *reinterpret_cast<Obj**>(raw) : nullptr;
}
inline Obj* singleton(const char* name) {
    try { return API::get()->get_managed_singleton(name); } catch (...) { return nullptr; }
}

// ---- arrays (System.Array's own methods: never a raw element layout for references) ----
inline TD* array_td() { return type("System.Array"); }
inline int32_t arr_len(Obj* a) {
    if (!a) return 0;
    auto r = invoke(method_of(array_td(), "get_Length", 0), a, {});
    return r.exception_thrown ? 0 : static_cast<int32_t>(r.dword);
}
inline Obj* arr_get(Obj* a, int32_t i) {
    if (!a) return nullptr;
    auto r = invoke(method_of(array_td(), "GetValue(System.Int32)"), a, { arg_i32(i) });
    return !r.exception_thrown && r.ptr ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}
inline bool arr_set(Obj* a, int32_t i, Obj* v) {
    if (!a) return false;
    return !invoke(method_of(array_td(), "SetValue(System.Object, System.Int32)"), a, { v, arg_i32(i) }).exception_thrown;
}
// An empty array of `element` (System.Array.CreateInstance, what REFramework's sdk.create_managed_array calls), rooted.
// NOT API::create_managed_array: plugin API 1.13 only, the players' REFramework ends its function table before it.
inline Obj* new_array(TD* element, int32_t n) {
    if (!element || n < 0) return nullptr;
    Method* create = method_of(array_td(), "CreateInstance(System.Type, System.Int32)");
    Obj* rt = element->get_runtime_type();
    if (!create || !rt) return nullptr;
    auto r = invoke(create, nullptr, { rt, arg_i32(n) });
    Obj* a = !r.exception_thrown ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
    if (!a || !live(a)) return nullptr;
    a->add_ref();
    return a;
}
// A new object of that type (simplify: no constructor run), rooted
inline Obj* new_obj(TD* td, bool simplify) {
    if (!td) return nullptr;
    Obj* o = nullptr;
    try { o = td->create_instance(simplify ? REFRAMEWORK_CREATE_INSTANCE_FLAGS_SIMPLIFY : 0); } catch (...) { return nullptr; }
    if (o) o->add_ref();
    return o;
}
// MemberwiseClone (core obj_clone: the reference it returns is ours, never released), rooted
inline Obj* clone(Obj* o) {
    if (!o || !core || !client) return nullptr;
    return reinterpret_cast<Obj*>(core->obj_clone(client, o));
}

// ---- dictionaries with UInt32 keys (TrainingManager._tfFuncs, _ViewUIWigetDict) ----
// Measured in game: _entries holds the entries INLINE (value types, not boxed), 24 bytes each: hashCode int32 @0, next
// int32 @4, key uint32 @8, value (a reference) @16, from the array's data (+0x20, as every array here); the array is
// longer than the dictionary (a trial: count 8, length 17), a freed entry has a negative hashCode.
inline std::vector<std::pair<uint32_t, Obj*>> dict_u32(Obj* dict) {
    std::vector<std::pair<uint32_t, Obj*>> out;
    Obj* ents = dict ? get_obj(dict, "_entries") : nullptr;
    const int32_t len = arr_len(ents);
    if (len <= 0) return out;
    int64_t cnt = inum(dict, "_count", -1);
    if (cnt < 0) cnt = inum(dict, "count", -1);
    if (cnt < 0) cnt = call_i32(dict, "get_Count").value_or(0);
    cnt = std::min<int64_t>(cnt, len);
    try {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(ents) + 0x20;
        for (int64_t i = 0; i < cnt; ++i) {
            const uint8_t* e = base + i * 24;
            if (*reinterpret_cast<const int32_t*>(e) < 0) continue;
            Obj* v = *reinterpret_cast<Obj* const*>(e + 16);
            if (v && live(v)) out.push_back({ *reinterpret_cast<const uint32_t*>(e + 8), v });
        }
    } catch (...) { out.clear(); }
    return out;
}

// ---- lists (System.Collections.Generic.List) ----
inline int32_t list_count(Obj* l) { return l ? call_i32(l, "get_Count").value_or(0) : 0; }
inline Obj* list_item(Obj* l, int32_t i) { return call_obj(l, "get_Item", { arg_i32(i) }); }
inline bool list_add(Obj* l, Obj* v) { return l && call_ok(l, "Add", { v }); }
inline bool list_clear(Obj* l) { return l && call_ok(l, "Clear"); }

// ---- UI controls ----
inline Obj* child(Obj* c) { return call_obj(c, "get_Child"); }
inline Obj* next(Obj* c) { return call_obj(c, "get_Next"); }
// The first descendant of that name, depth-first (the node's children, each one's subtree before its next sibling)
inline Obj* find_named(Obj* node, const char* name, int depth) {
    for (Obj* c = child(node); c; c = next(c)) {
        if (name_of(c) == name) return c;
        if (depth > 0)
            if (Obj* r = find_named(c, name, depth - 1)) return r;
    }
    return nullptr;
}
// The first direct child of that name
inline Obj* find_child(Obj* node, const char* name) {
    for (Obj* c = child(node); c; c = next(c)) if (name_of(c) == name) return c;
    return nullptr;
}
inline std::string message(Obj* text) { return call_str(text, "get_Message"); }
// Rule 3: a text identical to the one shown is never written (the core compares as well)
inline bool set_message(Obj* text, const std::string& s) {
    if (!text || !core || !client) return false;
    if (message(text) == s) return true;
    return core->ui_set_text(client, text, s.c_str()) == SF6CORE_OK;
}

inline std::vector<Obj*> agents(const char* go_name) {
    std::vector<Obj*> out;
    if (!core) return out;
    void* buf[16] = {};
    const uint32_t n = core->ui_agents(go_name, buf, 16);
    for (uint32_t i = 0; i < n && i < 16; ++i) if (buf[i]) out.push_back(reinterpret_cast<Obj*>(buf[i]));
    return out;
}

}  // namespace cc::g
