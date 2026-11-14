// SF6 Custom Combos (SF6_CustomCombos.dll): the player's own combos as trials, inside the game's Combo Trial mode.
// Fighting Ground > Practice > "Custom Combos" opens the game's own trial list and trial fight showing the files of
// reframework/data/SF6_CustomTrials_data/<Fighter>/ instead of the official trials.
//
// Engine model. A combo trial is gBattle.Mission (app.FBattleMission, the judge: Info = List<StageData>, InfoPtr,
// CheckPoint, Completed) built from mpStageAsset.Root (CharacterAsset.Mission.StageInfo: Info = ParamInfo[] the
// displayed steps, Team[2], PlayerInitX / EnemyInitX / CameraInitX, EndDelayTime, the unique gauge). We never create a
// trial, we borrow one: the list entry (BattleComboTrialData: StageID, TitleMsg, ExampleData.InputData) stays an
// official one, its asset is rewritten with our recipe when the row is focused in the list or entered, and every
// official value touched is kept and put back on leaving. A borrowed asset carries another trial's settings, so every
// install first writes a baseline, then the trial's own fields. The asset's team values are read by the game before we
// write them, so gauges, counter and dummy are also pushed live through the training data the trial runs on
// (TrainingManager._tData + the group's bApply), only while a trial of ours runs, and put back on leaving.
// Rows past the official count are MemberwiseClones of official entries; the game starts a copy as the trial it was
// copied from, so which of ours is under way is told by the row under the cursor when the list closed.
//
// The rules that each cost a crash in the reference implementation, kept here:
//   R1 an object we make and hand to the game is rooted for the life of the process (cc_game.hpp: new_obj, new_array, clone)
//   R3 a text identical to the one shown is never written (set_message compares; the core compares again)
//   R4 nothing is written under a held pause that the fight must pick up (live pushes run at attempt starts, out of pauses)
//   R5 no recipe written into an asset for 120 frames after a flow change, and only into a live asset holding one
//   R11 the data folder is never walked: each fighter's folder is enumerated, once per load
//   one owner per hook: off when reframework/autorun/SF6_CustomTrials.lua is installed (it hooks the same methods)
// Everything that writes into the game runs on the game thread (the LateUpdateBehavior callback, or a hook running on
// that thread). Every hook is pass-through outside our mode.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>          // (before windows.h) Edit Combo: is Combo Studio's local server up
#include <windows.h>
#include <shellapi.h>

#include "cc_game.hpp"
#include "cc_rec.hpp"
#include "cc_take.hpp"         // the recorder's offline logic (cc_recorder.hpp, included further down, uses it)

#include <atomic>
#include <chrono>
#include <functional>
#include <cstdarg>
#include <set>
#include <thread>

namespace cc {
namespace {

using namespace g;

constexpr const char* kVersion = "0.1.0";
const fs::path kData = L"reframework/data/SF6_CustomTrials_data";
const fs::path kLuaOwner = L"reframework/autorun/SF6_CustomTrials.lua";
constexpr int kFlowComboTrial = 14;
constexpr int kFlowReplay = 10;
constexpr int kPracticeCategory = 1;
constexpr const char* kListAgent = "UI11414";          // the trial list
constexpr const char* kRecipeAgent = "UI11439";        // the recipe of the trial fight
constexpr const char* kMark = " [CUSTOM]";
constexpr uint64_t kSettle = 120;                       // R5
constexpr uint64_t kResetTimeout = 300;

// ---------------------------------------------------------------- log
void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (core && client) core->log(client, SF6CORE_LOG_INFO, buf);
    else API::get()->log_info("custom_combos: %s", buf);
}
// Log a reason once per key until it changes (paths that run every frame or every call)
void why_once(const char* key, const std::string& why) {
    static std::map<std::string, std::string> last;
    static std::mutex mx;
    { std::lock_guard lk(mx); auto& l = last[key]; if (l == why) return; l = why; }
    logf("%s: %s", key, why.c_str());
}

// ---------------------------------------------------------------- config.json (the keys the mode reads)
struct Cfg {
    bool guard_save = true, scroll = true, rec = true, rec_menu = true, rec_menu_reset = true, extra_rows = true;
    int window = 12, scroll_row = 7, rec_min_steps = 2;
    std::string rec_counter;
    std::string editor_url = "http://127.0.0.1:8765/";    // Combo Studio's page (Edit Combo)
    std::string editor_start;                             // its ComboStudio.bat, started when it is not running
} cfg;

void load_cfg() {
    const json c = read_json(kData / "config.json");
    if (!c.is_object()) return;
    auto b = [&](const char* k, bool& dst) { if (const json* v = at(c, k)) dst = v->is_boolean() ? v->get<bool>() : true; };
    b("guard_save", cfg.guard_save);
    b("scroll", cfg.scroll);
    b("rec", cfg.rec);
    b("rec_menu", cfg.rec_menu);
    b("rec_menu_reset", cfg.rec_menu_reset);   // false: no Reset Battle when the training menu changed a setting
    b("extra_rows", cfg.extra_rows);
    if (auto v = num(at(c, "rec_min_steps"))) cfg.rec_min_steps = std::max(1, static_cast<int>(std::floor(*v)));
    const std::string rc = str_of(at(c, "rec_counter"));
    cfg.rec_counter = (rc == "punish" || rc == "counter") ? rc : "";
    if (const std::string u = str_of(at(c, "editor_url")); !u.empty()) cfg.editor_url = u;
    cfg.editor_start = str_of(at(c, "editor_start"));
    if (auto v = num(at(c, "scroll_row"))) cfg.scroll_row = std::max(1, static_cast<int>(std::floor(*v)));
    if (auto v = num(at(c, "window"))) cfg.window = std::max(4, static_cast<int>(std::floor(*v)));
}

// ---------------------------------------------------------------- state
std::recursive_mutex g_mx;                     // the mode's state: the frame callback and the hooks on the game thread
std::atomic<DWORD> g_game_thread{0};
bool on_game_thread() { return GetCurrentThreadId() == g_game_thread.load(std::memory_order_relaxed); }

uint64_t frame = 0;
uint64_t mc = 0;                               // ticks (the Lua's menu_check)
bool in_pause = false;
std::atomic<bool> custom_mode{false};          // our Fighting Ground row was chosen
std::atomic<bool> applied{false};              // our recipe is installed in the live mission
std::atomic<Obj*> menu_data{nullptr};          // app.menu.FGMainMenuData, captured from a hook
std::atomic<Obj*> menu_entry{nullptr};         // our ModeTypeData
std::atomic<int> last_chara{-1};               // the fighter whose catalog was last resolved
thread_local bool tl_own_call = false;         // our own GetComboTrialFighterData lookups

std::unique_ptr<rec::Recorder> g_rec = std::make_unique<rec::Recorder>();

// ---------------------------------------------------------------- the custom set (one file = one trial)
std::map<int, Labels> g_labels;
std::map<int, std::vector<TrialP>> all_sets;
bool sets_loaded = false;
struct SetCache { bool built[2] = { false, false }; std::array<LevelSet, 3> by[2]; };
std::map<int, SetCache> set_cache;
std::map<int, std::vector<int64_t>> cmd_ids;  // per fighter: the ids its commands name
json end_delays;
bool end_delays_read = false;

Labels& labels_of(int chara) {
    auto it = g_labels.find(chara);
    if (it != g_labels.end()) return it->second;
    const char* dir = fighter_dir(chara);
    return g_labels[chara] = dir ? load_labels(kData, dir) : Labels{};
}

void read_all_sets() {
    all_sets.clear();
    set_cache.clear();
    sets_loaded = true;
    size_t files = 0, removed = 0;
    for (const auto& f : kFighters) {
        std::vector<fs::path> paths;
        std::error_code ec;
        const fs::path dir = kData / f.dir;
        if (fs::is_directory(dir, ec))
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                if (!it->is_regular_file(e2)) continue;
                const std::wstring ext = it->path().extension().wstring();
                if (ext == L".json") paths.push_back(it->path());
            }
        std::sort(paths.begin(), paths.end());
        if (paths.empty() && !cfg.rec) continue;
        Labels& L = labels_of(f.id);
        auto& set = all_sets[f.id];
        for (const fs::path& p : paths) {
            const json j = read_json(p);
            bool deleted = false;
            TrialP t = trial_from(j, p, f.id, L, &deleted);
            // A file reduced to { "deleted": true }: a take or a trial deleted in game. Removing it loses nothing: the
            // marker holds no content, and the list's Delete wrote the content into _trash/<Fighter>/ and read it back
            // before it wrote the marker (cc_manage.hpp do_delete); _trash is never read as a list.
            if (deleted) {
                std::error_code e3;
                if (fs::remove(p, e3)) ++removed;
                continue;
            }
            if (t) { set.push_back(t); ++files; }
        }
        cmd_ids[f.id] = command_ids(L);
        if (!set.empty()) logf("custom set: %zu trial(s) for %s", set.size(), f.dir);
    }
    if (cfg.rec) {           // the recording slot: the first row of every tab of every fighter, files or not
        const rec::Settings rs = rec::load_settings(kData);
        for (const auto& f : kFighters)
            for (int lv = 2; lv >= 0; --lv) all_sets[f.id].insert(all_sets[f.id].begin(), rec::make_slot(f.id, rs, cfg.rec_counter, lv));
    }
    for (auto& [id, set] : all_sets)
        for (size_t i = 0; i < set.size(); ++i) set[i]->file_order = static_cast<int>(i) + 1;
    logf("custom set read: %zu trial file(s), %zu deleted take(s) removed", files, removed);
}

std::vector<TrialP>& trial_set(int chara) {
    if (!sets_loaded) read_all_sets();
    return all_sets[chara];
}

// rows each tab can show for this fighter: the official trials it has there (the smaller of Classic and Modern)
bool slot_caps(int chara, int out[3]) {
    const Labels& L = labels_of(chara);
    if (!L.has_slots) return false;
    for (int i = 0; i < 3; ++i) out[i] = std::min(L.slot_classic[i], L.slot_modern[i]);
    return true;
}

LevelSet& level_set(int chara, int level, bool modern) {
    const int lv = level >= 0 && level <= 2 ? level : 0;
    SetCache& c = set_cache[chara];
    const int key = modern ? 1 : 0;
    if (!c.built[key]) {
        int caps[3];
        const bool capped = !cfg.extra_rows && slot_caps(chara, caps);
        c.by[key] = level_sets(trial_set(chara), modern, capped ? caps : nullptr);
        c.built[key] = true;
    }
    return c.by[key][lv];
}

const json& delays() {
    if (!end_delays_read) { end_delays_read = true; end_delays = read_json(kData / "_end_delays.json"); }
    return end_delays;
}

// ---------------------------------------------------------------- game access
struct Diag { int flow = -1, chara = -1, level = -1, stage = -1, stage_id = -1; int64_t first_action = -1; } diag;

Obj* mission() { return static_obj("gBattle", "Mission"); }

Obj* ctm() {
    Method* m = method_of(type("app.training.ComboTrialManager"), "get_Instance", 0);
    auto r = invoke(m, nullptr, {});
    return !r.exception_thrown ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}

// The control type of the session: ComboTrialManager._InputType, 0 Classic, 1 Modern
bool input_modern() { Obj* i = ctm(); return i && (inum(i, "_InputType", 0) & 0xFF) == 1; }   // one byte, as the game reads it
bool cur_modern = false;

std::atomic<Obj*> g_ctm{nullptr};              // the trial manager, kept for the index hooks (cc_index.hpp)
void trial_slot(int& level, int& stage, int& id) {
    level = stage = id = -1;
    Obj* inst = ctm();
    g_ctm = inst;                                // (kept for the index hooks, which run on the menus' threads)
    if (!inst) return;
    level = call_i32(inst, "get_CurrentLevelCategory").value_or(-1);
    stage = static_cast<int>(inum(inst, "_StageIndex", -1));
    id = call_i32(inst, "GetStageID").value_or(-1);
}

Obj* flow_map() { return get_obj(get(singleton("app.bFlowManager"), "m_flow_work"), "_FlowMap"); }

// PauseManager._CurrentPauseTypeBit, read by bits (measured): 0 at the menus, 64 in a running fight (trial or Training),
// bit 256 = under the pause menu (pause type 8), bit 2048 = the training half speed toggling type 11 every other frame
// (2048 alone is not a pause). in_pause = no fight running: the menus, the pause menu, an option window of the core.
constexpr int64_t kFightBit = 64, kPauseMenuBit = 256;
void update_pause() {
    Obj* pm = singleton("app.PauseManager");
    const int64_t pb = pm ? inum(pm, "_CurrentPauseTypeBit", 0) : 0;
    in_pause = (pm && (!(pb & kFightBit) || (pb & kPauseMenuBit) != 0)) || (core && core->opt_window_open && core->opt_window_open() != 0);
}

// P1's action id (mpActParam.ActionPart._Engine.get_ActionID())
int64_t act_of(Obj* o) {
    Obj* eng = get_obj(get(get(ref(o), "mpActParam"), "ActionPart"), "_Engine");
    return eng ? call_i32(eng, "get_ActionID").value_or(-1) : -1;
}
Obj* p1() {
    Obj* sp = static_obj("gBattle", "Player");
    Obj* pl = sp ? get_obj(sp, "mcPlayer") : nullptr;
    return pl ? arr_get(pl, 0) : nullptr;
}

std::vector<Obj*> scene_components(const char* type_name) {
    std::vector<Obj*> out;
    void* sm = nullptr;
    try { sm = API::get()->get_native_singleton("via.SceneManager"); } catch (...) {}
    TD* t = type(type_name);
    if (!sm || !t) return out;
    auto rs = invoke(method_of(type("via.SceneManager"), "get_CurrentScene", 0), sm, {});
    Obj* scene = !rs.exception_thrown ? reinterpret_cast<Obj*>(rs.ptr) : nullptr;
    Obj* rt = t->get_runtime_type();
    if (!scene || !rt) return out;
    auto rc = invoke(method_of(type("via.Scene"), "findComponents(System.Type)"), scene, { rt });
    Obj* comps = !rc.exception_thrown ? reinterpret_cast<Obj*>(rc.ptr) : nullptr;
    for (int32_t i = 0, n = arr_len(comps); i < n; ++i) if (Obj* c = arr_get(comps, i)) out.push_back(c);
    return out;
}

double clock_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A live asset holding a live recipe (R5); `also`: another object that must be live
bool asset_ready(Obj* si, Obj* also = nullptr, bool need_also = false) {
    if (!live(si)) return false;
    if (need_also && !live(also)) return false;
    Obj* info = get_obj(si, "Info");
    return live(info) && arr_len(info) >= 0;
}

// ---------------------------------------------------------------- texts (GUIDs of ours, answered by the core)
bool text_guid(const std::string& text, uint8_t out[16]) {
    return core->text_new(client, text.c_str(), out) == SF6CORE_OK;
}
struct Released { uint64_t at; std::array<uint8_t, 16> g; };
std::vector<Released> to_release;              // our titles, given back to the core once off screen
void release_later(const uint8_t g16[16]) {
    Released r{ frame + 600, {} };
    std::memcpy(r.g.data(), g16, 16);
    to_release.push_back(r);
}
void release_due() {
    for (size_t i = 0; i < to_release.size();) {
        if (frame >= to_release[i].at) {
            core->text_release(client, to_release[i].g.data());
            to_release[i] = to_release.back();
            to_release.pop_back();
        } else ++i;
    }
}

