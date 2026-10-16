// SF6 Slots — native plugin: costume slots.
//
// What the costume loader's pak cannot do by itself, done where the game needs it:
//   - boot: the slots' colour records, their select screen data (with the colour squares the
//     loader computed), each character's carrier (carrier.hpp) with its colours and folder, once;
//   - ownership: every slot is owned (AddDlcSaveData), slots of removed mods are not; the game
//     clears DLC ownership at login, so it is given back after the title screen and whenever a
//     screen that lists outfits opens. A slot made from an outfit the player does not own (the
//     game's own list of the character's outfits says so, ownership.hpp) is not owned: the select
//     screen and Battle Settings do not list it;
//   - online alias: Battle Settings keeps the character's carrier in the save (DriveTech, or Outfit 1
//     for a character without DriveTech: carrier.hpp), which is what other players see, and the slot
//     chosen there as an intent; when the game mounts the carrier's folder for a match, the slot's
//     folder is mounted too and its visual manifest copied over the carrier's, for the player's own
//     fighter only (the battle description says which one), never in a mirror on the carrier. A mount is
//     seen through the manifest's constructor (FighterVisualHolder..ctor): via.Folder.activate is a
//     native method the engine calls internally, its hook only saw our own calls (30/09).
// Everything starts from an event (see slots.hpp); nothing is watched in between.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "ownership.hpp"
#include "carrier.hpp"
#include "perf.hpp"
#include "wjournal.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace slots::costumes {
namespace {

const char* kRegistry = "reframework/data/SF6_Costumes_Data/registry.json";
const char* kOutfits  = "reframework/data/SF6_Costumes_Data/outfits.json";
const char* kState    = "reframework/data/SF6_CostumeSlots_data/state.json";

// ---- registry (written by the loader at every launch) ----
struct Slot {
    int fighter = 0, costume_no = 0, record_id = 0;
    int base = -1;                   // the outfit the mod was made for (costume number), -1 unknown
    std::string name, label;         // "Outfit II", the mod's name (log lines)
    bool told_hidden = false;
    bool has_sw = false;
    uint32_t sw[10][2] = {};
    bool sw_ok[10] = {};
};
std::vector<Slot> g_slots;                              // registry order (colour record ids depend on it)
std::map<int, std::map<int, size_t>> g_slot_of;         // fighter -> costume_no -> index
std::vector<int> g_fighters;

bool is_slot(int fid, int cos) {
    auto f = g_slot_of.find(fid);
    return f != g_slot_of.end() && f->second.count(cos);
}

// Whether the outfit each slot is made from is the player's, as the game answered (ownership.hpp)
ownership::Book g_book;

// A slot of the registry the player sees: its outfit is the player's, or not known to be otherwise
bool shown(int fid, int cos) {
    auto f = g_slot_of.find(fid);
    if (f == g_slot_of.end()) return false;
    auto c = f->second.find(cos);
    return c != f->second.end() && g_book.shown(fid, g_slots[c->second].base);
}

// ---- intents (Battle Settings choices), state.json ----
struct Intent { int slot = 0; int color = -1; };       // slot 0: none
std::map<int, Intent> g_intent;

// ---- per character ----
struct Fighter {
    int carrier = -1;                // the alias's outfit (carrier.hpp), -1 not decided or none
    bool left_out = false;           // no carrier, or its folder not found: no alias for this character
    std::set<int> costumes;          // the character's own outfits in the game's costume table
    std::string dst_path;            // the carrier's folder (fighterNNN/esfNNNv04, ...v00)
    Obj* dst_folder = nullptr;
    std::set<int> base_colors; int base_min = 0; bool base_known = false;   // the carrier's colours
    // alias
    Obj* src_folder = nullptr;
    uintptr_t swapped_addr = 0;
    // colour swap in a CCVD: entries csw_a / csw_b hold each other's data (csw_da, csw_db before)
    bool has_csw = false; uintptr_t csw_addr = 0; int csw_a = 0, csw_b = 0;
    uintptr_t csw_da = 0, csw_db = 0;
    uint32_t last_mount = 0;
    std::string last_sig;
};
std::map<int, Fighter> g_f;

// Carriers decided (g_carriers_done), from the game's costume table (g_table_ok) or, when the table
// gives no answer, from the folders of the scene
bool g_carriers_done = false, g_table_ok = false;

int carrier_of(int fid) {
    auto it = g_f.find(fid);
    return it == g_f.end() ? -1 : it->second.carrier;
}
// What the save data holds while a slot is the intent: the carrier (DriveTech until it is decided)
int save_cos_of(int fid) {
    int c = carrier_of(fid);
    return c >= 0 ? c : carrier::kDriveTech;
}
std::string carrier_name(int fid) { return ownership::outfit_label(save_cos_of(fid)); }

// The carrier the save data was given in an earlier session (state.json "carrier"; DriveTech when
// it names none), to move a save that still holds it (carrier::stale)
std::map<int, int> g_saved_carrier;
int saved_carrier_of(int fid) {
    auto it = g_saved_carrier.find(fid);
    return it == g_saved_carrier.end() ? carrier::kDriveTech : it->second;
}

// The carriers in log lines: "DriveTech" while every character has it, else the others named
std::string carriers_text() {
    std::string other;
    for (int fid : g_fighters) {
        int c = carrier_of(fid);
        if (c < 0 || c == carrier::kDriveTech) continue;
        other += (other.empty() ? "" : ", ") + ownership::outfit_label(c) + " for F" + std::to_string(fid);
    }
    return other.empty() ? std::string("DriveTech") : "DriveTech, " + other;
}

// Alias check after an outfit mount, until this frame (0: none). Each mount pushes it further.
uint32_t g_mount_until = 0;
bool g_select_logged = false;

// ---- progress ----
bool g_boot_done = false;
uint32_t g_boot_last = 0;
bool g_dst_done = false, g_bc_done = false, g_colors_done = false, g_ui_done = false;
bool g_owned_once = false;
// sweeps of stale ownership, once per session, a slice per frame
int g_revoke_cursor = -1;           // 5000..8199, -1 not started, -2 done
int g_color_cursor = -1;            // 6000..12999
Obj* g_color_dict = nullptr;
std::string g_color_delete;

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

void load_registry() {
    Json j;
    if (!json_parse(read_file(kRegistry), j)) { logf("costumes: no registry"); return; }
    const Json* pos = j.get("possession");
    if (!pos || pos->kind != Json::Array) return;
    // The outfit each slot is made from: the registry's "slots" (fighter, original_costume_no,
    // new_costume_no), written by every version of the loader; its mod's name from outfits.json
    std::map<std::pair<int, int>, int> base_of;
    if (const Json* ss = j.get("slots"); ss && ss->kind == Json::Array)
        for (auto& e : ss->arr) {
            const Json* f = e.get("fighter"); const Json* o = e.get("original_costume_no"); const Json* n = e.get("new_costume_no");
            if (f && o && n) base_of[{static_cast<int>(f->as_int()), static_cast<int>(n->as_int())}] = static_cast<int>(o->as_int(-1));
        }
    std::map<std::pair<int, int>, std::string> label_of;
    Json oj;
    if (json_parse(read_file(kOutfits), oj))
        if (const Json* os = oj.get("outfits"); os && os->kind == Json::Array)
            for (auto& e : os->arr) {
                const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* l = e.get("label");
                if (f && c && l) label_of[{static_cast<int>(f->as_int()), static_cast<int>(c->as_int())}] = l->as_str();
            }
    for (auto& e : pos->arr) {
        Slot s;
        const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* r = e.get("record_id");
        if (!f || !c) continue;
        s.fighter = static_cast<int>(f->as_int()); s.costume_no = static_cast<int>(c->as_int());
        s.record_id = r ? static_cast<int>(r->as_int()) : 0;
        if (s.fighter <= 0 || s.costume_no <= 0) continue;
        if (const Json* nm = e.get("name")) s.name = nm->as_str();
        if (auto b = base_of.find({s.fighter, s.costume_no}); b != base_of.end()) s.base = b->second;
        if (auto l = label_of.find({s.fighter, s.costume_no}); l != label_of.end()) s.label = l->second;
        if (const Json* sw = e.get("swatches"); sw && sw->kind == Json::Array) {
            for (size_t i = 0; i < sw->arr.size() && i < 10; ++i) {
                auto& p = sw->arr[i];
                if (p.kind == Json::Array && p.arr.size() >= 2 && p.arr[0].kind == Json::Number && p.arr[1].kind == Json::Number) {
                    s.sw[i][0] = static_cast<uint32_t>(p.arr[0].num);
                    s.sw[i][1] = static_cast<uint32_t>(p.arr[1].num);
                    s.sw_ok[i] = true; s.has_sw = true;
                }
            }
        }
        if (!g_slot_of.count(s.fighter)) g_fighters.push_back(s.fighter);
        g_slot_of[s.fighter][s.costume_no] = g_slots.size();
        g_slots.push_back(s);
    }
    logf("costumes: %zu slots, %zu characters", g_slots.size(), g_fighters.size());
}

void load_state() {
    Json j;
    if (!json_parse(read_file(kState), j)) return;
    const Json* fs = j.get("fighters");
    if (!fs || fs->kind != Json::Object) return;
    for (auto& [k, v] : fs->obj) {
        int fid = atoi(k.c_str());
        if (fid <= 0) continue;
        Intent in;
        if (const Json* s = v.get("slot"); s && s->kind == Json::Number) in.slot = static_cast<int>(s->num);
        if (const Json* c = v.get("color"); c && c->kind == Json::Number) in.color = static_cast<int>(c->num);
        if (const Json* c = v.get("carrier"); c && c->kind == Json::Number) g_saved_carrier[fid] = static_cast<int>(c->num);
        if (in.slot && !is_slot(fid, in.slot)) in = Intent{};   // the slot's mod is gone
        g_intent[fid] = in;
    }
}

void save_state() {
    std::string o = "{\n    \"fighters\": {";
    bool first = true;
    for (auto& [fid, in] : g_intent) {
        char b[128], car[32] = "";
        // the carrier the save was given, when it is not DriveTech (what a file without it means)
        if (int sc = saved_carrier_of(fid); sc != carrier::kDriveTech) snprintf(car, sizeof car, ", \"carrier\": %d", sc);
        if (in.slot) {
            if (in.color >= 0) snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": %d, \"color\": %d%s }", first ? "" : ",", fid, in.slot, in.color, car);
            else snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": %d%s }", first ? "" : ",", fid, in.slot, car);
        } else snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": false%s }", first ? "" : ",", fid, car);
        o += b; first = false;
    }
    o += "\n    }\n}\n";
    { int64_t pt = perf::begin(); if (!write_file(kState, o)) logf("costumes: cannot write state.json"); perf::end(perf::StateWrite, pt); }
}

