// SF6 Profile Mod (profile_mod.dll): two things on the Fighter Profile, and Zen Mode (zen_engine.inl, zen_tab.inl: ranks, points,
// win streaks and player details hidden, from a tab of Battle Settings).
//   - full_profile: another player's limited profile gives way to the full one.
//   - steam_name: R / pad X swaps the SteamID64 of the platform line for the player's Steam name, and back.
// reframework\data\profile_mod.ini, read at start: full_profile=1, steam_name=1 (only a 0 turns one off).
//
// The swap (from the Lua PostGameFullOppProfile, validated 31/08): no hook (the native Start is folded with other methods). When the
// simple profile's agent exists (a look every 15 frames), UIFlowManager._Handles gives its flow; 5 frames later its ResponseData goes
// to a new full profile's param, that flow ends, and app.UICFNFightersProfileTop.Start opens the full one, from UpdateBehavior, as the
// Lua. It only reuses what the server sent: no request, and what it left empty stays empty. Decisions: swap_logic.hpp.
//
// THE RULE: act only when the line is ActualVisible and its text holds a SteamID64. A hidden line stays hidden even though the id is
// in the data; a visible non-Steam id: the key is inert. The id is only ever read from what that visible line shows.
// The key is the game's where it uses it: on some tabs R / pad X is its Battle Settings (input id 110, when IsBattleSettingEnable) or
// its 286 (Avatar tab). When the guide bar's own entries show that key, ours is inert too, no entry, and a shown name goes back.
//
// Network: only on the first press for an id, never by itself: one HTTPS GET of the profile's XML on a thread of its own (WinHTTP,
// about 10 s at most), and when it has no name (a Steam account with no community profile set up) a second one of its HTML page. The
// answer is kept in memory for the session, nothing on disk. Meanwhile, or on failure, the line stays. No name at all: the guide says
// so and that id is not asked again this session; no answer: the next press tries again.
// Logs: never an id or a name, counts only.
//
// Rules: writes only from LateUpdateBehavior through the core (R1); the guide bar's copy inside the guide list's own update (R19);
// controls found by name each frame, no reference kept (R35); no state OnEnter / OnExit hook (R18); set_Message only when the text
// differs (rule 15). While neither screen exists: two core registry reads every 15 frames, nothing else.
//
// On screen means on screen: Battle Settings keeps CFNFighterProfileSimpleTop loaded, not drawn, its controls still ActualVisible, its
// line still holding his id: the agents alone say nothing (Wael 07/11: "R Pseudo Steam" on its Character tab). The key, the entry and the
// swap work only when the GameObject is drawn, no flow of Battle Settings' exists and the flow of that very profile does
// (psn::profile_allowed, read every frame such a GUI is drawn); leaving gives the line back at once, and on any other screen the key reads
// nothing, sends nothing, writes nothing. The entry is a copy on the resident InputGuide's bar, which every screen shows: a copy left
// visible comes back with that bar everywhere, so hiding it never depends on the bar being on screen, and goes over every bar.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include "reframework/API.hpp"
#include "sf6core/sf6core_api.h"
#include "steamname_logic.hpp"
#include "steamname_net.hpp"
#include "swap_logic.hpp"
#include "zen_logic.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using API = reframework::API;
using Obj = API::ManagedObject;
using TD = API::TypeDefinition;
using Method = API::Method;

constexpr const char* kVersion = "1.1.0";
constexpr const char* kAgentTop = "CFNFighterProfileTop";
constexpr const char* kAgentSimple = "CFNFighterProfileSimpleTop";
const char* const kScreens[] = { kAgentTop, kAgentSimple };
constexpr const char* kSettingsFile = "reframework/data/profile_mod.ini";
constexpr const char* kBanner = "p_FightersProfileBanner_";
constexpr const char* kLine = "e_text_platform";
constexpr const char* kHintName = "e_text_steamname_hint";    // not "p_...": never taken for a UI part (R20)
constexpr uint32_t kPollEvery = 15, kHintPass = 10, kLayoutFrames = 2, kMeasureMax = 30;
constexpr int kKeyId = 341;                                   // app.InputAssign.Digital.Id: R on the keyboard, as stages.cpp
constexpr int kModeKeyboard = 2;
constexpr int kMaxRefused = 3;

const SF6CoreAPI* g_core = nullptr;
SF6CoreClient* g_client = nullptr;
uint32_t g_frame = 0;

template <typename... A> void logi(const char* fmt, A... a) { API::get()->log_info(fmt, a...); }

