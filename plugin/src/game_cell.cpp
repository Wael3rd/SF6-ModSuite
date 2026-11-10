// SF6 Slots -- a character cell of the game's own grids, made of a screen's own parts (game_cell.hpp)
#include "game_cell.hpp"
#include "slots_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>

namespace slots::gamecell {
namespace {

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// The game's cell (game_cell.hpp), read 10/11
constexpr float kW = 160, kH = 72;
constexpr float kShadowX = -8, kShadowY = -6, kShadowW = 180, kShadowH = 88.5625f;
constexpr float kMatX = -50, kMatY = -46, kMatW = 500, kMatH = 250;
// the square portrait: x 72 - 168 (cut at 160), y -12 - 84 (its middle 72 shows); its left 20 fading in, as 10 strips of 2
constexpr float kPicX = 72, kPicSide = 96, kFadeW = 20, kStripW = kFadeW / kFadeStrips;
constexpr float kPicV0 = 12.0f / 96.0f, kPicV1 = 84.0f / 96.0f;
constexpr float kNameX = 11, kNameY = 74, kOnX = 10, kOnY = 9, kTextW = 150, kTextH = 30, kFont = 24;
constexpr int64_t kTitleFontSlot = 6;
constexpr uint32_t kColOutline = 0x33FFFFFFu, kColDarken = 0x66000000u, kColClear = 0x00000000u;
// the cell's material: the game's cell is dark, a row's (the shell's template) nearly clear (05FFFFFF); the focused cell's is the
// shell's SELECT state's own (light). The game's cell shows #1F2027 on screen (Wael 10/11; its e_mat_bg's own F2332B29 drew #292B34
// here): that colour, the game's alpha
constexpr uint32_t kColMat = 0xF227201Fu;
constexpr int64_t kPrioInBg = 1;                               // the portrait over the shell's material (all 0 in its c_bg)
constexpr uint32_t kColName = 0xFFFFFFFFu, kColNameDim = 0xFF999999u, kColNameFocus = 0xFF2A2A2Au;
constexpr uint32_t kColOn = 0xFFFDBF3Fu, kColOnDim = 0xFF987226u, kColOnGlow = 0x99D8650Au;
constexpr float kOnGlowBlur = 15, kOnGlowStrength = 1;
constexpr Crop kHudCrop{ 0.13f, 0.16f, 0.72f };

int64_t g_lefttop = 0, g_leftbottom = 8;                       // via.gui.ControlPoint / PageAlignment (read from the game)
void read_enums() {
    static bool done = false;
    if (done) return;
    done = true;
    if (TD* cp = api().tdb()->find_type("via.gui.ControlPoint")) {
        if (auto* f = cp->find_field("LeftTop")) g_lefttop = f->get_data<int32_t>(nullptr);
        if (auto* f = cp->find_field("LeftBottom")) g_leftbottom = f->get_data<int32_t>(nullptr);
    }
}
void set_i(void* c, const char* sig, int64_t v) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_b(void* c, const char* sig, bool v) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_BOOL; a.v.i = v ? 1 : 0;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_f(void* c, const char* sig, double v) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_F64; a.v.f = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_colour(void* c, const char* sig, uint32_t v) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = &v;          // a value type goes by address
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_size(void* c, const char* sig, float w, float h) {
    if (!c) return;
    float v[2] = { w, h };
    SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void put(void* c, float x, float y, float w, float h) {
    if (!c) return;
    A()->ui_set_position(C(), c, x, y, 0);
    A()->ui_set_size(C(), c, w, h);
}
void hide_for_good(void* c) { if (c) A()->ui_set_force_invisible(C(), c, 1); }
void state(void* c, const char* st) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_STR; a.v.s = st;
    A()->obj_invoke(C(), c, "set_PlayState(System.String)", &a, 1, nullptr);
}
// A text of the cell: the title font at the game's size, no shadow; its point (and its alignment in its region) `point`
void style_text(void* t, int64_t point, int64_t prio) {
    if (!t) return;
    set_i(t, "set_ControlPoint(via.gui.ControlPoint)", point);
    set_i(t, "set_PageAlignment(via.gui.PageAlignment)", point);
    set_i(t, "set_AutoRegionFit(via.gui.RegionFitType)", 0);
    set_b(t, "set_AutoWrap(System.Boolean)", false);
    set_i(t, "set_FontSlot(via.gui.FontSlot)", kTitleFontSlot);
    set_b(t, "set_ShadowEnable(System.Boolean)", false);
    set_b(t, "set_GlowEnable(System.Boolean)", false);
    set_i(t, "set_MaskType(via.gui.MaskType)", 0);
    set_i(t, "set_Priority(System.UInt16)", prio);
    A()->ui_set_region_size(C(), t, kTextW, kTextH);
    A()->ui_set_font_size(C(), t, kFont, kFont);
}
// The pieces of the portrait, left to right: the fading strips, then the rest
void* piece(const Cell& c, int i) { return i < kFadeStrips ? c.get(fade(i)) : c.get(kPic); }
constexpr int kPieces = kFadeStrips + 1;

std::map<void*, Look> g_look;                                  // shell -> the look written
std::map<void*, std::tuple<void*, float, float, float>> g_pic; // shell -> the holder and crop written

}  // namespace

std::string name_of(const std::string& prefix, What w, int slot) {
    static const char kLetter[kFade0] = { 's', 'p', 'n', 'o', 'd' };
    const char letter = w < kFade0 ? kLetter[w] : static_cast<char>('0' + (w - kFade0));
    char b[16];
    std::snprintf(b, sizeof b, "g%c%02d", letter, slot);
    return prefix + b;
}

SF6CoreStatus make(void* parent, const Parts& t, const std::string& prefix, int slot) {
    const SF6CoreAPI* a = A();
    if (!parent || !t.ok()) return SF6CORE_ERR_INVALID_ARG;
    auto one = [&](void* under, void* tmpl, What w) -> SF6CoreStatus {
        const std::string nm = name_of(prefix, w, slot);
        if (a->ui_find_child(under, nm.c_str(), 0)) return SF6CORE_OK;
        void* c = nullptr;
        const SF6CoreStatus st = a->ui_clone_control(C(), tmpl, under, nm.c_str(), &c);
        if (st != SF6CORE_OK || !c) return st != SF6CORE_OK ? st : SF6CORE_ERR_INVALID_ARG;
        a->ui_set_force_invisible(C(), c, 1);
        a->ui_set_visible(C(), c, 0);
        return SF6CORE_OK;
    };
    SF6CoreStatus st = one(parent, t.shell, kShell);
    if (st != SF6CORE_OK) return st;
    void* shell = a->ui_find_child(parent, name_of(prefix, kShell, slot).c_str(), 0);
    void* bg = shell ? a->ui_find_child(shell, "c_bg", 1) : nullptr;
    if (!bg) return SF6CORE_ERR_INVALID_ARG;
    // the portrait's pieces inside the shell's c_bg (under its rounded mask); not ForceInvisible: shown with the shell, each shown
    // once it has its picture
    for (int i = 0; i < kPieces; ++i) {
        const What w = i < kFadeStrips ? fade(i) : kPic;
        st = one(bg, t.tex, w);
        if (st != SF6CORE_OK) return st;
        if (void* p = a->ui_find_child(bg, name_of(prefix, w, slot).c_str(), 0)) a->ui_set_force_invisible(C(), p, 0);
    }
    for (const auto& [tmpl, w] : { std::pair<void*, What>{ t.darken, kDarken }, { t.text, kName }, { t.text, kOn } }) {
        st = one(parent, tmpl, w);
        if (st != SF6CORE_OK) return st;
    }
    return SF6CORE_OK;
}

Cell find(void* parent, const std::string& prefix, int slot) {
    const SF6CoreAPI* a = A();
    Cell c;
    if (!parent) return c;
    for (What w : { kShell, kName, kOn, kDarken }) c.c[w] = a->ui_find_child(parent, name_of(prefix, w, slot).c_str(), 0);
    c.bg = c.c[kShell] ? a->ui_find_child(c.c[kShell], "c_bg", 1) : nullptr;
    c.mat = c.bg ? a->ui_find_child(c.bg, "e_mat_bg", 0) : nullptr;
    if (c.bg)
        for (int i = 0; i < kPieces; ++i) {
            const What w = i < kFadeStrips ? fade(i) : kPic;
            c.c[w] = a->ui_find_child(c.bg, name_of(prefix, w, slot).c_str(), 0);
        }
    return c;
}

void lay_out(const Cell& c, float x, float y, float ox, float oy, int64_t prio, const char* on_text) {
    const SF6CoreAPI* a = A();
    SF6CoreClient* cl = C();
    read_enums();
    void* shell = c.get(kShell);
    if (!shell || !c.bg) return;
    // the shell at the game's sizes: shadow, rounded mask (masking what follows in c_bg), material, outline; its marker bar hidden
    set_i(shell, "set_Priority(System.UInt16)", prio);
    a->ui_set_position(cl, shell, x - ox, y - oy, 0);
    put(a->ui_find_child(shell, "e_scale9grid_shadow", 1), kShadowX, kShadowY, kShadowW, kShadowH);
    a->ui_set_position(cl, c.bg, 0, 0, 0);
    put(a->ui_find_child(c.bg, "e_scale9grid_mask", 0), 0, 0, kW, kH);
    put(c.mat, kMatX, kMatY, kMatW, kMatH);
    for (const char* other : { "e_rect_bar", "e_focus_bar", "e_rect_bg", "e_tex_point_bg" }) hide_for_good(a->ui_find_child(c.bg, other, 0));
    if (void* d = a->ui_find_child(shell, "e_scale9grid_default", 1)) { put(d, 0, 0, kW, kH); a->ui_set_color(cl, d, kColOutline); }
    // the portrait's pieces over the material: the strips' opacity rising from 5 % to 95 % (a plain colour each: never a texture's
    // vertex colours), the rest opaque
    for (int i = 0; i < kPieces; ++i) {
        void* p = piece(c, i);
        set_i(p, "set_ControlPoint(via.gui.ControlPoint)", g_lefttop);
        set_i(p, "set_MaskType(via.gui.MaskType)", 0);
        set_i(p, "set_ColorType(via.gui.ColorType)", 0);       // Fill
        set_i(p, "set_Priority(System.UInt16)", kPrioInBg);
        if (i < kFadeStrips) {
            put(p, kPicX + kStripW * static_cast<float>(i), 0, kStripW, kH);
            const uint32_t alpha = static_cast<uint32_t>((static_cast<float>(i) + 0.5f) / kFadeStrips * 255.0f + 0.5f);
            a->ui_set_color(cl, p, (alpha << 24) | 0x00FFFFFFu);
        } else {
            put(p, kPicX + kFadeW, 0, kW - kPicX - kFadeW, kH);
            a->ui_set_color(cl, p, 0xFFFFFFFFu);
        }
    }
    // the darken over the cell (its colour says whether it darkens: look())
    if (void* d = c.get(kDarken)) {
        set_i(d, "set_ControlPoint(via.gui.ControlPoint)", g_lefttop);
        set_i(d, "set_MaskType(via.gui.MaskType)", 0);
        set_i(d, "set_Priority(System.UInt16)", prio + 1);
        put(d, x - ox, y - oy, kW, kH);
        a->ui_set_color(cl, d, kColClear);
    }
    // the texts: the name's left bottom at (11, 74), "On"'s left top at (10, 9), in the title font
    if (void* n = c.get(kName)) {
        style_text(n, g_leftbottom, prio + 2);
        a->ui_set_position(cl, n, x + kNameX - ox, y + kNameY - oy, 0);
    }
    if (void* o = c.get(kOn)) {
        style_text(o, g_lefttop, prio + 2);
        set_b(o, "set_GlowEnable(System.Boolean)", true);
        set_colour(o, "set_GlowColor(via.Color)", kColOnGlow);
        set_size(o, "set_GlowBlurSize(via.Size)", kOnGlowBlur, kOnGlowBlur);
        set_f(o, "set_GlowStrength(System.Single)", kOnGlowStrength);
        a->ui_set_position(cl, o, x + kOnX - ox, y + kOnY - oy, 0);
        a->ui_set_text(cl, o, on_text ? on_text : "");
        a->ui_set_color(cl, o, kColOn);
    }
    g_look.erase(shell);
    g_pic.erase(shell);
}

void look(const Cell& c, Look l) {
    void* shell = c.get(kShell);
    if (!shell) return;
    // the material's colour: written at every look (the core writes it only when it differs; the state's own animation may set it)
    if (l != Look::Focus && c.mat) A()->ui_set_color(C(), c.mat, kColMat);
    auto it = g_look.find(shell);
    if (it != g_look.end() && it->second == l) return;
    g_look[shell] = l;
    const char* st = l == Look::Focus ? "SELECT" : "DEFAULT";
    state(shell, st);
    state(c.bg, st);
    A()->ui_set_color(C(), c.get(kDarken), l == Look::Dim ? kColDarken : kColClear);
    A()->ui_set_color(C(), c.get(kName), l == Look::Focus ? kColNameFocus : l == Look::Dim ? kColNameDim : kColName);
    A()->ui_set_color(C(), c.get(kOn), l == Look::Dim ? kColOnDim : kColOn);
}

Crop crop_for(const std::string& res) {
    std::string low = res;
    std::transform(low.begin(), low.end(), low.begin(), [](char ch) { return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); });
    return low.find("battlehud/tex_battlehud_") != std::string::npos ? kHudCrop : Crop{};
}

