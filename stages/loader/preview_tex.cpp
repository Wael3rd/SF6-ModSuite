// Stage-select preview texture builder.
//
// Pipeline: decode PNG/JPEG (stb_image) -> crop to a 4:1 band -> resize to 2048x512 -> BC1
// (DXT1) compress with stb_dxt -> write an SF6 .tex.241101895 file, single mip.
//
// .tex header layout (SF6 / version 241101895, confirmed against the vanilla stage preview
// natives/stm/product/gui/data/area_image/ess/ess0000_00/tex_stageimage_ess0000_00_im.tex.241101895
// extracted from re_chunk_000.pak -- see loader_core.cpp / patch.cpp::texture_mip_check for the
// same layout used elsewhere in this project):
//
//   offset  size  field
//   0       4     magic "TEX\0"
//   4       4     version (int32)            = 241101895
//   8       2     width  (uint16)            = 2048
//   10      2     height (uint16)            = 512
//   12      2     depth  (uint16)             = 1
//   14      1     image_count (uint8)         = 1
//   15      1     mip_header_size (uint8)     = mip_count * 16 = 16
//   16      4     format (int32, DXGI)        = 72 (BC1_UNORM_SRGB)
//   20      4     swizzle_control (int32)     = -1  (matches vanilla)
//   24      4     cubemap_marker (uint32)     = 0
//   28      4     flags (int32)               = 0x800 (matches vanilla ess0000_00 preview)
//   32      1     swizzle_height_depth (uint8)= 0
//   33      1     swizzle_width (uint8)       = 0
//   34      2     null1 (uint16)              = 0
//   36      2     seven (uint16)              = 0
//   38      2     one (uint16)                = 0
//   40      16*n  mip table: { offset:int64, pitch:int32, size:int32 } per (mip_count*image_count)
//                 mip[0] = { offset:56, pitch:4096, size:524288 }  (byte-identical to vanilla)
//   56      524288 mip data: BC1 blocks, row-major, 8 bytes/block, pitch = (width/4)*8 = 4096
//
// Every header field (version, dimensions, image_count, mip_count, format, swizzle_control,
// cubemap_marker, flags, swizzle_*, mip offset/pitch/size) is byte-identical to the vanilla
// ess0000_00 preview: an in-game test showed a white texture and a crash right after setTexture
// when this module wrote an uncompressed RGBA8 texture instead (format/size differed from every
// other stage preview the game has ever loaded). Only the pixel payload differs now.
//
// sRGB: BC1_UNORM_SRGB stores the same bytes as BC1_UNORM; the GPU applies the sRGB->linear
// conversion on sampling. The encoder is fed the screenshot's sRGB-encoded bytes as-is -- they
// are not linearised before compression.

#include "preview_tex.hpp"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec.h"   // common/third_party: decodes a stage mod's own menu pictures (BC1/2/3/7)

#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace {

constexpr int32_t  TEX_VERSION      = 241101895;
constexpr int32_t  TEX_FORMAT_BC1_SRGB = 72;     // DXGI_FORMAT_BC1_UNORM_SRGB
constexpr int32_t  TEX_FLAGS        = 0x800;     // matches vanilla ess0000_00 preview
constexpr int32_t  TEX_SWIZZLE_CTRL = -1;        // matches vanilla
constexpr int      OUT_W = 2048;
constexpr int      OUT_H = 512;                  // OUT_W / 4, matches vanilla exactly
constexpr int      THUMB_W = 128;                // stage-strip cell thumbnail (vanilla tex_StageThumb: 128x256)
constexpr int      THUMB_H = 256;

// ---------------------------------------------------------------------------
// File I/O (wide path, no CRT stdio file handle held by stb_image itself)
// ---------------------------------------------------------------------------