// ---- the game's objects ----
API::Field* field_of(TD* td, const char* name) {
    for (auto* t = td; t; t = t->get_parent_type())
        if (auto* f = t->find_field(name)) return f;
    return nullptr;
}
Obj* get_obj(Obj* o, const char* name) {
    if (!o) return nullptr;
    auto* f = field_of(o->get_type_definition(), name);
    void* p = f ? f->get_data_raw(o, false) : nullptr;
    return p ? *reinterpret_cast<Obj**>(p) : nullptr;
}
Method* method_of(TD* td, const char* name, int nparams) {
    if (!td) return nullptr;
    if (auto* m = td->find_method(name))
        if (nparams < 0 || m->get_num_params() == static_cast<uint32_t>(nparams)) return m;
    for (auto* t = td; t; t = t->get_parent_type())
        for (auto* m : t->get_methods())
            if (m && m->get_name() && !strcmp(m->get_name(), name) && (nparams < 0 || m->get_num_params() == static_cast<uint32_t>(nparams))) return m;
    return nullptr;
}
reframework::InvokeRet call(Obj* o, const char* name, std::vector<void*> args = {}) {
    reframework::InvokeRet bad{};
    bad.exception_thrown = true;
    if (!o) return bad;
    Method* m = method_of(o->get_type_definition(), name, static_cast<int>(args.size()));
    return m ? m->invoke(o, args) : bad;
}
Obj* call_obj(Obj* o, const char* name, std::vector<void*> args = {}) {
    auto r = call(o, name, std::move(args));
    return !r.exception_thrown && r.ptr ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}
bool call_bool(Obj* o, const char* name, bool* ok = nullptr) {
    auto r = call(o, name);
    if (ok) *ok = !r.exception_thrown;
    return !r.exception_thrown && r.byte != 0;
}
void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }
std::string str(Obj* s) {
    if (!s) return {};
    const auto sz = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(s) + 0x10);
    if (sz <= 0 || sz > 8192) return {};
    auto* data = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(s) + 0x14);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, data, sz, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, sz, out.data(), bytes, nullptr, nullptr);
    return out;
}
int list_count(Obj* l) { auto r = call(l, "get_Count"); return r.exception_thrown ? 0 : static_cast<int>(r.dword); }
Obj* list_item(Obj* l, int i) { return call_obj(l, "get_Item", { arg_i32(i) }); }
bool actual_visible(Obj* c) { bool ok = false; const bool v = c && call_bool(c, "get_ActualVisible", &ok); return ok && v; }
std::string message(Obj* t) { return str(call_obj(t, "get_Message")); }
bool floats_of(Obj* o, const char* m, float* out, size_t n) {
    auto r = call(o, m);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * n);
    return true;
}
float drawn_width(Obj* o) {
    float s[2] = {};
    return floats_of(o, "get_DisplaySize", s, 2) && std::isfinite(s[0]) && s[0] > 0 ? s[0] : 0.0f;
}
void set_i64(void* c, const char* sig, int64_t v) {
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v;
    g_core->obj_invoke(g_client, c, sig, &a, 1, nullptr);
}

// The game's text language, as the Slots Loader's common.cpp
const char* lang_code(int32_t v) {
    switch (v) {
    case 0: return "ja"; case 1: return "en"; case 2: return "fr"; case 3: return "it"; case 4: return "de";
    case 5: return "es"; case 6: return "ru"; case 7: return "pl"; case 9: case 10: return "pt";
    case 11: return "ko"; case 12: return "zh-Hant"; case 13: return "zh-Hans"; case 21: return "ar"; case 32: return "es-419";
    default: return "en";
    }
}
std::string game_language() {
    auto* tdb = API::get()->tdb();
    Method* get = tdb->find_method("app.Option", "GetOptionValue");
    API::Field* list = tdb->find_field("app.Option", "LanguageList");
    int32_t val = -1;
    if (get && list) {
        auto ri = get->invoke(nullptr, std::vector<void*>{ arg_i32(611) });     // app.Option.ValueType.DispLanguage
        void* slot = ri.exception_thrown ? nullptr : list->get_data_raw(nullptr, false);
        Obj* arr = slot ? *reinterpret_cast<Obj**>(slot) : nullptr;
        if (arr) {
            auto rv = call(arr, "get_Item", { arg_i32(static_cast<int32_t>(ri.dword)) });
            if (!rv.exception_thrown) val = static_cast<int32_t>(rv.dword);
        }
    }
    if (val < 0)
        if (Method* sys = tdb->find_method("via.SystemService", "get_Language")) {
            auto r = sys->invoke(nullptr, std::vector<void*>{});
            if (!r.exception_thrown) val = static_cast<int32_t>(r.dword);
        }
    return lang_code(val);
}

