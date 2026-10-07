// screens::tracker - port of the colour-spin tracker's UI paint.
// See tracker.hpp header comment and docs/CPP_PORT_PHASE3.md section 3c.
//
// Rules kept:
//   R1  all GUI writes from the game thread (on_frame runs from the core's frame callback)
//   R7  never set_Message with identical text (core ui_set_text compare-before-write)
//   R16 /EHa (SEH around every non-trivial operation)
//   R37 discover through the core's cached registry, not full scans per frame

#include "tracker.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "ll_common.hpp"

#include "../common.hpp"

namespace screens::tracker {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

// ---- constants -----------------------------------------------------------

constexpr int kMaxFaults = 5;

// ---- core helpers --------------------------------------------------------

const SF6CoreAPI* api() { return cmn::core(); }
SF6CoreClient* cl() { return cmn::client(); }

// ---- fault handling (same pattern as dipanel) ----------------------------

bool g_on = false;
int  g_faults = 0;

void fault(const char* where) {
    ++g_faults;
    cmn::journal("[tracker] fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("[tracker] too many faults, off (Lua takes over)");
    }
}

// ---- per-side menu display state (pushed by Lua) -------------------------

struct MenuDisplay {
    uint64_t text_addr = 0;
    uint64_t rect0_addr = 0;
    uint64_t rect1_addr = 0;
    std::string label;            // "" = game owns the text (no custom entry)
    int64_t chip1 = -1, chip2 = -1;  // RGBA or -1 = no chips
    bool active = false;
};

// ---- Fighter Settings state ----------------------------------------------

struct FsState {
    uint64_t cm_addr = 0;        // ControlMain address for hint block hide
    bool setup_pending = false;  // one-shot: hide c_00 and c_02 blocks
    bool active = false;
};

// ---- camera hints state (pushed by Lua) ----------------------------------

struct CameraBlock {
    std::string block_name;
    std::string title;
    std::string icon1_name, icon1_text;
    std::string icon2_name, icon2_text;
    float title_x = 0, title_y = 0;
    float icon1_x = 0, icon1_y = 0;
    float icon2_x = 0, icon2_y = 0;
    float block_x = 0, block_y = 0;
    bool hide_textures = false;
    float font = 0;              // 0 = use ref_size
};

struct CameraState {
    bool show = false;
    bool pad = false;
    uint64_t agent_addr = 0;
    std::vector<CameraBlock> blocks;
    bool dirty = false;
};

// ---- dot markers state (pushed by Lua) -----------------------------------

struct DotEntry {
    std::string ctrl_name;       // e.g. "p_OptionUnit_0_switch_show"
    bool want = false;
    std::string glyph;
    float font = 16;
    float x = -4, y = -12;
};

struct DotsState {
    uint64_t param_addr = 0;
    std::vector<DotEntry> entries;
    bool dirty = false;
};

// ---- global state --------------------------------------------------------

std::mutex g_mx;
std::array<MenuDisplay, 13> g_menu;    // 1 = P1, 2 = P2 (select menus); 11 / 12 = the training popup (tcs.target + 10)
bool menu_side(int s) { return s == 1 || s == 2 || s == 11 || s == 12; }
FsState g_fs;
CameraState g_camera;
DotsState g_dots;
uint64_t g_paints = 0;                // diagnostics

// ---- CSV parsing helpers -------------------------------------------------

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim)) out.push_back(tok);
    return out;
}

static float to_float(const std::string& s, float dflt = 0) {
    try { return std::stof(s); } catch (...) { return dflt; }
}

// ---- paint: per-side menu label + chips ----------------------------------

void paint_menu(int side) {
    auto& d = g_menu[side];
    if (!d.active || !d.text_addr) return;

    // label: write only when non-empty (game colour = game owns the text)
    if (!d.label.empty()) {
        api()->ui_set_text(cl(), (void*)d.text_addr, d.label.c_str());
    }

    // chips
    bool has_chips = d.chip1 >= 0 && d.rect0_addr && d.rect1_addr;
    if (has_chips) {
        api()->ui_set_color(cl(), (void*)d.rect0_addr, (uint32_t)d.chip1);
        api()->ui_set_color(cl(), (void*)d.rect1_addr, (uint32_t)d.chip2);
    }
    ++g_paints;
}

// ---- paint: Fighter Settings setup (one-shot hide hint blocks) -----------

void paint_fs_setup() {
    if (!g_fs.setup_pending || !g_fs.cm_addr) return;
    g_fs.setup_pending = false;

    const char* names[] = { "c_00", "c_02" };
    void* found[2] = {};
    api()->ui_find_many((void*)g_fs.cm_addr, names, 2, found);
    for (int i = 0; i < 2; ++i) {
        if (found[i]) api()->ui_set_visible(cl(), found[i], 0);
    }
    cmn::journal("[tracker] fs_setup: hid %d hint blocks", (found[0] ? 1 : 0) + (found[1] ? 1 : 0));
}

// ---- paint: camera hints -------------------------------------------------

