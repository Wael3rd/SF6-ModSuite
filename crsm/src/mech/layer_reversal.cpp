// mech/reversal — the "reversal" layer (P3c) in the game (ReversalSlots.lua, RSL): the virtual-slot map of the reversal
// rows (_reversal_virtual.json), its hygiene on the game's setters, the 30 rows per category (ours 10-29 in
// _reversal_rows.json, the arrays installed while the menu is open), and the trigger: UpdateReversalCheck presents rows
// 11-30 through a work array of 10, and a virtual row reads its own recording from the 30-slot array (mech::bigc).
// Hooks only when settings.json gives the layer to the DLL, after the R18 check. The picker and the page stay in the Lua.
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

#include <windows.h>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "mech/bigc.hpp"
#include "mech/hooks.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/lutil.hpp"
#include "model/reversal.hpp"
#include "model/setup.hpp"
#include "model/store.hpp"
#include "perf.hpp"

namespace mech::reversal {
namespace {

namespace gm = game::gm;
namespace rv = model::reversal;
using gm::MO;
using F = cmn::Feature;

constexpr const char* kRdT = "app.training.TrainingData.TM_ReversalSetting.ReversalData";
constexpr const char* kRtFields[3] = { "DownReversalDatas", "GuardReversalDatas", "DamageReversalDatas" };
constexpr const char* kRdFields[9] = { "Type", "IsActive", "IsValid", "IsFrame", "Delay", "Count", "SkillIndex", "Level", "Frame" };

model::store::RevMap g_map;
std::optional<std::string> g_map_char;

struct Ext {
    uintptr_t fd_addr = 0;
    std::string char_name;
    MO* big[3]{};                       // ReversalData[30]: 0-9 the game's objects, 10-29 ours
    MO* game[3]{};                      // the game's 10-arrays
    MO* scratch[3]{};                   // the work arrays of 10 for the trigger
    bool installed = false;
};
Ext g_ext;
bool g_rows_dirty = false;
bool g_hand_back_due = false;                              // handed back while the pause menu was up: done at the unpause
model::draw::MtRng g_rng((uint64_t)std::time(nullptr) ^ 0x5eedull);
uint64_t g_triggers = 0, g_presented = 0, g_virtual = 0, g_map_writes = 0, g_saves = 0, g_nets = 0;

// Rows 11-30 (big[rt][10..29]) are our own objects: the game reaches them only while the 30-row arrays are installed (the
// menu, where its setters write them). So what the trigger needs of them (valid / active / type / skill) is read once after
// each change (a new Ext, the arrays installed or put back, a setter, a mech event) and kept: the trigger then reads the
// game's rows 1-10 only, and only for a category with a row 11-30 in play; with none in any category and no mapped row,
// it returns before any game read (UpdateReversalCheck runs in fights).
struct Quiet {
    bool valid = false;
    int fid = -1;                       // what it was read for: the files layer's dummy, the character, mech::events()
    std::string char_name;
    uint64_t events = 0;
    bool extra[3]{};                    // a row 11-30 valid and active, per category
    rv::Row rows[3][rv::kRowsTotal];    // rows 10-29 as read (0-9 unused: the game's, read live)
    MO* objs[3][rv::kRowsTotal]{};
};
Quiet g_q;
uint64_t g_quiet = 0;                   // triggers that returned before any game read
void quiet_reset() { g_q.valid = false; }

struct Trigger { std::vector<std::pair<MO*, int>> changed; bool pushed = false; };
thread_local Trigger t_trig;           // UpdateReversalCheck pre -> post (R29)
thread_local int t_row = -1;           // a setter's row, pre -> post

int32_t i32(void* v) { return (int32_t)(uint32_t)(uintptr_t)v; }
model::files::Engine& files() { return mech::files::engine(); }

// ---- the map (RSL:30-55) -------------------------------------------------------------------------------------------------
void map_load() {
    const auto c = files().char_name();
    if (!c || c == g_map_char) return;
    g_map_char = c;
    g_map.load(files().load_doc(model::store::reversal_virtual_path(*c)));
}
void map_save() {
    if (!g_map_char) return;
    files().write_doc(model::store::reversal_virtual_path(*g_map_char), g_map.to_file());
    ++g_map_writes;
}
std::optional<int64_t> map_get_now(int64_t rt, int64_t row) {
    map_load();
    const auto n = g_map.get(rt, row);
    if (!n) return std::nullopt;
    const auto num = model::lutil::to_number(*n);
    return num ? model::lutil::to_int_key(*num) : std::nullopt;
}
void map_set(int64_t rt, int64_t row, int n) {
    map_load();
    if (!g_map_char) return;
    g_map.set(rt, row, n);
    map_save();
}
void map_clear(int64_t rt, int64_t row) {
    map_load();
    if (!g_map_char) return;
    g_map.erase(rt, row);
    map_save();
}

// ---- the game's reversal data (RSL:56-72, 419-426) ----------------------------------------------------------------------
MO* reversal_setting() { return gm::ref(gm::ref(gm::singleton("app.training.TrainingManager"), "_tData"), "ReversalSetting"); }
std::optional<int64_t> current_rtype() {
    int64_t v = 0;
    if (!gm::get_int(reversal_setting(), "ReversalType", &v)) return std::nullopt;
    return v;
}
MO* fighter_rev() {
    const int fid = mech::files::fighter();
    if (fid < 0) return nullptr;
    MO* rs = reversal_setting();
    int32_t fi = -1;
    if (!rs || !gm::call_i32(rs, "GetFighterIndex", &fi, { gm::arg_i32(fid) }) || fi < 0) return nullptr;
    return gm::list_item(gm::ref(rs, "FighterDataList"), fi);
}
rv::Row row_of(MO* d) {
    rv::Row r;
    if (!d) return r;
    r.present = true;
    gm::get_bool(d, "IsValid", &r.valid);
    gm::get_bool(d, "IsActive", &r.active);
    gm::get_int(d, "Type", &r.type);
    gm::get_int(d, "SkillIndex", &r.skill);
    return r;
}
void rd_copy(MO* dst, MO* src) {                    // RSL:416
    for (const char* f : kRdFields) { int64_t v = 0; if (gm::get_int(src, f, &v)) gm::set_int(dst, f, v); }
}
model::store::RdRow rd_row(MO* d) {
    model::store::RdRow r;
    int64_t v = 0;
    if (gm::get_int(d, "Type", &v)) r.Type = v;
    gm::get_bool(d, "IsActive", &r.IsActive);
    gm::get_bool(d, "IsValid", &r.IsValid);
    gm::get_bool(d, "IsFrame", &r.IsFrame);
    if (gm::get_int(d, "Delay", &v)) r.Delay = v;
    if (gm::get_int(d, "Count", &v)) r.Count = v;
    if (gm::get_int(d, "SkillIndex", &v)) r.SkillIndex = v;
    if (gm::get_int(d, "Level", &v)) r.Level = v;
    if (gm::get_int(d, "Frame", &v)) r.Frame = v;
    return r;
}
void rd_from_table(MO* d, const json& t) {          // RSL:418: only the fields the table holds
    if (!t.is_object()) return;
    for (const char* f : kRdFields) {
        auto it = t.find(f);
        if (it == t.end() || it->is_null()) continue;
        if (it->is_boolean()) gm::set_int(d, f, it->get<bool>() ? 1 : 0);
        else if (it->is_number()) gm::set_int(d, f, (int64_t)it->get<double>());
    }
}
MO* new_rd() {
    MO* d = gm::new_object(gm::type(kRdT));           // add_ref'd, held (the game may keep pointers to it)
    if (d) gm::call(d, "Init", {});
    return d;
}

// The rows are read back at every menu close (RSL:499-505); the file is written only when what it gets differs from what
// this session last wrote into it (the close wrote the same file on the game thread every time: 3-4.6 ms a close in the
// perf log of 13/10). The first close of a character's session always writes, as before.
std::string g_saved_doc;                             // the character, a newline, the document: what was last written
uint64_t g_save_skips = 0;
void ext_save() {                                    // RSL:427-436
    if (g_ext.char_name.empty() || !g_ext.big[0]) return;
    model::store::RevRows rows;
    for (int rt = 0; rt < 3; ++rt)
        for (int k = rv::kRowsPage; k < rv::kRowsTotal; ++k)
            if (MO* d = gm::array_get(g_ext.big[rt], k)) rows[(size_t)rt][k] = rd_row(d);
    const json doc = model::store::rev_rows_to_file(rows);
    std::string key = g_ext.char_name + '\n' + doc.dump();
    g_rows_dirty = false;
    if (key == g_saved_doc) { ++g_save_skips; return; }
    if (files().write_doc(model::store::reversal_rows_path(g_ext.char_name), doc)) g_saved_doc = std::move(key);
    else g_saved_doc.clear();
    ++g_saves;
}

void ext_uninstall() {                               // RSL:499-505: the game's 10-arrays back, the extra rows saved
    if (!g_ext.installed) return;
    quiet_reset();                                   // the menu may have changed rows 11-30
    MO* fd = fighter_rev();
    if (fd) for (int rt = 0; rt < 3; ++rt) { try { gm::set_ref(fd, kRtFields[rt], g_ext.game[rt]); } catch (...) {} }
    g_ext.installed = false;
    ext_save();
}

// ensure_ext (RSL:438-471): the arrays of the current fighter, built once per fighter / character (fd: fighter_rev() when
// the caller has it already)
bool ensure_ext(MO* fd = nullptr) {
    if (!fd) fd = fighter_rev();
    if (!fd) return false;
    const auto c = files().char_name();
    if (!c) return false;
    if (g_ext.fd_addr == (uintptr_t)fd && g_ext.char_name == *c && g_ext.big[0]) return true;
    if (g_ext.installed) ext_uninstall();
    quiet_reset();
    Ext e;
    e.fd_addr = (uintptr_t)fd;
    e.char_name = *c;
    json saved = files().load_doc(model::store::reversal_rows_path(*c));
    if (!saved.is_object()) saved = json::object();
    for (int rt = 0; rt < 3; ++rt) {
        MO* cur = gm::ref(fd, kRtFields[rt]);
        if (!cur) return false;
        MO* game = cur;
        MO* big = nullptr;
        const int32_t len = gm::array_len(cur);
        if (len != rv::kRowsPage) {                      // a 30-array left by an earlier session: its rows kept
            game = gm::new_array(gm::type(kRdT), rv::kRowsPage);
            if (!game) return false;
            for (int k = 0; k < rv::kRowsPage; ++k) gm::array_set(game, k, gm::array_get(cur, k));
            big = cur;
            big->add_ref();                              // held: the field lets go of it (the Lua's ext table held it)
            gm::set_ref(fd, kRtFields[rt], game);
            cmn::journal("reversal: leftover %d-row array adopted (type %d)", len, rt);
        }
        game->add_ref();
        if (!big) {
            big = gm::new_array(gm::type(kRdT), rv::kRowsTotal);
            if (!big) return false;
            for (int k = 0; k < rv::kRowsPage; ++k) gm::array_set(big, k, gm::array_get(game, k));
            const json rows = saved.contains(std::to_string(rt)) ? saved[std::to_string(rt)] : json();
            for (int k = rv::kRowsPage; k < rv::kRowsTotal; ++k) {
                MO* d = new_rd();
                if (!d) return false;
                if (rows.is_object() && rows.contains(std::to_string(k))) rd_from_table(d, rows[std::to_string(k)]);
                gm::array_set(big, k, d);
            }
        }
        MO* scratch = gm::new_array(gm::type(kRdT), rv::kRowsPage);
        if (!scratch) return false;
        for (int k = 0; k < rv::kRowsPage; ++k) { MO* d = new_rd(); if (!d) return false; gm::array_set(scratch, k, d); }
        e.big[rt] = big; e.game[rt] = game; e.scratch[rt] = scratch;
    }
    g_ext = e;
    cmn::journal("reversal: rows 3 x 30 ready for %s", c->c_str());
    return true;
}

bool ext_install() {                                 // RSL:487-498 (the page windows reset stays in the Lua)
    if (!ensure_ext()) { cmn::journal("reversal: rows: no fighter / character yet"); return false; }
    if (g_ext.installed) return true;
    MO* fd = fighter_rev();
    if (!fd) return false;
    for (int rt = 0; rt < 3; ++rt) gm::set_ref(fd, kRtFields[rt], g_ext.big[rt]);
    g_ext.installed = true;
    quiet_reset();
    cmn::journal("reversal: rows: 30-arrays installed for the menu");
    return true;
}

// ---- the arrays built ahead (frame callback, menu closed) ----------------------------------------------------------------
// ensure_ext built a fighter's 3 x 30 rows at the first menu opening (CreateViewData pre: 18.7 ms with the replay array's
// build in the first opening of the session, perf log 13/10) or at the first trigger of a fight. The same build runs ahead
// here, one part a frame: the saved rows file, then per category the 30-array with rows 10-19, rows 20-29, the work array;
// then it is put in place at once, as ensure_ext does. It starts some frames after a mech event or a change of the files
// layer's dummy / character, only when it can change something (ensure_ext would build), never while the arrays are
// installed, and gives up (the opening builds, as before) when the fighter or the character changes under it or when an
// array is not the game's 10 (a leftover 30-array: ensure_ext adopts it, a write into the game's data, at the opening).
constexpr int kPreDelay = 30, kPreParts = 1 + 3 * 3;
struct PreExt {
    int part = -1;                                  // -1 = idle; 0 = the start; 1..9 = the categories' parts
    uintptr_t fd_addr = 0;
    std::string char_name;
    json saved;
    Ext e;
    MO* game[3]{};
};
PreExt g_pre;
uint64_t g_pre_events = UINT64_MAX;
int g_pre_fid = INT32_MIN;
std::optional<std::string> g_pre_char;
int g_pre_wait = -1;                                // frames before the start (-1 = nothing to start)
uint64_t g_prebuilds = 0, g_pre_aborts = 0;

void pre_abort(const char* why) {
    if (g_pre.part > 0) cmn::journal("reversal: rows built ahead: given up (%s): the menu opening builds them", why);
    ++g_pre_aborts;
    g_pre = PreExt{};
}

bool pre_rows(MO* big, int rt, int from, int to) {  // rows from..to-1 of category rt into our 30-array
    const json rows = g_pre.saved.contains(std::to_string(rt)) ? g_pre.saved[std::to_string(rt)] : json();
    for (int k = from; k < to; ++k) {
        MO* d = new_rd();
        if (!d) return false;
        if (rows.is_object() && rows.contains(std::to_string(k))) rd_from_table(d, rows[std::to_string(k)]);
        gm::array_set(big, k, d);
    }
    return true;
}

void pre_part() {                                   // one part of the build (under lock(), menu closed, not installed)
    if (g_pre.part == 0) {
        MO* fd = fighter_rev();
        const auto c = files().char_name();
        if (!fd || !c) { pre_abort("no fighter / character yet"); return; }
        if (g_ext.fd_addr == (uintptr_t)fd && g_ext.char_name == *c && g_ext.big[0]) { g_pre = PreExt{}; return; }   // built already
        for (int rt = 0; rt < 3; ++rt) {
            MO* cur = gm::ref(fd, kRtFields[rt]);
            if (!cur || gm::array_len(cur) != rv::kRowsPage) { pre_abort("not the game's 10-arrays"); return; }
            g_pre.game[rt] = cur;
        }
        g_pre.fd_addr = (uintptr_t)fd;
        g_pre.char_name = *c;
        g_pre.saved = files().load_doc(model::store::reversal_rows_path(*c));
        if (!g_pre.saved.is_object()) g_pre.saved = json::object();
        g_pre.part = 1;
        return;
    }
    if (g_ext.fd_addr == g_pre.fd_addr && g_ext.char_name == g_pre.char_name && g_ext.big[0]) { g_pre = PreExt{}; return; }   // a trigger built them meanwhile: never replaced
    MO* fd = fighter_rev();                         // still the same fighter and character, its arrays the game's 10
    const auto c = files().char_name();
    if ((uintptr_t)fd != g_pre.fd_addr || !c || *c != g_pre.char_name) { pre_abort("fighter or character changed"); return; }
    const int rt = (g_pre.part - 1) / 3, step = (g_pre.part - 1) % 3;
    if (gm::ref(fd, kRtFields[rt]) != g_pre.game[rt]) { pre_abort("the game's array changed"); return; }
    MO* game = g_pre.game[rt];
    if (step == 0) {
        MO* big = gm::new_array(gm::type(kRdT), rv::kRowsTotal);
        if (!big) { pre_abort("array"); return; }
        for (int k = 0; k < rv::kRowsPage; ++k) gm::array_set(big, k, gm::array_get(game, k));
        g_pre.e.big[rt] = big;
        if (!pre_rows(big, rt, rv::kRowsPage, 20)) { pre_abort("rows"); return; }
    } else if (step == 1) {
        if (!pre_rows(g_pre.e.big[rt], rt, 20, rv::kRowsTotal)) { pre_abort("rows"); return; }
    } else {
        MO* scratch = gm::new_array(gm::type(kRdT), rv::kRowsPage);
        if (!scratch) { pre_abort("array"); return; }
        for (int k = 0; k < rv::kRowsPage; ++k) { MO* d = new_rd(); if (!d) { pre_abort("rows"); return; } gm::array_set(scratch, k, d); }
        g_pre.e.scratch[rt] = scratch;
    }
    if (++g_pre.part < kPreParts) return;
    // done: in place as ensure_ext puts its build (the game's arrays held, the old Ext's objects kept alive as before)
    if (g_ext.installed) { pre_abort("arrays installed meanwhile"); return; }
    Ext e = g_pre.e;
    e.fd_addr = g_pre.fd_addr;
    e.char_name = g_pre.char_name;
    for (int r = 0; r < 3; ++r) { e.game[r] = g_pre.game[r]; e.game[r]->add_ref(); }
    quiet_reset();
    g_ext = e;
    ++g_prebuilds;
    cmn::journal("reversal: rows 3 x 30 built ahead for %s (menu closed)", g_ext.char_name.c_str());
    g_pre = PreExt{};
}

void prebuild_tick(bool training) {
    const uint64_t ev = mech::events();
    const int fid = mech::files::fighter();
    const auto c = files().char_name();
    if (ev != g_pre_events || fid != g_pre_fid || c != g_pre_char) {   // something to look at again, a little later
        g_pre_events = ev;
        g_pre_fid = fid;
        g_pre_char = c;
        if (g_pre.part >= 0) pre_abort("a new event");
        g_pre_wait = kPreDelay;
    }
    if (!training || g_ext.installed || g_hand_back_due) { if (g_pre.part >= 0) pre_abort("menu / training"); return; }
    if (g_pre.part < 0) {
        if (g_pre_wait < 0) return;
        if (g_pre_wait-- > 0) return;
        if (fid < 0 || !c) return;                  // (no dummy yet: the next event starts again)
        g_pre.part = 0;
    }
    PERF_SUB(pp, "Frame:reversal_build_ahead");
    pre_part();
}

// ---- the trigger (RSL:553-622) -----------------------------------------------------------------------------------------
int64_t vframes(int n) { return files().vslots().frames(n); }

bool quiet_current() {
    if (!g_q.valid || g_ext.installed || g_q.fid != mech::files::fighter() || g_q.events != mech::events()) return false;
    const auto c = files().char_name();
    return c && *c == g_q.char_name;
}

// Rows 11-30 of the three categories, read from our objects (after ensure_ext: g_ext is the current fighter's).
void quiet_read() {
    Quiet q;
    for (int rt = 0; rt < 3; ++rt) {
        for (int k = rv::kRowsPage; k < rv::kRowsTotal; ++k) {
            q.objs[rt][k] = g_ext.big[rt] ? gm::array_get(g_ext.big[rt], k) : nullptr;
            q.rows[rt][k] = row_of(q.objs[rt][k]);
            const rv::Row& r = q.rows[rt][k];
            if (r.present && r.valid && r.active) q.extra[rt] = true;
        }
    }
    q.fid = mech::files::fighter();
    q.char_name = g_ext.char_name;
    q.events = mech::events();
    q.valid = true;
    g_q = std::move(q);
}

// Nothing virtual can play: no category with a row 11-30 in play (present_rows' plan would not be used) and no mapped
// row (the mapped-rows path would find nothing). Model reads only.
bool quiet_now() {
    if (!quiet_current()) return false;
    for (int rt = 0; rt < 3; ++rt) {
        if (g_q.extra[rt]) return false;
        const auto* mp = g_map.rows_of(rt);
        if (mp && !mp->empty()) return false;
    }
    return true;
}

bool present_rows(int rt, MO* rows, void** argv, MO* fd) {
    if (!ensure_ext(fd) || !g_ext.big[rt]) return false;
    if (rows != g_ext.game[rt]) return false;                      // not the game's own array (menu open): nothing
    if (!quiet_current()) quiet_read();
    if (!g_q.extra[rt]) return false;                              // no row 11-30 valid and active: rv::present's plan is unused
    rv::Row big[rv::kRowsTotal];
    MO* objs[rv::kRowsTotal]{};
    for (int k = 0; k < rv::kRowsPage; ++k) { objs[k] = gm::array_get(g_ext.big[rt], k); big[k] = row_of(objs[k]); }
    for (int k = rv::kRowsPage; k < rv::kRowsTotal; ++k) { objs[k] = g_q.objs[rt][k]; big[k] = g_q.rows[rt][k]; }
    const rv::Plan plan = rv::present(big, [&](int k) { return map_get_now(rt, k); }, vframes, g_rng);
    if (!plan.use) return false;
    MO* scratch = g_ext.scratch[rt];
    for (int i = 0; i < rv::kRowsPage; ++i) {
        MO* sd = gm::array_get(scratch, i);
        if (!sd) continue;
        if (i < (int)plan.cands.size()) {
            rd_copy(sd, objs[plan.cands[(size_t)i]]);
            if (plan.virt[(size_t)i]) gm::set_int(sd, "SkillIndex", plan.virt[(size_t)i] - 1);
        } else gm::call(sd, "Init", {});
    }
    if (plan.need_push) { if (bigc::push()) t_trig.pushed = true; else return false; }
    argv[5] = scratch;                                             // the ReversalData[] argument
    ++g_presented;
    return true;
}

void check_pre(void** argv) {
    std::lock_guard lk(mech::lock());
    t_trig = Trigger{};
    try {
        map_load();
        if (quiet_now()) { ++g_quiet; return; }                     // nothing virtual can play: no game read
        MO* rows = static_cast<MO*>(argv[5]);
        if (!rows) return;
        MO* fd = fighter_rev();
        if (!fd) return;
        int rt = -1;
        for (int k = 0; k < 3; ++k) if (gm::ref(fd, kRtFields[k]) == rows) rt = k;
        if (rt < 0) return;
        ++g_triggers;
        if (present_rows(rt, rows, argv, fd)) return;              // rows 11-30 in play: the work array stands in
        const auto* mp = g_map.rows_of(rt);
        if (!mp || mp->empty()) return;
        const auto changes = rv::mapped(*mp, [&](int row, rv::Row* out) {
            MO* d = gm::array_get(rows, row);
            *out = row_of(d);
            return d != nullptr;
        }, vframes);
        if (changes.empty()) return;
        if (!bigc::push()) { cmn::journal("reversal: trigger: the 30-slot array could not be installed"); return; }
        t_trig.pushed = true;
        for (const auto& c : changes) {
            MO* d = gm::array_get(rows, c.row);
            if (!d) continue;
            gm::set_int(d, "SkillIndex", c.n - 1);
            t_trig.changed.push_back({ d, c.n });
        }
        g_virtual += changes.size();
    } catch (...) {
        cmn::fault(F::Reversal, "UpdateReversalCheck pre");
    }
}
void check_post() {
    std::lock_guard lk(mech::lock());
    try {
        for (auto& [d, n] : t_trig.changed) gm::set_int(d, "SkillIndex", rv::kWork);
        if (t_trig.pushed) bigc::pop();
    } catch (...) {
        cmn::fault(F::Reversal, "UpdateReversalCheck post");
    }
    t_trig = Trigger{};
}

int pre_check(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !cmn::owns(F::Reversal)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { check_pre(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Reversal, "UpdateReversalCheck pre (SEH)"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_check(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing()) return;
    __try { check_post(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Reversal, "UpdateReversalCheck post (SEH)"); }
}

// ---- the setters (RSL:286-301, 645-655) ----------------------------------------------------------------------------------
// SetReversalData(fighter, row, type, skill, level): a virtual slot chosen in the picker is stored as slot 8 + the map
void set_data_pre(void** argv) {
    std::lock_guard lk(mech::lock());
    quiet_reset();
    try {
        const int64_t row = i32(argv[3]), ty = i32(argv[4]), skill = i32(argv[5]);
        t_row = (int)row;
        const auto rt = current_rtype();
        const bool mapped = rt && map_get_now(*rt, row).has_value();
        const rv::SetResult r = rv::on_set(rt, row, ty, skill, mapped);
        if (r.map_set) {
            map_set(*rt, row, r.n);
            argv[5] = gm::arg_i32(rv::kWork);
            cmn::journal("rev: row %lld (type %lld) = virtual slot %d", (long long)row, (long long)*rt, r.n);
        } else if (r.map_clear) {
            map_clear(*rt, row);
            cmn::journal("rev: row %lld (type %lld) back to the game's own choice", (long long)row, (long long)*rt);
        }
    } catch (...) {
        cmn::fault(F::Reversal, "SetReversalData pre");
    }
}
// RemoveReversalData(fighter, row, type): map hygiene
void remove_pre(void** argv) {
    std::lock_guard lk(mech::lock());
    quiet_reset();
    try {
        const int64_t row = i32(argv[3]), rt = i32(argv[4]);
        t_row = (int)row;
        if (map_get_now(rt, row)) { map_clear(rt, row); cmn::journal("rev: row %lld (type %lld) removed", (long long)row, (long long)rt); }
    } catch (...) {
        cmn::fault(F::Reversal, "RemoveReversalData pre");
    }
}
void setter_post_body() {                                          // rows >= 10 while installed: saved (on the frame callback)
    std::lock_guard lk(mech::lock());
    quiet_reset();
    if (t_row >= rv::kRowsPage && g_ext.installed) g_rows_dirty = true;
    t_row = -1;
}

int pre_set_data(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !cmn::owns(F::Reversal)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { set_data_pre(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Reversal, "SetReversalData pre (SEH)"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_remove(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !cmn::owns(F::Reversal)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { remove_pre(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Reversal, "RemoveReversalData pre (SEH)"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_setter(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !cmn::owns(F::Reversal)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    t_row = i32(argv[3]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_setter(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing() || !cmn::owns(F::Reversal)) return;
    __try { setter_post_body(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cmn::fault(F::Reversal, "setter post (SEH)"); }
}

// ---- bridge functions (RSL's shims) --------------------------------------------------------------------------------------
#define REV_FN(name) int32_t SF6CORE_CALL name(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max)
#define REV_TRY std::lock_guard<std::recursive_mutex> lk_(mech::lock()); br::Out o{ r, max }; (void)a; (void)n; try {
#define REV_END(what) } catch (...) { cmn::fault(F::Reversal, what); return o.done(); } return o.done();

// rev_menu_open(): ext_install (true = the 30-arrays are in place: the Lua then resets the page windows)
REV_FN(f_menu_open) { REV_TRY o.boolean(ext_install()); REV_END("menu_open") }
// rev_menu_close(): ext_uninstall
REV_FN(f_menu_close) { REV_TRY ext_uninstall(); REV_END("menu_close") }
// rev_map_get(rt, row): the virtual slot of a row (nil = none), for the picker and the page's texts
REV_FN(f_map_get) {
    REV_TRY
    const auto v = map_get_now(br::i64(a, n, 0, -1), br::i64(a, n, 1, -1));
    if (v) o.i64(*v); else o.nil();
    REV_END("map_get")
}

void frame(uint64_t) {
    if (g_hand_back_due) {
        std::lock_guard lk(mech::lock());
        if (!mech::pause_now().bit) {
            g_hand_back_due = false;
            try { ext_uninstall(); bigc::pop(); } catch (...) {}
            cmn::journal("reversal: hand-back done at the unpause");
        }
        return;
    }
    if (!cmn::owns(F::Reversal)) return;
    std::lock_guard lk(mech::lock());
    try {
        bigc::late_pop();                                          // a push whose post-hook never ran (RMT:2018)
        if (g_rows_dirty) ext_save();
        const State st = mech::state();
        // never the 30-arrays in a fight (menu up: kept; installed: mech polls the pause, needs_pause)
        if (g_ext.installed && (!st.training || (!mech::pause_now().bit && !mech::lua_alive()))) {
            ++g_nets;
            cmn::log_warn("[CRSMNative] reversal: the 30-row arrays were still in place (%s): put back by the DLL",
                          st.training ? "the Lua stopped calling" : "training left");
            ext_uninstall();
        }
        prebuild_tick(st.training);                                // the rows built ahead, menu closed
    } catch (...) {
        cmn::fault(F::Reversal, "frame");
    }
}

void hand_back() {
    std::lock_guard lk(mech::lock());
    quiet_reset();
    if (g_ext.installed && mech::pause_now().bit) {         // the page's rows 11-30 point past 10: put back at the unpause
        g_hand_back_due = true;
        cmn::log_warn("[CRSMNative] reversal: back to the Lua; the game's 10-row arrays put back at the unpause");
        return;
    }
    try { ext_uninstall(); bigc::pop(); } catch (...) {}
    cmn::log_warn("[CRSMNative] reversal: back to the Lua (the game's 10-row arrays in place)");
}

std::string status() {
    std::lock_guard lk(mech::lock());
    char b[400];
    std::snprintf(b, sizeof(b), "%s, rows %s, triggers %llu (+%llu with nothing virtual), presented 11-30 %llu, virtual rows %llu, map writes %llu, row saves %llu (+%llu unchanged), built ahead %llu (gave up %llu), safety %llu",
                  cmn::owns(F::Reversal) ? "native" : "lua", g_ext.installed ? "installed" : (g_ext.big[0] ? "built" : "-"),
                  (unsigned long long)g_triggers, (unsigned long long)g_quiet, (unsigned long long)g_presented,
                  (unsigned long long)g_virtual, (unsigned long long)g_map_writes, (unsigned long long)g_saves,
                  (unsigned long long)g_save_skips, (unsigned long long)g_prebuilds, (unsigned long long)g_pre_aborts,
                  (unsigned long long)g_nets);
    return b;
}

// ---- a pack's reversal rows (the files layer's training block) ---------------------------------------------------------------
// The game's rows are written field by field into its own ReversalData objects (no setter: the setters need the page's
// category in ReversalSetting.ReversalType, and their hooks would map the rows again); the map and rows 11-30 the way the
// picker and the menu close leave them. Called from an import (the Manager page's decide or the Lua's bridge call, the
// menu open: the moment the game's own reversal page writes these objects).
struct RevTarget { MO* fd = nullptr; std::string char_name; bool built = false; };
bool rev_target(int fid, RevTarget* t, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (!cmn::owns(F::Reversal)) return fail("the reversal layer is not native");
    if (fid < 0 || fid != mech::files::fighter()) return fail("not the files layer's dummy");
    t->fd = fighter_rev();
    if (!t->fd) return fail("no reversal data for the dummy");
    const auto c = files().char_name();
    if (!c) return fail("no character");
    t->char_name = *c;
    t->built = g_ext.big[0] && g_ext.fd_addr == (uintptr_t)t->fd && g_ext.char_name == *c;
    map_load();
    if (!g_map_char || *g_map_char != *c) return fail("no reversal map for the character");
    return true;
}

bool setup_read_now(int fid, model::setup::Rows* out, std::string* why) {
    RevTarget t;
    if (!rev_target(fid, &t, why)) return false;
    model::store::RevRows saved;
    if (!t.built) saved = model::store::rev_rows_from_file(files().load_doc(model::store::reversal_rows_path(t.char_name)));
    model::setup::Rows rows;
    for (int rt = 0; rt < 3; ++rt) {
        MO* arr = gm::ref(t.fd, kRtFields[rt]);
        if (!arr) { if (why) *why = "a reversal array is missing"; return false; }
        std::vector<model::store::RdRow>& v = rows[(size_t)rt];
        v.resize(rv::kRowsTotal);
        for (int k = 0; k < rv::kRowsTotal; ++k) {
            model::store::RdRow r;
            if (k < rv::kRowsPage) {
                MO* d = gm::array_get(arr, k);
                if (!d) { if (why) *why = "a reversal row is missing"; return false; }
                r = rd_row(d);
            } else if (t.built) {
                if (MO* d = gm::array_get(g_ext.big[rt], k)) r = rd_row(d);
            } else r = saved[(size_t)rt][k];
            v[(size_t)k] = model::setup::from_game(r, map_get_now(rt, k));
        }
    }
    *out = std::move(rows);
    return true;
}

bool setup_write_now(int fid, const model::setup::Rows& rows, std::string* report) {
    RevTarget t;
    if (!rev_target(fid, &t, report)) return false;
    if (g_pre.part >= 0) pre_abort("a pack import");     // its saved rows file is about to change
    quiet_reset();
    model::store::RevRows extra;
    int valid = 0, virt = 0, missing = 0;
    bool map_changed = false;
    for (int rt = 0; rt < 3; ++rt) {
        MO* arr = gm::ref(t.fd, kRtFields[rt]);
        const std::vector<model::store::RdRow>& src = rows[(size_t)rt];
        for (int k = 0; k < rv::kRowsTotal; ++k) {
            const model::setup::GameRow g = model::setup::to_game(k < (int)src.size() ? src[(size_t)k] : model::store::RdRow{});
            if (k < rv::kRowsPage) {
                MO* d = arr ? gm::array_get(arr, k) : nullptr;
                if (!d) { ++missing; continue; }
                rd_from_table(d, model::store::rd_to_json(g.row));
            } else {
                if (t.built) if (MO* d = gm::array_get(g_ext.big[rt], k)) rd_from_table(d, model::store::rd_to_json(g.row));
                extra[(size_t)rt][k] = g.row;
            }
            if (g.row.IsValid) ++valid;
            if (g.virt) { ++virt; g_map.set(rt, k, g.virt); map_changed = true; }
            else if (g_map.get(rt, k)) { g_map.erase(rt, k); map_changed = true; }
        }
    }
    if (map_changed) map_save();
    if (t.built) ext_save();
    else {
        g_saved_doc.clear();
        files().write_doc(model::store::reversal_rows_path(t.char_name), model::store::rev_rows_to_file(extra));
    }
    quiet_reset();
    if (report) {
        char b[160];
        std::snprintf(b, sizeof(b), "reversal rows written (%d valid, %d on virtual slots, %d game rows missing)", valid, virt, missing);
        *report = b;
    }
    return missing == 0;
}

}  // namespace

bool setup_read(int fid, model::setup::Rows* out, std::string* why) {
    std::lock_guard lk(mech::lock());
    try { return setup_read_now(fid, out, why); } catch (...) { cmn::fault(F::Reversal, "setup_read"); if (why) *why = "fault"; return false; }
}
bool setup_write(int fid, const model::setup::Rows& rows, std::string* report) {
    std::lock_guard lk(mech::lock());
    try { return setup_write_now(fid, rows, report); } catch (...) { cmn::fault(F::Reversal, "setup_write"); if (report) *report = "reversal: fault"; return false; }
}

bool needs_pause() { return g_ext.installed || g_hand_back_due; }

bool menu_open() {
    std::lock_guard lk(mech::lock());
    try { return ext_install(); } catch (...) { cmn::fault(F::Reversal, "menu_open (ui)"); return false; }
}
void menu_close() {
    std::lock_guard lk(mech::lock());
    try { ext_uninstall(); } catch (...) { cmn::fault(F::Reversal, "menu_close (ui)"); }
}
std::optional<int64_t> map_get(int64_t rt, int64_t row) {
    std::lock_guard lk(mech::lock());
    try { return map_get_now(rt, row); } catch (...) { cmn::fault(F::Reversal, "map_get (ui)"); return std::nullopt; }
}

void init() {
    cmn::set_implemented(F::Reversal);
    cmn::on_hand_back(F::Reversal, hand_back);
    cmn::add_status("reversal", status);
    cmn::bridge_fn("rev_menu_open", f_menu_open);
    cmn::bridge_fn("rev_menu_close", f_menu_close);
    cmn::bridge_fn("rev_map_get", f_map_get);
    cmn::add_frame_fn(frame, "layer_reversal");
}

void hook_specs(std::vector<hooks::Spec>& out) {
    const char* fd = "app.training.tf_ReversalSetting.FuncData";
    out.push_back({ F::Reversal, "app.training.tf_ReversalSetting", "UpdateReversalCheck", pre_check, post_check,
                    "reversal rows 11-30 and virtual slots never play", true });
    out.push_back({ F::Reversal, fd, "SetReversalData", pre_set_data, post_setter,
                    "a virtual slot picked for a row would be stored as is", true });
    out.push_back({ F::Reversal, fd, "RemoveReversalData", pre_remove, post_setter, "the map keeps removed rows", true });
    for (const char* m : { "SetReversalActive", "SetReversalDelayFrame", "SetReversalCount", "SetReversalMeatyFrame" })
        out.push_back({ F::Reversal, fd, m, pre_setter, post_setter, "rows 11-30 saved at the menu close only", false });
}

void start() {
    if (cmn::owns(F::Reversal)) cmn::log_info("[CRSMNative] reversal: native (map, rows 11-30, the trigger)");
}

}  // namespace mech::reversal
