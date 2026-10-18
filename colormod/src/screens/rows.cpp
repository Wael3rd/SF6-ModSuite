// screens::rows - port of SF6_MenuRowsNative.cpp (creation) + DriveRows.lua (paint, input hijacks).
// See rows.hpp header comment and docs/CPP_PORT_PHASE3.md section 3a.
//
// Rules kept:
//   R1  all GUI writes from the game-thread post-hooks (Main.OnUpdate of the owning flow)
//   R2  proof-of-life: path re-followed from the live root (core ui_find_cached / ui_find_many)
//   R7  never set_Message with identical text (core ui_set_text compare-before-write)
//   R16 /EHa (SEH around every hook body)
//   R19 duplicate() only from Main.OnUpdate post-hook (same as SF6_MenuRowsNative)
//   R20 clone names p_<Class>_<Name>
//   R35 cloned rows are NOT managed objects: never is_managed_object
//   R37 discover through the core's cached registry, not full scans per frame

#include "rows.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ll_common.hpp"
#include "../common.hpp"
#include "../perf.hpp"
#include "../perf_hook.hpp"
#include "../vfx/drive_cfg.hpp"
#include "../vfx/feed.hpp"
#include "dipanel_mode.hpp"

namespace screens::rows {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

// ---- row definitions (same as SF6_MenuRowsNative.cpp kRows / kTargets) ------------------------------------------
struct RowDef { const char* ctrl; const char* title; bool hidden; };
struct Target { const char* agents[2]; const char* source; const RowDef* rows; size_t n; bool hidden; };

constexpr std::array<RowDef, 5> kRowsFS = { {
    { "p_Spin_drvimpact",   "Drive Impact",    false },
    { "p_Spin_drvreversal", "Drive Reversal",  false },
    { "p_Spin_drvparry",    "Drive Parry",     false },
    { "p_Spin_drvrush",     "Drive Rush",      false },
    { "p_Spin_drvspecial",  "Special Attacks",  true },
} };
constexpr std::array<RowDef, 1> kRowsCS = { {
    { "p_Spin_drvspecial", "Special Attacks", true },
} };
constexpr std::array<Target, 2> kTargets = { {
    { { "MatchingFighterSetting", nullptr }, "p_Spin_color", kRowsFS.data(), kRowsFS.size(), false },
    { { "ui10505", "ui10507" },             "p_Spin_preset", kRowsCS.data(), kRowsCS.size(), true  },
} };

// ---- state --------------------------------------------------------------------------------------------------------
bool g_on = false;                            // hooks installed and not faulted out
int  g_faults = 0;
constexpr int kMaxFaults = 5;
bool g_hooked = false;
bool g_gave_up = false;
bool g_suppressed = false;                    // F6: Lua suppress flag (editor transitions)
uint32_t g_tick = 0, g_retry = 0;
std::atomic<uint32_t> g_check{0};             // OnUpdate calls (two flows, two job threads)

// row creation tracking (same logic as SF6_MenuRowsNative)
std::vector<void*> g_failed;                  // agents on which Create returned null: never retry

// ---- paint state: what the Lua pushed (bridge, thread-safe) -------------------------------------------------------
struct RowDisplay {
    std::string title;
    std::string value;
    int64_t chip1 = -1, chip2 = -1;           // RGBA or -1 = no chips
    bool enabled = false;
    bool arrows = true;
    bool dirty = true;                         // set by push, cleared after paint
};
std::mutex g_mx;
std::array<RowDisplay, 5> g_display;          // [0..4] = impact, reversal, parry, rush, special

// ---- action queue: user actions captured by input hooks, pulled by the Lua ----------------------------------------
struct ActionEntry { int row; int dir; };     // dir: +1/-1/0(decide)
std::vector<ActionEntry> g_actions;

// ---- resolved row handles (re-resolved from the live agents) ------------------------------------------------------
struct ResolvedRow {
    void* ctrl = nullptr;           // the cloned control (via.gui)
    void* title = nullptr;          // e_text_title child
    void* text = nullptr;           // e_text child (value text)
    void* chip = nullptr;           // c_chip child (chip group)
    void* rect0 = nullptr;          // e_rect_color_0
    void* rect1 = nullptr;          // e_rect_color_1
    void* list = nullptr;           // p_TextList_color_h (the text holder for positioning)
    void* arrow_l = nullptr;        // p_MouseDecideOperable_L
    void* arrow_r = nullptr;        // p_MouseDecideOperable_R
    void* part = nullptr;           // UIPartsSpin part for the row
};
struct ScreenState {
    void* agent = nullptr;
    uint32_t agent_gen = 0;         // core agent generation for MatchingFighterSetting
    std::array<ResolvedRow, 5> rows;
    // reference row layout (Costume row's position and region, for value text placement)
    bool has_ref = false;
    float ref_pos_x = 0, ref_pos_y = 0, ref_pos_z = 0;
    float ref_region_w = 0, ref_region_h = 0;
    uint64_t resolved_at = 0;
    // titles: the message link is cut at creation and at each focus change (the game reposes it then), DriveRows.lua
    bool need_unbind = true;
    std::string last_focus;
};
ScreenState g_screen;

// ---- spin address -> row index map (for input hook filtering) -----------------------------------------------------
std::unordered_map<uint64_t, int> g_by_spin;  // [UIPartsSpin address] = row index (0..4)
std::unordered_map<uint64_t, bool> g_by_agent; // [agent address] = true

// ---- core helpers --------------------------------------------------------------------------------------------------
const SF6CoreAPI* api() { return cmn::core(); }
SF6CoreClient* cl() { return cmn::client(); }

// ---- LL helpers (duplicated from SF6_MenuRowsNative: method, call, obj, field, ...) --------------------------------
// These are used for row creation only (called from the game's Main.OnUpdate post-hook).
MO* obj(MO* o, const char* sig, std::vector<void*> args = {}) {
    return reinterpret_cast<MO*>(ll::invoke_ret(o, ll::find_method_full(o->get_type_definition(), sig), std::move(args)).ptr);
}
MO* field(MO* o, const char* name) { return ll::read_field_object(o, name); }
MO* control(MO* o) { return field(o, "<Control>k__BackingField"); }
int number(MO* o, const char* sig) {
    auto* m = ll::find_method_full(o->get_type_definition(), sig);
    if (!m) return 0;
    std::vector<void*> noargs;
    auto r = m->invoke(o, noargs);
    return r.exception_thrown ? 0 : (int)r.dword;
}
reframework::InvokeRet call_ret(MO* o, const char* sig, std::vector<void*> args = {}) {
    auto* m = ll::find_method_full(o->get_type_definition(), sig);
    if (!m) { reframework::InvokeRet r{}; r.exception_thrown = true; return r; }
    std::vector<void*> a = std::move(args);
    return m->invoke(o, a);
}
std::string name_of(MO* o) {
    if (!o) return "(null)";
    return ll::managed_string_to_utf8(obj(o, "get_Name()"));
}
MO* child_by_name(MO* o, const char* name) {
    return obj(o, "getObject(System.String)", { ll::create_utf8_string(name) });
}

// ---- agent discovery (through the core's registry, R37) -----------------------------------------------------------
std::vector<MO*> find_agents(const Target& t) {
    std::vector<MO*> out;
    for (auto* want : t.agents) {
        if (!want) continue;
        void* buf[8] = {};
        uint32_t n = api()->ui_agents(want, buf, 8);
        for (uint32_t i = 0; i < n; ++i)
            if (buf[i]) out.push_back(reinterpret_cast<MO*>(buf[i]));
    }
    return out;
}

// ---- part tree search (UIPartsSpin by name, used for by_spin mapping and row creation) ----------------------------
MO* find_part(MO* pm, const char* ctrl_name, int depth) {
    if (!pm || depth > 6) return nullptr;
    auto* list = field(pm, "_List");
    if (!list) return nullptr;
    const int n = number(list, "get_Count()");
    for (int j = 0; j < n; ++j) {
        auto* part_ret = ll::find_method_full(list->get_type_definition(), "get_Item(System.Int32)");
        if (!part_ret) break;
        auto ri = part_ret->invoke(list, { ll::arg_i32(j) });
        if (ri.exception_thrown || !ri.ptr) continue;
        auto* part = reinterpret_cast<MO*>(ri.ptr);
        auto* ctrl_obj = control(part);
        if (ctrl_obj && name_of(ctrl_obj) == ctrl_name) return part;
        auto* child_mgr = field(part, "<ChildManager>k__BackingField");
        if (auto* r = find_part(child_mgr, ctrl_name, depth + 1)) return r;
    }
    return nullptr;
}

// ---- row creation via the core (same as SF6_MenuRowsNative create_rows_core) ------------------------------------
void create_rows_core(const Target& t, MO* agent, MO* source) {
    auto* src = control(source);
    auto* parent = obj(src, "get_Parent()");
    auto* manager = obj(source, "get_Manager()");
    auto* group = field(manager, "<OwnerParts>k__BackingField");
    auto* children = group ? field(group, "_Children") : nullptr;
    if (!children) throw std::runtime_error("core: pas de _Children sur le groupe proprietaire");
    const int count = number(children, "get_Count()");
    if (count < 2) throw std::runtime_error("core: groupe trop court");
    auto p_last = call_ret(control(reinterpret_cast<MO*>(
        ll::find_method_full(children->get_type_definition(), "get_Item(System.Int32)")->invoke(children, { ll::arg_i32(count - 1) }).ptr
    )), "get_Position()");
    auto p_prev = call_ret(control(reinterpret_cast<MO*>(
        ll::find_method_full(children->get_type_definition(), "get_Item(System.Int32)")->invoke(children, { ll::arg_i32(count - 2) }).ptr
    )), "get_Position()");
    float y_last = 0, y_prev = 0;
    std::memcpy(&y_last, p_last.bytes.data() + 4, 4);
    std::memcpy(&y_prev, p_prev.bytes.data() + 4, 4);
    const float step = y_last - y_prev;
    int made = 0;
    for (size_t k = 0; k < t.n; ++k) {
        const auto& def = t.rows[k];
        // a hidden row is not created at all (SF6_MenuRowsNative, 08/09): created hidden and disabled, it stayed in the
        // navigation and the cursor fell on a ghost row under Drive Rush (Wael). Lost in the phase 3 port, back 06/10.
        if (t.hidden || def.hidden) continue;
        const float y = y_last + step * static_cast<float>(k + 1);
        const bool hid = t.hidden || def.hidden;
        float x_last = 0, z_last = 0;
        std::memcpy(&x_last, p_last.bytes.data(), 4);
        std::memcpy(&z_last, p_last.bytes.data() + 8, 4);
        SF6CoreRowCloneDesc desc{};
        desc.struct_size = sizeof(SF6CoreRowCloneDesc);
        desc.template_ctrl = (void*)src;
        desc.parent_ctrl = (void*)parent;
        desc.template_part = (void*)source;
        desc.new_name = def.ctrl;
        desc.pos_x = x_last; desc.pos_y = y; desc.pos_z = z_last;
        desc.visible = hid ? 0 : 1;
        desc.disabled = hid ? 1 : 0;
        desc.title_text = def.title;
        void* out_ctrl = nullptr;
        SF6CoreStatus st = api()->ui_clone_row(cl(), &desc, &out_ctrl);
        if (st != SF6CORE_OK) {
            cmn::log_warn("[rows] core ui_clone_row failed for %s (status %d)", def.ctrl, (int)st);
            throw std::runtime_error(std::string("core ui_clone_row failed for ") + def.ctrl);
        }
        ++made;
        cmn::journal("[rows] %s : core clone %p y %.0f", def.ctrl, out_ctrl, y);
    }
    cmn::journal("[rows] %d lignes creees (core) sur l agent %p (pas %.0f)", made, (void*)agent, step);
}

// ---- ensure rows exist (called from Main.OnUpdate post-hook, like SF6_MenuRowsNative) ----------------------------
void ensure_rows() {
    for (const auto& t : kTargets) {
        if (t.hidden) continue;
        for (auto* agent : find_agents(t)) {
            if (std::find(g_failed.begin(), g_failed.end(), (void*)agent) != g_failed.end()) continue;
            auto* pm = field(agent, "_PartsManager");
            if (find_part(pm, t.rows[0].ctrl, 0)) continue;     // already there
            auto* source = find_part(pm, t.source, 0);
            if (!source) continue;                               // screen not built yet
            try { create_rows_core(t, agent, source); }
            catch (const std::exception& e) {
                g_failed.push_back(agent);
                cmn::log_warn("[rows] creation abandonnee sur l agent %p : %s", (void*)agent, e.what());
            }
        }
    }
}

// ---- resolve rows + reference layout (the core's ui_find_many, one walk per screen) --------------------------------
bool resolve_screen() {
    void* agents[4] = {};
    uint32_t n = api()->ui_agents("MatchingFighterSetting", agents, 4);
    // F4: discard actions captured on the previous screen / agent — they must never apply to a new one. Only then:
    // this also runs every 10 frames on the same screen, and a press recorded just before such a pass (not pulled
    // yet, the host pulls every 3 frames) was thrown away (lost left / right presses on the Drive rows, 2026-10-06)
    if (n == 0 || agents[0] != g_screen.agent) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_actions.clear();
    }
    if (n == 0) { g_screen.agent = nullptr; return false; }
    auto* agent = reinterpret_cast<MO*>(agents[0]);
    g_screen.agent = (void*)agent;
    g_screen.agent_gen = api()->ui_agents_gen("MatchingFighterSetting");

