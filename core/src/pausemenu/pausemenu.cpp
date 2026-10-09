#ifndef NOMINMAX
#define NOMINMAX
#endif
// pausemenu - the game side of the training pause-menu engine (increment 9, ADR-0016). See pausemenu.hpp and
// docs/PAUSE_MENU.md; every decision is in pm_core.cpp.
//
// Rules carried as code (reference/04_PITFALLS_CRASHES, and the notes of the engine this one was ported from):
//   R4   the menu's update methods are never called back from their own chain: the page rebuild after a change of a
//        shown spin's labels runs 2 frames later from ANOTHER scroll group's update (never our list's own); the spin
//        repairs of UpdateSpinBox's post are not repeated in a nested UpdateSpinBox (set_SelectedIndex re-enters it:
//        the residual of crash C4). ViewUpdate(page) right after the page's own Function is the game's own pattern
//        (its record pages chain into it the same way).
//   R5   a row is ours by the GUID of its _MessageID (clones keep it); the game's DYNAMIC rows carry a delegate.
//   R17  UpdateSpinBox hooked by its full signature; rows are data (GUID texts); the one set_Message is the visible
//        option text of our own spin, compared first (R7), through the display API.
//   R18  each method is checked for machine code shared with another method before its hook (folded bodies, jump
//        thunks): shared = the hook is refused, logged with what it costs, the rest of the engine runs.
//   R26  key-guide hints appended to the game's own guide data in GetGuideData's post.
//   R29  hooks run concurrently on the game's UI job threads: per-thread state in thread_locals, the registry under
//        pm_core's lock, the cached menu param under its own lock with a reference held while used.

#include "pausemenu.hpp"

#include "../options/ll_common.hpp"   // REFramework C++ API (warnings off), windows.h

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../sf6core_internal.h"
#include "../perf.hpp"
#include "../options/guid_table.hpp"
#include "../options/guid_text.hpp"
#include "../options/options_core.hpp"
#include "pm_core.hpp"