bool read_whole_file(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    bool ok = false;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        long long sz = _ftelli64(f);
        if (sz > 0 && sz < (1ll << 31) && _fseeki64(f, 0, SEEK_SET) == 0) {
            out.resize((size_t)sz);
            ok = fread(out.data(), 1, out.size(), f) == out.size();
        }
    }
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Crop rectangle: full width, band height = width/4, centred at 60% of the image (screenshots keep the fighters in the lower half)
// height, clamped inside the image. If width/4 does not fit inside the image
// height (very wide/panoramic source), the band height is clamped to the image
// height instead of failing -- still a centred crop of the source.
// ---------------------------------------------------------------------------

void compute_crop(int img_w, int img_h, int& crop_y, int& crop_h) {
    int band_h = std::max(1, img_w / 4);
    if (band_h > img_h) band_h = img_h;
    int center_y = (int)std::lround(img_h * 0.60);
    int top = center_y - band_h / 2;
    top = std::max(0, std::min(top, img_h - band_h));
    crop_y = top;
    crop_h = band_h;
}

// ---------------------------------------------------------------------------
// Separable resize, RGBA8, one channel plane at a time. Upscale (dst >= src) uses
// bilinear interpolation; downscale uses a box filter (average over the
// source footprint of each destination pixel) to avoid aliasing.
// ---------------------------------------------------------------------------

// Resizes one channel plane (src_w x src_h, row stride src_w) horizontally to dst_w, writing into
// a temporary plane of dst_w x src_h (row stride dst_w).
void resize_horizontal(const uint8_t* src, int src_w, int h, int stride_in,
                        std::vector<uint8_t>& dst, int dst_w) {
    dst.assign((size_t)dst_w * h, 0);
    const double scale = (double)src_w / dst_w;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = src + (size_t)y * stride_in;
        uint8_t* out_row = dst.data() + (size_t)y * dst_w;
        if (dst_w >= src_w) {
            // Bilinear (upscale or 1:1).
            for (int x = 0; x < dst_w; x++) {
                double sx = (x + 0.5) * scale - 0.5;
                sx = std::max(0.0, std::min(sx, (double)src_w - 1));
                int x0 = (int)std::floor(sx);
                int x1 = std::min(x0 + 1, src_w - 1);
                double t = sx - x0;
                out_row[x] = (uint8_t)std::lround(row[x0] * (1.0 - t) + row[x1] * t);
            }
        } else {
            // Box filter (downscale): average the source footprint of each destination pixel.
            for (int x = 0; x < dst_w; x++) {
                double s0 = x * scale;
                double s1 = (x + 1) * scale;
                int i0 = (int)std::floor(s0);
                int i1 = (int)std::min((double)src_w, std::ceil(s1));
                i1 = std::max(i1, i0 + 1);
                double sum = 0.0, weight = 0.0;
                for (int i = i0; i < i1 && i < src_w; i++) {
                    double lo = std::max((double)i, s0);
                    double hi = std::min((double)i + 1, s1);
                    double w = std::max(0.0, hi - lo);
                    sum += row[i] * w;
                    weight += w;
                }
                out_row[x] = (uint8_t)std::lround(weight > 0 ? sum / weight : row[std::min(i0, src_w - 1)]);
            }
        }
    }
}

// Resizes one channel plane (w x src_h, row stride w) vertically to dst_h.
void resize_vertical(const uint8_t* src, int w, int src_h,
                      std::vector<uint8_t>& dst, int dst_h) {
    dst.assign((size_t)w * dst_h, 0);
    const double scale = (double)src_h / dst_h;
    if (dst_h >= src_h) {
        for (int y = 0; y < dst_h; y++) {
            double sy = (y + 0.5) * scale - 0.5;
            sy = std::max(0.0, std::min(sy, (double)src_h - 1));
            int y0 = (int)std::floor(sy);
            int y1 = std::min(y0 + 1, src_h - 1);
            double t = sy - y0;
            const uint8_t* r0 = src + (size_t)y0 * w;
            const uint8_t* r1 = src + (size_t)y1 * w;
            uint8_t* out_row = dst.data() + (size_t)y * w;
            for (int x = 0; x < w; x++)
                out_row[x] = (uint8_t)std::lround(r0[x] * (1.0 - t) + r1[x] * t);
        }
    } else {
        for (int y = 0; y < dst_h; y++) {
            double s0 = y * scale;
            double s1 = (y + 1) * scale;
            int i0 = (int)std::floor(s0);
            int i1 = (int)std::min((double)src_h, std::ceil(s1));
            i1 = std::max(i1, i0 + 1);
            uint8_t* out_row = dst.data() + (size_t)y * w;
            std::vector<double> sum(w, 0.0);
            double weight = 0.0;
            for (int i = i0; i < i1 && i < src_h; i++) {
                double lo = std::max((double)i, s0);
                double hi = std::min((double)i + 1, s1);
                double wgt = std::max(0.0, hi - lo);
                if (wgt <= 0) continue;
                const uint8_t* row = src + (size_t)i * w;
                for (int x = 0; x < w; x++) sum[x] += row[x] * wgt;
                weight += wgt;
            }
            for (int x = 0; x < w; x++)
                out_row[x] = (uint8_t)std::lround(weight > 0 ? sum[x] / weight : 0.0);
        }
    }
}

