#pragma once
// The pictures the loader converts from a mod's screenshot (title packs', UI packs'), kept on disk so a picture whose screenshot
// did not change is never decoded and compressed again (pure logic, no game, no Windows: stagetest --selftest). The loader reads
// and writes the files (stage_loader.cpp).
//
// A cache file holds what one conversion gave: the picture's .tex and, when asked for, the mosaic cell (RGBA), the thumbnail and
// the band. Its name is the key: a hash of the cache's namespace, the screenshot's path, its size and last write time, the cell
// size asked for and FORMAT. Any of them changed, another file.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "title_slots.hpp"   // its detail helpers: case, FNV-1a

namespace pic_cache {

// Bump it when the converter's output changes (preview_tex.cpp): every cached picture is then made again
constexpr int FORMAT = 1;

// 16 hex digits. ns: which pictures ("ui", "title", "titlegame"); path: the screenshot's, any case; cell_w / cell_h: the mosaic cell asked
// for (0 for none).
inline std::string key(const std::string& ns, const std::string& path, uint64_t size, uint64_t mtime, int cell_w, int cell_h) {
    const std::string text = ns + "|" + title_slots::detail::lower(path) + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "|" +
                             std::to_string(FORMAT) + "|" + std::to_string(cell_w) + "x" + std::to_string(cell_h);
    char b[17];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)title_slots::detail::fnv1a64(text));
    return b;
}

struct Parts {
    std::vector<uint8_t> tex, cell, thumb, band;
};

// "SF6PIC1\0", then the four sizes (tex, cell, thumb, band) as 32-bit little endian, then the four blocks
inline std::vector<uint8_t> pack(const Parts& p) {
    std::vector<uint8_t> out = { 'S', 'F', '6', 'P', 'I', 'C', '1', 0 };
    for (const std::vector<uint8_t>* v : { &p.tex, &p.cell, &p.thumb, &p.band })
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t((uint64_t(v->size()) >> (8 * i)) & 0xFF));
    for (const std::vector<uint8_t>* v : { &p.tex, &p.cell, &p.thumb, &p.band }) out.insert(out.end(), v->begin(), v->end());
    return out;
}

// false when the bytes are not a cache file of this layout whose sizes add up exactly to its length, or hold no .tex
inline bool unpack(const std::vector<uint8_t>& in, Parts& p) {
    if (in.size() < 24 || std::string(in.begin(), in.begin() + 8) != std::string("SF6PIC1\0", 8)) return false;
    uint64_t n[4], total = 24;
    for (int k = 0; k < 4; ++k) {
        n[k] = 0;
        for (int i = 0; i < 4; ++i) n[k] |= uint64_t(in[8 + 4 * k + i]) << (8 * i);
        total += n[k];
    }
    if (total != in.size() || n[0] == 0) return false;
    size_t at = 24;
    std::vector<uint8_t>* dst[4] = { &p.tex, &p.cell, &p.thumb, &p.band };
    for (int k = 0; k < 4; ++k) {
        dst[k]->assign(in.begin() + at, in.begin() + at + size_t(n[k]));
        at += size_t(n[k]);
    }
    return true;
}

}  // namespace pic_cache
