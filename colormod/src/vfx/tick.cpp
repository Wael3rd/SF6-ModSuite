#include "tick.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>

#include <reframework/API.hpp>

#include "../common.hpp"
#include "../perf.hpp"
#include "../engine/decide.hpp"
#include "body.hpp"
#include "data.hpp"
#include "drive.hpp"
#include "engine.hpp"
#include "mech.hpp"
#include "providers.hpp"
#include "scene.hpp"

namespace vfx::tick {
namespace {

using API = reframework::API;
using scene::MO;

constexpr const char* kSpotsPath = "reframework/data/SF6_DriveRecolor_data/_spots.json";
constexpr const char* kSigPath = "reframework/data/SF6_DriveRecolor_data/_spots_sig.txt";
constexpr const char* kTintProvider = "epvs_cmn_530_00";   // the element tints (parry sparks el 0, rush trail el 7)
// plain check: does the key end with /TINT_PROVIDER.pfb? (replaces std::regex)
bool is_tint_pfb(const std::string& key) {
    static const char kSuf[] = "/epvs_cmn_530_00.pfb";
    constexpr size_t kLen = sizeof(kSuf) - 1;
    return key.size() >= kLen && key.compare(key.size() - kLen, kLen, kSuf) == 0;
}
constexpr int kScanTimeout = 600;
constexpr double kBudgetMs = 1.0;                          // per-frame time budget for fight-start work (ms)

bool g_own = false, g_installed = false;
int g_faults = 0;
uint64_t g_frame = 0;

// QPC budget helper
double g_ticks_per_ms = 0;
void init_budget() {
    if (g_ticks_per_ms > 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ticks_per_ms = (double)f.QuadPart / 1000.0;
}
int64_t budget_now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
bool over_budget(int64_t t0) { return (double)(budget_now() - t0) >= kBudgetMs * g_ticks_per_ms; }

// cached spots: kept between fights so the file is not re-read
std::vector<drive::Spot> g_prev_spots;
std::string g_prev_sig;                                    // the saved sig string (palette|bases)
std::vector<drive::Base> g_cached_bases;                   // last enumerate_bases result
bool g_cached_bases_valid = false;
bool g_static_work_active = false;                         // incremental static_pass in progress (forward, used by leave_fight)
bool g_enter_work_active = false;                          // incremental enter_fight in progress

// ---- what the Lua pushes (settings stay Lua-side) ----
std::mutex g_in_mx;
drive::ColorPair g_dr[3][3];                                // [side][parry, rush, special]
providers::StaticCfg g_static[2];
providers::MechColors g_di[3][2];                           // [side][impact, reversal]
bool g_di_dirty = true;                                     // SF6_DIRecolor dirty (setting changed, side map changed)
std::string g_dr_text, g_di_text;                           // last pushes, for change detection
std::deque<std::string> g_ui;

// ---- SF6_DriveRecolor's battle state ----
bool g_in_fight = false, g_want_scan = false;
std::vector<drive::Spot> g_spots;
std::vector<size_t> g_live;
uint64_t g_scan_seq = 0, g_scan_pending = 0;
uint64_t g_scan_sent = 0;
std::string g_last_bases_sig = "?";
std::optional<uint64_t> g_rescan_at;
std::string g_rescan_chars;
std::string g_handoff_key;
bool g_handoff_set = false;
size_t g_handoff_lines = 0;
std::string g_status = "au repos";
std::string g_static_key;                                   // light fight: what was written once ("" = to write)
uint64_t g_relays = 0, g_scans = 0, g_di_applied = 0;

void queue_write(void* ctx, uint64_t addr, uint32_t value) {             // EfxMeshColor.queue: "addr value current"
    auto* q = static_cast<std::string*>(ctx);
    uint32_t cur = 0;
    if (!scene::rd32(addr, cur)) return;
    char b[64];
    std::snprintf(b, sizeof b, "%llX %08X %08X\n", (unsigned long long)addr, value, cur);
    *q += b;
}

// A provider line for the queue with the colour the slot must still hold: its original (or one of ours, the engine's
// history). queue_write's "what is there now" would let a dead address through: the light fight's writes once (08/10).
void queue_expect(std::string& q, uint64_t addr, uint32_t value, uint32_t expected) {
    char b[64];
    std::snprintf(b, sizeof b, "%llX %08X %08X\n", (unsigned long long)addr, value, expected);
    q += b;
}

std::vector<uint32_t> cands_locked() {                                  // validate_hook: the c1 of every (side, mechanic)
    std::vector<uint32_t> c;
    for (int s = 1; s <= 2; ++s) for (int m = 0; m < 3; ++m) if (g_dr[s][m].c1) c.push_back(*g_dr[s][m].c1);
    return c;
}

std::string last_component(const std::string& path) { auto p = path.find_last_of('/'); return p == std::string::npos ? path : path.substr(p + 1); }

// enumerate_bases("drive"): (file, entry array, count) of every loaded resource of the Drive palette
std::vector<drive::Base> enumerate_bases() {
    std::vector<drive::Base> bases;
    std::map<std::string, bool> seen;
    for (auto& e : scene::entries()) {
        if (!e.go) continue;
        scene::each_element(e.go, [&](MO* el, int) {
            scene::each_resource(el, [&](MO* h, const std::string& path) {
                const std::string sp = last_component(path);
                if (!data::drive_colors_find(sp)) return;
                uint64_t res = 0, arr = 0;
                uint32_t count = 0;
                if (!scene::rd64(scene::addr(h) + 0x10, res) || !res) return;
                if (!scene::rd32(res + 0xD8, count) || !scene::rd64(res + 0xA0, arr)) return;
                char k[48];
                std::snprintf(k, sizeof k, "@%llX", (unsigned long long)arr);
                const std::string key = sp + k;
                if (arr != 0 && count > 0 && count <= 128 && !seen[key]) {
                    seen[key] = true;
                    bases.push_back(drive::Base{ sp, arr, count });
                }
            });
        });
    }
    return bases;
}

std::vector<drive::Tint> tint_addresses() {
    std::vector<drive::Tint> out;
    for (auto& e : scene::entries()) {
        if (!is_tint_pfb(e.key) || !e.go) continue;
        scene::each_element(e.go, [&](MO* el, int j) {
            if (j != 0 && j != 7) return;
            const uint64_t a = scene::addr(el);
            uint32_t cur = 0;
            uint64_t ti = 0;
            if (!scene::rd32(a + scene::off_tint(), cur) || !scene::rd64(a, ti)) return;
            out.push_back(drive::Tint{ a + scene::off_tint(), cur, a, ti });
        });
    }
    return out;
}

void apply_tint(std::optional<uint32_t> col) {                          // the DR panel buttons only
    for (auto& e : scene::entries()) {
        if (!is_tint_pfb(e.key) || !e.go) continue;
        scene::each_element(e.go, [&](MO* el, int j) {
            if (j != 0 && j != 7) return;
            scene::wr8(scene::addr(el) + scene::off_enable(), 1);
            scene::wr32(scene::addr(el) + scene::off_tint(), col.value_or(0xFFFFFFFF));
        });
    }
}

// paint(setting, restore): every spot to its origin (restore) or tinted, through the engine's validated queue
void paint(std::optional<uint32_t> setting) {
    std::string q;
    for (auto& sp : g_spots) {
        const uint32_t col = setting ? data::tint_with(*setting, sp.r, sp.g, sp.b) : data::pack(sp.r, sp.g, sp.b);
        drive::remember(sp, col);
        queue_write(&q, sp.addr, col);
    }
    if (!q.empty()) engine::queue(q);
}

// validate a spot list against live memory: returns the count that still hold
size_t validate_spot_list(const std::vector<drive::Spot>& src, std::vector<drive::Spot>& dst) {
    std::vector<uint32_t> cands;
    { std::lock_guard<std::mutex> lk(g_in_mx); cands = cands_locked(); }
    dst.clear();
    for (auto& sp : src) {
        if (sp.addr < 0x200000000ULL || (sp.addr & 3)) continue;              // unaligned: a false match (dword_writable)
        uint32_t cur = 0;
        const bool ok = scene::rd32(sp.addr, cur);
        if (drive::spot_ok(ok ? std::optional<uint32_t>(cur) : std::nullopt, sp.r, sp.g, sp.b, sp.written, cands)) dst.push_back(sp);
    }
    return dst.size();
}

std::pair<size_t, size_t> load_spots() {
    // try cached spots first (from the previous fight / scan in this session)
    if (!g_prev_spots.empty()) {
        const size_t n = validate_spot_list(g_prev_spots, g_spots);
        return { n, g_prev_spots.size() };
    }
    // fall back to the file
    size_t total = 0;
    auto file = drive::load_spots_file(kSpotsPath, total);
    const size_t n = validate_spot_list(file, g_spots);
    return { n, total };
}

void save_spots() {
    drive::save_spots_file(kSpotsPath, g_spots);
    g_prev_sig = drive::palette_sig() + "|" + g_last_bases_sig;
    cmn::bgwriter::write(kSigPath, g_prev_sig);
}

std::string read_sig() {
    std::ifstream f(kSigPath, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    std::string v = ss.str();
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
}

std::pair<size_t, size_t> validate_spots() {
    std::vector<uint32_t> cands;
    { std::lock_guard<std::mutex> lk(g_in_mx); cands = cands_locked(); }
    g_live.clear();
    for (size_t i = 0; i < g_spots.size(); ++i) {
        uint32_t cur = 0;
        const bool ok = scene::rd32(g_spots[i].addr, cur);
        if (drive::spot_ok(ok ? std::optional<uint32_t>(cur) : std::nullopt, g_spots[i].r, g_spots[i].g, g_spots[i].b, g_spots[i].written, cands))
            g_live.push_back(i);
    }
    return { g_live.size(), g_spots.size() };
}

// enumerate_bases with caching: the result is kept until invalidated (fight exit, rescan)
std::vector<drive::Base>& cached_enumerate_bases() {
    if (!g_cached_bases_valid) {
        perf::Scope eb(perf::EnterBases);
        g_cached_bases = enumerate_bases();
        g_cached_bases_valid = true;
    }
    return g_cached_bases;
}

bool request_scan() {
    auto& bases = cached_enumerate_bases();
    if (bases.empty()) return false;
    ++g_scan_seq;
    engine::scan_request(drive::scan_request(g_scan_seq, bases));
    g_scan_pending = g_scan_seq;
    g_scan_sent = g_frame;
    g_last_bases_sig = drive::bases_sig(bases);
    ++g_scans;
    char b[96];
    std::snprintf(b, sizeof b, "balayage demande (%zu bases)", bases.size());
    g_status = b;
    return true;
}

void poll_scan() {
    std::vector<drive::Spot> found;
    int nodes = 0, ms = 0;
    if (!drive::scan_result(engine::scan_result(), g_scan_pending, found, nodes, ms)) {
        if (g_frame - g_scan_sent > (uint64_t)kScanTimeout) { g_scan_pending = 0; g_status = "pas de reponse du moteur : balayage impossible"; }
        return;
    }
    g_scan_pending = 0;
    // an old spot missing from the fresh scan is kept only if its memory still holds one of our written values
    std::map<uint64_t, bool> seen;
    for (auto& sp : found) seen[sp.addr] = true;
    for (auto& sp : g_spots) {
        if (seen[sp.addr] || sp.written.empty()) continue;
        uint32_t cur = 0;
        if (!scene::rd32(sp.addr, cur)) continue;
        if (std::find(sp.written.begin(), sp.written.end(), cur) != sp.written.end()) { seen[sp.addr] = true; found.push_back(sp); }
    }
    g_spots = std::move(found);
    if (!g_spots.empty()) { save_spots(); g_prev_spots = g_spots; }
    char b[128];
    std::snprintf(b, sizeof b, "%zu emplacements reperes (%d noeuds, %d ms)", g_spots.size(), nodes, ms);
    g_status = b;
    g_handoff_set = false;
}

// ---- incremental enter_fight state machine ------------------------------------------------------------------
// Phase 0: validate spots (chunked)
// Phase 1: enumerate bases (chunked per entry)
// Phase 2: finalize (decide rescan etc.)
struct EnterState {
    int phase = 0;
    // phase 0
    std::vector<drive::Spot> src_spots;
    bool src_loaded = false;
    size_t spot_idx = 0;
    size_t spot_total = 0;
    // phase 1
    std::vector<scene::Entry> entries;
    size_t entry_idx = 0;
    std::vector<drive::Base> bases;
    std::map<std::string, bool> seen;
    // phase 2
    std::string saved_sig;
    bool sig_loaded = false;
};
EnterState g_enter_st;

void enter_fight_start() {
    g_enter_work_active = true;
    g_enter_st = EnterState{};
    g_spots.clear();
    g_cached_bases_valid = false;
    g_handoff_set = false;
    g_status = "entree en combat (incremental)";
}

// Returns true when done, false when more frames are needed
bool enter_fight_step() {
    init_budget();
    const int64_t t0 = budget_now();

    // Phase 0: load and validate spots (chunked at 32 spots)
    if (g_enter_st.phase == 0) {
        perf::Scope ls(perf::EnterLoadSpots);
        if (!g_enter_st.src_loaded) {
            if (!g_prev_spots.empty()) g_enter_st.src_spots = g_prev_spots;
            else { g_enter_st.src_spots = drive::load_spots_file(kSpotsPath, g_enter_st.spot_total); }
            if (g_enter_st.spot_total == 0) g_enter_st.spot_total = g_enter_st.src_spots.size();
            g_enter_st.src_loaded = true;
            if (over_budget(t0)) return false;
        }
        std::vector<uint32_t> cands;
        { std::lock_guard<std::mutex> lk(g_in_mx); cands = cands_locked(); }
        while (g_enter_st.spot_idx < g_enter_st.src_spots.size()) {
            auto& sp = g_enter_st.src_spots[g_enter_st.spot_idx];
            if (sp.addr >= 0x200000000ULL) {
                uint32_t cur = 0;
                const bool ok = scene::rd32(sp.addr, cur);
                if (drive::spot_ok(ok ? std::optional<uint32_t>(cur) : std::nullopt, sp.r, sp.g, sp.b, sp.written, cands))
                    g_spots.push_back(sp);
            }
            ++g_enter_st.spot_idx;
            if ((g_enter_st.spot_idx & 31) == 0 && over_budget(t0)) return false;
        }
        g_enter_st.phase = 1;
        if (over_budget(t0)) return false;
    }

    // Phase 1: enumerate bases (chunked per entry)
    if (g_enter_st.phase == 1) {
        perf::Scope eb(perf::EnterBases);
        if (g_enter_st.entries.empty() && g_enter_st.entry_idx == 0) {
            g_enter_st.entries = scene::entries();
            if (over_budget(t0)) return false;
        }
        while (g_enter_st.entry_idx < g_enter_st.entries.size()) {
            auto& e = g_enter_st.entries[g_enter_st.entry_idx];
            if (e.go) {
                scene::each_element(e.go, [&](scene::MO* el, int) {
                    scene::each_resource(el, [&](scene::MO* h, const std::string& path) {
                        const std::string sp = last_component(path);
                        if (!data::drive_colors_find(sp)) return;
                        uint64_t res = 0, arr = 0;
                        uint32_t count = 0;
                        if (!scene::rd64(scene::addr(h) + 0x10, res) || !res) return;
                        if (!scene::rd32(res + 0xD8, count) || !scene::rd64(res + 0xA0, arr)) return;
                        char k[48];
                        std::snprintf(k, sizeof k, "@%llX", (unsigned long long)arr);
                        const std::string key = sp + k;
                        if (arr != 0 && count > 0 && count <= 128 && !g_enter_st.seen[key]) {
                            g_enter_st.seen[key] = true;
                            g_enter_st.bases.push_back(drive::Base{ sp, arr, count });
                        }
                    });
                });
            }
            ++g_enter_st.entry_idx;
            if (over_budget(t0)) return false;
        }
        g_cached_bases = std::move(g_enter_st.bases);
        g_cached_bases_valid = true;
        g_enter_st.phase = 2;
        if (over_budget(t0)) return false;
    }

    // Phase 2: read sig, decide what to do
    if (g_enter_st.phase == 2) {
        perf::Scope rs(perf::EnterReadSig);
        const std::string saved = g_prev_sig.empty() ? read_sig() : g_prev_sig;
        const auto bar = saved.find('|');
        const std::string pal_saved = saved.substr(0, bar);
        const std::string bases_saved = bar == std::string::npos ? std::string() : saved.substr(bar + 1);
        const bool sig_ok = pal_saved == drive::palette_sig();
        const std::string bsig = drive::bases_sig(g_cached_bases);
        const size_t n = g_spots.size();
        const size_t total = g_enter_st.spot_total;
        char b[160];
        if (sig_ok && n > 0 && bases_saved != bsig) {
            g_spots.clear(); g_want_scan = true;
            std::snprintf(b, sizeof b, "ressources rechargees (%s -> %s) : rebalayage", bases_saved.c_str(), bsig.c_str());
        } else if (!sig_ok && n > 0) {
            paint(std::nullopt);
            g_spots.clear(); g_want_scan = true;
            std::snprintf(b, sizeof b, "palette modifiee (%zu emplacements restaures) : rebalayage", n);
        } else if (n > 0 && (double)n >= (double)total * 0.6) {
            g_want_scan = false;
            std::snprintf(b, sizeof b, "%zu emplacements recuperes", n);
        } else {
            g_spots.clear(); g_want_scan = true;
            std::snprintf(b, sizeof b, "%zu/%zu emplacements encore valides : rebalayage", n, total);
        }
        g_status = b;
        g_handoff_set = false;
        g_enter_work_active = false;
        return true;
    }
    return false;
}

void leave_fight() {
    g_enter_work_active = false;
    g_static_key.clear();
    // keep spots for the next fight (the file is only for the next game start)
    if (!g_spots.empty()) g_prev_spots = g_spots;
    g_spots.clear();
    g_live.clear();
    g_scan_pending = 0;
    g_cached_bases_valid = false;
    g_static_work_active = false;
    mech::on_combat_exit();
    engine::relay_native("");
    g_handoff_lines = 0;
    g_handoff_set = false;
    g_status = "hors combat : relais vide";
}

// the relay key and text (SF6_DriveRecolor every 60 frames in a fight)
void relay_pass() {
    const auto [kept, before] = validate_spots();
    const int id1 = scene::side_char(0), id2 = scene::side_char(1);
    const std::string ck = (id1 && id2) ? std::to_string(id1) + "/" + std::to_string(id2) : std::string();
    if (!ck.empty() && g_rescan_chars.empty()) g_rescan_chars = ck;
    const bool stale_all = before > 0 && kept == 0;
    const bool chars_changed = !ck.empty() && !g_rescan_chars.empty() && ck != g_rescan_chars;
    if ((stale_all || chars_changed) && (!g_rescan_at || g_frame - *g_rescan_at > 600)) {
        g_rescan_at = g_frame; g_rescan_chars = ck;
        if (chars_changed && kept > 0) paint(std::nullopt);
        g_want_scan = true;
    }
    const int fid[3] = { 0, id1, id2 };
    if (!mech::manager(1) || !mech::manager(2)) mech::discover();   // both sides in the relay before their first effect
    const int cont1 = providers::container(id1), cont2 = providers::container(id2);
    mech::set_di_cont(cont1, cont2);
    drive::HandoffIn in;
    in.mgr[1] = mech::manager(1);
    in.mgr[2] = mech::manager(2);
    std::string key;
    {
        std::lock_guard<std::mutex> lk(g_in_mx);
        for (int s = 1; s <= 2; ++s) for (int m = 0; m < 3; ++m) in.col[s][m] = g_dr[s][m];
        char b[160];
        const size_t bucket = g_live.empty() ? 0 : 1 + g_live.size() / 50;
        std::snprintf(b, sizeof b, "0|%llX/%llX/%zu/%d/%d|", (unsigned long long)in.mgr[1], (unsigned long long)in.mgr[2], bucket, cont1, cont2);
        key = b + g_dr_text;
        for (int s = 1; s <= 2; ++s) for (int m = 0; m < 2; ++m) key += "|" + providers::key(fid[s], g_di[s][m]);
    }
    if (g_handoff_set && key == g_handoff_key) return;
    g_handoff_key = key;
    g_handoff_set = true;
    in.tints = tint_addresses();
    for (int s = 1; s <= 2; ++s) {
        for (int m = 0; m < 2; ++m) {
            providers::MechColors mc;
            { std::lock_guard<std::mutex> lk(g_in_mx); mc = g_di[s][m]; }
            std::vector<providers::WLine> wl;
            drive::DiBlock& d = in.di[s][m];
            d.present = providers::lines(fid[s], mc, wl);
            d.container = providers::container(fid[s]);
            for (auto& w : wl) d.lines.push_back(w.obj ? drive::w_line(w.addr, w.val, w.orig, w.obj, w.ti) : drive::w_line(w.addr, w.val, w.orig));
        }
    }
    size_t n = 0;
    engine::relay_native(drive::handoff(in, g_spots, g_live, n));
    g_handoff_lines = n;
    ++g_relays;
}

// ---- light fight (cmn::light_fight, default since 0.19.7) -----------------------------------------------------------
// Wael (07/10): the colours are decided in the menus, nothing is recomputed during the fight. Once per fight (and
// again only when a colour setting changes, or after a new scan) the colours are written into the effect data with
// the engine's validated queue, the relay is emptied (the requestEffect hooks find no block: no painting per effect)
// and nothing else runs: no relay pass, no spot revalidation, no providers pass.
// The game shares the effect data: Parry / Rush between both players, Impact / Reversal too in a mirror match. Each
// of them gets one colour: the player's own side's, or the other side's when the own side leaves it to the game.
uint64_t g_statics = 0;
size_t g_static_writes = 0;
int g_static_own = 0;

// ---- incremental static_pass state ----
// Phases: 0 = validate_spots, 1 = providers + DI queue, 2 = spot queue (chunked), 3 = tints + DI relay + apply
int g_static_work_phase = 0;
int g_static_work_own = 0;
size_t g_validate_idx = 0;                                          // phase 0: how far we are in g_spots
size_t g_spot_queue_idx = 0;                                        // phase 2: how far we are in g_live
int g_static_provider_step = 0;                                     // phase 1: 0=scan, 1..4=lines per (side,mech)
std::string g_static_queue;                                         // phase 1-2: accumulated queue text
struct CachedDiLines { bool present = false; int container = -1; std::vector<providers::WLine> wl; };
CachedDiLines g_di_line_cache[3][2];                                // [side][mech], reused by the relay
drive::ColorPair g_cached_parry, g_cached_rush;                     // computed in phase 2, used in phase 3
std::vector<uint32_t> g_cached_cands;                               // for phase 2 spot validation

std::string static_key(int own) {
    std::lock_guard<std::mutex> lk(g_in_mx);
    char b[96];
    std::snprintf(b, sizeof b, "#%d/%d/%d#%zu#%llu#", own, scene::side_char(0), scene::side_char(1), g_spots.size(), (unsigned long long)g_scan_seq);
    return g_dr_text + b + g_di_text;
}

// the element tints of the shared provider: element 0 = the parry sparks, element 7 = the rush trail
void static_tints(const drive::ColorPair& parry, const drive::ColorPair& rush, std::string& q) {
    for (auto& e : scene::entries()) {
        if (!is_tint_pfb(e.key) || !e.go) continue;
        scene::each_element(e.go, [&](MO* el, int j) {
            if (j != 0 && j != 7) return;
            const drive::ColorPair& c = j == 0 ? parry : rush;
            queue_write(&q, scene::addr(el) + scene::off_tint(), c.c2 ? *c.c2 : 0xFFFFFFFFu);
        });
    }
}

// Start the incremental static_pass: sets up the phases
void static_pass_start(int own) {
    g_static_work_active = true;
    g_static_work_phase = 0;
    g_static_work_own = own;
    g_validate_idx = 0;
    g_spot_queue_idx = 0;
    g_static_provider_step = 0;
    g_static_queue.clear();
    g_cached_cands.clear();
    for (int s = 0; s < 3; ++s) for (int m = 0; m < 2; ++m) g_di_line_cache[s][m] = CachedDiLines{};
}

// One frame of incremental static_pass work; returns true when done
bool static_pass_step() {
    init_budget();
    const int64_t t0 = budget_now();
    const int own = g_static_work_own;
    const int other = own == 1 ? 2 : 1;

    // phase 0: validate_spots (chunked)
    if (g_static_work_phase == 0) {
        perf::Scope sv(perf::StaticValidate);
        std::vector<uint32_t> cands;
        { std::lock_guard<std::mutex> lk(g_in_mx); cands = cands_locked(); }
        while (g_validate_idx < g_spots.size()) {
            uint32_t cur = 0;
            const bool ok = scene::rd32(g_spots[g_validate_idx].addr, cur);
            if (drive::spot_ok(ok ? std::optional<uint32_t>(cur) : std::nullopt, g_spots[g_validate_idx].r, g_spots[g_validate_idx].g, g_spots[g_validate_idx].b,
                               g_spots[g_validate_idx].written, cands))
                g_live.push_back(g_validate_idx);
            ++g_validate_idx;
            if ((g_validate_idx & 31) == 0 && over_budget(t0)) return false;
        }
        g_static_work_phase = 1;
        if (over_budget(t0)) return false;
    }

    // phase 1: providers scan + DI queue (incremental: scan first, then one (side,mech) per budget slice)
    if (g_static_work_phase == 1) {
        const int id1 = scene::side_char(0), id2 = scene::side_char(1);
        const int fid[3] = { 0, id1, id2 };
        perf::Scope sp(perf::StaticProviders);
        // sub-step 0: scan providers
        if (g_static_provider_step == 0) {
            providers::scan();
            if (!mech::manager(1) || !mech::manager(2)) mech::discover();
            mech::set_di_cont(providers::container(id1), providers::container(id2));
            g_static_provider_step = 1;
            if (over_budget(t0)) return false;
        }
        // sub-steps 1..4: providers::lines for (other,0) (other,1) (own,0) (own,1)
        while (g_static_provider_step <= 4) {
            const int idx = g_static_provider_step - 1;
            const int s = (idx < 2) ? other : own;
            const int m = idx % 2;
            providers::MechColors mc;
            { std::lock_guard<std::mutex> lk(g_in_mx); mc = g_di[s][m]; }
            auto& dc = g_di_line_cache[s][m];
            dc.present = providers::lines(fid[s], mc, dc.wl);
            dc.container = providers::container(fid[s]);
            if (dc.present) for (auto& w : dc.wl) queue_expect(g_static_queue, w.addr, w.val, w.orig);
            ++g_static_provider_step;
            if (over_budget(t0)) return false;
        }
        // prepare phase 2 state: colours and cands, computed once
        {
            std::lock_guard<std::mutex> lk(g_in_mx);
            g_cached_parry = g_dr[own][0].c1 ? g_dr[own][0] : g_dr[other][0];
            g_cached_rush = g_dr[own][1].c1 ? g_dr[own][1] : g_dr[other][1];
            g_cached_cands = cands_locked();
        }
        g_spot_queue_idx = 0;
        g_static_work_phase = 2;
        if (over_budget(t0)) return false;
    }

    // phase 2: Parry/Rush spot queue (chunked within the budget)
    if (g_static_work_phase == 2) {
        perf::Scope sq(perf::StaticQueue);
        while (g_spot_queue_idx < g_live.size()) {
            const size_t idx = g_live[g_spot_queue_idx];
            drive::Spot& sp = g_spots[idx];
            const std::string fam = drive::famille(sp.file);
            if (fam != "special") {
                uint32_t now = 0;
                const bool rd = scene::rd32(sp.addr, now);
                if (drive::spot_ok(rd ? std::optional<uint32_t>(now) : std::nullopt, sp.r, sp.g, sp.b, sp.written, g_cached_cands)) {
                    const drive::ColorPair& mc = fam == "parry" ? g_cached_parry : g_cached_rush;
                    const uint32_t orig = data::pack(sp.r, sp.g, sp.b);
                    const uint32_t col = mc.c1 ? data::tint_with(*mc.c1, sp.r, sp.g, sp.b) : orig;
                    drive::remember(sp, col);
                    queue_write(&g_static_queue, sp.addr, col);
                }
            }
            ++g_spot_queue_idx;
            if ((g_spot_queue_idx & 31) == 0 && over_budget(t0)) return false;
        }
        g_static_work_phase = 3;
        if (over_budget(t0)) return false;
    }

    // phase 3: tints + DI relay + apply (small, runs in one frame)
    if (g_static_work_phase == 3) {
        {
            perf::Scope sq(perf::StaticQueue);
            static_tints(g_cached_parry, g_cached_rush, g_static_queue);
        }
        {
            perf::Scope sd(perf::StaticDiRelay);
            drive::HandoffIn in;
            in.mgr[1] = mech::manager(1);
            in.mgr[2] = mech::manager(2);
            for (int s = 1; s <= 2; ++s) {
                for (int m = 0; m < 2; ++m) {
                    drive::DiBlock& d = in.di[s][m];
                    auto& dc = g_di_line_cache[s][m];
                    d.present = dc.present;
                    d.container = dc.container;
                    for (auto& w : dc.wl) d.lines.push_back(w.obj ? drive::w_line(w.addr, w.val, w.orig, w.obj, w.ti) : drive::w_line(w.addr, w.val, w.orig));
                }
            }
            const std::vector<size_t> no_spots;
            size_t di_lines = 0;
            engine::relay_native(drive::handoff(in, g_spots, no_spots, di_lines));
            g_handoff_set = false;
            g_handoff_lines = di_lines;
            size_t n = 0;
            for (char c : g_static_queue) if (c == '\n') ++n;
            if (!g_static_queue.empty()) engine::queue(g_static_queue);
            g_static_writes = n;
            g_static_own = own;
            ++g_statics;
            char b[160];
            std::snprintf(b, sizeof b, "combat leger : couleurs ecrites une fois (%zu ecritures, cote P%d)", n, own);
            g_status = b;
            cmn::journal("vfx tick: light fight, colours written once: %zu writes (%zu live spots, own side P%d); relay: %zu Drive Impact / Reversal lines", n, g_live.size(), own, di_lines);
        }
        g_static_queue.clear();
        g_cached_cands.clear();
        for (int s = 0; s < 3; ++s) for (int m = 0; m < 2; ++m) g_di_line_cache[s][m] = CachedDiLines{};
        g_static_work_active = false;
        return true;
    }
    return false;
}

// legacy entry point: runs the whole static_pass at once (for live_fight.txt mode)
void static_pass(int own) {
    static_pass_start(own);
    while (!static_pass_step()) {}
}

void static_tick() {                                                    // every 60 frames in a fight: a string compare
    if (g_enter_work_active) return;                                    // enter_fight still in progress
    if (g_want_scan || g_scan_pending) return;                          // the scan first
    if (g_static_work_active) return;                                   // the previous static_pass is still in progress
    // Once written, never again while the fight is torn down (a fighter gone, the battle description cleared: the key
    // changes but the effect objects are being freed).
    if (!g_static_key.empty() && (!scene::battle_char(0) || !scene::battle_char(1))) return;
    const int own = ::engine::decide::own_side();
    const std::string key = static_key(own);
    if (key == g_static_key) return;
    g_static_key = key;
    g_live.clear();
    static_pass_start(own);                                             // the work is spread over the next frames
}

void handle_ui(const std::string& req) {
    g_static_key.clear();                                               // any request: write again (light fight)
    if (req == "scan") { g_want_scan = true; return; }
    if (req == "rescan") {
        if (!g_spots.empty()) paint(std::nullopt);
        apply_tint(std::nullopt);
        g_spots.clear(); g_want_scan = true; g_handoff_set = false; g_cached_bases_valid = false;
        g_status = "couleurs restaurees, rebalayage en cours";
        return;
    }
    if (req == "restore") {
        paint(std::nullopt);
        apply_tint(std::nullopt);
        g_handoff_set = false;
        g_status = "couleurs d origine restaurees";
        return;
    }
    int side = 0; char mech[16] = {};
    if (std::sscanf(req.c_str(), "paint:%d:%15s", &side, mech) == 2 && (side == 1 || side == 2)) {
        const int m = std::string(mech) == "parry" ? 0 : std::string(mech) == "rush" ? 1 : std::string(mech) == "special" ? 2 : -1;
        if (m < 0) return;
        drive::ColorPair c;
        { std::lock_guard<std::mutex> lk(g_in_mx); c = g_dr[side][m]; }
        if (c.c1) { paint(*c.c1); apply_tint(c.c2 ? *c.c2 : *c.c1); }
        g_handoff_set = false;
    }
}

void frame_inner() {
    ++g_frame;
    if (g_frame % 30 == 0) {
        const bool now = scene::battle_char(0) != 0 || scene::battle_char(1) != 0;
        if (now != g_in_fight) {
            g_in_fight = now;
            if (now) enter_fight_start(); else leave_fight();
        }
    }
    // incremental enter_fight: advance each frame while in progress
    if (g_enter_work_active) enter_fight_step();
    // SF6_DIRecolor: the providers every 30 frames (at once when a setting changed)
    bool dirty;
    { std::lock_guard<std::mutex> lk(g_in_mx); dirty = g_di_dirty; }
    const bool frozen = cmn::light_fight() && g_in_fight && !g_static_key.empty();   // light fight: written, nothing runs
    if (dirty || (g_frame % 30 == 0 && !frozen)) {
        providers::StaticCfg st[2];
        { std::lock_guard<std::mutex> lk(g_in_mx); st[0] = g_static[0]; st[1] = g_static[1]; g_di_dirty = false; }
        const int chars[2] = { scene::side_char(0), scene::side_char(1) };
        std::string q;
        g_di_applied += providers::tick(st, chars, dirty, g_in_fight, providers::QueueFn{ queue_write, &q });
        if (!q.empty()) engine::queue(q);
    }
    if (g_want_scan && !g_enter_work_active && g_frame % 30 == 15) {
        g_want_scan = false;
        if (!request_scan()) g_status = "aucune ressource Drive chargee : pas de balayage";
        g_handoff_set = false;
    }
    if (g_scan_pending && g_frame % 5 == 0) poll_scan();
    // incremental static_pass: advance each frame (not only every 60) while work is pending
    if (g_static_work_active) static_pass_step();
    if (g_in_fight && !g_enter_work_active && g_frame % 60 == 0) { if (cmn::light_fight()) static_tick(); else { g_static_key.clear(); relay_pass(); } }
    std::deque<std::string> reqs;
    { std::lock_guard<std::mutex> lk(g_in_mx); reqs.swap(g_ui); }
    for (auto& r : reqs) handle_ui(r);
}

void frame_guarded() {
    try { frame_inner(); }
    catch (...) { ++g_faults; cmn::journal("vfx tick: exception (%d)", g_faults); }
}

void on_fault() {
    ++g_faults;
    cmn::journal("vfx tick: fault caught (%d)", g_faults);
    if (g_faults >= 3) {
        g_installed = false;
        engine::relay_native("");
        cmn::journal("vfx tick: off after 3 faults, the relay is empty (Drive colours stay the game's until a restart)");
    }
}

#pragma warning(push)
#pragma warning(disable: 4733)
void on_update_seh() {
    if (cmn::closing() || !g_own || !g_installed) return;
    __try { frame_guarded(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault(); }
}
#pragma warning(pop)
void on_update() {                                     // timed from outside the __try function (perf.hpp)
    const int64_t t0 = perf::begin();
    on_update_seh();
    perf::end(perf::VfxTickUpdate, t0);
}

}  // namespace

bool install(bool own) {
    g_own = own;
    g_scan_seq = (uint64_t)(std::time(nullptr) % 100000) * 100;
    if (!scene::init()) { g_own = false; return false; }
    // the spots and their sig are read once here: read at the first fight entry they stalled a frame (15 + 12 ms, 09/10)
    { size_t total = 0; g_prev_spots = drive::load_spots_file(kSpotsPath, total); g_prev_sig = read_sig(); }
    auto* fns = API::get()->param()->functions;
    if (!fns->on_pre_application_entry) { g_own = false; return false; }
    fns->on_pre_application_entry("UpdateBehavior", on_update);
    g_installed = true;
    cmn::journal(own ? "vfx tick: owner (scans, spots, relay, providers in the DLL; the Lua ticks stand by)"
                     : "vfx tick: shadow (each relay the Lua writes is rebuilt and compared, nothing written)");
    return true;
}

bool ready() { return g_own && g_installed; }

void set_dr_colors(const int64_t c[12]) {
    std::lock_guard<std::mutex> lk(g_in_mx);
    std::string text;
    for (int s = 1; s <= 2; ++s) for (int m = 0; m < 3; ++m) {
        const int64_t c1 = c[(s - 1) * 6 + m * 2], c2 = c[(s - 1) * 6 + m * 2 + 1];
        g_dr[s][m].c1 = c1 >= 0 ? std::optional<uint32_t>((uint32_t)c1) : std::nullopt;
        g_dr[s][m].c2 = c2 >= 0 ? std::optional<uint32_t>((uint32_t)c2) : std::nullopt;
        char b[32];
        std::snprintf(b, sizeof b, "%llX/%llX,", (unsigned long long)c1, (unsigned long long)c2);
        text += b;
    }
    g_dr_text = text;
    const int64_t body[4] = { c[0], c[2], c[6], c[8] };                  // the body glow: parry / rush colour 1 per side
    body::set_dr(body);
}

void set_di_state(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_in_mx);
    if (text == g_di_text) return;
    providers::StaticCfg st[2];
    providers::MechColors di[3][2];
    std::istringstream f(text);
    std::string l;
    while (std::getline(f, l)) {
        std::istringstream ls(l);
        std::string tag;
        ls >> tag;
        if (tag == "S") {
            int side = 0, fid = 0, en = 0, fol = 0;
            ls >> side >> fid >> en >> fol;
            if (side == 1 || side == 2) st[side - 1] = providers::StaticCfg{ fid, en != 0, fol != 0 };
        } else if (tag == "C") {
            int side = 0, fol = 0;
            std::string mech, c1, c2;
            ls >> side >> mech >> c1 >> c2 >> fol;
            const int m = mech == "impact" ? 0 : mech == "reversal" ? 1 : -1;
            if ((side != 1 && side != 2) || m < 0) continue;
            auto& mc = di[side][m];
            mc.has = c1 != "-"; if (mc.has) mc.c1 = (uint32_t)std::strtoul(c1.c_str(), nullptr, 16);
            mc.has_c2 = c2 != "-"; if (mc.has_c2) mc.c2 = (uint32_t)std::strtoul(c2.c_str(), nullptr, 16);
            mc.follow = fol != 0;
            std::string kv;
            while (ls >> kv) {
                const auto eq = kv.rfind('=');
                if (eq != std::string::npos) mc.parts[kv.substr(0, eq)] = (uint32_t)std::strtoul(kv.c_str() + eq + 1, nullptr, 16);
            }
        }
    }
    for (int s = 0; s < 2; ++s)
        if (st[s].fighter != g_static[s].fighter || st[s].enabled != g_static[s].enabled || st[s].follow != g_static[s].follow) g_di_dirty = true;
    g_static[0] = st[0]; g_static[1] = st[1];
    for (int s = 1; s <= 2; ++s) for (int m = 0; m < 2; ++m) g_di[s][m] = di[s][m];
    g_di_text = text;
    int64_t body[8];                                                      // the body glow: impact c1 c2, reversal c1 c2 per side
    for (int s = 1; s <= 2; ++s) for (int m = 0; m < 2; ++m) {
        body[(s - 1) * 4 + m * 2] = di[s][m].has ? (int64_t)di[s][m].c1 : -1;
        body[(s - 1) * 4 + m * 2 + 1] = (di[s][m].has && di[s][m].has_c2) ? (int64_t)di[s][m].c2 : -1;
    }
    body::set_di(body);
}

void ui(const std::string& req) {
    std::lock_guard<std::mutex> lk(g_in_mx);
    if (req == "di_dirty") { g_di_dirty = true; return; }             // SF6_DIRecolor.mark_dirty (a menu saved a setting)
    g_ui.push_back(req);
}

std::string parts(int fighter) { return providers::parts(fighter); }

std::string status() {
    char b[512];
    std::snprintf(b, sizeof b,
                  "vfx tick own=%d installed=%d faults=%d fight=%d spots=%zu live=%zu scans=%llu pending=%llu relays=%llu lines=%zu di_applied=%llu "
                  "light=%d statics=%llu static_writes=%zu static_own=%d providers: %s | %s",
                  g_own ? 1 : 0, g_installed ? 1 : 0, g_faults, g_in_fight ? 1 : 0, g_spots.size(), g_live.size(), (unsigned long long)g_scans,
                  (unsigned long long)g_scan_pending, (unsigned long long)g_relays, g_handoff_lines, (unsigned long long)g_di_applied,
                  cmn::light_fight() ? 1 : 0, (unsigned long long)g_statics, g_static_writes, g_static_own,
                  providers::status().c_str(), g_status.c_str());
    return b;
}

}  // namespace vfx::tick
