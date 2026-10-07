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
#include <regex>
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
constexpr int kScanTimeout = 600;

bool g_own = false, g_installed = false;
int g_faults = 0;
uint64_t g_frame = 0;

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
    static const std::regex kPfb("([^/]+)\\.pfb$");
    for (auto& e : scene::entries()) {
        std::smatch m;
        if (!std::regex_search(e.key, m, kPfb) || m[1].str() != kTintProvider || !e.go) continue;
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
        static const std::regex kPfb("([^/]+)\\.pfb$");
        std::smatch m;
        if (!std::regex_search(e.key, m, kPfb) || m[1].str() != kTintProvider || !e.go) continue;
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

std::pair<size_t, size_t> load_spots() {
    size_t total = 0;
    auto file = drive::load_spots_file(kSpotsPath, total);
    std::vector<uint32_t> cands;
    { std::lock_guard<std::mutex> lk(g_in_mx); cands = cands_locked(); }
    g_spots.clear();
    for (auto& sp : file) {
        if (sp.addr < 0x200000000ULL) continue;
        uint32_t cur = 0;
        const bool ok = scene::rd32(sp.addr, cur);
        if (drive::spot_ok(ok ? std::optional<uint32_t>(cur) : std::nullopt, sp.r, sp.g, sp.b, sp.written, cands)) g_spots.push_back(sp);
    }
    return { g_spots.size(), total };
}

void save_spots() {
    drive::save_spots_file(kSpotsPath, g_spots);
    std::ofstream f(kSigPath, std::ios::binary | std::ios::trunc);
    if (f) f << drive::palette_sig() << "|" << g_last_bases_sig;
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

bool request_scan() {
    auto bases = enumerate_bases();
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
    if (!g_spots.empty()) save_spots();
    char b[128];
    std::snprintf(b, sizeof b, "%zu emplacements reperes (%d noeuds, %d ms)", g_spots.size(), nodes, ms);
    g_status = b;
    g_handoff_set = false;
}

void enter_fight() {
    auto [n, total] = load_spots();
    const std::string saved = read_sig();
    const auto bar = saved.find('|');
    const std::string pal_saved = saved.substr(0, bar), bases_saved = bar == std::string::npos ? std::string() : saved.substr(bar + 1);
    const bool sig_ok = pal_saved == drive::palette_sig();
    const std::string bsig = drive::bases_sig(enumerate_bases());
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
}

void leave_fight() {
    g_static_key.clear();
    g_spots.clear();
    g_live.clear();
    g_scan_pending = 0;
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

std::string static_key(int own) {
    std::lock_guard<std::mutex> lk(g_in_mx);
    char b[96];
    std::snprintf(b, sizeof b, "#%d/%d/%d#%zu#%llu#", own, scene::side_char(0), scene::side_char(1), g_spots.size(), (unsigned long long)g_scan_seq);
    return g_dr_text + b + g_di_text;
}

// the element tints of the shared provider: element 0 = the parry sparks, element 7 = the rush trail
void static_tints(const drive::ColorPair& parry, const drive::ColorPair& rush, std::string& q) {
    static const std::regex kPfb("([^/]+)\\.pfb$");
    for (auto& e : scene::entries()) {
        std::smatch m;
        if (!std::regex_search(e.key, m, kPfb) || m[1].str() != kTintProvider || !e.go) continue;
        scene::each_element(e.go, [&](MO* el, int j) {
            if (j != 0 && j != 7) return;
            const drive::ColorPair& c = j == 0 ? parry : rush;
            queue_write(&q, scene::addr(el) + scene::off_tint(), c.c2 ? *c.c2 : 0xFFFFFFFFu);
        });
    }
}

void static_pass(int own) {
    const int other = own == 1 ? 2 : 1;
    validate_spots();
    const int id1 = scene::side_char(0), id2 = scene::side_char(1);
    const int fid[3] = { 0, id1, id2 };
    if (!mech::manager(1) || !mech::manager(2)) mech::discover();   // the body glow resolves its side per call anyway
    mech::set_di_cont(providers::container(id1), providers::container(id2));
    std::string q;
    // Drive Impact / Reversal: each fighter's lines, the own side's last (a mirror match shares them)
    for (int s : { other, own }) {
        for (int m = 0; m < 2; ++m) {
            providers::MechColors mc;
            { std::lock_guard<std::mutex> lk(g_in_mx); mc = g_di[s][m]; }
            std::vector<providers::WLine> wl;
            if (!providers::lines(fid[s], mc, wl)) continue;
            for (auto& w : wl) queue_write(&q, w.addr, w.val);
        }
    }
    // Drive Parry / Rush: one colour per mechanic, the own side's (the other's when the own side has none)
    drive::ColorPair parry, rush;
    {
        std::lock_guard<std::mutex> lk(g_in_mx);
        parry = g_dr[own][0].c1 ? g_dr[own][0] : g_dr[other][0];
        rush = g_dr[own][1].c1 ? g_dr[own][1] : g_dr[other][1];
    }
    for (size_t idx : g_live) {
        drive::Spot& sp = g_spots[idx];
        const std::string fam = drive::famille(sp.file);
        if (fam == "special") continue;                                 // the special attacks: dormant
        const drive::ColorPair& mc = fam == "parry" ? parry : rush;     // "drive" files and the rest (smoke): Rush
        const uint32_t orig = data::pack(sp.r, sp.g, sp.b);
        const uint32_t col = mc.c1 ? data::tint_with(*mc.c1, sp.r, sp.g, sp.b) : orig;
        drive::remember(sp, col);
        queue_write(&q, sp.addr, col);
    }
    static_tints(parry, rush, q);
    size_t n = 0;
    for (char c : q) if (c == '\n') ++n;
    // Drive Impact / Reversal: their colours do not hold when written once (07/10, mirror Mai: the static writes were
    // lost, the per-effect ones held). The relay keeps their small blocks only (handoff() writes the Parry / Rush
    // spots only for live spots: none given), painted at each Drive Impact / Reversal as before.
    drive::HandoffIn in;
    in.mgr[1] = mech::manager(1);
    in.mgr[2] = mech::manager(2);
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
    const std::vector<size_t> no_spots;
    size_t di_lines = 0;
    engine::relay_native(drive::handoff(in, g_spots, no_spots, di_lines));   // Drive Impact / Reversal blocks only
    g_handoff_set = false;
    g_handoff_lines = di_lines;
    if (!q.empty()) engine::queue(q);
    g_static_writes = n;
    g_static_own = own;
    ++g_statics;
    char b[160];
    std::snprintf(b, sizeof b, "combat leger : couleurs ecrites une fois (%zu ecritures, cote P%d)", n, own);
    g_status = b;
    cmn::journal("vfx tick: light fight, colours written once: %zu writes (%zu live spots, own side P%d); relay: %zu Drive Impact / Reversal lines", n, g_live.size(), own, di_lines);
}

void static_tick() {                                                    // every 60 frames in a fight: a string compare
    if (g_want_scan || g_scan_pending) return;                          // the scan first
    const int own = ::engine::decide::own_side();
    const std::string key = static_key(own);
    if (key == g_static_key) return;
    g_static_key = key;
    static_pass(own);
}

void handle_ui(const std::string& req) {
    g_static_key.clear();                                               // any request: write again (light fight)
    if (req == "scan") { g_want_scan = true; return; }
    if (req == "rescan") {
        if (!g_spots.empty()) paint(std::nullopt);
        apply_tint(std::nullopt);
        g_spots.clear(); g_want_scan = true; g_handoff_set = false;
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
            if (now) enter_fight(); else leave_fight();
        }
    }
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
    if (g_want_scan && g_frame % 30 == 15) {
        g_want_scan = false;
        if (!request_scan()) g_status = "aucune ressource Drive chargee : pas de balayage";
        g_handoff_set = false;
    }
    if (g_scan_pending && g_frame % 5 == 0) poll_scan();
    if (g_in_fight && g_frame % 60 == 0) { if (cmn::light_fight()) static_tick(); else { g_static_key.clear(); relay_pass(); } }
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
    if (!g_own || !g_installed) return;
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
