// dialogs::editor - the game side of the native costume colour editor and DI colour menu. See editor.hpp.
#include "editor.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

#include "ll_common.hpp"

#include "../common.hpp"
#include "../engine/live.hpp"
#include "../engine/live_internal.hpp"
#include "../screens/guide.hpp"
#include "camera.hpp"
#include "editor_core.hpp"
#include "locale.hpp"
#include "options_driver.hpp"
#include "row_look.hpp"
#include "../vfx/drive_cfg.hpp"
#include "../vfx/feed.hpp"
#include "../vfx/settings.hpp"
#include "../engine/decide.hpp"
#include "../screens/colour_track.hpp"

namespace dialogs::editor {

using API = reframework::API;
using MO = API::ManagedObject;
namespace fs = std::filesystem;

namespace {

bool g_installed = false;
int g_faults = 0;
constexpr int kMaxFaults = 5;
const char* const kData = "reframework/data/";
const char US = '\x1f', RS = '\x1e', GS = '\x1d';

// ---- what the relay pushes -------------------------------------------------------------------------------------
struct Screen {
    bool visible = false, pad = false, busy = false;
    uint64_t agent = 0;                                  // the Fighter Settings agent (camera gate)
    int64_t frame = 0;
    uint32_t pad_mask = 0;
    bool has_mouse = false; double mx = 0, my = 0, dw = 1920, dh = 1080;
    // what editor_open reads from the screen, pushed before an open
    int fighter = 0, costume = 0, color_id = 0, k = 0;
    bool has_preview = false, has_live = false, has_chips = false;
    uint32_t g0 = 0, g1 = 0;
} S;
std::map<std::string, std::vector<std::string>> g_union;         // "<fighter>_<costume>" -> the live colours' enabled slots
// the DI menu's data, as pushed at its open and changed by the menu since
struct DiData {
    bool available = false, special = false;
    std::string name;
    int fighter = 0, side = 1;                                    // whose settings the menu edits (phase 5c-1: the DLL's)
    bool has_cfg[5] = {};
    core::DiCfg cfg[5];
    bool has_list[3] = {};
    std::vector<core::DiPreset> list[3];
} D;

// ---- orders for the relay ----------------------------------------------------------------------------------------
std::string g_ops;
int g_nops = 0;
int g_next_id = 1;                                               // the core's group / entry ids (the relay maps them to its tables)
std::unordered_map<int, int64_t> g_engine_of;                    // core entry id -> the option engine's entry id
std::unordered_map<int64_t, int> g_core_of;
void op(std::initializer_list<std::string> fields) {
    bool first = true;
    for (auto& f : fields) { if (!first) g_ops += US; g_ops += f; first = false; }
    g_ops += RS;
    ++g_nops;
}
std::string n(int64_t v) { return std::to_string(v); }
std::string tri(core::Tri t) { return n((int)t); }
std::string col(const std::optional<uint32_t>& c) { return c ? n(*c) : std::string(); }

// ---- the option engine: orders out, queries in -------------------------------------------------------------------
int be_opt_group(const std::string& key, const std::string& title, const std::string& desc) { const int id = g_next_id++; op({ "G", n(id), key, title, desc }); return id; }
void be_opt_remove_group(int gid) { op({ "R", n(gid) }); }
int be_opt_button(int gid, const std::string& key, const std::string& title, const std::string& desc) { const int id = g_next_id++; op({ "B", n(id), n(gid), key, title, desc }); return id; }
int be_opt_slider(int gid, const std::string& key, const std::string& title, const std::string& desc, int mn, int mx, int init) {
    const int id = g_next_id++; op({ "S", n(id), n(gid), key, title, desc, n(mn), n(mx), n(init) }); return id;
}
int be_opt_choice(int gid, const std::string& key, const std::string& title, const std::string& desc, const std::vector<std::string>& options, int init) {
    const int id = g_next_id++;
    std::string opts;
    for (size_t i = 0; i < options.size(); ++i) { if (i) opts += GS; opts += options[i]; }
    op({ "C", n(id), n(gid), key, title, desc, n(init), opts });
    return id;
}
void be_opt_group_def(int gid, const std::string& title, const std::string& desc) { op({ "D", n(gid), title, desc }); }
void be_opt_rebuild() { op({ "RB" }); }
bool be_opt_ensure_built() { op({ "EB" }); return true; }
void be_opt_open_menu(const std::string& key, int focus) { op({ "OM", key, n(focus) }); }
void be_opt_navigate(const std::string& key, core::Tri rebuild, int focus, core::Tri now, core::Tri push, core::Tri pop) { op({ "NV", key, tri(rebuild), n(focus), tri(now), tri(push), tri(pop) }); }
void be_opt_close() { op({ "CL" }); }
bool be_opt_window_open() { return options::driver::window_open(); }
int be_opt_focused() {
    auto it = g_core_of.find(options::driver::focused_entry());
    return it == g_core_of.end() ? -1 : it->second;
}
core::Obj be_opt_row_control(int eid, core::Obj param) {
    auto it = g_engine_of.find(eid);
    if (it == g_engine_of.end()) return nullptr;
    return reinterpret_cast<core::Obj>(options::driver::row_control(it->second, param));
}
core::Obj be_opt_current_param() { return options::driver::current_param(); }
core::Obj be_opt_parent_param() { return options::driver::parent_param(); }

// ---- the dialog's controls ---------------------------------------------------------------------------------------
bool alive(void* o) { return o && API::get()->sdk()->managed_object->is_managed_object(o); }
reframework::InvokeRet call0(void* o, const char* name, bool& ok) {
    ok = false;
    if (!alive(o)) return {};
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(o)->get_type_definition(), name);
    if (!m) return {};
    auto r = m->invoke(reinterpret_cast<MO*>(o), std::vector<void*>{});
    ok = !r.exception_thrown;
    return r;
}
void* call_obj(void* o, const char* name) { bool ok; auto r = call0(o, name, ok); return ok ? r.ptr : nullptr; }
bool call_bool(void* o, const char* name, bool& ok) { auto r = call0(o, name, ok); return ok && r.dword != 0; }
std::string call_str(void* o, const char* name) { bool ok; auto r = call0(o, name, ok); return (ok && r.ptr) ? ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr)) : std::string(); }
bool call_floats(void* o, const char* name, float* out, int count) {
    bool ok; auto r = call0(o, name, ok);
    if (!ok) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * (size_t)count);
    return true;
}
core::Obj be_param_control_main(core::Obj param) {
    if (!alive(param)) return nullptr;
    MO* agent = ll::read_field_object(reinterpret_cast<MO*>(param), "<Agent>k__BackingField");
    return agent ? call_obj(agent, "get_ControlMain") : nullptr;
}
std::vector<core::Obj> be_dialog_control_mains() {
    std::vector<core::Obj> out;
    const SF6CoreAPI* api = cmn::core();
    if (!api || !SF6CORE_HAS(api, ui_agents)) return out;
    void* agents[16] = {};
    const uint32_t cnt = api->ui_agents("OptionDialog", agents, 16);
    for (uint32_t i = 0; i < cnt && i < 16; ++i) if (void* cm = call_obj(agents[i], "get_ControlMain")) out.push_back(cm);
    return out;
}
core::Obj be_ui_child(core::Obj o) { return call_obj(o, "get_Child"); }
core::Obj be_ui_next(core::Obj o) { return call_obj(o, "get_Next"); }
std::string be_ui_name(core::Obj o) { return call_str(o, "get_Name"); }
std::string be_ui_message(core::Obj o) { return call_str(o, "get_Message"); }
bool be_ui_visible(core::Obj o) { bool ok; return call_bool(o, "get_Visible", ok); }
bool be_ui_actual_visible(core::Obj o, bool& v) { bool ok; v = call_bool(o, "get_ActualVisible", ok); return ok; }
bool be_ui_force_invisible(core::Obj o) { bool ok; return call_bool(o, "get_ForceInvisible", ok); }
bool be_ui_position(core::Obj o, double& x, double& y, double& z) { float f[3]; if (!call_floats(o, "get_Position", f, 3)) return false; x = f[0]; y = f[1]; z = f[2]; return true; }
bool be_ui_size(core::Obj o, double& w, double& h) { float f[2]; if (!call_floats(o, "get_Size", f, 2)) return false; w = f[0]; h = f[1]; return true; }
bool be_ui_scale(core::Obj o, double& x) { float f[3]; if (!call_floats(o, "get_Scale", f, 3)) return false; x = f[0]; return true; }
bool be_ui_font_size(core::Obj o, double& h) { float f[2]; if (!call_floats(o, "get_FontSize", f, 2)) return false; h = f[1]; return true; }
core::Obj be_ui_find(core::Obj root, const std::string& name) {
    const SF6CoreAPI* api = cmn::core();
    return (api && alive(root) && SF6CORE_HAS(api, ui_find_cached)) ? api->ui_find_cached(root, name.c_str()) : nullptr;
}
// writes: the shared core's display API (compare-before-write, proof of life, as func/CoreUI)
void be_ui_set_text(core::Obj o, const std::string& s) { if (auto* a = cmn::core()) a->ui_set_text(cmn::client(), o, s.c_str()); }
void be_ui_set_position(core::Obj o, double x, double y, double z) { if (auto* a = cmn::core()) a->ui_set_position(cmn::client(), o, (float)x, (float)y, (float)z); }
void be_ui_set_size(core::Obj o, double w, double h) { if (auto* a = cmn::core()) a->ui_set_size(cmn::client(), o, (float)w, (float)h); }
void be_ui_set_scale(core::Obj o, double x, double y, double z) { if (auto* a = cmn::core()) a->ui_set_scale(cmn::client(), o, (float)x, (float)y, (float)z); }
void be_ui_set_visible(core::Obj o, bool on) { if (auto* a = cmn::core()) a->ui_set_visible(cmn::client(), o, on ? 1 : 0); }
void be_ui_set_force_invisible(core::Obj o, bool on) { if (auto* a = cmn::core()) a->ui_set_force_invisible(cmn::client(), o, on ? 1 : 0); }
void be_ui_set_font_size(core::Obj o, double w, double h) { if (auto* a = cmn::core()) a->ui_set_font_size(cmn::client(), o, (float)w, (float)h); }
// the menus' LEFT / RIGHT (the palette cursor, 0.21.0): the arrows and the physical A / D keys of the layout in use
// (Q / D on AZERTY, the game's defaults), the pad's d-pad and left stick (via.hid.GamePadButton LLeft / LRight,
// EmuLleft / EmuLright)
void be_select_lr(bool& left, bool& right) {
    static const int vl = (int)MapVirtualKeyW(0x1E, MAPVK_VSC_TO_VK), vr = (int)MapVirtualKeyW(0x20, MAPVK_VSC_TO_VK);
    left = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0 || (vl && (GetAsyncKeyState(vl) & 0x8000) != 0) || (S.pad_mask & (4u | 8388608u)) != 0;
    right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0 || (vr && (GetAsyncKeyState(vr) & 0x8000) != 0) || (S.pad_mask & (8u | 2097152u)) != 0;
}
void be_row_looks(core::Obj param, const std::vector<core::RowLook>& rows) { look::apply(param, rows, be_opt_row_control); }
// a row's value in place (0.21.0: the sliders follow a palette cell): the unit's own SetValue through the core's guarded
// invoke; the engine reads it back at its next poll and the editor takes that echo for its own
bool be_opt_set_value(int eid, int value) {
    auto it = g_engine_of.find(eid);
    const SF6CoreAPI* a = cmn::core();
    if (it == g_engine_of.end() || !a || !SF6CORE_HAS(a, opt_row_part)) return false;
    void* part = a->opt_row_part(it->second);
    MO* unit = alive(part) ? ll::read_field_object(reinterpret_cast<MO*>(part), "UnitData") : nullptr;
    if (!unit) return false;
    SF6CoreArg args[2] = {};
    args[0].type = SF6CORE_ARG_I64; args[0].v.i = value;
    args[1].type = SF6CORE_ARG_BOOL; args[1].v.i = 1;
    return a->obj_invoke(cmn::client(), unit, "SetValue(System.Int32, System.Boolean)", args, 2, nullptr) == SF6CORE_OK;
}

