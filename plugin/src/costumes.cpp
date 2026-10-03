// SF6 Slots — native plugin: costume slots.
//
// What the costume loader's pak cannot do by itself, done where the game needs it:
//   - boot: the slots' colour records, their select screen data (with the colour squares the
//     loader computed), DriveTech's colours and folders, once;
//   - ownership: every slot is owned (AddDlcSaveData), slots of removed mods are not; the game
//     clears DLC ownership at login, so it is given back after the title screen and whenever a
//     screen that lists outfits opens;
//   - online alias: Battle Settings keeps DriveTech in the save, which is what other players see,
//     and the slot chosen there as an intent; when the game mounts DriveTech's folder for a match,
//     the slot's folder is mounted too and its visual manifest copied over DriveTech's, for the
//     player's own fighter only (the battle description says which one). A mount is
//     seen through the manifest's constructor (FighterVisualHolder..ctor): via.Folder.activate is a
//     native method the engine calls internally, its hook only saw our own calls (30/09).
// Everything starts from an event (see slots.hpp); nothing is watched in between.
#include "slots.hpp"
#include "slots_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace slots::costumes {
namespace {

constexpr int BASE_COS = 4;   // DriveTech, costume 4 of every character
const char* kRegistry = "reframework/data/SF6_Costumes_Data/registry.json";
const char* kState    = "reframework/data/SF6_CostumeSlots_data/state.json";

// ---- registry (written by the loader at every launch) ----
struct Slot {
    int fighter = 0, costume_no = 0, record_id = 0;
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

// ---- intents (Battle Settings choices), state.json ----
struct Intent { int slot = 0; int color = -1; };       // slot 0: none
std::map<int, Intent> g_intent;

// ---- per character ----
struct Fighter {
    std::string dst_path;            // DriveTech folder (fighterNNN/esfNNNv04)
    Obj* dst_folder = nullptr;
    std::set<int> base_colors; int base_min = 0; bool base_known = false;
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
    for (auto& e : pos->arr) {
        Slot s;
        const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* r = e.get("record_id");
        if (!f || !c) continue;
        s.fighter = static_cast<int>(f->as_int()); s.costume_no = static_cast<int>(c->as_int());
        s.record_id = r ? static_cast<int>(r->as_int()) : 0;
        if (s.fighter <= 0 || s.costume_no <= 0) continue;
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
        if (in.slot && !is_slot(fid, in.slot)) in = Intent{};   // the slot's mod is gone
        g_intent[fid] = in;
    }
}

void save_state() {
    std::string o = "{\n    \"fighters\": {";
    bool first = true;
    for (auto& [fid, in] : g_intent) {
        char b[128];
        if (in.slot) {
            if (in.color >= 0) snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": %d, \"color\": %d }", first ? "" : ",", fid, in.slot, in.color);
            else snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": %d }", first ? "" : ",", fid, in.slot);
        } else snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": false }", first ? "" : ",", fid);
        o += b; first = false;
    }
    o += "\n    }\n}\n";
    if (!write_file(kState, o)) logf("costumes: cannot write state.json");
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

