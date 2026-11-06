// SF6 Slots — Replays > Settings: the game's music screens, started from the Replay Music row, and the row's Custom list at a
// replay's load.
//
// Character Music is the game's own flow, app.UIFlowCharaBgmSetting. Its static Start is what Battle Settings and the Options
// menu call; unlike Opponent Outfit Settings' page (replay_tab.cpp's top comment), its screen comes from a prefab by key
// (UIParam.get_ObjectPrefabKey, "CharaBgmSetting"), not from an object of Battle Settings' scene. Started with Battle Settings'
// scene type (it names the route in the game's own log). On Back the flow saves what changed itself, as anywhere else.
//
// The playlist dialog is app.UIFlowCharaBgmSelect: it has no Start of its own, and UIFlowManager.Start is generic (no address
// to call). Its parent screen starts it in Flow_CharaBgmSelect.OnEnter, which only reads two numbers of that screen's param
// (the player, the fighter) and stores the handle. So: a state object of that type and a param of that screen, ours, for that one
// call. The state's OnUpdate never runs: that is what writes the choice to the fighter (UIParam.ApplySettingData), and its
// param's OnEnd what saves (RequestSaveData); the dialog itself only reads the save, and writes neither (read 06/11).
// Our list on it: UIParam.InitMenu fills the marks from the fighter it was given (a fighter it must have: its name comes from it,
// a null name throws) and the heading with that name. Post-hook, in the same call, before anything is shown: the marks replaced by
// ours (InitBgmList draws them again), never a fighter's; the heading from LateUpdateBehavior (R1). The fighter is used nowhere
// after InitMenu. What is marked is read each frame from the running dialog.
//
// At a replay's load: BattleFlowMap.loadBgmPrefab(type, random) gets the fighter's playlist from the static
// SystemSaveManager.GetCharaBgmIdSettingList(fighter), a new list each call filled from the save's. For Custom the replay's
// description says Player1, and while that load runs on the armed flow map, the post-hook of the getter replaces the content of
// that new list. Never the save's own: checked by address each time, refused otherwise. The game then loads those tracks and picks
// one at random as for any playlist. Outside that load, on another map, or on a network description, the hooks do nothing.
//
// While a screen runs, the game's UIFlowManager holds its handle: looked for by the param's type, no reference kept (R35).
// Every hook is installed only after the R18 code check of its method (codecheck_rt.hpp, cache codecheck_music.txt).
#include "slots.hpp"
#include "slots_ui.hpp"
#include "codecheck_rt.hpp"
#include "replay_bgm.hpp"
#include "replay_music.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