// ---- input, time, random, locale ---------------------------------------------------------------------------------
bool be_key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }      // what reframework:is_key_down reads

// The game's tab keys (app.InputAssign.Digital.Id UITabL / UITabR, L1 / R1 on a pad): the editor turns its pages with
// them. On a keyboard the game names the physical key by the layout in use (A on an AZERTY keyboard, Q on a QWERTY
// one, where A moves left in the menus), so the symbol comes from the game (InputGuideManager.GetSymbolText,
// keyboard mode) and the key read is the letter it names (Windows letter keys follow the layout too). A symbol
// naming no letter or digit, or no answer: the game's default physical keys (scan codes 0x10 / 0x12) in the layout
// in use. Asked again every 120 frames: the key configuration or the layout can change while the game runs.
struct PageKeys { int64_t at = INT64_MIN; std::string sym[2]; int vk[2] = {}; std::string sig; };
PageKeys g_page_keys;
int key_letter(const std::string& s) {                         // "<ICON KeyQ>" -> 'Q'; 0 = no single letter / digit
    static const std::string pre = "<ICON Key";
    const size_t i = s.find(pre);
    if (i == std::string::npos || i + pre.size() + 1 >= s.size() || s[i + pre.size() + 1] != '>') return 0;
    const char c = s[i + pre.size()];
    return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : 0;
}
void read_page_keys() {
    static API::Method* m_symbol = nullptr;
    static int32_t ids[2] = { -1, -1 };
    static bool resolved = false;
    auto* tdb = API::get()->tdb();
    if (!resolved) {
        resolved = true;
        auto* id_td = tdb->find_type("app.InputAssign.Digital.Id");
        const char* names[2] = { "UITabL", "UITabR" };
        for (int i = 0; i < 2; ++i)
            if (auto* f = id_td ? id_td->find_field(names[i]) : nullptr) ids[i] = f->get_data<int32_t>(nullptr);
        if (auto* gm_td = tdb->find_type("app.InputGuideManager"))
            m_symbol = ll::find_method_full(gm_td, "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)");
    }
    MO* gm = API::get()->get_managed_singleton("app.InputGuideManager");
    std::string sig;
    for (int i = 0; i < 2; ++i) {
        std::string s;
        if (gm && m_symbol && ids[i] >= 0) {
            constexpr int32_t kModeKeyboard = 2;                // app.InputGuideMode: 1 GamePad, 2 KeyboardAndMouse
            auto r = m_symbol->invoke(gm, std::vector<void*>{ ll::arg_i32(ids[i]), ll::arg_i32(kModeKeyboard), ll::arg_i32(0), ll::arg_i32(0) });
            if (!r.exception_thrown && r.ptr) s = ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr));
        }
        int vk = key_letter(s);
        const char* from = "game";
        if (!vk) {
            const UINT pv = MapVirtualKeyW(i ? 0x12 : 0x10, MAPVK_VSC_TO_VK);
            vk = (int)pv;
            s = ((pv >= 'A' && pv <= 'Z') || (pv >= '0' && pv <= '9')) ? std::string("<ICON Key") + (char)pv + ">" : std::string();
            from = "physical default";
        }
        g_page_keys.sym[i] = s;
        g_page_keys.vk[i] = vk;
        char b[96];
        std::snprintf(b, sizeof b, "%s%s (vk 0x%02X, %s)", i ? " / " : "", s.c_str(), vk, from);
        sig += b;
    }
    if (sig != g_page_keys.sig) {
        g_page_keys.sig = sig;
        cmn::journal("editor page keys: %s", sig.c_str());
    }
}
bool be_page_key(bool next, std::string& symbol, int& vk) {
    if (g_page_keys.at == INT64_MIN || S.frame - g_page_keys.at >= 120 || S.frame < g_page_keys.at) {
        g_page_keys.at = S.frame;
        try { read_page_keys(); } catch (...) { g_page_keys.sym[0].clear(); g_page_keys.sym[1].clear(); }
    }
    const int i = next ? 1 : 0;
    symbol = g_page_keys.sym[i];
    vk = g_page_keys.vk[i];
    return !symbol.empty() && vk > 0;
}
uint32_t be_pad_mask() { return S.pad_mask; }
bool be_mouse(double& x, double& y, double& dw, double& dh) { if (!S.has_mouse) return false; x = S.mx; y = S.my; dw = S.dw; dh = S.dh; return true; }
int64_t be_frame() { return S.frame; }
double be_clock() { return (double)std::clock() / CLOCKS_PER_SEC; }            // os.clock()
std::mt19937& rng() { static std::mt19937 g{ std::random_device{}() }; return g; }
int be_rand_int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng()); }
double be_rand_float() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng()); }
std::string be_L(const std::string& key, const std::vector<std::string>& args) { return locale::get(key, args); }

