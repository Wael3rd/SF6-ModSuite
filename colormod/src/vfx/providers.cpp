#include "providers.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <regex>
#include <unordered_map>

#include "scene.hpp"

namespace vfx::providers {
namespace {

using scene::MO;

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
struct HolderMesh { std::string path; std::vector<MeshEntry> entries; };
std::vector<HolderMesh> locate_element(MO* el) {         // M.locate_element: holders with at least one mesh entry
    std::vector<HolderMesh> list;
    scene::each_resource(el, [&](MO* h, const std::string& path) {
        auto e = locate_holder(h);
        if (!e.empty()) list.push_back(HolderMesh{ path, std::move(e) });
    });
    return list;
}

// ---- provider state ----------------------------------------------------------------------------------------------
PState& provider_state(const Provider& p) {
    auto it = g_pstate.find(p.key);
    if (it == g_pstate.end() || it->second.addr != p.addr) {
        PState fresh;
        fresh.addr = p.addr;
        it = g_pstate.insert_or_assign(p.key, std::move(fresh)).first;
    }
    PState& st = it->second;
    if (st.is_target < 0) {
        if (p.hit) st.is_target = 1;
        else {
            bool has_drive = false;
            scene::each_extern(p.go, [&](MO*, int, int, const std::string&, const std::string& rn) {
                if (scene::lower(rn).find("efd_10_drive") != std::string::npos) has_drive = true;
            });
            st.is_target = has_drive ? 1 : 0;
        }
    }
    return st;
}

const std::map<int, bool>& mark_drive_elements(const Provider& p, PState& st) {
    if (st.drive_el) return *st.drive_el;
    std::map<int, bool> marks;
    scene::each_element(p.go, [&](MO* el, int idx) {
        bool drive = p.hit || scene::element_has_color(el);
        if (!drive) scene::each_resource(el, [&](MO*, const std::string& path) {
            if (scene::lower(path).find("efd_10_drive") != std::string::npos) drive = true;
        });
        if (!drive && !locate_element(el).empty()) drive = true;
        marks[idx] = drive;
    });
    if (!p.hit) scene::each_extern(p.go, [&](MO*, int el, int, const std::string&, const std::string& rn) {
        if (scene::lower(rn).find("efd_10_drive") != std::string::npos) marks[el] = true;
    });
    st.drive_el = std::move(marks);
    return *st.drive_el;
}
bool marked(const std::map<int, bool>& m, int idx) { auto it = m.find(idx); return it != m.end() && it->second; }

Originals collect_originals(const Provider& p, PState& st) {   // the caps of capture_originals / apply_provider
    const auto& marks = mark_drive_elements(p, st);
    Originals caps;
    scene::each_extern(p.go, [&](MO* param, int el, int pi, const std::string& name, const std::string&) {
        bool c2;
        if (slot_of(name, c2) && marked(marks, el)) {
            if (auto v = param_rgba(param)) caps.externs[{ el, pi }] = *v; else caps.externs.erase({ el, pi });
        }
    });
    scene::each_element(p.go, [&](MO* el, int idx) {
        if (!marked(marks, idx)) return;                           // a character's own effect: never touched
        const uint64_t ea = scene::addr(el);
        uint32_t rgba = 0;
        uint8_t en = 0;
        scene::rd32(ea + scene::off_tint(), rgba);
        scene::rd8(ea + scene::off_enable(), en);
        if (!scene::element_has_color(el)) caps.tints[idx] = Tint{ en, rgba, false };
        else if (rgba != 0xFFFFFFFF) caps.tints[idx] = Tint{ en, rgba, true };
    });
    return caps;
}
void capture_originals(const Provider& p, PState& st) {
    if (st.originals) return;
    st.originals = collect_originals(p, st);
}

std::vector<Mesh> mesh_collect(const Provider& p) {
    std::vector<Mesh> list;
    scene::each_element(p.go, [&](MO* el, int idx) {
        for (auto& r : locate_element(el))
            for (auto& e : r.entries) list.push_back(Mesh{ e.addr, e.kind, e.name, r.path, idx, e.c0, e.c1 });
    });
    return list;
}
std::string mesh_key(const Mesh& m) { return m.path + "#" + std::to_string(m.el) + "#" + m.name; }
void refresh_mesh(const Provider& p, PState& st) {
    std::vector<Mesh> fresh = mesh_collect(p);
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
bool apply_provider(const Provider& p, const StaticCfg& cfg, bool in_fight, QueueFn queue) {
    PState& st = provider_state(p);
    if (st.is_target != 1) return false;
    if (cfg.enabled) {
        refresh_mesh(p, st);
        if (!st.originals) {
            Originals caps = collect_originals(p, st);
            if (caps.externs.empty() && caps.tints.empty()) return false;
            st.originals = std::move(caps);
        }
        scene::each_element(p.go, [&](MO* el, int idx) {
            if (st.originals->tints.count(idx)) scene::wr8(scene::addr(el) + scene::off_enable(), 1);
        });
        st.written = true;
        return true;
    }
    if (st.written && st.originals) {
        scene::each_extern(p.go, [&](MO* param, int el, int pi, const std::string&, const std::string&) {
            auto it = st.originals->externs.find({ el, pi });
            if (it != st.originals->externs.end()) scene::wr32(scene::addr(param) + kExternColorOff, it->second);   // set_Color(original)
        });
        scene::each_element(p.go, [&](MO* el, int idx) {
            auto it = st.originals->tints.find(idx);
            if (it == st.originals->tints.end()) return;
            scene::wr8(scene::addr(el) + scene::off_enable(), it->second.enable);
            scene::wr32(scene::addr(el) + scene::off_tint(), it->second.rgba);
        });
        if (in_fight) for (auto& m : mesh_collect(p)) {                 // baked colours back (fresh addresses)
            auto o = st.mesh_orig.find(mesh_key(m));
            if (o == st.mesh_orig.end()) continue;
            queue.fn(queue.ctx, m.addr, o->second.first);
            queue.fn(queue.ctx, m.addr + 4, o->second.second);
        }
        st.written = false;
        return true;
    }
    return false;
}

// ---- scan --------------------------------------------------------------------------------------------------------
std::map<int, std::vector<Provider>> scan_providers() {
    static const std::regex kPath("/esf(\\d\\d\\d)/EffectProvider/([^/]+)\\.pfb$");
    static const std::regex kHit("^epve_esf\\d\\d\\d_hit_style$");
    static const std::regex kStyle("^epvs_esf\\d\\d\\d_style_");
    std::map<int, std::vector<Provider>> found;
    for (auto& e : scene::entries()) {
        std::smatch m;
        if (!std::regex_search(e.key, m, kPath)) continue;
        const std::string shortk = m[2].str();
        bool hit;
        if (std::regex_search(shortk, kHit)) hit = true;
        else if (std::regex_search(shortk, kStyle)) hit = false;
        else continue;
        if (!e.go) continue;
        found[std::atoi(m[1].str().c_str())].push_back(Provider{ shortk, e.go, scene::addr(e.go), hit });
    }
    return found;
}

const std::vector<Provider>* list_of(int fighter) {
    if (fighter <= 0) return nullptr;
    auto it = g_found.find(fighter);
    return it == g_found.end() ? nullptr : &it->second;
}

std::string part_key(const Provider& p, int idx) { return p.key + ":" + std::to_string(idx); }

std::string element_efx(const Provider& p, int idx) {    // the efx a part plays (efd_10_drive_001_04 -> drive_001_04)
    static const std::regex kEfx("([^/]+)\\.efx$");
    std::string name;
    bool done = false;
    scene::each_element(p.go, [&](MO* el, int i) {
        if (i != idx || done) return;
        scene::each_resource(el, [&](MO*, const std::string& path) {
            if (done) return;
            std::smatch m;
            if (std::regex_search(path, m, kEfx)) {
                name = m[1].str();
                if (name.rfind("efd_10_", 0) == 0) name = name.substr(7);
                done = true;
            }
        });
    });
    return name;
}

void collect_parts(int id, const std::vector<Provider>& list) {
    uint64_t sig = 0;
    for (auto& p : list) sig += p.addr;
    auto seen = g_parts_seen.find(id);
    if (seen != g_parts_seen.end() && seen->second == sig) return;
    g_parts_seen[id] = sig;
    static const std::regex kStyleN("_style_(\\d+)$");
    std::string out;
    for (auto& p : list) {
        PState& st = provider_state(p);
        if (st.is_target != 1) continue;
        capture_originals(p, st);
        refresh_mesh(p, st);
        const auto& marks = st.drive_el ? *st.drive_el : mark_drive_elements(p, st);
        std::map<int, bool> has_mesh;
        for (auto& m : st.mesh) has_mesh[m.el] = true;
        scene::each_element(p.go, [&](MO* el, int idx) {
            if (!marked(marks, idx)) return;
            if (!(scene::element_has_color(el) || st.originals->tints.count(idx) || has_mesh.count(idx))) return;
            std::smatch m;
            std::string base;
            if (p.hit) base = "Hit";
            else base = std::regex_search(p.key, m, kStyleN) ? "Style " + std::to_string(std::atoi(m[1].str().c_str())) : "Style ?";
            const std::string efx = element_efx(p, idx);
            std::string name = efx.empty() ? base + " - el " + std::to_string(idx) : base.substr(0, base.find(' ')) + " " + efx;
            out += part_key(p, idx) + "\t" + name + "\n";
        });
    }
    g_parts[id] = out;
}

// the armour rim (epve_cmn_000_00, shared): externs Color / Color2 whose resource is efd_10_armor_*
const decltype(g_armor)* armor_params() {
    for (auto& e : scene::entries()) {
        if (e.key.find("epve_cmn_000_00") == std::string::npos) continue;
        if (!e.go) return nullptr;
        const uint64_t a = scene::addr(e.go);
        if (g_armor.valid && g_armor.addr == a) return &g_armor;
        g_armor.valid = true;
        g_armor.addr = a;
        g_armor.params.clear();
        scene::each_extern(e.go, [&](MO* param, int, int, const std::string& name, const std::string& rn) {
            const bool c1 = name == "Color", c2 = name == "Color2";
            if (!(c1 || c2) || scene::lower(rn).find("efd_10_armor") == std::string::npos) return;
            auto o = param_rgba(param);
            g_armor.params.push_back(ArmorParam{ scene::addr(param) + kExternColorOff, c2, o.value_or(0), o.has_value() });
        });
        return &g_armor;
    }
    return nullptr;
}

}  // namespace

void reset() {
    g_found.clear(); g_pstate.clear(); g_side_map.clear(); g_parts_seen.clear(); g_parts.clear();
    g_armor.valid = false; g_armor.params.clear();
}

int tick(const StaticCfg statics[2], const int side_chars[2], bool dirty, bool in_fight, QueueFn queue) {
    g_found = scan_providers();
    int applied = 0;
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
                if (apply_provider(p, cfg, in_fight, queue)) ++applied;
        }
    }
    return applied;
}