// Resizes an RGBA8 image (src_w x src_h) to exactly dst_w x dst_h, writing interleaved RGBA into
// out (already sized dst_w*dst_h*4). Alpha is forced to 255 regardless of source alpha.
void resize_rgba(const uint8_t* src, int src_w, int src_h, uint8_t* out, int dst_w, int dst_h) {
    // De-interleave into per-channel planes (RGB only; alpha is forced to 255 below).
    std::vector<uint8_t> plane_r((size_t)src_w * src_h), plane_g((size_t)src_w * src_h), plane_b((size_t)src_w * src_h);
    for (size_t i = 0, n = (size_t)src_w * src_h; i < n; i++) {
        plane_r[i] = src[i * 4 + 0];
        plane_g[i] = src[i * 4 + 1];
        plane_b[i] = src[i * 4 + 2];
    }

    std::vector<uint8_t> tmp_r, tmp_g, tmp_b;   // src_h rows x dst_w cols, after horizontal pass
    resize_horizontal(plane_r.data(), src_w, src_h, src_w, tmp_r, dst_w);
    resize_horizontal(plane_g.data(), src_w, src_h, src_w, tmp_g, dst_w);
    resize_horizontal(plane_b.data(), src_w, src_h, src_w, tmp_b, dst_w);

    std::vector<uint8_t> out_r, out_g, out_b;   // dst_h rows x dst_w cols
    resize_vertical(tmp_r.data(), dst_w, src_h, out_r, dst_h);
    resize_vertical(tmp_g.data(), dst_w, src_h, out_g, dst_h);
    resize_vertical(tmp_b.data(), dst_w, src_h, out_b, dst_h);

    for (int i = 0; i < dst_w * dst_h; i++) {
        out[i * 4 + 0] = out_r[i];
        out[i * 4 + 1] = out_g[i];
        out[i * 4 + 2] = out_b[i];
        out[i * 4 + 3] = 255;
    }
}

// ---------------------------------------------------------------------------
// BC1 (DXT1) encoding. Input is interleaved RGBA8 (w x h, both multiples of 4); alpha is
// ignored (opaque BC1). Output is one 8-byte block per 4x4 pixel block, row-major, packed with
// no padding (pitch = (w/4)*8), matching the vanilla mip layout exactly.
// ---------------------------------------------------------------------------

void encode_bc1(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out) {
    const int blocks_w = w / 4, blocks_h = h / 4;
    out.resize((size_t)blocks_w * blocks_h * 8);
    uint8_t block[4 * 4 * 4];
    for (int by = 0; by < blocks_h; by++) {
        for (int bx = 0; bx < blocks_w; bx++) {
            for (int row = 0; row < 4; row++) {
                const uint8_t* src = rgba + ((size_t)(by * 4 + row) * w + bx * 4) * 4;
                memcpy(block + row * 16, src, 16);
            }
            uint8_t* dest = out.data() + ((size_t)by * blocks_w + bx) * 8;
            stb_compress_dxt_block(dest, block, /*alpha=*/0, STB_DXT_HIGHQUAL);
        }
    }
}

