#ifndef NOMINMAX
#define NOMINMAX
#endif
// dialogs::options - the native option-dialog engine (phase 4b): the game side. See options.hpp.
//
// Everything here is a game call or a hook; every decision is in options_core.cpp. Lines cited are those of
// func/NativeOptions.lua.

#include "options.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../perf.hpp"
#include "ll_common.hpp"
#include "opt_common.hpp"
#include "guid_text.hpp"
#include "options_core.hpp"
#include "../pausemenu/pausemenu.hpp"

namespace dialogs::options {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

constexpr int kMaxFaults = 5;          // same threshold as the screens modules
constexpr int kPauseType = 8;          // :713 BATTLE_MENU_PAUSE, the one that freezes the fight
constexpr int kDynamicRowFunc = 345;   // :254 TrainingFuncType.DYNAMIC (the game's own dynamic rows use it too)
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
    // the polled units share a few types: the method is looked up once per type, not once per read
    static std::unordered_map<API::TypeDefinition*, API::Method*> s_get_value;
    auto* td = mo(unit)->get_type_definition();
    auto cached = s_get_value.find(td);
    if (cached == s_get_value.end()) cached = s_get_value.emplace(td, ll::find_method_noargs(td, "get_Value")).first;
    auto* get_val = cached->second;
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

// A tab's unit list: mgr.UnitLists:call("get_Item", tab) (:271, :596)
MO* tab_list(int32_t tab) {
    auto* mgr = API::get()->get_managed_singleton("app.OptionManager");
    if (!mgr) return nullptr;
    MO* unit_lists = ll::read_field_object(mgr, "UnitLists");
    if (!unit_lists) return nullptr;
    auto* get_item = ll::find_method_full(unit_lists->get_type_definition(), "get_Item");
    if (!get_item) return nullptr;
    auto ret = get_item->invoke(unit_lists, std::vector<void*>{ ll::arg_i32(tab) });
    return (!ret.exception_thrown && ret.ptr) ? reinterpret_cast<MO*>(ret.ptr) : nullptr;
}

// The General tab's unit list (:271, :596)
MO* general_list() {
    if (!API::get()->get_managed_singleton("app.OptionManager") || !load_enums()) return nullptr;
    return tab_list(g_TabType_General);
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

// :244-260, made ownership-aware (core increment 8, ADR-0016 point 2): a DYNAMIC row of a primary tab is removed only
// when the core built it and no longer owns it. The game's own rows (a delegate: Battle Hub "End Training Session")
// and unknown rows (another engine's) stay; the purge used to remove every DYNAMIC row, the game's included.
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
            if (cd && ll::read_field_i32(cd, "_FuncType", ft) && ft == kDynamicRowFunc && pausemenu::dyn_row_removable(di.ptr))
                dl_remove->invoke(dl, std::vector<void*>{ ll::arg_i32(i) });
        }
    }
}

// ---- increment 11: a group's entries as rows of a section of the game's Options screen ----
// The units are the engine's (built below, the same as a page's rows); only their parent changes: the section's
// ChildUnits (and its setting's ChildUnitList, as attach_unit does) instead of a group unit of the core. The game
// draws them with its own rows. Their values never reach the game's save: LoadValueEvent / ResetEvent skip our ids
// (as for every entry) and SaveEvent skips the placed ones (hooked on the first build that places a group).

struct Section { int32_t tab; int32_t group_type; };
std::vector<Section> g_sections;              // every section a group was placed in (its stale rows are removed)
std::unordered_set<int32_t> g_placed_tids;   // the TypeIds the core put in a game section
bool g_save_hook = false, g_save_hook_tried = false;
size_t g_placed_live = 0;                    // placed rows in their section after the last sync
bool g_section_missing_logged = false;

int32_t list_count(MO* list) {
    auto* m = list ? ll::find_method_noargs(list->get_type_definition(), "get_Count") : nullptr;
    if (!m) return 0;
    auto r = m->invoke(list, std::vector<void*>{});
    return r.exception_thrown ? 0 : (int32_t)r.dword;
}
MO* list_item(MO* list, int32_t i) {
    auto* m = list ? ll::find_method_full(list->get_type_definition(), "get_Item") : nullptr;
    if (!m) return nullptr;
    auto r = m->invoke(list, std::vector<void*>{ ll::arg_i32(i) });
    return (!r.exception_thrown && r.ptr) ? reinterpret_cast<MO*>(r.ptr) : nullptr;
}
MO* call_obj(MO* o, const char* name) {
    auto* m = o ? ll::find_method_noargs(o->get_type_definition(), name) : nullptr;
    if (!m) return nullptr;
    auto r = m->invoke(o, std::vector<void*>{});
    return (!r.exception_thrown && r.ptr) ? reinterpret_cast<MO*>(r.ptr) : nullptr;
}

// The OptionGroupUnit whose get_GroupType() is group_type, in a tab's list or under its groups (sections nest).
MO* find_group_in(MO* list, int32_t group_type, int depth) {
    if (!list || depth > 4) return nullptr;
    const int32_t n = list_count(list);
    for (int32_t i = 0; i < n; ++i) {
        MO* u = list_item(list, i);
        auto* ggt = u ? ll::find_method_noargs(u->get_type_definition(), "get_GroupType") : nullptr;
        if (!ggt) continue;                                      // a value unit
        auto r = ggt->invoke(u, std::vector<void*>{});
        if (!r.exception_thrown && (int32_t)r.dword == group_type) return u;
        if (MO* f = find_group_in(call_obj(u, "get_ChildUnits"), group_type, depth + 1)) return f;
    }
    return nullptr;
}

// One section: the rows of ours that are not live go (all of them when !keep_live), then the live rows the section
// lacks are added at its end, in entry order. The game's own rows are never touched.
void sync_section(const Section& sec, bool keep_live) {
    MO* group = find_group_in(tab_list(sec.tab), sec.group_type, 0);
    if (!group) {
        if (keep_live && !g_section_missing_logged) {
            g_section_missing_logged = true;
            cmn::journal("[options] game section tab %d / group %d not found (yet): its placed rows wait", sec.tab, sec.group_type);
        }
        return;
    }
    MO* units = call_obj(group, "get_ChildUnits");
    MO* gsetting = call_obj(group, "get_Setting");
    MO* settings = gsetting ? ll::read_field_object(gsetting, "ChildUnitList") : nullptr;
    if (!units) return;

    std::vector<std::pair<MO*, MO*>> live;                       // unit, its setting
    if (keep_live) {
        for (auto& gp : core::groups()) {
            if (gp->place_tab != sec.tab || gp->place_section != sec.group_type) continue;
            for (auto& e : gp->entries) if (e->placed && e->unit) live.push_back({ mo(e->unit), call_obj(mo(e->unit), "get_Setting") });
        }
    }
    auto* remove_at = ll::find_method_full(units->get_type_definition(), "RemoveAt");
    auto* add_m = ll::find_method_full(units->get_type_definition(), "Add");
    std::vector<bool> seen(live.size(), false);
    for (int32_t i = list_count(units) - 1; i >= 0; --i) {
        MO* u = list_item(units, i);
        int32_t tid = 0;
        if (!u || !unit_setting(u, tid) || !g_placed_tids.count(tid)) continue;
        bool keep = false;
        for (size_t k = 0; k < live.size(); ++k) if (live[k].first == u) { keep = true; seen[k] = true; }
        if (!keep && remove_at) remove_at->invoke(units, std::vector<void*>{ ll::arg_i32(i) });
    }
    std::vector<bool> seen_s(live.size(), false);
    auto* remove_at_s = settings ? ll::find_method_full(settings->get_type_definition(), "RemoveAt") : nullptr;
    auto* add_s = settings ? ll::find_method_full(settings->get_type_definition(), "Add") : nullptr;
    for (int32_t i = settings ? list_count(settings) - 1 : -1; i >= 0; --i) {
        MO* st = list_item(settings, i);
        int32_t tid = 0;
        if (!st || !ll::read_field_i32(st, "TypeId", tid) || !g_placed_tids.count(tid)) continue;
        bool keep = false;
        for (size_t k = 0; k < live.size(); ++k) if (live[k].second == st) { keep = true; seen_s[k] = true; }
        if (!keep && remove_at_s) remove_at_s->invoke(settings, std::vector<void*>{ ll::arg_i32(i) });
    }
    for (size_t k = 0; k < live.size(); ++k) {
        if (!seen[k] && add_m) add_m->invoke(units, std::vector<void*>{ live[k].first });
        if (!seen_s[k] && add_s && live[k].second) add_s->invoke(settings, std::vector<void*>{ live[k].second });
        ++g_placed_live;
    }
}

