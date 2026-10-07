// vfx::data — implementation. Port of func/DriveColors.lua, SF6_DriveRecolor.lua (HSV math + config),
// SF6_DIRecolor.lua (config + presets). Every function reproduces exact Lua number semantics.
// Phase 2 step 1, docs/CPP_PORT_PHASE2.md. Checked by tools/vfx_harness/run.ps1.

#include "data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace vfx::data {

// ============================================================================
// 1. Drive colour table — generated from func/DriveColors.lua (31 files, 226 colours)
//    gen_module.py originally generated the Lua; here the data is static C++.
// ============================================================================

// clang-format off
static const RGB s_efd_10_drive_000_00[] = { {0,9,26}, {0,19,11}, {0,48,91}, {0,164,112}, {0,200,151}, {0,255,153}, {1,4,26}, {19,217,0}, {220,0,15}, {255,0,0}, {255,21,0} };
static const RGB s_efd_10_drive_000_10[] = { {0,19,11}, {0,37,186}, {0,164,112}, {0,200,151}, {0,255,153}, {7,7,59}, {11,26,226}, {19,217,0}, {22,49,224} };
static const RGB s_efd_10_drive_001_00[] = { {80,37,255}, {94,26,16}, {139,13,4}, {156,15,5}, {167,26,16}, {238,83,83} };
static const RGB s_efd_10_drive_001_01[] = { {255,156,169} };
static const RGB s_efd_10_drive_001_02[] = { {0,200,151}, {19,217,0}, {255,156,169} };
static const RGB s_efd_10_drive_001_04[] = { {0,9,26}, {12,14,23}, {249,33,0}, {255,238,0} };
static const RGB s_efd_10_drive_001_05[] = { {0,0,48}, {0,19,11}, {0,164,112}, {0,255,153}, {12,14,23}, {136,0,0}, {193,6,6}, {255,156,169}, {255,238,0} };
static const RGB s_efd_10_drive_001_90[] = { {255,156,169} };
static const RGB s_efd_10_drive_001_91[] = { {255,156,169} };
static const RGB s_efd_10_drive_010_00[] = { {0,19,11}, {0,88,220}, {0,164,112}, {0,200,151}, {0,255,4}, {9,32,0}, {12,134,0}, {13,255,124}, {18,108,45}, {19,217,0}, {24,123,0}, {32,145,71}, {62,255,53}, {79,119,114}, {97,255,0}, {179,199,255}, {235,255,244} };
static const RGB s_efd_10_drive_010_01[] = { {0,48,91}, {255,156,169} };
static const RGB s_efd_10_drive_010_02[] = { {0,48,91}, {0,200,151}, {19,217,0}, {20,255,120}, {25,134,180}, {87,255,13}, {97,255,3}, {145,217,0}, {255,94,27}, {255,143,49} };
static const RGB s_efd_10_drive_010_03[] = { {0,48,91}, {0,200,151}, {19,217,0}, {20,255,120}, {25,134,180}, {87,255,13}, {97,255,3}, {145,217,0}, {255,94,27}, {255,143,49} };
static const RGB s_efd_10_drive_010_04[] = { {0,48,91} };
static const RGB s_efd_10_drive_010_20[] = { {0,34,255}, {0,78,211}, {0,181,18}, {0,181,172}, {0,255,0}, {4,177,89}, {17,146,47}, {181,255,250}, {255,238,0} };
static const RGB s_efd_10_drive_010_21[] = { {255,156,169} };
static const RGB s_efd_10_drive_010_22[] = { {0,34,255}, {0,78,211}, {0,181,18}, {0,181,172}, {0,255,0}, {4,177,89}, {17,146,47}, {181,255,250}, {255,238,0} };
static const RGB s_efd_10_drive_010_50[] = { {0,48,91}, {0,200,151}, {19,217,0}, {20,255,120}, {25,134,180}, {87,255,13}, {97,255,3}, {145,217,0}, {255,94,27}, {255,143,49} };
static const RGB s_efd_10_parry_000_00[] = { {12,9,48}, {14,9,77}, {19,6,55}, {22,22,133}, {34,49,252}, {40,126,255}, {80,37,255}, {170,0,255} };
static const RGB s_efd_10_parry_000_01[] = { {0,48,91} };
static const RGB s_efd_10_parry_000_03[] = { {0,6,85}, {0,17,255}, {0,49,244}, {0,88,220}, {3,34,96}, {9,15,77}, {19,65,247}, {51,0,255}, {114,0,190}, {189,188,188} };
static const RGB s_efd_10_parry_000_10[] = { {12,9,48}, {14,9,77}, {22,22,133}, {34,49,252}, {40,126,255}, {80,37,255}, {170,0,255} };
static const RGB s_efd_10_parry_000_11[] = { {12,9,48}, {14,9,77}, {19,6,55}, {22,22,133}, {34,49,252}, {40,126,255}, {80,37,255}, {170,0,255} };
static const RGB s_efd_10_parry_001_10[] = { {0,3,13}, {0,22,88}, {0,48,181}, {0,50,199}, {0,54,164}, {1,4,26}, {11,15,59}, {12,3,153}, {232,0,0}, {255,238,0} };
static const RGB s_efd_10_parry_001_11[] = { {5,51,235} };
static const RGB s_efd_10_parry_002_00[] = { {0,3,13}, {0,6,22}, {0,17,255}, {0,25,108}, {0,34,255}, {0,35,255}, {0,48,181}, {0,54,164}, {0,73,178}, {0,73,255}, {0,85,255}, {0,255,153}, {4,0,28}, {18,0,53}, {51,0,255}, {68,0,255}, {255,238,0} };
static const RGB s_efd_10_parry_003_00[] = { {0,1,8}, {0,3,13}, {0,5,71}, {0,8,96}, {0,12,91}, {0,17,255}, {0,23,85}, {0,47,204}, {0,48,181}, {0,59,255}, {0,62,232}, {0,102,255}, {5,15,156}, {9,32,77}, {11,0,159}, {11,15,59}, {15,49,139}, {18,0,53}, {26,72,198}, {37,137,224}, {68,0,255}, {80,37,255}, {255,238,0} };
static const RGB s_efd_10_smoke_500_010_00[] = { {77,79,82}, {108,89,96}, {110,111,93}, {239,251,255} };
static const RGB s_efd_10_smoke_500_021[] = { {77,79,82}, {239,251,255} };
static const RGB s_efd_10_smoke_500_032[] = { {2,2,1}, {42,37,26}, {44,38,29}, {51,42,23}, {53,44,25}, {53,47,34}, {57,49,27}, {77,79,82}, {85,70,43}, {108,89,96}, {110,111,93}, {112,97,55}, {239,251,255}, {255,219,141}, {255,229,183} };
static const RGB s_efd_10_smoke_500_095[] = { {57,51,41}, {75,68,57}, {108,89,96}, {110,111,93}, {191,212,255}, {214,231,255} };
// clang-format on

