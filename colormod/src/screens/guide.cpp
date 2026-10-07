// screens::guide - port of the guide hints and corner_tick (phase 3 sub-step 3d).
// See guide.hpp header comment and docs/CPP_PORT_PHASE3.md section 3d.
//
// Rules kept:
//   R1  all GUI writes from the game-thread hooks / on_frame (LateUpdateBehavior)
//   R7  never set_Message with identical text (core ui_set_text compare-before-write)
//   R16 /EHa (SEH around every hook body)
//   CRITICAL: never touch a template we don't own (the 2-player screen has TWO
//   guide bars; writing "" into the wrong one's template corrupted P2's clone
//   model -> AV 0x2d419f3 on a background thread, 04/09)

#include "guide.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ll_common.hpp"
#include "../common.hpp"

namespace screens::guide {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

constexpr int kMaxFaults = 5;

// ---- state ---------------------------------------------------------------

bool g_on = false;                        // install succeeded
int  g_faults = 0;

// flags pushed by the Lua each frame
bool g_fsm_visible = false;              // Fighter Settings screen is up
bool g_tcs_guide = false;                // training Character Settings guide

// ---- corner state (the Lua's dbg.corner_own map) -------------------------

struct CornerEntry {
    uint64_t addr = 0;                   // template element address (key)
    uint64_t btn_addr = 0;               // parent c_guideBtn address
    std::string text;                    // text to show ("" = clear)
    float x = 0, y = 0;                 // position
    bool show = false;                   // whether the template should be visible
    uint64_t last_frame = 0;             // frame when last refreshed by the Lua
    float saved_y = 0;                   // for NewInputItem lower/raise
    bool has_saved_y = false;
};

std::mutex g_mx;
std::unordered_map<uint64_t, CornerEntry> g_corners;

// ---- guide fix state (per guide part) ------------------------------------

struct GuideFixEntry {
    uint64_t part_addr = 0;              // UIPartsInputGuideList address
    uint64_t text_addr = 0;              // text child address
    uint64_t ctrl_addr = 0;              // parent control address
    bool right = false;
    std::string want;                    // the full text to write
};

std::vector<GuideFixEntry> g_fixes;
std::unordered_set<uint64_t> g_fix_addrs; // part addresses for OnLateUpdate filtering

// ---- guide bar state (editor guide bar) ----------------------------------

std::string g_bar_want;                  // empty = no bar fix
std::string g_bar_desc;
uint64_t g_bar_param = 0;               // the Param address for filtering

// ---- helpers -------------------------------------------------------------

const SF6CoreAPI* api() { return cmn::core(); }
SF6CoreClient* cl() { return cmn::client(); }

void fault(const char* where) {
    ++g_faults;
    cmn::journal("[guide] fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("[guide] too many faults, off (Lua takes over)");
    }
}

// ---- via.gui child/next walk helpers (raw invoke) -------------------------

void* gui_child(void* ctrl) {
    if (!ctrl) return nullptr;
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_Child");
    if (!m) return nullptr;
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    return r.exception_thrown ? nullptr : r.ptr;
}

void* gui_next(void* ctrl) {
    if (!ctrl) return nullptr;
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_Next");
    if (!m) return nullptr;
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    return r.exception_thrown ? nullptr : r.ptr;
}

// Read the position of a via.gui control. Returns false if it fails.
bool gui_position(void* ctrl, float& x, float& y, float& z) {
    if (!ctrl) return false;
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_Position");
    if (!m) return false;
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    if (r.exception_thrown) return false;
    std::memcpy(&x, r.bytes.data(), 4);
    std::memcpy(&y, r.bytes.data() + 4, 4);
    std::memcpy(&z, r.bytes.data() + 8, 4);
    return true;
}

bool gui_force_invisible(void* ctrl) {
    if (!ctrl) return false;
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_ForceInvisible");
    if (!m) return false;
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    return !r.exception_thrown && r.dword != 0;
}

bool gui_visible(void* ctrl) {
    if (!ctrl) return false;
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_Visible");
    if (!m) return false;
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    return !r.exception_thrown && r.dword != 0;
}

std::string gui_name(void* ctrl) {
    if (!ctrl) return {};
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_Name");
    if (!m) return {};
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    if (r.exception_thrown || !r.ptr) return {};
    return ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr));
}