void be_sync_placed(bool keep_live) {
    if (g_sections.empty()) return;                              // no group was ever placed: no game call
    g_placed_live = 0;
    for (const auto& sec : g_sections) sync_section(sec, keep_live);
}

void note_section(int32_t tab, int32_t group_type) {
    for (const auto& s : g_sections) if (s.tab == tab && s.group_type == group_type) return;
    g_sections.push_back({ tab, group_type });
}

void install_save_hook();
void sections_sync();                         // increment 13 (below)

// The empty list templates, from the first unit of the General tab (:272-274): a List<System.Guid> and a
// List<app.Option.OptionSettingUnit> of no item each (GetRange(0, 0)), cloned for every unit made.
bool empty_lists(MO*& guids, MO*& settings) {
    MO* parent_list = general_list();
    if (!parent_list) return false;
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
    guids = empty_range(ll::read_field_object(first_setting, "ValueMessageList"));
    settings = empty_range(ll::read_field_object(first_setting, "ChildUnitList"));
    return guids && settings;
}

// :262-392 (radio groups and the composite window are not ported)
bool be_build_units() {
    if (!API::get()->get_managed_singleton("app.OptionManager")) return false;
    if (g_purge_pause_rows) purge_pause_menu_rows();
    if (!general_list()) return false;
    guid_text::begin_tree();

    // the empty list templates, from the first unit of the tab (:272-274)
    if (!empty_lists(g_empty_guids, g_empty_settings)) return false;

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
    bool any_placed = false;
    for (auto& gp : core::groups()) {
        auto& g = *gp;
        if (g.section_tab >= 0) { core::build_set_group(g, nullptr); continue; }   // increment 13: a section, no page, no row
        const std::string gkey = core::id_key_of(g);
        const bool placed = g.place_tab >= 0;                    // increment 11: rows of a game section, no page
        MO* gunit = nullptr;
        if (placed) {
            any_placed = true;
            note_section(g.place_tab, g.place_section);
            core::build_set_group(g, nullptr);
        } else {
            MO* gd = create_setting("g:" + gkey, tid);
            if (!gd) return false;
            guid_text::set_field(gd, "TitleMessage", guid_text::alloc(g.title));
            ll::write_field_i32(gd, "InputType", g_InputType_Button1);
            ll::write_field_i32(gd, "EventType", g_EventType_OpenBattleHud);     // HUD window over the fight
            gunit = make_unit(gd, g.desc, {}, nullptr);
            if (!gunit) return false;
            attach_unit(root, gunit);
            core::build_set_group(g, gunit);
        }

        std::vector<std::shared_ptr<core::Entry>> ordered;
        for (auto& e : g.entries) if (e->kind != core::Kind::Button) ordered.push_back(e);
        for (auto& e : g.entries) if (e->kind == core::Kind::Button) ordered.push_back(e);   // buttons at the bottom
        for (auto& e : ordered) {
            if (placed && e->kind == core::Kind::Button) continue;   // a section row holds a value
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
            if (placed) {
                u->add_ref();                                        // held: the game may rebuild its lists without it
                g_placed_tids.insert(tid);
            }
        }
    }
    if (any_placed) { install_save_hook(); be_sync_placed(true); }
    sections_sync();                                             // increment 13 (nothing without a section)
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
    be_sync_placed(keep_live);                                   // increment 11 (nothing without a placed group)
    guid_text::purge(keep_live);
}

// ======================== increment 13: a client's section in the left list of a game Options tab ========================
// A section is a client's group shown as one more line of the left list of an Options tab, its right side the client's
// page. Read 08/11 on Options > Interface (read-only probes): OptionManager.UnitLists[tab] is the list the screen copies
// into OptionMenuParam.CurrentMainUnitLists when it fills the tab (another list object; the left list's parts,
// mMainListGroup.GetChild(k), are aligned with that copy: UnitData == CurrentMainUnitLists[k]); a group's setting TypeId is
// its GroupType; for a group whose EventType is not OpenSubMenu (Battle HUD, 7) the game hides the sub list and the radio
// group itself (ForceInvisible) and shows nothing on the right.
//   - Ours: an app.Option.OptionGroupUnit, setting DataType Group, InputType Button_Type1, EventType and ExtraEventType
//     Invalid, empty ValueMessageList / ChildUnitList, SupportModeType / SupportPlatform / IsOnlineSetting copied from the
//     section it goes before. No child: nothing for the game to load, reset or save.
//   - Made once per section (held), kept in UnitLists[tab] just before the game's section `before` at each build and every
//     kSecSyncEvery frames (re-inserted after a rebuild of the game's lists, stale copies dropped by TypeId).
//     CurrentMainUnitLists is never touched: the section shows from the next time the game fills the tab.
//   - Its title and description are GUID texts owned by the sections (guid_text::alloc_owned): the engine's purge never
//     blanks them; a new text is written in place (retext_owned).
//   - The screen is read once per frame (agent "OptionSettingMenu" -> <InputDelegates> -> <Flow> -> get_MenuParam()), every
//     kSecLookEvery frames while it is not up. No write to the screen here: the client draws, the core only blocks input.
//   - A hold blocks the screen's agent (ui_set_block_input). H2, a pre-hook on Flow_MainList.InputDelegates.EventCancel,
//     swallows the left list's Back while a section is held (it would leave the Options screen, should the block leak).
//     H1, a post-hook on OptionMenuParam.InputEvent_DecideList, counts the Confirm presses on a section (its result forced
//     to Invalid). Both are installed only once the code check (R18) found no other method entering their code, one per
//     frame; the result is kept in reframework/data/sf6core/opt_codecheck.txt per game executable.
//   - 5 faults, or the option hooks switched off: every section withdrawn and every hold given back.

constexpr int kMaxSections = 8;
constexpr int kSecMaxFaults = 5;
constexpr int kSecMaxTries = 5;
constexpr uint32_t kSecSyncEvery = 60;
constexpr uint32_t kSecLookEvery = 15;
constexpr uint32_t kSecAreaEvery = 30;
constexpr int32_t kMenuMain = 0;               // app.UIOptionSettingMenu.eMenuState.MAIN (read 08/11)
constexpr int32_t kListMain = 0;               // app.UIOptionSettingMenu.eListState.MainList (read 08/11)
constexpr const char* kOptMenuAgent = "OptionSettingMenu";
constexpr float kAreaDefault[4] = { 912.0f, 145.0f, 910.0f, 833.0f };   // c_bg_right/e_bg, read 08/11

char g_sec_tag = 0;                            // the sections' GUID texts' owner (never purged by the engine)
const guid_text::Owner kSecOwner = &g_sec_tag;
bool sec_keep(guid_text::Owner) { return true; }

int32_t g_DataType_Group = 0, g_EventType_Invalid = 0, g_ExtraType_Invalid = 0;
bool g_sec_enums = false, g_sec_enums_tried = false;