void set_intent_slot(int fid, int slot, const char* why) {
    auto& in = g_intent[fid];
    if (in.slot == slot) return;
    in.slot = slot;
    if (!slot) in.color = -1;
    save_state();
    logf("[F%d] intent slot = %d (%s)", fid, slot, why);
    auto& f = g_f[fid];
    f.src_folder = nullptr; f.swapped_addr = 0;
}
void set_intent_color(int fid, int color, const char* why) {
    auto& in = g_intent[fid];
    if (in.color == color) return;
    in.color = color;
    save_state();
    logf("[F%d] intent colour = %d (%s)", fid, color, why);
}

// ---------------------------------------------------------------------------------------------
// Scene folders
// ---------------------------------------------------------------------------------------------

Method *m_first_folder = nullptr, *m_f_child = nullptr, *m_f_next = nullptr, *m_f_path = nullptr;
Method *m_f_active = nullptr, *m_f_activate = nullptr, *m_get_go = nullptr, *m_go_folder = nullptr;
TD* td_holder = nullptr;

bool cache_folder_methods() {
    if (m_f_path) return true;
    auto* tdb = api().tdb();
    auto* td_scene = tdb->find_type("via.Scene");
    auto* td_folder = tdb->find_type("via.Folder");
    auto* td_c = tdb->find_type("via.Component");
    auto* td_go = tdb->find_type("via.GameObject");
    td_holder = tdb->find_type("app.battle.assets.FighterVisualHolder");
    if (!td_scene || !td_folder || !td_c || !td_go || !td_holder) return false;
    m_first_folder = method_of(td_scene, "get_FirstFolder", 0);
    m_f_child = method_of(td_folder, "get_Child", 0);
    m_f_next = method_of(td_folder, "get_Next", 0);
    m_f_active = method_of(td_folder, "get_Active", 0);
    m_f_activate = method_of(td_folder, "activate", 0);
    m_get_go = method_of(td_c, "get_GameObject", 0);
    m_go_folder = method_of(td_go, "get_FolderSelf", 0);
    m_f_path = method_of(td_folder, "get_Path", 0);
    return m_first_folder && m_f_child && m_f_next && m_f_active && m_f_activate && m_get_go && m_go_folder && m_f_path;
}

std::string fld_path(Obj* f) { return str(call_obj(m_f_path, f)); }

bool ends_with(const std::string& s, const std::string& sfx) {
    return sfx.size() <= s.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}
// A slot folder lives under product/content/esf/... for a base character, under
// ver_02_0300/content/esf/... for a DLC one: compared by the suffix content/esf/fighterNNN/esfNNNvNN
std::string slot_suffix(int fid, int cos) {
    char b[64]; snprintf(b, sizeof b, "content/esf/fighter%03d/esf%03dv%02d", fid, fid, cos);
    return b;
}

template <typename Fn> void walk_folders(Obj* f, int depth, Fn&& fn) {
    if (!f || depth > 8) return;
    if (!fn(f)) return;
    walk_folders(call_obj(m_f_child, f), depth + 1, fn);
    walk_folders(call_obj(m_f_next, f), depth, fn);
}

// Every character's carrier folder, once the carriers are decided. A folder still missing after
// kDstWalks complete walks (one per boot pass: 10 s; every folder was found at the first one so far)
// leaves its character out of the alias, the others go on.
constexpr int kDstWalks = 20;
int g_dst_walks = 0;

void find_dst_all() {
    if (g_dst_done || !g_carriers_done || !cache_folder_methods()) return;
    auto* sc = current_scene();
    auto* root = sc ? call_obj(m_first_folder, sc) : nullptr;
    if (!root) return;
    std::map<std::string, int> want;
    for (int fid : g_fighters) {
        auto& fr = g_f[fid];
        if (fr.left_out || !fr.dst_path.empty()) continue;
        char b[64]; snprintf(b, sizeof b, "fighter%03d/esf%03dv%02d", fid, fid, fr.carrier);
        want[b] = fid;
    }
    if (want.empty()) { g_dst_done = true; return; }
    walk_folders(root, 0, [&](Obj* f) {
        if (want.empty()) return false;
        std::string p = fld_path(f);
        for (auto it = want.begin(); it != want.end(); ++it) {
            if (ends_with(p, it->first)) {
                auto& fr = g_f[it->second];
                fr.dst_path = p; fr.dst_folder = f;
                want.erase(it);
                break;
            }
        }
        return true;
    });
    if (!want.empty() && ++g_dst_walks >= kDstWalks) {
        for (auto& [sfx, fid] : want) {
            g_f[fid].left_out = true;
            logf("[F%d] %s folder %s not found: left out of the alias", fid, carrier_name(fid).c_str(), sfx.c_str());
        }
        want.clear();
    }
    g_dst_done = want.empty();
    if (g_dst_done) logf("costumes: %s folders found", carriers_text().c_str());
}

Obj* find_folder(const std::string& suffix) {
    if (!cache_folder_methods()) return nullptr;
    auto* sc = current_scene();
    auto* root = sc ? call_obj(m_first_folder, sc) : nullptr;
    Obj* found = nullptr;
    walk_folders(root, 0, [&](Obj* f) {
        if (found) return false;
        if (ends_with(fld_path(f), suffix)) { found = f; return false; }
        return true;
    });
    return found;
}

// ---------------------------------------------------------------------------------------------
// Save data: MatchingFighterSetting entries of the characters that have slots
// ---------------------------------------------------------------------------------------------