// ---- files, relative to reframework/data -------------------------------------------------------------------------
bool be_file_read(const std::string& rel, std::vector<uint8_t>& bytes) {
    std::ifstream f(std::string(kData) + rel, std::ios::binary);
    if (!f) return false;
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
// through a temporary file and a rename: a crash in the middle leaves the colour file as it was
bool be_file_write(const std::string& rel, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    const fs::path p = fs::path(kData) / rel;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = fs::path(p.string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { cmn::journal("[editor] cannot write %s", rel.c_str()); return false; }
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        if (!f) { cmn::journal("[editor] short write %s", rel.c_str()); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { cmn::journal("[editor] cannot replace %s: %s", rel.c_str(), ec.message().c_str()); fs::remove(tmp, ec); return false; }
    if (rel.size() >= 15 && rel.compare(rel.size() - 15, 15, "chip_slots.json") == 0) {
        if (!screens::colour::track::acting()) op({ "CHIPS" });
        screens::colour::track::chips_reload();
    }   // the Lua keeps that file in memory
    return true;
}
bool be_file_delete(const std::string& rel) {
    std::string r = rel;
    for (auto& c : r) if (c == '\\') c = '/';
    // the mod's own files only: its data folder, and the editor's colours in color_mods (MANUALCOLORS, never a mod's)
    const bool own = r.rfind("SF6_ColorSpinExtra_data/", 0) == 0 && r.find("..") == std::string::npos;
    const bool manual = r.rfind("../color_mods/", 0) == 0 && r.find("..", 3) == std::string::npos &&
                        r.find("/MANUALCOLORS") != std::string::npos && r.find("/.cache/") == std::string::npos;
    if (!own && !manual) return false;
    std::error_code ec;
    const fs::path p = fs::path(kData) / r;
    const bool ok = fs::is_regular_file(p, ec) && fs::remove(p, ec);
    if (ok) cmn::journal("file deleted: %s", r.c_str());
    return ok;
}
void be_purge_empty_files() {                                                   // the files emptied before the DLL could delete
    int removed = 0;
    // the edited copies, and the editor's colours in color_mods (MANUALCOLORS folders: never a mod's file)
    for (const fs::path& root : { fs::path(kData) / "SF6_ColorSpinExtra_data" / "overrides", fs::path(kData) / ".." / "color_mods" }) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        const bool mods = root.filename() == "color_mods";
        std::vector<fs::path> gone;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (mods && it->is_directory(ec) && it->path().filename() == ".cache") { it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec) || it->file_size(ec) != 0 || ec) continue;
            const std::string nm = it->path().filename().string();
            if (mods && it->path().parent_path().filename().string().rfind("MANUALCOLORS", 0) != 0) continue;
            if (nm.size() > 7 && nm.compare(nm.size() - 7, 7, ".user.2") == 0) gone.push_back(it->path());
        }
        for (auto& p : gone) if (fs::remove(p, ec)) ++removed;
    }
    if (removed > 0) cmn::journal("empty colour files deleted: %d", removed);
}

