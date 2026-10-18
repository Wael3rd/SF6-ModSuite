#include "decide.hpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings
#include "nlohmann/json.hpp"

#include "../common.hpp"
#include "../perf.hpp"
#include "../perf_hook.hpp"
#include "catalog.hpp"
#include "chips.hpp"
#include "color_mods.hpp"
#include "live.hpp"
#include "live_internal.hpp"

namespace engine::decide {
namespace {

using live::API;
using live::MO;

// Constants carried over from SF6_ColorSpinExtra.lua (line of origin in brackets)
constexpr uint32_t kBaseColor = 1;            // BASE_COLOR [29]: colour "02", the one every RW / MC entry rides on
constexpr uint32_t kP2VirtualId = 91;         // P2_VIRTUAL_ID [30]: stored for a P2 that wants BASE_COLOR in a mirror
constexpr uint64_t kTickEvery = 3;            // tick(): cs.frame % 3 [1422]
constexpr uint64_t kBattlePassEvery = 30;     // late_tasks: dbg.frame % 30 [4117]
constexpr uint64_t kP2RestoreAfter = 30;      // tick(): cs.frame - p2.opened_at > 30 [1443]
constexpr int kP2Repush = 12;                 // mirror_fix_post: p2_repush = 12 [1825]
constexpr int kMaxFaults = 3;                 // then this feature switches itself off (the Lua decides again)
constexpr const char* kDataDir = "reframework/data";
constexpr const char* kDataPrefix = "reframework/data/";
constexpr const char* kOverridesPrefix = "reframework/data/SF6_ColorSpinExtra_data/overrides/";
constexpr const char* kChipFile = "reframework/data/SF6_ColorSpinExtra_data/chip_slots.json";
constexpr const char* kStateFile = "reframework/data/SF6_ColorSpinExtra_data/state.json";
constexpr const char* kInfoType = "app.InventoryManager.ColorInventory.ColorInfo";

// ---- engine types and methods (resolved once) --------------------------------------------------------------------
struct {
    API::Method* comp_go = nullptr;           // via.Component.get_GameObject
    API::Method* go_tr = nullptr;             // via.GameObject.get_Transform
    API::Method* go_name = nullptr;           // via.GameObject.get_Name
    API::Method* tr_parent = nullptr;         // via.Transform.get_Parent
    API::Method* tr_go = nullptr;             // via.Transform.get_GameObject
    API::Method* scene_current = nullptr;     // via.SceneManager.get_CurrentScene
    API::Method* find_components = nullptr;   // via.Scene.findComponents(System.Type)
    MO* ctrl_type = nullptr;                  // System.Type of app.PlayerColorController
    API::Field* pbm = nullptr;                // gBattle.PBManager (static)
    API::Method* game_chips = nullptr;        // app.helper.hGUI.GetFighterCostumeColorData (static)
    API::Method* set_color = nullptr;         // app.PlayerColorController.SetColor(System.Int32)
    API::Method* tmp_cos_col = nullptr;       // app.GuiManager.GuiEsportsManager.setTempCosColData
    API::Method* tmp_col = nullptr;           // app.GuiManager.GuiEsportsManager.setTempColor
} R;

bool g_cm_pending = false;                    // reframework/color_mods changed: rebuild the catalogue at a quiet moment
int g_cm_calm = 0;
bool g_installed = false;
bool g_disabled = false;
int g_faults = 0;

// ---- what the Lua screens push (Lua: state.p1 / p2 / fs, menus[1..2], fsm.text) -----------------------------------
struct Sel { int rw = 0, fighter = 0, costume = 0; };
Sel g_sel[3];                                 // 0 = P1, 1 = P2 / CPU, 2 = Fighter Settings (state.fs)
MO* g_param[2] = {};                          // colour menus' Param (held: add_ref while pushed)
bool g_live[2] = {};                          // m.text and m.active and not m.restore_pending
MO* g_fs_text = nullptr;                      // Fighter Settings colour spin text (held)

// ---- decisions state (Lua locals of the same names) ---------------------------------------------------------------
bool g_list_known = false;                    // list_args (nil until the first GetColorList)
uint32_t g_list_f = 0, g_list_c = 0;
int g_extra_first = -1, g_extra_count = 0;    // extra_first (-1 = nil), extra_count
std::unordered_set<uintptr_t> g_p2_ctrls;     // controllers whose last SetColor carried P2_VIRTUAL_ID
uintptr_t g_side_data[3] = {};                // [1], [2]: ColorData address seen on that side's preview
std::unordered_map<uintptr_t, std::string> g_applied;   // controller -> signature of what it wears
uint64_t g_setups[4] = {};                    // dbg.setups[1..3]
MO* g_side_ctrl[3] = {};                      // dbg.side_ctrl[1..2] (held, like the Lua reference)
struct Worn { bool set = false; rsz::NodeRef tree; std::optional<Entry> entry; };
Worn g_worn[3];                               // dbg.worn[1..2]
// P2 colour menu, mirror rule support (Lua p2 = { color_id, active, opened_at, restore })
std::optional<int32_t> g_p2_color_id;
bool g_p2_active = false;
uint64_t g_p2_opened_at = 0;
struct P2Restore { uint32_t fighter = 0, costume = 0; int32_t player = 0; MO* self = nullptr; };
std::optional<P2Restore> g_p2_restore;        // self held
struct FixRequest { int32_t player = 0; uint32_t fighter = 0, costume = 0; std::optional<uint32_t> lvl; MO* self = nullptr; };
std::optional<FixRequest> g_fix;
bool g_fix_guard = false;
int g_p2_repush = 0;
// confirm re-issue (03/10): after a load the game sometimes shows a battle model in its game colour although its
// SetupColor was patched (seen with the Lua too); a SetColor issued later puts it right. Once per side and load.
constexpr uint64_t kConfirmAfter = 60;      // frames after the patch of a side's battle controller
constexpr uint64_t kConfirmQuiet = 120;     // no new confirm within this many frames of the last one (no loop)
uint64_t g_confirm_at[3] = {};              // [1], [2]: frame of the scheduled confirm (0 = none)
uint64_t g_confirm_last[3] = {};
uint64_t g_mirror_clears = 0;
uint64_t g_tick = 0;                          // Lua cs.frame (one per frame)

Catalog g_cat;
std::map<std::string, std::vector<void*>> g_infos;     // "<fighter>_<costume>" -> its ColorInfo objects (made once)
chips::Config g_chip_cfg;
std::map<std::string, chips::Pair> g_game_chip_cache;   // dbg.game_chip_cache
struct LiveCache { bool set = false; uintptr_t addr = 0; uint32_t num = 0; rsz::NodeRef tree; };
LiveCache g_live_cache[3];                    // dbg.live_cache[1..2]

std::unordered_set<uintptr_t> g_scene_set;    // dbg.scene_set, rebuilt at most once per frame
uint64_t g_scene_frame = UINT64_MAX;

struct {
    uint64_t setups = 0, patched = 0, shared = 0, no_data = 0, edit = 0, lists = 0, entries = 0, p2_marks = 0;
    uint64_t mirror = 0, mirror_left = 0, battle_fixes = 0, set_colors = 0, refused = 0, squares = 0, infos = 0, confirms = 0;
} g_n;

// ---- small helpers -----------------------------------------------------------------------------------------------
bool is_managed(void* o) { return o && API::get()->sdk()->managed_object->is_managed_object(o); }

void hold(MO*& slot, MO* obj) {                        // keep a reference while we keep the pointer (Lua semantics)
    if (slot == obj) return;
    if (obj) obj->add_ref();
    if (slot) slot->release();
    slot = obj;
}

std::map<std::pair<API::TypeDefinition*, std::string>, API::Method*> g_methods;
API::Method* method_of(MO* obj, const char* sig) {     // by full signature (bare-name fallback) or no-argument name
    auto* td = obj->get_type_definition();
    const auto key = std::make_pair(td, std::string(sig));
    if (auto it = g_methods.find(key); it != g_methods.end()) return it->second;
    API::Method* m = std::strchr(sig, '(') ? ll::find_method_full(td, sig) : ll::find_method_noargs(td, sig);
    g_methods[key] = m;
    return m;
}
reframework::InvokeRet call(MO* obj, const char* sig, const std::vector<void*>& args = {}) {   // Lua obj:call(...)
    reframework::InvokeRet r{};
    r.exception_thrown = true;
    if (!obj) return r;
    API::Method* m = method_of(obj, sig);
    if (!m) return r;
    return m->invoke(obj, args);
}
MO* call_obj(MO* obj, const char* sig, const std::vector<void*>& args = {}) {
    auto r = call(obj, sig, args);
    return r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
}

std::optional<uint32_t> color_num(MO* ctrl) {         // ctrl:get_field("ColorNum")
    uint32_t n = 0;
    if (!ll::read_field_u32(ctrl, "ColorNum", n)) return std::nullopt;
    return n;
}
MO* color_data_obj(MO* ctrl) { return ll::read_field_object(ctrl, "ColorData"); }

// the fighter / costume folder of the colour file a controller reads: Product/Model/esf/esf027/003/esf027_003_CCVD.user
// -> { 27, 2 } (costume 0-based); -1 = unknown. The costume comes from the folder ("esf027/003/"): an outfit added by
// another mod keeps the file name of the outfit it was made from (Lily's slot 13: esf012/014/esf012_001_CCVD.user, read
// as costume 0 before, 06/10); the file name ("esf%d%d%d_(%d%d%d)_CCVD") only when the path has no such folder.
std::pair<int, int> path_identity(MO* ctrl) {
    MO* cd = ctrl ? color_data_obj(ctrl) : nullptr;
    if (!cd) return { -1, -1 };
    auto r = call(cd, "get_Path");
    const std::string path = r.exception_thrown ? std::string() : ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr));
    auto dig = [&](size_t i) { return i < path.size() && path[i] >= '0' && path[i] <= '9'; };
    for (size_t i = 0; i + 11 <= path.size(); ++i) {
        if (path.compare(i, 3, "esf") == 0 && dig(i + 3) && dig(i + 4) && dig(i + 5) && path[i + 6] == '/' &&
            dig(i + 7) && dig(i + 8) && dig(i + 9) && path[i + 10] == '/')
            return { std::stoi(path.substr(i + 3, 3)), std::stoi(path.substr(i + 7, 3)) - 1 };
    }
    for (size_t i = 0; i + 15 <= path.size(); ++i) {
        if (path.compare(i, 3, "esf") == 0 && dig(i + 3) && dig(i + 4) && dig(i + 5) && path[i + 6] == '_' &&
            dig(i + 7) && dig(i + 8) && dig(i + 9) && path.compare(i + 10, 5, "_CCVD") == 0)
            return { std::stoi(path.substr(i + 3, 3)), std::stoi(path.substr(i + 7, 3)) - 1 };
    }
    return { -1, -1 };
}