struct Sec {
    int64_t gid = -1;
    int slot = -1;                             // its place in g_slot_unit / g_slot_decides (read by H1)
    int32_t tab = -1, before = -1, tid = 0;
    std::string key, title, desc;              // title / desc: the texts its GUIDs carry
    MO* setting = nullptr;                     // held
    MO* unit = nullptr;                        // held
    int32_t in_tab = -1;                       // the tab list it is in (-1: none)
    guid_text::GuidBytes tg{}, dg{};
    bool texts = false, fail_logged = false, held = false;
    int tries = 0;                             // unit makings that failed (given up after kSecMaxTries)
    int32_t list_index = -1;                   // its index in CurrentMainUnitLists, this frame
    MO* part = nullptr;                        // the left list's part that shows it (valid with g_parts_of)
    SF6CoreOptSectionState st{};               // this frame's
};
std::vector<Sec> g_secs;
std::atomic<MO*> g_slot_unit[kMaxSections] = {};
std::atomic<uint32_t> g_slot_decides[kMaxSections] = {};
std::atomic<int> g_slot_live{0};               // slots holding a unit (H1 does nothing without one)
std::atomic<int> g_held{0};                    // sections held: H2 swallows the left list's Back
std::atomic<uint32_t> g_cancels{0};            // Backs swallowed by H2
std::atomic<int> g_sec_faults{0};
std::atomic<bool> g_sec_off{false};
MO* g_held_agent = nullptr;                    // the agent the holds blocked
bool g_sec_any = false;                        // a section was placed once: the frame has something to do
bool g_secs_dirty = false, g_slots_full_logged = false, g_no_h2_logged = false;
uint32_t g_sec_frame = 0, g_sec_synced_at = 0, g_sec_looked_at = 0, g_area_at = 0;
std::unordered_set<int32_t> g_sec_tids;        // every section TypeId of this load
std::vector<int32_t> g_sec_tabs;               // every tab a section was put in

void sec_fault(const char* where) {            // any thread
    const int n = g_sec_faults.fetch_add(1) + 1;
    cmn::journal("[options] sections: fault in %s (%d)", where, n);
    if (n >= kSecMaxFaults && !g_sec_off.exchange(true))
        cmn::log_warn("[options] sections: too many faults: every section withdrawn, every hold given back");
}

bool sec_enums() {
    if (g_sec_enums_tried) return g_sec_enums;
    g_sec_enums_tried = true;
    bool ok = load_enums();
    ok &= load_enum_value("app.Option.SettingDataType", "Group", g_DataType_Group);
    ok &= load_enum_value("app.Option.DecideEventType", "Invalid", g_EventType_Invalid);
    ok &= load_enum_value("app.Option.ExtraButtonEventType", "Invalid", g_ExtraType_Invalid);
    g_sec_enums = ok;
    if (!ok) cmn::log_warn("[options] sections: app.Option enums not found: no section");
    return ok;
}

// No-argument methods of the screen's objects, looked up once per type (the screen is read every frame)
struct MKey {
    API::TypeDefinition* td; const char* name;
    bool operator==(const MKey& o) const { return td == o.td && name == o.name; }
};
struct MKeyHash {
    size_t operator()(const MKey& k) const { return std::hash<const void*>()(k.td) ^ (std::hash<const void*>()(k.name) << 1); }
};
std::unordered_map<MKey, API::Method*, MKeyHash> g_mcache;
API::Method* m0(MO* o, const char* name) {
    if (!o) return nullptr;
    const MKey k{ o->get_type_definition(), name };
    auto it = g_mcache.find(k);
    if (it != g_mcache.end()) return it->second;
    API::Method* m = ll::find_method_noargs(k.td, name);
    g_mcache.emplace(k, m);
    return m;
}
MO* sc_obj(MO* o, const char* name) {
    API::Method* m = m0(o, name);
    if (!m) return nullptr;
    auto r = m->invoke(o, std::vector<void*>{});
    return (!r.exception_thrown && r.ptr) ? reinterpret_cast<MO*>(r.ptr) : nullptr;
}
bool sc_bool(MO* o, const char* name, bool& out) {
    API::Method* m = m0(o, name);
    if (!m) return false;
    auto r = m->invoke(o, std::vector<void*>{});
    if (r.exception_thrown) return false;
    out = r.byte != 0;
    return true;
}
bool sc_i32(MO* o, const char* name, int32_t& out) {
    API::Method* m = m0(o, name);
    if (!m) return false;
    auto r = m->invoke(o, std::vector<void*>{});
    if (r.exception_thrown) return false;
    out = (int32_t)r.dword;
    return true;
}
bool sc_floats(MO* o, const char* name, float* out, size_t n) {   // a value-type result (via.vec3, via.Size)
    API::Method* m = m0(o, name);
    if (!m) return false;
    auto r = m->invoke(o, std::vector<void*>{});
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * n);
    return true;
}

Sec* find_sec(int64_t gid) {
    for (auto& s : g_secs) if (s.gid == gid) return &s;
    return nullptr;
}
int32_t group_type_of(MO* u) {                 // an OptionGroupUnit's GroupType, -1 for any other unit
    int32_t gt = -1;
    return (u && sc_i32(u, "get_GroupType", gt)) ? gt : -1;
}
void note_tab(int32_t tab) {
    for (int32_t t : g_sec_tabs) if (t == tab) return;
    g_sec_tabs.push_back(tab);
}
bool list_remove(MO* list, MO* u) {
    auto* m = list ? ll::find_method_full(list->get_type_definition(), "Remove") : nullptr;
    if (!m) return false;
    auto r = m->invoke(list, std::vector<void*>{ u });
    return !r.exception_thrown;
}
bool list_insert(MO* list, int32_t at, MO* u) {
    auto* m = list ? method_n(list->get_type_definition(), "Insert(System.Int32, app.Option.OptionUnitBase)", "Insert", 2) : nullptr;
    if (!m) return false;
    auto r = m->invoke(list, std::vector<void*>{ ll::arg_i32(at), u });
    return !r.exception_thrown;
}
bool list_add(MO* list, MO* u) {
    auto* m = list ? ll::find_method_full(list->get_type_definition(), "Add") : nullptr;
    if (!m) return false;
    auto r = m->invoke(list, std::vector<void*>{ u });
    return !r.exception_thrown;
}

bool agent_alive(MO* ag) {
    const auto* api = cmn::core();
    if (!ag || !api || !api->ui_agents) return false;
    void* buf[4] = {};
    const uint32_t n = api->ui_agents(kOptMenuAgent, buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) if (buf[i] == ag) return true;
    return false;
}
void unblock_if_free() {                       // the last hold went: the agent's input back
    if (g_held.load() > 0 || !g_held_agent) return;
    MO* ag = g_held_agent;
    g_held_agent = nullptr;
    const auto* api = cmn::core();
    if (agent_alive(ag) && api && api->ui_set_block_input) api->ui_set_block_input(cmn::client(), ag, 0);
}
void sec_release(Sec& s, const char* why) {
    if (!s.held) return;
    s.held = false;
    g_held.fetch_sub(1);
    s.st.held = 0;
    cmn::journal("[options] section %lld: hold given back (%s)", (long long)s.gid, why);
    unblock_if_free();
}

void sec_unlist(Sec& s) {
    if (s.in_tab >= 0 && s.unit) if (MO* list = tab_list(s.in_tab)) list_remove(list, s.unit);
    s.in_tab = -1;
}
// The section goes: hold, list, texts, unit. The screen's own copy of the list may still show it until the game fills the
// tab again (that copy holds the unit).
void sec_drop(Sec& s, const char* why) {
    sec_release(s, why);
    sec_unlist(s);
    if (s.slot >= 0 && g_slot_unit[s.slot].exchange(nullptr)) g_slot_live.fetch_sub(1);
    if (s.texts) {
        guid_text::release_owned(kSecOwner, s.tg);
        guid_text::release_owned(kSecOwner, s.dg);
        s.texts = false;
    }
    if (s.unit) { s.unit->release(); s.unit = nullptr; }
    if (s.setting) { s.setting->release(); s.setting = nullptr; }
    cmn::journal("[options] section %lld withdrawn (%s)", (long long)s.gid, why);
}

