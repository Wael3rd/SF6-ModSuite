// screens::dipanel::mode — port of func/DIPanelMode.lua M.enter / M.exit / M.toggle / M.tick (native path) and its
// step. Game reads through REFramework (getters), writes through the core's display API and obj_invoke, as the Lua's
// CoreUI calls did. Special Attacks stays gated off (the Lua's spgate answered false): its row is hidden, never ours.

#include "dipanel_mode.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include "ll_common.hpp"

#include "../common.hpp"
#include "../vfx/drive_cfg.hpp"
#include "../vfx/feed.hpp"
#include "dipanel.hpp"
#include "slots_panel.hpp"

namespace screens::dipanel::mode {
namespace {

using API = reframework::API;
using MO = API::ManagedObject;
namespace dc = vfx::drive_cfg;

// ---- the panel's rows (DIPanelMode MECS) ----------------------------------------------------------------------------
struct RowDesc { dc::Mech mech; const char* row; int spin; const char* alt_row; };
constexpr RowDesc kRows[] = {
    { dc::Mech::Impact, "p_Spin_costume", 0, nullptr },
    { dc::Mech::Reversal, "p_Spin_color", 1, nullptr },
    { dc::Mech::Parry, "p_Spin_type", 2, nullptr },
    { dc::Mech::Rush, "p_Spin_preset", 3, nullptr },
    { dc::Mech::Special, "p_Spin_disen", 4, "p_Spin_drvspecial" },   // gated off: hidden, its spin disabled
};

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Size2 { float w = 0, h = 0; };

struct Row {                                       // rows_of: the row control and its parts
    void* ctrl = nullptr; void* title = nullptr; void* text = nullptr; void* list = nullptr;
    void* chip = nullptr; void* rect0 = nullptr; void* rect1 = nullptr;
};
struct Mec {
    dc::Mech mech;
    std::string row;
    MO* sp = nullptr;
    std::vector<dc::Item> items;
    int32_t smin = 0, smax = 0, snum = 0;          // spin_save
    bool was_visible = false;
    std::optional<std::string> saved_text;
    std::optional<Vec3> saved_pos;
    std::optional<Size2> saved_region, saved_font;
    bool has_mid = false;
    uint8_t mid[16] = {};
};
struct Side {
    bool on = false, popup = false;
    MO* param = nullptr; MO* agent = nullptr; MO* owner = nullptr;
    const char* spins_field = "Spins"; const char* group_field = "Group";
    int fighter = 0, costume = 0;
    std::vector<std::string> hide;
    std::vector<Mec> mecs;
    std::optional<int32_t> ci, co;
    bool by_getter = false;
    std::optional<Vec3> ref_pos;
    std::optional<Size2> ref_region;
};
Side g_s[3];
uint64_t g_popup_left_at = 0;                                             // frame the training popup's DI mode was left

const SF6CoreAPI* api() { return cmn::core(); }
SF6CoreClient* cl() { return cmn::client(); }
bool alive(MO* o) { return o && API::get()->sdk()->managed_object->is_managed_object(o); }

// ---- reads (REFramework getters) ----
reframework::InvokeRet call0(MO* o, const char* name, bool& ok) {
    ok = false;
    if (!o) return {};
    auto* m = ll::find_method_noargs(o->get_type_definition(), name);
    if (!m) return {};
    auto r = m->invoke(o, std::vector<void*>{});
    ok = !r.exception_thrown;
    return r;
}
std::optional<int32_t> get_i32(MO* o, const char* name) { bool ok; auto r = call0(o, name, ok); if (!ok) return std::nullopt; return (int32_t)r.dword; }
std::optional<bool> get_bool(MO* o, const char* name) { bool ok; auto r = call0(o, name, ok); if (!ok) return std::nullopt; return r.byte != 0; }
MO* get_obj(MO* o, const char* name) { bool ok; auto r = call0(o, name, ok); return ok ? reinterpret_cast<MO*>(r.ptr) : nullptr; }
std::optional<Vec3> get_vec3(MO* o, const char* name) {
    bool ok; auto r = call0(o, name, ok);
    if (!ok) return std::nullopt;
    Vec3 v; std::memcpy(&v.x, r.bytes.data(), 4); std::memcpy(&v.y, r.bytes.data() + 4, 4); std::memcpy(&v.z, r.bytes.data() + 8, 4);
    return v;
}
std::optional<Size2> get_size(MO* o, const char* name) {
    bool ok; auto r = call0(o, name, ok);
    if (!ok) return std::nullopt;
    Size2 v; std::memcpy(&v.w, r.bytes.data(), 4); std::memcpy(&v.h, r.bytes.data() + 4, 4);
    return v;
}
std::optional<std::string> get_message(void* text) {
    MO* s = get_obj(reinterpret_cast<MO*>(text), "get_Message");
    if (!s) return std::nullopt;
    return ll::managed_string_to_utf8(s);
}

// ---- writes (the core, as CoreUI did) ----
void invoke(void* o, const char* sig) { if (o) api()->obj_invoke(cl(), o, sig, nullptr, 0, nullptr); }
void invoke_i(void* o, const char* sig, int64_t v) { if (!o) return; SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v; api()->obj_invoke(cl(), o, sig, &a, 1, nullptr); }
void invoke_ii(void* o, const char* sig, int64_t v0, int64_t v1) {
    if (!o) return;
    SF6CoreArg a[2]{}; a[0].type = a[1].type = SF6CORE_ARG_I64; a[0].v.i = v0; a[1].v.i = v1;
    api()->obj_invoke(cl(), o, sig, a, 2, nullptr);
}
void invoke_b(void* o, const char* sig, bool v) { if (!o) return; SF6CoreArg a{}; a.type = SF6CORE_ARG_BOOL; a.v.i = v ? 1 : 0; api()->obj_invoke(cl(), o, sig, &a, 1, nullptr); }
void set_vis(void* c, bool on) { if (c) api()->ui_set_visible(cl(), c, on ? 1 : 0); }
void set_text(void* t, const std::string& s) { if (t) api()->ui_set_text(cl(), t, s.c_str()); }   // compare-before-write in the core
void spin_set(MO* sp, int32_t mn, int32_t mx, int32_t num) {
    invoke_i(sp, "set_MinNum", mn); invoke_i(sp, "set_MaxNum", mx);
    if (sp) api()->obj_set_i64(cl(), sp, "_Num", num);
}

void* find(void* root, const char* name) { return root ? api()->ui_find_child(root, name, 8) : nullptr; }
Row row_of(void* cm, const std::string& nm) {
    Row r;
    r.ctrl = find(cm, nm.c_str());
    if (!r.ctrl) return r;
    r.title = find(r.ctrl, "e_text_title"); r.text = find(r.ctrl, "e_text");
    r.list = find(r.ctrl, "p_TextList_color_h");
    if (!r.list) r.list = find(r.ctrl, "p_TextList_type_h");
    if (nm == "p_Spin_color") { r.chip = find(r.ctrl, "c_chip"); r.rect0 = find(r.ctrl, "e_rect_color_0"); r.rect1 = find(r.ctrl, "e_rect_color_1"); }
    return r;
}
void* control_main(MO* agent) { return get_obj(agent, "get_ControlMain"); }

MO* list_item(MO* owner, const char* field, int i) {             // spin_of: owner.<field>:get_Item(i)
    MO* l = owner ? ll::read_field_object(owner, field) : nullptr;
    if (!l) return nullptr;
    auto* m = ll::find_method_full(l->get_type_definition(), "get_Item(System.Int32)");
    if (!m) return nullptr;
    auto r = m->invoke(l, std::vector<void*>{ ll::arg_i32(i) });
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}
MO* parts_spin_by_name(MO* agent, const char* name) {             // find_parts_spin_by_name: the plugin's row part
    std::function<MO*(MO*, int)> walk = [&](MO* item, int depth) -> MO* {
        if (!item || depth > 6) return nullptr;
        MO* nm = ll::read_field_object(item, "<Name>k__BackingField");
        if (nm && ll::managed_string_to_utf8(nm) == name && item->get_type_definition()->get_full_name() == "app.UIPartsSpin") return item;
        MO* ch = ll::read_field_object(item, "_Children");
        if (!ch) return nullptr;
        auto cnt = get_i32(ch, "get_Count");
        auto* gi = ll::find_method_full(ch->get_type_definition(), "get_Item(System.Int32)");
        for (int k = 0; cnt && gi && k < *cnt; ++k) {
            auto r = gi->invoke(ch, std::vector<void*>{ ll::arg_i32(k) });
            if (r.exception_thrown) continue;
            if (MO* f = walk(reinterpret_cast<MO*>(r.ptr), depth + 1)) return f;
        }
        return nullptr;
    };
    return walk(agent ? ll::read_field_object(agent, "_RootItem") : nullptr, 0);
}

std::vector<dc::Item> items_of(dc::Mech m) { return dc::items(m); }

// push_native: what the Lua handed screens/dipanel after enter
void push(int side) {
    Side& st = g_s[side];
    MO* group = st.owner ? ll::read_field_object(st.owner, st.group_field) : nullptr;
    dipanel::enter(side, (uint64_t)st.param, (uint64_t)st.agent, (uint64_t)st.owner, (uint64_t)group,
                   st.ci ? *st.ci : -1, st.co ? *st.co : -1, (int32_t)st.mecs.size());
    std::string rn, hn;
    for (auto& d : st.mecs) rn += (rn.empty() ? "" : ",") + d.row;
    for (auto& h : st.hide) hn += (hn.empty() ? "" : ",") + h;
    dipanel::set_names(side, rn, hn);
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    for (size_t i = 0; i < st.mecs.size(); ++i) {
        Mec& d = st.mecs[i];
        const dc::json* cfg = dc::cfg_of(d.mech, st.fighter, side);
        dipanel::set_spin(side, (int)i + 1, (uint64_t)d.sp, 0, 1 + (int32_t)d.items.size(), dc::value_of(cfg, d.items));
        if (auto lb = dc::label(cfg, d.items, dc::desc(d.mech).kind)) dipanel::set_text(side, (int)i + 1, dc::title(d.mech), *lb);
    }
    dipanel::set_geom(side, st.ref_pos ? st.ref_pos->x : 0, st.ref_region ? st.ref_region->w : 300);
}

bool enter(int side, MO* param, MO* agent, MO* owner, bool popup, const char* spins_field, const char* group_field,
           const std::vector<std::string>& host_hide, int fighter, int costume) {
    if (side < 1 || side > 2 || g_s[side].on || !param || !agent || !owner || !api()) return false;
    // the select panel has one owner: the Slots Loader's costume options borrow the same rows (slots_panel.hpp)
    if (!popup && !slots_panel::take(side)) {
        cmn::journal("DI panel P%d: the Slots Loader's costume options hold this panel, X ignored", side);
        return false;
    }
    auto fail = [&]() { if (!popup) slots_panel::release(side); return false; };
    Side st;
    st.popup = popup; st.param = param; st.agent = agent; st.owner = owner;
    st.spins_field = spins_field; st.group_field = group_field;
    st.fighter = fighter; st.costume = costume;
    st.hide = host_hide;
    for (auto& d : kRows) if (d.mech == dc::Mech::Special) st.hide.push_back(d.row);   // Special Attacks off: its host row hidden
    void* cm = control_main(agent);
    if (!cm) return fail();
    std::vector<std::pair<std::string, Row>> rows;                       // names = mechanics' rows (+ alt), then hidden
    auto row = [&](const std::string& nm) -> Row& {
        for (auto& r : rows) if (r.first == nm) return r.second;
        rows.emplace_back(nm, row_of(cm, nm));
        return rows.back().second;
    };
    for (auto& d : kRows) { row(d.row); if (d.alt_row) row(d.alt_row); }
    for (auto& h : st.hide) row(h);
    for (auto& d : kRows) {
        const bool gated_off = d.mech == dc::Mech::Special;
        if (gated_off) {                                                  // the plugin's row: hidden, out of the navigation
            if (d.alt_row && row(d.alt_row).ctrl) {
                set_vis(row(d.alt_row).ctrl, false);
                if (MO* asp = parts_spin_by_name(agent, "drvspecial")) invoke_b(asp, "set_IsDisable", true);
            }
            continue;
        }
        Row& r = row(d.row);
        MO* sp = list_item(owner, spins_field, d.spin);
        std::lock_guard<std::recursive_mutex> lk(dc::lock());
        const dc::json* cfg = dc::cfg_of(d.mech, fighter, side);
        if (!(r.ctrl && r.text && sp && cfg)) continue;
        auto vis = get_bool(reinterpret_cast<MO*>(r.ctrl), "get_Visible");
        invoke_b(sp, "set_IsDisable", false);
        Mec m;
        m.mech = d.mech; m.row = d.row; m.sp = sp; m.items = items_of(d.mech);
        m.smin = get_i32(sp, "get_MinNum").value_or(0); m.smax = get_i32(sp, "get_MaxNum").value_or(0);
        int32_t num = 0; ll::read_field_i32(sp, "_Num", num); m.snum = num;
        m.was_visible = vis && *vis;
        st.mecs.push_back(std::move(m));
    }
    if (st.mecs.size() < 2) return fail();                                // panel not built yet
    st.ci = get_i32(owner, "get__cacheCostumeIndex");                     // the game's own indices
    st.co = get_i32(owner, "get__cacheColorIndex");
    if (!st.ci) {                                                         // the popup has no cache pair: ask it directly
        st.ci = get_i32(owner, "GetCostumeId"); st.co = get_i32(owner, "GetColorId");
        st.by_getter = true;
    }
    if (st.by_getter) {                                                   // the popup's spins: the indices (DIPanelMode.lua)
        if (st.ci) st.mecs[0].snum = *st.ci;
        if (st.mecs.size() > 1 && st.co) st.mecs[1].snum = *st.co;
    }                                                                     // the select screen's spins keep their own _Num (0)
    cmn::journal("DI panel enter: costume idx %d colour idx %d, %d lignes", st.ci ? *st.ci : -1, st.co ? *st.co : -1, (int)st.mecs.size());
    {
        std::lock_guard<std::recursive_mutex> lk(dc::lock());
        for (auto& d : st.mecs) spin_set(d.sp, 0, 1 + (int32_t)d.items.size(), dc::value_of(dc::cfg_of(d.mech, fighter, side), d.items));
    }
    for (auto& d : st.mecs) {                                             // titles: message ids kept, the link cut
        void* t = row(d.row).title;
        if (!t) continue;
        d.has_mid = api()->ui_get_message_id(t, d.mid) == SF6CORE_OK;
        const uint8_t empty[16] = {};
        api()->ui_set_message_id(cl(), t, empty);
        const std::string want = dc::title(d.mech);
        auto cur = get_message(t);
        if (cur && *cur == want) set_text(t, want + " ");                 // still our old string: the real write must differ
    }
    for (auto& h : st.hide) set_vis(row(h).ctrl, false);
    for (auto& d : st.mecs) {                                             // the value texts and positions before us
        Row& r = row(d.row);
        if (r.text) d.saved_text = get_message(r.text);
        if (r.list) d.saved_pos = get_vec3(reinterpret_cast<MO*>(r.list), "get_Position");
    }
    {                                                                     // the reference geometry: the first row
        Row& r = row(st.mecs[0].row);
        if (r.list) st.ref_pos = get_vec3(reinterpret_cast<MO*>(r.list), "get_Position");
        if (r.text) st.ref_region = get_size(reinterpret_cast<MO*>(r.text), "get_RegionSize");
    }
    for (auto& d : st.mecs) {
        Row& r = row(d.row);
        if (!r.text) continue;
        d.saved_region = get_size(reinterpret_cast<MO*>(r.text), "get_RegionSize");
        d.saved_font = get_size(reinterpret_cast<MO*>(r.text), "get_FontSize");
    }
    g_s[side] = std::move(st);
    g_s[side].on = true;
    push(side);
    cmn::journal("DI panel mode ON P%d fighter=%d", side, fighter);
    return true;
}

void exit(int side) {
    Side& st = g_s[side];
    if (!st.on) return;
    dipanel::set_cancel_block((uint64_t)st.owner, 15);                    // the press that closed us must not cancel / confirm
    if (st.popup) g_popup_left_at = cmn::frame();                         // nor close the pause menu (popup_esc_guard)
    dipanel::leave(side);
    Side s = std::move(st);
    g_s[side] = Side{};
    void* cm = control_main(s.agent);
    auto row = [&](const std::string& nm) { return cm ? row_of(cm, nm) : Row{}; };
    for (auto& d : s.mecs) {
        spin_set(d.sp, d.smin, d.smax, d.snum);
        Row r = row(d.row);
        if (r.title && d.has_mid) api()->ui_set_message_id(cl(), r.title, d.mid);
    }
    for (auto& h : s.hide) set_vis(row(h).ctrl, true);
    for (auto& d : s.mecs) {
        Row r = row(d.row);
        if (r.ctrl && !d.was_visible) { set_vis(r.ctrl, false); invoke_b(d.sp, "set_IsDisable", true); }   // disen / drvspecial
        if (r.chip) { set_vis(r.chip, true); set_vis(r.rect0, true); set_vis(r.rect1, true); }
        if (r.list && d.saved_pos) api()->ui_set_position(cl(), r.list, d.saved_pos->x, d.saved_pos->y, d.saved_pos->z);
        if (r.text) {
            if (d.saved_region) api()->ui_set_region_size(cl(), r.text, d.saved_region->w, d.saved_region->h);
            if (d.saved_font) api()->ui_set_font_size(cl(), r.text, d.saved_font->w, d.saved_font->h);
        }
    }
    if (!s.by_getter) {
        if (s.ci) invoke_i(s.owner, "set__cacheCostumeIndex", *s.ci);
        if (s.co) invoke_i(s.owner, "set__cacheColorIndex", *s.co);
    }
    invoke(s.owner, "SetMessageCostume"); invoke(s.owner, "SetMessageColor");   // the game's texts back
    invoke(s.owner, "SetMessage");                                               // the popup's own refresh
    invoke(s.owner, "SelectChangeColorChip");                                    // and its own two chips
    // The select screen's SetMessageCostume / SetMessageColor do not read the caches put back above: called on their
    // own they reset the selection to costume 0 / colour 0 (probed 06/10, also outside DI mode), so the model came
    // back in the first outfit while the row still said "Outfit VII" (a bug of DIPanelMode.lua too, seen on 0.15.0).
    // The game's own setter puts the selection, the texts, the chips and the model back together.
    if (!s.by_getter && s.ci && s.co) invoke_ii(s.owner, "setCostumeAndColor", *s.ci, *s.co);
    for (auto& d : s.mecs)                                                // never the Color row's value text (a native fault)
        if (d.saved_text && d.row != "p_Spin_color") { Row r = row(d.row); if (r.text) set_text(r.text, *d.saved_text); }
    dc::save_di(s.fighter);
    vfx::feed::mark_di_dirty();
    dc::save_dr(s.fighter);                                               // parry / rush: their own file
    if (!s.popup) slots_panel::release(side);                             // the panel's claim given back last
    cmn::journal("DI panel mode OFF P%d", side);
}

}  // namespace

std::string enter_select(int side, uint64_t param, uint64_t agent, int fighter, int costume) {
    if (side < 1 || side > 2) return "failed";
    if (g_s[side].on) return "already on, L1 / X ignored (A / B leave)";
    MO* p = reinterpret_cast<MO*>(param);
    return enter(side, p, reinterpret_cast<MO*>(agent), p, false, "Spins", "Group", {}, fighter, costume) ? "on" : "failed";
}

bool enter_popup(int side, uint64_t param, uint64_t agent, uint64_t mfs, int fighter, int costume) {
    return enter(side, reinterpret_cast<MO*>(param), reinterpret_cast<MO*>(agent), reinterpret_cast<MO*>(mfs), true,
                 "mArrSpin", "mGroup", { "p_Spin_stick" }, fighter, costume);
}

bool active(int side) { return side >= 1 && side <= 2 && g_s[side].on; }

bool popup_esc_guard() {
    for (int s = 1; s <= 2; ++s) if (g_s[s].on && g_s[s].popup) return true;
    return g_popup_left_at && cmn::frame() - g_popup_left_at <= 15;
}

void tick(int side, uint64_t host_agent) {
    if (side < 1 || side > 2) return;
    Side& st = g_s[side];
    if (!st.on) return;
    if (!alive(st.param) || !alive(st.agent)) {                           // gone: tell the hooks, drop the side
        dipanel::leave(side);
        if (!st.popup) slots_panel::release(side);
        g_s[side] = Side{};
        return;
    }
    if (!st.popup && host_agent && host_agent != (uint64_t)st.agent) { exit(side); return; }   // panel rebuilt: leave cleanly
    for (int k = 0; k < 8; ++k) {                                         // the actions the hooks captured
        int32_t evt = 0, which = 0, dir = 0;
        dipanel::pull(side, evt, which, dir);
        if (evt == 0) break;
        if (evt == 2) { exit(side); return; }
        if (evt == 1 && which >= 1 && which <= (int)st.mecs.size()) {
            Mec& d = st.mecs[(size_t)which - 1];
            std::lock_guard<std::recursive_mutex> lk(dc::lock());
            dc::json* cfg = dc::cfg_of(d.mech, st.fighter, side);
            if (!cfg) continue;
            dc::apply_value(*cfg, d.items, dc::stepped(cfg, d.items, dir));
            vfx::feed::mark_di_dirty();
            dipanel::set_spin(side, which, (uint64_t)d.sp, 0, 1 + (int32_t)d.items.size(), dc::value_of(cfg, d.items));
            if (auto lb = dc::label(cfg, d.items, dc::desc(d.mech).kind)) dipanel::set_text(side, which, dc::title(d.mech), *lb);
        }
    }
}

}  // namespace screens::dipanel::mode