// ---------------------------------------------------------------- enums
// The values the mode writes are the game's literal static int32 fields, measured in game: they are written from this
// table; the reflection read is a check only (logged once when it disagrees). Enums not in it are read from the game.
using EnumVals = std::vector<std::pair<std::string, int64_t>>;
const EnumVals* known_enum(const std::string& n) {
    static const std::map<std::string, EnumVals> k = {
        { "app.training.TM_GS_GUARD_TYPE", { { "NO_GUARD", 0 }, { "GAP", 1 }, { "NEXT", 2 }, { "ALL", 3 }, { "RANDOM", 4 }, { "COUNT", 5 } } },
        { "app.training.TM_COMMON_TYPE", { { "OFF", 0 }, { "ON", 1 }, { "RANDOM", 2 } } },
        { "app.training.TM_DS_DUMMYTYPE", { { "STAND", 0 }, { "CROUCH", 1 }, { "JUMP", 2 }, { "SHORT_REPLAY", 3 }, { "HUMAN", 4 }, { "CPU", 5 } } },
        { "CharacterAsset.Mission.TeamInfo.ECounterType", { { "NONE", 0 }, { "COUNTER", 1 }, { "PUNISH", 2 } } },
        { "CharacterAsset.Mission.TeamInfo.EDummyActionType", { { "STAND", 0 }, { "CROUCH", 1 }, { "JUMP", 2 } } },
        { "app.training.TM_SA_STOCK", { { "STOCK_0", 0 }, { "STOCK_1", 1 }, { "STOCK_2", 2 }, { "STOCK_3", 3 } } },
        { "app.training.TM_DG_STOCK", { { "STOCK_0", 0 }, { "STOCK_1", 1 }, { "STOCK_2", 2 }, { "STOCK_3", 3 }, { "STOCK_4", 4 }, { "STOCK_5", 5 }, { "STOCK_6", 6 } } },
    };
    auto it = k.find(n);
    return it == k.end() ? nullptr : &it->second;
}
EnumVals read_enum(const char* type_name);
const EnumVals& enum_values(const char* type_name) {
    static std::map<std::string, EnumVals> cache;
    auto it = cache.find(type_name);
    if (it != cache.end()) return it->second;
    if (const EnumVals* k = known_enum(type_name)) {
        EnumVals r = read_enum(type_name);          // the check: the same values, whatever their names
        std::vector<int64_t> a, b;
        for (const auto& e : *k) a.push_back(e.second);
        for (const auto& e : r) b.push_back(e.second);
        std::sort(b.begin(), b.end());
        if (!r.empty() && a != b) logf("enum %s: the game's values differ from the measured ones (the measured ones are used)", type_name);
        return cache[type_name] = *k;
    }
    return cache[type_name] = read_enum(type_name);
}
EnumVals read_enum(const char* type_name) {
    EnumVals v;
    if (TD* td = type(type_name)) {
        for (Field* f : td->get_fields()) {
            if (!f || !f->is_static()) continue;
            try {
                void* p = f->is_literal() ? f->get_init_data() : f->get_data_raw(nullptr, false);
                TD* ft = f->get_type();
                if (!p || !ft || !ft->is_valuetype()) continue;
                uint32_t sz = ft->get_valuetype_size();
                if (sz == 0 || sz > 8) sz = 4;
                int64_t x = 0;
                std::memcpy(&x, p, sz);
                if (sz < 8) { const int sh = 64 - static_cast<int>(sz) * 8; x = (x << sh) >> sh; }
                v.push_back({ f->get_name() ? f->get_name() : "", x });
            } catch (...) {}
        }
    }
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return v;
}

std::string upper(std::string s) { for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); return s; }
bool is_sentinel(const std::string& n) {
    const std::string u = upper(n);
    for (const char* w : { "NUM", "COUNT", "INVALID", "UNKNOWN", "END" }) if (u.find(w) != std::string::npos) return true;
    return false;
}

// "max": the infinite / max entry if the enum has one, else its highest real value. "auto" (recovery): the fastest
// refill the enum names; nothing found = leave the asset's value. A number or an exact enum name is taken as is.
std::optional<int64_t> pick(const char* type_name, const json* want) {
    if (want && want->is_number()) return static_cast<int64_t>(want->get<double>());
    if (!want || !want->is_string()) return std::nullopt;
    const std::string w = want->get<std::string>();
    const EnumVals& vals = enum_values(type_name);
    for (const auto& e : vals) if (e.first == w) return e.second;
    auto find_key = [&](std::initializer_list<const char*> keys) -> std::optional<int64_t> {
        for (const char* k : keys)
            for (const auto& e : vals) if (upper(e.first).find(k) != std::string::npos && !is_sentinel(e.first)) return e.second;
        return std::nullopt;
    };
    if (w == "max") {
        if (auto v = find_key({ "INFINIT", "MAX" })) return v;
        std::optional<int64_t> best;
        for (const auto& e : vals) if (!is_sentinel(e.first)) best = e.second;
        return best;
    }
    if (w == "auto") return find_key({ "IMMEDIATE", "INSTANT", "QUICK", "FAST", "AUTO", "FULL", "RECOVER" });
    return std::nullopt;
}

// ---------------------------------------------------------------- skills (the label of a line)
// A step may name its skill ("skill": a name, "skill_cmd": a Classic command); the id is read from the game's
// command-list catalog (FighterSkillUICatalogUserDataHolder, every skill of every fighter with its id) and the
// resident CommonSkillData table (names, Classic command texts). Read at most once every 2 s until found.
struct Skill { int64_t id; std::string name, cmd; int64_t category; };
std::map<int, std::vector<Skill>> skill_cache;
double skill_scan_at = -99;

std::string guid_message(const uint8_t* g16) {
    // hMsg.GetMessage(System.Guid): a 16-byte value argument goes by its address
    Method* m = method_of(type("app.helper.hMsg"), "GetMessage(System.Guid)");
    auto r = invoke(m, nullptr, { const_cast<uint8_t*>(g16) });
    return !r.exception_thrown && r.ptr ? str(reinterpret_cast<Obj*>(r.ptr)) : std::string{};
}
std::string record_text(Obj* rec, const char* field) {
    Ref sn = get(ref(rec), field);
    void* raw = raw_of(sn, "GUID");
    return raw ? guid_message(reinterpret_cast<const uint8_t*>(raw)) : std::string{};
}

const std::vector<Skill>* read_skills(int chara) {
    auto it = skill_cache.find(chara);
    if (it != skill_cache.end()) return &it->second;
    const double now = clock_s();
    if (now - skill_scan_at < 2) return nullptr;      // findComponents walks the scene: rarely
    skill_scan_at = now;
    const auto comps = scene_components("app.FighterSkillUICatalogUserDataHolder");
    if (comps.empty()) { logf("skill catalog not in the scene"); return nullptr; }
    Obj* tm = singleton("app.TableDataManager");
    Obj* dict = tm ? call_obj(tm, "get_CommonSkillDataUserDataDict") : nullptr;
    std::vector<Skill> out;
    std::set<int64_t> seen;
    const Labels& L = labels_of(chara);
    for (Obj* c : comps) {
        Obj* list = get_obj(get(ref(c), "Data"), "DataList");
        for (int32_t k = 0, n = list_count(list); k < n; ++k) {
            Obj* e = list_item(list, k);
            const int64_t id = inum(e, "SkillId", 0);
            if (!e || id / 1000000 != chara || !seen.insert(id).second) continue;
            Skill sk{ id, {}, {}, inum(e, "CategoryType", -1) };
            if (dict && call_bool(dict, "ContainsKey", { arg_i32(static_cast<int32_t>(id)) }).value_or(false)) {
                Obj* holder = call_obj(dict, "get_Item", { arg_i32(static_cast<int32_t>(id)) });
                Obj* rec = get_obj(holder, "RecordData");
                if (rec) { sk.name = record_text(rec, "SkillName"); sk.cmd = record_text(rec, "FGAthleteCommand"); }
            }
            if (sk.cmd.empty()) {         // the same text, from the label table built from the game's files
                auto ls = L.skills.find(std::to_string(id));
                if (ls != L.skills.end()) sk.cmd = ls->second.first;
            }
            out.push_back(std::move(sk));
        }
    }
    std::sort(out.begin(), out.end(), [](const Skill& a, const Skill& b) { return a.id < b.id; });
    logf("skill catalog: %zu skill(s) for chara %d", out.size(), chara);
    if (out.empty()) return nullptr;
    return &(skill_cache[chara] = std::move(out));
}

std::string norm(const std::string& s) {
    std::string o;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u) || std::ispunct(u)) continue;
        o += static_cast<char>(std::tolower(u));
    }
    return o;
}

// Several ids share one name or command: prefer the ones whose group digits are not 00 (Super arts, category 1, the
// other way round), then the lowest
int64_t best_of(const std::vector<Skill>& list, const std::function<bool(const Skill&)>& match, bool flip_supers) {
    int64_t best = 0;
    bool best_grouped = false;
    for (const Skill& sk : list) {
        if (!match(sk)) continue;
        bool grouped = (sk.id / 10000) % 100 != 0;
        if (flip_supers && sk.category == 1) grouped = !grouped;
        if (!best || (grouped && !best_grouped) || (grouped == best_grouped && sk.id < best)) { best = sk.id; best_grouped = grouped; }
    }
    return best;
}

void resolve_skills(Trial& t, int chara) {
    for (size_t i = 0; i < t.steps.size(); ++i) {
        Step& st = t.steps[i];
        if (st.skill) continue;
        if (!st.skill_cmd.empty()) {
            if (const auto* list = read_skills(chara))
                for (const std::string& w : st.skill_cmd) {
                    st.skill = best_of(*list, [&](const Skill& sk) { return !sk.cmd.empty() && cmd_key(sk.cmd) == w; }, true);
                    if (st.skill) break;
                }
            if (!st.skill) logf("step %zu: no skill with command %s yet", i + 1, st.skill_cmd[0].c_str());
        }
        if (!st.skill && !st.skill_name.empty()) {
            if (const auto* list = read_skills(chara)) {
                const std::string want = norm(st.skill_name);
                st.skill = best_of(*list, [&](const Skill& sk) { return !sk.name.empty() && norm(sk.name) == want; }, false);
            }
            if (!st.skill) logf("step %zu: skill '%s' not found yet", i + 1, st.skill_name.c_str());
        }
    }
}

// Lines the game would draw blank get our text (Step::own): no skill at all, or a skill with no command in the control
// type on screen (a Classic normal in a Modern session, an Assisted Combo in a Classic one)
void own_texts(Trial& t, int chara) {
    const bool modern = input_modern();
    if (t.modern_seen != (modern ? 1 : 0)) {     // the texts read from the game's rows were the other type's
        t.modern_seen = modern ? 1 : 0;
        t.texts.clear(); t.texts_done = false; t.harvest.reset(); t.texts_failed = false; t.own_text = -1;
    }
    cur_modern = modern;
    const Labels& L = labels_of(chara);
    for (Step& st : t.steps) {
        st.own.clear();
        if (st.blank) continue;
        const int64_t sid = st.shown_skill();
        // the two Drive Rushes, every fighter (<fighter>040420 / <fighter>040520): the Drive Parry button instead of
        // the game's long text (Wael, 13/11): Parry Drive Rush = Drive Parry then forward forward, Cancel Drive Rush =
        // Drive Parry alone. The icon itself (<ICON dp>, the Drive Parry icon): a raw <CMDBTN> is only resolved by
        // the game's own message pass and shows nothing when written into a row (seen in game)
        if (sid > 0 && sid % 1000000 == 40420) { st.own = "<ICON dp><ICON 6><ICON 6>"; continue; }
        if (sid > 0 && sid % 1000000 == 40520) { st.own = "<ICON dp>"; continue; }
        if (sid == 0 || !has_cmd(L, sid, modern)) {
            std::string txt = step_text(L, st.notation, st.skill_id, static_cast<int>(st.strength_type), modern);
            if (txt.empty()) txt = st.text;
            if (!txt.empty()) st.own = txt;
        }
    }
}

// ---------------------------------------------------------------- the asset: kept, written, put back
struct AssetOrig { Obj* si; Obj* info; };
std::map<Obj*, AssetOrig> asset_orig;          // borrowed assets and their official ParamInfo[] (both rooted)
std::map<Obj*, std::map<std::string, double>> pos_orig;    // [StageInfo] official stage fields
std::map<Obj*, std::map<std::string, double>> team_orig;   // [TeamInfo] official team fields
std::map<Obj*, const Trial*> preview_done;     // [StageInfo] the trial previewed there
std::set<Obj*> setup_si;                       // assets the StageSetup hook wrote (rooted): their official values go back too
uint64_t settle_until = 0;

const char* const kStageRaw[] = { "PositionType", "PlayerInitX", "EnemyInitX", "CameraInitX", "EndDelayTime", "EndDelayMargin",
                                  "UGaugeType", "UniqueGaugeStock", "UniqueGaugeTimer" };
const char* const kTeamRaw[] = { "VGaugeHealType", "VGaugePoint", "SGaugeHealType", "SGaugeStock", "DGaugeHealType", "DGaugeStock",
                                 "DummyActionType", "GuardType", "CounterType" };
const char* const kGaugeFields[] = { "SGaugeStock", "SGaugeHealType", "DGaugeStock", "DGaugeHealType" };
const char* gauge_enum(const std::string& f) {
    if (f == "SGaugeStock") return "app.training.TM_SA_STOCK";
    if (f == "DGaugeStock") return "app.training.TM_DG_STOCK";
    return "CharacterAsset.Mission.TeamInfo.ERecoveryType";
}
constexpr const char* kCounterEnum = "CharacterAsset.Mission.TeamInfo.ECounterType";

void keep_asset(Obj* si) {
    if (asset_orig.count(si)) return;
    Obj* info = get_obj(si, "Info");
    si->add_ref();
    if (info) info->add_ref();
    asset_orig[si] = { si, info };
}
void keep_stage(Obj* si, const char* f) {
    auto& o = pos_orig[si];
    if (!o.count(f)) if (auto v = num(si, f)) o[f] = *v;
}
void keep_team(Obj* t, const char* f) {
    auto& o = team_orig[t];
    if (!o.count(f)) if (auto v = num(t, f)) o[f] = *v;
}
Obj* team(Obj* si, int i) { Obj* teams = get_obj(si, "Team"); return teams && i < arr_len(teams) ? arr_get(teams, i) : nullptr; }

// A borrowed asset carries another trial's settings (a guarding dummy, a Punish Counter...): every install first writes
// the values nearly all official trials share, then the trial's own settings on top. Each official value is kept.
void apply_baseline(Obj* si, const Trial& t) {
    static const std::pair<const char*, double> kStageBase[] = {
        { "PositionType", 0 }, { "EndDelayMargin", 0 }, { "UGaugeType", 0 }, { "UniqueGaugeStock", 0 }, { "UniqueGaugeTimer", 0 } };
    for (const auto& [f, v] : kStageBase) { keep_stage(si, f); set(si, f, v); }
    // the fighter's own gauge (Jamie's drinks, Juri's stocks, Ryu's Denjin Charge...): "unique": { "type", "stock",
    // "timer" }. UGaugeType 1 (CNT_START) takes both values as the training menu would (stock 0..6 / 7 infinite, timer
    // 0 off / 1 started / 2 infinite); 2 and 3 force everything to its maximum (measured)
    const json* u = obj(&t.j, "unique");
    const auto stock = num(at(u, "stock")), timer = num(at(u, "timer"));
    if (stock || timer) {
        set(si, "UGaugeType", std::floor(num(at(u, "type")).value_or(1)));
        set(si, "UniqueGaugeStock", std::max(0.0, std::floor(stock.value_or(0))));
        if (timer) set(si, "UniqueGaugeTimer", std::max(0.0, std::floor(*timer)));
    }
    static const std::pair<const char*, double> kPlayer[] = {
        { "VGaugeHealType", 1 }, { "VGaugePoint", 100 }, { "DummyActionType", 0 }, { "GuardType", 2 }, { "CounterType", 0 } };
    static const std::pair<const char*, double> kDummy[] = {
        { "VGaugeHealType", 1 }, { "VGaugePoint", 100 }, { "SGaugeHealType", 1 }, { "SGaugeStock", 0 }, { "DGaugeHealType", 1 },
        { "DGaugeStock", 6 }, { "DummyActionType", 0 }, { "GuardType", 2 }, { "CounterType", 0 } };
    for (int i = 0; i < 2; ++i) {
        Obj* tm = team(si, i);
        if (!tm) continue;
        for (const char* f : kTeamRaw) keep_team(tm, f);
        if (i == 0) for (const auto& [f, v] : kPlayer) set(tm, f, v);
        else for (const auto& [f, v] : kDummy) set(tm, f, v);
    }
}

