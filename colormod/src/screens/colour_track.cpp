// screens::colour::track — see colour_track.hpp. Line for line from SF6_ColorSpinExtra.lua (the functions named in
// the comments); game reads through REFramework, writes through the core's display API, as dbg.UI did.

#include "colour_track.hpp"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <optional>
#include <vector>

#include "ll_common.hpp"

#include "../common.hpp"
#include "../perf.hpp"
#include "../perf_hook.hpp"
#include "../dialogs/editor.hpp"
#include "../dialogs/editor_core.hpp"
#include "../dialogs/locale.hpp"
#include "../dialogs/options_driver.hpp"
#include "../fighters.hpp"
#include "../engine/catalog.hpp"
#include "../engine/chips.hpp"
#include "../engine/decide.hpp"
#include "../engine/decide_internal.hpp"
#include "../engine/live_internal.hpp"
#include "../engine/luajson.hpp"
#include "../vfx/feed.hpp"
#include "colour_state.hpp"
#include "dipanel_mode.hpp"
#include "guide.hpp"
#include "rows.hpp"
#include "tracker.hpp"

namespace screens::colour::track {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;
using json = nlohmann::json;
namespace dd = engine::decide::internal;
namespace lj = engine::luajson;
namespace chips = engine::chips;
namespace rsz = engine::rsz;
using engine::Entry;
using Opt = std::optional<int64_t>;

constexpr const char* kAgentP1 = "ui10505";                      // AGENT_NAME
constexpr const char* kAgentP2 = "ui10507";                      // P2_AGENT_NAME
constexpr const char* kAgentFs = "MatchingFighterSetting";       // FS_AGENT_NAME
constexpr const char* kAgentTcs = "GenericCharacterDetailSetting_FG";   // TCS_AGENT_NAME
constexpr uint32_t kBaseColor = 1;                               // BASE_COLOR
constexpr const char* kStateFile = "reframework/data/SF6_ColorSpinExtra_data/state.json";
constexpr const char* kChipFile = "reframework/data/SF6_ColorSpinExtra_data/chip_slots.json";
constexpr const char* kDataPrefix = "reframework/data/";
constexpr const char* kOverridesPrefix = "reframework/data/SF6_ColorSpinExtra_data/overrides/";
constexpr double kCornerY = -980, kCornerX = 0, kCornerXRight = 1495;   // dbg.corner_y / corner_x / corner_x_right
constexpr int kPadKeys[] = { 0x25, 0x26, 0x27, 0x28, 0x1B, 0x08, 0x20, 0x41, 0x45, 0x43, 0x46, 0x5A, 0x51, 0x53, 0x44, 0x56, 0x58, 0x57 };   // W: up on a QWERTY keyboard (Z on AZERTY)

// ---- held references (a Lua variable keeps its object alive) ------------------------------------------------------
// One reference per holder: copies take their own, destruction gives it back. The holders below live on the heap
// and are never destroyed (no release into the engine while the process exits).
class Ref {
public:
    Ref() = default;
    Ref(const Ref& o) { set(o.p_); }
    Ref& operator=(const Ref& o) { set(o.p_); return *this; }
    Ref(Ref&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Ref& operator=(Ref&& o) noexcept { if (this != &o) { set(nullptr); p_ = o.p_; o.p_ = nullptr; } return *this; }
    ~Ref() { set(nullptr); }
    MO* get() const { return p_; }
    void set(MO* o) {
        if (o == p_) return;
        if (o) o->add_ref();
        if (p_) p_->release();
        p_ = o;
    }
    explicit operator bool() const { return p_ != nullptr; }
private:
    MO* p_ = nullptr;
};

// ---- game reads -----------------------------------------------------------------------------------------------------
bool alive(const void* o) { return o && API::get()->sdk()->managed_object->is_managed_object(const_cast<void*>(o)); }
uint64_t addr(const Ref& r) { return alive(r.get()) ? (uint64_t)(uintptr_t)r.get() : 0; }   // the adapters' addr(o)

std::map<std::pair<API::TypeDefinition*, std::string>, API::Method*> g_methods;
API::Method* method_of(MO* o, const char* sig) {
    auto* td = o->get_type_definition();
    const auto key = std::make_pair(td, std::string(sig));
    if (auto it = g_methods.find(key); it != g_methods.end()) return it->second;
    API::Method* m = std::strchr(sig, '(') ? ll::find_method_full(td, sig) : ll::find_method_noargs(td, sig);
    g_methods[key] = m;
    return m;
}
std::optional<reframework::InvokeRet> call(MO* o, const char* sig, const std::vector<void*>& args = {}) {   // nullopt = raised
    if (!o) return std::nullopt;
    API::Method* m = method_of(o, sig);
    if (!m) return std::nullopt;
    auto r = m->invoke(o, args);
    if (r.exception_thrown) return std::nullopt;
    return r;
}
MO* call_obj(MO* o, const char* sig) { auto r = call(o, sig); return r ? reinterpret_cast<MO*>(r->ptr) : nullptr; }
std::optional<bool> call_bool(MO* o, const char* sig) { auto r = call(o, sig); if (!r) return std::nullopt; return r->byte != 0; }
std::optional<int32_t> call_i32(MO* o, const char* sig) { auto r = call(o, sig); if (!r) return std::nullopt; return (int32_t)r->dword; }
std::optional<std::string> message(MO* text) {                    // pcall(text.call, text, "get_Message") -> tostring
    if (!alive(text)) return std::nullopt;
    auto r = call(text, "get_Message");
    if (!r) return std::nullopt;
    MO* s = reinterpret_cast<MO*>(r->ptr);
    return s ? ll::managed_string_to_utf8(s) : std::string("nil");
}
std::string name_of(MO* c) { MO* s = call_obj(c, "get_Name"); return s ? ll::managed_string_to_utf8(s) : std::string("nil"); }
std::optional<uint32_t> rect_rgba(MO* rect) {                     // rect:call("get_Color").rgba
    auto r = call(rect, "get_Color");
    if (!r) return std::nullopt;
    return r->dword;
}
std::optional<int32_t> field_i32(MO* o, const char* f) { int32_t v = 0; if (!o || !ll::read_field_i32(o, f, v)) return std::nullopt; return v; }
std::optional<uint32_t> color_num(MO* c) { uint32_t v = 0; if (!c || !ll::read_field_u32(c, "ColorNum", v)) return std::nullopt; return v; }
MO* field_obj(MO* o, const char* f) { return o ? ll::read_field_object(o, f) : nullptr; }

const SF6CoreAPI* api() { return cmn::core(); }
MO* find(MO* root, const char* name) {                            // find_name(root, name, 0)
    return root ? reinterpret_cast<MO*>(api()->ui_find_child(root, name, 8)) : nullptr;
}
std::vector<MO*> agents(const char* name) {                       // dbg.find.agents(name): _Entries order
    void* buf[16] = {};
    const uint32_t n = api()->ui_agents(name, buf, 16);
    std::vector<MO*> out;
    for (uint32_t i = 0; i < n && i < 16; ++i) out.push_back(reinterpret_cast<MO*>(buf[i]));
    return out;
}
MO* agent_param(MO* agent) {                                      // agent_param
    MO* deleg = field_obj(agent, "<InputDelegates>k__BackingField");
    MO* flow = field_obj(deleg, "<Flow>k__BackingField");
    return field_obj(flow, "_Param");
}
MO* list_item(MO* list, int32_t i) {
    auto r = call(list, "get_Item(System.Int32)", { ll::arg_i32(i) });
    return r ? reinterpret_cast<MO*>(r->ptr) : nullptr;
}
MO* find_parts_spin(MO* item, const char* name, int depth) {      // find_parts_spin
    if (!item || depth > 6) return nullptr;
    MO* nm = field_obj(item, "<Name>k__BackingField");
    if (nm && ll::managed_string_to_utf8(nm) == name) {
        auto* td = item->get_type_definition();
        if (td && std::string(td->get_full_name()) == "app.UIPartsSpin") return item;
    }
    MO* children = field_obj(item, "_Children");
    if (children) {
        const int32_t n = call_i32(children, "get_Count").value_or(0);
        for (int32_t k = 0; k < n; ++k) if (MO* f = find_parts_spin(list_item(children, k), name, depth + 1)) return f;
    }
    return nullptr;
}
std::optional<std::string> focused_row_name(MO* agent) {          // focused_row_name
    MO* item = call_obj(agent, "GetFocusItem");
    MO* ctrl = field_obj(item, "<Control>k__BackingField");
    if (!ctrl) return std::nullopt;
    MO* s = call_obj(ctrl, "get_Name");
    if (!s) return std::nullopt;
    return ll::managed_string_to_utf8(s);
}
bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }   // reframework:is_key_down

// ---- Lua values ---------------------------------------------------------------------------------------------------
std::string tostr(const Opt& v) { return v ? std::to_string(*v) : std::string("nil"); }
std::string tostr(const json& v) {                                 // tostring of a state.json value
    if (v.is_null()) return "nil";
    if (v.is_number_integer() || v.is_number_unsigned()) return std::to_string(v.get<int64_t>());
    if (v.is_number_float()) { char b[64]; std::snprintf(b, sizeof b, "%.14g", v.get<double>()); std::string s = b;
        if (s.find_first_of(".eEni") == std::string::npos) s += ".0"; return s; }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_string()) return v.get<std::string>();
    return "table";
}
Opt num(const json& v) {                                           // a number of the state (nil otherwise)
    if (v.is_number_integer() || v.is_number_unsigned()) return v.get<int64_t>();
    if (v.is_number_float()) { const double d = v.get<double>(); if (d == std::floor(d)) return (int64_t)d; }
    return std::nullopt;
}
bool eq(const json& v, const Opt& n) {                             // Lua v == n (n an integer or nil)
    if (!n) return v.is_null();
    if (v.is_number()) return v.get<double>() == (double)*n;
    return false;
}
json jv(const Opt& n) { return n ? json(*n) : json(); }
int64_t rw_or0(const json& sel) { const json& v = lj::index(sel, "rw"); return v.is_number() ? (int64_t)v.get<double>() : 0; }   // (sel.rw or 0)