// ---------------------------------------------------------------------------
// .tex header writer
// ---------------------------------------------------------------------------

template <class T>
void put(std::vector<uint8_t>& buf, size_t off, T v) {
    memcpy(buf.data() + off, &v, sizeof(T));
}

void write_tex_header(std::vector<uint8_t>& out, int w, int h, uint32_t pitch, uint32_t data_size) {
    out.resize(56);   // 40-byte header + one 16-byte mip entry
    memset(out.data(), 0, out.size());
    memcpy(out.data() + 0, "TEX\0", 4);
    put<int32_t>(out, 4, TEX_VERSION);
    put<uint16_t>(out, 8, (uint16_t)w);
    put<uint16_t>(out, 10, (uint16_t)h);
    put<uint16_t>(out, 12, 1);              // depth
    out[14] = 1;                            // image_count
    out[15] = 16;                           // mip_header_size (1 mip * 16)
    put<int32_t>(out, 16, TEX_FORMAT_BC1_SRGB);
    put<int32_t>(out, 20, TEX_SWIZZLE_CTRL);
    put<uint32_t>(out, 24, 0);              // cubemap_marker
    put<int32_t>(out, 28, TEX_FLAGS);
    out[32] = 0;                            // swizzle_height_depth
    out[33] = 0;                            // swizzle_width
    put<uint16_t>(out, 34, 0);              // null1
    put<uint16_t>(out, 36, 0);              // "seven"
    put<uint16_t>(out, 38, 0);              // "one"
    // mip[0]
    put<int64_t>(out, 40, (int64_t)out.size());
    put<int32_t>(out, 48, (int32_t)pitch);
    put<int32_t>(out, 52, (int32_t)data_size);
}

// RGBA (w x h, tightly packed) -> the .tex. thumb: 128x256 cell thumbnail, else the 2048x512 preview. fit: the whole image
// is resized when its aspect is already the target's (within 10 %): a stage mod's own menu picture; otherwise the 4:1 band
// (compute_crop) and, for a thumbnail, its centre with a 1:2 aspect, as for a screenshot.
bool build_from_rgba(const uint8_t* pixels, int img_w, int img_h, bool thumb, bool fit, std::vector<uint8_t>& out_tex,
                     std::string& err) {
    out_tex.clear();
    if (!pixels || img_w <= 0 || img_h <= 0) { err = "image has no pixels"; return false; }
    const int out_w = thumb ? THUMB_W : OUT_W;
    const int out_h = thumb ? THUMB_H : OUT_H;
    const double target = double(out_w) / double(out_h), aspect = double(img_w) / double(img_h);
    const uint8_t* src = pixels;
    int src_w = img_w, src_h = img_h;
    std::vector<uint8_t> centre;   // thumb only: contiguous copy of the band's centre
    if (!(fit && aspect > target * 0.9 && aspect < target * 1.1)) {
        int crop_y = 0, crop_h = 0;
        compute_crop(img_w, img_h, crop_y, crop_h);
        if (crop_h <= 0 || crop_y < 0 || crop_y + crop_h > img_h) { err = "crop computation failed"; return false; }
        src = pixels + (size_t)crop_y * img_w * 4;
        src_h = crop_h;
        if (thumb) {
            // Centre of the band with a 1:2 aspect: width = band height / 2.
            src_w = std::max(1, std::min(img_w, crop_h / 2));
            const int x0 = (img_w - src_w) / 2;
            centre.resize((size_t)src_w * crop_h * 4);
            for (int y = 0; y < crop_h; y++)
                memcpy(centre.data() + (size_t)y * src_w * 4, src + ((size_t)y * img_w + x0) * 4, (size_t)src_w * 4);
            src = centre.data();
        }
    }
    std::vector<uint8_t> resized((size_t)out_w * out_h * 4);
    resize_rgba(src, src_w, src_h, resized.data(), out_w, out_h);

    std::vector<uint8_t> bc1;
    encode_bc1(resized.data(), out_w, out_h, bc1);
    const uint32_t pitch = (uint32_t)(out_w / 4) * 8;
    const uint32_t data_size = pitch * (uint32_t)(out_h / 4);
    if (bc1.size() != data_size) { err = "internal size mismatch"; return false; }
    write_tex_header(out_tex, out_w, out_h, pitch, data_size);
    out_tex.insert(out_tex.end(), bc1.begin(), bc1.end());
    return true;
}

