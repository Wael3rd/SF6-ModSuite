// SF6 Slots — native plugin: stage slots.
//
// On the stage select screen, UP / DOWN cycles the focused stage between its vanilla look and the
// stage mods installed for it. The loader (amd_ags_x64.dll) serves the chosen variant's files when
// the stage loads; this module keeps the choice (data/SF6_StageSlots_Data/state.json, which the
// loader watches) and shows it: stage name between UP / DOWN hints, preview, the focused stage's cell
// of the stage strip (core 0.8.1+), VS screen.
//
// Events: the stage select screen shown / hidden, focus changed or name and preview rewritten by the
// game, UP / DOWN on it, the VS screen shown or rewritten. Without stage mods: no hook at all.
//
// The stage grids (Battle Settings > Other > Stage Settings, Training's Change Stage: both are app.UIPartsStageSelectSimple,
// read 05/11 from the game's code) get the TILES of registry.json as entries of their own and cycle a vanilla entry's
// VARIATIONS with a pair of keys (stages.hpp, stages_logic.hpp; section "Stage grids" below). Each menu keeps its own
// choice per stage, applied when that menu's stage loads (section "Per-menu choice"). The game only ever holds vanilla
// ids: a tile entry is the vanilla id of its stage once more in the grid's list, so the save data, the opponent, the
// server and the replay files see the vanilla stage; only this PC loads the tile.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "perf.hpp"
#include "wjournal.hpp"
#include "stages.hpp"
#include "stages_logic.hpp"
#include "codecheck_rt.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "user32.lib")   // MapVirtualKeyW (keyboard hints)

namespace slots::stages {
namespace {

const char* kRegistry = "reframework/data/SF6_StageSlots_Data/registry.json";
const char* kState    = "reframework/data/SF6_StageSlots_Data/state.json";

struct Variant { std::string key, name, bundle, preview, thumb; bool tile = false; };
std::map<uint32_t, std::vector<Variant>> g_variants;      // stage id -> variants
size_t g_variant_count = 0;
// The choices: what the loader serves and each menu's own (state.json; stages_logic.hpp). Game thread and hooks: g_mx.
std::recursive_mutex g_mx;
stagelogic::State g_state;
stagelogic::Choice g_applied;                              // what the loader was last told to serve
std::map<uint32_t, std::string>& g_selected = g_state.menu[stagelogic::kClassic];   // the classic screen's (stage id -> key)
std::vector<stagelogic::TileRef> g_tiles;                  // every tile, registry order (the stage grids' extra entries)
std::map<std::string, uint32_t> g_ess_stage;               // "ess0000_00" (lower case) -> stage id, stages with variants

// ---- written by hooks ----
std::atomic<uintptr_t> g_hook_param{0};                    // UIFlowStageSelect.Param shown
std::atomic<uintptr_t> g_stage_agent{0};                   // StageSelect UI agent (input filter)
std::atomic<int> g_pending{0};                             // UP / DOWN presses
std::atomic<uintptr_t> g_vs_comp{0};                       // VSInfoOffline activated

// ---- game thread ----
struct Screen {
    bool active = false;
    uint32_t check_until = 0;             // after a show / hide event: whether the screen is visible
    Obj* agent = nullptr;
    Obj* param = nullptr;
    uint32_t stage = UINT32_MAX;          // focused
    uint32_t decided = UINT32_MAX;        // last focused: the stage fought
    int settle = 0;                       // frames to wait after the game rewrote name / image
    bool need = false;
    std::map<uint32_t, std::string> vanilla_name, written;
    std::map<uint32_t, bool> applied;
} g_scr;

struct Vs {
    uint32_t until = 0;                   // after the game's last write on the VS screen (frames)
    uint32_t cap = 0;                     // hard end of the window
    uint32_t next = 0;
    Obj* text = nullptr; Obj* tex = nullptr; uintptr_t comp = 0;
} g_vs;

std::unordered_set<std::string> g_ours;   // every text written here: never the game's own name

struct Preview { Obj* holder = nullptr; uint32_t frame = 0; bool failed = false; };
std::map<std::string, Preview> g_previews;
constexpr uint32_t kLoadFrames = 60;      // a texture handed to the GUI while it loads crashed the game
uint32_t g_preview_wait = 0;              // a preview is loading: frame when the last one is ready

void strip_open();                        // stage strip (below): look at the cells again for a while
Obj* find_child(Obj* ctrl, const char* name, int depth);
void strip_restore();                     // the game's images back on the cells, references released

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

void load_registry() {
    Json j;
    std::string text = read_file(kRegistry);
    if (text.empty() || !json_parse(text, j)) return;
    const Json* st = j.get("stages");
    if (!st || st->kind != Json::Array) return;
    for (auto& s : st->arr) {
        const Json* id = s.get("stage_id"); const Json* vs = s.get("variants");
        if (!id || !vs || vs->kind != Json::Array || vs->arr.empty()) continue;
        auto& list = g_variants[static_cast<uint32_t>(id->as_int())];
        if (auto* e = s.get("ess")) {
            std::string code = e->as_str();
            for (auto& c : code) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (!code.empty()) g_ess_stage[code] = static_cast<uint32_t>(id->as_int());
        }
        for (auto& v : vs->arr) {
            Variant x;
            if (auto* k = v.get("key")) x.key = k->as_str();
            if (auto* n = v.get("name")) x.name = n->as_str();
            if (auto* b = v.get("bundle")) x.bundle = b->as_str();
            if (auto* p = v.get("preview")) x.preview = p->as_str();
            if (auto* t = v.get("thumb")) x.thumb = t->as_str();
            if (auto* k = v.get("kind")) x.tile = k->as_str() == "tile";
            if (!x.key.empty()) { list.push_back(x); ++g_variant_count; }
        }
    }
}

// The tiles in registry order (the grids' extra entries)
void collect_tiles() {
    g_tiles.clear();
    for (auto& [id, vars] : g_variants) for (auto& v : vars) if (v.tile) g_tiles.push_back({ id, v.key });
}

void load_state() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    stagelogic::parse_state(read_file(kState), g_state);   // an older file: its "selected" is the classic screen's choice
    g_applied = g_state.active;                            // what the loader read at its start
}

// The loader's SF6StageSlots_Apply (amd_ags_x64.dll, stage_redirect.hpp): the selection served at once, not a few ms later
// when its watcher reads state.json. Absent (older loader): state.json alone.
using ApplyFn = int (*)(const char*);
ApplyFn loader_apply() {
    static ApplyFn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) fn = reinterpret_cast<ApplyFn>(GetProcAddress(h, "SF6StageSlots_Apply"));
        logf("stages: loader's SF6StageSlots_Apply %s", fn ? "found: a menu's choice is served at once" : "not found (older loader): state.json only");
    }
    return fn;
}

// The only writer of state.json. apply_menu >= 0: that menu's choice becomes what the loader serves (told at once through
// its export when it changed); -1: the menus' choices are saved, the served selection stays. Any thread (g_mx).
void save_state(int apply_menu = stagelogic::kClassic, const char* why = nullptr) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (apply_menu >= 0 && apply_menu < stagelogic::kMenus) g_state.active = g_state.menu[apply_menu];
    const std::string o = stagelogic::state_json(g_state);
    { int64_t pt = perf::begin(); if (!write_file(kState, o)) logf("stages: cannot write state.json"); perf::end(perf::StateWrite, pt); }
    if (apply_menu < 0 || g_state.active == g_applied) return;
    g_applied = g_state.active;
    std::string what;
    for (auto& [id, key] : g_applied) what += (what.empty() ? "" : ", ") + std::to_string(id) + " -> " + key;
    int r = -1;
    if (ApplyFn fn = loader_apply()) r = fn(o.c_str());
    logf("stages: %s's choice served%s%s: %s%s", stagelogic::menu_key(apply_menu), why ? " (" : "", why ? why : "",
         what.empty() ? "vanilla everywhere" : what.c_str(), r < 0 ? "" : r == 0 ? " [loader: not running]" : " [loader told]");
}

// 0 = vanilla, i = variants[i - 1]
int selected_index(uint32_t stage) {
    auto s = g_selected.find(stage);
    auto v = g_variants.find(stage);
    if (s == g_selected.end() || v == g_variants.end()) return 0;
    for (size_t i = 0; i < v->second.size(); ++i) if (v->second[i].key == s->second) return static_cast<int>(i) + 1;
    return 0;
}

void set_selected(uint32_t stage, int idx) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto v = g_variants.find(stage);
    if (idx == 0 || v == g_variants.end()) g_selected.erase(stage);
    else g_selected[stage] = v->second[static_cast<size_t>(idx - 1)].key;
    save_state(stagelogic::kClassic);
    logf("stages: stage %u -> %s", stage, idx == 0 ? "vanilla" : v->second[static_cast<size_t>(idx - 1)].name.c_str());
}

// ---------------------------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------------------------

// Ported to slots::ui (SL-13): compare-before-write handled by the core / legacy.
void set_text(Obj* text_obj, const std::string& s) {
    if (wjournal::on()) wjournal::log("UI_TEXT obj=%p text=%.80s", (void*)text_obj, s.c_str());
    slots::ui::set_text(text_obj, s.c_str());
}

// Ported to slots::ui (SL-14): compare-before-write handled by the core / legacy.
void set_texture(Obj* tex_obj, Obj* holder) {
    if (!tex_obj || !holder) return;
    if (wjournal::on()) wjournal::log("UI_TEX obj=%p holder=%p", (void*)tex_obj, (void*)holder);
    slots::ui::set_tex(tex_obj, holder);
}

// Created once and kept for the session; ported to slots::ui (SL-14).
// The core handles the 60-frame load wait via ui_texture_ready; the legacy
// path tracks it internally.  g_preview_wait is still set for the busy() signal.
Obj* preview_holder(const std::string& path) {
    if (path.empty()) return nullptr;
    auto& p = g_previews[path];
    if (!p.holder && !p.failed) {
        p.failed = true;
        Obj* h = slots::ui::tex_load(path.c_str());
        if (h) { p.holder = h; p.frame = g_frame; p.failed = false; g_preview_wait = std::max(g_preview_wait, g_frame + kLoadFrames); }
        if (p.failed) logf("stages: preview %s not created", path.c_str());
    }
    if (!p.holder) return nullptr;
    if (!slots::ui::tex_ready(p.holder)) return nullptr;
    return p.holder;
}

