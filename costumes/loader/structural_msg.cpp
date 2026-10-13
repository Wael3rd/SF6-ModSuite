#include "structural_msg.hpp"
#include <algorithm>
#include <cstring>
#include <map>

// ============================================================================
// Helpers
// ============================================================================

static uint32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    uint32_t v; std::memcpy(&v, d.data() + o, 4); return v;
}
static uint64_t rd64(const std::vector<uint8_t>& d, size_t o) {
    uint64_t v; std::memcpy(&v, d.data() + o, 8); return v;
}
static void wr32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(d.data() + o, &v, 4); }
static void wr64(std::vector<uint8_t>& d, size_t o, uint64_t v) { std::memcpy(d.data() + o, &v, 8); }

static size_t align8(size_t v) { return (v + 7) & ~size_t(7); }

typedef std::u16string U16;

// Strict UTF-8 -> UTF-16 (no overlongs, no surrogates, nothing above U+10FFFF).
static bool utf8_to_utf16(const std::string& s, U16& out) {
    out.clear();
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp; int extra;
        if (c < 0x80)                   { cp = c;        extra = 0; }
        else if (c >= 0xC2 && c < 0xE0) { cp = c & 0x1F; extra = 1; }
        else if (c >= 0xE0 && c < 0xF0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xF0 && c < 0xF5) { cp = c & 0x07; extra = 3; }
        else return false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= n) return false;
            uint8_t cc = static_cast<uint8_t>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return false;
        i += extra + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return true;
}

// "a1b2c3d4-0001-..." -> the 16 bytes of a Windows GUID (first three fields little-endian).
static bool parse_guid(const std::string& g, uint8_t out[16]) {
    uint8_t b[16];
    int nib = 0;
    for (char c : g) {
        if (c == '-') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        if (nib >= 32) return false;
        if (nib & 1) b[nib / 2] |= static_cast<uint8_t>(v);
        else         b[nib / 2] = static_cast<uint8_t>(v << 4);
        ++nib;
    }
    if (nib != 32) return false;
    static const int map[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) out[map[i]] = b[i];
    return true;
}

// Name hash of the engine: murmur3 (seed 0xFFFFFFFF) over the UTF-16LE bytes of the name.
static uint32_t murmur3_u16(const std::u16string& s) {
    std::vector<uint8_t> d;
    for (char16_t c : s) { d.push_back(c & 0xFF); d.push_back(c >> 8); }
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    uint32_t h = 0xFFFFFFFFu;
    size_t n = d.size(), i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t k = d[i] | (d[i + 1] << 8) | (d[i + 2] << 16) | (uint32_t(d[i + 3]) << 24);
        k *= c1; k = rotl(k, 15); k *= c2;
        h ^= k; h = rotl(h, 13); h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (n - i) {
        case 3: k ^= uint32_t(d[i + 2]) << 16; [[fallthrough]];
        case 2: k ^= uint32_t(d[i + 1]) << 8;  [[fallthrough]];
        case 1: k ^= d[i]; k *= c1; k = rotl(k, 15); k *= c2; h ^= k;
    }
    h ^= static_cast<uint32_t>(n);
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}

// The string pool is chained-XOR encrypted: each byte is mixed with the previous
// ciphertext byte and a 16-byte key.
static const uint8_t kKey[16] = {
    0xCF, 0xCE, 0xFB, 0xF8, 0xEC, 0x0A, 0x33, 0x66, 0x93, 0xA9, 0x1D, 0x93, 0x50, 0x39, 0x5F, 0x09};

static void pool_decrypt(uint8_t* p, size_t n) {
    uint8_t prev = 0;
    for (size_t i = 0; i < n; ++i) { uint8_t c = p[i]; p[i] = c ^ prev ^ kKey[i & 15]; prev = c; }
}
static void pool_encrypt(uint8_t* p, size_t n) {
    uint8_t prev = 0;
    for (size_t i = 0; i < n; ++i) { p[i] = p[i] ^ prev ^ kKey[i & 15]; prev = p[i]; }
}

static bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

struct Entry {
    uint8_t guid[16];
    uint32_t sound_id;
    U16 name;
    std::vector<U16> content;   // one per language
};

static U16 lower_ascii(U16 s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char16_t>(c + 32);
    return s;
}

// ============================================================================
// add_msg_entries
// ============================================================================

