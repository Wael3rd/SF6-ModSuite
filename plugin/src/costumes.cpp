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
//     the carrier included (see "Online alias" below);
//   - Random: every character's Random outfit (random_logic.hpp) is never worn; a draw takes its place where the outfit
//     worn is decided (see "Random" below). The same for a Random colour of any outfit (the Color Mod's Random value).
// Everything starts from an event (see slots.hpp); nothing is watched in between.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "rto.hpp"
#include "ownership.hpp"
#include "true_owner.hpp"
#include "manager_marker.hpp"
#include "../../common/file_sha256.hpp"
#include "../../common/manager_mode.hpp"
#include "../../common/outfit_display.hpp"
#include "carrier.hpp"
#include "codecheck_rt.hpp"
#include "colormod_link.hpp"
#include "perf.hpp"
#include "random_logic.hpp"
#include "words.hpp"
#include "wjournal.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
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
    bool random = false;             // the character's Random outfit ("random": true): never worn, a draw takes its place
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

// fighter -> its Random outfit's number (written once by load_registry, before the hooks: read from any thread)
std::map<int, int> g_random_no;
bool is_random(int fid, int cos) {
    auto r = g_random_no.find(fid);
    return r != g_random_no.end() && r->second == cos;
}
// One of our outfits a player can wear: a mod's (the Random outfit is never worn)
bool is_mod_slot(int fid, int cos) { return is_slot(fid, cos) && !is_random(fid, cos); }

// Whether the outfit each slot is made from is the player's, as the game answered (ownership.hpp)
ownership::Book g_book;

// Manager mode (common/manager_mode.hpp): the Slotsloader Manager installed this loader. The same answer as the
// loader's, from the same list (manager_installs.json) and this plugin's size and SHA-256 on disk. why: what decided
// (empty when there is no list).
bool g_managed = false;   // read once by init()
bool installed_by_manager(std::string& why) {
    why.clear();
    const std::string dir = std::string(manager_mode::kRecordDir) + "\\";
    const std::string text = read_file((dir + manager_mode::kRecordFile).c_str());
    if (text.empty()) return false;
    Json j;
    if (!json_parse(text, j) || j.kind != Json::Object) { why = "the list is not a JSON object"; return false; }
    std::vector<manager_mode::ListedFile> files;
    if (const Json* mods = j.get("mods"); mods && mods->kind == Json::Array)
        for (auto& m : mods->arr) {
            const Json* en = m.get("enabled");
            const bool on = !(en && en->kind == Json::Bool && !en->b);
            if (const Json* fs = m.get("files"); fs && fs->kind == Json::Array)
                for (auto& f : fs->arr) {
                    const Json* p = f.get("path");
                    const Json* sz = f.get("size");
                    const Json* sh = f.get("sha256");
                    files.push_back({ on, p ? p->as_str() : std::string(), sz && sz->kind == Json::Number && sz->num >= 0 ? (long long)sz->num : -1,
                                      sh && sh->kind == Json::String ? manager_mode::lower(sh->as_str()) : std::string() });
                }
        }
    const Json* fmt = j.get("format");
    const wchar_t* plugin = L"reframework\\plugins\\SF6_CostumeSlotsNative.dll";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    const long long size = GetFileAttributesExW(plugin, GetFileExInfoStandard, &fa)
        ? (long long)((uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) : -1;
    const manager_mode::Decision d = manager_mode::decide(fmt ? fmt->as_str() : std::string(), files, size,
                                                          [plugin] { return file_sha256::hex(plugin); });
    why = d.why;
    return d.manager;
}

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
std::recursive_mutex g_alias_mx;       // the alias's state (see "Online alias") and Random's below: hooks and tick

// ---- Random (random_logic.hpp; see "Random" below), under g_alias_mx ----
// What each character's Costume row lists (the Random outfit left out) and the colours each of its game outfits' Color row
// lists, read on the game thread offline (the screens that list outfits) and kept as numbers for the hooks that draw
std::map<int, std::vector<int>> g_rand_outfits;
// The same without the game outfits the player does not really have (true_owner.hpp: an unlock-all script lists them all): ranked
// matches draw from it, everything else from the whole row (Wael 10/11). The online draw made before matchmaking does not know the
// next match's mode: it follows the last online set's (ranked or none seen yet: the owned pool; another online mode: the whole row).
std::map<int, std::vector<int>> g_rand_owned;
std::map<int, std::vector<int>> g_rand_owned_seen;   // the last owned lists logged
bool g_online_owned = true;
constexpr int kRankedMatch = 14;       // app.EGameMode.RANKED_MATCH
std::map<std::pair<int, int>, std::vector<int>> g_rand_colours;
std::map<int, rnd::Fighter> g_rand;    // per character: its rounds and Battle Settings' next draw (state.json "random")
// Replays' own rounds (Wael 10/11: "two separate rounds"): Replays > Settings' Random outfit and Random colours draw from them, the
// battles from g_rand (state.json "random_replay"; no next draw)
std::map<int, rnd::Fighter> g_rand_replay;
// Online: what the player's fighter wears for the set against the current opponent (taken at the match's first load)
struct RandSet { rnd::Draw set; bool bound = false, local = false; };   // local: worn on this PC only (the alias)
std::map<int, RandSet> g_rand_set;
std::map<int, rnd::Draw> g_rand_cset;  // the same for Battle Settings' Random colour: the set's outfit and colour
// The entry the Color Mod dresses on each battle side now (its scopes 3 / 4), as asked last: [1] P1, [2] P2
struct RandReq { int fid = -1, cos = -1; std::string id; };
RandReq g_rand_req[3];
std::atomic<bool> g_draw_hooks{false}; // the battle's loads are hooked (install_hooks): a Random colour can be drawn
std::atomic<bool> g_rand_dirty{false}; // a draw since state.json was written: the tick writes it (read without the lock)
std::mt19937 g_rng{ std::random_device{}() };
// A Battle Settings choice of a retouch's former outfit (runtime options): the retouch is an option of the game
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
        // The Random outfit: a copy of Outfit 1 (owned as Outfit 1 is), never NSFW
        if (const Json* rn = e.get("random"); rn && rn->kind == Json::Bool && rn->b && !s.nsfw) {
            s.random = true;
            s.base = carrier::kOutfit1;
            g_random_no[s.fighter] = s.costume_no;
        }
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
    logf("costumes: %zu slots, %zu characters%s%s", g_slots.size(), g_fighters.size(),
         g_nsfw_slots ? (", " + std::to_string(g_nsfw_slots) + " NSFW").c_str() : "",
         g_random_no.empty() ? "" : (", a Random outfit for " + std::to_string(g_random_no.size()) + " of them").c_str());
}

// A fresh settings.json: every key with its default (select_options_*: rto_select.cpp, the costume options on the
// character select)
const char* kSettingsDefault = "{\n    \"display_nsfw\": false,\n    \"online_outfit\": \"Outfit 1\",\n"
                               "    \"select_options_button\": \"R1\",\n    \"select_options_key\": \"C\"\n}\n";
// settings.json with one key set, the other keys kept as they are (json.cpp's json_with_key)
std::string settings_with(const char* key, const Json& v) { return json_with_key(read_file(kSettings), key, v, kSettingsDefault); }
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
    // Random's rounds and next draws (a character without its Random outfit any more: its outfit draws dropped at the next
    // write; a Random colour needs none)
    for (auto& [fid, f] : rnd::parse_state(j.get("random"))) {
        if (!g_slot_of.count(fid)) continue;
        g_rand[fid] = f;
        if (!g_random_no.count(fid)) { g_rand[fid].outfits = rnd::Pool{}; g_rand[fid].next = rnd::Draw{}; }
    }
    for (auto& [fid, f] : rnd::parse_state(j.get("random_replay"))) {
        if (!g_slot_of.count(fid)) continue;
        g_rand_replay[fid] = f;
        g_rand_replay[fid].next = g_rand_replay[fid].next_colour = rnd::Draw{};
        if (!g_random_no.count(fid)) g_rand_replay[fid].outfits = rnd::Pool{};
    }
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
        // A variant slot of a modular mod folded into one outfit with options (runtime options): the
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
    o += "\n    }";
    {
        std::lock_guard lk(g_alias_mx);            // the hooks draw (Random)
        const std::string r = rnd::state_json(g_rand, 4);
        if (r != "{}") o += ",\n    \"random\": " + r;
        const std::string rr = rnd::state_json(g_rand_replay, 4);
        if (rr != "{}") o += ",\n    \"random_replay\": " + rr;
        g_rand_dirty = false;
    }
    o += "\n}\n";
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

// ---------------------------------------------------------------------------------------------
// Random (random_logic.hpp): the character's Random outfit is never worn, a draw takes its place
// ---------------------------------------------------------------------------------------------
//
// The pools: what each character's Costume row lists (GetCostumeNoList, read on the game thread offline, at the screens that
// list outfits) without the Random outfit, and without the NSFW outfits while they are hidden (checked again at each draw from
// the numbers load_registry kept and the atomic setting: the hooks draw). A colour among those the drawn outfit can show: for a
// game outfit its colour records (TableDataManager.GetFighterCostumeColors) the player has (a default one, else one in the
// save: the colours a character's level unlocks), once a session; ours have 0-9 (insert_colors). Never through
// ColorInventory.GetColorList: the Color Mod follows the last list the game asked for (where its entries sit in the Color row),
// a list asked by us in between would move them.
//   - Picked on a select screen (Arcade, Training, Versus...): a FighterDesc in Random gets its draw (Costume, Color) when the
//     game asks for the fighters' files, before the VS screen and the fighters read the description; the request in flight of the
//     Random outfit's files asks for the drawn outfit's (its argument), and so does its unload later (rand_on_load / _unload).
//   - Battle Settings (online, rooms, Battle Hub: the alias's modes): the intent is Random and its next draw is made at once
//     (before the profile the menu sends). A game outfit drawn is what the save holds outside the menu and in the profile (the
//     opponent sees it); one of ours leaves the carrier there and the alias wears it. A match's first managed load of the
//     player's fighter takes the next draw for the set (rand_bind; a rematch builds the fighters again without asking for files)
//     and makes the one after, which the save gets at the next profile built or screen that lists outfits.
bool online_now();
Obj* costume_inventory();
std::vector<uint32_t> listed_outfits(Method* m_list, Obj* ci, int fid, bool* ok);
std::map<std::pair<int, int>, int> g_game_rid;    // (fighter, game outfit) -> its first costume record (read_carriers)