// ---- the preview and the game data behind it ----------------------------------------------------------------------
void be_refresh() {                                                             // editor_refresh
    engine::rsz::NodeRef tree; bool hl_on = false; double k = 0.5; std::vector<const engine::rsz::Node*> hl;
    if (core::preview(tree, hl_on, k, hl)) engine::live::editor_share(tree, hl_on, k, hl);   // the working copy itself: no copy, no file
    else engine::live::editor_active(false);
    if (screens::colour::track::acting()) screens::colour::track::editor_refresh();   // every preview controller takes its colour again
    else op({ "RF" });
}
bool be_has_preview() { return S.has_preview; }
engine::rsz::NodeRef be_live_tree(int /*color_id*/) {                           // written by the relay before the open (its live_to_tree)
    if (!S.has_live) return nullptr;
    std::vector<uint8_t> bytes;
    if (!be_file_read("SF6_ColorSpinExtra_data/_editor_live.user.2", bytes)) return nullptr;
    return engine::rsz::parse(bytes).root;
}
std::vector<std::string> be_slot_union(int fighter, int costume) {
    auto it = g_union.find(std::to_string(fighter) + "_" + std::to_string(costume));
    return it == g_union.end() ? std::vector<std::string>{} : it->second;
}
void be_apply_original(int color_id, const engine::rsz::NodeRef& tree) {        // RESET of a game colour: the live data back (the relay's apply_tree)
    if (!tree) return;
    if (!be_file_write("SF6_ColorSpinExtra_data/_editor_original.user.2", engine::rsz::serialize(tree))) return;
    if (screens::colour::track::acting()) screens::colour::track::apply_original(color_id, tree);
    else op({ "AO", n(color_id) });
}
bool be_game_chips(int fighter, int costume, int num, uint32_t& c0, uint32_t& c1) {
    if (!S.has_chips || fighter != S.fighter || costume != S.costume || num != S.color_id) return false;
    c0 = S.g0; c1 = S.g1;
    return true;
}