void find_dst_all() {
    if (g_dst_done || !cache_folder_methods()) return;
    auto* sc = current_scene();
    auto* root = sc ? call_obj(m_first_folder, sc) : nullptr;
    if (!root) return;
    std::map<std::string, int> want;
    for (int fid : g_fighters) {
        if (!g_f[fid].dst_path.empty()) continue;
        char b[64]; snprintf(b, sizeof b, "fighter%03d/esf%03dv%02d", fid, fid, BASE_COS);
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
    g_dst_done = want.empty();
    if (g_dst_done) logf("costumes: DriveTech folders found");
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
    if (get_cos(e) == v) return;
    set_i32(e, "MatchingFighterCostume", v);
    logf("[F%d] save MatchingFighterCostume -> %d (%s)", fid, v, why);
}
void set_col(Obj* e, int v, int fid, const char* why) {
    if (get_col(e) == v) return;
    set_i32(e, "MatchingFighterColor", v);
    logf("[F%d] save MatchingFighterColor -> %d (%s)", fid, v, why);
}

// Battle Settings opened: the menu shows the slot where the save has DriveTech
void on_menu_open() {
    logf("costumes: Battle Settings opened");
    for (auto& [fid, e] : save_entries()) {
        auto& in = g_intent[fid];
        if (in.slot && get_cos(e) == BASE_COS) set_cos(e, in.slot, fid, "so the menu shows the slot");
        if (in.slot && in.color >= 0 && get_col(e) != in.color) set_col(e, in.color, fid, "so the menu shows the colour");
    }
}

// Battle Settings closed: a slot chosen there becomes the intent, the save gets DriveTech
void on_menu_close() {
    logf("costumes: Battle Settings closed");
    for (auto& [fid, e] : save_entries()) {
        int v = get_cos(e);
        set_intent_slot(fid, is_slot(fid, v) ? v : 0, "chosen in the menu");
        if (is_slot(fid, v)) {
            set_cos(e, BASE_COS, fid, "what the game and the network must see");
            int c = get_col(e);
            set_intent_color(fid, c, "chosen in the menu");
            auto& fr = g_f[fid];
            if (fr.base_known && !fr.base_colors.count(c)) set_col(e, fr.base_min, fid, "a colour DriveTech has");
        }
    }
}

// Outside the menu the save never keeps a slot number
void safety_net() {
    for (auto& [fid, e] : save_entries()) {
        int v = get_cos(e);
        if (is_slot(fid, v)) {
            set_intent_slot(fid, v, "slot found in the save outside the menu");
            set_cos(e, BASE_COS, fid, "safety net");
        }
        auto& fr = g_f[fid];
        if (g_intent[fid].slot && fr.base_known) {
            int c = get_col(e);
            if (!fr.base_colors.count(c)) {
                set_intent_color(fid, c, "colour found in the save outside the menu");
                set_col(e, fr.base_min, fid, "safety net");
            }
        }
    }
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

// true when the save answered (it is loaded)
bool give_ownership() {
    auto* ci = costume_inventory();
    if (!ci || g_slots.empty()) return false;
    int added = 0; bool any = false;
    for (auto& s : g_slots) {
        bool ok = false;
        bool has = call_bool(ci, "ExistsSaveData", { arg_i32(s.record_id) }, &ok);
        if (!ok) continue;
        any = true;
        if (!has) { call(ci, "AddDlcSaveData", { arg_i32(s.record_id) }); ++added; }
    }
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
        if (ok1 && ok2 && owned && !has_rec) { call(ci, g_color_delete.c_str(), { arg_i32(id) }); ++revoked; }
    }
    if (revoked) logf("costumes: %d orphan colour ownerships taken back", revoked);
    g_color_cursor = stop >= kColorMax ? -2 : stop + 1;
}

// ---------------------------------------------------------------------------------------------
// Boot: DriveTech's colours, the slots' colour records, their select screen data
// ---------------------------------------------------------------------------------------------

void read_base_colors() {
    if (g_bc_done) return;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    auto* ci = costume_inventory();
    if (!tm || !ci) return;
    Method* m_rec = method_of(ci->get_type_definition(), "GetMasterDataRecord", 1);
    Method* m_cols = method_of(tm->get_type_definition(), "GetFighterCostumeColors", 2);
    if (!m_rec || !m_cols) return;
    for (int rid = 1; rid <= 250; ++rid) {
        auto* rec = call_obj(m_rec, ci, { arg_i32(rid) });
        int32_t fid = 0, cn = 0;
        if (!rec || !get_i32(rec, "fighterId", fid) || !get_i32(rec, "costumeNo", cn)) continue;
        if (cn != BASE_COS || !g_slot_of.count(fid) || g_f[fid].base_known) continue;
        auto* cls = call_obj(m_cols, tm, { arg_i32(rid), arg_bool(false) });
        if (!cls) continue;
        auto& fr = g_f[fid];
        int n = list_count(cls), mn = 999;
        for (int j = 0; j < n; ++j) {
            int32_t c = 0;
            if (auto* cr = list_item(cls, j); cr && get_i32(cr, "colorNo", c)) { fr.base_colors.insert(c); mn = std::min(mn, static_cast<int>(c)); }
        }
        fr.base_min = mn < 999 ? mn : 0;
        fr.base_known = true;
    }
    g_bc_done = std::all_of(g_fighters.begin(), g_fighters.end(), [](int fid) { return g_f[fid].base_known; });
    if (g_bc_done) logf("costumes: DriveTech colours read");
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
                    ++added;
                }
            }
            if (col_inv) {
                bool ok2 = false;
                bool has = call_bool(col_inv, "ExistsSaveData", { arg_i32(id) }, &ok2);
                if (ok2 && !has && call_bool(col_inv, "AddSaveData", { arg_i32(id) })) ++owned;
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
            ++n;
        }
    }
    g_ui_done = true;
    logf("costumes: select screen data added for %d slots, %d colour squares", n, colored);
}