// owner checks (edit.native_bridge(feature) ~= nil)
bool nb(const char* f) { return cmn::bridge_owns(f); }
bool ed_active();                                      // edit.active (the relay's mirror while the DLL acts)

// ---- the screens' state (Lua locals of the same names) ----------------------------------------------------------------
json g_state;                                          // state (state.json)
bool g_state_ok = false;
json g_chip_cfg = json::object();                      // chip_cfg (truthiness of a key; learn writes it)
chips::Config g_cfg;                                   // the same file, for chip_for
std::map<std::string, chips::Pair> g_entry_chip;       // entry.chip ("<fighter>_<costume>|<path>")

Opt g_list_f, g_list_c;                                // list_args
Opt g_extra_first;                                     // extra_first (nil until a GetColorList)
int64_t g_extra_count = 0;
std::map<std::pair<int, int>, std::pair<int, int>> g_extras;   // per fighter / costume (engine::decide::Pull::extras)
uint64_t g_setups[4] = {};                             // dbg.setups[1..3]
std::optional<uint64_t> g_clears;                      // dbg.decide_clears

struct Menu {
    int side = 1; const char* key = "p1";
    Ref agent, param, text, rect0, rect1;
    int64_t selected = 0; bool active = false; uint64_t opened_at = 0; bool restore_pending = false;
};
std::array<Menu, 2>* make_menus() {
    auto* a = new std::array<Menu, 2>();
    (*a)[0].side = 1; (*a)[0].key = "p1"; (*a)[1].side = 2; (*a)[1].key = "p2";
    return a;
}
std::array<Menu, 2>& g_menus = *make_menus();                // menus[1..2] (never destroyed)
json& sel_of(int side) { return g_state[side == 2 ? "p2" : "p1"]; }

struct ToApply { bool set = false; int side = 0; Opt fighter; uint64_t frame = 0; };
ToApply g_apply_menu[3];                               // dbg.to_apply_menu[side]
ToApply g_apply_popup;                                 // dbg.to_apply
std::string g_side_hint;                               // dbg.side_hint

struct Guide { Ref part, text, ctrl, corner, btn; bool right = false; };
struct Refresh { std::optional<std::string> path; int step = 0; uint64_t at = 0; Ref spin; };
struct Fsm {                                           // fsm
    Ref agent, spin, text, rect0, rect1;
    int64_t last_k = 0; uint64_t opened_at = 0; int64_t restore_steps = 0; bool visible = false;
    bool color_focused = false, hidden_now = false, tcs_guide = false, cs_guide = false, guide_plain = false;
    std::optional<bool> pad;
    std::optional<std::string> guide_label;
    std::vector<Guide> guides;
    std::optional<Refresh> refresh;
    bool mouse_known = false; LONG mouse_x = 0, mouse_y = 0;
};
Fsm& fsm = *new Fsm();

struct Tcs {                                           // tcs
    Ref agent, param, text, rect0, rect1, spin;
    bool decided = false, visible = false; uint64_t opened_at = 0; int64_t restore_steps = 0; int target = 0;
    struct Base { int64_t f, c, n; };
    std::optional<Base> base;
    struct LiveRefresh { int side = 1; uint64_t at = 0; uint64_t setups = 0; uintptr_t addr = 0; std::optional<bool> reload; };
    std::optional<LiveRefresh> live_refresh;
};
Tcs& tcs = *new Tcs();

uint64_t g_F = 0;                                      // cs.frame of the pass running
struct { uint64_t frames = 0; } g_n;

// ---- catalogue, chips, state -------------------------------------------------------------------------------------------
const std::vector<Entry>* entries_for(const Opt& f, const Opt& c) {
    if (!f || !c) return nullptr;
    return dd::catalog().entries((int)*f, (int)*c);
}
const Entry* entry_k(const std::vector<Entry>* list, int64_t k) { return (list && k >= 1 && k <= (int64_t)list->size()) ? &(*list)[k - 1] : nullptr; }
int64_t label_to_k(const Opt& f, const Opt& c, const std::string& label) {
    if (!f || !c) return 0;
    return dd::catalog().label_to_k((int)*f, (int)*c, label);
}
// A select menu reads the list of its own fighter / costume: the other side's list (asked last) can hold more game
// colours, its first entry index then fell on a game colour of this side (P2's EX colour shown as RW 1).
std::optional<std::pair<int64_t, int64_t>> extras_of(int64_t f, int64_t c) {   // (extra_first, extra_count)
    const auto it = g_extras.find({ (int)f, (int)c });
    if (it == g_extras.end()) return std::nullopt;
    return std::make_pair((int64_t)it->second.first, (int64_t)it->second.second);
}
Opt entry_at(int64_t idx, int64_t f, int64_t c) {                  // list index -> entry number or nil
    const auto x = extras_of(f, c);
    if (!x || idx < x->first || idx >= x->first + x->second) return std::nullopt;
    return idx - x->first + 1;
}
Opt index_of_entry(int64_t k, int64_t f, int64_t c) {
    const auto x = extras_of(f, c);
    if (!x || k < 1 || k > x->second) return std::nullopt;
    return x->first + k - 1;
}
std::string fc_key(const Opt& f, const Opt& c) { return tostr(f) + "_" + tostr(c); }
bool chip_known(const std::string& key) { return lj::truthy(lj::index(g_chip_cfg, key.c_str())); }   // chip_cfg[key]

std::optional<chips::Pair> entry_chip(const Opt& f, const Opt& c, const Entry& e) {   // entry.chip (computed once)
    const std::string key = fc_key(f, c) + "|" + e.path;
    if (auto it = g_entry_chip.find(key); it != g_entry_chip.end()) return it->second;
    rsz::NodeRef tree;
    if (!e.okey.empty()) tree = engine::live::file_tree(kOverridesPrefix + e.okey + ".user.2");
    if (!tree) tree = engine::live::file_tree(kDataPrefix + e.path);
    if (!tree) return std::nullopt;
    g_cfg.load(kChipFile);
    const auto p = chips::chip_for(tree.get(), (int)f.value_or(-1), (int)c.value_or(-1), &e.path, std::nullopt, g_cfg,
                                   [](int ff, int cc, uint32_t n) { return dd::game_chips(ff, cc, n); });
    g_entry_chip[key] = p;
    return p;
}

void save_state() {
    colour::save_state(kStateFile, g_state);
}

void set_color(MO* ctrl, std::optional<uint32_t> n, const std::string& why, bool fresh) {   // dbg.set_color
    dd::set_color(ctrl, n, why, fresh);
}

void learn_chip_slots(MO* ctrl, const Opt& f, const Opt& c, std::optional<uint32_t> c0, std::optional<uint32_t> c1) {
    const std::string key = fc_key(f, c);
    if (chip_known(key) || !c0 || !c1) return;
    const auto n = color_num(ctrl);
    MO* data = n ? engine::live::color_data(ctrl, (int32_t)*n) : nullptr;
    rsz::NodeRef t = data ? engine::live::live_tree(data, 0) : nullptr;
    if (!t) return;
    const auto a = chips::nearest_slot(t.get(), *c0);
    const auto b = chips::nearest_slot(t.get(), *c1);
    if (a && b && a->second <= 48 && b->second <= 48) {
        colour::learn_chip_slots(g_chip_cfg, key, a->first, b->first, kChipFile);
        for (auto it = g_entry_chip.begin(); it != g_entry_chip.end();)
            it = it->first.compare(0, key.size() + 1, key + "|") == 0 ? g_entry_chip.erase(it) : std::next(it);
        cmn::journal("chip slots learned for %s from colour 01: %s (d=%d) + %s (d=%d)", key.c_str(), a->first.c_str(), a->second, b->first.c_str(), b->second);
    }
}

void spin_call(MO* spin, const char* dir, const char* /*who*/) {
    call(spin, dir);
}
void paint(int side, const Ref& text, const Ref& r0, const Ref& r1, const std::string& label, int64_t c0, int64_t c1,
           const Ref& param = Ref(), int64_t idx = -1) {
    const uint64_t a = text ? (uint64_t)(uintptr_t)text.get() : 0, b = r0 ? (uint64_t)(uintptr_t)r0.get() : 0, c = r1 ? (uint64_t)(uintptr_t)r1.get() : 0;
    screens::tracker::push_menu(side, a, b, c, label, c0, c1, param ? (uint64_t)(uintptr_t)param.get() : 0, idx);
}
void paint_rect(MO* rect, uint32_t rgba) {                         // paint_rect: compare, then the core
    if (rect_rgba(rect) == rgba) return;
    api()->ui_set_color(cmn::client(), rect, rgba);
}

// ---- guide (find_guide_list / guide_text_fix / set_bottom_hint) --------------------------------------------------------
std::string L(const char* key) { return dialogs::locale::get(key, {}); }
bool di_active(int side) { return nb("screens_dipanel") && screens::dipanel::mode::active(side); }   // dbg.di_panel.active

void find_guide_list() {
    fsm.guides.clear();
    for (MO* ag : agents("InputGuide")) {
        MO* cm = call_obj(ag, "get_ControlMain");
        const auto vis = call_bool(cm, "get_ActualVisible");
        if (!(vis && *vis)) continue;
        MO* lst = field_obj(field_obj(ag, "_PartsManager"), "_List");
        const int32_t n = lst ? call_i32(lst, "get_Count").value_or(0) : 0;
        for (int32_t k = 0; k < n; ++k) {
            MO* part = list_item(lst, k);
            auto* td = part ? part->get_type_definition() : nullptr;
            if (!(td && std::string(td->get_full_name()) == "app.UIPartsInputGuideList")) continue;
            MO* ctrl = field_obj(part, "<Control>k__BackingField");
            MO* btn = ctrl ? find(ctrl, "c_guideBtn") : nullptr;
            MO* c = btn ? call_obj(btn, "get_Child") : nullptr;
            MO* first_d = nullptr; MO* text = nullptr; double max_x = 0;
            while (c) {
                const std::string nm = name_of(c);
                if (nm.rfind("e_text_0_g_", 0) == 0) { text = c; break; }
                if (nm.rfind("e_text_0_d_", 0) == 0) {
                    if (!first_d) first_d = c;
                    if (auto p = call(c, "get_Position")) { float x; std::memcpy(&x, p->bytes.data(), 4); if (x > max_x) max_x = x; }
                }
                c = call_obj(c, "get_Next");
            }
            if (!text) text = first_d;
            if (text) {
                MO* corner = nullptr;
                for (MO* cc = call_obj(btn, "get_Child"); cc; cc = call_obj(cc, "get_Next"))
                    if (name_of(cc) == "e_text_0") corner = cc;
                Guide g;
                g.part.set(part); g.text.set(text); g.ctrl.set(ctrl); g.corner.set(corner); g.btn.set(btn); g.right = max_x > 700;
                fsm.guides.push_back(std::move(g));
            }
            break;
        }
    }
}