std::map<int, Obj*> save_entries() {
    std::map<int, Obj*> out;
    auto* mgr = api().get_managed_singleton("app.SystemSaveManager");
    auto* data = mgr ? get_obj(mgr, "Data") : nullptr;
    auto* list = data ? get_obj(data, "MatchingFighterSetting") : nullptr;
    int n = list_count(list);
    for (int i = 0; i < n; ++i) {
        auto* e = list_item(list, i);
        int32_t fid = 0;
        if (e && get_i32(e, "FighterId", fid) && g_slot_of.count(fid)) out[fid] = e;
    }
    return out;
}
int get_cos(Obj* e) { int32_t v = -1; get_i32(e, "MatchingFighterCostume", v); return v; }
int get_col(Obj* e) { int32_t v = -1; get_i32(e, "MatchingFighterColor", v); return v; }
void set_cos(Obj* e, int v, int fid, const char* why) {
    int old_v = get_cos(e);
    if (old_v == v) return;
    set_i32(e, "MatchingFighterCostume", v);
    if (wjournal::on()) wjournal::log("SAVE obj=%p MatchingFighterCostume old=%d new=%d F%d (%s)", (void*)e, old_v, v, fid, why);
    logf("[F%d] save MatchingFighterCostume -> %d (%s)", fid, v, why);
}
void set_col(Obj* e, int v, int fid, const char* why) {
    int old_v = get_col(e);
    if (old_v == v) return;
    set_i32(e, "MatchingFighterColor", v);
    if (wjournal::on()) wjournal::log("SAVE obj=%p MatchingFighterColor old=%d new=%d F%d (%s)", (void*)e, old_v, v, fid, why);
    logf("[F%d] save MatchingFighterColor -> %d (%s)", fid, v, why);
}

// Battle Settings opened: the menu shows the slot where the save has the carrier (a hidden slot is not
// in its list: the menu shows the carrier)
void on_menu_open() {
    logf("costumes: Battle Settings opened");
    for (auto& [fid, e] : save_entries()) {
        auto& in = g_intent[fid];
        if (!in.slot || !shown(fid, in.slot)) continue;
        if (get_cos(e) == save_cos_of(fid)) set_cos(e, in.slot, fid, "so the menu shows the slot");
        if (in.color >= 0 && get_col(e) != in.color) set_col(e, in.color, fid, "so the menu shows the colour");
    }
}

// Battle Settings closed: a slot chosen there becomes the intent, the save gets the carrier
void on_menu_close() {
    logf("costumes: Battle Settings closed");
    for (auto& [fid, e] : save_entries()) {
        int v = get_cos(e);
        // The menu showed the carrier in place of a hidden slot: left so, the choice is kept for the
        // day the outfit is the player's again (until then the alias leaves it out)
        if (v == save_cos_of(fid) && g_intent[fid].slot && !shown(fid, g_intent[fid].slot)) continue;
        set_intent_slot(fid, is_slot(fid, v) ? v : 0, "chosen in the menu");
        if (is_slot(fid, v)) {
            set_cos(e, save_cos_of(fid), fid, "what the game and the network must see");
            int c = get_col(e);
            set_intent_color(fid, c, "chosen in the menu");
            auto& fr = g_f[fid];
            if (fr.base_known && !fr.base_colors.count(c))
                set_col(e, fr.base_min, fid, ("a colour " + carrier_name(fid) + " has").c_str());
        }
    }
}

// Outside the menu the save never keeps a slot number, nor a former carrier
void safety_net() {
    bool carriers_saved = false;
    for (auto& [fid, e] : save_entries()) {
        int v = get_cos(e);
        auto& fr = g_f[fid];
        auto& in = g_intent[fid];
        // The carrier changed since the save was given one (a DriveTech added by a game update, or
        // DriveTech written for a character that has none by 1.11.1 and before): the save gets the
        // carrier, in the colour the menu would have given it
        if (g_table_ok) {
            const int was = saved_carrier_of(fid);
            if (carrier::stale(was, fr.carrier, v, in.slot != 0, fr.costumes.count(v) != 0)) {
                std::string why = "carrier changed: " + ownership::outfit_label(was) + " -> " + carrier_name(fid);
                set_cos(e, fr.carrier, fid, why.c_str());
                v = fr.carrier;
                if (fr.base_known) {
                    int c = in.color >= 0 ? in.color : get_col(e);
                    if (!fr.base_colors.count(c)) c = fr.base_min;
                    set_col(e, c, fid, why.c_str());
                }
            }
            if (fr.carrier >= 0 && was != fr.carrier) { g_saved_carrier[fid] = fr.carrier; carriers_saved = true; }
        }
        if (is_slot(fid, v)) {
            set_intent_slot(fid, v, "slot found in the save outside the menu");
            set_cos(e, save_cos_of(fid), fid, "safety net");
        }
        if (g_intent[fid].slot && fr.base_known) {
            int c = get_col(e);
            if (!fr.base_colors.count(c)) {
                set_intent_color(fid, c, "colour found in the save outside the menu");
                set_col(e, fr.base_min, fid, "safety net");
            }
        }
    }
    if (carriers_saved) save_state();
}

// ---------------------------------------------------------------------------------------------
// Ownership
// ---------------------------------------------------------------------------------------------

constexpr int kStaticMin = 5000, kStaticMax = 8199;    // the loader's static records
constexpr int kColorMin = 6000, kColorMax = 12999;     // colour ids ever used (slots: 9000+)

Obj* costume_inventory() {
    auto* im = api().get_managed_singleton("app.InventoryManager");
    return im ? get_obj(im, "_Costume") : nullptr;
}

// The description of the battle being loaded or played (bFlowManager -> flow map -> m_desc), if any
Obj* battle_desc() {
    auto* fm = api().get_managed_singleton("app.bFlowManager");
    auto* fw = fm ? get_obj(fm, "m_flow_work") : nullptr;
    auto* map = fw ? get_obj(fw, "_FlowMap") : nullptr;
    auto* desc = map ? get_obj(map, "m_desc") : nullptr;
    return alive(desc) ? desc : nullptr;
}

// An online battle is loaded or played. REFramework stops Lua scripts then, the hooks of an
// unlock-all script with them, so the game answers with what the player really owns: ownership is
// not asked then. (The description of the last battle stays after it: until the next offline
// battle, the answers already given are kept.)
bool online_now() {
    bool net = false;
    auto* desc = battle_desc();
    return desc && get_bool(desc, "IsNetwork", net) && net;
}

// ---- ownership probe: read-only, off by default ----
// With reframework/data/SF6_CostumeSlots_data/ownership_probe.txt present, each ownership pass
// (boot, title screen left, character select, Battle Settings) logs what the game's candidate
// methods answer for the outfits of the characters that have slots, 8 times a session at most:
// the list of the select screen (CostumeInventory.GetCostumeNoList), Contains(chara, no), and per
// record ExistsSaveData (bought), IsDefaultItem, IsValidItem and DlcManager.IsValidDlcItem.
const char* kProbe = "reframework/data/SF6_CostumeSlots_data/ownership_probe.txt";
int g_probe_left = -1;              // -1: switch file not looked for yet
bool g_probe_boot = false;          // one pass logged before the title screen (boot passes repeat)
bool g_after_title = false;         // the title screen was left (login) or a screen listing outfits opened

std::vector<uint32_t> listed_outfits(Method* m_list, Obj* ci, int fid, bool* ok) {
    std::vector<uint32_t> out;
    auto r = call(m_list, ci, { arg_i32(fid) });
    *ok = !r.exception_thrown && r.ptr;
    if (!*ok) return out;
    auto* lst = reinterpret_cast<Obj*>(r.ptr);
    int n = list_count(lst);
    for (int i = 0; i < n; ++i)
        if (auto el = list_elem(lst, i); el.ok && el.vt) out.push_back(el.ret.dword);
    return out;
}

