#include "gm.hpp"
#include "perf.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>

namespace game::gm {
namespace {

std::mutex g_mx;                                                    // the caches below (any thread)
std::unordered_map<std::string, TD*> g_types;
std::map<std::pair<TD*, std::string>, API::Field*> g_fields;
std::map<std::pair<TD*, std::string>, API::Method*> g_methods;

// System.Array / List`1 methods, resolved once per array / list type
API::Method* g_arr_len = nullptr;
API::Method* g_arr_get = nullptr;
API::Method* g_arr_set = nullptr;
std::map<TD*, std::pair<API::Method*, API::Method*>> g_list_methods;   // get_Count, get_Item

// UInt16[] layout check
std::once_flag g_u16_once;
bool g_u16 = false;
TD* g_u16_td = nullptr;                                             // System.UInt16[] (the type of the test array)
std::string g_u16_report = "not checked";
constexpr size_t kArrCount = 0x1C;                                  // REArrayBase::numElements (REFramework ReClass_Internal_SF6)
constexpr size_t kArrData = 0x20;                                   // first element (sizeof(REArrayBase))

API::Method* arr_method(const char* sig) {
    TD* a = type("System.Array");
    return a ? a->find_method(sig) : nullptr;
}

// ---- per-thread caches in front of the maps (hot paths: the menu's row hooks call these hundreds of times a frame) ----
// Lookups by (type, name): a direct-mapped table per thread, keyed by the type and the name's address and checked against a
// copy of the name (a buffer reused with another name misses), so a hit builds no string and takes no lock. Names of 48
// bytes or more always go to the maps. The kinds of value-field types (float / unsigned / bool) likewise, by type.
constexpr size_t kFastN = 256, kFastKey = 48;
struct FastEntry { TD* td; const char* key; void* v; char copy[kFastKey]; };
thread_local FastEntry t_fast_methods[kFastN];
thread_local FastEntry t_fast_fields[kFastN];
struct KindEntry { TD* td; uint8_t kind; };
thread_local KindEntry t_kinds[kFastN];
enum : uint8_t { kKindFloat = 1, kKindUnsigned = 2, kKindBool = 4 };

size_t fast_slot(const void* a, const void* b) {
    const uint64_t h = (uint64_t)(uintptr_t)a * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)b * 0xC2B2AE3D27D4EB4Full;
    return (size_t)(h >> 40) % kFastN;
}
bool fast_get(FastEntry* tab, TD* td, const char* key, void** out) {
    const FastEntry& e = tab[fast_slot(td, key)];
    if (e.td != td || e.key != key || std::strncmp(e.copy, key, kFastKey) != 0) return false;
    *out = e.v;
    return true;
}
void fast_put(FastEntry* tab, TD* td, const char* key, void* v) {
    const size_t n = std::strlen(key);
    if (n >= kFastKey) return;
    FastEntry& e = tab[fast_slot(td, key)];
    e.td = td;
    e.key = key;
    e.v = v;
    std::memcpy(e.copy, key, n + 1);
}

uint8_t kind_of(TD* t) {
    if (!t) return 0;
    KindEntry& e = t_kinds[fast_slot(t, nullptr)];
    if (e.td == t) return e.kind;
    const std::string full = t->get_full_name();
    uint8_t k = 0;
    if (full == "System.Single" || full == "System.Double") k |= kKindFloat;
    if (full == "System.UInt16" || full == "System.UInt32" || full == "System.UInt64" || full == "System.Byte" || full == "System.Char")
        k |= kKindUnsigned;
    if (full == "System.Boolean") k |= kKindBool;
    e.td = t;
    e.kind = k;
    return k;
}

bool is_unsigned(TD* t) { return (kind_of(t) & kKindUnsigned) != 0; }
bool is_float(TD* t) { return (kind_of(t) & kKindFloat) != 0; }
bool is_bool(TD* t) { return (kind_of(t) & kKindBool) != 0; }

// The value field `name` of o: its address and its type (nullptr when o has no such value field).
void* value_field(MO* o, const char* name, TD** ft, uint32_t* size) {
    if (!o) return nullptr;
    API::Field* f = field(o->get_type_definition(), name);
    if (!f || f->is_static()) return nullptr;
    TD* t = f->get_type();
    if (!t || !t->is_valuetype()) return nullptr;
    uint32_t sz = t->get_valuetype_size();
    if (sz == 0) return nullptr;
    if (ft) *ft = t;
    if (size) *size = sz;
    return f->get_data_raw(o, false);
}

}  // namespace

