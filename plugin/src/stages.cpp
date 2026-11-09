// SF6 Slots — native plugin: stage slots.
//
// On the stage select screen, UP / DOWN cycles the focused entry between its looks: a stage's own entry between its vanilla
// look and its variations. The tiles are entries of their own after the game's stages: RIGHT at the last cell scrolls the
// list to them (section "Classic list"). The loader (amd_ags_x64.dll) serves the chosen variant's files when
// the stage loads; this module keeps the choice (data/SF6_StageSlots_Data/state.json, which the
// loader watches) and shows it: stage name between UP / DOWN hints, preview, the stage strip's cells (core 0.8.1+),
// VS screen.
//
// Events: the stage select screen shown / hidden, focus changed or name and preview rewritten by the
// game, UP / DOWN on it, the VS screen shown or rewritten. Without stage mods: no hook at all.
//
// The stage grids (Battle Settings > Other > Stage Settings, Training's Change Stage: both are app.UIPartsStageSelectSimple,
// read 05/11 from the game's code) get the TILES of registry.json as entries of their own and cycle a vanilla entry's
// VARIATIONS with the pad's X button (the game's binding of it on the keyboard), with a "<key> Stage Variation" entry after
// the guide bar's own (stages.hpp, stages_logic.hpp; section "Stage grids" below). Each menu keeps its own
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
#include "name_scroll.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "user32.lib")   // MapVirtualKeyW (keyboard hints)

namespace slots::stages {
namespace {

namespace sl = stagelogic;

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
std::vector<stagelogic::TileGroup> g_tile_groups;          // the tiles as the classic list's entries: each with its looks
std::vector<stagelogic::TileRef> g_classic_tiles;          // the first tile of each group: the classic list's extra entries
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
    std::string written, log;             // the name last written on it; the last line logged
    int stage = -1;                       // the stage it was written for
} g_vs;
std::atomic<int32_t> g_played{ -1 };      // the stage last set up: a replay's, an online battle's, the classic screen's

std::unordered_set<std::string> g_ours;   // every text written here: never the game's own name

struct Preview { Obj* holder = nullptr; uint32_t frame = 0; bool failed = false; };
std::map<std::string, Preview> g_previews;
constexpr uint32_t kLoadFrames = 60;      // a texture handed to the GUI while it loads crashed the game
uint32_t g_preview_wait = 0;              // a preview is loading: frame when the last one is ready

void strip_open();                        // stage strip (below): look at the cells again for a while
Obj* find_child(Obj* ctrl, const char* name, int depth);
void strip_restore();                     // the game's images back on the cells, references released
// The classic list (section "Classic list", at the end of this namespace)
bool classic_on(Obj* param);              // this screen's list holds the tiles
void classic_work(Obj* param, bool input);
void classic_shown();
void classic_hidden();
void classic_frame();
bool strip_entries();
bool is_tile(uint32_t base, const std::string& key);

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
    std::vector<stagelogic::NamedTile> named;
    for (auto& [id, vars] : g_variants)
        for (auto& v : vars)
            if (v.tile) { g_tiles.push_back({ id, v.key }); named.push_back({ id, v.key, v.name }); }
    g_tile_groups = stagelogic::tile_groups(named);
    g_classic_tiles.clear();
    for (auto& g : g_tile_groups) g_classic_tiles.push_back({ g.base, g.keys.front() });
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

// The only writer of state.json. apply_menu >= 0: that menu's choice (or `serve`, a choice made for this load: Random's
// pick) becomes what the loader serves (told at once through its export when it changed); -1: the menus' choices are
// saved, the served selection stays. Any thread (g_mx).
void save_state(int apply_menu = stagelogic::kClassic, const char* why = nullptr, const stagelogic::Choice* serve = nullptr) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (apply_menu >= 0 && apply_menu < stagelogic::kMenus) g_state.active = serve ? *serve : g_state.menu[apply_menu];
    const std::string o = stagelogic::state_json(g_state);
    { int64_t pt = perf::begin(); if (!write_file(kState, o)) logf("stages: cannot write state.json"); perf::end(perf::StateWrite, pt); }
    if (apply_menu < 0 || g_state.active == g_applied) return;
    g_applied = g_state.active;
    post(EV_STAGE_GRID);                                   // the frame loads its pictures ahead of the VS screen
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
    classic_shown();
    if (g_state.active != g_selected) save_state(stagelogic::kClassic, "the classic stage select is shown");   // what its battle loads (as before)
    logf("stages: stage select shown (param %s, agent %s)", g_scr.param ? "ok" : "-", agent ? "ok" : "-");
}

void screen_hidden() {
    logf("stages: stage select hidden");
    strip_restore();
    classic_hidden();
    g_scr.active = false;
    g_scr.param = nullptr;
    g_stage_agent.store(0);
    g_pending.store(0);
    // the VS screen comes next: its image starts loading now
    if (g_scr.decided != UINT32_MAX) g_played.store(static_cast<int32_t>(g_scr.decided));
    if (g_scr.decided != UINT32_MAX && selected_index(g_scr.decided)) preload_previews(g_scr.decided);
}

void screen_work(bool input) {
    perf::Scope ps(perf::StgScreen);
    Obj* param = g_scr.param;
    if (!alive(param)) { param = find_stage_param(); g_scr.param = param; if (!param) return; }
    if (classic_on(param)) { classic_work(param, input); return; }
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
// VS screen (VSInfoOffline, every match's: offline, online, replays): stage name c_bg/e_text_stagename, stage image
// c_bg/e_texture_bg. It shows what the loader serves, whichever menu chose it (g_applied: stages_logic.hpp vs_stage).
// ---------------------------------------------------------------------------------------------
const std::string& vanilla_name(uint32_t id);
const Variant* variant_of(uint32_t base, const std::string& key);

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
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_applied.empty()) return true;                    // nothing served: the game's own name and picture
    uintptr_t comp = g_vs_comp.load();
    if (!g_vs.text || comp != g_vs.comp || !alive(g_vs.text)) {
        g_vs.comp = comp;
        // the screen's agent: the component's own GameObject, else by its usual name
        Obj* c = reinterpret_cast<Obj*>(comp);
        Obj* go = comp && alive(c) ? call_obj(c, "get_GameObject") : nullptr;
        const std::string go_name = go ? str(call_obj(go, "get_Name")) : std::string();
        Obj* agent = go_name.empty() ? nullptr : find_agent(go_name.c_str());
        if (!agent) agent = find_agent("VSInfoOffline");
        Obj* cm = agent ? call_obj(agent, "get_ControlMain") : nullptr;
        Obj* bg = cm ? find_child(cm, "c_bg", 0) : nullptr;
        g_vs.text = bg ? find_child(bg, "e_text_stagename", 0) : nullptr;
        g_vs.tex = bg ? find_child(bg, "e_texture_bg", 0) : nullptr;
        g_vs.written.clear();
        g_vs.stage = -1;
        if (!g_vs.text) {
            const std::string l = "VS screen: no stage name found (agent " + (go_name.empty() ? std::string("VSInfoOffline") : go_name) + (agent ? ", found)" : ", not found)");
            if (l != g_vs.log) { g_vs.log = l; logf("stages: %s", l.c_str()); }
            return false;
        }
    }
    // the stage on it: the game's own name, until we wrote over it
    const std::string shown = str(call_obj(g_vs.text, "get_Message"));
    if (shown.empty()) return false;                       // not written yet
    if (shown != g_vs.written || g_vs.stage < 0) {
        std::map<uint32_t, std::string> names;
        for (auto& [id, key] : g_applied) names[id] = vanilla_name(id);
        g_vs.stage = stagelogic::vs_stage(g_applied, names, shown, g_played.load());
        g_vs.written.clear();
    }
    const auto served = g_vs.stage >= 0 ? g_applied.find(static_cast<uint32_t>(g_vs.stage)) : g_applied.end();
    const Variant* v = served != g_applied.end() ? variant_of(served->first, served->second) : nullptr;
    const std::string l = "VS screen: the game's '" + shown + "', stage " + std::to_string(g_vs.stage) + ": " + (v ? v->name : std::string("nothing served for it"));
    if (!v) {
        if (l != g_vs.log) { g_vs.log = l; logf("stages: %s", l.c_str()); }
        return true;
    }
    // its name as the menus' banner: a tile's, a variation's one name line
    const std::string name = v->tile ? v->name : stagelogic::variation_label(vanilla_name(served->first), v->name, v->bundle);
    set_text(g_vs.text, name);
    g_vs.written = name;
    if (g_vs.tex && !v->preview.empty()) {
        Obj* h = preview_holder(v->preview);
        if (!h) return false;
        set_texture(g_vs.tex, h);
    }
    if (g_vs.log.rfind("VS screen: shows", 0) != 0 || g_vs.log.find(name) == std::string::npos) {
        g_vs.log = "VS screen: shows " + name;
        logf("stages: %s (stage %d)", g_vs.log.c_str(), g_vs.stage);
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
    if (classic_on(g_scr.param)) return strip_entries();
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
std::atomic<bool> g_classic_keys{ false };                     // the list holds tiles: it can scroll, and the game's UP / DOWN would turn its pages
bool up_down_ours(void** argv) {
    const uintptr_t agent = g_stage_agent.load(std::memory_order_relaxed);
    return agent && reinterpret_cast<uintptr_t>(argv[1]) == agent && g_classic_keys.load(std::memory_order_relaxed);
}
void direction(void** argv, int step) {
    uintptr_t agent = g_stage_agent.load(std::memory_order_relaxed);
    if (!agent || reinterpret_cast<uintptr_t>(argv[1]) != agent) return;
    uint32_t flag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[2])) & 0xF;
    if (flag & (kTrigger | kRepeat)) { g_pending.fetch_add(step); post(EV_STAGE_INPUT); }
}
int pre_up(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long)   { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); if (argc >= 3) direction(argv, -1); const bool ours = argc >= 3 && up_down_ours(argv); perf::end(perf::HkAgentInputUp, pt); return ours ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_down(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); if (argc >= 3) direction(argv, 1); const bool ours = argc >= 3 && up_down_ours(argv); perf::end(perf::HkAgentInputDown, pt); return ours ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL; }

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
//   - Pictures and names: the entries' cells (UIPartsStageSelectSimpleSelectItem.mTexThumbnail, a via.gui.Texture: its
//     getter is getTexture), the preview's picture and name, rewritten by the frame for 2 s after each build / refresh /
//     focus / key (compare-before-write; the game sets its picture again when its own load ends): a tile shows its own
//     pictures and name, a vanilla entry its cycled variation's picture and one name line (stages_logic.hpp
//     variation_label) in the game's own text.
//   - The chosen mark: the game's, on one cell (SetSelectIndex marks the cell of ConvertStageIdToGridIndex(decided id),
//     answered above); during Decide that index is the one of the choice from before (DecideGuard), so a tile confirmed
//     over its own stage is marked at once.
//   - The variation key: the pad's X button / its keyboard key (app.InputAssign.Digital.Id 341), read by the frame while
//     the grid's cells have the focus: one press, the next variation (wrapping). Battle Settings: the stage keeps it at
//     once (Wael 09/11); Training: a preview, Confirm chooses it (it loads the stage). The guide bar's "<key> Stage Variation":
//     a copy of the bar's item template after its entries, while the focused entry has variations (and for the replay
//     picker, replay_variation_hint).
// Every hook is installed only after the R18 code check of its method (codecheck_rt.hpp, cache codecheck_stages.txt).

constexpr const char* kCodecheckCache = "reframework/data/SF6_CostumeSlots_data/codecheck_stages.txt";

enum Hook : size_t { kSetupList, kGetList, kConvert, kDecide, kUpdateList, kSelChanged, kBattleDesc, kTrainStage, kReplayDesc,
                     kInitStageList, kButtonY, kHooks };
codecheck::Batch g_check;
bool g_check_started = false, g_hooks_done = false;
bool g_hooked[kHooks] = {};

// ---- the game's types, methods, fields (resolved on the first frame) ----
TD* t_simple = nullptr;                                        // app.UIPartsStageSelectSimple
API::Field *f_scroll = nullptr, *f_state = nullptr, *f_preview = nullptr, *f_is_matching = nullptr, *f_is_hub = nullptr;
API::Field *f_decide = nullptr, *f_rand_off = nullptr;         // mDecideStageId, mListRandomDisable (List<Int32>)
Method *m_owner = nullptr, *m_is_show = nullptr, *m_sel_index = nullptr, *m_items = nullptr, *m_list_index = nullptr;
Method *m_list_count = nullptr, *m_list_item = nullptr, *m_list_insert = nullptr;   // List<UInt32>
API::Field *f_map_desc = nullptr, *f_desc_stage = nullptr, *f_stage_id = nullptr, *f_desc_net = nullptr;   // replays
API::Field *f_param_random = nullptr;                          // app.menu.UIFlowStageSelect.Param.IsRandom