// An outfit the draws may take now (any thread: g_nsfw_nos is written once by load_registry, g_hide_nsfw is atomic)
bool rand_allowed(int fid, int cos) {
    if (cos < 0 || is_random(fid, cos)) return false;
    if (!g_hide_nsfw.load(std::memory_order_relaxed)) return true;
    auto it = g_nsfw_nos.find(fid);
    return it == g_nsfw_nos.end() || std::find(it->second.begin(), it->second.end(), static_cast<uint32_t>(cos)) == it->second.end();
}
// The pool's outfits, as the Costume row last listed them (owned: the ones the player really has); never read yet: Outfit 1 alone
// (under g_alias_mx)
std::vector<int> rand_outfits_of(int fid, bool owned = false) {
    const auto& m = owned ? g_rand_owned : g_rand_outfits;
    auto it = m.find(fid);
    return it != m.end() && !it->second.empty() ? it->second : std::vector<int>{ carrier::kOutfit1 };
}
std::vector<int> rand_colours_of(int fid, int cos) {
    if (is_mod_slot(fid, cos)) return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    auto it = g_rand_colours.find({ fid, cos });
    return it != g_rand_colours.end() ? it->second : std::vector<int>{};
}
// A game outfit's colours as its Color row listed them when the game showed it (rand_seen_row: any outfit of any character,
// where the player can choose Random): for the outfits rand_read_lists did not read
std::map<std::pair<int, int>, std::vector<int>> g_row_colours;
// Every colour a draw of that outfit takes: the game's (rand_colours_of, else its Color row's, none known: Color 1) and the
// Color Mod's RW / MC entries of it, when it can dress them in a battle (as numbers: rnd::entry_value). Under g_alias_mx (the
// Color Mod's lock is taken after it).
std::vector<int> rand_draw_colours_of(int fid, int cos) {
    std::vector<int> v = rand_colours_of(fid, cos);
    if (v.empty())
        if (auto r = g_row_colours.find({ fid, cos }); r != g_row_colours.end()) v = r->second;
    if (v.empty()) v.push_back(0);
    if (colormod::battle_ok())
        for (const auto& e : colormod::entries(fid, cos)) v.push_back(rnd::entry_value(e.id));
    return v;
}
// The Color Mod's entry a colour drawn stands for ("": a game colour, or an entry gone since)
std::string rand_entry_id(int fid, int cos, int v) {
    if (!rnd::is_entry(v)) return {};
    for (const auto& e : colormod::entries(fid, cos))
        if (rnd::entry_value(e.id) == v) return e.id;
    return {};
}
// A colour drawn in the log lines: "colour 3", "RW 2"
std::string rand_colour_name(int fid, int cos, int v) {
    if (!rnd::is_entry(v)) return "colour " + std::to_string(v);
    for (const auto& e : colormod::entries(fid, cos))
        if (rnd::entry_value(e.id) == v) return e.label;
    return "an entry gone since";
}
// The row lists it (not read yet this session: a draw kept from the last one stays)
bool rand_listed(int fid, int cos, bool owned = false) {
    const auto& m = owned ? g_rand_owned : g_rand_outfits;
    auto it = m.find(fid);
    return it == m.end() || std::find(it->second.begin(), it->second.end(), cos) != it->second.end();
}
std::string rand_name(int fid, int cos) {
    if (!is_mod_slot(fid, cos)) return ownership::outfit_label(cos);
    const Slot& s = g_slots[g_slot_of.find(fid)->second.find(cos)->second];
    char b[24];
    snprintf(b, sizeof b, "v%02d", cos);
    return b + (s.label.empty() ? std::string() : " (" + s.label + ")");
}
bool rand_intent(int fid) {
    auto in = g_intent.find(fid);
    return in != g_intent.end() && is_random(fid, in->second.slot);
}

// A draw from the character's pool (under g_alias_mx). `colour_ok(costume, colour)`: what the colour may be (a mirror's left
// out; none: any). owned: only the outfits the player really has (ranked).
rnd::Draw rand_draw(int fid, const char* why, const std::function<bool(int, int)>& colour_ok = nullptr, bool owned = false,
                    bool replay = false) {
    const rnd::Rng rng = [] { return static_cast<uint32_t>(g_rng()); };
    rnd::Fighter& f = replay ? g_rand_replay[fid] : g_rand[fid];
    const rnd::Draw d = rnd::draw_outfit(f, rand_outfits_of(fid, owned), [fid](int c) { return rand_allowed(fid, c); },
                                         [fid](int c) { return rand_draw_colours_of(fid, c); }, rng, colour_ok);
    if (!d.valid()) { logf("[F%d] Random: nothing to draw (%s)", fid, why); return d; }
    g_rand_dirty = true;
    size_t pool = 0;
    for (int c : rand_outfits_of(fid, owned)) pool += rand_allowed(fid, c) ? 1 : 0;
    logf("[F%d] Random: %s %s drawn (%s; %zu of %zu outfits given in this round%s)", fid, rand_name(fid, d.costume).c_str(),
         rand_colour_name(fid, d.costume, d.color).c_str(), why, f.outfits.drawn.size(), pool, owned ? ", the player's own only" : "");
    return d;
}
// A Random colour of outfit `cos` (the Color Mod's Random value), from that outfit's round (under g_alias_mx). `ok`: what it may
// be (a mirror's colour left out; none: any).
int rand_draw_colour(int fid, int cos, const rnd::Allowed& ok, const char* why, bool replay = false) {
    const rnd::Rng rng = [] { return static_cast<uint32_t>(g_rng()); };
    rnd::Fighter& f = replay ? g_rand_replay[fid] : g_rand[fid];
    const std::vector<int> cs = rand_draw_colours_of(fid, cos);
    const int v = rnd::draw_colour(f, cos, cs, ok, rng);
    g_rand_dirty = true;
    logf("[F%d] Random colour: %s of %s drawn (%s; %zu of %zu colours given in this round)", fid, rand_colour_name(fid, cos, v).c_str(),
         rand_name(fid, cos).c_str(), why, f.colours[cos].drawn.size(), cs.size());
    return v;
}
// Battle Settings' Random colour of outfit `cos`: its next colour, made again when it was another outfit's or its entry is gone
void rand_ensure_next_colour(int fid, int cos, const char* why) {
    rnd::Fighter& f = g_rand[fid];
    const rnd::Draw& n = f.next_colour;
    if (n.costume == cos && n.color >= 0 && (!rnd::is_entry(n.color) || !rand_entry_id(fid, cos, n.color).empty())) return;
    f.next_colour = { cos, rand_draw_colour(fid, cos, nullptr, why) };
}

// The Color Mod dresses an entry drawn on a battle side (its scopes 3 / 4) until the next battle's fighters are asked for
// (rand_clear_worn). A game colour drawn: nothing to ask (the game wears it). Under g_alias_mx.
void rand_wear_entry(int side, int fid, int cos, int v, const char* why) {
    if (!rnd::is_entry(v) || (side != 1 && side != 2)) return;
    const std::string id = rand_entry_id(fid, cos, v);
    RandReq& r = g_rand_req[side];
    if (!id.empty() && r.fid == fid && r.cos == cos && r.id == id) return;   // asked already (a rematch's fighters built again)
    if (id.empty()) { logf("[F%d] Random: the entry drawn for %s is gone: colour 02 (%s)", fid, rand_name(fid, cos).c_str(), why); return; }
    if (!colormod::request_battle(side, fid, cos, id)) { logf("[F%d] Random: the Color Mod refused the entry drawn: colour 02 (%s)", fid, why); return; }
    r = { fid, cos, id };
    logf("[F%d] Random: P%d wears %s of %s, dressed by the Color Mod (%s)", fid, side, rand_colour_name(fid, cos, v).c_str(),
         rand_name(fid, cos).c_str(), why);
}
void rand_clear_worn(const char* why) {
    if (g_rand_req[1].fid < 0 && g_rand_req[2].fid < 0) return;
    colormod::clear_battle();
    g_rand_req[1] = g_rand_req[2] = RandReq{};
    logf("costumes: Random: the entries drawn for the last battle are over (%s)", why);
}
// Battle Settings' Random: its next draw, made again when it is gone from the row or hidden (NSFW) (under g_alias_mx)
void rand_ensure_next(int fid, const char* why) {
    rnd::Fighter& f = g_rand[fid];
    if (f.next.valid() && rand_allowed(fid, f.next.costume) && rand_listed(fid, f.next.costume, g_online_owned)) return;
    f.next = rand_draw(fid, why, nullptr, g_online_owned);
}
// What the save holds for Random outside the menu: its next draw when that is a game outfit, else nothing (the carrier)
rnd::Draw rand_save_draw(int fid) {
    auto it = g_rand.find(fid);
    return it == g_rand.end() ? rnd::Draw{} : rnd::save_holds(it->second.next, is_mod_slot(fid, it->second.next.costume));
}
// What the save holds outside the menu while `fid` has an intent: the carrier, or the game outfit Random drew
int held_cos_of(int fid) {
    std::lock_guard lk(g_alias_mx);
    if (rand_intent(fid))
        if (const rnd::Draw d = rand_save_draw(fid); d.valid()) return d.costume;
    return save_cos_of(fid);
}
// The save entry of a character whose intent is Random: the game outfit drawn and its colour, else the carrier in a colour it
// has (one of ours drawn: the alias wears it). Its next draw is made first when there is none.
void rand_apply_save(int fid, Obj* e, const char* why) {
    std::lock_guard lk(g_alias_mx);
    rand_ensure_next(fid, why);
    if (const rnd::Draw d = rand_save_draw(fid); d.valid()) {
        set_cos(e, d.costume, fid, why);
        set_col(e, d.color, fid, why);
        return;
    }
    set_cos(e, save_cos_of(fid), fid, why);
    auto fr = g_f.find(fid);
    if (fr != g_f.end() && fr->second.base_known && !fr->second.base_colors.count(get_col(e))) set_col(e, fr->second.base_min, fid, why);
}
// What the player's fighter of `fid` wears for Random in a battle: online the set's draw (once taken), elsewhere the next one
rnd::Draw rand_worn(int fid, bool net) {
    if (net) {
        auto s = g_rand_set.find(fid);
        return s != g_rand_set.end() && s->second.bound ? s->second.set : rnd::Draw{};
    }
    auto r = g_rand.find(fid);
    return r != g_rand.end() ? r->second.next : rnd::Draw{};
}