// ---- the screens, the guide bar, the camera gate ------------------------------------------------------------------
bool be_screen_visible() { return S.visible; }
bool be_screen_pad() { return S.pad; }
bool be_screen_busy() { return S.busy; }
void be_screen_refresh_list(bool has_path, const std::string& path) {
    if (!screens::colour::track::acting()) op({ "RL", has_path ? "1" : "0", path });
    screens::colour::track::refresh_request(has_path, path);
}
bool be_open_args(int& f, int& c, int& color, int& k) { f = S.fighter; c = S.costume; color = S.color_id; k = S.k; return true; }
void be_catalog_changed() {
    if (screens::colour::track::acting()) engine::decide::rebuild_catalog(); else op({ "CAT" });   // the relay's build_catalog did that
    screens::colour::track::catalog_rebuilt();
}
void be_caches_invalidated() {
    if (!screens::colour::track::acting()) op({ "INV" });
    screens::colour::track::invalidate();
}
void be_guide_bar(const std::string& want, const std::string& desc, core::Obj param) { screens::guide::set_bar(want, desc, (uint64_t)reinterpret_cast<uintptr_t>(param)); }
void be_camera_open(bool pad, const std::string& mv, const std::string& zm) { camera::open(S.agent, pad, mv, zm); }
void be_camera_close() { camera::close(); }
void be_camera_hide_hints() { camera::hide_hints(S.agent); }
void be_teardown_restore() {
    look::forget();
    g_engine_of.clear(); g_core_of.clear();
    if (screens::colour::track::acting()) screens::colour::track::teardown();
    op({ "TD" });                                                               // the relay forgets its groups
}
void be_journal(const std::string& line) { cmn::journal("%s", line.c_str()); }

// ---- the DI menu's data (phase 5c-1: from the DLL's settings, vfx::drive_cfg) -----------------------------------------
const char* const kDiLists[3] = { "presets", "presets_dr", "presets_sp" };
nlohmann::json* di_store_list(int list) {                         // the preset list a DPS / DPA / DPR order edited
    if (list < 0 || list >= 3) return nullptr;
    nlohmann::json& pc = vfx::drive_cfg::presets();
    auto it = pc.is_object() ? pc.find(kDiLists[list]) : pc.end();
    return (pc.is_object() && it != pc.end() && it->is_array()) ? &*it : nullptr;
}
// what edit.ned.open_di pushed: the five mechanics' settings (as the bridge converted the Lua values) and the three
// preset lists; a value the bridge refuses (a table) left that mechanic out, as the failed pcall did
void di_fill(int fighter, int side) {
    namespace dc = vfx::drive_cfg;
    using json = nlohmann::json;
    D = DiData{};
    D.available = true;
    D.special = false;
    D.fighter = fighter;
    D.side = (side == 2) ? 2 : 1;
    if (fighter != 0) D.name = vfx::settings::char_name(fighter);
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    auto tri = [](const json& v, core::Tri& out) -> bool {
        if (v.is_null()) { out = core::kNil; return true; }
        if (v.is_object() || v.is_array()) return false;
        if (v.is_boolean()) out = v.get<bool>() ? core::kTrue : core::kFalse;
        else if (v.is_number_integer() || v.is_number_unsigned()) out = v.get<int64_t>() != 0 ? core::kTrue : core::kFalse;
        else out = core::kFalse;
        return true;
    };
    auto col = [](const json& v, std::optional<uint32_t>& out) -> bool {
        if (v.is_null()) { out.reset(); return true; }
        if (v.is_object() || v.is_array()) return false;
        if (v.is_number_integer()) out = (uint32_t)v.get<int64_t>();
        else if (v.is_number_unsigned()) out = (uint32_t)v.get<uint64_t>();
        else if (v.is_number_float()) out = (uint32_t)(int64_t)v.get<double>();
        else out = 0u;                                                  // a_int of a text or a boolean: 0
        return true;
    };
    auto field = [](const json& t, const char* k) -> const json& {
        static const json nil;
        auto it = t.find(k);
        return it == t.end() ? nil : *it;
    };
    for (int m = 0; m < 5; ++m) {
        json* c = fighter > 0 ? dc::cfg_of((dc::Mech)m, fighter, D.side) : nullptr;
        if (!c) continue;
        core::DiCfg out;
        bool ok = tri(field(*c, "enabled"), out.enabled) && tri(field(*c, "follow"), out.follow) && tri(field(*c, "ll"), out.ll) &&
                  col(field(*c, "c1"), out.c1) && col(field(*c, "c2"), out.c2);
        const json& sel = field(*c, "selected");                        // tonumber(c.selected)
        if (sel.is_number()) out.selected = sel.get<double>();
        else if (sel.is_string()) { char* e = nullptr; const std::string& s = sel.get_ref<const std::string&>(); const double d = std::strtod(s.c_str(), &e); if (e != s.c_str() && !*e) out.selected = d; }
        const json& sl = field(*c, "sel");
        if (sl.is_object() || sl.is_array()) ok = false;
        else if (!sl.is_null()) out.sel = sl.is_string() ? sl.get<std::string>() : std::string();
        if (!ok) continue;
        D.has_cfg[m] = true;
        D.cfg[m] = out;
    }
    json& pc = dc::presets();
    for (int l = 0; l < 3; ++l) {
        auto it = pc.is_object() ? pc.find(kDiLists[l]) : pc.end();
        if (!pc.is_object() || it == pc.end() || !(it->is_object() || it->is_array())) continue;
        D.has_list[l] = true;
        if (!it->is_array()) continue;                                  // a table with no sequence: an empty list
        for (const json& p : *it) {
            if (p.is_null()) break;                                     // ipairs
            core::DiPreset pr;
            const json& nm = p.is_object() ? field(p, "name") : json();
            if (!nm.is_null()) {                                        // tostring(p.name or "")
                if (nm.is_string()) pr.name = nm.get<std::string>();
                else if (nm.is_boolean()) pr.name = nm.get<bool>() ? std::string("true") : std::string();
                else if (nm.is_number_float()) { char b[64]; std::snprintf(b, sizeof b, "%.14g", nm.get<double>()); std::string s = b; if (s.find_first_of(".eEni") == std::string::npos) s += ".0"; pr.name = s; }
                else pr.name = nm.dump();
            }
            auto floor_col = [&](const char* k, std::optional<uint32_t>& out) {   // string.format("%d", math.floor(p.c))
                const json& v = p.is_object() ? field(p, k) : json();
                if (!(v.is_null() || (v.is_boolean() && !v.get<bool>()))) {
                    if (v.is_number()) out = (uint32_t)(int64_t)std::floor(v.get<double>());
                    else if (v.is_string()) { char* e = nullptr; const double d = std::strtod(v.get_ref<const std::string&>().c_str(), &e); if (!*e) out = (uint32_t)(int64_t)std::floor(d); }
                }
            };
            floor_col("c1", pr.c1);
            floor_col("c2", pr.c2);
            D.list[l].push_back(pr);
        }
    }
}

