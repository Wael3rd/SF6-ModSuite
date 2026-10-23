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
//   - online alias: Battle Settings keeps the character's carrier in the save (Outfit 1 or DriveTech, the
//     player's setting: carrier.hpp),
//     which is what other players see, and the slot chosen there as an intent; when the game asks for
//     the carrier's files for a match, the player's own fighter in the battle description gets the slot
//     as the game's own local costume override and the slot's files are asked for next to the
//     carrier's: that fighter is built from the slot, every other one from its own outfit, a mirror on
//     the carrier included (see "Online alias" below).
// Everything starts from an event (see slots.hpp); nothing is watched in between.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "rto.hpp"
#include "ownership.hpp"
#include "carrier.hpp"
#include "perf.hpp"
#include "wjournal.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
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
    bool nsfw = false;               // from costume_mods_nsfw: shown only while "Display NSFW Outfits" is on
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

// NSFW outfits (the registry's "nsfw" slots): shown only while settings.json's "display_nsfw" is true (off by
// default, also when the file is missing). Read at start and again, when the file changed, at every screen that
// lists outfits (no polling): a change takes effect there, without a relaunch.
const char* kSettings = "reframework/data/SF6_CostumeSlots_data/settings.json";
size_t g_nsfw_slots = 0;
bool g_show_nsfw = false;
uint64_t g_settings_stamp = 0;       // size and date of settings.json when read (0: never read / missing)
bool g_nsfw_apply = false;           // a change not applied yet (Battle Settings was open)
bool g_nsfw_row = false;             // the in-game row (nsfw_option.cpp) shows the setting: the file is followed even with no NSFW content
// The game's list of a fighter's outfits (CostumeInventory.GetCostumeNoList: character select screens, Battle Settings)
// leaves the NSFW outfits out while they are hidden (ownership::nsfw_drops: an unlock-all script keeps them in it
// whatever their ownership; 20/10, C. Viper's Lace lingerie still listed with the row Off). The hook runs on any
// thread: it reads g_nsfw_nos (written once by load_registry, before the hooks) and g_hide_nsfw only.
std::map<int, std::vector<uint32_t>> g_nsfw_nos;     // fighter -> its NSFW outfits' costume numbers
std::atomic<bool> g_hide_nsfw{true};                 // !g_show_nsfw
std::atomic<uint32_t> g_list_drops{0};               // NSFW outfits left out of the game's lists (log)
uint32_t g_list_drops_logged = 0;
void set_show_nsfw(bool on) { g_show_nsfw = on; g_hide_nsfw.store(!on, std::memory_order_relaxed); }

bool visible(const Slot& s) {
    return g_book.shown(s.fighter, s.base) && ownership::nsfw_shown(s.nsfw, g_show_nsfw);
}

// A slot of the registry the player sees: its outfit is the player's, or not known to be otherwise, and it is not
// an NSFW outfit while those are hidden
bool shown(int fid, int cos) {
    auto f = g_slot_of.find(fid);
    if (f == g_slot_of.end()) return false;
    auto c = f->second.find(cos);
    return c != f->second.end() && visible(g_slots[c->second]);
}
bool nsfw_hidden(int fid, int cos) {
    auto f = g_slot_of.find(fid);
    if (f == g_slot_of.end()) return false;
    auto c = f->second.find(cos);
    return c != f->second.end() && !ownership::nsfw_shown(g_slots[c->second].nsfw, g_show_nsfw);
}

// ---- intents (Battle Settings choices), state.json ----
struct Intent { int slot = 0; int color = -1; };       // slot 0: none
std::map<int, Intent> g_intent;
// A Battle Settings choice of a retouch's former outfit (RuntimeOptions=on): the retouch is an option of the game
// outfit it was made from (rto.cpp chose it for P1); the save gets that game outfit once it can be reached. Until
// then state.json keeps the former slot, so a session that ends before does it again.
struct ToGame { int costume = 0, color = -1, from = 0; };
std::map<int, ToGame> g_to_game;
// Outfits the loader numbered again because they became NSFW or stopped being NSFW (outfits.json "moved"):
// (fighter, old number) -> new number. A Battle Settings choice of the old number follows (load_state).
std::map<std::pair<int, int>, int> g_moved;

// ---- per character ----
struct Fighter {
    int carrier = -1;                // the alias's outfit (carrier.hpp), -1 not decided or none
    bool left_out = false;           // no carrier: no alias for this character
    std::set<int> costumes;          // the character's own outfits in the game's costume table
    std::set<int> base_colors; int base_min = 0; bool base_known = false;   // the carrier's colours
    std::map<int, std::set<int>> colours;   // Outfit 1's and DriveTech's colours (the carrier's are copied above)
    std::vector<int> o1_colors;      // Outfit 1's colours (an NSFW outfit hidden goes back to it), empty: not known
};
std::map<int, Fighter> g_f;

// Carriers decided (g_carriers_done), from the game's costume table (g_table_ok) or, when the table
// gives no answer, from the folders of the scene
bool g_carriers_done = false, g_table_ok = false;
// The carrier the player chose (settings.json "online_outfit"): DriveTech, else Outfit 1. A change is applied by
// the tick outside Battle Settings (g_carrier_apply): carriers decided again, saves moved (safety_net).
bool g_online_dt = false;
bool g_carrier_apply = false;

// The fighter's carrier from the setting, with its colours when they are known
void set_carrier(struct Fighter& fr);

int carrier_of(int fid) {
    auto it = g_f.find(fid);
    return it == g_f.end() ? -1 : it->second.carrier;
}
// What the save data holds while a slot is the intent: the carrier (the setting's until it is decided)
int save_cos_of(int fid) {
    int c = carrier_of(fid);
    return c >= 0 ? c : g_online_dt ? carrier::kDriveTech : carrier::kOutfit1;
}

void set_carrier(Fighter& fr) {
    fr.carrier = carrier::choose(fr.costumes, g_online_dt);
    auto c = fr.colours.find(fr.carrier);
    fr.base_known = c != fr.colours.end() && !c->second.empty();
    fr.base_colors = fr.base_known ? c->second : std::set<int>{};
    fr.base_min = fr.base_known ? *c->second.begin() : 0;
}
std::string carrier_name(int fid) { return ownership::outfit_label(save_cos_of(fid)); }

// The carrier the save data was given in an earlier session (state.json "carrier"; DriveTech when
// it names none), to move a save that still holds it (carrier::stale)
std::map<int, int> g_saved_carrier;
int saved_carrier_of(int fid) {
    auto it = g_saved_carrier.find(fid);
    return it == g_saved_carrier.end() ? carrier::kDriveTech : it->second;
}

// The carriers in log lines: the setting's outfit, with the characters that wear another named
std::string carriers_text() {
    const int want = g_online_dt ? carrier::kDriveTech : carrier::kOutfit1;
    std::string out;
    for (int fid : g_fighters)
        if (int c = carrier_of(fid); c != want)
            out += (out.empty() ? "" : ", ") + (c < 0 ? std::string("none") : ownership::outfit_label(c)) + " for F" + std::to_string(fid);
    const std::string w = ownership::outfit_label(want);
    return out.empty() ? w : w + " (" + out + ")";
}