// The section's unit, made once: its setting as described above, MakeUnitData, its children list checked.
MO* make_section_unit(Sec& s, MO* list) {
    MO* guids = nullptr, *settings = nullptr;
    if (!empty_lists(guids, settings)) return nullptr;
    MO* src = nullptr;                                           // the section it goes before, else the tab's first group
    for (int32_t i = 0, n = list_count(list); i < n; ++i) {
        MO* u = list_item(list, i);
        const int32_t gt = group_type_of(u);
        if (gt < 0) continue;
        if (!src) src = u;
        if (s.before >= 0 && gt == s.before) { src = u; break; }
    }
    MO* src_setting = src ? call_obj(src, "get_Setting") : nullptr;
    int32_t tid = 0;
    MO* d = create_setting("s:" + std::to_string(s.gid) + ":" + s.key, tid);   // held
    if (!d) return nullptr;
    ll::write_field_i32(d, "_DataType", g_DataType_Group);
    ll::write_field_i32(d, "InputType", g_InputType_Button1);
    ll::write_field_i32(d, "EventType", g_EventType_Invalid);
    ll::write_field_i32(d, "ExtraEventType", g_ExtraType_Invalid);
    if (src_setting) {
        int32_t v = 0;
        if (ll::read_field_i32(src_setting, "SupportModeType", v)) ll::write_field_i32(d, "SupportModeType", v);
        if (ll::read_field_i32(src_setting, "SupportPlatform", v)) ll::write_field_i32(d, "SupportPlatform", v);
        uint8_t b = 0;
        if (ll::read_field_raw(src_setting, "IsOnlineSetting", &b, 1)) ll::write_field_raw(d, "IsOnlineSetting", &b, 1);
    }
    auto texts_and_lists = [&](MO* st) {
        guid_text::set_field(st, "TitleMessage", s.tg);
        guid_text::set_field(st, "DescriptionMessage", s.dg);
        MO* vml = clone_list(guids);
        MO* cul = clone_list(settings);
        if (vml) ll::write_field_raw(st, "ValueMessageList", &vml, sizeof(void*));
        if (cul) ll::write_field_raw(st, "ChildUnitList", &cul, sizeof(void*));
    };
    texts_and_lists(d);                                          // before MakeUnitData: its Setup finds empty lists
    auto* make_m = ll::find_method_noargs(d->get_type_definition(), "MakeUnitData");
    auto ret = make_m ? make_m->invoke(d, std::vector<void*>{}) : reframework::InvokeRet{};
    if (!make_m || ret.exception_thrown || !ret.ptr) { d->release(); return nullptr; }
    MO* unit = reinterpret_cast<MO*>(ret.ptr);
    static API::TypeDefinition* s_group_td = API::get()->tdb()->find_type("app.Option.OptionGroupUnit");
    if (!s_group_td || unit->get_type_definition() != s_group_td) { d->release(); return nullptr; }
    unit->add_ref();                                             // held: the game may rebuild its lists without it
    MO* us = call_obj(unit, "get_Setting");
    if (us && us != d) texts_and_lists(us);
    if (!call_obj(unit, "get_ChildUnits")) {                     // never seen: an empty List<OptionUnitBase> of its own
        MO* src_kids = src ? call_obj(src, "get_ChildUnits") : nullptr;
        auto* gr_m = src_kids ? ll::find_method_full(src_kids->get_type_definition(), "GetRange") : nullptr;
        auto gr = gr_m ? gr_m->invoke(src_kids, std::vector<void*>{ ll::arg_i32(0), ll::arg_i32(0) }) : reframework::InvokeRet{};
        auto* set_m = ll::find_method_full(unit->get_type_definition(), "set_ChildUnits");
        if (gr_m && !gr.exception_thrown && gr.ptr && set_m) {
            MO* empty = reinterpret_cast<MO*>(gr.ptr);
            empty->add_ref();
            set_m->invoke(unit, std::vector<void*>{ empty });
        }
        if (!call_obj(unit, "get_ChildUnits")) { unit->release(); d->release(); return nullptr; }
    }
    s.setting = d;
    s.tid = tid;
    g_sec_tids.insert(tid);
    return unit;
}

// Just before the game's section `before` (at the end when the tab has none); a unit already there stays.
void sec_place(MO* list, Sec& s) {
    int32_t at = -1, before_at = -1;
    const int32_t n = list_count(list);
    for (int32_t i = 0; i < n; ++i) {
        MO* u = list_item(list, i);
        if (u == s.unit) { if (at < 0) at = i; continue; }
        if (s.before >= 0 && before_at < 0 && group_type_of(u) == s.before) before_at = i;
    }
    note_tab(s.tab);
    if (at >= 0 && (before_at < 0 || at == before_at - 1)) { s.in_tab = s.tab; return; }
    if (at >= 0) { list_remove(list, s.unit); if (before_at > at) --before_at; }
    const bool ok = before_at >= 0 ? list_insert(list, before_at, s.unit) : list_add(list, s.unit);
    s.in_tab = ok ? s.tab : -1;
    cmn::journal("[options] section %lld (\"%s\", type id %d) %s tab %d at %d (%s)", (long long)s.gid, s.title.c_str(), s.tid,
                 ok ? "put in" : "NOT put in", s.tab, before_at >= 0 ? before_at : n,
                 before_at >= 0 ? "before its section" : (s.before >= 0 ? "its section not found: at the end" : "at the end"));
}

// Units of ours the tabs' lists hold and no live section owns (a moved or removed section, a copy left by a rebuild)
void sec_remove_stale() {
    for (int32_t tab : g_sec_tabs) {
        MO* list = tab_list(tab);
        auto* remove_at = list ? ll::find_method_full(list->get_type_definition(), "RemoveAt") : nullptr;
        if (!remove_at) continue;
        std::vector<MO*> seen;
        for (int32_t i = list_count(list) - 1; i >= 0; --i) {
            MO* u = list_item(list, i);
            int32_t tid = 0;
            if (!u || !unit_setting(u, tid) || !g_sec_tids.count(tid)) continue;
            bool keep = false;
            for (auto& s : g_secs) if (s.unit == u && s.in_tab == tab) keep = true;
            for (MO* x : seen) if (x == u) keep = false;         // the same unit twice: one goes
            if (keep) { seen.push_back(u); continue; }
            remove_at->invoke(list, std::vector<void*>{ ll::arg_i32(i) });
            cmn::journal("[options] sections: a stale unit (type id %d) removed from tab %d", tid, tab);
        }
    }
}

int free_slot() {
    for (int i = 0; i < kMaxSections; ++i) {
        bool used = false;
        for (auto& s : g_secs) if (s.slot == i) used = true;
        if (!used) return i;
    }
    return -1;
}