// ---- the DI menu's data -------------------------------------------------------------------------------------------
bool be_di_available() { return D.available; }
std::string be_di_char_name(int) { return D.name; }
bool be_di_cfg(int mech, int, int, core::DiCfg& out) {
    if (mech < 0 || mech >= 5 || !D.has_cfg[mech]) return false;
    out = D.cfg[mech];
    return true;
}
void be_di_cfg_set(int mech, const char* field, int kind, bool b, double num, const std::string& s) {
    if (mech < 0 || mech >= 5) return;
    core::DiCfg& c = D.cfg[mech];
    const std::string f = field;
    const core::Tri t = kind == 0 ? core::kNil : (b ? core::kTrue : core::kFalse);
    if (f == "enabled") c.enabled = t; else if (f == "follow") c.follow = t; else if (f == "ll") c.ll = t;
    else if (f == "c1") { if (kind == 2) c.c1 = (uint32_t)num; else c.c1.reset(); }
    else if (f == "c2") { if (kind == 2) c.c2 = (uint32_t)num; else c.c2.reset(); }
    else if (f == "selected") { if (kind == 2) c.selected = num; else c.selected.reset(); }
    else if (f == "sel") { if (kind == 3) c.sel = s; else c.sel.reset(); }
    // the relay's DS: c[field] = nil | bool | tonumber(text of the integer) | text, on the table captured at open
    if (!D.has_cfg[mech] || D.fighter <= 0) return;
    std::lock_guard<std::recursive_mutex> lk(vfx::drive_cfg::lock());
    nlohmann::json* tbl = vfx::drive_cfg::cfg_of((vfx::drive_cfg::Mech)mech, D.fighter, D.side);
    if (!tbl) return;
    if (kind == 0) tbl->erase(f);
    else if (kind == 1) (*tbl)[f] = b;
    else if (kind == 2) (*tbl)[f] = (int64_t)num;
    else (*tbl)[f] = s;
}
bool be_di_presets(int list, std::vector<core::DiPreset>& out) {
    if (list < 0 || list >= 3 || !D.has_list[list]) return false;
    out = D.list[list];
    return true;
}
void be_di_preset_set(int list, int index, const std::optional<uint32_t>& c1, const std::optional<uint32_t>& c2) {
    if (list < 0 || list >= 3 || index < 1 || (size_t)index > D.list[list].size()) return;
    D.list[list][(size_t)index - 1].c1 = c1; D.list[list][(size_t)index - 1].c2 = c2;
    // the relay's DPS: p = l[index]; p.c1, p.c2 = tonumber(...) (nil erases)
    std::lock_guard<std::recursive_mutex> lk(vfx::drive_cfg::lock());
    nlohmann::json* l = di_store_list(list);
    if (!l || (size_t)index > l->size()) return;
    nlohmann::json& pr = (*l)[(size_t)index - 1];
    if (!pr.is_object()) return;
    if (c1) pr["c1"] = (int64_t)*c1; else pr.erase("c1");
    if (c2) pr["c2"] = (int64_t)*c2; else pr.erase("c2");
}
void be_di_preset_add(int list, const std::string& name, const std::optional<uint32_t>& c1, const std::optional<uint32_t>& c2) {
    if (list < 0 || list >= 3) return;
    D.list[list].push_back({ name, c1, c2 });
    // the relay's DPA: l[#l + 1] = { name = name, c1 = ..., c2 = ... }
    std::lock_guard<std::recursive_mutex> lk(vfx::drive_cfg::lock());
    nlohmann::json* l = di_store_list(list);
    if (!l) return;
    nlohmann::json pr = nlohmann::json{ { "name", name } };
    if (c1) pr["c1"] = (int64_t)*c1;
    if (c2) pr["c2"] = (int64_t)*c2;
    size_t len = 0;
    while (len < l->size() && !(*l)[len].is_null()) ++len;
    if (len < l->size()) (*l)[len] = std::move(pr); else l->push_back(std::move(pr));
}
void be_di_preset_remove(int list, int index) {
    if (list < 0 || list >= 3 || index < 1 || (size_t)index > D.list[list].size()) return;
    D.list[list].erase(D.list[list].begin() + (index - 1));
    // the relay's DPR: table.remove(l, index)
    std::lock_guard<std::recursive_mutex> lk(vfx::drive_cfg::lock());
    nlohmann::json* l = di_store_list(list);
    if (!l || (size_t)index > l->size()) return;
    l->erase(l->begin() + (index - 1));
}
void be_di_save_presets() { vfx::drive_cfg::save_presets(); }
void be_di_save_cfg(int f) { vfx::drive_cfg::save_di(f); }
void be_di_mark_dirty() { vfx::feed::mark_di_dirty(); }
void be_drv_save_cfg(int f) { vfx::drive_cfg::save_dr(f); }
bool be_di_special_enabled() { return D.special; }