void picture(const Cell& c, void* holder, const Crop& crop) {
    void* shell = c.get(kShell);
    if (!shell || !holder) return;
    const auto key = std::make_tuple(holder, crop.u0, crop.v0, crop.side);
    auto it = g_pic.find(shell);
    if (it != g_pic.end() && it->second == key) return;
    // the square's x from 0 to 88 of 96 shows (x 72 - 160 of the cell), y from 12 to 84; in the picture: crop's square
    const float v = crop.v0 + kPicV0 * crop.side, vh = (kPicV1 - kPicV0) * crop.side;
    for (int i = 0; i < kPieces; ++i) {
        void* p = piece(c, i);
        if (!p) return;
        const float x0 = i < kFadeStrips ? kStripW * static_cast<float>(i) : kFadeW;
        const float x1 = i < kFadeStrips ? x0 + kStripW : kW - kPicX;
        if (A()->ui_set_texture_rect(C(), p, holder, crop.u0 + x0 / kPicSide * crop.side, v, (x1 - x0) / kPicSide * crop.side, vh) != SF6CORE_OK)
            return;
    }
    for (int i = 0; i < kPieces; ++i) A()->ui_set_visible(C(), piece(c, i), 1);
    g_pic[shell] = key;
}

void no_picture(const Cell& c) {
    for (int i = 0; i < kPieces; ++i) if (void* p = piece(c, i)) A()->ui_set_visible(C(), p, 0);
    g_pic.erase(c.get(kShell));
}

void forget() {
    g_look.clear();
    g_pic.clear();
}

}  // namespace slots::gamecell
