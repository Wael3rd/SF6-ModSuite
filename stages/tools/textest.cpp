// textest: offline test harness for loader/preview_tex.{hpp,cpp}.
//
//   textest <in.png|in.jpg> <out.tex>     encode a stage screenshot into an SF6 preview .tex
//   textest --thumb <in.png|in.jpg> <out.tex>  same, as the 128x256 stage-strip thumbnail
//   textest --dump <in.tex> <out.png>     decode a .tex back to PNG for a visual check (BC1
//                                         formats 71/72, or uncompressed RGBA8 formats 28/29;
//                                         works on our own output AND on a vanilla .tex, since
//                                         both share the same header/mip layout)
//   textest --cmphdr <a.tex> <b.tex>      byte-compare the 56-byte header + mip[0] entry of two
//                                         .tex files and report any differing offsets
//
// No in-game testing here; this only validates the module offline (header fields, dimensions,
// and a visual round-trip of the pixels). The BC1 decoder (bcdec.h) is used only by this tool,
// never by preview_tex.cpp / the DllMain module.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../loader/preview_tex.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../loader/third_party/stb_image_write.h"

#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec.h"   // common/third_party

namespace {

template <class T>
T get(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

std::wstring widen(const char* s) {
    std::wstring w;
    while (*s) w += (wchar_t)(unsigned char)*s++;
    return w;
}

int do_encode(const char* in_path, const char* out_path, bool thumb) {
    std::vector<uint8_t> tex;
    std::string err;
    if (!(thumb ? make_stage_thumb_tex(widen(in_path), tex, err) : make_stage_preview_tex(widen(in_path), tex, err))) {
        fprintf(stderr, "encode failed: %s\n", err.c_str());
        return 1;
    }
    FILE* f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }
    fwrite(tex.data(), 1, tex.size(), f);
    fclose(f);

    uint16_t w = get<uint16_t>(tex, 8), h = get<uint16_t>(tex, 10);
    uint8_t image_count = tex[14], mip_header_size = tex[15];
    int32_t format = get<int32_t>(tex, 16), flags = get<int32_t>(tex, 28);
    int32_t swizzle = get<int32_t>(tex, 20);
    int64_t mip_off = get<int64_t>(tex, 40);
    int32_t pitch = get<int32_t>(tex, 48), size = get<int32_t>(tex, 52);
    printf("OK: %s (%zu bytes)\n", out_path, tex.size());
    printf("  %ux%u format=%d flags=0x%X swizzle_control=%d image_count=%u mip_count=%u\n",
           w, h, format, flags, swizzle, image_count, mip_header_size / 16);
    printf("  mip[0] offset=%lld pitch=%d size=%d\n", (long long)mip_off, pitch, size);
    return 0;
}

int do_dump(const char* in_path, const char* out_path) {
    FILE* f = fopen(in_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); fprintf(stderr, "read error\n"); return 1; }
    fclose(f);

    if (data.size() < 56 || memcmp(data.data(), "TEX\0", 4) != 0) { fprintf(stderr, "not a TEX file\n"); return 1; }
    int32_t format = get<int32_t>(data, 16);
    uint16_t w = get<uint16_t>(data, 8), h = get<uint16_t>(data, 10);
    int64_t mip_off = get<int64_t>(data, 40);
    int32_t pitch = get<int32_t>(data, 48), size = get<int32_t>(data, 52);
    if (mip_off < 0 || (size_t)mip_off + (size_t)size > data.size()) {
        fprintf(stderr, "inconsistent mip table\n");
        return 1;
    }

    std::vector<uint8_t> rgba((size_t)w * h * 4);

    if (format == 71 || format == 72) {   // BC1_UNORM / BC1_UNORM_SRGB
        const int blocks_w = w / 4, blocks_h = h / 4;
        if (pitch < blocks_w * 8) { fprintf(stderr, "inconsistent BC1 pitch\n"); return 1; }
        const uint8_t* base = data.data() + mip_off;
        for (int by = 0; by < blocks_h; by++) {
            const uint8_t* row = base + (size_t)by * pitch;
            for (int bx = 0; bx < blocks_w; bx++) {
                const uint8_t* block = row + (size_t)bx * 8;
                uint8_t* dst = rgba.data() + ((size_t)(by * 4) * w + bx * 4) * 4;
                bcdec_bc1(block, dst, w * 4);
            }
        }
    } else if (format == 28 || format == 29) {   // R8G8B8A8_UNORM / _SRGB
        if (pitch < w * 4) { fprintf(stderr, "inconsistent pitch\n"); return 1; }
        for (int y = 0; y < h; y++)
            memcpy(rgba.data() + (size_t)y * w * 4, data.data() + mip_off + (size_t)y * pitch, (size_t)w * 4);
    } else {
        fprintf(stderr, "unsupported format %d (this dumper reads BC1 71/72 or RGBA8 28/29)\n", format);
        return 1;
    }

    if (!stbi_write_png(out_path, w, h, 4, rgba.data(), w * 4)) {
        fprintf(stderr, "png write failed\n");
        return 1;
    }
    printf("OK: %s (%ux%u, format=%d, from %s)\n", out_path, w, h, format, in_path);
    return 0;
}

int do_cmphdr(const char* a_path, const char* b_path) {
    auto load = [](const char* p, std::vector<uint8_t>& out) -> bool {
        FILE* f = fopen(p, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        out.resize((size_t)sz);
        bool ok = fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        return ok;
    };
    std::vector<uint8_t> a, b;
    if (!load(a_path, a) || !load(b_path, b)) { fprintf(stderr, "cannot read one of the files\n"); return 1; }
    const size_t n = 56;
    if (a.size() < n || b.size() < n) { fprintf(stderr, "file too small for header+mip[0]\n"); return 1; }
    int diffs = 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            printf("  byte %zu differs: %s=0x%02X %s=0x%02X\n", i, a_path, a[i], b_path, b[i]);
            diffs++;
        }
    }
    printf("total file size: %s=%zu %s=%zu\n", a_path, a.size(), b_path, b.size());
    if (diffs == 0) printf("header+mip[0] (bytes 0..55): IDENTICAL\n");
    else printf("header+mip[0] (bytes 0..55): %d byte(s) differ\n", diffs);
    return diffs == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--dump") == 0) {
        return do_dump(argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "--cmphdr") == 0) {
        return do_cmphdr(argv[2], argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "--thumb") == 0) {
        return do_encode(argv[2], argv[3], true);
    }
    if (argc == 3) {
        return do_encode(argv[1], argv[2], false);
    }
    fprintf(stderr, "usage:\n  textest <in.png|in.jpg> <out.tex>\n  textest --thumb <in.png|in.jpg> <out.tex>\n  textest --dump <in.tex> <out.png>\n  textest --cmphdr <a.tex> <b.tex>\n");
    return 2;
}
