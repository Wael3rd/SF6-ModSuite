// SF6 Slots — "Replay Outfits": a 6th tab of the Replays menu (agent CFNReplayTop), after Search.
//
// The page lists every playable character; each row shows "Default" or "<outfit> - Color N", the outfit a replay's fighter
// of that character will wear (replay_logic.hpp; the playback override reads the file). Left / Right cycle Default ->
// the game's outfits -> the Slots Loader's outfits -> Default, R (pad X / Square) cycles the colour, Up / Down move. Every
// change writes reframework/data/SF6_CostumeSlots_data/replay_outfits.json at once (temporary file + rename).
//
// How (a virtual tab: the game never hears of a 6th tab, its own tab list stays on Search):
//   - The tab bar (CFNReplayTop > p_SimpleList_0_tab) has six items, c_item_00 - c_item_05, the game shows five
//     (app.UICFNReplayTop.TabType NUM 5). c_item_05 is shown with our label; its mouse hit area is hidden.
//   - E / R1 on Search: app.UIAgent.InputTab / InputTabNext of the CFNReplayTop agent (its _TabItem, an
//     app.UIPartsSimpleList, says SelectedIndex 4) are swallowed (pre-hook, SKIP) and the tab becomes ours. A / L1 on
//     ours: swallowed too, back to Search. Next on ours: swallowed (nothing after us). The core's code check found these
//     three methods unique (pm_codecheck.txt, R18).
//   - While ours: the Search page's agents (every visible CFN* agent but CFNTop and CFNReplayTop: CFNReplayChildSearch,
//     CFNSearchItemList) get ForceInvisible on their main control and BlockInput; both are put back when we leave, when
//     the game changes tab by itself (a mouse click) and when the screen goes.
//   - The page's texts are copies of the header's title text (c_title > e_text_title), made by the core's
//     ui_clone_control inside the tab hook (R19: the screen's own input update), named e_text_ro_*, under c_title (root
//     space). They live and die with the screen's tree (R35: found again by name every frame, every write through the
//     core). Text, place, font, colour and visibility from LateUpdateBehavior (R1).
//   - Input: read natively while the game window has the focus (keys by scancode, so any keyboard layout: W A S D
//     places and the arrows; via.hid.GamePad's merged device: d-pad, left stick, X / Square), never hooked. The
//     screen's whole input goes through the page's agent (its BlockInput also stops the tab bar's A / E and Esc: seen
//     05/11), so Back is ours too: the tab-back key (Q place, the game's default), Esc, pad L1 or B go back to Search,
//     and the page gets its input back only once that key is released (the game never sees the press).
//   - The tab items' look is their via.gui PlayState: c_item_05 is DISABLE as the game leaves it, DEFAULT shown,
//     SELECT while ours (c_item_04 DEFAULT meanwhile, SELECT again when we leave).
// Gate: nothing but one core registry read every 15 frames while the Replays screen is not up.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "replay_logic.hpp"
#include "replay_tab.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>

