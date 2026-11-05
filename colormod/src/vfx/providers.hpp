// vfx::providers - the Drive Impact / Reversal palettes of the characters' effect providers (phase 2 step 3, sub-step C,
// docs/CPP_PORT_PHASE2_STEP3.md). Port of SF6_DIRecolor.lua's battle side: scan_providers, provider_state,
// mark_drive_elements, capture_originals, the baked mesh colours (func/EfxMeshColor.lua locate_element), the armour rim
// (epve_cmn_000_00), relay_container, relay_lines, relay_key, the parts list, and the static path (apply_provider: the
// ColorEnable byte, or the originals back when a side is switched off). The colours come from the Lua (Colors below).
// Game thread only.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vfx::providers {

// what the Lua resolved for one side and one mechanic (side_colors of mech_cfg, plus what relay_lines reads of it)
struct MechColors {
    bool has = false;                            // c1 present (the mechanic is on and its colour resolved)
    uint32_t c1 = 0;
    bool has_c2 = false;
    uint32_t c2 = 0;
    bool follow = false;                         // parts are ignored in Follow
    std::map<std::string, uint32_t> parts;       // mc.parts: "<provider key>:<element>" -> colour
};
// the static path's setting for a side's fighter (get_cfg merged with the reversal's, as the DI tick does)
struct StaticCfg { int fighter = 0; bool enabled = false; bool follow = false; };

// one relay write: W <addr> <val> <orig> [<obj> <ti>]
struct WLine { uint64_t addr; uint32_t val, orig; uint64_t obj = 0, ti = 0; };

void reset();                                                         // forget every provider (state of the Lua module at load)
// every 30 frames (or when dirty): scan DataInstanceMap, then the static path for the fighters on screen.
// statics[0..1] = P1 / P2; side_chars[0..1] = Lua read_side_char; in_fight gates the baked-colour restore writes.
// queue(addr, value) receives the EfxMeshColor.queue writes (baked originals back).
struct QueueFn { void (*fn)(void* ctx, uint64_t addr, uint32_t value); void* ctx; };
int tick(const StaticCfg statics[2], const int side_chars[2], bool dirty, bool in_fight, QueueFn queue);
// tick() spread over frames (one provider's walk took ~2 ms, five in one frame, 28/10 perf logs): tick_start scans and
// lists the providers to set (the same conditions, right after the scan); tick_step sets them in order, each one's
// walk within the deadline (QueryPerformanceCounter ticks), and returns how many it set; tick_busy until the list is done.
void tick_start(const StaticCfg statics[2], const int side_chars[2], bool dirty, bool in_fight);
int tick_step(int64_t deadline, QueueFn queue);
bool tick_busy();
void scan();                                                           // shadow: the providers only, no static writes

// relay builders (SF6_DriveRecolor di_lines / di_container / di_key read these)
bool lines(int side_fighter, const MechColors& mc, std::vector<WLine>& out);   // false = no provider of that fighter
// lines() one step at a time (the parts list, then one provider per call, then the armour rim), so the static pass can
// stop between two steps within its frame budget. Start with a fresh cursor and an empty out, call until it returns
// true; c.present is then what lines() returns. Providers scanned again in between: the cursor starts over (out cleared).
// deadline (QueryPerformanceCounter ticks, 0 = none): a provider's walk stops at the first element past it and goes on
// at the next call (one provider took 2-3 ms in one frame, 24/10 perf logs); 0 = one whole provider per call, as before.
struct SnapJob;
struct LinesCursor { int stage = 0; size_t idx = 0; uint64_t gen = 0, sig = 0; std::string parts; bool present = false; std::shared_ptr<SnapJob> job; };
bool lines_step(int side_fighter, const MechColors& mc, std::vector<WLine>& out, LinesCursor& c, int64_t deadline = 0);
int container(int side_fighter);                                       // the style provider's number, -1 = unknown
// what container() walks when the style providers are new (their extern lists), done ahead within a deadline: call with
// a fresh cursor until it returns true, container(side_fighter) then reads no list. Scanned again: starts over.
bool container_step(int side_fighter, LinesCursor& c, int64_t deadline);
std::string key(int side_fighter, const MechColors& mc);               // changes when a relay rewrite is needed
// parts of a fighter (collect_parts): "key\tname" lines, recomputed when its providers move
std::string parts(int fighter);

std::string status();                                                  // fighters loaded, providers, targets

}  // namespace vfx::providers
