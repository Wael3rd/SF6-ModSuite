#include "bigc.hpp"

#include <cstdio>
#include <optional>

#include "common.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/replay.hpp"
#include "model/vslots.hpp"
#include "perf.hpp"

namespace mech::bigc {
namespace {

namespace gm = game::gm;
using gm::MO;

constexpr const char* kSlotDataT = "app.training.TrainingData.TM_RecordSetting.SlotData";
constexpr const char* kRecordDataT = "app.training.TrainingData.TM_RecordSetting.RecordData";
constexpr int kFirst = model::vslots::kFirst, kLast = model::vslots::kLast, kTotal = 30;

struct Cache {
    uintptr_t fd_addr = 0;
    MO* arr = nullptr;                        // SlotData[30]
    MO* orig = nullptr;                       // the game's 8-array this cache was built around
    MO* vs[kLast + 1]{};                      // our SlotData of virtual slot n
    std::string sig[kLast + 1];               // fill_virtual's content signature
};
Cache g_c;
bool g_have = false;
uint64_t g_seen_version = 0;                  // the files layer's virtual-slot version the entries follow
bool g_dirty = false;

struct Placed { MO* fd = nullptr; MO* orig = nullptr; MO* arr = nullptr; };
Placed g_big;                                 // install_big (menu)
bool g_big_on = false;
Placed g_push;                                // big_push (reversal trigger)
bool g_pushed = false;
uint64_t g_builds = 0, g_pushes = 0, g_pops = 0, g_late = 0, g_adopted = 0;

model::vslots::VSlots& vslots() { return mech::files::engine().vslots(); }

MO* slots_of(MO* fd) { return gm::ref(fd, "RecordSlots"); }

// make_virtual_slot (RMT:929-944): a SlotData for virtual slot n, empty when none
MO* make_virtual_slot(int n) {
    MO* sd = gm::new_object(gm::type(kSlotDataT));
    if (!sd) return nullptr;
    gm::call(sd, "Init", {});
    model::vslots::VSlots& v = vslots();
    const auto* e = v.get(n);
    const int64_t frames = v.frames(n);
    gm::set_bool(sd, "IsActive", e && e->is_object() && e->contains("active") && (*e)["active"] == model::vslots::json(true) && frames > 0);
    gm::set_bool(sd, "IsValid", frames > 0);
    gm::set_int(sd, "Weight", (int64_t)v.weight(n));
    gm::set_int(sd, "Frame", frames);
    if (!gm::ref(sd, "InputData")) {
        MO* rd = gm::new_object(gm::type(kRecordDataT));
        if (rd) {
            if (MO* buf = gm::new_array(gm::type("System.UInt16"), (uint32_t)(frames > 1 ? frames : 1))) gm::set_ref(rd, "buff", buf);
            gm::set_ref(sd, "InputData", rd);
        }
    }
    if (MO* rd = gm::ref(sd, "InputData")) {
        int64_t num = 0;
        if (!gm::get_int(rd, "Num", &num) || num != frames) gm::set_int(rd, "Num", frames);
    }
    return sd;
}

// fill_virtual (RMT:949-958): the entry's buffer follows the file, only when its content changed
void fill_virtual_in(Cache& c, int n) {
    MO* sd = c.vs[n];
    if (!sd) return;
    model::vslots::VSlots& v = vslots();
    const auto* e = v.get(n);
    const std::string sig = model::replay::fill_signature(e);
    if (c.sig[n] == sig) return;
    c.sig[n] = sig;
    MO* rd = gm::ref(sd, "InputData");
    if (v.frames(n) > 0 && rd && e) mech::files::fill_record(rd, *e);
}
void fill_virtual(int n) { if (g_have) fill_virtual_in(g_c, n); }

void sync_entry(int n) {
    MO* sd = g_have ? g_c.vs[n] : nullptr;
    if (!sd) return;
    model::vslots::VSlots& v = vslots();
    const auto* e = v.get(n);
    const int64_t frames = v.frames(n);
    gm::set_bool(sd, "IsValid", frames > 0);
    gm::set_int(sd, "Frame", frames);
    gm::set_int(sd, "Weight", (int64_t)v.weight(n));
    gm::set_bool(sd, "IsActive", e && e->is_object() && e->contains("active") && (*e)["active"] == model::vslots::json(true) && frames > 0);
    if (MO* rd = gm::ref(sd, "InputData")) {
        int64_t num = 0;
        if (!gm::get_int(rd, "Num", &num) || num != frames) gm::set_int(rd, "Num", frames);
    }
    fill_virtual(n);
}

// ensure_big (RMT:960-976): the cached array for the current fighter (orig = the game's 8)
bool ensure_big(MO* fd, MO* orig) {
    if (g_have && g_c.fd_addr == (uintptr_t)fd) {
        check_version();
        if (g_dirty) { g_dirty = false; for (int n = kFirst; n <= kLast; ++n) sync_entry(n); }
        return true;
    }
    MO* arr = gm::new_array(gm::type(kSlotDataT), kTotal);
    if (!arr) return false;
    Cache c;
    for (int k0 = 0; k0 < 8; ++k0) gm::array_set(arr, k0, gm::array_get(orig, k0));
    for (int n = kFirst; n <= kLast; ++n) {                       // a failure keeps the previous cache, as the Lua
        MO* sd = make_virtual_slot(n);
        if (!sd) return false;
        gm::array_set(arr, n - 1, sd);
        c.vs[n] = sd;
    }
    orig->add_ref();                              // held, as the Lua's orig:add_ref()
    c.fd_addr = (uintptr_t)fd;
    c.arr = arr;
    c.orig = orig;
    g_c = c;
    g_have = true;
    g_seen_version = mech::files::engine().vslots_version();
    g_dirty = false;
    for (int n = kFirst; n <= kLast; ++n) fill_virtual(n);
    ++g_builds;
    cmn::journal("replay: 30-slot array built (virtual buffers filled) for %s",
                 mech::files::engine().vslots_char() ? mech::files::engine().vslots_char()->c_str() : "?");
    return true;
}

// game_slots (RMT:977-989): the game's own 8-entry array, whatever is installed now; a 30-array left by an earlier
// session (its 8-array reference lost) is adopted: an 8-array is rebuilt around its entries 0-7
MO* game_slots(MO* fd) {
    MO* cur = slots_of(fd);
    if (!cur) return nullptr;
    if (gm::array_len(cur) == 8) return cur;
    if (g_have && cur == g_c.arr) return g_c.orig;
    MO* arr = gm::new_array(gm::type(kSlotDataT), 8);
    if (!arr) return nullptr;
    for (int k0 = 0; k0 < 8; ++k0) gm::array_set(arr, k0, gm::array_get(cur, k0));
    gm::set_ref(fd, "RecordSlots", arr);
    ++g_adopted;
    cmn::journal("replay: leftover %d-entry array adopted: 8-array rebuilt", gm::array_len(cur));
    return arr;
}

}  // namespace

MO* fighter_data() {
    const int fid = mech::files::fighter();
    if (fid < 0) return nullptr;
    MO* rs = gm::ref(gm::ref(gm::singleton("app.training.TrainingManager"), "_tData"), "RecordSetting");
    if (!rs) return nullptr;
    int32_t fi = -1;
    if (!gm::call_i32(rs, "GetFighterIndex", &fi, { gm::arg_i32(fid) }) || fi < 0) return nullptr;
    return gm::list_item(gm::ref(rs, "FighterDataList"), fi);
}

bool live(int k0, bool* active, bool* valid, int64_t* weight) {
    MO* sd = gm::array_get(slots_of(fighter_data()), k0);
    if (!sd) return false;
    bool a = false, v = false;
    int64_t w = 1;
    gm::get_bool(sd, "IsActive", &a);
    gm::get_bool(sd, "IsValid", &v);
    gm::get_int(sd, "Weight", &w);
    if (active) *active = a;
    if (valid) *valid = v;
    if (weight) *weight = w;
    return true;
}

bool set_active(int k0, bool on) {
    MO* sd = gm::array_get(slots_of(fighter_data()), k0);
    return sd && gm::set_bool(sd, "IsActive", on);
}

bool install() {
    if (g_big_on) return true;
    MO* fd = fighter_data();
    if (!fd) return false;
    MO* orig = game_slots(fd);
    if (!orig) { cmn::journal("replay: install: no 8-array"); return false; }
    if (!ensure_big(fd, orig)) return false;
    for (int k0 = 0; k0 < 8; ++k0) gm::array_set(g_c.arr, k0, gm::array_get(orig, k0));   // the game's live objects (it may replace one)
    orig->add_ref();                              // the field lets go of the game's 8-array: we hold it until it is back
    fd->add_ref();                                // (the Lua's `big` table held both)
    if (!gm::set_ref(fd, "RecordSlots", g_c.arr)) { orig->release(); fd->release(); return false; }
    g_big = { fd, orig, g_c.arr };
    g_big_on = true;
    cmn::journal("replay: 30 slots installed for the menu");
    return true;
}

bool installed() { return g_big_on; }
MO* installed_array() { return g_big_on ? g_big.arr : nullptr; }

void uninstall() {
    if (!g_big_on) return;
    Placed b = g_big;
    g_big_on = false;
    g_big = {};
    try { for (int k0 = 0; k0 < 8; ++k0) gm::array_set(b.orig, k0, gm::array_get(b.arr, k0)); } catch (...) {}   // the objects the game put into ours kept
    try { gm::set_ref(b.fd, "RecordSlots", b.orig); } catch (...) {}
    if (b.orig) b.orig->release();                // the field holds it again
    if (b.fd) b.fd->release();
    cmn::journal("replay: the game's 8 slots put back");
}

bool push() {
    if (g_big_on || g_pushed) return true;
    MO* fd = fighter_data();
    if (!fd) return false;
    MO* orig = game_slots(fd);
    if (!orig) { cmn::journal("reversal: push: no 8-array"); return false; }
    if (!ensure_big(fd, orig)) return false;
    for (int k0 = 0; k0 < 8; ++k0) gm::array_set(g_c.arr, k0, gm::array_get(orig, k0));
    orig->add_ref();
    fd->add_ref();
    if (!gm::set_ref(fd, "RecordSlots", g_c.arr)) { orig->release(); fd->release(); return false; }
    g_push = { fd, orig, g_c.arr };
    g_pushed = true;
    ++g_pushes;
    return true;
}

void pop() {
    if (!g_pushed) return;
    Placed p = g_push;
    g_pushed = false;
    g_push = {};
    try {
        if (g_have) for (int k0 = 0; k0 < 8; ++k0) gm::array_set(p.orig, k0, gm::array_get(g_c.arr, k0));
        gm::set_ref(p.fd, "RecordSlots", p.orig);
        ++g_pops;
    } catch (...) {}
    if (p.orig) p.orig->release();
    if (p.fd) p.fd->release();
}

bool pushed() { return g_pushed; }

void late_pop() {
    if (!g_pushed) return;
    pop();
    ++g_late;
    cmn::journal("reversal: late pop (a push whose post-hook never ran)");
}

bool entry(int n, bool* active, int64_t* weight) {
    if (n < kFirst || n > kLast || !g_have || !g_c.vs[n]) return false;
    bool a = false;
    int64_t w = 1;
    gm::get_bool(g_c.vs[n], "IsActive", &a);
    gm::get_int(g_c.vs[n], "Weight", &w);
    if (active) *active = a;
    if (weight) *weight = w;
    return true;
}

void entry_set_active(int n, bool on) {
    if (n >= kFirst && n <= kLast && g_have && g_c.vs[n]) gm::set_bool(g_c.vs[n], "IsActive", on);
}

void sync(int n) { if (n >= kFirst && n <= kLast) sync_entry(n); }

void check_version() {
    const uint64_t v = mech::files::engine().vslots_version();
    if (v != g_seen_version) { g_seen_version = v; g_dirty = true; }
}

// ---- the array built ahead (bigc.hpp) ----------------------------------------------------------------------------------
// ensure_big built the 22 entries (and their buffers) at the first menu opening of a fighter: with the reversal rows, the
// 18.7 ms CreateViewData pre of the session's first opening (perf log 13/10). Built here ahead, kPreSlots entries a frame,
// then put in place at once as ensure_big puts its build; the version of the virtual slots seen at the start is kept, so a
// change during the build is synced at the first use (check_version). Given up (the first use builds, as before) when the
// fighter or the game's 8-array changes under it, or when the array in place is not the game's 8 (a leftover: adopted by
// game_slots at the first use, a write into the game's data).
namespace {
constexpr int kPreDelay = 45, kPreSlots = 4;
struct Pre { bool on = false; Cache c; uint64_t version = 0; int next = 0; };
Pre g_pre;
uint64_t g_pre_events = UINT64_MAX;
int g_pre_fid = INT32_MIN;
std::optional<std::string> g_pre_char;
int g_pre_wait = -1;
uint64_t g_prebuilds = 0, g_pre_aborts = 0;

void pre_abort(const char* why) {
    if (g_pre.on && g_pre.next > 0) cmn::journal("replay: 30-slot array built ahead: given up (%s): the first use builds it", why);
    ++g_pre_aborts;
    g_pre = Pre{};
}

void pre_step() {
    MO* fd = fighter_data();
    if (!g_pre.c.arr) {                              // the start
        if (!fd) { pre_abort("no fighter yet"); return; }
        if (g_have && g_c.fd_addr == (uintptr_t)fd) { g_pre = Pre{}; return; }   // built already
        MO* cur = slots_of(fd);
        if (!cur || gm::array_len(cur) != 8) { pre_abort("not the game's 8-array"); return; }
        MO* arr = gm::new_array(gm::type(kSlotDataT), kTotal);
        if (!arr) { pre_abort("array"); return; }
        for (int k0 = 0; k0 < 8; ++k0) gm::array_set(arr, k0, gm::array_get(cur, k0));
        g_pre.c.fd_addr = (uintptr_t)fd;
        g_pre.c.arr = arr;
        g_pre.c.orig = cur;
        g_pre.version = mech::files::engine().vslots_version();
        g_pre.next = kFirst;
        return;
    }
    if (g_have && g_c.fd_addr == g_pre.c.fd_addr) { g_pre = Pre{}; return; }   // a first use built it meanwhile: never replaced
    if ((uintptr_t)fd != g_pre.c.fd_addr || slots_of(fd) != g_pre.c.orig) { pre_abort("fighter or slots changed"); return; }
    for (int i = 0; i < kPreSlots && g_pre.next <= kLast; ++i, ++g_pre.next) {
        const int n = g_pre.next;
        MO* sd = make_virtual_slot(n);
        if (!sd) { pre_abort("entry"); return; }
        gm::array_set(g_pre.c.arr, n - 1, sd);
        g_pre.c.vs[n] = sd;
        fill_virtual_in(g_pre.c, n);
    }
    if (g_pre.next <= kLast) return;
    // done: in place as ensure_big puts its build (the 8-array held; the entries follow the version seen at the start)
    g_pre.c.orig->add_ref();
    g_c = g_pre.c;
    g_have = true;
    g_seen_version = g_pre.version;
    g_dirty = false;
    ++g_builds;
    ++g_prebuilds;
    cmn::journal("replay: 30-slot array built ahead (virtual buffers filled) for %s (menu closed)",
                 mech::files::engine().vslots_char() ? mech::files::engine().vslots_char()->c_str() : "?");
    g_pre = Pre{};
}
}  // namespace

void prebuild_tick(bool training) {
    const uint64_t ev = mech::events();
    const int fid = mech::files::fighter();
    const auto c = mech::files::engine().char_name();
    if (ev != g_pre_events || fid != g_pre_fid || c != g_pre_char) {   // something to look at again, a little later
        g_pre_events = ev;
        g_pre_fid = fid;
        g_pre_char = c;
        if (g_pre.on) pre_abort("a new event");
        g_pre_wait = kPreDelay;
    }
    if (!training || g_big_on || g_pushed) { if (g_pre.on) pre_abort("menu / training"); return; }
    if (!g_pre.on) {
        if (g_pre_wait < 0) return;
        if (g_pre_wait-- > 0) return;
        if (fid < 0) return;                         // (no dummy yet: the next event starts again)
        g_pre.on = true;
    }
    PERF_SUB(pp, "Frame:replay_build_ahead");
    pre_step();
}

std::string status() {
    char b[260];
    std::snprintf(b, sizeof(b), "array %s, menu %s, pushed %s, builds %llu (ahead %llu, gave up %llu), pushes %llu, pops %llu, late pops %llu, adopted %llu",
                  g_have ? "built" : "none", g_big_on ? "installed" : "-", g_pushed ? "yes" : "no", (unsigned long long)g_builds,
                  (unsigned long long)g_prebuilds, (unsigned long long)g_pre_aborts,
                  (unsigned long long)g_pushes, (unsigned long long)g_pops, (unsigned long long)g_late, (unsigned long long)g_adopted);
    return b;
}

}  // namespace mech::bigc