// ---- Battle Settings' Random colour (the Color Mod's Random value on Fighter Settings), under g_alias_mx ----
// Chosen when Battle Settings closes on it (rand_colour_close): next_colour holds its outfit and next colour from then on.
// Outside the menu the save holds that colour for a game outfit (a game colour: what the profile sends, the opponent sees it;
// an entry: its colour 02, the Color Mod dresses it here); a slot's save holds the carrier, the alias wears the colour.
const rnd::Draw* rand_colour_of(int fid) {
    auto r = g_rand.find(fid);
    return r != g_rand.end() && r->second.next_colour.valid() ? &r->second.next_colour : nullptr;
}
int intent_slot(int fid) {
    auto in = g_intent.find(fid);
    return in == g_intent.end() ? 0 : in->second.slot;
}
// Battle Settings closed: the menu showed `shown` with outfit `v`. Random (on colour 02, the Color Mod's Fighter Settings
// choice for that fighter and outfit, kept per fighter: colormod::fighter_random): its next colour kept (made now for a new
// outfit), the save holds it for a game outfit. Anything else: no Random colour for that fighter.
void rand_colour_close(int fid, Obj* e, int v, int shown) {
    const bool on = shown == rnd::kEntryCarrier && v >= 0 && !is_random(fid, v) && colormod::fighter_random(fid) == v;
    rnd::Fighter& f = g_rand[fid];
    if (!on) {
        if (!f.next_colour.valid()) return;
        f.next_colour = rnd::Draw{};
        g_rand_cset.erase(fid);
        g_rand_dirty = true;
        logf("[F%d] Random colour: no longer the choice", fid);
        return;
    }
    if (f.next_colour.costume != v) g_rand_cset.erase(fid);   // another outfit: the set (online) is taken again
    rand_ensure_next_colour(fid, v, "Random colour chosen: its next colour");
    if (!is_slot(fid, v)) set_col(e, rnd::worn_colour(f.next_colour.color), fid, "Random colour: its next colour");
}
// Outside the menu: the save of a game outfit whose colour is Random holds its next colour (a profile built, a screen that
// lists outfits); a save that holds another outfit now is left as it is
void rand_colour_save(int fid, Obj* e, const char* why) {
    const rnd::Draw* n = rand_colour_of(fid);
    if (!n || intent_slot(fid) || get_cos(e) != n->costume) return;
    set_col(e, rnd::worn_colour(n->color), fid, why);
}
// Battle Settings' Random colour of a slot the alias wears: online the set's (once taken), elsewhere the next one; -1 none
int rand_slot_colour(int fid, int slot, bool net) {
    const rnd::Draw* n = rand_colour_of(fid);
    if (!n || n->costume != slot) return -1;
    if (!net) return n->color;
    auto s = g_rand_cset.find(fid);
    return s != g_rand_cset.end() && s->second.costume == slot ? s->second.color : -1;
}

// "Display NSFW Outfits" changed: a new round of every pool (the rows list other outfits now); a next draw, or a set's, hidden
// now is dropped (the next one is made at once, the alias stops wearing the set's at the next build of the fighters)
void rand_nsfw_changed() {
    std::lock_guard lk(g_alias_mx);
    for (auto& [fid, f] : g_rand) {
        rnd::refill(f);
        if (f.next.valid() && !rand_allowed(fid, f.next.costume)) f.next = rnd::Draw{};
        g_rand_dirty = true;
    }
    for (auto& [fid, f] : g_rand_replay) rnd::refill(f);
    for (auto& [fid, s] : g_rand_set)
        if (s.bound && !rand_allowed(fid, s.set.costume)) s.bound = false;
}

// The Costume row of every character that has a Random outfit, and the colours of its game outfits (once a session each). Game
// thread, offline: the game answers with what the player owns then, its DLC answer Steam's (true_owner.hpp: an unlock-all script
// makes the game's own DLC answer yes for everything; Wael 10/11, Random never draws an outfit the player does not have). When the
// game cannot be asked that way, every game outfit given only by DLCs Steam does not count as the player's is left out.
void rand_read_lists(const char* why) {
    if (g_random_no.empty() || online_now()) return;
    Obj* ci = costume_inventory();
    static Method* m_list = nullptr;
    if (ci && !m_list) m_list = method_of(ci->get_type_definition(), "GetCostumeNoList", 1);
    if (!ci || !m_list) return;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    Method* m_cols = tm ? method_of(tm->get_type_definition(), "GetFighterCostumeColors", 2) : nullptr;
    auto* im = api().get_managed_singleton("app.InventoryManager");
    Obj* col_inv = im ? get_obj(im, "_Color") : nullptr;
    std::map<int, std::vector<int>> outs, owns;
    std::map<std::pair<int, int>, std::vector<int>> cols;
    int not_owned = 0;
    for (auto& [fid, rno] : g_random_no) {
        bool ok = false;
        const std::vector<uint32_t> listed = listed_outfits(m_list, ci, fid, &ok);
        if (!ok) continue;
        std::vector<int>& v = outs[fid];
        std::vector<int>& vo = owns[fid];
        for (uint32_t c : listed) {
            const int cos = static_cast<int>(c);
            if (cos == rno || (is_slot(fid, cos) && !shown(fid, cos)) || std::find(v.begin(), v.end(), cos) != v.end()) continue;
            v.push_back(cos);
            if (is_slot(fid, cos) || trueown::owned(fid, cos)) vo.push_back(cos);
            else ++not_owned;
        }
        for (int cos : v) {
            const std::pair<int, int> key{ fid, cos };
            if (is_slot(fid, cos)) continue;
            {
                std::lock_guard lk(g_alias_mx);
                if (g_rand_colours.count(key)) continue;
            }
            auto r = g_game_rid.find(key);
            Obj* list = (m_cols && r != g_game_rid.end()) ? call_obj(m_cols, tm, { arg_i32(r->second), arg_bool(false) }) : nullptr;
            std::vector<int>& cs = cols[key];
            for (int j = 0, n = list_count(list); j < n; ++j) {
                Obj* cr = list_item(list, j);
                int32_t no = -1, id = 0;
                if (!cr || !get_i32(cr, "colorNo", no) || no < 0 || std::find(cs.begin(), cs.end(), no) != cs.end()) continue;
                get_i32(cr, "id", id);
                const auto* def = static_cast<const uint8_t*>(field_ptr(cr, "isDefault"));
                bool owned = def && *def != 0;
                if (!owned && col_inv) {
                    bool asked = false;
                    owned = call_bool(col_inv, "ExistsSaveData", { arg_i32(id) }, &asked) || !asked;
                }
                if (owned) cs.push_back(no);
            }
        }
    }
    std::lock_guard lk(g_alias_mx);
    for (auto& [k, v] : cols) g_rand_colours[k] = std::move(v);
    if (owns != g_rand_owned_seen) {
        g_rand_owned_seen = owns;
        logf("Random: ranked matches leave out %d game outfits the player does not really have (%s)", not_owned, why);
    }
    for (auto& [fid, v] : owns) g_rand_owned[fid] = std::move(v);
    for (auto& [fid, v] : outs) {
        if (g_rand_outfits[fid] == v) continue;
        std::string line;
        for (int cos : v) {
            const std::vector<int> cs = rand_colours_of(fid, cos);
            line += (line.empty() ? "" : ", ") + rand_name(fid, cos) + " (" + std::to_string(cs.size()) + " colours)";
        }
        logf("[F%d] Random: the pool is %s (%s)", fid, line.c_str(), why);
        g_rand_outfits[fid] = std::move(v);
    }
}

// Battle Settings opened: the menu shows the slot where the save has the carrier (a hidden slot is not
// in its list: the menu shows the carrier), Random where the save has what Random drew
void on_menu_open() {
    logf("costumes: Battle Settings opened");
    for (auto& [fid, e] : save_entries()) {
        auto& in = g_intent[fid];
        // a game outfit whose colour is Random: colour 02 back in the save, the Color Mod's Random value rides on it (its
        // Fighter Settings walk to Random starts there)
        if (const rnd::Draw* n = rand_colour_of(fid); n && !in.slot && get_cos(e) == n->costume && get_col(e) != rnd::kEntryCarrier)
            set_col(e, rnd::kEntryCarrier, fid, "so the menu shows Random");
        if (!in.slot || !shown(fid, in.slot)) continue;
        if (get_cos(e) == held_cos_of(fid)) set_cos(e, in.slot, fid, is_random(fid, in.slot) ? "so the menu shows Random" : "so the menu shows the slot");
        // Random's Color row has one value (insert_colors)
        const int col = is_random(fid, in.slot) ? 0 : in.color;
        if (col >= 0 && get_col(e) != col) set_col(e, col, fid, "so the menu shows the colour");
    }
}

