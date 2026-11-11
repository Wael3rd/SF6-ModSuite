// Zen Mode's tab in Battle Settings (agent MatchingSetting): part of profile_mod.cpp, after zen_engine.inl.
//
// Nothing is created: the screen's tab bar already holds six items and the game shows four (c_item_04 / c_item_05 built, hidden);
// c_item_04 is shown with our name, made selectable (the mouse) and the bar's right key glyph moved one tab further (the step measured between c_item_00 and
// c_item_01). The game has no page for a fifth tab: the Matchmaking page is borrowed (the agent's _RootItem focused on its first
// child), its six selector rows carrying our titles and values while our tab is the selected one, and given back as they were
// (title message ids, title visibility, value texts, the rank icon, the Master phase box) when another tab is selected or the
// screen goes. As MatchingSettingTab.lua (measured in game 05/09), natively:
//   - the tab keys reach app.UIPartsGuiLinearList.Input(32 next / 16 previous) of the agent's _TabItem: from the game's last tab
//     forward, from its first backward and from ours both ways, the call is skipped and the move made in LateUpdateBehavior;
//   - app.UIPartsListBase.InvokeSelectionChanged of that list is skipped while our index is selected (the game would look for a
//     fifth page);
//   - app.UIPartsSpin's steps on the six rows are ours while our tab shows (the game's matchmaking settings never move): a step
//     changes Zen Mode's settings at once (zen_use, the file written);
//   - the bottom description (InputGuide > p_ScrollText_0 > e_text): the game writes the focused row's line there on every
//     frame, as a plain text (measured 11/11: a text poked in is gone the next frame, its message id stays empty). Ours is
//     written over it once the game's late update is over (after LateUpdateBehavior, the main thread), every frame our tab
//     shows; nothing to put back, the game's next frame does. Never from a hook of app.UIPartsInputGuideList.OnLateUpdate:
//     the game runs it on its worker threads, several at once (crash 11/11).
// Gate: one registry read every 15 frames while Battle Settings is not up; the hooks are one atomic read and a comparison
// (they run on the game's threads: atomics only, nothing of the screen's state).

constexpr const char* kZtAgent = "MatchingSetting";
constexpr const char* kZtStrip = "p_SimpleList_0_tab";
constexpr const char* kZtItem = "c_item_04";
constexpr const char* kZtPage = "p_MatchingSettingMatching_0_h";
constexpr const char* kZtGuideKey = "p_InputGuide_R_s_dUITabR";
constexpr int kZtIndex = 4, kZtRows = 6;
constexpr uint32_t kZtPoll = 15;
constexpr uint32_t kZtNext = 32, kZtPrev = 16;       // UIPartsGuiLinearList.Input's button
constexpr uint32_t kZtActs = 2 | 8;                  // app.InputDigitalFlag Trigger | Repeat (as the Color Mod's rows)
constexpr int kZtCall = REFRAMEWORK_HOOK_CALL_ORIGINAL, kZtSkip = REFRAMEWORK_HOOK_SKIP_ORIGINAL;

struct ZtSaved { bool has = false; uint8_t mid[16] = {}; bool tvis = false; std::string vtxt; };
struct ZtScreen {
    Obj* agent = nullptr;
    Obj* tab = nullptr;
    Obj* page_part = nullptr;        // the borrowed page's parts (mGroup, mArrSpin)
    int num[kZtRows] = {};           // p_Spin_<num> of each row, top to bottom
    uintptr_t ctrl[kZtRows] = {};    // each row's control (the focused row is found by it)
    bool ok = false, ours = false, content = false;
    int settle = 0;                  // frames left in which a text not measured yet is written again
    ZtSaved saved[kZtRows];
    bool guide_x_has = false, decor_has = false, rank_vis = false, reward_vis = false;
    float guide_x = 0;
    const zen::Words* words = nullptr;
};
ZtScreen zt;
bool zt_up = false, zt_hooks = false, zt_refused = false;
std::atomic<uintptr_t> zt_tab{ 0 };
std::atomic<uintptr_t> zt_spin[kZtRows];
std::atomic<bool> zt_content{ false };
std::atomic<int> zt_pending{ -1 }, zt_step{ -1 };