void scan() { g_found = scan_providers(); }

bool lines(int fighter, const MechColors& mc, std::vector<WLine>& out) {
    const auto* list = list_of(fighter);
    if (!list) return false;
    collect_parts(fighter, *list);
    const bool c1 = mc.has;
    const bool use_parts = c1 && !mc.follow && !mc.parts.empty();
    auto el_col = [&](const Provider& p, int idx, uint32_t& a, bool& has_b, uint32_t& b) {
        if (use_parts) {
            auto it = mc.parts.find(part_key(p, idx));
            if (it != mc.parts.end()) { a = it->second; b = it->second; has_b = true; return; }
        }
        a = mc.c1; b = mc.c2; has_b = mc.has_c2;
    };
    for (auto& p : *list) {
        PState& st = provider_state(p);
        if (st.is_target != 1) continue;
        capture_originals(p, st);
        const auto& marks = st.drive_el ? *st.drive_el : mark_drive_elements(p, st);
        scene::each_extern(p.go, [&](MO* param, int el, int pi, const std::string& name, const std::string&) {
            bool slot_c2;
            if (!slot_of(name, slot_c2) || !marked(marks, el)) return;
            auto o = st.originals->externs.find({ el, pi });
            std::optional<uint32_t> orig = (o != st.originals->externs.end()) ? std::optional<uint32_t>(o->second) : std::nullopt;
            std::optional<uint32_t> v;
            if (c1) {
                uint32_t a, b; bool hb;
                el_col(p, el, a, hb, b);
                if (!slot_c2) v = a; else if (hb) v = b;
            } else v = orig;
            if (!orig) orig = param_rgba(param);
            if (!v || !orig) return;
            uint64_t ti = 0;
            scene::rd64(scene::addr(param), ti);
            out.push_back(WLine{ scene::addr(param) + kExternColorOff, *v, *orig, scene::addr(param), ti });
        });
        scene::each_element(p.go, [&](MO* el, int idx) {
            auto t = st.originals->tints.find(idx);
            if (t == st.originals->tints.end()) return;
            uint32_t v = t->second.rgba;
            if (c1) {
                if (t->second.white) v = 0xFFFFFFFF;
                else { uint32_t a, b; bool hb; el_col(p, idx, a, hb, b); v = a; }
            }
            uint64_t ti = 0;
            scene::rd64(scene::addr(el), ti);
            out.push_back(WLine{ scene::addr(el) + scene::off_tint(), v, t->second.rgba, scene::addr(el), ti });
        });
        refresh_mesh(p, st);                                            // baked colours (relocated here, Follow too)
        for (auto& m : st.mesh) {
            std::optional<uint32_t> v;
            if (!marked(marks, m.el)) v = std::nullopt;
            else if (c1) {
                uint32_t a, b; bool hb;
                el_col(p, m.el, a, hb, b);
                const bool c1slot = (m.kind == "main" || m.kind == "other");   // MESH_SLOT: accent / sub -> c2
                if (c1slot) v = a; else if (hb) v = b;
            } else v = m.o0;
            if (!v) continue;
            out.push_back(WLine{ m.addr, *v, m.o0 });
            out.push_back(WLine{ m.addr + 4, c1 ? *v : m.o1, m.o1 });
        }
    }
    if (const auto* armor = armor_params()) {                           // the armour rim: outside the fighter's providers
        for (auto& pr : armor->params) {
            // Lua: v = c1 and col[pr.slot] or pr.orig (a missing Color 2 falls back to the original)
            std::optional<uint32_t> v;
            if (c1 && !pr.c2) v = mc.c1;
            else if (c1 && mc.has_c2) v = mc.c2;
            else if (pr.has_orig) v = pr.orig;
            if (v && pr.has_orig) out.push_back(WLine{ pr.addr, *v, pr.orig });
        }
    }
    return true;
}

int container(int fighter) {
    static const std::regex kStyleN("_style_(\\d+)$");
    const auto* list = list_of(fighter);
    if (!list) return -1;
    for (auto& p : *list) {
        if (p.hit) continue;
        if (provider_state(p).is_target != 1) continue;
        std::smatch m;
        if (std::regex_search(p.key, m, kStyleN)) return std::atoi(m[1].str().c_str());
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