void guide_text_fix() {
    if (!nb("screens_guide")) return;
    screens::guide::clear_fixes();
    for (const Guide& g : fsm.guides) {
        if (g.text) {
            auto mr = alive(g.text.get()) ? call(g.text.get(), "get_Message") : std::nullopt;
            if (mr && mr->ptr) {                                    // ok and msg ~= nil
                std::string msg = ll::managed_string_to_utf8(reinterpret_cast<MO*>(mr->ptr));
                if (auto sep = msg.find("      <ICON"); sep != std::string::npos) msg = msg.substr(0, sep);
                const bool pad = (fsm.pad && *fsm.pad) || msg.find("<ICON Btn") != std::string::npos;   // fsm.pad or ...
                const std::string prefix = fsm.guide_plain ? "      " : (pad ? "      <ICON BtnD> " : "      <ICON KeyF><ICON MouseL> ");
                std::string want = fsm.guide_label ? msg + prefix + *fsm.guide_label : msg;
                const bool on_select = g_menus[g.right ? 1 : 0].active;
                const bool in_di_any = di_active(1) || di_active(2);
                if (!fsm.hidden_now && !on_select && !g.right && !in_di_any && !fsm.visible)
                    want += (pad ? "      <ICON BtnL> " : "      <ICON KeyX> ") + L("hint.edit_di_color");
                const uint64_t pa = (uint64_t)(uintptr_t)g.part.get(), ta = (uint64_t)(uintptr_t)g.text.get(), ca = (uint64_t)(uintptr_t)g.ctrl.get();
                screens::guide::set_fix(pa, ta, ca, g.right, want);
            }
        }
        if (g.corner) {
            const uint64_t a = (uint64_t)(uintptr_t)g.corner.get(), b = (uint64_t)(uintptr_t)g.btn.get();
            const bool mine = g_menus[g.right ? 1 : 0].active;
            const bool in_di = di_active(g.right ? 2 : 1);
            const bool show = !fsm.hidden_now && !in_di && mine;
            const bool pad2 = fsm.pad.value_or(false);
            const std::string text = show ? std::string(pad2 ? "<ICON BtnL> " : "<ICON KeyX> ") + L("hint.edit_di_color") : std::string();
            const double y = show ? kCornerY : 0.0, x = show ? (g.right ? kCornerXRight : kCornerX) : 0.0;
            screens::guide::set_corner(a, b, text, (float)x, (float)y, show);
            if (!show) screens::guide::release_corner(a);
        }
    }
}
void set_bottom_hint(std::optional<std::string> label) { fsm.guide_label = std::move(label); guide_text_fix(); }

// ---- select menus (find_agents / resolve_menu / resolve / tick_menu / on_selection_changed) -----------------------------
bool resolve_menu(Menu& m, MO* agent) {
    if (!agent) { m.agent.set(nullptr); m.param.set(nullptr); m.text.set(nullptr); return false; }
    if (agent == m.agent.get() && m.text) return true;
    m.active = false;                                              // new screen instance: activation edge below
    MO* param = agent_param(agent);
    MO* spins = field_obj(param, "Spins");
    MO* spin = (spins && call_i32(spins, "get_Count").value_or(0) > 1) ? list_item(spins, 1) : nullptr;
    MO* ctrl = field_obj(spin, "<Control>k__BackingField");
    MO* text = ctrl ? find(ctrl, "e_text") : nullptr;
    if (!text) return false;
    m.agent.set(agent); m.param.set(param); m.text.set(text);
    m.rect0.set(find(ctrl, "e_rect_color_0"));
    m.rect1.set(find(ctrl, "e_rect_color_1"));
    return true;
}
void resolve() {
    MO* a[2] = {};
    const char* names[2] = { kAgentP1, kAgentP2 };
    for (int i = 0; i < 2; ++i) for (MO* ag : agents(names[i])) if (agent_param(ag)) a[i] = ag;   // the last match
    resolve_menu(g_menus[0], a[0]);
    resolve_menu(g_menus[1], a[1]);
}

void on_selection_changed(Menu& m, int64_t k, int64_t fighter, int64_t costume) {
    cmn::journal("P%d selection -> %lld (fighter %lld costume %lld)", m.side, (long long)k, (long long)fighter, (long long)costume);
    json& sel = sel_of(m.side);
    lj::set(sel, "rw", k); lj::set(sel, "fighter", fighter); lj::set(sel, "costume", costume);
    const Entry* e = k > 0 ? entry_k(entries_for(fighter, costume), k) : nullptr;
    lj::set(sel, "path", e ? json(e->ident) : json());
    save_state();
    MO* ctrl = dd::find_visual_ctrl(m.side);
    if (!ctrl) return;
    if (color_num(ctrl) == kBaseColor) set_color(ctrl, kBaseColor, "selection P" + std::to_string(m.side), true);
}

void tick_menu(Menu& m) {
    if (di_active(m.side)) {                                       // the panel shows the DI rows: the Color row is the
        if (nb("screens_tracker")) screens::tracker::leave_menu(m.side);   // Drive Reversal row, no "MC 1" painted on it
        return;
    }
    if (!m.text) { m.active = false; return; }
    if (!alive(m.param.get()) || !alive(m.text.get())) { m.agent.set(nullptr); m.text.set(nullptr); m.active = false; return; }
    json& sel = sel_of(m.side);
    const auto act = call_bool(m.param.get(), "IsActive");
    const bool active = act && *act;
    if (active && !m.active) { m.opened_at = g_F; m.restore_pending = rw_or0(sel) > 0; }
    m.active = active;
    if (!active) {
        g_apply_menu[m.side].set = false;
        if (nb("screens_tracker")) screens::tracker::leave_menu(m.side);
        return;
    }
    const int64_t idx = call_i32(m.param.get(), "get__cacheColorIndex").value_or(-1);
    const auto fo = call_i32(m.param.get(), "get_FighterId");
    const auto co = call_i32(m.param.get(), "GetCostumeId");
    if (!fo || !co) return;                                        // the Lua raised there
    const int64_t fighter = *fo, costume = *co;
    if (!eq(lj::index(sel, "fighter"), fighter) || !eq(lj::index(sel, "costume"), costume)) {
        if (rw_or0(sel) == 0) { lj::set(sel, "fighter", fighter); lj::set(sel, "costume", costume); save_state(); }
    }
    if (m.restore_pending) {                                       // menu (re)activated with an entry remembered
        if (const json& p = lj::index(sel, "path"); lj::truthy(p) && p.is_string())
            if (const auto* list = entries_for(fighter, costume))
                for (size_t i = 0; i < list->size(); ++i) if ((*list)[i].ident == p.get<std::string>()) { lj::set(sel, "rw", (int64_t)i + 1); break; }
        const Opt target = index_of_entry(rw_or0(sel), fighter, costume);
        if (!eq(lj::index(sel, "fighter"), fighter) || !eq(lj::index(sel, "costume"), costume) || !target || g_F - m.opened_at > 300) {
            m.restore_pending = false; m.selected = entry_at(idx, fighter, costume).value_or(0);
        } else if (g_F - m.opened_at > 30) {
            m.restore_pending = false; m.selected = rw_or0(sel);
            call(m.param.get(), "setColor(System.Int32)", { ll::arg_i32((int32_t)*target) });   // moves the cursor
            cmn::journal("P%d cursor restored to %lld (RW entry %lld)", m.side, (long long)*target, (long long)m.selected);
            on_selection_changed(m, m.selected, fighter, costume);
            return;
        } else {
            return;
        }
    }
    const int64_t k = entry_at(idx, fighter, costume).value_or(0);
    if (k != m.selected || (k > 0 && (!eq(lj::index(sel, "fighter"), fighter) || !eq(lj::index(sel, "costume"), costume)))) {
        m.selected = k;
        on_selection_changed(m, k, fighter, costume);              // k = 0: the user left the entries
    }
    if (g_F % 9 == 0) g_apply_menu[m.side] = { true, m.side, fighter, g_F };   // "colour to apply" (the fighter is what is read)
    if (k == 0 && m.rect0 && m.rect1 && !chip_known(fc_key(fighter, costume)) && g_F % 30 == 0) {
        if (MO* ctrl = dd::find_visual_ctrl(m.side)) learn_chip_slots(ctrl, fighter, costume, rect_rgba(m.rect0.get()), rect_rgba(m.rect1.get()));
    }
    if (k > 0) {
        const Entry* entry = entry_k(entries_for(fighter, costume), k);
        const std::string label = entry ? entry->label : "RW " + std::to_string(k);
        std::optional<chips::Pair> chip;
        if (entry && m.rect0 && m.rect1) chip = entry_chip(fighter, costume, *entry);
        else if (entry) if (auto it = g_entry_chip.find(fc_key(fighter, costume) + "|" + entry->path); it != g_entry_chip.end()) chip = it->second;
        if (nb("screens_tracker")) paint(m.side, m.text, m.rect0, m.rect1, label, chip ? (int64_t)chip->first : -1, chip ? (int64_t)chip->second : -1, m.param, idx);
    } else if (nb("screens_tracker")) {
        paint(m.side, m.text, Ref(), Ref(), "", -1, -1);          // no entry: the game owns the text
    }
}

// ---- Fighter Settings (fs_entry / tick_fighter_settings) -------------------------------------------------------------
int64_t fs_entry() {
    if (!fsm.text || !alive(fsm.text.get())) {                     // spin closed: the remembered choice for this fighter
        const json& s = g_state["fs"];
        if (rw_or0(s) > 0 && eq(lj::index(s, "fighter"), g_list_f) && eq(lj::index(s, "costume"), g_list_c)) return rw_or0(s);
        return 0;
    }
    const auto msg = message(fsm.text.get());
    return msg ? label_to_k(g_list_f, g_list_c, *msg) : 0;
}