// ---- the relay's entry points ---------------------------------------------------------------------------------------
thread_local std::string t_ret;
const char* a_str(const SF6CoreValue* a, uint32_t cnt, uint32_t i) { return (i < cnt && a[i].type == SF6CORE_VAL_STRING && a[i].s) ? a[i].s : ""; }
bool a_bool(const SF6CoreValue* a, uint32_t cnt, uint32_t i) {
    if (i >= cnt) return false;
    if (a[i].type == SF6CORE_VAL_BOOL) return a[i].b != 0;
    if (a[i].type == SF6CORE_VAL_INT) return a[i].i != 0;
    return false;
}
double a_num(const SF6CoreValue* a, uint32_t cnt, uint32_t i, double dflt) {
    if (i >= cnt) return dflt;
    if (a[i].type == SF6CORE_VAL_DOUBLE) return a[i].d;
    if (a[i].type == SF6CORE_VAL_INT) return (double)a[i].i;
    return dflt;
}
int64_t a_int(const SF6CoreValue* a, uint32_t cnt, uint32_t i, int64_t dflt) {
    if (i >= cnt) return dflt;
    if (a[i].type == SF6CORE_VAL_INT) return a[i].i;
    if (a[i].type == SF6CORE_VAL_DOUBLE) return (int64_t)a[i].d;
    return dflt;
}
bool a_nil(const SF6CoreValue* a, uint32_t cnt, uint32_t i) { return i >= cnt || a[i].type == SF6CORE_VAL_NIL; }
void r_int(SF6CoreValue& r, int64_t v) { r.type = SF6CORE_VAL_INT; r.i = v; }
void r_bool(SF6CoreValue& r, bool v) { r.type = SF6CORE_VAL_BOOL; r.b = v ? 1 : 0; }
void fault(const char* where) {
    ++g_faults;
    cmn::journal("[editor] fault in %s (%d)", where, g_faults);
    if (g_faults == kMaxFaults) cmn::journal("[editor] too many faults: off (the Lua editor takes over at the next open)");
}
// every entry point: the colour engine's lock held (the working copy is shared with the preview), nothing escapes
#define ED_BEGIN std::lock_guard<std::recursive_mutex> lk(engine::live::mx());
#define ED_CATCH(name) catch (...) { fault(name); return -1; }

int32_t SF6CORE_CALL lua_ed_status(void*, const SF6CoreValue*, uint32_t, SF6CoreValue* r, uint32_t max) try {
    if (max < 1) return 0;
    t_ret = core::status() + " faults=" + std::to_string(g_faults) + " ops=" + std::to_string(g_nops) + " rows=" + std::to_string(g_engine_of.size()) + "; " + look::status();
    r[0].type = SF6CORE_VAL_STRING; r[0].s = t_ret.c_str();
    return 1;
} catch (...) { return -1; }

}  // namespace

bool install() {
    core::Backend b;
    b.opt_group = be_opt_group; b.opt_remove_group = be_opt_remove_group; b.opt_button = be_opt_button; b.opt_slider = be_opt_slider; b.opt_choice = be_opt_choice;
    b.opt_group_def = be_opt_group_def; b.opt_rebuild = be_opt_rebuild; b.opt_ensure_built = be_opt_ensure_built; b.opt_open_menu = be_opt_open_menu;
    b.opt_navigate = be_opt_navigate; b.opt_close = be_opt_close; b.opt_window_open = be_opt_window_open; b.opt_focused = be_opt_focused;
    b.opt_row_control = be_opt_row_control; b.opt_current_param = be_opt_current_param; b.opt_parent_param = be_opt_parent_param;
    b.param_control_main = be_param_control_main; b.dialog_control_mains = be_dialog_control_mains;
    b.ui_child = be_ui_child; b.ui_next = be_ui_next; b.ui_name = be_ui_name; b.ui_message = be_ui_message; b.ui_visible = be_ui_visible;
    b.ui_actual_visible = be_ui_actual_visible; b.ui_force_invisible = be_ui_force_invisible; b.ui_position = be_ui_position; b.ui_size = be_ui_size;
    b.ui_scale = be_ui_scale; b.ui_font_size = be_ui_font_size; b.ui_find = be_ui_find; b.ui_set_text = be_ui_set_text; b.ui_set_position = be_ui_set_position;
    b.ui_set_size = be_ui_set_size; b.ui_set_scale = be_ui_set_scale; b.ui_set_visible = be_ui_set_visible; b.ui_set_force_invisible = be_ui_set_force_invisible;
    b.ui_set_font_size = be_ui_set_font_size;
    b.key_down = be_key_down; b.page_key = be_page_key; b.pad_mask = be_pad_mask; b.mouse = be_mouse; b.frame = be_frame; b.clock = be_clock; b.rand_int = be_rand_int; b.rand_float = be_rand_float;
    b.L = be_L;
    b.file_read = be_file_read; b.file_write = be_file_write; b.file_delete = be_file_delete;
    b.refresh = be_refresh; b.has_preview = be_has_preview; b.live_tree = be_live_tree; b.slot_union = be_slot_union; b.apply_original = be_apply_original;
    b.game_chips = be_game_chips; b.purge_empty_files = be_purge_empty_files;
    b.screen_visible = be_screen_visible; b.screen_pad = be_screen_pad; b.screen_busy = be_screen_busy; b.screen_refresh_list = be_screen_refresh_list;
    b.open_args = be_open_args; b.catalog_changed = be_catalog_changed; b.caches_invalidated = be_caches_invalidated;
    b.guide_bar = be_guide_bar; b.camera_open = be_camera_open; b.camera_close = be_camera_close; b.camera_hide_hints = be_camera_hide_hints;
    b.teardown_restore = be_teardown_restore; b.journal = be_journal;
    b.di_available = be_di_available; b.di_char_name = be_di_char_name; b.di_cfg = be_di_cfg; b.di_cfg_set = be_di_cfg_set; b.di_presets = be_di_presets;
    b.di_preset_set = be_di_preset_set; b.di_preset_add = be_di_preset_add; b.di_preset_remove = be_di_preset_remove; b.di_save_presets = be_di_save_presets;
    b.di_save_cfg = be_di_save_cfg; b.di_mark_dirty = be_di_mark_dirty; b.drv_save_cfg = be_drv_save_cfg; b.di_special_enabled = be_di_special_enabled;
    b.row_looks = be_row_looks; b.opt_set_value = be_opt_set_value; b.select_lr = be_select_lr;
    core::set_backend(b);
    core::set_data_dir("reframework/data");
    g_installed = true;
    cmn::journal("[editor] installed");
    return true;
}
bool ready() { return g_installed && g_faults < kMaxFaults; }