// ---- the key ----
std::string symbol_of(int mode) {
    static std::map<int, std::string> cache;
    auto it = cache.find(mode);
    if (it != cache.end()) return it->second;
    auto* gm = API::get()->get_managed_singleton("app.InputGuideManager");
    if (!gm) return {};
    Method* m = method_of(gm->get_type_definition(), "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)", 4);
    if (!m) return {};
    auto r = m->invoke(gm, std::vector<void*>{ arg_i32(kKeyId), arg_i32(mode), arg_i32(0), arg_i32(0) });
    return cache[mode] = str(!r.exception_thrown && r.ptr ? reinterpret_cast<Obj*>(r.ptr) : nullptr);
}
int guide_mode_now() {
    auto* gm = API::get()->get_managed_singleton("app.InputGuideManager");
    Obj* modes = gm ? get_obj(gm, "_Modes") : nullptr;
    if (modes) {
        auto r = call(modes, "get_Item", { arg_i32(0) });
        if (!r.exception_thrown && static_cast<int32_t>(r.dword) > 0) return static_cast<int32_t>(r.dword);
    }
    return kModeKeyboard;
}
bool key_now() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return false;
    static unsigned sc = 0;
    static uint32_t sc_at = 0;
    if (!sc || g_frame - sc_at >= 120) { sc_at = g_frame; sc = psn::qwerty_scan(symbol_of(kModeKeyboard)); }
    const UINT vk = sc ? MapVirtualKeyW(sc, MAPVK_VSC_TO_VK) : 0;
    if (vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0) return true;
    static Method* merged = nullptr;
    static uint32_t bit = 64;                                                   // via.hid.GamePadButton.RLeft
    static bool init = false;
    auto* tdb = API::get()->tdb();
    if (!init) {
        init = true;
        TD* pad = tdb->find_type("via.hid.GamePad");
        merged = pad ? method_of(pad, "get_MergedDevice", 0) : nullptr;
        if (TD* btn = tdb->find_type("via.hid.GamePadButton"))
            if (auto* f = btn->find_field("RLeft")) bit = static_cast<uint32_t>(f->get_data<int32_t>(nullptr));
    }
    void* s = API::get()->get_native_singleton("via.hid.GamePad");
    if (!s || !merged) return false;
    auto rd = merged->invoke(reinterpret_cast<Obj*>(s), std::vector<void*>{});
    Obj* dev = !rd.exception_thrown && rd.ptr ? reinterpret_cast<Obj*>(rd.ptr) : nullptr;
    auto rb = call(dev, "get_Button");
    return !rb.exception_thrown && (rb.dword & bit) != 0;
}

// ---- the network: a thread per first press; the answers for the session ----
enum class Net { Pending, Ok, NoName, Failed };
struct Answer { Net state = Net::Pending; std::string name; };
std::mutex g_net_mx;
std::map<std::string, Answer> g_answers;      // the id -> its name (in memory only)

void fetch(std::string id) {                  // id: 17 digits, read from the visible line, checked before this
    std::thread([id] {
        std::wstring profile = L"/profiles/";
        for (const char c : id) profile += static_cast<wchar_t>(c);
        std::string xml, page;
        const bool xml_ok = psn::http_get(profile + L"/?xml=1", xml);
        bool page_ok = false;
        if (psn::needs_page(xml_ok, xml)) page_ok = psn::http_get(profile + L"/?l=english", page);
        const psn::NameAnswer got = psn::name_answer(xml_ok, xml, page_ok, page);
        {
            std::lock_guard lk(g_net_mx);
            Answer& a = g_answers[id];
            a.state = got.result == psn::NameResult::Name ? Net::Ok : got.result == psn::NameResult::NoName ? Net::NoName : Net::Failed;
            a.name = got.name;
        }
        logi("profile_mod: a request ended: %s", got.result == psn::NameResult::Name ? "name received" : got.result == psn::NameResult::NoName ? "no name in the answer" : "no answer");
    }).detach();
}

// ---- the guide bar's entry: a copy of the bar's item template (stages.cpp's way) ----
struct HintBar { Obj* agent = nullptr; Obj* part = nullptr; Obj* btn = nullptr; Obj* parent = nullptr; };
HintBar g_hbar;
struct { bool shown = false; uintptr_t styled = 0; std::string written; int cp = -1; uint32_t pass_at = 0, bar_at = 0; } g_hint;
std::atomic<bool> g_hint_req_on{ false };
std::atomic<uintptr_t> g_hint_req_part{ 0 };
std::atomic<uintptr_t> g_hint_made_parent{ 0 };
std::atomic<int> g_hint_refused{ 0 };
thread_local uintptr_t tl_hint_part = 0;
bool g_hook_tried = false, g_hook_on = false;