struct GridState {
    uintptr_t obj = 0;                 // the grid; no reference held: checked alive and of its type before each use
    int menu = -1;                     // sl::kBattle / sl::kTraining
    std::vector<sl::Slot> slots;       // its entries once the tiles are in (empty: none inserted)
    sl::Preview preview;               // the variation key's preview on the focused entry (stages_logic.hpp)
    uint32_t decided_seen = UINT32_MAX;// the game's decided stage as last seen (Random or not)
    int name_right = -1;               // the preview's name: 0 at the game's place, 1 right-aligned, -1 not set for this grid
    bool on_screen = false;            // on screen (looked at every frame while it is: the variation key, the hint)
    bool focused = false;              // its cells have the focus
    int focus_log = -1;                // the focus check's last logged answer
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
std::atomic<bool> g_tick_all_pending{false};                   // Random Stage Settings' Y key pressed (grid_work)
std::atomic<bool> g_grid_event{false};
struct SetupFlag { uintptr_t obj = 0; int menu = -1; };
thread_local SetupFlag tl_setup;
struct ConvertArgs { uintptr_t obj = 0; int32_t id = 0; };
thread_local ConvertArgs tl_convert;
// Inside the game's Decide (pre to post, this thread, this frame): the choice of the decided stage from before it, which
// its "already chosen" test must see (stages_logic.hpp decide_marks_now)
struct DecideGuard { uintptr_t obj = 0; uint32_t base = 0; std::string before; uint32_t frame = 0; bool on = false; };
thread_local DecideGuard tl_decide;
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
// A stage's entries in the grids: its own ("") and its tiles' keys (Random Stage Settings' ticks)
std::vector<std::string> entry_keys(uint32_t base) {
    std::vector<std::string> k = { std::string() };
    for (auto& t : g_tiles) if (t.base == base) k.push_back(t.key);
    return k;
}

std::string ess_of(uint32_t id) {
    char b[16];
    std::snprintf(b, sizeof b, "ess%04u_%02u", id / 100, id % 100);
    return b;
}
std::string vanilla_pic(uint32_t id, bool thumb) {
    if (id == sl::kRandomStage) return "Product/GUI/data/Area_Image/random/tex_random_IM.tex";
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
// The texture a via.gui.Texture shows: its resource path (lower case), "" none. The getter is getTexture() (no
// get_Texture: asked by that name, every picture read "" and no cell nor the preview was ever written, 05/11).
std::string shown_pic(Obj* tex_ctrl) {
    Obj* h = tex_ctrl ? call_obj(tex_ctrl, "getTexture") : nullptr;
    return lower_s(res_path(h));
}

// Key symbols of an app.InputAssign.Digital.Id per guide mode (the game's own icons): symbol_of below; asked again for
// each new grid (the bindings may have changed)
std::map<std::pair<int, int>, std::string> g_key_symbols;

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

// The grid's chosen entry (the game's decided stage, our choice for it) and its chosen variation; entry -1: not in this grid
void chosen_entry(Obj* grid, int& entry, int& var) {
    entry = -1; var = 0;
    const int32_t d = i32_field(grid, f_decide, -1);
    if (d < 0 || g_grid.menu < 0 || g_grid.slots.empty()) return;
    const uint32_t base = static_cast<uint32_t>(d);
    const auto& c = g_state.menu[g_grid.menu];
    auto it = c.find(base);
    const std::string key = it != c.end() ? it->second : std::string();
    const bool tile = is_tile(base, key);
    entry = sl::entry_index(g_grid.slots, base, tile ? key : std::string());
    if (entry >= 0 && !key.empty() && !tile) var = cycle_of(base, key);
}
// On screen: its control is drawn. Not IsShow: that is "activated" (its control's state pattern 0, set by Activate, read
// 05/11 in the game's code), false with the focus on Battle Settings' rows while the grid is on screen.
bool grid_visible(Obj* grid) {
    Obj* c = call_obj(grid, "get_Control");
    bool ok = false;
    const bool v = c && call_bool(c, "get_ActualVisible", {}, &ok);
    return ok && v;
}
// Battle Settings is open: its grid is built when it opens, on another tab, and no game event says when it comes on screen
// (the frame then looks at it every frame: grid_frame). EV_MENU_OPEN / EV_MENU_CLOSE, or its agent gone for 5 s.
bool g_grid_watch = false;
int g_watch_off = 0;
// The variation a stage's own entry shows now (0 for a tile or its own look)
int shown_var(Obj* grid, int entry) {
    if (entry < 0 || entry >= static_cast<int>(g_grid.slots.size()) || g_grid.slots[static_cast<size_t>(entry)].tile) return 0;
    if (g_grid.menu == sl::kBattle) {                          // each stage shows the variation it keeps (grid_work, the key)
        const auto& c = g_state.menu[sl::kBattle];
        const uint32_t base = g_grid.slots[static_cast<size_t>(entry)].base;
        auto it = c.find(base);
        return it == c.end() ? 0 : cycle_of(base, it->second);
    }
    int ce = -1, cv = 0;
    chosen_entry(grid, ce, cv);
    return sl::shown_variation(entry, ce, cv, g_grid.preview);
}
// Whether a stage is in the game's random pool (not in mListRandomDisable); known: the list was read
bool in_random_pool(Obj* grid, uint32_t base, bool* known = nullptr) {
    Obj* list = obj_field(grid, f_rand_off);
    if (known) *known = list != nullptr;
    if (!list) return true;
    const Ret rc = call(list, "get_Count");
    const int n = rc.exception_thrown ? 0 : static_cast<int32_t>(rc.dword);
    for (int i = 0; i < n; ++i) {
        const Ret r = call(list, "get_Item", { arg_i32(i) });
        if (!r.exception_thrown && static_cast<int32_t>(r.dword) == static_cast<int32_t>(base)) return false;
    }
    return true;
}
// The game's random-off list as numbers (false: not read)
bool read_rand_off(Obj* grid, std::vector<int32_t>& out) {
    out.clear();
    Obj* list = obj_field(grid, f_rand_off);
    const Ret rc = list ? call(list, "get_Count") : Ret{};
    if (!list || rc.exception_thrown) return false;
    const int n = static_cast<int32_t>(rc.dword);
    for (int i = 0; i < n; ++i) {
        const Ret r = call(list, "get_Item", { arg_i32(i) });
        if (r.exception_thrown) return false;
        out.push_back(static_cast<int32_t>(r.dword));
    }
    return true;
}
// Random Stage Settings' entries for the Y key (stages_logic.hpp tick_all): the grid's, Random and Battle Hub left out, each
// stage's entry keys; keep: the index among them of the grid's entry `focus` (-1: not one of them)
void tick_entries(int focus, std::vector<sl::TickEntry>& entries, std::map<uint32_t, std::vector<std::string>>& keys_of, int* keep) {
    if (keep) *keep = -1;
    for (int i = 0; i < static_cast<int>(g_grid.slots.size()); ++i) {
        const sl::Slot& s = g_grid.slots[static_cast<size_t>(i)];
        if (s.base == sl::kRandomStage || s.base == sl::kBattleHubStage) continue;
        if (keep && i == focus) *keep = static_cast<int>(entries.size());
        entries.push_back({ s.base, s.tile ? s.key : std::string() });
        if (!keys_of.count(s.base)) keys_of[s.base] = entry_keys(s.base);
    }
}
// Random Stage Settings' Decide on an entry of a stage with tiles (pre to post, this thread, this frame)
struct RandomGuard { uintptr_t obj = 0; int menu = -1; uint32_t base = 0; std::string key; bool want_in_pool = false; uint32_t frame = 0; bool on = false; };
thread_local RandomGuard tl_random;

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
    const uint32_t base = static_cast<uint32_t>(a.id);
    std::string key;
    if (tl_decide.on && tl_decide.obj == a.obj && tl_decide.base == base && tl_decide.frame == g_frame) {
        key = tl_decide.before;                                // Decide's own test: the choice from before it
    } else {
        const auto& choice = g_state.menu[g_grid.menu];
        auto c = choice.find(base);
        if (c != choice.end()) key = c->second;
    }
    if (key.empty() || !is_tile(base, key)) return;            // the stage's own entry: the game's answer (its first)
    const int idx = sl::entry_index(g_grid.slots, base, key);
    if (idx >= 0) *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(idx)));
}