// Shared path for a screenshot: decode -> build_from_rgba (band, no fit).
bool build_tex(const std::wstring& image_path, bool thumb, std::vector<uint8_t>& out_tex, std::string& err) {
    out_tex.clear();
    try {
        std::vector<uint8_t> file_data;
        if (!read_whole_file(image_path, file_data) || file_data.empty()) {
            err = "cannot read image file";
            return false;
        }
        if (file_data.size() > (size_t)INT_MAX) {
            err = "image file too large";
            return false;
        }

        int img_w = 0, img_h = 0, img_n = 0;
        uint8_t* pixels = stbi_load_from_memory(file_data.data(), (int)file_data.size(),
                                                 &img_w, &img_h, &img_n, 4 /* force RGBA */);
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            err = std::string("decode failed: ") + (reason ? reason : "unknown");
            return false;
        }
        const bool ok = build_from_rgba(pixels, img_w, img_h, thumb, false, out_tex, err);
        stbi_image_free(pixels);
        return ok;
    } catch (...) {
        err = "unexpected error building texture";
        out_tex.clear();
        return false;
    }
}

// A title pack's picture (title_slots.hpp): the whole screenshot at 16:9 -- its centre when its aspect differs, cut on the
// longer side -- resized to TITLE_W x TITLE_H (the screen: it is shown full screen), BC1, one mip, the same header as a stage
// preview.
constexpr int TITLE_W = 1920, TITLE_H = 1080;

// RGBA (w x h, multiples of 4) -> the .tex (BC1, one mip)
bool rgba_tex(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out_tex, std::string& err) {
    std::vector<uint8_t> bc1;
    encode_bc1(rgba.data(), w, h, bc1);
    const uint32_t pitch = (uint32_t)(w / 4) * 8;
    const uint32_t data_size = pitch * (uint32_t)(h / 4);
    if (bc1.size() != data_size) { err = "internal size mismatch"; return false; }
    write_tex_header(out_tex, w, h, pitch, data_size);
    out_tex.insert(out_tex.end(), bc1.begin(), bc1.end());
    return true;
}
// The screen's RGBA -> the .tex (BC1, one mip)
bool title_tex_of_screen(const std::vector<uint8_t>& screen, std::vector<uint8_t>& out_tex, std::string& err) {
    return rgba_tex(screen, TITLE_W, TITLE_H, out_tex, err);
}