// ---- progress ----
bool g_boot_done = false;
uint32_t g_boot_last = 0;
bool g_bc_done = false, g_colors_done = false, g_ui_done = false;
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
    if (json_parse(read_file(kOutfits), oj)) {
        if (const Json* os = oj.get("outfits"); os && os->kind == Json::Array)
            for (auto& e : os->arr) {
                const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* l = e.get("label");
                if (f && c && l) label_of[{static_cast<int>(f->as_int()), static_cast<int>(c->as_int())}] = l->as_str();
            }
        if (const Json* mv = oj.get("moved"); mv && mv->kind == Json::Array)
            for (auto& e : mv->arr) {
                const Json* f = e.get("fighter"); const Json* a = e.get("from"); const Json* b = e.get("to");
                if (f && a && b) g_moved[{static_cast<int>(f->as_int()), static_cast<int>(a->as_int())}] = static_cast<int>(b->as_int());
            }
    }
    for (auto& e : pos->arr) {
        Slot s;
        const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* r = e.get("record_id");
        if (!f || !c) continue;
        s.fighter = static_cast<int>(f->as_int()); s.costume_no = static_cast<int>(c->as_int());
        s.record_id = r ? static_cast<int>(r->as_int()) : 0;
        if (s.fighter <= 0 || s.costume_no <= 0) continue;
        if (const Json* nm = e.get("name")) s.name = nm->as_str();
        if (const Json* ns = e.get("nsfw"); ns && ns->kind == Json::Bool && ns->b) {
            s.nsfw = true; ++g_nsfw_slots;
            g_nsfw_nos[s.fighter].push_back(static_cast<uint32_t>(s.costume_no));
        }
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
    logf("costumes: %zu slots, %zu characters%s", g_slots.size(), g_fighters.size(),
         g_nsfw_slots ? (", " + std::to_string(g_nsfw_slots) + " NSFW").c_str() : "");
}

// A fresh settings.json: every key with its default (select_options_*: rto_select.cpp, the costume options on the
// character select)
const char* kSettingsDefault = "{\n    \"display_nsfw\": false,\n    \"online_outfit\": \"Outfit 1\",\n"
                               "    \"select_options_button\": \"R1\",\n    \"select_options_key\": \"C\"\n}\n";
// settings.json with one key set, the other keys kept as they are
std::string settings_with(const char* key, const Json& v) {
    Json j;
    std::string text = read_file(kSettings);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    if (text.empty() || !json_parse(text, j) || j.kind != Json::Object) json_parse(kSettingsDefault, j);
    bool set = false;
    for (auto& kv : j.obj) if (kv.first == key) { kv.second = v; set = true; }
    if (!set) j.obj.insert(j.obj.begin(), { key, v });
    return json_write(j) + "\n";
}
std::string settings_with_nsfw(bool on) {
    Json v;
    v.kind = Json::Bool;
    v.b = on;
    return settings_with("display_nsfw", v);
}

// settings.json's date and size in one number (0: no file)
uint64_t settings_stamp() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(kSettings, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
}
// Reads "display_nsfw" (false when the file or the key is missing) and "online_outfit" ("DriveTech", else Outfit 1;
// a change is applied by the tick). Returns true when the NSFW setting changed.
bool read_nsfw_setting(const char* why) {
    const uint64_t st = settings_stamp();
    if (st && st == g_settings_stamp) return false;
    g_settings_stamp = st;
    bool on = false, dt = false;
    Json j;
    std::string text = st ? read_file(kSettings) : std::string();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // saved by an editor with a BOM
    if (st && json_parse(text, j)) {
        if (const Json* d = j.get("display_nsfw"); d && d->kind == Json::Bool) on = d->b;
        if (const Json* o = j.get("online_outfit"); o && o->kind == Json::String) dt = _stricmp(o->s.c_str(), "DriveTech") == 0;
    }
    if (!st) {   // a fresh install: the file, so the setting can be found (off)
        CreateDirectoryA("reframework/data/SF6_CostumeSlots_data", nullptr);
        if (write_file(kSettings, kSettingsDefault)) g_settings_stamp = settings_stamp();
    }
    if (dt != g_online_dt || std::strcmp(why, "start") == 0) {
        if (dt != g_online_dt) g_carrier_apply = true;
        g_online_dt = dt;
        logf("costumes: Outfit Others See: %s (%s)", dt ? "DriveTech" : "Outfit 1", why);
    }
    if (!g_nsfw_slots && !rto::nsfw_options() && !g_nsfw_row) return false;
    if (on == g_show_nsfw && std::strcmp(why, "start") != 0) return false;
    const bool changed = on != g_show_nsfw;
    set_show_nsfw(on);
    logf("costumes: Display NSFW Outfits %s (%s): %zu NSFW outfit%s %s%s", on ? "on" : "off", why, g_nsfw_slots,
         g_nsfw_slots == 1 ? "" : "s", on ? "shown" : "hidden", rto::nsfw_options() ? ", NSFW options too" : "");
    return changed;
}

void save_state();
void load_state() {
    Json j;
    if (!json_parse(read_file(kState), j)) return;
    const Json* fs = j.get("fighters");
    if (!fs || fs->kind != Json::Object) return;
    bool migrated = false;
    for (auto& [k, v] : fs->obj) {
        int fid = atoi(k.c_str());
        if (fid <= 0) continue;
        Intent in;
        if (const Json* s = v.get("slot"); s && s->kind == Json::Number) in.slot = static_cast<int>(s->num);
        if (const Json* c = v.get("color"); c && c->kind == Json::Number) in.color = static_cast<int>(c->num);
        if (const Json* c = v.get("carrier"); c && c->kind == Json::Number) g_saved_carrier[fid] = static_cast<int>(c->num);
        // A variant slot of a modular mod folded into one outfit with options (RuntimeOptions=on): the
        // choice moves to the kept slot, the variant's options become the P1 choice (rto.cpp)
        // An outfit numbered again (it became NSFW, or stopped being NSFW): the choice follows it
        if (in.slot && !is_slot(fid, in.slot))
            if (auto mv = g_moved.find({fid, in.slot}); mv != g_moved.end() && is_slot(fid, mv->second)) {
                logf("costumes: [F%d] Battle Settings outfit %d is now outfit %d (numbered again: NSFW outfits come after "
                     "the others)", fid, in.slot, mv->second);
                in.slot = mv->second;
                migrated = true;
            }
        if (in.slot && !is_slot(fid, in.slot)) {
            bool game = false;
            int kept = rto::migrate_former(fid, in.slot, &game);
            if (kept >= 0 && game) { g_to_game[fid] = {kept, in.color, in.slot}; in = Intent{}; }
            else if (kept > 0 && is_slot(fid, kept)) { in.slot = kept; migrated = true; }
        }
        if (in.slot && !is_slot(fid, in.slot)) in = Intent{};   // the slot's mod is gone
        g_intent[fid] = in;
    }
    rto::migration_done();
    if (migrated) save_state();
}