// ---- the DLL's own screens (screens/colour_track, phase 5c-2) ------------------------------------------------------------
State state() {
    State st;
    st.active = core::active(); st.di = core::di_active(); st.nops = g_nops;
    st.has_closed_at = core::closed_at(st.closed_at);
    st.reopen = core::reopen_now();
    return st;
}
void set_screen(bool visible, bool pad, bool busy, uint64_t agent) try {
    ED_BEGIN
    S.visible = visible; S.pad = pad; S.busy = busy; S.agent = agent;
} catch (...) { fault("set_screen"); }
void set_input(uint32_t pad_mask, bool has_mouse, double x, double y, double w, double h) try {
    ED_BEGIN
    S.pad_mask = pad_mask; S.has_mouse = has_mouse;
    if (has_mouse) { S.mx = x; S.my = y; S.dw = w; S.dh = h; }
} catch (...) { fault("set_input"); }
void set_pad_bits(uint32_t btn, uint32_t a, uint32_t y, uint32_t b, uint32_t l1, uint32_t r1, uint32_t r3, uint32_t l3) try {
    ED_BEGIN
    core::set_pad_bits(btn, a, y, b, l1, r1, r3, l3);
} catch (...) { fault("set_pad_bits"); }
void set_open_args(int fighter, int costume, int color_id, int k, bool has_preview, bool has_live, bool has_chips, uint32_t g0, uint32_t g1) try {
    ED_BEGIN
    S.fighter = fighter; S.costume = costume; S.color_id = color_id; S.k = k; S.has_preview = has_preview; S.has_live = has_live;
    S.has_chips = has_chips; if (has_chips) { S.g0 = g0; S.g1 = g1; }
} catch (...) { fault("set_open_args"); }
void set_union(int fighter, int costume, std::vector<std::string> keys) try {
    ED_BEGIN
    g_union[std::to_string(fighter) + "_" + std::to_string(costume)] = std::move(keys);
} catch (...) { fault("set_union"); }
State open() try {
    ED_BEGIN
    look::preload();
    core::open();
    return state();
} catch (...) { fault("open"); return {}; }
State open_di(int fighter, int costume, bool simple, int side, const std::string& mech, bool at_select) try {
    ED_BEGIN
    di_fill(fighter, side);
    look::preload();
    core::open_di(fighter, costume, simple, side, mech, at_select);
    return state();
} catch (...) { fault("open_di"); return {}; }
std::string take_ops() try {
    ED_BEGIN
    std::string out;
    out.swap(g_ops); g_nops = 0;
    return out;
} catch (...) { fault("take_ops"); return {}; }
void map_entry(int id, int64_t eid) try {
    ED_BEGIN
    if (id >= 0 && eid >= 0) { g_engine_of[id] = eid; g_core_of[eid] = id; }
} catch (...) { fault("map_entry"); }
ButtonOut on_button(int id) try {
    ED_BEGIN
    const core::ButtonResult res = core::on_button(id);
    ButtonOut o;
    o.kind = res.kind == core::ButtonResult::Close ? 1 : res.kind == core::ButtonResult::Open ? 2 : 0;
    o.key = res.open; o.rebuild = (int)res.rebuild; o.push = (int)res.push; o.pop = (int)res.pop;
    return o;
} catch (...) { fault("on_button"); return {}; }
void on_value(int id, int value) try {
    ED_BEGIN
    core::on_value(id, value);
} catch (...) { fault("on_value"); }
int on_created(uint64_t param) try {
    ED_BEGIN
    core::on_dialog_created(reinterpret_cast<core::Obj>((uintptr_t)param));
    return g_nops;
} catch (...) { fault("on_created"); return 0; }
int on_page_setup(uint64_t param, int rows) try {
    ED_BEGIN
    core::on_page_setup(reinterpret_cast<core::Obj>((uintptr_t)param), rows);
    return g_nops;
} catch (...) { fault("on_page_setup"); return 0; }
int on_popped(uint64_t param) try {
    ED_BEGIN
    core::on_page_popped(reinterpret_cast<core::Obj>((uintptr_t)param));
    return g_nops;
} catch (...) { fault("on_popped"); return 0; }
State frame(int64_t f) try {
    ED_BEGIN
    S.frame = f;
    core::frame();
    return state();
} catch (...) { fault("frame"); return {}; }

const std::vector<LuaFn>& bridge_fns() {
    static const std::vector<LuaFn> fns = {
        { "ed_status", lua_ed_status },
    };
    return fns;
}

}  // namespace dialogs::editor