// `cell` (not null): the picture also at cell_w x cell_h, for the mosaic
bool build_title_from_rgba(const uint8_t* pixels, int img_w, int img_h, std::vector<uint8_t>& out_tex, std::string& err,
                           std::vector<uint8_t>* cell = nullptr, int cell_w = 0, int cell_h = 0, std::vector<uint8_t>* thumb_tex = nullptr,
                           std::vector<uint8_t>* band_tex = nullptr) {
    out_tex.clear();
    if (!pixels || img_w <= 0 || img_h <= 0) { err = "image has no pixels"; return false; }
    int src_w = img_w, src_h = img_h, x0 = 0, y0 = 0;
    if ((long long)img_w * TITLE_H > (long long)img_h * TITLE_W) {          // wider than 16:9: its centre, full height
        src_w = std::max(1, (int)((long long)img_h * TITLE_W / TITLE_H));
        x0 = (img_w - src_w) / 2;
    } else if ((long long)img_w * TITLE_H < (long long)img_h * TITLE_W) {   // taller: its centre, full width
        src_h = std::max(1, (int)((long long)img_w * TITLE_H / TITLE_W));
        y0 = (img_h - src_h) / 2;
    }
    std::vector<uint8_t> crop((size_t)src_w * src_h * 4);
    for (int y = 0; y < src_h; y++)
        memcpy(crop.data() + (size_t)y * src_w * 4, pixels + ((size_t)(y0 + y) * img_w + x0) * 4, (size_t)src_w * 4);
    std::vector<uint8_t> resized((size_t)TITLE_W * TITLE_H * 4);
    resize_rgba(crop.data(), src_w, src_h, resized.data(), TITLE_W, TITLE_H);
    if (cell && cell_w > 0 && cell_h > 0) {
        cell->assign((size_t)cell_w * cell_h * 4, 0);
        resize_rgba(resized.data(), TITLE_W, TITLE_H, cell->data(), cell_w, cell_h);
    }
    if (thumb_tex) {
        std::vector<uint8_t> thumb_px((size_t)TITLE_THUMB_W * TITLE_THUMB_H * 4, 0);
        resize_rgba(resized.data(), TITLE_W, TITLE_H, thumb_px.data(), TITLE_THUMB_W, TITLE_THUMB_H);
        if (!rgba_tex(thumb_px, TITLE_THUMB_W, TITLE_THUMB_H, *thumb_tex, err)) thumb_tex->clear();
    }
    if (band_tex) {                                       // the middle band, 4:1 (rows 300 - 779 of 1080)
        const int band_h = TITLE_W / 4, band_y = (TITLE_H - band_h) / 2;
        std::vector<uint8_t> band_px((size_t)TITLE_BAND_W * TITLE_BAND_H * 4, 0);
        resize_rgba(resized.data() + (size_t)band_y * TITLE_W * 4, TITLE_W, band_h, band_px.data(), TITLE_BAND_W, TITLE_BAND_H);
        if (!rgba_tex(band_px, TITLE_BAND_W, TITLE_BAND_H, *band_tex, err)) band_tex->clear();
    }
    return title_tex_of_screen(resized, out_tex, err);
}

template <class T>
T get_le(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

} // namespace

bool make_stage_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err) {
    return build_tex(image_path, false, out_tex, err);
}

bool make_stage_thumb_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err) {
    return build_tex(image_path, true, out_tex, err);
}

bool make_title_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err,
                            std::vector<uint8_t>* cell, int cell_w, int cell_h, std::vector<uint8_t>* thumb_tex,
                            std::vector<uint8_t>* band_tex) {
    out_tex.clear();
    try {
        std::vector<uint8_t> file_data;
        if (!read_whole_file(image_path, file_data) || file_data.empty()) { err = "cannot read image file"; return false; }
        if (file_data.size() > (size_t)INT_MAX) { err = "image file too large"; return false; }
        int img_w = 0, img_h = 0, img_n = 0;
        uint8_t* pixels = stbi_load_from_memory(file_data.data(), (int)file_data.size(), &img_w, &img_h, &img_n, 4);
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            err = std::string("decode failed: ") + (reason ? reason : "unknown");
            return false;
        }
        const bool ok = build_title_from_rgba(pixels, img_w, img_h, out_tex, err, cell, cell_w, cell_h, thumb_tex, band_tex);
        stbi_image_free(pixels);
        return ok;
    } catch (...) {
        err = "unexpected error building texture";
        out_tex.clear();
        return false;
    }
}

bool make_check_box_tex(bool on, std::vector<uint8_t>& out_tex) {
    constexpr int N = 64;
    std::vector<uint8_t> px((size_t)N * N * 4);
    auto set = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= N || y >= N) return;
        uint8_t* p = px.data() + ((size_t)y * N + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const bool edge = x < 6 || y < 6 || x >= N - 6 || y >= N - 6;
            if (edge) set(x, y, 0xD8, 0xDE, 0xEA);              // the light edge
            else set(x, y, 0x0E, 0x16, 0x2A);                    // the dark inside
        }
    if (on) {
        // the tick: two strokes, (14,33) -> (27,47) and (27,47) -> (52,15), 9 px thick, the game's yellow
        auto dist2 = [](float px_, float py_, float ax, float ay, float bx, float by) {
            const float dx = bx - ax, dy = by - ay;
            float t = ((px_ - ax) * dx + (py_ - ay) * dy) / (dx * dx + dy * dy);
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float qx = ax + t * dx - px_, qy = ay + t * dy - py_;
            return qx * qx + qy * qy;
        };
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const float fx = x + 0.5f, fy = y + 0.5f;
                const float d = std::min(dist2(fx, fy, 14, 33, 27, 47), dist2(fx, fy, 27, 47, 52, 15));
                if (d <= 4.5f * 4.5f) set(x, y, 0xF6, 0xE2, 0x3C);
            }
    }
    std::string err;
    return rgba_tex(px, N, N, out_tex, err);
}