int pre_decide(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    try {
        Obj* grid = reinterpret_cast<Obj*>(argv[1]);
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        const int32_t state = grid ? i32_field(grid, f_state, -1) : -1;
        if (grid && reinterpret_cast<uintptr_t>(grid) == g_grid.obj && !g_grid.slots.empty() && g_grid.menu >= 0 && state == 1) {
            // Random Stage Settings: an entry of a stage with tiles flips its own tick (stages_logic.hpp toggle_tick); the
            // game's Decide runs only when the stage's id enters or leaves its pool
            Obj* scroll = obj_field(grid, f_scroll);
            Ret r = scroll ? call(m_sel_index, scroll) : Ret{};
            const int sel = scroll && !r.exception_thrown ? static_cast<int32_t>(r.dword) : -1;
            if (sel >= 0 && sel < static_cast<int>(g_grid.slots.size())) {
                const sl::Slot& s = g_grid.slots[static_cast<size_t>(sel)];
                const auto keys = entry_keys(s.base);
                bool known = false;
                const bool in_pool = in_random_pool(grid, s.base, &known);
                if (keys.size() > 1 && known) {
                    const std::string key = s.tile ? s.key : std::string();
                    auto& off = g_state.off[g_grid.menu][s.base];
                    const sl::TickStep t = sl::toggle_tick(keys, off, in_pool, key);
                    off = t.off;
                    save_state(-1, "Random Stage Settings tick");
                    logf("stages: grid (%s): random tick of entry %d (%s) -> %s; the game's own Decide %s", sl::menu_key(g_grid.menu), sel,
                         key.empty() ? "the stage's own" : key.c_str(), t.off.count(key) ? "off" : "on", t.game_toggles ? "runs" : "skipped");
                    g_grid_event.store(true);
                    post(EV_STAGE_GRID);
                    if (!t.game_toggles) {
                        Obj* owner = m_owner ? call_obj(m_owner, grid) : nullptr;
                        if (owner) call(owner, "TriggerSoundDecide");        // the game's own decide sound, its Decide skipped
                        perf::end(perf::HkStgGrid, pt);
                        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
                    }
                    tl_random = { g_grid.obj, g_grid.menu, s.base, key, t.in_pool_after, g_frame, true };
                }
            }
        } else if (grid && reinterpret_cast<uintptr_t>(grid) == g_grid.obj && !g_grid.slots.empty() && g_grid.menu >= 0 && state == 0) {
            Obj* scroll = obj_field(grid, f_scroll);
            Ret r = scroll ? call(m_sel_index, scroll) : Ret{};
            const int sel = scroll && !r.exception_thrown ? static_cast<int32_t>(r.dword) : -1;
            if (sel >= 0 && sel < static_cast<int>(g_grid.slots.size())) {
                const sl::Slot& s = g_grid.slots[static_cast<size_t>(sel)];
                std::string key;
                if (s.tile) key = s.key;
                else {                                         // what the entry shows: Confirm chooses the preview
                    auto vars = variations_of(s.base);
                    const int c = shown_var(grid, sel);
                    if (c > 0 && c <= static_cast<int>(vars.size())) key = vars[static_cast<size_t>(c - 1)]->key;
                }
                g_grid.preview = {};
                if (g_state.random[g_grid.menu] != (s.base == sl::kRandomStage)) g_state.random[g_grid.menu] = s.base == sl::kRandomStage;
                auto& choice = g_state.menu[g_grid.menu];
                const auto old = choice.find(s.base);
                g_decided = { true, g_grid.menu, s.base, key, old != choice.end() ? old->second : std::string() };
                // the game's "already chosen" test sees the choice from before (the mark moves at once, 05/11)
                tl_decide = { g_grid.obj, s.base, g_decided.before, g_frame, true };
                if (sl::decide_marks_now(g_grid.slots, s.base, g_decided.before, sel))
                    logf("stages: grid (%s): entry %d confirmed: the game marks it now", sl::menu_key(g_grid.menu), sel);
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
void post_decide(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    tl_decide = {};
    const RandomGuard g = tl_random;
    tl_random = {};
    if (!g.on || g.frame != g_frame || g_closing.load(std::memory_order_relaxed)) return;
    try {
        // the game's own Decide ran (the id in or out of its pool): it may have refused (it keeps one stage): our tick back
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Obj* grid = grid_alive(g.obj);
        bool known = false;
        const bool in_pool = grid ? in_random_pool(grid, g.base, &known) : g.want_in_pool;
        if (known && in_pool != g.want_in_pool) {
            auto& off = g_state.off[g.menu][g.base];
            if (off.count(g.key)) off.erase(g.key); else off.insert(g.key);
            save_state(-1, "Random Stage Settings tick refused by the game");
            logf("stages: grid (%s): the game kept its pool as it was: the tick goes back", sl::menu_key(g.menu));
        }
        g_grid_event.store(true);
        post(EV_STAGE_GRID);
    } catch (...) {
        logf("stages: EXCEPTION after a Random Stage Settings tick");
    }
}

// UpdateList / SelectionChangedEvent of our grid: the frame looks at it again
int pre_grid_dirty(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (argc >= 2 && !g_closing.load(std::memory_order_relaxed) && reinterpret_cast<uintptr_t>(argv[1]) == g_grid.obj && g_grid.obj) {
        g_grid_event.store(true);
        post(EV_STAGE_GRID);
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- per-menu choice: served when that menu's stage loads ----
// A menu whose stage is Random: the game's random landed on `stage` (a vanilla id, the one the opponent and the server
// see); when that stage has tiles, one of its ticked entries is served on this PC (stages_logic.hpp pick_ticked).
void serve_menu(int menu, int32_t stage, const char* why) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (stage < 0 || !g_state.random[menu]) { save_state(menu, why); return; }
    const uint32_t base = static_cast<uint32_t>(stage);
    const auto keys = entry_keys(base);
    if (keys.size() < 2) { save_state(menu, why); return; }
    static uint32_t s_seed = GetTickCount() ^ 0x9E3779B9u;
    s_seed = s_seed * 1664525u + 1013904223u;
    const std::string k = sl::pick_ticked(keys, g_state.off[menu][base], s_seed >> 8);
    sl::Choice c = g_state.menu[menu];
    if (!k.empty()) c[base] = k;                                // a ticked tile
    else if (is_tile(base, c[base]) || c[base].empty()) c.erase(base);   // its own entry (with its chosen variation, if any)
    logf("stages: %s: Random landed on stage %u: entry %s picked among its ticked ones", sl::menu_key(menu), base,
         k.empty() ? "of the stage itself" : k.c_str());
    save_state(menu, why, &c);
}
thread_local uintptr_t tl_battle_desc = 0;
int pre_battle_desc(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_battle_desc = (argc >= 3 && !g_closing.load(std::memory_order_relaxed)) ? reinterpret_cast<uintptr_t>(argv[2]) : 0;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_battle_desc(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t d = tl_battle_desc;
    tl_battle_desc = 0;
    if (g_closing.load(std::memory_order_relaxed)) return;
    int64_t pt = perf::begin();
    try {
        Obj* desc = reinterpret_cast<Obj*>(d);
        Obj* stage = d && alive(desc) ? obj_field(desc, f_desc_stage) : nullptr;
        if (stage) g_played.store(i32_field(stage, f_stage_id, -1));
        serve_menu(sl::kBattle, stage ? i32_field(stage, f_stage_id, -1) : -1,
                   "an online battle is set up: on this PC only, the opponent and the server keep the vanilla id");
    } catch (...) { logf("stages: EXCEPTION serving Battle Settings' choice"); }
    perf::end(perf::HkStgApply, pt);
}
int pre_training_stage(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    try {
        Obj* desc = argc >= 3 ? reinterpret_cast<Obj*>(argv[2]) : nullptr;
        serve_menu(sl::kTraining, desc && alive(desc) ? i32_field(desc, f_stage_id, -1) : -1, "Training sets its stage");
    } catch (...) { logf("stages: EXCEPTION serving Training's choice"); }
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
        if (stage) g_played.store(i32_field(stage, f_stage_id, -1));   // after the forced stage's write above
        save_state(sl::kReplay, "a replay is set up");
    } catch (...) {
        logf("stages: EXCEPTION at the replay's description");
    }
    perf::end(perf::HkStgApply, pt);
}

// ---- the frame: keys of the grid's screen, cycle, Training's reload, pictures and names ----

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
    return p.find("area_image/ess/") != std::string::npos || p.find("_stageslots/") != std::string::npos ||
           p.find("tex_stageimage_") != std::string::npos || p.find("tex_stagethumb_") != std::string::npos;
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

// Battle Settings' summary (p_BattleSettingProfileGameSetting_ > e_texture_stage): the chosen entry's picture
bool summary_pic(Obj* grid, int entry, int var) {
    const SF6CoreAPI* a = slots::ui::core();
    if (!a || entry < 0 || entry >= static_cast<int>(g_grid.slots.size())) return true;
    Obj* owner = m_owner ? call_obj(m_owner, grid) : nullptr;
    Obj* cm = owner ? call_obj(owner, "get_ControlMain") : nullptr;
    void* part = cm ? a->ui_find_cached(cm, "p_BattleSettingProfileGameSetting_") : nullptr;
    Obj* tex = part ? reinterpret_cast<Obj*>(a->ui_find_cached(part, "e_texture_stage")) : nullptr;
    if (!tex) return true;
    return show_pic(tex, look_of(g_grid.slots[static_cast<size_t>(entry)], var, false));
}

// The preview's name (the game's e_text_stage: AutoRegionFit 1 of its own, read 05/11, probe p14: it squeezed a long name;
// off here, Wael 05/11): at the game's place, or right-aligned with a region as wide as the banner. That place is read while
// the text holds it, the probe's values until then.
struct NamePlace { float x = 208, y = 78, z = 0, w = 361, h = 46; } g_name_place;
constexpr int64_t kNameCentre = 5, kNameRight = 6;     // via.gui.ControlPoint / PageAlignment
void name_layout(Obj* text, bool right) {
    if (g_grid.name_right == (right ? 1 : 0)) return;
    const SF6CoreAPI* a = slots::ui::core();
    if (!a) return;
    SF6CoreClient* cl = slots::ui::client();
    const Ret rc = call(text, "get_ControlPoint");
    if (!rc.exception_thrown && static_cast<int64_t>(rc.dword) == kNameCentre) {
        float p[3] = {}, rs[2] = {};
        const Ret rp = call(text, "get_Position"), rr = call(text, "get_RegionSize");
        if (!rp.exception_thrown) { std::memcpy(p, rp.bytes.data(), sizeof p); g_name_place.x = p[0]; g_name_place.y = p[1]; g_name_place.z = p[2]; }
        if (!rr.exception_thrown) { std::memcpy(rs, rr.bytes.data(), sizeof rs); if (rs[0] > 0 && rs[1] > 0) { g_name_place.w = rs[0]; g_name_place.h = rs[1]; } }
    }
    SF6CoreArg v{}; v.type = SF6CORE_ARG_I64; v.v.i = 0;
    a->obj_invoke(cl, text, "set_AutoRegionFit(System.Boolean)", &v, 1, nullptr);
    v.v.i = right ? kNameRight : kNameCentre;
    a->obj_invoke(cl, text, "set_ControlPoint(via.gui.ControlPoint)", &v, 1, nullptr);
    a->obj_invoke(cl, text, "set_PageAlignment(via.gui.PageAlignment)", &v, 1, nullptr);
    a->ui_set_region_size(cl, text, right ? sl::kNameMaxW + 10 : g_name_place.w, g_name_place.h);
    a->ui_set_position(cl, text, right ? g_name_place.x + sl::kNameRightFromCentre : g_name_place.x, g_name_place.y, g_name_place.z);
    g_grid.name_right = right ? 1 : 0;
}
// Random Stage Settings: an entry of a stage with tiles shows its own tick (its c_check's StatePattern, which the game
// sets from the shared id: SetupRandomCheck)
void random_tick(Obj* item, const sl::Slot& s) {
    const auto keys = entry_keys(s.base);
    if (keys.size() < 2) return;
    Obj* grid = grid_alive(g_grid.obj);
    bool known = false;
    const bool in_pool = grid ? in_random_pool(grid, s.base, &known) : true;
    if (!known) return;
    Obj* check = find_child(item, "c_check", 0);
    if (!check) return;
    const int want = sl::is_ticked(keys, g_state.off[g_grid.menu][s.base], in_pool, s.tile ? s.key : std::string()) ? 1 : 0;
    const Ret cur = call(check, "get_StatePattern");
    if (!cur.exception_thrown && static_cast<int>(cur.dword) == want) return;
    SF6CoreArg v{}; v.type = SF6CORE_ARG_I64; v.v.i = want;
    if (const SF6CoreAPI* a = slots::ui::core()) a->obj_invoke(slots::ui::client(), check, "set_StatePattern(System.UInt32)", &v, 1, nullptr);
}

// The guide bar's "<Y> Tick all / Untick all": what the next press does (read again every few frames)
struct TickLabel { uintptr_t grid = 0; uint32_t at = 0; bool all = false; };
TickLabel g_tick_label;                                        // g_mx

// Random Stage Settings' Y key (Wael 09/11): every entry ticked, or every one but the entry under the cursor (stages_logic.hpp
// tick_all). The game's list is changed as its own Decide changes it: Decide only adds or removes the id in mListRandomDisable,
// the List the page holds (GetRandomDisable, the same object) and saves, then redraws the checks with SetupRandomCheck (probe
// 09/11). The tiles' ticks are the plugin's, redrawn by random_tick in the cells pass that follows.
void tick_all_now(Obj* grid, int focus) {
    Obj* list = obj_field(grid, f_rand_off);
    std::vector<int32_t> dis;
    if (!list || !read_rand_off(grid, dis)) { logf("stages: grid (%s): Y key: the game's random list unread: nothing changed", sl::menu_key(g_grid.menu)); return; }
    std::vector<sl::TickEntry> entries;
    std::map<uint32_t, std::vector<std::string>> keys_of;
    int keep = -1;
    tick_entries(focus, entries, keys_of, &keep);
    if (entries.empty()) return;
    const sl::TickAll t = sl::tick_all(entries, keys_of, dis, g_state.off[g_grid.menu], keep);
    if (call(list, "Clear").exception_thrown) { logf("stages: grid (%s): Y key: the game's random list not cleared: nothing changed", sl::menu_key(g_grid.menu)); return; }
    int failed = 0;
    for (int32_t id : t.disabled) failed += call(list, "Add", { arg_i32(id) }).exception_thrown ? 1 : 0;
    g_state.off[g_grid.menu] = t.off;
    save_state(-1, t.tick ? "Random Stage Settings: all ticked" : "Random Stage Settings: all unticked but one");
    call(grid, "SetupRandomCheck");
    if (Obj* owner = m_owner ? call_obj(m_owner, grid) : nullptr) call(owner, "TriggerSoundDecide");
    g_tick_label = { g_grid.obj, g_frame, t.tick };
    logf("stages: grid (%s): Y key: every entry %s; %zu id(s) out of the game's pool%s", sl::menu_key(g_grid.menu),
         t.tick ? "ticked" : "unticked but the one under the cursor", t.disabled.size(), failed ? " (SOME NOT WRITTEN)" : "");
}

bool grid_work() {
    perf::Scope ps(perf::StgGrid);
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Obj* grid = grid_alive(g_grid.obj);
    if (!grid || g_grid.slots.empty()) return true;
    int chosen_e = -1, chosen_v = 0;
    chosen_entry(grid, chosen_e, chosen_v);
    // Battle Settings' "Game Settings" summary shows the chosen entry on any tab
    const bool summary_done = g_grid.menu != sl::kBattle || summary_pic(grid, chosen_e, chosen_v);
    if (!grid_visible(grid)) return false;                   // set up but not on screen yet
    Obj* scroll = obj_field(grid, f_scroll);
    Ret rs = scroll ? call(m_sel_index, scroll) : Ret{};
    const int focus = scroll && !rs.exception_thrown ? static_cast<int32_t>(rs.dword) : -1;
    bool done = summary_done;

    // the variation key: the focused vanilla entry's next variation (one press, one step, wrapping)
    const int step = g_cycle_pending.exchange(0);
    if (step && focus >= 0 && focus < static_cast<int>(g_grid.slots.size())) {
        const sl::Slot& s = g_grid.slots[static_cast<size_t>(focus)];
        const int n = static_cast<int>(variations_of(s.base).size());
        if (!s.tile && n > 0 && g_grid.menu == sl::kBattle) {
            // Battle Settings: the stage keeps it at once, whether it is the chosen stage or not (Wael 09/11: the preview
            // left with Esc went back to the stage's own look, and the summary showed the chosen one, not the marked cell's
            // look); Confirm chooses the stage as it shows. Served as any of the menu's choices (serve_menu).
            auto& choice = g_state.menu[sl::kBattle];
            const int next = sl::cycle(shown_var(grid, focus), n, 1);
            if (next == 0) choice.erase(s.base);
            else choice[s.base] = variations_of(s.base)[static_cast<size_t>(next - 1)]->key;
            g_grid.preview = {};
            save_state(-1, "variation key (Battle Settings)");
            logf("stages: grid (%s): stage %u keeps variation %d of %d (%s)", sl::menu_key(g_grid.menu), s.base, next, n,
                 next ? choice[s.base].c_str() : "its own look");
        } else if (!s.tile && n > 0) {
            int ce = -1, cv = 0;
            chosen_entry(grid, ce, cv);
            g_grid.preview = sl::preview_next(focus, ce, cv, g_grid.preview, n);
            logf("stages: grid (%s): stage %u previews variation %d of %d (Confirm chooses it)", sl::menu_key(g_grid.menu), s.base,
                 g_grid.preview.var, n);
        }
    }
    // Random Stage Settings' Y key: tick all / untick all
    if (g_tick_all_pending.exchange(false) && i32_field(grid, f_state, -1) == 1) tick_all_now(grid, focus);

    // the preview: the picture and name of the focused entry, of the chosen one while the focus is on the rows
    Obj* preview = obj_field(grid, f_preview);
    const int shown_e = sl::banner_entry(g_grid.focused, focus, chosen_e);
    if (preview && shown_e >= 0 && shown_e < static_cast<int>(g_grid.slots.size())) {
        const sl::Slot& s = g_grid.slots[static_cast<size_t>(shown_e)];
        Obj* ptex = get_obj(preview, "mTexThumbnail");
        Obj* ptext = get_obj(preview, "mTextStageName");
        if (!g_grid.preview_pic && ptex) {
            g_grid.preview_pic = pic_kind(shown_pic(ptex));
            if (g_grid.preview_pic)
                logf("stages: grid (%s): preview picture: %s", sl::menu_key(g_grid.menu), g_grid.preview_pic == 2 ? "thumbnails" : "4:1 images");
            else done = false;                                 // its picture not loaded yet: looked at again
        }
        const Look l = look_of(s, shown_var(grid, shown_e), g_grid.preview_pic == 2);
        if (g_grid.preview_pic) done = show_pic(ptex, l) && done;
        const int nvar = s.tile ? 0 : static_cast<int>(variations_of(s.base).size());
        // one name line in the game's own text (its font and glow): a tile's name, a variation's (stages_logic.hpp
        // variation_label), the stage's own name back on its vanilla look; never squeezed (banner_name, name_layout): place
        // and text written together, right from the first frame
        std::string name = l.name;
        if (!s.tile && nvar > 0) name = sl::variation_label(vanilla_name(s.base), l.v ? l.v->name : std::string(), l.v ? l.v->bundle : std::string());
        if (ptext && !name.empty()) {
            const sl::BannerName bn = sl::banner_name(name);
            namescroll::release_unless(ptext, bn.text);          // another name: its place back first
            name_layout(ptext, bn.right);
            if (s.tile || nvar > 0 || bn.text != name) set_text(ptext, bn.text);
        }
    }

    // the cells: each item of the pool shows list[ListIndex]; in Random Stage Settings each entry of a stage with tiles its
    // own tick (the game ticks the shared id)
    const bool random_state = i32_field(grid, f_state, -1) == 1;
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
        const Look l = look_of(s, shown_var(grid, li), g_grid.pic == 2);
        if (tex && g_grid.pic) done = show_pic(tex, l) && done;
        if (random_state) random_tick(item, s);
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

// ---- the variation key (the game's binding of the pad's X button) --------------------------------------------------------
// Read as the replay picker reads its keys (replay_tab.cpp): while the game window is in front, the keyboard key the game
// binds to app.InputAssign.Digital.Id 341 (its symbol, at its QWERTY place: stages_logic.hpp qwerty_scan) or the X button
// (via.hid.GamePadButton RLeft) of the merged pad. A press is the key going down.
bool window_in_front() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
bool scan_down(unsigned sc) {
    const UINT vk = sc ? MapVirtualKeyW(sc, MAPVK_VSC_TO_VK) : 0;
    return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}
int guide_mode_now() {
    int mode = kModeKeyboard;
    auto* gm = api().get_managed_singleton("app.InputGuideManager");
    if (Obj* modes = gm ? get_obj(gm, "_Modes") : nullptr) {
        auto r = call(modes, "get_Item", { arg_i32(0) });
        if (!r.exception_thrown && static_cast<int32_t>(r.dword) > 0) mode = static_cast<int32_t>(r.dword);
    }
    return mode;
}
std::string symbol_of(int id, int mode) {
    auto k = std::make_pair(mode, id);
    auto it = g_key_symbols.find(k);
    if (it != g_key_symbols.end()) return it->second;
    auto* gm = api().get_managed_singleton("app.InputGuideManager");
    if (!gm) return {};
    Method* m = method_of(gm->get_type_definition(), "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)", 4);
    return g_key_symbols[k] = str(call_obj(m, gm, { arg_i32(id), arg_i32(mode), arg_i32(0), arg_i32(0) }));
}
// A key of the game's menus: its id (the symbol's binding on a keyboard), its pad button
struct GameKey {
    int id = 0;
    const char* what = "";
    const char* pad_field = "";           // via.hid.GamePadButton's field
    const char* pad_name = "";
    uint32_t pad_bit = 0;
    bool init = false;
    Method* merged = nullptr;
    Method* button = nullptr;
    unsigned sc = 0;                      // the keyboard key (scan code), 0 none
    uint32_t sc_at = 0;
    bool sc_known = false;
};
GameKey g_vk{ sl::kVariationKeyId, "variation key", "RLeft", "X", 64 };
GameKey g_mk{ sl::kCharaMusicKeyId, "music key", "RUp", "Y", 16 };
bool game_key_now(GameKey& k) {
    if (!k.init) {
        k.init = true;
        TD* pad = api().tdb()->find_type("via.hid.GamePad");
        k.merged = pad ? method_of(pad, "get_MergedDevice", 0) : nullptr;
        if (TD* btn = api().tdb()->find_type("via.hid.GamePadButton"))
            if (auto* f = btn->find_field(k.pad_field)) k.pad_bit = static_cast<uint32_t>(f->get_data<int32_t>(nullptr));
    }
    if (!window_in_front()) return false;
    if (!k.sc_known || g_frame - k.sc_at >= 120) {                // the binding can change while the game runs
        k.sc_at = g_frame;
        const std::string sym = symbol_of(k.id, kModeKeyboard);
        const unsigned sc = sl::qwerty_scan(sym);
        if (!k.sc_known || sc != k.sc)
            logf("stages: %s: keyboard %s (scan code 0x%X), pad %s (bit %u)", k.what, sym.empty() ? "-" : sym.c_str(), sc, k.pad_name, k.pad_bit);
        k.sc = sc;
        k.sc_known = true;
    }
    if (scan_down(k.sc)) return true;
    void* s = api().get_native_singleton("via.hid.GamePad");
    Obj* dev = (s && k.merged) ? call_obj(k.merged, reinterpret_cast<Obj*>(s)) : nullptr;
    if (!dev) return false;
    if (!k.button) k.button = method_of(dev->get_type_definition(), "get_Button", 0);
    const Ret r = call(k.button, dev);
    return !r.exception_thrown && (r.dword & k.pad_bit) != 0;
}
bool variation_key_now() { return game_key_now(g_vk); }

// ---- Battle Settings' Random Stage Settings, kept off the variation key ---------------------------------------------------
// The page (app.UIPartsMatchingSettingBattle) opens Random Stage Settings in its ButtonY, which the game calls for the Y
// button (UIMatchingSettingRandomStageSelect, 239: T on a keyboard, the key its guide bar shows) and for the X button too
// (R on a keyboard, not in the bar: seen 09/11, R alone -> ButtonY -> the grid in Random's state). X is the variation key:
// the press that showed the next variation also opened Random Stage Settings (Wael 09/11). While the grid's cells have the
// focus, ButtonY is skipped when the variation key is down and the Y key is not; Y, T and the bar's entry clicked with the
// mouse open it as before. Elsewhere on the page (Opponent Outfit, Online Music, the HUD row) ButtonY is untouched.
GameKey g_rk{ 239, "random stage key", "RUp", "Y", 16 };
int pre_button_y(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        bool focused = false;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            focused = g_grid.focused && g_grid.menu == sl::kBattle;
        }
        if (focused && variation_key_now() && !game_key_now(g_rk)) {
            logf("stages: grid (battle): the variation key's press kept from opening Random Stage Settings (the game's X on ButtonY)");
            return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
        }
    } catch (...) {}
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- the guide bar's hint "<key> Stage Variation" -------------------------------------------------------------------------
// A copy of the bar's item template (e_text_0 of c_guideBtn: the element the game duplicates for each entry, so its font,
// size, colour and icons), as fs_name.cpp's outfit name, placed after the bar's last entry, one entry's gap away. Never the
// bar's own text (it has one writer: the game, or the Color Mod on Fighter Settings).
//   - R19: the copy is made inside the guide list's own update (post-hook of app.UIPartsInputGuideList.OnLateUpdate, that
//     list only), asked by the frame; at most one per bar (it lives and dies with the bar's tree); 3 refusals: no more.
//   - R35: found again by name under its live parent each pass; every write through the core.
//   - R31: the copy keeps the template's animation; its place and text are written again every pass (10 frames).
// Shown while a stage grid has the focus on a stage with variations (here), or while the replay picker asks (replay_tab.cpp).
const char* kHintName = "e_text_slots_stagehint";      // not "p_...": never taken for a UI part (R20)
constexpr int kHintMaxRefused = 3;
constexpr uint32_t kHintPass = 10;
std::mutex g_hint_mx;
struct HintReq { uintptr_t part = 0; uint32_t frame = 0; } g_hint_req;
std::atomic<bool> g_hint_req_on{ false };
std::atomic<int> g_hint_refused{ 0 }, g_hint_status{ 1000 };
std::atomic<uintptr_t> g_hint_made_parent{ 0 };
thread_local uintptr_t tl_hint_part = 0;
bool g_hint_hook_tried = false, g_hint_hooked = false;
struct HintBar { Obj* agent = nullptr; Obj* part = nullptr; Obj* btn = nullptr; Obj* parent = nullptr; } g_hbar;
struct Hint {
    bool shown = false;
    std::string written;
    uintptr_t styled = 0;
    int cp = -1;
    uint32_t bar_at = 0, pass_at = 0;
    int status_log = 1000;
    bool lost_logged = false;
} g_hint;
uint32_t g_hint_replay_at = 0;                          // the frame the replay picker last asked for it (0 never)
std::string g_hint_replay_label;                        // ...and its label
int g_hint_replay_key = sl::kVariationKeyId;            // ...and its key

// c_guideBtn's item template and its entries (texts e_text_0_g_* / e_text_0_d_*)
Obj* bar_template(Obj* btn, std::vector<Obj*>* entries) {
    Obj* tmpl = nullptr;
    int n = 0;
    for (Obj* c = btn ? call_obj(btn, "get_Child") : nullptr; c && n < 64; c = call_obj(c, "get_Next"), ++n) {
        const std::string nm = str(call_obj(c, "get_Name"));
        if (nm == "e_text_0") tmpl = c;
        else if (entries && (nm.rfind("e_text_0_g_", 0) == 0 || nm.rfind("e_text_0_d_", 0) == 0)) entries->push_back(c);
    }
    return tmpl;
}
void hint_make_copy(uintptr_t part) {
    {
        std::lock_guard<std::mutex> lk(g_hint_mx);
        if (!g_hint_req.part || g_hint_req.part != part || g_frame - g_hint_req.frame > 2) return;
        g_hint_req = HintReq{};
        g_hint_req_on.store(false, std::memory_order_release);
    }
    const SF6CoreAPI* a = slots::ui::core();
    Obj* ctrl = get_obj(reinterpret_cast<Obj*>(part), "<Control>k__BackingField");
    Obj* btn = ctrl ? reinterpret_cast<Obj*>(a->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
    std::vector<Obj*> entries;
    Obj* tmpl = bar_template(btn, &entries);
    Obj* parent = btn ? call_obj(btn, "get_Parent") : nullptr;
    if (!parent || !(tmpl || !entries.empty())) { ++g_hint_refused; g_hint_status.store(-3000, std::memory_order_release); return; }
    if (a->ui_find_child(parent, kHintName, 0)) return;
    void* c = nullptr;
    const SF6CoreStatus st = a->ui_clone_control(slots::ui::client(), tmpl ? tmpl : entries.front(), parent, kHintName, &c);
    g_hint_status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st), std::memory_order_release);
    if (st != SF6CORE_OK || !c) { ++g_hint_refused; return; }
    g_hint_made_parent.store(reinterpret_cast<uintptr_t>(parent), std::memory_order_release);
    SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;          // via.gui.MaskType.NonTarget: no mask of the new parent clips it
    a->obj_invoke(slots::ui::client(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
    a->ui_set_visible(slots::ui::client(), c, 0);                // placed and shown by the next frame
}
int pre_hint_late(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_hint_req_on.load(std::memory_order_acquire) && !g_closing.load(std::memory_order_relaxed))
        tl_hint_part = reinterpret_cast<uintptr_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_hint_late(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t p = tl_hint_part;
    tl_hint_part = 0;
    if (!p || !g_hint_req_on.load(std::memory_order_acquire) || g_closing.load(std::memory_order_relaxed)) return;
    try { hint_make_copy(p); } catch (...) { ++g_hint_refused; g_hint_status.store(-2000, std::memory_order_release); }
}
// The hook on the guide list's update runs every frame for every guide bar: put the first time a hint is wanted
bool hint_hook() {
    if (g_hint_hook_tried) return g_hint_hooked;
    g_hint_hook_tried = true;
    const SF6CoreAPI* a = slots::ui::core();
    if (!a || !slots::ui::client() || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_agents)) {
        logf("stages: the core has no ui_clone_control (0.8.2+): no Stage Variation hint in the guide bar");
        return false;
    }
    auto* td = api().tdb()->find_type("app.UIPartsInputGuideList");
    Method* m = td ? method_of(td, "OnLateUpdate", 0) : nullptr;
    g_hint_hooked = m && m->get_function_raw() && m->add_hook(pre_hint_late, post_hint_late, false);
    logf("stages: guide bar hint %s", g_hint_hooked ? "on (the copy is made in app.UIPartsInputGuideList.OnLateUpdate)"
                                                    : "off (app.UIPartsInputGuideList.OnLateUpdate not hooked)");
    return g_hint_hooked;
}

bool agent_visible(Obj* ag) {
    Obj* cm = ag ? call_obj(ag, "get_ControlMain") : nullptr;
    bool ok = false;
    const bool v = cm && call_bool(cm, "get_ActualVisible", {}, &ok);
    return ok && v;
}
// The visible bar with entries (as fs_name.cpp finds it)
bool hint_find_bar(HintBar& b) {
    b = HintBar{};
    const SF6CoreAPI* a = slots::ui::core();
    void* buf[4] = {};
    const uint32_t n = a->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* ag = reinterpret_cast<Obj*>(buf[i]);
        if (!agent_visible(ag)) continue;
        Obj* lst = get_obj(get_obj(ag, "_PartsManager"), "_List");
        for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
            Obj* part = list_item(lst, k);
            if (!part || type_name(part) != "app.UIPartsInputGuideList") continue;
            Obj* ctrl = get_obj(part, "<Control>k__BackingField");
            Obj* btn = ctrl ? reinterpret_cast<Obj*>(a->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
            std::vector<Obj*> entries;
            bar_template(btn, &entries);
            if (entries.empty()) break;                         // this bar shows nothing: next agent
            Obj* parent = call_obj(btn, "get_Parent");
            if (!parent) break;
            b = { ag, part, btn, parent };
            return true;
        }
    }
    return false;
}
bool hint_bar_live(bool on_screen = true) {
    if (!g_hbar.agent) return false;
    void* buf[4] = {};
    const uint32_t n = slots::ui::core()->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i)
        if (buf[i] == g_hbar.agent) return (!on_screen || agent_visible(g_hbar.agent)) && alive(g_hbar.parent) && alive(g_hbar.btn);
    return false;
}
void* hint_copy() { return g_hbar.parent ? slots::ui::core()->ui_find_child(g_hbar.parent, kHintName, 0) : nullptr; }
void hint_hide(const char* why) {
    if (!g_hint.shown) return;
    if (hint_bar_live(false))                                // its agent hidden too: the copy would come back with it on every screen
        if (void* c = hint_copy()) slots::ui::core()->ui_set_visible(slots::ui::client(), c, 0);
    g_hint.shown = false;
    logf("stages: guide bar hint hidden (%s)", why);
}

bool floats_of(Obj* o, const char* m, float* out, int n) {
    const Ret r = call(o, m);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * static_cast<size_t>(n));
    return true;
}
struct Geo { float x = 0, y = 0, z = 0, sx = 1, sy = 1; int cp = 0; };
bool geo_of(Obj* o, Geo& g) {
    float p[3], s[3];
    if (!floats_of(o, "get_Position", p, 3)) return false;
    g.x = p[0]; g.y = p[1]; g.z = p[2];
    if (floats_of(o, "get_Scale", s, 3)) { g.sx = s[0]; g.sy = s[1]; }
    const Ret r = call(o, "get_ControlPoint");
    g.cp = r.exception_thrown ? 0 : static_cast<int>(r.dword);
    if (g.cp < 0 || g.cp > 10 || g.cp % 4 > 2) g.cp = 0;
    return true;
}
float drawn_width(Obj* o) {
    float s[2] = {};
    return floats_of(o, "get_DisplaySize", s, 2) && std::isfinite(s[0]) && s[0] > 0 ? s[0] : 0.0f;
}
double glyph_count(const std::string& s) {     // a tag such as <ICON KeyF> counted as two (its glyph and a space)
    double n = 0;
    bool tag = false;
    for (unsigned char ch : s) {
        if (ch == '<') { tag = true; n += 2; continue; }
        if (tag) { if (ch == '>') tag = false; continue; }
        if ((ch & 0xC0) != 0x80) n += 1;
    }
    return n;
}

// One pass: the copy asked for, or placed after the bar's last entry, written and shown
void hint_pass(const std::string& text) {
    const SF6CoreAPI* a = slots::ui::core();
    SF6CoreClient* cl = slots::ui::client();
    std::vector<Obj*> entries;
    bar_template(g_hbar.btn, &entries);
    void* c = hint_copy();
    if (!c) {
        g_hint.shown = false;
        if (g_hint_refused.load() >= kHintMaxRefused) return;
        if (g_hint_made_parent.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(g_hbar.parent)) {
            if (!g_hint.lost_logged) { g_hint.lost_logged = true; logf("stages: the hint's copy under this bar is not found by name: no other copy"); }
            return;                                             // never a second copy under one parent
        }
        std::lock_guard<std::mutex> lk(g_hint_mx);
        g_hint_req = { reinterpret_cast<uintptr_t>(g_hbar.part), g_frame };
        if (!g_hint_req_on.exchange(true, std::memory_order_acq_rel)) logf("stages: guide bar hint: copy of the bar's item template asked");
        return;
    }
    // the bar's entries in c_guideBtn's space: the right end of the last one, the gap the game leaves between two
    Geo b;
    if (!geo_of(g_hbar.btn, b)) { hint_hide("the bar's place unread"); return; }
    struct Span { double l, r; Geo g; Obj* o; };
    std::vector<Span> spans;
    for (Obj* e : entries) {
        bool ok = false;
        if (!call_bool(e, "get_Visible", {}, &ok) || !ok) continue;
        const std::string msg = str(call_obj(e, "get_Message"));
        Geo g;
        const float w = drawn_width(e);
        if (msg.empty() || !(w > 0) || !geo_of(e, g)) continue;
        const double l = g.x - (g.cp % 4) * 0.5 * w * g.sx;
        spans.push_back({ l, l + w * g.sx, g, e });
    }
    if (spans.empty()) { hint_hide("the bar shows no entry"); return; }
    std::sort(spans.begin(), spans.end(), [](const Span& p, const Span& q) { return p.l < q.l; });
    // the key free on this screen? Its symbol among the bar's own entries means the screen uses it too (once per bar)
    static uintptr_t s_checked = 0;
    if (s_checked != reinterpret_cast<uintptr_t>(g_hbar.parent)) {
        s_checked = reinterpret_cast<uintptr_t>(g_hbar.parent);
        const std::string sym = text.substr(0, text.find(' '));
        std::string all;
        for (const Span& sp : spans) all += (all.empty() ? "" : " | ") + str(call_obj(sp.o, "get_Message"));
        const bool used = !sym.empty() && all.find(sym) != std::string::npos;
        logf("stages: guide bar entries '%s': %s", all.c_str(), used ? "they show the variation key too: WARNING, the screen may use it"
                                                                      : "the variation key is not among them");
    }
    double gap = 0;
    for (size_t i = 1; i < spans.size(); ++i) {
        const double d = spans[i].l - spans[i - 1].r;
        if (d > 0 && (gap <= 0 || d < gap)) gap = d;
    }
    const Span& last = spans.back();
    if (gap <= 0) {                                          // one text holding every entry: six spaces (the bar's separator)
        const double gl = glyph_count(str(call_obj(last.o, "get_Message")));
        gap = gl > 0 ? (last.r - last.l) / gl * 6 : 40;
    }
    const float x = static_cast<float>(b.x + b.sx * (last.r + gap));
    const float y = static_cast<float>(b.y + b.sy * last.g.y);
    const int cp = (last.g.cp / 4) * 4;                      // the entry's row, its left point
    if (reinterpret_cast<uintptr_t>(c) != g_hint.styled) {  // a new copy
        g_hint.styled = reinterpret_cast<uintptr_t>(c);
        g_hint.written.clear();
        g_hint.cp = -1;
    }
    if (cp != g_hint.cp) {
        SF6CoreArg pc{}; pc.type = SF6CORE_ARG_I64; pc.v.i = cp;
        if (a->obj_invoke(cl, c, "set_ControlPoint(via.gui.ControlPoint)", &pc, 1, nullptr) == SF6CORE_OK) g_hint.cp = cp;
    }
    if (text != g_hint.written && a->ui_set_text(cl, c, text.c_str()) == SF6CORE_OK) g_hint.written = text;
    a->ui_set_scale(cl, c, b.sx * last.g.sx, b.sy * last.g.sy, 1.0f);
    a->ui_set_position(cl, c, x, y, last.g.z);
    a->ui_set_force_invisible(cl, c, 0);                    // the template's lock, copied with it
    a->ui_set_visible(cl, c, 1);
    if (!g_hint.shown) { g_hint.shown = true; logf("stages: guide bar hint shown '%s' (after the bar's %zu entries, x %.0f)", text.c_str(), spans.size(), x); }
}

// Every frame the hint may be wanted: shown with `text`, or hidden
void hint_frame(bool want, const std::string& text, const char* why_not) {
    const int st = g_hint_status.load(std::memory_order_acquire);
    if (st != g_hint.status_log) {
        g_hint.status_log = st;
        logf("stages: guide bar hint copy %s (status %d)", st == SF6CORE_OK ? "made" : "not made", st);
    }
    if (!want) { hint_hide(why_not); return; }
    if (!hint_hook()) return;
    if (!hint_bar_live() || g_frame - g_hint.bar_at > 60) {
        g_hint.bar_at = g_frame;
        HintBar nb;
        const bool ok = hint_find_bar(nb);
        if (!ok || nb.parent != g_hbar.parent) { hint_hide("the guide bar changed"); g_hint.pass_at = 0; g_hint.lost_logged = false; }
        if (!ok) return;
        g_hbar = nb;
    }
    if (g_hint.pass_at && g_frame - g_hint.pass_at < kHintPass && text == g_hint.written && g_hint.shown) return;
    g_hint.pass_at = g_frame;
    hint_pass(text);
}

// ---- every frame a grid lives: on screen, focus, the variation key, the hint ----
// The grid has the focus when its agent's focused item is the grid or under it (no answer: shown is all we know)
bool grid_has_focus(Obj* grid, Obj* scroll) {
    Obj* owner = m_owner ? call_obj(m_owner, grid) : nullptr;
    Obj* f = owner ? call_obj(owner, "GetFocusItem") : nullptr;
    if (!f) return true;
    for (int d = 0; f && d < 8; ++d) {
        if (f == grid || f == scroll) return true;
        f = call_obj(f, "get_Parent");
    }
    return false;
}
uintptr_t g_grid_seen = 0;                                   // the grid the frame last looked at (a new one: symbols again)
bool g_key_was = false;
bool g_y_was = false, g_rnd_was = false;                     // Random Stage Settings' Y key: down, the state, last frame
void grid_frame() {
    bool on = false, focus = false, tile = false, rnd_state = false, all_ticked = false;
    int nvar = 0;
    {                                                        // the grid, under the lock
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Obj* grid = grid_alive(g_grid.obj);
        if (grid && !g_grid.slots.empty()) {
            if (g_grid.obj != g_grid_seen) { g_grid_seen = g_grid.obj; g_key_symbols.clear(); }
            on = grid_visible(grid);
            if (on) {
                Obj* scroll = obj_field(grid, f_scroll);
                const Ret rv = m_is_show ? call(m_is_show, grid) : Ret{};
                const bool active = !m_is_show || rv.exception_thrown || rv.byte != 0;   // activated: the cursor is in it
                focus = active && grid_has_focus(grid, scroll);
                // Random Stage Settings: what the Y key does next (the bar's label), read every 10 frames
                rnd_state = i32_field(grid, f_state, -1) == 1;
                if (rnd_state && focus && (g_tick_label.grid != g_grid.obj || g_frame - g_tick_label.at >= 10)) {
                    std::vector<int32_t> dis;
                    std::vector<sl::TickEntry> entries;
                    std::map<uint32_t, std::vector<std::string>> keys_of;
                    if (read_rand_off(grid, dis)) {
                        tick_entries(-1, entries, keys_of, nullptr);
                        g_tick_label = { g_grid.obj, g_frame, sl::all_ticked(entries, keys_of, dis, g_state.off[g_grid.menu]) };
                    }
                }
                all_ticked = g_tick_label.grid == g_grid.obj && g_tick_label.all;
                const Ret rs = scroll ? call(m_sel_index, scroll) : Ret{};
                const int sel = scroll && !rs.exception_thrown ? static_cast<int32_t>(rs.dword) : -1;
                if (sel >= 0 && sel < static_cast<int>(g_grid.slots.size())) {
                    const sl::Slot& s = g_grid.slots[static_cast<size_t>(sel)];
                    tile = s.tile;
                    nvar = s.tile ? 0 : static_cast<int>(variations_of(s.base).size());
                }
                // the preview lives while the focus stays on its entry (left without Confirm, or the grid left: the entry as
                // it was)
                const sl::Preview p = focus ? sl::preview_follow(g_grid.preview, sel) : sl::Preview{};
                if (!(p == g_grid.preview)) { g_grid.preview = p; open_grid_window(); }
                // whether the menu's stage is Random (the game's random lands on a vanilla id: Random's pick, serve_menu)
                const int32_t d = i32_field(grid, f_decide, -1);
                if (d >= 0 && static_cast<uint32_t>(d) != g_grid.decided_seen) {
                    g_grid.decided_seen = static_cast<uint32_t>(d);
                    const bool rnd = static_cast<uint32_t>(d) == sl::kRandomStage;
                    if (g_grid.menu >= 0 && g_state.random[g_grid.menu] != rnd) {
                        g_state.random[g_grid.menu] = rnd;
                        save_state(-1, "the menu's stage is Random or not");
                    }
                }
            }
        }
        // off screen: its name back at the game's place (the same text may come back with a stage of the game's)
        if (grid && !on && g_grid.name_right == 1)
            if (Obj* prv = obj_field(grid, f_preview))
                if (Obj* ptext = get_obj(prv, "mTextStageName")) { namescroll::release(ptext); name_layout(ptext, false); }
        // the banner's name, wider than the banner: it scrolls inside it (right-aligned with the banner's width: name_layout)
        if (grid && on)
            if (Obj* prv = obj_field(grid, f_preview))
                if (Obj* ptext = get_obj(prv, "mTextStageName")) {
                    if (g_grid.name_right == 1) namescroll::drive(ptext, sl::kNameMaxW);
                    else namescroll::release(ptext);
                }
        // it came on screen (Battle Settings' tab, its Stage Settings row): the pictures and the banner at once
        if (on && !g_grid.on_screen) { g_grid.until = 0; open_grid_window(); }
        if (g_grid_watch) {
            Obj* owner = grid && m_owner ? call_obj(m_owner, grid) : nullptr;
            g_watch_off = owner && agent_visible(owner) ? 0 : g_watch_off + 1;
            if (g_watch_off > 300) { g_grid_watch = false; g_watch_off = 0; }
        }
        g_grid.on_screen = on;
        if (on && (focus ? 1 : 0) != g_grid.focus_log) {
            g_grid.focus_log = focus ? 1 : 0;
            logf("stages: grid (%s): its cells %s the focus", sl::menu_key(g_grid.menu), focus ? "have" : "do not have");
        }
        if ((on && focus) != g_grid.focused) open_grid_window();   // the banner changes entry (focused / chosen)
        g_grid.focused = on && focus;
    }
    // the key: one step per press, on a stage with variations
    const bool down = g_grid.focused && variation_key_now();
    if (down && !g_key_was && !tile && nvar > 0) {
        g_cycle_pending.fetch_add(1);
        open_grid_window();
    }
    g_key_was = down;
    // Random Stage Settings' Y key (T on a keyboard; the game does nothing with it there, probe 09/11): one press, tick all /
    // untick all. Not the press that opened it from the stage list (still down when the grid's state changed): a press
    // counts when the grid was in Random's state the frame before too.
    const bool rnd_focus = g_grid.focused && rnd_state;
    const bool ydown = g_grid.focused && game_key_now(g_rk);
    if (ydown && !g_y_was && rnd_focus && g_rnd_was) {
        g_tick_all_pending.store(true);
        open_grid_window();
    }
    g_y_was = ydown;
    g_rnd_was = rnd_focus;
    // the hint: the grid's ("<Y> Tick all" in Random Stage Settings first, then the variation key), else the replay picker's
    const bool replay_asks = g_hint_replay_at && g_frame - g_hint_replay_at <= 1;
    const std::string sym = g_grid.focused ? symbol_of(sl::kVariationKeyId, guide_mode_now()) : std::string();
    const std::string replay_sym = replay_asks ? symbol_of(g_hint_replay_key, guide_mode_now()) : std::string();
    const bool grid_want = sl::variation_hint(g_grid.focused, tile, nvar, !sym.empty());
    const bool replay_want = replay_asks && !replay_sym.empty();
    std::string tick_text;
    if (rnd_focus) {
        const std::string ys = symbol_of(g_rk.id, guide_mode_now());
        if (!ys.empty()) tick_text = ys + " " + words::tr(all_ticked ? words::W::TitleKeyUncheckAll : words::W::TitleKeyCheckAll);
    }
    const std::string var_text = grid_want ? sl::variation_hint_text(sym) : std::string();
    const std::string grid_text = tick_text.empty() ? var_text : var_text.empty() ? tick_text : tick_text + "     " + var_text;
    const char* why = !on ? "no stage grid on screen" : !g_grid.focused ? "the grid has not the focus" : tile ? "a tile" : "no variation";
    hint_frame(!grid_text.empty() || replay_want, !grid_text.empty() ? grid_text : sl::guide_hint_text(replay_sym, g_hint_replay_label), why);
}
// No stage slots here: only the entry Replays > Settings asks for
void replay_hint_frame() {
    const bool asks = g_hint_replay_at && g_frame - g_hint_replay_at <= 1;
    const std::string sym = asks ? symbol_of(g_hint_replay_key, guide_mode_now()) : std::string();
    hint_frame(asks && !sym.empty(), sl::guide_hint_text(sym, g_hint_replay_label), "not asked");
}

void grid_tick(uint32_t ev) {
    if (ev & EV_MENU_OPEN) { g_grid_watch = true; g_watch_off = 0; open_grid_window(); }
    if (ev & EV_MENU_CLOSE) g_grid_watch = false;
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
    grid_frame();
    if (!g_grid.until) return;
    if (g_frame >= g_grid.until || g_frame >= g_grid.cap) { g_grid.until = 0; return; }
    if (g_frame < g_grid.next) return;
    g_grid.next = g_frame + 3;
    if (!grid_work()) g_grid.until = std::min(std::max(g_grid.until, g_frame + 30), g_grid.cap);
}

// =============================================================================================
// Classic list: the full-screen Stage Select's strip with the tiles after the game's stages
// =============================================================================================
// Read 07/11 in the game's code. UIFlowStageSelect.Param.InitStageList: StageIdList = hGUI.GetStageSelectStageIdList ->
// partsScrollList.InitList(count, index, index), all of them cells (its _DefaultItemCount is written with the count) -> the
// list centred from that count -> UpdateStageList (each cell's picture from StageIdList[its ListIndex]; called from nowhere
// else) -> the previews' cache, one per id. The via.gui.ScrollList scrolls by itself once it holds more entries than cells
// (ItemCount: cells on screen, ItemMax: entries; ScrollTime 0: at once, the outlines stay and what is in them moves), with
// its cursor inside CursorRange (the whole row).
//   - So the screen is built by the game as ever, with its own stages; then (post-hook of InitStageList) the tiles' vanilla
//     ids go on the list and InitList runs again with the longer count: the game's own count of cells and place stay.
//   - Which entry an index is: our slots (sl::plan_slots, as the grids). The game only ever holds vanilla ids: Confirm,
//     the opponent and the replay see the vanilla stage, only this PC loads the tile.
//   - UP / DOWN only changes the focused entry's look: a stage's own entry between its own look and its variations, a tile
//     among its group (sl::tile_groups). What the focused entry shows is served at once.
//   - UP / DOWN reach the game no more on this screen: a list that scrolls turns its pages with them (07/11).
//   - The list scrolled: the cells' pictures are ours, by ListIndex, in the same frame, and UpdateShopIcon (only the cells'
//     owned / not owned state) follows. UpdateStageList is never called again: the game calls it once, from its flow (07/11:
//     called a second and third time, the game crashed in its prefab workers as the previews were loading; that or the
//     arrows' first copies, see below).
//   - Nothing but the banner's text is touched while the previews load (IsPreviewTexCahceReady): a texture handed to the
//     GUI while it loads crashes the game.
//   - The arrows: copies of the music selector's own arrow, a plain Texture (e_tex_arrow), in the free corner beside the first
//     and the last cell, shown while entries lie past that end. Each under its own p_MouseDecideOperable_L / _R, whose scale
//     mirrors it (a Texture has no scale of its own). R19: the copy is made in the post-hook of InitStageList, right after
//     InitList: the game itself duplicates this list's cells there (07/11: a copy of the whole panel, made from the guide
//     bar's update, another screen's, crashed the game; no per-frame update of this screen's own was found: its Wait.OnUpdate
//     and UIPartsBgmSelect.OnUpdate do not run). Every other write is the frame's, through the core (R1, R35).
//   - Random (R): the game draws among its first cells only; the frame that sees its IsRandom draws again among every entry, then
//     among that entry's looks (Wael 07/11: Random lands on the extra stages and on variations too), and moves the cursor there
//     the game's way (section "Random" below).
//   - The list opens on the entry last fought (a tile too), scrolled so that it is on screen (sl::view_cursor).
// Without a tile, or with the hook refused, none of this runs: the screen works as before.

struct ClassicList {
    Obj* param = nullptr;              // the screen's Param; no reference held
    std::vector<sl::Slot> slots;       // its entries, once the tiles are in (empty: none)
    int entry = -1;                    // the focused entry
    int scroll = -1;                   // the list's ScrollIndex last refreshed
    uint32_t arrows_at = 0;            // the arrows' last pass
    bool ready = false;                // the game's previews have loaded
    bool random_done = false;          // this Random's pick is made (IsRandom seen)
    std::map<uint32_t, std::string> last;   // a stage -> the tile last fought over it: the cursor opens there
} g_cl;
thread_local uintptr_t tl_init_stage = 0;
void arrows_make_copies(Obj* param);

bool classic_on(Obj* param) { return param && !g_cl.slots.empty() && g_cl.param == param; }

const sl::TileGroup* group_of(const std::string& first_key) {
    for (auto& g : g_tile_groups) if (g.keys.front() == first_key) return &g;
    return nullptr;
}
// An entry's looks: "" (its own) and its variations; a tile: its group's
std::vector<std::string> entry_looks(const sl::Slot& s) {
    std::vector<std::string> looks;
    if (s.tile) {
        if (const sl::TileGroup* g = group_of(s.key)) looks = g->keys;
        else looks.push_back(s.key);
        return looks;
    }
    looks.push_back(std::string());
    for (const Variant* v : variations_of(s.base)) looks.push_back(v->key);
    return looks;
}
// The look an entry shows now
std::string entry_key(const sl::Slot& s) {
    const auto looks = entry_looks(s);
    std::string key;
    if (s.tile) {
        auto it = g_state.tile_pick.find(s.key);
        key = it != g_state.tile_pick.end() ? it->second : s.key;
    } else {
        auto it = g_selected.find(s.base);
        if (it != g_selected.end()) key = it->second;
    }
    return std::find(looks.begin(), looks.end(), key) != looks.end() ? key : looks.front();
}
// UP / DOWN on an entry. True when its look changed.
bool entry_cycle(const sl::Slot& s, int step) {
    const auto looks = entry_looks(s);
    if (looks.size() < 2) return false;
    const std::string now = entry_key(s);
    const int at = static_cast<int>(std::find(looks.begin(), looks.end(), now) - looks.begin());
    const std::string& next = looks[static_cast<size_t>(sl::cycle(at, static_cast<int>(looks.size()) - 1, step))];
    if (next == now) return false;
    if (s.tile) {
        if (next == s.key) g_state.tile_pick.erase(s.key); else g_state.tile_pick[s.key] = next;
    } else if (next.empty()) g_selected.erase(s.base);
    else g_selected[s.base] = next;
    logf("stages: classic list: stage %u -> %s", s.base, next.empty() ? "vanilla" : next.c_str());
    return true;
}

// ---- the list ----
int pre_init_stage(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_init_stage = (argc >= 2 && !g_closing.load(std::memory_order_relaxed)) ? reinterpret_cast<uintptr_t>(argv[1]) : 0;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_init_stage(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t p = tl_init_stage;
    tl_init_stage = 0;
    if (!p || g_closing.load(std::memory_order_relaxed)) return;
    int64_t pt = perf::begin();
    try {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        Obj* param = reinterpret_cast<Obj*>(p);
        g_cl.param = param;
        g_cl.slots.clear();
        g_cl.entry = g_cl.scroll = -1;
        g_cl.ready = false;
        g_cl.random_done = false;
        g_classic_keys.store(false, std::memory_order_relaxed);
        bool ok_mode = false;
        const bool event_mode = call_bool(param, "get_IsEventMode", {}, &ok_mode);
        Obj* list = call_obj(param, "get_StageIdList");
        Obj* parts = get_obj(param, "partsScrollList");
        const int n = list ? list_count(list) : 0;
        if (!list || !parts || n <= 0 || !ok_mode || event_mode || g_classic_tiles.empty()) { perf::end(perf::HkStgGrid, pt); return; }
        std::vector<uint32_t> ids;
        for (int i = 0; i < n; ++i) {
            const Ret r = call(m_list_item, list, { arg_i32(i) });
            if (r.exception_thrown) { perf::end(perf::HkStgGrid, pt); return; }
            ids.push_back(r.dword);
        }
        size_t pos = 0;
        std::vector<uint32_t> ins;
        auto slots = sl::plan_slots(ids, g_classic_tiles, pos, ins);
        if (ins.empty()) { perf::end(perf::HkStgGrid, pt); return; }
        bool ok = true;
        for (size_t k = 0; k < ins.size() && ok; ++k)
            ok = !call(m_list_insert, list, { arg_i32(static_cast<int32_t>(pos + k)), arg_i32(static_cast<int32_t>(ins[k])) }).exception_thrown;
        if (!ok || list_count(list) != static_cast<int>(slots.size())) {
            logf("stages: classic list: the tiles could not be put in the list (%d entries now): list left as the game made it", list_count(list));
            perf::end(perf::HkStgGrid, pt);
            return;
        }
        // the cursor: the game's, or the tile last fought over that stage
        int32_t sel = n - 1;
        get_i32(param, "DefaultIndex", sel);
        sel = std::max(0, std::min(sel, n - 1));
        auto last = g_cl.last.find(ids[static_cast<size_t>(sel)]);
        if (last != g_cl.last.end()) {
            const int e = sl::entry_index(slots, last->first, last->second);
            if (e >= 0) sel = e;
        }
        Obj* cells = get_obj(param, "stCtrl");
        const Ret rv = cells ? call(cells, "get_ItemCount") : Ret{};
        const int view = cells && !rv.exception_thrown ? static_cast<int32_t>(rv.dword) : n;   // the game's InitList: one cell per stage
        const Ret r = call(parts, "InitList", { arg_i32(static_cast<int32_t>(slots.size())), arg_i32(sel), arg_i32(sl::view_cursor(sel, view)) });
        if (r.exception_thrown) {
            logf("stages: classic list: InitList threw: list left at the game's %d entries", n);
            perf::end(perf::HkStgGrid, pt);
            return;
        }
        g_cl.slots = std::move(slots);
        g_classic_keys.store(true, std::memory_order_relaxed);
        logf("stages: classic list: %d stages + %zu tile(s), cursor on entry %d", n, ins.size(), sel);
        arrows_make_copies(param);
    } catch (...) {
        logf("stages: EXCEPTION while putting the tiles in the classic list");
    }
    perf::end(perf::HkStgGrid, pt);
}

void classic_shown() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_cl.slots.empty()) return;
    bool dropped = false;
    for (auto it = g_selected.begin(); it != g_selected.end();) {     // a tile chosen with UP / DOWN by an older version
        if (is_tile(it->first, it->second)) { logf("stages: classic list: tile %s is an entry of its own now", it->second.c_str()); it = g_selected.erase(it); dropped = true; }
        else ++it;
    }
    if (dropped) save_state(-1, "the tiles are entries of their own");
    g_cl.entry = -1;
    g_cl.arrows_at = 0;
}
void classic_hidden() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_cl.entry < 0 || g_cl.entry >= static_cast<int>(g_cl.slots.size())) return;
    const sl::Slot& s = g_cl.slots[static_cast<size_t>(g_cl.entry)];
    if (s.tile) g_cl.last[s.base] = s.key; else g_cl.last.erase(s.base);
    g_cl.entry = -1;
}

