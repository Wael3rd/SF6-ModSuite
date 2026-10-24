#include "scene.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>
#include <unordered_map>

#include "../common.hpp"
#include "../write_journal.hpp"
#include "memguard.hpp"

namespace vfx::scene {
namespace {

using API = reframework::API;

struct ListM { API::Method* count = nullptr; API::Method* item = nullptr; };

API::Method* g_arr_get = nullptr;            // System.Array.GetValue(Int32): boxes a value type, returns a reference as is
API::Method* g_arr_len = nullptr;            // System.Array.GetLength(Int32)
API::Method* g_get_components = nullptr;     // via.GameObject.get_Components
API::Field* g_gbattle_player = nullptr;      // gBattle.Player (static)
uint32_t g_off_tint = 0, g_off_enable = 0;   // via.effect.script.EPVStandardData.Element Color / ColorEnable
std::unordered_map<API::TypeDefinition*, ListM> g_lists;
std::unordered_map<API::TypeDefinition*, bool> g_is_transform;
std::unordered_map<API::TypeDefinition*, API::Method*> g_methods[4];   // get_Name, getResourcesCount, getResources, get_ResourcePath
enum { kGetName, kResCount, kResGet, kResPath };

bool page_readable(uint64_t a, size_t n) { return memguard::readable(a, n); }   // one VirtualQuery per region and frame

#pragma warning(push)
#pragma warning(disable: 4733)
bool seh_copy(void* dst, uint64_t src, size_t n) {
    __try { std::memcpy(dst, (const void*)src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_put(uint64_t dst, const void* src, size_t n) {
    __try { std::memcpy((void*)dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#pragma warning(pop)

bool rd(uint64_t a, void* out, size_t n) {
    if (a < 0x10000 || !page_readable(a, n)) return false;
    return seh_copy(out, a, n);
}

API::Method* method_of(int which, MO* o, const char* sig) {
    auto* td = o->get_type_definition();
    auto& cache = g_methods[which];
    auto it = cache.find(td);
    if (it == cache.end()) it = cache.emplace(td, std::strchr(sig, '(') ? ll::find_method_full(td, sig) : ll::find_method_noargs(td, sig)).first;
    return it->second;
}

std::string str_of(MO* s) { return s ? ll::managed_string_to_utf8(s) : std::string(); }

MO* invoke_obj(MO* o, API::Method* m, std::vector<void*> args = {}) {
    if (!o || !m) return nullptr;
    auto r = m->invoke(o, args);
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

bool invoke_i32(MO* o, API::Method* m, int32_t& out, std::vector<void*> args = {}) {
    if (!o || !m) return false;
    auto r = m->invoke(o, args);
    if (r.exception_thrown) return false;
    out = (int32_t)r.dword;
    return true;
}

// List`1: get_Count / get_Item(System.Int32), resolved once per list type (the Lua's :call("get_Count") / ("get_Item", i))
const ListM* list_m(MO* lst) {
    if (!lst) return nullptr;
    auto* td = lst->get_type_definition();
    auto it = g_lists.find(td);
    if (it == g_lists.end()) it = g_lists.emplace(td, ListM{ ll::find_method_noargs(td, "get_Count"), ll::find_method_full(td, "get_Item(System.Int32)") }).first;
    return (it->second.count && it->second.item) ? &it->second : nullptr;
}

template <typename F> void each_item(MO* lst, F fn) {
    const ListM* m = list_m(lst);
    if (!m) return;
    int32_t n = 0;
    if (!invoke_i32(lst, m->count, n)) return;
    for (int32_t i = 0; i < n; ++i) {
        MO* it = invoke_obj(lst, m->item, { ll::arg_i32(i) });
        if (it) fn(it, i);
    }
}

uint32_t arr_len(MO* a) {
    if (!a || !g_arr_len) return 0u;
    void* slot[1] = { ll::arg_i32(0) };                  // stack slots: no vector allocation per call
    auto r = g_arr_len->invoke(a, std::span<void*>{ slot });
    return (!r.exception_thrown && (int32_t)r.dword > 0) ? (uint32_t)(int32_t)r.dword : 0u;
}
MO* arr_get(MO* a, uint32_t i) {
    if (!a || !g_arr_get) return nullptr;
    void* slot[1] = { ll::arg_i32((int32_t)i) };
    auto r = g_arr_get->invoke(a, std::span<void*>{ slot });
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

// ll::find_field_walk with a per-thread memo of (type, name) -> field (names are compared by content; a type's fields never
// change, a miss is memoised too). The by-name walk of the type database was paid ~10 times per frame in battle_act.
API::Field* field_of(API::TypeDefinition* td, const char* name) {
    struct E { API::TypeDefinition* td; char name[32]; API::Field* f; };
    thread_local E tab[48]; thread_local unsigned n = 0;
    if (!td || !name) return nullptr;
    for (unsigned i = 0; i < n; ++i) if (tab[i].td == td && std::strcmp(tab[i].name, name) == 0) return tab[i].f;
    API::Field* f = ll::find_field_walk(td, name);
    if (n < 48 && std::strlen(name) < sizeof(E::name)) { E& e = tab[n++]; e.td = td; std::memcpy(e.name, name, std::strlen(name) + 1); e.f = f; }
    return f;
}
MO* obj_field(MO* o, const char* name) {                 // ll::read_field_object through the memo
    if (!o) return nullptr;
    auto* f = field_of(o->get_type_definition(), name);
    void* raw = f ? f->get_data_raw(o, false) : nullptr;
    return raw ? *reinterpret_cast<MO**>(raw) : nullptr;
}

bool is_transform(MO* comp) {
    auto* td = comp->get_type_definition();
    auto it = g_is_transform.find(td);
    if (it == g_is_transform.end()) it = g_is_transform.emplace(td, td && td->get_full_name() == "via.Transform").first;
    return it->second;
}

// the components of a GameObject except its via.Transform (the Lua: get_Components, skip "via.Transform")
template <typename F> void each_component(MO* go, F fn) {
    MO* comps = invoke_obj(go, g_get_components);
    if (!comps) return;
    const uint32_t n = arr_len(comps);
    for (uint32_t c = 0; c < n; ++c) {
        MO* comp = arr_get(comps, c);
        if (comp && !is_transform(comp)) fn(comp);
    }
}

int read_i32_field(MO* o, const char* name) { int32_t v = 0; return (o && ll::read_field_i32(o, name, v)) ? v : 0; }

// a field path like the Lua's a.b.c, whether each step is an object or an inline value type
struct Ref { void* p = nullptr; API::TypeDefinition* td = nullptr; bool vt = false; };
Ref ref_of(MO* o) { return Ref{ o, o ? o->get_type_definition() : nullptr, false }; }
Ref fld(const Ref& r, const char* name) {
    if (!r.p || !r.td) return {};
    auto* f = field_of(r.td, name);
    if (!f) return {};
    void* raw = r.vt ? static_cast<uint8_t*>(r.p) + f->get_offset_from_fieldptr() : f->get_data_raw(r.p, false);
    if (!raw) return {};
    auto* ft = f->get_type();
    if (ft && ft->is_valuetype()) return Ref{ raw, ft, true };
    MO* o = *reinterpret_cast<MO**>(raw);
    return ref_of(o);
}
int i32_at(const Ref& r, const char* name) {           // an Int32 field of r
    if (!r.p || !r.td) return 0;
    auto* f = field_of(r.td, name);
    if (!f) return 0;
    void* raw = r.vt ? static_cast<uint8_t*>(r.p) + f->get_offset_from_fieldptr() : f->get_data_raw(r.p, false);
    int32_t v = 0;
    if (raw) std::memcpy(&v, raw, 4);
    return v;
}

}  // namespace

bool init() {
    auto* tdb = API::get()->tdb();
    g_arr_get = tdb->find_method("System.Array", "GetValue(System.Int32)");
    g_arr_len = tdb->find_method("System.Array", "GetLength(System.Int32)");
    g_get_components = tdb->find_method("via.GameObject", "get_Components");
    auto* gb = tdb->find_type("gBattle");
    g_gbattle_player = gb ? gb->find_field("Player") : nullptr;
    auto* el_td = tdb->find_type("via.effect.script.EPVStandardData.Element");
    auto* fc = el_td ? el_td->find_field("Color") : nullptr;
    auto* fe = el_td ? el_td->find_field("ColorEnable") : nullptr;
    g_off_tint = fc ? fc->get_offset_from_base() : 0;
    g_off_enable = fe ? fe->get_offset_from_base() : 0;
    const bool ok = g_arr_get && g_arr_len && g_get_components && g_gbattle_player && g_off_tint && g_off_enable;
    if (!ok) cmn::journal("vfx scene: missing types (GetValue %d GetLength %d get_Components %d gBattle.Player %d Element.Color %u/%u)",
                          g_arr_get != nullptr, g_arr_len != nullptr, g_get_components != nullptr, g_gbattle_player != nullptr, g_off_tint, g_off_enable);
    return ok;
}

bool rd32(uint64_t a, uint32_t& out) { return rd(a, &out, 4); }
bool rd64(uint64_t a, uint64_t& out) { return rd(a, &out, 8); }
bool rd8(uint64_t a, uint8_t& out) { return rd(a, &out, 1); }

std::vector<uint8_t> cstr(uint64_t a, size_t maxn) {
    std::vector<uint8_t> t;
    for (size_t i = 0; i < maxn; ++i) {
        uint8_t b = 0;
        if (!rd8(a + i, b) || b == 0) break;
        t.push_back(b);
    }
    return t;
}

bool wr8(uint64_t a, uint8_t v) {
    if (a < 0x10000 || !page_readable(a, 1)) return false;
    if (wjournal::active()) { uint8_t old = 0; seh_copy(&old, a, 1); wjournal::raw8("scene::wr8", a, old, v); }
    return seh_put(a, &v, 1);
}
bool wr32(uint64_t a, uint32_t v) {
    if (a < 0x10000 || (a & 3) || !page_readable(a, 4)) return false;      // a colour dword is 4-byte aligned
    if (wjournal::active()) { uint32_t old = 0; seh_copy(&old, a, 4); wjournal::raw32("scene::wr32", a, old, v); }
    return seh_put(a, &v, 4);
}

int battle_char(int idx) {
    if (!g_gbattle_player) return 0;
    void* slot = g_gbattle_player->get_data_raw(nullptr, false);
    MO* sp = slot ? *reinterpret_cast<MO**>(slot) : nullptr;
    MO* mc = sp ? ll::read_field_object(sp, "mcPlayer") : nullptr;
    if (!mc || (uint32_t)idx >= arr_len(mc)) return 0;
    MO* pl = arr_get(mc, (uint32_t)idx);
    const int id = i32_at(fld(fld(ref_of(pl), "mpActParam"), "ActionPart"), "_CharaID");
    return id > 0 ? id : 0;
}

bool battle_act(int idx, int64_t& out) {
    if (!g_gbattle_player) return false;
    void* slot = g_gbattle_player->get_data_raw(nullptr, false);
    MO* sp = slot ? *reinterpret_cast<MO**>(slot) : nullptr;
    MO* mc = sp ? obj_field(sp, "mcPlayer") : nullptr;
    if (!mc || (uint32_t)idx >= arr_len(mc)) return false;
    MO* pl = arr_get(mc, (uint32_t)idx);
    const Ref part = fld(fld(ref_of(pl), "mpActParam"), "ActionPart");
    const Ref eng = fld(part, "_Engine");
    if (!eng.p || eng.vt) return false;
    MO* e = reinterpret_cast<MO*>(eng.p);
    static API::Method* get_id = nullptr; static API::TypeDefinition* get_id_td = nullptr;   // re-resolved when the engine type changes
    if (e->get_type_definition() != get_id_td) { get_id_td = e->get_type_definition(); get_id = ll::find_method_noargs(get_id_td, "get_ActionID"); }
    int32_t v = 0;
    if (!invoke_i32(e, get_id, v)) return false;
    out = v;
    return true;
}

int side_char(int idx) {
    if (const int id = battle_char(idx)) return id;
    MO* tm = API::get()->get_managed_singleton("app.training.TrainingManager");
    MO* tdata = tm ? ll::read_field_object(tm, "_tData") : nullptr;
    MO* menu = tdata ? ll::read_field_object(tdata, "SelectMenu") : nullptr;
    MO* pds = menu ? ll::read_field_object(menu, "PlayerDatas") : nullptr;
    MO* pd = (pds && (uint32_t)idx < arr_len(pds)) ? arr_get(pds, (uint32_t)idx) : nullptr;
    const int id = read_i32_field(pd, "FighterID");
    return id > 0 ? id : 0;
}

std::vector<Entry> entries() {
    std::vector<Entry> out;
    MO* em = API::get()->get_managed_singleton("via.effect.script.EffectManager");
    MO* map = em ? ll::read_field_object(em, "DataInstanceMap") : nullptr;
    MO* arr = map ? ll::read_field_object(map, "_entries") : nullptr;
    if (!arr) return out;
    const uint32_t n = arr_len(arr);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        MO* e = arr_get(arr, i);                       // a boxed Dictionary entry (the Lua's get_elements)
        if (!e) continue;
        MO* k = ll::read_field_object(e, "key");
        if (!k) continue;                              // free slot
        MO* v = ll::read_field_object(e, "value");
        MO* go = v ? ll::read_field_object(v, "Instance") : nullptr;
        out.push_back(Entry{ str_of(k), go });
    }
    return out;
}

void each_element(MO* go, const std::function<void(MO*, int)>& fn) {
    if (!go) return;
    each_component(go, [&](MO* comp) {
        MO* elist = ll::read_field_object(comp, "Elements");
        each_item(elist, [&](MO* el, int j) { fn(el, j); });
    });
}

void each_extern(MO* go, const std::function<void(MO*, int, int, const std::string&, const std::string&)>& fn) {
    each_element(go, [&](MO* el, int j) {
        MO* xp = ll::read_field_object(el, "ExternParameters");
        each_item(xp, [&](MO* param, int p) {
            const std::string name = str_of(invoke_obj(param, method_of(kGetName, param, "get_Name")));
            const std::string rn = str_of(ll::read_field_object(param, "ResourceName"));
            fn(param, j, p, name, rn);
        });
    });
}

void each_resource(MO* el, const std::function<void(MO*, const std::string&)>& fn) {
    int32_t n = 0;
    if (!invoke_i32(el, method_of(kResCount, el, "getResourcesCount"), n)) return;
    for (int32_t i = 0; i < n; ++i) {
        MO* h = invoke_obj(el, method_of(kResGet, el, "getResources(System.Int32)"), { ll::arg_i32(i) });
        if (!h) continue;
        fn(h, str_of(invoke_obj(h, method_of(kResPath, h, "get_ResourcePath"))));
    }
}

bool element_has_color(MO* el) {
    bool has = false;
    MO* xp = ll::read_field_object(el, "ExternParameters");
    each_item(xp, [&](MO* param, int) {
        const std::string name = str_of(invoke_obj(param, method_of(kGetName, param, "get_Name")));
        if (name == "Color" || name == "SubColor" || name == "AColor") has = true;
    });
    return has;
}

uint32_t off_tint() { return g_off_tint; }
uint32_t off_enable() { return g_off_enable; }

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

}  // namespace vfx::scene