void sections_sync_inner() {
    if (cmn::closing()) return;                                  // the game is closing: no game call
    g_sec_synced_at = g_sec_frame;
    g_secs_dirty = false;
    const bool off = g_sec_off.load() || !g_on;
    std::vector<core::SectionDef> defs;
    if (!off) defs = core::sections();
    for (size_t i = g_secs.size(); i-- > 0;) {
        bool live = false;
        for (auto& d : defs) if (d.gid == g_secs[i].gid) { live = true; break; }
        if (live) continue;
        sec_drop(g_secs[i], off ? "the sections are off" : "no section any more");
        g_secs.erase(g_secs.begin() + (std::ptrdiff_t)i);
    }
    if (!defs.empty() && API::get()->get_managed_singleton("app.OptionManager") && sec_enums()) {
        for (auto& d : defs) {
            Sec* s = find_sec(d.gid);
            if (!s) {
                const int slot = free_slot();
                if (slot < 0) {
                    if (!g_slots_full_logged) { g_slots_full_logged = true; cmn::log_warn("[options] sections: more than %d: the others wait", kMaxSections); }
                    continue;
                }
                Sec n;
                n.gid = d.gid; n.slot = slot; n.key = d.key;
                n.st.struct_size = sizeof n.st;
                n.st.menu_state = n.st.list_state = n.st.tab = -1;
                g_slot_decides[slot].store(0);
                g_secs.push_back(n);
                s = &g_secs.back();
            }
            if (s->tab != d.tab || s->before != d.before) {
                sec_release(*s, "the section moved");
                sec_unlist(*s);
                s->tab = d.tab; s->before = d.before;
            }
            if (!s->texts) {
                if (!guid_text::alloc_owned(kSecOwner, d.title, sec_keep, s->tg, nullptr)) continue;
                if (!guid_text::alloc_owned(kSecOwner, d.desc, sec_keep, s->dg, nullptr)) { guid_text::release_owned(kSecOwner, s->tg); continue; }
                s->texts = true; s->title = d.title; s->desc = d.desc;
            } else {
                if (d.title != s->title && guid_text::retext_owned(kSecOwner, s->tg, d.title)) s->title = d.title;
                if (d.desc != s->desc && guid_text::retext_owned(kSecOwner, s->dg, d.desc)) s->desc = d.desc;
            }
            MO* list = tab_list(d.tab);
            if (!list) continue;
            if (!s->unit) {
                if (s->tries >= kSecMaxTries) continue;
                s->unit = make_section_unit(*s, list);
                if (!s->unit) {
                    if (++s->tries >= kSecMaxTries && !s->fail_logged) {
                        s->fail_logged = true;
                        cmn::log_warn("[options] section %lld: its unit could not be made (%d tries): not shown", (long long)s->gid, s->tries);
                    }
                    continue;
                }
                if (!g_slot_unit[s->slot].exchange(s->unit)) g_slot_live.fetch_add(1);
            }
            sec_place(list, *s);
        }
    }
    sec_remove_stale();
}

// The left list's part showing `unit`: GetChild(hint) first, else every child
MO* find_part(MO* group, MO* unit, int32_t hint) {
    auto* get_child = group ? ll::find_method_full(group->get_type_definition(), "GetChild(System.Int32)") : nullptr;
    if (!get_child) return nullptr;
    auto at = [&](int32_t i) -> MO* {
        auto r = get_child->invoke(group, std::vector<void*>{ ll::arg_i32(i) });
        return (!r.exception_thrown && r.ptr) ? reinterpret_cast<MO*>(r.ptr) : nullptr;
    };
    if (MO* p = hint >= 0 ? at(hint) : nullptr; p && ll::read_field_object(p, "UnitData") == unit) return p;
    int32_t n = 0;
    sc_i32(group, "GetChildCount", n);
    for (int32_t i = 0; i < n && i < 64; ++i)
        if (MO* p = at(i); p && ll::read_field_object(p, "UnitData") == unit) return p;
    return nullptr;
}

// ---- the screen, once per frame ----
struct Screen {
    bool up = false;
    MO* agent = nullptr; MO* cm = nullptr; MO* param = nullptr; MO* right = nullptr;
    MO* main_list = nullptr; MO* main_group = nullptr; MO* focus = nullptr;
    int32_t tab = -1, menu = -1, list = -1;
    float area[4] = {};
};
Screen g_scr;
MO* g_area_of = nullptr;                       // the panel g_area was read from
MO* g_parts_of = nullptr;                      // the param the sections' parts were found under
float g_area[4] = {};
API::TypeDefinition* g_menu_param_td = nullptr;

void read_area(MO* right, float out[4]) {      // c_bg_right/e_bg: its global place (top left) and size
    std::memcpy(out, kAreaDefault, sizeof kAreaDefault);
    MO* c = sc_obj(right, "get_Child");
    for (int g = 0; c && g < 16; ++g, c = sc_obj(c, "get_Next")) {
        if (ll::managed_string_to_utf8(sc_obj(c, "get_Name")) != "e_bg") continue;
        float pos[3] = {}, size[2] = {};
        if (sc_floats(c, "get_GlobalPosition", pos, 3) && sc_floats(c, "get_Size", size, 2) && size[0] > 0 && size[1] > 0) {
            out[0] = pos[0]; out[1] = pos[1]; out[2] = size[0]; out[3] = size[1];
        }
        return;
    }
}

void sec_read_screen() {
    if (!g_scr.up && g_sec_looked_at && g_sec_frame - g_sec_looked_at < kSecLookEvery) return;   // not up: looked again soon
    g_sec_looked_at = g_sec_frame;
    Screen sc;
    const auto* api = cmn::core();
    void* buf[4] = {};
    const uint32_t n = (api && api->ui_agents) ? api->ui_agents(kOptMenuAgent, buf, 4) : 0;
    if (!g_menu_param_td) g_menu_param_td = API::get()->tdb()->find_type("app.UIOptionSettingMenu.OptionMenuParam");
    for (uint32_t i = 0; i < n && i < 4 && !sc.up; ++i) {
        MO* ag = reinterpret_cast<MO*>(buf[i]);
        MO* go = sc_obj(ag, "get_GameObject");
        bool draw = false, showed = false;
        if (!go || !sc_bool(go, "get_DrawSelf", draw) || !draw) continue;
        MO* dl = ll::read_field_object(ag, "<InputDelegates>k__BackingField");
        MO* flow = dl ? ll::read_field_object(dl, "<Flow>k__BackingField") : nullptr;
        MO* p = flow ? sc_obj(flow, "get_MenuParam") : nullptr;
        if (!p || !g_menu_param_td || p->get_type_definition() != g_menu_param_td) continue;
        if (!ll::read_field_bool(p, "<IsShowed>k__BackingField", showed) || !showed) continue;
        sc.up = true; sc.agent = ag; sc.param = p;
    }
    if (sc.up) {
        sc.cm = sc_obj(sc.agent, "get_ControlMain");
        ll::read_field_i32(sc.param, "<MenuState>k__BackingField", sc.menu);
        ll::read_field_i32(sc.param, "<ListState>k__BackingField", sc.list);
        if (!sc_i32(sc.param, "get_CurrentTabType", sc.tab)) sc.tab = -1;
        sc.main_list = ll::read_field_object(sc.param, "CurrentMainUnitLists");
        sc.main_group = ll::read_field_object(sc.param, "mMainListGroup");
        sc.right = ll::read_field_object(sc.param, "mCtrlBGRight");
        sc.focus = sc_obj(sc.param, "GetMainListFocusUnit");
        if (sc.right && (sc.right != g_area_of || g_sec_frame - g_area_at >= kSecAreaEvery)) {
            g_area_of = sc.right;
            g_area_at = g_sec_frame;
            read_area(sc.right, g_area);
        }
        if (sc.right) std::memcpy(sc.area, g_area, sizeof g_area);
    }
    if (sc.param != g_parts_of) {                                // another screen: the parts found under the old one go
        g_parts_of = sc.param;
        for (auto& s : g_secs) { s.part = nullptr; s.list_index = -1; }
    }
    g_scr = sc;
}