Obj* zt_find(Obj* root, const char* name, int depth) { return root ? reinterpret_cast<Obj*>(g_core->ui_find_child(root, name, depth)) : nullptr; }
void zt_vis(Obj* c, bool on) {
    bool ok = false;
    if (c && (call_bool(c, "get_Visible", &ok) != on || !ok)) g_core->ui_set_visible(g_client, c, on ? 1 : 0);
}
// via.gui.SelectItem.CanSelect of our tab item: false as the game leaves it (a mouse click does nothing), true while the
// screen is up (measured 11/11: the engine's list then selects it on a click, as its four own tabs)
void zt_selectable(Obj* item, bool on) {
    bool ok = false;
    if (item && call_bool(item, "get_CanSelect", &ok) != on && ok) call(item, "set_CanSelect", { arg_i32(on ? 1 : 0) });
}
// A text written only when it differs. One never measured (written while hidden: display size 0) draws nothing: for a few
// frames after a write it is written twice, differently. The main thread only (get_DisplaySize faulted on a worker thread).
void zt_text(Obj* t, const std::string& s) {
    if (!t) return;
    if (message(t) != s) { g_core->ui_set_text(g_client, t, s.c_str()); zt.settle = 10; }
    if (zt.settle > 0 && !s.empty() && drawn_width(t) <= 0.0f) {
        g_core->ui_set_text(g_client, t, (s + " ").c_str());
        g_core->ui_set_text(g_client, t, s.c_str());
    }
}
int zt_cur(Obj* tab) {
    auto r = call(tab, "get_SelectedIndex");
    return r.exception_thrown ? -1 : static_cast<int>(r.dword);
}
uint32_t zt_u32(void* a) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFFu); }