// ---------------------------------------------------------------------------------------------
// Online alias: the slot's folder over DriveTech's
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

// DriveTech only has the colours of the master table; another colour announced to the network is a
// communication error. The wanted colour's material data goes under DriveTech's first colour.
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

// One pass of the mount: the slot's folder is mounted once the game has mounted DriveTech's
// (never before: the game skips mounting DriveTech when a folder of the character is already
// active, and the load hangs), then DriveTech's manifest is overwritten with the slot's.
// true when done.
bool mount_pass(int fid, const std::vector<Holder>& hs) {
    auto& fr = g_f[fid];
    int slot = g_intent[fid].slot;
    if (!slot || fr.dst_path.empty()) return true;
    std::string sfx = slot_suffix(fid, slot);
    Obj *src_h = nullptr, *dst_h = nullptr;
    for (auto& h : hs) {
        if (ends_with(h.path, sfx)) src_h = h.comp;
        else if (h.path == fr.dst_path && !dst_h) dst_h = h.comp;   // the first DriveTech only (mirror match: known limit)
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
            logf("[F%d] slot v%d mounted (the game mounted DriveTech)", fid, slot);
        }
        return false;
    }
    if (dst_h && src_h) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(dst_h);
        if (fr.swapped_addr == addr) return true;
        auto* s_set = get_obj(src_h, "_Setting");
        auto* d_set = get_obj(dst_h, "_Setting");
        if (!s_set || !d_set) return false;
        restore_color(fid, src_h);   // a swap left from the previous match, if its CCVD is still there
        auto ow = call(d_set, "overwrite", { arg_obj(s_set) });
        logf("[F%d] manifest v%d -> v%d = %d", fid, slot, BASE_COS, ow.exception_thrown ? 0 : 1);
        fr.swapped_addr = addr;
        if (!ow.exception_thrown) apply_color(fid, d_set);
        return true;
    }
    return false;
}

// Between matches (a screen that shows the slot as an outfit of its own): its colours put back
void restore_all() {
    bool any = false;
    for (auto& [fid, fr] : g_f) { fr.swapped_addr = 0; any |= fr.has_csw; }
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

bool any_intent() {
    for (auto& [fid, in] : g_intent) if (in.slot) return true;
    return false;
}

// The characters the player controls in the match being loaded, from its battle description
// (bFlowManager -> flow map -> m_desc): online, the fighter of our session member; offline, a
// fighter on a pad (the training dummy and the CPU have PadId -1). Kept only if it wears DriveTech:
// an opponent in DriveTech is not ours to dress (03/10). false when no player can be told apart
// (no description yet, a replay): then every intent applies, as before.
std::string g_own_sig;
bool own_fighters(std::set<int>& out) {
    out.clear();
    auto* fm = api().get_managed_singleton("app.bFlowManager");
    auto* fw = fm ? get_obj(fm, "m_flow_work") : nullptr;
    auto* map = fw ? get_obj(fw, "_FlowMap") : nullptr;
    auto* desc = map ? get_obj(map, "m_desc") : nullptr;
    if (!alive(desc)) return false;
    bool net = false; int32_t self = -1;
    get_bool(desc, "IsNetwork", net);
    get_i32(desc, "SelfSessionMemberindex", self);
    bool found = false;
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
            if (net ? (self < 0 || member != self) : pad < 0) continue;
            found = true;
            if (cos == BASE_COS) out.insert(fid);
        }
    }
    std::string sig = !found ? "none" : out.empty() ? "-" : "";
    for (int fid : out) sig += "F" + std::to_string(fid) + " ";
    if (sig != g_own_sig) {
        if (!found) logf("costumes: alias for every intent (no player in the battle description)");
        else logf("costumes: alias for the player's fighters in DriveTech: %s (%s)", sig.c_str(), net ? "online" : "offline");
        g_own_sig = sig;
    }
    return found;
}

