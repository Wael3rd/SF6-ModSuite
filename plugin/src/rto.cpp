// SF6 Slots — runtime costume options, the game side (rto.hpp, docs/RUNTIME_OPTIONS.md).
//
// A model is the Battle Settings preview (CaptureRoomFighter.FighterObj), a side's model on the character
// select (GameObject FighterVisual_01 / _02), on the VS screen (under Pos < Left / Right < FighterEmoteCtrl) or a
// fighter (PlayerBehavior, _PlayerID 0 / 1). Its slot is
// found from the paths its meshes load (the slot's folder), so the online alias (a slot shown over its
// carrier) needs nothing more. For the side's chosen options the manifest
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
// When: the preview while Battle Settings is up, each side's model while the character select is up and on the VS
// screen that follows (polled every 10 frames, menu screens); a fighter after an event (its model built:
// PlayerBehavior.SetupAppearance, or an outfit mounted for a fight, or the Training popup's options confirmed:
// fighters_refresh): a watch of 10 s, then nothing until the next event. A model re-applies when the choices'
// version changed since its last pass, rebuilt or not. After the Training popup's Confirm of options only, that
// fighter's swap is held until every file is ready (g_hold_side), then made in one pass behind the pause menu; the
// popup's option files are asked for ahead while it is up (g_warm_pop).
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
std::set<const Slot*> g_no_rows_off; // the same while NSFW options are hidden (only NSFW options can change live)

bool has_rows(const Slot* s) { return !g_no_rows.count(s) && (g_m.show_nsfw || !g_no_rows_off.count(s)); }

// ---- screens and events ----
bool g_select = false;              // character select open: only its two models (select_tick), no fighter watch
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
std::set<std::string> g_warm_prev;  // files of every option of the outfit the preview wears, kept while it wears it
std::set<std::string> g_warm_pop;   // files of every option of the outfits the Training popup's options showed (this visit)
// The Training popup confirmed (options only): that fighter's swap (0 P1, 1 P2) waits until every file is ready, then is
// made whole behind the pause menu; -1 none. g_hold_id tells one hold from the next (Model::hold_id: passed)
int g_hold_side = -1;
uint32_t g_hold_id = 0;
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
    Method *pcc_go = nullptr, *tr_parent = nullptr;
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
    T.pcc_go = method_of(T.pcc, "get_GameObject", 0);          // the character select's models (not required)
    T.tr_parent = method_of(T.tr, "get_Parent", 0);
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
bool swappable(const std::string& kind) { return kind == "mesh" || kind == "material" || kind == "texture" || kind == "chain"; }
// The files a set of options needs that are not ready yet (asked for now when not yet; one that cannot load is left
// out); total: how many it needs
int files_left(const std::map<std::string, Want>& want, int* total) {
    int left = 0, n = 0;
    for (auto& [t, w] : want) {
        if (w.orig || w.path.empty() || !swappable(w.kind)) continue;
        ++n;
        const Res* r = res_get(w.path, w.kind, false);
        if (!r->dead && !usable(r)) ++left;
    }
    if (total) *total = n;
    return left;
}
// TrainingManager._MenuState (0 NONE, 3 CLOSING, 4 FIRST, 11 CHARA_SELECT, ...: small values, its low byte whatever the
// enum's size), -1 without a TrainingManager
int menu_state_now() {
    Obj* tm = reinterpret_cast<Obj*>(api().get_managed_singleton("app.training.TrainingManager"));
    if (!alive(tm)) return -1;
    void* p = field_ptr(tm, "_MenuState");
    return p ? static_cast<int>(*static_cast<uint8_t*>(p)) : -1;
}
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
        if (!g_warm.count(it->first) && !g_warm_prev.count(it->first) && !g_warm_pop.count(it->first) && g_frame - it->second.used > 600) {
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
    uint32_t seen = 0;                           // the frame its root was found (VS screen models)
    std::set<std::string> told;                  // log lines given once
    uint32_t hold_id = 0, hold_told = 0;         // the hold (g_hold_id) it passed / it said it waits for
};
Model g_models[7];                                // preview, P1, P2 (fighters), select P1, select P2, VS P1, VS P2
// The character select builds a side's model under the same GameObject (its root stays): looked at more often, and the
// VS screen's (shown a few seconds)
bool select_model(const Model& md) { return &md >= &g_models[3] && &md <= &g_models[6]; }
// The VS screen's: shown at once and for a few seconds, every frame until it wears its options (vs_tick)
bool vs_model(const Model& md) { return &md >= &g_models[5] && &md <= &g_models[6]; }
uint32_t check_every(const Model& md) { return select_model(md) ? 10 : 30; }

// The last write of this module on a preview model (frame, what): told when the preview stays empty (preview_tick)
uint32_t g_prev_write_at = 0;
std::string g_prev_write;
void wrote(const Model& md, const std::string& what) {
    if (&md != &g_models[0]) return;
    g_prev_write_at = g_frame;
    g_prev_write = what;
}