    // get ControlMain
    auto* cm_m = ll::find_method_full(agent->get_type_definition(), "get_ControlMain()");
    if (!cm_m) return false;
    std::vector<void*> noargs;
    auto cm_r = cm_m->invoke(agent, noargs);
    if (cm_r.exception_thrown || !cm_r.ptr) return false;
    void* cm = cm_r.ptr;

    // one walk for all the names we need (R37: core ui_find_many records paths)
    const char* names[] = {
        "p_Spin_drvimpact", "p_Spin_drvreversal", "p_Spin_drvparry", "p_Spin_drvrush", "p_Spin_drvspecial",
        "p_Spin_costume", "p_Spin_color"
    };
    constexpr uint32_t N = 7;
    void* found[N] = {};
    api()->ui_find_many(cm, names, N, found);

    // check: all four required rows must exist (special is optional, index 4)
    for (int i = 0; i < 4; ++i) {
        if (!found[i]) { g_screen.agent = nullptr; return false; }
    }

    // for each row, find children through the core's cached lookup
    auto resolve_row = [&](int idx, void* ctrl) -> ResolvedRow {
        ResolvedRow r;
        r.ctrl = ctrl;
        if (!ctrl) return r;
        // children
        const char* child_names[] = {
            "e_text_title", "e_text", "c_chip", "e_rect_color_0", "e_rect_color_1",
            "p_TextList_color_h", "p_MouseDecideOperable_L", "p_MouseDecideOperable_R"
        };
        void* children_found[8] = {};
        api()->ui_find_many(ctrl, child_names, 8, children_found);
        r.title = children_found[0];
        r.text = children_found[1];
        r.chip = children_found[2];
        r.rect0 = children_found[3];
        r.rect1 = children_found[4];
        r.list = children_found[5];
        r.arrow_l = children_found[6];
        r.arrow_r = children_found[7];
        return r;
    };