void probe(Obj* ci, const char* why) {
    if (g_probe_left < 0) g_probe_left = GetFileAttributesA(kProbe) != INVALID_FILE_ATTRIBUTES ? 8 : 0;
    if (g_probe_left == 0 || (!g_after_title && g_probe_boot)) return;
    g_probe_boot = true;
    --g_probe_left;
    TD* td = ci->get_type_definition();
    Method* m_rec = method_of(td, "GetMasterDataRecord", 1);
    Method* m_list = method_of(td, "GetCostumeNoList", 1);
    Method* m_has = method_of(td, "Contains", 2);
    auto* dlc = api().get_managed_singleton("app.DlcManager");
    Method* m_dlc = dlc ? method_of(dlc->get_type_definition(), "IsValidDlcItem", 2) : nullptr;
    bool av_ok = false;
    bool av = call_bool(ci, "get_Available", {}, &av_ok);
    auto cat = call(ci, "get_Category");
    int category = cat.exception_thrown ? -1 : static_cast<int>(cat.dword);
    logf("probe (%s): online %d, inventory available %s, DlcManager %s, costume category %d, methods list %d contains %d record %d dlc %d",
         why, online_now() ? 1 : 0, av_ok ? (av ? "1" : "0") : "?", dlc ? "found" : "missing", category,
         m_list ? 1 : 0, m_has ? 1 : 0, m_rec ? 1 : 0, m_dlc ? 1 : 0);
    if (!m_rec) return;
    std::map<int, std::map<int, int>> recs;          // fighter -> costume -> vanilla record id (the loader's are 5000+)
    for (int rid = 1; rid < 1000; ++rid) {
        int32_t fid = 0, cn = -1;
        auto* rec = call_obj(m_rec, ci, { arg_i32(rid) });
        if (rec && get_i32(rec, "fighterId", fid) && get_i32(rec, "costumeNo", cn) && g_slot_of.count(fid) && cn >= 0)
            recs[fid][cn] = rid;
    }
    auto yn = [](bool ok, bool v) { return ok ? (v ? "1" : "0") : "?"; };
    for (int fid : g_fighters) {
        std::string line = "probe F" + std::to_string(fid) + " list=[";
        bool lok = false;
        auto listed = m_list ? listed_outfits(m_list, ci, fid, &lok) : std::vector<uint32_t>{};
        for (size_t i = 0; i < listed.size(); ++i) line += (i ? "," : "") + std::to_string(listed[i]);
        line += lok ? "]" : "?]";
        for (auto& [cn, rid] : recs[fid]) {
            bool k1 = false, k2 = false, k3 = false, k4 = false, k5 = false;
            bool has = false;
            if (m_has) { auto r = call(m_has, ci, { arg_i32(fid), arg_i32(cn) }); k1 = !r.exception_thrown; has = k1 && r.byte != 0; }
            bool save = call_bool(ci, "ExistsSaveData", { arg_i32(rid) }, &k2);
            bool def = call_bool(ci, "IsDefaultItem", { arg_i32(rid) }, &k3);
            bool valid = call_bool(ci, "IsValidItem", { arg_i32(rid) }, &k4);
            bool dv = false;
            if (m_dlc && category >= 0) { auto r = call(m_dlc, dlc, { arg_i32(category), arg_i32(rid) }); k5 = !r.exception_thrown; dv = k5 && r.byte != 0; }
            char b[128];
            snprintf(b, sizeof b, " | c%d r%d: has %s save %s default %s valid %s dlc %s", cn, rid,
                     yn(k1, has), yn(k2, save), yn(k3, def), yn(k4, valid), yn(k5, dv));
            line += b;
        }
        logf("%s", line.c_str());
    }
}

// The game's answer for the outfits the slots are made from: the costume numbers it lists for
// each character (CostumeInventory.GetCostumeNoList). Asked offline only, for the outfits not
// known to be owned yet; a slot whose outfit the game does not list once the title screen is left
// (purchases and DLC known) is hidden, with one log line.
bool g_check_off = false;
bool g_online_logged = false;

void check_outfits(Obj* ci, const char* why) {
    if (g_check_off) return;
    if (online_now()) {
        if (!g_online_logged) { logf("costumes: online battle, outfit ownership not asked (the answers already given are kept)"); g_online_logged = true; }
        return;
    }
    g_online_logged = false;
    probe(ci, why);
    static Method* m_list = nullptr;
    if (!m_list) m_list = method_of(ci->get_type_definition(), "GetCostumeNoList", 1);
    if (!m_list) {
        g_check_off = true;
        logf("costumes: CostumeInventory.GetCostumeNoList not found, every slot shown whatever outfit it is made from");
        return;
    }
    for (int fid : g_fighters) {
        bool ask = false;
        for (auto& [cos, idx] : g_slot_of[fid]) ask |= g_book.open(fid, g_slots[idx].base);
        if (!ask) continue;
        bool ok = false;
        auto listed = listed_outfits(m_list, ci, fid, &ok);
        if (!ok) continue;
        for (auto& [cos, idx] : g_slot_of[fid]) g_book.answer(fid, g_slots[idx].base, listed, g_after_title);
    }
    for (auto& s : g_slots) {
        bool hidden = !g_book.shown(s.fighter, s.base);
        if (hidden == s.told_hidden) continue;
        s.told_hidden = hidden;
        std::string outfit = ownership::outfit_label(s.base);
        const char* mod = s.label.empty() ? "?" : s.label.c_str();
        if (hidden) logf("[F%d] slot v%02d (%s, %s) hidden: made for %s, which the player does not own", s.fighter, s.costume_no, s.name.c_str(), mod, outfit.c_str());
        else logf("[F%d] slot v%02d (%s, %s) shown: the player owns %s now", s.fighter, s.costume_no, s.name.c_str(), mod, outfit.c_str());
    }
}

// true when the save answered (it is loaded)
bool give_ownership(const char* why) {
    auto* ci = costume_inventory();
    if (!ci || g_slots.empty()) return false;
    check_outfits(ci, why);
    int added = 0, taken = 0; bool any = false;
    for (auto& s : g_slots) {
        bool ok = false;
        bool has = call_bool(ci, "ExistsSaveData", { arg_i32(s.record_id) }, &ok);
        if (!ok) continue;
        any = true;
        bool show = g_book.shown(s.fighter, s.base);
        if (show && !has) {
            call(ci, "AddDlcSaveData", { arg_i32(s.record_id) });
            if (wjournal::on()) wjournal::log("OWN_ADD obj=%p AddDlcSaveData record=%d", (void*)ci, s.record_id);
            ++added;
        } else if (!show && has) {
            // owned in an earlier session, or before the game answered
            call(ci, "DeleteDlcSaveData", { arg_i32(s.record_id) });
            if (wjournal::on()) wjournal::log("OWN_DEL obj=%p DeleteDlcSaveData record=%d (hidden)", (void*)ci, s.record_id);
            ++taken;
        }
    }
    if (taken) logf("costumes: ownership taken back from %d hidden slots", taken);
    if (added || !g_owned_once) logf("costumes: ownership given to %d of %zu slots", added, g_slots.size());
    if (any && !g_owned_once) { g_owned_once = true; if (g_revoke_cursor == -1) g_revoke_cursor = kStaticMin; }
    return any;
}

// A static record owned but not in the registry: a slot whose mod was removed. DLC ownership is
// kept in the save, so it is taken back (a slice of ids per frame).
void revoke_slice() {
    if (g_revoke_cursor < kStaticMin) return;
    auto* ci = costume_inventory();
    if (!ci) return;
    static std::unordered_set<int> wanted;
    if (wanted.empty()) for (auto& s : g_slots) wanted.insert(s.record_id);
    int stop = std::min(g_revoke_cursor + 199, kStaticMax), revoked = 0;
    for (int id = g_revoke_cursor; id <= stop; ++id) {
        if (wanted.count(id)) continue;
        bool ok = false;
        if (call_bool(ci, "ExistsSaveData", { arg_i32(id) }, &ok) && ok) {
            call(ci, "DeleteDlcSaveData", { arg_i32(id) });
            if (wjournal::on()) wjournal::log("OWN_DEL obj=%p DeleteDlcSaveData record=%d", (void*)ci, id);
            ++revoked;
        }
    }
    if (revoked) logf("costumes: %d stale slot ownerships taken back", revoked);
    g_revoke_cursor = stop >= kStaticMax ? -2 : stop + 1;
}

// A colour owned but without a record: slots removed, old ids. Taken back, a slice per frame.
void color_cleanup_slice() {
    if (g_color_cursor < kColorMin || !g_color_dict || !alive(g_color_dict)) return;
    auto* im = api().get_managed_singleton("app.InventoryManager");
    auto* ci = im ? get_obj(im, "_Color") : nullptr;
    if (!ci) return;
    if (g_color_delete.empty()) {
        for (const char* n : { "DeleteSaveData", "RemoveSaveData", "DeleteDlcSaveData" })
            if (method_of(ci->get_type_definition(), n, 1)) { g_color_delete = n; break; }
        if (g_color_delete.empty()) { g_color_cursor = -2; return; }
    }
    int stop = std::min(g_color_cursor + 249, kColorMax), revoked = 0;
    for (int id = g_color_cursor; id <= stop; ++id) {
        bool ok1 = false, ok2 = false;
        bool has_rec = call_bool(g_color_dict, "ContainsKey", { arg_i32(id) }, &ok1);
        bool owned = call_bool(ci, "ExistsSaveData", { arg_i32(id) }, &ok2);
        if (ok1 && ok2 && owned && !has_rec) {
            call(ci, g_color_delete.c_str(), { arg_i32(id) });
            if (wjournal::on()) wjournal::log("COL_DEL obj=%p %s id=%d", (void*)ci, g_color_delete.c_str(), id);
            ++revoked;
        }
    }
    if (revoked) logf("costumes: %d orphan colour ownerships taken back", revoked);
    g_color_cursor = stop >= kColorMax ? -2 : stop + 1;
}