void preload_previews(uint32_t stage) {
    auto v = g_variants.find(stage);
    if (v == g_variants.end()) return;
    for (auto& x : v->second) {
        preview_holder(x.preview);
        if (!x.thumb.empty()) preview_holder(x.thumb);              // the strip cell's thumbnail, loaded ahead too
    }
}

Obj* vanilla_holder(Obj* param, uint32_t stage) {
    Obj* cache = get_obj(param, "PreviewTexCahceDataList");
    int n = list_count(cache);
    for (int i = 0; i < n; ++i) {
        Elem c = list_elem(cache, i);
        uint32_t id = 0;
        if (c.u32_field("StageId", id) && id == stage) return c.obj_field("Texture");
    }
    return nullptr;
}

// UP / DOWN hint around the name, the game's way for a value cycled with two inputs (its BGM
// selector shows "[A] Random [E]"): symbols of UISelectU / UISelectD for the device in use
constexpr int kSelectU = 65, kSelectD = 66, kModeKeyboard = 2;
std::map<int, std::pair<std::string, std::string>> g_symbols;

// Keyboard: the game's symbols of UISelectU / UISelectD name the keys of a QWERTY layout (W ... S on an
// AZERTY keyboard, where Z moves up), so the hints show the menus' default physical keys (scan codes
// 0x11 / 0x1F) in the layout in use, read again every 120 frames (the layout can change while the game
// runs). A key naming no letter or digit: the game's symbols. Pad: the game's symbols.
constexpr UINT kScanU = 0x11, kScanD = 0x1F;
struct { uint32_t at = 0; bool set = false; std::string up, down; } g_keys;
std::string key_symbol(UINT scan) {
    const UINT vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK);
    if (!((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))) return {};
    char b[16];
    std::snprintf(b, sizeof b, "<ICON Key%c>", static_cast<char>(vk));
    return b;
}

std::string decorate(const std::string& name) {
    auto* gm = api().get_managed_singleton("app.InputGuideManager");
    int mode = kModeKeyboard;
    if (Obj* modes = gm ? get_obj(gm, "_Modes") : nullptr) {
        auto r = call(modes, "get_Item", { arg_i32(0) });
        if (!r.exception_thrown && static_cast<int32_t>(r.dword) > 0) mode = static_cast<int32_t>(r.dword);
    }
    if (mode == kModeKeyboard) {
        if (!g_keys.set || g_frame - g_keys.at >= 120) {
            std::string up = key_symbol(kScanU), down = key_symbol(kScanD);
            if (!g_keys.set || up != g_keys.up || down != g_keys.down)
                logf("stages: keyboard hints %s / %s (scan codes 0x11 / 0x1F)", up.empty() ? "-" : up.c_str(), down.empty() ? "-" : down.c_str());
            g_keys.up = up; g_keys.down = down; g_keys.at = g_frame; g_keys.set = true;
        }
        if (!g_keys.up.empty() && !g_keys.down.empty()) return g_keys.up + " " + name + " " + g_keys.down;
    }
    auto it = g_symbols.find(mode);
    if (it == g_symbols.end() && gm) {
        Method* m = method_of(gm->get_type_definition(), "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)", 4);
        std::string up = str(call_obj(m, gm, { arg_i32(kSelectU), arg_i32(mode), arg_i32(0), arg_i32(0) }));
        std::string down = str(call_obj(m, gm, { arg_i32(kSelectD), arg_i32(mode), arg_i32(0), arg_i32(0) }));
        it = g_symbols.emplace(mode, std::make_pair(up, down)).first;
    }
    if (it == g_symbols.end() || (it->second.first.empty() && it->second.second.empty())) return name;
    return it->second.first + " " + name + " " + it->second.second;
}

// A variant named like the stage itself (a lighting pack: "Bather's Beach") shows its bundle name
std::string norm(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
}
std::string display_name(uint32_t stage, const Variant& v) {
    auto vn = g_scr.vanilla_name.find(stage);
    if (vn != g_scr.vanilla_name.end() && !v.bundle.empty() && norm(v.name) == norm(vn->second)) return v.bundle;
    return v.name;
}

void write_name(Obj* text0, uint32_t stage, const std::string& name) {
    std::string s = decorate(name);
    g_ours.insert(s);
    set_text(text0, s);
    g_scr.written[stage] = s;
}

// The game's own name of a stage: read once the game has written it, never one of ours
void learn_vanilla_name(Obj* text0, uint32_t stage) {
    std::string cur = str(call_obj(text0, "get_Message"));
    if (cur.empty() || g_ours.count(cur)) return;
    g_scr.vanilla_name[stage] = cur;
}

// Shows the selected variant of the focused stage (or puts the game's own image back). false while
// its preview is still loading.
bool apply(Obj* param, uint32_t stage) {
    Obj* text0 = get_obj(param, "text0");
    Obj* tex0 = get_obj(param, "texture0");
    if (!text0 || !tex0) return true;
    learn_vanilla_name(text0, stage);
    int idx = selected_index(stage);
    if (idx == 0) {
        if (g_scr.applied[stage]) { set_texture(tex0, vanilla_holder(param, stage)); g_scr.applied[stage] = false; }
        auto vn = g_scr.vanilla_name.find(stage);
        if (vn != g_scr.vanilla_name.end()) write_name(text0, stage, vn->second);
        return true;
    }
    const Variant& v = g_variants[stage][static_cast<size_t>(idx - 1)];
    write_name(text0, stage, display_name(stage, v));
    bool ready = true;
    if (!v.preview.empty()) {
        Obj* h = preview_holder(v.preview);
        if (h) set_texture(tex0, h); else ready = false;
    } else set_texture(tex0, vanilla_holder(param, stage));
    g_scr.applied[stage] = true;
    return ready;
}

// ---------------------------------------------------------------------------------------------
// Stage select screen
// ---------------------------------------------------------------------------------------------

Obj* find_agent(const char* go_name) {
    auto* mgr = api().get_managed_singleton("app.UIAgentManager");
    Obj* list = mgr ? get_obj(mgr, "_Entries") : nullptr;
    int n = list_count(list);
    for (int i = 0; i < n; ++i) {
        Obj* agent = list_elem(list, i).obj_field("Agent");
        Obj* go = agent ? call_obj(agent, "get_GameObject") : nullptr;
        if (go && str(call_obj(go, "get_Name")) == go_name) return agent;
    }
    return nullptr;
}

Obj* find_stage_param() {
    auto* td = api().tdb()->find_type("app.battle.bBattleStageSelectFlow");
    Obj* comps = find_components(td);
    Obj* flow = list_count(comps) > 0 ? list_item(comps, 0) : nullptr;
    return flow ? get_obj(flow, "mStageSelect") : nullptr;
}

bool focused_stage(Obj* param, uint32_t& out) {
    Obj* list = call_obj(param, "get_StageIdList");
    auto ri = call(param, "GetSelectIndex");
    if (!list || ri.exception_thrown) return false;
    int idx = static_cast<int32_t>(ri.dword);
    if (idx < 0 || idx >= list_count(list)) return false;
    auto r = call(list, "get_Item", { arg_i32(idx) });
    if (r.exception_thrown) return false;
    out = r.dword;
    return true;
}

// The agent's main control is on screen
bool shown(Obj* agent) {
    if (!alive(agent)) return false;
    Obj* cm = call_obj(agent, "get_ControlMain");
    return cm && call_bool(cm, "get_ActualVisible");
}

void screen_shown(Obj* agent) {
    g_scr.active = true;
    Obj* p = reinterpret_cast<Obj*>(g_hook_param.load());
    g_scr.param = alive(p) ? p : find_stage_param();
    g_stage_agent.store(reinterpret_cast<uintptr_t>(agent));
    g_symbols.clear();                                     // key bindings may have changed
    g_scr.stage = UINT32_MAX;
    g_scr.applied.clear(); g_scr.written.clear();
    g_scr.need = true; g_scr.settle = 2;
    g_pending.store(0);
    strip_restore();                                       // a visit that ended without its hide
    strip_open();
    if (g_state.active != g_selected) save_state(stagelogic::kClassic, "the classic stage select is shown");   // what its battle loads (as before)
    logf("stages: stage select shown (param %s, agent %s)", g_scr.param ? "ok" : "-", agent ? "ok" : "-");
}

void screen_hidden() {
    logf("stages: stage select hidden");
    strip_restore();
    g_scr.active = false;
    g_scr.param = nullptr;
    g_stage_agent.store(0);
    g_pending.store(0);
    // the VS screen comes next: its image starts loading now
    if (g_scr.decided != UINT32_MAX && selected_index(g_scr.decided)) preload_previews(g_scr.decided);
}

void screen_work(bool input) {
    perf::Scope ps(perf::StgScreen);
    Obj* param = g_scr.param;
    if (!alive(param)) { param = find_stage_param(); g_scr.param = param; if (!param) return; }
    uint32_t stage = 0;
    if (!focused_stage(param, stage)) return;
    g_scr.decided = stage;
    if (stage != g_scr.stage) {
        g_scr.stage = stage;
        g_scr.applied.erase(stage); g_scr.written.erase(stage);   // the game rewrites the name and the image
        g_scr.settle = 2;
        preload_previews(stage);
    }
    int pending = g_pending.exchange(0);
    if (g_scr.settle > 0) { --g_scr.settle; g_scr.need = true; return; }   // presses during a change are dropped
    auto v = g_variants.find(stage);
    if (v == g_variants.end()) { g_scr.need = false; return; }
    if (input && pending) {
        int n = static_cast<int>(v->second.size()) + 1;
        int idx = ((selected_index(stage) + pending) % n + n) % n;
        set_selected(stage, idx);
    }
    g_scr.need = !apply(param, stage);                    // again next frame while the preview loads
}

// ---------------------------------------------------------------------------------------------
// VS screen (VSInfoOffline): stage name c_bg/e_text_stagename, stage image c_bg/e_texture_bg
// ---------------------------------------------------------------------------------------------

Obj* find_child(Obj* ctrl, const char* name, int depth) {
    if (!ctrl || depth > 8) return nullptr;
    for (Obj* c = call_obj(ctrl, "get_Child"); c; c = call_obj(c, "get_Next")) {
        if (str(call_obj(c, "get_Name")) == name) return c;
        if (Obj* hit = find_child(c, name, depth + 1)) return hit;
    }
    return nullptr;
}