void save_state() {
    std::string o = "{\n    \"fighters\": {";
    bool first = true;
    for (auto& [fid, in] : g_intent) {
        char b[128], car[32] = "";
        // the carrier the save was given, when it is not DriveTech (what a file without it means: 1.14 and before)
        if (int sc = saved_carrier_of(fid); sc != carrier::kDriveTech) snprintf(car, sizeof car, ", \"carrier\": %d", sc);
        if (auto tg = g_to_game.find(fid); tg != g_to_game.end()) {   // not done yet: the former slot stays
            if (tg->second.color >= 0) snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": %d, \"color\": %d%s }", first ? "" : ",", fid, tg->second.from, tg->second.color, car);
            else snprintf(b, sizeof b, "%s\n        \"%d\": { \"slot\": %d%s }", first ? "" : ",", fid, tg->second.from, car);
        } else if (in.slot) {
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

bool cache_folder_methods() {
    if (m_f_path) return true;
    auto* tdb = api().tdb();
    auto* td_scene = tdb->find_type("via.Scene");
    auto* td_folder = tdb->find_type("via.Folder");
    if (!td_scene || !td_folder) return false;
    m_first_folder = method_of(td_scene, "get_FirstFolder", 0);
    m_f_child = method_of(td_folder, "get_Child", 0);
    m_f_next = method_of(td_folder, "get_Next", 0);
    m_f_path = method_of(td_folder, "get_Path", 0);
    return m_first_folder && m_f_child && m_f_next && m_f_path;
}

std::string fld_path(Obj* f) { return str(call_obj(m_f_path, f)); }

bool ends_with(const std::string& s, const std::string& sfx) {
    return sfx.size() <= s.size() && s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}

template <typename Fn> void walk_folders(Obj* f, int depth, Fn&& fn) {
    if (!f || depth > 8) return;
    if (!fn(f)) return;
    walk_folders(call_obj(m_f_child, f), depth + 1, fn);
    walk_folders(call_obj(m_f_next, f), depth, fn);
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

// "Display NSFW Outfits" off: a choice of an NSFW outfit (Battle Settings' choice kept as the intent, the save holding
// its carrier or its number) goes back to the character's Outfit 1, in the colour chosen with it when Outfit 1 has it.
// A character whose save entry cannot be reached yet keeps its choice until it can (the hidden outfit is never worn:
// the alias leaves hidden slots out).
void drop_nsfw_choices(const std::map<int, Obj*>& entries) {
    if (!g_nsfw_slots || g_show_nsfw) return;
    for (auto& [fid, in] : g_intent) {
        if (!in.slot || !nsfw_hidden(fid, in.slot)) continue;
        auto it = entries.find(fid);
        if (it == entries.end()) continue;
        Obj* e = it->second;
        const int was = in.slot, colour = in.color;
        const char* why = "NSFW outfit hidden (Display NSFW Outfits off): Outfit 1";
        logf("[F%d] slot v%02d is an NSFW outfit and NSFW outfits are hidden: the choice goes back to Outfit 1", fid, was);
        set_intent_slot(fid, 0, why);
        const int v = get_cos(e);
        if (v == save_cos_of(fid) || v == was) set_cos(e, carrier::kOutfit1, fid, why);
        const int c = ownership::outfit1_colour(colour, get_col(e), g_f[fid].o1_colors);
        if (c >= 0) set_col(e, c, fid, why);
    }
}

// Outside the menu the save never keeps a slot number, nor a former carrier
void safety_net() {
    bool carriers_saved = false;
    const auto entries = save_entries();
    for (auto& [fid, e] : entries) {
        int v = get_cos(e);
        auto& fr = g_f[fid];
        auto& in = g_intent[fid];
        // A retouch's former outfit chosen in Battle Settings: the game outfit it is an option of now, in the
        // colour chosen when the carrier has it (load_state, rto::migrate_former)
        if (auto tg = g_to_game.find(fid); tg != g_to_game.end()) {
            const ToGame t = tg->second;
            g_to_game.erase(tg);
            const std::string why = ownership::outfit_label(t.costume) + ": outfit " + std::to_string(t.from)
                                  + " was a retouch, now an option of it";
            set_cos(e, t.costume, fid, why.c_str());
            if (t.color >= 0 && (!fr.base_known || fr.base_colors.count(t.color))) set_col(e, t.color, fid, why.c_str());
            v = get_cos(e);
            carriers_saved = true;   // state.json: the former slot goes
        }
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
    drop_nsfw_choices(entries);
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
        bool show = visible(s);   // the player's outfit, and not an NSFW outfit while those are hidden
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

// The carriers from the folders the scene has (fighterNNN/esfNNNv00 and ...v04), one walk: only when the costume
// table gives no answer (a game update). false while the scene has no folder.
bool carriers_from_folders() {
    if (!cache_folder_methods()) return false;
    auto* sc = current_scene();
    auto* root = sc ? call_obj(m_first_folder, sc) : nullptr;
    if (!root) return false;
    std::map<std::string, std::pair<int, int>> want;   // folder suffix -> fighter, costume
    for (int fid : g_fighters)
        for (int c : { carrier::kOutfit1, carrier::kDriveTech }) {
            char b[64]; snprintf(b, sizeof b, "fighter%03d/esf%03dv%02d", fid, fid, c);
            want[b] = { fid, c };
        }
    std::map<int, std::set<int>> has;
    walk_folders(root, 0, [&](Obj* f) {
        std::string p = fld_path(f);
        for (auto& [sfx, fc] : want) if (ends_with(p, sfx)) { has[fc.first].insert(fc.second); break; }
        return true;
    });
    for (int fid : g_fighters) { g_f[fid].costumes = has[fid]; set_carrier(g_f[fid]); }
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
                set_carrier(fr);
            }
        } else if (++g_table_passes < kTablePasses || !carriers_from_folders()) return;
        else src = "the scene's folders";
        for (int fid : g_fighters) {
            auto& fr = g_f[fid];
            if (fr.carrier < 0) {
                fr.left_out = true;
                logf("[F%d] neither Outfit 1 nor DriveTech in %s: left out of the alias", fid, src);
            }
        }
        g_carriers_done = true;
    }
    if (!m_cols) return;
    if (g_nsfw_slots)   // Outfit 1's colours: where a hidden NSFW outfit's choice goes back (drop_nsfw_choices)
        for (int fid : g_fighters) {
            auto& fr = g_f[fid];
            auto r = rid_of[fid].find(carrier::kOutfit1);
            if (!fr.o1_colors.empty() || r == rid_of[fid].end()) continue;
            if (auto* cls = call_obj(m_cols, tm, { arg_i32(r->second), arg_bool(false) })) {
                int n = list_count(cls);
                for (int j = 0; j < n; ++j) {
                    int32_t c = 0;
                    if (auto* cr = list_item(cls, j); cr && get_i32(cr, "colorNo", c)) fr.o1_colors.push_back(c);
                }
            }
        }
    // Both carriers' colours (the setting can change during the session), the current one's copied for use
    for (int fid : g_fighters) {
        auto& fr = g_f[fid];
        for (int cn : { carrier::kOutfit1, carrier::kDriveTech }) {
            auto r = rid_of[fid].find(cn);
            if (fr.colours.count(cn) || r == rid_of[fid].end()) continue;
            auto* cls = call_obj(m_cols, tm, { arg_i32(r->second), arg_bool(false) });
            if (!cls) continue;
            std::set<int> cs;
            int n = list_count(cls);
            for (int j = 0; j < n; ++j) {
                int32_t c = 0;
                if (auto* cr = list_item(cls, j); cr && get_i32(cr, "colorNo", c)) cs.insert(c);
            }
            if (!cs.empty()) fr.colours[cn] = cs;
        }
        set_carrier(fr);
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
// Online alias: the player's fighter wears its slot, the save and the network keep the carrier (Outfit 1)
// ---------------------------------------------------------------------------------------------
//
// The battle description (bFlowManager -> flow map -> m_desc) has one FighterDesc per fighter. Its Costume and
// Color are what the save and the network hold (the carrier); next to them the game keeps a local override
// (FighterDesc.SetOverwriteDesc / GetCostume / ResetOverwriteDesc) and builds the fighter's visual from it
// (BattleFlowMap.setupFighterAsset). The fighters' files are asked for with the raw Costume, one request per
// fighter (BattleFlowMap.requestNotManagedFighterAssetLoad / ...Unload(fighter, class, costume, n)).
// So when the game asks for the files of a character's carrier and the player controls that character in it with
// a slot as the intent, the slot's files are asked for too, right after the carrier's (whose request goes on: in a
// mirror on the carrier the opponent is built from it), and the player's FighterDesc gets the slot (and its colour)
// as its override at once (the VS screen builds its models from the description before the fighters, 22/10: it
// showed Outfit 1); just before the fighters are built it is checked again, kept only if the slot's files are held. The slot's files are unloaded
// with the last of the carrier's the game holds: the two requests of a mirror are the same, so the plugin counts
// them (22/10, Training: the opponent's change of outfit unloaded the slot under the player's fighter, invisible).
// Seen in Training 22/10: the override alone gave an invisible fighter (the carrier's files, the slot's visual);
// with the slot's files asked for, the player's Ken wore the slot and the other Ken, in Outfit 1 too, Outfit 1.
// Before 1.15 the alias copied the slot's visual manifest over the carrier's; the two fighters of a mirror share
// that manifest (both wore the slot, 22/10), so a mirror on the carrier was not aliased.
// The hooks run on the flow map's own thread (its scene start), which is not the LateUpdateBehavior one: they read
// and write the description there and call the game's requests themselves, as the game does on that thread (the
// alias's own state is under a lock: the tick clears it between battles).
// The game keeps an override past the battle (one survived the description's reset to its defaults, 22/10): every
// override the plugin set is removed when its slot's files are unloaded and when fighters are built without it,
// never while the fighter built from it is on screen (taken off when Battle Settings opened from Training's pause
// menu, the game crashed 18 s later, 22/10).

// The character select screen mounts and unmounts outfits itself; the runtime costume options ask (rto.cpp)
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

Method *m_fd_set_ow = nullptr, *m_fd_reset_ow = nullptr, *m_fd_get_cos = nullptr;
Method *m_req_load = nullptr, *m_req_unload = nullptr, *m_req_managed = nullptr;
std::atomic<DWORD> g_game_thread{0};   // the LateUpdateBehavior thread (tick), for the log
std::recursive_mutex g_alias_mx;       // the alias's state below: hooks and tick
bool g_menu_open = false;

// For tests: with reframework/data/SF6_CostumeSlots_data/alias_everywhere.txt present at start, the modes that pick
// costumes on a select screen are aliased too (the Training shortcut of Fighting Ground loads its fighters the way
// online matches do), so the alias can be tried offline.
bool alias_everywhere() {
    static const bool on = GetFileAttributesA("reframework/data/SF6_CostumeSlots_data/alias_everywhere.txt") != INVALID_FILE_ATTRIBUTES;
    return on;
}

// The carrier's files the game holds, per fighter and carrier: its loads minus its unloads of them (one per fighter
// wearing it). Per carrier: the setting may change while files of the former one are held.
std::map<std::pair<int, int>, int> g_held;
// Slots whose files the plugin asked for next to a carrier's and not unloaded yet, per fighter: never more than the
// game holds of that carrier, so they go once every fighter in it is gone (a mirror's opponent leaving keeps them)
struct Extra { int slot, cls, n, carrier; };
std::map<int, std::vector<Extra>> g_extra;
// Online matches ask for the fighters' files as managed files (requestManagedFighterAssetLoad, no unload: the game
// releases them itself). The slot's asked for that way, per fighter: in the current batch of requests (until the
// fighters are built), and the last one (a rematch builds the fighters again without asking for files)
std::map<int, int> g_mbatch;            // fighter -> slot files asked for as managed files since fighters were last built
std::map<int, int> g_mlive;             // fighter -> slot of the last managed request
// A new batch of managed requests (the first one after fighters were built) is a new match: the slot files of the
// last one are the game's to release, they no longer count (23/10, user log: a Custom Room match on another character,
// then back on the slot's character with the network holding Outfit 1: no request of the carrier, and the fighters
// were built with the slot's override on files released two matches before: connection error)
bool g_mbatch_open = false;
// The FighterDescs the plugin gave a slot as their override
struct Dressed { int fid, slot, color; };
std::map<Obj*, Dressed> g_dressed;
Obj* g_reload_desc = nullptr;           // the description of a reload (SetupReloadBattleAsset), until fighters are built
Obj* g_reload_map = nullptr;            // the flow map that set that reload up
thread_local bool g_in_request = false; // the plugin's own request, seen by its hooks: let through
std::atomic<bool> g_thread_told{false};
std::string g_alias_sig;                // the last alias line (logged when it changes)

int arg_int(void* a) { return static_cast<int32_t>(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a))); }

// A battle description: its fighters, which of them the player controls (online the fighter of our session member,
// offline a fighter on a pad: the training dummy and the CPU have PadId -1; a replay has none), and whether its mode
// picks costumes on a select screen (app.EGameMode, one byte: 1 Arcade, 2 Training, 3 and 4 Versus, 19 Team
// Battle), where a fighter in its carrier really wears it (Sagat's Drive Tech picked in Training, 04/10).
struct DescFighter { Obj* fd; int fid, cos, color; bool ours; int team, pad, member, ctrl; };
struct DescInfo { std::vector<DescFighter> f; int mode = 0, self = -1; bool picked = false, net = false, room = false; };
bool g_room_load = false;              // the last load was a Custom Room's training area
bool read_desc(Obj* desc, DescInfo& d, bool at_load) {
    if (!desc || !alive(desc)) return false;
    uint8_t mode = 0;
    if (auto* rule = get_obj(desc, "Rule"))
        if (auto* p = field_ptr(rule, "GameMode")) mode = *static_cast<uint8_t*>(p);
    d.mode = mode;
    d.picked = (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 19) && !alias_everywhere();
    int32_t self = -1;
    get_bool(desc, "IsNetwork", d.net);
    get_i32(desc, "SelfSessionMemberindex", self);
    d.self = self;
    auto* teams = get_obj(desc, "Teams");
    int nt = list_count(teams);
    for (int t = 0; t < nt; ++t) {
        auto* team = list_item(teams, t);
        auto* fs = team ? get_obj(team, "Fighters") : nullptr;
        int nf = list_count(fs);
        for (int i = 0; i < nf; ++i) {
            auto* f = list_item(fs, i);
            int32_t fid = 0, cos = -1, col = 0, pad = -1, member = -1, ctrl = -1;
            if (!f || !get_i32(f, "FighterId", fid)) continue;
            get_i32(f, "Costume", cos); get_i32(f, "Color", col); get_i32(f, "PadId", pad); get_i32(f, "SessionMemberIndex", member);
            if (auto* p = field_ptr(f, "CtrlType")) ctrl = *static_cast<uint8_t*>(p);
            d.f.push_back({ f, fid, cos, col, d.net ? (self >= 0 && member == self) : pad >= 0, t, pad, member, ctrl });
        }
    }
    // The training area of a Custom Room: Training (game mode 2), offline, but its fighters are loaded before any of
    // them has a pad (a Training of the player's own has P1 on pad 0, 22/10) and the player's outfit comes from
    // Battle Settings. The player is P1 (team 0). Decided when the files are asked for: the fighters are built from
    // another description, where pads may be given already (22/10).
    const bool no_pads = std::none_of(d.f.begin(), d.f.end(), [](const DescFighter& f) { return f.pad >= 0; });
    if (at_load) g_room_load = !d.net && mode == 2 && no_pads;
    if (!d.net && mode == 2 && (no_pads || (!at_load && g_room_load))) {
        d.room = true;
        d.picked = false;
        for (auto& f : d.f) f.ours = f.team == 0;
    }
    return true;
}

// The slot the player's fighter of `fid` wears over its carrier now (0: none): a slot the player sees, chosen in
// Battle Settings (never while it is open)
int wanted_slot(int fid) {
    auto in = g_intent.find(fid);
    return (in != g_intent.end() && in->second.slot && shown(fid, in->second.slot) && !g_menu_open) ? in->second.slot : 0;
}
// Offline the player's fighter comes in the save's outfit, the carrier. Online in what the network holds: the carrier,
// or another outfit when the server had another one (the Custom Room took Outfit 1 for a slot, 23/10): the slot is
// worn over it all the same (the intent is the slot, others see that outfit).
bool wears_slot(const DescInfo& d, const DescFighter& f, int slot) {
    return slot && f.ours && !d.picked && (d.net || f.cos == carrier_of(f.fid));
}
// The slot's files asked for next to the carrier's: online only the managed ones of that battle count (the not managed
// ones belong to the scene being left, a Custom Room's training area: the game unloads them while the online fighters
// load, 23/10)
bool slot_held(int fid, int slot, bool online_only = false) {
    if (auto m = g_mlive.find(fid); m != g_mlive.end() && m->second == slot) return true;
    if (online_only) return false;
    auto ex = g_extra.find(fid);
    if (ex == g_extra.end()) return false;
    for (auto& x : ex->second) if (x.slot == slot) return true;
    return false;
}
void alias_line(int fid, const char* what, const DescInfo& d) {
    char sig[512];
    snprintf(sig, sizeof sig, "F%d %s (%s, game mode %d%s)", fid, what, d.net ? "online" : "offline", d.mode, d.room ? ", a Custom Room's training area" : "");
    if (sig != g_alias_sig) { logf("costumes: alias, %s", sig); g_alias_sig = sig; }
}

// The player's fighter gets the slot (and its colour) as its override; false if the game refused it
bool dress(const DescFighter& f, int slot) {
    const int col = g_intent[f.fid].color >= 0 ? g_intent[f.fid].color : f.color;
    auto dr = g_dressed.find(f.fd);
    auto now = call(m_fd_get_cos, f.fd);
    if (dr != g_dressed.end() && dr->second.slot == slot && dr->second.color == col && !now.exception_thrown
        && static_cast<int32_t>(now.dword) == slot) return true;
    auto r = call(m_fd_set_ow, f.fd, { arg_i32(slot), arg_i32(col) });
    if (wjournal::on()) wjournal::log("ALIAS_SET fd=%p F%d v%d c%d ok=%d", (void*)f.fd, f.fid, slot, col, r.exception_thrown ? 0 : 1);
    if (r.exception_thrown) { logf("[F%d] alias: the game refused the override", f.fid); return false; }
    g_dressed[f.fd] = { f.fid, slot, col };
    ui::set_effective(f.fid, carrier_of(f.fid), slot, col);
    logf("[F%d] alias: the player's fighter wears slot v%02d colour %d over %s", f.fid, slot, col, carrier_name(f.fid).c_str());
    return true;
}

// The override the plugin set, removed (the description is the game's again)
void undress(Obj* fd, const char* why) {
    auto it = g_dressed.find(fd);
    if (it == g_dressed.end()) return;
    const Dressed w = it->second;
    g_dressed.erase(it);
    if (alive(fd)) {
        auto r = call(m_fd_reset_ow, fd);
        if (wjournal::on()) wjournal::log("ALIAS_RESET fd=%p F%d v%d ok=%d (%s)", (void*)fd, w.fid, w.slot, r.exception_thrown ? 0 : 1, why);
    }
    ui::set_effective(w.fid, carrier_of(w.fid), -1, -1);
    logf("[F%d] alias: slot v%02d taken off the player's fighter (%s)", w.fid, w.slot, why);
}

// ---- hooks: the fighters' files asked for and unloaded, the fighters built (BattleFlowMap) ----
void tell_thread() {
    if (g_thread_told.exchange(true)) return;
    logf("costumes: fighter files asked for on thread %lu (LateUpdateBehavior: %lu)", GetCurrentThreadId(), g_game_thread.load());
}

// The description a request of `map` loads fighters for: the reload that map set up, else its own (else the current
// flow map's). A reload whose fighters were never built through setupFighterAsset stayed set before 23/10, and the
// next battle's requests read it: in a Custom Room, a character changed in the training area, then an online match on
// the carrier read the training area's description (offline, game mode 2 = picked on a select screen): no alias at
// the VS screen, then the slot dressed at setupFighterAsset on the training area's files, which the game unloads
// there: connection error. A reload is now used only by its own flow map, for an offline or online battle like it.
Obj* request_desc(Obj* map) {
    Obj* own = map ? get_obj(map, "m_desc") : nullptr;
    if (!alive(own)) own = battle_desc();
    if (g_reload_desc) {
        bool rn = false, on = false;
        const bool same = map == g_reload_map && alive(g_reload_desc)
                          && (!own || (get_bool(g_reload_desc, "IsNetwork", rn), get_bool(own, "IsNetwork", on), rn == on));
        if (same) return g_reload_desc;
        logf("costumes: alias, a reload's description left behind (its fighters were not built from it): dropped");
        g_reload_desc = nullptr;
        g_reload_map = nullptr;
    }
    return own;
}

// The game asks for the files of `fid`'s carrier: the slot's files are asked for too, once per fighter of the
// player's that will wear it (the description of the battle, or of the reload being set up)
void on_carrier_load(Obj* map, int fid, int cls, int n) {
    ++g_held[{ fid, carrier_of(fid) }];
    g_mlive.erase(fid);   // this battle's files are not managed ones
    DescInfo d;
    if (!read_desc(request_desc(map), d, true)) return;
    const int slot = wanted_slot(fid);
    int need = 0;
    for (auto& f : d.f) if (f.fid == fid && wears_slot(d, f, slot)) ++need;
    if (!need) {
        std::string what = d.picked ? "picked on a select screen" : !slot ? "no slot" : "not the player's";
        if (slot && !d.picked) {   // who the fighters are, for a battle where the player could not be told apart
            auto* td = map ? map->get_type_definition() : nullptr;
            what += " [" + (td ? td->get_full_name() : std::string("?")) + ", self " + std::to_string(d.self) + ":";
            for (auto& f : d.f) what += " F" + std::to_string(f.fid) + "/c" + std::to_string(f.cos) + " pad " + std::to_string(f.pad)
                                       + " member " + std::to_string(f.member) + " ctrl " + std::to_string(f.ctrl) + ";";
            what += "]";
        }
        alias_line(fid, what.c_str(), d);
        return;
    }
    int have = 0;
    for (auto& x : g_extra[fid]) have += x.slot == slot;
    if (have >= need) return;
    g_in_request = true;
    auto r = call(m_req_load, map, { arg_i32(fid), arg_i32(cls), arg_i32(slot), arg_i32(n) });
    g_in_request = false;
    if (wjournal::on()) wjournal::log("ALIAS_LOAD map=%p F%d v%d cls=%d n=%d ok=%d", (void*)map, fid, slot, cls, n, r.exception_thrown ? 0 : 1);
    if (r.exception_thrown) { logf("[F%d] alias: the slot's files could not be asked for: no alias", fid); return; }
    g_extra[fid].push_back({ slot, cls, n, carrier_of(fid) });
    for (auto& f : d.f) if (f.fid == fid && wears_slot(d, f, slot)) dress(f, slot);
    alias_line(fid, ("slot v" + std::to_string(slot)).c_str(), d);
    logf("[F%d] alias: slot v%02d's files asked for next to %s's", fid, slot, carrier_name(fid).c_str());
}

// The game unloads the files of a carrier `cos` of `fid` it holds: the slot's asked for next to them go with the last
void on_carrier_unload(Obj* map, int fid, int cos) {
    int& held = g_held[{ fid, cos }];
    if (held > 0) --held;
    auto& ex = g_extra[fid];
    for (;;) {
        int with = 0;
        for (auto& x : ex) with += x.carrier == cos;
        if (with <= held) break;
        auto it = std::find_if(ex.rbegin(), ex.rend(), [cos](const Extra& x) { return x.carrier == cos; });
        const Extra e = *it;
        ex.erase(std::next(it).base());
        g_in_request = true;
        auto r = call(m_req_unload, map, { arg_i32(fid), arg_i32(e.cls), arg_i32(e.slot), arg_i32(e.n) });
        g_in_request = false;
        if (wjournal::on()) wjournal::log("ALIAS_UNLOAD F%d v%d ok=%d", fid, e.slot, r.exception_thrown ? 0 : 1);
        logf("[F%d] alias: slot v%02d's files unloaded with the last of %s's", fid, e.slot, ownership::outfit_label(cos).c_str());
        if (!slot_held(fid, e.slot))
            for (auto d = g_dressed.begin(); d != g_dressed.end();) {
                auto cur = d++;
                if (cur->second.fid == fid && cur->second.slot == e.slot) undress(cur->first, "its files unloaded");
            }
    }
}

int pre_asset_load(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || g_in_request || argc < 6) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int fid = arg_int(argv[2]), cos = arg_int(argv[4]);
    auto it = g_f.find(fid);
    if (it != g_f.end() && it->second.carrier >= 0 && cos == it->second.carrier)
        try { tell_thread(); std::lock_guard lk(g_alias_mx); on_carrier_load(reinterpret_cast<Obj*>(argv[1]), fid, arg_int(argv[3]), arg_int(argv[5])); }
        catch (...) { g_in_request = false; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int pre_asset_unload(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || g_in_request || argc < 6) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int fid = arg_int(argv[2]), cos = arg_int(argv[4]);
    try {
        std::lock_guard lk(g_alias_mx);
        if (auto h = g_held.find({ fid, cos }); h != g_held.end()) on_carrier_unload(reinterpret_cast<Obj*>(argv[1]), fid, cos);
    } catch (...) { g_in_request = false; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// The game asks for the files of `fid`'s carrier as managed files (online, 22/10): the slot's are asked for the same
// way, once per fighter of the player's that will wear it in this batch (the slot's intent alone is enough: online the
// network may hold another outfit than the carrier, see wears_slot)
void on_carrier_managed(Obj* map, int fid, int cls, int n) {
    DescInfo d;
    if (!read_desc(request_desc(map), d, true)) return;
    const int slot = wanted_slot(fid);
    int need = 0;
    for (auto& f : d.f) if (f.fid == fid && wears_slot(d, f, slot)) ++need;
    if (!need) { alias_line(fid, d.picked ? "picked on a select screen" : !slot ? "no slot" : "not the player's", d); return; }
    int& have = g_mbatch[fid];
    if (have >= need) return;
    g_in_request = true;
    auto r = call(m_req_managed, map, { arg_i32(fid), arg_i32(cls), arg_i32(slot), arg_i32(n) });
    g_in_request = false;
    if (wjournal::on()) wjournal::log("ALIAS_LOAD_MANAGED map=%p F%d v%d cls=%d n=%d ok=%d", (void*)map, fid, slot, cls, n, r.exception_thrown ? 0 : 1);
    if (r.exception_thrown) { logf("[F%d] alias: the slot's files could not be asked for: no alias", fid); return; }
    ++have;
    g_mlive[fid] = slot;
    for (auto& f : d.f)
        if (f.fid == fid && wears_slot(d, f, slot)) {
            if (f.cos != carrier_of(fid))
                logf("[F%d] alias: the network holds %s for the player's fighter, not %s: the slot is worn over it", fid,
                     ownership::outfit_label(f.cos).c_str(), carrier_name(fid).c_str());
            dress(f, slot);
        }
    alias_line(fid, ("slot v" + std::to_string(slot)).c_str(), d);
    logf("[F%d] alias: slot v%02d's files asked for (managed) next to %s's", fid, slot, carrier_name(fid).c_str());
}

int pre_managed_load(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || g_in_request || argc < 6) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int fid = arg_int(argv[2]), cos = arg_int(argv[4]);
    try {
        std::lock_guard lk(g_alias_mx);
        if (!g_mbatch_open) {
            g_mbatch_open = true;
            for (auto& [f, s] : g_mlive) logf("[F%d] alias: slot v%02d's managed files are the last match's: no longer counted", f, s);
            g_mlive.clear();
        }
    } catch (...) {}
    auto it = g_f.find(fid);
    if (it != g_f.end() && it->second.carrier >= 0)
        try {
            tell_thread();
            std::lock_guard lk(g_alias_mx);
            if (cos == it->second.carrier || wanted_slot(fid)) on_carrier_managed(reinterpret_cast<Obj*>(argv[1]), fid, arg_int(argv[3]), arg_int(argv[5]));
        } catch (...) { g_in_request = false; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// A reload of the fighters (a character or costume changed in the battle): its description, read by the loads it asks for
int pre_reload_setup(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || argc < 3) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(g_alias_mx);
    g_reload_map = reinterpret_cast<Obj*>(argv[1]);
    g_reload_desc = reinterpret_cast<Obj*>(argv[2]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// The fighters are built from the description (argument 1): the player's fighter in its carrier gets the slot as its
// override when the slot's files are held, and only then (the slot's visual without its files is an invisible
// fighter); every other override of the plugin's is taken off
int pre_setup_fighters(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || argc < 3) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(g_alias_mx);
    try {
        DescInfo d;
        std::set<Obj*> keep;
        if (read_desc(reinterpret_cast<Obj*>(argv[2]), d, false))
            for (auto& f : d.f) {
                const int slot = wanted_slot(f.fid);
                if (!wears_slot(d, f, slot)) continue;
                if (!slot_held(f.fid, slot, d.net)) {
                    logf("[F%d] alias: the slot's files were not asked for before the fighters were built: not aliased (%s, game mode %d)",
                         f.fid, d.net ? "online" : "offline", d.mode);
                    continue;
                }
                if (dress(f, slot)) keep.insert(f.fd);
            }
        for (auto it = g_dressed.begin(); it != g_dressed.end();) {
            auto cur = it++;
            if (!keep.count(cur->first)) undress(cur->first, "not the player's slot in these fighters");
        }
    } catch (...) {}
    g_reload_desc = nullptr;
    g_reload_map = nullptr;
    g_mbatch.clear();
    g_mbatch_open = false;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}


// ---- hooks: the fighter profile the game sends to the server (app.TemporarilyDataManager) ----
// While Battle Settings is open the save holds the slots (so the menu shows them). A character changed there sends
// the profile at once, from the save: the server got the slot's number for Mai and the Custom Room gave her Outfit 1
// (23/10, user log). Around the profile's building, sending and checking, the save holds the carriers (and a colour
// they have); the slots are put back after, for the menu.
struct Swapped { Obj* e; int fid, cos, col; };
thread_local int tl_profile_depth = 0;
std::vector<Swapped> g_swapped;         // under g_alias_mx

// The slots and colours the profile hooks took out of the save, put back (Battle Settings still open)
void put_back_swapped() {
    if (g_menu_open)
        for (auto& s : g_swapped)
            if (alive(s.e) && get_cos(s.e) == save_cos_of(s.fid)) {
                set_i32(s.e, "MatchingFighterCostume", s.cos);
                set_i32(s.e, "MatchingFighterColor", s.col);
            }
    g_swapped.clear();
}

int pre_profile(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || tl_profile_depth++ > 0 || !g_menu_open) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        std::lock_guard lk(g_alias_mx);
        int n = 0;
        for (auto& [fid, e] : save_entries()) {
            const int v = get_cos(e), c = get_col(e);
            if (!is_slot(fid, v)) continue;
            g_swapped.push_back({ e, fid, v, c });
            ++n;
            set_i32(e, "MatchingFighterCostume", save_cos_of(fid));
            const auto& fr = g_f[fid];
            if (fr.base_known && !fr.base_colors.count(c)) set_i32(e, "MatchingFighterColor", fr.base_min);
        }
        static int told = 0;
        if (n && told++ < 20)
            logf("costumes: the fighter profile is read while Battle Settings is open: %d save entr%s hold the carrier for it",
                 n, n == 1 ? "y" : "ies");
    } catch (...) {}
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_profile(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_profile_depth > 0 && --tl_profile_depth > 0) return;
    try { std::lock_guard lk(g_alias_mx); if (!g_swapped.empty()) put_back_swapped(); } catch (...) {}
}

// ---- hook: an outfit's visual manifest created (the game mounted its folder) ----
// May run on a loading thread: it only records the event (runtime costume options follow it, rto.cpp).
int pre_holder_ctor(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int64_t pt = perf::begin(); post(EV_HOLDER); perf::end(perf::HkCosHolderCtor, pt);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// ---- hook: the game's list of a fighter's outfits (CostumeInventory.GetCostumeNoList(chara)) ----
// While NSFW outfits are hidden, the fighter's NSFW outfits are taken out of the list it returns (a new list, the
// caller's only: the Color Mod adds to ColorInventory.GetColorList's the same way). Nothing else is read or written.
thread_local int tl_list_fighter = -1;
std::mutex g_list_mx;                                   // the list type's methods, resolved once
TD* g_list_td = nullptr;
Method *g_list_count = nullptr, *g_list_item = nullptr, *g_list_remove = nullptr;

int pre_costume_list(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_list_fighter = (argc >= 3 && !g_closing.load(std::memory_order_relaxed) && g_hide_nsfw.load(std::memory_order_relaxed))
                          ? static_cast<int32_t>(reinterpret_cast<uintptr_t>(argv[2])) : -1;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void drop_nsfw_from_list(Obj* list, const std::vector<uint32_t>& nos) {
    std::lock_guard lk(g_list_mx);
    TD* td = list->get_type_definition();
    if (td != g_list_td) {
        g_list_td = td;
        g_list_count = method_of(td, "get_Count", 0);
        g_list_item = method_of(td, "get_Item", 1);
        g_list_remove = method_of(td, "RemoveAt", 1);
    }
    if (!g_list_count || !g_list_item || !g_list_remove) return;
    auto rc = call(g_list_count, list);
    if (rc.exception_thrown) return;
    std::vector<uint32_t> listed;
    for (int i = 0, n = static_cast<int>(rc.dword); i < n; ++i) {
        auto r = call(g_list_item, list, { arg_i32(i) });
        if (r.exception_thrown) return;
        listed.push_back(r.dword);
    }
    for (int i : ownership::nsfw_drops(listed, nos))
        if (!call(g_list_remove, list, { arg_i32(i) }).exception_thrown) g_list_drops.fetch_add(1, std::memory_order_relaxed);
}
void post_costume_list(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int fid = tl_list_fighter;
    tl_list_fighter = -1;
    if (fid < 0 || !ret_val || !*ret_val || !g_hide_nsfw.load(std::memory_order_relaxed)) return;
    auto it = g_nsfw_nos.find(fid);
    if (it == g_nsfw_nos.end()) return;
    try { drop_nsfw_from_list(reinterpret_cast<Obj*>(*ret_val), it->second); } catch (...) {}
}

// ---- hooks: screens ----
template <perf::Id ID> int pre_menu_open(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)  { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_MENU_OPEN); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_menu_close(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_MENU_CLOSE); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_select_start(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_SELECT_START); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_select_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)   { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_SELECT_END); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_title_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)    { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_TITLE_END); perf::end(perf::HkCosTitleOnDestroy, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }

} // namespace

bool display_nsfw() { return g_show_nsfw; }

// The in-game row "Display NSFW Outfits" (nsfw_option.cpp) exists: settings.json is followed (read now) even when
// no NSFW outfit or option is installed, so the row shows the file.
void nsfw_row_present(bool on) {
    g_nsfw_row = on;
    if (on) read_nsfw_setting("row");
}

// The row was changed in the game (core callback, game thread): written to settings.json and applied at once
// (ownership and the Outfit 1 fallback run on the next tick, outside Battle Settings).
void set_display_nsfw(bool on) {
    CreateDirectoryA("reframework/data/SF6_CostumeSlots_data", nullptr);
    const bool ok = write_file(kSettings, settings_with_nsfw(on));
    if (ok) g_settings_stamp = settings_stamp();
    logf("costumes: Display NSFW Outfits %s (option row)%s", on ? "on" : "off", ok ? "" : ": settings.json NOT written");
    if (on == g_show_nsfw) return;
    set_show_nsfw(on);
    g_nsfw_apply = true;
}

bool online_drivetech() { return g_online_dt; }

// The row "Outfit Others See" was changed in the game (core callback, game thread): written to settings.json, applied on
// the next tick outside Battle Settings (carriers decided again, the saves that hold a slot's carrier moved)
void set_online_drivetech(bool on) {
    CreateDirectoryA("reframework/data/SF6_CostumeSlots_data", nullptr);
    Json v;
    v.kind = Json::String;
    v.s = on ? "DriveTech" : "Outfit 1";
    const bool ok = write_file(kSettings, settings_with("online_outfit", v));
    if (ok) g_settings_stamp = settings_stamp();
    logf("costumes: Outfit Others See: %s (option row)%s", on ? "DriveTech" : "Outfit 1", ok ? "" : ": settings.json NOT written");
    if (on == g_online_dt) return;
    g_online_dt = on;
    g_carrier_apply = true;
}

// ---------------------------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------------------------

void init() {
    load_registry();
    read_nsfw_setting("start");
    load_state();
    for (int fid : g_fighters) g_f[fid];
}

void install_hooks() {
    if (g_slots.empty()) { logf("costumes: no slot, no hook"); return; }
    auto* tdb = api().tdb();
    hook_all(tdb->find_type("app.battle.assets.FighterVisualHolder"), ".ctor", pre_holder_ctor, nullptr);
    // online alias (see "Online alias")
    auto* bfm = tdb->find_type("app.battle.BattleFlowMap");
    m_fd_set_ow = method_of(tdb->find_type("app.battle.FighterDesc"), "SetOverwriteDesc", 2);
    m_fd_reset_ow = method_of(tdb->find_type("app.battle.FighterDesc"), "ResetOverwriteDesc", 0);
    m_fd_get_cos = method_of(tdb->find_type("app.battle.FighterDesc"), "GetCostume", 0);
    m_req_load = method_of(bfm, "requestNotManagedFighterAssetLoad", 4);
    m_req_unload = method_of(bfm, "requestNotManagedFighterAssetUnload", 4);
    m_req_managed = method_of(bfm, "requestManagedFighterAssetLoad", 4);
    if (m_fd_set_ow && m_fd_reset_ow && m_fd_get_cos && m_req_load && m_req_unload && m_req_managed) {
        hook_all(bfm, "requestNotManagedFighterAssetLoad", pre_asset_load, nullptr);
        hook_all(bfm, "requestNotManagedFighterAssetUnload", pre_asset_unload, nullptr);
        hook_all(bfm, "requestManagedFighterAssetLoad", pre_managed_load, nullptr);
        hook_all(bfm, "setupFighterAsset", pre_setup_fighters, nullptr);
        hook_all(bfm, "SetupReloadBattleAsset", pre_reload_setup, nullptr);
    } else logf("costumes: the battle description's costume override is missing (game update?): no alias");
    // the fighter profile sent to the server: built, sent, checked (see "the fighter profile")
    if (auto* tdm = tdb->find_type("app.TemporarilyDataManager"))
        for (const char* m : { "CreatePostFighterProfileBody", "UploadData", "InvalidCheck" })
            hook_all(tdm, m, pre_profile, post_profile);
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
    // Only with NSFW outfits installed: a fighter's outfit list, without them while they are hidden
    if (g_nsfw_slots) hook_all(tdb->find_type("app.InventoryManager.CostumeInventory"), "GetCostumeNoList", pre_costume_list, post_costume_list);
}

bool busy() {
    if (g_slots.empty()) return false;
    if (!g_boot_done || g_revoke_cursor >= kStaticMin || g_color_cursor >= kColorMin) return true;
    return g_nsfw_apply || g_carrier_apply;
}

void tick(uint32_t ev) {
    g_game_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_slots.empty()) return;

    // ---- boot, once: every 30 frames until everything is in place (title screen, menus) ----
    if (!g_boot_done && g_frame - g_boot_last >= 30 && g_frame > 60) {
        g_boot_last = g_frame;
        read_carriers();
        insert_colors();
        insert_select_ui();
        bool owned = give_ownership("boot");
        if (owned && g_carriers_done) safety_net();   // the carriers first: the save gets the right one
        g_boot_done = (g_bc_done && g_colors_done && g_ui_done && owned) || g_frame > 60 * 600;
        if (g_boot_done) logf("costumes: boot done (colours %d, records %d, ui %d, owned %d)",
                              g_bc_done, g_colors_done, g_ui_done, owned ? 1 : 0);
    }

    // ---- sweeps of stale ownership: a slice per frame, then never again ----
    if (g_revoke_cursor >= kStaticMin) revoke_slice();
    if (g_color_cursor >= kColorMin) color_cleanup_slice();

    // ---- NSFW outfits: settings.json read again (when it changed) at every screen that lists outfits, before the
    // ownership pass below (which then follows the new setting); applied once more below the Battle Settings work
    if ((ev & (EV_TITLE_END | EV_SELECT_START | EV_SELECT_END | EV_MENU_OPEN | EV_MENU_CLOSE))
        && read_nsfw_setting("settings.json changed"))
        g_nsfw_apply = true;

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
    if (ev & (EV_MENU_OPEN | EV_MENU_CLOSE)) {
        std::lock_guard lk(g_alias_mx);
        if (ev & EV_MENU_OPEN) { g_menu_open = true; on_menu_open(); }
        if ((ev & EV_MENU_CLOSE) && g_menu_open) { put_back_swapped(); g_menu_open = false; on_menu_close(); safety_net(); }
    }
    if (ev & (EV_SELECT_START | EV_SELECT_END)) safety_net();
    if (ev & (EV_MENU_CLOSE | EV_SELECT_END))
        if (uint32_t d = g_list_drops.load(std::memory_order_relaxed); d != g_list_drops_logged) {
            logf("costumes: NSFW outfits left out of the game's outfit lists (hidden): %u so far", d);
            g_list_drops_logged = d;
        }
    // A changed NSFW setting, outside Battle Settings: ownership at once (shown / hidden in the next list), and with the
    // setting off a choice of an NSFW outfit back to Outfit 1 (safety_net, drop_nsfw_choices)
    if (g_nsfw_apply && g_boot_done && !g_menu_open) {
        g_nsfw_apply = false;
        give_ownership("Display NSFW Outfits changed");
        safety_net();
    }
    // "Outfit Others See" changed, outside Battle Settings: every carrier decided again (the alias's hooks read it, under
    // their lock), the saves that hold the former carrier for a slot moved to the new one (safety_net, carrier::stale).
    // A fighter on screen keeps what it was built from; the files asked for with the former carrier are unloaded with it.
    if (g_carrier_apply && g_carriers_done && g_bc_done && !g_menu_open) {
        g_carrier_apply = false;
        {
            std::lock_guard lk(g_alias_mx);
            for (int fid : g_fighters) set_carrier(g_f[fid]);
        }
        logf("costumes: carriers now %s", carriers_text().c_str());
        safety_net();
    }

    // ---- online alias: it works in the hooks on the fighters' files (see "Online alias"), nothing here
}

bool online() { return online_now(); }
bool select_active() { return select_screen_active(); }
int battle_mode() {
    auto* desc = battle_desc();
    auto* rule = desc ? get_obj(desc, "Rule") : nullptr;
    auto* p = rule ? field_ptr(rule, "GameMode") : nullptr;
    return p ? *static_cast<uint8_t*>(p) : -1;
}
std::string slot_label(int fighter, int costume_no) {
    auto f = g_slot_of.find(fighter);
    if (f == g_slot_of.end()) return {};
    auto c = f->second.find(costume_no);
    return c == f->second.end() ? std::string() : g_slots[c->second].label;
}
int slot_by_name(int fighter, const std::string& name) {
    auto f = g_slot_of.find(fighter);
    if (f == g_slot_of.end() || name.empty()) return -1;
    for (auto& [cos, i] : f->second) if (g_slots[i].name == name) return cos;
    return -1;
}

} // namespace slots::costumes
