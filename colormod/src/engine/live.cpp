#include "live.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings

#include "../common.hpp"
#include "../perf.hpp"
#include "decide.hpp"
#include "live_internal.hpp"
#include "rsz.hpp"

namespace engine::live {
namespace {


constexpr int kMaxFaults = 3;                // then the feature switches itself off (the Lua patch takes over)
constexpr size_t kProbeFields = 8;           // fields read back after a patch, to validate a late restore

// ---- engine methods (resolved once) -----------------------------------------------------------------------------
struct {
    API::Method* comp_go = nullptr;          // via.Component.get_GameObject
    API::Method* go_tr = nullptr;            // via.GameObject.get_Transform
    API::Method* go_name = nullptr;          // via.GameObject.get_Name
    API::Method* tr_parent = nullptr;        // via.Transform.get_Parent
    API::Method* tr_go = nullptr;            // via.Transform.get_GameObject
    API::Method* arr_len = nullptr;          // System.Array.GetLength      (what REFramework's Lua get_size calls)
    API::Method* arr_get = nullptr;          // System.Array.GetValue(Int32) (what its get_element calls)
} M;

// One lock for everything below and for engine::decide: the SetupColor hooks, the other colour hooks and the Lua
// bridge calls (the Lua path was serialized by the Lua state lock in the same way). Recursive like that lock: the
// decisions call SetColor / setTemp* while holding it, and those land in our own hooks.
std::recursive_mutex g_mx;
bool g_installed = false;

// ---- editor working copy ----------------------------------------------------------------------------------------
rsz::NodeRef g_tree;
std::vector<rsz::NodeRef> g_nodes;          // serialization order: node i (1-based) = g_nodes[i - 1]
bool g_active = false;
bool g_hl_on = false;
double g_hl_k = 0.5;
std::unordered_set<const rsz::Node*> g_hl;

// ---- the patch in place (Lua: pending, pending_users, pending_data, pending_frame, dbg.pending_probe) ------------
struct Saved { MO* obj; uint8_t* addr; bool byte; uint32_t old; };
struct Probe { MO* obj; uint8_t* addr; bool byte; uint32_t now; };
std::vector<Saved> g_saved;
std::vector<Probe> g_probe;
bool g_pending = false;
int g_users = 0;
MO* g_pending_data = nullptr;
uint64_t g_pending_frame = 0;

int g_faults = 0;
bool g_disabled = false;
struct Counter { uint64_t n = 0; double us = 0.0; size_t fields = 0; };
Counter g_edit_stats, g_engine_stats;        // editor preview / every other side (owner "engine")
uint64_t g_engine_shared = 0;               // engine_patch calls that found their data already patched this frame
API::TypeDefinition* g_ctrl_td = nullptr;   // app.PlayerColorController

// ---- trees worn by the other sides (owner "engine"): read from the files the Lua names, re-read when they change
struct CachedTree { uintmax_t size = 0; std::filesystem::file_time_type mtime{}; rsz::NodeRef tree; };
std::unordered_map<std::string, CachedTree> g_files;

}  // namespace

rsz::NodeRef file_tree(const std::string& path) {                  // Lua: read_tree (empty / missing / unreadable = nil)
    if (path.empty()) return nullptr;
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) return nullptr;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return nullptr;
    auto& c = g_files[path];
    if (c.size == size && c.mtime == mtime) return c.tree;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = rsz::parse(bytes);
    if (!r.root) cmn::journal("parse error %s: %s", path.c_str(), r.error.c_str());
    c.size = size; c.mtime = mtime; c.tree = r.root;
    return c.tree;
}