TD* type(const char* full_name) {
    perf::gm(perf::GmType);
    {
        std::lock_guard lk(g_mx);
        auto it = g_types.find(full_name);
        if (it != g_types.end()) return it->second;
    }
    TD* t = API::get()->tdb()->find_type(full_name);
    std::lock_guard lk(g_mx);
    g_types[full_name] = t;
    return t;
}

API::Field* field(TD* td, const char* name) {
    perf::gm(perf::GmField);
    if (!td || !name) return nullptr;
    if (void* v = nullptr; fast_get(t_fast_fields, td, name, &v)) return static_cast<API::Field*>(v);
    const auto key = std::make_pair(td, std::string(name));
    {
        std::lock_guard lk(g_mx);
        auto it = g_fields.find(key);
        if (it != g_fields.end()) { fast_put(t_fast_fields, td, name, it->second); return it->second; }
    }
    API::Field* f = nullptr;
    for (TD* t = td; t && !f; t = t->get_parent_type()) f = t->find_field(name);
    {
        std::lock_guard lk(g_mx);
        g_fields[key] = f;
    }
    fast_put(t_fast_fields, td, name, f);
    return f;
}

API::Method* method(TD* td, const char* sig) {
    perf::gm(perf::GmMethod);
    if (!td || !sig) return nullptr;
    if (void* v = nullptr; fast_get(t_fast_methods, td, sig, &v)) return static_cast<API::Method*>(v);
    const auto key = std::make_pair(td, std::string(sig));
    {
        std::lock_guard lk(g_mx);
        auto it = g_methods.find(key);
        if (it != g_methods.end()) { fast_put(t_fast_methods, td, sig, it->second); return it->second; }
    }
    API::Method* m = nullptr;
    for (TD* t = td; t && !m; t = t->get_parent_type()) m = t->find_method(sig);
    {
        std::lock_guard lk(g_mx);
        g_methods[key] = m;
    }
    fast_put(t_fast_methods, td, sig, m);
    return m;
}

API::Method* method_n(TD* td, const char* name, uint32_t nparams) {
    perf::gm(perf::GmMethodN);
    if (!td || !name) return nullptr;
    const auto key = std::make_pair(td, std::string(name) + "#" + std::to_string(nparams));
    {
        std::lock_guard lk(g_mx);
        auto it = g_methods.find(key);
        if (it != g_methods.end()) return it->second;
    }
    API::Method* found = nullptr;
    for (TD* t = td; t && !found; t = t->get_parent_type()) {
        for (API::Method* m : t->get_methods()) {
            const char* mn = m ? m->get_name() : nullptr;
            if (mn && std::strcmp(mn, name) == 0 && m->get_num_params() == nparams) { found = m; break; }
        }
    }
    std::lock_guard lk(g_mx);
    g_methods[key] = found;
    return found;
}

MO* singleton(const char* name) { perf::gm(perf::GmSingleton); return API::get()->get_managed_singleton(name); }

bool is_managed(const void* p) {
    perf::gm(perf::GmIsManaged);
    if (!p || (uintptr_t)p < 0x10000) return false;
    return reinterpret_cast<const MO*>(p)->is_managed_object();
}

// ---- fields ----------------------------------------------------------------------------------------------------------------
MO* ref(MO* o, const char* name) {
    perf::gm(perf::GmRef);
    if (!o) return nullptr;
    API::Field* f = field(o->get_type_definition(), name);
    if (!f || f->is_static()) return nullptr;
    TD* t = f->get_type();
    if (t && t->is_valuetype()) return nullptr;
    void** p = static_cast<void**>(f->get_data_raw(o, false));
    return p ? static_cast<MO*>(*p) : nullptr;
}