std::string gui_message(void* ctrl) {
    if (!ctrl) return {};
    auto* m = ll::find_method_noargs(reinterpret_cast<MO*>(ctrl)->get_type_definition(), "get_Message");
    if (!m) return {};
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(ctrl), na);
    if (r.exception_thrown || !r.ptr) return {};
    return ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr));
}

bool is_alive(void* obj) {
    return obj && API::get()->sdk()->managed_object->is_managed_object(reinterpret_cast<MO*>(obj));
}

// Push all children of btn to y=0 (except skip_addr). This is the core of the
// guide clamp: fresh clones inherit the template's height, so pushing them down
// within one frame keeps the bar from jumping.
void clamp_children(void* btn, uint64_t skip_addr) {
    if (!btn || !api()) return;
    void* c = gui_child(btn);
    while (c) {
        if (reinterpret_cast<uint64_t>(c) != skip_addr) {
            float cx = 0, cy = 0, cz = 0;
            if (gui_position(c, cx, cy, cz) && std::abs(cy) > 0.5f) {
                api()->ui_set_position(cl(), c, cx, 0.0f, cz);
            }
        }
        c = gui_next(c);
    }
}

// ---- corner_tick (the Lua's dbg.corner_tick) ------------------------------
// Runs every frame from on_frame. For each owned template:
//   1. Push children of btn to y=0 if they drifted (fresh clones)
//   2. If not refreshed last frame: restore (clear text, ForceInvisible, y=0)

void corner_tick_inner() {
    std::lock_guard lk(g_mx);
    if (g_corners.empty()) return;
    auto* a = api();
    if (!a) return;

    std::vector<uint64_t> to_remove;

    for (auto& [addr, e] : g_corners) {
        // 1. Every frame: push children of btn to y=0 (except the template)
        if (is_alive(reinterpret_cast<void*>(e.btn_addr))) {
            clamp_children(reinterpret_cast<void*>(e.btn_addr), e.addr);
        }

        // 2. Not refreshed last frame: our screen is gone -> restore
        if ((cmn::frame() - e.last_frame) > 1) {
            to_remove.push_back(addr);

            // Restore the template: clear text, ForceInvisible true
            void* tmpl = reinterpret_cast<void*>(e.addr);
            if (is_alive(tmpl)) {
                std::string cur = gui_message(tmpl);
                if (!cur.empty()) {
                    a->ui_set_text(cl(), tmpl, "");
                }
                if (!gui_force_invisible(tmpl)) {
                    a->ui_set_force_invisible(cl(), tmpl, 1);
                }
            }

            // Push all children of btn to y=0
            if (is_alive(reinterpret_cast<void*>(e.btn_addr))) {
                clamp_children(reinterpret_cast<void*>(e.btn_addr), 0);
            }
        }
    }

    for (uint64_t a : to_remove) {
        g_corners.erase(a);
    }
}

// ---- guide text fix (the Lua's guide_fix_one + guide_text_fix) -----------
// Called from the OnLateUpdate post-hook when the hook's part address matches
// one of our tracked parts.

void guide_fix_inner(uint64_t part_addr) {
    auto* a = api();
    if (!a) return;

    std::lock_guard lk(g_mx);
    for (auto& gf : g_fixes) {
        if (gf.part_addr != part_addr) continue;
        void* text = reinterpret_cast<void*>(gf.text_addr);
        if (!text || gf.want.empty()) continue;
        // Write the full text (core handles R7 compare-before-write)
        a->ui_set_text(cl(), text, gf.want.c_str());

        // If right-hand bar and ctrl exists, slide it left when suffix present
        // (the Lua's guide_shift logic). Not ported: the shift value is set by
        // the Lua and the right-hand ctrl position is managed by guide_fix_one
        // which the Lua still drives (the hook just writes the text).
    }
}

