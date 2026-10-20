// SF6 Slots — runtime costume options, the game side (rto.hpp, docs/RUNTIME_OPTIONS.md).
//
// A model is the Battle Settings preview (CaptureRoomFighter.FighterObj) or a fighter (PlayerBehavior,
// _PlayerID 0 / 1). Its slot is found from the paths its meshes load (the slot's folder), so the online
// alias (a slot shown over its carrier) needs nothing more. For the side's chosen options the manifest
// says what each target must be (rto_data: desired); a pass compares that with what the model's
// components load and swaps what differs, per model: via.render.Mesh setMesh / set_Material /
// setMaterialTexture, via.motion.Chain set_ChainAsset + restart (the probes of 19/10). Never the shared
// costume manifest (FighterVisualHolder._Setting: one serves P1 and P2).
//
// Loading: a resource that is not resident yet shows a body invisible (mesh) or black (material) for
// a second and more. Each resource is created ahead (its holder kept by this module) and given to a
// model only once it had time to load; then the model's own flags say whether it is drawable
// (MeshReady, MaterialReady + MaterialNum): checked in the same frame and again on the next one, a
// part that is not ready goes back at once to what it wore (a holder of that file, made before the
// swap) and is tried again later. Never shown half loaded.
//
// When: the preview while Battle Settings is up (polled every 10 frames, a menu screen); a fighter
// after an event (its model built: PlayerBehavior.SetupAppearance, or an outfit mounted for a fight):
// a watch of 10 s, then nothing until the next event.
#include "rto.hpp"
#include "slots_ui.hpp"

#include <algorithm>
#include <set>

