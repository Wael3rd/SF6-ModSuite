// SF6 Slots — Replays > Settings: the game's music screens, started from the Battle BGM row.
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
//
// While one runs, the game's UIFlowManager holds its handle: looked for by the param's type, no reference kept (R35).
#include "slots.hpp"
#include "replay_music.hpp"

#include <string>
#include <vector>

namespace slots::replaymusic {
namespace {

constexpr int kSceneBattleSetting = 2;      // app.UIFlowCharaBgmSetting.eOpenSceneType.BattleSetting
constexpr int kPlayer = 0;                  // the menus' player
constexpr int kGoneFrames = 10;             // no handle for that long: over (a handle may be added a frame late)
constexpr uint32_t kDialogFighter = 1;      // the fighter the dialog reads its first marks and its name from: Ryu, for now

bool g_init = false;
TD* t_param = nullptr;                       // the Character Music screen's param
TD* t_state = nullptr;                       // its state that starts the dialog
TD* t_dialog = nullptr;                      // the dialog's param
Method* m_start = nullptr;
Method* m_enter = nullptr;
Method* m_active = nullptr;
TD* g_watch = nullptr;                       // the param's type of the screen started
int g_gone = 0;
std::vector<uint32_t> g_marked;              // the dialog's marked tracks, as last read
bool g_marked_read = false;

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
    if (!t_param || !m_start || !m_active) logf("replay music: the game's music flow not found (game update?): no T on the Battle BGM row");
    if (!t_param || !t_state || !t_dialog || !m_enter || !m_active) logf("replay music: the game's playlist dialog not found (game update?): no T on Custom");
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
// What the dialog has marked now, read when their number changed
void read_marked(Obj* param) {
    Obj* list = get_obj(param, "mSettingBgmIdList");
    if (!list) return;
    const int n = list_count(list);
    if (g_marked_read && n == static_cast<int>(g_marked.size())) return;
    g_marked_read = true;
    g_marked.clear();
    for (int i = 0; i < n; ++i) {
        auto r = call(list, "get_Item", { arg_i32(i) });   // List<UInt32>: the value itself
        if (!r.exception_thrown) g_marked.push_back(r.dword);
    }
    logf("replay music: the dialog has %d marked: %s", n, ids_text(g_marked).c_str());
}

}  // namespace

bool open_character_music() {
    init();
    if (!m_start || !t_param || !m_active) return false;
    Obj* handle = call_obj(m_start, nullptr, { arg_i32(kSceneBattleSetting), arg_i32(kPlayer) });
    logf("replay music: Character Music %s", handle ? "started" : "not started (no handle)");
    g_watch = t_param;
    g_gone = 0;
    return handle != nullptr;
}

bool open_custom_list() {
    init();
    if (!t_param || !t_state || !t_dialog || !m_enter || !m_active) return false;
    Obj* state = t_state->create_instance(0);
    if (!state) state = t_state->create_instance(1);
    Obj* param = t_param->create_instance(0);
    if (!param) param = t_param->create_instance(1);
    if (!state || !param) { logf("replay music: the dialog's start objects not made (state %p, param %p)", (void*)state, (void*)param); return false; }
    state->add_ref();
    param->add_ref();
    bool ok = set_i32(param, "PlayerIndex", kPlayer) && set_u32(param, "<FocusCharaId>k__BackingField", kDialogFighter) &&
              set_obj(state, "_Param", param);     // for this call only: put back below, so no count to keep right
    Obj* handle = nullptr;
    if (ok) {
        const Ret r = call(m_enter, state);
        ok = !r.exception_thrown;
        handle = get_obj(state, "_Handle");
        set_obj(state, "_Param", nullptr);
    }
    logf("replay music: the playlist dialog %s (fighter %u, %s)", handle ? "started" : "not started", kDialogFighter, ok ? "called" : "the call failed");
    state->release();
    param->release();
    g_watch = t_dialog;
    g_gone = 0;
    g_marked.clear();
    g_marked_read = false;
    return handle != nullptr;
}

bool up() {
    if (!g_watch || !m_active) return false;
    if (Obj* p = running(g_watch)) {
        g_gone = 0;
        if (g_watch == t_dialog) read_marked(p);
        return true;
    }
    if (++g_gone < kGoneFrames) return true;
    if (g_watch == t_dialog) logf("replay music: the dialog closed, %zu marked: %s", g_marked.size(), ids_text(g_marked).c_str());
    else logf("replay music: the screen closed");
    g_watch = nullptr;
    return false;
}

}  // namespace slots::replaymusic