namespace slots::replaymusic {
namespace {

constexpr int kSceneBattleSetting = 2;      // app.UIFlowCharaBgmSetting.eOpenSceneType.BattleSetting
constexpr int kPlayer = 0;                  // the menus' player
constexpr int kGoneFrames = 10;             // no handle for that long: over (a handle may be added a frame late)
constexpr uint32_t kDialogFighter = 1;      // the fighter InitMenu reads, Ryu: nothing of him is ever shown
constexpr int kLoadPlayer1 = 1;             // loadBgmPrefab's type: app.EBgmSet.Player1
constexpr float kTitleRegionW = 1000;       // the dialog's heading: wider than any of ours, never squeezed nor cut
constexpr const char* kCodecheckCache = "reframework/data/SF6_CostumeSlots_data/codecheck_music.txt";

enum Hook : size_t { kInitMenu, kLoadBgm, kGetList, kHooks };
codecheck::Batch g_check;
bool g_check_started = false, g_hooks_done = false;
std::atomic<bool> g_hooked[kHooks] = {};

bool g_init = false;
TD* t_param = nullptr;                       // the Character Music screen's param
TD* t_state = nullptr;                       // its state that starts the dialog
TD* t_dialog = nullptr;                      // the dialog's param
Method* m_start = nullptr;
Method* m_enter = nullptr;
Method* m_active = nullptr;
Method* m_init_list = nullptr;               // the dialog's UIParam.InitBgmList
Method* m_clear = nullptr;                   // List<UInt32>
Method* m_add = nullptr;
Method* m_count = nullptr;
Method* m_saved = nullptr;                   // SystemSaveManager.GetFighterSettingData
Method* m_inv_bgm = nullptr;                 // InventoryManager.get_Bgm
Method* m_inv_list = nullptr;                // its GetInventoryBgmUIDataList_CharacterBgmSetting

// ---- the running screen (game thread) ----
TD* g_watch = nullptr;                       // the param's type of the screen started
int g_gone = 0;
std::vector<uint32_t> g_marked;              // the dialog's marked tracks, as last read
bool g_marked_read = false, g_result_ready = false;
std::string g_title;
bool g_title_fit = false;                    // its heading made wide enough, for this dialog

// ---- the dialog's hook ----
std::atomic<bool> g_custom_open{ false };    // the dialog started here is the next InitMenu
std::atomic<bool> g_filled{ false };         // its marks are ours: from then on what it has marked is the answer
std::mutex g_mx;                             // g_open_list, g_swap_list
std::vector<uint32_t> g_open_list;
thread_local Obj* tl_menu = nullptr;

// ---- the replay's hooks ----
std::vector<uint32_t> g_swap_list;
std::atomic<uintptr_t> g_swap_map{ 0 };      // the replay's flow map (compared, never used as an object unless it is alive)
TD* g_swap_map_type = nullptr;
thread_local bool tl_swap = false;
thread_local int32_t tl_fighter = 0;
std::atomic<bool> g_swap_logged{ false };

void init() {
    if (g_init) return;
    g_init = true;
    auto* tdb = api().tdb();
    t_param = tdb->find_type("app.UIFlowCharaBgmSetting.UIParam");
    TD* flow = tdb->find_type("app.UIFlowCharaBgmSetting");
    m_start = flow ? method_of(flow, "Start", 2) : nullptr;
    TD* fm = tdb->find_type("app.UIFlowManager");
    m_active = fm ? method_of(fm, "Active", 1) : nullptr;
    t_state = tdb->find_type("app.UIFlowCharaBgmSetting.Flow_CharaBgmSelect");
    t_dialog = tdb->find_type("app.UIFlowCharaBgmSelect.UIParam");
    m_enter = t_state ? method_of(t_state, "OnEnter", 0) : nullptr;
    m_init_list = t_dialog ? method_of(t_dialog, "InitBgmList", 0) : nullptr;
    TD* t_list = tdb->find_type("System.Collections.Generic.List`1<System.UInt32>");
    m_clear = t_list ? method_of(t_list, "Clear", 0) : nullptr;
    m_add = t_list ? method_of(t_list, "Add", 1) : nullptr;
    m_count = t_list ? method_of(t_list, "get_Count", 0) : nullptr;
    TD* save = tdb->find_type("app.SystemSaveManager");
    m_saved = save ? method_of(save, "GetFighterSettingData", 1) : nullptr;
    TD* inv = tdb->find_type("app.InventoryManager");
    m_inv_bgm = inv ? method_of(inv, "get_Bgm", 0) : nullptr;
    TD* inv_bgm = tdb->find_type("app.InventoryManager.BgmInventory");
    m_inv_list = inv_bgm ? method_of(inv_bgm, "GetInventoryBgmUIDataList_CharacterBgmSetting", 0) : nullptr;
    if (!t_param || !m_start || !m_active) logf("replay music: the game's music flow not found (game update?): no T on the Replay Music row");
    if (!t_param || !t_state || !t_dialog || !m_enter || !m_active || !m_init_list || !m_clear || !m_add)
        logf("replay music: the game's playlist dialog not found (game update?): no T on Custom");
    if (!m_saved || !m_clear || !m_add) logf("replay music: the save's lists not found (game update?): Custom plays as Random");
    if (!m_inv_bgm || !m_inv_list) logf("replay music: the owned tracks not found (game update?): the Custom list is not filtered");
}

// The list of a flow map's description says this is no network match, and the map is the one armed
bool replay_map_ok(Obj* map) {
    if (!alive(map) || map->get_type_definition() != g_swap_map_type) return false;
    Obj* desc = get_obj(map, "m_desc");
    bool net = true;
    return alive(desc) && get_bool(desc, "IsNetwork", net) && !net;
}

constexpr int kCall = REFRAMEWORK_HOOK_CALL_ORIGINAL;

// UIParam.InitMenu of the dialog started here: its marks replaced by ours
int pre_init_menu(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_menu = (argc >= 2 && g_custom_open.load() && !g_closing.load(std::memory_order_relaxed)) ? reinterpret_cast<Obj*>(argv[1]) : nullptr;
    return kCall;
}
void post_init_menu(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    Obj* param = tl_menu;
    tl_menu = nullptr;
    if (!param) return;
    try {
        Obj* list = get_obj(param, "mSettingBgmIdList");
        if (!list) { logf("replay music: the dialog has no list to fill"); return; }
        std::vector<uint32_t> ids;
        { std::lock_guard lk(g_mx); ids = g_open_list; }
        call(m_clear, list);
        for (uint32_t id : ids) call(m_add, list, { arg_i32(static_cast<int32_t>(id)) });
        const Ret r = call(m_init_list, param);
        g_filled.store(true);
        logf("replay music: the dialog opens on %zu track%s of ours%s", ids.size(), ids.size() == 1 ? "" : "s", r.exception_thrown ? " (its list not redrawn)" : "");
    } catch (...) { logf("replay music: EXCEPTION filling the dialog"); }
}

// BattleFlowMap.loadBgmPrefab(type, random): the scope of the swap
int pre_load_bgm(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_swap = false;
    const uintptr_t armed = g_swap_map.load();
    if (!armed || argc < 3 || reinterpret_cast<uintptr_t>(argv[1]) != armed || g_closing.load(std::memory_order_relaxed)) return kCall;
    if (static_cast<int32_t>(reinterpret_cast<uintptr_t>(argv[2])) != kLoadPlayer1) return kCall;
    try { tl_swap = replay_map_ok(reinterpret_cast<Obj*>(argv[1])); } catch (...) { tl_swap = false; }
    return kCall;
}
void post_load_bgm(void**, REFrameworkTypeDefinitionHandle, unsigned long long) { tl_swap = false; }

// SystemSaveManager.GetCharaBgmIdSettingList(fighter): inside that scope only, the new list it made gets ours
int pre_get_list(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (tl_swap && argc >= 2) tl_fighter = static_cast<int32_t>(reinterpret_cast<uintptr_t>(argv[1]));
    return kCall;
}
void post_get_list(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_swap || !ret_val || !*ret_val || !g_swap_map.load()) return;
    try {
        Obj* list = reinterpret_cast<Obj*>(*ret_val);
        Obj* fsd = call_obj(m_saved, nullptr, { arg_i32(tl_fighter) });
        Obj* saved = fsd ? get_obj(fsd, "CharaBgmIdSettingList") : nullptr;
        if (!replay::bgm_swap_allowed(list, saved)) {
            logf("replay music: the game's getter gave the save's own list (game update?): left alone, this replay plays that fighter's music");
            return;
        }
        std::vector<uint32_t> ids;
        { std::lock_guard lk(g_mx); ids = g_swap_list; }
        call(m_clear, list);
        for (uint32_t id : ids) call(m_add, list, { arg_i32(static_cast<int32_t>(id)) });
        if (!g_swap_logged.exchange(true))
            logf("replay music: Custom, %zu track%s in the copy made for fighter %d; its list in the save untouched (%d)", ids.size(), ids.size() == 1 ? "" : "s",
                 tl_fighter, saved && m_count ? static_cast<int>(call(m_count, saved).dword) : 0);
    } catch (...) { logf("replay music: EXCEPTION at the list's swap"); }
}

void start_check() {
    init();
    TD* map = api().tdb()->find_type("app.battle.BattleFlowMap");
    TD* save = api().tdb()->find_type("app.SystemSaveManager");
    auto add = [&](TD* t, const char* sig, int n, const char* label) { g_check.add(t ? method_of(t, sig, n) : nullptr, label); };
    add(t_dialog, "InitMenu", 0, "app.UIFlowCharaBgmSelect.UIParam.InitMenu()");
    add(map, "loadBgmPrefab(System.Int32, System.Boolean)", 2, "app.battle.BattleFlowMap.loadBgmPrefab(Int32,Boolean)");
    add(save, "GetCharaBgmIdSettingList(System.Int32)", 1, "app.SystemSaveManager.GetCharaBgmIdSettingList(Int32)");
    g_check.start(kCodecheckCache, "replay music");
    g_check_started = true;
}

void install_hooks() {
    g_hooks_done = true;
    g_check.log_summary();
    if (g_check.failed()) return;
    struct H { Hook id; REFPreHookFn pre; REFPostHookFn post; };
    const H hooks[] = {
        { kInitMenu, pre_init_menu, post_init_menu },
        { kLoadBgm, pre_load_bgm, post_load_bgm },
        { kGetList, pre_get_list, post_get_list },
    };
    for (const H& h : hooks) {
        const auto& e = g_check.entry(h.id);
        Method* m = g_check.method(h.id);
        if (!m) { logf("hook %s: not found (game update?)", e.label.c_str()); continue; }
        if (!g_check.unique(h.id)) {
            logf("hook %s: REFUSED (R18): %d other method(s) enter its code, e.g. %s", e.label.c_str(), e.shared, g_check.other_label(h.id).c_str());
            continue;
        }
        g_hooked[h.id].store(m->add_hook(h.pre, h.post, false) != 0);
        logf("hook %s: %s", e.label.c_str(), g_hooked[h.id].load() ? "hooked" : "not hooked");
    }
    if (!g_hooked[kInitMenu].load()) logf("replay music: no T on Custom (its list could not be put on the dialog)");
    if (!can_swap()) logf("replay music: Custom plays as Random (its list could not be swapped in)");
}

// The param of an active handle of the UIFlowManager, of that type (for this frame only)
Obj* running(TD* param_type) {
    auto* fm = api().get_managed_singleton("app.UIFlowManager");
    Obj* hs = fm ? get_obj(fm, "_Handles") : nullptr;
    for (int i = 0, n = list_count(hs); i < n; ++i) {
        Obj* h = list_item(hs, i);
        Obj* p = h ? get_obj(h, "<Param>k__BackingField") : nullptr;
        if (!p || p->get_type_definition() != param_type) continue;
        auto r = call(m_active, nullptr, { arg_obj(h) });
        if (!r.exception_thrown && (r.dword & 0xFF)) return p;
    }
    return nullptr;
}

std::string ids_text(const std::vector<uint32_t>& ids) {
    std::string s;
    for (uint32_t id : ids) s += (s.empty() ? "" : ", ") + std::to_string(id);
    return s.empty() ? "none" : s;
}
// What the dialog has marked now
void read_marked(Obj* param) {
    Obj* list = get_obj(param, "mSettingBgmIdList");
    if (!list) return;
    std::vector<uint32_t> now;
    for (int i = 0, n = list_count(list); i < n; ++i) {
        auto r = call(list, "get_Item", { arg_i32(i) });   // List<UInt32>: the value itself
        if (r.exception_thrown) return;                    // read again next frame
        now.push_back(r.dword);
    }
    if (g_marked_read && now == g_marked) return;
    g_marked_read = true;
    g_marked = std::move(now);
    logf("replay music: the dialog has %zu marked: %s", g_marked.size(), ids_text(g_marked).c_str());
}
// The dialog's heading: ours, never squeezed (the game's fits a fighter's name)
void put_title(Obj* param) {
    Obj* t = get_obj(param, "mTextCharaName");
    const SF6CoreAPI* a = ui::core();
    if (!t || !a || !ui::client()) return;
    if (!g_title_fit) {
        g_title_fit = true;
        SF6CoreArg off{}; off.type = SF6CORE_ARG_I64; off.v.i = 0;
        a->obj_invoke(ui::client(), t, "set_AutoRegionFit(System.Boolean)", &off, 1, nullptr);
        float size[2] = {};
        if (auto r = call(t, "get_RegionSize"); !r.exception_thrown) {
            std::memcpy(size, r.bytes.data(), sizeof size);
            if (size[0] < kTitleRegionW) a->ui_set_region_size(ui::client(), t, kTitleRegionW, size[1]);
        }
    }
    a->ui_set_text(ui::client(), t, g_title.c_str());
}

}  // namespace