Obj* bar_template(Obj* btn, std::vector<Obj*>* entries) {
    Obj* tmpl = nullptr;
    int n = 0;
    for (Obj* c = btn ? call_obj(btn, "get_Child") : nullptr; c && n < 64; c = call_obj(c, "get_Next"), ++n) {
        const std::string nm = str(call_obj(c, "get_Name"));
        if (nm == "e_text_0") tmpl = c;
        else if (entries && (nm.rfind("e_text_0_g_", 0) == 0 || nm.rfind("e_text_0_d_", 0) == 0)) entries->push_back(c);
    }
    return tmpl;
}
void hint_make_copy(uintptr_t part) {
    if (g_hint_req_part.load() != part) return;
    g_hint_req_part.store(0);
    g_hint_req_on.store(false);
    Obj* ctrl = get_obj(reinterpret_cast<Obj*>(part), "<Control>k__BackingField");
    Obj* btn = ctrl ? reinterpret_cast<Obj*>(g_core->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
    std::vector<Obj*> entries;
    Obj* tmpl = bar_template(btn, &entries);
    Obj* parent = btn ? call_obj(btn, "get_Parent") : nullptr;
    if (!parent || !(tmpl || !entries.empty())) { ++g_hint_refused; return; }
    if (g_core->ui_find_child(parent, kHintName, 0)) return;
    void* c = nullptr;
    const SF6CoreStatus st = g_core->ui_clone_control(g_client, tmpl ? tmpl : entries.front(), parent, kHintName, &c);
    if (st != SF6CORE_OK || !c) { ++g_hint_refused; return; }
    g_hint_made_parent.store(reinterpret_cast<uintptr_t>(parent));
    set_i64(c, "set_MaskType(via.gui.MaskType)", 1);             // NonTarget: no mask of the new parent clips it
    g_core->ui_set_visible(g_client, c, 0);                      // placed and shown by the next frame
}
int pre_hint_late(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_hint_req_on.load(std::memory_order_acquire)) tl_hint_part = reinterpret_cast<uintptr_t>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_hint_late(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const uintptr_t p = tl_hint_part;
    tl_hint_part = 0;
    if (!p || !g_hint_req_on.load(std::memory_order_acquire)) return;
    try { hint_make_copy(p); } catch (...) { ++g_hint_refused; }
}
// On the guide list's update: only the first time an entry is wanted; while no copy is asked for, one atomic read per call
bool hint_hook() {
    if (g_hook_tried) return g_hook_on;
    g_hook_tried = true;
    TD* td = API::get()->tdb()->find_type("app.UIPartsInputGuideList");
    Method* m = td ? method_of(td, "OnLateUpdate", 0) : nullptr;
    g_hook_on = m && m->get_function_raw() && m->add_hook(pre_hint_late, post_hint_late, false);
    logi("profile_mod: guide bar entry %s", g_hook_on ? "on" : "off (no hook on the guide list's update)");
    return g_hook_on;
}
bool hint_find_bar(HintBar& b) {
    b = HintBar{};
    void* buf[4] = {};
    const uint32_t n = g_core->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* ag = reinterpret_cast<Obj*>(buf[i]);
        if (!actual_visible(call_obj(ag, "get_ControlMain"))) continue;
        Obj* lst = get_obj(get_obj(ag, "_PartsManager"), "_List");
        for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
            Obj* part = list_item(lst, k);
            if (!part || part->get_type_definition()->get_full_name() != "app.UIPartsInputGuideList") continue;
            Obj* ctrl = get_obj(part, "<Control>k__BackingField");
            Obj* btn = ctrl ? reinterpret_cast<Obj*>(g_core->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
            std::vector<Obj*> entries;
            bar_template(btn, &entries);
            if (entries.empty()) break;
            Obj* parent = call_obj(btn, "get_Parent");
            if (!parent) break;
            b = { ag, part, btn, parent };
            return true;
        }
    }
    return false;
}
bool hint_bar_live() {
    if (!g_hbar.agent) return false;
    void* buf[4] = {};
    const uint32_t n = g_core->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i)
        if (buf[i] == g_hbar.agent) return actual_visible(call_obj(g_hbar.agent, "get_ControlMain")) && g_hbar.parent->is_managed_object() && g_hbar.btn->is_managed_object();
    return false;
}
void* hint_copy() { return g_hbar.parent ? g_core->ui_find_child(g_hbar.parent, kHintName, 0) : nullptr; }
// The copy, on every bar of every InputGuide, on screen or not: found by name under each live bar (R35)
void hint_hide() {
    if (!g_hint.shown) return;
    g_hint.shown = false;
    void* buf[4] = {};
    const uint32_t n = g_core->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* lst = get_obj(get_obj(reinterpret_cast<Obj*>(buf[i]), "_PartsManager"), "_List");
        for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
            Obj* part = list_item(lst, k);
            if (!part || part->get_type_definition()->get_full_name() != "app.UIPartsInputGuideList") continue;
            Obj* ctrl = get_obj(part, "<Control>k__BackingField");
            Obj* btn = ctrl ? reinterpret_cast<Obj*>(g_core->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
            Obj* parent = btn ? call_obj(btn, "get_Parent") : nullptr;
            if (void* c = parent ? g_core->ui_find_child(parent, kHintName, 0) : nullptr) g_core->ui_set_visible(g_client, c, 0);
        }
    }
}
// The game uses the key on this screen now: its own entries in the bar show its symbol (ours is not one of them). Read every kHintPass frames.
bool key_taken() {
    static uint32_t at = 0;
    static bool taken = false;
    if (at && g_frame - at < kHintPass) return taken;
    at = g_frame;
    taken = false;
    if (!hint_bar_live()) {
        HintBar nb;
        if (!hint_find_bar(nb)) return false;
        if (nb.parent != g_hbar.parent) { hint_hide(); g_hint.pass_at = 0; }
        g_hbar = nb;
    }
    const std::string sym = symbol_of(guide_mode_now());
    if (sym.empty()) return false;
    std::vector<Obj*> entries;
    bar_template(g_hbar.btn, &entries);
    for (Obj* e : entries) {
        bool ok = false;
        if (call_bool(e, "get_Visible", &ok) && ok && message(e).find(sym) != std::string::npos) { taken = true; break; }
    }
    return taken;
}
struct Geo { float x = 0, y = 0, z = 0, sx = 1, sy = 1; int cp = 0; };
bool geo_of(Obj* o, Geo& g) {
    float p[3], s[3];
    if (!floats_of(o, "get_Position", p, 3)) return false;
    g.x = p[0]; g.y = p[1]; g.z = p[2];
    if (floats_of(o, "get_Scale", s, 3)) { g.sx = s[0]; g.sy = s[1]; }
    auto r = call(o, "get_ControlPoint");
    g.cp = r.exception_thrown ? 0 : static_cast<int>(r.dword);
    if (g.cp < 0 || g.cp > 10 || g.cp % 4 > 2) g.cp = 0;
    return true;
}
double glyph_count(const std::string& s) {     // a tag such as <ICON KeyF> counted as two (its glyph and a space)
    double n = 0;
    bool tag = false;
    for (const unsigned char ch : s) {
        if (ch == '<') { tag = true; n += 2; continue; }
        if (tag) { if (ch == '>') tag = false; continue; }
        if ((ch & 0xC0) != 0x80) n += 1;
    }
    return n;
}
void hint_pass(const std::string& text) {
    std::vector<Obj*> entries;
    bar_template(g_hbar.btn, &entries);
    void* c = hint_copy();
    if (!c) {
        g_hint.shown = false;
        if (g_hint_refused.load() >= kMaxRefused) return;
        if (g_hint_made_parent.load() == reinterpret_cast<uintptr_t>(g_hbar.parent)) return;   // never a second copy under one parent
        g_hint_req_part.store(reinterpret_cast<uintptr_t>(g_hbar.part));
        g_hint_req_on.store(true, std::memory_order_release);
        return;
    }
    Geo b;
    if (!geo_of(g_hbar.btn, b)) { hint_hide(); return; }
    struct Span { double l, r; Geo g; Obj* o; };
    std::vector<Span> spans;
    for (Obj* e : entries) {
        bool ok = false;
        if (!call_bool(e, "get_Visible", &ok) || !ok) continue;
        Geo g;
        const float w = drawn_width(e);
        if (message(e).empty() || !(w > 0) || !geo_of(e, g)) continue;
        const double l = g.x - (g.cp % 4) * 0.5 * w * g.sx;
        spans.push_back({ l, l + w * g.sx, g, e });
    }
    if (spans.empty()) { hint_hide(); return; }
    std::sort(spans.begin(), spans.end(), [](const Span& p, const Span& q) { return p.l < q.l; });
    double gap = 0;
    for (size_t i = 1; i < spans.size(); ++i) {
        const double d = spans[i].l - spans[i - 1].r;
        if (d > 0 && (gap <= 0 || d < gap)) gap = d;
    }
    const Span& last = spans.back();
    if (gap <= 0) {                                          // one text holding every entry: six spaces (the bar's separator)
        const double gl = glyph_count(message(last.o));
        gap = gl > 0 ? (last.r - last.l) / gl * 6 : 40;
    }
    const float x = static_cast<float>(b.x + b.sx * (last.r + gap));
    const float y = static_cast<float>(b.y + b.sy * last.g.y);
    const int cp = (last.g.cp / 4) * 4;
    if (reinterpret_cast<uintptr_t>(c) != g_hint.styled) {
        g_hint.styled = reinterpret_cast<uintptr_t>(c);
        g_hint.written.clear();
        g_hint.cp = -1;
    }
    if (cp != g_hint.cp) { set_i64(c, "set_ControlPoint(via.gui.ControlPoint)", cp); g_hint.cp = cp; }
    if (text != g_hint.written && g_core->ui_set_text(g_client, c, text.c_str()) == SF6CORE_OK) g_hint.written = text;
    g_core->ui_set_scale(g_client, c, b.sx * last.g.sx, b.sy * last.g.sy, 1.0f);
    g_core->ui_set_position(g_client, c, x, y, last.g.z);
    g_core->ui_set_force_invisible(g_client, c, 0);          // the template's lock, copied with it
    g_core->ui_set_visible(g_client, c, 1);
    g_hint.shown = true;
}
void hint_frame(bool want, const std::string& label) {
    if (!want) { hint_hide(); return; }
    if (!hint_hook()) return;
    const std::string text = symbol_of(guide_mode_now()) + " " + label;
    if (!hint_bar_live() || g_frame - g_hint.bar_at > 60) {
        g_hint.bar_at = g_frame;
        HintBar nb;
        const bool ok = hint_find_bar(nb);
        if (!ok || nb.parent != g_hbar.parent) { hint_hide(); g_hint.pass_at = 0; }
        if (!ok) return;
        g_hbar = nb;
    }
    if (g_hint.pass_at && g_frame - g_hint.pass_at < kHintPass && text == g_hint.written && g_hint.shown) return;
    g_hint.pass_at = g_frame;
    hint_pass(text);
}