// The raw recipe's stage and team fields ("recipe": { stage, teams }), before the gauges and positions so "gauges" /
// "start" / "dummy_counter" still win
void apply_recipe_raw(Obj* si, const Trial& t) {
    const json* r = obj(&t.j, "recipe");
    if (!r) return;
    if (const json* st = obj(r, "stage"))
        for (const char* f : kStageRaw) {
            const json* v = at(st, f);
            if (v && (v->is_number() || v->is_boolean())) {
                keep_stage(si, f);
                set(si, f, v->is_boolean() ? (v->get<bool>() ? 1.0 : 0.0) : v->get<double>());
            }
        }
    if (const json* teams = arr(r, "teams"))
        for (size_t i = 0; i < teams->size(); ++i) {
            const json& tr = (*teams)[i];
            Obj* tm = tr.is_object() ? team(si, static_cast<int>(i)) : nullptr;
            if (!tm) continue;
            for (const char* f : kTeamRaw)
                if (auto v = num(at(tr, f))) { keep_team(tm, f); set(tm, f, std::floor(*v)); }
        }
}

// "dummy": { "state": 0 stand / 1 crouch / 2 jump, "guard": TM_GS_GUARD_TYPE }, on the asset's dummy team
void apply_dummy_asset(Obj* si, const Trial& t) {
    const json* d = obj(&t.j, "dummy");
    Obj* tm = d ? team(si, 1) : nullptr;
    if (!tm) return;
    if (auto v = num(at(d, "state"))) set(tm, "DummyActionType", std::floor(*v));
    if (auto v = num(at(d, "guard"))) set(tm, "GuardType", std::floor(*v));
}

void apply_gauges(Obj* si, const Trial& t) {
    const json* gj = at(&t.j, "gauges");
    if (gj && gj->is_boolean() && !gj->get<bool>()) return;          // "gauges": false, off for this trial
    static const json kEmpty = json::object();
    const json& g = gj && gj->is_object() ? *gj : kEmpty;
    Obj* tm = team(si, static_cast<int>(inum(at(g, "team"), 0)));
    if (!tm) return;
    for (const char* f : kGaugeFields) keep_team(tm, f);
    const json kMax = "max", kAuto = "auto";
    const std::pair<const char*, const json*> want[] = {
        { "SGaugeStock", at(g, "sa_stock") ? at(g, "sa_stock") : &kMax }, { "SGaugeHealType", at(g, "sa_heal") ? at(g, "sa_heal") : &kAuto },
        { "DGaugeStock", at(g, "dg_stock") ? at(g, "dg_stock") : &kMax }, { "DGaugeHealType", at(g, "dg_heal") ? at(g, "dg_heal") : &kAuto } };
    for (const auto& [f, w] : want)
        if (auto v = pick(gauge_enum(f), w)) set(tm, f, static_cast<double>(*v));
    // the dummy's counter state (Team[1].CounterType): "punish" / "counter" match the enum names read from the game
    const json* dc = at(&t.j, "dummy_counter");
    Obj* d = dc ? team(si, static_cast<int>(inum(at(g, "dummy_team"), 1))) : nullptr;
    if (!d) return;
    keep_team(d, "CounterType");
    std::optional<int64_t> v;
    if (dc->is_number()) v = static_cast<int64_t>(dc->get<double>());
    else if (dc->is_string()) {
        const std::string s = dc->get<std::string>();
        for (const auto& e : enum_values(kCounterEnum)) {
            const std::string up = upper(e.first);
            if (e.first == s || (s == "punish" && up.find("PUNISH") != std::string::npos) ||
                (s == "counter" && up.find("COUNTER") != std::string::npos && up.find("PUNISH") == std::string::npos)) { v = e.second; break; }
        }
    }
    if (v) set(d, "CounterType", static_cast<double>(*v));
}

void apply_position(Obj* si, const Trial& t) {
    if (!t.rec) {                   // the slot has no last line: the asset's own value
        keep_stage(si, "EndDelayTime");
        set(si, "EndDelayTime", static_cast<double>(end_delay(t, delays())));
    }
    const json* st = obj(&t.j, "start");
    const auto p1v = num(at(st, "p1")), p2v = num(at(st, "p2"));
    if (!p1v || !p2v) return;
    for (const char* f : { "PlayerInitX", "EnemyInitX", "CameraInitX" }) keep_stage(si, f);
    set(si, "PlayerInitX", std::floor(*p1v));
    set(si, "EnemyInitX", std::floor(*p2v));
    set(si, "CameraInitX", std::floor(num(at(st, "camera")).value_or((*p1v + *p2v) / 2)));
}

void restore_position(Obj* si) {
    auto it = pos_orig.find(si);
    if (it == pos_orig.end()) return;
    for (const auto& [f, v] : it->second) set(si, f.c_str(), v);
    pos_orig.erase(it);
    logf("official start positions restored");
}
void restore_live_gauges(Obj* si) {
    if (team_orig.empty() || !si) return;
    Obj* teams = get_obj(si, "Team");
    for (int32_t i = 0, n = arr_len(teams); i < n; ++i) {
        Obj* t = arr_get(teams, i);
        auto it = t ? team_orig.find(t) : team_orig.end();
        if (it == team_orig.end()) continue;
        for (const auto& [f, v] : it->second) set(t, f.c_str(), v);
        team_orig.erase(it);
        logf("official gauges restored");
    }
}

// ---------------------------------------------------------------- the recipe: displayed lines and judged steps
// One ParamInfo (a DISPLAYED line). `preview`: the trial list's side panel, which shows the skills' names: the real ids
void write_param(Obj* p, const Step& st, int idx, bool preview) {
    set(p, "ProgressNo", static_cast<double>(st.progress_no.value_or(idx)));
    set(p, "ExtNo", static_cast<double>(st.ext_no.value_or(0)));
    set(p, "ActionNo", static_cast<double>(st.action_id));
    set(p, "CheckType", static_cast<double>(st.check_type));
    set(p, "JudgeType", static_cast<double>(st.judge_type));
    set(p, "StyleNo", static_cast<double>(st.style_no));
    set(p, "TriggerNo", static_cast<double>(st.trigger_no));
    set(p, "ExActionType", static_cast<double>(st.ex_action_type));
    // a line without a skill gets no row at all: ours carry an empty one, their text written into the row afterwards
    int64_t skill = st.shown_skill();
    if (!preview && (st.blank || !st.own.empty())) skill = rec::blank_skill(cur_modern);
    set(p, "SkillID", static_cast<double>(skill));
    set(p, "StrengthType", static_cast<double>(st.strength_type));
    set(p, "Option1Type", static_cast<double>(st.option1));
    set(p, "Option2Type", static_cast<double>(st.option2));
    set(p, "Flags", static_cast<double>(st.flags));
    set(p, "NextStepNo", static_cast<double>(st.next_step));
}

// A fresh ParamInfo[] of these steps (rooted); nullptr on a failure
Obj* param_array(const std::vector<const Step*>& steps, bool preview, int first_progress = -1) {
    TD* pt = type("CharacterAsset.Mission.ParamInfo");
    Obj* arr = new_array(pt, static_cast<int32_t>(steps.size()));
    if (!arr) return nullptr;
    for (size_t i = 0; i < steps.size(); ++i) {
        Obj* p = new_obj(pt, false);
        if (!p) return nullptr;
        write_param(p, *steps[i], static_cast<int>(i), preview);
        if (first_progress >= 0) set(p, "ProgressNo", static_cast<double>(i));
        if (!arr_set(arr, static_cast<int32_t>(i), p)) return nullptr;
    }
    return arr;
}
bool set_info(Obj* si, Obj* arr) {
    if (!arr) { why_once("set_InfoProp", "no step array (ParamInfo array or a ParamInfo not created)"); return false; }
    if (!live(si)) { why_once("set_InfoProp", "the StageInfo is not live"); return false; }
    if (!method_of(td_of(si), "set_InfoProp")) { why_once("set_InfoProp", "no set_InfoProp on " + td_of(si)->get_full_name()); return false; }
    const auto r = call(si, "set_InfoProp", { arr });
    if (r.exception_thrown) { why_once("set_InfoProp", "the call threw (array of " + std::to_string(arr_len(arr)) + ")"); return false; }
    return true;
}

void step_flags(Obj* sd, const Step& st) {
    set(sd, "IsHitCheck", st.check_type == 0);
    set(sd, "IsActCheck", st.check_type == 1);
    set(sd, "IsTrgCheck", st.check_type == 2);
    set(sd, "IsExActCheck", st.check_type == 3);
    set(sd, "IsStyleCheck", st.style_no != 0);
    set(sd, "JudgeType", static_cast<double>(st.judge_type));
}

Obj* new_check(int64_t act, const Step* st, int chara) {
    Obj* cd = new_obj(type("app.FBattleMission.CheckData"), true);
    if (!cd) return nullptr;
    set(cd, "Action", static_cast<double>(act));
    set(cd, "Style", static_cast<double>(st ? st->style_no : 0));
    set(cd, "Trigger", static_cast<double>(st ? st->trigger_no : 0));
    set(cd, "ExAction", static_cast<double>(st ? st->ex_action_type : 0));
    set(cd, "CharaID", chara);
    set(cd, "NextStepID", static_cast<double>(st ? st->next_step : -1));
    set(cd, "IsClear", 0);
    return cd;
}

// One StageData (a JUDGED step): every CheckData of the line is aimed at our actions, so an official "this move OR
// that move" line cannot let its own moves through
bool write_stage(Obj* sd, const Step& st, int chara, int idx) {
    Obj* dl = get_obj(sd, "Data");
    if (!dl || st.ids.empty()) return false;
    const double prog = static_cast<double>(st.progress_no.value_or(idx));
    set(sd, "Progress", prog);
    set(sd, "DispProgress", prog);
    list_clear(dl);
    for (int64_t id : st.ids) {
        Obj* cd = new_check(id, &st, chara);
        if (!cd || !list_add(dl, cd)) return false;
    }
    step_flags(sd, st);
    return true;
}

// A brand new recipe of any length: a fresh ParamInfo[] for the display, fresh StageData / CheckData for the judge
bool build(Obj* si, Obj* info, int chara, const Trial& t, std::string& why) {
    std::vector<const Step*> steps;
    for (const Step& s : t.steps) steps.push_back(&s);
    Obj* arr = param_array(steps, false);
    if (!arr) { why = "could not create the recipe lines"; return false; }
    if (!set_info(si, arr)) { why = "set_InfoProp refused"; return false; }
    list_clear(info);
    TD* sdt = type("app.FBattleMission.StageData");
    for (size_t i = 0; i < t.steps.size(); ++i) {
        Obj* sd = new_obj(sdt, false);                  // its constructor builds its check list (an empty List<CheckData>):
        if (sd && !get_obj(sd, "Data"))                 // without it Data is null, so it is run here when it did not run
            invoke(method_of(sdt, ".ctor", 0), sd, {});
        if (!sd || !get_obj(sd, "Data")) { why = "could not create a judged step"; return false; }
        if (!write_stage(sd, t.steps[i], chara, static_cast<int>(i))) { why = "could not fill step " + std::to_string(i + 1); return false; }
        if (!list_add(info, sd)) { why = "the judge refused a step"; return false; }
    }
    return true;
}

// ---------------------------------------------------------------- long recipes scroll
// The native recipe shows 12 lines. The DISPLAYED list is a window of the steps: ParamInfo[] holds steps off+1..off+W
// numbered 0..W-1, each judged step's DispProgress is relative to it; the flow rebuilds the recipe when the window has
// to be laid again (RequestRecipeUI(false), then (true) two frames later), or the rows are re-texted in place once
// every step's text is known (read from the game's own rows: one rebuild per window, once per trial and session).
struct PendingOpen { uint64_t at; Obj* map; };
std::optional<PendingOpen> pending_recipe_open;
std::optional<uint64_t> unhide_at;
std::set<Obj*> hidden_recipes;                 // recipes we hid before a rebuild (compared only)

int window_size(const Trial& t) {
    const int n = static_cast<int>(t.steps.size());
    return (!cfg.scroll || n <= cfg.window) ? n : cfg.window;
}

// "item<N>", a row of the game's scroll lists
bool is_item_name(const std::string& nm) {
    return nm.size() > 4 && nm.size() < 12 && nm.rfind("item", 0) == 0 && nm.find_first_not_of("0123456789", 4) == std::string::npos;
}

std::vector<Obj*> recipe_agents() { return agents(kRecipeAgent); }

void hide_current_recipes() {
    for (Obj* a : recipe_agents()) {
        Obj* ctrl = call_obj(a, "get_ControlMain");
        if (ctrl && !call_bool(ctrl, "get_ForceInvisible").value_or(true)) {
            core->ui_set_force_invisible(client, ctrl, 1);
            hidden_recipes.insert(a);
        }
    }
}
// Should the game hand a hidden agent back (the only recipe left, no rebuild under way), it shows again
void unhide_reused_recipe() {
    const auto ag = recipe_agents();
    if (ag.size() != 1 || !hidden_recipes.count(ag[0])) return;
    Obj* ctrl = call_obj(ag[0], "get_ControlMain");
    if (ctrl && call_bool(ctrl, "get_ForceInvisible").value_or(false)) core->ui_set_force_invisible(client, ctrl, 0);
    hidden_recipes.erase(ag[0]);
}
Obj* recipe_ctrl() {
    for (Obj* a : recipe_agents()) if (!hidden_recipes.count(a)) return call_obj(a, "get_ControlMain");
    return nullptr;
}

// The rows of the recipe on screen: [0] = c_item, [N] = itemN (their e_txt_item). Looked up on every use, never kept.
void rows_walk(Obj* node, int depth, int row, std::map<int, Obj*>& rows) {
    for (Obj* c = child(node); c; c = next(c)) {
        const std::string nm = name_of(c);
        int r = row;
        if (nm == "c_item") r = 0;
        else if (is_item_name(nm)) r = std::stoi(nm.substr(4));
        if (nm == "e_txt_item") { if (r >= 0) rows[r] = c; }
        else if (depth < 9) rows_walk(c, depth + 1, r, rows);
    }
}
std::optional<std::map<int, Obj*>> recipe_rows() {
    Obj* ctrl = recipe_ctrl();
    if (!ctrl) return std::nullopt;
    std::map<int, Obj*> rows;
    rows_walk(ctrl, 0, -1, rows);
    return rows;
}

Obj* row0_inner(Obj* node, int depth, bool in_row0) {
    for (Obj* c = child(node); c; c = next(c)) {
        const std::string nm = name_of(c);
        if (in_row0 && nm == "c_inner") return c;
        if (depth < 9)
            if (Obj* r = row0_inner(c, depth + 1, in_row0 || nm == "c_item")) return r;
    }
    return nullptr;
}

bool apply_window(Obj* m, Trial& t, int off, bool refresh) {
    Obj* si = get_obj(get_obj(m, "mpStageAsset"), "Root");
    Obj* info = get_obj(m, "Info");
    if (!si || !info) return false;
    const int w = window_size(t);
    std::vector<const Step*> steps;
    for (int i = 0; i < w && off + i < static_cast<int>(t.steps.size()); ++i) steps.push_back(&t.steps[off + i]);
    if (!set_info(si, param_array(steps, false, 0))) return false;
    for (int32_t k = 0, n = list_count(info); k < n; ++k)
        if (Obj* sd = list_item(info, k)) set(sd, "DispProgress", std::max(0, std::min(k - off, w - 1)));
    t.win_off = off;
    if (refresh) {
        Obj* fmap = flow_map();
        Method* req = fmap ? method_of(td_of(fmap), "RequestRecipeUI(System.Boolean)") : nullptr;
        if (!req) { logf("RequestRecipeUI not found: the list is not redrawn"); return false; }
        try { hide_current_recipes(); } catch (...) {}
        invoke(req, fmap, { arg_bool(false) });
        pending_recipe_open = PendingOpen{ frame + 2, fmap };
    }
    return true;
}