void fault(const char* where) {
    ++g_faults;
    cmn::journal("decisions: fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults && !g_disabled) {
        g_disabled = true;
        cmn::log_warn("[ColorModNative] decisions: %d faults (last in %s), switched off - the Lua decides again", g_faults, where);
    }
}

// ---- scene (dbg.scene_ctrls / dbg.in_scene) ------------------------------------------------------------------------
std::vector<MO*> scene_ctrls() {
    std::vector<MO*> out;
    void* sm = API::get()->get_native_singleton("via.SceneManager");
    if (!sm || !R.scene_current || !R.find_components || !R.ctrl_type) return out;
    auto rs = R.scene_current->invoke(reinterpret_cast<MO*>(sm), std::vector<void*>{});
    MO* scene = rs.exception_thrown ? nullptr : reinterpret_cast<MO*>(rs.ptr);
    if (!scene) return out;
    auto rc = R.find_components->invoke(scene, std::vector<void*>{ (void*)R.ctrl_type });
    MO* comps = rc.exception_thrown ? nullptr : reinterpret_cast<MO*>(rc.ptr);
    if (!comps) return out;
    const uint32_t n = live::arr_size(comps);       // get_Length / GetValue: the 02/10 get_size quirk does not apply
    for (uint32_t i = 0; i < n; ++i) if (MO* c = live::arr_at(comps, i)) out.push_back(c);
    return out;
}
bool in_scene(MO* ctrl) {
    if (!is_managed(ctrl)) return false;
    if (g_scene_frame != cmn::frame()) {
        g_scene_set.clear();
        for (MO* c : scene_ctrls()) g_scene_set.insert((uintptr_t)c);
        g_scene_frame = cmn::frame();
    }
    return g_scene_set.count((uintptr_t)ctrl) != 0;
}

// dbg.set_color: only on a live colour controller still in the current scene (crash 02/10 07:38). why = journal label.
bool set_color(MO* ctrl, std::optional<uint32_t> num, const std::string& why, bool fresh) {
    if (!is_managed(ctrl)) { cmn::journal("SetColor %s refused: dead controller", why.c_str()); ++g_n.refused; return false; }
    if (!live::is_color_ctrl(ctrl)) { cmn::journal("SetColor %s refused: not a colour controller any more", why.c_str()); ++g_n.refused; return false; }
    if (!fresh) {
        g_scene_frame = UINT64_MAX;                  // a fresh scan: this call can follow a change in the same frame
        if (!in_scene(ctrl)) { cmn::journal("SetColor %s refused: 0x%llx not in the scene", why.c_str(), (unsigned long long)(uintptr_t)ctrl); ++g_n.refused; return false; }
    }
    if (!num) num = color_num(ctrl);
    if (!num || !R.set_color) return false;
    if (!why.empty()) cmn::journal("SetColor %s: 0x%llx <- %u", why.c_str(), (unsigned long long)(uintptr_t)ctrl, *num);
    if (wjournal::active()) wjournal::set_color("decide::SetColor", (uint64_t)(uintptr_t)ctrl, *num);
    ++g_n.set_colors;
    R.set_color->invoke(ctrl, std::vector<void*>{ ll::arg_i32((int32_t)*num) });
    return true;
}

MO* find_visual_ctrl(int side) {                      // preview model controller of a side (char select)
    const std::string want = "FighterVisual_0" + std::to_string(side);
    for (MO* c : scene_ctrls()) if (live::go_name(c) == want) return c;
    return nullptr;
}

// ---- who is P1? ----------------------------------------------------------------------------------------------------
MO* battle_ctrl(int side) {                           // gBattle.PBManager.Players[side - 1]._ColorCtrl
    if (!R.pbm) return nullptr;
    void* slot = R.pbm->get_data_raw(nullptr, false);
    MO* pbm = slot ? *reinterpret_cast<MO**>(slot) : nullptr;
    MO* players = pbm ? ll::read_field_object(pbm, "Players") : nullptr;
    if (!players) return nullptr;
    auto has = call(players, "ContainsKey(System.Int32)", { ll::arg_i32(side - 1) });
    if (has.exception_thrown || has.byte == 0) return nullptr;
    MO* pb = call_obj(players, "get_Item(System.Int32)", { ll::arg_i32(side - 1) });
    return pb ? ll::read_field_object(pb, "_ColorCtrl") : nullptr;
}

MO* go_of(MO* comp) { return live::invoke_obj(comp, R.comp_go); }

int side_from_battle(MO* ctrl) {
    for (int side = 1; side <= 2; ++side) if (battle_ctrl(side) == ctrl) return side;
    // sub-model controllers (Mai: sleeves / fan...) hang under the player's model: walk up the parents to a side's
    // _ColorCtrl object (a mirror match shares ONE ColorData between both players, 02/09)
    MO* a1 = battle_ctrl(1);
    MO* a2 = battle_ctrl(2);
    MO* g1 = go_of(a1);
    MO* g2 = go_of(a2);
    MO* t = live::invoke_obj(go_of(ctrl), R.go_tr);
    for (int i = 0; i < 6 && t; ++i) {
        MO* par = live::invoke_obj(t, R.tr_parent);
        if (!par) break;
        MO* go = live::invoke_obj(par, R.tr_go);
        if (g1 && go == g1) return 1;
        if (g2 && go == g2) return 2;
        t = par;
    }
    // clones (after-images, projectiles) share the ColorData: the side whose _ColorCtrl wears the same ColorNum
    MO* cd = color_data_obj(ctrl);
    if (!cd) return 0;
    const auto num = color_num(ctrl);
    std::vector<int> cands;
    for (int side = 1; side <= 2; ++side) {
        MO* cc = battle_ctrl(side);
        if (cc && color_data_obj(cc) == cd) cands.push_back(side);
    }
    if (cands.size() == 1) return cands[0];
    for (int side : cands) { MO* cc = battle_ctrl(side); if (cc && color_num(cc) == num) return side; }
    return cands.empty() ? 0 : cands[0];
}

std::pair<int, int> battle_identity(const std::string& name) {   // "^esf(%d%d%d)v(%d%d)" -> fighter, nn (-1 = nil)
    auto dig = [&](size_t i) { return i < name.size() && name[i] >= '0' && name[i] <= '9'; };
    if (name.compare(0, 3, "esf") != 0 || !dig(3) || !dig(4) || !dig(5) || name.size() < 9 || name[6] != 'v' || !dig(7) || !dig(8))
        return { -1, -1 };
    return { std::stoi(name.substr(3, 3)), std::stoi(name.substr(7, 2)) };
}

// ---- which entry, which tree ---------------------------------------------------------------------------------------
int entry_at(int idx) {                               // list index -> entry number (0 = none)
    if (g_extra_first < 0 || idx < g_extra_first || idx >= g_extra_first + g_extra_count) return 0;
    return idx - g_extra_first + 1;
}

const std::vector<Entry>* entries_for(int fighter, int costume) {
    if (fighter < 0 || costume < 0) return nullptr;
    return g_cat.entries(fighter, costume);
}

