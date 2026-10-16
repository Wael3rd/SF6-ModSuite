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

// Shared path: decode -> the 4:1 band (compute_crop) -> optional centre cut (thumb) -> resize -> BC1 -> .tex.
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
        if (img_w <= 0 || img_h <= 0) {
            stbi_image_free(pixels);
            err = "decoded image has no pixels";
            return false;
        }

        int crop_y = 0, crop_h = 0;
        compute_crop(img_w, img_h, crop_y, crop_h);
        if (crop_h <= 0 || crop_y < 0 || crop_y + crop_h > img_h) {
            stbi_image_free(pixels);
            err = "crop computation failed";
            return false;
        }

        const int out_w = thumb ? THUMB_W : OUT_W;
        const int out_h = thumb ? THUMB_H : OUT_H;
        const uint8_t* crop_ptr = pixels + (size_t)crop_y * img_w * 4;
        std::vector<uint8_t> centre;   // thumb only: contiguous copy of the band's centre
        int src_w = img_w;
        if (thumb) {
            // Centre of the band with a 1:2 aspect: width = band height / 2.
            src_w = std::max(1, std::min(img_w, crop_h / 2));
            const int x0 = (img_w - src_w) / 2;
            centre.resize((size_t)src_w * crop_h * 4);
            for (int y = 0; y < crop_h; y++)
                memcpy(centre.data() + (size_t)y * src_w * 4, crop_ptr + ((size_t)y * img_w + x0) * 4, (size_t)src_w * 4);
            crop_ptr = centre.data();
        }

        std::vector<uint8_t> resized((size_t)out_w * out_h * 4);
        resize_rgba(crop_ptr, src_w, crop_h, resized.data(), out_w, out_h);
        stbi_image_free(pixels);

        std::vector<uint8_t> bc1;
        encode_bc1(resized.data(), out_w, out_h, bc1);

        const uint32_t pitch = (uint32_t)(out_w / 4) * 8;
        const uint32_t data_size = pitch * (uint32_t)(out_h / 4);
        if (bc1.size() != data_size) {
            err = "internal size mismatch";
            return false;
        }

        write_tex_header(out_tex, out_w, out_h, pitch, data_size);
        out_tex.insert(out_tex.end(), bc1.begin(), bc1.end());
        return true;
    } catch (...) {
        err = "unexpected error building texture";
        out_tex.clear();
        return false;
    }
}

} // namespace

bool make_stage_preview_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err) {
    return build_tex(image_path, false, out_tex, err);
}

bool make_stage_thumb_tex(const std::wstring& image_path, std::vector<uint8_t>& out_tex, std::string& err) {
    return build_tex(image_path, true, out_tex, err);
}
