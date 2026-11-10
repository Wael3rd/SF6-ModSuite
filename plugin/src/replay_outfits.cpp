// SF6 Slots — native plugin: Replay Outfits (the rules: replay_outfits.hpp).
//
// Two hooks, both on the thread the game calls them on; they read the game's objects there and keep numbers only:
//   - app.BattleReplayDataManager.CreateSaveData (post): the game builds the replay of the match that just ended. For an
//     online match, one line goes to replay_journal.jsonl: the replay's key, the player's side, what the replay holds for
//     both sides and the extra outfit the player really wore (the online alias's slot). Read-only and short: the end of
//     an online match is where the game is most fragile.
//   - app.battle.ReplayFlowMap.setupDesc (post; FGReplayFlowMap inherits it): the replay's battle description is set up
//     from the replay, before the game asks for the fighters' files. Each fighter's Costume and Color are written there
//     (replay_outfits.hpp plan()), so the game asks for that outfit's files and builds the fighter from it itself, the way
//     it does for an outfit picked on a select screen. The replay's own data (ReplayInfo) is never written. Never in a
//     live battle (IsNetwork, or no replay being played). A forced fighter's Color Mod entry (RW / MC, on colour 1) or
//     its game colour is asked of the Color Mod for this replay (colormod_link.hpp); the forced stage is written too, and
//     the Replay Music row's music (Rule.BgmType: this replay's description only, the player's online setting is the save's
//     MatchingSelectBgm, never touched; Custom's list: replay_music.cpp). Its costume options are told to the runtime options
//     (rto.hpp, the player's own choices stay as they are). A fighter set to the Random outfit wears a draw of its pool, one
//     per playback (costumes::replay_random: the Random outfit's rules, its colour among the outfit's and the Color Mod's); a
//     fighter whose colour is Random, a colour of its outfit's round (costumes::replay_random_colour), one per playback too.
#include "slots.hpp"
#include "replay_outfits.hpp"
#include "colormod_link.hpp"
#include "replay_music.hpp"
#include "rto.hpp"

#include <ctime>
#include <mutex>