// ---- the line ----
struct Line {
    bool showing = false;
    uintptr_t text = 0;                       // the control, compared only (R35)
    std::string original, written, name;      // the game's line, what we wrote, the whole name
    size_t id_at = 0, keep = 0;
    float region = 0;
    uint32_t written_at = 0, shown_at = 0;
    bool settled = false, fit_was = true;
} g_line;
std::string g_wanted;                         // the id of a press whose answer is awaited
std::string g_lang = "en";
bool g_up = false, g_key_was = false;

// A write only when the text differs (rule 15); what the game then holds is what is compared from then on
bool write_line(Obj* t, const std::string& text) {
    if (message(t) != text && g_core->ui_set_text(g_client, t, text.c_str()) != SF6CORE_OK) return false;
    g_line.written = message(t);
    g_line.written_at = g_frame;
    return true;
}
void show_name(Obj* t, const std::string& cur, size_t id_at, const std::string& name) {
    float region[2] = {};
    floats_of(t, "get_RegionSize", region, 2);
    bool read_ok = false;
    const bool fit = call_bool(t, "get_AutoRegionFit", &read_ok);
    g_line = Line{};
    g_line.fit_was = read_ok ? fit : true;
    g_line.region = region[0];
    g_line.text = reinterpret_cast<uintptr_t>(t);
    g_line.original = cur;
    g_line.name = name;
    g_line.id_at = id_at;
    g_line.keep = name.size();
    g_line.shown_at = g_frame;
    set_i64(t, "set_AutoRegionFit(System.Boolean)", 0);          // never squeezed: cut with "..." instead
    if (!write_line(t, psn::line_with_name(cur, id_at, name))) {
        set_i64(t, "set_AutoRegionFit(System.Boolean)", g_line.fit_was ? 1 : 0);
        g_line = Line{};
        return;
    }
    g_line.showing = true;
}
void fit_step(Obj* t) {
    if (g_line.settled) return;
    if (g_frame < g_line.written_at + kLayoutFrames) return;
    const float drawn = drawn_width(t);
    if (drawn <= 0 && g_frame - g_line.shown_at < kMeasureMax) return;
    const size_t next = psn::next_keep(g_line.keep, psn::shown_bytes(g_line.written), drawn, g_line.region);
    if (g_line.region > 0 && drawn > 0 && next != g_line.keep) {
        g_line.keep = next;
        write_line(t, psn::line_with_name(g_line.original, g_line.id_at, psn::cut_name(g_line.name, next)));
        return;
    }
    g_line.settled = true;
}
// The game's line and its fit back; nothing of ours left on the control
void give_back(Obj* t, bool write_it) {
    if (write_it && message(t) != g_line.original) g_core->ui_set_text(g_client, t, g_line.original.c_str());
    set_i64(t, "set_AutoRegionFit(System.Boolean)", g_line.fit_was ? 1 : 0);
    g_line = Line{};
}