// The Fighter Settings entry shown changed (k = what the spin shows now): every RW / MC entry rides on BASE_COLOR, so
// the game skips SetColor on the preview; re-issue it on every Fighter Settings preview controller wearing that id.
void fs_shown(int64_t k, const char* why) {
    fsm.last_k = k;
    for (MO* c : dd::scene_ctrls())
        if (engine::live::side_from_parents(c) == 3 && color_num(c) == kBaseColor) set_color(c, kBaseColor, why, true);
}

void tick_fighter_settings() {
    const auto ags = agents(kAgentFs);
    MO* agent = ags.empty() ? nullptr : ags[0];
    if (!agent) { fsm.agent.set(nullptr); fsm.text.set(nullptr); fsm.spin.set(nullptr); fsm.last_k = 0; fsm.visible = false; return; }
    MO* cm = call_obj(agent, "get_ControlMain");
    const auto v = call_bool(cm, "get_ActualVisible");
    if (!(v && *v)) {
        if (fsm.visible) { fsm.color_focused = false; fsm.hidden_now = true; set_bottom_hint(std::nullopt); fsm.hidden_now = false; }
        fsm.visible = false; fsm.text.set(nullptr); fsm.last_k = 0; return;
    }
    if (agent != fsm.agent.get() || !fsm.text || !fsm.visible) {
        fsm.visible = true;
        fsm.guide_label.reset(); fsm.color_focused = false;
        find_guide_list();
        if (nb("screens_tracker")) screens::tracker::push_fs_setup((uint64_t)(uintptr_t)cm);
        MO* spin = find(cm, "p_Spin_color");
        fsm.agent.set(agent);
        fsm.text.set(spin ? find(spin, "e_text") : nullptr);
        fsm.rect0.set(spin ? find(spin, "e_rect_color_0") : nullptr);
        fsm.rect1.set(spin ? find(spin, "e_rect_color_1") : nullptr);
        fsm.spin.set(find_parts_spin(field_obj(agent, "_RootItem"), "color", 0));
        fsm.opened_at = g_F;
        const json& s = g_state["fs"];
        fsm.restore_steps = (rw_or0(s) > 0 && eq(lj::index(s, "fighter"), g_list_f) && eq(lj::index(s, "costume"), g_list_c)) ? g_extra_count + 30 : 0;
    }
    if (!fsm.text || !fsm.rect0 || !fsm.rect1) return;
    if (!alive(fsm.text.get())) { fsm.agent.set(nullptr); fsm.text.set(nullptr); return; }
    const auto row = focused_row_name(agent);
    fsm.color_focused = row && *row == "p_Spin_color";
    if (g_F % 5 == 0) {
        find_guide_list();
        fsm.guide_plain = false;                                    // (fsm.status: no chained message any more)
        const bool on_drive = nb("screens_rows") && !screens::rows::focused_mech().empty();
        if (fsm.color_focused || on_drive) set_bottom_hint(L("hint.edit_color"));
        else if (row && *row == "p_GroupItem_0") set_bottom_hint(L("hint.edit_controls"));
        else set_bottom_hint(std::nullopt);
    }
    const int64_t k = fs_entry();
    if (k <= 0 && !chip_known(fc_key(g_list_f, g_list_c)) && g_F % 30 == 0) {
        if (const auto msg = message(fsm.text.get()); msg && *msg == "01") {
            for (MO* c : dd::scene_ctrls())
                if (engine::live::side_from_parents(c) == 3) { learn_chip_slots(c, g_list_f, g_list_c, rect_rgba(fsm.rect0.get()), rect_rgba(fsm.rect1.get())); break; }
        }
    }
    if (fsm.restore_steps <= 0 && k != fsm.last_k) {               // remember (not while walking)
        json& s = g_state["fs"];
        lj::set(s, "rw", k); lj::set(s, "fighter", jv(g_list_f)); lj::set(s, "costume", jv(g_list_c));
        const Entry* e = k > 0 ? entry_k(entries_for(num(lj::index(s, "fighter")), num(lj::index(s, "costume"))), k) : nullptr;
        lj::set(s, "path", e ? json(e->ident) : json());
        save_state();
    }
    if (k != fsm.last_k) fs_shown(k, "fighter settings");
    if (k <= 0) return;
    const Entry* entry = entry_k(entries_for(g_list_f, g_list_c), k);
    if (!entry) return;
    if (const auto chip = entry_chip(g_list_f, g_list_c, *entry)) {
        paint_rect(fsm.rect0.get(), chip->first);
        paint_rect(fsm.rect1.get(), chip->second);
    }
}

// ---- training pause > Character Settings (tick_training_settings) ------------------------------------------------------
void tick_training_settings() {
    const auto ags = agents(kAgentTcs);
    MO* agent = ags.empty() ? nullptr : ags[0];
    if (!agent) { tcs.agent.set(nullptr); tcs.text.set(nullptr); tcs.spin.set(nullptr); tcs.visible = false; return; }
    MO* cm = call_obj(agent, "get_ControlMain");
    const auto v = call_bool(cm, "get_ActualVisible");
    if (!(v && *v)) {
        g_apply_popup.set = false;
        if (tcs.visible && fsm.tcs_guide) { fsm.hidden_now = true; set_bottom_hint(std::nullopt); fsm.hidden_now = false; fsm.tcs_guide = false; }
        tcs.visible = false; tcs.text.set(nullptr); tcs.spin.set(nullptr); return;
    }
    if (agent != tcs.agent.get() || !tcs.text || !tcs.visible) {
        tcs.visible = true;
        fsm.guide_label.reset(); fsm.tcs_guide = true;
        MO* spin = find(cm, "p_Spin_color");
        tcs.agent.set(agent);
        tcs.param.set(agent_param(agent));
        tcs.text.set(spin ? find(spin, "e_text") : nullptr);
        tcs.rect0.set(spin ? find(spin, "e_rect_color_0") : nullptr);
        tcs.rect1.set(spin ? find(spin, "e_rect_color_1") : nullptr);
        tcs.spin.set(find_parts_spin(field_obj(agent, "_RootItem"), "color", 0));
        tcs.decided = false;
        tcs.base.reset();
        tcs.opened_at = g_F;
        tcs.target = field_i32(tcs.param.get(), "SettingTargetPlayerIndex") == 1 ? 2 : 1;
        const json& s = sel_of(tcs.target);
        tcs.restore_steps = (rw_or0(s) > 0 && eq(lj::index(s, "fighter"), g_list_f) && eq(lj::index(s, "costume"), g_list_c)) ? g_extra_count + 30 : 0;
    }
    if (!tcs.text || !tcs.param || !alive(tcs.text.get()) || !alive(tcs.param.get())) { tcs.agent.set(nullptr); tcs.text.set(nullptr); return; }
    if (g_F % 5 == 0 && !fsm.visible) { find_guide_list(); fsm.tcs_guide = true; set_bottom_hint(std::nullopt); }
    const auto msg = message(tcs.text.get());
    const int64_t k = msg ? label_to_k(g_list_f, g_list_c, *msg) : 0;
    if (msg && g_F % 9 == 0) {                                      // "colour to apply": the popup's live fighter / costume / colour
        MO* mfs = field_obj(tcs.param.get(), "MatchingFighterSetting");
        if (mfs && !alive(mfs)) mfs = nullptr;
        const auto lf = field_i32(mfs, "FighterId");
        const auto lc = call_i32(mfs, "GetCostumeId");
        const auto ln = call_i32(mfs, "GetColorId");
        const Opt f2 = lf ? Opt(*lf) : g_list_f;
        if (!tcs.base && lf && lc && ln) tcs.base = Tcs::Base{ *lf, *lc, *ln };
        g_apply_popup = { true, field_i32(tcs.param.get(), "SettingTargetPlayerIndex") == 1 ? 2 : 1, f2, g_F };
    }
    const auto dec = [&]() -> bool { bool b = false; return tcs.param && ll::read_field_bool(tcs.param.get(), "IsDecide", b) && b; }();
    if (dec && !tcs.decided) {                                      // confirm: commit to the targeted side
        const int side = field_i32(tcs.param.get(), "SettingTargetPlayerIndex") == 1 ? 2 : 1;
        json& s = sel_of(side);
        lj::set(s, "rw", k); lj::set(s, "fighter", jv(g_list_f)); lj::set(s, "costume", jv(g_list_c));
        const Entry* e = k > 0 ? entry_k(entries_for(g_list_f, g_list_c), k) : nullptr;
        lj::set(s, "path", e ? json(e->ident) : json());
        save_state();
        Tcs::LiveRefresh r;
        r.side = side; r.at = g_F + 15; r.setups = g_setups[side];
        MO* ctrl = dd::battle_ctrl(side);
        r.addr = (uintptr_t)ctrl;
        MO* mfs = field_obj(tcs.param.get(), "MatchingFighterSetting");
        if (mfs && tcs.base) {                                     // a nil read compares unequal: a reload
            const auto pf = field_i32(mfs, "FighterId"); const auto pc = call_i32(mfs, "GetCostumeId"); const auto pn = call_i32(mfs, "GetColorId");
            const int mf = ctrl ? dd::ctrl_fighter(ctrl) : -1;
            const auto& b = *tcs.base;
            const bool same = pf && pc && pn && *pf == b.f && *pc == b.c && *pn == b.n;
            r.reload = !same || (mf >= 0 && pf && *pf != mf);
        }
        cmn::journal("training settings: P%d -> RW %lld reload=%s", side, (long long)k, r.reload ? (*r.reload ? "true" : "false") : "nil");
        tcs.live_refresh = r;
    }
    tcs.decided = dec;
    if (k > 0 && tcs.rect0 && tcs.rect1) {
        if (const Entry* entry = entry_k(entries_for(g_list_f, g_list_c), k))
            if (const auto chip = entry_chip(g_list_f, g_list_c, *entry))
                if (nb("screens_tracker")) paint(tcs.target + 10, tcs.text, tcs.rect0, tcs.rect1, "", chip->first, chip->second);
    }
}