void retext_rows(const Trial& t, int off) {
    auto rows = recipe_rows();
    if (!rows) return;
    for (int i = 0; i < window_size(t); ++i) {
        auto it = rows->find(i);
        const size_t k = static_cast<size_t>(off + i);
        if (it != rows->end() && k < t.texts.size() && !t.texts[k].empty()) set_message(it->second, t.texts[k]);
    }
}

void harvest_tick(Obj* m, Trial& t) {
    const int n = static_cast<int>(t.steps.size()), w = window_size(t);
    if (t.texts.size() < static_cast<size_t>(n)) t.texts.resize(n);
    if (!t.harvest) {
        Trial::Harvest h;
        for (int o = 0;;) {
            h.offs.push_back(o);
            if (o + w >= n) break;
            o = std::min(o + w, n - w);
        }
        h.since = frame;
        t.harvest = h;
    }
    auto& h = *t.harvest;
    if (pending_recipe_open || unhide_at || in_pause) return;
    const int off = h.offs[h.i];
    if (t.win_off != off) { apply_window(m, t, off, true); h.since = frame; return; }
    if (frame - h.since < 40) return;                  // let the (re)built list fill in
    auto rows = recipe_rows();
    if (!rows || !rows->count(0) || message((*rows)[0]).empty()) return;
    for (int i = 0; i < w; ++i) if (rows->count(i)) t.texts[off + i] = message((*rows)[i]);
    if (++h.i < h.offs.size()) return;
    for (int i = 0; i < n; ++i) {
        if (!t.steps[i].own.empty()) t.texts[i] = t.steps[i].own;
        if (t.texts[i].empty()) { logf("recipe texts: step %d missing, scrolling falls back to rebuilds", i + 1); t.texts_failed = true; }
    }
    t.texts_done = true;
    apply_window(m, t, 0, false);                      // back to the start, by text only
    retext_rows(t, 0);
    logf("recipe texts: %d step(s) read in %zu build(s)", n, h.offs.size());
}

// A failed attempt drops the progression to step 0 at once, but the game keeps its row states for about 2 s: the view
// waits for row 0's c_inner PlayState to turn CURRENT, 5 s at most
bool rows_were_reset(Trial& t) {
    if (!t.reset_wait) { t.reset_wait = frame; return false; }
    std::string st;
    if (Obj* ctrl = recipe_ctrl()) if (Obj* in = row0_inner(ctrl, 0, false)) st = call_str(in, "get_PlayState");
    if (st == "CURRENT" || frame - *t.reset_wait >= kResetTimeout) { t.reset_wait.reset(); return true; }
    return false;
}

// Lines that carry their own text: written into their rows a few times a second (the game rebuilds the list on a restart)
void own_text_tick(Trial& t) {
    Trial& tm = t;
    if (tm.own_text < 0) { tm.own_text = 0; for (const Step& s : t.steps) if (!s.own.empty()) tm.own_text = 1; }
    // every frame, the pause and the example included: the game rewrites its rows (a start, Reset Battle, the example)
    // and a line of ours stays blank until this puts it back; set_message compares first, so a row already right costs
    // one read (seen in game: every 15 frames and not under a pause, the line was blank for a while, and after a
    // Reset Battle or an example it was not written again)
    // A recipe longer than the window (it scrolls) shows steps win_off.. in rows 0..: row i is step win_off + i (seen
    // in game: a 13-step trial was skipped here, its Drive Rush line blank after every Reset Battle and example)
    if (!tm.own_text || pending_recipe_open) return;
    auto rows = recipe_rows();
    if (!rows) return;
    const int n = static_cast<int>(t.steps.size()), w = window_size(t);
    const int off = w < n ? t.win_off : 0;
    for (int i = 0; i < w && off + i < n; ++i) {
        const Step& st = t.steps[static_cast<size_t>(off + i)];
        auto it = rows->find(i);
        if (it == rows->end() || st.own.empty()) continue;
        if (!set_message(it->second, st.own)) why_once("own line text", "row " + std::to_string(i) + " not written");
    }
}

void scroll_tick(Obj* m, Trial& t) {
    if (pending_recipe_open && frame >= pending_recipe_open->at) {
        const PendingOpen p = *pending_recipe_open;
        pending_recipe_open.reset();
        if (Method* req = method_of(td_of(p.map), "RequestRecipeUI(System.Boolean)")) invoke(req, p.map, { arg_bool(true) });
        unhide_at = frame + 10;
    }
    if (unhide_at && frame >= *unhide_at) { unhide_at.reset(); try { unhide_reused_recipe(); } catch (...) {} }
    const int n = static_cast<int>(t.steps.size()), w = window_size(t);
    if (w >= n || pending_recipe_open || in_pause) return;
    if (!t.texts_done) { harvest_tick(m, t); return; }
    const int cur = static_cast<int>(inum(m, "InfoPtr", 0));
    const int off = t.win_off;
    if (cur < off) { if (!rows_were_reset(t)) return; }       // keep the view until the game resets its rows
    else t.reset_wait.reset();
    int want = off;
    const int anchor = std::min(cfg.scroll_row, w) - 1;      // the cursor goes down to this row, then the list moves
    if (cur - off > anchor) want = std::min(cur - anchor, n - w);
    if (cur < off) want = std::max(0, std::min(cur - 2, n - w));
    if (want != off) {
        if (t.texts_failed) apply_window(m, t, want, true);
        else { apply_window(m, t, want, false); retext_rows(t, want); }
    }
}

// ---------------------------------------------------------------- install
bool install(Obj* m, Trial& t, std::string& why) {
    Obj* si = get_obj(get_obj(m, "mpStageAsset"), "Root");
    Obj* arr = si ? get_obj(si, "Info") : nullptr;
    Obj* info = get_obj(m, "Info");
    if (!si || !arr || !info || !asset_ready(si)) { why = "mission not loaded yet"; return false; }
    const int want = static_cast<int>(t.steps.size());
    const bool fresh = !asset_orig.count(si);
    keep_asset(si);
    const int pn = arr_len(arr), sn = list_count(info);
    const int chara = static_cast<int>(inum(m, "CharaID", 0));
    resolve_skills(t, chara);
    own_texts(t, chara);
    try { apply_baseline(si, t); } catch (...) { logf("baseline settings failed"); }
    try { apply_recipe_raw(si, t); } catch (...) { logf("raw recipe fields failed"); }
    try { apply_dummy_asset(si, t); } catch (...) {}
    try { apply_gauges(si, t); } catch (...) { logf("gauges failed"); }
    try { apply_position(si, t); } catch (...) { logf("start position failed"); }
    if (fresh || pn != want || sn != want) {
        if (!build(si, info, chara, t, why)) return false;
        logf("built a %d-step recipe (was %d line(s) / %d judge step(s))", want, pn, sn);
    } else {
        for (int i = 0; i < want; ++i) {
            if (Obj* p = arr_get(arr, i)) write_param(p, t.steps[i], i, false);
            if (Obj* sd = list_item(info, i)) write_stage(sd, t.steps[i], chara, i);
        }
    }
    // a recipe longer than the native list shows its first window; scroll_tick moves it along
    if (window_size(t) < want) apply_window(m, t, 0, false);
    set(m, "InfoPtr", 0);
    set(m, "CheckPoint", 0);
    set(m, "Completed", 0);
    return true;
}

// ---------------------------------------------------------------- live settings (training data the trial runs on)
// Written only while a trial of ours is under way (flow 14, our section, installed), at attempt starts out of pauses
// (R4), and put back when the trial or our section is left. Never in Training mode.
struct StockOrig { double sa, dg; };
std::map<int, StockOrig> params_orig;
std::optional<std::pair<double, double>> counter_orig;
std::optional<std::pair<double, double>> dummy_orig;
int64_t refill_prev_ptr = -1;
uint64_t refill_next = 0;

// TrainingManager._tfFuncs: Dictionary<UInt32, TrainingFuncBase>, keyed by bit flags (1 tf_SelectMenu, 2 tf_DummyStatus,
// 4 tf_GuardSetting, 8 tf_DefenseSystem, 16 tf_CounterSetting, 32 tf_ParameterSetting, 64 tf_DisplaySetting, 1024
// tf_OtherSetting; a combo trial has no Record / Reversal function). Found by its type's name.
Obj* training_tf(Obj* tm, const char* name) {
    for (const auto& [key, v] : dict_u32(get_obj(tm, "_tfFuncs"))) {
        (void)key;
        if (type_name(v).find(name) != std::string::npos) return v;
    }
    return nullptr;
}
bool tf_apply(Obj* tm, const char* name) {
    Obj* tf = training_tf(tm, name);
    return tf && call_ok(tf, "bApply");
}
Ref tdata(Obj* tm, const char* group) { return get(get(get(ref(tm), "_tData"), group), "DummyData"); }

void set_player_stocks(int side, double sa, double dg) {
    Obj* tm = singleton("app.training.TrainingManager");
    Obj* pdl = get_obj(get(get(ref(tm), "_tData"), "ParameterSetting"), "PlayerDatas");
    Obj* pd = pdl && side >= 0 && side < arr_len(pdl) ? arr_get(pdl, side) : nullptr;
    if (!pd) return;
    if (!params_orig.count(side)) params_orig[side] = { num(pd, "SA_Stock").value_or(0), num(pd, "DG_Stock").value_or(0) };
    if (num(pd, "SA_Stock") != sa || num(pd, "DG_Stock") != dg) {
        set(pd, "SA_Stock", sa);
        set(pd, "DG_Stock", dg);
        Obj* tf = training_tf(tm, "tf_ParameterSetting");
        if (!tf) return;
        call_ok(tf, "bApply");
        // bApply stores the setting for the next attempt reset; UpdateGameData() pushes it to the fight now
        if (Method* upd = method_of(td_of(tf), "UpdateGameData", 0)) invoke(upd, tf, {});
    }
}
void restore_player_stocks() {
    if (params_orig.empty()) return;
    Obj* tm = singleton("app.training.TrainingManager");
    Obj* pdl = get_obj(get(get(ref(tm), "_tData"), "ParameterSetting"), "PlayerDatas");
    for (const auto& [side, o] : params_orig)
        if (Obj* pd = pdl && side < arr_len(pdl) ? arr_get(pdl, side) : nullptr) {
            set(pd, "SA_Stock", o.sa);
            set(pd, "DG_Stock", o.dg);
            tf_apply(tm, "tf_ParameterSetting");
        }
    params_orig.clear();
    logf("training gauge settings restored");
}

void set_dummy_counter(double nc, double pc) {
    Obj* tm = singleton("app.training.TrainingManager");
    Ref dd = tdata(tm, "CounterSetting");
    if (!dd) return;
    if (!counter_orig) counter_orig = std::make_pair(num(dd, "NC_TYPE").value_or(0), num(dd, "PC_TYPE").value_or(0));
    if (num(dd, "NC_TYPE") != nc || num(dd, "PC_TYPE") != pc) {
        set(dd, "NC_TYPE", nc);
        set(dd, "PC_TYPE", pc);
        tf_apply(tm, "tf_CounterSetting");
    }
}
void restore_dummy_counter() {
    if (!counter_orig) return;
    Obj* tm = singleton("app.training.TrainingManager");
    Ref dd = tdata(tm, "CounterSetting");
    if (dd) {
        set(dd, "NC_TYPE", counter_orig->first);
        set(dd, "PC_TYPE", counter_orig->second);
        tf_apply(tm, "tf_CounterSetting");
        logf("dummy counter setting restored");
    }
    counter_orig.reset();
}

void dummy_live(double state, double guard, bool restoring) {
    Obj* tm = singleton("app.training.TrainingManager");
    Ref ds = tdata(tm, "DummyStatus"), gs = tdata(tm, "GuardSetting");
    if (!ds || !gs) return;
    if (!restoring && !dummy_orig) dummy_orig = std::make_pair(num(ds, "DummyActionType").value_or(0), num(gs, "GuardType").value_or(2));
    if (num(ds, "DummyActionType") != state) { set(ds, "DummyActionType", state); tf_apply(tm, "tf_DummyStatus"); }
    if (num(gs, "GuardType") != guard) { set(gs, "GuardType", guard); tf_apply(tm, "tf_GuardSetting"); }
}
void restore_dummy_live() {
    if (!dummy_orig) return;
    const auto o = *dummy_orig;
    dummy_orig.reset();
    dummy_live(o.first, o.second, true);
}

void restore_live_all() {
    try { restore_dummy_counter(); } catch (...) { counter_orig.reset(); }
    try { restore_dummy_live(); } catch (...) { dummy_orig.reset(); }
    try { restore_player_stocks(); } catch (...) { params_orig.clear(); }
}

// At each attempt start (progression on step 0), and once a second while it stays there: the side's stocks, the
// dummy's bars, counter state and stance, written only when they differ from what the training data holds
void refill_live(Obj* m, const Trial& t) {
    if (g_rec->training_menu_open()) return;      // the training menu is up in the slot: its values are the truth
    const int64_t ptr = inum(m, "InfoPtr", 0);
    const bool due = ptr == 0 && (refill_prev_ptr != 0 || frame >= refill_next);
    refill_prev_ptr = ptr;
    if (!due || flag(m, "Completed")) return;
    refill_next = frame + 60;
    const json* gj = at(&t.j, "gauges");
    if (!(gj && gj->is_boolean() && !gj->get<bool>())) {
        const int side = static_cast<int>(inum(m, "PlayerID", 0));
        set_player_stocks(side, static_cast<double>(inum(at(gj, "sa_stock"), 3)), static_cast<double>(inum(at(gj, "dg_stock"), 6)));
        // the dummy's own bars, when the trial sets them ("recipe": { "teams": [ {}, { SGaugeStock, DGaugeStock } ] })
        const json* teams = arr(obj(&t.j, "recipe"), "teams");
        const json* r2 = teams && teams->size() > 1 && (*teams)[1].is_object() ? &(*teams)[1] : nullptr;
        if (r2 && (num(at(r2, "SGaugeStock")) || num(at(r2, "DGaugeStock"))))
            set_player_stocks(1 - side, static_cast<double>(inum(at(r2, "SGaugeStock"), 0)), static_cast<double>(inum(at(r2, "DGaugeStock"), 6)));
    }
    if (const json* dc = at(&t.j, "dummy_counter")) {
        const bool punish = is_str(dc, "punish") || (dc->is_number() && dc->get<double>() == 2);
        const bool counter = is_str(dc, "counter") || (dc->is_number() && dc->get<double>() == 1);
        set_dummy_counter(counter ? 1 : 0, punish ? 1 : 0);
    }
    if (const json* d = obj(&t.j, "dummy"))
        dummy_live(static_cast<double>(inum(at(d, "state"), 0)), static_cast<double>(inum(at(d, "guard"), 2)), false);
}

// ---------------------------------------------------------------- follow-up actions
// A held move starts with one action and hits with another the game chains to by itself (Ryu SA2 1212 -> 1216). While
// the current hit step's own move runs, the action it moves on to by itself (an id no command names) is added to that
// step's checks before it hits. Projectiles hit as their own work (nBattle.cWork): once the step's own move started,
// a hit by one of the player's projectiles counts for it (TrialOnAttack, pre).
int64_t follow_last = -1;
int64_t armed_step = -1;
const Trial* follow_trial = nullptr;

bool add_check(Obj* m, Obj* dl, int64_t act) {
    Obj* cd = new_check(act, nullptr, static_cast<int>(inum(m, "CharaID", 0)));
    return cd && list_add(dl, cd);
}
bool has_action(Obj* dl, int64_t act) {
    for (int32_t k = 0, n = list_count(dl); k < n; ++k) if (inum(list_item(dl, k), "Action", -1) == act) return true;
    return false;
}