    for (int i = 0; i < 5; ++i) {
        g_screen.rows[i] = resolve_row(i, found[i]);
    }

    // find parts for by_spin mapping: walk the parts tree of the agent
    auto* pm = field(agent, "_PartsManager");
    g_by_spin.clear();
    g_by_agent.clear();
    if (pm) {
        const char* part_names[] = { "p_Spin_drvimpact", "p_Spin_drvreversal", "p_Spin_drvparry", "p_Spin_drvrush", "p_Spin_drvspecial" };
        for (int i = 0; i < 5; ++i) {
            auto* p = find_part(pm, part_names[i], 0);
            g_screen.rows[i].part = (void*)p;
            if (p) g_by_spin[(uint64_t)p] = i;
        }
    }
    g_by_agent[(uint64_t)agent] = true;

    // reference row layout: Costume row's position and region
    g_screen.has_ref = false;
    void* costume_ctrl = found[5];   // p_Spin_costume
    if (costume_ctrl) {
        const char* ref_names[] = { "p_TextList_type_h", "e_text" };
        void* ref_found[2] = {};
        api()->ui_find_many(costume_ctrl, ref_names, 2, ref_found);
        if (ref_found[0] && ref_found[1]) {
            // position of the text list
            auto* pos_m = ll::find_method_full(reinterpret_cast<MO*>(ref_found[0])->get_type_definition(), "get_Position()");
            if (pos_m) {
                std::vector<void*> na;
                auto pr = pos_m->invoke(reinterpret_cast<MO*>(ref_found[0]), na);
                if (!pr.exception_thrown) {
                    std::memcpy(&g_screen.ref_pos_x, pr.bytes.data(), 4);
                    std::memcpy(&g_screen.ref_pos_y, pr.bytes.data() + 4, 4);
                    std::memcpy(&g_screen.ref_pos_z, pr.bytes.data() + 8, 4);
                    g_screen.has_ref = true;
                }
            }
            // region size of the text
            auto* reg_m = ll::find_method_full(reinterpret_cast<MO*>(ref_found[1])->get_type_definition(), "get_RegionSize()");
            if (reg_m) {
                std::vector<void*> na2;
                auto rr = reg_m->invoke(reinterpret_cast<MO*>(ref_found[1]), na2);
                if (!rr.exception_thrown) {
                    std::memcpy(&g_screen.ref_region_w, rr.bytes.data(), 4);
                    std::memcpy(&g_screen.ref_region_h, rr.bytes.data() + 4, 4);
                }
            }
        }
    }