void paint_camera() {
    auto& cam = g_camera;
    if (!cam.agent_addr) return;

    auto* api_p = api();
    if (!api_p) return;

    // resolve ControlMain from the agent
    auto* agent = reinterpret_cast<MO*>(cam.agent_addr);
    if (!API::get()->sdk()->managed_object->is_managed_object(agent)) return;

    auto* get_cm = ll::find_method_noargs(agent->get_type_definition(), "get_ControlMain");
    if (!get_cm) return;
    std::vector<void*> noargs;
    auto rc = get_cm->invoke(agent, noargs);
    if (rc.exception_thrown || !rc.ptr) return;
    void* cm = rc.ptr;

    // resolve reference font size from c_01/e_text_0
    float ref_w = 0, ref_h = 0;
    {
        void* c01 = api_p->ui_find_cached(cm, "c_01");
        if (c01) {
            void* ref_t = api_p->ui_find_cached(c01, "e_text_0");
            if (ref_t) {
                auto* fs_m = ll::find_method_noargs(
                    reinterpret_cast<MO*>(ref_t)->get_type_definition(), "get_FontSize");
                if (fs_m) {
                    auto fr = fs_m->invoke(reinterpret_cast<MO*>(ref_t), noargs);
                    if (!fr.exception_thrown) {
                        std::memcpy(&ref_w, fr.bytes.data(), 4);
                        std::memcpy(&ref_h, fr.bytes.data() + 4, 4);
                    }
                }
            }
        }
    }

    for (auto& b : cam.blocks) {
        void* blk = api_p->ui_find_cached(cm, b.block_name.c_str());
        if (!blk) continue;

        // hide textures in the block (camera-Y arrows)
        if (b.hide_textures) {
            auto* blk_mo = reinterpret_cast<MO*>(blk);
            auto* get_child = ll::find_method_noargs(blk_mo->get_type_definition(), "get_Child");
            if (get_child) {
                auto cr = get_child->invoke(blk_mo, noargs);
                void* c = cr.exception_thrown ? nullptr : cr.ptr;
                while (c) {
                    auto* c_mo = reinterpret_cast<MO*>(c);
                    auto tn = std::string(c_mo->get_type_definition()->get_full_name());
                    if (tn == "via.gui.Texture") {
                        auto* gv = ll::find_method_noargs(c_mo->get_type_definition(), "get_Visible");
                        if (gv) {
                            auto vr = gv->invoke(c_mo, noargs);
                            if (!vr.exception_thrown && vr.dword)
                                api_p->ui_set_visible(cl(), c, 0);
                        }
                    }
                    auto* gn = ll::find_method_noargs(c_mo->get_type_definition(), "get_Next");
                    if (!gn) break;
                    auto nr = gn->invoke(c_mo, noargs);
                    c = nr.exception_thrown ? nullptr : nr.ptr;
                }
            }
        }

        if (!cam.show) {
            auto* gv = ll::find_method_noargs(
                reinterpret_cast<MO*>(blk)->get_type_definition(), "get_Visible");
            if (gv) {
                auto vr = gv->invoke(reinterpret_cast<MO*>(blk), noargs);
                if (!vr.exception_thrown && vr.dword)
                    api_p->ui_set_visible(cl(), blk, 0);
            }
            continue;
        }

        // title text
        void* title = api_p->ui_find_cached(blk, "e_text_title0");
        if (!title) title = api_p->ui_find_cached(blk, "e_text_title");
        if (title) {
            api_p->ui_set_text(cl(), title, b.title.c_str());
            api_p->ui_set_position(cl(), title, b.title_x, b.title_y, 0);
        }

        // icons
        struct { const std::string& name; const std::string& text; float x; float y; } icons[2] = {
            { b.icon1_name, b.icon1_text, b.icon1_x, b.icon1_y },
            { b.icon2_name, b.icon2_text, b.icon2_x, b.icon2_y },
        };
        for (auto& ic : icons) {
            if (ic.name.empty()) continue;
            void* panel = api_p->ui_find_cached(blk, ic.name.c_str());
            if (!panel) continue;
            api_p->ui_set_visible(cl(), panel, 1);
            api_p->ui_set_position(cl(), panel, ic.x, ic.y, 0);

            void* t = api_p->ui_find_cached(panel, "e_text_0");
            if (t) {
                api_p->ui_set_text(cl(), t, ic.text.c_str());
                // font size: use block-specific font if > 0, else reference size
                float fw = b.font > 0 ? b.font : ref_w;
                float fh = b.font > 0 ? b.font : ref_h;
                if (fw > 0 || fh > 0)
                    api_p->ui_set_font_size(cl(), t, fw, fh);
            }
        }

        // block position
        api_p->ui_set_position(cl(), blk, b.block_x, b.block_y, 0);
        // show the block
        api_p->ui_set_visible(cl(), blk, 1);
    }
    ++g_paints;
}

// ---- paint: dot markers --------------------------------------------------