// The main control of an agent of that name really drawn; nullptr none (psn::profile_drawn)
Obj* drawn_main(const char* name) {
    void* buf[2] = {};
    const uint32_t n = g_core->ui_agents(name, buf, 2);
    for (uint32_t i = 0; i < n && i < 2; ++i) {
        Obj* agent = reinterpret_cast<Obj*>(buf[i]);
        Obj* go = call_obj(agent, "get_GameObject");
        bool ok = false;
        const bool draw = go && call_bool(go, "get_DrawSelf", &ok) && ok;
        Obj* main = draw ? call_obj(agent, "get_ControlMain") : nullptr;
        if (psn::profile_drawn(draw, actual_visible(main))) return main;
    }
    return nullptr;
}
// The UIFlowManager's flows now, by their params' types
std::vector<std::string> flows_now() {
    std::vector<std::string> flows;
    auto* fm = API::get()->get_managed_singleton("app.UIFlowManager");
    Obj* hs = fm ? get_obj(fm, "_Handles") : nullptr;
    for (int i = 0, n = list_count(hs); i < n; ++i) {
        Obj* h = list_item(hs, i);
        Obj* p = h ? get_obj(h, "<Param>k__BackingField") : nullptr;
        if (p) flows.push_back(p->get_type_definition()->get_full_name());
    }
    return flows;
}
// The screen's main control: drawn, and a real profile (psn::profile_allowed, read now); nullptr otherwise: nothing to do, anywhere else
Obj* screen_up() {
    for (const char* name : kScreens) {
        Obj* main = drawn_main(name);
        if (!main) continue;
        const bool allowed = psn::profile_allowed(flows_now(), strcmp(name, kAgentTop) == 0);
        static bool last_allowed = true;
        if (allowed != last_allowed) {
            last_allowed = allowed;
            if (!allowed) logi("profile_mod: %s is drawn, not as a profile opened: nothing from us", name);
        }
        return allowed ? main : nullptr;
    }
    return nullptr;
}
Obj* find_line(Obj* main) {
    void* banner = g_core->ui_find_cached(main, kBanner);
    return banner ? static_cast<Obj*>(g_core->ui_find_cached(banner, kLine)) : nullptr;
}

// The screen is gone, or hidden with its controls still there: nothing of ours left. Its agents looked for again by name.
void went_away() {
    if (g_line.showing)
        for (const char* name : kScreens) {
            void* buf[2] = {};
            const uint32_t n = g_core->ui_agents(name, buf, 2);
            for (uint32_t i = 0; i < n && i < 2; ++i) {
                Obj* main = call_obj(reinterpret_cast<Obj*>(buf[i]), "get_ControlMain");
                Obj* t = main ? find_line(main) : nullptr;
                if (t && reinterpret_cast<uintptr_t>(t) == g_line.text) { give_back(t, true); break; }
            }
        }
    g_line = Line{};
    g_wanted.clear();
    hint_hide();
    g_up = false;
}