#define ENTRY(name) { #name ".efx", s_##name, sizeof(s_##name) / sizeof(s_##name[0]) }

const EfxEntry DRIVE_COLORS[] = {
    ENTRY(efd_10_drive_000_00),
    ENTRY(efd_10_drive_000_10),
    ENTRY(efd_10_drive_001_00),
    ENTRY(efd_10_drive_001_01),
    ENTRY(efd_10_drive_001_02),
    ENTRY(efd_10_drive_001_04),
    ENTRY(efd_10_drive_001_05),
    ENTRY(efd_10_drive_001_90),
    ENTRY(efd_10_drive_001_91),
    ENTRY(efd_10_drive_010_00),
    ENTRY(efd_10_drive_010_01),
    ENTRY(efd_10_drive_010_02),
    ENTRY(efd_10_drive_010_03),
    ENTRY(efd_10_drive_010_04),
    ENTRY(efd_10_drive_010_20),
    ENTRY(efd_10_drive_010_21),
    ENTRY(efd_10_drive_010_22),
    ENTRY(efd_10_drive_010_50),
    ENTRY(efd_10_parry_000_00),
    ENTRY(efd_10_parry_000_01),
    ENTRY(efd_10_parry_000_03),
    ENTRY(efd_10_parry_000_10),
    ENTRY(efd_10_parry_000_11),
    ENTRY(efd_10_parry_001_10),
    ENTRY(efd_10_parry_001_11),
    ENTRY(efd_10_parry_002_00),
    ENTRY(efd_10_parry_003_00),
    ENTRY(efd_10_smoke_500_010_00),
    ENTRY(efd_10_smoke_500_021),
    ENTRY(efd_10_smoke_500_032),
    ENTRY(efd_10_smoke_500_095),
};
const size_t DRIVE_COLORS_COUNT = sizeof(DRIVE_COLORS) / sizeof(DRIVE_COLORS[0]);