MosaicLayout title_mosaic_layout(int count) {
    MosaicLayout best;
    if (count <= 0) return best;
    for (int cols = 1; cols <= count; ++cols) {
        const int rows = (count + cols - 1) / cols;
        if (cols > 1 && (cols - 1) * rows >= count) continue;   // a column fewer holds them all: that one is tried instead
        // the largest 16:9 place that fits cols x rows times on the screen (its width a multiple of 16: the height exact)
        int w = std::min(TITLE_W / cols, TITLE_H / rows * 16 / 9);
        w -= w % 16;
        if (w < 32) continue;
        const int empty = cols * rows - count;
        const bool better = w > best.place_w || (w == best.place_w && (empty < best.cols * best.rows - count ||
                                                                     (empty == best.cols * best.rows - count && cols > best.cols)));
        if (!best.cols || better) { best.cols = cols; best.rows = rows; best.place_w = w; best.place_h = w * 9 / 16; }
    }
    best.cell_w = std::max(4, best.place_w - 2 * TITLE_MOSAIC_GAP);
    best.cell_h = std::max(4, best.place_h - 2 * TITLE_MOSAIC_GAP);
    return best;
}

bool make_title_mosaic_tex(const std::vector<std::vector<uint8_t>>& cells, const MosaicLayout& lay, std::vector<uint8_t>& out_tex, std::string& err) {
    out_tex.clear();
    if (cells.empty() || lay.cols <= 0 || lay.rows <= 0) { err = "no picture"; return false; }
    std::vector<uint8_t> screen((size_t)TITLE_W * TITLE_H * 4, 0);
    for (size_t i = 0; i < screen.size(); i += 4) screen[i + 3] = 255;   // opaque black
    const int n = (int)std::min(cells.size(), (size_t)(lay.cols * lay.rows));
    const int rows = (n + lay.cols - 1) / lay.cols;                       // the rows used (a picture that could not be made leaves none)
    const int top = (TITLE_H - rows * lay.place_h) / 2;
    for (int i = 0; i < n; ++i) {
        const std::vector<uint8_t>& cell = cells[(size_t)i];
        if (cell.size() != (size_t)lay.cell_w * lay.cell_h * 4) continue;
        const int row = i / lay.cols, col = i % lay.cols;
        const int in_row = std::min(lay.cols, n - row * lay.cols);       // the last row may hold fewer: centred
        const int left = (TITLE_W - in_row * lay.place_w) / 2;
        const int x0 = left + col * lay.place_w + TITLE_MOSAIC_GAP, y0 = top + row * lay.place_h + TITLE_MOSAIC_GAP;
        if (x0 < 0 || y0 < 0) continue;
        for (int y = 0; y < lay.cell_h && y0 + y < TITLE_H; ++y) {
            const int w = std::min(lay.cell_w, TITLE_W - x0);
            if (w > 0) memcpy(screen.data() + ((size_t)(y0 + y) * TITLE_W + x0) * 4, cell.data() + (size_t)y * lay.cell_w * 4, (size_t)w * 4);
        }
    }
    return title_tex_of_screen(screen, out_tex, err);
}