void tick() {
    Obj* main = screen_up();
    if (!main) {
        if (g_up) went_away();
        return;
    }
    if (!g_up) {
        g_up = true;
        g_lang = game_language();
        g_key_was = key_now();                // a key held on arrival acts only once released
        g_wanted.clear();
    }
    Obj* t = find_line(main);
    if (!t) { g_line = Line{}; g_wanted.clear(); hint_hide(); return; }
    const bool vis = actual_visible(t);
    const std::string cur = message(t);
    if (g_line.showing) {
        if (reinterpret_cast<uintptr_t>(t) != g_line.text) g_line = Line{};   // another control: ours went with the old one
        else if (cur != g_line.written) give_back(t, false);                  // the game rewrote it: its text stays
        else if (!vis) give_back(t, true);                                    // a hidden line: as the game had it
    }
    const bool down = key_now();
    const bool press = down && !g_key_was;
    g_key_was = down;
    const bool taken = key_taken();
    if (g_line.showing && taken) give_back(t, true);                          // the key is the game's here
    if (g_line.showing) {
        fit_step(t);
        hint_frame(true, psn::word_id(g_lang));
        if (press) { give_back(t, true); hint_frame(true, psn::word_name(g_lang)); }
        return;
    }
    const size_t at = psn::find_steam_id(cur);
    if (!vis || at == std::string::npos || taken) {   // THE RULE, or the game's key: inert, no entry
        g_wanted.clear();
        hint_hide();
        return;
    }
    const std::string id = cur.substr(at, psn::kIdLen);
    bool no_name = false;
    {
        std::lock_guard lk(g_net_mx);
        auto it = g_answers.find(id);
        no_name = it != g_answers.end() && it->second.state == Net::NoName;
    }
    hint_frame(true, no_name ? psn::word_unavailable(g_lang) : psn::word_name(g_lang));
    if (!g_wanted.empty() && g_wanted != id) g_wanted.clear();                // another player: that press is over
    if (press && g_wanted.empty() && !no_name) {                              // no name: the key does nothing more for that id
        g_wanted = id;
        bool start = false;
        {
            std::lock_guard lk(g_net_mx);
            auto it = g_answers.find(id);
            if (it == g_answers.end() || it->second.state == Net::Failed) { g_answers[id] = Answer{}; start = true; }   // a failure: this press tries again
        }
        if (start) fetch(id);
    }
    if (!g_wanted.empty()) {
        Answer a;
        {
            std::lock_guard lk(g_net_mx);
            auto it = g_answers.find(g_wanted);
            if (it != g_answers.end()) a = it->second;
        }
        if (a.state == Net::Ok) { g_wanted.clear(); show_name(t, cur, at, a.name); }
        else if (a.state != Net::Pending) g_wanted.clear();                   // the id stays
    }
}

void on_late_update() {
    ++g_frame;
    if (!g_core || !g_client) return;
    if (!g_up && (g_frame % kPollEvery) != 0) return;
    try {
        tick();
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logi("profile_mod: an exception in a frame: its state dropped"); }
        g_line = Line{};
        g_wanted.clear();
        g_up = false;
        try { hint_hide(); } catch (...) { g_hint.shown = false; }
    }
}

// ---- the swap ----
TD* t_simple = nullptr;                       // the simple profile's param
TD* t_top = nullptr;                          // the full profile's param
Method* m_top_start = nullptr;
psn::SwapTracker g_swap;
uint32_t g_swap_frame = 0;