bool get_int(MO* o, const char* name, int64_t* v) {
    perf::gm(perf::GmGetInt);
    TD* t = nullptr;
    uint32_t sz = 0;
    void* p = value_field(o, name, &t, &sz);
    if (!p || is_float(t)) return false;
    const bool u = is_unsigned(t);
    switch (sz) {
        case 1: *v = u ? (int64_t) * (uint8_t*)p : (int64_t) * (int8_t*)p; break;
        case 2: *v = u ? (int64_t) * (uint16_t*)p : (int64_t) * (int16_t*)p; break;
        case 4: *v = u ? (int64_t) * (uint32_t*)p : (int64_t) * (int32_t*)p; break;
        case 8: *v = *(int64_t*)p; break;
        default: return false;
    }
    if (is_bool(t)) *v = (*v != 0) ? 1 : 0;
    return true;
}

bool get_bool(MO* o, const char* name, bool* v) {
    int64_t x = 0;
    if (!get_int(o, name, &x)) return false;
    *v = x != 0;
    return true;
}

bool set_int(MO* o, const char* name, int64_t v) {
    perf::gm(perf::GmSetInt);
    TD* t = nullptr;
    uint32_t sz = 0;
    void* p = value_field(o, name, &t, &sz);
    if (!p || is_float(t)) return false;
    switch (sz) {
        case 1: *(uint8_t*)p = (uint8_t)v; break;
        case 2: *(uint16_t*)p = (uint16_t)v; break;
        case 4: *(uint32_t*)p = (uint32_t)v; break;
        case 8: *(uint64_t*)p = (uint64_t)v; break;
        default: return false;
    }
    return true;
}

bool set_bool(MO* o, const char* name, bool v) {
    perf::gm(perf::GmSetInt);
    TD* t = nullptr;
    uint32_t sz = 0;
    void* p = value_field(o, name, &t, &sz);
    if (!p || sz != 1 || !t || !is_bool(t)) return false;
    *(uint8_t*)p = v ? 1 : 0;
    return true;
}

bool is_float_field(MO* o, const char* name) {
    perf::gm(perf::GmIsFloat);
    TD* t = nullptr;
    return value_field(o, name, &t, nullptr) && is_float(t);
}

bool int_field(MO* o, const char* name, IntField* out) {
    perf::gm(perf::GmGetInt);
    TD* t = nullptr;
    uint32_t sz = 0;
    void* p = value_field(o, name, &t, &sz);
    if (!p || is_float(t) || (sz != 1 && sz != 2 && sz != 4 && sz != 8)) return false;
    *out = IntField{ p, sz, is_unsigned(t), is_bool(t) };
    return true;
}

bool read_int(const IntField& f, int64_t* v) {   // get_int's conversion, on the address int_field found
    const void* p = f.p;
    if (!p) return false;
    const bool u = f.is_unsigned;
    switch (f.size) {
        case 1: *v = u ? (int64_t) * (const volatile uint8_t*)p : (int64_t) * (const volatile int8_t*)p; break;
        case 2: *v = u ? (int64_t) * (const volatile uint16_t*)p : (int64_t) * (const volatile int16_t*)p; break;
        case 4: *v = u ? (int64_t) * (const volatile uint32_t*)p : (int64_t) * (const volatile int32_t*)p; break;
        case 8: *v = *(const volatile int64_t*)p; break;
        default: return false;
    }
    if (f.is_bool) *v = (*v != 0) ? 1 : 0;
    return true;
}

// R34, exactly as the game's own compiled field stores, which test the TARGET first: a target whose reference count
// ([o+8]) is negative is a local object (not refcounted yet, swept with its frame) and gets a plain store, no add_ref, no
// release: its fields own no reference. Any other target: add_ref(new) -> atomic exchange -> release(old). Releasing the
// old value of a local target's field drops a reference the field never held: the Native Core's write_field_managed did
// that until 0.8.0 (commit fa312e8, the pause-menu crashes of 11/10). The DLL's own targets are all refcounted
// (the training data's FighterData / RecordData, objects of ours add_ref'd at creation): ref_status()'s "into a local
// target" count (status.json, "refs") is expected to stay 0.
namespace {
std::atomic<uint64_t> g_ref_writes{0}, g_local_ref_writes{0};
}  // namespace

