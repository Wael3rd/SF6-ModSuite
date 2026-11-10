// SF6 Slots -- a character cell of the game's own grids, as Battle Settings > Other > Opponent Outfit Settings draws it (MatchingSetting
// > p_FighterSelectSimple_cos_h > p_ScrollGrid_0_h > itemNN, read 10/11: probes g91 / g92), made of a screen's own parts. Used by the
// character grids drawn over the game's screens: Options > Interface's portrait packs (ui_packs_grid.cpp) and Replays > Settings'
// Replay Outfit Settings (replay_tab.cpp). Wael 10/11: "the game's page is much better: be much more faithful to it" (the cells were
// plain rectangles, the portrait cut sharp, the texts in the rows' font).
//
// The game's cell, 160 x 72 (places from its top left):
//   c_bg_m          e_scale9grid_shadow (-8, -6) 180 x 88.5625; c_bg: e_scale9grid_mask 160 x 72 (UVS_hub, borders 19 / 24 / 19 / 24),
//                   under it e_mat_bg (mat_Crystal_s, (-50, -46) 500 x 250, F2332B29) and c_chara_mask; e_scale9grid_default 160 x 72,
//                   33FFFFFF
//   c_chara_mask    centred on (120, 36): a 96 x 96 mask (uv_cmn_9grid, its left border 20: the picture's left 20 units fade in) over
//                   e_texture_thumb 96 x 96 (the character's square face picture, characterselect tex_EsfNNN_face_L_s); what shows:
//                   x 72 - 160, y 0 - 72 (the cell's mask)
//   e_text_name     FontSlot 6, 24, white, its left bottom at (11, 74)
//   e_text_on       FontSlot 6, 24, FFFDBF3F (light blue), glow 99D8650A 15 x 15 strength 1, its left top at (10, 9)
//   e_darken        160 x 72, 66000000 over the cell while the grid does not have the cursor
// The focused cell: the cell's SELECT state (light), its name dark (2A2A2A).
//
// Here: the screen's cell shell (a selector row's background panel: Options' c_bg_s, the resident rows' c_bg_m: the same hub parts) at
// the game's sizes, its material the game cell's colour, its state SELECT on the focused cell (as replay_tab.cpp's rows); the portrait
// inside the shell's c_bg (under its rounded mask, over its material), cut in pieces: its left 20 units as 10 strips of 2 whose opacity
// rises from 5 % to 95 % (the game's fading mask; a texture's vertex colours did not fade it, seen 10/11), the rest plain; the texts copies
// of the screen's header title (the title font, slot 6); the darken a copy of a row's e_darken. A battle HUD portrait (512 x 512, the
// portrait packs' pictures) is shown through the crop that puts the face where the game's face picture has it (crop_for). Every write from
// LateUpdateBehavior, through the core (R1); copies found again by name (R35).
#pragma once

#include "slots.hpp"
#include <sf6core/sf6core_api.h>

#include <string>

namespace slots::gamecell {

// A screen's parts a cell is copied from (all needed)
struct Parts {
    void* shell = nullptr;      // a row's background panel (c_bg_s / c_bg_m: e_scale9grid_shadow, c_bg [e_scale9grid_mask, e_mat_bg...],
                                // e_scale9grid_default)
    void* text = nullptr;       // a Text (the screen's header title: the title font)
    void* tex = nullptr;        // a plain Texture
    void* darken = nullptr;     // a row's e_darken (Scale9Grid)
    bool ok() const { return shell && text && tex && darken; }
};

constexpr int kFadeStrips = 10;
enum What { kShell, kPic, kName, kOn, kDarken, kFade0, kWhatCount = kFade0 + kFadeStrips };
// A cell's copy names: "<prefix>g<letter><NN>" (s shell, p its picture and 0 - 9 its fading strips inside the shell's c_bg, n name,
// o On, d darken)
std::string name_of(const std::string& prefix, What w, int slot);
inline What fade(int k) { return static_cast<What>(kFade0 + k); }

// The copies of cell `slot` under `parent` (those missing made; inside a hook where the core may clone: R19). The shell first, the
// darken and the texts after it: drawn over it.
SF6CoreStatus make(void* parent, const Parts& t, const std::string& prefix, int slot);

struct Cell {
    void* c[kWhatCount] = {};
    void* bg = nullptr;         // the shell's c_bg
    void* mat = nullptr;        // its e_mat_bg (may be missing)
    void* get(What w) const { return c[w]; }
    bool all() const { for (void* x : c) if (!x) return false; return bg != nullptr; }
};
Cell find(void* parent, const std::string& prefix, int slot);

// Once per screen: the game's sizes, fonts and colours; the cell's top left at (x, y) of the screen, written as (x - ox, y - oy) in
// the parent's space. prio: the shell's Priority (its pictures inside it), the darken prio + 1, the texts prio + 2.
void lay_out(const Cell& c, float x, float y, float ox, float oy, int64_t prio, const char* on_text);

enum class Look { Dim, Plain, Focus };
// Dim: the darken over the cell, its texts dimmed (the grid has not the cursor); Plain; Focus: the cell light, its name dark.
// Written when it changed (per shell); the cell's dark material written again at every call (the core skips an equal colour):
// call it at every drawing.
void look(const Cell& c, Look l);

// The square part of a picture that stands for the game's face picture (ratios of the picture)
struct Crop { float u0 = 0, v0 = 0, side = 1; };
// A battle HUD portrait (battlehud/tex_battlehud_NNN_*: 512 x 512, the face smaller and higher): the game's face picture is its square
// (0.13, 0.16) side 0.72 (fitted 10/11 on the game's own pictures of Ryu, Luke, Manon, Zangief, Ken, Blanka, E. Honda, C. Viper);
// anything else: the whole picture
Crop crop_for(const std::string& res);
// The portrait (its holder, loaded: ui_texture_ready 1), drawn as the game's. Written when it changed.
void picture(const Cell& c, void* holder, const Crop& crop = Crop{});
// No portrait (none, or not loaded): its pieces hidden
void no_picture(const Cell& c);
// Forgets what was written (a new screen: the copies are new)
void forget();

}  // namespace slots::gamecell