void follow_tick(Obj* m, const Trial& t, int chara) {
    follow_trial = &t;
    const int64_t act = act_of(p1());
    if (act == follow_last) return;
    const int64_t prev = follow_last;
    follow_last = act;
    if (act < 100) return;
    const int64_t ptr = inum(m, "InfoPtr", 0);
    Obj* sd = list_item(get_obj(m, "Info"), static_cast<int32_t>(ptr));
    if (!sd || !flag(sd, "IsHitCheck")) return;
    Obj* dl = get_obj(sd, "Data");
    if (!dl) return;
    if (has_action(dl, act)) { armed_step = ptr; return; }
    const auto& cmds = cmd_ids[chara];
    if (prev < 100 || !has_action(dl, prev) || std::binary_search(cmds.begin(), cmds.end(), act)) return;
    if (add_check(m, dl, act)) { armed_step = ptr; logf("step %lld: %lld follows %lld, accepted for the hit", ptr + 1, act, prev); }
}

void on_attack_mode(Obj* m, Obj* w) {
    if (!applied.load() || !custom_mode.load() || armed_step < 0 || !w) return;
    TD* work = type("nBattle.cWork");
    if (!work || td_of(w) != work || inum(w, "pl_no", -1) != 0) return;      // players are cPlayer: projectiles only
    if (!m || inum(m, "InfoPtr", 0) != armed_step) return;
    Obj* sd = list_item(get_obj(m, "Info"), static_cast<int32_t>(armed_step));
    if (!sd || !flag(sd, "IsHitCheck")) return;
    const int64_t act = act_of(w);
    Obj* dl = get_obj(sd, "Data");
    if (act <= 0 || !dl || has_action(dl, act)) return;
    if (add_check(m, dl, act)) logf("step %lld: projectile %lld accepted for the hit", armed_step + 1, act);
}

// The game rebuilds the judge from the displayed recipe (Restart, some attempt resets): a line carries one ActionNo,
// so the rebuilt steps judge one id each. Ours is intact while it has every step and each its own ids at least.
bool judge_intact(Obj* info, const Trial& t) {
    if (list_count(info) != static_cast<int32_t>(t.steps.size())) return false;
    for (size_t i = 0; i < t.steps.size(); ++i) {
        Obj* dl = get_obj(list_item(info, static_cast<int32_t>(i)), "Data");
        if (!dl || list_count(dl) < static_cast<int32_t>(t.steps[i].ids.size())) return false;
    }
    return true;
}

// ---------------------------------------------------------------- our own trial lists
// The list is built from the OFFICIAL entries (real identities: selection, auto-progression and the pause spinner work
// natively) with a title GUID of ours written onto each (the original 16 bytes kept); past the official count an entry
// is a MemberwiseClone of an official one with its own example record. A demo ("Watch Example") is the entry's
// ExampleData.InputData (app.TrainingInput.Record): buff = one UInt16 input mask per frame, Num = frames used.
struct Named { std::array<uint8_t, 16> orig; std::array<uint8_t, 16> ours; bool has_ours = false; };
std::map<Obj*, Named> renamed;                 // [entry] its official title GUID (the first one seen)
struct DemoOrig { Obj* rec; Obj* buff; double num; };
std::map<Obj*, DemoOrig> demo_orig;            // [entry]
struct DemoBuf { std::string key; Obj* buf = nullptr; };
std::map<Obj*, DemoBuf> demo_bufs;             // [entry] the example buffer of ours it holds (rooted), and the demo it was made from
std::map<Obj*, TrialP> entry_trial;            // [entry] our trial shown on that row
std::map<Obj*, std::map<int, Obj*>> extra_copies;   // [official array] the copies past its count, made once
std::set<Obj*> copied;                         // entries that are copies
// The manager's index made real (rule 26, cc_index.hpp): each entry's row when its array was built, how many rows each
// custom list has. Read by hooks on the menus' threads under idx_mx (map reads only, no game call under it).
struct RowOf { int row; int size; Obj* list; };
std::mutex idx_mx;
std::map<Obj*, RowOf> row_map;                 // [entry] its row in our list, the official count, the official array
std::map<Obj*, int> list_n;                    // [official array] the rows of the list of ours built from it
std::atomic<bool> real_index{false};           // the index hooks are placed: the manager's index IS the row
std::atomic<bool> raw_ok{false};               // reference arrays read from memory: layout checked on a list of ours
std::atomic<bool> g_decided{false};            // a row of ours decided, its StageSetup not seen yet: previews hold off
uint64_t decided_since = 0;
struct SetupSeen { int level = -1, stage = -1; uint64_t at = 0; } g_setup;   // the last StageSetup of ours
struct Seen { int level, stage; uint64_t since, floor, last; bool ok; };
std::optional<Seen> g_seen;                    // the row under way, and whether its StageSetup was seen
uint64_t out_at = 0;
std::map<Obj*, Obj*> copy_roots;               // [copied entry] its own StageInfoRoot (a shallow copy, made once)
std::map<Obj*, Obj*> orig_lists;               // [level list] its official array (rooted)
std::set<Obj*> custom_lists;                   // level lists holding ours now
std::map<Obj*, bool> list_modern;              // [level list] a Modern list (GetLevelListData's 2nd argument)
std::map<int, bool> lists_done;                // [fighter] its lists carry ours (custom mode)
std::map<int, Obj*> known_fd;                  // [fighter] its BattleComboTrialFighterData (rooted)
std::map<Obj*, int> fd_chara;
Obj* catalog_data = nullptr;
double scene_scan_at = -99;

// The UInt16[] element layout, checked once on a real array (count at 0x1C, data at 0x20): written raw, 3600 frames
// at a time, instead of 3600 boxed SetValue calls
int u16_state = -1;
uint16_t* u16_data(Obj* a, int32_t n) {
    if (u16_state < 0) {
        u16_state = 0;
        Obj* t = new_array(type("System.UInt16"), 3);
        if (t && *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(t) + 0x1C) == 3 && arr_len(t) == 3) {
            reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(t) + 0x20)[1] = 0xBEEF;
            Obj* boxed = arr_get(t, 1);
            if (boxed && num(boxed, "mValue") == 0xBEEF) u16_state = 1;
        }
        logf("UInt16[] layout %s", u16_state ? "checked" : "not as expected: no demo of ours is written");
    }
    if (!a || *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(a) + 0x1C) != n || !u16_state) return nullptr;
    return reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(a) + 0x20);
}

uint16_t demo_mask(const std::string& inp) {
    uint16_t m = 0;
    std::string tok;
    auto flush = [&] {
        static const std::pair<const char*, int> k[] = {
            { "1", 10 }, { "2", 2 }, { "3", 6 }, { "4", 8 }, { "5", 0 }, { "6", 4 }, { "7", 9 }, { "8", 1 }, { "9", 5 },
            { "LP", 16 }, { "MP", 32 }, { "HP", 64 }, { "LK", 128 }, { "MK", 256 }, { "HK", 512 } };
        for (const auto& e : k) if (tok == e.first) m = static_cast<uint16_t>(m | e.second);
        tok.clear();
    };
    for (char c : inp) { if (c == ' ' || c == '+' || c == '\t') flush(); else tok += c; }
    flush();
    return m;
}

void apply_demo(Obj* entry, const Trial& t) {
    std::vector<std::pair<int64_t, uint16_t>> runs;           // { frames, mask }
    const json* demo = obj(&t.j, "demo");
    const json* rle = arr(demo, "rle");
    if (rle && !rle->empty()) {                               // the recorder's exact masks
        for (const json& e : *rle)
            if (e.is_array() && e.size() >= 2) runs.push_back({ inum(&e[0], 0), static_cast<uint16_t>(inum(&e[1], 0)) });
    } else if (const json* tl = arr(demo, "timeline")) {      // "5f : 2+HP"
        for (const json& e : *tl) {
            const std::string s = e.is_string() ? e.get<std::string>() : std::string{};
            size_t i = 0;
            while (i < s.size() && text::digit(s[i])) ++i;
            if (!i || s.compare(i, 4, "f : ") != 0 || s.size() <= i + 4) continue;
            const int64_t n = std::stoll(s.substr(0, i));
            const std::string inp = s.substr(i + 4);
            const uint16_t mask = inp[0] == '#' ? static_cast<uint16_t>(std::atoll(inp.c_str() + 1)) : demo_mask(inp);
            runs.push_back({ n, mask });
        }
    }
    if (runs.empty()) return;
    Obj* recd = get_obj(get_obj(entry, "ExampleData"), "InputData");
    if (!recd) return;
    if (!demo_orig.count(entry)) {
        Obj* buff = get_obj(recd, "buff");
        if (buff) buff->add_ref();                            // the official buffer stays alive until it is put back
        recd->add_ref();
        demo_orig[entry] = { recd, buff, num(recd, "Num").value_or(0) };
    }
    int64_t total = 0;
    std::string key;
    for (const auto& r : runs) { total += std::max<int64_t>(0, r.first); key += std::to_string(r.first) + ":" + std::to_string(r.second) + ","; }
    const int32_t len = static_cast<int32_t>(std::max<int64_t>(3600, total));
    // The buffer is the game's from here on and read long after (rule 20): rooted for the life of the process
    // (new_array), and an entry keeps the one it has while its demo does not change (the lists are rebuilt after
    // every take)
    DemoBuf& db = demo_bufs[entry];
    Obj* buf = db.key == key ? db.buf : nullptr;
    if (!buf) {
        buf = new_array(type("System.UInt16"), len);
        if (!buf) return;
        uint16_t* data = u16_data(buf, len);
        if (!data) return;                                    // the layout is not the one checked: the official demo stays
        int64_t head = 0;
        for (const auto& [n, mask] : runs) {
            if (mask) for (int64_t k = head; k < head + n && k < len; ++k) data[k] = mask;
            head += std::max<int64_t>(0, n);
        }
        db = DemoBuf{ key, buf };
    }
    set_ref(recd, "buff", buf);
    set(recd, "Num", static_cast<double>(total));
    set(recd, "Cursor", 0);
}

void restore_demos() {
    for (auto& [e, o] : demo_orig) {
        try { set_ref(o.rec, "buff", o.buff); set(o.rec, "Num", o.num); } catch (...) {}
    }
    demo_orig.clear();
}

void restore_names() {
    restore_demos();
    for (auto& [e, n] : renamed) {
        try { write_guid(e, "TitleMsg", n.orig.data()); } catch (...) {}
        if (n.has_ours) release_later(n.ours.data());
    }
    renamed.clear();
}

void rename(Obj* entry, const std::string& title) {
    Named& n = renamed[entry];
    if (!n.has_ours && !read_guid(entry, "TitleMsg", n.orig.data())) { renamed.erase(entry); return; }
    uint8_t g16[16];
    if (!text_guid(title, g16)) return;
    if (n.has_ours) release_later(n.ours.data());
    if (write_guid(entry, "TitleMsg", g16)) { std::memcpy(n.ours.data(), g16, 16); n.has_ours = true; }
    else core->text_release(client, g16);
}

Obj* build_custom_array(Obj* orig, LevelSet& ls, bool modern, int& n_out, std::string& why) {
    const int size = arr_len(orig);
    const int n = !cfg.extra_rows ? std::min(static_cast<int>(ls.rows.size()), size) : static_cast<int>(ls.rows.size());
    ls.size = size;
    if (n < 1 || size < 1) { why = "nothing to install: " + std::to_string(ls.rows.size()) + " file(s), " + std::to_string(size) + " official slot(s)"; return nullptr; }
    auto& copies = extra_copies[orig];
    TD* et = type("app.battle.assets.BattleComboTrialData");
    Obj* arr = new_array(et, n);
    if (!arr) { why = "could not create the trial array"; return nullptr; }
    for (int i = 1; i <= n; ++i) {
        Obj* src = nullptr;
        if (i <= size) src = arr_get(orig, i - 1);
        else {
            src = copies[i];
            if (!src) {
                Obj* from = arr_get(orig, (i - 1) % size);
                src = clone(from);
                if (!src) { why = "could not copy an official entry"; return nullptr; }
                if (Obj* ex = get_obj(from, "ExampleData")) {
                    Obj* ex2 = clone(ex);
                    if (ex2) {
                        if (Obj* recd = get_obj(ex, "InputData")) if (Obj* rec2 = clone(recd)) set_ref(ex2, "InputData", rec2);
                        set_ref(src, "ExampleData", ex2);
                    }
                }
                copies[i] = src;
            }
            copied.insert(src);
        }
        if (!src) { why = "missing official entry " + std::to_string(i); return nullptr; }
        arr_set(arr, i - 1, src);
        const TrialP& t = ls.rows[static_cast<size_t>(i - 1)];
        entry_trial[src] = t;
        { std::lock_guard lk(idx_mx); row_map[src] = RowOf{ i - 1, size, orig }; }   // its true row (the index hooks)
        try { apply_demo(src, *t); } catch (...) { logf("demo not written (row %d)", i); }
        try { rename(src, row_title(*t, modern)); } catch (...) { logf("rename failed (row %d)", i); }
    }
    if (!raw_ok.load()) raw_ok = arr_raw_ok(arr);  // (the hooks read arrays from memory only once this held)
    n_out = n;
    return arr;
}

// The official arrays are back: an index past their count goes back on the entry it borrowed (rows past the count)
void clamp_index(Obj* ld, int size) {
    Obj* mgr = ctm();
    Obj* fd = mgr ? get_obj(mgr, "_ComboTrialFighterData") : nullptr;
    if (!fd || !ld || size <= 0) return;
    static const char* const kLists[2][3] = { { "ElementaryData", "IntermediateData", "AdvancedData" },
                                              { "ElementaryData_Modern", "IntermediateData_Modern", "AdvancedData_Modern" } };
    static const char* const kIdx[2][3] = { { "_Level", "_InputType", "_StageIndex" }, { "_PrevLevel", "_PrevInputType", "_PrevStageIndex" } };
    const int64_t lv = inum(ld, "LevelCategory", -1);
    for (int it = 0; it < 2; ++it)
        for (const char* n : kLists[it]) {
            if (get_obj(fd, n) != ld) continue;
            for (const auto& f : kIdx) {
                const int64_t i = inum(mgr, f[2], -1);
                if (inum(mgr, f[0], -1) == lv && (inum(mgr, f[1], -1) & 0xFF) == it && i >= size) {
                    set(mgr, f[2], rows::clamp(static_cast<int>(i), size));
                    logf("trial index %lld brought back to %d (the official list is back)", static_cast<long long>(i), rows::clamp(static_cast<int>(i), size));
                }
            }
        }
}
void forget_list(Obj* ld) {
    auto it = orig_lists.find(ld);
    if (it == orig_lists.end()) return;
    { std::lock_guard lk(idx_mx); list_n.erase(it->second); }
    try { clamp_index(ld, arr_len(it->second)); } catch (...) {}
}

// One level list: our trials in, or the official array back
void update_list(Obj* ld, int chara, int it) {
    Obj* cur = ld ? get_obj(ld, "ComboTrialDatas") : nullptr;
    if (!cur) { why_once("level list", std::string("no ComboTrialDatas on ") + (ld && td_of(ld) ? td_of(ld)->get_full_name() : "null")); return; }
    if (custom_mode.load()) {
        if (custom_lists.count(ld)) return;
        if (!orig_lists.count(ld)) { cur->add_ref(); ld->add_ref(); orig_lists[ld] = cur; }
        if (it == 0 || it == 1) list_modern[ld] = it == 1;
        const bool modern = list_modern.count(ld) ? list_modern[ld] : input_modern();
        LevelSet& ls = level_set(chara, static_cast<int>(inum(ld, "LevelCategory", 0)), modern);
        int n = 0;
        std::string why;
        Obj* arr = build_custom_array(orig_lists[ld], ls, modern, n, why);
        if (arr && !set_ref(ld, "ComboTrialDatas", arr)) why_once("level list", "ComboTrialDatas not written");
        if (arr && get_obj(ld, "ComboTrialDatas") == arr) {
            custom_lists.insert(ld);
            { std::lock_guard lk(idx_mx); list_n[orig_lists[ld]] = n; }   // (rows the game's index may name)
            logf("custom list installed: %d trial(s) (fighter %d)", n, chara);
        } else if (!arr) {
            static std::string last;
            if (why != last) { last = why; logf("custom list not installed: %s", why.c_str()); }
        }
    } else if (custom_lists.count(ld)) {
        set_ref(ld, "ComboTrialDatas", orig_lists[ld]);
        custom_lists.erase(ld);
        forget_list(ld);
        restore_names();
        logf("official trial list and names restored");
    }
}