bool set_ref(MO* o, const char* name, MO* v) {
    perf::gm(perf::GmSetRef);
    if (!o) return false;
    API::Field* f = field(o->get_type_definition(), name);
    if (!f || f->is_static()) return false;
    TD* t = f->get_type();
    if (t && t->is_valuetype()) return false;
    void* raw = f->get_data_raw(o, false);
    if (!raw) return false;
    void* volatile* slot = static_cast<void* volatile*>(raw);
    if (*slot == v) return true;
    g_ref_writes.fetch_add(1, std::memory_order_relaxed);
    if (*reinterpret_cast<const volatile int32_t*>(reinterpret_cast<const uint8_t*>(o) + 8) < 0) {   // a local target
        g_local_ref_writes.fetch_add(1, std::memory_order_relaxed);
        _InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), v);
        return true;
    }
    if (v) v->add_ref();
    void* old = *slot;
    while (_InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), v, old) != old) old = *slot;
    if (old) static_cast<MO*>(old)->release();
    return true;
}

std::string ref_status() {
    return std::to_string(g_ref_writes.load(std::memory_order_relaxed)) + " reference writes, " +
           std::to_string(g_local_ref_writes.load(std::memory_order_relaxed)) + " into a local target (plain store)";
}

// ---- calls ---------------------------------------------------------------------------------------------------------------------
void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }

bool call(MO* o, API::Method* m, std::initializer_list<void*> args, reframework::InvokeRet* out) {
    if (!m) return false;
    perf::gm(perf::GmInvoke);
    void* a[16] = {};
    size_t n = 0;
    for (void* x : args) { if (n < 16) a[n++] = x; }
    reframework::InvokeRet r = m->invoke(o, std::span<void*>(a, n));
    if (out) *out = r;
    return !r.exception_thrown;
}

bool call(MO* o, const char* sig, std::initializer_list<void*> args, reframework::InvokeRet* out) {
    if (!o) return false;
    return call(o, method(o->get_type_definition(), sig), args, out);
}

MO* call_obj(MO* o, const char* sig, std::initializer_list<void*> args) {
    reframework::InvokeRet r{};
    if (!call(o, sig, args, &r)) return nullptr;
    return static_cast<MO*>(r.ptr);
}

bool call_i32(MO* o, const char* sig, int32_t* v, std::initializer_list<void*> args) {
    reframework::InvokeRet r{};
    if (!call(o, sig, args, &r)) return false;
    *v = (int32_t)r.dword;
    return true;
}

bool call_bool(MO* o, const char* sig, bool* v, std::initializer_list<void*> args) {
    reframework::InvokeRet r{};
    if (!call(o, sig, args, &r)) return false;
    *v = r.byte != 0;
    return true;
}

// ---- arrays and lists -----------------------------------------------------------------------------------------------------------
int32_t array_len(MO* arr) {
    if (!arr) return -1;
    if (!g_arr_len) g_arr_len = arr_method("get_Length");
    reframework::InvokeRet r{};
    if (!call(arr, g_arr_len, {}, &r)) return -1;
    return (int32_t)r.dword;
}

MO* array_get(MO* arr, int32_t i) {
    if (!arr) return nullptr;
    if (!g_arr_get) g_arr_get = arr_method("GetValue(System.Int32)");
    reframework::InvokeRet r{};
    if (!call(arr, g_arr_get, { arg_i32(i) }, &r)) return nullptr;
    return static_cast<MO*>(r.ptr);
}

bool array_set(MO* arr, int32_t i, MO* v) {
    if (!arr) return false;
    if (!g_arr_set) g_arr_set = arr_method("SetValue(System.Object, System.Int32)");
    return call(arr, g_arr_set, { v, arg_i32(i) });
}

MO* new_array(TD* element, uint32_t n) {
    if (!element) return nullptr;
    // NOT API::create_managed_array: that member of the plugin function table came with plugin API 1.13 (03/2025); the
    // players' REFramework (1.5.8 nightly 12/2024, plugin API 1.10) ends its table before it, so the call would read past
    // the table and run whatever follows. System.Array.CreateInstance(System.Type, System.Int32) is what REFramework's own
    // sdk.create_managed_array calls.
    static API::Method* create = nullptr;
    if (!create) {
        create = method(type("System.Array"), "CreateInstance(System.Type, System.Int32)");
        if (!create) return nullptr;
    }
    MO* rt = element->get_runtime_type();   // typeof(element): the VM's own object, never freed
    if (!rt) return nullptr;
    reframework::InvokeRet r{};
    if (!call(nullptr, create, { rt, arg_i32((int32_t)n) }, &r) || !r.ptr) return nullptr;
    MO* a = static_cast<MO*>(r.ptr);
    if (!a->is_managed_object()) return nullptr;
    a->add_ref();
    return a;
}

