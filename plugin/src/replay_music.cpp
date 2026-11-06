// SF6 Slots — Replays > Settings: the game's music screens, started from the Battle BGM row.
//
// Character Music is the game's own flow, app.UIFlowCharaBgmSetting. Its static Start is what Battle Settings and the Options
// menu call; unlike Opponent Outfit Settings' page (replay_tab.cpp's top comment), its screen comes from a prefab by key
// (UIParam.get_ObjectPrefabKey, "CharaBgmSetting"), not from an object of Battle Settings' scene. Started with Battle Settings'
// scene type (it names the route in the game's own log). On Back the flow saves what changed itself, as anywhere else.
//
// While it runs, the game's UIFlowManager holds its handle: looked for by the param's type, no reference kept (R35).
#include "slots.hpp"
#include "replay_music.hpp"

namespace slots::replaymusic {
namespace {

constexpr int kSceneBattleSetting = 2;      // app.UIFlowCharaBgmSetting.eOpenSceneType.BattleSetting
constexpr int kPlayer = 0;                  // the menus' player
constexpr int kGoneFrames = 10;             // no handle for that long: over (a handle may be added a frame late)

bool g_init = false;
TD* t_param = nullptr;
Method* m_start = nullptr;
Method* m_active = nullptr;
int g_gone = 0;

void init() {
    if (g_init) return;
    g_init = true;
    auto* tdb = api().tdb();
    t_param = tdb->find_type("app.UIFlowCharaBgmSetting.UIParam");
    TD* flow = tdb->find_type("app.UIFlowCharaBgmSetting");
    m_start = flow ? method_of(flow, "Start", 2) : nullptr;
    TD* fm = tdb->find_type("app.UIFlowManager");
    m_active = fm ? method_of(fm, "Active", 1) : nullptr;
    if (!t_param || !m_start || !m_active) logf("replay music: the game's music flow not found (game update?): no T on the Battle BGM row");
}

// The UIFlowManager has an active handle whose param is of that type
bool running(TD* param_type) {
    auto* fm = api().get_managed_singleton("app.UIFlowManager");
    Obj* hs = fm ? get_obj(fm, "_Handles") : nullptr;
    for (int i = 0, n = list_count(hs); i < n; ++i) {
        Obj* h = list_item(hs, i);
        Obj* p = h ? get_obj(h, "<Param>k__BackingField") : nullptr;
        if (!p || p->get_type_definition() != param_type) continue;
        auto r = call(m_active, nullptr, { arg_obj(h) });
        if (!r.exception_thrown && (r.dword & 0xFF)) return true;
    }
    return false;
}

}  // namespace

bool open_character_music() {
    init();
    if (!m_start || !t_param || !m_active) return false;
    Obj* handle = call_obj(m_start, nullptr, { arg_i32(kSceneBattleSetting), arg_i32(kPlayer) });
    logf("replay music: Character Music %s", handle ? "started" : "not started (no handle)");
    g_gone = 0;
    return handle != nullptr;
}

bool up() {
    if (!t_param || !m_active) return false;
    if (running(t_param)) { g_gone = 0; return true; }
    if (++g_gone < kGoneFrames) return true;
    logf("replay music: the screen closed");
    return false;
}

}  // namespace slots::replaymusic