struct Flows { Obj* simple_handle = nullptr; Obj* simple_param = nullptr; bool top = false, battle_settings = false; };
Flows walk_flows() {
    Flows f;
    auto* fm = API::get()->get_managed_singleton("app.UIFlowManager");
    Obj* hs = fm ? get_obj(fm, "_Handles") : nullptr;
    for (int i = 0, n = list_count(hs); i < n; ++i) {
        Obj* h = list_item(hs, i);
        Obj* p = h ? get_obj(h, "<Param>k__BackingField") : nullptr;
        if (!p) continue;
        TD* td = p->get_type_definition();
        if (td == t_simple) { f.simple_handle = h; f.simple_param = p; }
        else if (td == t_top) f.top = true;
        else if (psn::flow_is_battle_settings(td->get_full_name())) f.battle_settings = true;
    }
    return f;
}
// `h`: found in the handles this frame, with a param of the simple profile's exact type. The new param is made before the flow ends: a
// failure leaves the simple profile as it was. The two references are kept for good: the flow's, one small object per swap.
bool do_swap(Obj* h, Obj* param) {
    Obj* resp = call_obj(param, "get_ResponseData");
    if (!resp) { logi("profile_mod: swap: no response in the simple profile: it stays"); return false; }
    Obj* top = t_top->create_instance(REFRAMEWORK_CREATE_INSTANCE_FLAGS_SIMPLIFY);
    if (!top) { logi("profile_mod: swap: the full profile's param not made: the simple one stays"); return false; }
    resp->add_ref();
    top->add_ref();
    if (call(top, "set_ResponseData", { reinterpret_cast<void*>(resp) }).exception_thrown) {
        logi("profile_mod: swap: the response not accepted: the simple one stays");
        return false;
    }
    call(top, "set_ThemeId", { arg_i32(1) });
    call(top, "set_IsBattleSettingEnable", { arg_i32(0) });
    call(top, "set_IsFromMultiMenu", { arg_i32(0) });
    call(h, "End");
    auto r = m_top_start->invoke(nullptr, std::vector<void*>{ reinterpret_cast<void*>(top) });
    const bool ok = !r.exception_thrown && r.ptr;
    logi("profile_mod: swap: %s", ok ? "the full profile started" : "the full profile not started");
    return ok;
}
void swap_tick() {
    ++g_swap_frame;
    if (!g_swap.pending && (g_swap_frame % kPollEvery) != 0) return;   // nothing awaited: the two registry reads every 15 frames only
    bool top = drawn_main(kAgentTop) != nullptr;
    const bool simple_drawn = drawn_main(kAgentSimple) != nullptr;   // Battle Settings keeps one loaded, not drawn: not a profile opened
    Flows f;
    if (simple_drawn || g_swap.pending) {
        f = walk_flows();
        top = top || f.top;
    }
    if (!simple_drawn || f.battle_settings) f.simple_handle = nullptr;
    switch (g_swap.step(reinterpret_cast<uintptr_t>(f.simple_handle), top, g_swap_frame)) {
    case psn::SwapTracker::Do::Swap:
        if (f.simple_handle && do_swap(f.simple_handle, f.simple_param)) g_swap.swapped(g_swap_frame);
        break;
    case psn::SwapTracker::Do::LeaveAlone:
        logi("profile_mod: swap: a simple profile left as it is (%s)", top ? "a full profile is up" : "a full profile just closed");
        break;
    default:
        break;
    }
}
void on_update() {
    if (!g_core || !g_client) return;
    try {
        swap_tick();
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logi("profile_mod: an exception at the swap"); }
    }
}
#include "zen_engine.inl"
#include "zen_tab.inl"

psn::Settings read_settings() {
    FILE* f = fopen(kSettingsFile, "rb");
    if (!f) return {};
    std::string text(4096, '\0');
    text.resize(fread(text.data(), 1, text.size(), f));
    fclose(f);
    return psn::parse_settings(text);
}

}  // namespace

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1;
    v->minor = 10;    // API of REFramework 1.5.8
    v->patch = 0;
    v->game_name = nullptr;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->functions->on_pre_application_entry) return false;
    try { API::initialize(param); } catch (...) { return false; }
    const psn::Settings set = read_settings();
    HMODULE h = GetModuleHandleW(L"sf6core_native.dll");
    auto fn = h ? reinterpret_cast<SF6CoreGetApiFn>(GetProcAddress(h, "sf6core_get_api")) : nullptr;
    const SF6CoreAPI* a = fn ? fn(SF6CORE_ABI_VERSION) : nullptr;
    if (!a || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, ui_find_cached) || !SF6CORE_HAS(a, ui_set_scale)) {
        logi("profile_mod: off (needs the SF6 Native Core 0.8.2 or newer)");
        return true;
    }
    SF6CoreClientDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.client_id = "profile_mod";
    desc.client_version = kVersion;
    desc.priority = 100;
    if (a->register_client(&desc, &g_client) != SF6CORE_OK) { logi("profile_mod: off (the core refused it)"); return true; }
    g_core = a;
    bool full = set.full_profile;
    if (full) {
        auto* tdb = API::get()->tdb();
        t_simple = tdb->find_type("app.UICFNFightersProfileSimpleTop.FlowParam");
        t_top = tdb->find_type("app.UICFNFightersProfileTop.FlowParam");
        TD* top = tdb->find_type("app.UICFNFightersProfileTop");
        m_top_start = top ? method_of(top, "Start(app.UICFNFightersProfileTop.FlowParam)", 1) : nullptr;
        if (!t_simple || !t_top || !m_top_start) { full = false; logi("profile_mod: the full profile's flow not found (game update?): no swap"); }
    }
    if (full) param->functions->on_pre_application_entry("UpdateBehavior", on_update);
    if (set.steam_name) param->functions->on_pre_application_entry("LateUpdateBehavior", on_late_update);
    logi("profile_mod: on (full profile %d, Steam name %d)", full ? 1 : 0, set.steam_name ? 1 : 0);
    try { zen_init(param); } catch (...) { logi("profile_mod: Zen Mode off (an exception at start)"); }
    return true;
}

// The version the SF6 Mod Suite updater reads to know what is installed (updater/src/update_game.cpp: the loaded DLL's export, never its
// bytes). Every package's main DLL has one.
extern "C" __declspec(dllexport) const char* sf6suite_package_version(void) { return kVersion; }