// ---------------------------------------------------------------------------------------------
// Boot: each character's carrier and its colours, the slots' colour records, their select screen data
// ---------------------------------------------------------------------------------------------

// Boot passes without an answer from the costume table before the carriers are taken from the
// scene's folders instead (30 s; the table answered at the first pass so far)
constexpr int kTablePasses = 60;
int g_table_passes = 0;

// The carriers from the folders the scene has (fighterNNN/esfNNNv04, else ...v00), one walk: only
// when the costume table gives no answer (a game update). false while the scene has no folder.
bool carriers_from_folders() {
    if (!cache_folder_methods()) return false;
    auto* sc = current_scene();
    auto* root = sc ? call_obj(m_first_folder, sc) : nullptr;
    if (!root) return false;
    std::map<std::string, std::pair<int, int>> want;   // folder suffix -> fighter, costume
    for (int fid : g_fighters)
        for (int c : { carrier::kDriveTech, carrier::kOutfit1 }) {
            char b[64]; snprintf(b, sizeof b, "fighter%03d/esf%03dv%02d", fid, fid, c);
            want[b] = { fid, c };
        }
    std::map<int, std::set<int>> has;
    walk_folders(root, 0, [&](Obj* f) {
        std::string p = fld_path(f);
        for (auto& [sfx, fc] : want) if (ends_with(p, sfx)) { has[fc.first].insert(fc.second); break; }
        return true;
    });
    for (int fid : g_fighters) g_f[fid].carrier = carrier::choose(has[fid]);
    logf("costumes: the game's costume table gives no answer, carriers taken from the scene's folders");
    return true;
}

// The carriers once (carrier.hpp), from the game's costume table: CostumeInventory.GetMasterDataRecord,
// records 1.. (the loader's start at 5000) with fighterId and costumeNo, read until 100 ids in a row
// have none (the game's ids run 1-148 on 13/10, no gap). Then the carrier's colours
// (TableDataManager.GetFighterCostumeColors of its record), until every character's are known.
void read_carriers() {
    if (g_bc_done) return;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    auto* ci = costume_inventory();
    Method* m_rec = ci ? method_of(ci->get_type_definition(), "GetMasterDataRecord", 1) : nullptr;
    Method* m_cols = tm ? method_of(tm->get_type_definition(), "GetFighterCostumeColors", 2) : nullptr;
    std::map<int, std::map<int, int>> rid_of;        // fighter -> costume -> record id
    int hits = 0;
    if (m_rec)
        for (int rid = 1, last = 0; rid < kStaticMin && rid - last <= 100; ++rid) {
            auto* rec = call_obj(m_rec, ci, { arg_i32(rid) });
            int32_t fid = 0, cn = -1;
            if (!rec || !get_i32(rec, "fighterId", fid) || !get_i32(rec, "costumeNo", cn)) continue;
            last = rid; ++hits;
            if (g_slot_of.count(fid) && cn >= 0) rid_of[fid].emplace(cn, rid);   // the first record of an outfit
        }
    if (!g_carriers_done) {
        const char* src = "the game's costume table";
        if (hits) {
            g_table_ok = true;
            for (int fid : g_fighters) {
                auto& fr = g_f[fid];
                for (auto& [cn, rid] : rid_of[fid]) fr.costumes.insert(cn);
                fr.carrier = carrier::choose(fr.costumes);
            }
        } else if (++g_table_passes < kTablePasses || !carriers_from_folders()) return;
        else src = "the scene's folders";
        for (int fid : g_fighters) {
            auto& fr = g_f[fid];
            if (fr.carrier < 0) {
                fr.left_out = true;
                logf("[F%d] neither DriveTech nor Outfit 1 in %s: left out of the alias", fid, src);
            } else if (fr.carrier != carrier::kDriveTech)
                logf("[F%d] carrier: %s (no DriveTech in %s)", fid, carrier_name(fid).c_str(), src);
        }
        g_carriers_done = true;
    }
    if (!m_cols) return;
    for (int fid : g_fighters) {
        auto& fr = g_f[fid];
        if (fr.carrier < 0 || fr.base_known) continue;
        auto r = rid_of[fid].find(fr.carrier);
        if (r == rid_of[fid].end()) continue;
        auto* cls = call_obj(m_cols, tm, { arg_i32(r->second), arg_bool(false) });
        if (!cls) continue;
        int n = list_count(cls), mn = 999;
        for (int j = 0; j < n; ++j) {
            int32_t c = 0;
            if (auto* cr = list_item(cls, j); cr && get_i32(cr, "colorNo", c)) { fr.base_colors.insert(c); mn = std::min(mn, static_cast<int>(c)); }
        }
        fr.base_min = mn < 999 ? mn : 0;
        fr.base_known = true;
    }
    g_bc_done = std::all_of(g_fighters.begin(), g_fighters.end(), [](int fid) { return g_f[fid].carrier < 0 || g_f[fid].base_known; });
    if (g_bc_done) logf("costumes: %s colours read", carriers_text().c_str());
}

// Colour records of the slots, cloned from the game's own (a record made from scratch is refused
// by IsValidItem): 10 per slot, ids 9000 + slot index * 10 + colour, in
// TableDataManager.FighterCostumeColorUserDataDict, owned in the colour inventory.
void insert_colors() {
    if (g_colors_done) return;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    if (!tm) return;
    Obj* dict = nullptr;
    for (auto* t = tm->get_type_definition(); t && !dict; t = t->get_parent_type())
        for (auto* f : t->get_fields())
            if (f && f->get_name() && strstr(f->get_name(), "FighterCostumeColorUserDataDict")) {
                void* p = f->get_data_raw(tm, false);
                dict = p ? *reinterpret_cast<Obj**>(p) : nullptr;
                break;
            }
    if (!dict) return;
    Obj* sample = call_obj(dict, "get_Item", { arg_i32(1) });
    if (!sample) return;
    std::map<int, Obj*> models;
    if (auto* lst = call_obj(tm, "GetFighterCostumeColors", { arg_i32(1), arg_bool(false) })) {
        int n = list_count(lst);
        for (int i = 0; i < n; ++i) {
            int32_t c = 0;
            if (auto* m = list_item(lst, i); m && get_i32(m, "colorNo", c)) models[c] = m;
        }
    }
    auto* im = api().get_managed_singleton("app.InventoryManager");
    auto* col_inv = im ? get_obj(im, "_Color") : nullptr;
    int added = 0, present = 0, owned = 0;
    for (size_t i = 0; i < g_slots.size(); ++i) {
        for (int c = 0; c < 10; ++c) {
            int id = 9000 + static_cast<int>(i) * 10 + c;
            bool ok = false;
            if (ui::dict_contains(dict, id, &ok) && ok) ++present;
            else if (auto mi = models.find(c); mi != models.end()) {
                Obj* rec = ui::clone(mi->second);
                Obj* holder = rec ? ui::clone(sample) : nullptr;
                if (rec && holder) {
                    ui::set_i64(rec, "id", id); ui::set_i64(rec, "ManageId", id);
                    ui::set_i64(rec, "fighterCostumeId", g_slots[i].record_id);
                    ui::set_i64(rec, "isDefault", 1); ui::set_i64(rec, "colorNo", c); ui::set_i64(rec, "isBundleCostume", 0);
                    ui::set_i64(holder, "RefCount", 1);
                    ui::set_ref(holder, "RecordData", rec);
                    ui::dict_add(dict, id, holder);
                    if (wjournal::on()) wjournal::log("BOOT_COLOR dict_add dict=%p id=%d holder=%p rec=%p costId=%d color=%d",
                        (void*)dict, id, (void*)holder, (void*)rec, g_slots[i].record_id, c);
                    ++added;
                }
            }
            if (col_inv) {
                bool ok2 = false;
                bool has = call_bool(col_inv, "ExistsSaveData", { arg_i32(id) }, &ok2);
                if (ok2 && !has && call_bool(col_inv, "AddSaveData", { arg_i32(id) })) {
                    if (wjournal::on()) wjournal::log("BOOT_COL_OWN obj=%p AddSaveData id=%d", (void*)col_inv, id);
                    ++owned;
                }
            }
        }
    }
    g_color_dict = dict;
    g_colors_done = true;
    g_color_cursor = kColorMin;
    logf("costumes: colour records added %d, present %d, owned %d", added, present, owned);
}