// The banner: the focused entry's name and picture. False while its picture loads.
bool classic_banner(Obj* param, const sl::Slot& s, const std::string& key, size_t looks) {
    Obj* text0 = get_obj(param, "text0");
    Obj* tex0 = get_obj(param, "texture0");
    if (!text0 || !tex0) return true;
    const std::string& own = vanilla_name(s.base);
    if (!own.empty()) g_scr.vanilla_name[s.base] = own;
    const Variant* v = key.empty() ? nullptr : variant_of(s.base, key);
    const std::string name = !v ? own : s.tile ? v->name : display_name(s.base, *v);
    if (!name.empty() && (v || looks > 1)) {                    // a stage with one look: the game's own text stays
        const std::string shown_text = looks > 1 ? decorate(name) : name;
        g_ours.insert(shown_text);
        set_text(text0, shown_text);
    }
    if (v && !v->preview.empty()) {
        if (!g_cl.ready) return false;
        Obj* h = preview_holder(v->preview);
        if (!h) return false;
        set_texture(tex0, h);
        return true;
    }
    // its own look: the game's picture, unless one of ours is left on it
    if (shown_pic(tex0).find("_stageslots/") != std::string::npos) {
        if (!g_cl.ready) return false;
        set_texture(tex0, vanilla_holder(param, s.base));
    }
    return true;
}