namespace pausemenu {
namespace {

namespace pc = ::pausemenu::core;
namespace gt = ::dialogs::guid_text;
using API = reframework::API;
using MO = API::ManagedObject;
using TD = API::TypeDefinition;

constexpr int kMaxFaults = 5;                  // the engine's hooks off after 5 faults (as the option engine's)
constexpr int32_t kTrainingFlow = 3;           // training
constexpr int32_t kHubTrainingFlow = 9;        // Battle Hub training room
constexpr uint32_t kPauseNone = 64;            // PauseManager._CurrentPauseTypeBit with no menu up
constexpr uint32_t kPauseNone2 = 2112;
constexpr int kGroupMaxDepth = 6;              // a spin part's control is at most 6 levels under its p_Group_<row>_
constexpr int kNest = 4;                       // per-thread nesting kept for UpdateSpinBox / UpdateScroll

std::atomic<bool> g_on{false};
std::atomic<int> g_faults{0};
Calls g_calls;
SF6CoreClient* g_self = nullptr;               // the engine's own client: display writes, texts
const SF6CoreAPI* g_api = nullptr;

// ---- types (resolved once) ----
std::mutex g_types_mx;
bool g_types_done = false;
TD* g_td_menu = nullptr;                       // app.training.TrainingMenuData
API::Field* g_f_msg = nullptr;                 // its _MessageID (System.Guid)
API::Field* g_f_ftype = nullptr;               // its _FuncType
TD* g_td_param = nullptr;                      // app.training.UIFlowTrainingMenu.Param
TD* g_td_spin = nullptr;                       // app.UIPartsSpin
TD* g_td_guide = nullptr;                      // app.InputGuideData
API::Method* g_m_viewupdate = nullptr;         // app.training.TrainingMenuFunc.ViewUpdate

// ---- per-thread state (R29) ----
thread_local pc::Ctx tl_ctx = pc::Ctx::None;   // the row being laid out on this thread
thread_local int64_t tl_ctx_row = -1;
thread_local int64_t tl_building = -1;         // our page being built on this thread (its ViewUpdate)

uint64_t now() { return core_frame_pub(); }

void fault(const char* where) {
    const int n = ++g_faults;
    core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] fault in %s (%d)", where, n);
    if (n >= kMaxFaults && g_on.exchange(false))
        core_log(SF6CORE_LOG_ERROR, nullptr, "[pausemenu] too many faults: hooks off, the game answers its menu alone");
}

// ======================== game helpers ========================

// Method lookup, cached per (type, signature literal): a full signature first (through the parent types), then the
// bare name with that many parameters.
std::mutex g_mc_mx;
std::map<std::pair<TD*, const char*>, API::Method*> g_mc;

API::Method* meth(TD* td, const char* sig) {
    if (!td) return nullptr;
    {
        std::lock_guard lk(g_mc_mx);
        auto it = g_mc.find({ td, sig });
        if (it != g_mc.end()) return it->second;
    }
    API::Method* m = nullptr;
    const std::string_view s{ sig };
    const size_t open = s.find('(');
    if (open == std::string_view::npos) {
        m = ll::find_method_noargs(td, sig);
    } else {
        for (TD* t = td; t && !m; t = t->get_parent_type()) m = t->find_method(sig);
        if (!m) {
            const std::string_view bare = s.substr(0, open);
            uint32_t arity = 0;
            if (s.size() > open + 1 && s[open + 1] != ')') { arity = 1; for (char c : s) if (c == ',') ++arity; }
            for (TD* t = td; t && !m; t = t->get_parent_type())
                for (auto* x : t->get_methods())
                    if (x && x->get_name() && bare == x->get_name() && x->get_num_params() == arity) { m = x; break; }
        }
    }
    std::lock_guard lk(g_mc_mx);
    g_mc[{ td, sig }] = m;
    return m;
}

bool call(MO* o, const char* sig, const std::vector<void*>& args, reframework::InvokeRet& out) {
    if (!o) return false;
    API::Method* m = meth(o->get_type_definition(), sig);
    if (!m) return false;
    std::vector<void*> a = args;
    out = m->invoke(o, a);
    return !out.exception_thrown;
}
MO* call_obj(MO* o, const char* sig, const std::vector<void*>& args = {}) {
    reframework::InvokeRet r;
    return call(o, sig, args, r) ? reinterpret_cast<MO*>(r.ptr) : nullptr;
}
bool call_i32(MO* o, const char* sig, int32_t& v) {
    reframework::InvokeRet r;
    if (!call(o, sig, {}, r)) return false;
    v = (int32_t)r.dword;
    return true;
}
bool call_size(MO* o, const char* sig, float& w, float& h) {     // via.Size {w, h}
    reframework::InvokeRet r;
    if (!call(o, sig, {}, r)) return false;
    std::memcpy(&w, r.bytes.data(), 4);
    std::memcpy(&h, r.bytes.data() + 4, 4);
    return true;
}
bool call_pos(MO* o, float& x, float& y, float& z) {               // via.vec3
    reframework::InvokeRet r;
    if (!call(o, "get_Position", {}, r)) return false;
    std::memcpy(&x, r.bytes.data(), 4);
    std::memcpy(&y, r.bytes.data() + 4, 4);
    std::memcpy(&z, r.bytes.data() + 8, 4);
    return true;
}

MO* field_obj(MO* o, const char* name) { return o ? ll::read_field_object(o, name) : nullptr; }

int32_t list_count(MO* l) { int32_t n = 0; return (l && call_i32(l, "get_Count", n)) ? n : 0; }
MO* list_item(MO* l, int32_t i) { return call_obj(l, "get_Item(System.Int32)", { ll::arg_i32(i) }); }
int32_t array_length(MO* a) { int32_t n = 0; return (a && call_i32(a, "get_Length", n)) ? n : 0; }
MO* array_get(MO* a, int32_t i) { return call_obj(a, "GetValue(System.Int32)", { ll::arg_i32(i) }); }
bool array_set(MO* a, MO* v, int32_t i) {
    reframework::InvokeRet r;
    return call(a, "SetValue(System.Object, System.Int32)", { v, ll::arg_i32(i) }, r);
}

std::string name_of(MO* ctrl) {
    MO* s = call_obj(ctrl, "get_Name");
    return s ? ll::managed_string_to_utf8(s) : std::string();
}
MO* child(MO* ctrl, const char* name) {      // a direct child by name (read-only, SEH inside the core's query)
    return ctrl ? reinterpret_cast<MO*>(g_api->ui_find_child(ctrl, name, 0)) : nullptr;
}

bool is_menu_data(MO* o) { return o && (uintptr_t)o > 0x10000 && o->get_type_definition() == g_td_menu; }

// The identity of a TrainingMenuData: its _MessageID (R5).
bool menu_key(MO* o, pc::Key& k) {
    if (!is_menu_data(o)) return false;
    const void* raw = g_f_msg->get_data_raw(o, false);
    if (!raw) return false;
    std::memcpy(&k.hi, raw, 8);
    std::memcpy(&k.lo, static_cast<const uint8_t*>(raw) + 8, 8);
    return true;
}
int32_t menu_ftype(MO* o) {
    if (!is_menu_data(o)) return -1;
    const void* raw = g_f_ftype->get_data_raw(o, false);
    int32_t v = -1;
    if (raw) std::memcpy(&v, raw, 4);
    return v;
}
pc::Ident ident_of(MO* o) {
    pc::Key k;
    return menu_key(o, k) ? pc::identify(k) : pc::Ident{};
}
// A list entry (ViewData): its Data, or ParentData on the secondary pages (Data = the option shown).
pc::Ident row_ident_of_entry(MO* vd) {
    pc::Ident i = ident_of(field_obj(vd, "Data"));
    if (i.what == pc::What::None) i = ident_of(field_obj(vd, "ParentData"));
    return i;
}
bool is_our_spin(const pc::Ident& i) {
    return (i.what == pc::What::Row || i.what == pc::What::Option) && !i.removed && i.kind == pc::Kind::Spin;
}

// ---- the menu's param ----
MO* view_entry(MO* param, int32_t idx) {
    MO* l = field_obj(param, "_ViewDataList");
    if (!l || idx < 0 || idx >= list_count(l)) return nullptr;   // shorter during a rebuild
    return list_item(l, idx);
}
MO* group_at(MO* param, int32_t idx) {
    MO* ch = field_obj(field_obj(param, "_SecondaryList"), "_Children");
    if (!ch || idx < 0 || idx >= list_count(ch)) return nullptr;
    return list_item(ch, idx);
}
MO* control_of(MO* part) { return call_obj(part, "get_Control"); }

// The spin part of a row: the UIPartsSpin with a SpinAction in the row's group.
MO* spin_part(MO* param, int32_t idx) {
    MO* ch = field_obj(group_at(param, idx), "_Children");
    const int32_t n = list_count(ch);
    for (int32_t j = 0; j < n; ++j) {
        MO* p = list_item(ch, j);
        if (p && p->get_type_definition() == g_td_spin && field_obj(p, "<SpinAction>k__BackingField")) return p;
    }
    return nullptr;
}
MO* option_list(MO* part) { return child(control_of(part), "p_ScrollList_0_h"); }       // the carousel
MO* option_text(MO* part) { return child(child(option_list(part), "c_selectItem"), "e_txt_0"); }   // the option shown

// The menu param seen last (a reference held while kept, released when the menu closes).
std::mutex g_param_mx;
MO* g_param = nullptr;
std::atomic<uintptr_t> g_sec_list{0};          // its _SecondaryList (our row list's scroll group)

void param_seen(MO* p) {
    if (!p || p->get_type_definition() != g_td_param) return;
    MO* sec = field_obj(p, "_SecondaryList");
    std::lock_guard lk(g_param_mx);
    if (g_param == p) return;
    p->add_ref();
    if (g_param) g_param->release();
    g_param = p;
    g_sec_list = reinterpret_cast<uintptr_t>(sec);
}
MO* param_get() {
    std::lock_guard lk(g_param_mx);
    if (g_param) g_param->add_ref();
    return g_param;
}
void param_put(MO* p) { if (p) p->release(); }
void param_drop() {
    std::lock_guard lk(g_param_mx);
    if (g_param) g_param->release();
    g_param = nullptr;
    g_sec_list = 0;
}

bool page_shown_live(MO* param, int64_t page) {
    if (tl_building == page) return true;
    const pc::Ident i = row_ident_of_entry(view_entry(param, 1));   // the page's first row (0 = the strip row)
    return (i.what == pc::What::Row || i.what == pc::What::Option) && i.page == page;
}

// ---- display writes (ADR-0011: the core's guarded primitives, for the engine's own client) ----
void set_i64(MO* o, const char* f, int64_t v) { g_api->obj_set_i64(g_self, o, f, v); }
bool set_ref(MO* o, const char* f, void* v) { return g_api->obj_set_ref(g_self, o, f, v) == SF6CORE_OK; }
void set_guid(MO* o, const char* f, const pc::Key& k) {
    uint8_t g[16];
    std::memcpy(g, &k.hi, 8);
    std::memcpy(g + 8, &k.lo, 8);
    g_api->obj_set_guid(g_self, o, f, g);
}
void invoke_i(MO* o, const char* sig, int64_t v, int32_t type) {
    SF6CoreArg a{};
    a.type = type;
    a.v.i = v;
    g_api->obj_invoke(g_self, o, sig, &a, 1, nullptr);
}

// ---- the game's training data ----
MO* training_manager() { return API::get()->get_managed_singleton("app.training.TrainingManager"); }
MO* menu_tabs() {
    MO* ui = field_obj(training_manager(), "_UIData");
    return ui ? call_obj(ui, "get_MenuData") : nullptr;
}

// The delegate field of DynamicData (AddDynamicMenu's Action): the game's rows carry one, a mod's nil (R5).
std::mutex g_dlg_mx;
TD* g_dlg_td = nullptr;
API::Field* g_dlg_field = nullptr;

bool is_delegate_type(TD* t) {
    for (TD* p = t; p; p = p->get_parent_type()) {
        const char* ns = p->get_namespace();
        const char* nm = p->get_name();
        if (ns && nm && std::string_view{ ns } == "System" &&
            (std::string_view{ nm } == "Delegate" || std::string_view{ nm } == "MulticastDelegate")) return true;
    }
    return false;
}
int has_delegate(MO* dd) {                     // 1 yes, 0 no, -1 unknown (DynamicData has no delegate field)
    if (!dd) return 0;
    TD* td = dd->get_type_definition();
    API::Field* f = nullptr;
    {
        std::lock_guard lk(g_dlg_mx);
        if (g_dlg_td != td) {
            g_dlg_td = td;
            g_dlg_field = nullptr;
            for (TD* t = td; t && !g_dlg_field; t = t->get_parent_type())
                for (auto* x : t->get_fields())
                    if (x && !x->is_static() && x->get_type() && is_delegate_type(x->get_type())) { g_dlg_field = x; break; }
            core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] DynamicData delegate field: %s",
                     g_dlg_field && g_dlg_field->get_name() ? g_dlg_field->get_name() : "none (every unknown DYNAMIC row counts as the game's)");
        }
        f = g_dlg_field;
    }
    if (!f) return -1;
    const void* raw = f->get_data_raw(dd, false);
    return raw && *static_cast<void* const*>(raw) ? 1 : 0;
}

// Every DynamicData of the primary tabs (their DynamicChildData): fn(dynamic_data, its ChildData).
template <class Fn> void each_dynamic(Fn&& fn) {
    MO* arr = menu_tabs();
    const int32_t n = array_length(arr);
    for (int32_t t = 0; t < n; ++t) {
        MO* dl = field_obj(array_get(arr, t), "DynamicChildData");
        const int32_t c = list_count(dl);
        for (int32_t i = 0; i < c; ++i) {
            MO* dd = list_item(dl, i);
            if (dd && fn(dd, field_obj(dd, "ChildData"))) return;
        }
    }
}