bool add_msg_entries(std::vector<uint8_t>& msg, const std::vector<MsgEntry>& add, std::string* err) {
    // --- Header (encrypted layout, 72 bytes) -------------------------------
    if (msg.size() < 72) return fail(err, "file too small");
    if (std::memcmp(msg.data() + 4, "GMSG", 4) != 0) return fail(err, "no GMSG magic");
    uint32_t version = rd32(msg, 0);
    // Only the by-hash, encrypted family (REasy: version > 15, not 0x2022033D) is reproduced.
    if (version <= 15 || version == 0x2022033Du) return fail(err, "unsupported msg version");
    uint32_t count = rd32(msg, 16), nparam = rd32(msg, 20), nlang = rd32(msg, 24);
    if (nparam != 0) return fail(err, "user parameters not supported");
    if (nlang > 256 || count > 100000) return fail(err, "implausible counts");
    uint64_t data_off = rd64(msg, 32), lang_off = rd64(msg, 48);
    if (data_off < 72 || data_off > msg.size()) return fail(err, "bad string pool offset");
    if (lang_off < 72 || lang_off + uint64_t(nlang) * 4 > data_off) return fail(err, "bad language table");
    uint64_t esize = 40 + uint64_t(nlang) * 8;
    if (72 + uint64_t(count) * 8 > data_off) return fail(err, "entry table past pool");

    std::vector<uint8_t> pool(msg.begin() + static_cast<ptrdiff_t>(data_off), msg.end());
    pool_decrypt(pool.data(), pool.size());
    auto read_str = [&](uint64_t ptr, U16& out) -> bool {
        out.clear();
        if (ptr == 0) return true;
        if (ptr < data_off) return false;
        for (uint64_t p = ptr - data_off; p + 1 < pool.size(); p += 2) {
            if (pool[p] == 0 && pool[p + 1] == 0) return true;
            out.push_back(static_cast<char16_t>(pool[p] | (pool[p + 1] << 8)));
        }
        return false;   // unterminated
    };

    // --- Existing entries ---------------------------------------------------
    std::vector<Entry> all;
    all.reserve(count + add.size());
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t eo = rd64(msg, 72 + size_t(i) * 8);
        if (eo < 72 || eo + esize > data_off) return fail(err, "entry outside file");
        Entry e;
        std::memcpy(e.guid, msg.data() + eo, 16);
        e.sound_id = rd32(msg, eo + 16);
        if (!read_str(rd64(msg, eo + 24), e.name)) return fail(err, "bad name string");
        e.content.resize(nlang);
        for (uint32_t l = 0; l < nlang; ++l)
            if (!read_str(rd64(msg, eo + 40 + l * 8), e.content[l])) return fail(err, "bad text string");
        all.push_back(std::move(e));
    }

    // --- New entries ----------------------------------------------------------
    for (const MsgEntry& m : add) {
        Entry e;
        e.sound_id = 0;
        if (!parse_guid(m.guid, e.guid)) return fail(err, "bad GUID");
        U16 text;
        if (!utf8_to_utf16(m.name, e.name) || !utf8_to_utf16(m.text, text)) return fail(err, "bad UTF-8");
        e.content.assign(nlang, text);
        all.push_back(std::move(e));
    }
    // Sorting uses ASCII lower-casing; anything else would not match Python's str.lower().
    for (const Entry& e : all)
        for (char16_t c : e.name)
            if (c >= 0x80) return fail(err, "non-ASCII entry name");

    // Sorted by lower-cased name; ties keep file order.
    std::stable_sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
        return lower_ascii(a.name) < lower_ascii(b.name);
    });

    // --- Layout -----------------------------------------------------------------
    size_t n = all.size();
    size_t unknown_off = 72 + n * 8;
    size_t lang_new = unknown_off + 8;
    size_t attr_off = align8(lang_new + size_t(nlang) * 4);   // no attributes: also the name table
    size_t entry_start = attr_off;
    size_t pool_start = align8(entry_start + n * esize);

    // The reference's first pass writes strings (de-duplicated) after a 0x1000-byte scratch area.
    // Those offsets order the final pool and leave a dangling attribute pointer in each entry.
    // Mirror that: key = string, value = offset relative to the pool start.
    std::map<U16, uint64_t> first;
    uint64_t run = 0x1000;
    auto first_off = [&](const U16& s) {
        auto it = first.find(s);
        if (it != first.end()) return it->second;
        uint64_t o = run;
        run += s.size() * 2 + 2;
        first.emplace(s, o);
        return o;
    };

    struct Ref { uint64_t order; const U16* text; size_t pos; };
    std::vector<Ref> refs;
    std::vector<uint8_t> out(pool_start, 0);
    std::memcpy(out.data(), msg.data(), 72);   // header bytes we do not own stay as they were
    wr32(out, 16, static_cast<uint32_t>(n));
    wr32(out, 20, 0);
    wr64(out, 8, 16);
    wr64(out, 32, pool_start);
    wr64(out, 40, unknown_off);
    wr64(out, 48, lang_new);
    wr64(out, 56, attr_off);
    wr64(out, 64, attr_off);
    for (uint32_t l = 0; l < nlang; ++l)
        wr32(out, lang_new + l * 4, rd32(msg, lang_off + l * 4));

    static const U16 kEmpty;
    for (size_t i = 0; i < n; ++i) {
        const Entry& e = all[i];
        size_t eo = entry_start + i * esize;
        wr64(out, 72 + i * 8, eo);
        std::memcpy(out.data() + eo, e.guid, 16);
        wr32(out, eo + 16, e.sound_id);
        wr32(out, eo + 20, e.name.empty() ? 0 : murmur3_u16(e.name));
        refs.push_back({first_off(e.name), &e.name, eo + 24});
        wr64(out, eo + 32, pool_start + first_off(kEmpty));
        for (uint32_t l = 0; l < nlang; ++l)
            refs.push_back({first_off(e.content[l]), &e.content[l], eo + 40 + l * 8});
    }

    // --- String pool: two zero bytes, then one copy per non-empty reference, in first-pass order
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.order < b.order; });
    std::vector<uint8_t> np = {0, 0};
    for (const Ref& r : refs) {
        if (r.text->empty()) { wr64(out, r.pos, pool_start); continue; }
        wr64(out, r.pos, pool_start + np.size());
        for (char16_t c : *r.text) { np.push_back(c & 0xFF); np.push_back(c >> 8); }
        np.push_back(0); np.push_back(0);
    }
    pool_encrypt(np.data(), np.size());
    out.insert(out.end(), np.begin(), np.end());

    msg.swap(out);
    return true;
}