void classic_work(Obj* param, bool input) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const Ret ri = call(param, "GetSelectIndex");
    const int sel = ri.exception_thrown ? -1 : static_cast<int32_t>(ri.dword);
    if (sel < 0 || sel >= static_cast<int>(g_cl.slots.size())) return;
    const sl::Slot s = g_cl.slots[static_cast<size_t>(sel)];
    g_scr.decided = g_scr.stage = s.base;
    if (sel != g_cl.entry) {
        g_cl.entry = sel;
        g_scr.settle = 2;                                       // the game writes its banner for this id: ours again over it
    }
    const int pending = g_pending.exchange(0);
    const bool settling = g_scr.settle > 0;                     // presses during a change are dropped
    if (settling) --g_scr.settle;
    const bool changed = !settling && input && pending && entry_cycle(s, pending);
    const std::string key = entry_key(s);
    const sl::Choice served = sl::classic_served(g_selected, s.base, key, [](uint32_t b, const std::string& k) { return is_tile(b, k); });
    if (changed || served != g_state.active) save_state(sl::kClassic, nullptr, &served);
    if (changed) strip_open();
    const bool ready = classic_banner(param, s, key, entry_looks(s).size());
    g_scr.need = settling || !ready;
}

// ---- the cells: each shows its entry's look, read from its ListIndex (a cell's image names no entry: a tile shares its id) ----
bool strip_entries() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!g_cl.ready) return false;                              // the game's previews still load
    if (!g_strip.scanned || (g_strip.cells.empty() && g_frame - g_strip.scanned_at >= 30)) { if (!strip_scan()) return false; }
    for (auto& c : g_strip.cells) if (!alive(c.ctrl) || (c.mat && !alive(c.mat))) { if (!strip_scan()) return false; break; }
    bool done = true;
    for (auto& c : g_strip.cells) {
        if (!c.mat || call_bool(c.ctrl, "get_Dummy")) continue;
        const Ret ri = call(m_list_index, c.ctrl);
        const int li = ri.exception_thrown ? -1 : static_cast<int32_t>(ri.dword);
        if (li < 0 || li >= static_cast<int>(g_cl.slots.size())) continue;
        const sl::Slot& s = g_cl.slots[static_cast<size_t>(li)];
        const std::string key = entry_key(s);
        const Variant* v = key.empty() ? nullptr : variant_of(s.base, key);
        // what it must show: its look's thumbnail, else its stage's own
        const std::string path = v && !v->thumb.empty() ? v->thumb : vanilla_pic(s.base, true);
        Obj* cur = slots::ui::get_mat_tex(c.mat, kStageImage);
        const bool same = cur && lower_s(res_path(cur)).find(lower_s(path)) != std::string::npos;
        if (cur) cur->release();
        if (same) continue;
        Obj* want = preview_holder(path);
        if (want) slots::ui::set_mat_tex(c.mat, kStageImage, want);
        else done = false;                                      // loading: the cell keeps what it shows
    }
    return done;
}