int fs_entry() {                                      // entry shown by the Fighter Settings spin right now (0 = none)
    if (!g_fs_text || !is_managed(g_fs_text)) {
        // spin closed (Battle Settings preview, or leaving the screen): the remembered choice for this fighter
        const Sel& s = g_sel[2];
        if (s.rw > 0 && g_list_known && s.fighter == (int)g_list_f && s.costume == (int)g_list_c) return s.rw;
        return 0;
    }
    auto r = call(g_fs_text, "get_Message");
    if (r.exception_thrown || !g_list_known) return 0;
    return g_cat.label_to_k((int)g_list_f, (int)g_list_c, ll::managed_string_to_utf8(reinterpret_cast<MO*>(r.ptr)));
}

int wanted_entry(int side) {                          // the RW entry a side should wear right now (0 = none)
    if (side == 3) return fs_entry();
    if (g_live[side - 1]) {                           // player on the menu: live cursor
        MO* prm = g_param[side - 1];
        auto r = call(prm, "get__cacheColorIndex");
        return entry_at(r.exception_thrown ? -1 : (int32_t)r.dword);
    }
    return g_sel[side - 1].rw;                        // menu inactive / battle: remembered
}

Sel side_sel(int side) {
    if (side == 3) {
        Sel s;
        s.fighter = g_list_known ? (int)g_list_f : -1;
        s.costume = g_list_known ? (int)g_list_c : -1;
        return s;
    }
    return g_sel[side - 1];
}

// ---- Battle Settings in a match (0.19.5) ----------------------------------------------------------------------------
// A mode that takes the costume and colour from Battle Settings (online, Battle Hub, Combo Trials...: every mode but
// Arcade, Training, Versus and Team Battle, where a select screen picks them) dresses the player's own fighter with
// what Fighter Settings remembers (state.fs: one fighter, the last one edited there), as the Edit Character preview
// shows it, instead of the select screen's choice (state.p1 / p2). Before, such a match wore the game's 02.
// The player comes from the battle description, read as SF6_SlotsLoader's alias reads it: online the fighter of our
// session member, offline a fighter on a pad. Its model is the one of that fighter; in a mirror match (the other
// player on the same fighter), the one on its team's side (team 0 = P1).
struct BsOwn { int fighter = -1; int team = -1; bool mirror = false; };
struct Bs { bool on = false; std::vector<BsOwn> own; };
Bs g_bs;
MO* g_bs_desc = nullptr;                              // the description read last (compared only, never dereferenced)
uint64_t g_bs_frame = UINT64_MAX, g_bs_read = 0;
std::string g_bs_sig;
constexpr uint64_t kBsRereadEvery = 15;               // the description fills in while the match loads

int list_count(MO* list) { auto r = call(list, "get_Count"); return r.exception_thrown ? 0 : (int)(int32_t)r.dword; }
MO* list_item(MO* list, int i) { return call_obj(list, "get_Item(System.Int32)", { ll::arg_i32(i) }); }

uint64_t g_bs_quiet_until = 0;                       // after a raise: Battle Settings colours off for a while
const Bs& bs_read(uint64_t fr);

const Bs& bs_state() {                                // once per frame at most; lock held
    const uint64_t fr = cmn::frame();
    if (g_bs_frame == fr) return g_bs;
    g_bs_frame = fr;
    if (fr < g_bs_quiet_until) return g_bs;
    try {
        return bs_read(fr);
    } catch (...) {                                   // never a decisions fault: those switch every colour off
        g_bs = Bs{};
        g_bs_desc = nullptr;
        g_bs_quiet_until = fr + 300;
        g_bs_sig = "off: battle description unreadable";
        cmn::journal("Battle Settings colours: battle description unreadable, off for 300 frames");
        return g_bs;
    }
}

const Bs& bs_read(uint64_t fr) {
    MO* bfm = API::get()->get_managed_singleton("app.bFlowManager");
    MO* work = bfm ? ll::read_field_object(bfm, "m_flow_work") : nullptr;
    MO* map = work ? ll::read_field_object(work, "_FlowMap") : nullptr;
    MO* desc = map ? ll::read_field_object(map, "m_desc") : nullptr;
    if (desc && !is_managed(desc)) desc = nullptr;
    if (desc == g_bs_desc && fr - g_bs_read < kBsRereadEvery) return g_bs;
    g_bs_desc = desc;
    g_bs_read = fr;
    Bs bs;
    std::string sig = "off: no battle description";
    if (desc) {
        uint8_t mode = 0;                             // app.EGameMode, one byte
        if (MO* rule = ll::read_field_object(desc, "Rule")) ll::read_field_raw(rule, "GameMode", &mode, 1);
        const bool picked = mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 19;
        bool net = false;
        int32_t self = -1;
        ll::read_field_bool(desc, "IsNetwork", net);
        ll::read_field_i32(desc, "SelfSessionMemberindex", self);
        std::vector<int> others;
        MO* teams = ll::read_field_object(desc, "Teams");
        const int nt = list_count(teams);
        for (int t = 0; t < nt; ++t) {
            MO* team = list_item(teams, t);
            MO* fighters = team ? ll::read_field_object(team, "Fighters") : nullptr;
            const int nf = list_count(fighters);
            for (int i = 0; i < nf; ++i) {
                MO* f = list_item(fighters, i);
                int32_t fid = -1, pad = -1, member = -1;
                if (!f || !ll::read_field_i32(f, "FighterId", fid)) continue;
                ll::read_field_i32(f, "PadId", pad);
                ll::read_field_i32(f, "SessionMemberIndex", member);
                const bool own = net ? (self >= 0 && member == self) : pad >= 0;
                if (own) bs.own.push_back({ fid, t, false });
                else others.push_back(fid);
            }
        }
        for (BsOwn& o : bs.own)
            for (int f : others) if (f == o.fighter) o.mirror = true;
        bs.on = !picked && !bs.own.empty();
        char b[64];
        std::snprintf(b, sizeof b, "%s mode %d %s:", bs.on ? "on" : "off", (int)mode, net ? "online" : "offline");
        sig = b;
        for (const BsOwn& o : bs.own) sig += " F" + std::to_string(o.fighter) + "/team " + std::to_string(o.team) + (o.mirror ? " mirror" : "");
        if (bs.own.empty()) sig += " no player";
    }
    if (sig != g_bs_sig) {
        g_bs_sig = sig;
        cmn::journal("Battle Settings colours: %s", sig.c_str());
    }
    g_bs = std::move(bs);
    return g_bs;
}

// whether this side's controller is the player's own fighter in such a mode (the select screens' previews never)
bool bs_applies(MO* ctrl, int side, int fighter) {
    if (side != 1 && side != 2) return false;
    const Bs& bs = bs_state();
    if (!bs.on || fighter < 0) return false;
    const std::string name = live::go_name(ctrl);
    if (name == "FighterVisual_01" || name == "FighterVisual_02") return false;
    for (const BsOwn& o : bs.own)
        if (o.fighter == fighter && (!o.mirror || o.team + 1 == side)) return true;
    return false;
}

rsz::NodeRef entry_tree(const Entry& e) {             // its override when one exists, else the file itself
    if (!e.okey.empty()) if (auto ov = live::file_tree(kOverridesPrefix + e.okey + ".user.2")) return ov;
    return live::file_tree(kDataPrefix + e.path);
}

struct Resolved { rsz::NodeRef tree; std::optional<Entry> entry; int k = 0; bool bs = false; };   // k > 0 entry, 0 override

// The player's own fighter in a Battle Settings mode: the Fighter Settings entry when its fighter and costume are the
// model's and the model wears BASE_COLOR, else a game-colour override of the model's own fighter / costume / colour.
// Under SF6_SlotsLoader's alias the model is shown as DriveTech: the core says which slot and colour it really wears.
Resolved resolve_bs(MO* ctrl, int fighter, uint32_t id) {
    Resolved out;
    out.bs = true;
    int costume = path_identity(ctrl).second;
    uint32_t num = id;
    const SF6CoreAPI* a = cmn::core();
    int32_t rc = -1, rn = -1;
    if (a && SF6CORE_HAS(a, sel_get_effective) && costume >= 0 && a->sel_get_effective(fighter, costume, &rc, &rn) && rc >= 0) {
        costume = rc;
        if (rn >= 0) num = (uint32_t)rn;
    }
    if (costume < 0) return out;
    const Sel& fs = g_sel[2];
    if (num == kBaseColor && fs.rw > 0 && fs.fighter == fighter && fs.costume == costume) {
        const auto* entries = entries_for(fighter, costume);
        if (entries && (size_t)fs.rw <= entries->size()) {
            const Entry& e = (*entries)[(size_t)fs.rw - 1];
            out.tree = entry_tree(e);
            out.entry = e;
            out.k = fs.rw;
            return out;
        }
    }
    char key[96];
    std::snprintf(key, sizeof(key), "%d_%d_game_%u", fighter, costume, num);
    if (auto ov = live::file_tree(std::string(kOverridesPrefix) + key + ".user.2")) { out.tree = ov; out.k = 0; }
    return out;
}