    g_screen.resolved_at = cmn::frame();
    g_screen.need_unbind = true;            // fresh rows: their titles still carry the Color row's message link
    return true;
}

// ---- unbind title message GUID (same as DriveRows.lua unbind_title) ------------------------------------------------
// The clone keeps the message link from the Color row; the game reposes it on focus change.
// Setting the GUID to Empty breaks the link so our text stays.
void unbind_title(void* text_ctrl) {
    if (!text_ctrl) return;
    // set the message GUID to System.Guid.Empty via the core
    uint8_t empty_guid[16] = {};
    api()->ui_set_message_id(cl(), text_ctrl, empty_guid);
}

// ---- paint one row (core display API, all writes guarded) ----------------------------------------------------------
void paint_row(int idx, bool unbind) {
    if (idx < 0 || idx >= 5) return;
    auto& rr = g_screen.rows[idx];
    if (!rr.ctrl) return;

    RowDisplay disp;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        disp = g_display[idx];
        g_display[idx].dirty = false;
    }

    // title. Cutting the link blanks the display but get_Message still returns our text, so the compare-before-write
    // (R7) would skip the rewrite: write a variant first (one more space), then the real text (DriveRows.lua unbind_title)
    if (rr.title && unbind) {
        unbind_title(rr.title);
        api()->ui_set_text(cl(), rr.title, (disp.title + " ").c_str());
    }
    if (rr.title) api()->ui_set_text(cl(), rr.title, disp.title.c_str());

    // value text
    if (rr.text) api()->ui_set_text(cl(), rr.text, disp.value.c_str());

    // chips
    bool has_chips = disp.chip1 >= 0 && rr.rect0 && rr.rect1;
    if (rr.chip) api()->ui_set_visible(cl(), rr.chip, has_chips ? 1 : 0);
    if (has_chips) {
        api()->ui_set_color(cl(), rr.rect0, (uint32_t)disp.chip1);
        api()->ui_set_color(cl(), rr.rect1, (uint32_t)disp.chip2);
    }

    // value text position (same algorithm as DriveRows.lua place_value)
    if (g_screen.has_ref && rr.list && rr.text) {
        float want_x = g_screen.ref_pos_x;
        if (has_chips) {
            // measure displayed text width
            auto* dsm = ll::find_method_full(reinterpret_cast<MO*>(rr.text)->get_type_definition(), "get_DisplaySize()");
            float w = 45.0f;
            if (dsm) {
                std::vector<void*> na3;
                auto dr = dsm->invoke(reinterpret_cast<MO*>(rr.text), na3);
                if (!dr.exception_thrown) {
                    float dw = 0;
                    std::memcpy(&dw, dr.bytes.data(), 4);
                    if (dw > 1.0f) w = dw;
                }
            }
            want_x = g_screen.ref_pos_x + (401.0f - 412.0f) - w / 2.0f;
        }
        // read current position, only update if different
        auto* pos_m = ll::find_method_full(reinterpret_cast<MO*>(rr.list)->get_type_definition(), "get_Position()");
        if (pos_m) {
            std::vector<void*> na4;
            auto pr = pos_m->invoke(reinterpret_cast<MO*>(rr.list), na4);
            if (!pr.exception_thrown) {
                float cur_x = 0;
                std::memcpy(&cur_x, pr.bytes.data(), 4);
                if (std::abs(cur_x - want_x) > 1.0f) {
                    api()->ui_set_position(cl(), rr.list, want_x, g_screen.ref_pos_y, g_screen.ref_pos_z);
                }
            }
        }
        // region size: always Costume's region
        auto* reg_m = ll::find_method_full(reinterpret_cast<MO*>(rr.text)->get_type_definition(), "get_RegionSize()");
        if (reg_m) {
            std::vector<void*> na5;
            auto rg = reg_m->invoke(reinterpret_cast<MO*>(rr.text), na5);
            if (!rg.exception_thrown) {
                float cur_w = 0;
                std::memcpy(&cur_w, rg.bytes.data(), 4);
                if (std::abs(cur_w - g_screen.ref_region_w) > 0.5f)
                    api()->ui_set_region_size(cl(), rr.text, g_screen.ref_region_w, g_screen.ref_region_h);
            }
        }
    }

    // arrows: the game hides them after setup, lift ForceInvisible (same as DriveRows.lua force_vis)
    if (disp.arrows) {
        if (rr.arrow_l) { api()->ui_set_force_invisible(cl(), rr.arrow_l, 0); api()->ui_set_visible(cl(), rr.arrow_l, 1); }
        if (rr.arrow_r) { api()->ui_set_force_invisible(cl(), rr.arrow_r, 0); api()->ui_set_visible(cl(), rr.arrow_r, 1); }
    }
}