void reset(Model& md) {
    // a chain this module stopped starts again: between two frames its part always wears a whole unit
    for (auto& u : md.units)
        if (u.stopped && T.ch_enable && alive_as(u.chain, T.chain)) {
            call(T.ch_enable, u.chain, { arg_bool(true) });
            wrote(md, u.go + " chain started again (model reset)");
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

void warm_preview(const Slot* s);

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
    // one of our slots before a game outfit with options (a slot may keep a part in its outfit's game folder)
    std::vector<std::string> paths;
    for (auto& mc : md.meshes) {
        paths.push_back(holder_path(call_obj(T.get_mesh, mc.c)));
        paths.push_back(holder_path(call_obj(T.get_mat, mc.c)));
    }
    const Slot* found = g_m.slot_of_paths(paths);
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
        if (&md == &g_models[0]) warm_preview(found);
    }
}

// The files of every option of the outfit the Battle Settings preview wears, asked for as soon as it is seen: a pick
// made in the menu a second later then swaps at once, whole. Asked for only at the pick, a file of an option never
// worn before (and the textures its material names) was not resident after its 30 frames (20/10, Ryu Aloha C2 on its
// first pick: body put back, hair white; right on the next picks, its files asked for since). Kept while the preview
// wears that outfit (res_gc), given back after.
constexpr size_t kWarmPreview = 160;
void warm_preview(const Slot* s) {
    std::set<std::string> want;
    if (s) {
        auto add = [&](const File& f) {
            if (f.path.empty() || f.path == f.target || want.size() >= kWarmPreview) return;
            if (f.kind != "mesh" && f.kind != "material" && f.kind != "texture" && f.kind != "chain") return;
            if (want.insert(f.path).second) res_get(f.path, f.kind, false);
        };
        for (auto& [id, fs] : s->files)
            if (!hidden(g_m, *s, id)) for (auto& f : fs) add(f);
        for (auto& f : s->base) add(f);
    }
    if (want != g_warm_prev && !want.empty())
        logf("rto: %zu files of %s/%s's options asked for ahead (the preview wears it)%s", want.size(), s->fighter_dir.c_str(),
             s->folder.c_str(), want.size() >= kWarmPreview ? " (the first ones)" : "");
    g_warm_prev = std::move(want);
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
        wrote(md, "colours set again");
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
    if (u.stopped && T.ch_enable && alive_as(u.chain, T.chain)) { call(T.ch_enable, u.chain, { arg_bool(true) }); wrote(md, u.go + " chain started again"); }
    logf("rto: %s %s chain %s (%s)", md.name, u.go.c_str(), u.stopped ? "started again" : "left as the game had it", why);
}

void pass(Model& md) {
    const Slot& s = *md.slot;
    const auto chosen = chosen_of(g_m, s, g_c, md.side);
    const auto want = desired(g_m, s, chosen);
    for (auto& [t, w] : want) if (!w.orig) if (auto it = g_res.find(w.path); it != g_res.end()) it->second.used = g_frame;
    bool pending = false, colour = false;
    const char* side_name = md.side == 1 ? "P2" : "P1";

    // The Training popup confirmed: this fighter's swap waits (behind the pause menu) until every file its options need
    // is ready, then is made in one pass (a part unit's chain stopped a frame before its swap); never half loaded
    if (g_hold_side >= 0 && &md == &g_models[1 + g_hold_side] && md.hold_id != g_hold_id && md.units.empty() && md.verify.empty()) {
        int total = 0;
        const int left = files_left(want, &total);
        if (left) {
            if (md.hold_told != g_hold_id) {
                md.hold_told = g_hold_id;
                logf("rto: %s %s: its options wait for %d of their %d files (held behind the pause menu)", md.name, side_name, left, total);
            }
            md.next = g_frame + 1;
            return;
        }
        md.hold_id = g_hold_id;
        logf("rto: %s %s: the %d files of its options ready: swapped behind the pause menu at frame %u (menu state %d)", md.name,
             side_name, total, g_frame, menu_state_now());
    }

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
            wrote(md, u.go + " part put back");
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
        wrote(md, "put back " + short_path(v.prev));
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
                if (n.stopped) { call(T.ch_enable, cc->c, { arg_bool(false) }); wrote(md, mc.go + " chain stopped"); }
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
                wrote(md, mc.go + " part put back at once");
                logf("rto: %s %s %s: part not drawable, put back", md.name, side_name, mc.go.c_str());
                u->stage = 3; u->at = g_frame; pending = true;
                colour = true;                        // a material set again lost the colours SetColor wrote
                continue;
            }
            wrote(md, mc.go + " part swapped");
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
            // Put back in the same frame. A material set again comes with its file's colours, not the ones SetColor
            // wrote: without a recolour the part stays black (20/10, Ryu Aloha C2's body on its first pick)
            if (need_t) call(T.set_mat, mc.c, { arg_obj(back_t->holder) });
            if (need_m) call(T.set_mesh, mc.c, { arg_obj(back_m->holder) });
            if (need_m) backoff(wm);
            if (need_t) backoff(wt);
            wrote(md, mc.go + " put back at once");
            logf("rto: %s %s %s: not drawable at once (still loading), put back, tried again later:%s%s%s%s", md.name, side_name,
                 mc.go.c_str(), need_m ? " mesh " : "", need_m ? short_path(wm).c_str() : "", need_t ? " material " : "",
                 need_t ? short_path(wt).c_str() : "");
            pending = true;
            colour = true;
            continue;
        }
        wrote(md, mc.go + " swapped");
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
                wrote(md, mc.go + " texture");
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
        wrote(md, cc.go + " chain");
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
    if (pending) { md.next = g_frame + (vs_model(md) ? 1 : 5); return; }   // a VS model: its first ready frame
    if (!md.verify.empty()) { md.next = g_frame + 1; return; }
    md.settled = true;
    for (auto& mc : md.meshes)
        if (alive_as(mc.c, T.mesh)) { mc.exp_mesh = holder_path(call_obj(T.get_mesh, mc.c)); mc.exp_mat = holder_path(call_obj(T.get_mat, mc.c)); }
    md.check_at = g_frame + check_every(md);
}