namespace slots::replaytab {
namespace {

using namespace slots::replay;

constexpr const char* kTop = "CFNReplayTop";
constexpr int kSearch = 4;                        // app.UICFNReplayTop.TabType.SEARCH
constexpr uint32_t kPollEvery = 15;
constexpr int kRows = 20;                         // rows shown at once
constexpr int kMaxRefused = 3;
constexpr uint32_t kActFlags = 2 | 8;             // app.InputDigitalFlag Trigger | Repeat
constexpr uint32_t kDirTabBack = 16, kDirTabNext = 32;   // app.UIPartsItemDirection
constexpr const char* kTabLabel = "Replay Outfits";
// layout, root space (1920 x 1080): c_title sits at the root's origin
constexpr float kXName = 170, kXValue = 560, kYHead = 150, kYRow0 = 200, kDy = 37, kYHelp = 200 + kRows * 37 + 18;
constexpr float kFontRow = 28, kFontHead = 30, kFontHelp = 24;
constexpr uint32_t kColText = 0xFFFFFFFF, kColDim = 0xFFC8C8C8, kColFocus = 0xFF40D0FF;   // via.Color raw (R low byte)

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// ---- the hooks' side (any thread) ----
std::atomic<uintptr_t> g_top_addr{ 0 };          // the CFNReplayTop agent while its screen is up (frame)
std::atomic<uintptr_t> g_main_addr{ 0 };         // its main control (frame)
std::atomic<int> g_tab{ -1 };                     // the game's tab as the frame last read it
std::atomic<bool> g_ours{ false };
std::atomic<bool> g_enter{ false }, g_leave{ false };
bool g_leaving = false;                           // Back pressed on our page: the page waits for the key's release
std::atomic<int> g_refused{ 0 };
std::atomic<int> g_copy_status{ 1000 };
std::atomic<uint32_t> g_swallowed{ 0 };
std::atomic<uint32_t> g_leave_frame{ 0 };
thread_local bool tl_skipped = false;

// ---- frame state ----
bool g_init = false, g_ok = false;
bool g_up = false;
uintptr_t g_main_seen = 0;                        // the screen build the label / guide were set up for
bool g_guide_moved = false;
std::vector<std::string> g_hidden;                // agents hidden for our page (names, found again to restore)
std::vector<Fighter> g_cat;
Choices g_choices;
int g_cursor = 0, g_top_row = 0;
bool g_copies_shown = false;
std::set<uintptr_t> g_styled;
int g_status_logged = 1000;
bool g_place_logged = false;
uint32_t g_enter_frame = 0;
std::string g_back_hint = "L1";                   // the bar's tab-back guide text (the game's icon of the key)

std::string copy_name(const char* what, int i) {
    char b[48];
    std::snprintf(b, sizeof b, "e_text_ro_%s_%02d", what, i);
    return b;
}
std::vector<std::string> all_copy_names() {
    std::vector<std::string> v;
    for (int i = 0; i < kRows; ++i) { v.push_back(copy_name("name", i)); v.push_back(copy_name("value", i)); }
    v.push_back("e_text_ro_head");
    v.push_back("e_text_ro_help");
    return v;
}

// ---- the copies, made inside the tab hook (R19) ----
bool make_copies() {
    const SF6CoreAPI* a = A();
    if (!a || g_refused.load() >= kMaxRefused) return false;
    void* main = reinterpret_cast<void*>(g_main_addr.load(std::memory_order_acquire));
    void* title = main ? a->ui_find_child(main, "c_title", 8) : nullptr;
    void* tmpl = title ? a->ui_find_child(title, "e_text_title", 1) : nullptr;
    if (!tmpl) { ++g_refused; g_copy_status.store(-3000); return false; }
    for (const std::string& nm : all_copy_names()) {
        if (a->ui_find_child(title, nm.c_str(), 0)) continue;
        void* c = nullptr;
        const SF6CoreStatus st = a->ui_clone_control(C(), tmpl, title, nm.c_str(), &c);
        g_copy_status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st));
        if (st != SF6CORE_OK || !c) { ++g_refused; return false; }
        SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;    // via.gui.MaskType.NonTarget
        a->obj_invoke(C(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
        a->ui_set_visible(C(), c, 0);                           // placed and shown by the frame
    }
    return true;
}

constexpr int kCall = REFRAMEWORK_HOOK_CALL_ORIGINAL, kSkip = REFRAMEWORK_HOOK_SKIP_ORIGINAL;

int decide(void* self, int dir, uint32_t flag) {
    const uintptr_t top = g_top_addr.load(std::memory_order_acquire);
    if (!top || reinterpret_cast<uintptr_t>(self) != top || g_closing.load(std::memory_order_relaxed)) return kCall;
    const bool acts = (flag & kActFlags) != 0;
    if (g_ours.load(std::memory_order_acquire)) {
        if (acts && dir < 0) { g_ours.store(false); g_leave.store(true); g_leave_frame.store(g_frame); }
        g_swallowed.fetch_add(1);
        return kSkip;                                   // our tab: the game's list stays on Search
    }
    if (dir < 0 && g_leave_frame.load() == g_frame) return kSkip;   // the same press seen again (InputTab + InputTabBack)
    if (dir > 0 && acts && g_tab.load(std::memory_order_acquire) == kSearch) {
        bool ok = false;
        try { ok = make_copies(); } catch (...) { ++g_refused; g_copy_status.store(-2000); }
        if (!ok) return kCall;                          // no page to show: the game does its own thing
        g_ours.store(true);
        g_enter.store(true);
        g_swallowed.fetch_add(1);
        return kSkip;
    }
    return kCall;
}
uint32_t u32(void* a) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFF); }