bool image_header_ok(const std::wstring& image_path) {
    FILE* f = _wfopen(image_path.c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> head(256 * 1024);                 // a JPEG's size can come after large EXIF segments
    const size_t got = fread(head.data(), 1, head.size(), f);
    fclose(f);
    int w = 0, h = 0, n = 0;
    return got > 0 && stbi_info_from_memory(head.data(), (int)got, &w, &h, &n) && w >= 16 && h >= 16;
}

bool decode_game_tex_rgba(const std::vector<uint8_t>& tex, std::vector<uint8_t>& rgba, int& w, int& h, std::string& err) {
    rgba.clear();
    w = h = 0;
    try {
        if (tex.size() < 56 || memcmp(tex.data(), "TEX\0", 4) != 0) { err = "not a texture"; return false; }
        const int tw = get_le<uint16_t>(tex, 8), th = get_le<uint16_t>(tex, 10);
        const int images = tex[14], mip_bytes = tex[15];
        const int32_t format = get_le<int32_t>(tex, 16);
        if (tw <= 0 || th <= 0 || tw > 8192 || th > 8192) { err = "bad size"; return false; }
        if (images < 1 || mip_bytes < 16 || 40 + (size_t)mip_bytes * images > tex.size()) { err = "bad mip table"; return false; }
        const int64_t off = get_le<int64_t>(tex, 40);
        const int32_t pitch = get_le<int32_t>(tex, 48), size = get_le<int32_t>(tex, 52);
        int block = 0;                    // bytes per 4x4 block, 0: uncompressed 32-bit
        switch (format) {
        case 71: case 72: block = 8; break;                    // BC1
        case 74: case 75: case 77: case 78: block = 16; break; // BC2, BC3
        case 98: case 99: block = 16; break;                   // BC7
        case 28: case 29: case 87: case 91: block = 0; break;  // R8G8B8A8, B8G8R8A8
        default: { char b[64]; snprintf(b, sizeof b, "unsupported format %d", format); err = b; return false; }
        }
        const int bw = (tw + 3) / 4, bh = (th + 3) / 4;
        const int64_t row = block ? (int64_t)bw * block : (int64_t)tw * 4;
        const int64_t rows = block ? bh : th;
        if (off < 56 || pitch < row || (int64_t)size < (int64_t)pitch * rows || off + (int64_t)pitch * rows > (int64_t)tex.size()) {
            err = "mip 0 not in the file (a streaming texture's header?)";
            return false;
        }
        const int pw = block ? bw * 4 : tw, ph = block ? bh * 4 : th;   // padded to whole blocks
        std::vector<uint8_t> full((size_t)pw * ph * 4);
        const uint8_t* base = tex.data() + off;
        for (int64_t y = 0; y < rows; ++y) {
            const uint8_t* line = base + y * pitch;
            if (!block) {
                uint8_t* dst = full.data() + (size_t)y * pw * 4;
                memcpy(dst, line, (size_t)tw * 4);
                if (format == 87 || format == 91)
                    for (int x = 0; x < tw; ++x) std::swap(dst[x * 4], dst[x * 4 + 2]);
                continue;
            }
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* blk = line + (size_t)bx * block;
                uint8_t* dst = full.data() + ((size_t)(y * 4) * pw + (size_t)bx * 4) * 4;
                switch (format) {
                case 71: case 72: bcdec_bc1(blk, dst, pw * 4); break;
                case 74: case 75: bcdec_bc2(blk, dst, pw * 4); break;
                case 77: case 78: bcdec_bc3(blk, dst, pw * 4); break;
                default: bcdec_bc7(blk, dst, pw * 4); break;
                }
            }
        }
        rgba.resize((size_t)tw * th * 4);
        for (int y = 0; y < th; ++y) memcpy(rgba.data() + (size_t)y * tw * 4, full.data() + (size_t)y * pw * 4, (size_t)tw * 4);
        w = tw;
        h = th;
        return true;
    } catch (...) {
        err = "unexpected error decoding texture";
        rgba.clear();
        return false;
    }
}

bool make_stage_tex_from_game_tex(const std::vector<uint8_t>& tex, bool thumb, std::vector<uint8_t>& out_tex, std::string& err) {
    out_tex.clear();
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!decode_game_tex_rgba(tex, rgba, w, h, err)) return false;
    try {
        return build_from_rgba(rgba.data(), w, h, thumb, true, out_tex, err);
    } catch (...) {
        err = "unexpected error building texture";
        out_tex.clear();
        return false;
    }
}