void restore_lists(const char* why) {
    if (custom_lists.empty()) return;
    lists_done.clear();
    for (Obj* ld : custom_lists) if (orig_lists.count(ld)) {
        try { set_ref(ld, "ComboTrialDatas", orig_lists[ld]); } catch (...) {}
        if (!custom_mode.load()) forget_list(ld);   // (rebuilt with the new set: the index stays where it is)
    }
    custom_lists.clear();
    try { restore_names(); } catch (...) {}
    logf("%s", why);
}

Obj* own_fd(Obj* cat, int id) {
    tl_own_call = true;
    // asked through the method found by name alone: in game the lookup by name + parameter count answered null where
    // this one answered the entry (same holder, same frame)
    Method* m = method_of(td_of(cat), "GetComboTrialFighterData");
    const auto r1 = invoke(m, cat, { arg_i32(id) });
    Obj* f = !r1.exception_thrown && r1.ptr ? reinterpret_cast<Obj*>(r1.ptr) : nullptr;
    static bool told = false;
    if (!told && f) {
        told = true;
        Obj* g = call_obj(cat, "GetComboTrialFighterData", { arg_i32(id) });
        Method* m1 = method_of(td_of(cat), "GetComboTrialFighterData", 1);
        logf("catalog lookup: by name %p, by name + 1 parameter %p (methods %p / %p)", (void*)f, (void*)g, (void*)m, (void*)m1);
    }
    tl_own_call = false;
    return f;
}
void keep_fd(int id, Obj* f) {
    if (!f || known_fd.count(id)) return;
    f->add_ref();
    known_fd[id] = f;
}

// The catalog is several holders (app.esports.FGComboTrialListUserDataHolder components of the scene, measured: 15):
// the first answers the launch roster (ids 1 to 12, 15 to 18, 20, 21), every fighter added since has a holder of its
// own answering its id alone. All are asked, once per id, and the answers kept: an entry's fighter only ever comes
// from what a holder (or the game's own lookup) answered for that id, never from the fighter seen last (that wrote
// one fighter's trials into another's lists).
std::vector<Obj*> catalogs;                    // every holder's Data (rooted)
std::map<int, Obj*> fd_sure;                   // [fighter] the entry a holder answered for that id
std::map<int, double> fd_miss;                 // [fighter] when no holder had it (asked again 5 s later)

void know_fd(int id, Obj* f) {                 // an answer for that id: sure
    if (!f || id <= 0) return;
    if (fd_sure[id] != f) { f->add_ref(); fd_sure[id] = f; }
    fd_chara[f] = id;
    known_fd[id] = f;
}

Obj* get_catalog_data() {
    if (catalog_data) return catalog_data;
    const double now = clock_s();
    if (now - scene_scan_at < 2) return nullptr;
    scene_scan_at = now;
    for (Obj* c : scene_components("app.esports.FGComboTrialListUserDataHolder"))
        if (Obj* d = get_obj(c, "Data")) { d->add_ref(); catalogs.push_back(d); }
    if (!catalogs.empty()) { catalog_data = catalogs[0]; logf("trial catalog: %zu holder(s)", catalogs.size()); }
    return catalog_data;
}

// a fighter's catalog entry, whichever holder has it
Obj* catalog_fd(int id) {
    auto s = fd_sure.find(id);
    if (s != fd_sure.end()) return s->second;
    const double now = clock_s();
    auto m = fd_miss.find(id);
    if (m != fd_miss.end() && now - m->second < 5) return nullptr;
    if (!get_catalog_data()) return nullptr;
    for (Obj* d : catalogs)
        if (Obj* f = own_fd(d, id)) { know_fd(id, f); return f; }
    fd_miss[id] = now;
    return nullptr;
}

// Whose list this is, from the object asked (a fighter's catalog entry): -1 when no holder said it
int chara_of_fd(Obj* fd) {
    if (!fd) return -1;
    auto it = fd_chara.find(fd);
    if (it != fd_chara.end()) return it->second;
    if (!get_catalog_data()) return -1;
    for (const auto& f : kFighters) catalog_fd(f.id);
    it = fd_chara.find(fd);
    return it != fd_chara.end() ? it->second : -1;
}

// The list screen never calls GetLevelListData (it already holds its list objects): the catalog holders live in the
// scene and hand out the very same objects
Obj* find_catalog_fd(int chara) {
    if (Obj* f = catalog_fd(chara)) return f;
    auto k = known_fd.find(chara);
    return k != known_fd.end() ? k->second : nullptr;
}

void catalog_update(Obj* fd, int chara) {
    for (int lv = 0; lv <= 2; ++lv)
        for (int it = 0; it <= 1; ++it)
            if (Obj* ld = call_obj(fd, "GetLevelListData", { arg_i32(lv), arg_i32(it) })) update_list(ld, chara, it);
}

int current_chara() {
    if (last_chara.load() > 0) return last_chara.load();
    if (Obj* inst = ctm()) { const int pv = static_cast<int>(inum(inst, "_PrevFighterID", 0)); if (pv > 0) return pv; }
    if (Obj* m = mission()) { const int c = static_cast<int>(inum(m, "CharaID", 0)); if (c > 0) return c; }
    return -1;
}

// ---------------------------------------------------------------- the Fighting Ground entry
// The Practice submenu is data: FGMainMenuData.ArrData[c].ModeTypes is an array of ModeTypeData. One more row = one more
// cell, copied from the Combo Trial row (Map 14) with our own message GUIDs for the label and the guide line.
// Why the entry is not in yet: logged once per reason (the function runs every 30 frames)
int menu_why_last = -1;
void menu_why(int code, const char* why) { if (code != menu_why_last) { menu_why_last = code; logf("menu entry not added yet: %s", why); } }

void ensure_menu_entry() {
    Obj* md = menu_data.load();
    if (!md) {                                   // app.GuiManager holds it from the start (found 14/11): no menu call needed
        Obj* gm = singleton("app.GuiManager");
        Obj* d = gm ? get_obj(gm, "FGMainMenuData") : nullptr;
        if (d && live(d)) {
            Obj* expected = nullptr;
            if (menu_data.compare_exchange_strong(expected, d)) d->add_ref();
            md = menu_data.load();
        }
    }
    if (!md) { menu_why(1, "the Fighting Ground menu data not reached yet (app.GuiManager.FGMainMenuData, no menu call seen)"); return; }
    Obj* arrd = get_obj(md, "ArrData");
    for (int32_t i = 0, n = arr_len(arrd); i < n; ++i) {
        Obj* sub = arr_get(arrd, i);
        if (!sub || inum(get(ref(sub), "Category"), "Category", -1) != kPracticeCategory) continue;
        Obj* modes = get_obj(sub, "ModeTypes");
        const int nm = arr_len(modes);
        if (nm < 1) { menu_why(2, "Practice has no ModeTypes"); return; }
        Obj* tpl = nullptr;
        int dup = -1;
        for (int k = 0; k < nm; ++k) {
            Obj* e = arr_get(modes, k);
            if (e && inum(e, "Map", -1) == kFlowComboTrial) { if (tpl) dup = k; else tpl = e; }
        }
        if (!tpl) { menu_why(3, "no Combo Trial row (Map 14) in Practice"); return; }
        Obj* mine = menu_entry.load();
        if (mine && dup >= 0 && arr_get(modes, dup) == mine) return;      // already in
        Obj* e = new_obj(type("app.menu.ModeTypeData"), false);
        if (!e) { menu_why(4, "ModeTypeData not created"); return; }
        set(e, "Enable", 1);
        for (const char* f : { "ModeType", "IsRootFlow", "Map", "Scene", "BGM_Key", "DecideNar" }) {
            Field* fl = field_of(td_of(tpl), f);
            TD* ft = fl ? fl->get_type() : nullptr;
            if (!ft) continue;
            if (ft->is_valuetype()) {                         // a value as it is (number, enum, or a struct such as a Guid)
                void* src = raw_of(ref(tpl), f);
                void* dst = raw_of(ref(e), f);
                const uint32_t sz = ft->get_valuetype_size();
                if (src && dst && sz && sz <= 64) std::memcpy(dst, src, sz);
            } else set_ref(e, f, get_obj(tpl, f));
        }
        uint8_t gn[16], gg[16];
        if (!text_guid("Custom Combos", gn) || !text_guid("Your own combos as trials. Record one in the first row: play it, it is saved.", gg)) { menu_why(5, "the core gave no text GUID"); return; }
        write_guid(e, "Name", gn);
        write_guid(e, "GuideMessage", gg);
        if (dup >= 0) {                                           // a stale row of ours (never seen natively; kept as the Lua)
            arr_set(modes, dup, e);
            menu_entry.store(e);
            logf("menu entry replaced (row %d)", dup);
        } else {
            Obj* bigger = new_array(type("app.menu.ModeTypeData"), nm + 1);
            if (!bigger) { menu_why(6, "the bigger ModeTypes array not created"); return; }
            for (int k = 0; k < nm; ++k) arr_set(bigger, k, arr_get(modes, k));
            arr_set(bigger, nm, e);
            if (!set_ref(sub, "ModeTypes", bigger)) { menu_why(7, "ModeTypes not written"); return; }
            menu_entry.store(e);
            logf("menu entry added to Practice (%d -> %d rows)", nm, nm + 1);
        }
        return;
    }
    if (menu_why_last != 8) {                                    // what ArrData holds instead, once
        std::string seen;
        for (int32_t i = 0, n = arr_len(arrd); i < n; ++i) {
            Obj* sub = arr_get(arrd, i);
            seen += (seen.empty() ? "" : ",") + std::to_string(sub ? inum(get(ref(sub), "Category"), "Category", -1) : -9);
        }
        logf("ArrData: %d categories [%s]", arr_len(arrd), seen.c_str());
        // the captured object, its type and every field (parents included), once: to see where the categories are
        TD* t = td_of(md);
        logf("menu data object: %s, ArrData %s", t ? t->get_full_name().c_str() : "?", arrd ? "found" : "null");
        for (TD* c = t; c; c = c->get_parent_type()) {
            std::string fl;
            for (Field* f : c->get_fields()) {
                if (!f || !f->get_name()) continue;
                TD* ft = f->get_type();
                fl += std::string(fl.empty() ? "" : ", ") + f->get_name() + ":" + (ft ? ft->get_full_name() : "?");
            }
            logf("  fields of %s: %s", c->get_full_name().c_str(), fl.c_str());
            if (c->get_full_name() == "System.Object") break;
        }
    }
    menu_why(8, "no Practice category (Category 1) in ArrData");
}

// ---------------------------------------------------------------- [CUSTOM] on the guide line
// Only while the trial list is on screen or a trial of ours is fought; taken off the guide line otherwise
bool guide_marked = false;
namespace manage_ui { bool available(); bool busy(); void tick(); void leave(); }   // (cc_manage.hpp)
namespace lab { void tick(Obj* m, const Trial& t); void status_tick(); int pre_next_stage(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long); void post_next_stage(void**, REFrameworkTypeDefinitionHandle, unsigned long long); }   // (cc_lab.hpp)

// On the trial list, when the sub-menu of a trial can open, the mark also names its key
void mark_guide(bool want_mode, bool in_fight) {
    const bool has_list = !agents(kListAgent).empty();
    bool want = want_mode && (has_list || in_fight);
    if (g_rec->training_menu_open()) want = false;   // the training menu in the slot: its guides are its own
    const std::string mark = kMark, hint = std::string(kMark) + "   T: Rename / Move / Delete";
    auto ends = [](const std::string& a, const std::string& b) { return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0; };
    for (Obj* a : agents("InputGuide")) {
        Obj* ctrl = call_obj(a, "get_ControlMain");
        Obj* t = ctrl ? find_named(ctrl, "e_text", 5) : nullptr;
        if (!t) continue;
        const std::string cur = message(t);
        std::string base = cur;
        if (ends(base, hint)) base.resize(base.size() - hint.size());
        else if (ends(base, mark)) base.resize(base.size() - mark.size());
        const std::string next = base + (want ? (has_list && manage_ui::available() ? hint : mark) : std::string{});
        if (next != cur && !base.empty() && base.find(kMark) == std::string::npos) set_message(t, next);
    }
    guide_marked = want;
}

// ---------------------------------------------------------------- list preview
// The trial list (UI11414) keeps one ItemDataInfo per row: its official entry and the official StageInfoRoot it previews
// in the side recipe, the same asset object the battle uses. So while our list shows, each borrowed row's asset carries
// our recipe (a fresh ParamInfo[]; the official one is kept and put back on leaving).
struct Focus { TrialP trial; int64_t level; uint64_t at; };
std::optional<Focus> g_focus;                  // the row under the cursor while the list is up
struct Picked { TrialP trial; int64_t level; std::optional<int> stage; int wait = 0; };
std::optional<Picked> g_picked;                // the row decided, adopted when a trial starts right after
struct Now { TrialP trial; int level; int stage; uint64_t at; };
std::optional<Now> g_now;                      // the trial the fight runs, its tab and index (read by the StageSetup hook)

// The slot's row focused and no take yet: the side recipe reads the hint (texts only, compared first)
void list_hint(Obj* ctrl, int chara) {
    if (g_rec->has_take(chara)) return;
    Obj* list = find_child(ctrl, "p_ScrollList_0_v");
    if (!list) return;
    Obj* focused = nullptr;
    for (Obj* it = child(list); it && !focused; it = next(it)) {
        const std::string nm = name_of(it);
        if ((nm == "c_Item" || is_item_name(nm)) && call_str(it, "get_PlayState") == "SELECT") focused = it;
    }
    Obj* title = focused ? find_child(focused, "e_txt_title") : nullptr;
    if (!title || message(title) != rec::kSlotTitle) return;
    Obj* panel = find_child(ctrl, "p_ComboTrialRecipe_0");
    Obj* rows = panel ? find_child(panel, "p_ScrollList_0_v") : nullptr;
    for (Obj* row = rows ? child(rows) : nullptr; row; row = next(row)) {
        const std::string nm = name_of(row);
        if (nm == "c_item" || is_item_name(nm)) {
            if (Obj* txt = find_child(row, "e_txt_item")) set_message(txt, nm == "c_item" ? rec::kSlotHint : "");
        }
    }
}

void preview_tick() {
    static uint64_t pc = 0;
    ++pc;
    for (Obj* a : agents(kListAgent)) {
        Obj* prm = get_obj(get_obj(get_obj(a, "<InputDelegates>k__BackingField"), "<Flow>k__BackingField"), "_Param");
        if (!prm) continue;
        if (cfg.rec) {
            Obj* ctrl = call_obj(a, "get_ControlMain");
            const auto cid = num(prm, "<CharaId>k__BackingField");
            if (ctrl && cid) list_hint(ctrl, static_cast<int>(*cid));
        }
        // the row under the cursor, every frame: the trial that starts when it is decided
        Obj* sl = get_obj(prm, "<PartsScrollListItem>k__BackingField");
        Obj* cur_items = get_obj(prm, "<CurrentItemDataInfoList>k__BackingField");
        if (sl && cur_items) {
            const auto fi = call_i32(sl, "get_SelectedIndex");
            if (fi && *fi >= 0 && *fi < list_count(cur_items)) {
                Obj* fit = list_item(cur_items, *fi);
                Obj* fentry = get_obj(fit, "<BattleFGComboTrialData>k__BackingField");
                auto ft = fentry ? entry_trial.find(fentry) : entry_trial.end();
                if (ft != entry_trial.end()) g_focus = Focus{ ft->second, inum(fit, "<LevelCategory>k__BackingField", 0), frame };
            }
        }
        if (pc % 3 != 0) continue;
        Obj* items = get_obj(prm, "<AllItemDataInfoList>k__BackingField");
        for (int32_t k = 0, n = list_count(items); k < n; ++k) {
            Obj* it = list_item(items, k);
            Obj* entry = get_obj(it, "<BattleFGComboTrialData>k__BackingField");
            auto et = entry ? entry_trial.find(entry) : entry_trial.end();
            if (et == entry_trial.end()) continue;
            Trial& t = *et->second;
            Obj* root = get_obj(it, "<StageInfoRoot>k__BackingField");
            // A copy previews through an asset of its own: two rows writing their recipe into one asset show each
            // other's lines. A shallow copy of the root and of its StageInfo, made once per copied entry and kept.
            if (root && copied.count(entry)) {
                Obj*& own = copy_roots[entry];
                if (!own) {
                    if (Obj* si0 = get_obj(root, "Root")) {
                        Obj* r2 = clone(root);
                        Obj* si1 = r2 ? clone(si0) : nullptr;
                        if (si1 && set_ref(r2, "Root", si1)) own = r2;
                    }
                }
                if (own && root != own && set_ref(it, "<StageInfoRoot>k__BackingField", own)) root = own;
            }
            Obj* si = root ? get_obj(root, "Root") : nullptr;
            if (!si || preview_done[si] == &t || !asset_ready(si) || g_decided.load()) continue;   // (rule 29)
            keep_asset(si);
            resolve_skills(t, t.fighter_id);
            std::vector<Step> slot_steps;
            std::vector<const Step*> ps;
            if (t.rec) { slot_steps = g_rec->preview_steps(t); for (const Step& s : slot_steps) ps.push_back(&s); }
            else for (const Step& s : t.steps) ps.push_back(&s);
            if (set_info(si, param_array(ps, true))) preview_done[si] = &t;
            // (NOT the list's own UpdateRecipe(): an access violation at the list's first opening after a game start)
        }
    }
}