// what a controller should wear: an entry of the side's list on BASE_COLOR, else a game-colour override
Resolved resolve_tree(MO* ctrl, int side) {
    Resolved out;
    const auto id = color_num(ctrl);
    if (!id) return out;
    if (side == 1 || side == 2) {
        const int fc = path_identity(ctrl).first;
        if (bs_applies(ctrl, side, fc)) return resolve_bs(ctrl, fc, *id);
    }
    const Sel sel = side_sel(side);
    // a side's remembered entry / game-colour override belongs to its remembered fighter only: another fighter on that
    // side (picked on a game colour, a replay, an online opponent) wore its same-name clusters (Ryu got Mai MC 2's
    // skin, 03/10). The fighter comes from the controller's colour file; unknown = no check (as before).
    if (side == 1 || side == 2) {
        const int fc = path_identity(ctrl).first;
        if (fc >= 0 && sel.fighter >= 0 && fc != sel.fighter) return out;
    }
    if (*id == kBaseColor) {
        const int k = wanted_entry(side);
        if (k > 0) {
            const auto* entries = entries_for(sel.fighter, sel.costume);
            if (entries && (size_t)k <= entries->size()) {
                const Entry& e = (*entries)[(size_t)k - 1];
                out.tree = entry_tree(e);             // (no tree = nothing worn: the Lua returns here too)
                out.entry = e;
                out.k = k;
                return out;
            }
        }
    }
    if (sel.fighter >= 0 && sel.costume >= 0) {
        char key[96];
        std::snprintf(key, sizeof(key), "%d_%d_game_%u", sel.fighter, sel.costume, *id);
        if (auto ov = live::file_tree(std::string(kOverridesPrefix) + key + ".user.2")) { out.tree = ov; out.k = 0; }
    }
    return out;
}

std::string sig_of(const Resolved& r, std::optional<uint32_t> num) {   // want_sig: "k:3", "ov:2" ("" = nothing)
    if (!r.tree) return "";
    if (r.k > 0) return "k:" + std::to_string(r.k);
    return "ov:" + (num ? std::to_string(*num) : std::string("nil"));
}

// which side does this controller belong to? (0 = unknown). Same order as the Lua ctrl_side.
int ctrl_side(MO* ctrl) {
    if (g_p2_ctrls.count((uintptr_t)ctrl)) return 2;                 // wore P2_VIRTUAL_ID: P2 for sure
    const std::string name = live::go_name(ctrl);
    if (name == "FighterVisual_01") return 1;                        // previews
    if (name == "FighterVisual_02") return 2;
    int side = live::side_from_parents(ctrl);                        // intro rig / VS screen
    if (side == 0) side = side_from_battle(ctrl);
    if (side) return side;
    MO* cd = color_data_obj(ctrl);
    if (!cd) return 0;
    const bool m1 = g_side_data[1] == (uintptr_t)cd, m2 = g_side_data[2] == (uintptr_t)cd;
    if (m1 && !m2) return 1;
    if (m2 && !m1) return 2;
    if (m1 && m2) {
        const int w1 = wanted_entry(1), w2 = wanted_entry(2);
        if (w1 > 0 && w2 <= 0) return 1;
        if (w2 > 0 && w1 <= 0) return 2;
        return 1;                                                    // both want one: P2 carries the marker, so P1
    }
    return 0;
}

int should_inject(MO* ctrl, Resolved& r) {           // side to dress, 0 = none (r = what it wears)
    int side = ctrl_side(ctrl);
    if (!side) {                                     // no hint (script loaded mid-flow): fighter/costume name, P1 only
        const auto [f, c] = battle_identity(live::go_name(ctrl));
        if (f >= 0 && f == g_sel[0].fighter && c == g_sel[0].costume) side = 1;
    }
    if (!side) return 0;
    ++g_setups[side];                                // the game is (re)dressing that side
    ++g_n.setups;
    if (side == 1 || side == 2) hold(g_side_ctrl[side], ctrl);
    r = resolve_tree(ctrl, side);
    return r.tree ? side : 0;
}

int inject(MO* ctrl, int side, const Resolved& r) {  // INT_MIN = not patched
    if (side == 1 || side == 2) { g_worn[side].set = true; g_worn[side].tree = r.tree; g_worn[side].entry = r.entry; }
    const auto num = color_num(ctrl);
    MO* data = (r.tree && num) ? live::color_data(ctrl, (int32_t)*num) : nullptr;
    if (!data) {
        cmn::journal("inject P%d: no %s (colour %s)", side, r.tree ? "colour data" : "tree", num ? std::to_string(*num).c_str() : "nil");
        ++g_n.no_data;
        return INT_MIN;
    }
    const int n = live::engine_apply(data, r.tree.get());
    if (n == 0) ++g_n.shared; else ++g_n.patched;
    g_applied[(uintptr_t)ctrl] = sig_of(r, num);
    if ((side == 1 || side == 2) && !g_confirm_at[side] &&                 // a battle model was just dressed
        (g_confirm_last[side] == 0 || cmn::frame() - g_confirm_last[side] > kConfirmQuiet) && battle_ctrl(side) == ctrl)
        g_confirm_at[side] = cmn::frame() + kConfirmAfter + (side == 2 ? 3 : 0);   // one side per frame (03/10: the
                                                                                    // model set up first in a frame lost its colours)
    return n;
}

std::string describe(MO* ctrl) {                      // journal: "<name> <parent<parent...> num=<n>"
    std::string chain;
    MO* t = live::invoke_obj(go_of(ctrl), R.go_tr);
    for (int i = 0; i < 4 && t; ++i) {
        MO* par = live::invoke_obj(t, R.tr_parent);
        if (!par) break;
        MO* pgo = live::invoke_obj(par, R.tr_go);
        if (!chain.empty()) chain += "<";
        chain += pgo ? ll::managed_string_to_utf8(live::invoke_obj(pgo, R.go_name)) : std::string("?");
        t = par;
    }
    const auto num = color_num(ctrl);
    return live::go_name(ctrl) + " <" + chain + "> num=" + (num ? std::to_string(*num) : std::string("nil"));
}

// ---- the game's chip pair for a game colour, and the live data as a tree (squares) --------------------------------
std::optional<chips::Pair> game_chips(int fighter, int costume, uint32_t num) {   // dbg.game_chips
    if (fighter < 0 || costume < 0 || !R.game_chips) return std::nullopt;
    const std::string key = std::to_string(fighter) + "_" + std::to_string(costume) + "_" + std::to_string(num);
    if (auto it = g_game_chip_cache.find(key); it != g_game_chip_cache.end()) return it->second;
    auto r = R.game_chips->invoke(nullptr, std::vector<void*>{ ll::arg_u32((uint32_t)fighter), ll::arg_u32((uint32_t)costume), ll::arg_u32(num) });
    MO* fcd = r.exception_thrown ? nullptr : reinterpret_cast<MO*>(r.ptr);
    uint32_t c0 = 0, c1 = 0;
    if (!fcd || !ll::read_field_u32(fcd, "Color00", c0) || !ll::read_field_u32(fcd, "Color01", c1)) return std::nullopt;
    g_game_chip_cache[key] = { c0, c1 };
    return chips::Pair{ c0, c1 };
}

rsz::NodeRef live_to_tree(MO* obj, int depth) {        // Lua live_to_tree: the live data in the parse_user shape
    if (!obj || depth > 12) return nullptr;
    auto* td = obj->get_type_definition();
    const rsz::ClassSpec* cls = td ? rsz::spec_by_name(td->get_full_name()) : nullptr;
    if (!cls) return nullptr;
    auto node = rsz::make_node(cls);
    for (size_t fi = 0; fi < cls->fields.size(); ++fi) {
        const auto& fd = cls->fields[fi];
        rsz::Value& v = node->fields[fi];
        switch (fd.kind) {
        case rsz::Kind::Obj: v.obj = live_to_tree(ll::read_field_object(obj, fd.name), depth + 1); break;
        case rsz::Kind::Arr: {
            MO* arr = ll::read_field_object(obj, fd.name);
            const uint32_t n = arr ? live::arr_size(arr) : 0;
            for (uint32_t i = 0; i < n; ++i) v.arr.push_back(live_to_tree(live::arr_at(arr, i), depth + 1));
            break;
        }
        case rsz::Kind::Str: {
            MO* s = ll::read_field_object(obj, fd.name);
            v.s = s ? ll::managed_string_to_utf8(s) : std::string("nil");   // tostring(nil)
            break;
        }
        case rsz::Kind::S32: { int32_t x = 0; ll::read_field_i32(obj, fd.name, x); v.u = (uint32_t)x; break; }
        default: {
            // the LAST offset: the `_Value*` runtime cache holds the effective value (02/09)
            const auto& offs = live::offsets_of(cls, fi);
            if (offs.empty()) break;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(obj) + offs.back();
            if (fd.kind == rsz::Kind::Bool) v.b = *p != 0;
            else if (fd.kind == rsz::Kind::U32) std::memcpy(&v.u, p, 4);
            else std::memcpy(&v.f, p, 4);
        }
        }
    }
    return node;
}