// ---- guide bar fix (the Lua's guide_bar_fix) -----------------------------
// Called from the UpdateInputGuide post-hook.

void bar_fix_inner(uint64_t param_addr) {
    auto* a = api();
    if (!a) return;

    std::string want, desc;
    {
        std::lock_guard lk(g_mx);
        if (g_bar_want.empty() || g_bar_param != param_addr) return;
        want = g_bar_want;
        desc = g_bar_desc;
    }

    // Resolve: param -> Agent -> ControlMain -> p_InputGuideList_0 -> c_guideBtn
    auto* param = reinterpret_cast<MO*>(param_addr);
    if (!is_alive(param)) return;

    MO* agent = ll::read_field_object(param, "<Agent>k__BackingField");
    if (!agent) return;
    auto* get_cm = ll::find_method_noargs(agent->get_type_definition(), "get_ControlMain");
    if (!get_cm) return;
    std::vector<void*> na;
    auto rc = get_cm->invoke(agent, na);
    if (rc.exception_thrown || !rc.ptr) return;
    void* cm = rc.ptr;

    // find p_InputGuideList_0 then c_guideBtn
    void* lst = a->ui_find_child(cm, "p_InputGuideList_0", 6);
    if (!lst) return;
    void* btn = a->ui_find_child(lst, "c_guideBtn", 4);
    if (!btn) return;

    // Iterate children of btn: hide e_text_0_* entries, find e_text_guide
    void* guide = nullptr;
    void* c = gui_child(btn);
    while (c) {
        std::string nm = gui_name(c);
        if (nm.size() >= 9 && nm.compare(0, 9, "e_text_0_") == 0) {
            // The game's "Esc Back": hidden while the editor is up
            if (gui_visible(c)) {
                a->ui_set_visible(cl(), c, 0);
            }
        } else if (nm == "e_text_guide") {
            guide = c;
        }
        c = gui_next(c);
    }

    // Write our hints text to e_text_guide
    if (guide) {
        std::string full = !desc.empty() ? (want + "\n" + desc) : want;
        // core handles R7 compare-before-write
        a->ui_set_text(cl(), guide, full.c_str());
    }
}

// ---- NewInputItem hook: lower/raise owned templates ----------------------
// PRE: for each owned template, if abs(y) > 0.5, save y and lower to 0.
// POST: restore saved_y on each.
// This ensures clones are always taken from a template at y=0.

void new_item_lower() {
    std::lock_guard lk(g_mx);
    if (g_corners.empty()) return;
    auto* a = api();
    if (!a) return;

    for (auto& [addr, e] : g_corners) {
        void* tmpl = reinterpret_cast<void*>(e.addr);
        if (!is_alive(tmpl)) continue;
        float tx = 0, ty = 0, tz = 0;
        if (gui_position(tmpl, tx, ty, tz) && std::abs(ty) > 0.5f) {
            e.saved_y = ty;
            e.has_saved_y = true;
            a->ui_set_position(cl(), tmpl, tx, 0.0f, tz);
        }
    }
}

void new_item_raise() {
    std::lock_guard lk(g_mx);
    if (g_corners.empty()) return;
    auto* a = api();
    if (!a) return;

    for (auto& [addr, e] : g_corners) {
        if (!e.has_saved_y) continue;
        float y = e.saved_y;
        e.has_saved_y = false;
        e.saved_y = 0;
        void* tmpl = reinterpret_cast<void*>(e.addr);
        if (!is_alive(tmpl)) continue;
        float tx = 0, ty = 0, tz = 0;
        if (gui_position(tmpl, tx, ty, tz)) {
            a->ui_set_position(cl(), tmpl, tx, y, tz);
        }
    }
}