void paint_dots() {
    auto& ds = g_dots;
    if (!ds.param_addr || ds.entries.empty()) return;

    auto* api_p = api();
    if (!api_p) return;

    // resolve agent and ControlMain from the Param
    auto* param = reinterpret_cast<MO*>(ds.param_addr);
    if (!API::get()->sdk()->managed_object->is_managed_object(param)) return;

    auto* agent = ll::read_field_object(param, "<Agent>k__BackingField");
    if (!agent) return;

    auto* get_cm = ll::find_method_noargs(agent->get_type_definition(), "get_ControlMain");
    if (!get_cm) return;
    std::vector<void*> noargs;
    auto rc = get_cm->invoke(agent, noargs);
    if (rc.exception_thrown || !rc.ptr) return;
    void* cm = rc.ptr;

    for (auto& dot : ds.entries) {
        void* ctrl = api_p->ui_find_cached(cm, dot.ctrl_name.c_str());
        if (!ctrl) continue;

        // walk children to find the visible prefab
        const char* prefab_names[] = { "p_Slider_0_s", "p_Spin_spintext_h", "p_GroupItem_button" };
        void* visible_prefab = nullptr;
        for (auto* pn : prefab_names) {
            void* p = api_p->ui_find_cached(ctrl, pn);
            if (!p) continue;
            auto* gv = ll::find_method_noargs(
                reinterpret_cast<MO*>(p)->get_type_definition(), "get_ActualVisible");
            if (!gv) continue;
            auto vr = gv->invoke(reinterpret_cast<MO*>(p), noargs);
            if (!vr.exception_thrown && vr.dword) { visible_prefab = p; break; }
        }
        if (!visible_prefab) visible_prefab = ctrl;

        // find the dot panel and text inside the visible prefab
        void* pnl = api_p->ui_find_cached(visible_prefab, "p_InputGuide_0_dUIOptionExtraButton");
        void* tx = pnl ? api_p->ui_find_cached(pnl, "e_text_0") : nullptr;
        if (!pnl || !tx) continue;

        if (dot.want) {
            api_p->ui_set_text(cl(), tx, dot.glyph.c_str());
            api_p->ui_set_font_size(cl(), tx, dot.font, dot.font);
            api_p->ui_set_position(cl(), pnl, dot.x, dot.y, 0);
            api_p->ui_set_force_invisible(cl(), pnl, 0);
            api_p->ui_set_visible(cl(), pnl, 1);
        } else {
            api_p->ui_set_visible(cl(), pnl, 0);
            api_p->ui_set_force_invisible(cl(), pnl, 1);
        }
    }
    ++g_paints;
}

// ---- tick (called from on_frame) -----------------------------------------

void tick_all() {
    std::lock_guard lk(g_mx);
    for (int s : { 1, 2, 11, 12 }) {
        paint_menu(s);
        // the training popup's chips are painted once per Lua push (the Lua repaints each tick only while an entry
        // shows, like paint_rect): never keep painting after the popup closed or went back to a game colour
        if (s >= 11) g_menu[s].active = false;
    }
    paint_fs_setup();
    if (g_camera.dirty) { paint_camera(); g_camera.dirty = false; }
    if (g_dots.dirty) { paint_dots(); g_dots.dirty = false; }
}

}  // namespace

// ---- public API ----------------------------------------------------------

bool install() {
    if (!api() || !SF6CORE_HAS(api(), ui_set_text) || !SF6CORE_HAS(api(), ui_set_color) || !SF6CORE_HAS(api(), ui_find_many)) {
        cmn::log_warn("[tracker] core without required APIs: off");
        return false;
    }
    g_on = true;
    cmn::journal("[tracker] installed");
    return true;
}

bool ready() { return g_on; }

void push_menu(int side, uint64_t text_addr, uint64_t rect0_addr, uint64_t rect1_addr,
               const std::string& label, int64_t chip1, int64_t chip2) {
    if (!menu_side(side)) return;
    std::lock_guard lk(g_mx);
    auto& d = g_menu[side];
    d.text_addr = text_addr;
    d.rect0_addr = rect0_addr;
    d.rect1_addr = rect1_addr;
    d.label = label;
    d.chip1 = chip1;
    d.chip2 = chip2;
    d.active = true;
}

void leave_menu(int side) {
    if (!menu_side(side)) return;
    std::lock_guard lk(g_mx);
    g_menu[side].active = false;
}

void push_fs_setup(uint64_t cm_addr) {
    std::lock_guard lk(g_mx);
    g_fs.cm_addr = cm_addr;
    g_fs.setup_pending = true;
    g_fs.active = true;
}

void on_frame() {
    if (!g_on) return;
    __try { tick_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("tick"); }
}

std::string status() {
    std::lock_guard lk(g_mx);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "tracker: on=%d faults=%d P1=%s P2=%s fs=%s paints=%llu",
                  g_on ? 1 : 0, g_faults,
                  g_menu[1].active ? "active" : "idle",
                  g_menu[2].active ? "active" : "idle",
                  g_fs.active ? "active" : "idle",
                  (unsigned long long)g_paints);
    return buf;
}

}  // namespace screens::tracker