// ---- Random ----
// R: the game draws among its first cells and puts the cursor there (UIPartsScrollList.SetSelectedIndex), then sets IsRandom and
// IsActive off, then IsDecide 22 frames later; BattleFlowMap.onStageSelectEnd writes the focused entry's stage (probe 07/11). The
// frame that first sees IsRandom draws again among every entry, then among that entry's looks, and moves the cursor there the
// game's way: what is shown, what is fought and the list after the fight are that one entry.
void classic_random(Obj* param) {
    std::vector<int> counts;
    for (auto& e : g_cl.slots) counts.push_back(static_cast<int>(entry_looks(e).size()));
    static uint32_t s_seed = GetTickCount() ^ 0x5bd1e995u;
    s_seed = s_seed * 1664525u + 1013904223u;
    const uint32_t r1 = s_seed >> 8;
    s_seed = s_seed * 1664525u + 1013904223u;
    const auto [e, l] = sl::random_pick(counts, r1, s_seed >> 8);
    if (e < 0) return;
    const sl::Slot s = g_cl.slots[static_cast<size_t>(e)];
    const std::string key = entry_looks(s)[static_cast<size_t>(l)];
    // the look, as UP / DOWN sets it
    if (s.tile) { if (key == s.key) g_state.tile_pick.erase(s.key); else g_state.tile_pick[s.key] = key; }
    else if (key.empty()) g_selected.erase(s.base);
    else g_selected[s.base] = key;
    const Ret ri = call(param, "GetSelectIndex");
    Obj* parts = get_obj(param, "partsScrollList");
    const Ret r = parts ? call(parts, "SetSelectedIndex", { arg_i32(e), arg_bool(false) }) : Ret{ };
    const Ret rn = call(param, "GetSelectIndex");
    const int now = rn.exception_thrown ? -1 : static_cast<int32_t>(rn.dword);
    logf("stages: classic list: Random: entry %d of %zu (stage %u, %s) in place of the game's entry %d%s", e, g_cl.slots.size(), s.base,
         key.empty() ? "its own look" : key.c_str(), ri.exception_thrown ? -1 : static_cast<int32_t>(ri.dword),
         !parts || r.exception_thrown || now != e ? ": the cursor did not move, the game's pick stays" : "");
    g_scr.settle = std::max(g_scr.settle, 1);                  // the game rewrites the banner for the new focus: ours over it
    g_scr.need = true;
    strip_open();
}