// ---- a corner's writes (the Lua's guide_corner): text, position, ForceInvisible; g_mx held -----------------
void apply_corner(const CornerEntry& e) {
    auto* a = api();
    if (!a) return;
    void* tmpl = reinterpret_cast<void*>(e.addr);
    if (!is_alive(tmpl)) return;
    // Push children of btn to y=0 (except the template)
    if (is_alive(reinterpret_cast<void*>(e.btn_addr))) clamp_children(reinterpret_cast<void*>(e.btn_addr), e.addr);
    // Text (core handles R7 compare-before-write)
    a->ui_set_text(cl(), tmpl, e.text.c_str());
    // Position
    float tx = 0, ty = 0, tz = 0;
    if (gui_position(tmpl, tx, ty, tz) && (std::abs(ty - e.y) > 0.5f || std::abs(tx - e.x) > 0.5f))
        a->ui_set_position(cl(), tmpl, e.x, e.y, tz);
    // ForceInvisible
    const bool locked = gui_force_invisible(tmpl);
    if (e.show && locked) a->ui_set_force_invisible(cl(), tmpl, 0);
    else if (!e.show && !locked) a->ui_set_force_invisible(cl(), tmpl, 1);
}

// The Lua's OnLateUpdate post-hook ran guide_text_fix, which wrote the corners again every frame the guide list
// updated. Without it the corners were written only by the screens' pass (every 5 frames) and corner_tick, which
// puts back a corner not refreshed last frame, hid them in between: the "X Edit Drive Colors" prompt of the select
// screen blinked (Wael, 06/10). Same refresh here, with what the last pass decided.
void refresh_corners() {
    std::lock_guard lk(g_mx);
    for (auto& [addr, e] : g_corners) {
        e.last_frame = cmn::frame();
        apply_corner(e);
    }
}

// ---- hook functions (SEH-wrapped) ----------------------------------------

// thread-local for OnLateUpdate pre->post this address
thread_local uint64_t tl_part_addr = 0;
// thread-local for UpdateInputGuide pre->post this address
thread_local uint64_t tl_param_addr = 0;

#pragma warning(push)
#pragma warning(disable: 4733)