// the costume a battle side wears: the colour file's path, else the training select data, else the model name
int battle_costume(int side, int fighter, int from_name, MO* ctrl) {
    if (const int c = path_identity(ctrl).second; c >= 0) return c;   // the controller's colour file
    MO* tm = API::get()->get_managed_singleton("app.training.TrainingManager");
    MO* tdata = tm ? ll::read_field_object(tm, "_tData") : nullptr;
    MO* menu = tdata ? ll::read_field_object(tdata, "SelectMenu") : nullptr;
    MO* pds = menu ? ll::read_field_object(menu, "PlayerDatas") : nullptr;
    MO* pd = (pds && (uint32_t)(side - 1) < live::arr_size(pds)) ? live::arr_at(pds, (uint32_t)(side - 1)) : nullptr;
    int32_t fid = -1, cid = -1;
    if (pd && ll::read_field_i32(pd, "FighterID", fid) && fid == fighter && ll::read_field_i32(pd, "CosutumeID", cid)) return cid;
    return from_name;
}

chips::GameChips game_chips_fn() { return [](int f, int c, uint32_t n) { return game_chips(f, c, n); }; }

// What a battle side really wears when another mod dresses it otherwise (SF6-SlotsLoader's alias: the game shows
// DriveTech, the fighter wears a slot whose colour may sit under DriveTech's first one). The aliasing mod declares it
// to the core (sf6core 0.7.0, ADR-0014); seen 06/10 in Combo Trials: Lily shown as DriveTech colour 1, worn as slot 13
// colour 4, so the Follow squares took DriveTech's palette.
void effective_selection(int side, int fighter, int& costume, uint32_t& num) {
    const SF6CoreAPI* a = cmn::core();
    if (!a || !SF6CORE_HAS(a, sel_get_effective) || fighter < 0 || costume < 0) return;
    int32_t rc = -1, rn = -1;
    if (!a->sel_get_effective(fighter, costume, &rc, &rn) || rc < 0) return;
    static std::string last[3];
    char b[64];
    std::snprintf(b, sizeof b, "%d/%d/%u>%d/%d", fighter, costume, num, rc, rn);
    if ((unsigned)side < 3 && last[side] != b) {
        last[side] = b;
        cmn::journal("squares P%d: fighter %d shown in costume %d colour %u wears costume %d colour %d (core)", side, fighter, costume, num, rc, rn);
    }
    costume = rc;
    if (rn >= 0) num = (uint32_t)rn;
}

std::optional<chips::Pair> squares_locked(int fighter_id, int only_side) {
    static uint64_t cfg_frame = UINT64_MAX;          // chip_slots.json checked once per frame (squares runs per effect)
    if (cfg_frame != cmn::frame()) { cfg_frame = cmn::frame(); g_chip_cfg.load(kChipFile); }
    for (int side = 1; side <= 2; ++side) {
        MO* bc = battle_ctrl(side);                                    // the player's own (exact, even in a mirror)
        if (!(bc && in_scene(bc))) bc = nullptr;                       // PBManager keeps a finished battle's players
        MO* ctrl = bc ? bc : g_side_ctrl[side];                        // else the controller seen by the colour hooks
        if (only_side && side != only_side) ctrl = nullptr;
        if (!ctrl || !in_scene(ctrl)) continue;
        const auto [f, from_name] = battle_identity(live::go_name(ctrl));
        const int c = battle_costume(side, f, from_name, ctrl);
        if (f != fighter_id) continue;
        const Worn& w = g_worn[side];
        auto it = g_applied.find((uintptr_t)ctrl);
        const bool injected = it != g_applied.end() && !it->second.empty();
        rsz::NodeRef tree;
        const Entry* entry = nullptr;
        if (injected && w.set && w.tree) { tree = w.tree; entry = w.entry ? &*w.entry : nullptr; }   // what we apply
        else {                                                         // plain game colour: the game's pair, else live
            const auto num = color_num(ctrl);
            if (!num) return std::nullopt;
            int ce = c; uint32_t ne = *num;
            effective_selection(side, f, ce, ne);                      // the slot worn under DriveTech, if any
            if (auto g = game_chips(f, ce, ne)) return g;
            LiveCache& lc = g_live_cache[side];
            if (lc.set && lc.addr == (uintptr_t)ctrl && lc.num == *num && lc.tree) tree = lc.tree;
            else {
                MO* data = live::color_data(ctrl, (int32_t)*num);
                tree = data ? live_to_tree(data, 0) : nullptr;
                lc = { true, (uintptr_t)ctrl, *num, tree };
            }
        }
        // (the Lua returns entry.chip here when the screens computed it: the same chip_for on the same tree)
        if (!tree) return std::nullopt;
        return chips::chip_for(tree.get(), f, c, entry ? &entry->path : nullptr, color_num(ctrl), g_chip_cfg, game_chips_fn());
    }
    return std::nullopt;
}

