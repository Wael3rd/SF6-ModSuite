#pragma once

// Turns a stage mod's screenshot (PNG or JPEG) into an SF6 stage-select preview texture
// (.tex.241101895): decode -> crop to 4:1 -> resize to 2048x512 -> BC1 (DXT1) compress -> a
// single-mip .tex whose header is byte-identical to the vanilla stage preview (format
// BC1_UNORM_SRGB, flags, swizzle_control, ...). See preview_tex.cpp for the exact header layout.
//
// SEH-safe: never throws, never lets an exception escape. Runs fine from DllMain (no threads,
// no LoadLibrary, static CRT).

#include <cstdint>
#include <string>
#include <vector>

// image_path: PNG or JPEG file on disk (read with _wfopen, not stb's stdio path, so it works
// with wide/UNC paths under DllMain).
// out_tex: filled with the full .tex file bytes on success.
// err: filled with a short message on failure; untouched on success.
// Returns false on any problem (bad path, decode failure, degenerate image, OOM, ...).
bool make_stage_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err);

// Stage-strip cell thumbnail (128x256 BC1, same header layout): the horizontal centre of the preview band
// (1:2 aspect, width = band height / 2) of the same screenshot.
bool make_stage_thumb_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err);

// A title pack's picture: the whole screenshot at 16:9 (its centre when its aspect differs), 1920x1080, BC1, one mip
// `cell` (not null): the picture also at cell_w x cell_h (RGBA), for the mosaic
bool make_title_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err,
                            std::vector<uint8_t>* cell = nullptr, int cell_w = 0, int cell_h = 0, std::vector<uint8_t>* thumb_tex = nullptr,
                            std::vector<uint8_t>* band_tex = nullptr);
// A title picture's thumbnail (Random's live mosaic and its list's banner, title_preview.cpp): TITLE_THUMB_W x TITLE_THUMB_H, BC1. Half the
// screen: a mosaic place is never larger (one picture alone takes the whole picture), so it is never stretched (Wael 08/11: blurred).
constexpr int TITLE_THUMB_W = 960, TITLE_THUMB_H = 540;
// Its middle 4:1 band at the list's cell size (the stage grid's cells are 252 x 63): TITLE_BAND_W x TITLE_BAND_H, BC1
constexpr int TITLE_BAND_W = 256, TITLE_BAND_H = 64;
// The Random list's check box (64 x 64, BC1): a dark box with a light edge; `on`: the game's yellow tick in it
bool make_check_box_tex(bool on, std::vector<uint8_t>& out_tex);
// The mosaic of `count` pictures (Random's, Wael 08/11): cols x rows places of 16:9, every picture whole. The layout is the one with
// the largest places that fit the screen; between equals, the fewest empty places, then the most columns (5 or 6: 3 x 2, 3: 2 x 2,
// 7 to 9: 3 x 3, 10: 4 x 3). The block is centred on the screen and a last row that holds fewer is centred too. Each picture sits in
// its place with a black gap of TITLE_MOSAIC_GAP round it: its size cell_w x cell_h.
constexpr int TITLE_MOSAIC_GAP = 3;
struct MosaicLayout { int cols = 0, rows = 0, place_w = 0, place_h = 0, cell_w = 0, cell_h = 0; };
MosaicLayout title_mosaic_layout(int count);
// The mosaic's .tex (1920x1080, as a pack's picture) from the cells, in order (left to right, top to bottom)
bool make_title_mosaic_tex(const std::vector<std::vector<uint8_t>>& cells, const MosaicLayout& lay, std::vector<uint8_t>& out_tex, std::string& err);
// A PNG or JPEG whose header gives a size of 16 x 16 or more (only its first 256 KB are read)
bool image_header_ok(const std::wstring& image_path);

// A stage mod's own menu picture (its tex_StageImage / tex_StageThumb, any of BC1/BC2/BC3/BC7/RGBA8/BGRA8, largest mip) made
// into our preview (thumb false: 2048x512) or cell thumbnail (128x256): the whole picture when its aspect is the target's,
// else cut like a screenshot. False (err set) when mip 0 is not in the file (a streaming texture's base file) or the format
// is not one of those.
bool make_stage_tex_from_game_tex(const std::vector<uint8_t>& tex, bool thumb, std::vector<uint8_t>& out_tex, std::string& err);
// The decoding step alone (tests): RGBA8, w x h.
bool decode_game_tex_rgba(const std::vector<uint8_t>& tex, std::vector<uint8_t>& rgba, int& w, int& h, std::string& err);
