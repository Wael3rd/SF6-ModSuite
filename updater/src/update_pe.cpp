// SF6 Mod Suite updater: a package's version read from its main DLL's file (update_pe.hpp).
#include "update_pe.hpp"

#include <cstdint>
#include <cstring>

#include "update_fs.hpp"
#include "update_logic.hpp"

namespace sf6up {

namespace {

constexpr const char* kExport = "sf6suite_package_version";

struct Image {
    const std::string& b;
    struct Section { uint32_t va, vsize, raw, rawsize; };
    Section sec[96];
    int nsec = 0;

    bool has(uint64_t off, uint64_t n) const { return off <= b.size() && n <= b.size() - off; }
    bool u16(uint64_t off, uint16_t* v) const { if (!has(off, 2)) return false; std::memcpy(v, b.data() + off, 2); return true; }
    bool u32(uint64_t off, uint32_t* v) const { if (!has(off, 4)) return false; std::memcpy(v, b.data() + off, 4); return true; }
    // The file offset of `n` bytes at a virtual address: inside one section's bytes on disk
    bool at(uint32_t rva, uint32_t n, uint64_t* off) const {
        for (int i = 0; i < nsec; ++i) {
            const Section& s = sec[i];
            if (rva < s.va) continue;
            const uint64_t d = uint64_t(rva) - s.va;
            if (d + n > s.rawsize) continue;
            if (!has(uint64_t(s.raw) + d, n)) return false;
            *off = uint64_t(s.raw) + d;
            return true;
        }
        return false;
    }
};

}  // namespace

std::string image_package_version(const std::string& image) {
    Image im{image};
    uint16_t mz = 0, machine = 0, nsec = 0, optsize = 0, magic = 0;
    uint32_t pe = 0, sig = 0, ndirs = 0, exp_rva = 0;
    if (!im.u16(0, &mz) || mz != 0x5A4D || !im.u32(0x3C, &pe) || !im.u32(pe, &sig) || sig != 0x00004550) return {};
    const uint64_t coff = uint64_t(pe) + 4;
    if (!im.u16(coff, &machine) || machine != 0x8664 || !im.u16(coff + 2, &nsec) || !im.u16(coff + 16, &optsize)) return {};
    const uint64_t opt = coff + 20;
    if (!im.u16(opt, &magic) || magic != 0x20B || optsize < 112 + 8) return {};        // PE32+, with its first data directory
    if (!im.u32(opt + 108, &ndirs) || ndirs < 1 || !im.u32(opt + 112, &exp_rva) || !exp_rva) return {};
    if (nsec == 0 || nsec > 96) return {};
    const uint64_t table = opt + optsize;
    for (int i = 0; i < nsec; ++i) {
        Image::Section& s = im.sec[i];
        const uint64_t row = table + uint64_t(i) * 40;
        if (!im.u32(row + 8, &s.vsize) || !im.u32(row + 12, &s.va) || !im.u32(row + 16, &s.rawsize) || !im.u32(row + 20, &s.raw)) return {};
    }
    im.nsec = nsec;

    // the export directory: the function named kExport
    uint64_t dir = 0;
    uint32_t nfuncs = 0, nnames = 0, funcs = 0, names = 0, ords = 0;
    if (!im.at(exp_rva, 40, &dir)) return {};
    if (!im.u32(dir + 20, &nfuncs) || !im.u32(dir + 24, &nnames) || !im.u32(dir + 28, &funcs) || !im.u32(dir + 32, &names) ||
        !im.u32(dir + 36, &ords))
        return {};
    if (nnames > 4096 || nfuncs > 65536) return {};
    const size_t want = std::strlen(kExport) + 1;
    uint32_t fn = 0;
    for (uint32_t i = 0; i < nnames && !fn; ++i) {
        uint64_t o = 0;
        uint32_t name_rva = 0;
        if (!im.at(names + i * 4, 4, &o) || !im.u32(o, &name_rva)) return {};
        if (!im.at(name_rva, (uint32_t)want, &o) || std::memcmp(image.data() + o, kExport, want) != 0) continue;
        uint16_t ord = 0;
        if (!im.at(ords + i * 2, 2, &o) || !im.u16(o, &ord) || ord >= nfuncs) return {};
        if (!im.at(funcs + uint32_t(ord) * 4, 4, &o) || !im.u32(o, &fn)) return {};
        if (!fn) return {};
    }
    if (!fn) return {};

    // its code: lea rax, [rip + d32] / ret (after one `jmp rel32` at most: a linker's thunk)
    uint64_t code = 0;
    if (!im.at(fn, 8, &code)) return {};
    const unsigned char* c = reinterpret_cast<const unsigned char*>(image.data()) + code;
    if (c[0] == 0xE9) {
        int32_t rel = 0;
        std::memcpy(&rel, c + 1, 4);
        const int64_t to = int64_t(fn) + 5 + rel;
        if (to <= 0 || to > 0x7FFFFFFF) return {};
        fn = uint32_t(to);
        if (!im.at(fn, 8, &code)) return {};
        c = reinterpret_cast<const unsigned char*>(image.data()) + code;
    }
    if (!(c[0] == 0x48 && c[1] == 0x8D && c[2] == 0x05 && c[7] == 0xC3)) return {};
    int32_t disp = 0;
    std::memcpy(&disp, c + 3, 4);
    const int64_t str = int64_t(fn) + 7 + disp;
    if (str <= 0 || str > 0x7FFFFFFF) return {};

    // the string: a version, 32 characters at most
    std::string v;
    for (uint32_t i = 0; i < 33; ++i) {
        uint64_t o = 0;
        if (!im.at(uint32_t(str) + i, 1, &o)) return {};
        const char ch = image[o];
        if (!ch) return parse_version(v) ? v : std::string();
        v.push_back(ch);
    }
    return {};
}

std::string file_package_version(const std::wstring& dll) {
    std::string bytes;
    if (!read_file(dll, &bytes, 64u << 20)) return {};
    return image_package_version(bytes);
}

}  // namespace sf6up
