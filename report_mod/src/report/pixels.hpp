// report::pixels - a back buffer's pixels to 8-bit BGR for the JPEG encoder, pure (no D3D, no Windows), off the game thread.
// The swap chain's format decides how its numbers read:
//   RGBA8 / BGRA8   already display-referred 8 bits; only the channel order differs
//   RGB10A2         SDR in 10 bits (the low bits dropped)
//   RGB10A2_PQ      HDR10: ST 2084 in BT.2020. To the picture a screenshot means: nits, BT.709, paper white 203 nits = 1.0, a soft
//                   shoulder for what is above it, sRGB. Not what the player's HDR screen showed, but what an SDR page can show of it.
//   RGBA16F         scRGB: linear BT.709, 1.0 = SDR white; the same shoulder, sRGB
// Downscale: to at most 1920 x 1080 keeping the shape, never up, each output pixel the average of the source pixels it covers.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace report::pixels {

enum class Format { Unknown, RGBA8, BGRA8, RGB10A2, RGB10A2_PQ, RGBA16F };
constexpr int kMaxW = 1920, kMaxH = 1080;

inline int bytes_per_pixel(Format f) { return f == Format::RGBA16F ? 8 : f == Format::Unknown ? 0 : 4; }

inline void scaled_size(int w, int h, int* out_w, int* out_h) {
    *out_w = w; *out_h = h;
    if (w <= 0 || h <= 0 || (w <= kMaxW && h <= kMaxH)) return;
    const double k = std::min((double)kMaxW / w, (double)kMaxH / h);
    *out_w = std::clamp((int)std::lround(w * k), 1, kMaxW);
    *out_h = std::clamp((int)std::lround(h * k), 1, kMaxH);
}

inline uint8_t srgb8(float linear) {                  // [0, 1] linear to 8-bit sRGB, by a table
    static const std::array<uint8_t, 4096> t = [] {
        std::array<uint8_t, 4096> a{};
        for (size_t i = 0; i < a.size(); ++i) {
            const double c = (double)i / (a.size() - 1);
            const double v = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
            a[i] = (uint8_t)std::clamp((int)std::lround(v * 255.0), 0, 255);
        }
        return a;
    }();
    return t[(size_t)std::clamp((int)(linear * 4095.0f + 0.5f), 0, 4095)];
}

inline float shoulder(float x) {                      // extended Reinhard: 1.0 stays bright, 4.0 reaches white
    if (!(x > 0)) return 0;
    constexpr float w2 = 4.0f * 4.0f;
    return x * (1.0f + x / w2) / (1.0f + x);
}

inline float pq_nits(uint32_t code10) {               // ST 2084 EOTF of a 10-bit code, by a table, in units of 203 nits
    static const std::array<float, 1024> t = [] {
        std::array<float, 1024> a{};
        constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
        for (size_t i = 0; i < a.size(); ++i) {
            const double e = std::pow((double)i / 1023, 1 / m2);
            const double l = std::pow(std::max(e - c1, 0.0) / (c2 - c3 * e), 1 / m1) * 10000.0;
            a[i] = (float)(l / 203.0);
        }
        return a;
    }();
    return t[code10 & 1023];
}