void frame() {
    if (g_hooks_done) return;
    try {
        if (!g_check_started) start_check();
        if (g_check.done()) install_hooks();
    } catch (...) { g_hooks_done = true; logf("replay music: EXCEPTION at the hooks' check: no hook"); }
}

std::string guide_text(int input_id) {
    static Method* m = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        TD* t = api().tdb()->find_type("app.InputGuideManager");
        m = t ? method_of(t, "GetGuideText(app.InputAssign.Digital.Id)", 1) : nullptr;
    }
    return m ? str(call_obj(m, nullptr, { arg_i32(input_id) })) : std::string();
}

bool can_swap() { return g_hooked[kLoadBgm].load() && g_hooked[kGetList].load() && m_saved && m_clear && m_add; }

bool open_character_music() {
    init();
    if (!m_start || !t_param || !m_active) return false;
    g_custom_open.store(false);
    Obj* handle = call_obj(m_start, nullptr, { arg_i32(kSceneBattleSetting), arg_i32(kPlayer) });
    logf("replay music: Character Music %s", handle ? "started" : "not started (no handle)");
    g_watch = t_param;
    g_gone = 0;
    g_result_ready = false;
    return handle != nullptr;
}

bool open_custom_list(const std::vector<uint32_t>& list, const std::string& title) {
    init();
    if (!t_param || !t_state || !t_dialog || !m_enter || !m_active || !m_init_list || !m_clear || !m_add) return false;
    if (!g_hooked[kInitMenu].load()) { logf("replay music: the dialog not started: %s", g_hooks_done ? "its hook is not in" : "the hooks' check is still running"); return false; }
    Obj* state = t_state->create_instance(0);
    if (!state) state = t_state->create_instance(1);
    Obj* param = t_param->create_instance(0);
    if (!param) param = t_param->create_instance(1);
    if (!state || !param) { logf("replay music: the dialog's start objects not made (state %p, param %p)", (void*)state, (void*)param); return false; }
    state->add_ref();
    param->add_ref();
    { std::lock_guard lk(g_mx); g_open_list = list; }
    g_title = title;
    g_title_fit = false;
    g_filled.store(false);
    g_custom_open.store(true);
    bool ok = set_i32(param, "PlayerIndex", kPlayer) && set_u32(param, "<FocusCharaId>k__BackingField", kDialogFighter) &&
              set_obj(state, "_Param", param);     // for this call only: put back below, so no count to keep right
    Obj* handle = nullptr;
    if (ok) {
        const Ret r = call(m_enter, state);
        ok = !r.exception_thrown;
        handle = get_obj(state, "_Handle");
        set_obj(state, "_Param", nullptr);
    }
    logf("replay music: the playlist dialog %s (%zu of ours, %s)", handle ? "started" : "not started", list.size(), ok ? "called" : "the call failed");
    state->release();
    param->release();
    if (!handle) { g_custom_open.store(false); return false; }
    g_watch = t_dialog;
    g_gone = 0;
    g_marked = list;
    g_marked_read = false;
    g_result_ready = false;
    return true;
}

