#ifndef NOMINMAX
#define NOMINMAX
#endif
// dialogs::options - the native option-dialog engine (phase 4b): the game side. See options.hpp.
//
// Everything here is a game call or a hook; every decision is in options_core.cpp. Lines cited are those of
// func/NativeOptions.lua.

#include "options.hpp"

#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ll_common.hpp"
#include "opt_common.hpp"
#include "guid_text.hpp"
#include "options_core.hpp"

namespace dialogs::options {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

constexpr int kMaxFaults = 5;          // same threshold as the screens modules
constexpr int kPauseType = 8;          // :713 BATTLE_MENU_PAUSE, the one that freezes the fight
constexpr int kDynamicRowFunc = 345;   // :254 rows a previous Lua state added to the training pause menu
constexpr int kEventSettingReset = 1;  // :430, :503 DecideEventType.SettingReset

bool g_on = false;
int  g_faults = 0;
bool g_purge_pause_rows = true;        // :267, pushed by the wrapper before a build

API::TypeDefinition* g_bg_param_td = nullptr;

void fault(const char* where) {
    ++g_faults;
    cmn::journal("[options] fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("[options] too many faults, hooks off");
    }
}

MO* mo(core::Obj o) { return reinterpret_cast<MO*>(o); }

// The Lua names the overload (full signature) or lets :call match the arguments. Here: the full signature first,
// else the overload of that name with that many parameters; never "the first one called so" (R17, crash C4).
API::Method* method_n(API::TypeDefinition* td, const char* full_sig, const char* bare, uint32_t n_params) {
    if (!td) return nullptr;
    if (auto* m = td->find_method(full_sig)) return m;
    for (auto* t = td; t != nullptr; t = t->get_parent_type())
        for (auto* m : t->get_methods())
            if (m && m->get_name() && std::string_view{m->get_name()} == bare && m->get_num_params() == n_params) return m;
    return nullptr;
}

// unit:call("get_Setting").TypeId, and optionally its EventType
bool unit_setting(MO* unit, int32_t& type_id, int32_t* event_type = nullptr) {
    if (!unit) return false;
    auto* gs = ll::find_method_noargs(unit->get_type_definition(), "get_Setting");
    if (!gs) return false;
    auto sr = gs->invoke(unit, std::vector<void*>{});
    if (sr.exception_thrown || !sr.ptr) return false;
    MO* st = reinterpret_cast<MO*>(sr.ptr);
    if (!ll::read_field_i32(st, "TypeId", type_id)) return false;
    if (event_type) ll::read_field_i32(st, "EventType", *event_type);
    return true;
}

// param:call("GetFocusUnit") -> its TypeId
bool focus_type_id(MO* param, int32_t& type_id) {
    if (!param) return false;
    auto* get_focus = ll::find_method_noargs(param->get_type_definition(), "GetFocusUnit");
    if (!get_focus) return false;
    auto fr = get_focus->invoke(param, std::vector<void*>{});
    if (fr.exception_thrown || !fr.ptr) return false;
    return unit_setting(reinterpret_cast<MO*>(fr.ptr), type_id);
}

// The rows of a page: param.OptionUnits[i] (the part), its UnitData (the unit).
struct Row { MO* part; int32_t type_id; int32_t event_type; };
std::vector<Row> page_rows(MO* param) {
    std::vector<Row> rows;
    MO* parts = param ? ll::read_field_object(param, "OptionUnits") : nullptr;
    if (!parts) return rows;
    auto* get_len = ll::find_method_noargs(parts->get_type_definition(), "get_Length");
    auto* get_val = ll::find_method_full(parts->get_type_definition(), "GetValue");
    if (!get_len || !get_val) return rows;
    auto lr = get_len->invoke(parts, std::vector<void*>{});
    if (lr.exception_thrown) return rows;
    for (int32_t i = 0; i < (int32_t)lr.dword; ++i) {
        auto iv = get_val->invoke(parts, std::vector<void*>{ ll::arg_i32(i) });
        if (iv.exception_thrown || !iv.ptr) { rows.push_back({ nullptr, 0, 0 }); continue; }   // index kept
        MO* part = reinterpret_cast<MO*>(iv.ptr);
        Row r{ part, 0, 0 };
        MO* ud = ll::read_field_object(part, "UnitData");
        if (!ud || !unit_setting(ud, r.type_id, &r.event_type)) r.type_id = 0;
        rows.push_back(r);
    }
    return rows;
}

// ======================== backend: the game operations of the core ========================

void be_retain(core::Obj o) { if (o) mo(o)->add_ref(); }
void be_release(core::Obj o) { if (o) mo(o)->release(); }

bool be_get_is_end(core::Obj h, bool& ended) {
    if (!h) return false;
    auto* m = ll::find_method_noargs(mo(h)->get_type_definition(), "get_IsEnd");
    if (!m) return false;
    auto ret = m->invoke(mo(h), std::vector<void*>{});
    if (ret.exception_thrown) return false;
    ended = ret.byte != 0;
    return true;
}

// :733 if not h:call("get_IsEnd") then h:call("End") end
void be_end_handle(core::Obj h) {
    bool ended = false;
    if (!be_get_is_end(h, ended) || ended) return;
    auto* end_m = ll::find_method_noargs(mo(h)->get_type_definition(), "End");
    if (end_m) end_m->invoke(mo(h), std::vector<void*>{});
}

// :757-760
core::Obj be_start_dialog(core::Obj unit) {
    auto* tdb = API::get()->tdb();
    auto* sd_td = tdb ? tdb->find_type("app.UIFlowOptionBGDialog.SettingData") : nullptr;
    if (!sd_td) return nullptr;
    MO* sd = sd_td->create_instance();
    if (!sd) return nullptr;
    sd->add_ref();                                               // held: the GC collects an unreferenced instance (03/09)
    MO* u = mo(unit);
    ll::write_field_raw(sd, "TopUnit", &u, sizeof(void*));
    ll::write_field_i32(sd, "SupportMode", 32);
    uint8_t f = 0;
    ll::write_field_raw(sd, "UseBattleHudBG", &f, 1);
    ll::write_field_i32(sd, "PlayerIndex", 0);
    auto* dialog_td = tdb->find_type("app.UIFlowOptionBGDialog");
    auto* start_m = method_n(dialog_td, "Start(app.UIFlowOptionBGDialog.SettingData, System.Boolean)", "Start", 2);   // :759
    if (!start_m) return nullptr;
    auto ret = start_m->invoke(nullptr, std::vector<void*>{ sd, ll::arg_bool(false) });
    return (!ret.exception_thrown && ret.ptr) ? ret.ptr : nullptr;
}

// :707 Find.agent(name) ~= nil: the core's agent registry (app.UIAgentManager._Entries by GameObject name)
bool be_agent_present(const char* name) {
    const auto* api = cmn::core();
    return api && api->ui_find_agent && api->ui_find_agent(name) != nullptr;
}

// :718-719 requestPause(bool start, PauseType)
bool be_request_pause(bool on) {
    auto* mgr = API::get()->get_managed_singleton("app.PauseManager");
    if (!mgr) return false;
    auto* m = method_n(mgr->get_type_definition(), "requestPause(System.Boolean, app.PauseManager.PauseType)", "requestPause", 2);
    if (!m) return false;
    auto ret = m->invoke(mgr, std::vector<void*>{ ll::arg_bool(on), ll::arg_i32(kPauseType) });
    return !ret.exception_thrown;
}

bool be_get_value(core::Obj unit, int32_t& out) {
    if (!unit) return false;
    auto* get_val = ll::find_method_noargs(mo(unit)->get_type_definition(), "get_Value");
    if (!get_val) return false;
    auto vr = get_val->invoke(mo(unit), std::vector<void*>{});
    if (vr.exception_thrown) return false;
    out = (int32_t)vr.dword;
    return true;
}

// e.unit:call("SetValue", n, refresh) (:196, :1099)
void be_set_value(core::Obj unit, int32_t value, bool refresh) {
    if (!unit) return;
    auto* sv = ll::find_method_full(mo(unit)->get_type_definition(), "SetValue");
    if (sv) sv->invoke(mo(unit), std::vector<void*>{ ll::arg_i32(value), ll::arg_bool(refresh) });
}

// :110-118 fresh ids per load, above the game's own ValueType values
int32_t be_type_id_base() {
    auto* tdb = API::get()->tdb();
    auto* vt_td = tdb ? tdb->find_type("app.Option.ValueType") : nullptr;
    int32_t max_val = 0;
    if (vt_td) {
        for (auto* f : vt_td->get_fields()) {
            if (!f || !f->is_static()) continue;
            auto* data = f->get_data_raw(nullptr, true);
            if (!data) continue;
            int32_t v;
            std::memcpy(&v, data, 4);
            if (v < 2100000000 && v > max_val) max_val = v;
        }
    }
    int32_t base = (max_val > 100000) ? (((max_val + 1 + 9) / 10) * 10 + 100000) : 100000;   // math.ceil((max + 1) / 10) * 10 + 100000
    base += (int32_t)((GetTickCount64() / 1000) % 5000) * 100;   // os.time() % 5000: the UI caches widget kinds by TypeId across reloads
    return base;
}

// ---- unit building (:199-240, :262-392) ----

MO* g_empty_guids = nullptr;
MO* g_empty_settings = nullptr;

int32_t g_InputType_Button1 = 0, g_InputType_Button2 = 0, g_InputType_Slider = 0, g_InputType_SpinText = 0;
int32_t g_EventType_OpenSubMenu = 0, g_EventType_OpenBattleHud = 0, g_EventType_SettingReset = 0;
int32_t g_DataType_Value = 0;
int32_t g_TabType_General = 0;
bool g_enums_loaded = false;

bool load_enum_value(const char* type_name, const char* field_name, int32_t& out) {
    auto* tdb = API::get()->tdb();
    auto* td = tdb ? tdb->find_type(type_name) : nullptr;
    auto* f = td ? td->find_field(field_name) : nullptr;
    auto* data = f ? f->get_data_raw(nullptr, true) : nullptr;
    if (!data) return false;
    std::memcpy(&out, data, 4);
    return true;
}

bool load_enums() {
    if (g_enums_loaded) return true;
    bool ok = true;
    ok &= load_enum_value("app.Option.UnitInputType", "Button_Type1", g_InputType_Button1);
    ok &= load_enum_value("app.Option.UnitInputType", "Button_Type2", g_InputType_Button2);
    ok &= load_enum_value("app.Option.UnitInputType", "Slider", g_InputType_Slider);
    ok &= load_enum_value("app.Option.UnitInputType", "SpinText", g_InputType_SpinText);
    ok &= load_enum_value("app.Option.DecideEventType", "OpenSubMenu", g_EventType_OpenSubMenu);
    ok &= load_enum_value("app.Option.DecideEventType", "OpenBattleHudSetting", g_EventType_OpenBattleHud);
    ok &= load_enum_value("app.Option.DecideEventType", "SettingReset", g_EventType_SettingReset);
    ok &= load_enum_value("app.Option.SettingDataType", "Value", g_DataType_Value);
    ok &= load_enum_value("app.Option.TabType", "General", g_TabType_General);
    g_enums_loaded = ok;
    return ok;
}

// The General tab's unit list: mgr.UnitLists:call("get_Item", TabType.General) (:271, :596)
MO* general_list() {
    auto* mgr = API::get()->get_managed_singleton("app.OptionManager");
    if (!mgr || !load_enums()) return nullptr;
    MO* unit_lists = ll::read_field_object(mgr, "UnitLists");
    if (!unit_lists) return nullptr;
    auto* get_item = ll::find_method_full(unit_lists->get_type_definition(), "get_Item");
    if (!get_item) return nullptr;
    auto ret = get_item->invoke(unit_lists, std::vector<void*>{ ll::arg_i32(g_TabType_General) });
    return (!ret.exception_thrown && ret.ptr) ? reinterpret_cast<MO*>(ret.ptr) : nullptr;
}

// :210-219 new_setting
MO* create_setting(const std::string& key, int32_t& type_id) {
    auto* td = API::get()->tdb()->find_type("app.Option.OptionSettingUnit");
    MO* d = td ? td->create_instance() : nullptr;
    if (!d) return nullptr;
    d->add_ref();                                                // held: the GC collects an unreferenced instance (03/09)
    type_id = core::type_id_for_key(key);
    ll::write_field_i32(d, "TypeId", type_id);
    return d;
}

MO* clone_list(MO* empty) {
    auto* clone_m = empty ? ll::find_method_noargs(empty->get_type_definition(), "MemberwiseClone") : nullptr;
    if (!clone_m) return nullptr;
    auto cr = clone_m->invoke(empty, std::vector<void*>{});
    if (cr.exception_thrown || !cr.ptr) return nullptr;
    MO* l = reinterpret_cast<MO*>(cr.ptr);
    l->add_ref();                                                // held like the units (R28 / R34)
    return l;
}

// :223-236 make_unit. option_guids receives hi, lo of each value message.
MO* make_unit(MO* desc, const std::string& description, const std::vector<std::string>& value_messages,
              std::vector<uint64_t>* option_guids) {
    if (!desc) return nullptr;
    auto* make_m = ll::find_method_noargs(desc->get_type_definition(), "MakeUnitData");
    if (!make_m) return nullptr;
    auto ret = make_m->invoke(desc, std::vector<void*>{});
    if (ret.exception_thrown || !ret.ptr) return nullptr;
    MO* unit = reinterpret_cast<MO*>(ret.ptr);
    auto* get_setting = ll::find_method_noargs(unit->get_type_definition(), "get_Setting");
    if (!get_setting) return nullptr;
    auto sret = get_setting->invoke(unit, std::vector<void*>{});
    if (sret.exception_thrown || !sret.ptr) return nullptr;
    MO* s = reinterpret_cast<MO*>(sret.ptr);

    guid_text::set_field(s, "DescriptionMessage", guid_text::alloc(description));
    MO* vml = clone_list(g_empty_guids);
    MO* cul = clone_list(g_empty_settings);
    if (vml) ll::write_field_raw(s, "ValueMessageList", &vml, sizeof(void*));
    if (cul) ll::write_field_raw(s, "ChildUnitList", &cul, sizeof(void*));
    if (vml && !value_messages.empty()) {
        auto* add_m = ll::find_method_full(vml->get_type_definition(), "Add");
        for (auto& msg : value_messages) {
            const auto gb = guid_text::alloc(msg);
            if (option_guids) { option_guids->push_back(gb.hi); option_guids->push_back(gb.lo); }
            if (add_m) {                                         // List<System.Guid>.Add: a value type goes by pointer
                uint8_t buf[16];
                std::memcpy(buf, &gb.hi, 8);
                std::memcpy(buf + 8, &gb.lo, 8);
                add_m->invoke(vml, std::vector<void*>{ buf });
            }
        }
    }
    return unit;
}

// :237-240
void attach_unit(MO* parent, MO* child) {
    if (!parent || !child) return;
    if (auto* get_children = ll::find_method_noargs(parent->get_type_definition(), "get_ChildUnits")) {
        auto cr = get_children->invoke(parent, std::vector<void*>{});
        if (!cr.exception_thrown && cr.ptr) {
            MO* list = reinterpret_cast<MO*>(cr.ptr);
            if (auto* add_m = ll::find_method_full(list->get_type_definition(), "Add")) add_m->invoke(list, std::vector<void*>{ child });
        }
    }
    auto* get_s1 = ll::find_method_noargs(parent->get_type_definition(), "get_Setting");
    auto* get_s2 = ll::find_method_noargs(child->get_type_definition(), "get_Setting");
    if (!get_s1 || !get_s2) return;
    auto sr1 = get_s1->invoke(parent, std::vector<void*>{});
    auto sr2 = get_s2->invoke(child, std::vector<void*>{});
    if (sr1.exception_thrown || !sr1.ptr || sr2.exception_thrown || !sr2.ptr) return;
    MO* cul = ll::read_field_object(reinterpret_cast<MO*>(sr1.ptr), "ChildUnitList");
    if (!cul) return;
    if (auto* add_m = ll::find_method_full(cul->get_type_definition(), "Add")) add_m->invoke(cul, std::vector<void*>{ sr2.ptr });
}

// :244-260
void purge_pause_menu_rows() {
    auto* tm = API::get()->get_managed_singleton("app.training.TrainingManager");
    MO* ui_data = tm ? ll::read_field_object(tm, "_UIData") : nullptr;
    if (!ui_data) return;
    auto* get_menu = ll::find_method_noargs(ui_data->get_type_definition(), "get_MenuData");
    if (!get_menu) return;
    auto arr_ret = get_menu->invoke(ui_data, std::vector<void*>{});
    if (arr_ret.exception_thrown || !arr_ret.ptr) return;
    MO* arr = reinterpret_cast<MO*>(arr_ret.ptr);
    auto* get_len = ll::find_method_noargs(arr->get_type_definition(), "get_Length");
    auto* get_val = ll::find_method_full(arr->get_type_definition(), "GetValue");
    if (!get_len || !get_val) return;
    auto lr = get_len->invoke(arr, std::vector<void*>{});
    if (lr.exception_thrown) return;
    for (int32_t t = 0; t < (int32_t)lr.dword; ++t) {
        auto tv = get_val->invoke(arr, std::vector<void*>{ ll::arg_i32(t) });
        if (tv.exception_thrown || !tv.ptr) continue;
        MO* dl = ll::read_field_object(reinterpret_cast<MO*>(tv.ptr), "DynamicChildData");
        if (!dl) continue;
        auto* dl_count = ll::find_method_noargs(dl->get_type_definition(), "get_Count");
        auto* dl_item = ll::find_method_full(dl->get_type_definition(), "get_Item");
        auto* dl_remove = ll::find_method_full(dl->get_type_definition(), "RemoveAt");
        if (!dl_count || !dl_item || !dl_remove) continue;
        auto dc = dl_count->invoke(dl, std::vector<void*>{});
        if (dc.exception_thrown) continue;
        for (int32_t i = (int32_t)dc.dword - 1; i >= 0; --i) {
            auto di = dl_item->invoke(dl, std::vector<void*>{ ll::arg_i32(i) });
            if (di.exception_thrown || !di.ptr) continue;
            MO* cd = ll::read_field_object(reinterpret_cast<MO*>(di.ptr), "ChildData");
            int32_t ft = 0;
            if (cd && ll::read_field_i32(cd, "_FuncType", ft) && ft == kDynamicRowFunc) dl_remove->invoke(dl, std::vector<void*>{ ll::arg_i32(i) });
        }
    }
}

// :262-392 (radio groups and the composite window are not ported)
bool be_build_units() {
    if (!API::get()->get_managed_singleton("app.OptionManager")) return false;
    if (g_purge_pause_rows) purge_pause_menu_rows();
    MO* parent_list = general_list();
    if (!parent_list) return false;
    guid_text::begin_tree();

    // the empty list templates, from the first unit of the tab (:272-274)
    auto* get_item = ll::find_method_full(parent_list->get_type_definition(), "get_Item");
    if (!get_item) return false;
    auto first_ret = get_item->invoke(parent_list, std::vector<void*>{ ll::arg_i32(0) });
    if (first_ret.exception_thrown || !first_ret.ptr) return false;
    MO* first_unit = reinterpret_cast<MO*>(first_ret.ptr);
    auto* get_setting = ll::find_method_noargs(first_unit->get_type_definition(), "get_Setting");
    if (!get_setting) return false;
    auto fs_ret = get_setting->invoke(first_unit, std::vector<void*>{});
    if (fs_ret.exception_thrown || !fs_ret.ptr) return false;
    MO* first_setting = reinterpret_cast<MO*>(fs_ret.ptr);
    auto empty_range = [](MO* list) -> MO* {
        auto* gr_m = list ? ll::find_method_full(list->get_type_definition(), "GetRange") : nullptr;
        if (!gr_m) return nullptr;
        auto gr = gr_m->invoke(list, std::vector<void*>{ ll::arg_i32(0), ll::arg_i32(0) });
        return (!gr.exception_thrown && gr.ptr) ? reinterpret_cast<MO*>(gr.ptr) : nullptr;
    };
    g_empty_guids = empty_range(ll::read_field_object(first_setting, "ValueMessageList"));
    g_empty_settings = empty_range(ll::read_field_object(first_setting, "ChildUnitList"));
    if (!g_empty_guids || !g_empty_settings) return false;

    // root (:277-281)
    int32_t tid = 0;
    MO* rd = create_setting("__root", tid);
    if (!rd) return false;
    guid_text::set_field(rd, "TitleMessage", guid_text::alloc(core::kComposite));
    ll::write_field_i32(rd, "InputType", g_InputType_Button1);
    ll::write_field_i32(rd, "EventType", g_EventType_OpenSubMenu);
    MO* root = make_unit(rd, "Settings for the SF6 training scripts.", {}, nullptr);
    if (!root) return false;
    // The root is NOT added to the game's General list (the Lua did, :282): that list is what Options > General
    // shows, and the "SF6 Tools" row it gave there opened nothing useful (04/10). The Lua needed the list to
    // keep the tree alive; here every unit, setting and list is held by add_ref, and the dialogs are started on
    // the group units directly.
    core::build_set_root(root);

    // groups and entries (:284-332)
    for (auto& gp : core::groups()) {
        auto& g = *gp;
        const std::string gkey = core::id_key_of(g);
        MO* gd = create_setting("g:" + gkey, tid);
        if (!gd) return false;
        guid_text::set_field(gd, "TitleMessage", guid_text::alloc(g.title));
        ll::write_field_i32(gd, "InputType", g_InputType_Button1);
        ll::write_field_i32(gd, "EventType", g_EventType_OpenBattleHud);     // HUD window over the fight
        MO* gunit = make_unit(gd, g.desc, {}, nullptr);
        if (!gunit) return false;
        attach_unit(root, gunit);
        core::build_set_group(g, gunit);

        std::vector<std::shared_ptr<core::Entry>> ordered;
        for (auto& e : g.entries) if (e->kind != core::Kind::Button) ordered.push_back(e);
        for (auto& e : g.entries) if (e->kind == core::Kind::Button) ordered.push_back(e);   // buttons at the bottom
        for (auto& e : ordered) {
            MO* ed = create_setting("e:" + gkey + "/" + e->key, tid);
            if (!ed) return false;
            const auto title_guid = guid_text::alloc(e->title);
            e->title_guid[0] = title_guid.hi; e->title_guid[1] = title_guid.lo;
            guid_text::set_field(ed, "TitleMessage", title_guid);
            e->option_guids.clear();
            if (e->kind == core::Kind::Button) {
                ll::write_field_i32(ed, "InputType", g_InputType_Button2);       // exactly the Restore-row pattern
                ll::write_field_i32(ed, "EventType", g_EventType_SettingReset);
                MO* u = make_unit(ed, e->desc, {}, nullptr);
                if (!u) return false;
                core::build_set_entry(e, tid, u);
                attach_unit(gunit, u);
                continue;
            }
            ll::write_field_i32(ed, "_DataType", g_DataType_Value);
            ll::write_field_i32(ed, "InputType", (e->kind == core::Kind::Slider) ? g_InputType_Slider : g_InputType_SpinText);
            MO* u = make_unit(ed, e->desc, (e->kind == core::Kind::Slider) ? std::vector<std::string>{} : e->options, &e->option_guids);
            if (!u) return false;
            attach_unit(gunit, u);
            auto* vs_td = API::get()->tdb()->find_type("app.Option.OptionValueSetting");
            MO* vs = vs_td ? vs_td->create_instance() : nullptr;
            if (!vs) return false;
            vs->add_ref();                                           // held (03/09)
            const int32_t init = (int32_t)e->value;
            const bool slider = e->kind == core::Kind::Slider;
            ll::write_field_i32(vs, "TypeId", tid);
            ll::write_field_i32(vs, "MinValue", slider ? e->min_val : 0);
            ll::write_field_i32(vs, "MaxValue", slider ? e->max_val : (int32_t)e->options.size() - 1);
            ll::write_field_i32(vs, "InitValue", init);
            if (auto* set_pv = ll::find_method_full(u->get_type_definition(), "set_PrevValue")) set_pv->invoke(u, std::vector<void*>{ ll::arg_i32(init) });
            if (auto* set_vs = ll::find_method_full(u->get_type_definition(), "set_ValueSetting")) set_vs->invoke(u, std::vector<void*>{ vs });
            if (auto* make_vd = ll::find_method_noargs(vs->get_type_definition(), "MakeValueData")) {
                auto vd_ret = make_vd->invoke(vs, std::vector<void*>{});
                if (!vd_ret.exception_thrown && vd_ret.ptr) {
                    MO* vd = reinterpret_cast<MO*>(vd_ret.ptr);
                    vd->add_ref();                                   // a raw store alone would leave it to the GC (R34)
                    ll::write_field_raw(u, "ValueData", &vd, sizeof(void*));
                }
            }
            core::build_set_entry(e, tid, u);
        }
    }
    cmn::journal("[options] built: %zu groups, guid_text %s", core::groups().size(), guid_text::status().c_str());
    return true;
}

// :593-606. The texts of the purged trees go with them (decision Q3); the tree kept in the list keeps its own.
void be_purge_roots(bool keep_live) {
    MO* cur = general_list();
    int32_t root_type_id = 0;
    if (cur && core::has_type_id_for_key("__root", root_type_id)) {
        MO* root = mo(core::root_unit());
        const bool keep = keep_live && root && API::get()->sdk()->managed_object->is_managed_object(root);
        auto* get_count = ll::find_method_noargs(cur->get_type_definition(), "get_Count");
        auto* get_item = ll::find_method_full(cur->get_type_definition(), "get_Item");
        auto* remove_m = ll::find_method_full(cur->get_type_definition(), "Remove");
        auto cnt_ret = (get_count && get_item && remove_m) ? get_count->invoke(cur, std::vector<void*>{}) : reframework::InvokeRet{};
        if (get_count && get_item && remove_m && !cnt_ret.exception_thrown) {
            for (int32_t i = (int32_t)cnt_ret.dword - 1; i >= 0; --i) {
                auto item_ret = get_item->invoke(cur, std::vector<void*>{ ll::arg_i32(i) });
                if (item_ret.exception_thrown || !item_ret.ptr) continue;
                MO* u = reinterpret_cast<MO*>(item_ret.ptr);
                int32_t tid = 0;
                if (!unit_setting(u, tid) || tid != root_type_id) continue;
                if (keep && u == root) continue;
                remove_m->invoke(cur, std::vector<void*>{ u });
            }
        }
    }
    guid_text::purge(keep_live);
}

// ======================== hooks ========================

#pragma warning(push)
#pragma warning(disable: 4733)

// FlowEvent_ResetCurrentUnits (:405-418): one of our buttons under the cursor = its callback, not the game's reset
int pre_reset_units_inner(void** argv) {
    int32_t tid = 0;
    if (focus_type_id(reinterpret_cast<MO*>(argv[1]), tid) && core::on_button(tid)) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_reset_units(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return pre_reset_units_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("reset_units_pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_reset_units(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// GetFocusDecideEventType (:423-442): Invalid for our buttons, so the FSM does nothing (no "Revert settings?")
thread_local MO* tl_decide_this = nullptr;
int pre_decide_event(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_decide_this = g_on ? reinterpret_cast<MO*>(argv[1]) : nullptr;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_decide_event_inner(void** ret_val) {
    if ((reinterpret_cast<int64_t>(*ret_val) & 0xFF) != kEventSettingReset) return;
    int32_t tid = 0;
    if (focus_type_id(tl_decide_this, tid) && core::on_button(tid)) *ret_val = nullptr;      // DecideEventType.Invalid
}
void post_decide_event(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_decide_this) return;
    __try { post_decide_event_inner(ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("decide_event_post"); }
}

// CreatedObject (:451-474): our dialogs skip the screen fade (a fade frozen under the pause = the black screens)
thread_local MO* tl_created_param = nullptr;
void pre_created_object_inner(void** argv) {
    MO* param = reinterpret_cast<MO*>(argv[1]);
    if (!param || !core::window_open()) return;
    const uint8_t v = 1;
    ll::write_field_raw(param, "<ImmediateFade>k__BackingField", &v, 1);
    tl_created_param = param;
}
int pre_created_object(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_created_param = nullptr;
    if (!g_on) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { pre_created_object_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("created_object_pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_created_object(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_created_param) return;
    __try { core::on_created_object(tl_created_param); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("created_object_post"); }
    tl_created_param = nullptr;
}

// SetupDispUnits (:476-529): the page just built. Ours: its entries are recorded, and the dialog's own
// "Restore Default Settings" row is hidden and disabled (a no-op there: our ids skip ResetEvent).
thread_local MO* tl_setup_this = nullptr;
void pre_setup_disp_units_inner(void** argv) {
    tl_setup_this = reinterpret_cast<MO*>(argv[1]);
    const int focus = core::take_pending_focus();                // cursor back on the row we came from
    if (focus >= 0) { if (MO* data = reinterpret_cast<MO*>(argv[2])) ll::write_field_i32(data, "FocusIndex", focus); }
}
int pre_setup_disp_units(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_setup_this = nullptr;
    if (!g_on) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { pre_setup_disp_units_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("setup_disp_pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_setup_disp_units_inner() {
    MO* param = tl_setup_this;
    const auto rows = page_rows(param);
    if (rows.empty()) return;
    std::vector<core::PagePart> parts;
    for (auto& r : rows) if (r.part && r.type_id) parts.push_back({ r.type_id, r.event_type, r.part });
    if (!core::on_page_setup(param, parts)) return;              // a foreign dialog keeps its row
    const auto* api = cmn::core();
    for (auto& r : rows) {
        if (!r.part || !r.type_id || r.event_type != kEventSettingReset || core::known(r.type_id)) continue;
        if (auto* get_ctrl = ll::find_method_noargs(r.part->get_type_definition(), "get_Control")) {
            auto cr = get_ctrl->invoke(r.part, std::vector<void*>{});
            if (!cr.exception_thrown && cr.ptr && api) api->ui_set_force_invisible(cmn::client(), cr.ptr, 1);
        }
        if (api) { SF6CoreArg a{}; a.type = SF6CORE_ARG_BOOL; a.v.i = 1; api->obj_invoke(cmn::client(), r.part, "SetDisable", &a, 1, nullptr); }
    }
}
void post_setup_disp_units(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_setup_this) return;
    __try { post_setup_disp_units_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("setup_disp_post"); }
    tl_setup_this = nullptr;
}

// CheckCanSelect (:533-569): the hidden reset row must not take the cursor on our pages
thread_local MO* tl_cs_this = nullptr;
thread_local int32_t tl_cs_index = 0;
int pre_check_can_select(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_cs_this = g_on ? reinterpret_cast<MO*>(argv[1]) : nullptr;
    tl_cs_index = static_cast<int32_t>(reinterpret_cast<uintptr_t>(argv[2]) & 0xFFFFFFFF);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_check_can_select_inner(void** ret_val) {
    MO* param = tl_cs_this;
    if (!param || param->get_type_definition() != g_bg_param_td) return;
    const auto rows = page_rows(param);
    if (tl_cs_index < 0 || tl_cs_index >= (int32_t)rows.size()) return;
    const Row& row = rows[tl_cs_index];
    // the game's own reset row: SettingReset AND not one of our ids (our buttons are SettingReset too)
    if (!row.part || !row.type_id || row.event_type != kEventSettingReset || core::known(row.type_id)) return;
    bool ours = false;
    for (auto& r : rows) if (r.type_id && core::known(r.type_id)) { ours = true; break; }
    if (!ours) return;
    int32_t focus_tid = 0;
    if (focus_type_id(param, focus_tid) && focus_tid == row.type_id) return;   // never forbid the focused row (no dead end)
    *ret_val = nullptr;
}
void post_check_can_select(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_cs_this || !core::window_open()) return;
    __try { post_check_can_select_inner(ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("check_can_select_post"); }
}

// LoadValueEvent + ResetEvent (:574-581): the game must not load / reset our ids from its save data
int pre_skip_ours_inner(void** argv) {
    int32_t tid = 0;
    if (unit_setting(reinterpret_cast<MO*>(argv[1]), tid) && core::known(tid)) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_skip_ours(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { return pre_skip_ours_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("skip_ours_pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_skip_ours(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// TrainingManager.OpenMenu (:666-673): the Esc / Start that closes our dialog also reaches the training pause menu
int pre_open_menu(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_on && core::suppress_open_menu()) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_open_menu(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

#pragma warning(pop)

}  // namespace

// ======================== public API ========================

bool install() {
    auto* tdb = API::get()->tdb();
    if (!tdb) return false;

    core::Backend be;
    be.retain = be_retain; be.release = be_release;
    be.get_is_end = be_get_is_end; be.end_handle = be_end_handle; be.start_dialog = be_start_dialog;
    be.agent_present = be_agent_present; be.request_pause = be_request_pause;
    be.build_units = be_build_units; be.purge_roots = be_purge_roots;
    be.get_value = be_get_value; be.set_value = be_set_value; be.type_id_base = be_type_id_base;
    core::set_backend(be);

    int total = 0;
    g_bg_param_td = tdb->find_type("app.UIFlowOptionBGDialog.Param");
    if (g_bg_param_td) {
        auto* m = g_bg_param_td->find_method("FlowEvent_ResetCurrentUnits");
        if (m && m->get_function_raw() && m->add_hook(pre_reset_units, post_reset_units, false)) ++total;
        auto* md = g_bg_param_td->find_method("GetFocusDecideEventType");
        if (md && md->get_function_raw() && md->add_hook(pre_decide_event, post_decide_event, false)) ++total;
        auto* mc = g_bg_param_td->find_method("CreatedObject");
        if (mc && mc->get_function_raw() && mc->add_hook(pre_created_object, post_created_object, false)) ++total;
        auto* ms = g_bg_param_td->find_method("SetupDispUnits");
        if (ms && ms->get_function_raw() && ms->add_hook(pre_setup_disp_units, post_setup_disp_units, false)) ++total;
        auto* mcs = g_bg_param_td->find_method("CheckCanSelect");
        if (mcs && mcs->get_function_raw() && mcs->add_hook(pre_check_can_select, post_check_can_select, false)) ++total;
    }
    if (auto* vu_td = tdb->find_type("app.Option.OptionValueUnit")) {
        auto* ml = vu_td->find_method("LoadValueEvent");
        if (ml && ml->get_function_raw() && ml->add_hook(pre_skip_ours, post_skip_ours, false)) ++total;
        auto* mr = vu_td->find_method("ResetEvent");
        if (mr && mr->get_function_raw() && mr->add_hook(pre_skip_ours, post_skip_ours, false)) ++total;
    }
    if (auto* tm_td = tdb->find_type("app.training.TrainingManager")) {
        auto* mo_ = tm_td->find_method("OpenMenu(app.training.TrainingManager.MenuType, app.training.BaseParam)");
        if (!mo_) for (auto* m : tm_td->get_methods()) if (m && m->get_name() && std::string_view{m->get_name()} == "OpenMenu") { mo_ = m; break; }
        if (mo_ && mo_->get_function_raw() && mo_->add_hook(pre_open_menu, post_open_menu, false)) ++total;
    }
    // the five Param hooks and the two value-unit hooks are the engine; OpenMenu is a comfort
    g_on = total >= 7;
    cmn::journal("[options] %d / 8 hooks installed%s", total, g_on ? "" : ": off");
    return g_on;
}

bool ready() { return g_on; }
void note_fault(const char* where) { fault(where); }
void set_purge_pause_rows(bool on) { g_purge_pause_rows = on; }

int64_t add_entry(int64_t group_id, const std::string& kind, const std::string& key) {
    const core::Kind k = kind == "toggle" ? core::Kind::Toggle : kind == "choice" ? core::Kind::Choice
                       : kind == "slider" ? core::Kind::Slider : core::Kind::Button;
    return core::add_entry(group_id, k, key);
}

int64_t focused_entry() {
    int32_t tid = 0;
    if (!focus_type_id(mo(core::current_param()), tid)) return -1;
    auto e = core::entry_by_type(tid);
    return e ? e->id : -1;
}

namespace {
MO* find_row(int64_t entry_id, uint64_t param_addr) {
    auto e = core::entry_by_id(entry_id);
    MO* param = param_addr ? reinterpret_cast<MO*>(param_addr) : mo(core::current_param());
    if (!e || !e->type_id || !param) return nullptr;
    for (auto& r : page_rows(param)) if (r.part && r.type_id == e->type_id) return r.part;
    return nullptr;
}
}  // namespace

uint64_t row_control(int64_t entry_id, uint64_t param_addr) {
    MO* part = find_row(entry_id, param_addr);
    auto* gc = part ? ll::find_method_noargs(part->get_type_definition(), "get_Control") : nullptr;
    if (!gc) return 0;
    auto cr = gc->invoke(part, std::vector<void*>{});
    return cr.exception_thrown ? 0 : reinterpret_cast<uint64_t>(cr.ptr);
}

uint64_t row_part(int64_t entry_id) { return reinterpret_cast<uint64_t>(find_row(entry_id, 0)); }

// Retext one option of a spin IN PLACE: the game resolves the GUID through our hook every time it draws, so no
// unit is rebuilt and nothing is freed (:1088-1111).
bool set_option_text(int64_t entry_id, int index, const std::string& text) {
    auto e = core::entry_by_id(entry_id);
    if (!e || !e->unit || index < 1 || (size_t)index * 2 > e->option_guids.size()) return false;
    if (!guid_text::retext({ e->option_guids[(size_t)(index - 1) * 2], e->option_guids[(size_t)(index - 1) * 2 + 1] }, text)) return false;
    if ((size_t)index <= e->options.size()) e->options[(size_t)index - 1] = text;
    return true;
}

std::string status() {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "on=%d faults=%d ", (int)g_on, g_faults);
    return std::string(buf) + core::status() + " | guid_text " + guid_text::status();
}

}  // namespace dialogs::options