// ---- paint all rows -------------------------------------------------------------------------------------------------
std::string focused_name();
void paint_all() {
    const std::string nm = focused_name();
    const bool unbind = g_screen.need_unbind || nm != g_screen.last_focus;
    if (unbind) { g_screen.need_unbind = false; g_screen.last_focus = nm; }
    for (int i = 0; i < 5; ++i) {
        if (g_screen.rows[i].ctrl) paint_row(i, unbind);
    }
}

// ---- fault handling (same pattern as vfx::body) -------------------------------------------------------------------
void fault(const char* where) {
    ++g_faults;
    cmn::journal("[rows] fault caught in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("[rows] too many faults (%d), off until game restart", g_faults);
    }
}

// ---- hooks: Main.OnUpdate post for row creation and painting ------------------------------------------------------

int update_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

#pragma warning(push)
#pragma warning(disable: 4733)
void post_body() {
    {
        ensure_rows();
        // periodic resolve + paint (every 10 post-hooks that fire, same as DriveRows.lua frame % 10)
        uint32_t gen = api()->ui_agents_gen("MatchingFighterSetting");
        bool need_resolve = g_screen.agent == nullptr
                         || gen != g_screen.agent_gen
                         || (cmn::frame() - g_screen.resolved_at) > 10;
        if (need_resolve) {
            if (!resolve_screen()) return;
        }
        if (g_suppressed) return;              // F6: skip paint while Lua says the screen is transitioning
        paint_all();
    }
}
// Battle Hub runs the OnUpdate of MatchingSetting and of MatchingFighterSetting at the same time, on two job threads:
// both created the Drive rows (two Drive Impact rows on top of each other, the second Drive Reversal refused, 18/10).
// One pass at a time; a call that finds another one running skips (the next one comes 10 calls later).
std::atomic<bool> g_in_post{false};
void update_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing() || !g_on) return;
    if ((g_check.fetch_add(1, std::memory_order_relaxed) + 1) % 10 != 0) return;   // check every 10 calls, same as SF6_MenuRowsNative
    if (g_in_post.exchange(true, std::memory_order_acquire)) return;
    __try { post_body(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("update_post"); }
    g_in_post.store(false, std::memory_order_release);
}
#pragma warning(pop)

bool hook_main(const char* type_name) {
    auto* type = API::get()->tdb()->find_type(type_name);
    auto* m = type ? type->find_method("OnUpdate()") : nullptr;
    if (!m) { cmn::journal("[rows] %s.OnUpdate() introuvable", type_name); return false; }
    if (!m->add_hook(perf::timed_pre<perf::HkRowsOnUpdate, update_pre>, perf::timed_post<perf::HkRowsOnUpdate, update_post>, false)) { cmn::journal("[rows] hook %s.OnUpdate() refuse", type_name); return false; }
    perf::journal_hooked(type_name, m);
    return true;
}

// ---- input hooks: UIPartsSpin.Add/Sub/... filtered by the spin's address ------------------------------------------
// When the spin is one of ours, capture the action for the Lua to pull, and SKIP_ORIGINAL.

void record_action(int row, int dir) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_actions.push_back({ row, dir });
}

int spin_which(void** args) {
    if (!g_on) return -1;
    auto ok = reinterpret_cast<uint64_t>(args[1]);
    auto it = g_by_spin.find(ok);
    return (it != g_by_spin.end()) ? it->second : -1;
}

// step debounce: same 12-frame hold as DriveRows.lua
uint64_t g_last_step_frame = 0;
bool debounce_step() {
    uint64_t f = cmn::frame();
    if (g_last_step_frame && (f - g_last_step_frame) < 12) return true;
    g_last_step_frame = f;
    return false;
}

