// vfx::engine - SF6_MemWriterNative absorbed into SF6_ColorModNative (owner "vfx_engine", phase 2 step 2,
// docs/CPP_PORT_PHASE2.md). The write engine is MemWriter's, unchanged: validated writes (origin, sampled dword, our
// own values, same saturation / luminosity, managed object type), module and page guards, SEH, the 5-AV switch-off,
// the requestEffect / addMaterialEffect hooks that write each side's colours before the effect is born, the dword
// queue and the pointer-walk scan. Only the transport changes: the Lua (SF6_DriveRecolor, EfxMeshColor) hands the
// same texts it wrote to _drivecolors.txt, _memwrite.txt and _drivescan.txt through the core's Lua bridge
// (vfx_relay, vfx_queue, vfx_scan) and reads the scan result back with vfx_scan_result, instead of files.
// Formats (unchanged, see the MemWriter source kept in plugins/native/memwriter for the history):
//   relay  "M <mgr>", "E <mgr> <cont> <cat> <el>", "W <addr> <val> <orig> [<obj> <ti>]", "V", "V3", "VC" lines;
//   queue  "hexaddr hexvalue hexorig" lines;
//   scan   "R <seq>", "P ...", "X ...", "F ...", "D ...", "END" -> "R", "N", "S", "T", "H", "END".
// Never loaded together with SF6_MemWriterNative.dll: both would hook the same methods (install() refuses then).

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common.hpp"
#include "../perf.hpp"
#include "../perf_hook.hpp"
#include "../write_journal.hpp"
#include "memguard.hpp"
#include "engine.hpp"
#include "mech.hpp"
#include "tick.hpp"

#pragma comment(lib, "psapi.lib")

namespace vfx::engine {
namespace {
using namespace ll;

constexpr const char* TAG = "[SF6MemWriter] ";
bool g_active = false;
bool g_installed = false;

// what the Lua hands over through the bridge (was: the files), taken by the game-thread passes below
std::mutex g_io_mx;
std::string g_relay_text; bool g_relay_new = false;          // _drivecolors.txt
std::vector<std::string> g_queue_lines;                       // _memwrite.txt (incoming)
std::deque<std::string> g_pending_queue;                      // lines taken for budgeted processing
std::string g_scan_req_text;                                  // _drivescan.txt
std::string g_scan_res_text;                                  // _drivespots.txt

// ---- journal ----------------------------------------------------------------------------------------------------
template <typename... A> void info(const char* f, A... a) { char b[512]; std::snprintf(b, sizeof b, f, a...); reapi().log_info("%s%s", TAG, b); cmn::journal("%s%s", TAG, b); }
template <typename... A> void err(const char* f, A... a) { char b[512]; std::snprintf(b, sizeof b, f, a...); reapi().log_error("%s%s", TAG, b); cmn::journal("%s%s", TAG, b); }

// ---- modules charges : jamais d ecriture dedans ----------------------------------------------------------------
std::vector<std::pair<uint64_t, uint64_t>> g_modules;
void map_modules() {
    g_modules.clear();
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi))
            g_modules.emplace_back((uint64_t)mi.lpBaseOfDll, (uint64_t)mi.lpBaseOfDll + mi.SizeOfImage);
    }
}
bool in_module(uint64_t a) { for (auto& m : g_modules) if (a >= m.first && a < m.second) return true; return false; }