namespace slots::replay {
namespace {

const char* kOutfits  = "reframework/data/SF6_CostumeSlots_data/replay_outfits.json";
const char* kJournal  = "reframework/data/SF6_CostumeSlots_data/replay_journal.jsonl";
const char* kRegistry = "reframework/data/SF6_Costumes_Data/registry.json";

std::mutex g_mx;                                    // everything below: the hooks may run on any thread
std::vector<RegSlot> g_reg;   uint64_t g_reg_stamp = 0;
std::map<int, Forced> g_forced; uint64_t g_forced_stamp = 0;
int g_stage = -1;                                   // the forced stage (StageDesc.StageId), -1: the replay's own
BgmChoice g_bgm;                                    // the Replay Music row
std::map<std::string, Entry> g_journal; uint64_t g_journal_stamp = 0;
std::string g_last_written;                         // the last journal key written (one line per match)
std::mutex g_list_mx;                               // g_list_methods (read before g_mx is taken)
std::unordered_map<TD*, std::pair<Method*, Method*>> g_list_methods;   // get_Count, get_Item per collection type
Method* m_get_replay = nullptr;                     // BattleReplayDataManager.GetReplayData()

// A file's date and size in one number (0: no file)
uint64_t stamp(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
}

// The registry, the tab's choices and the journal, read again when their file changed (under g_mx)
void refresh() {
    if (uint64_t s = stamp(kRegistry); s != g_reg_stamp) {
        g_reg = parse_registry(read_file(kRegistry, 16u << 20));
        g_reg_stamp = s;
    }
    if (uint64_t s = stamp(kOutfits); s != g_forced_stamp) {
        std::map<int, Forced> f;
        int stage = -1;
        BgmChoice bgm;
        if (!s) { g_forced.clear(); g_stage = -1; g_bgm = {}; g_forced_stamp = 0; }
        else if (parse_outfits(read_file(kOutfits), f, &stage, &bgm)) {
            g_forced = std::move(f);
            g_stage = stage;
            g_bgm = bgm;
            g_forced_stamp = s;
            logf("replay outfits: replay_outfits.json read, %zu character%s forced, stage %s", g_forced.size(),
                 g_forced.size() == 1 ? "" : "s", stage >= 0 ? std::to_string(stage).c_str() : "the replay's own");
        } else logf("replay outfits: replay_outfits.json unreadable: the last choices kept");   // read again next time
    }
    if (uint64_t s = stamp(kJournal); s != g_journal_stamp) {
        g_journal.clear();
        size_t n = s ? parse_journal(read_file(kJournal, 64u << 20), g_journal) : 0;
        g_journal_stamp = s;
        logf("replay outfits: journal read, %zu match%s", n, n == 1 ? "" : "es");
    }
}

// Collections (List`1 and arrays): their methods looked up once per type, under their own lock (common.cpp's cache is
// the game thread's)
std::pair<Method*, Method*> list_methods(Obj* list) {
    auto* td = list ? list->get_type_definition() : nullptr;
    if (!td) return { nullptr, nullptr };
    std::lock_guard lk(g_list_mx);
    auto it = g_list_methods.find(td);
    if (it == g_list_methods.end())
        it = g_list_methods.emplace(td, std::make_pair(method_of(td, "get_Count", 0), method_of(td, "get_Item", 1))).first;
    return it->second;
}
int count_of(Obj* list) {
    auto m = list_methods(list);
    if (!m.first) return 0;
    auto r = call(m.first, list);
    return r.exception_thrown ? 0 : static_cast<int>(r.dword);
}
Obj* item_of(Obj* list, int i) {
    auto m = list_methods(list);
    return m.second ? call_obj(m.second, list, { arg_i32(i) }) : nullptr;
}

// app.ReplayInfo: the key's facts and what it holds per side (ReplayInfo.Fighters order)
bool read_info(Obj* ri, ReplayFacts& rf, std::vector<Side>& sides) {
    if (!alive(ri)) return false;
    int32_t stage = 0;
    if (!get_u32(ri, "RandomSeed", rf.seed) || !get_i32(ri, "StageId", stage)) return false;
    rf.stage = stage;
    Obj* fs = get_obj(ri, "Fighters");
    for (int i = 0, n = count_of(fs); i < n; ++i) {
        Obj* f = item_of(fs, i);
        Side s;
        int32_t v = 0;
        if (!f || !get_i32(f, "FighterID", v)) return false;
        s.fid = v;
        if (get_i32(f, "Costume", v)) s.costume = v;
        if (get_i32(f, "Color", v)) s.color = v;
        rf.fighters.push_back(s.fid);
        sides.push_back(s);
    }
    Obj* rs = get_obj(ri, "RoundInfo");
    for (int i = 0, n = count_of(rs); i < n; ++i) {
        Obj* r = item_of(rs, i);
        auto* p = r ? field_ptr(r, "RandomSeed") : nullptr;
        if (!p) return false;
        uint16_t s16 = 0;
        memcpy(&s16, p, 2);
        rf.round_seeds.push_back(s16);
    }
    return true;
}

// The battle description's first fighter of each team (team order: P1, P2)
struct DescSide { Obj* fd = nullptr; Side s; int member = -1; };
std::vector<DescSide> desc_sides(Obj* desc) {
    std::vector<DescSide> out;
    Obj* teams = get_obj(desc, "Teams");
    for (int t = 0, nt = count_of(teams); t < nt; ++t) {
        Obj* team = item_of(teams, t);
        Obj* fs = team ? get_obj(team, "Fighters") : nullptr;
        Obj* fd = count_of(fs) > 0 ? item_of(fs, 0) : nullptr;
        DescSide d;
        int32_t v = 0;
        if (fd && get_i32(fd, "FighterId", v)) {
            d.fd = fd;
            d.s.fid = v;
            if (get_i32(fd, "Costume", v)) d.s.costume = v;
            if (get_i32(fd, "Color", v)) d.s.color = v;
            if (get_i32(fd, "SessionMemberIndex", v)) d.member = v;
        }
        out.push_back(d);
    }
    return out;
}

int game_mode(Obj* desc) {
    auto* rule = desc ? get_obj(desc, "Rule") : nullptr;
    auto* p = rule ? field_ptr(rule, "GameMode") : nullptr;
    return p ? *static_cast<uint8_t*>(p) : -1;
}

// ---- the journal: the replay built at the end of a match ----
thread_local Obj* tl_save_desc = nullptr;

int pre_create_save(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_save_desc = (argc >= 3 && !g_closing.load(std::memory_order_relaxed)) ? reinterpret_cast<Obj*>(argv[2]) : nullptr;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void journal_match(Obj* desc, Obj* save) {
    bool net = false;
    if (!alive(desc) || !get_bool(desc, "IsNetwork", net) || !net || !alive(save)) return;
    Obj* data = get_obj(save, "BattleReplayData");
    ReplayFacts rf;
    Entry e;
    if (!read_info(data ? get_obj(data, "ReplayInfo") : nullptr, rf, e.sides)) { logf("replay outfits: journal, the match's replay could not be read"); return; }
    e.key = replay_key(rf);
    if (e.key.empty()) return;
    e.time = static_cast<long long>(_time64(nullptr));
    e.mode = game_mode(desc);
    // The player's side: the team of our session member; the replay's sides are in team order (checked by fighter)
    int32_t self = -1;
    get_i32(desc, "SelfSessionMemberindex", self);
    auto ds = desc_sides(desc);
    Obj* my_fd = nullptr;
    for (size_t t = 0; t < ds.size(); ++t)
        if (ds[t].fd && self >= 0 && ds[t].member == self) { e.me = static_cast<int>(t); my_fd = ds[t].fd; }
    if (e.me < 0 || e.me >= static_cast<int>(e.sides.size()) || e.sides[e.me].fid != ds[e.me].s.fid) {
        logf("replay outfits: journal, the player's side is not known (self %d, %zu teams): not written", self, ds.size());
        return;
    }
    // What the player really wore: the alias's slot on the player's fighter, named by its mod
    int slot = -1, color = 0;
    if (costumes::dressed_slot(my_fd, e.sides[e.me].fid, slot, color)) {
        std::lock_guard lk(g_mx);
        refresh();
        if (const RegSlot* rs = slot_of(g_reg, e.sides[e.me].fid, slot)) {
            e.has_slot = true;
            e.mod_id = rs->mod_id;
            e.slot_costume = slot;
            e.slot_color = color;
            e.slot_base = rs->base;
        }
    }
    std::lock_guard lk(g_mx);
    if (e.key == g_last_written) return;
    const std::string line = journal_line(e);
    FILE* f = fopen(kJournal, "ab");
    if (!f) { logf("replay outfits: cannot write the journal"); return; }
    const bool ok = fwrite(line.data(), 1, line.size(), f) == line.size();
    fclose(f);
    if (!ok) { logf("replay outfits: cannot write the journal"); return; }
    g_last_written = e.key;
    g_journal[e.key] = e;
    g_journal_stamp = stamp(kJournal);
    logf("replay outfits: journal, match %s (game mode %d): P%d F%d %s", e.key.c_str(), e.mode, e.me + 1, e.sides[e.me].fid,
         e.has_slot ? ("wore slot v" + std::to_string(e.slot_costume) + " colour " + std::to_string(e.slot_color)).c_str()
                    : "wore what the replay holds");
}

void post_create_save(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    Obj* desc = tl_save_desc;
    tl_save_desc = nullptr;
    if (!desc || !ret_val || !*ret_val || g_closing.load(std::memory_order_relaxed)) return;
    try { journal_match(desc, reinterpret_cast<Obj*>(*ret_val)); } catch (...) { logf("replay outfits: EXCEPTION in the journal"); }
}

// ---- the replay's description ----
thread_local Obj* tl_replay_map = nullptr;

int pre_setup_desc(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_replay_map = (argc >= 2 && !g_closing.load(std::memory_order_relaxed)) ? reinterpret_cast<Obj*>(argv[1]) : nullptr;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void dress_replay(Obj* map) {
    Obj* desc = alive(map) ? get_obj(map, "m_desc") : nullptr;
    bool net = false;
    if (!alive(desc) || (get_bool(desc, "IsNetwork", net) && net)) return;
    auto* brdm = api().get_managed_singleton("app.BattleReplayDataManager");
    Obj* rd = (brdm && m_get_replay) ? call_obj(m_get_replay, brdm) : nullptr;
    Obj* data = alive(rd) ? get_obj(rd, "ReplayData") : nullptr;
    ReplayFacts rf;
    std::vector<Side> held;
    if (!read_info(data ? get_obj(data, "ReplayInfo") : nullptr, rf, held)) { logf("replay outfits: a replay with no readable ReplayInfo: left alone"); return; }
    const std::string key = replay_key(rf);
    auto ds = desc_sides(desc);
    std::vector<Side> recorded;
    for (auto& d : ds) {
        if (!d.fd) { logf("replay outfits: a replay side with no fighter: left alone"); return; }
        recorded.push_back(d.s);
    }
    std::lock_guard lk(g_mx);
    refresh();
    auto je = g_journal.find(key);
    const Entry* entry = je != g_journal.end() ? &je->second : nullptr;
    auto wear = plan(recorded, entry, g_forced, g_reg);
    // A fighter set to its Random outfit: a draw of its pool for this playback (the Random outfit's rules), its colour one of
    // the outfit's or a Color Mod entry (asked below, on colour 1, as a forced entry is). A fighter whose colour is Random (any
    // other outfit): a colour of that outfit's round, the same way. The same flow map set up again within a few seconds (a
    // derived flow map's setupDesc calling its base's: both hooked) keeps its draws.
    static uintptr_t s_map = 0;
    static ULONGLONG s_at = 0;
    struct Drawn { int forced = -1; bool colour = false; int cos = -1, col = 0; std::string cm; };
    static std::map<size_t, Drawn> s_drawn;                     // side -> the outfit forced, what was drawn for it
    const ULONGLONG now = GetTickCount64();
    if (s_map != reinterpret_cast<uintptr_t>(map) || now - s_at > 5000) s_drawn.clear();
    s_map = reinterpret_cast<uintptr_t>(map);
    s_at = now;
    for (size_t i = 0; i < ds.size(); ++i) {
        Wear& w = wear[i];
        int cos = -1, col = 0;
        std::string cm;
        if (!w.forced) continue;
        bool colour = false;
        if (auto d = s_drawn.find(i); d != s_drawn.end() && d->second.forced == w.costume && d->second.colour == w.random_color) {
            cos = d->second.cos; col = d->second.col; cm = d->second.cm; colour = d->second.colour;
        } else if (costumes::replay_random(recorded[i].fid, w.costume, cos, col, cm)) {
            s_drawn[i] = { w.costume, w.random_color, cos, col, cm };
        } else if (w.random_color && costumes::replay_random_colour(recorded[i].fid, w.costume, col, cm)) {
            cos = w.costume;
            colour = true;
            s_drawn[i] = { w.costume, true, cos, col, cm };
        } else continue;
        w.costume = cos;
        w.color = col;
        w.colormod = cm;
        w.why = colour ? "forced: Random colour" : "forced: Random";
        w.change = cos != recorded[i].costume || col != recorded[i].color;
    }
    std::string line;
    for (size_t i = 0; i < ds.size(); ++i) {
        char b[160];
        const Wear& w = wear[i];
        if (w.change) {
            set_i32(ds[i].fd, "Costume", w.costume);
            set_i32(ds[i].fd, "Color", w.color);
            snprintf(b, sizeof b, "%sP%zu F%d c%d/%d -> c%d/%d (%s)", i ? ", " : "", i + 1, recorded[i].fid, recorded[i].costume,
                     recorded[i].color, w.costume, w.color, w.why);
        } else snprintf(b, sizeof b, "%sP%zu F%d c%d/%d (%s)", i ? ", " : "", i + 1, recorded[i].fid, recorded[i].costume, recorded[i].color, w.why);
        line += b;
    }
    // The Color Mod (colormod_link.hpp): a forced fighter wears exactly the colour chosen, over the player's own Color
    // Mod choices (never changed), for this replay only: the request names this replay's flow map and the Color Mod drops
    // it once another one runs. Its entry (RW / MC) on colour 1, or "" = the game's own colours (not the player's MC
    // choice on 02). Earlier requests (another replay) go first.
    colormod::clear(SF6CM_SCOPE_REPLAY);
    for (size_t i = 0; i < ds.size(); ++i) {
        const Wear& w = wear[i];
        if (!w.forced) continue;
        const bool ok = colormod::request(SF6CM_SCOPE_REPLAY, recorded[i].fid, w.costume, w.colormod, map);
        if (!w.colormod.empty() || ok)
            line += ", P" + std::to_string(i + 1) + " Color Mod: " + (w.colormod.empty() ? std::string("game colours") : w.colormod) +
                    (ok ? "" : " (no Color Mod)");
    }
    // The costume options: a forced fighter wears what is saved for its outfit, or that outfit's defaults, over the player's
    // own choices (never written), by fighter so on whichever side it plays, for this replay only (its flow map, as the Color
    // Mod's requests); their files are asked for now, before the game asks for the fighters'.
    std::map<int, rto::ForcedFighter> forced_opts;
    for (size_t i = 0; i < ds.size(); ++i) {
        const Wear& w = wear[i];
        if (!w.forced) continue;
        rto::ForcedFighter& ff = forced_opts[recorded[i].fid];
        ff.costume = w.costume;
        if (auto f = g_forced.find(recorded[i].fid); f != g_forced.end()) ff.by_mod = f->second.options;
    }
    if (forced_opts.empty()) rto::replay_wear_clear();
    else rto::replay_wear_set(std::move(forced_opts), reinterpret_cast<uintptr_t>(map));
    // The stage: the description's StageDesc.StageId (the replay's ReplayInfo.StageId is left as recorded); the game loads
    // the stage of the description. Only the stage's look changes: every battle stage is as wide as the others.
    if (g_stage >= 0) {
        int32_t recorded_stage = -1;
        Obj* sd = get_obj(desc, "Stage");
        if (sd && get_i32(sd, "StageId", recorded_stage)) {
            if (recorded_stage != g_stage) set_i32(sd, "StageId", g_stage);
            line += ", stage " + std::to_string(recorded_stage) + " -> " + std::to_string(g_stage);
        } else line += ", stage: no StageDesc (left alone)";
    }
    // The music: the game picks it from the description when it loads the battle (BattleFlowMap.loadBgmPrefab, one byte,
    // app.EBgmSet). Random: left as the game set it. Custom: its tracks still owned, Player1 with that list in place of its
    // playlist for this flow map's load; none left, or no swap: as Random.
    BgmChoice music = g_bgm;
    if (music.mode == BgmMode::Custom) {
        music.list = replaymusic::can_swap() ? bgm_keep_owned(music.list, replaymusic::owned_tracks()) : std::vector<uint32_t>{};
        line += ", Custom list " + std::to_string(music.list.size());
    }
    replaymusic::arm_replay(music.mode == BgmMode::Custom && !music.list.empty() ? map : nullptr, music.list);
    if (const int set = bgm_set(music); set >= 0) {
        Obj* rule = get_obj(desc, "Rule");
        auto* p = rule ? static_cast<uint8_t*>(field_ptr(rule, "BgmType")) : nullptr;
        if (p) {
            line += ", music " + std::to_string(*p) + " -> " + std::to_string(set);
            *p = static_cast<uint8_t>(set);
        } else line += ", music: no BattleRule.BgmType (left alone)";
    }
    logf("replay outfits: replay %s%s: %s", key.c_str(), entry ? " (in the journal)" : "", line.c_str());
}

void post_setup_desc(void** /*ret_val*/, REFrameworkTypeDefinitionHandle, unsigned long long) {
    Obj* map = tl_replay_map;
    tl_replay_map = nullptr;
    if (!map || g_closing.load(std::memory_order_relaxed)) return;
    try { dress_replay(map); } catch (...) { logf("replay outfits: EXCEPTION at the replay's description"); }
}

} // namespace

void install_hooks() {
    auto* tdb = api().tdb();
    auto* brdm = tdb->find_type("app.BattleReplayDataManager");
    m_get_replay = brdm ? method_of(brdm, "GetReplayData", 0) : nullptr;
    if (!m_get_replay) { logf("replay outfits: BattleReplayDataManager.GetReplayData missing (game update?): off"); return; }
    hook_all(brdm, "CreateSaveData", pre_create_save, post_create_save);
    hook_all(tdb->find_type("app.battle.ReplayFlowMap"), "setupDesc", pre_setup_desc, post_setup_desc);
}

} // namespace slots::replay