bool game_owns(MO* d) {                        // this data is a game DYNAMIC row (its delegate runs on decide)
    bool owned = false;
    each_dynamic([&](MO* dd, MO* cd) {
        if (cd != d) return false;
        owned = has_delegate(dd) != 0;         // unknown field: treated as the game's (passes, as an unknown row)
        return true;
    });
    return owned;
}

void scan_game_rows() {
    bool present = false;
    each_dynamic([&](MO* dd, MO* cd) {
        if (menu_ftype(cd) != pc::kDynamic) return false;
        const int dl = has_delegate(dd);
        if (dl > 0 || (dl < 0 && ident_of(cd).what == pc::What::None)) { present = true; return true; }
        return false;
    });
    if (present != pc::game_rows())
        core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] game DYNAMIC rows in the menu: %s", present ? "yes" : "no");
    pc::set_game_rows(present);
}

// ======================== backend of the state machine ========================

bool keep_owner(gt::Owner o) { return o && static_cast<const SF6CoreClient*>(o)->active; }

bool be_text_new(const std::string& t, pc::Key& out) {
    gt::GuidBytes gb;
    if (!g_self || !gt::alloc_owned(g_self, t, keep_owner, gb, nullptr)) return false;
    out.hi = gb.hi;
    out.lo = gb.lo;
    return true;
}
bool be_text_set(const pc::Key& k, const std::string& t) {
    gt::GuidBytes gb;
    gb.hi = k.hi; gb.lo = k.lo;
    return g_self && gt::retext_owned(g_self, gb, t);
}
void be_text_release(const pc::Key& k) {
    gt::GuidBytes gb;
    gb.hi = k.hi; gb.lo = k.lo;
    if (g_self) gt::release_owned(g_self, gb);
}

MO* new_menu_data(int32_t item_type) {
    MO* d = g_td_menu->create_instance();
    if (!d) return nullptr;
    d->add_ref();                                        // held: the game keeps it in its arrays (R28)
    set_i64(d, "_Type", item_type);
    set_i64(d, "_FuncType", pc::kDynamic);
    set_i64(d, "IsEnabled", 1);
    set_i64(d, "_Interval", 1);
    set_i64(d, "_Interval_Option", 1);
    set_i64(d, "_GuidIcon", -1);
    set_i64(d, "_GuidAddIcon", -1);
    set_i64(d, "VisibleCase", 0);
    return d;
}
pc::Obj be_new_row_data(int32_t item_type, const pc::Key& title, const pc::Key& guide) {
    MO* d = new_menu_data(item_type);
    if (!d) return nullptr;
    set_guid(d, "_MessageID", title);
    set_guid(d, "_GuideMessage", guide);
    return d;
}
pc::Obj be_new_option_data(const pc::Key& text) {
    MO* d = new_menu_data(pc::kItemTextOnly);
    if (d) set_guid(d, "_MessageID", text);
    return d;
}
pc::Obj be_new_array(const std::vector<pc::Obj>& items) {
    MO* a = API::get()->create_managed_array(g_td_menu, (uint32_t)items.size());
    if (!a) return nullptr;
    a->add_ref();
    for (size_t i = 0; i < items.size(); ++i)
        if (!array_set(a, reinterpret_cast<MO*>(items[i]), (int32_t)i)) { a->release(); return nullptr; }
    return a;
}
bool be_set_children(pc::Obj data, pc::Obj arr) { return set_ref(reinterpret_cast<MO*>(data), "_ChildData", arr); }

// A value field copied as it is, a reference field assigned (R34).
void copy_field(MO* dst, MO* src, const char* name) {
    API::Field* f = ll::find_field_walk(src->get_type_definition(), name);
    TD* ft = f ? f->get_type() : nullptr;
    if (!ft) return;
    void* s = f->get_data_raw(src, false);
    void* d = f->get_data_raw(dst, false);
    if (!s || !d) return;
    if (ft->is_valuetype()) {
        const uint32_t n = ft->get_valuetype_size();
        if (n && n <= 64) std::memcpy(d, s, n);
    } else {
        set_ref(dst, name, *static_cast<void**>(s));
    }
}
pc::Obj be_new_page_data(pc::Obj template_page, const pc::Key& title, const pc::Key& guide, pc::Obj rows) {
    MO* tp = reinterpret_cast<MO*>(template_page);
    int32_t type = 0;
    if (!tp || !ll::read_field_i32(tp, "_Type", type)) return nullptr;
    MO* d = new_menu_data(type);                         // typed like the strip's own pages (TAB_ITEM)
    if (!d) return nullptr;
    copy_field(d, tp, "NextButton");
    copy_field(d, tp, "PrevButton");
    set_guid(d, "_MessageID", title);
    set_guid(d, "_GuideMessage", guide);
    set_ref(d, "_ChildData", rows);
    if (MO* empty = call_obj(field_obj(tp, "DynamicChildData"), "GetRange(System.Int32, System.Int32)",
                             { ll::arg_i32(0), ll::arg_i32(0) })) {   // an empty List<DynamicData> of the right type
        empty->add_ref();
        set_ref(d, "DynamicChildData", empty);
    }
    return d;
}
pc::Obj be_new_hint(int32_t button, const pc::Key& text) {
    if (!g_td_guide) return nullptr;
    MO* g = g_td_guide->create_instance();
    if (!g) return nullptr;
    g->add_ref();
    set_i64(g, "<Type>k__BackingField", 0);              // InputGuideDataType.DigitalConfig: the glyph of the device in use
    set_i64(g, "<DigitalId>k__BackingField", -1);
    set_i64(g, "<DigitalConfigId>k__BackingField", button);
    set_guid(g, "<MessageId>k__BackingField", text);
    return g;
}
void be_release(pc::Obj o) { if (o) reinterpret_cast<MO*>(o)->release(); }

bool be_strip_read(int32_t primary_func, pc::Strip& s) {
    MO* arr = menu_tabs();
    const int32_t n = array_length(arr);
    for (int32_t t = 0; t < n; ++t) {
        MO* tab = array_get(arr, t);
        if (menu_ftype(tab) != primary_func) continue;
        MO* kids = field_obj(tab, "_ChildData");
        MO* row = array_length(kids) > 0 ? array_get(kids, 0) : nullptr;   // the strip is the tab's first row
        int32_t type = 0;
        if (!row || !ll::read_field_i32(row, "_Type", type) || (type != pc::kStripTwo && type != pc::kStripThree)) return false;
        MO* pages = field_obj(row, "_ChildData");
        if (!pages) return false;
        s.row = row;
        s.type = type;
        s.array = pages;
        const int32_t np = array_length(pages);
        for (int32_t i = 0; i < np; ++i) {
            MO* p = array_get(pages, i);
            pc::Key k;
            menu_key(p, k);
            s.pages.push_back(p);
            s.keys.push_back(k);
        }
        return true;
    }
    return false;
}
std::vector<MO*> g_strip_arrays;                         // the strip arrays we wrote (held for the session)
bool be_strip_write(const pc::Strip& s, const std::vector<pc::Obj>& pages, int32_t type) {
    MO* arr = reinterpret_cast<MO*>(be_new_array(pages));
    if (!arr) return false;
    MO* row = reinterpret_cast<MO*>(s.row);
    if (!set_ref(row, "_ChildData", arr)) { arr->release(); return false; }
    set_i64(row, "_Type", type);
    g_strip_arrays.push_back(arr);
    return true;
}
void be_log(const char* line) { core_log(SF6CORE_LOG_INFO, nullptr, "%s", line); }

// ======================== R18: one hook per machine code ========================