// ---------------------------------------------------------------- what the recorder borrows (part 2)
namespace mode {
[[maybe_unused]] bool rebuild_recipe(Obj* m, Trial& t) { return apply_window(m, t, 0, true); }
[[maybe_unused]] bool example_playing() { Obj* fm = flow_map(); return fm && flag(fm, "IsExamplePlaying"); }
// the slot's settings changed in the pause menu: on the asset (the next Reset Battle reads it) and live
[[maybe_unused]] bool apply_settings(Trial& t, bool asset_only) {
    Obj* m = mission();
    Obj* si = get_obj(get_obj(m, "mpStageAsset"), "Root");
    if (!si || !asset_ready(si)) return false;
    try { apply_baseline(si, t); apply_recipe_raw(si, t); apply_dummy_asset(si, t); apply_gauges(si, t); apply_position(si, t); } catch (...) {}
    if (!asset_only) refill_next = 0;
    return true;
}
// a saved take joins its fighter's set at once; the lists are rebuilt on the next look
std::atomic<bool> want_refresh{false};
[[maybe_unused]] void on_saved(int chara, TrialP t) {
    if (!sets_loaded) return;
    for (Step& st : t->steps) resolve_step(st, labels_of(chara).groups);
    auto& set = all_sets[chara];
    auto it = std::find_if(set.begin(), set.end(), [&](const TrialP& x) { return !x->path.empty() && x->path == t->path; });
    if (it != set.end()) { t->file_order = (*it)->file_order; *it = t; }
    else { t->file_order = static_cast<int>(set.size()) + 1; set.push_back(t); }
    set_cache.erase(chara);
    want_refresh = true;
}
// a trial was renamed or moved from the list's sub-menu: the tabs are sorted again and the lists rebuilt
[[maybe_unused]] void manage_changed(int chara) {
    set_cache.erase(chara);
    want_refresh = true;
}
[[maybe_unused]] void on_discard(int chara, const std::string& path) {
    auto& set = all_sets[chara];
    set.erase(std::remove_if(set.begin(), set.end(), [&](const TrialP& x) { return x->path == path; }), set.end());
    set_cache.erase(chara);
    want_refresh = true;
}
}  // namespace mode

// The recipe's header ("Beginner 4") names the official trial a row past the count borrows: its number replaced by the
// row's own (the hook on UpdateContents does it first, cc_index.hpp; this is the check behind it)
void header_tick(int row, int size) {
    if (size < 1 || row < size) return;          // one of the game's own rows: its name is right
    Obj* ctrl = recipe_ctrl();
    Obj* head = ctrl ? find_child(ctrl, "c_head") : nullptr;
    Obj* title = head ? find_child(head, "e_txt_title") : nullptr;
    if (!title) return;
    if (auto want = rows::header_text(message(title), row)) set_message(title, *want);
}

// ---------------------------------------------------------------- per frame
int flow_prev = -999;
uint64_t judge_check_at = 0;
int64_t applied_slot = -1;                     // the row our recipe was written for (a change of row rewrites it)
Obj* slot_si = nullptr;
TrialP slot_trial;
std::optional<bool> slot_preview;
bool rec_live = false;
std::optional<double> editor_stamp;

void leave_trial() {
    g_seen.reset();
    out_at = frame;
    slot_si = nullptr;
    slot_trial.reset();
    slot_preview.reset();
    if (rec_live) { rec_live = false; g_rec->leave(); }
    if (applied.exchange(false)) logf("left combo trial, disarmed the installed trial");
    applied_slot = -1;
    restore_live_all();
    pending_recipe_open.reset();
    follow_trial = nullptr;
    diag = Diag{ diag.flow };
}

void tick() {
    const int f = core->get_flow_id();
    try { lab::status_tick(); } catch (...) {}
    diag.flow = f;
    try { g_rec->frame(f); } catch (...) {}
    if (g_decided.load()) { if (!decided_since) decided_since = frame; else if (frame - decided_since > 600) { g_decided = false; decided_since = 0; } }
    else decided_since = 0;
    if (f == kFlowReplay || g_rec->replay_on()) {
        try { g_rec->replay_tick(f == kFlowReplay); } catch (...) {}
    }
    if (f != flow_prev) { flow_prev = f; settle_until = frame + kSettle; }
    ++mc;
    if (mc % 30 == 1) try { ensure_menu_entry(); } catch (...) {}
    release_due();

    const bool custom = custom_mode.load();
    // the lists only matter on menu screens (and until ours are in): not during the fight
    // (and only while our section is chosen or one of our lists is still in: the official screens are left alone)
    const bool catalog_due = f != kFlowComboTrial || in_pause || (custom && custom_lists.empty());
    if ((custom || !custom_lists.empty()) && catalog_due && mc % 10 == 1) {
        Obj* inst = ctm();
        int chara = last_chara.load() > 0 ? last_chara.load() : (inst ? static_cast<int>(inum(inst, "_PrevFighterID", -1)) : -1);
        Obj* fd = inst ? get_obj(inst, "_ComboTrialFighterData") : nullptr;
        if (fd) {
            const int c = chara_of_fd(fd);
            if (c > 0) chara = c;
            else if (catalog_data) fd = nullptr;   // the catalog does not know it: never given to the fighter seen last
        }
        if (fd && chara > 0 && !catalog_data) keep_fd(chara, fd);   // (a guess only while no catalog is reachable)
        if (!fd && chara > 0) fd = find_catalog_fd(chara);
        if (custom) {
            // every fighter with trials gets its lists, once (done again after a restore)
            if (!sets_loaded) read_all_sets();
            std::vector<int> ids;
            for (const auto& [id, set] : all_sets) if (!set.empty() && !lists_done[id]) ids.push_back(id);
            for (int id : ids) {
                Obj* x = catalog_fd(id);
                if (!x) x = known_fd.count(id) ? known_fd[id] : nullptr;
                if (x) { try { catalog_update(x, id); lists_done[id] = true; why_once("lists", "fighter " + std::to_string(id) + "'s lists given"); } catch (...) { why_once("lists", "exception for fighter " + std::to_string(id)); } }
                else why_once("lists", "no catalog entry yet for fighter " + std::to_string(id));
            }
        }
        if (fd && chara > 0) try { catalog_update(fd, chara); } catch (...) {}
    }
    // The editor bumps _editor.json's stamp when it saves: the set, the config and the trial on screen are reloaded
    if (custom && mc % 60 == 30) {
        const json sig = read_json(kData / "_editor.json");
        const auto stamp = num(at(sig, "stamp"));
        if (stamp && stamp != editor_stamp) {
            if (editor_stamp) {
                load_cfg();
                sets_loaded = false;
                set_cache.clear();
                mode::want_refresh = true;
                applied_slot = -1;
                logf("the editor saved: custom set reloaded");
            }
            editor_stamp = stamp;
        } else if (!stamp && !editor_stamp) editor_stamp = 0;
    }
    // a recording was saved: the lists are put back and rebuilt with it (by the catalog look above)
    const bool refresh = custom && mode::want_refresh.exchange(false);
    if (!custom || refresh) restore_lists(refresh ? "trial lists rebuilt with the new set" : "official trial lists and names restored");
    if (!custom && !asset_orig.empty() && frame >= settle_until) {
        int n = 0, gone = 0;
        for (auto& [a, o] : asset_orig) {
            if (asset_ready(o.si, o.info, true)) {
                try { set_info(o.si, o.info); restore_position(o.si); restore_live_gauges(o.si); } catch (...) {}
                ++n;
            } else ++gone;
            preview_done.erase(a);
        }
        asset_orig.clear();
        logf("%d borrowed official trial(s) put back, %d no longer loaded", n, gone);
    }
    // an asset the StageSetup hook wrote and no install followed: its stage and team values go back the same way
    if (!custom && !setup_si.empty() && frame >= settle_until) {
        for (Obj* s : setup_si) {
            if (asset_ready(s)) try { restore_position(s); restore_live_gauges(s); } catch (...) {}
            s->release();
        }
        setup_si.clear();
    }
    const bool in_menu = f != kFlowComboTrial || in_pause;
    if ((custom || guide_marked) && in_menu && mc % 15 == 0 && frame >= settle_until)
        try { mark_guide(custom, f == kFlowComboTrial); } catch (...) {}
    if (custom && in_menu && frame >= settle_until)
        try { preview_tick(); } catch (...) {}
    // the sub-menu of a trial (the list on screen, or a window / box of it still up)
    if ((custom && in_menu) || manage_ui::busy()) try { manage_ui::tick(); } catch (...) {}
    if (!custom && manage_ui::busy()) try { manage_ui::leave(); } catch (...) {}
    if (f != kFlowComboTrial) { leave_trial(); return; }
    // an official trial with nothing of ours to put back: not looked at
    if (!custom && !applied.load() && !rec_live && team_orig.empty() && pos_orig.empty() && params_orig.empty() &&
        !counter_orig && !dummy_orig) return;

    if (diag.level == -1 || mc % 5 == 0) trial_slot(diag.level, diag.stage, diag.stage_id);
    if (!hidden_recipes.empty() && !pending_recipe_open && !unhide_at && mc % 30 == 0) try { unhide_reused_recipe(); } catch (...) {}

    Obj* m = mission();
    if (!m) return;
    const int chara = static_cast<int>(inum(m, "CharaID", -1));
    diag.chara = chara;
    Obj* si = get_obj(get_obj(m, "mpStageAsset"), "Root");
    Obj* info = get_obj(m, "Info");
    if (mc % 5 == 0 || diag.first_action == -1) {
        int64_t c = -1;
        Obj* dl = list_count(info) > 0 ? get_obj(list_item(info, 0), "Data") : nullptr;
        if (dl && list_count(dl) > 0) c = inum(list_item(dl, 0), "Action", -1);
        diag.first_action = c;
    }
    const int64_t cur = diag.first_action;

    if (!custom) {
        if (rec_live) { rec_live = false; g_rec->leave(); }
        if (applied.exchange(false)) applied_slot = -1;
        if (si && !team_orig.empty()) try { restore_live_gauges(si); } catch (...) {}
        if (si && !pos_orig.empty()) try { restore_position(si); } catch (...) {}
        restore_live_all();
        return;
    }
    // one file per row; past the last one the game's auto-progression wraps back to ours
    LevelSet& ls = level_set(chara, diag.level, input_modern());
    const auto& set = ls.rows;
    if (set.empty()) return;
    int64_t slot = static_cast<int64_t>(std::max(diag.level, 0)) * 1000 + std::max(diag.stage, 0);
    int row = std::max(diag.stage, 0) % static_cast<int>(set.size());
    TrialP T = set[static_cast<size_t>(row)];
    // More rows than the game's own: a row past the official count runs as the official trial it borrows (row % size).
    // The row picked in the list stands while the manager is on that index; when the game moves on by one, so does it.
    // (With the index hooks placed the manager's index IS the row and none of this runs: kept for a build without them.)
    if (!real_index.load()) {
        const int size = ls.size;
        const int n = static_cast<int>(set.size());
        auto index_of = [&](const TrialP& t) { for (int i = 0; i < n; ++i) if (set[i] == t) return i + 1; return 0; };
        if (g_focus && size > 0 && n > size && !in_pause && frame - g_focus->at < 1500) {
            Obj* gm = static_obj("gBattle", "Game");
            const auto clock = gm ? num(gm, "stage_timer") : std::nullopt;
            if (clock && *clock < 200) {      // a trial that starts (its clock is young) within 25 s of the list being up
                g_picked = Picked{ g_focus->trial, g_focus->level, std::nullopt, 0 };
                g_focus.reset();
            }
        }
        if (size > 0 && n > size && g_picked && !g_picked->stage && g_picked->wait < 30) {
            const int pi0 = index_of(g_picked->trial);
            if (!(pi0 && g_picked->level == diag.level && ((pi0 - 1) % size) == diag.stage)) { ++g_picked->wait; return; }
        }
        if (size > 0 && n > size && g_picked) {
            int pi = g_picked->level == diag.level ? index_of(g_picked->trial) : 0;
            if (pi && g_picked->stage && *g_picked->stage != diag.stage) {
                if (pi < n && (pi % size) == diag.stage) { ++pi; g_picked->trial = set[pi - 1]; } else pi = 0;
            }
            if (pi && ((pi - 1) % size) == diag.stage) {
                g_picked->stage = diag.stage;
                row = pi - 1;
                T = set[static_cast<size_t>(row)];
                slot = static_cast<int64_t>(std::max(diag.level, 0)) * 1000 + row;
            } else g_picked.reset();
        }
    }
    if (!T) return;
    // The manager's index changes the frame a row is decided; the mission and its asset follow when the game has loaded
    // the trial, 2 to 150 frames later. Installed in between, the new row's recipe went into the OLD trial's asset and
    // the game built the judge and the lines from the new asset as it stood. So after a change of row nothing is
    // installed before the game's StageSetup for that row has been seen (its pre-hook lays the recipe).
    if (real_index.load()) {
        if (!g_seen || g_seen->level != diag.level || g_seen->stage != diag.stage)
            g_seen = Seen{ diag.level, diag.stage, frame, (g_seen ? g_seen->last : out_at) > 10 ? (g_seen ? g_seen->last : out_at) - 10 : 0, frame, false };
        g_seen->last = frame;
        if (!g_seen->ok) {
            if (g_setup.level == diag.level && g_setup.stage == diag.stage && g_setup.at >= g_seen->floor) g_seen->ok = true;
            else if (frame - g_seen->since > 600) {
                g_seen->ok = true;
                logf("no StageSetup seen for level %d index %d in 600 frames: installed anyway", diag.level, diag.stage);
            } else return;
        }
    }
    g_now = Now{ T, diag.level, diag.stage, frame };
    const int64_t want = T->steps.empty() ? 0 : T->steps[0].action_id;
    bool rebuilt = false;
    if (cur == want && applied_slot == slot && info && frame >= judge_check_at) {
        judge_check_at = frame + 10;
        rebuilt = !judge_intact(info, *T);
        if (rebuilt) logf("the game rebuilt the judge from the recipe: reinstalled");
    }
    if (cur != want || applied_slot != slot || rebuilt) {
        if (ls.size > 0 && static_cast<int>(set.size()) > ls.size) {       // two rows may share that entry
            try { if (Obj* inst = ctm()) if (Obj* entry = call_obj(inst, "GetComboTrialData")) apply_demo(entry, *T); } catch (...) {}
        }
        std::string why;
        bool ok = false;
        try { ok = install(m, *T, why); } catch (...) { why = "an exception"; }
        applied = ok;
        applied_slot = ok ? slot : -1;
        if (ok) { diag.first_action = want; follow_last = -1; armed_step = -1; }
        static std::string last_why;
        if (ok) { logf("installed %s (row %d)", T->title.c_str(), row); last_why.clear(); }
        else if (why != last_why) { last_why = why; logf("not installed: %s", why.c_str()); }
    } else applied = true;
    if (applied && mc % 30 == 7 && input_modern() != cur_modern) {
        applied_slot = -1;                              // the control type changed: lines and texts are laid again
        logf("control type changed: recipe laid again");
    }
    const bool rec_now = applied && T->rec;
    // The slot's asset holds 12 empty lines while the fight runs in it, and the list previews that very asset: the rest
    // of the time (pause menu up, another trial on screen) it carries the preview instead
    if (rec_now && si) { slot_si = si; slot_trial = T; }
    if (slot_si && slot_trial) {
        const bool w = !rec_now || in_pause;
        if (slot_preview != w) {
            slot_preview = w;
            if (w) {
                const std::vector<Step> ps = g_rec->preview_steps(*slot_trial);
                std::vector<const Step*> pp;
                for (const Step& s : ps) pp.push_back(&s);
                if (asset_ready(slot_si)) set_info(slot_si, param_array(pp, true));
            } else try { apply_window(m, *T, 0, false); } catch (...) {}
        }
    }
    if (rec_live && !rec_now) g_rec->leave();
    rec_live = rec_now;
    if (!applied) return;
    try { lab::tick(m, *T); } catch (...) {}               // Combo Studio's example builder, the game as its judge
    try { refill_live(m, *T); } catch (...) {}
    try { scroll_tick(m, *T); } catch (...) {}
    if (rec_now) { try { g_rec->tick(m, *T, chara); } catch (...) {} }
    else {
        try { follow_tick(m, *T, chara); } catch (...) {}
        try { own_text_tick(*T); } catch (...) {}
        if (frame % 20 == 3 && !pending_recipe_open && !in_pause) try { header_tick(row, ls.size); } catch (...) {}
    }
}