int pre_input_tab(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputTab(dir, flag)
    tl_skipped = false;
    const uint32_t d = u32(argv[2]);
    const int dir = (d & kDirTabNext) ? 1 : (d & kDirTabBack) ? -1 : 0;
    if (!dir) return kCall;
    const int rc = decide(argv[1], dir, u32(argv[3]));
    tl_skipped = rc == kSkip;
    return rc;
}
void post_input_tab(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_skipped && ret_val) *ret_val = nullptr;      // System.Boolean false: no tab change happened
    tl_skipped = false;
}
int pre_tab_next(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { return decide(argv[1], 1, u32(argv[2])); }
int pre_tab_back(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { return decide(argv[1], -1, u32(argv[2])); }
void post_none(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- the catalog: every character's outfits and their colours ----
void build_catalog() {
    std::map<int, Fighter> by;
    std::set<std::pair<int, int>> slot_keys;
    // the Slots Loader's outfits: registry.json (possession: fighter, costume_no, name, nsfw; slots: mod_id), labels
    // from outfits.json
    Json reg, outs;
    std::map<std::pair<int, int>, std::string> mod_of, label_of;
    if (json_parse(read_file("reframework/data/SF6_Costumes_Data/outfits.json"), outs))
        if (const Json* os = outs.get("outfits"); os && os->kind == Json::Array)
            for (auto& e : os->arr) {
                const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* l = e.get("label");
                if (f && c && l) label_of[{ static_cast<int>(f->as_int()), static_cast<int>(c->as_int()) }] = l->as_str();
            }
    int slots = 0, nsfw_left = 0;
    if (json_parse(read_file("reframework/data/SF6_Costumes_Data/registry.json"), reg)) {
        if (const Json* ss = reg.get("slots"); ss && ss->kind == Json::Array)
            for (auto& e : ss->arr) {
                const Json* f = e.get("fighter"); const Json* n = e.get("new_costume_no"); const Json* m = e.get("mod_id");
                if (f && n && m) mod_of[{ static_cast<int>(f->as_int()), static_cast<int>(n->as_int()) }] = m->as_str();
            }
        const bool nsfw_ok = costumes::display_nsfw();
        if (const Json* ps = reg.get("possession"); ps && ps->kind == Json::Array)
            for (auto& e : ps->arr) {
                const Json* f = e.get("fighter"); const Json* c = e.get("costume_no");
                if (!f || !c) continue;
                const int fid = static_cast<int>(f->as_int()), cos = static_cast<int>(c->as_int());
                if (fid <= 0 || cos <= 0) continue;
                slot_keys.insert({ fid, cos });
                if (const Json* ns = e.get("nsfw"); ns && ns->kind == Json::Bool && ns->b && !nsfw_ok) { ++nsfw_left; continue; }
                Outfit o;
                o.costume = cos;
                auto m = mod_of.find({ fid, cos });
                o.extra = true;
                if (m != mod_of.end()) o.mod_id = m->second;            // none known: the file names the slot by its number
                auto l = label_of.find({ fid, cos });
                const Json* nm = e.get("name");
                o.name = l != label_of.end() && !l->second.empty() ? l->second : nm ? nm->as_str() : "Outfit " + std::to_string(cos);
                for (int k = 0; k < 10; ++k) o.colors.push_back(k);   // costumes.cpp gives every slot colours 0-9
                by[fid].outfits.push_back(o);
                ++slots;
            }
    }
    // the game's outfits: CostumeInventory.GetMasterDataRecord, records 1.. (the loader's start at 5000), the colours of
    // each from TableDataManager.GetFighterCostumeColors(record, false)
    int game = 0;
    auto* im = api().get_managed_singleton("app.InventoryManager");
    Obj* ci = im ? get_obj(im, "_Costume") : nullptr;
    Method* m_rec = ci ? method_of(ci->get_type_definition(), "GetMasterDataRecord", 1) : nullptr;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    Method* m_cols = tm ? method_of(tm->get_type_definition(), "GetFighterCostumeColors", 2) : nullptr;
    std::set<std::pair<int, int>> seen;
    if (m_rec && m_cols)
        for (int rid = 1, last = 0; rid < 5000 && rid - last <= 100; ++rid) {
            Obj* rec = call_obj(m_rec, ci, { arg_i32(rid) });
            int32_t fid = 0, cn = -1;
            if (!rec || !get_i32(rec, "fighterId", fid) || !get_i32(rec, "costumeNo", cn)) continue;
            last = rid;
            if (fid <= 0 || cn < 0 || slot_keys.count({ fid, cn }) || !seen.insert({ fid, cn }).second) continue;
            Outfit o;
            o.costume = cn;
            o.name = game_outfit_name(cn);
            if (Obj* cls = call_obj(m_cols, tm, { arg_i32(rid), arg_bool(false) })) {
                std::set<int> cs;
                for (int j = 0, n = list_count(cls); j < n; ++j) {
                    int32_t c = 0;
                    if (Obj* cr = list_item(cls, j); cr && get_i32(cr, "colorNo", c) && c >= 0) cs.insert(c);
                }
                o.colors.assign(cs.begin(), cs.end());
            }
            by[fid].outfits.push_back(o);
            ++game;
        }
    g_cat.clear();
    for (auto& [fid, f] : by) {
        f.fid = fid;
        const char* n = fighter_name(fid);
        f.name = n ? n : "Fighter " + std::to_string(fid);
        sort_outfits(f.outfits);
        if (!f.outfits.empty()) g_cat.push_back(f);
    }
    order_fighters(g_cat);
    logf("replay tab: %zu characters, %d game outfits, %d extra outfits%s", g_cat.size(), game, slots,
         nsfw_left ? (" (" + std::to_string(nsfw_left) + " NSFW hidden)").c_str() : "");
}

void load_choices() {
    Choices c;
    const std::string text = read_file(kFile);
    if (!text.empty() && !from_json(text, c)) logf("replay tab: %s unreadable, starting from Default", kFile);
    g_choices = c;
}
void save_choices() {
    if (!write_file(kFile, to_json(g_choices))) logf("replay tab: could not write %s", kFile);
}

// ---- input ----
struct Rep {
    bool held = false;
    uint32_t next = 0;
    bool step(bool now) {                          // a press, then repeats after 18 frames, every 5
        if (!now) { held = false; return false; }
        if (!held) { held = true; next = g_frame + 18; return true; }
        if (g_frame >= next) { next = g_frame + 5; return true; }
        return false;
    }
    void prime(bool now) { held = now; next = g_frame + 1000000; }   // a key held at entry acts only once released
};
Rep k_up, k_down, k_left, k_right, k_color, k_back;
Method* m_pad_merged = nullptr;
Method* m_pad_button = nullptr;
uint32_t b_up = 1, b_down = 2, b_left = 4, b_right = 8, b_x = 64, b_l1 = 256, b_b = 128;   // via.hid.GamePadButton
uint32_t b_sup = 0, b_sdown = 0, b_sleft = 0, b_sright = 0;

bool focused() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
bool sc_down(UINT sc) {
    const UINT vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK);
    return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}
bool vk_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
uint32_t pad_buttons() {
    void* s = api().get_native_singleton("via.hid.GamePad");
    Obj* dev = (s && m_pad_merged) ? call_obj(m_pad_merged, reinterpret_cast<Obj*>(s)) : nullptr;
    if (!dev) return 0;
    if (!m_pad_button) m_pad_button = method_of(dev->get_type_definition(), "get_Button", 0);
    auto r = call(m_pad_button, dev);
    return r.exception_thrown ? 0u : r.dword;
}
struct Keys { bool up, down, left, right, color, back; };
Keys read_keys() {
    Keys k{};
    if (focused()) {
        k.up = vk_down(VK_UP) || sc_down(0x11);
        k.down = vk_down(VK_DOWN) || sc_down(0x1F);
        k.left = vk_down(VK_LEFT) || sc_down(0x1E);
        k.right = vk_down(VK_RIGHT) || sc_down(0x20);
        k.color = sc_down(0x13);                   // R (its place on a QWERTY keyboard)
        k.back = vk_down(VK_ESCAPE) || sc_down(0x10);   // Esc, the tab-back key (Q's place: A on AZERTY)
    }
    const uint32_t b = pad_buttons();
    k.up = k.up || (b & b_up) || (b_sup && (b & b_sup));
    k.down = k.down || (b & b_down) || (b_sdown && (b & b_sdown));
    k.left = k.left || (b & b_left) || (b_sleft && (b & b_sleft));
    k.right = k.right || (b & b_right) || (b_sright && (b & b_sright));
    k.color = k.color || (b & b_x);
    k.back = k.back || (b & b_l1) || (b & b_b);
    return k;
}
void prime_keys() {
    const Keys k = read_keys();
    k_up.prime(k.up); k_down.prime(k.down); k_left.prime(k.left); k_right.prime(k.right); k_color.prime(k.color); k_back.prime(k.back);
}
void input() {
    const Keys k = read_keys();
    if (k_back.step(k.back)) {                     // back to Search (the page's input comes back once released)
        g_ours.store(false);
        g_leaving = true;
        return;
    }
    const int n = static_cast<int>(g_cat.size());
    if (k_up.step(k.up)) g_cursor = move_cursor(g_cursor, -1, n);
    if (k_down.step(k.down)) g_cursor = move_cursor(g_cursor, 1, n);
    g_cursor = std::max(0, std::min(g_cursor, n - 1));
    if (g_cat.empty()) return;
    const Fighter& f = g_cat[static_cast<size_t>(g_cursor)];
    auto it = g_choices.find(f.fid);
    const Choice cur = it != g_choices.end() ? it->second : Choice{};
    Choice next = cur;
    if (k_left.step(k.left)) next = cycle_outfit(f, next, -1);
    if (k_right.step(k.right)) next = cycle_outfit(f, next, 1);
    if (k_color.step(k.color)) next = cycle_color(f, next, 1);
    if (next == cur) return;
    if (next.forced) g_choices[f.fid] = next; else g_choices.erase(f.fid);
    save_choices();
    logf("replay tab: %s -> %s", f.name.c_str(), row_text(f, next).c_str());
}

// ---- the screen ----
bool actual_visible(Obj* ctrl) {
    bool ok = false;
    const bool v = ctrl && call_bool(ctrl, "get_ActualVisible", {}, &ok);
    return ok && v;
}
void* find(void* root, const char* name) { return root ? A()->ui_find_cached(root, name) : nullptr; }
void set_vis(void* c, bool on) { if (c) A()->ui_set_visible(C(), c, on ? 1 : 0); }

// A tab item's look (via.gui PlayState: DEFAULT, SELECT, DISABLE), written when it differs
void set_state(void* item, const char* st) {
    if (!item) return;
    if (str(call_obj(static_cast<Obj*>(item), "get_PlayState")) == st) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_STR; a.v.s = st;
    A()->obj_invoke(C(), item, "set_PlayState(System.String)", &a, 1, nullptr);
}

// The tab bar: c_item_05 shown with our label (its mouse hit area hidden: the game's list has 5 items), selected while
// ours; the bar's right guide moved past it, once per screen build
void tab_bar(Obj* main) {
    void* item5 = find(main, "c_item_05");
    if (!item5) return;
    set_vis(item5, true);
    if (void* t = A()->ui_find_child(item5, "e_text_menu", 1)) A()->ui_set_text(C(), t, kTabLabel);
    set_vis(A()->ui_find_child(item5, "e_hit_mouse", 1), false);
    set_vis(A()->ui_find_child(item5, "c_batch", 1), false);
    set_state(item5, g_ours.load() ? "SELECT" : "DEFAULT");
    if (g_ours.load()) set_state(find(main, "c_item_04"), "DEFAULT");
    if (!g_guide_moved) {
        g_guide_moved = true;
        Obj* guide = static_cast<Obj*>(find(main, "p_InputGuide_R_s_dUITabR"));
        Obj* item4 = static_cast<Obj*>(find(main, "c_item_04"));
        float gp[3] = {}, p4[3] = {}, p5[3] = {};
        auto pos = [](Obj* o, float out[3]) {
            if (!o) return false;
            auto r = call(o, "get_Position");
            if (r.exception_thrown) return false;
            std::memcpy(out, r.bytes.data(), 12);
            return true;
        };
        if (pos(guide, gp) && pos(item4, p4) && pos(static_cast<Obj*>(item5), p5)) {
            const float x = gp[0] + (p5[0] - p4[0]);
            A()->ui_set_position(C(), guide, x, gp[1], gp[2]);
            logf("replay tab: tab bar: item 4 x %.0f, item 5 x %.0f, right guide x %.0f -> %.0f", p4[0], p5[0], gp[0], x);
        }
    }
}

// The Search page's agents: every visible CFN* agent but the hub and the tab bar's
void hide_page(bool quiet) {
    g_hidden.clear();
    TD* td = api().tdb()->find_type("app.UIAgent");
    Obj* list = find_components(td);
    for (int i = 0, n = list_count(list); i < n; ++i) {
        Obj* ag = list_item(list, i);
        Obj* go = ag ? call_obj(ag, "get_GameObject") : nullptr;
        const std::string nm = go ? str(call_obj(go, "get_Name")) : "";
        if (nm.rfind("CFN", 0) != 0 || nm == "CFNTop" || nm == kTop) continue;
        Obj* cm = call_obj(ag, "get_ControlMain");
        if (!actual_visible(cm)) continue;
        A()->ui_set_block_input(C(), ag, 1);
        A()->ui_set_force_invisible(C(), cm, 1);
        g_hidden.push_back(nm);
    }
    std::string names;
    for (auto& s : g_hidden) names += (names.empty() ? "" : ", ") + s;
    if (!quiet || !names.empty()) logf("replay tab: page hidden: %s", names.empty() ? "(no agent found)" : names.c_str());
}
void restore_page() {
    for (const std::string& nm : g_hidden) {
        void* buf[4] = {};
        const uint32_t n = A()->ui_agents(nm.c_str(), buf, 4);
        for (uint32_t i = 0; i < n && i < 4; ++i) {
            Obj* ag = static_cast<Obj*>(buf[i]);
            A()->ui_set_block_input(C(), ag, 0);
            if (Obj* cm = call_obj(ag, "get_ControlMain")) A()->ui_set_force_invisible(C(), cm, 0);
        }
    }
    g_hidden.clear();
}

void hide_copies(Obj* main) {
    if (!g_copies_shown) return;
    g_copies_shown = false;
    void* title = find(main, "c_title");
    if (!title) return;
    for (const std::string& nm : all_copy_names()) set_vis(A()->ui_find_child(title, nm.c_str(), 0), false);
}

void put(void* title, const std::string& name, const std::string& text, float x, float y, float font, uint32_t col) {
    void* c = A()->ui_find_cached(title, name.c_str());
    if (!c) return;
    if (!g_styled.count(reinterpret_cast<uintptr_t>(c))) {
        g_styled.insert(reinterpret_cast<uintptr_t>(c));
        SF6CoreArg t{}; t.type = SF6CORE_ARG_I64; t.v.i = 1;
        A()->obj_invoke(C(), c, "set_AutoRegionFit(System.Boolean)", &t, 1, nullptr);
    }
    A()->ui_set_text(C(), c, text.c_str());
    A()->ui_set_font_size(C(), c, font, font);
    A()->ui_set_color(C(), c, col);
    A()->ui_set_position(C(), c, x, y, 0);
    A()->ui_set_force_invisible(C(), c, 0);
    A()->ui_set_visible(C(), c, 1);
}

void render(Obj* main) {
    void* title = find(main, "c_title");
    if (!title) return;
    const int n = static_cast<int>(g_cat.size());
    g_top_row = window_top(g_top_row, g_cursor, n, kRows);
    char head[96];
    std::snprintf(head, sizeof head, "Outfits in replays   %d / %d", n ? g_cursor + 1 : 0, n);   // the title font has no triangle
    put(title, "e_text_ro_head", n ? head : "No character found", kXName, kYHead, kFontHead, kColText);
    for (int r = 0; r < kRows; ++r) {
        const int i = g_top_row + r;
        const float y = kYRow0 + r * kDy;
        if (i >= n) {
            set_vis(A()->ui_find_cached(title, copy_name("name", r).c_str()), false);
            set_vis(A()->ui_find_cached(title, copy_name("value", r).c_str()), false);
            continue;
        }
        const Fighter& f = g_cat[static_cast<size_t>(i)];
        auto it = g_choices.find(f.fid);
        const Choice c = it != g_choices.end() ? it->second : Choice{};
        const bool foc = i == g_cursor;
        // the focused row in the focus colour only: the title font draws no triangle, and '<' opens a text tag
        put(title, copy_name("name", r), f.name, kXName, y, kFontRow, foc ? kColFocus : kColText);
        put(title, copy_name("value", r), row_text(f, c), kXValue, y, kFontRow, foc ? kColFocus : (c.forced ? kColText : kColDim));
    }
    put(title, "e_text_ro_help", "Up / Down: character     Left / Right: outfit     R / Square (X): colour     " + g_back_hint +
        " / Esc: back     Saved at once", kXName, kYHelp, kFontHelp, kColDim);
    g_copies_shown = true;
    if (!g_place_logged && g_frame - g_enter_frame == 3) {
        g_place_logged = true;
        if (Obj* c = static_cast<Obj*>(A()->ui_find_cached(title, copy_name("name", 0).c_str()))) {
            auto r = call(c, "get_Position");
            float p[3] = {};
            if (!r.exception_thrown) std::memcpy(p, r.bytes.data(), 12);
            logf("replay tab: first row placed at %.0f, %.0f (asked %.0f, %.0f)", p[0], p[1], kXName, kYRow0);
        }
    }
}

void enter(Obj* main) {
    g_enter_frame = g_frame;
    if (void* g = find(main, "p_InputGuide_L_s_dUITabL")) {
        if (Obj* t = static_cast<Obj*>(A()->ui_find_child(g, "e_text_0", 1))) {
            const std::string m = str(call_obj(t, "get_Message"));
            if (!m.empty()) g_back_hint = m;
        }
    }
    g_place_logged = false;
    build_catalog();                              // each time: the NSFW setting may have changed
    load_choices();
    hide_page(false);
    prime_keys();
    g_cursor = std::max(0, std::min(g_cursor, static_cast<int>(g_cat.size()) - 1));
    logf("replay tab: entered (%zu forced in the file, back key shown as %s)", g_choices.size(), g_back_hint.c_str());
}

void leave(Obj* main, const char* why) {
    restore_page();
    hide_copies(main);
    if (main) set_state(find(main, "c_item_04"), "SELECT");
    if (main) set_state(find(main, "c_item_05"), "DEFAULT");
    g_ours.store(false);
    logf("replay tab: left (%s)", why);
}

void down() {                                      // the screen went: nothing of ours left behind
    g_leaving = false;
    if (g_ours.load() || !g_hidden.empty()) leave(nullptr, "the Replays screen closed");
    g_enter.store(false);
    g_leave.store(false);
    g_top_addr.store(0);
    g_main_addr.store(0);
    g_tab.store(-1);
    g_up = false;
    g_main_seen = 0;
    g_copies_shown = false;
    g_styled.clear();
}

void init() {
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, ui_find_cached)) {
        logf("replay tab: off (needs the core with ui_clone_control, 0.8.2)");
        return;
    }
    TD* td = api().tdb()->find_type("app.UIAgent");
    Method* m_tab = td ? td->find_method("InputTab(app.UIPartsItemDirection, app.InputDigitalFlag)") : nullptr;
    if (!m_tab) m_tab = td ? method_of(td, "InputTab", 2) : nullptr;
    Method* m_next = td ? method_of(td, "InputTabNext", 1) : nullptr;
    Method* m_back = td ? method_of(td, "InputTabBack", 1) : nullptr;
    const bool h1 = m_tab && m_tab->get_function_raw() && m_tab->add_hook(pre_input_tab, post_input_tab, false);
    const bool h2 = m_next && m_next->get_function_raw() && m_next->add_hook(pre_tab_next, post_none, false);
    const bool h3 = m_back && m_back->get_function_raw() && m_back->add_hook(pre_tab_back, post_none, false);
    if (!h1 && !h2) {
        logf("replay tab: off (app.UIAgent.InputTab / InputTabNext not hooked)");
        return;
    }
    TD* tdb_pad = api().tdb()->find_type("via.hid.GamePad");
    m_pad_merged = tdb_pad ? method_of(tdb_pad, "get_MergedDevice", 0) : nullptr;
    if (TD* btn = api().tdb()->find_type("via.hid.GamePadButton")) {
        auto bit = [&](const char* f, uint32_t fb) { auto* x = btn->find_field(f); return x ? static_cast<uint32_t>(x->get_data<int32_t>(nullptr)) : fb; };
        b_up = bit("LUp", 1); b_down = bit("LDown", 2); b_left = bit("LLeft", 4); b_right = bit("LRight", 8); b_x = bit("RLeft", 64);
        b_l1 = bit("LTrigTop", 256); b_b = bit("RRight", 128);
        b_sup = bit("EmuLup", 0); b_sdown = bit("EmuLdown", 0); b_sleft = bit("EmuLleft", 0); b_sright = bit("EmuLright", 0);
    }
    g_ok = true;
    logf("replay tab: on (hooks InputTab %d, InputTabNext %d, InputTabBack %d; pad bits %u %u %u %u x %u)", h1, h2, h3,
         b_up, b_down, b_left, b_right, b_x);
}

