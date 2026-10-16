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
#include "slots.hpp"
#include "slots_ui.hpp"
#include "perf.hpp"
#include "wjournal.hpp"

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "user32.lib")   // MapVirtualKeyW (keyboard hints)

namespace slots::stages {
namespace {

const char* kRegistry = "reframework/data/SF6_StageSlots_Data/registry.json";
const char* kState    = "reframework/data/SF6_StageSlots_Data/state.json";

struct Variant { std::string key, name, bundle, preview, thumb; };
std::map<uint32_t, std::vector<Variant>> g_variants;      // stage id -> variants
size_t g_variant_count = 0;
std::map<uint32_t, std::string> g_selected;                // stage id -> variant key
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
            if (!x.key.empty()) { list.push_back(x); ++g_variant_count; }
        }
    }
}

void load_state() {
    Json j;
    if (!json_parse(read_file(kState), j)) return;
    const Json* sel = j.get("selected");
    if (!sel || sel->kind != Json::Object) return;
    for (auto& [k, v] : sel->obj) if (v.kind == Json::String) g_selected[static_cast<uint32_t>(strtoul(k.c_str(), nullptr, 10))] = v.s;
}

void save_state() {
    std::string o = "{\n    \"selected\": {";
    bool first = true;
    for (auto& [id, key] : g_selected) {
        o += first ? "\n        \"" : ",\n        \"";
        o += std::to_string(id) + "\": \"" + json_escape(key) + "\"";
        first = false;
    }
    o += first ? "}\n}\n" : "\n    }\n}\n";
    { int64_t pt = perf::begin(); if (!write_file(kState, o)) logf("stages: cannot write state.json"); perf::end(perf::StateWrite, pt); }
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
    auto v = g_variants.find(stage);
    if (idx == 0 || v == g_variants.end()) g_selected.erase(stage);
    else g_selected[stage] = v->second[static_cast<size_t>(idx - 1)].key;
    save_state();
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

void strip_collect(Obj* ctrl, int depth, std::vector<Cell>& out) {
    if (!ctrl || depth > 10) return;
    for (Obj* c = call_obj(ctrl, "get_Child"); c; c = call_obj(c, "get_Next")) {
        std::string n = str(call_obj(c, "get_Name"));
        if (n.compare(0, strlen(kCellPrefix), kCellPrefix) == 0 || n.compare(0, 4, "item") == 0) {
            Cell x;
            x.ctrl = c; x.name = n;
            x.mat = find_child(c, "e_mat_thumb", 0);
            if (x.mat) out.push_back(std::move(x));
        } else strip_collect(c, depth + 1, out);
    }
}

// The strip's cells; what was learnt of those still there is kept
bool strip_scan() {
    g_strip.scanned = true;
    g_strip.scanned_at = g_frame;
    Obj* cm = alive(g_scr.agent) ? call_obj(g_scr.agent, "get_ControlMain") : nullptr;
    Obj* root = cm ? find_child(cm, "c_stage", 0) : nullptr;
    // the list sits next to c_stage under the screen's main control (seen in game 16/10): its cells are item1..itemN
    Obj* list = cm ? find_child(cm, "p_ScrollList_0_h_loop", 0) : nullptr;
    std::vector<Cell> found;
    if (list) strip_collect(list, 0, found);
    for (auto& f : found)
        for (auto& o : g_strip.cells)
            if (o.ctrl == f.ctrl && o.mat == f.mat) { f.vanilla = o.vanilla; o.vanilla = nullptr; f.stage = o.stage; f.shown = o.shown; break; }
    for (auto& o : g_strip.cells) drop(o.vanilla);
    g_strip.cells = std::move(found);
    if (g_strip.cells.empty() && !g_strip.warned) {
        g_strip.warned = true;
        logf("stages: stage strip not found (c_stage %s, p_ScrollList_0_h_loop %s): its cells keep the game's images",
             root ? "ok" : "-", list ? "ok" : "-");
    }
    return !g_strip.cells.empty();
}

// false while something is left to do (strip not built yet, a preview loading)
bool strip_work() {
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

} // namespace

// ---------------------------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------------------------

void init() {
    load_registry();
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
}

bool busy() {
    if (!g_variant_count) return false;
    return (g_scr.active && (g_scr.need || g_scr.settle > 0 || g_strip.until != 0)) || g_vs.until != 0 || g_scr.check_until != 0;
}

void tick(uint32_t ev) {
    if (!g_variant_count) return;
    // The game calls BeforeHideObject while it sets the screen up, next to ShowedObject, and coming
    // back from the result screen the screen shows well after its events: a show or hide event, or
    // the game rewriting the screen while it is not followed, starts a check of its visibility for
    // 3 s (every 5 frames)
    if ((ev & (EV_STAGE_SHOW | EV_STAGE_HIDE)) || ((ev & EV_STAGE_DIRTY) && !g_scr.active)) g_scr.check_until = g_frame + 180;
    if (g_scr.check_until && (g_frame % 5 == 0 || (ev & (EV_STAGE_SHOW | EV_STAGE_HIDE)))) {
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