MO* new_object(TD* td) {
    if (!td) return nullptr;
    perf::gm(perf::GmNewObject);
    MO* o = td->create_instance();
    if (o) o->add_ref();
    return o;
}

int32_t list_count(MO* list) {
    if (!list) return -1;
    TD* t = list->get_type_definition();
    API::Method* m = nullptr;
    {
        std::lock_guard lk(g_mx);
        auto it = g_list_methods.find(t);
        if (it != g_list_methods.end()) m = it->second.first;
    }
    if (!m) {
        m = method(t, "get_Count");
        API::Method* item = method(t, "get_Item(System.Int32)");
        if (!item) item = method_n(t, "get_Item", 1);
        std::lock_guard lk(g_mx);
        g_list_methods[t] = { m, item };
    }
    reframework::InvokeRet r{};
    if (!call(list, m, {}, &r)) return -1;
    return (int32_t)r.dword;
}

MO* list_item(MO* list, int32_t i) {
    if (!list) return nullptr;
    TD* t = list->get_type_definition();
    API::Method* m = nullptr;
    {
        std::lock_guard lk(g_mx);
        auto it = g_list_methods.find(t);
        if (it != g_list_methods.end()) m = it->second.second;
    }
    if (!m) {
        API::Method* cnt = method(t, "get_Count");
        m = method(t, "get_Item(System.Int32)");
        if (!m) m = method_n(t, "get_Item", 1);
        std::lock_guard lk(g_mx);
        g_list_methods[t] = { cnt, m };
    }
    reframework::InvokeRet r{};
    if (!call(list, m, { arg_i32(i) }, &r)) return nullptr;
    return static_cast<MO*>(r.ptr);
}

// ---- UInt16[] --------------------------------------------------------------------------------------------------------------------
namespace {
void u16_check_body() {
    TD* u16 = type("System.UInt16");
    if (!u16) { g_u16_report = "System.UInt16 not found"; return; }
    MO* a = new_array(u16, 3);
    if (!a) { g_u16_report = "System.Array.CreateInstance(System.UInt16, 3) failed"; return; }
    const int32_t raw_count = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a) + kArrCount);
    const int32_t len = array_len(a);
    if (raw_count != 3 || len != 3) {
        g_u16_report = "element count: raw " + std::to_string(raw_count) + ", get_Length " + std::to_string(len) + " (want 3)";
        return;
    }
    uint16_t* data = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(a) + kArrData);
    data[0] = 0x1111; data[1] = 0xBEEF; data[2] = 0x2222;
    MO* boxed = array_get(a, 1);
    uint16_t back = 0;
    bool read = false;
    if (boxed) {
        API::Field* f = field(boxed->get_type_definition(), "mValue");
        if (f) { void* p = f->get_data_raw(boxed, false); if (p) { back = *static_cast<uint16_t*>(p); read = true; } }
    }
    if (!read || back != 0xBEEF) {
        g_u16_report = std::string("GetValue(1) after a raw write: ") + (read ? std::to_string(back) : std::string("unreadable")) + " (want 48879)";
        return;
    }
    g_u16_td = a->get_type_definition();
    g_u16 = true;
    g_u16_report = "ok (count at 0x1C, data at 0x20, " + (g_u16_td ? g_u16_td->get_full_name() : std::string("?")) + ")";
}
bool u16_check_seh() {
    __try { u16_check_body(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
}  // namespace

bool u16_ok() {
    std::call_once(g_u16_once, [] {
        if (!u16_check_seh()) { g_u16 = false; g_u16_report = "the check faulted"; }
    });
    return g_u16;
}

std::string u16_report() { return g_u16_report; }

uint16_t* u16_data(MO* arr, int32_t* len) {
    if (!arr || !u16_ok() || arr->get_type_definition() != g_u16_td) return nullptr;
    const int32_t n = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(arr) + kArrCount);
    if (n < 0) return nullptr;
    if (len) *len = n;
    return reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(arr) + kArrData);
}

}  // namespace game::gm