// NewInputItem PRE: lower all owned templates
int pre_new_item(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { new_item_lower(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("new_item_pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// NewInputItem POST: raise all owned templates
void post_new_item(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on) return;
    __try { new_item_raise(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("new_item_post"); }
}

// OnLateUpdate PRE: save this address
int pre_on_late_update(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_part_addr = reinterpret_cast<uint64_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// OnLateUpdate POST: apply guide text fix if the part matches
void post_on_late_update_inner() {
    uint64_t addr = tl_part_addr;
    if (!addr) return;
    bool active;
    {
        std::lock_guard lk(g_mx);
        if (g_fix_addrs.find(addr) == g_fix_addrs.end()) return;
        active = g_fsm_visible || g_tcs_guide;
    }
    if (!active) return;
    guide_fix_inner(addr);
    refresh_corners();
}

void post_on_late_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on) return;
    __try { post_on_late_update_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("on_late_update_post"); }
}

// UpdateInputGuide PRE: save this address
int pre_update_guide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_param_addr = reinterpret_cast<uint64_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// UpdateInputGuide POST: apply guide bar fix
void post_update_guide_inner() {
    uint64_t addr = tl_param_addr;
    if (!addr) return;
    std::string want;
    {
        std::lock_guard lk(g_mx);
        want = g_bar_want;
    }
    if (want.empty()) return;
    bar_fix_inner(addr);
}

void post_update_guide(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on) return;
    __try { post_update_guide_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("update_guide_post"); }
}

#pragma warning(pop)

}  // namespace

// ---- public API ----------------------------------------------------------

bool install() {
    auto* tdb = API::get()->tdb();
    if (!tdb) return false;
    if (!api()) {
        cmn::log_warn("[guide] core absent: off");
        return false;
    }

    int total = 0;

    // Hook UIPartsInputGuideBase.NewInputItem (all overloads)
    auto* guide_base_td = tdb->find_type("app.UIPartsInputGuideBase");
    if (guide_base_td) {
        for (auto* m : guide_base_td->get_methods()) {
            if (!m || !m->get_name()) continue;
            if (std::string_view{m->get_name()} == "NewInputItem") {
                if (m->get_function_raw() && m->add_hook(pre_new_item, post_new_item, false)) {
                    ++total;
                }
            }
        }
    }

    // Hook UIPartsInputGuideList.OnLateUpdate
    auto* guide_list_td = tdb->find_type("app.UIPartsInputGuideList");
    if (guide_list_td) {
        auto* m = guide_list_td->find_method("OnLateUpdate");
        if (m && m->get_function_raw() && m->add_hook(pre_on_late_update, post_on_late_update, false)) {
            ++total;
        }
    }

    // Hook UIFlowOptionBGDialog.Param.UpdateInputGuide
    auto* dialog_param_td = tdb->find_type("app.UIFlowOptionBGDialog.Param");
    if (dialog_param_td) {
        auto* m = dialog_param_td->find_method("UpdateInputGuide");
        if (m && m->get_function_raw() && m->add_hook(pre_update_guide, post_update_guide, false)) {
            ++total;
        }
    }

    g_on = total > 0;
    cmn::journal("[guide] %d hooks installed (NewInputItem + OnLateUpdate + UpdateInputGuide)", total);
    return g_on;
}

bool ready() { return g_on; }

void set_corner(uint64_t addr, uint64_t btn_addr, const std::string& text,
                float x, float y, bool show) {
    std::lock_guard lk(g_mx);
    auto& e = g_corners[addr];
    e.addr = addr;
    e.btn_addr = btn_addr;
    e.text = text;
    e.x = x;
    e.y = y;
    e.show = show;
    e.last_frame = cmn::frame();
    apply_corner(e);                     // the Lua's guide_corner writes, at once
}

void release_corner(uint64_t addr) {
    std::lock_guard lk(g_mx);
    g_corners.erase(addr);
}

void set_fix(uint64_t part_addr, uint64_t text_addr, uint64_t ctrl_addr,
             bool right, const std::string& want) {
    std::lock_guard lk(g_mx);
    // Update existing or add new
    for (auto& gf : g_fixes) {
        if (gf.part_addr == part_addr) {
            gf.text_addr = text_addr;
            gf.ctrl_addr = ctrl_addr;
            gf.right = right;
            gf.want = want;
            return;
        }
    }
    g_fixes.push_back({ part_addr, text_addr, ctrl_addr, right, want });
    g_fix_addrs.insert(part_addr);
}

void clear_fixes() {
    std::lock_guard lk(g_mx);
    g_fixes.clear();
    g_fix_addrs.clear();
}

namespace {
void bar_store(const std::string& want, const std::string& desc, uint64_t param_addr) {
    std::lock_guard lk(g_mx);
    g_bar_want = want;
    g_bar_desc = desc;
    g_bar_param = param_addr;
}
}  // namespace

void set_bar(const std::string& want, const std::string& desc,
             uint64_t param_addr) {
    bar_store(want, desc, param_addr);
    // The Lua applies it here too (guide_bar_fix(Opt.current_param) every 5 frames), not only from the
    // UpdateInputGuide hook: that hook does not fire when a page opens, and the game's own "Confirm / Back" stayed.
    if (!want.empty() && param_addr) {
        __try { bar_fix_inner(param_addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fault("set_bar"); }
    }
}

void set_flags(bool fsm_visible, bool tcs_guide) {
    std::lock_guard lk(g_mx);
    g_fsm_visible = fsm_visible;
    g_tcs_guide = tcs_guide;
}

void on_frame() {
    if (!g_on) return;
    __try { corner_tick_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("corner_tick"); }
}

std::string status() {
    std::lock_guard lk(g_mx);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "guide: on=%d faults=%d corners=%d fixes=%d bar=%s",
                  g_on ? 1 : 0, g_faults, (int)g_corners.size(),
                  (int)g_fixes.size(), g_bar_want.empty() ? "off" : "on");
    return buf;
}

}  // namespace screens::guide