bool up() {
    if (!g_watch || !m_active) return false;
    if (Obj* p = running(g_watch)) {
        g_gone = 0;
        if (g_watch == t_dialog && g_filled.load()) { put_title(p); read_marked(p); }
        return true;
    }
    if (++g_gone < kGoneFrames) return true;
    if (g_watch == t_dialog) {
        logf("replay music: the dialog closed, %zu marked: %s", g_marked.size(), ids_text(g_marked).c_str());
        g_custom_open.store(false);
        g_result_ready = g_marked_read;    // never filled as far as we saw: nothing to take, the list stays
    } else logf("replay music: the screen closed");
    g_watch = nullptr;
    return false;
}

bool take_custom_list(std::vector<uint32_t>& out) {
    if (!g_result_ready) return false;
    g_result_ready = false;
    out = g_marked;
    return true;
}

void forget() {
    g_watch = nullptr;
    g_custom_open.store(false);
    g_result_ready = false;
}

std::vector<uint32_t> owned_tracks() {
    init();
    std::vector<uint32_t> out;
    if (!m_inv_bgm || !m_inv_list) return out;
    static std::mutex mx;
    std::lock_guard lk(mx);
    auto* mgr = api().get_managed_singleton("app.InventoryManager");
    Obj* inv = call_obj(m_inv_bgm, reinterpret_cast<Obj*>(mgr));
    Obj* list = inv ? call_obj(m_inv_list, inv) : nullptr;
    if (!alive(list)) return out;
    static TD* seen = nullptr;
    static Method* cnt = nullptr;
    static Method* item = nullptr;
    if (list->get_type_definition() != seen) {
        seen = list->get_type_definition();
        cnt = method_of(seen, "get_Count", 0);
        item = method_of(seen, "get_Item", 1);
    }
    if (!cnt || !item) return out;
    const Ret rn = call(cnt, list);
    const int n = rn.exception_thrown ? 0 : static_cast<int>(rn.dword);
    for (int i = 0; i < n; ++i) {
        Obj* rec = call_obj(item, list, { arg_i32(i) });
        uint32_t id = 0;
        if (rec && get_u32(rec, "ManageId", id) && id) out.push_back(id);
    }
    return out;
}

void arm_replay(Obj* map, std::vector<uint32_t> list) {
    init();
    { std::lock_guard lk(g_mx); g_swap_list = std::move(list); }
    g_swap_map_type = map ? map->get_type_definition() : nullptr;
    g_swap_logged.store(false);
    g_swap_map.store(reinterpret_cast<uintptr_t>(map));
}

}  // namespace slots::replaymusic