// ---- System.Array, read the way REFramework's Lua get_size / get_element do --------------------------------------
uint32_t arr_size(MO* a) {
    auto r = M.arr_len->invoke(a, std::vector<void*>{ ll::arg_i32(0) });
    return r.exception_thrown ? 0u : (uint32_t)std::max<int32_t>(0, (int32_t)r.dword);
}
MO* arr_at(MO* a, uint32_t i) {
    auto r = M.arr_get->invoke(a, std::vector<void*>{ ll::arg_i32((int32_t)i) });
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

namespace {

// ---- List`1 through get_Count / get_Item (color_data, like the Lua's :call) -------------------------------------
struct ListMethods { API::Method* count = nullptr; API::Method* item = nullptr; };
std::unordered_map<API::TypeDefinition*, ListMethods> g_list;
ListMethods list_methods(MO* c) {
    auto* td = c->get_type_definition();
    auto it = g_list.find(td);
    if (it != g_list.end()) return it->second;
    ListMethods m;
    m.count = ll::find_method_noargs(td, "get_Count");
    m.item = ll::find_method_full(td, "get_Item(System.Int32)");
    g_list[td] = m;
    return m;
}

// ---- live field offsets per (class, serialized field): the plain field, the auto-property backing field and the
// `_Value*` runtime cache, like OFFSETS in the Lua (only bool / u32 / f32 fields are written) --------------------------
std::unordered_map<const rsz::ClassSpec*, std::vector<std::vector<uint32_t>>> g_offsets;

}  // namespace

const std::vector<uint32_t>& offsets_of(const rsz::ClassSpec* cls, size_t fi) {
    auto it = g_offsets.find(cls);
    if (it == g_offsets.end()) {
        std::vector<std::vector<uint32_t>> per(cls->fields.size());
        auto* td = API::get()->tdb()->find_type(cls->name);
        for (size_t i = 0; td && i < cls->fields.size(); ++i) {
            const auto k = cls->fields[i].kind;
            if (k != rsz::Kind::Bool && k != rsz::Kind::U32 && k != rsz::Kind::F32) continue;
            const std::string n = cls->fields[i].name;
            for (const std::string& rn : { n, "<" + n + ">k__BackingField", "_" + n }) {
                auto* f = ll::find_field_walk(td, rn.c_str());
                if (!f) continue;
                const uint32_t off = f->get_offset_from_base();
                if (std::find(per[i].begin(), per[i].end(), off) == per[i].end()) per[i].push_back(off);
            }
        }
        it = g_offsets.emplace(cls, std::move(per)).first;
    }
    return it->second[fi];
}

namespace {

const rsz::ClassSpec* g_cluster_cls = nullptr;     // ClusterData: matched by position AND name (like the Lua)

uint32_t read_raw(const uint8_t* addr, bool byte) {
    if (byte) return *addr;
    uint32_t v; std::memcpy(&v, addr, 4); return v;
}
void write(MO* obj, uint8_t* addr, bool byte, uint32_t value) {
    g_saved.push_back({ obj, addr, byte, read_raw(addr, byte) });
    if (byte) *addr = (uint8_t)value; else std::memcpy(addr, &value, 4);
}

uint32_t pull_to_white(uint32_t c, double k) {       // Lua: pack_rgba(floor(r + (255 - r) * k), ..., a)
    const double r = c % 0x100, g = (c / 0x100) % 0x100, b = (c / 0x10000) % 0x100;
    const uint32_t a = (c / 0x1000000) % 0x100;
    const uint32_t r2 = (uint32_t)std::floor(r + (255 - r) * k), g2 = (uint32_t)std::floor(g + (255 - g) * k), b2 = (uint32_t)std::floor(b + (255 - b) * k);
    return a * 0x1000000u + b2 * 0x10000u + g2 * 0x100u + r2;
}

void apply(MO* live, const rsz::Node* node, int depth) {          // Lua: apply_tree (+ inject's highlight)
    if (!live || !node || depth > 12) return;
    const bool hl = g_hl_on && g_hl.count(node);                    // Lua: cc.Color pulled to white, cc.Enable = true
    for (size_t fi = 0; fi < node->cls->fields.size(); ++fi) {
        const auto& fd = node->cls->fields[fi];
        const rsz::Value& v = node->fields[fi];
        switch (fd.kind) {
        case rsz::Kind::Obj:
            if (v.obj) apply(ll::read_field_object(live, fd.name), v.obj.get(), depth + 1);
            break;
        case rsz::Kind::Arr: {
            MO* arr = ll::read_field_object(live, fd.name);
            if (!arr) break;
            const uint32_t n = std::min<uint32_t>(arr_size(arr), (uint32_t)v.arr.size());
            for (uint32_t i = 0; i < n; ++i) {
                const rsz::Node* tv = v.arr[i].get();
                if (!tv) continue;
                MO* elem = arr_at(arr, i);
                if (!elem) continue;
                if (tv->cls == g_cluster_cls) {                       // clusters: same position AND same name
                    const rsz::Value* tn = tv->field("Name");
                    if (!tn || ll::managed_string_to_utf8(ll::read_field_object(elem, "Name")) != tn->s) continue;
                }
                apply(elem, tv, depth + 1);
            }
            break;
        }
        case rsz::Kind::Bool: case rsz::Kind::U32: case rsz::Kind::F32: {
            const auto& offs = offsets_of(node->cls, fi);
            if (offs.empty()) break;
            const bool byte = fd.kind == rsz::Kind::Bool;
            uint32_t value;
            if (byte) value = v.b ? 1u : 0u;
            else if (fd.kind == rsz::Kind::U32) value = v.u;
            else std::memcpy(&value, &v.f, 4);
            if (hl) {                                                  // texture slots (Enable false) show while pulsing
                if (std::string_view(fd.name) == "Enable") value = 1u;
                else if (std::string_view(fd.name) == "Color") value = pull_to_white(v.u, g_hl_k);
            }
            for (uint32_t off : offs) write(live, reinterpret_cast<uint8_t*>(live) + off, byte, value);
            break;
        }
        default: break;                                                // S32 / Str: not written (as in the Lua)
        }
    }
}

// Lua apply_tree(data, tree, {}, 0) of the relay's "AO" order: written for good (nothing saved for a restore)
void write_tree_now(MO* live, const rsz::Node* tree) {
    const size_t keep = g_saved.size();
    const bool hl = g_hl_on;
    g_hl_on = false;
    apply(live, tree, 0);
    g_hl_on = hl;
    g_saved.resize(keep);
}

void fault(const char* where) {
    ++g_faults;
    cmn::journal("editor preview: fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults && !g_disabled) {
        g_disabled = true;
        cmn::log_warn("[ColorModNative] editor preview: %d faults (last in %s), switched off - the Lua patch takes over", g_faults, where);
    }
}

void drop_pending() { g_saved.clear(); g_probe.clear(); g_pending = false; g_users = 0; g_pending_data = nullptr; }

void restore(bool force) {                                             // Lua: restore(force)
    if (!g_pending) return;
    if (!force && --g_users > 0) return;
    if (g_pending_frame != cmn::frame()) {                             // left over from another frame: still ours?
        bool ok = true;
        for (auto& p : g_probe) {
            if (!API::get()->sdk()->managed_object->is_managed_object(p.obj) || read_raw(p.addr, p.byte) != p.now) { ok = false; break; }
        }
        cmn::journal("stale colour patch (frame %llu, %zu fields): %s", (unsigned long long)g_pending_frame, g_saved.size(), ok ? "restored" : "dropped, data no longer ours");
        if (!ok) { drop_pending(); return; }
    }
    // backwards: an address written twice gets its first saved value (the true original) back last
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
        if (it->byte) *it->addr = (uint8_t)it->old; else std::memcpy(it->addr, &it->old, 4);
    }
    drop_pending();
}

}  // namespace