// ---- QPC budget helper (cached frequency) ------------------------------------------------------------------------
double g_ticks_per_ms = 0;
void init_budget() {
    if (g_ticks_per_ms > 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_ticks_per_ms = (double)f.QuadPart / 1000.0;
}
int64_t budget_now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
bool over_ms(int64_t t0, double ms) { return (double)(budget_now() - t0) >= ms * g_ticks_per_ms; }

// ---- garde de pages: vfx::memguard (one VirtualQuery per region and frame, 07/10) --------------------------------
uint64_t g_page_refused = 0;
int page_state(uint64_t a) { return memguard::state(a); }   // bit 0 lisible, bit 1 inscriptible
bool range_readable(uint64_t a, size_t len) { for (uint64_t p = a & ~0xFFFULL; p < a + len; p += 0x1000) if (!(page_state(p) & 1)) return false; return true; }
// game data is 4-byte aligned: addr and addr+3 are in the same 4KB page, one query suffices
// A colour dword sits at a 4-byte boundary: an unaligned address is a false match of the scan (09/10: "200000002D9", in
// the game allocator's own page data, written each fight -> "corrupted thread-free list"; colour bytes landed in pointers).
bool dword_writable(uint64_t a) { return (a & 3) == 0 && (page_state(a) & 2) != 0; }
uint32_t rd32(uint64_t a) { uint32_t v; std::memcpy(&v, (const void*)a, 4); return v; }
void wr32(uint64_t a, uint32_t v) { std::memcpy((void*)a, &v, 4); }

#pragma warning(push)
#pragma warning(disable: 4733)
static bool safe_rd32(uint64_t a, uint32_t* out) {
    __try { std::memcpy(out, (const void*)a, 4); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool safe_wr32(uint64_t a, uint32_t v) {
    __try { std::memcpy((void*)a, &v, 4); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool safe_memcpy_from(void* dst, uint64_t src, size_t n) {
    __try { std::memcpy(dst, (const void*)src, n); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#pragma warning(pop)

// ---- ecritures validees --------------------------------------------------------------------------------------------
struct VWrite { uint64_t addr; std::vector<uint32_t> val, orig; int stride; bool is_float; };
// W : orig = dword lu par le script a cette adresse en fabriquant le relais ; obj / ti = objet gere porteur du champ et
// son pointeur d info de type (obj = 0 : emplacement dans une ressource, pas d objet a verifier).
struct Write { uint64_t addr; uint32_t val, orig; uint64_t obj, ti; };
std::unordered_map<uint64_t, std::vector<std::vector<uint32_t>>> g_vhist;   // valeurs que NOUS avons ecrites, par adresse
std::unordered_map<uint64_t, uint32_t> g_sampled;                            // dword lu a chaque adresse W au chargement du relais
uint64_t g_v_written = 0, g_v_skipped = 0, g_drive_writes = 0, g_w_stale = 0, g_w_legacy = 0, g_queue_stale = 0;
bool hist_has(uint64_t a, const std::vector<uint32_t>& cur) {
    auto it = g_vhist.find(a); if (it == g_vhist.end()) return false;
    for (auto& h : it->second) if (h == cur) return true;
    return false;
}
// single-dword overload: no heap allocation (the hot path in write_block / slot_accepts)
bool hist_has_one(uint64_t a, uint32_t cur) {
    auto it = g_vhist.find(a); if (it == g_vhist.end()) return false;
    for (auto& h : it->second) if (h.size() == 1 && h[0] == cur) return true;
    return false;
}
void hist_push(uint64_t a, uint32_t v) {
    if (hist_has_one(a, v)) return;
    auto& hl = g_vhist[a]; hl.push_back(std::vector<uint32_t>{v}); if (hl.size() > 8) hl.erase(hl.begin());
}
float f32(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
void sat_val(double r, double g, double b, double& s, double& v) {
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    v = mx; s = mx <= 0 ? 0 : (mx - mn) / mx;
}
bool same_sat_val(const VWrite& w, const std::vector<uint32_t>& cur) {
    double r0, g0, b0, r1, g1, b1;
    if (w.is_float) {
        r0 = f32(w.orig[0]); g0 = f32(w.orig[1]); b0 = f32(w.orig[2]);
        r1 = f32(cur[0]); g1 = f32(cur[1]); b1 = f32(cur[2]);
        if (std::isnan(r1) || std::isnan(g1) || std::isnan(b1) || r1 < 0 || g1 < 0 || b1 < 0 || r1 > 1e4 || g1 > 1e4 || b1 > 1e4) return false;
    } else {
        if (cur[0] > 1023 || cur[1] > 1023 || cur[2] > 1023) return false;
        r0 = w.orig[0]; g0 = w.orig[1]; b0 = w.orig[2]; r1 = cur[0]; g1 = cur[1]; b1 = cur[2];
    }
    double s0, v0, s1, v1; sat_val(r0, g0, b0, s0, v0); sat_val(r1, g1, b1, s1, v1);
    const double vmax = std::max(v0, v1);
    if (vmax <= 0) return v0 == v1;
    return std::fabs(s0 - s1) < 0.03 && std::fabs(v0 - v1) / vmax < 0.03;
}
uint64_t slot(const VWrite& w, size_t k) { return w.addr + (w.stride > 0 ? (uint64_t)w.stride * k : 4ull * k); }
bool write_validated(const std::vector<VWrite>& list) {
    for (auto& w : list) {
        const size_t n = w.val.size();
        for (size_t k = 0; k < n; ++k) if (!dword_writable(slot(w, k))) return false;
        std::vector<uint32_t> cur(n);
        bool read_ok = true;
        for (size_t k = 0; k < n; ++k) { if (!safe_rd32(slot(w, k), &cur[k])) { read_ok = false; break; } }
        if (!read_ok) { err("AV caught reading %llX, dropping block", (unsigned long long)w.addr); return false; }
        if (cur != w.orig && !hist_has(w.addr, cur) && !(n == 3 && same_sat_val(w, cur))) { ++g_v_skipped; continue; }
        if (cur == w.val) continue;
        for (size_t k = 0; k < n; ++k) {
            if (wjournal::active()) wjournal::raw32("vfx::write_validated", slot(w, k), cur[k], w.val[k]);
            if (!safe_wr32(slot(w, k), w.val[k])) { err("AV caught writing %llX, dropping block", (unsigned long long)slot(w, k)); return false; }
        }
        ++g_v_written;
        auto& hl = g_vhist[w.addr];
        if (!hist_has(w.addr, w.val)) { hl.push_back(w.val); if (hl.size() > 8) hl.erase(hl.begin()); }
    }
    return true;
}
// Une ecriture directe n est posee que si l emplacement est encore ce que le script a vu : le dword vaut l origine, la
// valeur demandee, le dword echantillonne au chargement du relais, une valeur deja ecrite par nous, ou une couleur de
// meme saturation / luminosite (teinte deja changee par nous). Sinon l adresse est morte : objet libere ou ressource
// dechargee, et sa memoire sert deja a autre chose (gel du 13/09 : un layer ShadowCast ecrase pendant le rendu).
bool same_sat_val_argb(uint32_t o, uint32_t c) {
    if ((o >> 24) != (c >> 24)) return false;
    double s0, v0, s1, v1;
    sat_val(o & 0xFF, (o >> 8) & 0xFF, (o >> 16) & 0xFF, s0, v0);
    sat_val(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, s1, v1);
    const double vmax = std::max(v0, v1);
    if (vmax <= 0) return v0 == v1;
    return std::fabs(s0 - s1) < 0.03 && std::fabs(v0 - v1) / vmax < 0.03;
}
bool slot_accepts(const Write& w, uint32_t cur) {
    if (cur == w.orig || cur == w.val) return true;
    auto s = g_sampled.find(w.addr); if (s != g_sampled.end() && s->second == cur) return true;
    if (hist_has_one(w.addr, cur)) return true;
    return same_sat_val_argb(w.orig, cur);
}
// A relay block: its writes, and the page verdict write_block used to take at every paint (one VirtualQuery per region
// and frame: ~1 ms per Drive effect, 16/10 perf logs). The verdict = every address an aligned dword of a committed,
// writable, unguarded page (dword_writable), and whether every carrier object's first 8 bytes are readable.
// Invalidation rule: a verdict is used only when it is positive and younger than kPageFrames game frames AND kPageMs
// of wall time (hooks can run while no frame passes); otherwise write_block takes it again before writing, as before.
// The game thread re-takes every block's verdict every kPageRefreshFrames (refresh_pages, budgeted), so in a fight the
// hooks find a fresh one. A new relay replaces every block (fresh verdicts, taken while it is sampled). A failed page
// check, an AV on a read or a write: the block is dropped (dead) as before. What a verdict never replaces: the value
// checks of every write (slot_accepts: origin / ours / sampled / same saturation and luminosity; the carrier's type
// pointer, read under SEH), the alignment of every address, and SEH on every raw access.
constexpr uint64_t kPageFrames = 120;
constexpr ULONGLONG kPageMs = 2000;
constexpr uint64_t kPageRefreshFrames = 60;
struct Block {
    std::vector<Write> w;
    bool dead = false;                                            // dropped: absent for the paints (was: erased)
    bool pages_ok = false, objs_ok = false;
    uint64_t pages_frame = 0;
    ULONGLONG pages_ms = 0;                                       // 0 = no verdict yet
};
void take_verdict(Block& b) {
    bool ok = true, objs = true;
    for (auto& w : b.w) {
        if (!dword_writable(w.addr)) { ok = false; break; }
        if (w.obj && objs && !range_readable(w.obj, 8)) objs = false;
    }
    b.pages_ok = ok;
    b.objs_ok = ok && objs;
    b.pages_frame = cmn::frame();
    b.pages_ms = GetTickCount64();
}
bool verdict_fresh(const Block& b) {
    return b.pages_ok && b.pages_ms && cmn::frame() - b.pages_frame <= kPageFrames && GetTickCount64() - b.pages_ms <= kPageMs;
}
bool write_block(Block& b) {
    // page check: the first unwritable address drops the whole block (addresses share a few regions)
    if (!verdict_fresh(b)) {
        const int64_t t0 = perf::begin();
        take_verdict(b);
        perf::end(perf::HookReqPages, t0);
        if (!b.pages_ok) return false;
    }
    const bool objs_ok = b.objs_ok;
    {
        const int64_t t0 = perf::begin();
        for (auto& w : b.w) {
            if (w.obj) {                                              // l objet porteur doit avoir garde son type
                uint64_t ti = 0;
                if ((!objs_ok && !range_readable(w.obj, 8)) || !safe_memcpy_from(&ti, w.obj, 8) || ti != w.ti) { ++g_w_stale; continue; }
            }
            uint32_t cur;
            if (!safe_rd32(w.addr, &cur)) { b.pages_ok = false; err("AV caught reading %llX, dropping block", (unsigned long long)w.addr); perf::end(perf::HookReqWrites, t0); return false; }
            if (cur == w.val) continue;                               // already holds our value: skip both the validation and the write
            if (!slot_accepts(w, cur)) { ++g_w_stale; continue; }
            if (wjournal::active()) wjournal::raw32("vfx::write_block", w.addr, cur, w.val, w.obj, w.ti);
            if (!safe_wr32(w.addr, w.val)) { b.pages_ok = false; err("AV caught writing %llX, dropping block", (unsigned long long)w.addr); perf::end(perf::HookReqWrites, t0); return false; }
            hist_push(w.addr, w.val);
        }
        perf::end(perf::HookReqWrites, t0);
    }
    return true;
}

// ---- relais -----------------------------------------------------------------------------------------------------
struct ElKey { uint64_t mgr; int cont, cat, el; bool operator==(const ElKey& o) const { return mgr == o.mgr && cont == o.cont && cat == o.cat && el == o.el; } };
struct ElHash { size_t operator()(const ElKey& k) const { return std::hash<uint64_t>()(k.mgr ^ ((uint64_t)k.cont << 40) ^ ((uint64_t)(uint32_t)k.cat << 20) ^ (uint32_t)k.el); } };
struct ContKey { uint64_t mgr; int cont; bool operator==(const ContKey& o) const { return mgr == o.mgr && cont == o.cont; } };
struct ContHash { size_t operator()(const ContKey& k) const { return std::hash<uint64_t>()(k.mgr ^ ((uint64_t)k.cont << 40)); } };
std::mutex g_mx;
std::unordered_map<uint64_t, Block> g_by_mgr;
std::unordered_map<uint64_t, std::vector<VWrite>> g_v_by_mgr;
std::unordered_map<ElKey, Block, ElHash> g_by_el;
std::unordered_map<ContKey, Block, ContHash> g_by_cont;
std::vector<Block*> g_blocks;                                     // every block of the three maps (refresh_pages)
size_t g_refresh_i = 0;
std::atomic<bool> g_any_mgr{ false };                             // a manager block exists (W or V): else paint_for_manager has nothing to do
uint64_t g_last_mgr = 0; ElKey g_last_el{0, -1, -1, -1}; ContKey g_last_cont{0, -1};
std::unordered_set<uint64_t> g_blacklisted_mgrs;
int g_av_count = 0;

uint64_t hex64(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }
uint32_t hex32(const std::string& s) { return (uint32_t)std::strtoul(s.c_str(), nullptr, 16); }
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out; std::istringstream is(line); std::string w;
    while (is >> w) out.push_back(w);
    return out;
}
// The relay text is parsed, sampled and given its page verdicts in slices of kRelayBudgetMs per apply() call (twice a
// frame); the blocks in use stay the previous relay's until the new one is complete, then they are swapped at once
// (up to 10 ms in one frame before, 16/10 perf logs). A newer text restarts the job (the last text wins, as before).
constexpr double kRelayBudgetMs = 0.3;
struct ReloadJob {
    bool active = false;
    int phase = 0;                                                // 0 parse, 1 sample + page verdicts, then the swap
    std::string text;
    size_t pos = 0;
    std::unordered_map<uint64_t, Block> by_mgr;
    std::unordered_map<uint64_t, std::vector<VWrite>> v_by_mgr;
    std::unordered_map<ElKey, Block, ElHash> by_el;
    std::unordered_map<ContKey, Block, ContHash> by_cont;
    Block* cur = nullptr;                                         // element references stay valid when a map rehashes
    std::vector<VWrite>* vcur = nullptr;
    size_t nv = 0, legacy = 0;
    std::vector<Block*> blocks;
    size_t bi = 0;
    std::unordered_map<uint64_t, uint32_t> sampled;
};
ReloadJob g_reload;

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }   // istringstream >>
// hex64 / hex32 / atoi on a word, without a std::string per word
uint64_t hex64v(std::string_view w) {
    char b[48];
    if (w.size() >= sizeof b) return hex64(std::string(w));
    std::memcpy(b, w.data(), w.size()); b[w.size()] = 0;
    return std::strtoull(b, nullptr, 16);
}
uint32_t hex32v(std::string_view w) {
    char b[48];
    if (w.size() >= sizeof b) return hex32(std::string(w));
    std::memcpy(b, w.data(), w.size()); b[w.size()] = 0;
    return (uint32_t)std::strtoul(b, nullptr, 16);
}
int atoiv(std::string_view w) {
    char b[48];
    if (w.size() >= sizeof b) return std::atoi(std::string(w).c_str());
    std::memcpy(b, w.data(), w.size()); b[w.size()] = 0;
    return std::atoi(b);
}

// one relay line (same rules as the getline / split parse it replaces)
void parse_relay_line(ReloadJob& j, const char* b, const char* e) {
    std::string_view p[9];
    size_t n = 0;
    for (const char* q = b; q < e;) {
        while (q < e && is_ws(*q)) ++q;
        if (q >= e) break;
        const char* w = q;
        while (q < e && !is_ws(*q)) ++q;
        if (n < 9) p[n] = std::string_view(w, (size_t)(q - w));
        ++n;
    }
    if (n < 2) return;
    if (p[0] == "M") {
        const uint64_t mgr = hex64v(p[1]);
        j.cur = &j.by_mgr[mgr]; j.vcur = &j.v_by_mgr[mgr];
    } else if (p[0] == "E" && n >= 5) {
        const ElKey k{hex64v(p[1]), atoiv(p[2]), atoiv(p[3]), atoiv(p[4])};
        j.vcur = nullptr;
        if (k.cat < 0 && k.el < 0) j.cur = &j.by_cont[ContKey{k.mgr, k.cont}]; else j.cur = &j.by_el[k];
    } else if (p[0] == "W" && n >= 3 && j.cur) {
        const uint64_t a = hex64v(p[1]);
        if (a <= 0x10000 || in_module(a)) return;
        if (n < 4) { ++j.legacy; return; }                        // sans origine : refusee (script trop ancien)
        Write w{a, hex32v(p[2]), hex32v(p[3]), 0, 0};
        if (n >= 6) {
            w.obj = hex64v(p[4]); w.ti = hex64v(p[5]);
            if (w.obj <= 0x10000 || in_module(w.obj) || w.ti == 0) return;
        }
        j.cur->w.push_back(w);
    } else if (p[0] == "V" && n >= 4 && j.vcur) {
        const uint64_t a = hex64v(p[1]);
        if (a > 0x10000 && !in_module(a)) { j.vcur->push_back({a, {hex32v(p[2])}, {hex32v(p[3])}, 0, false}); ++j.nv; }
    } else if (p[0] == "V3" && n >= 8 && j.vcur) {
        const uint64_t a = hex64v(p[1]);
        if (a > 0x10000 && !in_module(a)) { j.vcur->push_back({a, {hex32v(p[2]), hex32v(p[3]), hex32v(p[4])}, {hex32v(p[5]), hex32v(p[6]), hex32v(p[7])}, 0, true}); ++j.nv; }
    } else if (p[0] == "VC" && n >= 9 && j.vcur) {
        const uint64_t a = hex64v(p[1]); const int st = atoiv(p[2]);
        if (a > 0x10000 && !in_module(a) && st > 0 && st < 0x10000) { j.vcur->push_back({a, {hex32v(p[3]), hex32v(p[4]), hex32v(p[5])}, {hex32v(p[6]), hex32v(p[7]), hex32v(p[8])}, st, false}); ++j.nv; }
    }
}

void swap_relay(ReloadJob& j) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_by_mgr.swap(j.by_mgr); g_by_el.swap(j.by_el); g_by_cont.swap(j.by_cont);
    g_v_by_mgr.clear();
    for (auto& kv : j.v_by_mgr) if (!kv.second.empty()) g_v_by_mgr[kv.first] = std::move(kv.second);
    g_sampled.swap(j.sampled);
    g_blocks.clear();
    for (auto& kv : g_by_mgr) g_blocks.push_back(&kv.second);
    for (auto& kv : g_by_el) g_blocks.push_back(&kv.second);
    for (auto& kv : g_by_cont) g_blocks.push_back(&kv.second);
    g_refresh_i = 0;
    g_any_mgr.store(!g_by_mgr.empty() || !g_v_by_mgr.empty(), std::memory_order_relaxed);
    if (g_vhist.size() > 20000) g_vhist.clear();                 // borne : des adresses mortes s y accumulent au fil des rechargements
    g_last_mgr = 0; g_last_el = ElKey{0, -1, -1, -1}; g_last_cont = ContKey{0, -1};
    g_blacklisted_mgrs.clear(); g_av_count = 0; g_active = true;
    g_w_legacy += j.legacy;
    info("drive colours reloaded, %zu actors, %zu elements, %zu containers, %zu validated writes, %zu addresses sampled, %zu stale writes refused so far", g_by_mgr.size(), g_by_el.size(), g_by_cont.size(), j.nv, g_sampled.size(), (size_t)g_w_stale);
    if (j.legacy) err("%zu 'W' lines without origin refused (SF6_DriveRecolor / SF6_DIRecolor too old for this plugin)", j.legacy);
}

void reload_relay() {
    {
        std::lock_guard<std::mutex> lk(g_io_mx);
        if (g_relay_new) {                                        // (was: relu quand le fichier change)
            g_relay_new = false;
            g_reload = ReloadJob{};
            g_reload.active = true;
            g_reload.text.swap(g_relay_text);
        }
    }
    ReloadJob& j = g_reload;
    if (!j.active) return;
    perf::Scope timed(perf::VfxRelayReload);
    init_budget();
    const int64_t t0 = budget_now();
    if (j.phase == 0) {
        const char* base = j.text.data();
        const size_t size = j.text.size();
        unsigned lines = 0;
        while (j.pos < size) {
            const char* nl = static_cast<const char*>(std::memchr(base + j.pos, '\n', size - j.pos));
            const size_t end = nl ? (size_t)(nl - base) : size;
            parse_relay_line(j, base + j.pos, base + end);
            j.pos = nl ? end + 1 : size;
            if ((++lines & 31) == 0 && over_ms(t0, kRelayBudgetMs)) return;
        }
        for (auto& kv : j.by_mgr) j.blocks.push_back(&kv.second);
        for (auto& kv : j.by_el) j.blocks.push_back(&kv.second);
        for (auto& kv : j.by_cont) j.blocks.push_back(&kv.second);
        j.phase = 1;
        if (over_ms(t0, kRelayBudgetMs)) return;
    }
    // Echantillon : le dword present a chaque adresse W maintenant, sur le thread du jeu, juste apres que le script
    // les a resolues sur des objets vivants. Une adresse qui ne le porte plus (ni l origine, ni une valeur a nous)
    // au moment d ecrire est une adresse morte. Each block also gets its page verdict here.
    while (j.bi < j.blocks.size()) {
        Block& b = *j.blocks[j.bi++];
        for (auto& w : b.w) {
            uint32_t v;
            if (!j.sampled.count(w.addr) && (page_state(w.addr) & 1) && (page_state(w.addr + 3) & 1) && safe_rd32(w.addr, &v)) j.sampled[w.addr] = v;
        }
        take_verdict(b);
        if (over_ms(t0, kRelayBudgetMs)) return;
    }
    swap_relay(j);
    g_reload = ReloadJob{};
}

// The page verdicts of the blocks in use, taken again every kPageRefreshFrames on the game thread (within
// kRefreshBudgetMs per call), so that the effect hooks rarely have to (see Block).
constexpr double kRefreshBudgetMs = 0.1;
void refresh_pages() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_blocks.empty()) return;
    perf::Scope timed(perf::VfxPageRefresh);
    init_budget();
    const int64_t t0 = budget_now();
    const uint64_t fr = cmn::frame();
    for (size_t n = 0; n < g_blocks.size(); ++n) {
        if (g_refresh_i >= g_blocks.size()) g_refresh_i = 0;
        Block& b = *g_blocks[g_refresh_i++];
        if (b.dead || (b.pages_ms && fr - b.pages_frame < kPageRefreshFrames)) continue;
        take_verdict(b);
        if (over_ms(t0, kRefreshBudgetMs)) break;
    }
}

// Appele DEPUIS requestEffect / addMaterialEffect du jeu, sur le thread du jeu, avant que l effet existe.
void paint_for_manager(uint64_t mgr) {
    if (!g_any_mgr.load(std::memory_order_relaxed)) return;      // no manager block in the relay (light fight: never)
    if (mgr == 0 || mgr == g_last_mgr || g_blacklisted_mgrs.count(mgr)) return;
    Block* list = nullptr; std::vector<VWrite>* vlist = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto vi = g_v_by_mgr.find(mgr); if (vi != g_v_by_mgr.end()) vlist = &vi->second;
        auto li = g_by_mgr.find(mgr); if (li != g_by_mgr.end() && !li->second.dead) list = &li->second;
        if (!list && !vlist) return;
        g_last_mgr = mgr;
        if (vlist && !vlist->empty()) {
            if (!write_validated(*vlist)) { g_v_by_mgr.erase(mgr); err("validated block %llX dropped: a page is no longer writable", (unsigned long long)mgr); }
            else g_drive_writes += vlist->size();
        }
        if (!list) return;
        if (!write_block(*list)) { list->dead = true; err("actor block %llX dropped: a page is no longer writable", (unsigned long long)mgr); return; }
        g_drive_writes += list->w.size();
    }
}
bool paint_for_element(uint64_t mgr, int cont, int cat, int el) {
    const ElKey key{mgr, cont, cat, el};
    if (key == g_last_el) return true;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_by_el.find(key); if (it == g_by_el.end() || it->second.dead) return false;
    g_last_el = key;
    if (!write_block(it->second)) { it->second.dead = true; err("element block %llX %d/%d/%d dropped: a page is no longer writable", (unsigned long long)mgr, cont, cat, el); return true; }
    g_drive_writes += it->second.w.size();
    return true;
}
bool paint_for_container(uint64_t mgr, int cont) {
    const ContKey key{mgr, cont};
    if (key == g_last_cont) return true;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_by_cont.find(key); if (it == g_by_cont.end() || it->second.dead) return false;
    g_last_cont = key;
    if (!write_block(it->second)) { it->second.dead = true; err("container block %llX %d dropped: a page is no longer writable", (unsigned long long)mgr, cont); return true; }
    g_drive_writes += it->second.w.size();
    return true;
}

// ---- hooks du jeu -----------------------------------------------------------------------------------------------
// argv[1] = this (le gestionnaire d effets de L ACTEUR), argv[2] = le VfxID demande (categorie @0x10, conteneur
// @0x14, element @0x18 - releve du 05/09). Meme disposition que le C# (Span<ulong> args).
#pragma warning(push)
#pragma warning(disable: 4733)
static int pre_request_effect_inner(int argc, void** argv) {
    const uint64_t mgr = (uint64_t)argv[1];
    const uint64_t id = (uint64_t)argv[2];
    bool done = false;
    if (id > 0x10000) {                                           // the game's own VfxID argument: read under SEH (safe_rd32)
        uint32_t cat_v, cont_v, el_v;
        if (safe_rd32(id + 0x10, &cat_v) && safe_rd32(id + 0x14, &cont_v) && safe_rd32(id + 0x18, &el_v)) {
            done = paint_for_element(mgr, (int)cont_v, (int)cat_v, (int)el_v);
            if (!done) done = paint_for_container(mgr, (int)cont_v);
            else g_last_cont = ContKey{0, -1};
            vfx::mech::on_request(mgr, cat_v, cont_v, el_v);
        }
    }
    if (!done) paint_for_manager(mgr);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_request_effect_seh(int argc, void** argv) {
    if (!g_active || argc < 3) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const uint64_t mgr = (uint64_t)argv[1];
    __try { return pre_request_effect_inner(argc, argv); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        ++g_av_count;
        g_blacklisted_mgrs.insert(mgr);
        err("AV caught in requestEffect hook (mgr %llX blacklisted, total AVs %d)", (unsigned long long)mgr, g_av_count);
        if (g_av_count >= 5) { g_active = false; err("too many AVs, plugin deactivated until next relay reload"); }
        return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    }
}
int pre_add_material_seh(int argc, void** argv) {
    if (!g_active || argc < 2) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const uint64_t mgr = (uint64_t)argv[1];
    __try { paint_for_manager(mgr); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        ++g_av_count;
        g_blacklisted_mgrs.insert(mgr);
        err("AV caught in addMaterialEffect hook (mgr %llX blacklisted, total AVs %d)", (unsigned long long)mgr, g_av_count);
        if (g_av_count >= 5) { g_active = false; err("too many AVs, plugin deactivated until next relay reload"); }
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
#pragma warning(pop)
// timed from outside the __try functions (perf.hpp)
int pre_request_effect(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int64_t t0 = perf::begin();
    const int r = pre_request_effect_seh(argc, argv);
    perf::end(perf::HookRequestEffect, t0);
    return r;
}
int pre_add_material(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    const int64_t t0 = perf::begin();
    const int r = pre_add_material_seh(argc, argv);
    perf::end(perf::HookAddMaterial, t0);
    return r;
}
void post_nop(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

bool install_hooks() {
    auto* td = reapi().tdb()->find_type("app.vfx.VfxActorEffectManager");
    if (!td) { err("type app.vfx.VfxActorEffectManager introuvable"); return false; }
    int n = 0;
    for (auto* m : td->get_methods()) {
        if (std::string(m->get_name()) != "requestEffect" || !m->get_function_raw()) continue;
        // on lit argv[2] : le VfxID doit etre le PREMIER parametre (les surcharges sans VfxID sont ignorees)
        bool ok = true;
        const auto params = m->get_params();
        if (!params.empty() && params[0].t) {
            reframework::API::TypeDefinition* t0 = reinterpret_cast<reframework::API::TypeDefinition*>(params[0].t);
            ok = t0->get_full_name().find("VfxID") != std::string::npos;
        }
        if (!ok) continue;
        if (m->add_hook(pre_request_effect, post_nop, false)) { ++n; perf::journal_hooked("VfxActorEffectManager.requestEffect (HookRequestEffect)", m); }
    }
    auto* add = td->find_method("addMaterialEffect");
    bool ok_add = false;
    if (add && add->get_function_raw()) { ok_add = add->add_hook(pre_add_material, post_nop, false) != 0; if (ok_add) perf::journal_hooked("VfxActorEffectManager.addMaterialEffect (HookAddMaterial)", add); }
    info("hooks: requestEffect x%d, addMaterialEffect %s", n, ok_add ? "ok" : "absent");
    return n > 0;
}

// ---- balayage (portage de CheckScanRequest / ScanNode) ------------------------------------------------------------
struct Pat { std::vector<uint8_t> pre, suf; int hole, idx; };
struct Palette {
    std::unordered_set<uint32_t> colours;
    std::unordered_map<uint32_t, std::vector<std::array<uint8_t, 8>>> sigs;
    std::unordered_map<uint32_t, std::vector<Pat>> pats;
    std::array<bool, 256> first{};
    int pat_count = 0;
    bool match(const uint8_t* buf, int i, int avail) const {
        const uint32_t key = buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16);
        if (!colours.count(key)) return false;
        auto it = sigs.find(key); if (it == sigs.end()) return true;
        if (i + 12 > avail) return false;
        for (auto& sg : it->second) if (std::memcmp(buf + i + 4, sg.data(), 8) == 0) return true;
        return false;
    }
    void add_pat(int idx, std::vector<uint8_t> pre, int hole, std::vector<uint8_t> suf) {
        if (pre.size() < 4 || hole < 1 || hole > 16) return;
        const uint32_t k = pre[0] | (pre[1] << 8) | (pre[2] << 16) | ((uint32_t)pre[3] << 24);
        first[pre[0]] = true;
        pats[k].push_back(Pat{std::move(pre), std::move(suf), hole, idx});
        ++pat_count;
    }
};
std::vector<uint8_t> hex_bytes(const std::string& h) {
    std::vector<uint8_t> b;
    if (h == "-" || (h.size() & 1)) return b;
    for (size_t i = 0; i + 1 < h.size(); i += 2) b.push_back((uint8_t)std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
    return b;
}
constexpr int SCAN_NODE_MAX = 40000, SCAN_DEPTH = 3, SCAN_NODE_BYTES = 0x400, SCAN_PAT_BYTES = 0x600;
struct Spot { uint64_t addr; uint8_t r, g, b; };
struct PSpot { uint64_t addr; int idx; std::string cur; };

// ---- scan cache: remember spots found per base file so a subsequent scan with the same resources skips the walk ----
struct CachedSpot { uint64_t addr; uint8_t r, g, b; std::string file; };
struct CachedPSpot { uint64_t addr; int idx; std::string cur; std::string file; };
std::vector<CachedSpot> g_spot_cache;
std::vector<CachedPSpot> g_pspot_cache;
bool g_cache_valid = false;

// ---- incremental scan state ----
struct IncrScan {
    bool active = false;
    std::string seq;
    std::unordered_map<std::string, Palette> palettes;
    struct SBase { std::string file; uint64_t arr; int count; };
    std::vector<SBase> bases;
    std::vector<std::pair<uint64_t, int>> dumps;
    bool walking = false;                                             // the walk is on the background thread
    std::unordered_set<uint64_t> seen;
    std::vector<std::string> out;
    long long nodes_total = 0;
    int spots_total = 0;
    std::chrono::steady_clock::time_point t0_wall;                    // start wall time for the final log
};
IncrScan g_incr;

// ---- the walk, on a background thread ----------------------------------------------------------------------------
// The pointer walk reads raw memory only (under SEH, behind a page check with the thread's own memguard::Local cache)
// and finds candidate spots; nothing is written there. Every spot is checked again on the game thread before any write
// (tick: spot_ok on the live value, the queue: page check + value check). Up to 1.7 ms a frame for ~8 s at fight start
// before (16/10), and the budget cut each slice's walk short: whole subtrees were skipped (99 to 333 spots per scan).
// Now each base is walked whole, one visited set and SCAN_NODE_MAX per base, as the walk did before 0.19.10.
struct Walk {
    const Palette& pal;
    memguard::Local& pg;
    std::array<std::vector<uint8_t>, SCAN_DEPTH + 1>& buf;
    std::unordered_set<uint64_t> visited;
    int nodes = 0;
    std::vector<Spot> spots;
    std::vector<PSpot> pspots;
    bool stop = false;                                                // the game is closing
};
void scan_node(Walk& wk, uint64_t addr, int depth) {
    if (wk.stop || wk.nodes > SCAN_NODE_MAX || !wk.visited.insert(addr).second) return;
    ++wk.nodes;
    if ((wk.nodes & 255) == 0 && cmn::closing()) { wk.stop = true; return; }
    const Palette& pal = wk.pal;
    const int want = pal.pat_count > 0 ? SCAN_PAT_BYTES : SCAN_NODE_BYTES;
    int avail = 0;
    for (uint64_t p = addr & ~0xFFFULL; p < addr + want && (wk.pg.state(p) & 1); p += 0x1000) avail = (int)std::min<uint64_t>(want, p + 0x1000 - addr);
    if (avail < 8) return;
    auto& buf = wk.buf[depth];
    if (buf.size() < (size_t)SCAN_PAT_BYTES) buf.resize(SCAN_PAT_BYTES);
    if (!safe_memcpy_from(buf.data(), addr, avail)) return;
    const int avail_col = std::min(avail, SCAN_NODE_BYTES);
    for (int i = 0; i + 3 < avail_col; ++i) {
        if (buf[i + 3] != 255) continue;
        if (((addr + i) & 3) == 0 && pal.match(buf.data(), i, avail_col)) wk.spots.push_back({addr + i, buf[i], buf[i + 1], buf[i + 2]});   // aligned only (dword_writable)
    }
    if (pal.pat_count > 0) {
        for (int i = 0; i + 4 <= avail; ++i) {
            if (!pal.first[buf[i]]) continue;
            const uint32_t k = buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
            auto it = pal.pats.find(k); if (it == pal.pats.end()) continue;
            for (auto& p : it->second) {
                const int h0 = i + (int)p.pre.size(), s0 = h0 + p.hole;
                if ((addr + h0) & 3) continue;                                 // aligned only (dword_writable)
                if (s0 + (int)p.suf.size() > avail) continue;
                if (std::memcmp(buf.data() + i + 4, p.pre.data() + 4, p.pre.size() - 4) != 0) continue;
                if (!p.suf.empty() && std::memcmp(buf.data() + s0, p.suf.data(), p.suf.size()) != 0) continue;
                std::string cur; char t[3];
                for (int q = 0; q < p.hole; ++q) { std::snprintf(t, sizeof t, "%02x", buf[h0 + q]); cur += t; }
                wk.pspots.push_back({addr + h0, p.idx, cur});
            }
        }
    }
    if (depth <= 0) return;
    for (int off = 0; off <= 0xF8 && off + 8 <= avail && !wk.stop; off += 8) {
        uint64_t pv; std::memcpy(&pv, buf.data() + off, 8);
        if (pv >= 0x200000000ULL && pv < 0x7FFFFFFFFFFFULL && (pv & 7) == 0 && !wk.visited.count(pv))
            scan_node(wk, pv, depth - 1);
    }
}

struct WalkOut { std::vector<Spot> spots; std::vector<PSpot> pspots; int nodes = 0; };
struct WalkJob {
    std::unordered_map<std::string, Palette> palettes;
    std::vector<IncrScan::SBase> bases;
    std::vector<WalkOut> outs;                                        // per base, in the request's order
};
std::mutex g_walk_mx;
std::condition_variable g_walk_cv;
std::unique_ptr<WalkJob> g_walk_todo, g_walk_done;
bool g_walk_started = false;                                          // game thread only

void walk_job(WalkJob& j) {
    memguard::Local pg;
    std::array<std::vector<uint8_t>, SCAN_DEPTH + 1> buf;
    j.outs.resize(j.bases.size());
    for (size_t i = 0; i < j.bases.size(); ++i) {
        const auto& b = j.bases[i];
        auto pi = j.palettes.find(b.file);
        if (pi == j.palettes.end() || b.count <= 0 || b.count > 128 || in_module(b.arr)) continue;
        Walk wk{ pi->second, pg, buf };
        for (int x = 0; x < b.count && !wk.stop; ++x) scan_node(wk, b.arr + (uint64_t)x * 0x50, SCAN_DEPTH);
        j.outs[i].spots = std::move(wk.spots);
        j.outs[i].pspots = std::move(wk.pspots);
        j.outs[i].nodes = wk.nodes;
        if (wk.stop) break;
    }
}
void walk_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::unique_ptr<WalkJob> j;
        {
            std::unique_lock<std::mutex> lk(g_walk_mx);
            g_walk_cv.wait(lk, [] { return g_walk_todo != nullptr; });
            j = std::move(g_walk_todo);
        }
        try { walk_job(*j); } catch (...) {}                          // (bad_alloc) what was found so far is handed back
        std::lock_guard<std::mutex> lk(g_walk_mx);
        g_walk_done = std::move(j);
    }
}

// try_scan_cache: if the cache holds spots for every base file and they still validate, skip the walk
std::string g_cache_bases;                                        // the bases (file, entry array, count) the cache was found in
std::string bases_key(const IncrScan& sc) {
    std::string k;
    char b[64];
    for (auto& x : sc.bases) { std::snprintf(b, sizeof b, "@%llX#%d|", (unsigned long long)x.arr, x.count); k += x.file + b; }
    return k;
}
// The cache holds only for the very same resources (same entry arrays): spots of resources the game freed may still
// hold their colours, a colour check alone would let writes into freed memory through.
bool try_scan_cache(const IncrScan& req) {
    if (!g_cache_valid || g_spot_cache.empty() || bases_key(req) != g_cache_bases) return false;
    std::unordered_set<std::string> needed;
    for (auto& b : req.bases) needed.insert(b.file);
    // every needed base file must have at least one cached spot, and all cached spots of needed files must validate
    std::unordered_set<std::string> covered;
    for (auto& cs : g_spot_cache) {
        if (!needed.count(cs.file)) continue;
        covered.insert(cs.file);
        uint32_t cur;
        if (!safe_rd32(cs.addr, &cur)) return false;
        const uint32_t expected = cs.r | (cs.g << 8) | (cs.b << 16) | (0xFFu << 24);
        if (cur != expected) return false;
    }
    return covered.size() == needed.size();
}

void finish_scan(IncrScan& sc) {
    // process dumps
    char line[256];
    for (auto& d : sc.dumps) {
        std::snprintf(line, sizeof line, "H %llX ", (unsigned long long)d.first);
        std::string l = line;
        if (d.first < 0x10000 || in_module(d.first) || !range_readable(d.first, d.second)) l += "-";
        else { char t[3]; for (int i = 0; i < d.second; ++i) { std::snprintf(t, sizeof t, "%02x", ((const uint8_t*)d.first)[i]); l += t; } }
        sc.out.push_back(l);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sc.t0_wall).count();
    std::snprintf(line, sizeof line, "N %d %lld %lld", sc.spots_total, sc.nodes_total, (long long)ms);
    sc.out.insert(sc.out.begin() + 1, line);
    sc.out.emplace_back("END");
    {
        std::string res;
        for (auto& l : sc.out) { res += l; res += '\n'; }
        std::lock_guard<std::mutex> lk(g_io_mx);
        g_scan_res_text.swap(res);
    }
    info("drive scan #%s: %d spots in %zu bases, %lld nodes, %lld ms (background walk)", sc.seq.c_str(), sc.spots_total, sc.bases.size(), sc.nodes_total, (long long)ms);
    sc.active = false;
}

// scan_step: start a new scan (parse, cache), hand its walk to the background thread, commit the walk's result
void scan_step() {
    init_budget();
    if (!g_incr.active) {
        // check for a new request
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(g_io_mx);
            if (g_scan_req_text.empty()) return;
            std::istringstream f(g_scan_req_text); std::string l; while (std::getline(f, l)) lines.push_back(l);
            g_scan_req_text.clear();
        }
        if (lines.empty()) return;
        { std::string last = lines.back(); while (!last.empty() && (last.back() == ' ' || last.back() == '\r')) last.pop_back(); if (last != "END") return; }
        // parse the request (sub-scope ScanParse)
        {
            perf::Scope sp(perf::ScanParse);
            g_incr = IncrScan{};
            g_incr.active = true;
            g_incr.seq = "0";
            g_incr.t0_wall = std::chrono::steady_clock::now();
            g_incr.out.push_back("R 0");
            for (auto& raw : lines) {
                const auto p = split(raw);
                if (p.size() < 2) continue;
                if (p[0] == "R") { g_incr.seq = p[1]; g_incr.out[0] = "R " + p[1]; }
                else if (p[0] == "P") {
                    auto& pal = g_incr.palettes[p[1]];
                    for (size_t i = 2; i < p.size(); ++i) {
                        const auto colon = p[i].find(':');
                        const std::string cs = colon == std::string::npos ? p[i] : p[i].substr(0, colon);
                        int r, g, b; if (std::sscanf(cs.c_str(), "%d,%d,%d", &r, &g, &b) != 3) continue;
                        const uint32_t key = (uint32_t)(r | (g << 8) | (b << 16));
                        pal.colours.insert(key);
                        if (colon != std::string::npos && p[i].size() - colon - 1 == 16) {
                            const auto sg = hex_bytes(p[i].substr(colon + 1));
                            std::array<uint8_t, 8> a{}; std::memcpy(a.data(), sg.data(), 8); pal.sigs[key].push_back(a);
                        }
                    }
                } else if (p[0] == "X" && p.size() >= 6) {
                    g_incr.palettes[p[1]].add_pat(std::atoi(p[2].c_str()), hex_bytes(p[3]), std::atoi(p[4].c_str()), hex_bytes(p[5]));
                } else if (p[0] == "F" && p.size() >= 4) {
                    g_incr.bases.push_back({p[1], hex64(p[2]), std::atoi(p[3].c_str())});
                } else if (p[0] == "D" && p.size() >= 3) {
                    g_incr.dumps.emplace_back(hex64(p[1]), std::min(0x400, std::atoi(p[2].c_str())));
                }
            }
        }
        // try the scan cache: if cached spots for every base file still validate, skip the full walk
        if (try_scan_cache(g_incr)) {
            for (auto& cs : g_spot_cache) {
                if (!g_incr.seen.insert(cs.addr).second) continue;
                char line[256];
                std::snprintf(line, sizeof line, "S %llX %d %d %d %s", (unsigned long long)cs.addr, cs.r, cs.g, cs.b, cs.file.c_str());
                g_incr.out.emplace_back(line);
                ++g_incr.spots_total;
            }
            for (auto& cp : g_pspot_cache) {
                if (!g_incr.seen.insert(cp.addr).second) continue;
                char line[256];
                std::snprintf(line, sizeof line, "T %llX %d %s %s", (unsigned long long)cp.addr, cp.idx, cp.cur.c_str(), cp.file.c_str());
                g_incr.out.emplace_back(line);
                ++g_incr.spots_total;
            }
            info("drive scan #%s: cache hit (%d spots), walk skipped", g_incr.seq.c_str(), g_incr.spots_total);
            finish_scan(g_incr);
            return;
        }
    }
    if (!g_incr.walking) {                                            // hand the walk over (sub-scope ScanWalk)
        perf::Scope sw(perf::ScanWalk);
        g_spot_cache.clear(); g_pspot_cache.clear(); g_cache_valid = false;   // a new scan replaces the old cache
        auto j = std::make_unique<WalkJob>();
        j->palettes = std::move(g_incr.palettes);
        j->bases = g_incr.bases;
        if (!g_walk_started) { std::thread(walk_thread).detach(); g_walk_started = true; }
        {
            std::lock_guard<std::mutex> lk(g_walk_mx);
            g_walk_todo = std::move(j);
        }
        g_walk_cv.notify_one();
        g_incr.walking = true;
        return;
    }
    std::unique_ptr<WalkJob> done;
    {
        std::lock_guard<std::mutex> lk(g_walk_mx);
        done = std::move(g_walk_done);
    }
    if (!done) return;                                                // still walking
    perf::Scope sc(perf::ScanCommit);
    char line[256];
    for (size_t i = 0; i < done->outs.size() && i < g_incr.bases.size(); ++i) {
        const auto& b = g_incr.bases[i];
        const WalkOut& o = done->outs[i];
        g_incr.nodes_total += o.nodes;
        for (auto& s : o.spots) {
            if (!g_incr.seen.insert(s.addr).second) continue;
            std::snprintf(line, sizeof line, "S %llX %d %d %d %s", (unsigned long long)s.addr, s.r, s.g, s.b, b.file.c_str());
            g_incr.out.emplace_back(line);
            ++g_incr.spots_total;
            g_spot_cache.push_back(CachedSpot{ s.addr, s.r, s.g, s.b, b.file });
        }
        for (auto& t : o.pspots) {
            if (!g_incr.seen.insert(t.addr).second) continue;
            std::snprintf(line, sizeof line, "T %llX %d %s %s", (unsigned long long)t.addr, t.idx, t.cur.c_str(), b.file.c_str());
            g_incr.out.emplace_back(line);
            ++g_incr.spots_total;
            g_pspot_cache.push_back(CachedPSpot{ t.addr, t.idx, t.cur, b.file });
        }
    }
    // all bases done: the scan cache was filled from the walk, finish
    g_cache_valid = !g_spot_cache.empty() || !g_pspot_cache.empty();
    g_cache_bases = bases_key(g_incr);
    finish_scan(g_incr);
}

// ---- la file de dwords (_memwrite.txt) ----------------------------------------------------------------------------
uint64_t g_applied = 0, g_refused = 0;
constexpr double kQueueBudgetMs = 0.5;            // apply() runs twice per frame (Update + LateUpdate)
void apply_queue() {
    // take new lines from the input
    {
        std::lock_guard<std::mutex> lk(g_io_mx);
        if (!g_queue_lines.empty()) {
            for (auto& l : g_queue_lines) g_pending_queue.push_back(std::move(l));
            g_queue_lines.clear();
        }
    }
    if (g_pending_queue.empty()) return;
    init_budget();
    const int64_t t0 = budget_now();
    int n = 0, stale = 0;
    while (!g_pending_queue.empty()) {
        const std::string raw = std::move(g_pending_queue.front());
        g_pending_queue.pop_front();
        const auto p = split(raw);
        if (p.size() < 2 || p[0].empty() || p[0][0] == '#') continue;
        auto strip = [](std::string s) { if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2); return s; };
        const uint64_t addr = hex64(strip(p[0])); const uint32_t val = hex32(strip(p[1]));
        if (addr < 0x10000) continue;
        if (in_module(addr)) { ++g_refused; err("refused %llX : inside a loaded module", (unsigned long long)addr); continue; }
        if (!dword_writable(addr)) { ++g_refused; ++g_page_refused; continue; }
        uint32_t cur;
        if (!safe_rd32(addr, &cur)) { ++g_refused; continue; }
        bool ok = (cur == val) || hist_has(addr, std::vector<uint32_t>{cur});
        if (!ok && p.size() >= 3) ok = (cur == hex32(strip(p[2])));
        if (!ok) { ++g_refused; ++stale; continue; }
        if (cur != val) {
            if (wjournal::active()) wjournal::raw32("vfx::apply_queue", addr, cur, val);
            if (!safe_wr32(addr, val)) { ++g_refused; err("AV writing queue addr %llX", (unsigned long long)addr); continue; }
            hist_push(addr, val);
        }
        ++n;
        if ((n & 3) == 0 && (double)(budget_now() - t0) >= kQueueBudgetMs * g_ticks_per_ms) break;
    }
    g_applied += n;
    if (stale) { g_queue_stale += stale; err("queue: %d line(s) refused, the address no longer holds what the script read", stale); }
}
void apply() { if (!g_active) return; reload_relay(); refresh_pages(); apply_queue(); }

// ---- points d entree ---------------------------------------------------------------------------------------------
bool g_hooked = false; unsigned g_tick = 0, g_retry = 0; bool g_gave_up = false;
void on_update() {
    if (cmn::closing() || !g_installed) return;
    perf::Scope timed(perf::VfxEngineUpdate);
    if (!g_hooked) {
        if (g_gave_up || ++g_tick % 300) return;
        if (++g_retry > 20) { g_gave_up = true; err("hook impossible, abandon"); return; }
        g_hooked = install_hooks();
        if (!g_hooked) return;
    }
    if (g_active) { perf::Scope scan(perf::VfxScan); scan_step(); }
    perf::Scope a(perf::VfxApply);
    apply();
}
void on_late_update() {
    if (cmn::closing() || !g_installed) return;
    perf::Scope timed(perf::VfxEngineLate);
    perf::Scope a(perf::VfxApply);
    apply();
}

// Reset Scripts: the Lua empties the relay in re.on_script_reset; done here too, so no address outlives the scripts
// that validate it even if that callback did not run.
void on_lua_destroyed(lua_State*) {
    std::lock_guard<std::mutex> lk(g_io_mx);
    g_queue_lines.clear();
    if (vfx::tick::ready()) return;                       // owner vfx: the DLL keeps revalidating its own relay and scan
    g_relay_text.clear(); g_relay_new = true;
    g_scan_req_text.clear();
}

}  // namespace

bool install() {
    // the two would hook requestEffect / addMaterialEffect and both write: the old plugin keeps the job while loaded
    if (GetModuleHandleA("SF6_MemWriterNative.dll")) {
        cmn::log_warn("[ColorModNative] vfx_engine: SF6_MemWriterNative.dll is loaded and keeps the job (rename it to .dll.off to hand over)");
        return false;
    }
    auto* fns = reframework::API::get()->param()->functions;
    if (!fns->on_pre_application_entry) return false;
    map_modules();
    g_hooked = install_hooks();
    fns->on_pre_application_entry("UpdateBehavior", on_update);
    fns->on_pre_application_entry("LateUpdateBehavior", on_late_update);
    fns->on_lua_state_destroyed(on_lua_destroyed);
    g_active = true;
    g_installed = true;
    info("ready (in SF6_ColorModNative), %zu modules mapped (writes inside them are refused), relay / queue / scan through the Lua bridge", g_modules.size());
    return true;
}

bool ready() { return g_installed; }

void relay(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_io_mx);
    g_relay_text = text;
    g_relay_new = true;
    if (text.empty()) mech::on_combat_exit();     // the Lua empties the relay when it leaves a fight
}