namespace slots::rto {
namespace {

const char* kManifest = "reframework/data/SF6_Costumes_Data/costume_options.json";
const char* kOutfits  = "reframework/data/SF6_Costumes_Data/outfits.json";
const char* kChoices  = "reframework/data/SF6_CostumeSlots_data/costume_choices.json";

Manifest g_m;
Choices g_c;
bool g_on = false;
bool g_disabled = false;
int g_faults = 0;
uint32_t g_ver = 1;                 // choices version: a model re-applies when it changes
bool g_migrated = false;
std::set<const Slot*> g_no_rows;    // slots whose menu would be empty: no F hint, no menu

// ---- screens and events ----
bool g_select = false;              // character select open: nothing at all
bool g_bs = false;                  // Battle Settings opened, until it is closed (its sub-screens included)
bool g_bs_closed = false;           // the close event came: g_bs ends once Fighter Settings is not seen for 3 s
uint32_t g_bs_seen = 0;
bool g_fs_was = false;              // Fighter Settings visible on the last frame (log on change)
uint32_t g_watch_until = 0;         // fighters watched after an event, until this frame
uint32_t g_watch_next = 0;
uint32_t g_watch_start = 0;
bool g_model_noisy = false;         // SetupAppearance seen all the time: that event is ignored (no watch without end)
std::atomic<uint32_t> g_model_calls{0};
bool g_warm_done = false;
std::set<std::string> g_warm;       // files of the choices made in the menu, kept loaded
uint32_t g_gc_at = 0;

// ---- game types and methods ----
struct Types {
    bool ok = false, tried = false;
    TD *mesh = nullptr, *chain = nullptr, *pcc = nullptr, *crf = nullptr, *pb = nullptr, *go = nullptr, *tr = nullptr;
    Obj *t_mesh = nullptr, *t_chain = nullptr, *t_pcc = nullptr;
    Method *get_comp = nullptr, *go_tr = nullptr, *go_name = nullptr, *tr_child = nullptr, *tr_next = nullptr, *tr_go = nullptr;
    Method *get_mesh = nullptr, *get_mat = nullptr, *set_mesh = nullptr, *set_mat = nullptr, *mesh_ready = nullptr,
           *mat_ready = nullptr, *mat_num = nullptr, *tex_num = nullptr, *get_tex = nullptr, *set_tex = nullptr;
    Method *ch_get = nullptr, *ch_set = nullptr, *ch_restart = nullptr, *ch_enable = nullptr, *ch_enabled = nullptr;
    Method *set_color = nullptr;
    Method *crf_obj = nullptr, *crf_setup = nullptr, *pb_go = nullptr, *pb_applied = nullptr;
};
Types T;

bool types() {
    if (T.tried) return T.ok;
    T.tried = true;
    auto* tdb = api().tdb();
    T.mesh = tdb->find_type("via.render.Mesh");
    T.chain = tdb->find_type("via.motion.Chain");
    T.pcc = tdb->find_type("app.PlayerColorController");
    T.crf = tdb->find_type("app.worldtour.CaptureRoomFighter");
    T.pb = tdb->find_type("app.PlayerBehavior");
    T.go = tdb->find_type("via.GameObject");
    T.tr = tdb->find_type("via.Transform");
    if (!T.mesh || !T.chain || !T.pcc || !T.crf || !T.pb || !T.go || !T.tr) { logf("rto: game types missing, runtime options off"); return false; }
    T.t_mesh = api().typeof("via.render.Mesh");
    T.t_chain = api().typeof("via.motion.Chain");
    T.t_pcc = api().typeof("app.PlayerColorController");
    T.get_comp = method_of(T.go, "getComponent(System.Type)", 1);
    T.go_tr = method_of(T.go, "get_Transform", 0);
    T.go_name = method_of(T.go, "get_Name", 0);
    T.tr_child = method_of(T.tr, "get_Child", 0);
    T.tr_next = method_of(T.tr, "get_Next", 0);
    T.tr_go = method_of(T.tr, "get_GameObject", 0);
    T.get_mesh = method_of(T.mesh, "getMesh", 0);
    T.get_mat = method_of(T.mesh, "get_Material", 0);
    T.set_mesh = method_of(T.mesh, "setMesh", 1);
    T.set_mat = method_of(T.mesh, "set_Material", 1);
    T.mesh_ready = method_of(T.mesh, "get_MeshReady", 0);
    T.mat_ready = method_of(T.mesh, "get_MaterialReady", 0);
    T.mat_num = method_of(T.mesh, "get_MaterialNum", 0);
    T.tex_num = method_of(T.mesh, "getMaterialTextureNum", 1);
    T.get_tex = method_of(T.mesh, "getMaterialTexture", 2);
    T.set_tex = method_of(T.mesh, "setMaterialTexture", 3);
    T.ch_get = method_of(T.chain, "get_ChainAsset", 0);
    T.ch_set = method_of(T.chain, "set_ChainAsset", 1);
    T.ch_restart = method_of(T.chain, "restart", 0);
    T.ch_enable = method_of(T.chain, "set_Enabled", 1);       // via.Component: a part unit's swap (not required)
    T.ch_enabled = method_of(T.chain, "get_Enabled", 0);
    T.set_color = method_of(T.pcc, "SetColor", 1);
    T.crf_obj = method_of(T.crf, "get_FighterObj", 0);
    T.crf_setup = method_of(T.crf, "get_IsSetUpEnd", 0);
    T.pb_go = method_of(T.pb, "get_GameObject", 0);
    T.pb_applied = method_of(T.pb, "get_IsAppliedResource", 0);
    T.ok = T.t_mesh && T.t_chain && T.t_pcc && T.get_comp && T.go_tr && T.tr_child && T.tr_next && T.tr_go && T.get_mesh
        && T.get_mat && T.set_mesh && T.set_mat && T.mesh_ready && T.mat_ready && T.mat_num && T.tex_num && T.get_tex
        && T.set_tex && T.ch_get && T.ch_set && T.ch_restart && T.set_color && T.crf_obj && T.pb_go;
    if (!T.ok) logf("rto: a game method is missing, runtime options off");
    return T.ok;
}

// A kept game object is used only if it is still a live object of that type (or a type made from it)
bool alive_as(Obj* o, TD* td) {
    if (!o || !o->is_managed_object()) return false;
    for (auto* t = o->get_type_definition(); t; t = t->get_parent_type()) if (t == td) return true;
    return false;
}
bool ret_bool(Method* m, Obj* o) { auto r = call(m, o); return !r.exception_thrown && r.byte != 0; }
uint32_t ret_u32(Method* m, Obj* o, std::vector<void*> args = {}) { auto r = call(m, o, std::move(args)); return r.exception_thrown ? 0 : r.dword; }
std::string holder_path(Obj* h) {
    if (!h) return {};
    static std::unordered_map<TD*, Method*> cache;
    auto* td = h->get_type_definition();
    auto it = cache.find(td);
    Method* m = it != cache.end() ? it->second : (cache[td] = method_of(td, "get_ResourcePath", 0));
    return norm_path(str(call_obj(m, h)));
}
std::string short_path(const std::string& p) { auto k = p.find("/esf/"); return k == std::string::npos ? p : p.substr(k + 5); }

// ---------------------------------------------------------------------------------------------
// Resources: created ahead, holders kept by this module
// ---------------------------------------------------------------------------------------------
struct Res {
    std::string kind;
    Obj* holder = nullptr;
    uint32_t ready_at = 0;          // frame from which it may be given to a model
    uint32_t used = 0;
    int fails = 0;
    bool dead = false;
};
std::unordered_map<std::string, Res> g_res;

uint32_t load_frames(const std::string& kind) { return kind == "texture" ? 60 : 30; }

// resident: the model wears this file now (a way back), usable at once
Res* res_get(const std::string& path, const std::string& kind, bool resident) {
    auto it = g_res.find(path);
    if (it != g_res.end()) { it->second.used = g_frame; return &it->second; }
    Res r;
    r.kind = kind;
    r.used = g_frame;
    r.ready_at = resident ? 0 : g_frame + load_frames(kind);
    const char* rt = kind == "mesh" ? "via.render.MeshResource" : kind == "material" ? "via.render.MeshMaterialResource"
                   : kind == "texture" ? "via.render.TextureResource" : "via.motion.ChainResource";
    const char* ht = kind == "mesh" ? "via.render.MeshResourceHolder" : kind == "material" ? "via.render.MeshMaterialResourceHolder"
                   : kind == "texture" ? "via.render.TextureResourceHolder" : "via.motion.ChainResourceHolder";
    auto* rm = api().resource_manager();
    auto* res = rm ? rm->create_resource(rt, path) : nullptr;
    if (res) {
        res->add_ref();
        auto fn = api().sdk()->resource->create_holder;
        r.holder = fn ? reinterpret_cast<Obj*>(fn(reinterpret_cast<REFrameworkResourceHandle>(res), ht)) : nullptr;
        res->release();
        if (r.holder) r.holder->add_ref();
    }
    if (!r.holder) { r.dead = true; logf("rto: %s %s cannot be created (not in the paks?)", kind.c_str(), short_path(path).c_str()); }
    return &(g_res[path] = r);
}
bool usable(const Res* r) { return r && r->holder && !r->dead && g_frame >= r->ready_at; }
void backoff(const std::string& path) {
    auto it = g_res.find(path);
    if (it == g_res.end()) return;
    Res& r = it->second;
    ++r.fails;
    r.ready_at = g_frame + (60u << std::min(r.fails, 4));
    if (r.fails >= 6) { r.dead = true; logf("rto: %s never ready, left out", short_path(path).c_str()); }
}
// Holders not wanted for 10 s are given back (a model that wears one keeps its own reference)
void res_gc() {
    size_t n = 0;
    for (auto it = g_res.begin(); it != g_res.end();) {
        if (!g_warm.count(it->first) && g_frame - it->second.used > 600) {
            if (it->second.holder) it->second.holder->release();
            it = g_res.erase(it); ++n;
        } else ++it;
    }
    if (n) logf("rto: %zu holders given back, %zu kept", n, g_res.size());
}

// ---------------------------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------------------------
struct TexBind { uint32_t mi = 0, ti = 0; std::string target; };
struct MeshComp { Obj* c = nullptr; Obj* gobj = nullptr; std::string go, mat_key, exp_mesh, exp_mat; std::vector<TexBind> binds; };
struct ChainComp { Obj* c = nullptr; Obj* gobj = nullptr; std::string go, target; };
// A part unit being swapped (mesh and/or material + the chain of the same GameObject, docs/RUNTIME_OPTIONS.md,
// Part units): 1 = chain stopped on frame `at`, swap on a later frame; 2 = swapped on `at`, checked and the chain
// started again on a later frame; 3 = put back on `at` (not drawable), chain started again on a later frame.
// Between two frames the part always wears a whole (mesh, material, chain) of one source.
struct UnitSwap {
    Obj* mesh = nullptr; Obj* chain = nullptr;
    int stage = 1;
    uint32_t at = 0;
    bool stopped = false;                        // the chain was enabled and this module stopped it
    std::string go, pm, wm, pt, wt, pc, wc;      // what it wore, what it gets (mesh, material, chain); "" = unchanged
};
struct Verify { Obj* c = nullptr; bool mesh = true; std::string path, prev; };
struct Model {
    const char* name = "";
    Obj* root = nullptr;
    const Slot* slot = nullptr;
    int side = -1;
    std::vector<MeshComp> meshes;
    std::vector<ChainComp> chains;
    std::vector<Obj*> colors;
    std::map<std::string, std::string> orig;     // target -> what the model wore there when first seen (chains)
    std::vector<Verify> verify;
    std::vector<UnitSwap> units;
    bool walked = false, settled = false, chains_mapped = false;
    uint32_t ver = 0, next = 0, walk_at = 0, check_at = 0, since = 0;
    std::set<std::string> told;                  // log lines given once
};
Model g_models[3];                                // preview, P1, P2

void reset(Model& md) {
    // a chain this module stopped starts again: between two frames its part always wears a whole unit
    for (auto& u : md.units)
        if (u.stopped && T.ch_enable && alive_as(u.chain, T.chain)) {
            call(T.ch_enable, u.chain, { arg_bool(true) });
            logf("rto: %s %s chain started again (model reset)", md.name, u.go.c_str());
        }
    const char* nm = md.name;
    md = Model{};
    md.name = nm;
}

void tell_once(Model& md, const std::string& line);
Obj* comp_of(Obj* go, Obj* type) { return call_obj(T.get_comp, go, { arg_obj(type) }); }

void visit(Model& md, Obj* go) {
    if (!go) return;
    std::string name = str(call_obj(T.go_name, go));
    if (Obj* m = comp_of(go, T.t_mesh)) { MeshComp mc; mc.c = m; mc.gobj = go; mc.go = name; md.meshes.push_back(mc); }
    if (Obj* c = comp_of(go, T.t_chain)) { ChainComp cc; cc.c = c; cc.gobj = go; cc.go = name; md.chains.push_back(cc); }
    if (Obj* p = comp_of(go, T.t_pcc)) md.colors.push_back(p);
}

void walk(Model& md) {
    md.meshes.clear(); md.chains.clear(); md.colors.clear(); md.verify.clear();
    md.walked = true; md.chains_mapped = false; md.settled = false;
    visit(md, md.root);
    int n = 0;
    struct Rec {
        Model& md; int& n;
        void run(Obj* t, int depth) {
            while (t && n < 400) {
                ++n;
                visit(md, call_obj(T.tr_go, t));
                if (depth < 12) run(call_obj(T.tr_child, t), depth + 1);
                t = call_obj(T.tr_next, t);
            }
        }
    } rec{ md, n };
    Obj* tr = call_obj(T.go_tr, md.root);
    if (tr) rec.run(call_obj(T.tr_child, tr), 1);
    const Slot* found = nullptr;
    for (auto& mc : md.meshes) {
        if ((found = g_m.slot_of_path(holder_path(call_obj(T.get_mesh, mc.c))))) break;
        if ((found = g_m.slot_of_path(holder_path(call_obj(T.get_mat, mc.c))))) break;
    }
    if (found) md.since = g_frame;
    else {
        std::string first;
        for (auto& mc : md.meshes) { first = holder_path(call_obj(T.get_mesh, mc.c)); if (!first.empty()) break; }
        tell_once(md, "has no outfit with options: " + std::to_string(md.meshes.size()) + " meshes, first " + (first.empty() ? std::string("none") : short_path(first)));
    }
    if (found != md.slot) {
        md.slot = found;
        md.orig.clear();
        if (found) logf("rto: %s wears %s/%s (%s): %zu meshes, %zu chains", md.name, found->fighter_dir.c_str(), found->folder.c_str(),
                        found->label.c_str(), md.meshes.size(), md.chains.size());
    }
}

std::string target_of(const Slot& s, const std::string& p) {
    auto it = s.rev.find(p);
    return it == s.rev.end() ? p : it->second;
}
// "" = the manifest says nothing about this target
std::string want_path(Model& md, const std::map<std::string, Want>& want, const std::string& t) {
    auto it = want.find(t);
    if (it == want.end()) return {};
    if (!it->second.orig) return it->second.path;
    auto o = md.orig.find(t);
    return o == md.orig.end() ? t : o->second;
}

// An option's chain the model does not load under that name (the body's chain comes from the fighter's
// own folder): it stands for the chain of the part with the same number ("..._01_chain.chain" -> the
// GameObject whose mesh is "..._01.mesh"), which keeps its own file as the way back.
void map_chains(Model& md, const std::map<std::string, Want>& want) {
    md.chains_mapped = true;
    const Slot& s = *md.slot;
    std::set<std::string> seen;
    for (auto& cc : md.chains) seen.insert(target_of(s, holder_path(call_obj(T.ch_get, cc.c))));
    for (auto& [t, w] : want) {
        if (w.kind != "chain" || seen.count(t)) continue;
        auto e = t.rfind("_chain.chain");
        if (e == std::string::npos || e < 3 || t[e - 3] != '_') continue;
        const std::string part = t.substr(e - 3, 3);                      // "_01"
        for (auto& mc : md.meshes) {
            const std::string mt = target_of(s, holder_path(call_obj(T.get_mesh, mc.c)));
            if (mt.size() < 9 || mt.compare(mt.size() - 8, 8, part + ".mesh") != 0) continue;
            for (auto& cc : md.chains) {
                if (cc.go != mc.go || !cc.target.empty()) continue;
                cc.target = t;
                const std::string cur = holder_path(call_obj(T.ch_get, cc.c));
                if (!md.orig.count(t) && !s.rev.count(cur)) md.orig[t] = cur;   // one of our copies is not a way back
                logf("rto: %s chain %s stands for %s (the way back: %s)", md.name, cc.go.c_str(), short_path(t).c_str(),
                     short_path(md.orig[t]).c_str());
            }
        }
    }
}

void tell_once(Model& md, const std::string& line) {
    if (md.told.insert(line).second) logf("rto: %s %s", md.name, line.c_str());
}

void recolor(Model& md) {
    for (Obj* p : md.colors) {
        if (!alive_as(p, T.pcc)) continue;
        uint32_t n = 0;
        if (!get_u32(p, "ColorNum", n)) continue;
        call(T.set_color, p, { arg_i32(static_cast<int32_t>(n)) });
    }
}

bool drifted(Model& md) {
    if (!alive_as(md.root, T.go)) return true;
    for (auto& mc : md.meshes) {
        if (!alive_as(mc.c, T.mesh)) return true;
        if (holder_path(call_obj(T.get_mesh, mc.c)) != mc.exp_mesh || holder_path(call_obj(T.get_mat, mc.c)) != mc.exp_mat) return true;
    }
    return false;
}

bool drawable(Obj* mesh, bool m, bool t) {
    return (!m || ret_bool(T.mesh_ready, mesh)) && (!t || (ret_bool(T.mat_ready, mesh) && ret_u32(T.mat_num, mesh) > 0));
}
// A part unit's files (each null when unchanged): the chain first, on the mesh still worn (a chain set on a mesh
// that lacks some of its joints leaves them out: the game sets Mai's esf028_001_30_Chain on four prop meshes that
// each lack most of them), then the mesh and the material, then the chain set up again on the mesh now worn. The
// loader checked that the chain set first names no joint that goes away with the mesh it replaces.
void put_unit(Obj* mesh, Obj* chain, Res* m, Res* t, Res* c) {
    if (c) { call(T.ch_set, chain, { arg_obj(c->holder) }); call(T.ch_restart, chain); }
    if (m) call(T.set_mesh, mesh, { arg_obj(m->holder) });
    if (t) call(T.set_mat, mesh, { arg_obj(t->holder) });
    if (c) call(T.ch_restart, chain);
}
void unit_done(Model& md, UnitSwap& u, const char* why) {
    if (u.stopped && T.ch_enable && alive_as(u.chain, T.chain)) call(T.ch_enable, u.chain, { arg_bool(true) });
    logf("rto: %s %s chain %s (%s)", md.name, u.go.c_str(), u.stopped ? "started again" : "left as the game had it", why);
}

void pass(Model& md) {
    const Slot& s = *md.slot;
    const auto chosen = chosen_of(g_m, s, g_c, md.side);
    const auto want = desired(g_m, s, chosen);
    for (auto& [t, w] : want) if (!w.orig) if (auto it = g_res.find(w.path); it != g_res.end()) it->second.used = g_frame;
    bool pending = false, colour = false;
    const char* side_name = md.side == 1 ? "P2" : "P1";

    // 0. part units in flight (a chain stopped): checked a frame after their swap, then their chain runs again
    for (auto it = md.units.begin(); it != md.units.end();) {
        UnitSwap& u = *it;
        if (!alive_as(u.mesh, T.mesh) || !alive_as(u.chain, T.chain)) {           // the model was rebuilt
            md.walked = false; pending = true; it = md.units.erase(it); continue;
        }
        if (g_frame - u.at > 120) { unit_done(md, u, "given up"); it = md.units.erase(it); continue; }
        if (u.stage == 1 || g_frame <= u.at) { ++it; continue; }                      // its swap: step 2
        if (u.stage == 2 && !drawable(u.mesh, !u.wm.empty(), !u.wt.empty())) {
            Res* bm = u.wm.empty() ? nullptr : res_get(u.pm, "mesh", true);
            Res* bt = u.wt.empty() ? nullptr : res_get(u.pt, "material", true);
            Res* bc = u.wc.empty() ? nullptr : res_get(u.pc, "chain", true);
            put_unit(u.mesh, u.chain, bm && bm->holder ? bm : nullptr, bt && bt->holder ? bt : nullptr,
                     bc && bc->holder ? bc : nullptr);
            if (!u.wm.empty()) backoff(u.wm);
            if (!u.wt.empty()) backoff(u.wt);
            logf("rto: %s %s not drawable a frame later, back to %s", md.name, u.go.c_str(),
                 short_path(u.pm.empty() ? u.pt : u.pm).c_str());
            u.stage = 3; u.at = g_frame; pending = true; colour = true;
            ++it; continue;
        }
        unit_done(md, u, u.stage == 2 ? "part swapped" : "part put back");
        it = md.units.erase(it);
    }
    std::set<Obj*> unit_chains;                       // chains of parts swapped as units: not by step 4
    for (auto& u : md.units) unit_chains.insert(u.chain);

    // 1. the parts swapped on the last frame must still be drawable
    for (auto& v : md.verify) {
        if (!alive_as(v.c, T.mesh)) continue;
        const bool ok = v.mesh ? ret_bool(T.mesh_ready, v.c) : (ret_bool(T.mat_ready, v.c) && ret_u32(T.mat_num, v.c) > 0);
        if (ok) continue;
        auto it = g_res.find(v.prev);
        if (it != g_res.end() && it->second.holder) call(v.mesh ? T.set_mesh : T.set_mat, v.c, { arg_obj(it->second.holder) });
        backoff(v.path);
        logf("rto: %s %s not ready a frame later, back to %s", md.name, short_path(v.path).c_str(), short_path(v.prev).c_str());
        pending = true; colour = true;
    }
    std::set<Obj*> just_set;                          // parts whose material changes this frame: textures next frame
    md.verify.clear();

    if (!md.chains_mapped) map_chains(md, want);

    // 2. meshes and materials (a pair when both change; with the chain of their GameObject: a part unit)
    for (auto& mc : md.meshes) {
        if (!alive_as(mc.c, T.mesh)) { md.walked = false; pending = true; break; }
        const std::string pm = holder_path(call_obj(T.get_mesh, mc.c)), pt = holder_path(call_obj(T.get_mat, mc.c));
        const std::string wm = want_path(md, want, target_of(s, pm)), wt = want_path(md, want, target_of(s, pt));
        const bool need_m = !wm.empty() && wm != pm, need_t = !wt.empty() && wt != pt;
        if (!need_m && !need_t) continue;
        // The chain of the same GameObject, when it changes too: the part is swapped as one unit
        ChainComp* cc = nullptr;
        for (auto& c : md.chains) if (c.gobj == mc.gobj && alive_as(c.c, T.chain)) { cc = &c; break; }
        std::string pc, wc;
        if (cc) {
            pc = holder_path(call_obj(T.ch_get, cc->c));
            wc = want_path(md, want, cc->target.empty() ? target_of(s, pc) : cc->target);
            if (wc == pc) wc.clear();
        }
        // a unit's mesh comes with its chain, never without it (19/10: a mesh lacking the joints of the chain
        // still bound to it stopped the game)
        if (auto w = want.find(target_of(s, need_m ? pm : pt)); w != want.end() && w->second.unit.size() > 6
            && w->second.unit.compare(w->second.unit.size() - 6, 6, ".chain") == 0
            && (!cc || (cc->target.empty() ? target_of(s, pc) : cc->target) != w->second.unit)) {
            tell_once(md, mc.go + ": the chain of its part (" + short_path(w->second.unit) + ") is not on the model, part left as is");
            continue;
        }
        const bool unit = !wc.empty();
        if (unit) unit_chains.insert(cc->c);
        if (unit && !T.ch_enable) { tell_once(md, mc.go + ": its chain cannot be stopped, part left as is"); continue; }
        if (!ret_bool(T.mesh_ready, mc.c) || !ret_bool(T.mat_ready, mc.c)) { pending = true; continue; }   // the game still loads it
        Res* rm = need_m ? res_get(wm, "mesh", false) : nullptr;
        Res* rt = need_t ? res_get(wt, "material", false) : nullptr;
        Res* rc = unit ? res_get(wc, "chain", false) : nullptr;
        if ((rm && rm->dead) || (rt && rt->dead) || (rc && rc->dead)) {
            tell_once(md, "cannot load " + short_path(rm && rm->dead ? wm : rt && rt->dead ? wt : wc));
            continue;
        }
        if ((need_m && !usable(rm)) || (need_t && !usable(rt)) || (unit && !usable(rc))) { pending = true; continue; }
        Res* back_m = need_m ? res_get(pm, "mesh", true) : nullptr;      // the way back, made before the swap
        Res* back_t = need_t ? res_get(pt, "material", true) : nullptr;
        Res* back_c = unit ? res_get(pc, "chain", true) : nullptr;
        if ((need_m && !usable(back_m)) || (need_t && !usable(back_t)) || (unit && !usable(back_c))) {
            tell_once(md, "no way back from " + mc.go);
            continue;
        }
        if (unit) {
            // A part unit (docs/RUNTIME_OPTIONS.md, Part units): its chain stopped first, the swap a frame later (no
            // job of that chain runs meanwhile), checked and the chain started again a frame after that (step 0)
            UnitSwap* u = nullptr;
            for (auto& x : md.units) if (x.chain == cc->c) u = &x;
            if (!u) {
                UnitSwap n;
                n.mesh = mc.c; n.chain = cc->c; n.go = mc.go; n.at = g_frame;
                n.stopped = !T.ch_enabled || ret_bool(T.ch_enabled, cc->c);
                if (n.stopped) call(T.ch_enable, cc->c, { arg_bool(false) });
                md.units.push_back(n);
                logf("rto: %s %s %s: chain %s for its part's swap", md.name, side_name, mc.go.c_str(),
                     n.stopped ? "stopped" : "already stopped");
                continue;
            }
            if (u->stage != 1 || g_frame <= u->at) continue;
            u->pm = need_m ? pm : ""; u->wm = need_m ? wm : ""; u->pt = need_t ? pt : ""; u->wt = need_t ? wt : "";
            u->pc = pc; u->wc = wc;
            put_unit(mc.c, cc->c, rm, rt, rc);
            if (!drawable(mc.c, need_m, need_t)) {
                put_unit(mc.c, cc->c, back_m, back_t, back_c);
                if (need_m) backoff(wm);
                if (need_t) backoff(wt);
                logf("rto: %s %s %s: part not drawable, put back", md.name, side_name, mc.go.c_str());
                u->stage = 3; u->at = g_frame; pending = true;
                continue;
            }
            u->stage = 2; u->at = g_frame;
            just_set.insert(mc.c);
            colour = true;
            logf("rto: %s %s %s: part%s%s%s%s chain %s (joints %d -> %d)", md.name, side_name, mc.go.c_str(),
                 need_m ? " mesh " : "", need_m ? short_path(wm).c_str() : "", need_t ? " material " : "",
                 need_t ? short_path(wt).c_str() : "", short_path(wc).c_str(), joints_at(g_m, s, pm), joints_at(g_m, s, wm));
            continue;
        }
        if (need_m) call(T.set_mesh, mc.c, { arg_obj(rm->holder) });
        if (need_t) call(T.set_mat, mc.c, { arg_obj(rt->holder) });
        const bool ok = (!need_m || ret_bool(T.mesh_ready, mc.c)) && (!need_t || (ret_bool(T.mat_ready, mc.c) && ret_u32(T.mat_num, mc.c) > 0));
        if (!ok) {
            if (need_t) call(T.set_mat, mc.c, { arg_obj(back_t->holder) });
            if (need_m) call(T.set_mesh, mc.c, { arg_obj(back_m->holder) });
            if (need_m) backoff(wm);
            if (need_t) backoff(wt);
            pending = true;
            continue;
        }
        if (need_m) md.verify.push_back({ mc.c, true, wm, pm });
        if (need_t) md.verify.push_back({ mc.c, false, wt, pt });
        just_set.insert(mc.c);
        colour = true;
        logf("rto: %s %s %s:%s%s%s%s", md.name, side_name, mc.go.c_str(), need_m ? " mesh " : "", need_m ? short_path(wm).c_str() : "",
             need_t ? " material " : "", need_t ? short_path(wt).c_str() : "");
    }

    if (colour) { recolor(md); colour = false; }     // a new material comes with its file's colours

    // 3. textures, on the parts whose material is settled
    bool any_tex = false;
    for (auto& [t, w] : want) if (w.kind == "texture") { any_tex = true; break; }
    if (any_tex && md.walked) {
        for (auto& mc : md.meshes) {
            if (!alive_as(mc.c, T.mesh) || just_set.count(mc.c)) continue;
            if (!ret_bool(T.mat_ready, mc.c)) { pending = true; continue; }
            const uint32_t nm = ret_u32(T.mat_num, mc.c);
            if (!nm) continue;
            const std::string pt = holder_path(call_obj(T.get_mat, mc.c));
            if (pt != mc.mat_key) {                    // the bindings of a material that names an option's target
                mc.mat_key = pt;
                mc.binds.clear();
                for (uint32_t i = 0; i < nm && i < 64; ++i) {
                    const uint32_t tn = ret_u32(T.tex_num, mc.c, { arg_i32(static_cast<int32_t>(i)) });
                    for (uint32_t j = 0; j < tn && j < 64; ++j) {
                        const std::string p = holder_path(call_obj(T.get_tex, mc.c, { arg_i32(static_cast<int32_t>(i)), arg_i32(static_cast<int32_t>(j)) }));
                        const std::string t = target_of(s, p);
                        auto it = want.find(t);
                        if (it != want.end() && it->second.kind == "texture") mc.binds.push_back({ i, j, t });
                    }
                }
            }
            for (auto& b : mc.binds) {
                const std::string cur = holder_path(call_obj(T.get_tex, mc.c, { arg_i32(static_cast<int32_t>(b.mi)), arg_i32(static_cast<int32_t>(b.ti)) }));
                const std::string w = want_path(md, want, b.target);
                if (w.empty() || w == cur) continue;
                Res* r = res_get(w, "texture", false);
                if (r->dead) { tell_once(md, "cannot load " + short_path(w)); continue; }
                if (!usable(r)) { pending = true; continue; }
                call(T.set_tex, mc.c, { arg_i32(static_cast<int32_t>(b.mi)), arg_i32(static_cast<int32_t>(b.ti)), arg_obj(r->holder) });
                logf("rto: %s %s %s texture %u/%u %s", md.name, side_name, mc.go.c_str(), b.mi, b.ti, short_path(w).c_str());
            }
        }
    }

    // 4. chains (mapped before step 2)
    for (auto& cc : md.chains) {
        if (!alive_as(cc.c, T.chain)) { md.walked = false; pending = true; break; }
        if (unit_chains.count(cc.c)) continue;        // swapped with its part (step 2)
        const std::string cur = holder_path(call_obj(T.ch_get, cc.c));
        const std::string t = cc.target.empty() ? target_of(s, cur) : cc.target;
        const std::string w = want_path(md, want, t);
        if (w.empty() || w == cur) continue;
        if (auto u = want.find(t); u != want.end() && !u->second.unit.empty()) continue;   // a unit's chain: only with its mesh
        Res* r = res_get(w, "chain", false);
        if (r->dead) { tell_once(md, "cannot load " + short_path(w)); continue; }
        if (!usable(r)) { pending = true; continue; }
        call(T.ch_set, cc.c, { arg_obj(r->holder) });
        call(T.ch_restart, cc.c);
        logf("rto: %s %s %s chain %s", md.name, side_name, cc.go.c_str(), short_path(w).c_str());
    }

    // a part waiting for its swap that no longer needs one (the choice changed back): its chain runs again
    for (auto it = md.units.begin(); it != md.units.end();) {
        if (it->stage == 1 && !unit_chains.count(it->chain) && g_frame > it->at) {
            unit_done(md, *it, "no swap needed any more");
            it = md.units.erase(it);
        } else ++it;
    }
    if (!md.units.empty()) { md.next = g_frame + 1; return; }
    if (pending) { md.next = g_frame + 5; return; }
    if (!md.verify.empty()) { md.next = g_frame + 1; return; }
    md.settled = true;
    for (auto& mc : md.meshes)
        if (alive_as(mc.c, T.mesh)) { mc.exp_mesh = holder_path(call_obj(T.get_mesh, mc.c)); mc.exp_mat = holder_path(call_obj(T.get_mat, mc.c)); }
    md.check_at = g_frame + 30;
}

// light: compare what the model loads with what it was left with (menu screens and fight watches only)
void process(Model& md, int side, bool light) {
    if (!alive_as(md.root, T.go)) { reset(md); return; }
    if (!md.walked || (!md.slot && g_frame >= md.walk_at)) { walk(md); md.walk_at = g_frame + 60; }
    if (!md.slot) return;
    if (md.side != side || md.ver != g_ver) { md.side = side; md.ver = g_ver; md.settled = false; md.next = 0; }
    if (md.settled) {
        if (!light || g_frame < md.check_at) return;
        md.check_at = g_frame + 30;
        if (!drifted(md)) return;
        logf("rto: %s was rebuilt by the game, options again", md.name);
        walk(md);
        if (!md.slot) return;
    }
    if (g_frame < md.next) return;
    pass(md);
}

// ---------------------------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------------------------
Obj* g_crf = nullptr;                // the preview's CaptureRoomFighter, checked alive before each use
uint32_t g_crf_scan = 0, g_prev_next = 0;

std::string g_crf_log;               // the last preview search result logged (logged on change)
void preview_tick() {
    if (g_frame < g_prev_next) return;
    g_prev_next = g_frame + 10;
    if (!alive_as(g_crf, T.crf) || g_frame - g_crf_scan > 120) {
        g_crf_scan = g_frame;
        g_crf = nullptr;
        Obj* list = find_components(T.crf);
        const int n = list_count(list);
        Obj* any = nullptr;                          // one with a model, set up or not
        int with_obj = 0;
        for (int i = 0; i < n; ++i) {
            Obj* c = list_item(list, i);
            if (!c || !call_obj(T.crf_obj, c)) continue;
            ++with_obj;
            if (!any) any = c;
            if (!g_crf && (!T.crf_setup || ret_bool(T.crf_setup, c))) g_crf = c;
        }
        if (!g_crf) g_crf = any;
        char b[160];
        snprintf(b, sizeof b, "%d CaptureRoomFighter, %d with a model, %s", n, with_obj,
                 !g_crf ? "no preview" : (g_crf == any && T.crf_setup && !ret_bool(T.crf_setup, g_crf)) ? "using one not set up yet" : "using one set up");
        if (g_crf_log != b) { g_crf_log = b; logf("rto: preview search: %s", b); }
    }
    Model& md = g_models[0];
    Obj* root = g_crf ? call_obj(T.crf_obj, g_crf) : nullptr;
    if (root && T.crf_setup && !ret_bool(T.crf_setup, g_crf)) root = nullptr;   // still building
    if (root != md.root) {
        reset(md); md.root = root;
        if (root) logf("rto: preview model %s", str(call_obj(T.go_name, root)).c_str());
    }
    if (!root) return;
    process(md, menu::active() ? menu::side() : 0, true);
    if (!md.units.empty()) g_prev_next = g_frame + 1;     // a part unit in flight: its chain stays stopped one frame only
}

void watch_tick() {
    if (g_frame >= g_watch_until) {
        g_watch_until = 0;
        logf("rto: fighters watch done (%u model builds seen)", g_model_calls.exchange(0));
        res_gc();
        return;
    }
    if (g_frame < g_watch_next) return;
    g_watch_next = g_frame + 10;
    bool net = costumes::online();
    Obj* list = find_components(T.pb);
    bool seen[2] = {};
    for (int i = 0, n = list_count(list); i < n; ++i) {
        Obj* pb = list_item(list, i);
        int32_t id = -1;
        if (!pb || !get_i32(pb, "_PlayerID", id) || id < 0 || id > 1) continue;
        Obj* go = call_obj(T.pb_go, pb);
        if (T.pb_applied && !ret_bool(T.pb_applied, pb)) continue;     // its resources are not set up yet
        Model& md = g_models[1 + id];
        seen[id] = true;
        if (go != md.root) { reset(md); md.root = go; }
        if (go) process(md, net ? 0 : id, true);      // online: the player's own fighter wears P1's options
        if (!md.units.empty()) g_watch_next = g_frame + 1;
    }
    for (int i = 0; i < 2; ++i) if (!seen[i] && g_models[1 + i].root) reset(g_models[1 + i]);
}

bool fighter_settings_visible() {
    const SF6CoreAPI* a = ui::core();
    if (!a || !SF6CORE_HAS(a, ui_agents)) return g_bs;
    void* buf[4] = {};
    if (!a->ui_agents("MatchingFighterSetting", buf, 4)) return false;
    Obj* cm = call_obj(reinterpret_cast<Obj*>(buf[0]), "get_ControlMain");
    return cm && call_bool(cm, "get_ActualVisible");
}

// The files of the choices made in the menu, loaded once the game is up: a fight then shows them at once
void warm() {
    g_warm_done = true;
    std::set<std::string> want;
    for (auto& s : g_m.slots) {
        auto it = g_c.mods.find(s.mod);
        if (it == g_c.mods.end()) continue;
        for (int side = 0; side < 2; ++side) {
            if (!it->second[static_cast<size_t>(side)].changed) continue;
            for (auto& [t, w] : desired(g_m, s, chosen_of(g_m, s, g_c, side)))
                if (!w.orig && want.size() < 64 && want.insert(w.path).second) res_get(w.path, w.kind, false);
        }
    }
    g_warm = want;
    if (!want.empty()) logf("rto: %zu files of the chosen options loaded ahead", want.size());
}

int pre_setup_appearance(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_closing.load(std::memory_order_relaxed) && !g_model_noisy) { g_model_calls.fetch_add(1, std::memory_order_relaxed); post(EV_RTO_MODEL); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void fault(const char* where) {
    ++g_faults;
    logf("rto: EXCEPTION in %s (%d)", where, g_faults);
    for (auto& md : g_models) reset(md);
    if (g_faults >= 5) { g_disabled = true; logf("rto: 5 faults, runtime options off for this session"); }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------------------------

bool on() { return g_on && !g_disabled; }
const Manifest& manifest() { return g_m; }
Choices& choices() { return g_c; }
int frame_now() { return static_cast<int>(g_frame); }

void save_choices() {
    if (!write_file(kChoices, write_choices(g_c, g_m))) logf("rto: cannot write costume_choices.json");
}
void choices_changed() {
    ++g_ver;
    g_prev_next = 0;
    // what the new choices need stays loaded for the fights
    std::set<std::string> want;
    for (auto& s : g_m.slots) {
        auto it = g_c.mods.find(s.mod);
        if (it == g_c.mods.end()) continue;
        for (int side = 0; side < 2; ++side)
            if (it->second[static_cast<size_t>(side)].changed)
                for (auto& [t, w] : desired(g_m, s, chosen_of(g_m, s, g_c, side))) if (!w.orig && want.size() < 64) want.insert(w.path);
    }
    g_warm = want;
}
int preview_since() {
    const Model& md = g_models[0];
    return (md.root && md.slot && !g_no_rows.count(md.slot)) ? static_cast<int>(md.since) : -1;
}
const Slot* preview_slot() {
    const Model& md = g_models[0];
    return (md.root && md.slot && !g_no_rows.count(md.slot)) ? md.slot : nullptr;
}

void init() {
    g_models[0].name = "preview"; g_models[1].name = "P1"; g_models[2].name = "P2";
    std::string err;
    if (!parse_manifest(read_file(kManifest, 64u << 20), g_m, err)) {
        if (err != "no file") logf("rto: costume_options.json not used: %s", err.c_str());
        return;
    }
    Json oj;
    std::string pak;
    if (json_parse(read_file(kOutfits), oj)) pak = oj.get("pak") ? oj.get("pak")->as_str() : "";
    if (pak != g_m.pak) { logf("rto: costume_options.json names %s, the costume pak is %s: stale, not used", g_m.pak.c_str(), pak.c_str()); g_m = Manifest{}; return; }
    if (parse_choices(read_file(kChoices), g_c)) {
        std::vector<std::string> notes;
        sanitize(g_c, g_m, notes);
        for (auto& n : notes) logf("rto: choices: %s", n.c_str());
    }
    g_on = true;
    // Toggles the slot wears whose Off would change nothing (no copy of the part without them): not in the menu
    for (auto& s : g_m.slots) {
        const Mod* mod = g_m.mod_of(s);
        if (!mod) continue;
        for (auto& o : mod->options)
            if (o.mode == "toggle" && offered(s, o.id) && !removable(g_m, s, o.id))
                logf("rto: %s/%s: \"%s\" cannot be turned off live (no copy of the outfit without it): not in the menu",
                     s.fighter_dir.c_str(), s.folder.c_str(), o.name.c_str());
        if (rows(g_m, s, s.defaults).empty()) {
            g_no_rows.insert(&s);
            logf("rto: %s/%s: no option can change live, no options menu", s.fighter_dir.c_str(), s.folder.c_str());
        }
    }
    size_t opts = 0;
    for (auto& m : g_m.mods) opts += m.options.size();
    logf("rto: %zu modular mods, %zu options, %zu slots (layout %s, generated %s)", g_m.mods.size(), opts, g_m.slots.size(),
         g_m.layout.c_str(), g_m.generated.c_str());
}

int migrate_former(int fighter, int costume_no) {
    if (!g_on) return -1;
    const Former* f = nullptr;
    const Slot* k = former_of(g_m, fighter, costume_no, &f);
    if (!k || !f) return -1;
    auto& sd = g_c.mods[k->mod][0];
    sd.changed = true;
    sd.ids = f->ids;
    sd.names.clear();
    g_migrated = true;
    logf("rto: [F%d] Battle Settings outfit %d (%s) is now outfit %d with that variant's options", fighter, costume_no,
         f->label.c_str(), k->costume_no);
    return k->costume_no;
}

void migration_done() {
    if (!g_migrated) return;
    g_c.former_done = g_m.generated + " " + g_m.pak;
    save_choices();
}

void install_hooks() {
    if (!g_on) return;
    if (!types()) { g_on = false; return; }
    hook_all(T.pb, "SetupAppearance", pre_setup_appearance, nullptr);
    menu::install();
}

bool busy() {
    if (!on()) return false;
    if (g_bs || g_watch_until || menu::busy()) return true;
    return !g_warm_done && g_frame < 60 * 120;
}

void tick(uint32_t ev) {
    if (!on() || !T.ok) return;
    try {
        if (ev & EV_SELECT_START) { g_select = true; g_watch_until = 0; for (auto& md : g_models) reset(md); }
        if (ev & EV_SELECT_END) g_select = false;
        if (ev & EV_MENU_OPEN) {
            if (!g_bs || g_bs_closed) logf("rto: Battle Settings open: its preview is followed");
            g_bs = true; g_bs_closed = false; g_bs_seen = g_frame;
        }
        if ((ev & EV_MENU_CLOSE) && g_bs && !g_bs_closed) { g_bs_closed = true; g_bs_seen = g_frame; }
        if ((ev & (EV_HOLDER | EV_RTO_MODEL)) && !g_select) {
            if (!g_watch_until) {
                if (costumes::select_active()) { g_select = true; return; }   // the select screen builds its own models
                logf("rto: fighters watch");
                g_watch_start = g_frame;
            }
            if ((ev & EV_HOLDER) || g_frame - g_watch_start < 60 * 30) g_watch_until = g_frame + 600;
            else if (!g_model_noisy) {
                g_model_noisy = true;
                logf("rto: PlayerBehavior.SetupAppearance is called all the time: not a build event, ignored from now on");
            }
        }
        if (g_select) return;
        bool fs = false;
        if (g_bs) {
            fs = fighter_settings_visible();
            if (fs != g_fs_was) { g_fs_was = fs; logf("rto: Fighter Settings %s", fs ? "on screen" : "left"); }
            if (fs) g_bs_seen = g_frame;
            else if (g_bs_closed && g_frame - g_bs_seen > 180 && !menu::active()) {
                // closed (HidObject also comes when a sub-screen hides it: Fighter Settings on screen keeps it)
                logf("rto: Battle Settings closed: its preview is no longer followed");
                g_bs = false;
                g_fs_was = false;
                reset(g_models[0]);
                g_crf = nullptr;
                res_gc();
            }
        }
        menu::tick(fs);
        if (g_bs || menu::active()) preview_tick();
        if (g_watch_until) watch_tick();
        if (!g_warm_done && g_frame > 60 * 30 && !g_watch_until && !g_bs) warm();
    } catch (...) {
        fault("tick");
    }
}

} // namespace slots::rto