// ---- the arrows ----
const char* const kArrowName = "e_tex_slots_stage_more";       // not "p_...": never taken for a UI part (R20)
const char* const kArrowPanel[2] = { "p_MouseDecideOperable_L", "p_MouseDecideOperable_R" };
// From the centre of the first / last cell to the arrow, in the free corner its slanted side leaves (Wael's marks, 07/11)
constexpr float kArrowDx = 57, kArrowDy = 86;

// From the post-hook of InitStageList (R19): hidden and ForceInvisible, the frame places and shows them. Found again by name:
// they live and die with the screen's tree, which outlives a visit.
void arrows_make_copies(Obj* param) {
    const SF6CoreAPI* a = slots::ui::core();
    SF6CoreClient* cl = slots::ui::client();
    if (!a || !cl || !SF6CORE_HAS(a, ui_clone_control)) { logf("stages: classic list: the core has no ui_clone_control (0.8.2+): no arrows"); return; }
    Obj* part = get_obj(param, "partsBgmSelect");
    Obj* bgm = part ? get_obj(part, "<Control>k__BackingField") : nullptr;
    for (int i = 0; i < 2; ++i) {
        Obj* panel = bgm ? find_child(bgm, kArrowPanel[i], 0) : nullptr;
        if (!panel) { logf("stages: classic list: %s not found: no arrow on that side", kArrowPanel[i]); continue; }
        if (a->ui_find_child(panel, kArrowName, 0)) continue;
        void* tmpl = a->ui_find_child(panel, "e_tex_arrow", 0);
        void* c = nullptr;
        const SF6CoreStatus st = tmpl ? a->ui_clone_control(cl, tmpl, panel, kArrowName, &c) : SF6CORE_ERR_NOT_FOUND;
        if (st != SF6CORE_OK || !c) { logf("stages: classic list: arrow under %s not copied (status %d)", kArrowPanel[i], static_cast<int>(st)); continue; }
        a->ui_set_force_invisible(cl, c, 1);
        a->ui_set_visible(cl, c, 0);
        logf("stages: classic list: arrow under %s copied", kArrowPanel[i]);
    }
}