void relay_native(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_io_mx);
    g_relay_text = text;
    g_relay_new = true;
}

void queue(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_io_mx);
    std::istringstream f(text); std::string l;
    while (std::getline(f, l)) if (!l.empty()) g_queue_lines.push_back(l);
}

void scan_request(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_io_mx);
    g_scan_req_text = text;
}

std::string scan_result() {
    std::lock_guard<std::mutex> lk(g_io_mx);
    return g_scan_res_text;
}

std::string status() {
    char b[512];
    size_t actors = 0, elements = 0, containers = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        actors = g_by_mgr.size(); elements = g_by_el.size(); containers = g_by_cont.size();
    }
    std::snprintf(b, sizeof b, "vfx_engine installed=%d hooked=%d active=%d actors=%zu elements=%zu containers=%zu drive_writes=%llu v_written=%llu v_skipped=%llu w_stale=%llu queue_applied=%llu queue_refused=%llu avs=%d",
                  g_installed ? 1 : 0, g_hooked ? 1 : 0, g_active ? 1 : 0, actors, elements, containers, (unsigned long long)g_drive_writes,
                  (unsigned long long)g_v_written, (unsigned long long)g_v_skipped, (unsigned long long)g_w_stale, (unsigned long long)g_applied,
                  (unsigned long long)g_refused, g_av_count);
    return b;
}

}  // namespace vfx::engine
