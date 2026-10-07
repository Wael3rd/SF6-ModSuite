// vfx::data — Drive colour tables, HSV/tint math, DR/DI config parsing.
// Phase 2 step 1, docs/CPP_PORT_PHASE2.md. Pure data and functions, no REFramework dependency.
// Checked offline by tools/vfx_harness/run.ps1 against the shipped Lua.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vfx::data {

// ============================================================================
// 1. Drive colour table (port of func/DriveColors.lua: 31 .efx files, 226 RGB triples)
// ============================================================================

struct RGB { uint8_t r, g, b; };

struct EfxEntry {
    const char* filename;           // e.g. "efd_10_drive_000_00.efx"
    const RGB*  colors;
    size_t      count;
};

// 31 entries, same order as DriveColors.lua (alphabetical on filename).
extern const EfxEntry DRIVE_COLORS[];
extern const size_t   DRIVE_COLORS_COUNT;  // 31

// Lookup by filename (linear scan, 31 entries). nullptr when not found.
const EfxEntry* drive_colors_find(std::string_view filename);

// ============================================================================
// 2. HSV / tint math (port of SF6_DriveRecolor.lua lines 119-706)
// ============================================================================

struct HSV { double h, s, v; };

// to_hsv: R,G,B in [0,255] -> H in [0,360), S in [0,1], V in [0,1].
// Exact Lua semantics (SF6_DriveRecolor.lua:119-130).
// Double overload: accepts non-integer values to match Lua's loose typing (tint_floats passes
// fractional R/G/B like 191.25 — truncating to int before dividing by 255 loses precision).
HSV to_hsv(double r, double g, double b);
inline HSV to_hsv(int r, int g, int b) { return to_hsv((double)r, (double)g, (double)b); }

// to_rgb: H in [0,360), S in [0,1], V in [0,1] -> R,G,B in [0,255].
// Uses math.floor(x + 0.5) rounding like the Lua (SF6_DriveRecolor.lua:131-143).
void to_rgb(double h, double s, double v, int& r, int& g, int& b);

// pack: ABGR uint32 with A=255.  (SF6_DriveRecolor.lua:159)
inline uint32_t pack(int r, int g, int b) {
    return (255u << 24) | ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)g << 8) | (uint32_t)(uint8_t)r;
}

// unpack_col: ABGR -> R, G, B.  (SF6_DriveRecolor.lua:161)
inline void unpack_col(uint32_t c, int& r, int& g, int& b) {
    r = (int)(c & 0xFF);
    g = (int)((c >> 8) & 0xFF);
    b = (int)((c >> 16) & 0xFF);
}

// hue_col: hue in degrees -> fully saturated ABGR colour.  (SF6_DriveRecolor.lua:160)
uint32_t hue_col(double h);

// tint_with: replace hue of col with the hue from (r,g,b), multiply S and V.
// col = ABGR packed colour (the "setting"), r/g/b = the origin colour to tint (0-255).
// Returns packed ABGR.  (SF6_DriveRecolor.lua:691-696)
uint32_t tint_with(uint32_t col, int r, int g, int b);

// tint_floats: same as tint_with but for HDR float triples (can exceed 1.0).
// Returns the tinted float triple.  (SF6_DriveRecolor.lua:698-706)
void tint_floats(uint32_t col, double r, double g, double b, double& out_r, double& out_g, double& out_b);

// tint_with_d: same as tint_with, but r/g/b are fractional (0-255 range, not necessarily integral) —
// the body-glow hook calls it with float32 channel values already multiplied by 255.
// (SF6_DriveRecolor.lua tint_with, called by repaint_param with fractional r/g/b)
uint32_t tint_with_d(uint32_t col, double r, double g, double b);

// is_drive_hue: true when (r,g,b) sits in the Drive palette's hue band (saturation >= 0.35,
// hue in [100, 260]). Any common scale for r/g/b: hue and saturation do not depend on it (the body hook passes the
// material's 0-1 floats, like the Lua).  (SF6_DriveRecolor.lua:1040-1045)
bool is_drive_hue(double r, double g, double b);