#pragma warning(push)
#pragma warning(disable: 4733)
int pre_spin_add(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = spin_which(argv);
        if (w >= 0) {
            if (!debounce_step()) record_action(w, 1);
            return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_spin_add"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_spin_sub(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = spin_which(argv);
        if (w >= 0) {
            if (!debounce_step()) record_action(w, -1);
            return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_spin_sub"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_spin_dir(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 3) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = spin_which(argv);
        if (w < 0) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        uint32_t dir = (uint32_t)(uint64_t)argv[2];
        if (dir & 4) { if (!debounce_step()) record_action(w, -1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
        if (dir & 8) { if (!debounce_step()) record_action(w, 1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_spin_dir"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
#pragma warning(pop)
void post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- input hooks: UIAgent.InputLeft/Right/Direction/Decide filtered by the agent + focused row --------------------

// the name of the agent's focused control, whichever it is (DriveRows.lua focused_row_name)
std::string focused_name() {
    if (!g_screen.agent) return {};
    auto* agent = reinterpret_cast<MO*>(g_screen.agent);
    auto* m = ll::find_method_full(agent->get_type_definition(), "GetFocusItem()");
    if (!m) return {};
    std::vector<void*> noargs;
    auto r = m->invoke(agent, noargs);
    if (r.exception_thrown || !r.ptr) return {};
    auto* ctrl_obj = field(reinterpret_cast<MO*>(r.ptr), "<Control>k__BackingField");
    return ctrl_obj ? name_of(ctrl_obj) : std::string();
}

int focused_row() {
    // which of our rows is focused? Ask the agent for its focused item name
    if (!g_screen.agent) return -1;
    auto* agent = reinterpret_cast<MO*>(g_screen.agent);
    auto* m = ll::find_method_full(agent->get_type_definition(), "GetFocusItem()");
    if (!m) return -1;
    std::vector<void*> noargs2;
    auto r = m->invoke(agent, noargs2);
    if (r.exception_thrown || !r.ptr) return -1;
    auto* item = reinterpret_cast<MO*>(r.ptr);
    auto* ctrl_obj = field(item, "<Control>k__BackingField");
    if (!ctrl_obj) return -1;
    std::string nm = name_of(ctrl_obj);
    for (int i = 0; i < 5; ++i) {
        if (nm == kRowsFS[i].ctrl) return i;
    }
    return -1;
}

int agent_ours(void** args) {
    if (!g_on) return -1;
    auto addr = (uint64_t)args[1];
    if (g_by_agent.find(addr) == g_by_agent.end()) return -1;
    return focused_row();
}

#pragma warning(push)
#pragma warning(disable: 4733)
int pre_agent_left(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = agent_ours(argv);
        if (w >= 0) { if (!debounce_step()) record_action(w, -1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_agent_left"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_agent_right(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = agent_ours(argv);
        if (w >= 0) { if (!debounce_step()) record_action(w, 1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_agent_right"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_agent_dir(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 3) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = agent_ours(argv);
        if (w < 0) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        uint32_t dir = (uint32_t)(uint64_t)argv[2];
        if (dir & 4) { if (!debounce_step()) record_action(w, -1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
        if (dir & 8) { if (!debounce_step()) record_action(w, 1); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_agent_dir"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_agent_decide(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !g_on || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try {
        int w = agent_ours(argv);
        if (w >= 0) { record_action(w, 0); return REFRAMEWORK_HOOK_SKIP_ORIGINAL; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { fault("pre_agent_decide"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
#pragma warning(pop)

// ---- hook installation: same methods as DriveRows.lua, native hooks filtered by address ---------------------------
bool install_input_hooks() {
    auto* spin_td = API::get()->tdb()->find_type("app.UIPartsSpin");
    if (!spin_td) { cmn::journal("[rows] app.UIPartsSpin not found"); return false; }
    int n_spin = 0;
    // hook Add, Sub, AddNum, SubNum, InputRight, InputLeft, MouseAdd, MouseSub -> pre_spin_add/sub
    // and InputDirection -> pre_spin_dir
    struct SpinHook { const char* name; decltype(&pre_spin_add) fn; };
    SpinHook spin_hooks[] = {
        { "Add",       perf::timed_pre<perf::HkRowsSpinAdd, pre_spin_add> }, { "AddNum",    perf::timed_pre<perf::HkRowsSpinAddNum, pre_spin_add> },
        { "InputRight", perf::timed_pre<perf::HkRowsSpinInputRight, pre_spin_add> }, { "MouseAdd",  perf::timed_pre<perf::HkRowsSpinMouseAdd, pre_spin_add> },
        { "Sub",       perf::timed_pre<perf::HkRowsSpinSub, pre_spin_sub> }, { "SubNum",    perf::timed_pre<perf::HkRowsSpinSubNum, pre_spin_sub> },
        { "InputLeft", perf::timed_pre<perf::HkRowsSpinInputLeft, pre_spin_sub> }, { "MouseSub",  perf::timed_pre<perf::HkRowsSpinMouseSub, pre_spin_sub> },
    };
    for (auto& sh : spin_hooks) {
        auto* m = spin_td->find_method(sh.name);
        if (m && m->get_function_raw() && m->add_hook(sh.fn, post_nop, false)) { ++n_spin; perf::journal_hooked(sh.name, m); }
    }
    auto* sd = spin_td->find_method("InputDirection");
    if (sd && sd->get_function_raw() && sd->add_hook(perf::timed_pre<perf::HkRowsSpinInputDirection, pre_spin_dir>, post_nop, false)) { ++n_spin; perf::journal_hooked("UIPartsSpin.InputDirection", sd); }

    // UIAgent hooks
    auto* agent_td = API::get()->tdb()->find_type("app.UIAgent");
    int n_agent = 0;
    if (agent_td) {
        struct AgentHook { const char* name; decltype(&pre_agent_left) fn; };
        AgentHook agent_hooks[] = {
            { "InputLeft",      perf::timed_pre<perf::HkRowsAgentInputLeft, pre_agent_left> },
            { "InputRight",     perf::timed_pre<perf::HkRowsAgentInputRight, pre_agent_right> },
        };
        for (auto& ah : agent_hooks) {
            auto* m = agent_td->find_method(ah.name);
            if (m && m->get_function_raw() && m->add_hook(ah.fn, post_nop, false)) { ++n_agent; perf::journal_hooked(ah.name, m); }
        }
        auto* md = agent_td->find_method("InputDirection");
        if (md && md->get_function_raw() && md->add_hook(perf::timed_pre<perf::HkRowsAgentInputDirection, pre_agent_dir>, post_nop, false)) { ++n_agent; perf::journal_hooked("UIAgent.InputDirection", md); }
        auto* decide = agent_td->find_method("InputDecide");
        if (decide && decide->get_function_raw() && decide->add_hook(perf::timed_pre<perf::HkRowsAgentInputDecide, pre_agent_decide>, post_nop, false)) { ++n_agent; perf::journal_hooked("UIAgent.InputDecide", decide); }
    }
    cmn::journal("[rows] input hooks: spin x%d, agent x%d", n_spin, n_agent);
    return n_spin > 0;
}

}  // namespace

// ---- public API ----------------------------------------------------------------------------------------------------

bool install() {
    // refuse if SF6_MenuRowsNative.dll is loaded (same logic as vfx_engine / SF6_MemWriterNative)
    if (GetModuleHandleA("SF6_MenuRowsNative.dll")) {
        cmn::log_warn("[rows] SF6_MenuRowsNative.dll is loaded: screens_rows stays lua (rename to .dll.off for native)");
        return false;
    }
    if (!api() || !SF6CORE_HAS(api(), ui_clone_row) || !SF6CORE_HAS(api(), ui_find_many)) {
        cmn::log_warn("[rows] core without the required APIs: off");
        return false;
    }
    // hook Main.OnUpdate of the three flows (row creation + paint)
    const bool ms = hook_main("app.UIFlowMatchingSetting.Main");
    const bool fs = hook_main("app.UIFlowMatchingFighterSetting.Main");
    const bool cs = hook_main("app.UIFlowUI10505.CharaSetting");
    if (!ms && !fs && !cs) {
        cmn::log_warn("[rows] no flow hook installed: off");
        return false;
    }
    // input hooks (UIPartsSpin + UIAgent methods)
    install_input_hooks();

    g_on = true;
    cmn::journal("[rows] installed (MatchingSetting %d, FighterSetting %d, CharaSetting %d)", ms, fs, cs);
    return true;
}

bool ready() { return g_on; }

std::string status() {
    char buf[256];
    std::snprintf(buf, sizeof buf, "on=%d faults=%d by_spin=%d by_agent=%d resolved=%llu",
                  g_on ? 1 : 0, g_faults, (int)g_by_spin.size(), (int)g_by_agent.size(),
                  (unsigned long long)g_screen.resolved_at);
    return buf;
}

void push_row(int row, const char* title, const char* value,
              int64_t chip1, int64_t chip2, bool enabled, bool arrows) {
    if (row < 0 || row >= 5) return;
    std::lock_guard<std::mutex> lk(g_mx);
    auto& d = g_display[row];
    d.title = title ? title : "";
    d.value = value ? value : "";
    d.chip1 = chip1;
    d.chip2 = chip2;
    d.enabled = enabled;
    d.arrows = arrows;
    d.dirty = true;
}

int pull_actions(RowAction* out, int max) {
    std::lock_guard<std::mutex> lk(g_mx);
    int n = std::min((int)g_actions.size(), max);
    for (int i = 0; i < n; ++i) {
        out[i].row = g_actions[i].row;
        out[i].dir = g_actions[i].dir;
    }
    g_actions.clear();
    return n;
}

void set_suppress(bool on) {
    g_suppressed = on;
}

// ---- the model (phase 5c-1): func/DriveRows.lua M.tick, native path -------------------------------------------------
namespace {
namespace dc = vfx::drive_cfg;
const char* const kMechName[5] = { "impact", "reversal", "parry", "rush", "special" };
struct Model {
    bool on = false;
    uint64_t agent = 0;
    int fighter = 0, costume = 0, side = 1;
    uint64_t resolved_at = 0;
    bool suppress = false, was_suppressed = false;
    bool has[5] = {};
    std::vector<dc::Item> items[5];
    bool dirty = false;
    uint64_t dirty_at = 0, last_step = 0;
    bool has_last_step = false;
    void* color_list = nullptr; void* color_text = nullptr;      // the game's Color row (place_value)
    bool has_ref = false;
    float ref_x = 0, ref_y = 0, ref_z = 0, ref_w = 0, ref_h = 0;
};
Model g_m;
std::string g_focused, g_decide;

bool get_floats(void* o, const char* sig, float* out, int n) {
    if (!o) return false;
    auto* m = ll::find_method_full(reinterpret_cast<MO*>(o)->get_type_definition(), sig);
    if (!m) return false;
    std::vector<void*> na;
    auto r = m->invoke(reinterpret_cast<MO*>(o), na);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), 4 * (size_t)n);
    return true;
}

// place_value(list, text, chips): the value text's centre and its fold width (Costume's region)
void place_value(void* list, void* text, bool chips) {
    if (!(list && text && g_m.has_ref)) return;
    float cur[3] = {};
    const bool okp = get_floats(list, "get_Position()", cur, 3);
    float want_x = g_m.ref_x;
    if (chips) {
        float ds[2] = {};
        const float w = (get_floats(text, "get_DisplaySize()", ds, 2) && ds[0] > 1.0f) ? ds[0] : 45.0f;
        want_x = g_m.ref_x + (401.0f - 412.0f) - w / 2.0f;
    }
    if (okp && std::abs(cur[0] - want_x) > 1.0f) api()->ui_set_position(cl(), list, want_x, g_m.ref_y, g_m.ref_z);
    float reg[2] = {};
    if (get_floats(text, "get_RegionSize()", reg, 2) && std::abs(reg[0] - g_m.ref_w) > 0.5f)
        api()->ui_set_region_size(cl(), text, g_m.ref_w, g_m.ref_h);
}
bool di_mode_on() { return dipanel::mode::active(1) || dipanel::mode::active(2); }
std::string focused_of(MO* agent) {                               // focused_row_name(agent) on the host's agent
    auto* m = ll::find_method_full(agent->get_type_definition(), "GetFocusItem()");
    if (!m) return {};
    std::vector<void*> na;
    auto r = m->invoke(agent, na);
    if (r.exception_thrown || !r.ptr) return {};
    auto* ctrl_obj = field(reinterpret_cast<MO*>(r.ptr), "<Control>k__BackingField");
    return ctrl_obj ? name_of(ctrl_obj) : std::string();
}
void color_placement() {                                          // paint()'s native half: the game's Color row
    if (g_m.suppress) return;
    if (g_m.color_list && !di_mode_on()) place_value(g_m.color_list, g_m.color_text, true);
}

void resolve_color_row(uint64_t agent) {                          // F2: the Color row + the reference layout (Costume)
    g_m.color_list = g_m.color_text = nullptr;
    g_m.has_ref = false;
    auto* a = reinterpret_cast<MO*>(agent);
    auto* cm_m = ll::find_method_full(a->get_type_definition(), "get_ControlMain()");
    if (!cm_m) return;
    std::vector<void*> na;
    auto cr = cm_m->invoke(a, na);
    if (cr.exception_thrown || !cr.ptr) return;
    void* cm = cr.ptr;
    void* color = api()->ui_find_cached(cm, "p_Spin_color");
    void* cost = api()->ui_find_cached(cm, "p_Spin_costume");
    if (color) {
        const char* names[] = { "p_TextList_color_h", "e_text" };
        void* f[2] = {};
        api()->ui_find_many(color, names, 2, f);
        g_m.color_list = f[0]; g_m.color_text = f[1];
    }
    if (cost) {
        const char* names[] = { "p_TextList_type_h", "e_text" };
        void* f[2] = {};
        api()->ui_find_many(cost, names, 2, f);
        float pos[3] = {}, reg[2] = {};
        if (f[0] && get_floats(f[0], "get_Position()", pos, 3)) { g_m.ref_x = pos[0]; g_m.ref_y = pos[1]; g_m.ref_z = pos[2]; g_m.has_ref = true; }
        if (f[1] && get_floats(f[1], "get_RegionSize()", reg, 2)) { g_m.ref_w = reg[0]; g_m.ref_h = reg[1]; }
    }
}

void save_now() {
    dc::save_di(g_m.fighter);
    vfx::feed::mark_di_dirty();
    dc::save_dr(g_m.fighter);
    g_m.dirty = false;
}

void step(int which, int dir) {                                   // which 1..5: our left / right (the spin never moves)
    const uint64_t now = cmn::frame();
    if (g_m.has_last_step && now - g_m.last_step < 12) return;    // one step per press: several input paths per press
    g_m.has_last_step = true; g_m.last_step = now;
    if (which < 1 || which > 5 || !g_m.has[which - 1]) return;
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    dc::json* cfg = dc::cfg_of((dc::Mech)(which - 1), g_m.fighter, g_m.side);
    if (!cfg) return;
    const auto& items = g_m.items[which - 1];
    dc::apply_value(*cfg, items, dc::stepped(cfg, items, dir));
    g_m.dirty = true; g_m.dirty_at = now;
    vfx::feed::mark_di_dirty();
    color_placement();
}

void push_display() {
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    for (int i = 0; i < 5; ++i) {
        if (!g_m.has[i]) break;                                   // ipairs(st.mecs): stops at the first hole
        const dc::json* cfg = dc::cfg_of((dc::Mech)i, g_m.fighter, g_m.side);
        auto r = dc::row_show((dc::Mech)i, cfg, g_m.items[i], g_m.fighter, g_m.side);
        if (!r) break;
        push_row(i, r->title.c_str(), r->label.c_str(), r->chip1, r->chip2, r->enabled, true);
    }
}

void pull_apply() {
    RowAction out[8];
    const int n = pull_actions(out, 8);
    for (int k = 0; k < n; ++k) {
        if (out[k].dir == 0) { if (out[k].row >= 0 && out[k].row < 5) g_decide = kMechName[out[k].row]; }
        else step(out[k].row + 1, out[k].dir);
    }
}
}  // namespace

void host(bool visible, uint64_t agent, int fighter, int costume, int side, bool suppress) {
    if (!visible || !agent) {
        if (g_m.on) g_m = Model{};
        g_focused.clear();
        return;
    }
    if (!API::get()->sdk()->managed_object->is_managed_object(reinterpret_cast<void*>(agent))) { g_m.on = false; return; }
    const uint64_t now = cmn::frame();
    const bool periodic = g_m.on && !suppress && (g_m.was_suppressed || now - g_m.resolved_at > 10);
    const bool same = g_m.on && g_m.agent == agent && g_m.fighter == fighter && g_m.side == side;
    if (!g_m.on || !same || periodic) {
        const bool first = !g_m.on || g_m.agent != agent;
        const bool keep_dirty = g_m.on && g_m.dirty;
        const uint64_t dirty_at = g_m.dirty_at, last_step = g_m.last_step;
        const bool has_last_step = g_m.has_last_step;
        const int old_fighter = g_m.fighter;
        g_m.on = true; g_m.agent = agent; g_m.fighter = fighter; g_m.costume = costume; g_m.side = side; g_m.resolved_at = now;
        // st.dirty / st.dirty_at / st.last_step survive a rebuild of the same st table in the Lua
        g_m.dirty = keep_dirty; g_m.dirty_at = dirty_at; g_m.last_step = last_step; g_m.has_last_step = has_last_step;
        if (keep_dirty && old_fighter != fighter) { /* the Lua saved the new fighter then (st.fighter overwritten) */ }
        std::lock_guard<std::recursive_mutex> lk(dc::lock());
        for (int i = 0; i < 5; ++i) {
            g_m.items[i] = dc::items((dc::Mech)i);
            g_m.has[i] = dc::cfg_of((dc::Mech)i, fighter, side) != nullptr;
        }
        resolve_color_row(agent);
        if (first) {
            int n = 0; for (bool h : g_m.has) n += h ? 1 : 0;
            cmn::journal("DriveRows (native): %d mecs, fighter %d P%d", n, fighter, side);
        }
    }
    g_m.suppress = suppress;
    g_m.was_suppressed = suppress;
    set_suppress(suppress);
    if (!suppress) { push_display(); pull_apply(); }
    const std::string nm = focused_of(reinterpret_cast<MO*>(agent));   // the host detects F / A / click on our rows
    g_focused.clear();
    for (int i = 0; i < 5; ++i) if (nm == kRowsFS[i].ctrl) g_focused = kMechName[i];
    if (g_m.dirty && now - g_m.dirty_at > 45) save_now();
    color_placement();
}

std::string focused_mech() { return g_focused; }
std::string take_decide() { std::string d = g_decide; g_decide.clear(); return d; }

}  // namespace screens::rows