void tick() {
    void* buf[2] = {};
    const uint32_t n = A()->ui_agents(kTop, buf, 2);
    Obj* agent = n ? static_cast<Obj*>(buf[0]) : nullptr;
    Obj* main = agent ? call_obj(agent, "get_ControlMain") : nullptr;
    if (!main || !actual_visible(main)) {
        if (g_up) down();
        return;
    }
    if (!g_up) logf("replay tab: Replays screen up");
    g_up = true;
    g_top_addr.store(reinterpret_cast<uintptr_t>(agent), std::memory_order_release);
    g_main_addr.store(reinterpret_cast<uintptr_t>(main), std::memory_order_release);
    if (g_main_seen != reinterpret_cast<uintptr_t>(main)) {   // a new build of the screen
        g_main_seen = reinterpret_cast<uintptr_t>(main);
        g_guide_moved = false;
        g_styled.clear();
        g_copies_shown = false;
    }
    int32_t sel = -1;
    if (Obj* ti = get_obj(agent, "_TabItem")) {
        auto r = call(ti, "get_SelectedIndex");
        if (!r.exception_thrown) sel = static_cast<int32_t>(r.dword);
    }
    const int st = g_copy_status.load();
    if (st != g_status_logged) { g_status_logged = st; logf("replay tab: copies status %d (refused %d)", st, g_refused.load()); }
    if (g_ours.load()) {
        if (sel != kSearch && sel >= 0) {             // the game changed tab by itself (a mouse click): ours gives way
            leave(main, "the game changed tab");
            g_enter.store(false);
        }
    } else {
        g_tab.store(sel, std::memory_order_release);
    }
    if (g_enter.exchange(false) && g_ours.load()) enter(main);
    if (g_leave.exchange(false)) leave(main, "tab back");
    if (g_leaving) {
        hide_copies(main);
        const Keys k = read_keys();
        if (!k.back) { g_leaving = false; leave(main, "back key"); }
    }
    tab_bar(main);
    if (g_ours.load()) {
        if (g_frame % 30 == 0 && g_hidden.empty()) hide_page(true);   // the page's agents were not up yet at entry
        input();
        render(main);
    } else {
        hide_copies(main);
    }
}

}  // namespace

void frame() {
    if (!g_init) { g_init = true; try { init(); } catch (...) { logf("replay tab: EXCEPTION at init"); } }
    if (!g_ok) return;
    if (!g_up && !g_ours.load() && !g_leaving && (g_frame % kPollEvery) != 0) return;
    try {
        tick();
    } catch (const std::exception& e) {
        logf("replay tab: EXCEPTION %s", e.what());
    } catch (...) {
        logf("replay tab: EXCEPTION (unknown)");
    }
}

}  // namespace slots::replaytab