// ---- tick() ------------------------------------------------------------------------------------------------------------
void tick() {
    if (g_F % 3 != 0) return;
    if (nb("screens_guide")) screens::guide::set_flags(fsm.visible, fsm.tcs_guide);
    resolve();
    if (nb("screens_dipanel")) {
        const uint64_t a1 = addr(g_menus[0].agent), a2 = addr(g_menus[1].agent);
        screens::dipanel::mode::tick(1, a1); screens::dipanel::mode::tick(2, a2);
    }
    if (nb("screens_rows")) {
        const int side = (g_menus[1].active && !g_menus[0].active) ? 2 : 1;
        const bool suppress = ed_active() || dialogs::options::driver::window_open();
        const uint64_t a = addr(fsm.agent);
        screens::rows::host(fsm.visible, a, (int)g_list_f.value_or(0), (int)g_list_c.value_or(0), side, suppress);
    }
    tick_fighter_settings();
    tick_training_settings();
    for (Menu& m : g_menus) tick_menu(m);
    // the fighter each side is choosing (to_apply: the popup first, then the select menus seen in the last 60 frames)
    int64_t f[3] = { 0, 0, 0 };
    std::vector<const ToApply*> list;
    if (g_apply_popup.set) list.push_back(&g_apply_popup);
    for (int side = 1; side <= 2; ++side) if (g_apply_menu[side].set && g_F - g_apply_menu[side].frame < 60) list.push_back(&g_apply_menu[side]);
    for (const ToApply* t : list) if ((t->side == 1 || t->side == 2) && f[t->side] == 0 && t->fighter) f[t->side] = *t->fighter;
    const std::string key = std::to_string(f[1]) + "," + std::to_string(f[2]);
    if (key != g_side_hint && nb("vfx")) {
        vfx::feed::set_side_hint(1, (int)f[1]); vfx::feed::set_side_hint(2, (int)f[2]);
        g_side_hint = key;
    }
}

// ---- late steps ----------------------------------------------------------------------------------------------------------
void refresh_fn() {                                                // tcs.refresh_fn
    if (!tcs.live_refresh || g_F < tcs.live_refresh->at) return;
    const auto r = *tcs.live_refresh;
    if (g_setups[r.side] != r.setups) { tcs.live_refresh.reset(); return; }
    if (r.reload != false) {                                       // reload expected (or unknown): never touch the old model
        if (g_F < r.at + 600) return;
        tcs.live_refresh.reset(); return;
    }
    tcs.live_refresh.reset();
    MO* ctrl = dd::battle_ctrl(r.side);
    if (!ctrl || (uintptr_t)ctrl != r.addr) return;
    set_color(ctrl, std::nullopt, "popup refresh P" + std::to_string(r.side), false);
}

void fs_refresh_step() {                                           // SAVE follow-up: Outfit +1, Outfit -1, then walk
    if (!fsm.refresh || ed_active() || dialogs::options::driver::window_open()) return;
    if (!fsm.agent || !alive(fsm.agent.get())) { fsm.refresh.reset(); return; }
    Refresh& rq = *fsm.refresh;
    const uint64_t elapsed = g_F - rq.at;
    if (rq.step == 0 && elapsed >= 10) {
        MO* spin = find_parts_spin(field_obj(fsm.agent.get(), "_RootItem"), "costume", 0);
        if (!spin) { fsm.refresh.reset(); return; }
        rq.spin.set(spin); spin_call(spin, "Add", "refresh"); rq.step = 1;
    } else if (rq.step == 1 && elapsed >= 40) {
        spin_call(rq.spin.get(), "Sub", "refresh"); rq.step = 2;
    } else if (rq.step == 2 && elapsed >= 70) {
        const auto path = rq.path;
        fsm.refresh.reset();
        if (const auto* list = entries_for(g_list_f, g_list_c))
            for (size_t i = 0; i < list->size(); ++i) {
                if (path && (*list)[i].ident == *path) {
                    json& s = g_state["fs"];
                    lj::set(s, "rw", (int64_t)i + 1); lj::set(s, "path", (*list)[i].ident);
                    lj::set(s, "fighter", jv(g_list_f)); lj::set(s, "costume", jv(g_list_c));
                    save_state();
                    fsm.opened_at = g_F - 30; fsm.restore_steps = g_extra_count + 30; fsm.last_k = 0;
                    break;
                }
            }
    }
}

uint32_t pad_mask() {                                              // every connected pad's buttons
    static API::Method* m_devices = nullptr;
    void* gp = API::get()->get_native_singleton("via.hid.GamePad");
    if (!gp) return 0;
    if (!m_devices) if (auto* td = API::get()->tdb()->find_type("via.hid.GamePad")) m_devices = ll::find_method_noargs(td, "get_ConnectingDevices");
    if (!m_devices) return 0;
    auto r = m_devices->invoke(reinterpret_cast<MO*>(gp), std::vector<void*>{});
    MO* devs = r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
    const int32_t n = devs ? call_i32(devs, "get_Count").value_or(0) : 0;
    uint32_t mask = 0;
    for (int32_t i = 0; i < n; ++i) if (MO* pad = list_item(devs, i)) if (auto b = call(pad, "get_Button")) mask |= b->dword;
    return mask;
}
void device_tick() {                                               // the last device that produced an input
    if (pad_mask() != 0) { fsm.pad = true; return; }
    for (int vk : kPadKeys) if (key_down(vk)) { fsm.pad = false; return; }
    if (key_down(0x01) || key_down(0x02) || key_down(0x04)) { fsm.pad = false; return; }
    POINT p{};
    if (GetCursorPos(&p)) {
        if (fsm.mouse_known && (std::abs(p.x - fsm.mouse_x) > 2 || std::abs(p.y - fsm.mouse_y) > 2)) fsm.pad = false;
        fsm.mouse_known = true; fsm.mouse_x = p.x; fsm.mouse_y = p.y;
    }
}
// a method found by its name alone (Lua obj:call("Name", ...) on a method whose signature we do not spell)
std::optional<reframework::InvokeRet> call_named(MO* o, const char* name, const std::vector<void*>& args) {
    if (!o) return std::nullopt;
    for (auto* m : o->get_type_definition()->get_methods()) {
        if (!m || !m->get_name() || std::string_view(m->get_name()) != name || m->get_num_params() != args.size()) continue;
        auto r = m->invoke(o, args);
        if (r.exception_thrown) return std::nullopt;
        return r;
    }
    return std::nullopt;
}

// ---- the editor relay's screen half (edit.ned: push / prepare / open / open_di / frame), owner screens_colour ---------
struct EditMirror {                                    // edit.* as the relay kept them (updated from what the editor answers)
    bool active = false, di = false, di_simple = false, di_at_select = false, reopen = false, bits = false;
    bool has_closed_at = false; int64_t closed_at = 0;
    bool key_was_down = false, di_was_down = false, drv_was_down = false;
    bool di_side_was[3] = {};
    std::set<std::string> pristine_done, union_done;
    Ref ctrl;                                          // edit.ctrl
    std::vector<Ref> blocked;                          // edit.blocked_agents
    bool tm_input_off = false;
    int64_t reopen_wait_from = -1;                     // SAVE AS: the frame the reopen started waiting for the preview
};
EditMirror& edit = *new EditMirror();
bool ed_active() { return edit.active; }
bool window_open() { return dialogs::options::driver::window_open(); }                 // _G.NativeOptionsWindowOpen
int64_t dbg_frame() { return (int64_t)g_F - 1; }                                      // dbg.frame (cs.frame - 1 here)

struct PadBits { uint32_t btn = 64, a = 32, y = 16, b = 128, l1 = 256, r1 = 1024, r3 = 8192, l3 = 4096, l2 = 512, r2 = 2048; bool done = false; };
PadBits g_pad;                                         // PAD: via.hid.GamePadButton read from the live enum
void resolve_pad() {
    if (g_pad.done) return;
    g_pad.done = true;
    auto* td = API::get()->tdb()->find_type("via.hid.GamePadButton");
    if (!td) return;
    struct { const char* f; uint32_t* v; } fields[] = { { "RLeft", &g_pad.btn }, { "RDown", &g_pad.a }, { "RUp", &g_pad.y }, { "LTrigTop", &g_pad.l1 },
        { "RTrigTop", &g_pad.r1 }, { "RStickPush", &g_pad.r3 }, { "LTrigBottom", &g_pad.l2 }, { "RTrigBottom", &g_pad.r2 }, { "LStickPush", &g_pad.l3 },
        { "RRight", &g_pad.b } };
    for (auto& e : fields) if (auto* f = td->find_field(e.f)) *e.v = (uint32_t)f->get_data<int32_t>(nullptr);
}
bool pad_btn_down() { resolve_pad(); return (pad_mask() & g_pad.btn) != 0; }

// the X / Square of one player's device: app.InputManager's own per-player reading, else any device
bool side_x_down(int side) {
    resolve_pad();
    if (MO* im = API::get()->get_managed_singleton("app.InputManager")) {
        static API::Method* m_flags = nullptr;
        if (!m_flags) m_flags = im->get_type_definition()->find_method("GetGamePadRawFlags");
        if (m_flags) {
            auto r = m_flags->invoke(im, std::vector<void*>{ ll::arg_u32(g_pad.btn), ll::arg_i32(side - 1) });
            if (!r.exception_thrown) {
                if (r.dword & 1) return true;
                if (const auto kb = call_i32(im, "GetKeyboardMousePlayerIndex")) return *kb == side - 1 && key_down(0x58);
            }
        }
    }
    return key_down(0x58) || pad_btn_down();
}