// ---- the hooks (the game's own input update) ----
int zt_pre_input(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const uintptr_t tab = zt_tab.load(std::memory_order_relaxed);
    if (!tab || reinterpret_cast<uintptr_t>(argv[1]) != tab) return kZtCall;
    try {
        const uint32_t btn = zt_u32(argv[2]);
        if (btn != kZtNext && btn != kZtPrev) return kZtCall;
        const bool fwd = btn == kZtNext;
        const int c = zt_cur(reinterpret_cast<Obj*>(tab));
        int target = -1;
        if (c == kZtIndex) target = fwd ? 0 : kZtIndex - 1;
        else if (c == kZtIndex - 1 && fwd) target = kZtIndex;
        else if (c == 0 && !fwd) target = kZtIndex;
        if (target < 0) return kZtCall;
        zt_pending.store(target, std::memory_order_relaxed);
        return kZtSkip;
    } catch (...) { return kZtCall; }
}
int zt_pre_changed(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const uintptr_t tab = zt_tab.load(std::memory_order_relaxed);
    if (!tab || reinterpret_cast<uintptr_t>(argv[1]) != tab) return kZtCall;
    try { return zt_cur(reinterpret_cast<Obj*>(tab)) == kZtIndex ? kZtSkip : kZtCall; } catch (...) { return kZtCall; }
}
int zt_row_of(void* spin) {
    if (!zt_content.load(std::memory_order_relaxed)) return -1;
    for (int i = 0; i < kZtRows; ++i)
        if (zt_spin[i].load(std::memory_order_relaxed) == reinterpret_cast<uintptr_t>(spin)) return i;
    return -1;
}
// Add / Sub / AddNum / SubNum / MouseAdd / MouseSub
int zt_pre_spin(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int r = zt_row_of(argv[1]);
    if (r < 0) return kZtCall;
    zt_step.store(r, std::memory_order_relaxed);
    return kZtSkip;
}
// InputLeft / InputRight(flag): a held or released key does nothing
int zt_pre_spin_lr(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int r = zt_row_of(argv[1]);
    if (r < 0) return kZtCall;
    if (zt_u32(argv[2]) & kZtActs) zt_step.store(r, std::memory_order_relaxed);
    return kZtSkip;
}
// InputDirection(direction, flag): 4 left, 8 right; up and down stay the game's (the focus)
int zt_pre_spin_dir(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int r = zt_row_of(argv[1]);
    if (r < 0 || !(zt_u32(argv[2]) & 12)) return kZtCall;
    if (zt_u32(argv[3]) & kZtActs) zt_step.store(r, std::memory_order_relaxed);
    return kZtSkip;
}
void zt_post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- the bottom description ----
Obj* zt_desc_text() {
    void* buf[4] = {};
    const uint32_t n = g_core->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* cm = call_obj(reinterpret_cast<Obj*>(buf[i]), "get_ControlMain");
        if (!cm || !actual_visible(cm)) continue;
        if (Obj* t = zt_find(zt_find(cm, "p_ScrollText_0", 6), "e_text", 6)) return t;
    }
    return nullptr;
}
// The focused row: the group's focused child, known by its control (the children are not the spins, nor in their order)
int zt_focus_row() {
    Obj* g = get_obj(zt.page_part, "mGroup");
    if (!g) return -1;
    auto r = call(g, "GetFocusIndex");
    Obj* kids = r.exception_thrown ? nullptr : get_obj(g, "_Children");
    Obj* child = kids ? list_item(kids, static_cast<int>(r.dword)) : nullptr;
    const uintptr_t c = child ? reinterpret_cast<uintptr_t>(get_obj(child, "<Control>k__BackingField")) : 0;
    for (int i = 0; c && i < kZtRows; ++i)
        if (zt.ctrl[i] == c) return i;
    return -1;
}
// After the game's late update, our tab showing: the focused row's line over the one the game just wrote
void zt_desc_paint() {
    const int row = zt_focus_row();
    if (row < 0 || !zt.words) return;
    const char* want = row == 0 ? zt.words->help_master : zt.words->help_group[row - 1];
    Obj* t = zt_desc_text();
    if (!t) return;
    if (message(t) != want) g_core->ui_set_text(g_client, t, want);
}