void on_late_update() {
    g_game_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!core || !client) return;
    std::lock_guard lk(g_mx);
    ++frame;
    try {
        update_pause();
        tick();
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("an exception in a frame"); }
    }
}

// ---------------------------------------------------------------- hooks (pass-through outside our mode)
thread_local int tl_pending_chara = -1;
thread_local int tl_lvl = -1, tl_it = -1;
thread_local void* tl_fd = nullptr;
thread_local bool tl_save_skipped = false;
bool save_returns = false;

int pre_fighter_data(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    // our own lookups are not the game showing that fighter
    tl_pending_chara = tl_own_call ? -1 : static_cast<int>(reinterpret_cast<intptr_t>(argv[2]));
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_fighter_data(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int chara = tl_pending_chara;
    tl_pending_chara = -1;
    if (chara <= 0 || !ret) return;
    try {
        Obj* fd = reinterpret_cast<Obj*>(*ret);
        why_once("game's GetComboTrialFighterData", "fighter " + std::to_string(chara) + (fd ? " answered" : " null"));
        if (!fd) return;
        last_chara = chara;
        if (!on_game_thread()) {
            std::unique_lock lk(g_mx, std::try_to_lock);
            if (lk.owns_lock()) know_fd(chara, fd);
            return;
        }
        std::lock_guard lk(g_mx);
        know_fd(chara, fd);
        if (custom_mode.load() || !custom_lists.empty()) catalog_update(fd, chara);   // nothing of ours: nothing more
    } catch (...) {}
}

int pre_level_list(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_fd = argv[1];
    tl_lvl = static_cast<int>(reinterpret_cast<intptr_t>(argv[2]));
    tl_it = static_cast<int>(reinterpret_cast<intptr_t>(argv[3]) & 0xFF);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// Every screen that shows trials asks for a level list: swapping the array here means the caller builds its rows from ours
void post_level_list(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const int level = tl_lvl, it = tl_it;
    Obj* fdo = reinterpret_cast<Obj*>(tl_fd);
    tl_lvl = tl_it = -1;
    tl_fd = nullptr;
    if (!ret || !*ret || level < 0 || level > 2 || !on_game_thread()) {
        if (custom_mode.load()) why_once("GetLevelListData", !ret || !*ret ? "no list returned" : (level < 0 || level > 2) ? "level " + std::to_string(level) : "not the game thread");
        return;
    }
    if (!custom_mode.load() && custom_lists.empty()) return;     // nothing of ours: one atomic read and a size
    try {
        std::lock_guard lk(g_mx);
        Obj* ld = reinterpret_cast<Obj*>(*ret);
        int chara = chara_of_fd(fdo);
        if (chara > 0) last_chara = chara;
        else chara = catalog_data ? -1 : current_chara();   // (a guess only while no catalog is reachable)
        if (chara > 0) update_list(ld, chara, it);
        else if (custom_mode.load()) why_once("GetLevelListData", "whose list it is not known (no catalog answer yet)");
    } catch (const std::exception& e) { why_once("GetLevelListData", std::string("exception: ") + e.what()); }
    catch (...) { why_once("GetLevelListData", "exception"); }
}

// These small getters can share their machine code with other types' (seen in game: the first call came with a closure
// object as "this"), so the object is kept only when it IS the menu data
TD* menu_data_td() { static TD* t = type("app.menu.FGMainMenuData"); return t; }
int pre_menu_data(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (menu_data.load() || !argv[1]) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    Obj* md = reinterpret_cast<Obj*>(argv[1]);
    if (!live(md) || td_of(md) != menu_data_td()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    Obj* expected = nullptr;
    if (menu_data.compare_exchange_strong(expected, md)) {
        md->add_ref();                                                           // the menu asset, kept
        // the entry at once, before this build reads the Practice rows (the tick's pass came after: missed on screen)
        if (on_game_thread()) { std::lock_guard lk(g_mx); try { ensure_menu_entry(); } catch (...) {} }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

int pre_pass(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void post_pass(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}
// Which row is being confirmed: by identity, since ours is a copy of the Combo Trial row
void post_select_data(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!ret || !*ret) return;
    Obj* mine = menu_entry.load();
    custom_mode = mine && *ret == mine;
}

int pre_trial_on_attack(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!custom_mode.load() || !applied.load() || !on_game_thread()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        std::lock_guard lk(g_mx);
        Obj* m = reinterpret_cast<Obj*>(argv[1]);
        Obj* w = reinterpret_cast<Obj*>(argv[2]);
        static TD* const mission_td = type("app.FBattleMission");                // shared machine code: only the judge itself
        if (td_of(m) != mission_td) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        if (rec_live) g_rec->on_attack(m, w);
        else on_attack_mode(m, w);
    } catch (...) {}
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// A custom trial must never mark an official trial cleared in the save data: skipped only in a combo trial of our section
int pre_save_stage(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_save_skipped = custom_mode.load() && cfg.guard_save && core && core->get_flow_id() == kFlowComboTrial;
    return tl_save_skipped ? REFRAMEWORK_HOOK_SKIP_ORIGINAL : REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_save_stage(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_save_skipped && ret && save_returns) *ret = nullptr;
    tl_save_skipped = false;
}

bool hook(const char* type_name, const char* method, int nparams, REFPreHookFn pre, REFPostHookFn post, Method** out = nullptr) {
    Method* m = method_of(type(type_name), method, nparams);
    const bool ok = m && m->get_function_raw() && m->add_hook(pre, post, false);
    if (out) *out = m;
    if (!ok) logf("hook not installed: %s.%s", type_name, method);
    return ok;
}

// ---------------------------------------------------------------- part 2: the recorder (uses everything above)
#include "cc_recorder.hpp"
#include "cc_manage.hpp"
#include "cc_index.hpp"
#include "cc_lab.hpp"

// ---------------------------------------------------------------- the stage settings, when the game reads them (rule 13)
// The game reads a trial's stage settings in ComboTrialManager.StageSetup(StageInfo): twice at every start and every
// Reset Battle, after TrainingManager.InitTempData / InitModeData and before tf_ParameterSetting.bBattleStart(), on
// the game thread (measured). install() runs from the fight's tick, after the first of those: written there only, the
// start positions, life and the own gauge arrived one battle late. So the same fields are written in a pre-hook of
// that call, on the asset the game is about to read. Which of our trials it is for, in order: the row the list was
// left on a moment ago, the row the fight adopted, the trial under way, the plain index; each only when it is a row of
// this set that runs as this index (a row past the game's own count runs as the index it borrows).
// the trial's first window of lines, on the asset StageSetup is given
void lay_recipe(Obj* si, Trial& t, int chara) {
    keep_asset(si);
    resolve_skills(t, chara);
    own_texts(t, chara);
    std::vector<const Step*> steps;
    for (int i = 0; i < window_size(t); ++i) steps.push_back(&t.steps[static_cast<size_t>(i)]);
    if (!set_info(si, param_array(steps, false, 0))) throw std::runtime_error("set_InfoProp refused");
    t.win_off = 0;
    preview_done.erase(si);                      // (the trial list draws this asset again when it shows)
}

void stage_settings(Obj* si) {
    if (!si || !live(si)) return;
    int lv = -1, st = -1, id = -1;
    trial_slot(lv, st, id);
    if (lv < 0 || st < 0) return;
    Obj* m = mission();
    int chara = m ? static_cast<int>(inum(m, "CharaID", 0)) : 0;
    const bool recent = g_focus && g_focus->trial && frame - g_focus->at < 600;   // (measured: 76 to 197 frames after the list)
    if (recent && g_focus->trial->fighter_id > 0) chara = g_focus->trial->fighter_id;
    if (chara <= 0) return;
    LevelSet& ls = level_set(chara, lv, input_modern());
    const auto& set = ls.rows;
    const int n = static_cast<int>(set.size()), size = ls.size;
    if (n == 0) return;
    auto fits = [&](const TrialP& t) {
        for (int i = 0; t && i < n; ++i)
            if (set[static_cast<size_t>(i)] == t) return i == st % n || (size > 0 && i + 1 > size && i % size == st);
        return false;
    };
    TrialP T;
    const char* how = "index";
    if (real_index.load()) T = set[static_cast<size_t>(st % n)];
    else if (recent && g_focus->level == lv && fits(g_focus->trial)) { T = g_focus->trial; how = "list"; }
    else if (g_picked && g_picked->level == lv && fits(g_picked->trial)) { T = g_picked->trial; how = "picked"; }
    else if (g_now && g_now->level == lv && g_now->stage == st && fits(g_now->trial)) { T = g_now->trial; how = "under way"; }
    else T = set[static_cast<size_t>(st % n)];
    if (!T) return;
    if (setup_si.insert(si).second) si->add_ref();   // its official values go back with the others when the section is left
    // The recipe's lines too (rule 29), when this row is not the one installed (a Reset Battle keeps its lines and its
    // scroll): the game builds the judge and the recipe on screen from this asset right after this call
    if (real_index.load() && applied_slot != static_cast<int64_t>(lv) * 1000 + st) {
        try { lay_recipe(si, *T, chara); } catch (...) { logf("stage setup: recipe not laid"); }
    }
    g_setup = SetupSeen{ lv, st, frame };
    g_decided = false;
    try { apply_baseline(si, *T); } catch (...) {}
    try { apply_recipe_raw(si, *T); } catch (...) {}
    try { apply_dummy_asset(si, *T); } catch (...) {}
    try { apply_gauges(si, *T); } catch (...) {}
    try { apply_position(si, *T); } catch (...) {}
    // the fighter's own gauge: the trial's training data too (C. Viper's timer is never copied there by the game)
    recorder::stage_own_gauge(T->fighter_id, obj(&T->j, "unique"));
    static std::string last;
    const std::string line = T->title + " [" + how + "]";
    if (line != last) { last = line; logf("stage settings at setup: %s", line.c_str()); }
}

int pre_stage_setup(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    recorder::setup_calls.fetch_add(1);           // (a reset left pending by the training menu ends here, whoever caused it)
    if (!on_game_thread() || (!custom_mode.load() && recorder::setup_uq.empty())) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(g_mx);
    try {
        if (custom_mode.load()) stage_settings(reinterpret_cast<Obj*>(argv[2]));
        // An official trial starts: the own-gauge values we wrote into the trial's training data go back to zero before
        // the game writes its own. From this call only (a trial's by nature), never from a tick that may run with
        // another mode's training data in place (Training keeps the player's own Character-Specific Settings there).
        else recorder::own_gauge_clear_all();
    } catch (...) { logf("stage settings at setup failed"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void install_hooks() {
    hook("app.esports.FGComboTrialListUserData", "GetComboTrialFighterData", 1, pre_fighter_data, post_fighter_data);
    hook("app.esports.FGComboTrialListData", "GetComboTrialFighterData", 1, pre_fighter_data, post_fighter_data);
    hook("app.battle.assets.BattleComboTrialFighterData", "GetLevelListData", 2, pre_level_list, post_level_list);
    for (const char* mn : { "GetCategory", "GetCategoryName", "GetModeTypeName" })
        hook("app.menu.FGMainMenuData", mn, -1, pre_menu_data, post_pass);
    hook("app.menu.UIFlowFGMainMenuList.Param", "GetSelectData", -1, pre_pass, post_select_data);
    hook("app.FBattleMission", "TrialOnAttack", -1, pre_trial_on_attack, post_pass);
    Method* save = nullptr;
    if (hook("app.training.ComboTrialManager", "SaveStage", -1, pre_save_stage, post_save_stage, &save)) {
        TD* rt = save->get_return_type();
        save_returns = rt && rt->get_full_name() != "System.Void";
    } else logf("SaveStage not hooked: progression guard OFF");
    hook("app.training.ComboTrialManager", "StageSetup(CharacterAsset.Mission.StageInfo)", -1, pre_stage_setup, post_pass);
    hook("app.training.ComboTrialManager", "NextStage", 0, lab::pre_next_stage, lab::post_next_stage);   // only while the lab plays
    recorder::install_hooks();
    manage_ui::install_hooks();
    index_ui::install_hooks();
}

}  // namespace
}  // namespace cc

using namespace cc;

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1;
    v->minor = 10;    // API of REFramework 1.5.8
    v->patch = 0;
    v->game_name = nullptr;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->functions->on_pre_application_entry) return false;
    try { API::initialize(param); } catch (...) { return false; }
    std::error_code ec;
    if (fs::exists(kLuaOwner, ec)) {   // one owner per hook: the Lua version hooks the same methods
        API::get()->log_info("custom_combos: off (reframework/autorun/SF6_CustomTrials.lua is installed and owns these hooks)");
        return true;
    }
    HMODULE h = GetModuleHandleW(L"sf6core_native.dll");
    auto fn = h ? reinterpret_cast<SF6CoreGetApiFn>(GetProcAddress(h, "sf6core_get_api")) : nullptr;
    const SF6CoreAPI* a = fn ? fn(SF6CORE_ABI_VERSION) : nullptr;
    if (!a || !SF6CORE_HAS(a, obj_set_guid) || !SF6CORE_HAS(a, ui_agents)) {
        API::get()->log_info("custom_combos: off (needs the SF6 Native Core 0.9.14 or newer: its GUID texts)");
        return true;
    }
    SF6CoreClientDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.client_id = "custom_combos";
    desc.client_version = kVersion;
    desc.priority = 100;
    SF6CoreClient* c = nullptr;
    if (a->register_client(&desc, &c) != SF6CORE_OK) { API::get()->log_info("custom_combos: off (the core refused it)"); return true; }
    g::core = a;
    g::client = c;
    load_cfg();
    g_rec = recorder::make();
    install_hooks();
    param->functions->on_pre_application_entry("LateUpdateBehavior", on_late_update);
    if (param->functions->on_message) param->functions->on_message(manage_ui::on_message);   // T: the trial sub-menu
    logf("on (version %s)", kVersion);
    return true;
}

// The version the SF6 Mod Suite updater reads (updater/src/update_game.cpp)
extern "C" __declspec(dllexport) const char* sf6suite_package_version(void) { return kVersion; }