// false while something is left to show (control not found yet, image loading)
bool vs_work() {
    perf::Scope ps(perf::StgVs);
    uint32_t stage = g_scr.decided;
    int idx = stage != UINT32_MAX ? selected_index(stage) : 0;
    if (idx == 0) return true;
    uintptr_t comp = g_vs_comp.load();
    if (!g_vs.text || comp != g_vs.comp || !alive(g_vs.text)) {
        g_vs.comp = comp;
        Obj* agent = find_agent("VSInfoOffline");
        Obj* cm = agent ? call_obj(agent, "get_ControlMain") : nullptr;
        Obj* bg = cm ? find_child(cm, "c_bg", 0) : nullptr;
        g_vs.text = bg ? find_child(bg, "e_text_stagename", 0) : nullptr;
        g_vs.tex = bg ? find_child(bg, "e_texture_bg", 0) : nullptr;
        if (!g_vs.text) return false;
    }
    const Variant& v = g_variants[stage][static_cast<size_t>(idx - 1)];
    set_text(g_vs.text, display_name(stage, v));
    if (g_vs.tex && !v.preview.empty()) {
        Obj* h = preview_holder(v.preview);
        if (!h) return false;
        set_texture(g_vs.tex, h);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Stage strip (ADR-0019, core 0.8.1+): the focused stage's cell shows the selected variant
// ---------------------------------------------------------------------------------------------
// The strip is a looping list (c_stage/p_ScrollList_0_h_loop): its cells, p_FGStageSelectListItem_*, are
// reused as it scrolls, each showing a stage through a via.gui.Material (e_mat_thumb, texture variable
// "tex"). A cell's stage is read from the image it shows: one of our thumbnails (its variant's stage),
// or the game's thumbnail of a stage, whose path names the stage (tex_StageThumb_ess0000_00_IM). A cell
// whose image names no single stage is left alone. The focused stage's cells show the selected variant's
// thumbnail (cut from its screenshot by the loader; a variant without one leaves the game's image); every other cell, a vanilla selection and the screen's end put the
// game's image back (the holder read from the cell before ours was set). After a show, a focus change, a
// game rewrite or UP / DOWN, the cells are looked at every 5 frames for 2 s (the list scrolls, the game
// rewrites them): no polling otherwise.

const char* kCellPrefix = "p_FGStageSelectListItem_";
const char* kStageImage = "tex";            // the cell material's thumbnail variable (mt_simplemask; "tex_mask" is the cell mask)

struct Cell {
    Obj* ctrl = nullptr;
    Obj* mat = nullptr;                   // e_mat_thumb
    std::string name;
    Obj* vanilla = nullptr;               // the game's image last seen on the cell: one reference of ours
    uint32_t stage = UINT32_MAX;          // the stage of that image
    std::string shown;                    // variant key we set ("" = the game's image)
};
struct Strip {
    std::vector<Cell> cells;
    bool scanned = false;
    uint32_t scanned_at = 0;
    uint32_t until = 0, cap = 0, next = 0;
    bool logged = false;                  // cell -> stage mapping logged this visit
    bool warned = false;                  // strip not found, logged this visit
} g_strip;
bool g_strip_off = false;                 // core without the op: logged once

void drop(Obj*& h) { if (h) { h->release(); h = nullptr; } }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// The stage a cell's image belongs to (UINT32_MAX: none, or not one of the stages with variants). ours: one
// of our previews. code: the variant key or the stage code the path names (the path itself when none), for the log.
uint32_t stage_of_image(Obj* holder, bool& ours, std::string& code) {
    ours = false; code.clear();
    for (auto& [stage, list] : g_variants)
        for (auto& v : list) {
            auto p = g_previews.find(v.thumb);
            if (!v.thumb.empty() && p != g_previews.end() && p->second.holder == holder) { ours = true; code = v.key; return stage; }
        }
    const std::string path = lower(str(call_obj(holder, "get_ResourcePath")));
    for (auto& [stage, list] : g_variants)
        for (auto& v : list)
            if (path.find("_stageslots/" + lower(v.key) + "/") != std::string::npos) { ours = true; code = v.key; return stage; }
    // ess####_## : every code the path names must be the same one
    auto dig = [&](size_t i) { return i < path.size() && path[i] >= '0' && path[i] <= '9'; };
    bool many = false;
    for (size_t i = path.find("ess"); i != std::string::npos; i = path.find("ess", i + 1)) {
        if (!(dig(i + 3) && dig(i + 4) && dig(i + 5) && dig(i + 6) && i + 7 < path.size() && path[i + 7] == '_' && dig(i + 8) && dig(i + 9))) continue;
        std::string c = path.substr(i, 10);
        if (code.empty()) code = c; else if (c != code) many = true;
    }
    if (code.empty() || many) { code = path.empty() ? "no path" : path; return UINT32_MAX; }
    auto it = g_ess_stage.find(code);
    return it != g_ess_stage.end() ? it->second : UINT32_MAX;
}

// A control found earlier by find_child(root, name, 0), used again while it still is what that search would find: alive,
// still named so, still under root within find_child's depth (its parents climbed). Each find_child of the strip walks
// the screen's GUI tree with three game calls per control (28/10: 3-4 ms on the frame the strip was first looked at,
// at every visit of the stage select); the same controls live from one visit to the next.
bool still_under(Obj* c, Obj* root, const char* name) {
    if (!c || !root || !alive(c) || str(call_obj(c, "get_Name")) != name) return false;
    Obj* p = c;
    for (int up = 0; up < 10 && p; ++up) {
        p = call_obj(p, "get_Parent");
        if (p == root) return true;
    }
    return false;
}
struct Found { Obj* root = nullptr; Obj* c = nullptr; };
std::map<std::string, Found> g_found;                  // name -> the last hit of find_child under a root (strip, list)
std::map<Obj*, Obj*> g_cell_mat;                       // a cell -> its e_mat_thumb
Obj* find_child_kept(Obj* root, const char* name) {
    auto it = g_found.find(name);
    if (it != g_found.end() && it->second.root == root && still_under(it->second.c, root, name)) return it->second.c;
    Obj* c = find_child(root, name, 0);
    g_found[name] = { root, c };
    return c;
}

void strip_collect(Obj* ctrl, int depth, std::vector<Cell>& out) {
    if (!ctrl || depth > 10) return;
    for (Obj* c = call_obj(ctrl, "get_Child"); c; c = call_obj(c, "get_Next")) {
        std::string n = str(call_obj(c, "get_Name"));
        if (n.compare(0, strlen(kCellPrefix), kCellPrefix) == 0 || n.compare(0, 4, "item") == 0) {
            Cell x;
            x.ctrl = c; x.name = n;
            auto k = g_cell_mat.find(c);
            if (k != g_cell_mat.end() && still_under(k->second, c, "e_mat_thumb")) x.mat = k->second;
            else { x.mat = find_child(c, "e_mat_thumb", 0); g_cell_mat[c] = x.mat; }
            if (x.mat) out.push_back(std::move(x));
        } else strip_collect(c, depth + 1, out);
    }
}

// The strip's cells; what was learnt of those still there is kept
bool strip_scan() {
    perf::Scope ps(perf::StgStripScan);
    g_strip.scanned = true;
    g_strip.scanned_at = g_frame;
    Obj* cm = alive(g_scr.agent) ? call_obj(g_scr.agent, "get_ControlMain") : nullptr;
    // the list sits next to c_stage under the screen's main control (seen in game 16/10): its cells are item1..itemN
    Obj* list = cm ? find_child_kept(cm, "p_ScrollList_0_h_loop") : nullptr;
    std::vector<Cell> found;
    if (list) strip_collect(list, 0, found);
    for (auto it = g_cell_mat.begin(); it != g_cell_mat.end();) {    // the cells of this list only
        bool here = false;
        for (auto& f : found) if (f.ctrl == it->first) { here = true; break; }
        it = here ? std::next(it) : g_cell_mat.erase(it);
    }
    for (auto& f : found)
        for (auto& o : g_strip.cells)
            if (o.ctrl == f.ctrl && o.mat == f.mat) { f.vanilla = o.vanilla; o.vanilla = nullptr; f.stage = o.stage; f.shown = o.shown; break; }
    for (auto& o : g_strip.cells) drop(o.vanilla);
    g_strip.cells = std::move(found);
    if (g_strip.cells.empty() && !g_strip.warned) {
        g_strip.warned = true;
        Obj* root = cm ? find_child(cm, "c_stage", 0) : nullptr;   // for this line only
        logf("stages: stage strip not found (c_stage %s, p_ScrollList_0_h_loop %s): its cells keep the game's images",
             root ? "ok" : "-", list ? "ok" : "-");
    }
    return !g_strip.cells.empty();
}

// false while something is left to do (strip not built yet, a preview loading)
bool strip_work() {
    perf::Scope ps(perf::StgStrip);
    if (!slots::ui::has_mat_tex()) {
        if (!g_strip_off) { g_strip_off = true; logf("stages: the core has no ui_set_material_texture (core 0.8.1+): the stage strip shows the game's images"); }
        return true;
    }
    const uint32_t focused = g_scr.stage;
    if (focused == UINT32_MAX || g_scr.settle > 0) return false;   // the game is writing the screen
    // the selected variants' thumbnails, loaded ahead (each cell shows its own stage's selected variant)
    for (auto& [stage, key] : g_selected) {
        const int si = selected_index(stage);
        if (si && !g_variants[stage][static_cast<size_t>(si - 1)].thumb.empty()) preview_holder(g_variants[stage][static_cast<size_t>(si - 1)].thumb);
    }
    bool rescan = !g_strip.scanned || (g_strip.cells.empty() && g_frame - g_strip.scanned_at >= 30);
    for (auto& c : g_strip.cells) if (!alive(c.ctrl) || (c.mat && !alive(c.mat))) rescan = true;
    if (rescan && !strip_scan()) return false;
    bool done = true, identified = false;
    std::string map;
    for (auto& c : g_strip.cells) {
        std::string what = "? (no e_mat_thumb)";
        int status = 0;
        Obj* cur = c.mat ? slots::ui::get_mat_tex(c.mat, kStageImage, &status) : nullptr;
        bool ours = false;
        if (cur) {
            std::string code;
            uint32_t id = stage_of_image(cur, ours, code);
            if (ours) cur->release();
            else {
                if (cur != c.vanilla) { drop(c.vanilla); c.vanilla = cur; } else cur->release();
                c.stage = id;
                c.shown.clear();                           // the game's image is on the cell
            }
            if (c.stage != UINT32_MAX) { what = std::to_string(c.stage); identified = true; }
            else what = "? (" + code + ")";
            if (ours) what += " (variant " + code + ")";
        } else if (c.mat) what = "? (no texture, status " + std::to_string(status) + ")";
        map += (map.empty() ? "" : ", ") + c.name + " -> " + what;
        if (!cur || c.stage == UINT32_MAX) continue;      // stage unknown: never touched
        // what the cell must show: its stage's selected variant's thumbnail, else the game's image
        const int ci = selected_index(c.stage);
        const Variant* var = ci ? &g_variants[c.stage][static_cast<size_t>(ci - 1)] : nullptr;
        Obj* want = nullptr;
        if (var && !var->thumb.empty()) {
            want = preview_holder(var->thumb);
            if (!want) { done = false; continue; }         // loading: the cell keeps what it shows
        }
        if (want) {
            if (slots::ui::set_mat_tex(c.mat, kStageImage, want) && c.shown != var->key) {
                c.shown = var->key;
                logf("stages: strip cell %s (stage %u) -> %s", c.name.c_str(), c.stage, var->name.c_str());
            }
        } else if (ours && c.vanilla) {
            if (slots::ui::set_mat_tex(c.mat, kStageImage, c.vanilla)) {
                c.shown.clear();
                logf("stages: strip cell %s (stage %u) -> the game's image", c.name.c_str(), c.stage);
            }
        }
    }
    if (!g_strip.logged && (identified || g_frame + 5 >= g_strip.until)) {
        g_strip.logged = true;
        logf("stages: strip cells (focused stage %u): %s", focused, map.empty() ? "none" : map.c_str());
    }
    return done;
}

void strip_open() {
    // every change opens a fresh window (16/10: a cap set at the first change closed it while the user kept cycling,
    // and the last variant's thumbnail, still loading, was never shown)
    g_strip.cap = g_frame + 600;
    g_strip.until = g_frame + 120;
    g_strip.next = g_frame;
}

void strip_restore() {
    if (slots::ui::has_mat_tex())
        for (auto& c : g_strip.cells) {
            if (c.shown.empty() || !c.vanilla || !c.mat || !alive(c.ctrl) || !alive(c.mat)) continue;
            Obj* cur = slots::ui::get_mat_tex(c.mat, kStageImage);
            if (!cur) continue;
            bool ours = false;
            std::string code;
            stage_of_image(cur, ours, code);
            cur->release();
            if (ours && slots::ui::set_mat_tex(c.mat, kStageImage, c.vanilla))
                logf("stages: strip cell %s (stage %u) -> the game's image (screen left)", c.name.c_str(), c.stage);
        }
    for (auto& c : g_strip.cells) drop(c.vanilla);
    g_strip = Strip{};
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

template <perf::Id ID> int pre_shown(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    if (argc >= 2) g_hook_param.store(reinterpret_cast<uintptr_t>(argv[1]));
    post(EV_STAGE_SHOW);
    perf::end(ID, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
template <perf::Id ID> int pre_hidden(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_STAGE_HIDE); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> void post_dirty(void**, REFrameworkTypeDefinitionHandle, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return; int64_t pt = perf::begin(); post(EV_STAGE_DIRTY); perf::end(ID, pt); }

constexpr uint32_t kTrigger = 2, kRepeat = 8;
void direction(void** argv, int step) {
    uintptr_t agent = g_stage_agent.load(std::memory_order_relaxed);
    if (!agent || reinterpret_cast<uintptr_t>(argv[1]) != agent) return;
    uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[2])) & 0xF;
    if (flag & (kTrigger | kRepeat)) { g_pending.fetch_add(step); post(EV_STAGE_INPUT); }
}
int pre_up(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long)   { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); if (argc >= 3) direction(argv, -1); perf::end(perf::HkAgentInputUp, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_down(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); if (argc >= 3) direction(argv, 1); perf::end(perf::HkAgentInputDown, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }

int pre_vs(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    if (argc >= 2) g_vs_comp.store(reinterpret_cast<uintptr_t>(argv[1]));
    post(EV_VS);
    perf::end(perf::HkVsActivate, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
template <perf::Id ID> void post_vs(void**, REFrameworkTypeDefinitionHandle, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return; int64_t pt = perf::begin(); post(EV_VS); perf::end(ID, pt); }

// =============================================================================================
// Stage grids: Battle Settings > Other > Stage Settings, Training's Change Stage
// =============================================================================================
// Both are app.UIPartsStageSelectSimple (read 05/11 in the game's code: UIFlowMatchingSetting.Param.BeforeShowObject and
// UIFlowGenericStageSetting.Param.BeforeShowObject call its Setup). Setup -> SetupList: list = hGUI.GetStageSelectStageIdList
// (a new List<UInt32> per call) -> mStageIdList = list -> mScrollGrid.InitGrid(list.Count, 0) -> UpdateList (a pool of
// via.gui.SelectItem, each showing list[item.ListIndex]). Decide: list[grid.SelectedIndex] -> mDecideStageId (GetStageId),
// which the menu keeps (Battle Settings' save, Training's ChangeStage).
//   - Tiles: while SetupList runs (thread-local flag), the list GetStageSelectStageIdList returns gets each tile's vanilla id
//     once more, before Battle Hub / Random (stages_logic.hpp plan_slots): the grid grows (InitGrid takes the count) and
//     every game path keeps a valid id. Which entry an index is: our slot table.
//   - ConvertStageIdToGridIndex (FindIndex: the first match) answers the chosen tile's index for its stage: the cursor
//     opens on it and Decide's "already chosen" test stays right.
//   - Decide (default state only; Random's state toggles the random checks): the entry of grid.SelectedIndex is the menu's
//     choice for its stage (a tile's key, or the variation cycled on the vanilla entry).
//   - Pictures and names: the entries' cells, the preview's picture and name, rewritten by the frame for 2 s after each
//     build / refresh / focus / key (compare-before-write): a tile shows its own pictures and name, a vanilla entry its
//     cycled variation's picture and "<L> Training Room · Red <R>".
//   - The cycle keys: LT / RT (UIAgent.InputChildTabBack / Next) of the grid's agent unless its screen uses them, else LB /
//     RB, else none (stages_logic.hpp pick_keys).
// Every hook is installed only after the R18 code check of its method (codecheck_rt.hpp, cache codecheck_stages.txt).

namespace sl = stagelogic;

constexpr const char* kCodecheckCache = "reframework/data/SF6_CostumeSlots_data/codecheck_stages.txt";

enum Hook : size_t { kSetupList, kGetList, kConvert, kDecide, kUpdateList, kSelChanged, kChildBack, kChildNext, kTabBack,
                     kTabNext, kBattleDesc, kTrainStage, kReplayDesc, kHooks };
codecheck::Batch g_check;
bool g_check_started = false, g_hooks_done = false;
bool g_hooked[kHooks] = {};

// ---- the game's types, methods, fields (resolved on the first frame) ----
TD* t_simple = nullptr;                                        // app.UIPartsStageSelectSimple
API::Field *f_scroll = nullptr, *f_state = nullptr, *f_preview = nullptr, *f_is_matching = nullptr, *f_is_hub = nullptr;
Method *m_owner = nullptr, *m_is_show = nullptr, *m_sel_index = nullptr, *m_items = nullptr, *m_list_index = nullptr;
Method *m_list_count = nullptr, *m_list_item = nullptr, *m_list_insert = nullptr;   // List<UInt32>
API::Field *f_map_desc = nullptr, *f_desc_stage = nullptr, *f_stage_id = nullptr, *f_desc_net = nullptr;   // replays



struct GridState {
    uintptr_t obj = 0;                 // the grid; no reference held: checked alive and of its type before each use
    int menu = -1;                     // sl::kBattle / sl::kTraining
    std::vector<sl::Slot> slots;       // its entries once the tiles are in (empty: none inserted)
    std::map<uint32_t, int> cycle;     // vanilla stage -> variation shown on its entry (0: the vanilla look)
    uintptr_t owner = 0;               // its UIAgent (the cycle keys' filter)
    int keys = sl::kKeysNone;
    bool keys_known = false;
    uint32_t until = 0, cap = 0, next = 0;
    std::string cell_pic;              // name of the picture control under each item (learnt from the first cell found)
    std::string cell_text;             // name of the stage-name text under each item, if the cells have one (learnt)
    int pic = 0, preview_pic = 0;      // what the cells / the preview show: 0 not known yet, 1 the 4:1 image, 2 the thumbnail
    std::unordered_set<std::string> names_written;   // tile names written on cells (a reused cell gets its stage's back)
    bool logged = false;
};
GridState g_grid;                                              // g_mx
struct Decided { bool pending = false; int menu = -1; uint32_t base = 0; std::string key, before; };
Decided g_decided;                                             // g_mx
std::atomic<int> g_cycle_pending{0};
std::atomic<bool> g_grid_event{false};
struct SetupFlag { uintptr_t obj = 0; int menu = -1; };
thread_local SetupFlag tl_setup;
struct ConvertArgs { uintptr_t obj = 0; int32_t id = 0; };
thread_local ConvertArgs tl_convert;
thread_local uintptr_t tl_replay_map = 0;
std::atomic<int32_t> g_replay_stage{-1};

const Variant* variant_of(uint32_t base, const std::string& key) {
    auto v = g_variants.find(base);
    if (v == g_variants.end()) return nullptr;
    for (auto& x : v->second) if (x.key == key) return &x;
    return nullptr;
}
std::vector<const Variant*> variations_of(uint32_t base) {
    std::vector<const Variant*> out;
    auto v = g_variants.find(base);
    if (v != g_variants.end()) for (auto& x : v->second) if (!x.tile) out.push_back(&x);
    return out;
}
// The cycle position of a key among a stage's variations (0: vanilla look or not a variation)
int cycle_of(uint32_t base, const std::string& key) {
    auto vars = variations_of(base);
    for (size_t i = 0; i < vars.size(); ++i) if (vars[i]->key == key) return static_cast<int>(i) + 1;
    return 0;
}
bool is_tile(uint32_t base, const std::string& key) {
    const Variant* v = key.empty() ? nullptr : variant_of(base, key);
    return v && v->tile;
}

std::string ess_of(uint32_t id) {
    char b[16];
    std::snprintf(b, sizeof b, "ess%04u_%02u", id / 100, id % 100);
    return b;
}
std::string vanilla_pic(uint32_t id, bool thumb) {
    const std::string e = ess_of(id);
    return "Product/GUI/data/Area_Image/ess/" + e + (thumb ? "/tex_StageThumb_" : "/tex_StageImage_") + e + "_IM.tex";
}

// The game's name of a stage (app.InventoryManager.Stage.GetName), cached. Game thread.
std::map<uint32_t, std::string> g_stage_names;
const std::string& vanilla_name(uint32_t id) {
    auto it = g_stage_names.find(id);
    if (it != g_stage_names.end()) return it->second;
    std::string n;
    auto* inv_td = api().tdb()->find_type("app.InventoryManager");
    Method* get_stage = inv_td ? method_of(inv_td, "get_Stage", 0) : nullptr;
    Obj* inv = get_stage ? call_obj(get_stage, nullptr) : nullptr;
    if (inv) n = str(call_obj(inv, "GetName", { arg_i32(static_cast<int32_t>(id)) }));
    if (n.empty()) return g_stage_names[id];        // not cached for good: asked again later
    return g_stage_names[id] = n;
}

std::string res_path(Obj* holder) {
    return holder ? str(call_obj(holder, "get_ResourcePath")) : std::string();
}
std::string lower_s(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
// The texture a via.gui.Texture shows: its resource path (lower case), "" none
std::string shown_pic(Obj* tex_ctrl) {
    Obj* h = tex_ctrl ? call_obj(tex_ctrl, "get_Texture") : nullptr;
    return lower_s(res_path(h));
}

// Key symbols of an app.InputAssign.Digital.Id for the device in use (the game's own icons)
std::map<std::pair<int, int>, std::string> g_key_symbols;
std::string key_symbol_id(int id) {
    if (id < 0) return {};
    auto* gm = api().get_managed_singleton("app.InputGuideManager");
    if (!gm) return {};
    int mode = kModeKeyboard;
    if (Obj* modes = get_obj(gm, "_Modes")) {
        auto r = call(modes, "get_Item", { arg_i32(0) });
        if (!r.exception_thrown && static_cast<int32_t>(r.dword) > 0) mode = static_cast<int32_t>(r.dword);
    }
    auto k = std::make_pair(mode, id);
    auto it = g_key_symbols.find(k);
    if (it != g_key_symbols.end()) return it->second;
    Method* m = method_of(gm->get_type_definition(), "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)", 4);
    std::string s = str(call_obj(m, gm, { arg_i32(id), arg_i32(mode), arg_i32(0), arg_i32(0) }));
    return g_key_symbols[k] = s;
}

Obj* grid_alive(uintptr_t p) {
    Obj* o = reinterpret_cast<Obj*>(p);
    return p && alive(o) && o->get_type_definition() == t_simple ? o : nullptr;
}
Obj* obj_field(Obj* o, API::Field* f) {
    void* p = o && f ? f->get_data_raw(o, false) : nullptr;
    return p ? *reinterpret_cast<Obj**>(p) : nullptr;
}
bool bool_field(Obj* o, API::Field* f) {
    void* p = o && f ? f->get_data_raw(o, false) : nullptr;
    return p && *reinterpret_cast<uint8_t*>(p) != 0;
}
int32_t i32_field(Obj* o, API::Field* f, int32_t def) {
    void* p = o && f ? f->get_data_raw(o, false) : nullptr;
    if (!p) return def;
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Which menu a grid belongs to (any thread: fields and managed calls only). -1: neither (Battle Hub, Avatar, Rival AI...).
int grid_menu(Obj* grid) {
    if (bool_field(grid, f_is_hub)) return -1;
    if (bool_field(grid, f_is_matching)) return sl::kBattle;
    Obj* owner = m_owner ? call_obj(m_owner, grid) : nullptr;
    Obj* go = owner ? call_obj(owner, "get_GameObject") : nullptr;
    if (go && str(call_obj(go, "get_Name")) == "MatchingSetting") return sl::kBattle;
    if (api().get_managed_singleton("app.training.TrainingManager")) return sl::kTraining;
    return -1;
}

void open_grid_window() {
    if (!g_grid.until) g_grid.cap = g_frame + 600;
    g_grid.until = g_frame + 120;
    g_grid.next = g_frame;
}

// ---- hooks (any thread: numbers, fields, managed calls; the GUI is the frame's) ----
int pre_setup_list(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    try {
        Obj* grid = reinterpret_cast<Obj*>(argv[1]);
        tl_setup = { reinterpret_cast<uintptr_t>(grid), grid ? grid_menu(grid) : -1 };
    } catch (...) { tl_setup = {}; }
    perf::end(perf::HkStgGrid, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_setup_list(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    tl_setup = {};
    g_grid_event.store(true);
    post(EV_STAGE_GRID);
}

// The list the game builds for the grid: the tiles go in (only while SetupList runs on this thread)
void post_get_list(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const SetupFlag f = tl_setup;
    if (!f.obj || g_closing.load(std::memory_order_relaxed) || !ret_val || !*ret_val) return;
    tl_setup.obj = 0;                                          // the first list of this SetupList only
    int64_t pt = perf::begin();
    try {
        Obj* list = reinterpret_cast<Obj*>(*ret_val);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        GridState g;
        g.obj = f.obj;
        g.menu = f.menu;
        std::vector<uint32_t> ids;
        Ret rc = call(m_list_count, list);
        const int n = rc.exception_thrown ? -1 : static_cast<int>(rc.dword);
        for (int i = 0; i < n; ++i) {
            Ret r = call(m_list_item, list, { arg_i32(i) });
            if (r.exception_thrown) { ids.clear(); break; }
            ids.push_back(r.dword);
        }
        if (f.menu >= 0 && n > 0 && static_cast<int>(ids.size()) == n) {
            size_t pos = 0;
            std::vector<uint32_t> ins;
            auto slots = sl::plan_slots(ids, g_tiles, pos, ins);
            bool ok = true;
            for (size_t k = 0; k < ins.size() && ok; ++k)
                ok = !call(m_list_insert, list, { arg_i32(static_cast<int32_t>(pos + k)), arg_i32(static_cast<int32_t>(ins[k])) }).exception_thrown;
            Ret rc2 = call(m_list_count, list);
            if (ok && !rc2.exception_thrown && static_cast<size_t>(rc2.dword) == slots.size()) g.slots = std::move(slots);
            else logf("stages: grid (%s): the tiles could not be put in the list (%d entries now): grid left as the game made it",
                      sl::menu_key(f.menu), rc2.exception_thrown ? -1 : static_cast<int>(rc2.dword));
            const auto& choice = g_state.menu[f.menu];
            for (auto& s : g.slots)
                if (!s.tile) { auto c = choice.find(s.base); g.cycle[s.base] = c != choice.end() ? cycle_of(s.base, c->second) : 0; }
        }
        g_grid = std::move(g);
    } catch (...) {
        logf("stages: EXCEPTION while putting the tiles in a stage grid");
    }
    perf::end(perf::HkStgGrid, pt);
}

int pre_convert(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_convert = argc >= 3 ? ConvertArgs{ reinterpret_cast<uintptr_t>(argv[1]), static_cast<int32_t>(reinterpret_cast<uintptr_t>(argv[2])) } : ConvertArgs{};
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// The chosen tile's index for its stage (the game's FindIndex finds the stage's own entry first)
void post_convert(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const ConvertArgs a = tl_convert;
    tl_convert = {};
    if (!a.obj || !ret_val || g_closing.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (a.obj != g_grid.obj || g_grid.slots.empty() || g_grid.menu < 0 || a.id < 0) return;
    const auto& choice = g_state.menu[g_grid.menu];
    auto c = choice.find(static_cast<uint32_t>(a.id));
    if (c == choice.end() || !is_tile(c->first, c->second)) return;
    const int idx = sl::entry_index(g_grid.slots, c->first, c->second);
    if (idx >= 0) *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(idx)));
}

int pre_decide(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    try {
        Obj* grid = reinterpret_cast<Obj*>(argv[1]);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (grid && reinterpret_cast<uintptr_t>(grid) == g_grid.obj && !g_grid.slots.empty() && g_grid.menu >= 0 &&
            i32_field(grid, f_state, -1) == 0) {               // the default state (Random's toggles its checks)
            Obj* scroll = obj_field(grid, f_scroll);
            Ret r = scroll ? call(m_sel_index, scroll) : Ret{};
            const int sel = scroll && !r.exception_thrown ? static_cast<int32_t>(r.dword) : -1;
            if (sel >= 0 && sel < static_cast<int>(g_grid.slots.size())) {
                const sl::Slot& s = g_grid.slots[static_cast<size_t>(sel)];
                std::string key;
                if (s.tile) key = s.key;
                else {
                    auto vars = variations_of(s.base);
                    const int c = g_grid.cycle[s.base];
                    if (c > 0 && c <= static_cast<int>(vars.size())) key = vars[static_cast<size_t>(c - 1)]->key;
                }
                auto& choice = g_state.menu[g_grid.menu];
                const auto old = choice.find(s.base);
                g_decided = { true, g_grid.menu, s.base, key, old != choice.end() ? old->second : std::string() };
                if (key.empty()) choice.erase(s.base); else choice[s.base] = key;
                // Training: served at once (its reload follows); Battle Settings: when an online battle is set up (at once
                // when that hook was refused)
                const bool now = g_grid.menu == sl::kTraining || !g_hooked[kBattleDesc];
                save_state(now ? g_grid.menu : -1, "decided in its stage grid");
                g_grid_event.store(true);
                post(EV_STAGE_GRID);
            }
        }
    } catch (...) {
        logf("stages: EXCEPTION in the stage grid's Decide hook");
    }
    perf::end(perf::HkStgGrid, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// UpdateList / SelectionChangedEvent of our grid: the frame looks at it again
int pre_grid_dirty(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (argc >= 2 && !g_closing.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(argv[1]) == g_grid.obj && g_grid.obj) {
        g_grid_event.store(true);
        post(EV_STAGE_GRID);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

template <int Keys, int Step>
int pre_cycle_key(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (argc < 3 || g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const uintptr_t owner = g_grid.owner;
    if (!owner || reinterpret_cast<uintptr_t>(argv[1]) != owner || g_grid.keys != Keys) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[2])) & 0xF;
    if (flag & (kTrigger | kRepeat)) {
        g_cycle_pending.fetch_add(Step);
        g_grid_event.store(true);
        post(EV_STAGE_GRID);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- per-menu choice: served when that menu's stage loads ----
void post_battle_desc(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return;
    int64_t pt = perf::begin();
    try { save_state(sl::kBattle, "an online battle is set up: on this PC only, the opponent and the server keep the vanilla id"); }
    catch (...) { logf("stages: EXCEPTION serving Battle Settings' choice"); }
    perf::end(perf::HkStgApply, pt);
}
int pre_training_stage(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    try { save_state(sl::kTraining, "Training sets its stage"); }
    catch (...) { logf("stages: EXCEPTION serving Training's choice"); }
    perf::end(perf::HkStgApply, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_replay_desc(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_replay_map = (argc >= 2 && !g_closing.load(std::memory_order_relaxed)) ? reinterpret_cast<uintptr_t>(argv[1]) : 0;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_replay_desc(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t map = tl_replay_map;
    tl_replay_map = 0;
    if (!map || g_closing.load(std::memory_order_relaxed)) return;
    int64_t pt = perf::begin();
    try {
        Obj* m = reinterpret_cast<Obj*>(map);
        Obj* desc = alive(m) ? obj_field(m, f_map_desc) : nullptr;
        Obj* stage = desc ? obj_field(desc, f_desc_stage) : nullptr;
        const int32_t want = g_replay_stage.load();
        if (stage && want >= 0 && !bool_field(desc, f_desc_net)) {
            const int32_t was = i32_field(stage, f_stage_id, -1);
            void* p = f_stage_id ? f_stage_id->get_data_raw(stage, false) : nullptr;
            if (p && was != want) {
                memcpy(p, &want, 4);
                logf("stages: replay played on stage %d (recorded on %d; width, position and inputs unchanged)", want, was);
            }
        }
        save_state(sl::kReplay, "a replay is set up");
    } catch (...) {
        logf("stages: EXCEPTION at the replay's description");
    }
    perf::end(perf::HkStgApply, pt);
}

// ---- the frame: keys of the grid's screen, cycle, Training's reload, pictures and names ----

// The pair of cycle keys the grid's screen leaves free (its input delegates' type and the agent's switched-off inputs)
void learn_keys(Obj* grid) {
    g_grid.keys_known = true;
    Obj* owner = m_owner ? call_obj(m_owner, grid) : nullptr;
    g_grid.owner = reinterpret_cast<uintptr_t>(owner);
    if (!owner) { g_grid.keys = sl::kKeysNone; logf("stages: grid (%s): no agent found: no cycle keys", sl::menu_key(g_grid.menu)); return; }
    Obj* dels = call_obj(owner, "get_InputDelegates");
    bool child_taken = false, tab_taken = false;
    for (TD* t = dels ? dels->get_type_definition() : nullptr; t; t = t->get_parent_type()) {
        if (t->get_full_name() == "app.UIAgentInputDelegates") break;
        for (auto* m : t->get_methods()) {
            const char* n = m ? m->get_name() : nullptr;
            if (!n) continue;
            if (!strcmp(n, "EventChildTabBack") || !strcmp(n, "EventChildTabNext")) child_taken = true;
            if (!strcmp(n, "EventTabBack") || !strcmp(n, "EventTabNext")) tab_taken = true;
        }
    }
    auto r = call(owner, "get_DisableSystemInputFlags");
    const uint32_t off = r.exception_thrown ? 0 : r.dword;
    int k = sl::pick_keys(child_taken, (off & 32) != 0, tab_taken, (off & 16) != 0);
    if (k == sl::kKeysChildTab && !(g_hooked[kChildBack] && g_hooked[kChildNext])) k = (!tab_taken && !(off & 16)) ? sl::kKeysTab : sl::kKeysNone;
    if (k == sl::kKeysTab && !(g_hooked[kTabBack] && g_hooked[kTabNext])) k = sl::kKeysNone;
    g_grid.keys = k;
    logf("stages: grid (%s): cycle keys %s (screen delegates %s: child tabs %s, tabs %s; switched off 0x%X)", sl::menu_key(g_grid.menu),
         sl::keys_name(k), dels ? dels->get_type_definition()->get_full_name().c_str() : "-", child_taken ? "used" : "free",
         tab_taken ? "used" : "free", off);
}

// Training: a choice on the stage already loaded (Training Room -> a tile over it) is no change of stage for the game
// (it compares the ids: read 05/11), so its stage is set again through the menu's own ChangeStage.
void training_reload(const Decided& d) {
    if (d.key == d.before) return;
    auto* tm = api().get_managed_singleton("app.training.TrainingManager");
    Obj* func = tm ? call_obj(tm, "get_MenuFunc") : nullptr;
    Obj* gdata = func ? get_obj(func, "_gData") : nullptr;
    int32_t cur = -1;
    if (!gdata || !get_i32(gdata, "StageID", cur)) { logf("stages: Training's stage not readable: no reload for the new choice"); return; }
    if (cur != static_cast<int32_t>(d.base)) return;          // another stage: the game changes it itself
    auto r = call(func, "ChangeStage", { arg_i32(cur) });
    logf("stages: Training: stage %d chosen again with %s: %s", cur, d.key.empty() ? "the vanilla look" : d.key.c_str(),
         r.exception_thrown ? "ChangeStage threw" : "reloaded when the menu closes");
}

// One via.gui control under `root` (depth-first) of a type and for which `pred` holds
template <class P>
Obj* find_ctrl(Obj* root, const char* type, P pred, int depth = 0) {
    if (!root || depth > 8) return nullptr;
    for (Obj* c = call_obj(root, "get_Child"); c; c = call_obj(c, "get_Next")) {
        if (type_name(c) == type && pred(c)) return c;
        if (Obj* hit = find_ctrl(c, type, pred, depth + 1)) return hit;
    }
    return nullptr;
}
bool is_stage_pic(const std::string& p) {
    return p.find("area_image/ess/") != std::string::npos || p.find("_stageslots/") != std::string::npos;
}
int pic_kind(const std::string& p) {   // 1 image, 2 thumbnail, 0 not known
    if (p.find("tex_stagethumb") != std::string::npos || p.find("/thumb.tex") != std::string::npos) return 2;
    if (p.find("tex_stageimage") != std::string::npos || p.find("/preview.tex") != std::string::npos) return 1;
    return 0;
}

// What an entry shows: its picture (ours: a variant's) and its name
struct Look { std::string pic; bool ours = false; std::string name; const Variant* v = nullptr; };
Look look_of(const sl::Slot& s, int cyc, bool thumb) {
    Look l;
    if (s.tile) l.v = variant_of(s.base, s.key);
    else if (cyc > 0) { auto vars = variations_of(s.base); if (cyc <= static_cast<int>(vars.size())) l.v = vars[static_cast<size_t>(cyc - 1)]; }
    const std::string own = l.v ? (thumb ? l.v->thumb : l.v->preview) : std::string();
    l.ours = !own.empty();
    l.pic = l.ours ? own : vanilla_pic(s.base, thumb);
    l.name = s.tile && l.v ? l.v->name : vanilla_name(s.base);
    return l;
}

// A picture control shows `want`: ours when it is not yet; the stage's own back when one of ours is left on it. False while
// the texture loads.
bool show_pic(Obj* tex, const Look& l) {
    if (!tex) return true;
    const std::string cur = shown_pic(tex);
    const bool cur_ours = cur.find("_stageslots/") != std::string::npos;
    if (!l.ours && !cur_ours) return true;                   // the game's own picture: left to the game
    if (cur == lower_s(l.pic)) return true;
    Obj* h = preview_holder(l.pic);
    if (!h) return false;
    set_texture(tex, h);
    return true;
}

const char* const kNoCellText = "<no cell text>";   // not a control name

bool grid_work() {
    perf::Scope ps(perf::StgGrid);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Obj* grid = grid_alive(g_grid.obj);
    if (!grid || g_grid.slots.empty()) return true;
    bool ok = false;
    const bool shown = m_is_show && call_bool(grid, "IsShow", {}, &ok);
    if (ok && !shown) return false;                          // set up but not on screen yet
    if (!g_grid.keys_known) learn_keys(grid);
    Obj* scroll = obj_field(grid, f_scroll);
    Ret rs = scroll ? call(m_sel_index, scroll) : Ret{};
    const int focus = scroll && !rs.exception_thrown ? static_cast<int32_t>(rs.dword) : -1;
    bool done = true;

    // the cycle keys: the focused vanilla entry's variations
    const int step = g_cycle_pending.exchange(0);
    if (step && focus >= 0 && focus < static_cast<int>(g_grid.slots.size())) {
        const sl::Slot& s = g_grid.slots[static_cast<size_t>(focus)];
        const int n = static_cast<int>(variations_of(s.base).size());
        if (!s.tile && n > 0) {
            g_grid.cycle[s.base] = sl::cycle(g_grid.cycle[s.base], n, step > 0 ? 1 : -1);
            logf("stages: grid (%s): stage %u shows variation %d of %d", sl::menu_key(g_grid.menu), s.base, g_grid.cycle[s.base], n);
        }
    }

    // the preview: the focused entry's picture and name
    Obj* preview = obj_field(grid, f_preview);
    if (preview && focus >= 0 && focus < static_cast<int>(g_grid.slots.size())) {
        const sl::Slot& s = g_grid.slots[static_cast<size_t>(focus)];
        Obj* ptex = get_obj(preview, "mTexThumbnail");
        Obj* ptext = get_obj(preview, "mTextStageName");
        if (!g_grid.preview_pic && ptex) g_grid.preview_pic = pic_kind(shown_pic(ptex));
        const int cyc = s.tile ? 0 : g_grid.cycle[s.base];
        const Look l = look_of(s, cyc, g_grid.preview_pic == 2);
        if (g_grid.preview_pic) done = show_pic(ptex, l) && done;
        const int nvar = s.tile ? 0 : static_cast<int>(variations_of(s.base).size());
        std::string name = l.name;
        if (!s.tile && nvar > 0)
            name = sl::cycle_label(key_symbol_id(sl::key_left_id(g_grid.keys)), vanilla_name(s.base), l.v ? l.v->name : std::string(),
                                   key_symbol_id(sl::key_right_id(g_grid.keys)));
        if (ptext && !name.empty() && (s.tile || nvar > 0)) set_text(ptext, name);
    }

    // the cells: each item of the pool shows list[ListIndex]
    Obj* items = scroll && m_items ? call_obj(m_items, scroll) : nullptr;
    Ret rl = items ? call(items, "get_Length") : Ret{};
    const int n_items = items && !rl.exception_thrown ? static_cast<int32_t>(rl.dword) : 0;
    std::string map;
    int text_misses = 0;
    for (int i = 0; i < n_items && i < 64; ++i) {
        Obj* item = call_obj(items, "Get", { arg_i32(i) });
        if (!item) item = call_obj(items, "get_Item", { arg_i32(i) });
        if (!item) continue;
        Ret ri = call(m_list_index, item);
        const int li = ri.exception_thrown ? -1 : static_cast<int32_t>(ri.dword);
        if (li < 0 || li >= static_cast<int>(g_grid.slots.size())) continue;
        const sl::Slot& s = g_grid.slots[static_cast<size_t>(li)];
        Obj* tex = nullptr;
        if (!g_grid.cell_pic.empty()) tex = find_child(item, g_grid.cell_pic.c_str(), 0);
        if (!tex) {
            tex = find_ctrl(item, "via.gui.Texture", [](Obj* c) { return is_stage_pic(shown_pic(c)); });
            if (tex) {
                g_grid.cell_pic = str(call_obj(tex, "get_Name"));
                g_grid.pic = pic_kind(shown_pic(tex));
                logf("stages: grid (%s): cell picture control \"%s\" (%s)", sl::menu_key(g_grid.menu), g_grid.cell_pic.c_str(),
                     g_grid.pic == 2 ? "thumbnails" : g_grid.pic == 1 ? "4:1 images" : "kind not known");
            }
        }
        const int cyc = s.tile ? 0 : g_grid.cycle[s.base];
        const Look l = look_of(s, cyc, g_grid.pic == 2);
        if (tex && g_grid.pic) done = show_pic(tex, l) && done;
        // a name on the cell: the stage's own name written by the game becomes the tile's; a reused cell gets its own back
        const std::string vname = vanilla_name(s.base);
        const std::string want = s.tile ? l.name : vname;
        auto names_stage = [&](Obj* c) {
            const std::string m = c ? str(call_obj(c, "get_Message")) : std::string();
            return !m.empty() && (m == vname || g_grid.names_written.count(m) != 0);
        };
        Obj* text = nullptr;
        if (g_grid.cell_text == kNoCellText) {
        } else if (!g_grid.cell_text.empty()) {
            text = find_child(item, g_grid.cell_text.c_str(), 0);
            if (!names_stage(text)) text = nullptr;
        } else if (!vname.empty()) {
            text = find_ctrl(item, "via.gui.Text", names_stage);
            if (text) g_grid.cell_text = str(call_obj(text, "get_Name"));
            else ++text_misses;
        }
        if (text && !want.empty() && str(call_obj(text, "get_Message")) != want) {
            set_text(text, want);
            if (s.tile) g_grid.names_written.insert(want);
        }
        if (!g_grid.logged) map += (map.empty() ? "" : ", ") + std::to_string(li) + ":" + (s.tile ? s.key : std::to_string(s.base));
    }
    if (g_grid.cell_text.empty() && text_misses >= 3) {
        g_grid.cell_text = kNoCellText;                        // the cells show no stage name: not looked for again
        logf("stages: grid (%s): the cells show no stage name", sl::menu_key(g_grid.menu));
    }
    if (!g_grid.logged && n_items > 0) {
        g_grid.logged = true;
        logf("stages: grid (%s): %zu entries, %d cells; cells -> entries: %s", sl::menu_key(g_grid.menu), g_grid.slots.size(), n_items, map.c_str());
    }
    return done;
}

void grid_tick(uint32_t ev) {
    if ((ev & EV_STAGE_GRID) || g_grid_event.exchange(false)) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (g_grid.obj && !g_grid.slots.empty() && !g_grid.until) {
            int tiles = 0;
            for (auto& s : g_grid.slots) tiles += s.tile ? 1 : 0;
            static uintptr_t s_logged = 0;
            if (s_logged != g_grid.obj) {
                s_logged = g_grid.obj;
                logf("stages: grid (%s) built: %zu entries, %d tile(s)", sl::menu_key(g_grid.menu), g_grid.slots.size(), tiles);
            }
        }
        open_grid_window();
    }
    Decided d;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (g_decided.pending) { d = g_decided; g_decided = {}; }
    }
    if (d.pending) {
        logf("stages: grid (%s): stage %u -> %s", sl::menu_key(d.menu), d.base, d.key.empty() ? "vanilla look" : d.key.c_str());
        if (d.menu == sl::kTraining) training_reload(d);
    }
    if (!g_grid.until) return;
    if (g_frame >= g_grid.until || g_frame >= g_grid.cap) { g_grid.until = 0; return; }
    if (g_frame < g_grid.next) return;
    g_grid.next = g_frame + 3;
    if (!grid_work()) g_grid.until = std::min(std::max(g_grid.until, g_frame + 30), g_grid.cap);
}

// ---- install: the code check first, the hooks once it answered ----
void start_grid_check() {
    auto* tdb = api().tdb();
    t_simple = tdb->find_type("app.UIPartsStageSelectSimple");
    TD* t_gui = tdb->find_type("app.helper.hGUI");
    TD* t_agent = tdb->find_type("app.UIAgent");
    TD* t_scroll = tdb->find_type("app.UIPartsScrollGrid");
    TD* t_list = tdb->find_type("System.Collections.Generic.List`1<System.UInt32>");
    TD* t_item = tdb->find_type("via.gui.SelectItem");
    TD* t_base = tdb->find_type("app.UIPartsBase");
    TD* t_session = tdb->find_type("app.network.FGBattleSession");
    TD* t_training = tdb->find_type("app.training.TrainingManager");
    TD* t_replay = tdb->find_type("app.battle.ReplayFlowMap");
    TD* t_desc = tdb->find_type("app.battle.BattleDesc");
    TD* t_stage = tdb->find_type("app.battle.StageDesc");
    f_scroll = t_simple ? field_of(t_simple, "mScrollGrid") : nullptr;
    f_state = t_simple ? field_of(t_simple, "mState") : nullptr;
    f_preview = t_simple ? field_of(t_simple, "mPreviewParts") : nullptr;
    f_is_matching = t_simple ? field_of(t_simple, "IsMatchingSetting") : nullptr;
    f_is_hub = t_simple ? field_of(t_simple, "IsBattleHub") : nullptr;
    m_owner = t_base ? method_of(t_base, "get_Owner", 0) : nullptr;
    m_is_show = t_simple ? method_of(t_simple, "IsShow", 0) : nullptr;
    m_sel_index = t_scroll ? method_of(t_scroll, "get_SelectedIndex", 0) : nullptr;
    m_items = t_scroll ? method_of(t_scroll, "get_Items", 0) : nullptr;
    m_list_index = t_item ? method_of(t_item, "get_ListIndex", 0) : nullptr;
    m_list_count = t_list ? method_of(t_list, "get_Count", 0) : nullptr;
    m_list_item = t_list ? method_of(t_list, "get_Item", 1) : nullptr;
    m_list_insert = t_list ? method_of(t_list, "Insert(System.Int32, System.UInt32)", 2) : nullptr;
    f_map_desc = t_replay ? field_of(t_replay, "m_desc") : nullptr;
    f_desc_stage = t_desc ? field_of(t_desc, "Stage") : nullptr;
    f_desc_net = t_desc ? field_of(t_desc, "IsNetwork") : nullptr;
    f_stage_id = t_stage ? field_of(t_stage, "StageId") : nullptr;
    const bool grid_ok = t_simple && f_scroll && f_state && f_preview && m_owner && m_sel_index && m_items && m_list_index &&
                         m_list_count && m_list_item && m_list_insert;
    if (!grid_ok) logf("stages: the stage grids' types or members not all found (game update?): no tiles in Battle Settings / Training");
    auto add = [&](TD* t, const char* sig, int n, const char* label, bool want) {
        g_check.add(want && t ? method_of(t, sig, n) : nullptr, label);
    };
    add(t_simple, "SetupList", 0, "app.UIPartsStageSelectSimple.SetupList()", grid_ok);
    add(t_gui, "GetStageSelectStageIdList(System.Boolean, System.Boolean, System.Boolean)", 3, "app.helper.hGUI.GetStageSelectStageIdList(Boolean,Boolean,Boolean)", grid_ok);
    add(t_simple, "ConvertStageIdToGridIndex", 1, "app.UIPartsStageSelectSimple.ConvertStageIdToGridIndex(Int32)", grid_ok);
    add(t_simple, "Decide", 0, "app.UIPartsStageSelectSimple.Decide()", grid_ok);
    add(t_simple, "UpdateList", 0, "app.UIPartsStageSelectSimple.UpdateList()", grid_ok);
    add(t_simple, "SelectionChangedEvent", 0, "app.UIPartsStageSelectSimple.SelectionChangedEvent()", grid_ok);
    add(t_agent, "InputChildTabBack", 1, "app.UIAgent.InputChildTabBack(InputDigitalFlag)", grid_ok);
    add(t_agent, "InputChildTabNext", 1, "app.UIAgent.InputChildTabNext(InputDigitalFlag)", grid_ok);
    add(t_agent, "InputTabBack", 1, "app.UIAgent.InputTabBack(InputDigitalFlag)", grid_ok);
    add(t_agent, "InputTabNext", 1, "app.UIAgent.InputTabNext(InputDigitalFlag)", grid_ok);
    add(t_session, "SetupBattleDesc", 2, "app.network.FGBattleSession.SetupBattleDesc(BattleDesc,Boolean)", true);
    add(t_training, "SetStage", 1, "app.training.TrainingManager.SetStage(StageDesc)", true);
    add(t_replay, "setupDesc", 0, "app.battle.ReplayFlowMap.setupDesc()", f_map_desc && f_desc_stage && f_stage_id);
    g_check.start(kCodecheckCache, "stage grids");
    g_check_started = true;
}

void install_grid_hooks() {
    g_hooks_done = true;
    g_check.log_summary();
    if (g_check.failed()) return;
    struct H { Hook id; REFPreHookFn pre; REFPostHookFn post; };
    const H hooks[] = {
        { kSetupList, pre_setup_list, post_setup_list },
        { kGetList, nullptr, post_get_list },
        { kConvert, pre_convert, post_convert },
        { kDecide, pre_decide, nullptr },
        { kUpdateList, pre_grid_dirty, nullptr },
        { kSelChanged, pre_grid_dirty, nullptr },
        { kChildBack, pre_cycle_key<sl::kKeysChildTab, -1>, nullptr },
        { kChildNext, pre_cycle_key<sl::kKeysChildTab, 1>, nullptr },
        { kTabBack, pre_cycle_key<sl::kKeysTab, -1>, nullptr },
        { kTabNext, pre_cycle_key<sl::kKeysTab, 1>, nullptr },
        { kBattleDesc, nullptr, post_battle_desc },
        { kTrainStage, pre_training_stage, nullptr },
        { kReplayDesc, pre_replay_desc, post_replay_desc },
    };
    // the tiles need the list, its owner (SetupList) and Decide: one refused, none of the three
    const bool core = g_check.unique(kSetupList) && g_check.unique(kGetList) && g_check.unique(kDecide);
    for (auto& h : hooks) {
        const auto& e = g_check.entry(h.id);
        Method* m = g_check.method(h.id);
        const bool grid_part = h.id <= kTabNext;
        if (!m) { logf("hook %s: not found (game update?)", e.label.c_str()); continue; }
        if (!g_check.unique(h.id)) {
            logf("hook %s: REFUSED (R18): %d other method(s) enter its code, e.g. %s", e.label.c_str(), e.shared, g_check.other_label(h.id).c_str());
            continue;
        }
        if (grid_part && !core) { logf("hook %s: not installed (the grid's core hooks were refused)", e.label.c_str()); continue; }
        g_hooked[h.id] = m->add_hook(h.pre ? h.pre : hook_pass, h.post ? h.post : hook_post_pass, false) != 0;
        logf("hook %s: %s", e.label.c_str(), g_hooked[h.id] ? "installed (its own code)" : "add_hook refused");
    }
    if (!g_hooked[kBattleDesc]) logf("stages: Battle Settings' stage choice is served as soon as it is made (its battle hook is missing)");
    if (!g_hooked[kReplayDesc]) logf("stages: the replay picker's choice is served as soon as it is made (its replay hook is missing)");
}

std::vector<uint32_t> g_vanilla_ids;                           // the game's stage-select order (entries())

} // namespace

// ---------------------------------------------------------------------------------------------
// For the other modules of the plugin (stages.hpp)
// ---------------------------------------------------------------------------------------------

std::vector<Entry> entries() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_vanilla_ids.empty()) {
        TD* t = api().tdb()->find_type("app.helper.hGUI");
        Method* m = t ? method_of(t, "GetStageSelectStageIdList(System.Boolean, System.Boolean, System.Boolean)", 3) : nullptr;
        Obj* list = m ? call_obj(m, nullptr, { arg_bool(false), arg_bool(false), arg_bool(false) }) : nullptr;
        for (int i = 0, n = list_count(list); i < n; ++i) {
            auto r = call(list, "get_Item", { arg_i32(i) });
            if (!r.exception_thrown) g_vanilla_ids.push_back(r.dword);
        }
    }
    std::vector<uint32_t> ids = g_vanilla_ids;
    if (ids.empty()) for (auto& [id, v] : g_variants) ids.push_back(id);
    std::vector<Entry> out;
    auto add = [&](uint32_t base, const Variant* v, Entry::Kind kind) {
        Entry e;
        e.base_id = base;
        e.kind = kind;
        e.key = v ? v->key : std::string();
        const std::string& vn = vanilla_name(base);
        e.name = v ? v->name : (vn.empty() ? ess_of(base) : vn);
        e.has_own_picture = v && (!v->preview.empty() || !v->thumb.empty());
        e.banner = v && !v->preview.empty() ? v->preview : vanilla_pic(base, false);
        e.thumb = v && !v->thumb.empty() ? v->thumb : vanilla_pic(base, true);
        out.push_back(std::move(e));
    };
    for (uint32_t id : ids) add(id, nullptr, Entry::Vanilla);
    for (auto& t : g_tiles) if (const Variant* v = variant_of(t.base, t.key)) add(t.base, v, Entry::Tile);
    for (auto& [id, vars] : g_variants) for (auto& v : vars) if (!v.tile) add(id, &v, Entry::Variation);
    return out;
}

std::string chosen(int menu, uint32_t base_id) {
    if (menu < 0 || menu >= stagelogic::kMenus) return {};
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto& c = g_state.menu[menu];
    auto it = c.find(base_id);
    return it != c.end() ? it->second : std::string();
}

bool choose(int menu, uint32_t base_id, const std::string& key) {
    if (menu < 0 || menu >= stagelogic::kMenus) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!key.empty() && !variant_of(base_id, key)) return false;
    auto& c = g_state.menu[menu];
    if (key.empty()) c.erase(base_id); else c[base_id] = key;
    // served at once only where no load hook serves it later
    int apply = -1;
    if (menu == stagelogic::kClassic) apply = stagelogic::kClassic;
    else if (menu == stagelogic::kReplay && !g_hooked[kReplayDesc]) apply = stagelogic::kReplay;
    else if (menu == stagelogic::kBattle && !g_hooked[kBattleDesc]) apply = stagelogic::kBattle;
    else if (menu == stagelogic::kTraining && !g_hooked[kTrainStage]) apply = stagelogic::kTraining;
    save_state(apply, "chosen by another module");
    logf("stages: %s: stage %u -> %s (another module)", stagelogic::menu_key(menu), base_id, key.empty() ? "vanilla look" : key.c_str());
    return true;
}

void choose_replay_stage(int32_t base_id) {
    const int32_t was = g_replay_stage.exchange(base_id < 0 ? -1 : base_id);
    if (was != base_id) logf("stages: replays play on %s", base_id < 0 ? "their recorded stage" : ("stage " + std::to_string(base_id)).c_str());
}
int32_t replay_stage() { return g_replay_stage.load(); }

// ---------------------------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------------------------

void init() {
    load_registry();
    collect_tiles();
    load_state();
    logf("stages: %zu variant(s) of %zu stage(s)%s", g_variant_count, g_variants.size(), g_variant_count ? "" : ": inactive");
}

void install_hooks() {
    if (!g_variant_count) return;
    auto* tdb = api().tdb();
    auto* prm = tdb->find_type("app.menu.UIFlowStageSelect.Param");
    hook_all(prm, "GotObject", pre_shown<perf::HkStgShown_GotObject>, nullptr);
    hook_all(prm, "BeforeShowObject", pre_shown<perf::HkStgShown_BeforeShowObject>, nullptr);
    hook_all(prm, "ShowedObject", pre_shown<perf::HkStgShown_ShowedObject>, nullptr);
    // BeforeHideObject is NOT hooked: its native code is shared with methods the whole game calls (09/10: 236,000
    // calls per second, in fights too, each through REFramework's hook; the hide event then ran the visibility check
    // every frame). The screen's end is OnEnd, or the poll below while it is shown.
    hook_all(prm, "OnEnd", pre_hidden<perf::HkStgHidden_OnEnd>, nullptr);
    hook_all(prm, "SelectionChangedWithArgs_List", nullptr, post_dirty<perf::HkStgDirty_SelectionChangedWithArgs_List>);
    hook_all(prm, "SetPreview", nullptr, post_dirty<perf::HkStgDirty_SetPreview>);
    hook_all(prm, "ChangePreview", nullptr, post_dirty<perf::HkStgDirty_ChangePreview>);
    auto* agent = tdb->find_type("app.UIAgent");
    hook_all(agent, "InputUp", pre_up, nullptr);
    hook_all(agent, "InputDown", pre_down, nullptr);
    auto* vs = tdb->find_type("app.esports.VSInfoOffline");
    hook_all(vs, "Activate", pre_vs, nullptr);
    hook_all(vs, "SetDispInfo", nullptr, post_vs<perf::HkVsSetDispInfo>);
    hook_all(vs, "SetTexture", nullptr, post_vs<perf::HkVsSetTexture>);
    start_grid_check();       // the stage grids' hooks: after their code check (tick)
}

bool busy() {
    if (!g_variant_count) return false;
    if ((g_check_started && !g_hooks_done) || g_grid.until != 0 || g_decided.pending || g_cycle_pending.load() != 0) return true;
    return (g_scr.active && (g_scr.need || g_scr.settle > 0 || g_strip.until != 0)) || g_vs.until != 0 || g_scr.check_until != 0;
}

void tick(uint32_t ev) {
    if (!g_variant_count) return;
    if (g_check_started && !g_hooks_done && g_check.done()) install_grid_hooks();
    grid_tick(ev);
    // The game calls BeforeHideObject while it sets the screen up, next to ShowedObject, and coming
    // back from the result screen the screen shows well after its events: a show or hide event, or
    // the game rewriting the screen while it is not followed, starts a check of its visibility for
    // 3 s (every 5 frames)
    if ((ev & (EV_STAGE_SHOW | EV_STAGE_HIDE)) || ((ev & EV_STAGE_DIRTY) && !g_scr.active)) g_scr.check_until = g_frame + 180;
    if (g_scr.check_until && (g_frame % 5 == 0 || (ev & (EV_STAGE_SHOW | EV_STAGE_HIDE)))) {
        perf::Scope ps(perf::StgCheck);
        if (g_frame >= g_scr.check_until) g_scr.check_until = 0;
        // the agent kept from an earlier visit can outlive its screen (Result -> Change Stage makes a
        // new one): when it is not on screen, the current one is looked for
        bool visible = shown(g_scr.agent);
        if (!visible) { g_scr.agent = find_agent("StageSelect"); visible = shown(g_scr.agent); }
        if (visible && !g_scr.active) { screen_shown(g_scr.agent); g_scr.check_until = 0; }
        else if (!visible && g_scr.active) screen_hidden();
    }
    // while the screen is shown, its visibility is polled every 30 frames (BeforeHideObject is not hooked)
    if (g_scr.active && !g_scr.check_until && g_frame % 30 == 0 && !shown(g_scr.agent)) screen_hidden();
    if (g_scr.active) {
        bool input = (ev & EV_STAGE_INPUT) != 0;
        if (ev & EV_STAGE_DIRTY) { g_scr.settle = std::max(g_scr.settle, 1); g_scr.need = true; }
        if (input || g_scr.need || g_scr.settle > 0) screen_work(input);
        // stage strip: looked at for 2 s after each change, longer while a preview loads (10 s at most)
        if (ev & (EV_STAGE_DIRTY | EV_STAGE_INPUT)) strip_open();
        if (g_strip.until) {
            if (g_frame >= g_strip.until || g_frame >= g_strip.cap) g_strip.until = 0;
            else if (g_frame >= g_strip.next) {
                g_strip.next = g_frame + 5;
                if (!strip_work()) g_strip.until = std::min(std::max(g_strip.until, g_frame + 30), g_strip.cap);
            }
        }
    }
    // VS screen: shown again after every write of the game on it for 2 s, longer while the image
    // loads, never more than 10 s after the screen appeared
    if (ev & EV_VS) {
        if (!g_vs.until) g_vs.cap = g_frame + 600;
        g_vs.until = g_frame + 120;
        g_vs.next = g_frame;
    }
    if (g_vs.until) {
        if (g_frame >= g_vs.until || g_frame >= g_vs.cap) g_vs.until = 0;
        else if (g_frame >= g_vs.next) {
            g_vs.next = g_frame + 10;
            if (!vs_work()) g_vs.until = std::max(g_vs.until, g_frame + 30);
        }
    }
}

} // namespace slots::stages