rsz::NodeRef live_tree(MO* live, int depth) {
    if (!live || depth > 12) return nullptr;
    const rsz::ClassSpec* cls = rsz::spec_by_name(live->get_type_definition()->get_full_name());
    if (!cls) return nullptr;
    rsz::NodeRef node = rsz::make_node(cls);
    for (size_t fi = 0; fi < cls->fields.size(); ++fi) {
        const auto& fd = cls->fields[fi];
        rsz::Value& v = node->fields[fi];
        switch (fd.kind) {
        case rsz::Kind::Obj: v.obj = live_tree(ll::read_field_object(live, fd.name), depth + 1); break;
        case rsz::Kind::Arr: {
            MO* arr = ll::read_field_object(live, fd.name);
            const uint32_t n = arr ? arr_size(arr) : 0;
            for (uint32_t i = 0; i < n; ++i) v.arr.push_back(live_tree(arr_at(arr, i), depth + 1));   // nil -> false: a null entry
            break;
        }
        case rsz::Kind::Str: v.s = ll::managed_string_to_utf8(ll::read_field_object(live, fd.name)); break;
        case rsz::Kind::S32: { int32_t x = 0; ll::read_field_i32(live, fd.name, x); v.u = (uint32_t)x; break; }
        default: {
            const auto& offs = offsets_of(cls, fi);
            if (offs.empty()) break;
            const uint8_t* a = reinterpret_cast<const uint8_t*>(live) + offs.back();
            if (fd.kind == rsz::Kind::Bool) v.b = *a != 0;
            else if (fd.kind == rsz::Kind::U32) std::memcpy(&v.u, a, 4);
            else std::memcpy(&v.f, a, 4);
            break;
        }
        }
    }
    return node;
}