void sec_states() {
    for (auto& s : g_secs) {
        SF6CoreOptSectionState st{};
        st.struct_size = sizeof st;
        st.menu_state = st.list_state = st.tab = -1;
        st.held = s.held ? 1 : 0;
        st.decides = s.slot >= 0 ? g_slot_decides[s.slot].load(std::memory_order_relaxed) : 0;
        if (g_scr.up) {
            st.menu_state = g_scr.menu; st.list_state = g_scr.list; st.tab = g_scr.tab;
            st.agent = g_scr.agent; st.control_main = g_scr.cm; st.right_panel = g_scr.right;
            std::memcpy(st.area, g_scr.area, sizeof st.area);
            if (s.unit && g_scr.main_list && g_scr.tab == s.tab) {
                if (!(s.list_index >= 0 && list_item(g_scr.main_list, s.list_index) == s.unit)) {
                    s.list_index = -1;
                    s.part = nullptr;
                    for (int32_t i = 0, n = list_count(g_scr.main_list); i < n && i < 64; ++i)
                        if (list_item(g_scr.main_list, i) == s.unit) { s.list_index = i; break; }
                }
                if (s.list_index >= 0) {
                    st.shown = 1;
                    st.focused = (g_scr.menu == kMenuMain && g_scr.list == kListMain && g_scr.focus == s.unit) ? 1 : 0;
                    if (!s.part || ll::read_field_object(s.part, "UnitData") != s.unit) s.part = find_part(g_scr.main_group, s.unit, s.list_index);
                    st.item = s.part ? sc_obj(s.part, "get_Control") : nullptr;
                }
            } else {
                s.list_index = -1;
                s.part = nullptr;
            }
        }
        s.st = st;
    }
}

void sec_holds() {
    for (auto& s : g_secs) {
        if (!s.held) continue;
        if (!g_scr.up) sec_release(s, "the Options screen went");
        else if (g_scr.agent != g_held_agent) sec_release(s, "another Options screen");
        else if (!s.st.shown) sec_release(s, "the section is not in the left list any more");
    }
    if (g_held.load() == 0 && g_held_agent) unblock_if_free();
}