// ---- hook: an outfit's visual manifest created (the game mounted its folder) ----
// May run on a loading thread: it only records the event.
int pre_holder_ctor(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    post(EV_HOLDER);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- hooks: screens ----
int pre_menu_open(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)  { post(EV_MENU_OPEN); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_menu_close(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { post(EV_MENU_CLOSE); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_select_start(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { post(EV_SELECT_START); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_select_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)   { post(EV_SELECT_END); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_title_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)    { post(EV_TITLE_END); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }

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
    hook_all(ms, "CreatedObject", pre_menu_open, nullptr);
    hook_all(ms, "ShowedObject", pre_menu_open, nullptr);
    // closing: OnEnd is not called when the menu is left with Esc (30/09). HidObject is; a sub-menu
    // may hide it too, which the handlers tolerate (ShowedObject puts the slot back)
    hook_all(ms, "HidObject", pre_menu_close, nullptr);
    hook_all(ms, "OnEnd", pre_menu_close, nullptr);
    auto* fs = tdb->find_type("app.battle.bBattleFighterSelectFlow");
    hook_all(fs, "start", pre_select_start, nullptr);
    hook_all(fs, "onDestroy", pre_select_end, nullptr);
    auto* ui = tdb->find_type("app.UIFlowUI10501");
    hook_all(ui, "Start", pre_select_start, nullptr);
    hook_all(ui, "End", pre_select_end, nullptr);
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
        find_dst_all();
        read_base_colors();
        insert_colors();
        insert_select_ui();
        bool owned = give_ownership();
        if (owned) safety_net();
        g_boot_done = (g_dst_done && g_bc_done && g_colors_done && g_ui_done && owned) || g_frame > 60 * 600;
        if (g_boot_done) logf("costumes: boot done (dst %d, colours %d, records %d, ui %d, owned %d)",
                              g_dst_done, g_bc_done, g_colors_done, g_ui_done, owned ? 1 : 0);
    }

    // ---- sweeps of stale ownership: a slice per frame, then never again ----
    if (g_revoke_cursor >= kStaticMin) revoke_slice();
    if (g_color_cursor >= kColorMin) color_cleanup_slice();

    // ---- ownership back after the title screen (login clears it, 1 slot seen 30/09), and at every
    // screen that lists outfits. (UIFlowTitle is also destroyed when a battle loads: one pass then.)
    if (ev & (EV_TITLE_END | EV_SELECT_START | EV_MENU_OPEN)) give_ownership();

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
                for (auto& [fid, fr] : g_f) { fr.swapped_addr = 0; fr.last_sig.clear(); }
                g_own_sig.clear();
            }
            g_mount_until = g_frame + 60 * 2;
        }
    }
    if (g_mount_until) {
        if (g_frame >= g_mount_until) { g_mount_until = 0; logf("costumes: alias check done"); }
        else if (g_frame % 3 == 0) {
            if (g_menu_open || select_screen_active()) { g_mount_until = 0; logf("costumes: alias check stopped (%s)", g_menu_open ? "Battle Settings" : "select screen"); return; }
            auto hs = read_holders();
            std::set<int> own;
            bool known = own_fighters(own);
            for (auto& [fid, in] : g_intent) if (in.slot && (!known || own.count(fid))) mount_pass(fid, hs);
        }
    }
}

} // namespace slots::costumes