MO* invoke_obj(MO* obj, API::Method* m) {
    if (!obj || !m) return nullptr;
    auto r = m->invoke(obj, std::vector<void*>{});
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

std::string go_name(MO* comp) {                                        // Lua: ctrl_go_name
    return ll::managed_string_to_utf8(invoke_obj(invoke_obj(comp, M.comp_go), M.go_name));
}

int side_from_parents(MO* ctrl) {                                      // Lua: side_from_parents (0 = nil)
    MO* t = invoke_obj(invoke_obj(ctrl, M.comp_go), M.go_tr);
    for (int i = 0; i < 4 && t; ++i) {
        MO* par = invoke_obj(t, M.tr_parent);
        if (!par) return 0;
        const std::string nm = ll::managed_string_to_utf8(invoke_obj(invoke_obj(par, M.tr_go), M.go_name));
        if (nm == "FighterVisual_01" || nm == "Left") return 1;
        if (nm == "FighterVisual_02" || nm == "Right") return 2;
        if (nm == "FighterReady_BattleSetting") return 3;            // Fighter Settings preview
        t = par;
    }
    return 0;
}

MO* color_data(MO* ctrl, int32_t id) {                                 // Lua: color_data
    MO* cd = ll::read_field_object(ctrl, "ColorData");
    MO* colors = cd ? ll::read_field_object(cd, "Colors") : nullptr;
    if (!colors) return nullptr;
    const auto lm = list_methods(colors);
    if (!lm.count || !lm.item) return nullptr;
    auto rc = lm.count->invoke(colors, std::vector<void*>{});
    const int32_t n = rc.exception_thrown ? 0 : (int32_t)rc.dword;
    for (int32_t i = 0; i < n; ++i) {
        auto ri = lm.item->invoke(colors, std::vector<void*>{ ll::arg_i32(i) });
        MO* e = ri.exception_thrown ? nullptr : reinterpret_cast<MO*>(ri.ptr);
        int32_t cid = -1;
        if (e && ll::read_field_i32(e, "ColorId", cid) && cid == id) return ll::read_field_object(e, "Data");
    }
    return nullptr;
}

namespace {

// Lua: the end of inject - same data in the same frame = one more user, else a fresh patch (validated by probes)
size_t patch(MO* data, const rsz::Node* tree, Counter& stats) {
    const uint64_t frame = cmn::frame();
    if (g_pending && g_pending_data == data && g_pending_frame == frame) { ++g_users; return 0; }   // sub-models, same data
    restore(true);                                                     // a different data object: start over
    const auto t0 = std::chrono::steady_clock::now();
    apply(data, tree, 0);
    for (size_t i = 0; i < g_saved.size() && i < kProbeFields; ++i) g_probe.push_back({ g_saved[i].obj, g_saved[i].addr, g_saved[i].byte, read_raw(g_saved[i].addr, g_saved[i].byte) });
    g_pending = true; g_users = 1; g_pending_data = data; g_pending_frame = frame;
    stats.us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    stats.fields = g_saved.size();
    ++stats.n;
    return g_saved.size();
}

}  // namespace

std::recursive_mutex& mx() { return g_mx; }

bool editor_on() { return g_active && g_tree && !g_disabled; }

int editor_patch(MO* ctrl) {                                           // Lua: inject of the editor's side 3
    int32_t num = -1;
    if (!ll::read_field_i32(ctrl, "ColorNum", num)) return -1;
    MO* data = color_data(ctrl, num);
    if (!data) { cmn::journal("inject P3: no colour data (colour %d)", num); return -3; }
    return (int)patch(data, g_tree.get(), g_edit_stats);
}

int engine_apply(MO* data, const rsz::Node* tree) {
    // same data, same frame: one more user before anything else (Lua: the pending check comes first)
    if (g_pending && g_pending_data == data && g_pending_frame == cmn::frame()) { ++g_users; ++g_engine_shared; return 0; }
    return (int)patch(data, tree, g_engine_stats);
}

void abort_patch() { try { restore(true); } catch (...) { drop_pending(); } }
void write_tree(MO* live, const rsz::Node* tree) { write_tree_now(live, tree); }

namespace {

void on_setup_pre(MO* ctrl) {                                          // Lua: on_setup_pre -> should_inject -> inject
    if (g_pending && g_pending_frame != cmn::frame()) restore(true);  // stale patch from another frame: never left behind
    if (!ctrl) return;
    if (decide::active()) { decide::on_setup_pre(ctrl); return; }      // owner "decisions": the DLL decides every side
    if (!editor_on()) return;                                          // else only the editor's side 3 (the Lua does the rest)
    if (side_from_parents(ctrl) != 3) return;
    editor_patch(ctrl);
}

// Diagnostics (03/10, colours lost after a replay -> training): did SetupColor really set the colours of the controller
// we patched, and does the game re-read colour data through SetupPartsColor alone (no patch on) later? Journal only.
MO* g_diag_ctrl = nullptr;                   // controller of the SetupColor in progress, when it got a patch
std::unordered_map<MO*, uint64_t> g_diag_parts;   // SetupPartsColor-without-patch lines, one per controller per 120 frames

int pre_setup_color(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    perf::Scope timed(perf::HookSetupColor);
    std::lock_guard lk(g_mx);
    g_diag_ctrl = nullptr;
    try {
        if (argc >= 2) {
            MO* ctrl = reinterpret_cast<MO*>(argv[1]);
            on_setup_pre(ctrl);
            if (g_pending && g_pending_frame == cmn::frame() && decide::active()) g_diag_ctrl = ctrl;
        }
    }
    catch (...) { try { restore(true); } catch (...) { drop_pending(); } fault("SetupColor pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_parts(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    perf::Scope timed(perf::HookSetupParts);
    std::lock_guard lk(g_mx);
    try {
        if (argc >= 2 && !g_pending && decide::active()) {     // outside SetupColor, on a dressed controller: journal only
            MO* ctrl = reinterpret_cast<MO*>(argv[1]);
            const std::string sig = decide::applied_sig(ctrl);
            auto& last = g_diag_parts[ctrl];
            if (!sig.empty() && (last == 0 || cmn::frame() - last >= 120)) {
                last = cmn::frame();
                cmn::journal("SetupPartsColor %s outside SetupColor (wears %s, no patch on)", go_name(ctrl).c_str(), sig.c_str());
            }
        }
    } catch (...) { fault("SetupPartsColor pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_restore(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    perf::Scope timed(perf::HookSetupParts);
    std::lock_guard lk(g_mx);
    try { restore(false); } catch (...) { drop_pending(); fault("SetupColor post"); }
}
void post_setup_color(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    perf::Scope timed(perf::HookSetupColor);
    std::lock_guard lk(g_mx);
    try {
        if (g_diag_ctrl && ret_val && ((uintptr_t)*ret_val & 0xFF) == 0)
            cmn::journal("SetupColor %s returned false after its patch: the game sets its colours later", go_name(g_diag_ctrl).c_str());
        g_diag_ctrl = nullptr;
        restore(false);
    } catch (...) { drop_pending(); fault("SetupColor post"); }
}

void on_lua_destroyed(lua_State*) {                                    // Reset Scripts: the Lua editor is gone
    std::lock_guard lk(g_mx);
    if (g_active) cmn::journal("Lua state destroyed: editor preview off");
    g_active = false; g_hl_on = false; g_hl.clear();
}

}  // namespace

bool install() {
    auto* tdb = API::get()->tdb();
    M.comp_go = tdb->find_method("via.Component", "get_GameObject");
    M.go_tr = tdb->find_method("via.GameObject", "get_Transform");
    M.go_name = tdb->find_method("via.GameObject", "get_Name");
    M.tr_parent = tdb->find_method("via.Transform", "get_Parent");
    M.tr_go = tdb->find_method("via.Transform", "get_GameObject");
    M.arr_len = tdb->find_method("System.Array", "GetLength");
    M.arr_get = tdb->find_method("System.Array", "GetValue(System.Int32)");
    g_cluster_cls = rsz::spec_by_name("app.CostumeMaterialData.ClusterData");
    g_ctrl_td = tdb->find_type("app.PlayerColorController");
    auto* setup = tdb->find_method("app.PlayerColorController", "SetupColor");
    auto* parts = tdb->find_method("app.PlayerColorController", "SetupPartsColor");
    if (!M.comp_go || !M.go_tr || !M.go_name || !M.tr_parent || !M.tr_go || !M.arr_len || !M.arr_get || !g_ctrl_td || !setup || !parts || !g_cluster_cls) {
        cmn::log_warn("[ColorModNative] colour patch: engine types or methods missing, editor/engine features off");
        return false;
    }
    setup->add_hook(pre_setup_color, post_setup_color, false);
    parts->add_hook(pre_parts, post_restore, false);
    API::get()->param()->functions->on_lua_state_destroyed(on_lua_destroyed);
    g_installed = true;
    cmn::log_info("[ColorModNative] colour patch: hooks on PlayerColorController.SetupColor / SetupPartsColor");
    return true;
}

bool ready() {
    std::lock_guard lk(g_mx);
    return g_installed && !g_disabled;
}

void editor_active(bool on) {
    std::lock_guard lk(g_mx);
    if (on != g_active) cmn::journal("editor preview %s", on ? "on" : "off");
    g_active = on;
    if (!on) { g_hl_on = false; g_hl.clear(); }
}

void editor_share(const rsz::NodeRef& tree, bool hl_on, double hl_k, const std::vector<const rsz::Node*>& hl) {
    std::lock_guard lk(g_mx);
    if (tree != g_tree) {
        g_tree = tree;
        g_nodes = rsz::emit_order(tree);
        cmn::journal("editor tree shared: %zu nodes", g_nodes.size());
    }
    g_hl_on = hl_on; g_hl_k = hl_k;
    g_hl.clear();
    for (auto* n : hl) g_hl.insert(n);
    if (!g_active) cmn::journal("editor preview on");
    g_active = true;
}

// the controller class or one derived from it (the Lua has no type check here; dbg.set_color uses is_a)
bool is_color_ctrl(MO* obj) {
    for (auto* t = obj->get_type_definition(); t != nullptr; t = t->get_parent_type()) if (t == g_ctrl_td) return true;
    return false;
}

std::string status() {
    std::lock_guard lk(g_mx);
    char b[384];
    auto avg = [](const Counter& c) { return c.n ? c.us / (double)c.n : 0.0; };
    std::snprintf(b, sizeof(b), "frame=%llu installed=%d active=%d nodes=%zu patches=%llu fields=%zu avg=%.1fus engine_patches=%llu shared=%llu engine_fields=%zu engine_avg=%.1fus files=%zu pending=%d users=%d faults=%d disabled=%d",
                  (unsigned long long)cmn::frame(), g_installed ? 1 : 0, g_active ? 1 : 0, g_nodes.size(), (unsigned long long)g_edit_stats.n, g_edit_stats.fields, avg(g_edit_stats),
                  (unsigned long long)g_engine_stats.n, (unsigned long long)g_engine_shared, g_engine_stats.fields, avg(g_engine_stats), g_files.size(),
                  g_pending ? 1 : 0, g_users, g_faults, g_disabled ? 1 : 0);
    return b;
}

void debug_offline() {
    std::lock_guard lk(g_mx);
    g_installed = true;
}

std::vector<uint8_t> debug_serialize() {
    std::lock_guard lk(g_mx);
    return g_tree ? rsz::serialize(g_tree) : std::vector<uint8_t>{};
}

std::string debug_hl() {
    std::lock_guard lk(g_mx);
    std::string out;
    if (!g_hl_on) return out;
    for (size_t i = 0; i < g_nodes.size(); ++i) {
        const rsz::Node* n = g_nodes[i].get();
        if (!g_hl.count(n)) continue;
        const rsz::Value* c = n->field("Color");
        if (!out.empty()) out += ",";
        out += std::to_string(i + 1) + ":" + n->cls->name + ":" + (c ? std::to_string(c->u) : std::string("-"));
    }
    return out;
}

}  // namespace engine::live