// imgui.get_mouse / get_display_size: the game window's client area, refreshed while the game has the focus
HWND game_window() {
    static HWND cached = nullptr;
    if (cached && IsWindow(cached)) return cached;
    cached = nullptr;
    struct Ctx { DWORD pid; HWND best = nullptr; long best_area = 0; };
    Ctx ctx{ GetCurrentProcessId() };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0; GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != c->pid || !IsWindowVisible(hwnd)) return TRUE;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return TRUE;
        const long area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > 0 && (!c->best || area > c->best_area)) { c->best = hwnd; c->best_area = area; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    cached = ctx.best;
    return cached;
}
bool mouse_state(double& x, double& y, double& w, double& h) {
    static bool have = false; static double lx = 0, ly = 0;
    HWND hw = game_window();
    RECT rc{};
    if (!hw || !GetClientRect(hw, &rc)) return false;
    w = rc.right - rc.left; h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    if (GetForegroundWindow() == hw) { POINT p{}; if (GetCursorPos(&p) && ScreenToClient(hw, &p)) { lx = p.x; ly = p.y; have = true; } }
    if (!have) return false;
    x = lx; y = ly;
    return true;
}
bool mouse_over_color_row() {                          // the Color row of Fighter Settings (1080p: x 156-716, y 283-336)
    double mx, my, w, h;
    if (!mouse_state(mx, my, w, h)) return false;
    const double sx = w / 1920, sy = h / 1080;
    return mx >= 156 * sx && mx <= 716 * sx && my >= 283 * sy && my <= 336 * sy;
}
bool mouse_over_drive_row() {                          // dbg.drive_rows.mouse_over: the focused Drive row, 70 px apart
    const std::string fm = screens::rows::focused_mech();
    const char* names[5] = { "impact", "reversal", "parry", "rush", "special" };
    int wi = 0;
    for (int i = 0; i < 5; ++i) if (fm == names[i]) wi = i + 1;
    if (!wi) return false;
    double mx, my, w, h;
    if (!mouse_state(mx, my, w, h)) return false;
    const double sx = w / 1920, sy = h / 1080, top = 213 + 70 * (4 + wi);
    return mx >= 156 * sx && mx <= 716 * sx && my >= top * sy && my <= (top + 53) * sy;
}

std::vector<MO*> editor_ctrls() {                     // the preview's controllers, the main one (no "_Parts") first
    std::vector<MO*> out;
    for (MO* c : dd::scene_ctrls()) {
        if (engine::live::side_from_parents(c) != 3) continue;
        if (engine::live::go_name(c).find("_Parts") != std::string::npos) out.push_back(c); else out.insert(out.begin(), c);
    }
    return out;
}
MO* editor_ctrl() {
    MO* c = edit.ctrl.get();
    if (c && alive(c) && engine::live::side_from_parents(c) == 3) return c;
    const auto v = editor_ctrls();
    edit.ctrl.set(v.empty() ? nullptr : v[0]);
    return edit.ctrl.get();
}

bool write_data(const std::string& rel, const std::vector<uint8_t>& bytes) {    // write_file: folders made on the way
    const std::filesystem::path p = std::filesystem::path(kDataPrefix) / rel;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { cmn::journal("save error %s", rel.c_str()); return false; }
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    return (bool)f;
}
int64_t data_size(const std::string& rel) {            // file_size (-1 = missing)
    std::error_code ec;
    const auto n = std::filesystem::file_size(std::filesystem::path(kDataPrefix) / rel, ec);
    return ec ? -1 : (int64_t)n;
}
template <class F> void for_colors(MO* ctrl, F&& fn) {  // ColorData.Colors[i] (a List)
    MO* colors = field_obj(field_obj(ctrl, "ColorData"), "Colors");
    const int32_t n = colors ? call_i32(colors, "get_Count").value_or(0) : 0;
    for (int32_t i = 0; i < n; ++i) fn(list_item(colors, i), n);
}

void snapshot_pristine(MO* ctrl, int64_t f, int64_t c) {   // the game's own colours, once per fighter / costume
    const std::string key = std::to_string(f) + "_" + std::to_string(c);
    if (!edit.pristine_done.insert(key).second) return;
    int taken = 0, total = 0;
    for_colors(ctrl, [&](MO* entry, int32_t n) {
        total = n;
        const auto id = field_i32(entry, "ColorId");
        if (!id) return;
        const std::string rel = "SF6_ColorSpinExtra_data/pristine/" + key + "_game_" + std::to_string(*id) + ".user.2";
        if (data_size(rel) > 0) return;
        auto t = engine::live::live_tree(field_obj(entry, "Data"), 0);
        if (!t) { cmn::journal("pristine %d: no tree", *id); return; }
        if (write_data(rel, rsz::serialize(t))) ++taken;
    });
    cmn::journal("pristine snapshot %s: %d colours written (of %d)", key.c_str(), taken, total);
}

void ned_prepare() {                                   // edit.ned.prepare: what editor_open reads from the screen and the model
    edit.ctrl.set(nullptr);                            // the screen rebuilds its model (outfit change): fresh lookup
    MO* ctrl = editor_ctrl();
    if (!ctrl || !g_list_f || !g_list_c) {
        dialogs::editor::set_open_args((int)g_list_f.value_or(0), (int)g_list_c.value_or(0), 0, 0, false, false, false, 0, 0);
        return;
    }
    const int64_t f = *g_list_f, c = *g_list_c;
    const auto cid = color_num(ctrl);
    const int64_t k = fs_entry();
    const Entry* entry = k > 0 ? entry_k(entries_for(f, c), k) : nullptr;
    if (!entry) snapshot_pristine(ctrl, f, c);
    bool has_live = false;
    if (cid) if (MO* data = engine::live::color_data(ctrl, (int32_t)*cid))
        if (auto live = engine::live::live_tree(data, 0)) has_live = write_data("SF6_ColorSpinExtra_data/_editor_live.user.2", rsz::serialize(live));
    const std::string ukey = std::to_string(f) + "_" + std::to_string(c);
    if (edit.union_done.insert(ukey).second) {         // once per costume: the slots enabled in any of its game colours
        std::vector<std::string> keys;
        std::set<std::string> seen;
        for_colors(ctrl, [&](MO* e, int32_t) {
            auto t = e ? engine::live::live_tree(field_obj(e, "Data"), 0) : nullptr;
            const rsz::Value* parts = t ? t->field("Parts") : nullptr;
            if (!parts) return;
            for (size_t pi = 0; pi < parts->arr.size(); ++pi) {
                const rsz::Value* cls = parts->arr[pi] ? parts->arr[pi]->field("Clusters") : nullptr;
                if (!cls) continue;
                for (size_t ci = 0; ci < cls->arr.size(); ++ci) {
                    const rsz::Value* ccs = cls->arr[ci] ? cls->arr[ci]->field("CustomizeColors") : nullptr;
                    if (!ccs) continue;
                    for (size_t kk = 0; kk < ccs->arr.size(); ++kk) {
                        const rsz::Node* cc = ccs->arr[kk].get();
                        const rsz::Value* en = cc ? cc->field("Enable") : nullptr;
                        if (!(en && en->b)) continue;
                        const std::string key = std::to_string(pi + 1) + "_" + std::to_string(ci + 1) + "_" + std::to_string(kk + 1);
                        if (seen.insert(key).second) keys.push_back(key);
                    }
                }
            }
        });
        dialogs::editor::set_union((int)f, (int)c, std::move(keys));
    }
    const auto g = cid ? dd::game_chips((int)f, (int)c, *cid) : std::nullopt;
    dialogs::editor::set_open_args((int)f, (int)c, (int)cid.value_or(0), (int)k, true, has_live, g.has_value(), g ? g->first : 0, g ? g->second : 0);
}
// SAVE AS reopens the editor once the list is reloaded and walked; the preview model is still being rebuilt then
// (costume +1 / -1, new colour) and the single try gave up on "no preview controller". The screen stays busy while
// the reopen waits for the preview's controller, 3 s at most (then the try happens as before).
bool reopen_waits_for_preview() {
    if (!(edit.reopen && !edit.active && !window_open() && fsm.visible && !fsm.refresh && fsm.restore_steps <= 0) || editor_ctrl()) {
        edit.reopen_wait_from = -1;
        return false;
    }
    if (edit.reopen_wait_from < 0) edit.reopen_wait_from = dbg_frame();
    return dbg_frame() - edit.reopen_wait_from < 180;
}
void ned_push() {                                      // edit.ned.push: what the editor reads from the screens
    if (!edit.bits) { edit.bits = true; resolve_pad(); dialogs::editor::set_pad_bits(g_pad.btn, g_pad.a, g_pad.y, g_pad.b, g_pad.l1, g_pad.r1, g_pad.r3, g_pad.l3); }
    const bool busy = fsm.refresh.has_value() || fsm.restore_steps > 0 || reopen_waits_for_preview();
    dialogs::editor::set_screen(fsm.visible, fsm.pad == true, busy, addr(fsm.agent));
}
void take(const dialogs::editor::State& st) {
    edit.active = st.active; edit.di = st.di;
    if (st.has_closed_at) { edit.has_closed_at = true; edit.closed_at = st.closed_at; }
    edit.reopen = st.reopen;
}
bool settled_since_close() { return dbg_frame() - (edit.has_closed_at ? edit.closed_at : -1000) > 30; }
void ned_open() {
    if (edit.active || window_open()) return;
    ned_prepare();
    ned_push();
    const auto st = dialogs::editor::open();
    edit.active = st.active; edit.di = false;
}
void ned_open_di(const Opt& f, const Opt& c, bool simple, int side, const std::string& mech) {
    if (edit.active || window_open()) return;
    ned_push();
    const auto st = dialogs::editor::open_di((int)f.value_or(0), (int)c.value_or(0), simple, side == 2 ? 2 : 1, mech, edit.di_at_select);
    edit.active = st.active; edit.di = st.active;
    edit.di_simple = st.active && simple;
}
void ned_frame() {                                     // the loop: once per frame while a screen or the editor is up
    if (!(fsm.visible || edit.active || edit.reopen)) return;
    if (edit.reopen && !edit.active && !window_open() && fsm.visible && !fsm.refresh && fsm.restore_steps <= 0) ned_prepare();
    ned_push();
    if (edit.active) {
        double x = 0, y = 0, w = 1920, h = 1080;
        const bool m = mouse_state(x, y, w, h);
        dialogs::editor::set_input(pad_mask(), m, x, y, w, h);
    }
    take(dialogs::editor::frame(dbg_frame()));
}

void block_popup_agents() {                            // the popup must not see the Esc that closes our menu
    edit.blocked.clear();
    if (MO* tm = API::get()->get_managed_singleton("app.training.TrainingManager"))
        if (call_named(tm, "SetAllMenuInput", { ll::arg_bool(false) })) edit.tm_input_off = true;
    for (const char* n : { kAgentTcs, "GenericCharacterSetting_FG", "ui11200" })
        for (MO* a : agents(n)) { api()->ui_set_block_input(cmn::client(), a, 1); Ref r; r.set(a); edit.blocked.push_back(std::move(r)); }
}