// label_mech: VfxID (container/category/element) -> mechanic name, or nullptr when none matches.
// di_cont = the side's DI container (-1 = Lua nil / unknown).
// Port of ETIQUETTES/ETIQ_INV (SF6_DriveRecolor.lua:639-668) plus the DI container rule
// (SF6_DriveRecolor.lua:1150-1155).
const char* label_mech(uint32_t cont, uint32_t cat, uint32_t el, int64_t di_cont);

// ============================================================================
// 3. Config parsing: Drive Recolor (SF6_DriveRecolor_data/<Name>.json)
// ============================================================================

// Per-mechanic config (parry, rush, special). Same fields as Lua mech_cfg() + upgrade().
struct DRMechCfg {
    bool     enabled = false;
    bool     follow  = false;
    uint32_t c1      = 0;     // ABGR; default = hue_col(0)
    uint32_t c2      = 0;     // ABGR; default = hue_col(0)
    int      selected = 0;
    std::string sel;
};

// Per-character DR config: { p1: {parry,rush,special}, p2: {parry,rush,special} }
struct DRCharCfg {
    DRMechCfg parry, rush, special;
};

struct DRFileCfg {
    DRCharCfg p1, p2;
};

// Parse a DriveRecolor per-character JSON (with migration from flat/legacy formats).
// text = the JSON file content. Returns the parsed config with Lua-identical defaults.
// (SF6_DriveRecolor.lua:176-199)
DRFileCfg dr_parse_char(const std::string& text);

// ============================================================================
// 4. Config parsing: DI Recolor (SF6_DIRecolor_data/<Name>.json + _presets.json)
// ============================================================================

// Per-side DI config. Fields match the Lua (SF6_DIRecolor.lua:74-103).
struct DISideCfg {
    bool     enabled  = false;
    bool     follow   = false;
    uint32_t c1       = 0xFF0000FF;  // ABGR red
    uint32_t c2       = 0xFFFFFFFF;  // ABGR white
    int      selected = 1;
    std::string sel;

    // Reversal sub-config (mech_cfg in Lua: if mech=="reversal", use cfg.rev)
    struct Rev {
        bool     enabled  = false;
        bool     follow   = false;
        uint32_t c1       = 0xFF0000FF;
        uint32_t c2       = 0xFFFFFFFF;
        int      selected = 1;
        std::string sel;
    };
    std::optional<Rev> rev;  // populated from JSON when present; Lua creates it lazily
};

struct DIFileCfg {
    DISideCfg p1, p2;
};

// Parse a DIRecolor per-character JSON (with Jamie legacy migration).
// text = the JSON file content. Returns the parsed config with Lua-identical defaults.
// (SF6_DIRecolor.lua:78-103)
DIFileCfg di_parse_char(const std::string& text);

// Preset entry (shared across DI, DR, SP families).
struct Preset {
    std::string name;
    uint32_t    c1 = 0xFF0000FF;
    uint32_t    c2 = 0xFFFFFFFF;
};

// The three preset families stored in _presets.json.
struct PresetsCfg {
    std::vector<Preset> presets;     // DI presets
    std::vector<Preset> presets_dr;  // DR presets
    std::vector<Preset> presets_sp;  // SP presets
};

// Parse _presets.json. Applies the same defaults as the Lua when families are empty.
// (SF6_DIRecolor.lua:40-70)
PresetsCfg di_parse_presets(const std::string& text);

// ============================================================================
// 5. Left out (need live memory, will be ported in steps 2-3)
// ============================================================================
// - EfxMeshColor locate logic (resource holder + entry parsing)
// - Spot scanning / persistence (_spots.json, _spots_sig.txt, validation)
// - Provider scanning (DataInstanceMap iteration, mark_drive_elements, capture_originals)
// - Relay building (relay_lines, write_handoff)
// - Body glow hooks

}  // namespace vfx::data