// ---- hooks -----------------------------------------------------------------------------------------------------------
// ColorInventory.GetColorList(chara, costume): N ColorInfo{ Name = "RW n" / "MC n", No = BASE_COLOR } appended.
// GetColorNoList / Param.GetEnableColorList derive from this list: hooking them too would append twice.
int pre_get_color_list(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    perf::Scope timed(perf::HookGetColorList);
    std::lock_guard lk(live::mx());
    if (argc < 4 || !active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    g_list_f = (uint32_t)(uintptr_t)argv[2];
    g_list_c = (uint32_t)(uintptr_t)argv[3];
    g_list_known = true;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_get_color_list(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing()) return;
    perf::Scope timed(perf::HookGetColorList);
    std::lock_guard lk(live::mx());
    if (!active() || !ret_val || !g_list_known) return;
    try {
        MO* list = reinterpret_cast<MO*>(*ret_val);
        const auto* entries = g_cat.entries((int)g_list_f, (int)g_list_c);
        const SF6CoreAPI* core = cmn::core();
        if (!list || !entries || entries->empty() || !core) return;
        auto rc = call(list, "get_Count");
        const int32_t n = rc.exception_thrown ? 0 : (int32_t)rc.dword;
        if (n <= 0) return;
        g_extra_first = n;
        g_extra_count = 0;
        // The Edit Character screen asks for the list every frame (OnUpdate -> GetColorId -> GetColorNoList ->
        // GetColorList, 03/10): the ColorInfo objects of a fighter / costume are made once and the same ones are added
        // to every new list (the Lua made new ones per call and never freed them). Each keeps the ref obj_new gave.
        auto& infos = g_infos[std::to_string(g_list_f) + "_" + std::to_string(g_list_c)];
        if (infos.size() != entries->size()) {
            infos.clear();
            for (const Entry& e : *entries) {
                void* ci = core->obj_new(cmn::client(), kInfoType);
                if (ci) {
                    core->obj_set_string(cmn::client(), ci, "<Name>k__BackingField", e.label.c_str());
                    core->obj_set_i64(cmn::client(), ci, "<No>k__BackingField", kBaseColor);
                    ++g_n.infos;
                }
                infos.push_back(ci);
            }
        }
        for (void* ci : infos) {
            if (!ci) continue;
            core->list_add(cmn::client(), list, ci);
            ++g_extra_count;                                         // (counted whatever list_add says, as the Lua)
        }
        ++g_n.lists;
        g_n.entries += (uint64_t)g_extra_count;
    } catch (...) { fault("GetColorList"); }
}

// SetColor: P2_VIRTUAL_ID -> BASE_COLOR on any model; that model is P2
int pre_set_color(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    perf::Scope timed(perf::HookSetColor);
    std::lock_guard lk(live::mx());
    if (argc < 3 || !active() || !argv[1]) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        MO* ctrl = reinterpret_cast<MO*>(argv[1]);
        const uint32_t num = (uint32_t)(uintptr_t)argv[2];
        if (num == kP2VirtualId) {
            argv[2] = reinterpret_cast<void*>((uintptr_t)kBaseColor);
            g_p2_ctrls.insert((uintptr_t)ctrl);
            ++g_n.p2_marks;
            cmn::journal("SetColor(P2 virtual) on %s -> %u", live::go_name(ctrl).c_str(), kBaseColor);
        } else {
            g_p2_ctrls.erase((uintptr_t)ctrl);
            if (num != kBaseColor) g_applied.erase((uintptr_t)ctrl);
        }
    } catch (...) { fault("SetColor"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- mirror rule: keep P2's intended "02" ------------------------------------------------------------------------------
MO* fighter_descs() {                                 // the char select flow's FighterDesc[] (battle descriptor)
    MO* fm = API::get()->get_managed_singleton("app.UIFlowManager");
    MO* hs = fm ? ll::read_field_object(fm, "_Handles") : nullptr;
    if (!hs) return nullptr;
    auto rc = call(hs, "get_Count");
    const int32_t n = rc.exception_thrown ? 0 : (int32_t)rc.dword;
    for (int32_t i = 0; i < n; ++i) {
        MO* h = call_obj(hs, "get_Item(System.Int32)", { ll::arg_i32(i) });
        MO* prm = h ? ll::read_field_object(h, "<Param>k__BackingField") : nullptr;
        auto* td = prm ? prm->get_type_definition() : nullptr;
        if (td && td->get_full_name() == "app.UIFlowUI10501.FlowParam") return ll::read_field_object(prm, "<FighterDescArray>k__BackingField");
    }
    return nullptr;
}

// setTempCosColData(player, fighter, costume, color, cpuLevel) / setTempColor(player, fighter, color): the inlined
// confirm code has already replaced P2's colour by 0 when it equalled P1's.
void mirror_fix(int argc, void** argv, int color_i, bool has_lvl) {
    if (g_fix_guard || argc <= color_i || (has_lvl && argc <= color_i + 1)) return;
    const int32_t player = (int32_t)(intptr_t)argv[2];
    const uint32_t color = (uint32_t)(uintptr_t)argv[color_i];
    if (player == 0 || g_sel[0].rw <= 0 || color != 0) return;
    if (!g_p2_color_id || *g_p2_color_id != (int32_t)kBaseColor) return;
    // same RW entry on both sides (same fighter/costume): keep the game's own duplicate rule (P2 -> 01)
    Sel& s1 = g_sel[0];
    Sel& s2 = g_sel[1];
    if (s2.rw > 0 && s2.rw == s1.rw && s2.fighter == s1.fighter && s2.costume == s1.costume) {
        s2.rw = 0;                                    // the Lua clears path / menus[2].selected and saves (pull)
        ++g_mirror_clears;
        ++g_n.mirror_left;
        cmn::journal("mirror rule: P2 picked P1's entry -> left to the game (01)");
        return;
    }
    argv[color_i] = reinterpret_cast<void*>((uintptr_t)kP2VirtualId);
    FixRequest rq;
    rq.player = player;
    rq.fighter = (uint32_t)(uintptr_t)argv[3];
    rq.costume = (uint32_t)(uintptr_t)argv[4];        // (setTempColor: the colour slot, now P2_VIRTUAL_ID, as the Lua)
    if (has_lvl) rq.lvl = (uint32_t)(uintptr_t)argv[color_i + 1];
    rq.self = reinterpret_cast<MO*>(argv[1]);
    g_fix = rq;
    if (g_param[1]) call(g_param[1], "setColor(System.Int32)", { ll::arg_i32((int32_t)kBaseColor) });   // index = id
    ++g_n.mirror;
    cmn::journal("mirror rule: player %d colour 0 -> stored as %u", player, kP2VirtualId);
}
void mirror_fix_post() {
    if (!g_fix) return;
    const FixRequest rq = *g_fix;
    g_fix.reset();
    if (!rq.self) return;
    g_fix_guard = true;
    try {
        if (rq.lvl) R.tmp_cos_col->invoke(rq.self, std::vector<void*>{ ll::arg_i32(rq.player), ll::arg_i32((int32_t)rq.fighter),
                                                                       ll::arg_i32((int32_t)rq.costume), ll::arg_i32((int32_t)kP2VirtualId), ll::arg_u32(*rq.lvl) });
        else R.tmp_col->invoke(rq.self, std::vector<void*>{ ll::arg_i32(rq.player), ll::arg_i32((int32_t)rq.fighter), ll::arg_i32((int32_t)kP2VirtualId) });
        MO* arr = fighter_descs();                    // battle descriptor, fixed now (the flow ends right after)
        MO* fd = (arr && live::arr_size(arr) >= 2) ? live::arr_at(arr, 1) : nullptr;
        int32_t c = -1;
        if (fd && ll::read_field_i32(fd, "Color", c) && c == 0) { ll::write_field_i32(fd, "Color", (int32_t)kP2VirtualId); g_p2_repush = kP2Repush; }
    } catch (...) { g_fix_guard = false; fault("mirror fix"); }
    g_fix_guard = false;
    if (g_p2_restore && g_p2_restore->self) g_p2_restore->self->release();
    P2Restore pr;
    pr.fighter = rq.fighter; pr.costume = rq.costume; pr.player = rq.player; pr.self = rq.self;
    rq.self->add_ref();
    g_p2_restore = pr;
}
int pre_tmp_cos_col(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    perf::Scope timed(perf::HookTempColor);
    std::lock_guard lk(live::mx());
    if (!active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try { mirror_fix(argc, argv, 5, true); } catch (...) { fault("setTempCosColData"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_tmp_col(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    perf::Scope timed(perf::HookTempColor);
    std::lock_guard lk(live::mx());
    if (!active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try { mirror_fix(argc, argv, 4, false); } catch (...) { fault("setTempColor"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_tmp(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    perf::Scope timed(perf::HookTempColor);
    std::lock_guard lk(live::mx());
    if (!active()) return;
    mirror_fix_post();
}

// ---- per frame -------------------------------------------------------------------------------------------------------
void p2_poll() {                                      // tick(): P2's intended colour, independent of P1's menu
    MO* prm = g_param[1];
    if (!prm) { g_p2_color_id.reset(); g_p2_active = false; return; }
    auto rid = call(prm, "get__cacheColorId");
    if (rid.exception_thrown) g_p2_color_id.reset(); else g_p2_color_id = (int32_t)rid.dword;
    auto ra = call(prm, "IsActive");
    const bool act = !ra.exception_thrown && ra.byte != 0;
    if (act && !g_p2_active) g_p2_opened_at = g_tick;
    g_p2_active = act;
    if (act && g_p2_restore && g_tick - g_p2_opened_at > kP2RestoreAfter) {
        const P2Restore r = *g_p2_restore;
        g_p2_restore.reset();
        auto rf = call(prm, "get_FighterId");
        if (!rf.exception_thrown && (int32_t)rf.dword == (int32_t)r.fighter) {
            if (g_sel[1].rw <= 0) call(prm, "setColor(System.Int32)", { ll::arg_i32((int32_t)kBaseColor) });   // own RW: the menu restores it
            if (r.self && is_managed(r.self)) {
                g_fix_guard = true;
                R.tmp_col->invoke(r.self, std::vector<void*>{ ll::arg_i32(r.player), ll::arg_i32((int32_t)r.fighter), ll::arg_i32((int32_t)kBaseColor) });
                g_fix_guard = false;
            }
            cmn::journal("P2 cursor/store restored to %u", kBaseColor);
        }
        if (r.self) r.self->release();
    }
}

bool in_battle_flow() {                               // battle / battle hub / replay (PBManager is stale outside)
    MO* bfm = API::get()->get_managed_singleton("app.bFlowManager");
    MO* work = bfm ? ll::read_field_object(bfm, "m_flow_work") : nullptr;
    MO* map = work ? ll::read_field_object(work, "_FlowMap") : nullptr;
    int32_t id = -1;
    if (!map || !ll::read_field_i32(map, "_ID", id)) return false;
    return id == 6 || id == 9 || id == 10;
}

// a side's battle controller and its sub-models (same ColorData, same side: a mirror match shares one ColorData)
std::vector<MO*> side_list(int side, MO* ctrl, std::optional<std::vector<MO*>>& subs) {
    std::vector<MO*> list{ ctrl };
    MO* cd = color_data_obj(ctrl);
    if (!cd) return list;
    if (!subs) subs = scene_ctrls();                 // every colour controller of the scene
    for (MO* c : *subs) {
        if (c == ctrl || color_data_obj(c) != cd) continue;
        if (side_from_battle(c) == side) list.push_back(c);
    }
    return list;
}

void confirm_side(int side) {                        // the confirm re-issue (see kConfirmAfter)
    MO* ctrl = battle_ctrl(side);
    if (!is_managed(ctrl)) return;
    const std::string want = sig_of(resolve_tree(ctrl, side), color_num(ctrl));
    if (want.empty()) return;
    std::optional<std::vector<MO*>> subs;
    const auto list = side_list(side, ctrl, subs);
    cmn::journal("confirm P%d: %s wears '%s' -> SetColor on %zu controller(s)", side, live::go_name(ctrl).c_str(), want.c_str(), list.size());
    ++g_n.confirms;
    for (MO* c : list) {                             // a P2-marked controller keeps its marker (P2_VIRTUAL_ID again)
        const std::optional<uint32_t> num = g_p2_ctrls.count((uintptr_t)c) ? std::optional<uint32_t>(kP2VirtualId) : std::nullopt;
        set_color(c, num, "confirm P" + std::to_string(side), c != ctrl);
    }
}

// battle pass: once PBManager knows the players, make sure each side wears what it should; re-issue SetColor if not
uint64_t g_battle_since = 0;                          // frame the current battle flow was first seen (0 = not in one)
constexpr uint64_t kLightBattleChecks = 600;          // light fight: the battle pass runs for the first 10 s only
void battle_pass() {
    if (!in_battle_flow()) { g_battle_since = 0; return; }
    if (!g_battle_since) g_battle_since = cmn::frame();
    // Light fight (cmn::light_fight): the models are dressed by the SetupColor hook when the game sets them up; this
    // safety net only checks the start of the fight, then nothing is recomputed until the next battle flow.
    if (cmn::light_fight() && cmn::frame() - g_battle_since > kLightBattleChecks) return;
    std::optional<std::vector<MO*>> subs;            // every colour controller of the scene (sub-models share a ColorData)
    for (int side = 1; side <= 2; ++side) {
        MO* ctrl = battle_ctrl(side);
        if (!is_managed(ctrl)) continue;
        const std::string want = sig_of(resolve_tree(ctrl, side), color_num(ctrl));
        const auto list = side_list(side, ctrl, subs);
        for (MO* c : list) {
            auto it = g_applied.find((uintptr_t)c);
            const std::string have = it == g_applied.end() ? std::string() : it->second;
            if (want == have) continue;
            cmn::journal("battle pass: P%d %s wears '%s', wants '%s' -> SetColor", side, live::go_name(c).c_str(), have.c_str(), want.c_str());
            ++g_n.battle_fixes;
            g_applied[(uintptr_t)c] = want;          // SetupColor next frame does the injection
            set_color(c, std::nullopt, "battle pass P" + std::to_string(side), c != ctrl);   // PBManager's own: scene check
        }
    }
}

void on_lua_destroyed(lua_State*) {                   // Reset Scripts: the screens' objects are no longer pushed
    std::lock_guard lk(live::mx());
    hold(g_param[0], nullptr);
    hold(g_param[1], nullptr);
    hold(g_fs_text, nullptr);
    g_live[0] = g_live[1] = false;
    if (g_installed) cmn::journal("Lua state destroyed: decisions keep the last selections, screens unknown until the next push");
}

void load_state_file() {                              // state.json as the Lua reads it (the Lua pushes it again at load)
    std::ifstream f(kStateFile, std::ios::binary);
    if (!f) return;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    auto num = [](const nlohmann::json& o, const char* k, int dflt) {
        auto it = o.find(k);
        return (it != o.end() && it->is_number()) ? (int)it->get<double>() : dflt;
    };
    if (j.contains("rw") || j.contains("rw1")) {      // older single-side files
        const int rw = j.contains("rw") ? num(j, "rw", 0) : 1;
        g_sel[0] = { rw, num(j, "fighter", 0), num(j, "costume", 0) };
        return;
    }
    const char* keys[3] = { "p1", "p2", "fs" };
    for (int i = 0; i < 3; ++i) {
        auto it = j.find(keys[i]);
        if (it == j.end() || !it->is_object()) continue;
        g_sel[i] = { num(*it, "rw", 0), num(*it, "fighter", -2), num(*it, "costume", -2) };
    }
}

}  // namespace

// ---- public ------------------------------------------------------------------------------------------------------------
bool ready() {
    std::lock_guard lk(live::mx());
    return g_installed && !g_disabled;
}

bool active() {
    return g_installed && !g_disabled && cmn::owns(cmn::Feature::Decisions) && cmn::owns(cmn::Feature::Engine) && live::ready();
}

void on_setup_pre(void* p) {                          // Lua on_setup_pre after the stale restore (lock held by live)
    MO* ctrl = reinterpret_cast<MO*>(p);
    try {
        const std::string nm = live::go_name(ctrl);
        if (nm == "FighterVisual_01" || nm == "FighterVisual_02") {   // remember each side's data object
            if (MO* cd = color_data_obj(ctrl)) g_side_data[nm == "FighterVisual_01" ? 1 : 2] = (uintptr_t)cd;
        }
        int side = 0, n = INT_MIN;
        bool bs = false;
        if (live::editor_on() && live::side_from_parents(ctrl) == 3) {   // editor: the working copy, any base colour
            side = 3;
            n = live::editor_patch(ctrl);
            g_applied[(uintptr_t)ctrl] = "edit";
            ++g_n.edit;
        } else {
            Resolved r;
            side = should_inject(ctrl, r);
            bs = r.bs;
            if (!side) g_applied.erase((uintptr_t)ctrl);
            else n = inject(ctrl, side, r);
        }
        auto it = g_applied.find((uintptr_t)ctrl);                     // what it wears: k:<entry> / ov:<colour> / edit
        cmn::journal("SetupColor %s -> %s%s%s (rw p1=%d p2=%d%s)", describe(ctrl).c_str(), side ? std::to_string(side).c_str() : "false",
                     (it != g_applied.end() && !it->second.empty()) ? (" " + it->second).c_str() : "",
                     n != INT_MIN ? (" native=" + std::to_string(n)).c_str() : "", g_sel[0].rw, g_sel[1].rw,
                     bs ? (" fs=" + std::to_string(g_sel[2].rw) + " Battle Settings").c_str() : "");
    } catch (...) {
        live::abort_patch();
        fault("SetupColor");
    }
}

std::string applied_sig(void* ctrl) {
    auto it = g_applied.find((uintptr_t)ctrl);
    return it == g_applied.end() ? std::string() : it->second;
}

static int rebuild_locked() {                               // live::mx() held
    // the selections are ranks in the list: a new RW entry shifts the MC ones, so each is followed by its label
    std::string lab[3];
    for (int i = 0; i < 3; ++i)
        if (g_sel[i].rw > 0) if (const auto* l = g_cat.entries(g_sel[i].fighter, g_sel[i].costume))
            if ((size_t)g_sel[i].rw <= l->size()) lab[i] = (*l)[(size_t)g_sel[i].rw - 1].label;
    g_cat.build(kDataDir);
    g_infos.clear();                                  // labels can change (MC n): new ColorInfo objects at the next list
    for (int i = 0; i < 3; ++i)
        if (!lab[i].empty()) g_sel[i].rw = g_cat.label_to_k(g_sel[i].fighter, g_sel[i].costume, lab[i]);
    cmn::journal("catalogue rebuilt: %zu fighter/costume lists", g_cat.all().size());
    return (int)g_cat.all().size();
}

// reframework/color_mods: the worker unpacked archives. The catalogue is rebuilt only where the game is not showing
// or using a colour list: not in a fight / the hub (in_battle_flow), no colour menu live on the character select,
// no Fighter Settings spin (the colour editor opens from it); and only after 2 calm checks in a row (about 1 s).
// Anything else waits (the colours then appear at the next quiet moment, or at the next start).
static void color_mods_tick() {
    if (g_tick % 30 != 7) return;
    if (color_mods::take_changed()) g_cm_pending = true;
    if (!g_cm_pending) return;
    const bool busy = in_battle_flow() || g_live[0] || g_live[1] || (g_fs_text && is_managed(g_fs_text));
    if (busy) { g_cm_calm = 0; return; }
    if (++g_cm_calm < 2) return;
    g_cm_pending = false;
    g_cm_calm = 0;
    cmn::journal("color_mods: new colour mods unpacked, rebuilding the catalogue");
    rebuild_locked();
}

void on_frame() {
    std::lock_guard lk(live::mx());
    if (!active()) return;
    try {
        ++g_tick;
        if (g_tick % 30 == 0) bs_state();             // journals the Battle Settings colours' state when it changes
        color_mods_tick();
        if (g_tick % kTickEvery == 0) p2_poll();
        if (cmn::frame() % kBattlePassEvery == 0) battle_pass();
        for (int side = 1; side <= 2; ++side) {
            if (!g_confirm_at[side] || cmn::frame() < g_confirm_at[side]) continue;
            g_confirm_at[side] = 0;
            g_confirm_last[side] = cmn::frame();
            confirm_side(side);
        }
        if (g_p2_repush > 0 && --g_p2_repush == 0) {
            if (MO* c = find_visual_ctrl(2)) set_color(c, kBaseColor, "P2 repush", true);   // the game pushed 0 on the P2 preview first
        }
    } catch (...) { g_fix_guard = false; fault("frame"); }
}

void push_ui(uint64_t p1_param, bool p1_live, uint64_t p2_param, bool p2_live, uint64_t fs_text) {
    std::lock_guard lk(live::mx());
    auto valid = [](uint64_t a) -> MO* { MO* o = reinterpret_cast<MO*>(a); return (a && is_managed(o)) ? o : nullptr; };
    MO* p1 = valid(p1_param);
    MO* p2 = valid(p2_param);
    MO* fs = valid(fs_text);
    if (p1 != g_param[0] || p2 != g_param[1] || fs != g_fs_text)
        cmn::journal("screens: P1 menu %llx, P2 menu %llx, Fighter Settings spin %llx", (unsigned long long)(uintptr_t)p1, (unsigned long long)(uintptr_t)p2, (unsigned long long)(uintptr_t)fs);
    hold(g_param[0], p1);
    hold(g_param[1], p2);
    hold(g_fs_text, fs);
    g_live[0] = p1_live && p1;
    g_live[1] = p2_live && p2;
}

void push_sels(const int v[9]) {
    std::lock_guard lk(live::mx());
    for (int i = 0; i < 3; ++i) g_sel[i] = { v[i * 3], v[i * 3 + 1], v[i * 3 + 2] };
}

Pull pull() {
    std::lock_guard lk(live::mx());
    Pull p;
    p.list_known = g_list_known; p.list_fighter = g_list_f; p.list_costume = g_list_c;
    p.extra_first = g_extra_first; p.extra_count = g_extra_count;
    for (int i = 0; i < 3; ++i) p.setups[i] = g_setups[i + 1];
    p.mirror_clears = g_mirror_clears;
    return p;
}

int rebuild_catalog() {
    std::lock_guard lk(live::mx());
    return rebuild_locked();
}

std::optional<std::pair<uint32_t, uint32_t>> squares(int fighter_id, int only_side) {
    std::lock_guard lk(live::mx());
    if (!active()) return std::nullopt;
    try {
        ++g_n.squares;
        return squares_locked(fighter_id, only_side);
    } catch (...) { fault("squares"); return std::nullopt; }
}

std::string follow_debug(int fighter_id) {
    std::string out = "decisions: SF6_ColorModNative\n";
    {
        std::lock_guard lk(live::mx());
        try {
            for (int side = 1; side <= 2; ++side) {
                MO* bc = battle_ctrl(side);
                if (!(bc && in_scene(bc))) bc = nullptr;
                MO* ctrl = bc ? bc : g_side_ctrl[side];
                char b[512];
                if (!ctrl) std::snprintf(b, sizeof(b), "P%d: no controller seen", side);
                else if (!in_scene(ctrl)) std::snprintf(b, sizeof(b), "P%d: controller dead or not in the scene", side);
                else {
                    const auto [f, from_name] = battle_identity(live::go_name(ctrl));
                    const int c = battle_costume(side, f, from_name, ctrl);
                    const Worn& w = g_worn[side];
                    auto it = g_applied.find((uintptr_t)ctrl);
                    const std::string worn = !w.set ? "none" : (w.entry ? w.entry->label : (w.tree ? "override/game" : "none"));
                    std::snprintf(b, sizeof(b), "P%d: %s fighter=%s costume=%s applied='%s' worn=%s%s", side, live::go_name(ctrl).c_str(),
                                  f >= 0 ? std::to_string(f).c_str() : "nil", c >= 0 ? std::to_string(c).c_str() : "nil",
                                  it == g_applied.end() ? "" : it->second.c_str(), worn.c_str(), f == fighter_id ? "  <- match" : "");
                }
                out += b;
                out += "\n";
            }
        } catch (...) { fault("follow_debug"); }
    }
    const auto sq = squares(fighter_id, 0);
    char b[96];
    if (sq) std::snprintf(b, sizeof(b), "squares(%d) -> %08X / %08X", fighter_id, sq->first, sq->second);
    else std::snprintf(b, sizeof(b), "squares(%d) -> nil / nil", fighter_id);
    return out + b;
}

int own_side() {
    std::lock_guard lk(live::mx());
    try {
        const Bs& bs = bs_state();
        for (const BsOwn& o : bs.own) if (o.team == 0 || o.team == 1) return o.team + 1;
    } catch (...) {}
    return 1;
}

std::string status() {
    std::lock_guard lk(live::mx());
    char b[640];
    int len = std::snprintf(b, sizeof(b),
                  "decisions installed=%d active=%d faults=%d setups=%llu patched=%llu shared=%llu edit=%llu no_data=%llu lists=%llu entries=%llu infos=%llu "
                  "p2_marks=%llu mirror=%llu mirror_left=%llu battle_fixes=%llu confirms=%llu set_colors=%llu refused=%llu squares=%llu "
                  "sel p1=%d/%d/%d p2=%d/%d/%d fs=%d/%d/%d list=%s%u/%u first=%d count=%d ui=%d%d%d live=%d%d",
                  g_installed ? 1 : 0, active() ? 1 : 0, g_faults, (unsigned long long)g_n.setups, (unsigned long long)g_n.patched,
                  (unsigned long long)g_n.shared, (unsigned long long)g_n.edit, (unsigned long long)g_n.no_data, (unsigned long long)g_n.lists,
                  (unsigned long long)g_n.entries, (unsigned long long)g_n.infos, (unsigned long long)g_n.p2_marks, (unsigned long long)g_n.mirror, (unsigned long long)g_n.mirror_left,
                  (unsigned long long)g_n.battle_fixes, (unsigned long long)g_n.confirms, (unsigned long long)g_n.set_colors, (unsigned long long)g_n.refused, (unsigned long long)g_n.squares,
                  g_sel[0].rw, g_sel[0].fighter, g_sel[0].costume, g_sel[1].rw, g_sel[1].fighter, g_sel[1].costume, g_sel[2].rw, g_sel[2].fighter, g_sel[2].costume,
                  g_list_known ? "" : "?", g_list_f, g_list_c, g_extra_first, g_extra_count,
                  g_param[0] ? 1 : 0, g_param[1] ? 1 : 0, g_fs_text ? 1 : 0, g_live[0] ? 1 : 0, g_live[1] ? 1 : 0);
    if (len < 0) len = 0;
    return std::string(b, (size_t)len < sizeof(b) ? (size_t)len : sizeof(b) - 1) + " bs=" + g_bs_sig;
}

bool install() {
    auto* tdb = API::get()->tdb();
    R.comp_go = tdb->find_method("via.Component", "get_GameObject");
    R.go_tr = tdb->find_method("via.GameObject", "get_Transform");
    R.go_name = tdb->find_method("via.GameObject", "get_Name");
    R.tr_parent = tdb->find_method("via.Transform", "get_Parent");
    R.tr_go = tdb->find_method("via.Transform", "get_GameObject");
    R.scene_current = tdb->find_method("via.SceneManager", "get_CurrentScene");
    R.find_components = tdb->find_method("via.Scene", "findComponents(System.Type)");
    auto* ctrl_td = tdb->find_type("app.PlayerColorController");
    R.ctrl_type = ctrl_td ? ctrl_td->get_runtime_type() : nullptr;
    auto* gb = tdb->find_type("gBattle");
    R.pbm = gb ? gb->find_field("PBManager") : nullptr;
    R.game_chips = tdb->find_method("app.helper.hGUI", "GetFighterCostumeColorData");
    R.set_color = tdb->find_method("app.PlayerColorController", "SetColor");
    R.tmp_cos_col = tdb->find_method("app.GuiManager.GuiEsportsManager", "setTempCosColData");
    R.tmp_col = tdb->find_method("app.GuiManager.GuiEsportsManager", "setTempColor");
    auto* gcl = tdb->find_method("app.InventoryManager.ColorInventory", "GetColorList");
    const SF6CoreAPI* core = cmn::core();
    if (!R.comp_go || !R.go_tr || !R.go_name || !R.tr_parent || !R.tr_go || !R.scene_current || !R.find_components || !R.ctrl_type ||
        !R.pbm || !R.set_color || !R.tmp_cos_col || !R.tmp_col || !gcl || !core ||
        !SF6CORE_HAS(core, obj_new) || !SF6CORE_HAS(core, list_add) || !tdb->find_type(kInfoType)) {
        cmn::log_warn("[ColorModNative] decisions: engine types, methods or core data API missing, feature off (the Lua decides)");
        return false;
    }
    if (!R.game_chips) cmn::journal("decisions: hGUI.GetFighterCostumeColorData missing, squares fall back to the materials");
    {
        std::lock_guard lk(live::mx());
        g_cat.build(kDataDir);
        load_state_file();
    }
    gcl->add_hook(pre_get_color_list, post_get_color_list, false);
    R.set_color->add_hook(pre_set_color, post_nop, false);
    R.tmp_cos_col->add_hook(pre_tmp_cos_col, post_tmp, false);
    R.tmp_col->add_hook(pre_tmp_col, post_tmp, false);
    perf::journal_hooked("ColorInventory.GetColorList (HookGetColorList)", gcl);
    perf::journal_hooked("PlayerColorController.SetColor (HookSetColor)", R.set_color);
    perf::journal_hooked("GuiEsportsManager.setTempCosColData (HookTempColor)", R.tmp_cos_col);
    perf::journal_hooked("GuiEsportsManager.setTempColor (HookTempColor)", R.tmp_col);
    API::get()->param()->functions->on_lua_state_destroyed(on_lua_destroyed);
    g_installed = true;
    cmn::log_info("[ColorModNative] decisions: hooks on GetColorList, SetColor, setTempCosColData, setTempColor; %zu colour lists, state p1=%d p2=%d fs=%d",
                  g_cat.all().size(), g_sel[0].rw, g_sel[1].rw, g_sel[2].rw);
    return true;
}

}  // namespace engine::decide

// ---- for the colour screens (engine/decide_internal.hpp) ---------------------------------------------------------------
namespace engine::decide::internal {
Catalog& catalog() { return g_cat; }
const chips::Config& chip_cfg() { return g_chip_cfg; }
std::vector<MO*> scene_ctrls() { return engine::decide::scene_ctrls(); }
MO* find_visual_ctrl(int side) { return engine::decide::find_visual_ctrl(side); }
std::optional<chips::Pair> game_chips(int fighter, int costume, uint32_t num) { return engine::decide::game_chips(fighter, costume, num); }
MO* battle_ctrl(int side) { return engine::decide::battle_ctrl(side); }
bool set_color(MO* ctrl, std::optional<uint32_t> num, const std::string& why, bool fresh) { return engine::decide::set_color(ctrl, num, why, fresh); }
int ctrl_fighter(MO* ctrl) { return engine::decide::path_identity(ctrl).first; }
}  // namespace engine::decide::internal