// editor_tick: the device, the triggers (C / F / A / click, X per player, the Drive rows), the select screens' guide
void editor_tick() {
    resolve_pad();
    if (fsm.visible || ed_active()) device_tick();
    bool down = false, di_down = false;
    if (fsm.visible) {                                                 // (C opened it from any row too: a Lua-era shortcut, removed 0.19.4)
        di_down = key_down(0x58) || pad_btn_down();                    // X / Square: the Drive colour menu
        if (fsm.color_focused) {
            down = key_down(0x46) || (pad_mask() & g_pad.a) != 0;       // F / A on the Color row
            if (key_down(0x01) && mouse_over_color_row()) down = true; // left click on the row only
        }
    }
    if (down && !edit.key_was_down && !edit.active && !window_open() && settled_since_close()) {
        cmn::journal("editor key (fsm.visible=%s)", fsm.visible ? "true" : "false");
        ned_open();
    }
    edit.key_was_down = down;
    Opt di_fighter = g_list_f, di_costume = g_list_c;
    int di_side = (g_menus[1].active && !g_menus[0].active) ? 2 : 1;
    MO* popup_mfs = nullptr;
    if (!fsm.visible && tcs.visible) {                                 // training pause > Character Settings popup
        di_down = key_down(0x58) || pad_btn_down();
        const json& s = sel_of(tcs.target);
        di_fighter = num(lj::index(s, "fighter")); di_costume = num(lj::index(s, "costume"));
        di_side = tcs.target;
        MO* mfs = field_obj(tcs.param.get(), "MatchingFighterSetting");
        if (mfs && alive(mfs)) {                                       // the popup's live choice
            popup_mfs = mfs;
            if (const auto f = field_i32(mfs, "FighterId"); f && *f > 0) di_fighter = *f;
            if (const auto c = call_i32(mfs, "GetCostumeId")) di_costume = *c;
        }
    }
    Menu* csm = nullptr;
    if (!fsm.visible && !tcs.visible) csm = g_menus[0].active ? &g_menus[0] : (g_menus[1].active ? &g_menus[1] : nullptr);
    if (csm) {
        di_down = key_down(0x58) || pad_btn_down();
        const json& s = sel_of(csm->side);
        di_fighter = num(lj::index(s, "fighter")); di_costume = num(lj::index(s, "costume"));
        di_side = csm->side;
        if (const auto f = call_i32(csm->param.get(), "get_FighterId"); f && *f > 0) di_fighter = *f;
        if (const auto c = call_i32(csm->param.get(), "GetCostumeId")) di_costume = *c;
        if (dbg_frame() % 5 == 0) { find_guide_list(); fsm.tcs_guide = true; fsm.cs_guide = true; set_bottom_hint(std::nullopt); }
    } else if (fsm.cs_guide) {                                         // left the panel: strip the suffix
        fsm.cs_guide = false;
        if (!tcs.visible) { fsm.hidden_now = true; set_bottom_hint(std::nullopt); fsm.hidden_now = false; fsm.tcs_guide = false; }
    }
    // the EDIT DRIVE COLORS menu for this screen (mech = the page to land on, "" = the first)
    auto open_di_editor = [&](const std::string& mech) {
        edit.di_at_select = csm != nullptr;
        ned_open_di(di_fighter, di_costume, !fsm.visible, di_side, mech);
        if (edit.active && !fsm.visible) block_popup_agents();
    };
    if (csm) {                                                         // character select: DI mode in each player's own panel
        for (int side = 1; side <= 2; ++side) {
            Menu& m = g_menus[side - 1];
            const bool sdown = m.active && side_x_down(side);
            bool ready = false;
            if (sdown && dbg_frame() - (int64_t)m.opened_at > 20) {
                const auto row = focused_row_name(m.agent.get());
                ready = row && row->rfind("p_Spin_", 0) == 0;          // the panel is settled and focused (not the grid's X)
            }
            if (sdown && !ready) edit.di_side_was[side] = true;
            if (ready && !edit.di_side_was[side]) {
                Opt f = num(lj::index(sel_of(side), "fighter")), c = num(lj::index(sel_of(side), "costume"));
                if (const auto lf = call_i32(m.param.get(), "get_FighterId"); lf && *lf > 0) f = *lf;
                if (const auto lc = call_i32(m.param.get(), "GetCostumeId")) c = *lc;
                const std::string res = nb("screens_dipanel")
                    ? screens::dipanel::mode::toggle_select(side, addr(m.param), addr(m.agent), (int)f.value_or(0), (int)c.value_or(0)) : std::string("failed");
                cmn::journal("DI panel P%d: %s", side, res.c_str());
            }
            edit.di_side_was[side] = sdown;
        }
        di_down = false;                                               // consumed per side
    } else if (di_down && !edit.di_was_down && tcs.visible && !edit.active && !window_open()) {
        if (popup_mfs && tcs.agent) {                                  // the popup's own rows become the DI rows
            if (di_active(di_side)) cmn::journal("DI panel (training popup) P%d: already on, X ignored (B leaves)", di_side);
            else {
                const bool ok = nb("screens_dipanel") && screens::dipanel::mode::enter_popup(di_side, addr(tcs.param), addr(tcs.agent),
                                                                                             (uint64_t)(uintptr_t)popup_mfs, (int)di_fighter.value_or(0), (int)di_costume.value_or(0));
                cmn::journal("DI panel (training popup) P%d: %s", di_side, ok ? "true" : "false");
            }
        } else {
            cmn::journal("DI panel (training popup): no MatchingFighterSetting");
        }
    } else if (di_down && !edit.di_was_down && !edit.active && !window_open() && settled_since_close()) {
        open_di_editor("");
    }
    edit.di_was_down = di_down;
    if (nb("screens_rows")) {                                          // the Drive rows: Decide / F / A / click opens that page
        std::string mech = screens::rows::take_decide();
        const std::string fm = screens::rows::focused_mech();
        if (fsm.visible && !fm.empty() && !edit.active && !window_open()) {
            bool rdown = key_down(0x46) || (pad_mask() & g_pad.a) != 0;
            if (key_down(0x01) && mouse_over_drive_row()) rdown = true;
            if (rdown && !edit.drv_was_down && mech.empty()) mech = fm;
            edit.drv_was_down = rdown;
        } else {
            edit.drv_was_down = false;
        }
        if (!mech.empty() && !edit.active && !window_open() && settled_since_close()) open_di_editor(mech);
    }
    if (nb("dialogs_editor")) ned_frame();                             // the editor's loop
}

const char* spin_direction(const std::string& label, const Opt& f, const Opt& c, int64_t k) {   // nullptr = the Lua raised
    if (!g_extra_first) return "Add";
    const int64_t total = *g_extra_first + g_extra_count;
    Opt cur;
    const int64_t kk = label_to_k(f, c, label);
    if (kk > 0) cur = *g_extra_first + kk - 1;
    else if (label.size() == 2 && std::isdigit((unsigned char)label[0]) && std::isdigit((unsigned char)label[1])) cur = std::stoi(label) - 1;
    if (!cur) return "Add";
    if (total == 0) return nullptr;                                // integer modulo by zero
    auto mod = [&](int64_t a) { int64_t r = a % total; return (r != 0 && ((r < 0) != (total < 0))) ? r + total : r; };
    const int64_t target = *g_extra_first + k - 1;
    return mod(cur.value() - target) < mod(target - *cur) ? "Sub" : "Add";
}
void track_path(json& s) {                                         // packs may have changed: find the file again
    const json& p = lj::index(s, "path");
    if (!(lj::truthy(p) && p.is_string())) return;
    if (const auto* list = entries_for(num(lj::index(s, "fighter")), num(lj::index(s, "costume"))))
        for (size_t i = 0; i < list->size(); ++i) if ((*list)[i].ident == p.get<std::string>()) { lj::set(s, "rw", (int64_t)i + 1); break; }
}
void fs_restore_step() {
    if (fsm.restore_steps <= 0 || !fsm.spin) return;
    if (g_F - fsm.opened_at < 30) return;                          // let the screen settle
    if (!alive(fsm.spin.get())) { fsm.restore_steps = 0; return; }
    json& s = g_state["fs"];
    track_path(s);
    if (eq(lj::index(s, "rw"), fs_entry())) {
        // The spin shows the remembered entry. The preview was dressed when the screen opened, while the spin still
        // showed the game's colour (02): it wears 02 until a SetColor, and the walk's steps never issue one (Add /
        // Sub between entries keep BASE_COLOR, the game skips it). Re-issue it here unless the tick already did.
        fsm.restore_steps = 0;
        if (fsm.last_k != rw_or0(s)) fs_shown(rw_or0(s), "fighter settings restored");
        return;
    }
    fsm.restore_steps -= 1;
    const auto msg = fsm.text ? message(fsm.text.get()) : std::nullopt;
    const char* dir = spin_direction(msg.value_or(""), num(lj::index(s, "fighter")), num(lj::index(s, "costume")), rw_or0(s));
    if (dir) spin_call(fsm.spin.get(), dir, "fsstep");
}
void tcs_restore_step() {
    if (tcs.restore_steps <= 0 || !tcs.spin || !tcs.text) return;
    if (g_F - tcs.opened_at < 30) return;
    if (!alive(tcs.spin.get()) || !alive(tcs.text.get())) { tcs.restore_steps = 0; return; }
    json& s = sel_of(tcs.target);
    track_path(s);
    const auto msg = message(tcs.text.get());
    const Opt sf = num(lj::index(s, "fighter")), sc = num(lj::index(s, "costume"));
    const int64_t k = msg ? label_to_k(sf, sc, *msg) : 0;
    if (eq(lj::index(s, "rw"), k)) { tcs.restore_steps = 0; return; }
    tcs.restore_steps -= 1;
    const char* dir = spin_direction(msg.value_or(""), sf, sc, rw_or0(s));
    if (dir) spin_call(tcs.spin.get(), dir, "tcsstep");
}