// The code a function entry runs: its jump thunks followed (jmp rel32 / rel8 / jmp [rip+disp32]), as REFramework's
// add_hook does (ignore_jmp = false).
uintptr_t code_of(uintptr_t p) {
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

struct FoldTarget {
    API::Method* m = nullptr;
    uint32_t index = 0;
    uintptr_t raw = 0, code = 0;
    int shared = 0;                                       // other methods with the same entry or code
    API::Method* other = nullptr;                          // one of them, for the log
    std::vector<uint32_t> sharers;                         // their method indexes (the first 16)
};

// One pass over every method of the game: who else enters the same code.
void fold_scan(std::vector<FoldTarget>& ts) {
    auto* tdb = API::get()->tdb();
    const uint32_t n = tdb->get_num_methods();
    for (uint32_t i = 0; i < n; ++i) {
        API::Method* m = tdb->get_method(i);
        if (!m) continue;
        const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
        if (!fn) continue;
        uintptr_t code = 0;
        bool code_done = false;
        for (auto& t : ts) {
            if (!t.raw) continue;
            bool same = fn == t.raw || fn == t.code;
            if (!same) {
                if (!code_done) { code = code_of(fn); code_done = true; }
                same = code && code == t.code;
            }
            const uint32_t mi = m->get_index();
            if (!same || mi == t.index) continue;
            ++t.shared;
            if (!t.other) t.other = m;
            if (t.sharers.size() < 16) t.sharers.push_back(mi);
        }
    }
}
bool fold_scan_seh(std::vector<FoldTarget>* ts) {
    __try { fold_scan(*ts); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

std::string method_label(API::Method* m) {
    if (!m) return "?";
    TD* t = m->get_declaring_type();
    return (t ? t->get_full_name() : std::string("?")) + "." + (m->get_name() ? m->get_name() : "?");
}

// ======================== spin repairs ========================

int32_t value_of(const pc::RowInfo& r) {      // the spin's option: the client's, else the one the core kept
    int32_t v = 0;
    if (g_calls.spin_value && g_calls.spin_value(r.owner, r.id, v)) {
        v = pc::clamp_value(r.id, v);
        pc::store_value(r.id, v);
        return v;
    }
    if (!pc::stored_value(r.id, v)) v = 0;
    return pc::clamp_value(r.id, v);
}

// The carousel's own selected index: a press scrolls from it, and a page build leaves it at 0.
void enforce_index(MO* part, int32_t target, int32_t n) {
    MO* sl = option_list(part);
    int32_t cur = 0;
    if (!sl || !call_i32(sl, "get_SelectedIndex", cur) || cur == target) return;
    invoke_i(sl, "set_SelectedIndex(System.Int32)", target, SF6CORE_ARG_I64);
    if (call_i32(sl, "get_SelectedIndex", cur) && cur != target) {
        int32_t count = 0;
        if (call_i32(sl, "get_ItemCount", count) && count < n) invoke_i(sl, "set_ItemCount(System.Int32)", n, SF6CORE_ARG_I64);
        invoke_i(sl, "set_SelectedIndex(System.Int32)", target, SF6CORE_ARG_I64);
    }
}

// The visible option text: ours while the page is shown, compared first (R7).
void enforce_text(MO* part, const std::string& want) {
    MO* t = option_text(part);
    if (!t) return;
    MO* cur = call_obj(t, "get_Message");
    if (cur && ll::managed_string_to_utf8(cur) == want) return;
    invoke_i(t, "set_DisableMessageTag(System.Boolean)", 1, SF6CORE_ARG_BOOL);   // names shown literally ("<x>" is no tag)
    g_api->ui_set_text(g_self, t, want.c_str());
}

// Pooled groups whose arrows / option font we changed: put back when they show something else.
struct Moved { bool font = false; float w = 0, h = 0; };
std::mutex g_mv_mx;
std::map<int32_t, Moved> g_moved;
std::atomic<bool> g_any_moved{false};

void mark_moved(int32_t idx, bool font, float w, float h) {
    std::lock_guard lk(g_mv_mx);
    Moved& m = g_moved[idx];
    if (font && !m.font) { m.font = true; m.w = w; m.h = h; }
    g_any_moved = true;
}
bool take_moved(int32_t idx, Moved& out) {
    std::lock_guard lk(g_mv_mx);
    auto it = g_moved.find(idx);
    if (it == g_moved.end()) return false;
    out = it->second;
    g_moved.erase(it);
    g_any_moved = !g_moved.empty();
    return true;
}
std::vector<int32_t> moved_list() {
    std::lock_guard lk(g_mv_mx);
    std::vector<int32_t> v;
    for (auto& [idx, m] : g_moved) v.push_back(idx);
    return v;
}

bool place(MO* el, float x) {
    float px = 0, py = 0, pz = 0;
    if (!el || !call_pos(el, px, py, pz) || std::fabs(px - x) <= pc::kEpsilon) return false;
    g_api->ui_set_position(g_self, el, x, py, pz);
    return true;
}
// The two arrows of the spin part(s) of a pooled group: apart by `spread`, or the game's layout (0).
void arrows(MO* param, int32_t idx, float spread) {
    MO* gc = control_of(group_at(param, idx));
    for (MO* c = call_obj(gc, "get_Child"); c; c = call_obj(c, "get_Next")) {
        if (name_of(c).rfind("p_Spin", 0) != 0) continue;
        place(child(c, "p_MouseDecideOperable_L"), pc::arrow_x(false, spread));
        place(child(c, "p_MouseDecideOperable_R"), pc::arrow_x(true, spread));
    }
    if (spread > 0) mark_moved(idx, false, 0, 0);
}

// A long option text shrunk to the room between the arrows (pm_core::fit: stable, rate-limited).
void fit_row(int64_t row, MO* part, int32_t idx) {
    MO* t = option_text(part);
    float cw = 0, ch = 0, dw = 0, dh = 0;
    if (!t || !call_size(t, "get_FontSize", cw, ch) || !call_size(t, "get_DisplaySize", dw, dh)) return;
    const pc::Fit f = pc::fit(row, cw, ch, dw);
    if (!f.write) return;
    float bw = 0, bh = 0;
    if (pc::base_font(row, bw, bh)) mark_moved(idx, true, bw, bh);   // the game's own font, put back off the page
    g_api->ui_set_font_size(g_self, t, f.w, f.h);
}

void restore_group(MO* param, int32_t idx) {
    Moved m;
    if (!take_moved(idx, m)) return;
    arrows(param, idx, 0.0f);
    if (!m.font) return;
    MO* part = spin_part(param, idx);
    MO* t = part ? option_text(part) : nullptr;
    float cw = 0, ch = 0;
    if (t && call_size(t, "get_FontSize", cw, ch) && std::fabs(cw - m.w) > pc::kEpsilon)
        g_api->ui_set_font_size(g_self, t, m.w, m.h);
}

// The row's entry in the list is still that row.
bool entry_is(MO* param, int32_t idx, int64_t row) {
    const pc::Ident i = row_ident_of_entry(view_entry(param, idx));
    return (i.what == pc::What::Row || i.what == pc::What::Option) && i.id == row;
}

void notify(const std::vector<pc::Edge>& edges) {
    if (!g_calls.page_shown) return;
    for (const auto& e : edges) g_calls.page_shown(e.owner, e.page, e.shown);
}

// ======================== hooks ========================

#pragma warning(push)
#pragma warning(disable: 4733)

int32_t arg_i32(void* a) { return static_cast<int32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFF); }

void set_ctx(MO* data) {                      // a TrainingMenuData laid out on this thread
    if (!is_menu_data(data)) return;
    if (menu_ftype(data) != pc::kDynamic) { tl_ctx = pc::Ctx::None; tl_ctx_row = -1; return; }
    const pc::Ident i = ident_of(data);
    if (i.what == pc::What::None) { tl_ctx = pc::Ctx::Foreign; tl_ctx_row = -1; return; }
    tl_ctx = pc::Ctx::Ours;
    tl_ctx_row = i.what == pc::What::Page ? -1 : i.id;
}

// ---- TrainingMenuFunc.ViewUpdate(BaseParam, TrainingMenuData, Int32): the row being laid out ----
int pre_view_update(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    __try { set_ctx(reinterpret_cast<MO*>(argv[3])); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("ViewUpdate"); }
    perf::end(perf::PmHooks, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_none(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- the four FuncType-only methods and GetOptionIndex(ftype, wanted, option ftype) ----
thread_local int64_t tl_ret[5];
thread_local bool tl_ret_set[5];

int const_inner(int m, void** argv) {
    const pc::Route r = pc::route(arg_i32(argv[2]), tl_ctx, pc::game_rows());
    if (r == pc::Route::Pass) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    tl_ret[m] = pc::const_answer(static_cast<pc::Method>(m), r, m == pc::kGetOptionIndex ? arg_i32(argv[3]) : 0);
    tl_ret_set[m] = true;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
template <int M> int pre_const(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_ret_set[M] = false;
    if (!g_on.load(std::memory_order_relaxed) || core_closing() || arg_i32(argv[2]) != pc::kDynamic)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = const_inner(M, argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("TrainingMenuFunc (FuncType-only)"); }
    perf::end(perf::PmHooks, pt);
    return rc;
}
template <int M> void post_const(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_ret_set[M]) return;
    tl_ret_set[M] = false;
    *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(tl_ret[M]));
}

// ---- GetOptionText(ftype) / GetOptionText2(ftype) ----
thread_local std::string tl_text[2];
thread_local bool tl_text_set[2];

int text_inner(int which, void** argv) {
    const pc::Route r = pc::route(arg_i32(argv[2]), tl_ctx, pc::game_rows());
    if (r == pc::Route::Pass) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::string s;
    pc::RowInfo ri;
    if (which == 0 && r == pc::Route::Ours && tl_ctx_row >= 0 && pc::row_info(tl_ctx_row, ri) && ri.kind == pc::Kind::Spin)
        s = pc::label(ri.id, value_of(ri));
    tl_text[which] = std::move(s);
    tl_text_set[which] = true;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
void text_out(int which, void** ret_val) {
    MO* s = ll::create_utf8_string(tl_text[which]);       // a fresh string, one extra reference for the game (R6)
    if (s) s->add_ref();
    *ret_val = s;
}
template <int W> int pre_text(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_text_set[W] = false;
    if (!g_on.load(std::memory_order_relaxed) || core_closing() || arg_i32(argv[2]) != pc::kDynamic)
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = text_inner(W, argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("GetOptionText"); }
    perf::end(perf::PmHooks, pt);
    return rc;
}
template <int W> void post_text(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_text_set[W]) return;
    tl_text_set[W] = false;
    __try { text_out(W, ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("GetOptionText post"); }
}

// ---- Function(ftype, BaseParam, ViewData, value) ----
struct FnState { bool set; int64_t ret; int64_t page; MO* param; };
thread_local FnState tl_fn;

int64_t spin_function(const pc::FnPlan& plan, MO* param, MO* vd, int32_t idx) {
    pc::RowInfo r;
    if (!pc::row_info(plan.id, r)) return 0;
    int32_t cur = value_of(r);
    if (!plan.at_build) {                     // after a press the part's _Num is the option the game moved to
        MO* part = spin_part(param, idx);
        int32_t num = 0;
        if (part && call_i32(part, "get_Num", num)) {
            const int32_t w = pc::wrap(r.id, num);
            if (w != cur) {
                pc::store_value(r.id, w);
                if (g_calls.on_spin) g_calls.on_spin(r.owner, r.id, w);
            }
        }
        cur = value_of(r);
    }
    if (vd) {
        set_i64(vd, "SliderValue", cur);
        set_i64(vd, "Index", cur);
    }
    if (plan.at_build) {
        // The row text is the option object the ViewData points at: at the build it points at option 0.
        pc::Obj o = vd ? pc::option_data(r.id, cur) : nullptr;
        if (o) set_ref(vd, "Data", o);
        // The ViewData handed to Function at the build is not the list entry the flow renders from: that one too.
        MO* e = view_entry(param, idx);
        pc::Key k;
        if (e && menu_key(field_obj(e, "Data"), k) && k == r.key) {
            set_i64(e, "Index", cur);
            set_i64(e, "SliderValue", cur);
        }
    }
    return 0;
}

int function_inner(void** argv) {
    const int32_t ft = arg_i32(argv[2]);
    MO* param = reinterpret_cast<MO*>(argv[3]);
    MO* vd = reinterpret_cast<MO*>(argv[4]);
    const int32_t idx = arg_i32(argv[5]);
    if (ft != pc::kDynamic) {
        // a game page of a strip we use was selected: our pages there are no longer shown
        if (pc::any_shown()) {
            MO* gd = field_obj(vd, "Data");
            if (gd && pc::is_game_page(gd)) notify(pc::game_page_built(gd));
        }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    MO* d = field_obj(vd, "Data");
    pc::FnInput in;
    in.ftype = ft;
    in.frame = now();
    in.row_index = idx;
    in.ctx = tl_ctx;
    in.ctx_row = tl_ctx_row;
    in.data_null = d == nullptr;
    in.data = ident_of(d);
    if (in.data.what == pc::What::None && d) in.parent = ident_of(field_obj(vd, "ParentData"));
    if (d && in.data.what == pc::What::None && in.parent.what == pc::What::None) in.game_owned = game_owns(d);
    const pc::FnPlan plan = pc::plan_function(in);
    switch (plan.act) {
    case pc::FnAct::Pass:
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;          // the game's rows and unknown rows (ADR-0016 point 2)
    case pc::FnAct::Retired:
        tl_fn = { true, 0, -1, nullptr };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    case pc::FnAct::Page:                              // our page selected in its strip: built in the post-hook
        param_seen(param);
        tl_ctx = pc::Ctx::Ours; tl_ctx_row = -1;
        tl_fn = { true, 0, plan.id, param };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    case pc::FnAct::Button: {
        tl_ctx = pc::Ctx::Ours; tl_ctx_row = plan.id;
        int64_t ret = 0;
        if (!plan.at_build) {                          // a decide: synchronous, on this thread (ADR-0016 point 4)
            if (g_calls.decide) g_calls.decide(plan.owner, plan.id);
            ret = plan.keep_open ? 0 : 1;              // 1 = the game closes the menu
        }
        tl_fn = { true, ret, -1, nullptr };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    case pc::FnAct::Spin:
        tl_ctx = pc::Ctx::Ours; tl_ctx_row = plan.id;
        param_seen(param);
        tl_fn = { true, spin_function(plan, param, vd, idx), -1, nullptr };
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_function(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_fn = FnState{ false, 0, -1, nullptr };
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = function_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("Function"); rc = tl_fn.set ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL; }
    perf::end(perf::PmHooks, pt);
    return rc;
}

// Our page in its strip: shown, rows marked "at build", option arrays sized, then the rows built by the game's
// ViewUpdate(page) on this same thread, as its own record pages do after their Function.
void view_update(MO* param, int64_t page) {
    MO* pd = reinterpret_cast<MO*>(pc::page_data(page));
    if (!pd || !g_m_viewupdate || !param) return;
    tl_building = page;
    const auto r = g_m_viewupdate->invoke(nullptr, std::vector<void*>{ param, pd, ll::arg_i32(0) });
    tl_building = -1;
    if (r.exception_thrown) core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] ViewUpdate of page %lld threw", (long long)page);
}
void page_build(int64_t page, MO* param) {
    pc::building(page);                        // labels the owner sets from on_page_shown need no rebuild
    notify(pc::set_shown(page));
    pc::page_built(page, now());
    view_update(param, page);
}
void post_function(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_fn.set) return;
    const FnState s = tl_fn;
    tl_fn = FnState{ false, 0, -1, nullptr };
    *ret_val = reinterpret_cast<void*>(static_cast<uintptr_t>(s.ret));
    if (s.page < 0 || !g_on.load(std::memory_order_relaxed) || core_closing()) return;
    int64_t pt = perf::begin();
    __try { page_build(s.page, s.param); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("Function post (page build)"); }
    tl_building = -1;
    perf::end(perf::PmHooks, pt);
}

// ---- UIFlowTrainingMenu.Param.UpdateSpinBox(Int32, Boolean, Boolean): the spin repairs after the game's layout ----
struct SbFrame { int64_t row; MO* param; int32_t idx; };
thread_local SbFrame tl_sb[kNest];
thread_local int tl_sb_n = 0;

void spinbox_pre(int d, MO* param, int32_t idx) {
    tl_sb[d].param = param;
    tl_sb[d].idx = idx;
    if (!pc::any_shown() || !param || param->get_type_definition() != g_td_param) return;
    // the row at that index must be OUR data: the row list is shared by every page of the menu
    const pc::Ident i = row_ident_of_entry(view_entry(param, idx));
    if (!is_our_spin(i)) return;
    tl_sb[d].row = i.id;
    tl_ctx = pc::Ctx::Ours;
    tl_ctx_row = i.id;
    pc::set_row_index(i.id, idx);
    param_seen(param);
}
int pre_spinbox(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int d = tl_sb_n++;
    if (d >= kNest) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    tl_sb[d] = SbFrame{ -1, nullptr, -1 };
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!pc::any_shown() && !g_any_moved.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    __try { spinbox_pre(d, reinterpret_cast<MO*>(argv[1]), arg_i32(argv[2])); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("UpdateSpinBox"); }
    perf::end(perf::PmHooks, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void spinbox_post(const SbFrame f, bool outermost) {
    if (f.row < 0) { restore_group(f.param, f.idx); return; }   // a game row in a group we changed
    if (!outermost) return;                   // nested (our set_SelectedIndex re-enters it, R4): the outer one repairs
    pc::RowInfo r;
    MO* part = pc::row_info(f.row, r) ? spin_part(f.param, f.idx) : nullptr;
    if (!part) return;
    const int32_t v = value_of(r);
    enforce_index(part, v, r.labels_n);
    enforce_text(part, pc::label(r.id, v));
    if (r.spread > 0) fit_row(r.id, part, f.idx);
}
void post_spinbox(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int d = --tl_sb_n;
    if (d < 0) { tl_sb_n = 0; return; }
    if (d >= kNest || !tl_sb[d].param || !g_on.load(std::memory_order_relaxed) || core_closing()) return;
    if (tl_sb[d].row < 0 && !g_any_moved.load(std::memory_order_relaxed)) return;
    int64_t pt = perf::begin();
    __try { spinbox_post(tl_sb[d], d == 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("UpdateSpinBox post"); }
    perf::end(perf::PmHooks, pt);
}

// ---- UIPartsSpin.Init(min, max, num, action[, bool]) / set_Num(num): the option shown after a build or a focus ----
// The game initialises the spin part with its own num (0 for ours) at every build and focus, then calls set_Num with
// it: num replaced by the row's option, and set_Num held for kForceFrames after Init.
bool row_of_part(MO* part, int32_t& idx, MO*& gctrl) {
    MO* c = control_of(part);
    for (int i = 0; i < kGroupMaxDepth && c; ++i) {
        const std::string nm = name_of(c);
        if (nm.rfind("p_Group_", 0) == 0) {           // p_Group_<row>_...
            size_t p = 8;
            int32_t v = 0;
            while (p < nm.size() && nm[p] >= '0' && nm[p] <= '9') v = v * 10 + (nm[p++] - '0');
            if (p == 8 || p >= nm.size() || nm[p] != '_') return false;
            idx = v;
            gctrl = c;
            return true;
        }
        c = call_obj(c, "get_Parent");
    }
    return false;
}
int64_t spin_row_at(int32_t idx, MO* gctrl) {
    pc::RowInfo r;
    if (tl_building >= 0) {                   // our page's build on this thread: the index Function recorded
        const int64_t row = pc::row_at(idx);
        return row >= 0 && pc::row_info(row, r) && r.page == tl_building && r.kind == pc::Kind::Spin && !r.removed ? row : -1;
    }
    MO* param = param_get();
    if (!param) return -1;
    int64_t out = -1;
    if (control_of(group_at(param, idx)) == gctrl) {   // this part really is in row idx of the menu's list
        const pc::Ident i = row_ident_of_entry(view_entry(param, idx));
        if (is_our_spin(i)) out = i.id;
    }
    param_put(param);
    return out;
}
void num_inner(void** argv, int slot, bool init) {
    MO* part = reinterpret_cast<MO*>(argv[1]);
    if (!part || part->get_type_definition() != g_td_spin) return;
    int32_t idx = -1;
    MO* gctrl = nullptr;
    if (!row_of_part(part, idx, gctrl)) return;
    const int64_t row = spin_row_at(idx, gctrl);
    if (row < 0) return;
    const uint64_t f = now();
    if (!init && !pc::forcing(row, f)) return;
    pc::RowInfo r;
    if (!pc::row_info(row, r)) return;
    const int32_t target = value_of(r);
    if (init) pc::force(row, f);
    if (arg_i32(argv[slot]) != target) argv[slot] = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(target)));
}
template <int Slot, bool Init> int pre_num(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on.load(std::memory_order_relaxed) || core_closing() || !pc::any_shown()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin();
    __try { num_inner(argv, Slot, Init); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault(Init ? "UIPartsSpin.Init" : "UIPartsSpin.set_Num"); }
    perf::end(perf::PmHooks, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- TrainingManager.GetGuideData(focused data, ref List<InputGuideData>, ref List<String>): hints (R26) ----
thread_local void* tl_guide_focus = nullptr;
thread_local void** tl_guide_slot = nullptr;

int pre_guide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_guide_slot = nullptr;
    if (!g_on.load(std::memory_order_relaxed) || core_closing() || !g_calls.any_hints || !g_calls.any_hints())
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    tl_guide_focus = argv[2];
    tl_guide_slot = reinterpret_cast<void**>(argv[3]);    // the lists are created inside the call: read in the post
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void guide_post(void* focus, void** slot) {
    MO* list = slot ? reinterpret_cast<MO*>(*slot) : nullptr;
    if (!list) return;
    std::vector<Hint> hints;
    g_calls.hints(focus, hints);
    for (const Hint& h : hints)
        if (pc::Obj o = pc::hint_obj(h.owner, h.button, h.text)) g_api->list_add(g_self, list, o);
}
void post_guide(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_guide_slot) return;
    void** slot = tl_guide_slot;
    void* focus = tl_guide_focus;
    tl_guide_slot = nullptr;
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return;
    int64_t pt = perf::begin();
    __try { guide_post(focus, slot); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("GetGuideData post"); }
    perf::end(perf::PmHooks, pt);
}

// ---- UIPartsGroupScroll.UpdateScroll: per frame on the menu's thread ----
thread_local uintptr_t tl_scroll[kNest];
thread_local int tl_scroll_n = 0;

void rebuild(MO* param, int64_t page) {
    pc::page_built(page, now());              // rows "at build" (no decide, no change), arrays sized to the labels
    view_update(param, page);
}
void per_frame(MO* param, int64_t page) {
    for (int64_t rid : pc::take_retext(page)) {           // labels changed: the visible text at once
        pc::RowInfo r;
        if (!pc::row_info(rid, r) || r.row_index < 0 || !entry_is(param, r.row_index, rid)) continue;
        if (MO* part = spin_part(param, r.row_index)) enforce_text(part, pc::label(rid, value_of(r)));
    }
    std::vector<int32_t> kept;
    for (int64_t rid : pc::page_rows(page)) {
        pc::RowInfo r;
        if (!pc::row_info(rid, r) || r.kind != pc::Kind::Spin || r.spread <= 0 || r.row_index < 0) continue;
        if (!entry_is(param, r.row_index, rid)) continue;
        MO* part = spin_part(param, r.row_index);
        if (!part) continue;
        fit_row(rid, part, r.row_index);      // the game's focus state puts its own font back: checked per frame
        arrows(param, r.row_index, r.spread);
        kept.push_back(r.row_index);
    }
    for (int32_t idx : moved_list())
        if (std::find(kept.begin(), kept.end(), idx) == kept.end()) restore_group(param, idx);
}
void scroll_post(bool our_list) {
    MO* param = param_get();
    if (!param) return;
    if (!our_list) {
        // R4: the rebuild after a label change runs here, in ANOTHER group's update, 2 frames after the change, held
        // while an option window is up, dropped if the page is no longer shown
        const int64_t p = pc::take_rebuild(now(), dialogs::options::core::window_open());
        if (p >= 0 && page_shown_live(param, p)) rebuild(param, p);
    } else {
        const int64_t p = pc::shown_page();
        if (p >= 0 && page_shown_live(param, p)) per_frame(param, p);
        else for (int32_t idx : moved_list()) restore_group(param, idx);
    }
    param_put(param);
}
int pre_scroll(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int d = tl_scroll_n++;
    if (d < kNest) tl_scroll[d] = reinterpret_cast<uintptr_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_scroll(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int d = --tl_scroll_n;
    if (d < 0) { tl_scroll_n = 0; return; }
    if (d >= kNest || !g_on.load(std::memory_order_relaxed) || core_closing()) return;
    const uintptr_t ours = g_sec_list.load(std::memory_order_relaxed);
    const bool our_list = ours && tl_scroll[d] == ours;
    if (our_list ? (!pc::any_shown() && !g_any_moved.load(std::memory_order_relaxed)) : !pc::rebuild_pending()) return;
    int64_t pt = perf::begin();
    __try { scroll_post(our_list); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("UpdateScroll post"); }
    tl_building = -1;
    perf::end(perf::PmHooks, pt);
}

#pragma warning(pop)

// ======================== frame (game thread) ========================

uint64_t g_training_checked = 0;
bool g_training = false;
bool g_menu_up = false;

bool pause_up() {
    MO* pm = API::get()->get_managed_singleton("app.PauseManager");
    uint32_t b = kPauseNone;
    return pm && ll::read_field_u32(pm, "_CurrentPauseTypeBit", b) && b != kPauseNone && b != kPauseNone2;
}

void frame_inner(uint64_t f) {
    const int32_t flow = g_api->get_flow_id();
    const bool training_flow = flow == kTrainingFlow || flow == kHubTrainingFlow;
    if (!training_flow) g_training = false;
    else if (f - g_training_checked >= 60 || g_training_checked == 0) {
        MO* tm = training_manager();
        g_training = tm && field_obj(tm, "_tData");
        g_training_checked = f;
    }
    const bool paused = g_training && (pause_up() || dialogs::options::core::window_open());
    if (g_menu_up && !paused) {               // the menu closed: nothing shown, the menu's param let go
        notify(pc::menu_closed());
        param_drop();
    }
    g_menu_up = paused;
    pc::tick(f, g_training, paused);
    if (g_training && !paused && f % 60 == 20) scan_game_rows();
}
bool frame_seh(uint64_t f) {
    __try { frame_inner(f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// ======================== install ========================

struct Spec {
    const char* type;
    const char* sig;                          // full signature, or the name (the first method so called: as the
                                              // reference engine's get_method)
    REFPreHookFn pre;
    REFPostHookFn post;
    bool required;
    const char* without;                      // what the engine loses when the hook is refused
    API::Method* m = nullptr;
    bool hooked = false, refused = false;
    int shared = 0;
    std::string other;
    uintptr_t raw = 0, code = 0;
};
std::vector<Spec> g_specs;
uint64_t g_scan_ms = 0;
std::string g_refused;

}  // namespace

// ======================== public ========================

bool install(const Calls& calls, SF6CoreClient* engine_client) {
    g_calls = calls;
    g_self = engine_client;
    g_api = sf6core_api_ptr();
    {
        std::lock_guard lk(g_types_mx);
        if (!g_types_done) {
            auto* tdb = API::get()->tdb();
            if (!tdb) return false;
            g_types_done = true;
            g_td_menu = tdb->find_type("app.training.TrainingMenuData");
            if (g_td_menu) { g_f_msg = ll::find_field_walk(g_td_menu, "_MessageID"); g_f_ftype = ll::find_field_walk(g_td_menu, "_FuncType"); }
            g_td_param = tdb->find_type("app.training.UIFlowTrainingMenu.Param");
            g_td_spin = tdb->find_type("app.UIPartsSpin");
            g_td_guide = tdb->find_type("app.InputGuideData");
            if (TD* tmf = tdb->find_type("app.training.TrainingMenuFunc")) g_m_viewupdate = tmf->find_method("ViewUpdate");
        }
    }
    if (!g_self || !g_td_menu || !g_f_msg || !g_f_ftype || !g_td_param || !g_m_viewupdate) {
        core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] game types not found (TrainingMenuData %s, Param %s, ViewUpdate %s): off",
                 g_td_menu ? "ok" : "no", g_td_param ? "ok" : "no", g_m_viewupdate ? "ok" : "no");
        return false;
    }

    pc::Backend be;
    be.text_new = be_text_new; be.text_set = be_text_set; be.text_release = be_text_release;
    be.new_row_data = be_new_row_data; be.new_option_data = be_new_option_data; be.new_array = be_new_array;
    be.set_children = be_set_children; be.new_page_data = be_new_page_data; be.new_hint = be_new_hint;
    be.release = be_release; be.strip_read = be_strip_read; be.strip_write = be_strip_write; be.log = be_log;
    pc::set_backend(be);

    const char* tmf = "app.training.TrainingMenuFunc";
    g_specs = {
        { tmf, "GetIsActive", pre_const<pc::kGetIsActive>, post_const<pc::kGetIsActive>, false,
          "the game's own answer for the clients' rows (as for its own DYNAMIC rows)" },
        { tmf, "IsValueType", pre_const<pc::kIsValueType>, post_const<pc::kIsValueType>, false,
          "the game's own answer for the clients' rows" },
        { tmf, "GetVisibleCase", pre_const<pc::kGetVisibleCase>, post_const<pc::kGetVisibleCase>, false,
          "the game's own answer for the clients' rows" },
        { tmf, "IsChangedValue", pre_const<pc::kIsChangedValue>, post_const<pc::kIsChangedValue>, false,
          "the game's own answer for the clients' rows" },
        { tmf, "ViewUpdate", pre_view_update, post_none, false,
          "no row context from the layout (UpdateSpinBox and Function still give it)" },
        { tmf, "GetOptionIndex", pre_const<pc::kGetOptionIndex>, post_const<pc::kGetOptionIndex>, false,
          "the spins' display probes answered by the game" },
        { tmf, "GetOptionText", pre_text<0>, post_text<0>, false,
          "the game's option text for the clients' spins (the visible text is still repaired after UpdateSpinBox)" },
        { tmf, "GetOptionText2", pre_text<1>, post_text<1>, false, "the game's second option text for the clients' rows" },
        { tmf, "Function", pre_function, post_function, true, "no page, no decide: the engine is off" },
        { "app.training.UIFlowTrainingMenu.Param", "UpdateSpinBox(System.Int32, System.Boolean, System.Boolean)",
          pre_spinbox, post_spinbox, false, "no spin repair after the game's layout (carousel index, option text, font)" },
        { "app.UIPartsSpin", "Init(System.Int32, System.Int32, System.Int32, System.Action)", pre_num<4, true>,
          post_none, false, "a spin shows option 0 after a build or a focus until the next layout repair" },
        { "app.UIPartsSpin", "Init(System.Int32, System.Int32, System.Int32, System.Action, System.Boolean)",
          pre_num<4, true>, post_none, false, "a spin shows option 0 after a build or a focus until the next layout repair" },
        { "app.UIPartsSpin", "set_Num(System.Int32)", pre_num<2, false>, post_none, false,
          "the flow's own set_Num right after Init is not held: a spin may start a press from option 0 after a build or "
          "a focus (its text is repaired at the next layout)" },
        { "app.training.TrainingManager", "GetGuideData", pre_guide, post_guide, false, "no key-guide hints" },
        { "app.UIPartsGroupScroll", "UpdateScroll", pre_scroll, post_scroll, false,
          "no per-frame font fit / arrow reset, no page rebuild after a change of a shown spin's labels (the page shows "
          "them at its next build)" },
    };

    auto* tdb = API::get()->tdb();
    std::vector<FoldTarget> targets;
    for (auto& s : g_specs) {
        TD* td = tdb->find_type(s.type);
        s.m = td ? td->find_method(s.sig) : nullptr;
        FoldTarget t;
        if (s.m) {
            t.m = s.m;
            t.index = s.m->get_index();
            t.raw = reinterpret_cast<uintptr_t>(s.m->get_function_raw());
            t.code = t.raw ? code_of(t.raw) : 0;
            s.raw = t.raw;
            s.code = t.code;
        }
        targets.push_back(t);
    }
    // R18: every hooked method must own its machine code; a method folded with another one (or reached through the
    // same jump) would put our hook on every call of the others (the 1.10.5 slowdown, the 18/09 freeze).
    const uint64_t t0 = GetTickCount64();
    const bool scanned = fold_scan_seh(&targets);
    g_scan_ms = GetTickCount64() - t0;
    g_refused.clear();
    // the decision (pm_core::hook_verdicts, checked offline): a hook group = the same pre / post functions
    std::vector<pc::HookCode> codes;
    for (size_t i = 0; i < g_specs.size(); ++i) {
        pc::HookCode h;
        h.found = targets[i].m != nullptr;
        h.index = targets[i].index;
        h.raw = targets[i].raw;
        h.code = targets[i].code;
        h.shared = scanned ? targets[i].shared : -1;
        h.sharers = targets[i].sharers;
        h.group = (int)i;
        for (size_t j = 0; j < i; ++j)
            if (g_specs[j].pre == g_specs[i].pre && g_specs[j].post == g_specs[i].post) { h.group = codes[j].group; break; }
        codes.push_back(h);
    }
    const std::vector<pc::Verdict> verdicts = pc::hook_verdicts(codes);
    bool required_ok = true;
    int hooked = 0;
    for (size_t i = 0; i < g_specs.size(); ++i) {
        Spec& s = g_specs[i];
        const FoldTarget& t = targets[i];
        s.shared = scanned ? t.shared : -1;
        s.other = t.other ? method_label(t.other) : std::string();
        const std::string label = std::string(s.type) + "." + s.sig;
        switch (verdicts[i]) {
        case pc::Verdict::Missing:
            core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] hook %s: not found (%s)", label.c_str(), s.without);
            break;
        case pc::Verdict::Covered:
            core_log(SF6CORE_LOG_INFO, nullptr,
                     "[pausemenu] hook %s at 0x%llX (code 0x%llX): shares its code only with a method of the same hook (%s): covered by it",
                     label.c_str(), (unsigned long long)s.raw, (unsigned long long)s.code, s.other.c_str());
            break;
        case pc::Verdict::Refused: {
            s.refused = true;
            const std::string why = scanned ? "shares its code with " + std::to_string(s.shared) + " other method(s), e.g. " + s.other
                                            : std::string("the code check failed");
            core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] hook %s at 0x%llX (code 0x%llX, declared by %s): %s: REFUSED (R18): %s",
                     label.c_str(), (unsigned long long)s.raw, (unsigned long long)s.code, method_label(s.m).c_str(),
                     why.c_str(), s.without);
            g_refused += (g_refused.empty() ? "" : ", ") + std::string(s.sig);
            break;
        }
        case pc::Verdict::Hook:
            if (s.m->add_hook(s.pre, s.post, false)) {
                s.hooked = true;
                ++hooked;
                core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] hook %s at 0x%llX (code 0x%llX, declared by %s): %s, hooked",
                         label.c_str(), (unsigned long long)s.raw, (unsigned long long)s.code, method_label(s.m).c_str(),
                         t.shared ? "its code shared only within this hook" : "own code");
            } else {
                core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] hook %s: add_hook failed (%s)", label.c_str(), s.without);
            }
            break;
        }
        if (s.required && !s.hooked) required_ok = false;
    }
    core_log(SF6CORE_LOG_INFO, nullptr, "[pausemenu] %d / %zu hooks installed (code check over every method: %llu ms)%s",
             hooked, g_specs.size(), (unsigned long long)g_scan_ms, required_ok ? "" : ": the engine is off");
    g_on = required_ok;
    return required_ok;
}

bool ready() { return g_on.load(); }

void frame(uint64_t f) {
    if (!g_on.load(std::memory_order_relaxed) || core_closing()) return;
    int64_t pt = perf::begin();
    if (!frame_seh(f)) fault("frame");
    perf::end(perf::PmFrame, pt);
}

bool dyn_row_removable(void* dynamic_data) {
    {
        std::lock_guard lk(g_types_mx);
        if (!g_types_done) {
            auto* tdb = API::get()->tdb();
            if (!tdb) return false;
            if (!g_td_menu) g_td_menu = tdb->find_type("app.training.TrainingMenuData");
            if (g_td_menu && !g_f_msg) g_f_msg = ll::find_field_walk(g_td_menu, "_MessageID");
            if (g_td_menu && !g_f_ftype) g_f_ftype = ll::find_field_walk(g_td_menu, "_FuncType");
        }
    }
    if (!g_td_menu || !g_f_msg || !g_f_ftype) return false;
    MO* dd = reinterpret_cast<MO*>(dynamic_data);
    pc::Key k;
    if (!menu_key(field_obj(dd, "ChildData"), k)) return false;
    return pc::removable(k, has_delegate(dd) > 0);
}

namespace {
int32_t page_shown_inner(int64_t page) {
    MO* param = param_get();
    if (!param) return tl_building == page ? 1 : 0;
    const bool on = page_shown_live(param, page);
    param_put(param);
    return on ? 1 : 0;
}
bool page_shown_seh(int64_t page, int32_t* out) {
    __try { *out = page_shown_inner(page); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
void* row_part_inner(int64_t row) {
    pc::RowInfo r;
    if (!pc::row_info(row, r) || r.removed || r.row_index < 0) return nullptr;
    MO* param = param_get();
    if (!param) return nullptr;
    void* out = entry_is(param, r.row_index, row) ? group_at(param, r.row_index) : nullptr;
    param_put(param);
    return out;
}
bool row_part_seh(int64_t row, void** out) {
    __try { *out = row_part_inner(row); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
}  // namespace

int32_t page_shown(int64_t page) {
    if (!g_on.load() || core_closing()) return 0;
    int32_t v = 0;
    return page_shown_seh(page, &v) ? v : 0;
}

void* row_part(int64_t row) {
    if (!g_on.load() || core_closing()) return nullptr;
    void* p = nullptr;
    return row_part_seh(row, &p) ? p : nullptr;
}

std::string status() {
    int hooked = 0;
    for (const auto& s : g_specs) if (s.hooked) ++hooked;
    char b[200];
    std::snprintf(b, sizeof b, "on=%d faults=%d hooks %d/%zu (code check %llu ms)%s%s; ", g_on.load() ? 1 : 0,
                  g_faults.load(), hooked, g_specs.size(), (unsigned long long)g_scan_ms,
                  g_refused.empty() ? "" : ", refused: ", g_refused.c_str());
    return std::string(b) + pc::status();
}

}  // namespace pausemenu