// ---- R18: the code check of the sections' hooks, on a thread of its own, and its cache ----
// As the pause-menu engine's (pausemenu.cpp, "R18"): every method of the game is compared with each target (its entry,
// and the code its jump thunks lead to, as REFramework's add_hook follows them); a target another method enters is never
// hooked. The type database and the game's code are static and only read.
int pre_main_cancel(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long);
int pre_decide_list(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long);
void post_decide_list(void**, REFrameworkTypeDefinitionHandle, unsigned long long);
void post_sec_none(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

struct SecHook {
    const char* type; const char* name; const char* tag;
    int (*pre)(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long);
    void (*post)(void**, REFrameworkTypeDefinitionHandle, unsigned long long);
    const char* loss;
    API::Method* m = nullptr;
    uint32_t index = 0;
    uintptr_t raw = 0, code = 0;
    int shared = -1;
    long long other = -1;
    bool tried = false, hooked = false;
};
SecHook g_hooks[2] = {
    { "app.UIOptionSettingMenu.Flow_MainList.InputDelegates", "EventCancel", "H2", pre_main_cancel, post_sec_none,
      "a Back that leaks through a hold's input block leaves the Options screen" },
    { "app.UIOptionSettingMenu.OptionMenuParam", "InputEvent_DecideList", "H1", pre_decide_list, post_decide_list,
      "no Confirm count: SF6CoreOptSectionState.decides stays 0" },
};
std::atomic<int> g_check_state{0};             // 0 not started, 1 running, 2 done
bool g_check_ok = false, g_check_cache = false, g_check_written = false, g_check_logged = false;
uint64_t g_check_ms = 0;
uint32_t g_check_methods = 0;
char g_check_key[200] = {};
char g_check_note[120] = {};
constexpr const char* kSecCacheFile = "opt_codecheck.txt";
constexpr const char* kSecCacheHeader = "sf6core option code check 1";

uintptr_t sec_code_of(uintptr_t p) {           // jmp rel32 / rel8 / jmp [rip+disp32] followed, as add_hook does
    __try {
        for (int hop = 0; hop < 4 && p; ++hop) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            if (b[0] == 0xE9) p = p + 5 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 1);
            else if (b[0] == 0xEB) p = p + 2 + (intptr_t) * reinterpret_cast<const int8_t*>(b + 1);
            else if (b[0] == 0xFF && b[1] == 0x25) p = *reinterpret_cast<const uintptr_t*>(p + 6 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 2));
            else break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return p;
}
uintptr_t sec_exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
long long sec_rva(uintptr_t a) { return a ? (long long)(a - sec_exe_base()) : 0; }
long long sec_code_rva(uintptr_t a) {          // outside the exe (another module's hook): -1
    if (!a) return 0;
    const auto* base = reinterpret_cast<const uint8_t*>(sec_exe_base());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uintptr_t size = (uintptr_t)nt->OptionalHeader.SizeOfImage;
    return a >= sec_exe_base() && a - sec_exe_base() < size ? (long long)(a - sec_exe_base()) : -1;
}
void sec_data_path(const char* name, char* out, size_t n) {    // reframework/data/sf6core/<name>
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* sep = std::strrchr(exe, '\\')) *sep = '\0';
    std::snprintf(out, n, "%s\\reframework\\data\\sf6core\\%s", exe, name);
}
void sec_exe_key(char* out, size_t n, uint32_t methods) {      // the pause-menu cache's key
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    unsigned long long size = 0, time = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        time = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    std::snprintf(out, n, "exe %08lX %08lX %08lX size %llu time %llX methods %u",
                  (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage,
                  (unsigned long)nt->OptionalHeader.CheckSum, size, time, methods);
}
std::string sec_hook_label(const SecHook& h) { return std::string(h.type) + "." + h.name; }
std::string sec_method_label(API::Method* m) {
    if (!m) return "?";
    auto* t = m->get_declaring_type();
    return (t ? t->get_full_name() : std::string("?")) + "." + (m->get_name() ? m->get_name() : "?");
}
void sec_chomp(char* s) {
    size_t n = std::strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

// The cached sharing of each target, when the file is of this game and each target is where it was
bool sec_cache_read() {
    char path[MAX_PATH];
    sec_data_path(kSecCacheFile, path, sizeof path);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) { std::snprintf(g_check_note, sizeof g_check_note, "no cache file yet"); return false; }
    char line[600];
    const char* why = nullptr;
    int shared[2] = { -1, -1 };
    long long other[2] = { -1, -1 };
    if (!std::fgets(line, sizeof line, f)) why = "empty cache file";
    else { sec_chomp(line); if (std::strcmp(line, kSecCacheHeader) != 0) why = "another cache format"; }
    if (!why) {
        if (!std::fgets(line, sizeof line, f)) why = "no key";
        else { sec_chomp(line); if (std::strncmp(line, "key ", 4) != 0 || std::strcmp(line + 4, g_check_key) != 0) why = "another game executable"; }
    }
    for (size_t i = 0; !why && i < 2; ++i) {
        const SecHook& h = g_hooks[i];
        if (!std::fgets(line, sizeof line, f)) { why = "targets missing"; break; }
        sec_chomp(line);
        char* bar = std::strchr(line, '|');
        if (!bar) { why = "unreadable target line"; break; }
        *bar = 0;
        if (sec_hook_label(h) != bar + 1) { why = "the hooked methods changed"; break; }
        int found = 0;
        unsigned index = 0;
        long long raw = 0, code = 0;
        if (sscanf_s(line, "t %d %u %lld %lld %d %lld", &found, &index, &raw, &code, &shared[i], &other[i]) != 6) { why = "unreadable target line"; break; }
        if ((found != 0) != (h.m != nullptr) || index != h.index || raw != sec_rva(h.raw) || code != sec_code_rva(h.code)) {
            why = "a hooked method's entry or code moved";
            break;
        }
    }
    std::fclose(f);
    if (why) { std::snprintf(g_check_note, sizeof g_check_note, "%s", why); return false; }
    for (size_t i = 0; i < 2; ++i) { g_hooks[i].shared = shared[i]; g_hooks[i].other = other[i]; }
    return true;
}
bool sec_cache_write() {
    char path[MAX_PATH], tmp[MAX_PATH];
    sec_data_path(kSecCacheFile, path, sizeof path);
    std::snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "w") != 0 || !f) return false;
    std::fprintf(f, "%s\nkey %s\n", kSecCacheHeader, g_check_key);
    for (const SecHook& h : g_hooks)
        std::fprintf(f, "t %d %u %lld %lld %d %lld|%s\n", h.m ? 1 : 0, h.index, sec_rva(h.raw), sec_code_rva(h.code), h.shared, h.other,
                     sec_hook_label(h).c_str());
    const bool ok = std::fclose(f) == 0;
    return ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
}
bool sec_scan() {                              // every method of the game against the targets
    auto* tdb = API::get()->tdb();
    const uint32_t n = tdb->get_num_methods();
    for (auto& h : g_hooks) if (h.raw) { h.shared = 0; h.other = -1; }
    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 0xFFFF) == 0 && cmn::closing()) return false;
        API::Method* m = tdb->get_method(i);
        if (!m) continue;
        const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
        if (!fn) continue;
        uintptr_t code = 0;
        bool code_done = false;
        for (auto& h : g_hooks) {
            if (!h.raw) continue;
            bool same = fn == h.raw || fn == h.code;
            if (!same) {
                if (!code_done) { code = sec_code_of(fn); code_done = true; }
                same = code && code == h.code;
            }
            const uint32_t mi = m->get_index();
            if (!same || mi == h.index) continue;
            ++h.shared;
            if (h.other < 0) h.other = (long long)mi;
        }
    }
    return true;
}
void sec_check_run() {
    const uint64_t t0 = GetTickCount64();
    auto* tdb = API::get()->tdb();
    g_check_methods = tdb ? tdb->get_num_methods() : 0;
    for (auto& h : g_hooks) {
        auto* td = tdb ? tdb->find_type(h.type) : nullptr;
        API::Method* m = td ? td->find_method(h.name) : nullptr;
        if (m && m->get_declaring_type() != td) m = nullptr;     // only the type's own method, never a base one
        h.m = m;
        h.index = m ? m->get_index() : 0;
        h.raw = m ? reinterpret_cast<uintptr_t>(m->get_function_raw()) : 0;
        h.code = h.raw ? sec_code_of(h.raw) : 0;
    }
    if (tdb) {
        sec_exe_key(g_check_key, sizeof g_check_key, g_check_methods);
        if (sec_cache_read()) {
            g_check_cache = true;
            g_check_ok = true;
        } else {
            g_check_ok = sec_scan();
            if (g_check_ok) g_check_written = sec_cache_write();
        }
    }
    g_check_ms = GetTickCount64() - t0;
}
bool sec_check_seh() {
    __try { sec_check_run(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
DWORD WINAPI sec_check_thread(LPVOID) {
    if (!sec_check_seh()) { g_check_ok = false; std::snprintf(g_check_note, sizeof g_check_note, "a fault during the check"); }
    g_check_state.store(2, std::memory_order_release);
    return 0;
}

// Game thread: the check started on the first frame with a section, its verdicts logged once it ended, then one hook per frame
void sec_hooks_step() {
    const int st = g_check_state.load(std::memory_order_acquire);
    if (st == 0) {
        g_check_state.store(1, std::memory_order_release);
        HANDLE h = CreateThread(nullptr, 0, sec_check_thread, nullptr, 0, nullptr);
        if (h) {
            CloseHandle(h);
            cmn::journal("[options] sections: code check (R18) of their hooks started on a thread of its own");
        } else {
            std::snprintf(g_check_note, sizeof g_check_note, "its thread was not created (error %lu)", GetLastError());
            g_check_state.store(2, std::memory_order_release);
        }
        return;
    }
    if (st != 2) return;
    if (!g_check_logged) {
        g_check_logged = true;
        if (g_check_cache)
            cmn::journal("[options] sections: code check (R18): read from %s (same game executable), %llu ms", kSecCacheFile, (unsigned long long)g_check_ms);
        else if (g_check_ok)
            cmn::journal("[options] sections: code check (R18): %u methods compared in %llu ms (background), cache %s (%s)", g_check_methods,
                         (unsigned long long)g_check_ms, g_check_written ? "written" : "NOT written", g_check_note);
        else
            cmn::log_warn("[options] sections: code check (R18) not done (%s): no section hook", g_check_note[0] ? g_check_note : "the game closed");
        return;                                                  // the hooks from the next frame
    }
    for (auto& h : g_hooks) {
        if (h.tried) continue;
        h.tried = true;
        const std::string label = sec_hook_label(h);
        if (!g_check_ok || !h.m || !h.raw) {
            cmn::log_warn("[options] sections: %s %s not hooked (%s): %s", h.tag, label.c_str(), h.m ? "not checked" : "not found", h.loss);
            continue;
        }
        if (h.shared != 0) {
            auto* other = h.other >= 0 ? API::get()->tdb()->get_method((uint32_t)h.other) : nullptr;
            cmn::log_warn("[options] sections: %s %s REFUSED (R18): %d other method(s) enter its code, e.g. %s: %s", h.tag, label.c_str(),
                          h.shared, sec_method_label(other).c_str(), h.loss);
            continue;
        }
        h.hooked = h.m->add_hook(h.pre, h.post, false) != 0;
        cmn::journal("[options] sections: %s %s %s (entry 0x%llX, its own code)", h.tag, label.c_str(), h.hooked ? "hooked" : "NOT hooked (add_hook refused)",
                     (unsigned long long)sec_rva(h.raw));
        return;                                                  // one add_hook per frame (each one stops the game's threads)
    }
}

#pragma warning(push)
#pragma warning(disable: 4733)

// H2: the left list's Back does nothing while a section is held (the client's page has the input)
int pre_main_cancel(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_held.load(std::memory_order_acquire) <= 0 || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    g_cancels.fetch_add(1, std::memory_order_relaxed);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}

// H1: Confirm on the left list with the cursor on a section: counted, and the decide event is Invalid (the unit's own)
thread_local MO* tl_dl_param = nullptr;
int pre_decide_list(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_dl_param = (g_slot_live.load(std::memory_order_acquire) > 0 && !g_sec_off.load() && !cmn::closing()) ? reinterpret_cast<MO*>(argv[1]) : nullptr;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_decide_list_inner(MO* p, void** ret_val) {
    int32_t ls = -1;
    if (!ll::read_field_i32(p, "<ListState>k__BackingField", ls) || ls != kListMain) return;
    MO* fu = call_obj(p, "GetMainListFocusUnit");
    if (!fu) return;
    for (int i = 0; i < kMaxSections; ++i) {
        if (g_slot_unit[i].load(std::memory_order_acquire) != fu) continue;
        g_slot_decides[i].fetch_add(1, std::memory_order_relaxed);
        *ret_val = nullptr;                                      // DecideEventType.Invalid
        return;
    }
}
void post_decide_list(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    MO* p = tl_dl_param;
    tl_dl_param = nullptr;
    if (!p) return;
    int64_t pt = perf::begin();
    __try { post_decide_list_inner(p, ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { sec_fault("decide_list_post"); }
    perf::end(perf::OptHooks, pt);
}

#pragma warning(pop)

void sections_sync() {
    if (!g_sec_any) return;                                      // no section ever: nothing
    sections_sync_inner();
}

void sections_frame_inner() {
    ++g_sec_frame;
    const bool off = g_sec_off.load() || !g_on;                  // withdrawn at once (the sync drops every section)
    if (g_secs_dirty || g_sec_frame - g_sec_synced_at >= kSecSyncEvery || (off && !g_secs.empty())) sections_sync_inner();
    if (g_secs.empty()) { g_scr = Screen{}; g_parts_of = nullptr; return; }
    sec_hooks_step();
    sec_read_screen();
    sec_states();
    sec_holds();
}
bool sections_frame_seh() {
    __try { sections_frame_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
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
    if (!g_on || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = pre_reset_units_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("reset_units_pre"); }
    perf::end(perf::OptHooks, pt);
    return rc;
}
void post_reset_units(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// GetFocusDecideEventType (:423-442): Invalid for our buttons, so the FSM does nothing (no "Revert settings?")
thread_local MO* tl_decide_this = nullptr;
int pre_decide_event(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_decide_this = (g_on && !cmn::closing()) ? reinterpret_cast<MO*>(argv[1]) : nullptr;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_decide_event_inner(void** ret_val) {
    if ((reinterpret_cast<int64_t>(*ret_val) & 0xFF) != kEventSettingReset) return;
    int32_t tid = 0;
    if (focus_type_id(tl_decide_this, tid) && core::on_button(tid)) *ret_val = nullptr;      // DecideEventType.Invalid
}
void post_decide_event(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_decide_this) return;
    int64_t pt = perf::begin();
    __try { post_decide_event_inner(ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("decide_event_post"); }
    perf::end(perf::OptHooks, pt);
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
    if (!g_on || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    __try { pre_created_object_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("created_object_pre"); }
    perf::end(perf::OptHooks, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_created_object(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_created_param) return;
    int64_t pt = perf::begin();
    __try { core::on_created_object(tl_created_param); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("created_object_post"); }
    perf::end(perf::OptHooks, pt);
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
    if (!g_on || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    __try { pre_setup_disp_units_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("setup_disp_pre"); }
    perf::end(perf::OptHooks, pt);
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
    int64_t pt = perf::begin();
    __try { post_setup_disp_units_inner(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("setup_disp_post"); }
    perf::end(perf::OptHooks, pt);
    tl_setup_this = nullptr;
}

// CheckCanSelect (:533-569): the hidden reset row must not take the cursor on our pages
thread_local MO* tl_cs_this = nullptr;
thread_local int32_t tl_cs_index = 0;
int pre_check_can_select(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_cs_this = (g_on && !cmn::closing()) ? reinterpret_cast<MO*>(argv[1]) : nullptr;
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
    int64_t pt = perf::begin();
    __try { post_check_can_select_inner(ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("check_can_select_post"); }
    perf::end(perf::OptHooks, pt);
}

// LoadValueEvent + ResetEvent (:574-581): the game must not load / reset our ids from its save data
int pre_skip_ours_inner(void** argv) {
    int32_t tid = 0;
    if (unit_setting(reinterpret_cast<MO*>(argv[1]), tid) && core::known(tid)) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_skip_ours(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = pre_skip_ours_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("skip_ours_pre"); }
    perf::end(perf::OptHooks, pt);
    return rc;
}
void post_skip_ours(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// SaveEvent (increment 11): a placed row is never written into the game's save. Its PrevValue follows its value, as
// the game's save would make it (no pending change left on the screen). Every other unit: the game's, untouched.
int pre_save_ours_inner(void** argv) {
    MO* u = reinterpret_cast<MO*>(argv[1]);
    int32_t tid = 0;
    if (!unit_setting(u, tid) || !core::placed_type(tid)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int32_t v = 0;
    if (be_get_value(u, v)) {
        if (auto* set_pv = ll::find_method_full(u->get_type_definition(), "set_PrevValue")) set_pv->invoke(u, std::vector<void*>{ ll::arg_i32(v) });
    }
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
int pre_save_ours(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = pre_save_ours_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("save_ours_pre"); }
    perf::end(perf::OptHooks, pt);
    return rc;
}

// TrainingManager.OpenMenu (:666-673): the Esc / Start that closes our dialog also reaches the training pause menu
int pre_open_menu(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_on && !cmn::closing() && core::suppress_open_menu()) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_open_menu(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

#pragma warning(pop)

// Once, on the first build that places a group: a core whose clients place nothing hooks nothing more.
void install_save_hook() {
    if (g_save_hook_tried) return;
    g_save_hook_tried = true;
    auto* tdb = API::get()->tdb();
    auto* vu_td = tdb ? tdb->find_type("app.Option.OptionValueUnit") : nullptr;
    auto* ms = vu_td ? vu_td->find_method("SaveEvent") : nullptr;
    g_save_hook = ms && ms->get_function_raw() && ms->add_hook(pre_save_ours, post_skip_ours, false);
    cmn::journal("[options] placed rows: SaveEvent hook %s", g_save_hook ? "installed" : "NOT installed (the game may save them)");
}

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
    be.sync_placed = be_sync_placed;
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

// ---- increment 13 ----
void sections_frame() {
    if (!g_sec_any) return;                                      // no section ever placed: nothing, not even a look
    int64_t pt = perf::begin();
    if (!sections_frame_seh()) sec_fault("sections_frame");
    perf::end(perf::OptHooks, pt);
}

void sections_dirty() { g_sec_any = true; g_secs_dirty = true; }

bool section_state(int64_t group_id, SF6CoreOptSectionState& out) {
    const Sec* s = find_sec(group_id);
    if (!s) return false;
    out = s->st;
    out.struct_size = sizeof out;
    out.held = s->held ? 1 : 0;
    out.decides = s->slot >= 0 ? g_slot_decides[s->slot].load(std::memory_order_relaxed) : 0;
    return true;
}

SF6CoreStatus section_hold(int64_t group_id, bool hold) {
    Sec* s = find_sec(group_id);
    if (!hold) { if (s) sec_release(*s, "given back by its client"); return SF6CORE_OK; }
    if (!s || g_sec_off.load() || !g_on || cmn::closing()) return SF6CORE_ERR_NOT_READY;
    if (s->held) return SF6CORE_OK;
    if (!s->st.shown || !g_scr.up || !g_scr.agent) return SF6CORE_ERR_NOT_READY;
    const auto* api = cmn::core();
    if (!api || !api->ui_set_block_input) return SF6CORE_ERR_NOT_READY;
    if (g_held_agent != g_scr.agent) {
        if (g_held_agent) { for (auto& o : g_secs) sec_release(o, "another Options screen"); }
        const SF6CoreStatus st = api->ui_set_block_input(cmn::client(), g_scr.agent, 1);
        if (st != SF6CORE_OK) return st;
        g_held_agent = g_scr.agent;
    }
    s->held = true;
    s->st.held = 1;
    g_held.fetch_add(1);
    const bool h2 = g_hooks[0].hooked;
    if (!h2 && !g_no_h2_logged) {
        g_no_h2_logged = true;
        cmn::log_warn("[options] sections: a hold without H2 (%s): the input block alone keeps the left list's Back away",
                      g_check_state.load() == 2 ? "not hooked" : "its code check is still running");
    }
    cmn::journal("[options] section %lld held (the Options screen's input blocked%s)", (long long)group_id,
                 h2 ? ", its left list's Back swallowed" : "");
    return SF6CORE_OK;
}

std::string status() {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "on=%d faults=%d ", (int)g_on, g_faults);
    std::string out = std::string(buf) + core::status() + " | guid_text " + guid_text::status();
    if (!g_sections.empty()) {                                   // increment 11, once a group was placed
        std::snprintf(buf, sizeof(buf), " | placed sections=%zu rows=%zu save_hook=%d", g_sections.size(), g_placed_live, (int)g_save_hook);
        out += buf;
    }
    if (g_sec_any) {                                             // increment 13, once a section was placed
        size_t live = 0;
        for (auto& s : g_secs) if (s.in_tab >= 0) ++live;
        const int cs = g_check_state.load();
        const char* check = cs == 0 ? "pending" : cs == 1 ? "running" : (g_check_ok ? (g_check_cache ? "cache" : "scanned") : "failed");
        auto hk = [](const SecHook& h) { return h.hooked ? "on" : (!h.tried ? "wait" : (h.shared > 0 ? "refused" : "off")); };
        std::snprintf(buf, sizeof(buf), " | sections=%zu live=%zu hooks=check:%s,H2:%s,H1:%s held=%d cancels=%u faults=%d%s",
                      g_secs.size(), live, check, hk(g_hooks[0]), hk(g_hooks[1]), g_held.load(), g_cancels.load(), g_sec_faults.load(),
                      g_sec_off.load() ? " OFF" : "");
        out += buf;
    }
    return out;
}

}  // namespace dialogs::options