// The loader's colour squares, on a list of the clone's own (GetRange copies the list, every colour
// entry is cloned): the original outfit's squares stay as they are.
// Ported to slots::ui (SL-08): clone, set_color, list_set, set_ref.
int apply_swatches(Obj* clone, const Slot& s) {
    Obj* cds = get_obj(clone, "ColorDatas");
    int n = list_count(cds);
    if (!cds || n <= 0) return 0;
    Obj* copy = ui::list_range(cds, 0, n);
    if (!copy) return 0;
    int done = 0;
    for (int i = 0; i < n; ++i) {
        Obj* e = list_item(copy, i);
        int32_t idx = -1;
        if (!e || !get_i32(e, "ColorIndex", idx) || idx < 0 || idx > 9 || !s.sw_ok[idx]) continue;
        Obj* ne = ui::clone(e);
        if (!ne) continue;
        if (ui::set_color(ne, "Color00", s.sw[idx][0]) && ui::set_color(ne, "Color01", s.sw[idx][1])) {
            ui::list_set(copy, i, ne);
            ++done;
        }
    }
    ui::set_ref(clone, "ColorDatas", copy);
    return done;
}

// Without a SelectFighterUIData entry for a slot, GetFighterCostumeColorData returns nothing and
// the colour spin of the select screen locks: each slot gets a clone of Outfit 1's entry
void insert_select_ui() {
    if (g_ui_done) return;
    auto* gm = api().get_managed_singleton("app.GuiManager");
    auto* mgr = gm ? get_obj(gm, "<SelectFighterUIData>k__BackingField") : nullptr;
    auto* ml = mgr ? get_obj(mgr, "ManagedList") : nullptr;
    if (!ml || list_count(ml) == 0) return;
    int n = 0, colored = 0;
    for (int fid : g_fighters) {
        auto* lst = call_obj(mgr, "GetData_Fighter", { arg_i32(fid) });
        auto* cd = lst ? get_obj(lst, "CosDatas") : nullptr;
        auto* base = call_obj(mgr, "GetData_Costume", { arg_i32(fid), arg_i32(0) });
        if (!cd || !base) continue;
        for (auto& [cos, idx] : g_slot_of[fid]) {
            if (call_obj(mgr, "GetData_Costume", { arg_i32(fid), arg_i32(cos) })) continue;
            Obj* cl = ui::clone(base);
            if (!cl) continue;
            ui::set_i64(cl, "CostumeId", cos);
            if (g_slots[idx].has_sw) colored += apply_swatches(cl, g_slots[idx]);
            ui::list_add(cd, cl);
            if (wjournal::on()) wjournal::log("BOOT_UI list_add list=%p clone=%p F%d v%d", (void*)cd, (void*)cl, fid, cos);
            ++n;
        }
    }
    g_ui_done = true;
    logf("costumes: select screen data added for %d slots, %d colour squares", n, colored);
}

// ---------------------------------------------------------------------------------------------
// Online alias: the slot's folder over its carrier's (DriveTech, or Outfit 1: carrier.hpp)
// ---------------------------------------------------------------------------------------------

// The character select screen mounts and unmounts outfits itself: touching folders while it runs
// hung the game (22/09). Checked when a mount is handled, not watched.
bool select_screen_active() {
    auto* fm = api().get_managed_singleton("app.UIFlowManager");
    auto* hs = fm ? get_obj(fm, "_Handles") : nullptr;
    static std::unordered_map<TD*, bool> is_select;
    int n = list_count(hs);
    for (int i = 0; i < n; ++i) {
        auto* h = list_item(hs, i);
        auto* prm = h ? get_obj(h, "<Param>k__BackingField") : nullptr;
        auto* td = prm ? prm->get_type_definition() : nullptr;
        if (!td) continue;
        auto it = is_select.find(td);
        if (it == is_select.end()) {
            auto nm = td->get_full_name();
            it = is_select.emplace(td, nm.find("UIFlowUI105") != std::string::npos || nm.find("SelectFighter") != std::string::npos).first;
        }
        if (it->second) return true;
    }
    return false;
}

std::string holder_folder_path(Obj* comp) {
    auto* go = call_obj(m_get_go, comp);
    auto* fld = go ? call_obj(m_go_folder, go) : nullptr;
    return fld ? fld_path(fld) : std::string{};
}

// Entries a and b of a CCVD's colour list (Colors: List<{ColorId, Data}>)
bool ccvd_pair(Obj* ccvd, int a, int b, Obj*& ea, Obj*& eb) {
    ea = eb = nullptr;
    auto* colors = ccvd ? get_obj(ccvd, "Colors") : nullptr;
    int n = list_count(colors);
    for (int i = 0; i < n; ++i) {
        auto* it = list_item(colors, i);
        int32_t id = -1;
        if (it && get_i32(it, "ColorId", id)) { if (id == a) ea = it; else if (id == b) eb = it; }
    }
    return ea && eb;
}

// Whether this CCVD holds the fighter's swap: its two entries hold each other's data. Checked on
// the data, not on the CCVD's address: a CCVD loaded again for the next match can take the address
// of the one freed, unswapped.
bool swap_in_place(const Fighter& fr, Obj* ccvd) {
    Obj *ea, *eb;
    if (!fr.has_csw || !ccvd_pair(ccvd, fr.csw_a, fr.csw_b, ea, eb)) return false;
    return reinterpret_cast<uintptr_t>(get_obj(ea, "Data")) == fr.csw_db
        && reinterpret_cast<uintptr_t>(get_obj(eb, "Data")) == fr.csw_da;
}

// The carrier only has the colours of the master table; another colour announced to the network is a
// communication error. The wanted colour's material data goes under the carrier's first colour.
void apply_color(int fid, Obj* d_set) {
    auto& fr = g_f[fid];
    int want = g_intent[fid].color;
    if (want < 0 || (fr.base_known && fr.base_colors.count(want))) return;
    auto* ccvd = get_obj(d_set, "costumeColorVariation");
    if (!ccvd) { logf("[F%d] colour: no costumeColorVariation", fid); return; }
    if (fr.csw_b == want && swap_in_place(fr, ccvd)) return;   // this CCVD is swapped already
    Obj *ea, *eb;
    Obj *da = nullptr, *db = nullptr;
    if (ccvd_pair(ccvd, fr.base_min, want, ea, eb)) { da = get_obj(ea, "Data"); db = get_obj(eb, "Data"); }
    if (!da || !db) { logf("[F%d] colour: entries %d/%d not found", fid, fr.base_min, want); return; }
    set_obj(ea, "Data", db); set_obj(eb, "Data", da);
    if (wjournal::on()) wjournal::log("CCVD_SWAP ccvd=%p ea=%p Data old=%p new=%p eb=%p Data old=%p new=%p F%d a=%d b=%d",
        (void*)ccvd, (void*)ea, (void*)da, (void*)db, (void*)eb, (void*)db, (void*)da, fid, fr.base_min, want);
    fr.has_csw = true; fr.csw_addr = reinterpret_cast<uintptr_t>(ccvd); fr.csw_a = fr.base_min; fr.csw_b = want;
    fr.csw_da = reinterpret_cast<uintptr_t>(da); fr.csw_db = reinterpret_cast<uintptr_t>(db);
    logf("[F%d] colour %d placed under colour %d", fid, want, fr.base_min);
}

// Put back through a live object read again (the slot's CCVD), never a kept pointer
void restore_color(int fid, Obj* src_h) {
    auto& fr = g_f[fid];
    if (!fr.has_csw) return;
    auto* s_set = src_h ? get_obj(src_h, "_Setting") : nullptr;
    auto* ccvd = s_set ? get_obj(s_set, "costumeColorVariation") : nullptr;
    Obj *ea, *eb;
    if (swap_in_place(fr, ccvd) && ccvd_pair(ccvd, fr.csw_a, fr.csw_b, ea, eb)) {
        auto* da = get_obj(ea, "Data"); auto* db = get_obj(eb, "Data");
        set_obj(ea, "Data", db); set_obj(eb, "Data", da);
        if (wjournal::on()) wjournal::log("CCVD_RESTORE ccvd=%p ea=%p Data old=%p new=%p eb=%p Data old=%p new=%p F%d",
            (void*)ccvd, (void*)ea, (void*)da, (void*)db, (void*)eb, (void*)db, (void*)da, fid);
        logf("[F%d] colour put back", fid);
    } else logf("[F%d] colour: the slot's CCVD was loaded again, nothing to put back", fid);
    fr.has_csw = false;
}