void arrows_pass(Obj* param, int first, int count) {
    const SF6CoreAPI* a = slots::ui::core();
    SF6CoreClient* cl = slots::ui::client();
    Obj* cm = alive(g_scr.agent) ? call_obj(g_scr.agent, "get_ControlMain") : nullptr;
    Obj* bgm = cm ? find_child_kept(cm, "p_BgmSelect_0") : nullptr;
    Obj* list = get_obj(param, "stCtrl");
    if (!a || !bgm || !list) return;
    const Ret rv = call(list, "get_ItemCount");
    const int view = rv.exception_thrown ? 0 : static_cast<int32_t>(rv.dword);
    float pos[3] = {}, step[3] = {}, bpos[3] = {};
    if (!floats_of(list, "get_Position", pos, 3) || !floats_of(list, "get_ItemOffset", step, 3) || !floats_of(bgm, "get_Position", bpos, 3)) return;
    const sl::MoreArrows more = sl::more_arrows(first, view, count);
    const float cell_x[2] = { pos[0], pos[0] + static_cast<float>(view - 1) * step[0] };
    for (int i = 0; i < 2; ++i) {
        const bool want = i ? more.right : more.left;
        Obj* panel = find_child(bgm, kArrowPanel[i], 0);
        Geo g;
        if (!panel || !geo_of(panel, g) || g.sx == 0 || g.sy == 0) continue;
        void* c = a->ui_find_child(panel, kArrowName, 0);
        if (!c) continue;
        if (want) {
            // the place in the panel's own space: it sits in the music selector, and its scale mirrors the arrow
            const float x = cell_x[i] + (i ? kArrowDx : -kArrowDx), y = pos[1] + (i ? kArrowDy : -kArrowDy);
            a->ui_set_position(cl, c, (x - bpos[0] - g.x) / g.sx, (y - bpos[1] - g.y) / g.sy, 0.0f);
            a->ui_set_force_invisible(cl, c, 0);
        }
        a->ui_set_visible(cl, c, want ? 1 : 0);
    }
}

// Every frame the screen is up with its tiles: the previews loaded?, the list scrolled?, the arrows
void classic_frame() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Obj* param = g_scr.param;
    if (!alive(param) || !classic_on(param)) return;
    const bool rnd = bool_field(param, f_param_random);
    if (rnd && !g_cl.random_done) { g_cl.random_done = true; classic_random(param); }
    else if (!rnd) g_cl.random_done = false;
    if (!g_cl.ready) {
        bool ok = false;
        const bool loaded = call_bool(param, "IsPreviewTexCahceReady", {}, &ok);
        if (!ok || !loaded) return;
        g_cl.ready = true;
        std::set<uint32_t> bases;                              // ahead of the cursor: the tiles' pictures
        for (auto& t : g_tiles) bases.insert(t.base);
        for (uint32_t b : bases) preload_previews(b);
        for (auto& e : g_cl.slots) preview_holder(vanilla_pic(e.base, true));
        strip_open();
        g_scr.need = true;
        logf("stages: classic list: the game's previews are loaded");
    }
    Obj* list = get_obj(param, "stCtrl");
    if (!list) return;
    const Ret rs = call(list, "get_ScrollIndex");
    if (rs.exception_thrown) return;
    const int scroll = static_cast<int32_t>(rs.dword);
    const bool moved = scroll != g_cl.scroll;
    if (moved) {
        const bool scrolled = g_cl.scroll >= 0 || scroll > 0;  // not the list's first look, as the game left it
        g_cl.scroll = scroll;
        if (scrolled) call(param, "UpdateShopIcon");
        strip_entries();
        strip_open();
        const Ret rc = call(list, "get_CursorIndex"), rv = call(list, "get_ItemCount"), rm = call(list, "get_ItemMax");
        logf("stages: classic list: scroll %d, cursor %d (%d cells, %d entries)", scroll, rc.exception_thrown ? -1 : static_cast<int32_t>(rc.dword),
             rv.exception_thrown ? -1 : static_cast<int32_t>(rv.dword), rm.exception_thrown ? -1 : static_cast<int32_t>(rm.dword));
    }
    if (moved || g_frame - g_cl.arrows_at >= 10 || !g_cl.arrows_at) {
        g_cl.arrows_at = g_frame;
        arrows_pass(param, scroll, static_cast<int>(g_cl.slots.size()));
    }
}

// ---- install: the code check first, the hooks once it answered ----
void start_grid_check() {
    auto* tdb = api().tdb();
    t_simple = tdb->find_type("app.UIPartsStageSelectSimple");
    TD* t_gui = tdb->find_type("app.helper.hGUI");
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
    f_decide = t_simple ? field_of(t_simple, "mDecideStageId") : nullptr;
    f_rand_off = t_simple ? field_of(t_simple, "mListRandomDisable") : nullptr;
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
    if (TD* t_param = tdb->find_type("app.menu.UIFlowStageSelect.Param")) f_param_random = field_of(t_param, "IsRandom");
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
    add(t_session, "SetupBattleDesc", 2, "app.network.FGBattleSession.SetupBattleDesc(BattleDesc,Boolean)", true);
    add(t_training, "SetStage", 1, "app.training.TrainingManager.SetStage(StageDesc)", true);
    add(t_replay, "setupDesc", 0, "app.battle.ReplayFlowMap.setupDesc()", f_map_desc && f_desc_stage && f_stage_id);
    add(tdb->find_type("app.menu.UIFlowStageSelect.Param"), "InitStageList", 0, "app.menu.UIFlowStageSelect.Param.InitStageList()",
        !g_classic_tiles.empty() && m_list_item && m_list_insert && m_list_index);
    add(tdb->find_type("app.UIPartsMatchingSettingBattle"), "ButtonY", 0, "app.UIPartsMatchingSettingBattle.ButtonY()", grid_ok);
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
        { kDecide, pre_decide, post_decide },
        { kUpdateList, pre_grid_dirty, nullptr },
        { kSelChanged, pre_grid_dirty, nullptr },
        { kBattleDesc, pre_battle_desc, post_battle_desc },
        { kTrainStage, pre_training_stage, nullptr },
        { kReplayDesc, pre_replay_desc, post_replay_desc },
        { kInitStageList, pre_init_stage, post_init_stage },
        { kButtonY, pre_button_y, nullptr },
    };
    // the tiles need the list, its owner (SetupList) and Decide: one refused, none of the three
    const bool core = g_check.unique(kSetupList) && g_check.unique(kGetList) && g_check.unique(kDecide);
    for (auto& h : hooks) {
        const auto& e = g_check.entry(h.id);
        Method* m = g_check.method(h.id);
        const bool grid_part = h.id <= kSelChanged;
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
        e.bundle = v ? v->bundle : std::string();
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

bool variation_key_down() { return g_variant_count && variation_key_now(); }

bool replay_key_down() { return variation_key_now(); }
static void replay_hint(const char* label, int key) {
    const bool asked = g_hint_replay_at && g_frame - g_hint_replay_at <= 2;
    g_hint_replay_at = g_frame;
    g_hint_replay_label = label;
    g_hint_replay_key = key;
    if (!asked) post(EV_STAGE_GRID);                     // the stages' frame runs while it is asked (busy)
}
void replay_guide_hint(const char* label) { replay_hint(label, sl::kVariationKeyId); }
bool replay_music_key_down() { return game_key_now(g_mk); }
void replay_music_hint(const char* label) { replay_hint(label, sl::kCharaMusicKeyId); }
void replay_variation_hint() {
    if (g_variant_count) replay_guide_hint(words::tr(words::W::StageVariation));
}

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
    if (!g_variant_count) return g_hint.shown || (g_hint_replay_at && g_frame - g_hint_replay_at <= 2);
    if ((g_check_started && !g_hooks_done) || g_grid.until != 0 || g_decided.pending || g_cycle_pending.load() != 0 || g_tick_all_pending.load()) return true;
    if (g_grid.on_screen || g_grid_watch || g_hint.shown || (g_hint_replay_at && g_frame - g_hint_replay_at <= 2)) return true;   // the key, the hint
    if (g_scr.active && classic_on(g_scr.param)) return true;   // the list's scroll, the arrows
    return (g_scr.active && (g_scr.need || g_scr.settle > 0 || g_strip.until != 0)) || g_vs.until != 0 || g_scr.check_until != 0;
}

// The pictures the VS screen may need, loaded ahead (a picture takes a second to be usable, the screen is up by then): what
// the loader serves, each menu's choices, and the tiles of a menu on Random (its pick is made when the battle is set up).
void preload_choices() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    static stagelogic::State s_seen;
    bool same = s_seen.active == g_state.active;
    for (int m = 0; same && m < stagelogic::kMenus; ++m) same = s_seen.menu[m] == g_state.menu[m] && s_seen.random[m] == g_state.random[m];
    if (same) return;
    s_seen = g_state;
    auto load = [](uint32_t id, const std::string& key) {
        const Variant* v = key.empty() ? nullptr : variant_of(id, key);
        if (v && !v->preview.empty()) preview_holder(v->preview);
    };
    for (auto& [id, key] : g_state.active) load(id, key);
    for (int m = 0; m < stagelogic::kMenus; ++m) {
        for (auto& [id, key] : g_state.menu[m]) load(id, key);
        if (g_state.random[m]) for (auto& t : g_tiles) load(t.base, t.key);
    }
}

void tick(uint32_t ev) {
    if (!g_variant_count) {
        if (g_hint_replay_at) replay_hint_frame();
        return;
    }
    preload_choices();
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
        classic_frame();
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