// ---- the screen ----
bool zt_enter(Obj* agent) {
    zt = ZtScreen{};
    zt.agent = agent;
    zt.tab = get_obj(agent, "_TabItem");
    zt.words = &zen::words_of(game_language());
    struct Sp { uintptr_t spin, ctrl; int num; float y; };
    std::vector<Sp> sp;
    Obj* root = get_obj(agent, "_RootItem");
    Obj* kids = root ? get_obj(root, "_Children") : nullptr;
    for (int i = 0, n = kids ? list_count(kids) : 0; i < n && !zt.page_part; ++i) {
        Obj* page = list_item(kids, i);
        Obj* arr = page && field_of(page->get_type_definition(), "mArrSpin") ? get_obj(page, "mArrSpin") : nullptr;
        if (!arr) continue;
        zt.page_part = page;
        for (int k = 0, m = list_count(arr); k < m && k < 16; ++k) {
            Obj* spin = list_item(arr, k);
            Obj* c = spin ? get_obj(spin, "<Control>k__BackingField") : nullptr;
            const std::string name = c ? str(call_obj(c, "get_Name")) : std::string();
            float p[3] = {};
            if (name.rfind("p_Spin_", 0) != 0 || !floats_of(c, "get_Position", p, 3)) continue;
            sp.push_back({ reinterpret_cast<uintptr_t>(spin), reinterpret_cast<uintptr_t>(c), atoi(name.c_str() + 7), p[1] });
        }
    }
    std::sort(sp.begin(), sp.end(), [](const Sp& a, const Sp& b) { return a.y < b.y; });   // not laid out in their numbers' order
    zt.ok = zt.tab && sp.size() == static_cast<size_t>(kZtRows);
    if (!zt.ok) {
        if (!zt_refused) logi("profile_mod: Zen Mode's tab not shown (Battle Settings is not built as known: %zu rows)", sp.size());
        zt_refused = true;
        return false;
    }
    for (int i = 0; i < kZtRows; ++i) {
        const Sp& r = sp[static_cast<size_t>(i)];
        zt.num[i] = r.num;
        zt.ctrl[i] = r.ctrl;
        zt_spin[i].store(r.spin);
    }
    zt_pending.store(-1);
    zt_step.store(-1);
    zt_tab.store(reinterpret_cast<uintptr_t>(zt.tab));
    return true;
}
void zt_rows_back(Obj* cm) {
    Obj* page = zt_find(cm, kZtPage, 6);
    Obj* grp = zt_find(page, "p_Group_0_v", 6);
    for (int i = 0; i < kZtRows; ++i) {
        ZtSaved& sv = zt.saved[i];
        if (!sv.has) continue;
        char name[24];
        snprintf(name, sizeof name, "p_Spin_%d", zt.num[i]);
        Obj* row = zt_find(grp, name, 1);
        Obj* title = zt_find(row, "e_text_title", 5);
        Obj* val = zt_find(row, "e_text", 5);
        if (title) { g_core->ui_set_message_id(g_client, title, sv.mid); zt_vis(title, sv.tvis); }
        if (val && message(val) != sv.vtxt) g_core->ui_set_text(g_client, val, sv.vtxt.c_str());
        sv = ZtSaved{};
    }
    if (zt.decor_has) {
        zt_vis(zt_find(cm, "c_RankIcon", 6), zt.rank_vis);
        zt_vis(zt_find(page, "p_MasterRatePassRewardPreview_0", 6), zt.reward_vis);
        zt.decor_has = false;
    }
}
void zt_rows_ours(Obj* cm) {
    Obj* page = zt_find(cm, kZtPage, 6);
    Obj* grp = zt_find(page, "p_Group_0_v", 6);
    if (!grp) return;
    Obj* rank = zt_find(cm, "c_RankIcon", 6);
    Obj* reward = zt_find(page, "p_MasterRatePassRewardPreview_0", 6);
    if (!zt.decor_has) {
        zt.rank_vis = rank && call_bool(rank, "get_Visible");
        zt.reward_vis = reward && call_bool(reward, "get_Visible");
        zt.decor_has = true;
    }
    zt_vis(rank, false);                                 // the ranked search's icon and Master phase box: nothing of ours
    zt_vis(reward, false);
    zt_vis(zt_find(cm, "c_profile", 6), true);           // hidden by the game when coming from Fighter Profile
    const zen::State& s = zen_settings();
    const zen::Words& w = *zt.words;
    for (int i = 0; i < kZtRows; ++i) {
        char name[24];
        snprintf(name, sizeof name, "p_Spin_%d", zt.num[i]);
        Obj* row = zt_find(grp, name, 1);
        Obj* title = zt_find(row, "e_text_title", 5);
        Obj* val = zt_find(row, "e_text", 5);
        if (!title) continue;
        ZtSaved& sv = zt.saved[i];
        if (!sv.has) {
            g_core->ui_get_message_id(title, sv.mid);
            sv.tvis = call_bool(title, "get_Visible");
            sv.vtxt = val ? message(val) : std::string();
            sv.has = true;
            const uint8_t none[16] = {};
            g_core->ui_set_message_id(g_client, title, none);     // the title alone: the value is only written over
        }
        zt_vis(title, true);
        zt_text(title, i == 0 ? w.tab : w.group[i - 1]);
        const char* v = w.on;
        if (i == 0) v = s.enabled ? w.on : w.off;
        else switch (zen::group_value(s, i - 1)) {
            case zen::GroupValue::Hidden: v = w.hidden; break;
            case zen::GroupValue::Shown: v = w.shown; break;
            default: v = w.mixed; break;
        }
        zt_text(val, v);
    }
}
void zt_tab_paint(Obj* cm) {
    Obj* strip = zt_find(cm, kZtStrip, 6);
    Obj* it0 = zt_find(strip, "c_item_00", 2);
    Obj* it1 = zt_find(strip, "c_item_01", 2);
    Obj* it4 = zt_find(strip, kZtItem, 2);
    if (!it0 || !it1 || !it4) return;
    zt_vis(it4, true);
    zt_selectable(it4, true);
    for (const char* n : { "e_hit_mouse", "e_scale9grid_focus_line", "e_scale9grid_focus_line0", "e_text_menu" }) zt_vis(zt_find(it4, n, 4), true);
    zt_text(zt_find(it4, "e_text_menu", 4), zt.words->tab);
    float p0[3] = {}, p1[3] = {}, g[3] = {};
    Obj* key = zt_find(strip, kZtGuideKey, 2);
    if (!key || !floats_of(it0, "get_Position", p0, 3) || !floats_of(it1, "get_Position", p1, 3) || !floats_of(key, "get_Position", g, 3)) return;
    if (!zt.guide_x_has) { zt.guide_x = g[0]; zt.guide_x_has = true; }
    const float want = zt.guide_x + (p1[0] - p0[0]);
    if (std::fabs(g[0] - want) > 0.5f) g_core->ui_set_position(g_client, key, want, g[1], g[2]);
}
// The screen is no longer shown. alive: its agent still exists (what was changed is given back; else it went with its tree)
void zt_leave(bool alive) {
    zt_tab.store(0);
    zt_content.store(false);
    if (alive && zt.ok) {
        if (Obj* cm = call_obj(zt.agent, "get_ControlMain")) {
            zt_rows_back(cm);
            Obj* strip = zt_find(cm, kZtStrip, 6);
            zt_selectable(zt_find(strip, kZtItem, 2), false);
            zt_vis(zt_find(strip, kZtItem, 2), false);
            float g[3] = {};
            Obj* key = zt_find(strip, kZtGuideKey, 2);
            if (zt.guide_x_has && key && floats_of(key, "get_Position", g, 3)) g_core->ui_set_position(g_client, key, zt.guide_x, g[1], g[2]);
        }
    }
    zt = ZtScreen{};
    zt_up = false;
}
// Our tab stops being the one shown: the rows as the game had them (the description is the game's again on its next frame)
void zt_content_off(Obj* cm) {
    zt.content = false;
    zt_content.store(false);
    zt_rows_back(cm);
}
void zt_frame(Obj* cm) {
    if (zt.settle > 0) --zt.settle;
    zt_tab_paint(cm);
    Obj* root = get_obj(zt.agent, "_RootItem");
    const int p = zt_pending.exchange(-1);
    if (p >= 0) {
        if (p != kZtIndex && zt.content) zt_content_off(cm);   // before the game's own change: what it then writes stays
        zt.ours = p == kZtIndex;
        call(zt.tab, "SetSelectedIndex", { arg_i32(p), nullptr });
        // The four pages are the children of the agent's switch group: its focus index says which one shows
        if (zt.ours) { if (root) call(root, "ChangeFocusIndex", { arg_i32(0) }); }
        else call(zt.tab, "InvokeSelectionChanged");
        if (zg_debug) logi("profile_mod: Zen tab: move to %d", p);
    }
    const int c = zt_cur(zt.tab);
    if (c == kZtIndex && !zt.ours) {                     // selected another way (the mouse)
        zt.ours = true;
        if (root) call(root, "ChangeFocusIndex", { arg_i32(0) });
    }
    if (c != kZtIndex) zt.ours = false;
    const bool want = c == kZtIndex;
    if (!want && zt.content) zt_content_off(cm);
    if (!want) return;
    if (!zt.content) { zt.content = true; zt.settle = 10; }
    const int r = zt_step.exchange(-1);
    if (r >= 0) {
        zen::State s = zen_settings();
        if (r == 0) s.enabled = !s.enabled; else zen::step_group(s, r - 1);
        zen_use(s, true);
        if (zg_debug) logi("profile_mod: Zen tab: row %d stepped", r);
    }
    zt_rows_ours(cm);
    zt_content.store(true);
}
// LateUpdateBehavior
void zen_tab_late(uint32_t f) {
    if (!zt_hooks || (!zt_up && f % kZtPoll)) return;
    try {
        void* buf[2] = {};
        const uint32_t n = g_core->ui_agents(kZtAgent, buf, 2);
        Obj* agent = nullptr;
        Obj* cm = nullptr;
        bool alive = false;
        for (uint32_t i = 0; i < n && i < 2; ++i) {
            Obj* a = reinterpret_cast<Obj*>(buf[i]);
            if (a == zt.agent) alive = true;
            Obj* m = agent ? nullptr : call_obj(a, "get_ControlMain");
            if (m && actual_visible(m)) { agent = a; cm = m; }
        }
        if (zt_up && agent != zt.agent) zt_leave(alive);
        if (!agent) return;
        if (!zt_up) {
            if (zt_refused) return;
            zt_up = true;
            if (!zt_enter(agent)) { zt.agent = agent; return; }
            if (zg_debug) logi("profile_mod: Zen tab: Battle Settings up");
        }
        if (zt.ok) zt_frame(cm);
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logi("profile_mod: an exception in Zen Mode's tab"); }
        zt_tab.store(0);
        zt_content.store(false);
        zt = ZtScreen{};
        zt_up = false;
    }
}
// After LateUpdateBehavior (the main thread, the game's parts updated)
void zen_tab_after() {
    if (!zt_up || !zt.ok || !zt.content) return;
    try { zt_desc_paint(); } catch (...) {}
}
bool zt_hook(TD* td, const char* name, REFPreHookFn pre, REFPostHookFn post) {
    Method* m = td ? td->find_method(name) : nullptr;
    return m && m->get_function_raw() && m->add_hook(pre, post, false);
}
// After zen_init succeeded
void zen_tab_init(const REFrameworkPluginInitializeParam* param) {
    auto* tdb = API::get()->tdb();
    const bool a = zt_hook(tdb->find_type("app.UIPartsGuiLinearList"), "Input", zt_pre_input, zt_post_nop);
    const bool b = a && zt_hook(tdb->find_type("app.UIPartsListBase"), "InvokeSelectionChanged", zt_pre_changed, zt_post_nop);
    if (!a || !b) { logi("profile_mod: Zen Mode's tab off (the tab list's methods not found: game update?)"); return; }
    TD* spin = tdb->find_type("app.UIPartsSpin");
    int n = 0;
    for (const char* name : { "Add", "Sub", "AddNum", "SubNum", "MouseAdd", "MouseSub" }) n += zt_hook(spin, name, zt_pre_spin, zt_post_nop) ? 1 : 0;
    for (const char* name : { "InputLeft", "InputRight" }) n += zt_hook(spin, name, zt_pre_spin_lr, zt_post_nop) ? 1 : 0;
    n += zt_hook(spin, "InputDirection", zt_pre_spin_dir, zt_post_nop) ? 1 : 0;
    zt_hooks = n > 0;
    const bool d = zt_hooks && param->functions->on_post_application_entry && param->functions->on_post_application_entry("LateUpdateBehavior", zen_tab_after);
    logi("profile_mod: Zen Mode's tab %s (%d row hooks, description %s)", zt_hooks ? "on" : "off (the rows' methods not found)", n, d ? "on" : "off");
}