// light: compare what the model loads with what it was left with (menu screens and fight watches only)
void process(Model& md, int side, bool light) {
    if (!alive_as(md.root, T.go)) { reset(md); return; }
    if (!md.walked || (!md.slot && g_frame >= md.walk_at)) {
        walk(md);
        // a VS model with no outfit of ours yet: its parts may still be set up, looked at again each frame for a second
        md.walk_at = g_frame + (vs_model(md) && g_frame - md.seen < 60 ? 1 : select_model(md) ? 20 : 60);
    }
    if (!md.slot) return;
    if (md.side != side || md.ver != g_ver) { md.side = side; md.ver = g_ver; md.settled = false; md.next = 0; }
    if (md.settled) {
        if (!light || g_frame < md.check_at) return;
        md.check_at = g_frame + check_every(md);
        if (!drifted(md)) return;
        logf("rto: %s was rebuilt by the game, options again", md.name);
        walk(md);
        if (!md.slot) return;
    }
    if (g_frame < md.next) return;
    pass(md);
}

// ---------------------------------------------------------------------------------------------
// Diagnostics of an empty preview (20/10: after fast costume changes that stop on a game outfit the preview stayed
// without a model and the game built none, CaptureRoomFighter made and dropped again every 30-50 frames): which
// CaptureRoomFighter methods ran between two preview models, and the fields of one waiting for its model (a normal
// load and a stuck one compared). The hooks only count calls; the rest is reads from LateUpdateBehavior.
// ---------------------------------------------------------------------------------------------
constexpr int kCrfHooks = 24;
std::string g_crf_hook_name[kCrfHooks];
std::atomic<uint32_t> g_crf_calls[kCrfHooks];
uint32_t g_crf_mark[kCrfHooks] = {};
int g_crf_hook_n = 0;
template <int I> int crf_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_crf_calls[I].fetch_add(1, std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
template <int... I> constexpr REFPreHookFn crf_fn(int i, std::integer_sequence<int, I...>) {
    constexpr REFPreHookFn t[] = { &crf_pre<I>... };
    return t[i];
}
// Its own methods (no getter / setter), the ones named like a load step first
void crf_hooks() {
    std::vector<Method*> key, rest;
    std::string all;
    for (auto* m : T.crf->get_methods()) {
        if (!m || !m->get_name() || m->is_static()) continue;
        const std::string n = m->get_name();
        all += (all.empty() ? "" : " ") + n;
        if (n.rfind("get_", 0) == 0 || n.rfind("set_", 0) == 0 || n.rfind("op_", 0) == 0 || n[0] == '<') continue;
        std::string low = n;
        for (auto& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        bool k = false;
        for (const char* w : { "setup", "request", "load", "create", "change", "release", "destroy", "start", "init",
                               "ready", "wait", "end", "ctor", "apply", "build" })
            if (low.find(w) != std::string::npos) k = true;
        (k ? key : rest).push_back(m);
    }
    key.insert(key.end(), rest.begin(), rest.end());
    std::string hooked;
    for (Method* m : key) {
        if (g_crf_hook_n >= kCrfHooks) break;
        if (!m->get_function_raw()) continue;
        const int i = g_crf_hook_n;
        if (!m->add_hook(crf_fn(i, std::make_integer_sequence<int, kCrfHooks>{}), hook_post_pass, false)) continue;
        g_crf_hook_name[i] = std::string(m->get_name()) + "/" + std::to_string(m->get_num_params());
        hooked += (hooked.empty() ? "" : " ") + g_crf_hook_name[i];
        ++g_crf_hook_n;
    }
    logf("rto: preview diagnostics: CaptureRoomFighter methods: %s", all.c_str());
    logf("rto: preview diagnostics: calls counted: %s", hooked.empty() ? "none" : hooked.c_str());
}
// The counted calls since the last time this was asked
std::string crf_calls_since() {
    std::string s;
    for (int i = 0; i < g_crf_hook_n; ++i) {
        const uint32_t c = g_crf_calls[i].load(std::memory_order_relaxed);
        if (c != g_crf_mark[i]) s += (s.empty() ? "" : ", ") + g_crf_hook_name[i] + " x" + std::to_string(c - g_crf_mark[i]);
        g_crf_mark[i] = c;
    }
    return s.empty() ? "none" : s;
}
// An object's fields (its type, then its parents up to via.Component): booleans, numbers, enums, strings, the type of
// an object (or null); structs left out
std::string fields_of(Obj* o) {
    std::string s;
    int n = 0;
    for (TD* t = o ? o->get_type_definition() : nullptr; t && n < 80; t = t->get_parent_type()) {
        const std::string tn = t->get_full_name();
        if (tn == "via.Component" || tn == "System.Object") break;
        for (auto* f : t->get_fields()) {
            if (!f || !f->get_name() || f->is_static() || n >= 80) continue;
            TD* ft = f->get_type();
            void* p = ft ? f->get_data_raw(o, false) : nullptr;
            if (!p) continue;
            const std::string ftn = ft->get_full_name();
            std::string v;
            if (ftn == "System.Boolean") v = *static_cast<uint8_t*>(p) ? "true" : "false";
            else if (ftn == "System.Single") { char b[32]; snprintf(b, sizeof b, "%g", *static_cast<float*>(p)); v = b; }
            else if (ft->is_valuetype()) {
                const uint32_t sz = ft->get_valuetype_size();
                if (sz == 1) v = std::to_string(*static_cast<uint8_t*>(p));
                else if (sz == 2) v = std::to_string(*static_cast<int16_t*>(p));
                else if (sz == 4) v = std::to_string(*static_cast<int32_t*>(p));
                else if (sz == 8) v = std::to_string(*static_cast<int64_t*>(p));
                else continue;
            } else {
                Obj* r = *static_cast<Obj**>(p);
                v = !r ? "null" : !alive(r) ? "?" : ftn == "System.String" ? "\"" + str(r) + "\"" : type_name(r);
            }
            s += (s.empty() ? "" : ", ") + std::string(f->get_name()) + "=" + v;
            ++n;
        }
    }
    return s.empty() ? "no field read" : s;
}
Obj* g_crf_wait = nullptr;           // the last CaptureRoomFighter seen without a model, since g_crf_wait_at
uint32_t g_crf_wait_at = 0;
bool g_crf_wait_told = false;
int g_crf_wait_dumps = 0;            // fields told for a normal wait (3 a session; every stuck one is told)

// ---------------------------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------------------------
Obj* g_crf = nullptr;                // the preview's CaptureRoomFighter, checked alive before each use
uint32_t g_crf_scan = 0, g_prev_next = 0;

std::string g_crf_log;               // the last preview search result logged (logged on change)
std::atomic<uint32_t> g_builds{0};   // PlayerBehavior.SetupAppearance calls (every model build, the preview's too)
uint32_t g_holders = 0;              // frames with an outfit's visual manifest created (EV_HOLDER: the game loads an outfit)
uint32_t g_gone_at = 0, g_gone_builds = 0, g_gone_holders = 0;   // the preview without a model since this frame (0: it has one)
bool g_gone_told = false;
int g_prev_fighter = -1;             // the fighter of the last preview model (its name "esfNNNvMM"), -1 none
uint32_t g_prev_fighter_at = 0;      // the frame that model was seen
void preview_tick() {
    if (g_frame < g_prev_next) return;
    g_prev_next = g_frame + 10;
    if (!alive_as(g_crf, T.crf) || g_frame - g_crf_scan > 120) {
        g_crf_scan = g_frame;
        g_crf = nullptr;
        Obj* list = find_components(T.crf);
        const int n = list_count(list);
        Obj* any = nullptr;                          // one with a model, set up or not
        Obj* wait = nullptr;                         // one without a model
        int with_obj = 0;
        for (int i = 0; i < n; ++i) {
            Obj* c = list_item(list, i);
            if (c && !call_obj(T.crf_obj, c) && !wait) wait = c;
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
        if (wait != g_crf_wait) { g_crf_wait = wait; g_crf_wait_at = g_frame; g_crf_wait_told = false; }
    }
    // one waiting 30 frames for its model: its fields (a normal load takes about 40), every one while the preview is
    // stuck (since the empty preview was told)
    if (g_crf_wait && !g_crf_wait_told && g_frame - g_crf_wait_at >= 30 && (g_crf_wait_dumps < 3 || g_gone_told)) {
        g_crf_wait_told = true;
        if (alive_as(g_crf_wait, T.crf)) {
            ++g_crf_wait_dumps;
            logf("rto: preview diagnostics: CaptureRoomFighter waiting %u frames for its model%s: %s", g_frame - g_crf_wait_at,
                 g_gone_told ? " (preview stuck)" : "", fields_of(g_crf_wait).c_str());
        }
    }
    Model& md = g_models[0];
    Obj* root = g_crf ? call_obj(T.crf_obj, g_crf) : nullptr;
    // The preview without a model while Fighter Settings is up (20/10: after fast costume changes it stayed empty
    // until the character grid): told once after 5 s with this module's last write on a preview model and the model
    // builds seen since (none = the game never started the next one)
    if (root) {
        if (g_gone_at && g_gone_told) logf("rto: preview: a model again after %u frames", g_frame - g_gone_at);
        g_gone_at = 0;
    } else if (!g_gone_at) {
        g_gone_at = g_frame; g_gone_told = false; g_gone_builds = g_builds.load(std::memory_order_relaxed);
        g_gone_holders = g_holders;
    } else if (!g_gone_told && g_fs_was && g_frame - g_gone_at > 300) {
        g_gone_told = true;
        logf("rto: preview: no model for %u frames (since f%u, %s); since then: %u outfit manifests created, %u model "
             "builds (SetupAppearance); our last write on a preview model: %s; CaptureRoomFighter calls since the last "
             "model: %s", g_frame - g_gone_at, g_gone_at,
             g_crf_log.c_str(), g_holders - g_gone_holders, g_builds.load(std::memory_order_relaxed) - g_gone_builds,
             g_prev_write_at ? ("f" + std::to_string(g_prev_write_at) + " " + g_prev_write).c_str() : "none",
             crf_calls_since().c_str());
    }
    if (root && T.crf_setup && !ret_bool(T.crf_setup, g_crf)) root = nullptr;   // still building
    if (root != md.root) {
        reset(md); md.root = root;
        if (root) {
            const std::string nm = str(call_obj(T.go_name, root));
            logf("rto: preview model %s (CaptureRoomFighter calls since the last one: %s)", nm.c_str(), crf_calls_since().c_str());
            if (nm.size() >= 6 && nm.compare(0, 3, "esf") == 0 && isdigit(static_cast<unsigned char>(nm[3])) &&
                isdigit(static_cast<unsigned char>(nm[4])) && isdigit(static_cast<unsigned char>(nm[5]))) {
                g_prev_fighter = (nm[3] - '0') * 100 + (nm[4] - '0') * 10 + (nm[5] - '0');
                g_prev_fighter_at = g_frame;
            }
        }
    }
    if (!root) return;
    process(md, menu::active() ? menu::side() : 0, true);
    if (!md.units.empty()) g_prev_next = g_frame + 1;     // a part unit in flight: its chain stays stopped one frame only
}

uint32_t g_fighters_at = 0;         // both fighters seen built since the VS screen's models were looked for (vs_tick)
void watch_tick() {
    if (g_frame >= g_watch_until && g_hold_side < 0) {
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
        else if (!md.settled && md.next && md.next < g_watch_next) g_watch_next = md.next;   // a held swap: its next frame
    }
    for (int i = 0; i < 2; ++i) if (!seen[i] && g_models[1 + i].root) reset(g_models[1 + i]);
    if (seen[0] && seen[1] && !g_fighters_at) g_fighters_at = g_frame;   // the fight is built (vs_tick: no VS screen to wait for)
}

// The character select's models: each side's PlayerColorController sits on the GameObject FighterVisual_01 (P1) /
// FighterVisual_02 (P2), its meshes below it (the Color Mod's finding; a parent of that name also counts). Polled
// every 10 frames while the screen is up, like the Battle Settings preview: the options chosen in the menu show
// there too (20/10: C. Viper Outfit 1 Coatless in Battle Settings, Outfit 1 with its coat on the character select).
// Online, the player's own side wears P1's options (as in a fight).
uint32_t g_sel_scan = 0, g_sel_next = 0, g_sel_start = 0;
std::string g_sel_log;                // the last search result logged (logged on change)
int visual_side(Obj* go) {
    const std::string nm = str(call_obj(T.go_name, go));
    return nm == "FighterVisual_01" ? 1 : nm == "FighterVisual_02" ? 2 : 0;
}
void vs_open(uint32_t frames, const char* why);
void warm_vs();
void select_tick() {
    if (!T.pcc_go || g_frame < g_sel_next) return;
    g_sel_next = g_frame + 10;
    Model* sm[2] = { &g_models[3], &g_models[4] };
    bool want_scan = g_frame - g_sel_scan >= 120;
    for (auto* md : sm) if (!md->root || !alive_as(md->root, T.go)) want_scan = want_scan || g_frame - g_sel_scan >= 30;
    if (want_scan) {
        g_sel_scan = g_frame;
        // its end missed (no EV_SELECT_END): never into a fight (the intro's rigs hang under FighterVisual_0N too)
        if (g_frame - g_sel_start > 120 && !costumes::select_active()) {
            logf("rto: character select no longer up: its models are no longer followed");
            g_select = false;
            select::leave_all("character select no longer up");
            warm_vs();
            reset(*sm[0]); reset(*sm[1]);
            vs_open(60 * 90, "character select no longer up");
            return;
        }
        Obj* found[2] = {};
        Obj* list = find_components(T.pcc);
        const int n = list_count(list);
        for (int i = 0; i < n; ++i) {
            Obj* c = list_item(list, i);
            Obj* go = c ? call_obj(T.pcc_go, c) : nullptr;
            int side = go ? visual_side(go) : 0;
            Obj* tr = (!side && go && T.tr_parent) ? call_obj(T.go_tr, go) : nullptr;
            for (int up = 0; up < 4 && tr && !side; ++up) {
                tr = call_obj(T.tr_parent, tr);
                go = tr ? call_obj(T.tr_go, tr) : nullptr;
                side = go ? visual_side(go) : 0;
            }
            if (side && !found[side - 1]) found[side - 1] = go;
        }
        char b[120];
        snprintf(b, sizeof b, "%d colour controllers, P1 model %s, P2 model %s", n, found[0] ? "found" : "none",
                 found[1] ? "found" : "none");
        if (g_sel_log != b) { g_sel_log = b; logf("rto: character select: %s", b); }
        for (int i = 0; i < 2; ++i) {
            if (found[i] == sm[i]->root) continue;
            reset(*sm[i]);
            sm[i]->root = found[i];
        }
    }
    const bool net = costumes::online();
    for (int i = 0; i < 2; ++i) {
        Model& md = *sm[i];
        if (!md.root) continue;
        process(md, net ? 0 : i, true);
        if (!md.units.empty()) g_sel_next = g_frame + 1;     // a part unit in flight: its chain stays stopped one frame only
        else if (!md.settled && md.next && md.next < g_sel_next) g_sel_next = md.next;
    }
}

// The VS screen's models (the versus intro between the select / stage select and the fight): each side's model sits
// under Pos < Left (P1) / Right (P2) < FighterEmoteCtrl (the Color Mod's finding, rto_data vs_model_side); never a
// fight's (the intro's rigs hang under FighterVisual_0N, the fighters under nothing). Looked for from the end of the
// character select (90 s: the stage select comes between) or the start of a fighters watch (20 s: online, no select
// here), and let go when the screen's models are gone, 20 s after they were first seen at the latest, or 5 s after both
// fighters are built without a VS screen (Training): never into the fight. Online the player's own side wears P1's
// options (as in a fight).
// No visible swap (20/10: the base outfit showed a moment, then the option): the files of both sides' options asked
// for when the select ends (warm_vs; mostly loaded already, the select models wore them); the scene searched every
// frame for a second after any model's colours are set up (PlayerColorController.SetupColor, a hook that only counts:
// the VS models get theirs when built), else every 30 frames (every 5 without that hook); a model found is looked at
// every frame until it wears its options: swapped on the first frame its parts are ready (a part with a chain one
// frame later: its chain is stopped a frame before the swap, docs/RUNTIME_OPTIONS.md Part units).
uint32_t g_vs_until = 0, g_vs_scan = 0, g_vs_first = 0;
std::atomic<uint32_t> g_color_setups{ 0 };  // PlayerColorController.SetupColor calls (hook: counted only)
bool g_color_hook = false;
uint32_t g_vs_setups = 0, g_vs_burst = 0;
std::string g_vs_log;
std::set<std::string> g_vs_odd;       // Left / Right chains not under FighterEmoteCtrl, told once a session
int pre_color_setup(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_color_setups.fetch_add(1, std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// The files of the options the select's models wear (both sides), asked for before those models go: the VS screen's
// models then swap at once
void warm_vs() {
    size_t n = 0, fresh = 0;
    for (int i = 3; i <= 4; ++i) {
        const Model& md = g_models[i];
        if (!md.slot || md.side < 0) continue;
        for (auto& [t, w] : desired(g_m, *md.slot, chosen_of(g_m, *md.slot, g_c, md.side))) {
            if (w.orig || w.path.empty()) continue;
            if (!g_res.count(w.path)) ++fresh;
            res_get(w.path, w.kind, false);
            ++n;
        }
    }
    if (n) logf("rto: %zu files of the select models' options kept for the VS screen (%zu not loaded yet)", n, fresh);
}
void vs_open(uint32_t frames, const char* why) {
    if (!g_vs_until) {
        g_vs_scan = g_vs_first = g_fighters_at = 0;
        g_vs_setups = g_color_setups.load(std::memory_order_relaxed);
        g_vs_burst = 0;
        g_vs_log.clear();
        logf("rto: VS screen models looked for (%s)", why);
    }
    g_vs_until = std::max(g_vs_until, g_frame + frames);
}
void vs_close(const char* why) {
    if (!g_vs_until) return;
    g_vs_until = 0;
    reset(g_models[5]); reset(g_models[6]);
    logf("rto: VS screen models no longer followed (%s)", why);
}
void vs_tick() {
    if (!T.pcc_go || !T.tr_parent) return;
    Model* vm[2] = { &g_models[5], &g_models[6] };
    if (g_vs_first && g_frame - g_vs_first > 60 * 20) { vs_close("20 s after they came"); return; }
    if (!g_vs_first && g_fighters_at && g_frame - g_fighters_at > 300) { vs_close("both fighters built, no VS screen"); return; }
    if (!g_vs_first && g_frame >= g_vs_until) { vs_close("none came"); return; }
    const bool both = vm[0]->root && vm[1]->root;
    const uint32_t setups = g_color_setups.load(std::memory_order_relaxed);
    if (setups != g_vs_setups) { g_vs_setups = setups; g_vs_burst = g_frame + 60; }   // a model was just built
    const uint32_t every = both ? 120u : (g_frame < g_vs_burst ? 1u : g_color_hook ? 30u : 5u);
    if (g_frame - g_vs_scan >= every) {
        g_vs_scan = g_frame;
        Obj* found[2] = {};
        Obj* list = find_components(T.pcc);
        const int n = list_count(list);
        for (int i = 0; i < n; ++i) {
            Obj* c = list_item(list, i);
            Obj* go = c ? call_obj(T.pcc_go, c) : nullptr;
            std::vector<std::string> up;
            std::vector<Obj*> objs;
            Obj* tr = go ? call_obj(T.go_tr, go) : nullptr;
            for (int d = 0; go && d < 7; ++d) {
                up.push_back(str(call_obj(T.go_name, go)));
                objs.push_back(go);
                tr = tr ? call_obj(T.tr_parent, tr) : nullptr;
                go = tr ? call_obj(T.tr_go, tr) : nullptr;
            }
            int root = -1;
            const int side = vs_model_side(up, &root);
            if (side && !found[side - 1]) found[side - 1] = objs[static_cast<size_t>(root)];
            if (!side) {                               // a Left / Right elsewhere: told once (a layout the rule misses)
                std::string chain;
                bool lr = false;
                for (size_t k = 0; k < up.size(); ++k) { chain += (k ? " < " : "") + up[k]; lr = lr || (k < 5 && (up[k] == "Left" || up[k] == "Right")); }
                if (lr && g_vs_odd.size() < 8 && g_vs_odd.insert(chain).second)
                    logf("rto: VS screen: a colour controller under %s: not under FighterEmoteCtrl, not followed", chain.c_str());
            }
        }
        char b[120];
        snprintf(b, sizeof b, "%d colour controllers, P1 model %s, P2 model %s", n, found[0] ? "found" : "none",
                 found[1] ? "found" : "none");
        if (g_vs_log != b) { g_vs_log = b; logf("rto: VS screen: %s", b); }
        if ((found[0] || found[1]) && !g_vs_first) g_vs_first = g_frame;
        if (g_vs_first && !found[0] && !found[1]) { vs_close("the screen ended"); return; }
        for (int i = 0; i < 2; ++i) {
            if (found[i] == vm[i]->root) continue;
            reset(*vm[i]);
            vm[i]->root = found[i];
            vm[i]->seen = g_frame;
            if (found[i]) logf("rto: VS P%d model found (%s)", i + 1, g_frame < g_vs_burst ? "a model was just built" : "periodic search");
        }
    }
    // every frame (process waits for md.next / check_at itself): a part unit's chain stays stopped one frame only, a
    // model not ready yet is swapped on its first ready frame
    const bool net = costumes::online();
    for (int i = 0; i < 2; ++i) {
        Model& md = *vm[i];
        if (!md.root) continue;
        process(md, net ? 0 : i, true);
    }
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
    g_builds.fetch_add(1, std::memory_order_relaxed);
    if (!g_closing.load(std::memory_order_relaxed) && !g_model_noisy) { g_model_calls.fetch_add(1, std::memory_order_relaxed); post(EV_RTO_MODEL); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void fault(const char* where) {
    ++g_faults;
    logf("rto: EXCEPTION in %s (%d)", where, g_faults);
    for (auto& md : g_models) reset(md);
    if (g_faults >= 5) { g_disabled = true; select::leave_all("runtime options off"); logf("rto: 5 faults, runtime options off for this session"); }}

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
void fighters_refresh(const char* why, int hold_side) {
    if (!on() || g_select) return;
    // watch_tick: each fighter's model compares its choices version with g_ver (choices_changed bumped it) and wears
    // its side's choices again (pass); a rebuild that follows (the costume or colour changed too) extends the watch
    if (!g_watch_until) { logf("rto: fighters watch (%s)", why); g_watch_start = g_frame; }
    g_watch_until = g_frame + 600;
    g_watch_next = 0;
    if (hold_side == 0 || hold_side == 1) {
        g_hold_side = hold_side;
        ++g_hold_id;
        logf("rto: P%d's swap held until every file is ready (behind the pause menu, state %d)", hold_side + 1, menu_state_now());
    }
}
void fighters_hold_end(const char* why) {
    if (g_hold_side < 0) return;
    const Model& md = g_models[1 + g_hold_side];
    if (md.hold_id != g_hold_id)
        logf("rto: P%d's held swap: %s before every file was ready: swapped as each one is ready", g_hold_side + 1, why);
    g_hold_side = -1;
    if (g_watch_until && g_watch_until < g_frame + 600) g_watch_until = g_frame + 600;   // the rest as soon as ready
}
int training_menu_state() { return menu_state_now(); }
size_t popup_preload(const std::vector<const Slot*>& slots) {
    std::set<std::string> want;
    for (const Slot* s : slots) {
        if (!s) continue;
        size_t n = 0;
        auto add = [&](const File& f) {
            if (f.path.empty() || f.path == f.target || n >= kWarmPreview || !swappable(f.kind)) return;
            ++n;
            if (want.insert(f.path).second) res_get(f.path, f.kind, false);
        };
        for (auto& [id, fs] : s->files)
            if (!hidden(g_m, *s, id)) for (auto& f : fs) add(f);
        for (auto& f : s->base) add(f);
    }
    if (want != g_warm_pop) {
        if (want.empty()) logf("rto: the %zu files asked for by the Training popup's options no longer kept", g_warm_pop.size());
        else logf("rto: %zu files of the Training popup's options asked for ahead", want.size());
    }
    g_warm_pop = std::move(want);
    return g_warm_pop.size();
}
int options_files_left(const Slot& s, const std::vector<std::string>& chosen, int* total) {
    return files_left(desired(g_m, s, chosen), total);
}
const Slot* outfit_slot(int fighter, int costume_no) {
    if (!on()) return nullptr;
    const Slot* s = g_m.slot_of(fighter, costume_no);
    return s && has_rows(s) ? s : nullptr;
}
int preview_since() {
    const Model& md = g_models[0];
    return (md.root && md.slot && has_rows(md.slot)) ? static_cast<int>(md.since) : -1;
}
int preview_fighter(uint32_t* at) {
    if (at) *at = g_prev_fighter_at;
    return g_prev_fighter;
}
const Slot* select_slot(int side, uint32_t* since) {
    if (side < 1 || side > 2 || !g_select) return nullptr;
    const Model& md = g_models[2 + side];
    if (!(md.root && md.slot && has_rows(md.slot) && alive_as(md.root, T.go))) return nullptr;
    if (since) *since = md.since;
    return md.slot;
}
const Slot* preview_slot() {
    const Model& md = g_models[0];
    return (md.root && md.slot && has_rows(md.slot)) ? md.slot : nullptr;
}

void init() {
    g_models[0].name = "preview"; g_models[1].name = "P1"; g_models[2].name = "P2";
    g_models[3].name = "select P1"; g_models[4].name = "select P2"; g_models[5].name = "VS P1"; g_models[6].name = "VS P2";
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
        g_m.show_nsfw = true;                     // NSFW options count here; without them: g_no_rows_off
        if (rows(g_m, s, s.defaults).empty()) {
            g_no_rows.insert(&s);
            logf("rto: %s/%s: no option can change live, no options menu", s.fighter_dir.c_str(), s.folder.c_str());
        }
        g_m.show_nsfw = false;
        if (g_m.has_nsfw && !g_no_rows.count(&s) && rows(g_m, s, s.defaults).empty()) {
            g_no_rows_off.insert(&s);
            logf("rto: %s/%s: NSFW options only: no options menu while Display NSFW Outfits is off", s.fighter_dir.c_str(),
                 s.folder.c_str());
        }
    }
    size_t opts = 0, nsfw = 0;
    for (auto& m : g_m.mods) { opts += m.options.size(); for (auto& o : m.options) nsfw += o.nsfw ? 1 : 0; }
    if (nsfw) logf("rto: %zu NSFW options (offered and worn only while Display NSFW Outfits is on)", nsfw);
    logf("rto: %zu modular mods, %zu options, %zu slots (layout %s, generated %s)", g_m.mods.size(), opts, g_m.slots.size(),
         g_m.layout.c_str(), g_m.generated.c_str());
}

int migrate_former(int fighter, int costume_no, bool* game) {
    if (!g_on) return -1;
    const Former* f = nullptr;
    const Slot* k = former_of(g_m, fighter, costume_no, &f);
    if (!k || !f) return -1;
    if (game) *game = k->game;
    auto& sd = g_c.mods[k->mod][0];
    sd.changed = true;
    sd.ids = f->ids;
    sd.names.clear();
    g_migrated = true;
    logf("rto: [F%d] Battle Settings outfit %d (%s) is now outfit %d (%s%s) with that variant's options", fighter, costume_no,
         f->label.c_str(), k->costume_no, k->name.c_str(), k->game ? ", the game's" : "");
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
    g_color_hook = hook_all(T.pcc, "SetupColor", pre_color_setup, nullptr) > 0;   // VS screen models: found when built
    crf_hooks();
    menu::install();
    select::install_popup_hooks();
}

bool busy() {
    if (!on()) return false;
    if (g_bs || g_select || g_watch_until || g_vs_until || menu::busy() || select::popup_busy()) return true;
    return !g_warm_done && g_frame < 60 * 120;
}

bool nsfw_options() { return on() && g_m.has_nsfw; }

void tick(uint32_t ev) {
    if (!on() || !T.ok) return;
    try {
        // "Display NSFW Outfits" (costumes.cpp reads settings.json): NSFW options offered and worn, or not; the models
        // apply it at once (the preview now, fighters at their next build)
        if (g_m.has_nsfw && costumes::display_nsfw() != g_m.show_nsfw) {
            g_m.show_nsfw = costumes::display_nsfw();
            logf("rto: NSFW options %s", g_m.show_nsfw ? "shown" : "hidden");
            choices_changed();
        }
        if (ev & EV_SELECT_START) {
            select::leave_all("character select started");   // a visit whose end was missed: its panels let go
            vs_close("character select started");
            g_select = true; g_watch_until = 0; g_sel_scan = g_sel_next = 0; g_sel_start = g_frame; g_sel_log.clear();
            for (auto& md : g_models) reset(md);
        }
        if ((ev & EV_SELECT_END) && g_select) {
            g_select = false;
            select::leave_all("character select ended");
            warm_vs();
            reset(g_models[3]); reset(g_models[4]);
            vs_open(60 * 90, "character select ended");
        }
        if (ev & EV_MENU_OPEN) {
            if (!g_bs || g_bs_closed) logf("rto: Battle Settings open: its preview is followed");
            g_bs = true; g_bs_closed = false; g_bs_seen = g_frame;
        }
        if ((ev & EV_MENU_CLOSE) && g_bs && !g_bs_closed) { g_bs_closed = true; g_bs_seen = g_frame; }
        if (ev & EV_HOLDER) ++g_holders;
        if ((ev & (EV_HOLDER | EV_RTO_MODEL)) && !g_select) {
            if (!g_watch_until) {
                if (costumes::select_active()) {                 // the select screen: its own models (select_tick)
                    g_select = true; g_sel_scan = g_sel_next = 0; g_sel_start = g_frame; g_sel_log.clear();
                    return;
                }
                logf("rto: fighters watch");
                g_watch_start = g_frame;
                vs_open(60 * 20, "fighters watch");
            }
            if ((ev & EV_HOLDER) || g_frame - g_watch_start < 60 * 30) g_watch_until = g_frame + 600;
            else if (!g_model_noisy) {
                g_model_noisy = true;
                logf("rto: PlayerBehavior.SetupAppearance is called all the time: not a build event, ignored from now on");
            }
        }
        if (g_select) { select::tick(); select_tick(); return; }
        if ((ev & EV_POPUP) || select::popup_busy()) select::popup_tick((ev & EV_POPUP) != 0);   // Training pause menu
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
                g_gone_at = 0;
                g_prev_fighter = -1;
                warm_preview(nullptr);
                res_gc();
            }
        }
        menu::tick(fs);
        if (g_bs || menu::active()) preview_tick();
        if (g_watch_until) watch_tick();
        if (g_vs_until) vs_tick();
        if (!g_warm_done && g_frame > 60 * 30 && !g_watch_until && !g_bs) warm();
    } catch (...) {
        fault("tick");
    }
}

} // namespace slots::rto