// The manifests present now, with their folder's path (read once per pass, for every character)
struct Holder { Obj* comp; std::string path; };
std::vector<Holder> read_holders() {
    std::vector<Holder> out;
    if (!cache_folder_methods()) return out;
    auto comps = find_components(td_holder);
    int n = list_count(comps);
    for (int i = 0; i < n; ++i) {
        auto* c = list_item(comps, i);
        if (!c) continue;
        auto p = holder_folder_path(c);
        if (!p.empty()) out.push_back({c, std::move(p)});
    }
    return out;
}

// One pass of the mount: the slot's folder is mounted once the game has mounted the carrier's
// (never before: the game skips mounting the carrier when a folder of the character is already
// active, and the load hangs), then the carrier's manifest is overwritten with the slot's.
// true when done.
bool mount_pass(int fid, const std::vector<Holder>& hs) {
    auto& fr = g_f[fid];
    int slot = g_intent[fid].slot;
    if (!slot || fr.dst_path.empty()) return true;
    std::string sfx = slot_suffix(fid, slot);
    Obj *src_h = nullptr, *dst_h = nullptr;
    for (auto& h : hs) {
        if (ends_with(h.path, sfx)) src_h = h.comp;
        else if (h.path == fr.dst_path && !dst_h) dst_h = h.comp;   // the first carrier (a mirror on it is not aliased: own_fighters)
    }
    char sig[32]; snprintf(sig, sizeof sig, "src=%s dst=%s", src_h ? "y" : "-", dst_h ? "y" : "-");
    if (fr.last_sig != sig) { logf("[F%d] holders %s (n=%zu, v%d)", fid, sig, hs.size(), slot); fr.last_sig = sig; }

    if (dst_h && !src_h) {
        if (!fr.src_folder || !alive(fr.src_folder)) fr.src_folder = find_folder(sfx);
        if (!fr.src_folder) return false;
        auto r = call(m_f_active, fr.src_folder);
        bool active = !r.exception_thrown && r.byte != 0;
        if (!active && g_frame - fr.last_mount > 120) {
            fr.last_mount = g_frame;
            call(m_f_activate, fr.src_folder);
            if (wjournal::on()) wjournal::log("MOUNT folder.activate obj=%p F%d v%d", (void*)fr.src_folder, fid, slot);
            logf("[F%d] slot v%d mounted (the game mounted %s)", fid, slot, carrier_name(fid).c_str());
        }
        return false;
    }
    if (dst_h && src_h) {
        if (!alive(dst_h) || !alive(src_h)) return false;   // safety: re-check before use
        uintptr_t addr = reinterpret_cast<uintptr_t>(dst_h);
        if (fr.swapped_addr == addr) return true;
        auto* s_set = get_obj(src_h, "_Setting");
        auto* d_set = get_obj(dst_h, "_Setting");
        if (!s_set || !d_set) return false;
        restore_color(fid, src_h);   // a swap left from the previous match, if its CCVD is still there
        auto ow = call(d_set, "overwrite", { arg_obj(s_set) });
        if (wjournal::on()) wjournal::log("MANIFEST overwrite dst=%p src=%p F%d v%d ok=%d", (void*)d_set, (void*)s_set, fid, slot, ow.exception_thrown ? 0 : 1);
        logf("[F%d] manifest v%d -> v%d = %d", fid, slot, fr.carrier, ow.exception_thrown ? 0 : 1);
        fr.swapped_addr = addr;
        if (!ow.exception_thrown) {
            apply_color(fid, d_set);
            // what the fighter really wears, for the other mods reading its costume / colour (through the core)
            ui::set_effective(fid, fr.carrier, slot, fr.has_csw ? fr.csw_b : -1);
            if (wjournal::on()) wjournal::log("EFFECTIVE F%d shown=%d real=%d color=%d", fid, fr.carrier, slot, fr.has_csw ? fr.csw_b : -1);
        }
        return true;
    }
    return false;
}

// Between matches (a screen that shows the slot as an outfit of its own): its colours put back
void restore_all() {
    bool any = false;
    for (auto& [fid, fr] : g_f) {
        fr.swapped_addr = 0; any |= fr.has_csw;
        if (fr.carrier < 0) continue;   // left out: never declared
        ui::set_effective(fid, fr.carrier, -1, -1);
        if (wjournal::on()) wjournal::log("EFFECTIVE_CLR F%d", fid);
    }
    if (!any) return;
    auto hs = read_holders();
    for (auto& [fid, fr] : g_f) {
        if (!fr.has_csw) continue;
        int slot = g_intent[fid].slot;
        std::string sfx = slot ? slot_suffix(fid, slot) : std::string{};
        Obj* src_h = nullptr;
        for (auto& h : hs) if (!sfx.empty() && ends_with(h.path, sfx)) { src_h = h.comp; break; }
        restore_color(fid, src_h);
    }
}

// An intent the alias applies: a slot the player sees (a hidden slot's intent waits)
bool any_intent() {
    for (auto& [fid, in] : g_intent) if (in.slot && shown(fid, in.slot)) return true;
    return false;
}

// The characters the player controls in the match being loaded, from its battle description
// (bFlowManager -> flow map -> m_desc): online, the fighter of our session member; offline, a
// fighter on a pad (the training dummy and the CPU have PadId -1). Kept only if it wears its carrier:
// an opponent in DriveTech is not ours to dress (03/10), and only in a mode that takes the costume
// from Battle Settings (online, Battle Hub cabinets). false when no player can be told apart
// (no description yet, a replay): then every intent applies, as before.
// `mirror`: the characters two fighters of the battle wear in their carrier (carrier::mirror), never
// aliased in that battle, whoever the player is.
std::string g_own_sig;
bool own_fighters(std::set<int>& out, std::set<int>& mirror) {
    out.clear(); mirror.clear();
    auto* desc = battle_desc();
    if (!desc) return false;
    // A mode where the player picks the costume on a select screen (Arcade, Training, Versus, Team
    // Battle): a slot is picked there as itself, so a fighter in its carrier really wears it and is
    // not ours to dress (Sagat's Drive Tech picked in Training wore his slot, 04/10). The rule's
    // GameMode is app.EGameMode, one byte: 1 Arcade, 2 Training, 3 and 4 Versus, 19 Team Battle.
    uint8_t mode = 0;
    if (auto* rule = get_obj(desc, "Rule"))
        if (auto* p = field_ptr(rule, "GameMode")) mode = *static_cast<uint8_t*>(p);
    const bool picked = mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 19;
    bool net = false; int32_t self = -1;
    get_bool(desc, "IsNetwork", net);
    get_i32(desc, "SelfSessionMemberindex", self);
    bool found = false;
    std::vector<std::pair<int, int>> worn;           // (fighter, costume) of every fighter of the battle
    auto* teams = get_obj(desc, "Teams");
    int nt = list_count(teams);
    for (int t = 0; t < nt; ++t) {
        auto* team = list_item(teams, t);
        auto* fs = team ? get_obj(team, "Fighters") : nullptr;
        int nf = list_count(fs);
        for (int i = 0; i < nf; ++i) {
            auto* f = list_item(fs, i);
            int32_t fid = 0, cos = -1, pad = -1, member = -1;
            if (!f || !get_i32(f, "FighterId", fid)) continue;
            get_i32(f, "Costume", cos); get_i32(f, "PadId", pad); get_i32(f, "SessionMemberIndex", member);
            worn.emplace_back(fid, cos);
            if (net ? (self < 0 || member != self) : pad < 0) continue;
            found = true;
            const int c = carrier_of(fid);
            if (c >= 0 && cos == c && !picked) out.insert(fid);
        }
    }
    for (int fid : g_fighters)
        if (carrier::mirror(worn, fid, carrier_of(fid))) { mirror.insert(fid); out.erase(fid); }
    bool all_dt = true;
    for (int fid : out) all_dt &= carrier_of(fid) == carrier::kDriveTech;
    std::string sig = !found ? "none" : out.empty() ? "-" : "";
    for (int fid : out) sig += "F" + std::to_string(fid) + (all_dt ? std::string() : " (" + carrier_name(fid) + ")") + " ";
    if (picked) sig += "(picked)";
    for (int fid : mirror) sig += " mirror F" + std::to_string(fid);
    if (sig != g_own_sig) {
        if (!found) logf("costumes: alias for every intent (no player in the battle description)");
        else if (picked) logf("costumes: no alias, game mode %d picks costumes on a select screen (%s)", mode, net ? "online" : "offline");
        else logf("costumes: alias for the player's fighters in %s: %s (%s, game mode %d)",
                  all_dt ? "DriveTech" : "their carrier", sig.substr(0, sig.find(" mirror")).c_str(), net ? "online" : "offline", mode);
        // a mirror matters where an intent would apply: no player told apart, or a mode that aliases
        if (!found || !picked)
            for (int fid : mirror) {
                auto in = g_intent.find(fid);
                if (in == g_intent.end() || !in->second.slot || !shown(fid, in->second.slot)) continue;
                logf("[F%d] alias skipped: mirror on the carrier (%s)", fid, carrier_name(fid).c_str());
                ui::set_effective(fid, carrier_of(fid), -1, -1);   // a declaration left from the last match
            }
        g_own_sig = sig;
    }
    return found;
}