// Battle Settings closed: a slot chosen there becomes the intent, the save gets the carrier
void on_menu_close() {
    logf("costumes: Battle Settings closed");
    for (auto& [fid, e] : save_entries()) {
        int v = get_cos(e);
        const int col_shown = get_col(e);
        // The menu showed the carrier in place of a hidden slot: left so, the choice is kept for the
        // day the outfit is the player's again (until then the alias leaves it out)
        if (v == save_cos_of(fid) && g_intent[fid].slot && !shown(fid, g_intent[fid].slot)) continue;
        rand_colour_close(fid, e, v, col_shown);       // a Random colour (before the slot's save gets its carrier colour)
        const int chosen = is_slot(fid, v) ? v : 0;
        if (chosen != g_intent[fid].slot) g_rand_set.erase(fid);   // another choice: Random's set (online) is taken again
        set_intent_slot(fid, chosen, "chosen in the menu");
        if (is_random(fid, v)) {
            set_intent_color(fid, get_col(e), "chosen in the menu");
            rand_apply_save(fid, e, "Random chosen: its next draw");
        } else if (is_slot(fid, v)) {
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
    std::lock_guard lk(g_alias_mx);   // the intents and Random's draws, which the hooks read
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
        if (g_intent[fid].slot && fr.base_known && !is_random(fid, g_intent[fid].slot)) {
            int c = get_col(e);
            if (!fr.base_colors.count(c)) {
                set_intent_color(fid, c, "colour found in the save outside the menu");
                set_col(e, fr.base_min, fid, "safety net");
            }
        }
        // Random: the save holds its next draw (a game outfit) or the carrier, whatever was written above
        if (is_random(fid, g_intent[fid].slot)) rand_apply_save(fid, e, "Random: its next draw");
        rand_colour_save(fid, e, "Random colour: its next colour");
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
    for (auto& [fid, by] : rid_of)
        for (auto& [cn, rid] : by) g_game_rid[{ fid, cn }] = rid;     // Random's colours (rand_read_lists)
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
        // the Random outfit: one colour, the one value of its Color row (named Random: post_color_list); a list of one is what
        // the game itself gives an outfit whose other colours are not unlocked yet
        for (int c = 0; c < (g_slots[i].random ? 1 : 10); ++c) {
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
// The FighterDescs the plugin gave a slot as their override (seq: the order they were dressed in)
struct Dressed { int fid, slot, color; uint32_t seq; };
// Each FighterDesc here is held (add_ref in dress, release in undress): the plugin takes the override off later, at the next
// fighters' build, and the game may have dropped that description by then (a ranked match's: its VS screen's description
// freed before the battle's fighters were built, 09/11: ResetOverwriteDesc ran on freed memory, an access violation
// REFramework caught). Held, its memory stays valid; whether the game still holds it is game_holds.
std::map<Obj*, Dressed> g_dressed;
// A dressed description the game still holds (a reference besides the plugin's own)
bool game_holds(Obj* o) { return o && o->is_managed_object() && o->get_ref_count() > 1; }
uint32_t g_dress_seq = 0;
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
// The fighters' files asked for (requests of the game, any fighter; a reload set up) and what the last fighters were
// built from (setupFighterAsset's description): rto's VS screen search reads them (training_built)
std::atomic<uint32_t> g_fighter_requests{ 0 };
std::atomic<int> g_built_mode{ -1 };    // app.EGameMode of the last fighters' description, -1 unknown
std::atomic<bool> g_built_net{ false };
std::atomic<uint32_t> g_built_requests{ 0 };   // g_fighter_requests when they were built
// The last load the hooks saw (numbers only, read on the game thread by load_fighters): the flow map of a request of
// the fighters' files, or the description the fighters are built from (setupFighterAsset; cleared by the next request)
std::atomic<Obj*> g_load_map{ nullptr };
std::atomic<Obj*> g_load_desc{ nullptr };
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
// Battle Settings (never while it is open). Random: what it drew for this battle when this PC must wear it (`d`: online the
// set's, one of ours or a game outfit the match does not hold; elsewhere the next draw when it is one of ours: a game outfit
// drawn is in the save, the game wears it itself).
int wanted_slot(int fid, const DescInfo* d = nullptr) {
    auto in = g_intent.find(fid);
    if (in == g_intent.end() || !in->second.slot || g_menu_open) return 0;
    if (!is_random(fid, in->second.slot)) return shown(fid, in->second.slot) ? in->second.slot : 0;
    if (d && d->net) {                     // the set's draw, when this PC wears it over what the match holds (rand_bind)
        auto s = g_rand_set.find(fid);
        if (s == g_rand_set.end() || !s->second.bound || !s->second.local) return 0;
        const int c = s->second.set.costume;
        return c > 0 && rand_allowed(fid, c) && (!is_slot(fid, c) || shown(fid, c)) ? c : 0;
    }
    const rnd::Draw w = rand_worn(fid, false);
    return w.valid() && is_mod_slot(fid, w.costume) && rand_allowed(fid, w.costume) && shown(fid, w.costume) ? w.costume : 0;
}
// The colour the player's fighter wears with that slot, as drawn (rnd::is_entry: one of the Color Mod's entries): the one
// chosen with it, Random's drawn one, a Random colour's (Battle Settings), else the description's
int wanted_value(const DescInfo& d, const DescFighter& f) {
    auto in = g_intent.find(f.fid);
    if (in != g_intent.end() && is_random(f.fid, in->second.slot)) {
        const rnd::Draw w = rand_worn(f.fid, d.net);
        return w.valid() && w.color >= 0 ? w.color : f.color;
    }
    if (in != g_intent.end() && in->second.slot)
        if (const int v = rand_slot_colour(f.fid, in->second.slot, d.net); v >= 0) return v;
    return in != g_intent.end() && in->second.color >= 0 ? in->second.color : f.color;
}

// A match's first managed load of the player's fighter of `fid` (a new opponent): Random's next draw is taken for the whole set,
// unless the match holds another game outfit than the one drawn (random_logic.hpp bind_set), and the one after is made
std::set<int> g_rand_batch;             // characters whose set was taken in this batch of managed requests (until fighters are built)
void rand_bind(int fid, const DescInfo& d) {
    if (!d.net || !rand_intent(fid) || g_rand_batch.count(fid)) return;
    const DescFighter* mine = nullptr;
    for (auto& f : d.f) if (f.fid == fid && f.ours) { mine = &f; break; }
    if (!mine) return;
    g_rand_batch.insert(fid);
    if (const bool ranked = d.mode == kRankedMatch; ranked != g_online_owned) {
        g_online_owned = ranked;
        logf("Random: %s: the online draws take %s", ranked ? "a ranked match" : "an online match that is not ranked",
             ranked ? "only the outfits the player really has" : "from the whole Costume row");
    }
    rand_ensure_next(fid, "a match begins");
    rnd::Fighter& rf = g_rand[fid];
    const rnd::Bind b = rnd::bind_set(rf.next, is_mod_slot(fid, rf.next.costume), mine->cos, mine->color);
    g_rand_set[fid] = { b.set, true, b.local };
    if (b.consumed) {
        logf("[F%d] Random: this set wears %s %s%s", fid, rand_name(fid, b.set.costume).c_str(), rand_colour_name(fid, b.set.costume, b.set.color).c_str(),
             !b.local ? " (what the opponent sees)" : is_mod_slot(fid, b.set.costume) ? " (on this PC: the opponent sees what the match holds)"
                      : " (on this PC: no fighter profile sent since the draw, the opponent sees what the match holds)");
        if (!b.local) rand_wear_entry(mine->team + 1, fid, b.set.costume, b.set.color, "this set");   // (the alias's: as it dresses)
        rf.next = rand_draw(fid, "the next set", nullptr, g_online_owned);
    } else {
        logf("[F%d] Random: the match holds %s, not %s drawn (no fighter profile sent since the draw, and Outfit 1 cannot be "
             "worn over another): this set wears it, the draw waits for the next one", fid, ownership::outfit_label(mine->cos).c_str(),
             rand_name(fid, rf.next.costume).c_str());
    }
}
// The same for Battle Settings' Random colour: its next colour is taken for the set when the match holds it (random_logic.hpp
// bind_colour) or when the alias wears the outfit (a slot: on this PC whatever the match holds), and the one after is made. An
// entry is dressed by the Color Mod on this PC (the alias's: as it dresses).
std::set<int> g_rand_cbatch;            // characters whose colour set was taken in this batch of managed requests
void rand_bind_colour(int fid, const DescInfo& d) {
    const rnd::Draw* n = rand_colour_of(fid);
    if (!d.net || !n || rand_intent(fid) || g_rand_cbatch.count(fid)) return;
    const DescFighter* mine = nullptr;
    for (auto& f : d.f) if (f.fid == fid && f.ours) { mine = &f; break; }
    if (!mine) return;
    g_rand_cbatch.insert(fid);
    const int slot = intent_slot(fid);
    if (slot && slot != n->costume) return;
    const int cos = n->costume;
    rand_ensure_next_colour(fid, cos, "a match begins");
    const rnd::Draw next = *rand_colour_of(fid);
    const rnd::Bind b = slot ? rnd::Bind{ next, true, true } : rnd::bind_colour(next, mine->cos, mine->color);
    if (!b.consumed) {
        g_rand_cset.erase(fid);
        logf("[F%d] Random colour: the match holds %s colour %d, not %s drawn (no fighter profile sent since the draw): this set "
             "wears it, the draw waits for the next one", fid, ownership::outfit_label(mine->cos).c_str(), mine->color,
             rand_colour_name(fid, cos, next.color).c_str());
        return;
    }
    g_rand_cset[fid] = b.set;
    logf("[F%d] Random colour: this set wears %s of %s%s", fid, rand_colour_name(fid, cos, b.set.color).c_str(), rand_name(fid, cos).c_str(),
         slot ? " (the alias's, on this PC)" : rnd::is_entry(b.set.color) ? " (on this PC: the opponent sees colour 02)" : " (what the opponent sees)");
    if (!slot) rand_wear_entry(mine->team + 1, fid, cos, b.set.color, "this set");
    g_rand[fid].next_colour = { cos, rand_draw_colour(fid, cos, nullptr, "the next set") };
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

// The player's fighter gets the slot (and its colour, wanted_color) as its override; false if the game refused it
bool dress(const DescFighter& f, int slot, int col) {
    auto dr = g_dressed.find(f.fd);
    auto now = call(m_fd_get_cos, f.fd);
    if (dr != g_dressed.end() && dr->second.slot == slot && dr->second.color == col && !now.exception_thrown
        && static_cast<int32_t>(now.dword) == slot) return true;
    auto r = call(m_fd_set_ow, f.fd, { arg_i32(slot), arg_i32(col) });
    if (wjournal::on()) wjournal::log("ALIAS_SET fd=%p F%d v%d c%d ok=%d", (void*)f.fd, f.fid, slot, col, r.exception_thrown ? 0 : 1);
    if (r.exception_thrown) { logf("[F%d] alias: the game refused the override", f.fid); return false; }
    if (g_dressed.insert_or_assign(f.fd, Dressed{ f.fid, slot, col, ++g_dress_seq }).second) f.fd->add_ref();
    ui::set_effective(f.fid, carrier_of(f.fid), slot, col);
    logf("[F%d] alias: the player's fighter wears slot v%02d colour %d over %s", f.fid, slot, col, carrier_name(f.fid).c_str());
    return true;
}
// dress, in the colour drawn for it (wanted_value): an entry rides on colour 02 and the Color Mod dresses it on this PC
bool dress_drawn(const DescInfo& d, const DescFighter& f, int slot) {
    const int v = wanted_value(d, f);
    if (!dress(f, slot, rnd::worn_colour(v))) return false;
    rand_wear_entry(f.team + 1, f.fid, slot, v, "the alias's outfit");
    return true;
}

// What `fid` wears for the core after an override was taken off. The core keeps one effective selection per fighter and
// carrier, shared by all its FighterDescs: a FighterDesc still dressed for that fighter (and alive) keeps it (the last dressed);
// cleared only when none is left. A Custom Room match: its description is dressed when its files are asked for, then the
// training area's is undressed when the match's fighters are built: clearing there left the match's fighter without its
// slot for the Color Mod (the game colour worn instead of a custom one, 27/10).
void declare_effective(int fid) {
    const Dressed* still = nullptr;
    for (auto& [o, d] : g_dressed)
        if (d.fid == fid && (!still || d.seq > still->seq) && game_holds(o)) still = &d;
    if (!still) { ui::set_effective(fid, carrier_of(fid), -1, -1); return; }
    ui::set_effective(fid, carrier_of(fid), still->slot, still->color);
    logf("[F%d] alias: another FighterDesc of the player's still wears slot v%02d colour %d: still declared to the core",
         fid, still->slot, still->color);
}

// The override the plugin set, removed (the description is the game's again). `declare` false: the caller removes
// several and calls declare_effective once after.
void undress(Obj* fd, const char* why, bool declare = true) {
    auto it = g_dressed.find(fd);
    if (it == g_dressed.end()) return;
    const Dressed w = it->second;
    g_dressed.erase(it);
    if (fd->is_managed_object()) {                           // held since dress: valid memory, the game's or not
        auto r = call(m_fd_reset_ow, fd);
        if (wjournal::on()) wjournal::log("ALIAS_RESET fd=%p F%d v%d ok=%d (%s)", (void*)fd, w.fid, w.slot, r.exception_thrown ? 0 : 1, why);
    }
    fd->release();
    logf("[F%d] alias: slot v%02d taken off the player's fighter (%s)", w.fid, w.slot, why);
    if (declare) declare_effective(w.fid);
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
    const int slot = wanted_slot(fid, &d);
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
    for (auto& f : d.f) if (f.fid == fid && wears_slot(d, f, slot)) dress_drawn(d, f, slot);
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
        if (!slot_held(fid, e.slot)) {
            bool any = false;
            for (auto d = g_dressed.begin(); d != g_dressed.end();) {
                auto cur = d++;
                if (cur->second.fid == fid && cur->second.slot == e.slot) { undress(cur->first, "its files unloaded", false); any = true; }
            }
            if (any) declare_effective(fid);
        }
    }
}

// ---- Random, picked on a select screen: the draws of a battle (see "Random") ----
// Per character, under g_alias_mx: the outfits drawn for a FighterDesc whose request is not seen yet (until the fighters are
// built), the last one drawn (a further request of Random's number), and the outfits loaded in place of Random's number (its
// unload unloads them, in the order they were loaded)
std::map<int, std::deque<int>> g_rand_pending;
std::map<int, int> g_rand_last;
std::map<int, std::deque<int>> g_rand_loaded;

// The FighterDescs drawn for in this batch of offline requests (a colour drawn as an entry leaves Color on 02: a further request
// of the files must not draw again), until the fighters are built; a batch older than 20 s is over (a load given up)
std::set<Obj*> g_rand_done;
bool g_rand_load_open = false;
ULONGLONG g_rand_load_at = 0;
constexpr int kColourModP2 = 91;       // the Color Mod's mark of a P2 on colour 02 in a mirror (its P2_VIRTUAL_ID): 02 too

// What Battle Settings drew that the save holds and the game wears itself (a game outfit): the Random outfit's next draw, else
// the Random colour's next colour (a slot's is the alias's)
rnd::Draw rand_settings_draw(int fid) {
    if (rand_intent(fid)) {
        const rnd::Draw w = rand_worn(fid, false);
        return w.valid() && !is_mod_slot(fid, w.costume) ? w : rnd::Draw{};
    }
    const rnd::Draw* n = rand_colour_of(fid);
    return n && !intent_slot(fid) ? *n : rnd::Draw{};
}

// The draws of an offline battle (Costume and Color written as a replay's are, replay_outfits.cpp: the game asks for that
// outfit's files and builds the fighter from it itself):
//   - every FighterDesc in a Random outfit: an outfit and its colour;
//   - picked on a select screen (Arcade: the player's fighters only, the CPU's colours are the game's), a FighterDesc on colour
//     02 whose side (team + 1: the Color Mod's P1 / P2) chose the Random value for that fighter and outfit: its colour.
// A colour another fighter of the same outfit wears in this battle is not drawn (the game's own rule: no mirror in one colour;
// an entry is colour 02). An entry drawn is dressed by the Color Mod on that side (rand_wear_entry).
// Elsewhere (Battle Settings' choice, offline: Combo Trials, the hub's...): the save holds what was drawn and the game wears
// it; an entry drawn there is dressed on the player's side.
void rand_dress_desc(Obj* desc) {
    struct One { Obj* f; int fid, cos, col, team, pad; bool pending; };
    std::vector<One> v;
    uint8_t mode = 0;
    if (Obj* rule = get_obj(desc, "Rule"))
        if (auto* p = field_ptr(rule, "GameMode")) mode = *static_cast<uint8_t*>(p);
    Obj* teams = get_obj(desc, "Teams");
    for (int t = 0, nt = list_count(teams); t < nt; ++t) {
        Obj* team = list_item(teams, t);
        Obj* fs = team ? get_obj(team, "Fighters") : nullptr;
        for (int i = 0, nf = list_count(fs); i < nf; ++i) {
            Obj* f = list_item(fs, i);
            int32_t fid = 0, cos = -1, col = 0, pad = -1;
            if (!f || !get_i32(f, "FighterId", fid) || !get_i32(f, "Costume", cos)) continue;
            get_i32(f, "Color", col);
            get_i32(f, "PadId", pad);
            v.push_back({ f, fid, cos, col, t, pad, false });
        }
    }
    const bool any_pad = std::any_of(v.begin(), v.end(), [](const One& o) { return o.pad >= 0; });
    const bool picked = (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 19) && any_pad;   // (a replay has no pad)
    const bool colours = colormod::battle_ok();
    auto worn = [](int col) { return col == kColourModP2 ? rnd::kEntryCarrier : col; };
    if (!picked && colours)
        for (const One& o : v) {
            if (o.pad < 0 || worn(o.col) != rnd::kEntryCarrier || is_random(o.fid, o.cos) || g_rand_done.count(o.f)) continue;
            g_rand_done.insert(o.f);
            const rnd::Draw w = rand_settings_draw(o.fid);
            if (w.valid() && w.costume == o.cos) rand_wear_entry(o.team + 1, o.fid, o.cos, w.color, "Battle Settings' draw");
        }
    for (One& o : v) {
        if (g_rand_done.count(o.f)) continue;
        int cf = -1, cc = -1;
        o.pending = is_random(o.fid, o.cos)
                    || (picked && colours && (mode != 1 || o.pad >= 0) && worn(o.col) == rnd::kEntryCarrier
                        && colormod::random_choice(o.team + 1, cf, cc) && cf == o.fid && cc == o.cos);
    }
    // a colour (as worn) another fighter of that fighter and outfit wears, the ones still to draw left out
    auto taken = [&](const One& me, int cos, int col) {
        for (const One& o : v)
            if (&o != &me && !o.pending && o.fid == me.fid && o.cos == cos && worn(o.col) == rnd::worn_colour(col)) return true;
        return false;
    };
    for (One& o : v) {
        if (!o.pending) continue;
        const int side = o.team + 1;
        if (is_random(o.fid, o.cos)) {
            const int was = o.cos;
            const rnd::Draw d = rand_draw(o.fid, "picked on a select screen: this battle", [&](int cos, int col) { return !taken(o, cos, col); });
            o.pending = false;
            g_rand_done.insert(o.f);
            if (!d.valid()) continue;              // nothing to draw: the Random outfit's own files, Outfit 1's scene
            o.cos = d.costume;
            o.col = rnd::worn_colour(d.color);
            set_i32(o.f, "Costume", o.cos);
            set_i32(o.f, "Color", o.col);
            if (wjournal::on()) wjournal::log("RANDOM_DESC fd=%p F%d c%d -> c%d/%d", (void*)o.f, o.fid, was, d.costume, d.color);
            g_rand_pending[o.fid].push_back(d.costume);
            g_rand_last[o.fid] = d.costume;
            rand_wear_entry(side, o.fid, d.costume, d.color, "picked on a select screen: this battle");
        } else {
            const int c = rand_draw_colour(o.fid, o.cos, [&](int col) { return !taken(o, o.cos, col); }, "picked on a select screen: this battle");
            o.pending = false;
            g_rand_done.insert(o.f);
            if (!rnd::is_entry(c)) {               // a game colour: worn by the game (an entry: 02 stays, the Color Mod's P2 mark too)
                o.col = c;
                set_i32(o.f, "Color", c);
            }
            if (wjournal::on()) wjournal::log("RANDOM_COLOUR fd=%p F%d c%d -> %d", (void*)o.f, o.fid, o.cos, c);
            rand_wear_entry(side, o.fid, o.cos, c, "picked on a select screen: this battle");
        }
    }
}
// The first offline request of a battle's files: the last battle's entries are over (not for a Custom Room's training area,
// mode 2 with no pad: the room's online set keeps its own), the FighterDescs drawn for before are forgotten
void rand_batch(Obj* desc) {
    const ULONGLONG now = GetTickCount64();
    const bool fresh = !g_rand_load_open || now - g_rand_load_at > 20000;
    g_rand_load_at = now;
    if (!fresh) return;
    g_rand_load_open = true;
    g_rand_done.clear();
    uint8_t mode = 0;
    if (Obj* rule = get_obj(desc, "Rule"))
        if (auto* p = field_ptr(rule, "GameMode")) mode = *static_cast<uint8_t*>(p);
    bool pads = false;
    Obj* teams = get_obj(desc, "Teams");
    for (int t = 0, nt = list_count(teams); t < nt && !pads; ++t) {
        Obj* team = list_item(teams, t);
        Obj* fs = team ? get_obj(team, "Fighters") : nullptr;
        for (int i = 0, nf = list_count(fs); i < nf; ++i) {
            int32_t pad = -1;
            if (Obj* f = list_item(fs, i); f && get_i32(f, "PadId", pad) && pad >= 0) { pads = true; break; }
        }
    }
    if (mode != 2 || pads) rand_clear_worn("a battle's fighters asked for");
}
// The game asks for a fighter's files (an offline battle): its description's Random outfits drawn, then this request of Random's
// number asks for the drawn outfit's files (the game read the number before the description changed) - or, when it asks for a
// drawn outfit's, the game read the description again: nothing more to do for it
void rand_on_load(Obj* map, int fid, void** argv) {
    // the description request_desc gives, without dropping a reload's (the alias's requests own that); never online
    Obj* own = map ? get_obj(map, "m_desc") : nullptr;
    if (!alive(own)) own = battle_desc();
    bool net = false;
    if (alive(own) && get_bool(own, "IsNetwork", net) && net) return;
    Obj* desc = g_reload_desc && map == g_reload_map && alive(g_reload_desc) ? g_reload_desc : own;
    if (!alive(desc) || (get_bool(desc, "IsNetwork", net) && net)) return;
    rand_batch(desc);
    rand_dress_desc(desc);
    const int cos = arg_int(argv[4]);
    auto& q = g_rand_pending[fid];
    if (!is_random(fid, cos)) {
        if (auto at = std::find(q.begin(), q.end(), cos); at != q.end()) q.erase(at);
        return;
    }
    int to = -1;
    if (!q.empty()) { to = q.front(); q.pop_front(); }
    else if (auto l = g_rand_last.find(fid); l != g_rand_last.end()) to = l->second;
    if (to < 0) { logf("[F%d] Random: its files asked for with no draw: Outfit 1's scene, as its preview", fid); return; }
    argv[4] = arg_i32(to);
    g_rand_loaded[fid].push_back(to);
    if (wjournal::on()) wjournal::log("RANDOM_LOAD map=%p F%d c%d -> c%d", (void*)map, fid, cos, to);
}
// The game unloads a fighter's files: Random's number unloads what was loaded in its place (an unload of the drawn outfit's own
// number takes it off that list)
void rand_on_unload(int fid, void** argv) {
    auto it = g_rand_loaded.find(fid);
    if (it == g_rand_loaded.end() || it->second.empty()) return;
    const int cos = arg_int(argv[4]);
    auto& v = it->second;
    if (is_random(fid, cos)) {
        const int to = v.front();
        v.pop_front();
        argv[4] = arg_i32(to);
        if (wjournal::on()) wjournal::log("RANDOM_UNLOAD F%d c%d -> c%d", fid, cos, to);
    } else if (auto at = std::find(v.begin(), v.end(), cos); at != v.end()) {
        v.erase(at);
    }
}

int pre_asset_load(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || g_in_request || argc < 6) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    g_fighter_requests.fetch_add(1, std::memory_order_relaxed);
    g_load_map.store(reinterpret_cast<Obj*>(argv[1]), std::memory_order_relaxed);
    g_load_desc.store(nullptr, std::memory_order_relaxed);
    post(EV_FIGHTER_LOAD);
    if (!g_random_no.empty() || colormod::battle_ok())         // (a Random colour needs no Random outfit)
        try { std::lock_guard lk(g_alias_mx); rand_on_load(reinterpret_cast<Obj*>(argv[1]), arg_int(argv[2]), argv); } catch (...) {}
    const int fid = arg_int(argv[2]), cos = arg_int(argv[4]);
    auto it = g_f.find(fid);
    if (it != g_f.end() && it->second.carrier >= 0 && cos == it->second.carrier)
        try { tell_thread(); std::lock_guard lk(g_alias_mx); on_carrier_load(reinterpret_cast<Obj*>(argv[1]), fid, arg_int(argv[3]), arg_int(argv[5])); }
        catch (...) { g_in_request = false; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int pre_asset_unload(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || g_in_request || argc < 6) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!g_random_no.empty())
        try { std::lock_guard lk(g_alias_mx); rand_on_unload(arg_int(argv[2]), argv); } catch (...) {}
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
    rand_bind(fid, d);                                   // Random: the set against this opponent
    const int slot = wanted_slot(fid, &d);
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
            dress_drawn(d, f, slot);
        }
    alias_line(fid, ("slot v" + std::to_string(slot)).c_str(), d);
    logf("[F%d] alias: slot v%02d's files asked for (managed) next to %s's", fid, slot, carrier_name(fid).c_str());
}

int pre_managed_load(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || g_in_request || argc < 6) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    g_fighter_requests.fetch_add(1, std::memory_order_relaxed);
    g_load_map.store(reinterpret_cast<Obj*>(argv[1]), std::memory_order_relaxed);
    g_load_desc.store(nullptr, std::memory_order_relaxed);
    post(EV_FIGHTER_LOAD);
    const int fid = arg_int(argv[2]), cos = arg_int(argv[4]);
    try {
        std::lock_guard lk(g_alias_mx);
        if (!g_mbatch_open) {
            g_mbatch_open = true;
            for (auto& [f, s] : g_mlive) logf("[F%d] alias: slot v%02d's managed files are the last match's: no longer counted", f, s);
            g_mlive.clear();
            rand_clear_worn("a new match's fighters asked for");
        }
        // Battle Settings' Random colour: the set against this opponent
        if (rand_colour_of(fid) && !rand_intent(fid) && !g_rand_cbatch.count(fid)) {
            DescInfo d;
            if (read_desc(request_desc(reinterpret_cast<Obj*>(argv[1])), d, true)) rand_bind_colour(fid, d);
        }
    } catch (...) {}
    auto it = g_f.find(fid);
    if (it != g_f.end() && it->second.carrier >= 0)
        try {
            tell_thread();
            std::lock_guard lk(g_alias_mx);
            if (cos == it->second.carrier || wanted_slot(fid) || rand_intent(fid))
                on_carrier_managed(reinterpret_cast<Obj*>(argv[1]), fid, arg_int(argv[3]), arg_int(argv[5]));
        } catch (...) { g_in_request = false; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// A reload of the fighters (a character or costume changed in the battle): its description, read by the loads it asks for
int pre_reload_setup(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || argc < 3) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    g_fighter_requests.fetch_add(1, std::memory_order_relaxed);
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
    g_load_desc.store(reinterpret_cast<Obj*>(argv[2]), std::memory_order_relaxed);
    post(EV_FIGHTER_LOAD);
    std::lock_guard lk(g_alias_mx);
    try {
        DescInfo d;
        std::set<Obj*> keep;
        const bool read = read_desc(reinterpret_cast<Obj*>(argv[2]), d, false);
        g_built_mode.store(read ? d.mode : -1, std::memory_order_relaxed);
        g_built_net.store(read && d.net, std::memory_order_relaxed);
        g_built_requests.store(g_fighter_requests.load(std::memory_order_relaxed), std::memory_order_relaxed);
        if (read)
            for (auto& f : d.f) {
                const int slot = wanted_slot(f.fid, &d);
                if (!wears_slot(d, f, slot)) continue;
                if (!slot_held(f.fid, slot, d.net)) {
                    logf("[F%d] alias: the slot's files were not asked for before the fighters were built: not aliased (%s, game mode %d)",
                         f.fid, d.net ? "online" : "offline", d.mode);
                    continue;
                }
                if (dress_drawn(d, f, slot)) keep.insert(f.fd);
            }
        std::set<int> gone;
        for (auto it = g_dressed.begin(); it != g_dressed.end();) {
            auto cur = it++;
            if (!keep.count(cur->first)) { gone.insert(cur->second.fid); undress(cur->first, "not the player's slot in these fighters", false); }
        }
        for (int fid : gone) declare_effective(fid);
    } catch (...) {}
    g_reload_desc = nullptr;
    g_reload_map = nullptr;
    g_mbatch.clear();
    g_mbatch_open = false;
    g_rand_batch.clear();
    g_rand_cbatch.clear();
    g_rand_pending.clear();
    g_rand_last.clear();
    g_rand_done.clear();
    g_rand_load_open = false;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}


// ---- hooks: the fighter profile the game sends to the server (app.TemporarilyDataManager) ----
// While Battle Settings is open the save holds the slots (so the menu shows them). A character changed there sends
// the profile at once, from the save: the server got the slot's number for Mai and the Custom Room gave her Outfit 1
// (23/10, user log). Around the profile's building, sending and checking, the save holds the carriers (and a colour
// they have); the slots are put back after, for the menu.
struct Swapped { Obj* e; int fid, cos, col, held; };
thread_local int tl_profile_depth = 0;
std::vector<Swapped> g_swapped;         // under g_alias_mx
std::atomic<int> g_swapped_n{ 0 };      // its size, written under g_alias_mx: the post hook skips the lock when nothing is swapped

// The slots and colours the profile hooks took out of the save, put back (Battle Settings still open)
void put_back_swapped() {
    if (g_menu_open)
        for (auto& s : g_swapped)
            if (alive(s.e) && get_cos(s.e) == s.held) {
                set_i32(s.e, "MatchingFighterCostume", s.cos);
                set_i32(s.e, "MatchingFighterColor", s.col);
            }
    g_swapped.clear();
    g_swapped_n.store(0, std::memory_order_relaxed);
}

// Outside Battle Settings: the save of a character whose intent is Random holds its next draw when the profile is built (a draw
// made since the last screen that lists outfits: a set began online), so the profile sends it; the same for a Random colour
void rand_profile_outside_menu() {
    std::lock_guard lk(g_alias_mx);
    const bool outfit = std::any_of(g_intent.begin(), g_intent.end(), [](const auto& in) { return is_random(in.first, in.second.slot); });
    const bool colour = std::any_of(g_rand.begin(), g_rand.end(), [](const auto& r) { return r.second.next_colour.valid(); });
    if (!outfit && !colour) return;
    for (auto& [fid, e] : save_entries()) {
        if (rand_intent(fid)) rand_apply_save(fid, e, "the fighter profile is built: Random's next draw");
        else rand_colour_save(fid, e, "the fighter profile is built: the Random colour's next one");
    }
}

int pre_profile(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || tl_profile_depth++ > 0) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (!g_menu_open) {
        try { rand_profile_outside_menu(); } catch (...) {}
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
    try {
        std::lock_guard lk(g_alias_mx);
        int n = 0;
        for (auto& [fid, e] : save_entries()) {
            const int v = get_cos(e), c = get_col(e);
            if (!is_slot(fid, v)) {
                // a game outfit on the Color Mod's Random value (colour 02, its Fighter Settings choice for that fighter): the
                // profile sends the next colour (made now when there is none), the menu gets 02 back after
                if (c != rnd::kEntryCarrier || colormod::fighter_random(fid) != v) continue;
                rand_ensure_next_colour(fid, v, "Random colour chosen: its next colour");
                const int nc = rnd::worn_colour(g_rand[fid].next_colour.color);
                if (nc == c) continue;
                g_swapped.push_back({ e, fid, v, c, v });
                ++n;
                g_swapped_n.store(static_cast<int>(g_swapped.size()), std::memory_order_relaxed);
                set_i32(e, "MatchingFighterColor", nc);
                continue;
            }
            // Random: its next draw (made now when there is none: the profile the menu sends must hold it), a game outfit in
            // its colour, else the carrier
            rnd::Draw rd;
            if (is_random(fid, v)) { rand_ensure_next(fid, "Random chosen: its next draw"); rd = rand_save_draw(fid); }
            const int held = rd.valid() ? rd.costume : save_cos_of(fid);
            g_swapped.push_back({ e, fid, v, c, held });
            ++n;
            g_swapped_n.store(static_cast<int>(g_swapped.size()), std::memory_order_relaxed);
            set_i32(e, "MatchingFighterCostume", held);
            const auto& fr = g_f[fid];
            if (rd.valid()) set_i32(e, "MatchingFighterColor", rd.color);
            else if (fr.base_known && !fr.base_colors.count(c)) set_i32(e, "MatchingFighterColor", fr.base_min);
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
    if (!g_swapped_n.load(std::memory_order_relaxed)) return;     // nothing swapped (menu closed): no lock
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

// ---- hook: a Random outfit's Color row (ColorInventory.GetColorList(chara, costume)) ----
// Its one colour (insert_colors) is named Random, the game's own word in its language (words.hpp, G::Random): that entry of the
// list the game asked for, written through the core (as the Color Mod names its own entries in this list, which it hooks too:
// REFramework calls both; it adds nothing to a Random outfit's list). Hooked once the code check passed (R18).
thread_local bool tl_rand_colours = false;
std::mutex g_cl_mx;                                     // the list type's methods, resolved once
TD* g_cl_td = nullptr;
Method *g_cl_count = nullptr, *g_cl_item = nullptr;
codecheck::Batch g_cl_check;
bool g_cl_check_started = false, g_cl_hooked = false;

thread_local int tl_cl_fid = -1, tl_cl_cos = -1;      // a game outfit's Color row asked for (rand_seen_row)
std::map<std::pair<int, int>, int> g_row_len;           // under g_cl_mx: each row's length when its colours were read

int pre_color_list(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const bool on = argc >= 4 && !g_closing.load(std::memory_order_relaxed);
    const int fid = on ? arg_int(argv[2]) : -1, cos = on ? arg_int(argv[3]) : -1;
    tl_rand_colours = on && is_random(fid, cos);
    const bool game = on && fid > 0 && cos >= 0 && !is_slot(fid, cos);
    tl_cl_fid = game ? fid : -1;
    tl_cl_cos = game ? cos : -1;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// A game outfit's colours as its Color row lists them (each number once: the Color Mod's entries and Random value ride on 1),
// for the draws of that outfit (rand_colours_of). Read again when the row's length changed: the Edit Character screen asks
// for it every frame. The numbers are taken under g_cl_mx, kept under g_alias_mx (never both).
void rand_seen_row(Obj* list, int fid, int cos) {
    std::vector<int> nos;
    {
        std::lock_guard lk(g_cl_mx);
        TD* td = list->get_type_definition();
        if (td != g_cl_td) {
            g_cl_td = td;
            g_cl_count = method_of(td, "get_Count", 0);
            g_cl_item = method_of(td, "get_Item", 1);
        }
        if (!g_cl_count || !g_cl_item) return;
        auto rc = call(g_cl_count, list);
        if (rc.exception_thrown) return;
        const int n = static_cast<int>(rc.dword);
        int& len = g_row_len[{ fid, cos }];
        if (n <= 0 || n == len) return;
        for (int i = 0; i < n && i < 64; ++i) {
            Obj* ci = call_obj(g_cl_item, list, { arg_i32(i) });
            int32_t no = -1;
            if (ci && get_i32(ci, "<No>k__BackingField", no) && no >= 0 && no < rnd::kEntryMin && std::find(nos.begin(), nos.end(), no) == nos.end())
                nos.push_back(no);
        }
        len = n;
    }
    if (nos.empty()) return;
    std::lock_guard lk(g_alias_mx);
    std::vector<int>& was = g_row_colours[{ fid, cos }];
    if (was == nos) return;
    was = nos;
    logf("[F%d] Random: %s's Color row lists %zu colours", fid, ownership::outfit_label(cos).c_str(), nos.size());
}
void name_random_colours(Obj* list) {
    const SF6CoreAPI* core = ui::core();
    if (!core || !SF6CORE_HAS(core, obj_set_string)) return;
    std::lock_guard lk(g_cl_mx);
    TD* td = list->get_type_definition();
    if (td != g_cl_td) {
        g_cl_td = td;
        g_cl_count = method_of(td, "get_Count", 0);
        g_cl_item = method_of(td, "get_Item", 1);
    }
    if (!g_cl_count || !g_cl_item) return;
    auto rc = call(g_cl_count, list);
    if (rc.exception_thrown) return;
    const std::string word = words::game(words::G::Random);
    for (int i = 0, n = static_cast<int>(rc.dword); i < n && i < 16; ++i) {
        Obj* ci = call_obj(g_cl_item, list, { arg_i32(i) });
        int32_t no = -1;
        if (!ci || !get_i32(ci, "<No>k__BackingField", no) || no != 0) continue;   // (the Color Mod's entries ride on colour 1)
        if (str(get_obj(ci, "<Name>k__BackingField")) == word) continue;
        core->obj_set_string(ui::client(), ci, "<Name>k__BackingField", word.c_str());
    }
}
void post_color_list(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const bool rnd_row = tl_rand_colours;
    const int fid = tl_cl_fid, cos = tl_cl_cos;
    tl_rand_colours = false;
    tl_cl_fid = tl_cl_cos = -1;
    if ((!rnd_row && fid < 0) || !ret_val || !*ret_val || g_closing.load(std::memory_order_relaxed)) return;
    try {
        if (rnd_row) name_random_colours(reinterpret_cast<Obj*>(*ret_val));
        else rand_seen_row(reinterpret_cast<Obj*>(*ret_val), fid, cos);
    } catch (...) {}
}

// ---- hooks: screens ----
template <perf::Id ID> int pre_menu_open(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)  { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_MENU_OPEN); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_menu_close(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_MENU_CLOSE); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_select_start(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_SELECT_START); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
template <perf::Id ID> int pre_select_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)   { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_SELECT_END); perf::end(ID, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
int pre_title_end(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)    { if (g_closing.load(std::memory_order_relaxed)) return REFRAMEWORK_HOOK_CALL_ORIGINAL; int64_t pt = perf::begin(); post(EV_TITLE_END); perf::end(perf::HkCosTitleOnDestroy, pt); return REFRAMEWORK_HOOK_CALL_ORIGINAL; }

} // namespace

bool display_nsfw() { return g_show_nsfw; }
int random_outfit(int fighter) { auto r = g_random_no.find(fighter); return r == g_random_no.end() ? -1 : r->second; }

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
namespace {
// "title_pack" and "skip_intro" (the title-screen packs, read by the loader at the next launch): read from settings.json
// when its stamp changes, kept apart from the NSFW reading above because nothing here applies to the running game.
std::string g_title_pack;         // "" = the game's own, a pack key, or "random"
bool g_skip_intro = false;
uint64_t g_title_stamp = 0;
void read_title_settings() {
    const uint64_t st = settings_stamp();
    if (st == g_title_stamp) return;
    g_title_stamp = st;
    g_title_pack.clear();
    g_skip_intro = false;
    if (!st) return;
    std::string text = read_file(kSettings);
    if (text.compare(0, 3, "ï»¿") == 0) text.erase(0, 3);
    Json j;
    if (!json_parse(text, j)) return;
    if (const Json* t = j.get("title_pack"); t && t->kind == Json::String) g_title_pack = t->s;
    if (const Json* k = j.get("skip_intro"); k && k->kind == Json::Bool) g_skip_intro = k->b;
}
void write_title_setting(const char* key, const Json& v) {
    CreateDirectoryA("reframework/data/SF6_CostumeSlots_data", nullptr);
    const bool ok = write_file(kSettings, settings_with(key, v));
    if (ok) g_settings_stamp = settings_stamp();
    g_title_stamp = 0;            // read again on the next call
    logf("costumes: %s changed (option row)%s", key, ok ? "" : ": settings.json NOT written");
}
} // namespace

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

bool random_pool(std::vector<std::string>& keys) {
    keys.clear();
    std::string text = read_file(kSettings);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    Json j;
    if (text.empty() || !json_parse(text, j)) return false;
    const Json* a = j.get("title_random_pool");
    if (!a || a->kind != Json::Array) return false;
    for (const Json& k : a->arr) if (k.kind == Json::String && !k.s.empty()) keys.push_back(k.s);
    return true;
}
void set_random_pool(const std::vector<std::string>& keys) {
    Json v;
    v.kind = Json::Array;
    for (const std::string& k : keys) {
        Json e;
        e.kind = Json::String;
        e.s = k;
        v.arr.push_back(e);
    }
    write_title_setting("title_random_pool", v);
}
std::string title_pack() { read_title_settings(); return g_title_pack; }
bool skip_intro() { read_title_settings(); return g_skip_intro; }
void set_title_pack(const std::string& key) {
    Json v;
    v.kind = Json::String;
    v.s = key;
    write_title_setting("title_pack", v);
}
void set_skip_intro(bool on) {
    Json v;
    v.kind = Json::Bool;
    v.b = on;
    write_title_setting("skip_intro", v);
}
// "ui_choices" (the interface packs' ticks, ui_packs.cpp): read whenever asked (the editor opens, a tick changes), written whole
Json ui_choices() {
    std::string text = read_file(kSettings);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    Json j, out;
    out.kind = Json::Object;
    if (json_parse(text, j) && j.kind == Json::Object)
        if (const Json* c = j.get("ui_choices"); c && c->kind == Json::Object) out = *c;
    return out;
}
bool set_ui_choices(const Json& v) {
    CreateDirectoryA("reframework/data/SF6_CostumeSlots_data", nullptr);
    const bool ok = write_file(kSettings, settings_with("ui_choices", v));
    if (ok) g_settings_stamp = settings_stamp();
    g_title_stamp = 0;
    logf("costumes: ui_choices changed (%zu pack(s) with a choice)%s", v.obj.size(), ok ? "" : ": settings.json NOT written");
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------------------------

void init() {
    load_registry();
    read_nsfw_setting("start");
    load_state();
    for (int fid : g_fighters) g_f[fid];
    // Manager mode: the ownership rules of the Manager's Slotsloader, read once (they stay until the game is closed)
    std::string why;
    g_managed = installed_by_manager(why);
    if (g_managed) {
        const bool check = !marker::opt_out_present(L"reframework\\data\\SF6_CostumeSlots_data");
        g_book.set_manager_rules(check);
        logf("costumes: Manager mode (installed by the Slotsloader Manager; manager_installs.json: %s): Outfit 1 and DriveTech "
             "slots always shown%s", why.c_str(), check ? "" : ", ownership filter inactive");
    } else if (!why.empty()) {
        logf("costumes: manager_installs.json: %s: normal mode (not the Slotsloader Manager's install)", why.c_str());
    }
}

bool manager_mode() { return g_managed; }

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
        g_draw_hooks = true;
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
    // The Color rows, once the code check (R18) passed (tick): the Random outfits' one value named Random, and the colours a
    // game outfit's row lists (a Random colour's draws, any character)
    {
        TD* ci = tdb->find_type("app.InventoryManager.ColorInventory");
        g_cl_check.add(ci ? method_of(ci, "GetColorList", 2) : nullptr, "app.InventoryManager.ColorInventory.GetColorList(Int32, Int32)");
        g_cl_check.start("reframework/data/SF6_CostumeSlots_data/codecheck_random.txt", "costumes: Random's Color row");
        g_cl_check_started = true;
    }
    // Opponent Outfit Settings: the summary of a fighter with more outfits than its labels (opp_summary.cpp)
    oppsum::install_hooks();
}

bool busy() {
    if (g_slots.empty()) return false;
    if (!g_boot_done || g_revoke_cursor >= kStaticMin || g_color_cursor >= kColorMin) return true;
    if (g_cl_check_started && !g_cl_hooked) return true;     // Random's Color row: the code check's verdict not acted on yet
    return g_nsfw_apply || g_carrier_apply || g_rand_dirty.load();
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
        if (owned && g_bc_done) rand_read_lists("boot");     // Random's pools (the carriers' records first: their colours)
        if (owned && g_carriers_done) safety_net();   // the carriers first: the save gets the right one
        g_boot_done = (g_bc_done && g_colors_done && g_ui_done && owned) || g_frame > 60 * 600;
        if (g_boot_done) logf("costumes: boot done (colours %d, records %d, ui %d, owned %d)",
                              g_bc_done, g_colors_done, g_ui_done, owned ? 1 : 0);
    }

    // ---- Random's Color row: hooked once the code check (R18) passed ----
    if (g_cl_check_started && !g_cl_hooked && g_cl_check.done()) {
        g_cl_hooked = true;
        g_cl_check.log_summary();
        Method* m = g_cl_check.method(0);
        if (g_cl_check.unique(0) && m && m->add_hook(pre_color_list, post_color_list, false))
            logf("costumes: Random's Color row named Random, the Color rows' colours followed (ColorInventory.GetColorList hooked)");
        else
            logf("costumes: ColorInventory.GetColorList %s: the Random outfit's one colour keeps the game's name for it, a Random "
                 "colour draws among the colours read for the Random outfit (else Color 1) and the Color Mod's entries",
                 m ? ("refused by the code check (R18, another method enters its code: " + g_cl_check.other_label(0) + ")").c_str() : "not found");
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
        const char* why = (ev & EV_SELECT_START) ? "character select" : (ev & EV_MENU_OPEN) ? "Battle Settings" : "title screen left";
        give_ownership(why);
        rand_read_lists(why);                     // Random's pools: what the Costume rows list now
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
        rand_read_lists("Display NSFW Outfits changed");
        rand_nsfw_changed();                      // Random: a new round, a hidden draw made again (safety_net)
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

    // ---- Random: the draws the hooks made, kept (state.json) ----
    if (g_rand_dirty.load()) save_state();
}

bool online() { return online_now(); }
bool select_active() { return select_screen_active(); }
bool training_built() {
    return g_built_mode.load(std::memory_order_relaxed) == 2 && !g_built_net.load(std::memory_order_relaxed)
           && g_built_requests.load(std::memory_order_relaxed) == g_fighter_requests.load(std::memory_order_relaxed);
}
// The description of the last load (the hooks keep numbers only): the one the fighters are built from, else the one a
// request of their files reads (request_desc's choice, without dropping anything: the hooks own that)
bool load_fighters(std::vector<LoadFighter>& out, const char** why) {
    std::lock_guard lk(g_alias_mx);
    Obj* desc = g_load_desc.load(std::memory_order_relaxed);
    if (why) *why = "fighters built from their description";
    if (!alive(desc)) {
        if (why) *why = "fighters' files asked for";
        Obj* map = g_load_map.load(std::memory_order_relaxed);
        if (g_reload_desc && map == g_reload_map && alive(g_reload_desc)) desc = g_reload_desc;
        else {
            desc = alive(map) ? get_obj(map, "m_desc") : nullptr;
            if (!alive(desc)) desc = battle_desc();
        }
    }
    DescInfo d;
    if (!read_desc(desc, d, false)) return false;
    for (auto& f : d.f) {
        auto dr = g_dressed.find(f.fd);                       // the alias: the slot worn over the carrier
        const int cos = dr != g_dressed.end() ? dr->second.slot : f.cos;
        const int side = d.net ? 0 : f.team;
        if (cos >= 0 && (side == 0 || side == 1)) out.push_back({ f.fid, cos, side });
    }
    return !out.empty();
}
bool replay_random_colour(int fighter, int costume, int& out_color, std::string& entry) {
    if (fighter <= 0 || costume < 0 || is_random(fighter, costume)) return false;
    std::lock_guard lk(g_alias_mx);
    const int v = rand_draw_colour(fighter, costume, nullptr, "a replay's colour set to Random in Replays > Settings", true);
    out_color = rnd::worn_colour(v);
    entry = rand_entry_id(fighter, costume, v);
    return true;
}
bool replay_random(int fighter, int costume, int& out_costume, int& out_color, std::string& entry) {
    if (!is_random(fighter, costume)) return false;
    std::lock_guard lk(g_alias_mx);
    const rnd::Draw d = rand_draw(fighter, "a replay set to Random in Replays > Settings", nullptr, false, true);
    if (!d.valid()) return false;
    out_costume = d.costume;
    out_color = rnd::worn_colour(d.color);
    entry = rand_entry_id(fighter, d.costume, d.color);
    return true;
}
bool dressed_slot(Obj* fd, int fid, int& slot, int& color) {
    std::lock_guard lk(g_alias_mx);
    const Dressed* w = nullptr;
    if (auto it = g_dressed.find(fd); it != g_dressed.end()) w = &it->second;
    else
        for (auto& [o, d] : g_dressed)
            if (d.fid == fid && (!w || d.seq > w->seq) && game_holds(o)) w = &d;
    if (!w) return false;
    slot = w->slot;
    color = w->color;
    return true;
}
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
    // what a costume row shows: the label the loader wrote in the game's name file, or "Outfit VII" when it has none
    for (auto& [cos, i] : f->second) if (g_slots[i].name == name || outfit_display_matches(g_slots[i].label, name)) return cos;
    return -1;
}

// The Color Mod's Random value (colormod/include/sf6colormod_entries.h, SF6Slots_RandomColourFn): every outfit of a character
// this plugin manages but its Random outfit (whose one colour is Random already), while the battles' loads are hooked (the
// draws are made there); the game's word for Random
int random_colour(int fighter, int costume, char* word, int cap) {
    if (fighter <= 0 || costume < 0 || !g_draw_hooks.load(std::memory_order_relaxed) || !g_slot_of.count(fighter) || is_random(fighter, costume))
        return 0;
    if (word && cap > 0) std::snprintf(word, static_cast<size_t>(cap), "%s", words::game(words::G::Random).c_str());
    return 1;
}

// The training popup confirmed on Random where the game reloads nothing (colormod/include/sf6colormod_entries.h,
// SF6Slots_RandomNowFn): a colour of that outfit's round now, as a battle load draws one. An entry: dressed by the Color Mod on
// that side (its battle scope), the side wears 02; a game colour: worn in place by the Color Mod, an entry asked before for that
// side is over. -1: not drawn here (the Random outfit needs its files: the game reloads for it).
int random_now(int side, int fighter, int costume) {
    if ((side != 1 && side != 2) || fighter <= 0 || costume < 0 || !g_draw_hooks.load(std::memory_order_relaxed) ||
        !g_slot_of.count(fighter) || is_random(fighter, costume))
        return -1;
    std::lock_guard lk(g_alias_mx);
    const int v = rand_draw_colour(fighter, costume, nullptr, "the training popup: Random chosen, worn now");
    if (rnd::is_entry(v)) {
        rand_wear_entry(side, fighter, costume, v, "the training popup");
        return rnd::kEntryCarrier;
    }
    if (g_rand_req[side].fid >= 0) {
        colormod::clear(SF6CM_SCOPE_BATTLE_P1 + side - 1);
        g_rand_req[side] = RandReq{};
    }
    return v;
}

} // namespace slots::costumes

// Asked by the Color Mod at each Color row it fills (any thread, no game call): see random_colour above
extern "C" __declspec(dllexport) int SF6Slots_RandomColour(int fighter, int costume, char* word, int cap) {
    try { return slots::costumes::random_colour(fighter, costume, word, cap); } catch (...) { return 0; }
}
// Asked by the Color Mod when the training popup is confirmed on Random with no reload (any thread): see random_now above
extern "C" __declspec(dllexport) int SF6Slots_RandomNow(int side, int fighter, int costume) {
    try { return slots::costumes::random_now(side, fighter, costume); } catch (...) { return -1; }
}