inline float half_to_float(uint16_t h) {
    const uint32_t s = (uint32_t)(h & 0x8000) << 16;
    const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t bits;
    if (e == 0) {
        if (m == 0) bits = s;
        else {                                         // a subnormal: normalised for the float
            int shift = 0;
            uint32_t mm = m;
            while (!(mm & 0x400)) { mm <<= 1; ++shift; }
            bits = s | ((127 - 15 - shift + 1) << 23) | ((mm & 0x3FF) << 13);
        }
    } else if (e == 31) bits = s | 0x7F800000 | (m << 13);
    else bits = s | ((e + 127 - 15) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// One source pixel at p to b, g, r
template <Format F> inline void read_px(const uint8_t* p, uint32_t* b, uint32_t* g, uint32_t* r) {
    if constexpr (F == Format::RGBA8) { *r = p[0]; *g = p[1]; *b = p[2]; }
    else if constexpr (F == Format::BGRA8) { *b = p[0]; *g = p[1]; *r = p[2]; }
    else if constexpr (F == Format::RGB10A2) {
        uint32_t v; std::memcpy(&v, p, 4);
        *r = (v & 1023) >> 2; *g = ((v >> 10) & 1023) >> 2; *b = ((v >> 20) & 1023) >> 2;
    } else if constexpr (F == Format::RGB10A2_PQ) {
        uint32_t v; std::memcpy(&v, p, 4);
        const float r2 = pq_nits(v & 1023), g2 = pq_nits((v >> 10) & 1023), b2 = pq_nits((v >> 20) & 1023);
        *r = srgb8(shoulder(1.6605f * r2 - 0.5876f * g2 - 0.0728f * b2));
        *g = srgb8(shoulder(-0.1246f * r2 + 1.1329f * g2 - 0.0083f * b2));
        *b = srgb8(shoulder(-0.0182f * r2 - 0.1006f * g2 + 1.1187f * b2));
    } else {
        uint16_t c[3]; std::memcpy(c, p, 6);
        *r = srgb8(shoulder(half_to_float(c[0]))); *g = srgb8(shoulder(half_to_float(c[1]))); *b = srgb8(shoulder(half_to_float(c[2])));
    }
}

template <Format F>
inline void convert_as(const uint8_t* src, size_t pitch, int w, int h, int dw, int dh, uint8_t* out, size_t out_pitch) {
    const int bpp = bytes_per_pixel(F);
    std::vector<int> xs((size_t)dw + 1);
    for (int x = 0; x <= dw; ++x) xs[(size_t)x] = (int)((int64_t)x * w / dw);
    for (int y = 0; y < dh; ++y) {
        const int y0 = (int)((int64_t)y * h / dh);
        const int y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / dh));
        uint8_t* d = out + (size_t)y * out_pitch;
        for (int x = 0; x < dw; ++x) {
            const int x0 = xs[(size_t)x], x1 = std::max(x0 + 1, xs[(size_t)x + 1]);
            uint32_t sb = 0, sg = 0, sr = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* line = src + (size_t)sy * pitch;
                for (int sx = x0; sx < x1; ++sx) {
                    uint32_t b, g, r;
                    read_px<F>(line + (size_t)sx * bpp, &b, &g, &r);
                    sb += b; sg += g; sr += r;
                }
            }
            const uint32_t n = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
            *d++ = (uint8_t)((sb + n / 2) / n);
            *d++ = (uint8_t)((sg + n / 2) / n);
            *d++ = (uint8_t)((sr + n / 2) / n);
        }
    }
}

// src: w x h at `pitch` bytes a row. out: dh rows of out_pitch bytes (3 * dw at least). false: a format not handled.
inline bool to_bgr(const uint8_t* src, size_t pitch, int w, int h, Format f, int dw, int dh, uint8_t* out, size_t out_pitch) {
    if (!src || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return false;
    switch (f) {
        case Format::RGBA8: convert_as<Format::RGBA8>(src, pitch, w, h, dw, dh, out, out_pitch); return true;
        case Format::BGRA8: convert_as<Format::BGRA8>(src, pitch, w, h, dw, dh, out, out_pitch); return true;
        case Format::RGB10A2: convert_as<Format::RGB10A2>(src, pitch, w, h, dw, dh, out, out_pitch); return true;
        case Format::RGB10A2_PQ: convert_as<Format::RGB10A2_PQ>(src, pitch, w, h, dw, dh, out, out_pitch); return true;
        case Format::RGBA16F: convert_as<Format::RGBA16F>(src, pitch, w, h, dw, dh, out, out_pitch); return true;
        default: return false;
    }
}

}  // namespace report::pixels