// ---- hook: an outfit's visual manifest created (the game mounted its folder) ----
// May run on a loading thread: it only records the event.
int pre_holder_ctor(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin(); post(EV_HOLDER); perf::end(perf::HkCosHolderCtor, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- hooks: screens ----
template <perf::Id ID> int pre_menu_open(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)  { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_MENU_OPEN); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_menu_close(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_MENU_CLOSE); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_select_start(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_SELECT_START); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_select_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)   { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_SELECT_END); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_title_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)    { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_TITLE_END); perf::end(perf::HkCosTitleOnDestroy, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }

bool g_menu_open = false;

} // namespace

// ---------------------------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------------------------

void init() {
    load_registry();
    load_state();
    for (int fid : g_fighters) g_f[fid];
}

void install_hooks() {
    if (g_slots.empty()) { logf("costumes: no slot, no hook"); return; }
    auto* tdb = api().tdb();
    hook_all(tdb->find_type("app.battle.assets.FighterVisualHolder"), ".ctor", pre_holder_ctor, nullptr);
    auto* ms = tdb->find_type("app.UIFlowMatchingSetting.Param");
    hook_all(ms, "CreatedObject", pre_menu_open<perf::HkCosMenuCreatedObject>, nullptr);
    hook_all(ms, "ShowedObject", pre_menu_open<perf::HkCosMenuShowedObject>, nullptr);
    // closing: OnEnd is not called when the menu is left with Esc (30/09). HidObject is; a sub-menu
    // may hide it too, which the handlers tolerate (ShowedObject puts the slot back)
    hook_all(ms, "HidObject", pre_menu_close<perf::HkCosMenuHidObject>, nullptr);
    hook_all(ms, "OnEnd", pre_menu_close<perf::HkCosMenuOnEnd>, nullptr);
    auto* fs = tdb->find_type("app.battle.bBattleFighterSelectFlow");
    hook_all(fs, "start", pre_select_start<perf::HkCosFlowStart>, nullptr);
    hook_all(fs, "onDestroy", pre_select_end<perf::HkCosFlowOnDestroy>, nullptr);
    auto* ui = tdb->find_type("app.UIFlowUI10501");
    hook_all(ui, "Start", pre_select_start<perf::HkCosUI10501Start>, nullptr);
    hook_all(ui, "End", pre_select_end<perf::HkCosUI10501End>, nullptr);
    hook_all(tdb->find_type("app.menu.UIFlowTitle"), "onDestroy", pre_title_end, nullptr);
}

bool busy() {
    if (g_slots.empty()) return false;
    if (!g_boot_done || g_revoke_cursor >= kStaticMin || g_color_cursor >= kColorMin) return true;
    return g_mount_until != 0;
}

void tick(uint32_t ev) {
    if (g_slots.empty()) return;

    // ---- boot, once: every 30 frames until everything is in place (title screen, menus) ----
    if (!g_boot_done && g_frame - g_boot_last >= 30 && g_frame > 60) {
        g_boot_last = g_frame;
        read_carriers();
        find_dst_all();
        insert_colors();
        insert_select_ui();
        bool owned = give_ownership("boot");
        if (owned && g_carriers_done) safety_net();   // the carriers first: the save gets the right one
        g_boot_done = (g_dst_done && g_bc_done && g_colors_done && g_ui_done && owned) || g_frame > 60 * 600;
        if (g_boot_done) logf("costumes: boot done (dst %d, colours %d, records %d, ui %d, owned %d)",
                              g_dst_done, g_bc_done, g_colors_done, g_ui_done, owned ? 1 : 0);
    }

    // ---- sweeps of stale ownership: a slice per frame, then never again ----
    if (g_revoke_cursor >= kStaticMin) revoke_slice();
    if (g_color_cursor >= kColorMin) color_cleanup_slice();

    // ---- ownership back after the title screen (login clears it, 1 slot seen 30/09), and at every
    // screen that lists outfits. (UIFlowTitle is also destroyed when a battle loads: one pass then.)
    // From there on, a slot whose outfit the game does not count as the player's is hidden.
    if (ev & (EV_TITLE_END | EV_SELECT_START | EV_MENU_OPEN)) {
        g_after_title = true;
        give_ownership((ev & EV_SELECT_START) ? "character select" : (ev & EV_MENU_OPEN) ? "Battle Settings" : "title screen left");
    }

    // ---- Battle Settings ----
    // The open handler can run twice (CreatedObject and ShowedObject). A close counts only after an
    // open: the game calls HidObject on its way to other screens too (entering Versus), and a close
    // handled there reads DriveTech in the save and drops every intent (30/09).
    if (ev & EV_MENU_OPEN) { g_menu_open = true; on_menu_open(); }
    if ((ev & EV_MENU_CLOSE) && g_menu_open) { g_menu_open = false; on_menu_close(); safety_net(); }
    if (ev & (EV_SELECT_START | EV_SELECT_END)) safety_net();

    // ---- online alias: an outfit was mounted ----
    // Between matches, on a screen that shows the slot as an outfit of its own, its colours are put
    // back. On the character select screen the slot is a real outfit and the screen mounts folders
    // itself: a mount seen there is not ours to follow, and a check in progress stops there. (The
    // screen's own flow list says whether it is open: read when a mount is seen, never watched.)
    // Elsewhere a mount opens a short check, pushed further by each mount (the slot's own folder,
    // mounted by the check, is one): the manifests present are read every 3 frames until it ends,
    // during the loading screen. A check that finds the select screen open stops.
    if ((ev & (EV_SELECT_START | EV_MENU_OPEN)) && !g_mount_until) restore_all();
    // Battle Settings shows outfits too, DriveTech among them: nothing is aliased there (a slot was
    // mounted over the DriveTech it previewed, 03/10).
    if ((ev & EV_HOLDER) && any_intent() && g_dst_done && !g_menu_open) {
        if (select_screen_active()) {
            if (!g_select_logged) { logf("costumes: outfit mounted by the select screen, left alone"); g_select_logged = true; }
        } else {
            g_select_logged = false;
            if (!g_mount_until) {
                logf("costumes: outfit mounted, alias check");
                if (wjournal::on()) wjournal::log("ALIAS_START frame=%u", g_frame);
                for (auto& [fid, fr] : g_f) { fr.swapped_addr = 0; fr.last_sig.clear(); }
                g_own_sig.clear();
            }
            g_mount_until = g_frame + 60 * 2;
        }
    }
    if (g_mount_until) {
        if (g_frame >= g_mount_until) { g_mount_until = 0; if (wjournal::on()) wjournal::log("ALIAS_END frame=%u", g_frame); logf("costumes: alias check done"); }
        else if (g_frame % 3 == 0) {
            if (g_menu_open || select_screen_active()) {
                g_mount_until = 0;
                if (wjournal::on()) wjournal::log("ALIAS_STOP frame=%u reason=%s", g_frame, g_menu_open ? "menu" : "select");
                logf("costumes: alias check stopped (%s)", g_menu_open ? "Battle Settings" : "select screen");
                return;
            }
            auto hs = read_holders();
            std::set<int> own, mirror;
            bool known = own_fighters(own, mirror);
            if (wjournal::on()) wjournal::log("ALIAS_TICK frame=%u holders=%zu known=%d own=%zu", g_frame, hs.size(), known ? 1 : 0, own.size());
            for (auto& [fid, in] : g_intent)
                if (in.slot && shown(fid, in.slot) && (!known || own.count(fid)) && !mirror.count(fid)) mount_pass(fid, hs);
        }
    }
}

} // namespace slots::costumes