#undef ENTRY

const EfxEntry* drive_colors_find(std::string_view filename) {
    for (size_t i = 0; i < DRIVE_COLORS_COUNT; ++i)
        if (filename == DRIVE_COLORS[i].filename) return &DRIVE_COLORS[i];
    return nullptr;
}

// ============================================================================
// 2. HSV / tint math
// ============================================================================

// Lua's % (modulo) always returns a non-negative result when the divisor is positive:
//   (-1) % 6 == 5 in Lua, but std::fmod(-1, 6) == -1 in C++.
static double lua_fmod(double a, double b) {
    double r = std::fmod(a, b);
    if (r < 0) r += b;
    return r;
}

// Exact port of SF6_DriveRecolor.lua:119-130
// Takes double to match Lua's loose typing (tint_floats passes fractional values like 191.25).
HSV to_hsv(double r, double g, double b) {
    double rd = r / 255.0, gd = g / 255.0, bd = b / 255.0;
    double mx = std::max({rd, gd, bd});
    double mn = std::min({rd, gd, bd});
    double d  = mx - mn;
    double h  = 0.0;
    if (d > 1e-6) {
        if (mx == rd)      h = 60.0 * lua_fmod((gd - bd) / d, 6.0);
        else if (mx == gd) h = 60.0 * ((bd - rd) / d + 2.0);
        else               h = 60.0 * ((rd - gd) / d + 4.0);
    }
    double s = (mx > 0.0) ? (d / mx) : 0.0;
    return { h, s, mx };
}

