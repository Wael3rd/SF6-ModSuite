#include "providers.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <unordered_map>

#include "../perf.hpp"
#include "scene.hpp"

namespace vfx::providers {
namespace {

using scene::MO;

// plain string helpers (replace std::regex in per-frame loops)
bool parse_provider_path(const std::string& key, int& fighter_id, std::string& shortk) {
    // matches: .../esfNNN/EffectProvider/NAME.pfb
    if (key.size() < 28 || key.compare(key.size() - 4, 4, ".pfb") != 0) return false;
    const size_t ep = key.rfind("/EffectProvider/");
    if (ep == std::string::npos || ep < 7) return false;
    if (key[ep - 7] != '/' || key[ep - 6] != 'e' || key[ep - 5] != 's' || key[ep - 4] != 'f') return false;
    const char d0 = key[ep - 3], d1 = key[ep - 2], d2 = key[ep - 1];
    if (d0 < '0' || d0 > '9' || d1 < '0' || d1 > '9' || d2 < '0' || d2 > '9') return false;
    fighter_id = (d0 - '0') * 100 + (d1 - '0') * 10 + (d2 - '0');
    shortk.assign(key, ep + 16, key.size() - ep - 20);
    return true;
}
bool is_hit_style(const std::string& s) {
    return s.size() == 21 && s.compare(0, 8, "epve_esf") == 0 &&
           s[8] >= '0' && s[8] <= '9' && s[9] >= '0' && s[9] <= '9' && s[10] >= '0' && s[10] <= '9' &&
           s.compare(11, 10, "_hit_style") == 0;
}
bool is_effect_style(const std::string& s) {
    return s.size() >= 19 && s.compare(0, 8, "epvs_esf") == 0 &&
           s[8] >= '0' && s[8] <= '9' && s[9] >= '0' && s[9] <= '9' && s[10] >= '0' && s[10] <= '9' &&
           s.compare(11, 7, "_style_") == 0;
}
int parse_style_number(const std::string& s) {
    const size_t pos = s.rfind("_style_");
    if (pos == std::string::npos) return -1;
    const size_t dstart = pos + 7;
    if (dstart >= s.size()) return -1;
    int val = 0;
    for (size_t i = dstart; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        val = val * 10 + (s[i] - '0');
    }
    return val;
}
bool parse_efx_name(const std::string& path, std::string& name) {
    if (path.size() < 5 || path.compare(path.size() - 4, 4, ".efx") != 0) return false;
    const size_t slash = path.rfind('/');
    const size_t start = (slash != std::string::npos) ? slash + 1 : 0;
    name.assign(path, start, path.size() - start - 4);
    return true;
}

constexpr uint32_t kExternColorOff = 0x28;   // EffectCustomExternParameter: the RGBA get_Color returns (DI EXTERN_COLOR_OFF)
constexpr uint32_t kTypeGpuMesh = 0xFB;      // EfxMeshColor TYPE_GPU_MESH

struct Provider { std::string key; MO* go = nullptr; uint64_t addr = 0; bool hit = false; };
struct Tint { uint8_t enable = 0; uint32_t rgba = 0; bool white = false; };   // mode "white" or "c1"
struct Originals { std::map<std::pair<int, int>, uint32_t> externs; std::map<int, Tint> tints; };
struct Mesh { uint64_t addr; std::string kind; std::string name; std::string path; int el; uint32_t o0, o1; };
struct PState {
    uint64_t addr = 0;
    int is_target = -1;                                  // -1 = not decided yet (Lua nil)
    std::optional<Originals> originals;
    bool written = false;
    std::optional<std::map<int, bool>> drive_el;
    std::map<std::string, std::pair<uint32_t, uint32_t>> mesh_orig;
    std::vector<Mesh> mesh;
    bool has_mesh = false;                               // st.mesh set (nmesh counts only after a refresh)
};

std::map<int, std::vector<Provider>> g_found;            // last_found
uint64_t g_found_gen = 0;                                // +1 each time g_found is replaced (LinesCursor)
bool g_armor_seen = false;                               // the armour rim's entry at the last scan (armor_params)
MO* g_armor_go = nullptr;
std::map<std::string, PState> g_pstate;                  // pstate, by provider key
std::map<int, std::vector<int>> g_side_map;              // side_map
std::map<int, uint64_t> g_parts_seen;                    // parts_seen (sum of the provider addresses)
std::map<int, std::string> g_parts;                      // the last collected list, "key\tname" lines
struct ArmorParam { uint64_t addr; bool c2; uint32_t orig; bool has_orig; };
struct { bool valid = false; uint64_t addr = 0; std::vector<ArmorParam> params; } g_armor;

std::optional<uint32_t> param_rgba(MO* param) {          // get_param_rgba: the via.Color get_Color returns = RGBA at +0x28
    uint32_t v = 0;
    if (!scene::rd32(scene::addr(param) + kExternColorOff, v)) return std::nullopt;
    return v;
}
bool slot_of(const std::string& name, bool& c2) {        // SLOT_OF: Color -> c1, SubColor / AColor -> c2
    if (name == "Color") { c2 = false; return true; }
    if (name == "SubColor" || name == "AColor") { c2 = true; return true; }
    return false;
}

// ---- EfxMeshColor ------------------------------------------------------------------------------------------------
bool has_seq(const std::vector<uint8_t>& t, const std::vector<uint8_t>& seq) {
    if (t.size() < seq.size()) return false;
    for (size_t i = 0; i + seq.size() <= t.size(); ++i) if (std::equal(seq.begin(), seq.end(), t.begin() + i)) return true;
    return false;
}
std::string classify(const std::vector<uint8_t>& b) {    // M.classify (the start flag is not used by the relay)
    static const std::vector<uint8_t> MAIN = { 0xE3, 0x83, 0xA1, 0xE3, 0x82, 0xA4, 0xE3, 0x83, 0xB3 };
    static const std::vector<uint8_t> ACCENT = { 0xE3, 0x82, 0xA2, 0xE3, 0x82, 0xAF, 0xE3, 0x82, 0xBB, 0xE3, 0x83, 0xB3, 0xE3, 0x83, 0x88 };
    static const std::vector<uint8_t> SUB = { 0xE3, 0x82, 0xB5, 0xE3, 0x83, 0x96 };
    if (has_seq(b, ACCENT)) return "accent";
    if (has_seq(b, SUB)) return "sub";
    if (has_seq(b, MAIN)) return "main";
    return "other";
}
struct MeshEntry { uint64_t addr; std::string kind; std::string name; uint32_t c0, c1; };
// M.locate_holder: holder+0x10 -> res; res+0xA0 entries (stride 0x50), res+0xD8 count; entry+0x18 -> type attribute
// (0xFB at +0x08 = GPU mesh), entry+0x28 -> name; attribute +0x40 / +0x44 = the two colours
std::vector<MeshEntry> locate_holder(MO* holder) {
    std::vector<MeshEntry> out;
    uint64_t res = 0, arr = 0;
    uint32_t count = 0;
    if (!scene::rd64(scene::addr(holder) + 0x10, res) || !res) return out;
    if (!scene::rd32(res + 0xD8, count) || !scene::rd64(res + 0xA0, arr)) return out;
    if (!arr || count == 0 || count > 64) return out;
    for (uint32_t e = 0; e < count; ++e) {
        const uint64_t ent = arr + (uint64_t)e * 0x50;
        uint64_t attr = 0;
        uint32_t type = 0;
        if (!scene::rd64(ent + 0x18, attr)) break;
        if (!attr) continue;
        if (!scene::rd32(attr + 0x08, type)) break;
        if (type != kTypeGpuMesh) continue;
        uint64_t nameptr = 0;
        if (!scene::rd64(ent + 0x28, nameptr)) break;
        const std::vector<uint8_t> bytes = nameptr ? scene::cstr(nameptr, 96) : std::vector<uint8_t>{};
        uint32_t c0 = 0, c1 = 0;
        if (!scene::rd32(attr + 0x40, c0) || !scene::rd32(attr + 0x44, c1)) break;
        out.push_back(MeshEntry{ attr + 0x40, classify(bytes), std::string(bytes.begin(), bytes.end()), c0, c1 });
    }
    return out;
}

// ---- provider snapshot -------------------------------------------------------------------------------------------
// What the per-function walks used to read of a provider (each_extern, each_element, element_has_color, each_resource,
// locate_element: up to five walks of the same lists per call, and one more per part name in collect_part), read once
// in each_element order: every element, its extern parameters (name, resource name) and, with res, its resource
// holders (path, locate_holder). The functions below compute from it what they computed from their own walks, in the
// same order; the raw reads (colours, type pointers, ColorEnable) stay where they were, when the result is built.
// The static pass takes it one element per budget check (snap_step with a deadline); every other caller in one go.
struct SnapExt { MO* param; int pi; std::string name, rn; uint64_t ti = 0; };   // ti: its type pointer when read
struct SnapRes { std::string path; std::vector<MeshEntry> mesh; };
struct SnapEl { MO* el = nullptr; int idx = 0; bool color = false; uint64_t ti = 0; std::vector<SnapExt> ext; std::vector<SnapRes> res; };
// apply_provider writes into objects of its snapshot, which a walk spread over frames read earlier: only into an object
// that still carries the type pointer it had then (always true in the frame of the walk, as before)
bool same_object(MO* o, uint64_t ti) { uint64_t now = 0; return scene::rd64(scene::addr(o), now) && now == ti; }
struct Snap {
    bool res = true; bool done = false; bool gone = false; int restarts = 0; scene::WalkCursor cur; std::vector<SnapEl> els;
    bool part = false; SnapEl cur_el; int res_n = 0, res_i = 0; bool changed = false;   // an element read in pieces
};

int64_t qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
bool past(int64_t deadline) { return deadline && qpc() >= deadline; }

// deadline 0 = no budget (the whole walk in this call). true when the snapshot is complete. A walk spread over frames
// first checks that the provider is still a live object (s.gone: the caller then leaves that provider alone).
bool snap_step(MO* go, Snap& s, int64_t deadline) {
    if (s.done) return true;
    perf::Scope timed(perf::ProvSnap);
    if (deadline && !reframework::API::get()->sdk()->managed_object->is_managed_object(go)) {
        s.els.clear(); s.gone = true; s.done = true;
        return true;
    }
    // An element: its parameters at once, then its resource holders one at a time (one holder's entries can take most
    // of a budget), the element given again by the walk until its last holder is read.
    const int r = scene::walk_elements_ex(go, s.cur, [&](MO* el, int idx) -> int {
        if (!s.part) {
            SnapEl e;
            e.el = el;
            e.idx = idx;
            scene::rd64(scene::addr(el), e.ti);
            scene::each_param(el, [&](MO* param, int pi, const std::string& name, const std::string& rn) {
                if (name == "Color" || name == "SubColor" || name == "AColor") e.color = true;   // element_has_color
                SnapExt x{ param, pi, name, rn };
                scene::rd64(scene::addr(param), x.ti);
                e.ext.push_back(std::move(x));
            });
            s.cur_el = std::move(e);
            s.part = true;
            s.res_i = 0;
            s.res_n = s.res ? scene::resource_count(el) : 0;
            if (s.res_n > 0 && past(deadline)) return 2;
        } else if (s.res && scene::resource_count(el) != s.res_n) {   // its holders changed meanwhile: start over
            s.changed = true;
            return 0;
        }
        while (s.res_i < s.res_n) {                       // each_resource's loop
            MO* h = nullptr;
            std::string path;
            if (scene::resource_at(el, s.res_i, h, path)) s.cur_el.res.push_back(SnapRes{ path, locate_holder(h) });
            ++s.res_i;
            if (s.res_i < s.res_n && past(deadline)) return 2;
        }
        s.els.push_back(std::move(s.cur_el));
        s.cur_el = SnapEl{};
        s.part = false;
        return past(deadline) ? 0 : 1;
    });
    if (r == 1 && !s.changed) { s.done = true; return true; }
    if (s.changed) {                                     // (as a change seen by the walk)
        const bool res = s.res;
        const int tries = s.restarts + 1;
        s = Snap{};
        s.res = res;
        s.restarts = tries;
        if (tries > 3) { snap_step(go, s, 0); return true; }
        return false;
    }
    if (r < 0) {                                         // the provider's lists changed under the walk: start over
        const bool res = s.res;
        const int tries = s.restarts + 1;
        s = Snap{};
        s.res = res;
        s.restarts = tries;
        if (tries > 3) { snap_step(go, s, 0); return true; }   // then in one call, as before (one call cannot see a change)
    }
    return false;
}
Snap snap_now(MO* go, bool res) { Snap s; s.res = res; snap_step(go, s, 0); return s; }

// ---- provider state ----------------------------------------------------------------------------------------------
// The state of a provider, fresh when it moved. is_target is decided where it is needed (decide_target / target_state),
// from the same extern walk as before.
PState& provider_state(const Provider& p) {
    auto it = g_pstate.find(p.key);
    if (it == g_pstate.end() || it->second.addr != p.addr) {
        PState fresh;
        fresh.addr = p.addr;
        it = g_pstate.insert_or_assign(p.key, std::move(fresh)).first;
    }
    return it->second;
}
bool drive_rn(const std::string& rn) { return scene::lower(rn).find("efd_10_drive") != std::string::npos; }
void decide_target(const Provider& p, PState& st, const Snap& s) {
    if (st.is_target >= 0) return;
    if (p.hit) { st.is_target = 1; return; }
    bool has_drive = false;
    for (auto& e : s.els) for (auto& x : e.ext) if (drive_rn(x.rn)) has_drive = true;
    st.is_target = has_drive ? 1 : 0;
}
PState& target_state(const Provider& p) {               // the old provider_state: is_target decided now when unknown
    PState& st = provider_state(p);
    if (st.is_target < 0) {
        if (p.hit) st.is_target = 1;
        else decide_target(p, st, snap_now(p.go, false));
    }
    return st;
}

const std::map<int, bool>& mark_drive_elements(const Provider& p, PState& st, const Snap& s) {
    if (st.drive_el) return *st.drive_el;
    std::map<int, bool> marks;
    for (auto& e : s.els) {
        bool drive = p.hit || e.color;
        if (!drive) for (auto& r : e.res) if (scene::lower(r.path).find("efd_10_drive") != std::string::npos) drive = true;
        if (!drive) for (auto& r : e.res) if (!r.mesh.empty()) { drive = true; break; }   // locate_element non-empty
        marks[e.idx] = drive;
    }
    if (!p.hit) for (auto& e : s.els) for (auto& x : e.ext) if (drive_rn(x.rn)) marks[e.idx] = true;
    st.drive_el = std::move(marks);
    return *st.drive_el;
}
bool marked(const std::map<int, bool>& m, int idx) { auto it = m.find(idx); return it != m.end() && it->second; }

Originals collect_originals(const Provider& p, PState& st, const Snap& s) {   // the caps of capture_originals / apply_provider
    const auto& marks = mark_drive_elements(p, st, s);
    Originals caps;
    for (auto& e : s.els) for (auto& x : e.ext) {
        bool c2;
        if (slot_of(x.name, c2) && marked(marks, e.idx)) {
            if (auto v = param_rgba(x.param)) caps.externs[{ e.idx, x.pi }] = *v; else caps.externs.erase({ e.idx, x.pi });
        }
    }
    for (auto& e : s.els) {
        if (!marked(marks, e.idx)) continue;                        // a character's own effect: never touched
        const uint64_t ea = scene::addr(e.el);
        uint32_t rgba = 0;
        uint8_t en = 0;
        scene::rd32(ea + scene::off_tint(), rgba);
        scene::rd8(ea + scene::off_enable(), en);
        if (!e.color) caps.tints[e.idx] = Tint{ en, rgba, false };
        else if (rgba != 0xFFFFFFFF) caps.tints[e.idx] = Tint{ en, rgba, true };
    }
    return caps;
}
void capture_originals(const Provider& p, PState& st, const Snap& s) {
    if (st.originals) return;
    st.originals = collect_originals(p, st, s);
}

std::vector<Mesh> mesh_collect(const Snap& s) {
    std::vector<Mesh> list;
    for (auto& e : s.els)
        for (auto& r : e.res)
            for (auto& m : r.mesh) list.push_back(Mesh{ m.addr, m.kind, m.name, r.path, e.idx, m.c0, m.c1 });
    return list;
}
std::string mesh_key(const Mesh& m) { return m.path + "#" + std::to_string(m.el) + "#" + m.name; }
void refresh_mesh(PState& st, const Snap& s) {
    std::vector<Mesh> fresh = mesh_collect(s);
    for (auto& m : fresh) {
        const std::string k = mesh_key(m);
        auto it = st.mesh_orig.find(k);
        if (it == st.mesh_orig.end()) it = st.mesh_orig.emplace(k, std::make_pair(m.o0, m.o1)).first;
        m.o0 = it->second.first; m.o1 = it->second.second;
    }
    st.mesh = std::move(fresh);
    st.has_mesh = true;
}

// apply_provider: the static path. The colours themselves go through the relay; this only sets ColorEnable (the relay
// carries dwords only) or, when the side was switched off, puts the originals back.
// apply_provider's checks before its walk (false = it returns false without reading anything). Nothing to set and
// nothing to put back: returned false after deciding is_target, which is now decided where it is used (the static
// pass, container), from the same walk.
bool apply_needs_walk(const Provider& p, const StaticCfg& cfg) {
    PState& st = provider_state(p);
    if (!cfg.enabled && !(st.written && st.originals)) return false;
    return st.is_target != 0;
}
bool apply_from(const Provider& p, const StaticCfg& cfg, bool in_fight, QueueFn queue, const Snap& s);
bool apply_provider(const Provider& p, const StaticCfg& cfg, bool in_fight, QueueFn queue) {
    if (!apply_needs_walk(p, cfg)) return false;
    return apply_from(p, cfg, in_fight, queue, snap_now(p.go, cfg.enabled || in_fight));   // resources: marks / mesh (enabled), baked colours back
}
// apply_provider after its walk: the same checks again (they are what apply_provider sees at the moment it writes)
bool apply_from(const Provider& p, const StaticCfg& cfg, bool in_fight, QueueFn queue, const Snap& s) {
    if (s.gone || !apply_needs_walk(p, cfg)) return false;
    PState& st = provider_state(p);
    decide_target(p, st, s);
    if (st.is_target != 1) return false;
    if (cfg.enabled) {
        refresh_mesh(st, s);
        if (!st.originals) {
            Originals caps = collect_originals(p, st, s);
            if (caps.externs.empty() && caps.tints.empty()) return false;
            st.originals = std::move(caps);
        }
        for (auto& e : s.els) if (st.originals->tints.count(e.idx) && same_object(e.el, e.ti)) scene::wr8(scene::addr(e.el) + scene::off_enable(), 1);
        st.written = true;
        return true;
    }
    for (auto& e : s.els) for (auto& x : e.ext) {
        auto it = st.originals->externs.find({ e.idx, x.pi });
        if (it != st.originals->externs.end() && same_object(x.param, x.ti)) scene::wr32(scene::addr(x.param) + kExternColorOff, it->second);   // set_Color(original)
    }
    for (auto& e : s.els) {
        auto it = st.originals->tints.find(e.idx);
        if (it == st.originals->tints.end() || !same_object(e.el, e.ti)) continue;
        scene::wr8(scene::addr(e.el) + scene::off_enable(), it->second.enable);
        scene::wr32(scene::addr(e.el) + scene::off_tint(), it->second.rgba);
    }
    if (in_fight) for (auto& m : mesh_collect(s)) {                     // baked colours back (fresh addresses)
        auto o = st.mesh_orig.find(mesh_key(m));
        if (o == st.mesh_orig.end()) continue;
        queue.fn(queue.ctx, m.addr, o->second.first);
        queue.fn(queue.ctx, m.addr + 4, o->second.second);
    }
    st.written = false;
    return true;
}

// ---- scan --------------------------------------------------------------------------------------------------------
// also notes the armour rim's entry (epve_cmn_000_00), so armor_params does not walk DataInstanceMap again
std::map<int, std::vector<Provider>> scan_providers() {
    perf::Scope timed(perf::ProvScan);
    std::map<int, std::vector<Provider>> found;
    ++g_found_gen;
    g_armor_seen = false;
    g_armor_go = nullptr;
    for (auto& e : scene::entries()) {
        if (!g_armor_seen && e.key.find("epve_cmn_000_00") != std::string::npos) { g_armor_seen = true; g_armor_go = e.go; }
        int fid = 0;
        std::string shortk;
        if (!parse_provider_path(e.key, fid, shortk)) continue;
        bool hit;
        if (is_hit_style(shortk)) hit = true;
        else if (is_effect_style(shortk)) hit = false;
        else continue;
        if (!e.go) continue;
        found[fid].push_back(Provider{ shortk, e.go, scene::addr(e.go), hit });
    }
    return found;
}

const std::vector<Provider>* list_of(int fighter) {
    if (fighter <= 0) return nullptr;
    auto it = g_found.find(fighter);
    return it == g_found.end() ? nullptr : &it->second;
}

std::string part_key(const Provider& p, int idx) { return p.key + ":" + std::to_string(idx); }

std::string element_efx(const Snap& s, int idx) {        // the efx a part plays (efd_10_drive_001_04 -> drive_001_04)
    for (auto& e : s.els) {                              // the first element of that index with an efx resource
        if (e.idx != idx) continue;
        for (auto& r : e.res) {
            std::string n;
            if (parse_efx_name(r.path, n)) {
                if (n.compare(0, 7, "efd_10_") == 0) n = n.substr(7);
                return n;
            }
        }
    }
    return {};
}

// one provider's lines of the parts list ("key\tname"), collect_parts' loop body
void collect_part(const Provider& p, std::string& out, const Snap& s) {
    PState& st = provider_state(p);
    decide_target(p, st, s);
    if (st.is_target != 1) return;
    capture_originals(p, st, s);
    refresh_mesh(st, s);
    const auto& marks = st.drive_el ? *st.drive_el : mark_drive_elements(p, st, s);
    std::map<int, bool> has_mesh;
    for (auto& m : st.mesh) has_mesh[m.el] = true;
    for (auto& e : s.els) {
        const int idx = e.idx;
        if (!marked(marks, idx)) continue;
        if (!(e.color || st.originals->tints.count(idx) || has_mesh.count(idx))) continue;
        std::string base;
        if (p.hit) base = "Hit";
        else { const int sn = parse_style_number(p.key); base = sn >= 0 ? "Style " + std::to_string(sn) : "Style ?"; }
        const std::string efx = element_efx(s, idx);
        std::string name = efx.empty() ? base + " - el " + std::to_string(idx) : base.substr(0, base.find(' ')) + " " + efx;
        out += part_key(p, idx) + "\t" + name + "\n";
    }
}

uint64_t parts_sig(const std::vector<Provider>& list) { uint64_t sig = 0; for (auto& p : list) sig += p.addr; return sig; }

// the armour rim (epve_cmn_000_00, shared): externs Color / Color2 whose resource is efd_10_armor_*. Its entry is the
// one the last provider scan saw (was: DataInstanceMap walked again at each call, 4 times per static pass).
bool armor_pending() {                                   // armor_params would walk the rim's externs now
    if (!g_armor_seen || !g_armor_go) return false;
    return !(g_armor.valid && g_armor.addr == scene::addr(g_armor_go));
}
bool armor_alive() { return reframework::API::get()->sdk()->managed_object->is_managed_object(g_armor_go); }
void armor_commit(const Snap& s) {
    g_armor.valid = true;
    g_armor.addr = scene::addr(g_armor_go);
    g_armor.params.clear();
    for (auto& e : s.els) for (auto& x : e.ext) {
        const bool c1 = x.name == "Color", c2 = x.name == "Color2";
        if (!(c1 || c2) || scene::lower(x.rn).find("efd_10_armor") == std::string::npos) continue;
        auto o = param_rgba(x.param);
        g_armor.params.push_back(ArmorParam{ scene::addr(x.param) + kExternColorOff, c2, o.value_or(0), o.has_value() });
    }
}
const decltype(g_armor)* armor_params() {
    if (!g_armor_seen || !g_armor_go) return nullptr;
    if (!armor_pending()) return &g_armor;
    if (!armor_alive()) return nullptr;                  // freed since the scan
    armor_commit(snap_now(g_armor_go, false));
    return &g_armor;
}

// one provider's relay lines (lines' loop body)
void provider_lines(const Provider& p, const MechColors& mc, std::vector<WLine>& out, const Snap& s) {
    const bool c1 = mc.has;
    const bool use_parts = c1 && !mc.follow && !mc.parts.empty();
    auto el_col = [&](int idx, uint32_t& a, bool& has_b, uint32_t& b) {
        if (use_parts) {
            auto it = mc.parts.find(part_key(p, idx));
            if (it != mc.parts.end()) { a = it->second; b = it->second; has_b = true; return; }
        }
        a = mc.c1; b = mc.c2; has_b = mc.has_c2;
    };
    PState& st = provider_state(p);
    decide_target(p, st, s);
    if (st.is_target != 1) return;
    capture_originals(p, st, s);
    const auto& marks = st.drive_el ? *st.drive_el : mark_drive_elements(p, st, s);
    for (auto& e : s.els) for (auto& x : e.ext) {
        const int el = e.idx;
        MO* param = x.param;
        bool slot_c2;
        if (!slot_of(x.name, slot_c2) || !marked(marks, el)) continue;
        auto o = st.originals->externs.find({ el, x.pi });
        std::optional<uint32_t> orig = (o != st.originals->externs.end()) ? std::optional<uint32_t>(o->second) : std::nullopt;
        std::optional<uint32_t> v;
        if (c1) {
            uint32_t a, b; bool hb;
            el_col(el, a, hb, b);
            if (!slot_c2) v = a; else if (hb) v = b;
        } else v = orig;
        if (!orig) orig = param_rgba(param);
        if (!v || !orig) continue;
        uint64_t ti = 0;
        scene::rd64(scene::addr(param), ti);
        out.push_back(WLine{ scene::addr(param) + kExternColorOff, *v, *orig, scene::addr(param), ti });
    }
    for (auto& e : s.els) {
        const int idx = e.idx;
        auto t = st.originals->tints.find(idx);
        if (t == st.originals->tints.end()) continue;
        uint32_t v = t->second.rgba;
        if (c1) {
            if (t->second.white) v = 0xFFFFFFFF;
            else { uint32_t a, b; bool hb; el_col(idx, a, hb, b); v = a; }
        }
        uint64_t ti = 0;
        scene::rd64(scene::addr(e.el), ti);
        out.push_back(WLine{ scene::addr(e.el) + scene::off_tint(), v, t->second.rgba, scene::addr(e.el), ti });
    }
    refresh_mesh(st, s);                                                // baked colours (relocated here, Follow too)
    for (auto& m : st.mesh) {
        std::optional<uint32_t> v;
        if (!marked(marks, m.el)) v = std::nullopt;
        else if (c1) {
            uint32_t a, b; bool hb;
            el_col(m.el, a, hb, b);
            const bool c1slot = (m.kind == "main" || m.kind == "other");   // MESH_SLOT: accent / sub -> c2
            if (c1slot) v = a; else if (hb) v = b;
        } else v = m.o0;
        if (!v) continue;
        out.push_back(WLine{ m.addr, *v, m.o0 });
        out.push_back(WLine{ m.addr + 4, c1 ? *v : m.o1, m.o1 });
    }
}

void armor_lines(const MechColors& mc, std::vector<WLine>& out) {     // the armour rim: outside the fighter's providers
    const bool c1 = mc.has;
    if (const auto* armor = armor_params()) {
        for (auto& pr : armor->params) {
            // Lua: v = c1 and col[pr.slot] or pr.orig (a missing Color 2 falls back to the original)
            std::optional<uint32_t> v;
            if (c1 && !pr.c2) v = mc.c1;
            else if (c1 && mc.has_c2) v = mc.c2;
            else if (pr.has_orig) v = pr.orig;
            if (v && pr.has_orig) out.push_back(WLine{ pr.addr, *v, pr.orig });
        }
    }
}

}  // namespace

struct SnapJob { Snap snap; };                           // LinesCursor's walk in progress

void reset() {
    ++g_found_gen;
    g_armor_seen = false; g_armor_go = nullptr;
    g_found.clear(); g_pstate.clear(); g_side_map.clear(); g_parts_seen.clear(); g_parts.clear();
    g_armor.valid = false; g_armor.params.clear();
}

// tick spread over frames: the scan and the list of providers to set (same conditions as before, taken right after
// the scan), then one provider at a time, its walk within the deadline, set as soon as its walk is complete.
struct TickJob {
    bool active = false, in_fight = false;
    std::vector<std::pair<Provider, StaticCfg>> todo;
    size_t i = 0;
    uint64_t gen = 0;
    std::shared_ptr<SnapJob> job;
};
TickJob g_tick;

bool still_found(const Provider& p) {                                   // the same provider object in the last scan
    for (auto& [id, list] : g_found) for (auto& q : list) if (q.key == p.key && q.addr == p.addr) return true;
    return false;
}

void tick_start(const StaticCfg statics[2], const int side_chars[2], bool dirty, bool in_fight) {
    perf::Scope timed(perf::ProvTick);
    g_found = scan_providers();
    g_tick = TickJob{};
    g_tick.in_fight = in_fight;
    g_tick.gen = g_found_gen;
    for (auto& [id, list] : g_found) {
        std::vector<int> sides;                                         // sides_of(id)
        for (int s = 0; s < 2; ++s) if (side_chars[s] == id) sides.push_back(s + 1);
        if (sides.empty()) sides.push_back(1);
        auto prev = g_side_map.find(id);
        if (prev == g_side_map.end() || prev->second.size() != sides.size() || prev->second[0] != sides[0]) dirty = true;
        g_side_map[id] = sides;
        const StaticCfg& cfg = statics[sides[0] - 1];
        if (cfg.fighter != id) continue;                                // the Lua pushed no setting for this fighter
        if (sides.size() == 2) continue;                                // mirror: the relay writes at each DI start
        if (cfg.enabled && cfg.follow) continue;                        // Follow: the relay reads the worn colour
        for (auto& p : list) {
            auto it = g_pstate.find(p.key);
            const bool fresh = it == g_pstate.end() || it->second.addr != p.addr;
            if (dirty || fresh || (it->second.is_target == 1 && it->second.written != cfg.enabled))
                g_tick.todo.emplace_back(p, cfg);                       // (only apply_provider(p) changes what p's test reads)
        }
    }
    g_tick.active = !g_tick.todo.empty();
}

bool tick_busy() { return g_tick.active; }

int tick_step(int64_t deadline, QueueFn queue) {
    perf::Scope timed(perf::ProvTick);
    int applied = 0;
    while (g_tick.active && g_tick.i < g_tick.todo.size()) {
        if (g_tick.gen != g_found_gen) {                                // scanned again: keep the providers still there, unmoved
            std::vector<std::pair<Provider, StaticCfg>> keep;
            for (size_t k = g_tick.i; k < g_tick.todo.size(); ++k) {
                if (still_found(g_tick.todo[k].first)) keep.push_back(g_tick.todo[k]);
                else if (k == g_tick.i) g_tick.job.reset();
            }
            g_tick.todo = std::move(keep);
            g_tick.i = 0;
            g_tick.gen = g_found_gen;
            continue;
        }
        const Provider p = g_tick.todo[g_tick.i].first;
        const StaticCfg cfg = g_tick.todo[g_tick.i].second;
        if (!g_tick.job) {
            if (!apply_needs_walk(p, cfg)) { ++g_tick.i; if (past(deadline)) break; continue; }
            g_tick.job = std::make_shared<SnapJob>();
            g_tick.job->snap.res = cfg.enabled || g_tick.in_fight;
        }
        if (!snap_step(p.go, g_tick.job->snap, deadline)) return applied;
        if (!g_tick.job->snap.gone && past(deadline)) return applied;   // complete: set at the next call (fresh budget)
        {
            perf::Scope b(perf::ProvBuild);
            const bool alive = !deadline || reframework::API::get()->sdk()->managed_object->is_managed_object(p.go);
            if (alive && apply_from(p, cfg, g_tick.in_fight, queue, g_tick.job->snap)) ++applied;
        }
        g_tick.job.reset();
        ++g_tick.i;
        if (past(deadline)) break;
    }
    if (g_tick.i >= g_tick.todo.size()) g_tick = TickJob{};
    return applied;
}

int tick(const StaticCfg statics[2], const int side_chars[2], bool dirty, bool in_fight, QueueFn queue) {
    tick_start(statics, side_chars, dirty, in_fight);
    int applied = 0;
    while (tick_busy()) applied += tick_step(0, queue);
    return applied;
}

void scan() { g_found = scan_providers(); }

bool lines_step(int fighter, const MechColors& mc, std::vector<WLine>& out, LinesCursor& c, int64_t deadline) {
    if (c.stage != 0 && c.gen != g_found_gen) { c = LinesCursor{}; out.clear(); }   // scanned again: start over
    const auto* list = list_of(fighter);
    if (c.stage == 0) {
        c.gen = g_found_gen;
        if (!list) { c.present = false; c.stage = 4; return true; }
        // collect_parts: the parts list again only when the fighter's providers moved
        c.sig = parts_sig(*list);
        auto seen = g_parts_seen.find(fighter);
        c.stage = (seen != g_parts_seen.end() && seen->second == c.sig) ? 2 : 1;
        c.idx = 0;
        c.parts.clear();
        c.job.reset();
        return false;
    }
    if (!list) { c = LinesCursor{}; out.clear(); return false; }
    // stages 1 and 2: one provider at a time, its snapshot taken within the deadline (it may need several calls)
    // (a provider known not to be a Drive one reads nothing, as before: provider_state, then return)
    auto provider_snap = [&](const Provider& p) -> const Snap* {
        if (!c.job) c.job = std::make_shared<SnapJob>();
        if (!snap_step(p.go, c.job->snap, deadline)) return nullptr;
        if (!c.job->snap.gone && past(deadline)) return nullptr;        // complete: its result at the next call (fresh budget)
        return &c.job->snap;
    };
    if (c.stage == 1) {
        if (c.idx < list->size()) {
            const Provider& p = (*list)[c.idx];
            const bool skip = !c.job && provider_state(p).is_target == 0;
            if (!skip) {
                const Snap* s = provider_snap(p);
                if (!s) return false;
                perf::Scope b(perf::ProvBuild);
                if (!s->gone) collect_part(p, c.parts, *s);             // freed meanwhile: nothing read from it
                c.job.reset();
            }
            ++c.idx;
            return false;
        }
        g_parts_seen[fighter] = c.sig;                                 // the list is complete: kept with its signature
        g_parts[fighter] = std::move(c.parts);
        c.parts.clear();
        c.stage = 2; c.idx = 0;
        return false;
    }
    if (c.stage == 2) {
        if (c.idx < list->size()) {
            const Provider& p = (*list)[c.idx];
            const bool skip = !c.job && provider_state(p).is_target == 0;
            if (!skip) {
                const Snap* s = provider_snap(p);
                if (!s) return false;
                perf::Scope b(perf::ProvBuild);
                if (!s->gone) provider_lines(p, mc, out, *s);
                c.job.reset();
            }
            ++c.idx;
            return false;
        }
        c.stage = 3;
        return false;
    }
    if (c.stage == 3) {
        if (armor_pending()) {                                          // the rim's externs, within the deadline too
            if (!armor_alive()) c.job.reset();                          // freed since the scan: armor_params says so
            else {
                if (!c.job) { c.job = std::make_shared<SnapJob>(); c.job->snap.res = false; }
                if (!snap_step(g_armor_go, c.job->snap, deadline)) return false;
                armor_commit(c.job->snap);
                c.job.reset();
            }
        }
        armor_lines(mc, out);
        c.present = true;
        c.stage = 4;
    }
    return true;
}

bool lines(int fighter, const MechColors& mc, std::vector<WLine>& out) {
    LinesCursor c;
    while (!lines_step(fighter, mc, out, c)) {}
    return c.present;
}

bool container_step(int fighter, LinesCursor& c, int64_t deadline) {
    const auto* list = list_of(fighter);
    if (!list) return true;
    if (c.gen != g_found_gen) { c = LinesCursor{}; c.gen = g_found_gen; }   // first call, or scanned again
    while (c.idx < list->size()) {
        const Provider& p = (*list)[c.idx];
        if (p.hit) { ++c.idx; continue; }                               // container skips the hit provider
        if (provider_state(p).is_target < 0) {                          // its extern walk, within the deadline
            if (!c.job) { c.job = std::make_shared<SnapJob>(); c.job->snap.res = false; }
            if (!snap_step(p.go, c.job->snap, deadline)) return false;
            if (!c.job->snap.gone) decide_target(p, provider_state(p), c.job->snap);
            c.job.reset();
        }
        if (provider_state(p).is_target == 1 && parse_style_number(p.key) >= 0) return true;   // where container stops
        ++c.idx;
        if (past(deadline)) return false;
    }
    return true;
}

int container(int fighter) {
    const auto* list = list_of(fighter);
    if (!list) return -1;
    for (auto& p : *list) {
        if (p.hit) continue;
        if (target_state(p).is_target != 1) continue;
        const int sn = parse_style_number(p.key);
        if (sn >= 0) return sn;
    }
    return -1;
}

std::string key(int fighter, const MechColors& mc) {
    const auto* list = list_of(fighter);
    if (!list) return "-";
    uint64_t sum = 0, nmesh = 0;
    for (auto& p : *list) {
        sum += p.addr;
        auto it = g_pstate.find(p.key);
        if (it != g_pstate.end() && it->second.has_mesh) nmesh += it->second.mesh.size();
    }
    char b[160];
    std::snprintf(b, sizeof b, "%d/%s%08X/%s%08X/%llX/%llu/%d", fighter, mc.has ? "" : "nil", mc.c1, mc.has_c2 ? "" : "nil", mc.c2,
                  (unsigned long long)(sum & 0xFFFFFFF), (unsigned long long)nmesh, mc.follow ? 1 : 0);
    std::string k = b;
    for (auto& [pk, c] : mc.parts) { char pb[16]; std::snprintf(pb, sizeof pb, "=%08X", c); k += "|" + pk + pb; }
    return k;
}

std::string parts(int fighter) {
    auto it = g_parts.find(fighter);
    return it == g_parts.end() ? std::string() : "ok\n" + it->second;   // "ok" = computed (an empty list is a list)
}

std::string status() {
    std::string o;
    for (auto& [id, list] : g_found) {
        int targets = 0;
        for (auto& p : list) { auto it = g_pstate.find(p.key); if (it != g_pstate.end() && it->second.is_target == 1) ++targets; }
        auto sm = g_side_map.find(id);
        o += (o.empty() ? "" : ", ") + std::to_string(id) + ":" + std::to_string(list.size()) + "p/" + std::to_string(targets) + "t" +
             ((sm != g_side_map.end() && sm->second.size() == 2) ? "(mirror)" : "");
    }
    return o.empty() ? "no provider" : o;
}

}  // namespace vfx::providers