// ---- decide_pull / decide_push -------------------------------------------------------------------------------------------
void decide_pull() {
    const auto p = engine::decide::pull();
    g_list_f = p.list_known ? Opt(p.list_fighter) : std::nullopt;
    g_list_c = p.list_known ? Opt(p.list_costume) : std::nullopt;
    g_extra_first = p.extra_first >= 0 ? Opt(p.extra_first) : std::nullopt;
    g_extra_count = p.extra_count;
    g_extras = p.extras;
    for (int i = 0; i < 3; ++i) g_setups[i + 1] = p.setups[i];
    if (g_clears && p.mirror_clears != *g_clears) {                 // mirror rule: P2 picked P1's entry, left to the game
        json& s = g_state["p2"];
        lj::set(s, "rw", 0); lj::set(s, "path", json()); g_menus[1].selected = 0;
        save_state();
    }
    g_clears = p.mirror_clears;
}
struct { bool set = false; uint64_t a1 = 0, a2 = 0, at = 0; bool l1 = false, l2 = false; std::string sels; } g_sent;
void decide_push() {
    const Menu& m1 = g_menus[0]; const Menu& m2 = g_menus[1];
    const uint64_t a1 = addr(m1.param), a2 = addr(m2.param), at = addr(fsm.text);
    const bool l1 = m1.text && m1.active && !m1.restore_pending, l2 = m2.text && m2.active && !m2.restore_pending;
    if (!g_sent.set || g_sent.a1 != a1 || g_sent.l1 != l1 || g_sent.a2 != a2 || g_sent.l2 != l2 || g_sent.at != at) {
        engine::decide::push_ui(a1, l1, a2, l2, at);
        g_sent.set = true; g_sent.a1 = a1; g_sent.l1 = l1; g_sent.a2 = a2; g_sent.l2 = l2; g_sent.at = at;
    }
    const json& p1 = g_state["p1"]; const json& p2 = g_state["p2"]; const json& fs = g_state["fs"];
    const json* sels[3] = { &p1, &p2, &fs };
    std::string key;
    int v[9];
    for (int i = 0; i < 3; ++i) {
        const json& s = *sels[i];
        key += (i ? "," : "") + tostr(lj::index(s, "rw")) + "," + tostr(lj::index(s, "fighter")) + "," + tostr(lj::index(s, "costume"));
        v[i * 3] = (int)num(lj::index(s, "rw")).value_or(0);
        v[i * 3 + 1] = (int)num(lj::index(s, "fighter")).value_or(-2);
        v[i * 3 + 2] = (int)num(lj::index(s, "costume")).value_or(-2);
    }
    if (g_sent.sels != key) {
        engine::decide::push_sels(v);
        g_sent.sels = key;
    }
}

// The remembered selections are ranks in a colour list, and a rank moves when RW colours are added (a new pack, a
// colour mod from reframework/color_mods) or the MC ones are renumbered. Each one therefore also stores the colour's
// stable identity ("path", Entry::ident) and the rank is re-derived from it: at load, and after every catalogue
// rebuild. A state saved without "path" (older versions) keeps its rank as it is and gets its "path" now (first save).
// A colour whose file is not there now leaves the selection empty (default colour) but its identity is kept
// (colour_state resolve_selection): it comes back when the colour does. Does nothing while the catalogue is
// empty (not built yet, or every colour folder removed): the ranks then stay as they were.
void resolve_state() {
    if (dd::catalog().all().empty()) return;
    bool changed = false;
    for (const char* key : { "p1", "p2", "fs" }) {
        std::string note;
        if (colour::resolve_selection(g_state[key], dd::catalog(), &note)) changed = true;
        if (!note.empty()) cmn::journal("state %s: %s", key, note.c_str());
    }
    if (changed) { save_state(); g_sent.sels.clear(); }               // (the decisions get the new ranks at the next push)
}
void on_catalog_rebuilt() { if (g_state_ok) resolve_state(); }       // (live::mx() held by the rebuild)

bool boot() {                                                      // the main chunk: state and chip_cfg read once
    if (g_state_ok) return true;
    auto st = colour::load_state(kStateFile);
    if (!st) return false;
    g_state = std::move(*st);
    resolve_state();
    engine::decide::set_rebuilt_callback(on_catalog_rebuilt);
    if (auto cc = colour::load_chip_cfg(kChipFile)) g_chip_cfg = std::move(*cc);
    g_state_ok = true;
    return true;
}

// Training popup + the small DI menu: the Esc that closes our dialog also reaches the popup / the pause menu (their
// cancel does not go through the agents' BlockInput). While the small DI menu is up, every agent but our dialog
// ignores InputCancel, and the training menu refuses to close.
// Same for the popup's DI mode (the Drive rows borrowed): its own hook (screens/dipanel) leaves the mode on the popup
// agent's InputCancel, but the pause menu's agent took the same press and closed the whole menu, the mode's exit then
// refreshing a popup being torn down (a caught NullReference in SelectChangeColorChip). Pre-existing, Lua included.
bool esc_blocked() { return (edit.active && edit.di_simple && window_open()) || screens::dipanel::mode::popup_esc_guard(); }
int pre_input_cancel(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        if (!esc_blocked()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        MO* agent = reinterpret_cast<MO*>(argv[1]);
        if (!alive(agent)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        MO* go = call_obj(agent, "get_GameObject");
        if (go && name_of(go) == "OptionDialog") return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    } catch (...) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
int pre_close_menu(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try { return esc_blocked() ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL; } catch (...) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
}
void post_nothing(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

}  // namespace

// ---- public ----------------------------------------------------------------------------------------------------------------
void refresh_request(bool has_path, const std::string& path) {
    std::lock_guard lk(engine::live::mx());
    Refresh r;
    if (has_path) { std::string p = path; for (char& ch : p) if (ch == '/') ch = '\\'; r.path = p; }
    r.at = g_F;
    fsm.refresh = std::move(r);
}
void catalog_rebuilt() { std::lock_guard lk(engine::live::mx()); g_entry_chip.clear(); }
void invalidate() { std::lock_guard lk(engine::live::mx()); g_entry_chip.clear(); }
void chips_reload() {
    std::lock_guard lk(engine::live::mx());
    if (auto cc = colour::load_chip_cfg(kChipFile)) g_chip_cfg = std::move(*cc);
}

// ---- owner "screens_colour": the DLL tracks and acts (phase 5c-2) ---------------------------------------------------------
void frame(uint64_t lua_frame) {
    std::lock_guard lk(engine::live::mx());
    if (!boot()) return;
    g_F = lua_frame;
    { const int64_t t = perf::begin(); decide_pull(); perf::end(perf::ScreensDecidePull, t); }
    { const int64_t t = perf::begin(); tick(); perf::end(perf::ScreensTick, t); }
    { const int64_t t = perf::begin(); refresh_fn(); perf::end(perf::ScreensRefreshFn, t); }
    { const int64_t t = perf::begin(); fs_refresh_step(); perf::end(perf::ScreensFsRefresh, t); }
    { const int64_t t = perf::begin(); editor_tick(); perf::end(perf::ScreensEditorTick, t); }
    { const int64_t t = perf::begin(); fs_restore_step(); perf::end(perf::ScreensFsRestore, t); }
    { const int64_t t = perf::begin(); tcs_restore_step(); perf::end(perf::ScreensTcsRestore, t); }
    { const int64_t t = perf::begin(); decide_push(); perf::end(perf::ScreensDecidePush, t); }
    { const int64_t t = perf::begin(); dialogs::options::driver::apply(); perf::end(perf::ScreensOptApply, t); }
}

bool acting() { return true; }

void editor_refresh() {                                            // "RF": every preview controller takes its colour again
    std::lock_guard lk(engine::live::mx());
    for (MO* c : editor_ctrls()) dd::set_color(c, std::nullopt, "", true);   // why nil: not journaled
}

void apply_original(int color_id, const engine::rsz::NodeRef& tree) {   // "AO": RESET of a game colour, the live data back
    std::lock_guard lk(engine::live::mx());
    MO* ctrl = editor_ctrl();
    MO* data = ctrl ? engine::live::color_data(ctrl, color_id) : nullptr;
    if (data && tree) engine::live::write_tree(data, tree.get());
}

void teardown() {                                                  // "TD": what the triggers blocked for the small DI menu
    std::lock_guard lk(engine::live::mx());
    for (const Ref& a : edit.blocked) if (alive(a.get())) api()->ui_set_block_input(cmn::client(), a.get(), 0);
    edit.blocked.clear();
    if (edit.tm_input_off) {
        edit.tm_input_off = false;
        if (MO* tm = API::get()->get_managed_singleton("app.training.TrainingManager")) call_named(tm, "SetAllMenuInput", { ll::arg_bool(true) });
    }
}

bool install() {
    auto* tdb = API::get()->tdb();
    if (!tdb) return false;
    int n = 0;
    if (auto* td = tdb->find_type("app.UIAgent"))
        if (auto* m = td->find_method("InputCancel"); m && m->get_function_raw() && m->add_hook(perf::timed_pre<perf::HkTrkAgentInputCancel, pre_input_cancel>, post_nothing, false)) { ++n; perf::journal_hooked("UIAgent.InputCancel (HkTrkAgentInputCancel)", m); }
    if (auto* td = tdb->find_type("app.training.TrainingManager")) {
        using PreFn = int (*)(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long);
        struct CloseHook { const char* sig; PreFn fn; };
        const CloseHook close_hooks[] = {
            { "RequestCloseMenu", perf::timed_pre<perf::HkTrkRequestCloseMenu, pre_close_menu> },
            { "CloseMenu(System.Boolean)", perf::timed_pre<perf::HkTrkCloseMenuBool, pre_close_menu> },
            { "CloseMenu(System.Boolean, System.Boolean, System.Boolean)", perf::timed_pre<perf::HkTrkCloseMenu3, pre_close_menu> },
        };
        for (const auto& ch : close_hooks)
            if (auto* m = td->find_method(ch.sig); m && m->get_function_raw() && m->add_hook(ch.fn, post_nothing, false)) { ++n; perf::journal_hooked(ch.sig, m); }
    }
    cmn::journal("colour track: %d / 4 Esc hooks installed", n);
    return true;
}

std::string status() {
    char b[160];
    std::snprintf(b, sizeof b, "colour track: native, frames %llu", (unsigned long long)g_n.frames);
    return b;
}

}  // namespace screens::colour::track