// Exact port of SF6_DriveRecolor.lua:131-143
void to_rgb(double h, double s, double v, int& r, int& g, int& b) {
    double c = v * s;
    double x = c * (1.0 - std::fabs(lua_fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;
    double rd = 0, gd = 0, bd = 0;
    if      (h < 60.0)  { rd = c; gd = x; bd = 0; }
    else if (h < 120.0) { rd = x; gd = c; bd = 0; }
    else if (h < 180.0) { rd = 0; gd = c; bd = x; }
    else if (h < 240.0) { rd = 0; gd = x; bd = c; }
    else if (h < 300.0) { rd = x; gd = 0; bd = c; }
    else                { rd = c; gd = 0; bd = x; }
    // math.floor(x + 0.5) rounding — same as Lua
    r = (int)std::floor((rd + m) * 255.0 + 0.5);
    g = (int)std::floor((gd + m) * 255.0 + 0.5);
    b = (int)std::floor((bd + m) * 255.0 + 0.5);
}

// SF6_DriveRecolor.lua:160
uint32_t hue_col(double h) {
    double hm = lua_fmod(h, 360.0);
    int r, g, b;
    to_rgb(hm, 1.0, 1.0, r, g, b);
    return pack(r, g, b);
}

// SF6_DriveRecolor.lua:691-696
uint32_t tint_with(uint32_t col, int r, int g, int b) {
    int cr, cg, cb;
    unpack_col(col, cr, cg, cb);
    auto [h1, s1, v1] = to_hsv(cr, cg, cb);
    auto [h0, s0, v0] = to_hsv(r, g, b);
    (void)h0;
    int nr, ng, nb;
    to_rgb(lua_fmod(h1, 360.0), std::min(1.0, s0 * s1), std::min(1.0, v0 * v1), nr, ng, nb);
    return pack(nr, ng, nb);
}

// SF6_DriveRecolor.lua:691-696 (tint_with called with fractional r/g/b — the body-glow hook's case)
uint32_t tint_with_d(uint32_t col, double r, double g, double b) {
    int cr, cg, cb;
    unpack_col(col, cr, cg, cb);
    auto [h1, s1, v1] = to_hsv(cr, cg, cb);
    auto [h0, s0, v0] = to_hsv(r, g, b);
    (void)h0;
    int nr, ng, nb;
    to_rgb(lua_fmod(h1, 360.0), std::min(1.0, s0 * s1), std::min(1.0, v0 * v1), nr, ng, nb);   // h1 in [0,360): % 360 is a no-op
    return pack(nr, ng, nb);
}

// SF6_DriveRecolor.lua:74,1040-1045
static constexpr double kHueMin = 100.0, kHueMax = 260.0;
bool is_drive_hue(double r, double g, double b) {
    const auto hsv = to_hsv(r, g, b);
    if (hsv.s < 0.35) return false;
    return hsv.h >= kHueMin && hsv.h <= kHueMax;
}

// SF6_DriveRecolor.lua:639-658 ETIQUETTES -> 665-668 ETIQ_INV: "cont/cat/el" -> mechanic. "special"
// ({-1,-1,-1}) never matches a label read from memory (unsigned dwords), so it is left out.
static const std::map<std::string, const char*>& etiq_inv() {
    static const std::map<std::string, const char*> m = {
        { "530/0/0", "parry" }, { "530/0/1", "parry" },
        { "530/0/10", "rush" }, { "530/0/20", "rush" }, { "530/0/21", "rush" }, { "530/0/50", "rush" }, { "530/0/51", "rush" }, { "530/0/61", "rush" },
        { "201/2/2", "impact" }, { "201/2/100", "impact" },
        { "201/2/1", "reversal" },
    };
    return m;
}

// SF6_DriveRecolor.lua:1150-1155 (requestEffect hook)
const char* label_mech(uint32_t cont, uint32_t cat, uint32_t el, int64_t di_cont) {
    char lab[48];
    std::snprintf(lab, sizeof lab, "%u/%u/%u", cont, cat, el);
    const auto& m = etiq_inv();
    auto it = m.find(lab);
    if (it != m.end()) return it->second;
    // the DI container changes with the fighter: (side's container, category 2) + element
    if (cat == 2 && di_cont >= 0 && (int64_t)cont == di_cont) return (el == 1) ? "reversal" : "impact";
    return nullptr;
}

// SF6_DriveRecolor.lua:698-706
void tint_floats(uint32_t col, double r, double g, double b, double& out_r, double& out_g, double& out_b) {
    double m = std::max({r, g, b});
    if (m <= 0.0) { out_r = r; out_g = g; out_b = b; return; }
    auto [h_unused, s0, v0] = to_hsv(r / m * 255.0, g / m * 255.0, b / m * 255.0);
    (void)h_unused;
    if (s0 < 0.08) { out_r = r; out_g = g; out_b = b; return; }
    int cr, cg, cb;
    unpack_col(col, cr, cg, cb);
    auto [h1, s1, v1] = to_hsv(cr, cg, cb);
    int nr, ng, nb;
    to_rgb(lua_fmod(h1, 360.0), std::min(1.0, s0 * s1), std::min(1.0, v0 * v1), nr, ng, nb);
    out_r = nr / 255.0 * m;
    out_g = ng / 255.0 * m;
    out_b = nb / 255.0 * m;
}

// ============================================================================
// 3. DR config parsing (SF6_DriveRecolor.lua:148-199)
// ============================================================================

// Default hue_col(0) = fully saturated red at hue 0
static const uint32_t DR_DEFAULT_C = hue_col(0);

static DRMechCfg dr_mech_default() {
    return { false, false, DR_DEFAULT_C, DR_DEFAULT_C, 0, "" };
}

// SF6_DriveRecolor.lua:168-174 — upgrade a legacy flat mechanic config
static DRMechCfg dr_upgrade(const json& m) {
    if (!m.is_object()) return dr_mech_default();
    DRMechCfg out;
    out.enabled = m.value("enabled", false);
    out.follow  = m.value("follow", false);
    out.selected = m.value("selected", 0);
    if (m.contains("sel") && m["sel"].is_string()) out.sel = m["sel"].get<std::string>();
    if (m.contains("c1")) {
        out.c1 = (uint32_t)m["c1"].get<int64_t>();
    } else {
        double hue = m.value("hue", 0.0);
        out.c1 = hue_col(hue);
    }
    if (m.contains("c2")) {
        out.c2 = (uint32_t)m["c2"].get<int64_t>();
    } else {
        // tint_hue fallback to hue, then 0
        double th = m.contains("tint_hue") ? m["tint_hue"].get<double>() : m.value("hue", 0.0);
        out.c2 = hue_col(th);
    }
    return out;
}

static DRCharCfg dr_char_default() {
    return { dr_mech_default(), dr_mech_default(), dr_mech_default() };
}

// SF6_DriveRecolor.lua:176-183 — migrer
static DRCharCfg dr_migrer(const json& c) {
    if (!c.is_object()) return dr_char_default();
    if (c.contains("parry") || c.contains("rush")) {
        // New format: per-mechanic
        DRCharCfg out;
        out.parry   = c.contains("parry")   ? dr_upgrade(c["parry"])   : dr_mech_default();
        out.rush    = c.contains("rush")     ? dr_upgrade(c["rush"])    : dr_mech_default();
        out.special = c.contains("special")  ? dr_upgrade(c["special"]) : dr_mech_default();
        return out;
    }
    // Legacy flat format: copy to both parry and rush
    json flat = json::object();
    flat["enabled"] = c.value("enabled", false);
    flat["follow"]  = c.value("follow", false);
    flat["hue"]     = c.value("hue", 0.0);
    if (c.contains("tint_hue")) flat["tint_hue"] = c["tint_hue"];
    DRCharCfg out;
    out.parry   = dr_upgrade(flat);
    out.rush    = dr_upgrade(flat);
    out.special = dr_mech_default();
    return out;
}

// SF6_DriveRecolor.lua:186-199
DRFileCfg dr_parse_char(const std::string& text) {
    DRFileCfg result;
    json j;
    try { j = json::parse(text); } catch (...) { result.p1 = dr_char_default(); result.p2 = dr_char_default(); return result; }
    if (!j.is_object() || !j.contains("p1")) { result.p1 = dr_char_default(); result.p2 = dr_char_default(); return result; }
    result.p1 = dr_migrer(j.value("p1", json::object()));
    // p2 defaults to p1 when absent — same as Lua: migrer(c.p2 or c.p1)
    if (j.contains("p2"))
        result.p2 = dr_migrer(j["p2"]);
    else
        result.p2 = dr_migrer(j["p1"]);
    return result;
}

// ============================================================================
// 4. DI config parsing (SF6_DIRecolor.lua:40-103)
// ============================================================================

static DISideCfg::Rev di_parse_rev(const json& r) {
    DISideCfg::Rev out;
    out.enabled  = r.value("enabled", false);
    out.follow   = r.value("follow", false);
    out.c1       = r.contains("c1") ? (uint32_t)r["c1"].get<int64_t>() : 0xFF0000FF;
    out.c2       = r.contains("c2") ? (uint32_t)r["c2"].get<int64_t>() : 0xFFFFFFFF;
    out.selected = r.value("selected", 1);
    if (r.contains("sel") && r["sel"].is_string()) out.sel = r["sel"].get<std::string>();
    return out;
}

static DISideCfg di_parse_side(const json& s) {
    DISideCfg out;
    out.enabled  = s.value("enabled", false);
    out.follow   = s.value("follow", false);
    out.c1       = s.contains("c1") ? (uint32_t)s["c1"].get<int64_t>() : 0xFF0000FF;
    out.c2       = s.contains("c2") ? (uint32_t)s["c2"].get<int64_t>() : 0xFFFFFFFF;
    out.selected = s.value("selected", 1);
    if (s.contains("sel") && s["sel"].is_string()) out.sel = s["sel"].get<std::string>();
    if (s.contains("rev") && s["rev"].is_object()) out.rev = di_parse_rev(s["rev"]);
    return out;
}

DIFileCfg di_parse_char(const std::string& text) {
    DIFileCfg result;
    json j;
    try { j = json::parse(text); } catch (...) { return result; }
    if (!j.is_object()) return result;
    if (j.contains("p1") && j["p1"].is_object()) {
        // Current layout: { p1: {...}, p2: {...} }
        result.p1 = di_parse_side(j["p1"]);
        if (j.contains("p2") && j["p2"].is_object())
            result.p2 = di_parse_side(j["p2"]);
        else
            result.p2 = result.p1;  // copy_cfg(c.p1)
    } else {
        // Flat (legacy) or empty: the whole object is one side's config
        if (j.contains("enabled") || j.contains("c1")) {
            result.p1 = di_parse_side(j);
            result.p2 = result.p1;
        }
        // else: empty JSON -> defaults
    }
    return result;
}

// ---- Presets ----

static Preset parse_preset(const json& p) {
    Preset out;
    if (p.contains("name") && p["name"].is_string()) out.name = p["name"].get<std::string>();
    if (p.contains("c1")) out.c1 = (uint32_t)p["c1"].get<int64_t>();
    if (p.contains("c2")) out.c2 = (uint32_t)p["c2"].get<int64_t>();
    return out;
}

static std::vector<Preset> parse_preset_array(const json& j) {
    std::vector<Preset> out;
    if (!j.is_array()) return out;
    for (auto& e : j) if (e.is_object()) out.push_back(parse_preset(e));
    return out;
}

// SF6_DIRecolor.lua:40-70 — same default families
PresetsCfg di_parse_presets(const std::string& text) {
    PresetsCfg cfg;
    json j;
    try { j = json::parse(text); } catch (...) {}

    if (j.is_object() && j.contains("presets")) cfg.presets = parse_preset_array(j["presets"]);
    if (cfg.presets.empty()) cfg.presets = { { "DI 1", 0xFF0000FF, 0xFFFFFFFF } };

    if (j.is_object() && j.contains("presets_dr")) cfg.presets_dr = parse_preset_array(j["presets_dr"]);
    if (cfg.presets_dr.empty()) cfg.presets_dr = { { "DR 1", 0xFF0000FF, 0xFF0000FF } };

    if (j.is_object() && j.contains("presets_sp")) cfg.presets_sp = parse_preset_array(j["presets_sp"]);
    if (cfg.presets_sp.empty()) {
        cfg.presets_sp = {
            { "SP Red",         0xFF0000FF, 0xFF0000FF },
            { "SP Orange",      0xFF002FFF, 0xFF002FFF },
            { "SP Yellow",      0xFF00BFFF, 0xFF00BFFF },
            { "SP Lemon Green", 0xFF00FF80, 0xFF00FF80 },
            { "SP Green",       0xFF00FF00, 0xFF00FF00 },
            { "SP Cyan",        0xFFFFFF00, 0xFFFFFF00 },
            { "SP Purple",      0xFFFF0080, 0xFFFF0080 },
            { "SP Magenta",     0xFFFF00FF, 0xFFFF00FF },
            { "SP Pink",        0xFF8000FF, 0xFF8000FF },
            { "SP White",       0xFFFFFFFF, 0xFFFFFFFF },
        };
    }
    return cfg;
}

}  // namespace vfx::data
