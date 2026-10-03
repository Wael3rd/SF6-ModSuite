#include "pak.hpp"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>

// -- Third-party forward declarations (avoids including huge headers) ---------

// zstd (zstddeclib.c -- decode only)
extern "C" {
size_t ZSTD_decompress(void* dst, size_t dstCapacity,
                       const void* src, size_t compressedSize);
unsigned    ZSTD_isError(size_t code);
const char* ZSTD_getErrorName(size_t code);
}

// miniz (miniz.h / miniz.c)
#include "miniz.h"

// ============================================================================
// Murmur3  (x86-32, seed 0xFFFFFFFF)
// ============================================================================

static inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static uint32_t murmur3_32(const void* data, size_t len, uint32_t seed) {
    auto p = static_cast<const uint8_t*>(data);
    const uint32_t c1 = 0xCC9E2D51;
    const uint32_t c2 = 0x1B873593;
    uint32_t h1 = seed;

    size_t nblocks = len / 4;
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t k1;
        std::memcpy(&k1, p + i * 4, 4);           // little-endian on x86
        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xE6546B64;
    }

    const uint8_t* tail = p + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= uint32_t(tail[2]) << 16; [[fallthrough]];
        case 2: k1 ^= uint32_t(tail[1]) << 8;  [[fallthrough]];
        case 1: k1 ^= uint32_t(tail[0]);
                k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
                h1 ^= k1;
    }

    h1 ^= uint32_t(len);
    h1 ^= h1 >> 16; h1 *= 0x85EBCA6B;
    h1 ^= h1 >> 13; h1 *= 0xC2B2AE35;
    h1 ^= h1 >> 16;
    return h1;
}

// ============================================================================
// pak_path_hash
// ============================================================================

static std::wstring normalize_path(std::wstring_view path) {
    // Trim whitespace
    size_t s = 0, e = path.size();
    while (s < e && (path[s] == L' ' || path[s] == L'\t' ||
                     path[s] == L'\r' || path[s] == L'\n'))
        ++s;
    while (e > s && (path[e-1] == L' ' || path[e-1] == L'\t' ||
                     path[e-1] == L'\r' || path[e-1] == L'\n'))
        --e;

    std::wstring p(path.substr(s, e - s));

    // Backslash -> slash
    for (auto& c : p) if (c == L'\\') c = L'/';

    // Collapse double slashes
    std::wstring out;
    out.reserve(p.size());
    bool prev = false;
    for (wchar_t c : p) {
        if (c == L'/') {
            if (!prev) out += c;
            prev = true;
        } else {
            out += c;
            prev = false;
        }
    }
    return out;
}

uint64_t pak_path_hash(std::wstring_view path) {
    std::wstring p = normalize_path(path);

    // Lowercase copy
    std::wstring lo = p;
    for (auto& c : lo) c = towlower(c);
    uint32_t h_lo = murmur3_32(lo.data(), lo.size() * sizeof(wchar_t), 0xFFFFFFFF);

    // Uppercase copy
    std::wstring hi = p;
    for (auto& c : hi) c = towupper(c);
    uint32_t h_hi = murmur3_32(hi.data(), hi.size() * sizeof(wchar_t), 0xFFFFFFFF);

    return (uint64_t(h_hi) << 32) | h_lo;
}

uint64_t pak_path_hash(std::string_view path) {
    // For ASCII paths: widen byte-by-byte
    std::wstring wide;
    wide.reserve(path.size());
    for (char c : path) wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return pak_path_hash(std::wstring_view(wide));
}

// ============================================================================
// PakReader
// ============================================================================

static constexpr uint32_t PAK_MAGIC  = 0x414B504B;  // 'KPKA'
static constexpr int      ENTRY_SIZE = 48;

#pragma pack(push, 1)
struct PakHeader {
    uint32_t magic;
    uint8_t  major;
    uint8_t  minor;
    int16_t  features;
    uint32_t count;
    uint32_t fingerprint;
};
static_assert(sizeof(PakHeader) == 16);

struct PakEntryDisk {
    uint32_t hash_lower;
    uint32_t hash_upper;
    int64_t  offset;
    int64_t  compressed_size;
    int64_t  decompressed_size;
    int64_t  attributes;
    int64_t  checksum;
};
static_assert(sizeof(PakEntryDisk) == 48);
#pragma pack(pop)

PakReader::~PakReader() { close(); }

bool PakReader::open(const char* path) {
    close();
    fp_ = std::fopen(path, "rb");
    if (!fp_) return false;

    PakHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), fp_) != sizeof(hdr)) {
        close(); return false;
    }
    if (hdr.magic != PAK_MAGIC || hdr.major != 4) {
        close(); return false;
    }

    // Read full entry table in one shot
    std::vector<PakEntryDisk> table(hdr.count);
    size_t table_bytes = size_t(hdr.count) * sizeof(PakEntryDisk);
    if (std::fread(table.data(), 1, table_bytes, fp_) != table_bytes) {
        close(); return false;
    }

    entries_.reserve(hdr.count + hdr.count / 4); // ~25% slack for unordered_map
    for (uint32_t i = 0; i < hdr.count; i++) {
        auto& d = table[i];
        PakEntry e;
        e.hash_lower       = d.hash_lower;
        e.hash_upper       = d.hash_upper;
        e.offset            = d.offset;
        e.compressed_size   = d.compressed_size;
        e.decompressed_size = d.decompressed_size;
        e.attributes        = d.attributes;
        e.checksum          = d.checksum;
        entries_[e.combined_hash()] = e;
    }
    return true;
}

void PakReader::close() {
    if (fp_) { std::fclose(fp_); fp_ = nullptr; }
    entries_.clear();
}

const PakEntry* PakReader::find(uint64_t hash) const {
    auto it = entries_.find(hash);
    if (it != entries_.end()) return &it->second;
    for (auto* o : over_)
        if (auto* e = o->find(hash)) return e;
    return nullptr;
}

PakReader* PakReader::owner_of(const PakEntry& e) {
    for (auto* o : over_) {
        auto it = o->entries_.find(e.combined_hash());
        if (it != o->entries_.end() && &it->second == &e) return o;
    }
    return nullptr;
}

std::vector<uint8_t> PakReader::read_raw(const PakEntry& e) {
    if (auto* o = owner_of(e)) return o->read_raw(e);
    if (!fp_) return {};
    std::vector<uint8_t> buf(size_t(e.compressed_size));
    _fseeki64(fp_, e.offset, SEEK_SET);
    std::fread(buf.data(), 1, buf.size(), fp_);
    return buf;
}

std::vector<uint8_t> PakReader::read(const PakEntry& e) {
    return pak_decompress(read_raw(e), e.compression(), e.decompressed_size);
}

std::vector<uint8_t> pak_decompress(std::vector<uint8_t> raw, int comp, int64_t decompressed_size) {
    // Uncompressed
    if (comp == 0 || int64_t(raw.size()) == decompressed_size) {
        return raw;
    }

    std::vector<uint8_t> out(static_cast<size_t>(decompressed_size));

    // zstd (compression type 2, 0x82, etc.)
    if (comp == 2 || (comp & 0xF) == 2) {
        size_t ret = ZSTD_decompress(out.data(), out.size(),
                                     raw.data(), raw.size());
        if (ZSTD_isError(ret))
            throw std::runtime_error(std::string("zstd: ") +
                                     ZSTD_getErrorName(ret));
        out.resize(ret);
        return out;
    }

    // deflate (compression type 1, 0x81, etc.)
    if (comp == 1 || (comp & 0xF) == 1) {
        // Try raw deflate first (no zlib header)
        mz_stream strm{};
        strm.next_in   = raw.data();
        strm.avail_in  = static_cast<unsigned int>(raw.size());
        strm.next_out  = out.data();
        strm.avail_out = static_cast<unsigned int>(out.size());

        int rc = mz_inflateInit2(&strm, -MZ_DEFAULT_WINDOW_BITS);
        if (rc == MZ_OK) {
            rc = mz_inflate(&strm, MZ_FINISH);
            mz_inflateEnd(&strm);
        }

        if (rc == MZ_STREAM_END) {
            out.resize(strm.total_out);
            return out;
        }

        // Fallback: zlib format (with header)
        strm = {};
        strm.next_in   = raw.data();
        strm.avail_in  = static_cast<unsigned int>(raw.size());
        strm.next_out  = out.data();
        strm.avail_out = static_cast<unsigned int>(out.size());

        rc = mz_inflateInit(&strm);
        if (rc == MZ_OK) {
            rc = mz_inflate(&strm, MZ_FINISH);
            mz_inflateEnd(&strm);
        }

        if (rc == MZ_STREAM_END) {
            out.resize(strm.total_out);
            return out;
        }

        throw std::runtime_error("deflate decompression failed");
    }

    // Unknown compression -- try zstd as fallback
    {
        size_t ret = ZSTD_decompress(out.data(), out.size(),
                                     raw.data(), raw.size());
        if (!ZSTD_isError(ret)) {
            out.resize(ret);
            return out;
        }
    }

    // Last resort: return raw
    return raw;
}

// ============================================================================
// PakWriter
// ============================================================================

// Cheap content signature: size, attributes and sampled bytes. Two blobs with the same signature
// are then compared in full before being shared, so a collision can never merge different data.
static uint64_t blob_signature(const std::vector<uint8_t>& b, int64_t attrib) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint8_t c) { h ^= c; h *= 1099511628211ULL; };
    uint64_t n = b.size();
    for (int i = 0; i < 8; ++i) mix(uint8_t(n >> (8 * i)));
    mix(uint8_t(attrib));
    size_t head = std::min<size_t>(b.size(), 4096);
    for (size_t i = 0; i < head; ++i) mix(b[i]);
    size_t tail = std::min<size_t>(b.size(), 4096);
    for (size_t i = b.size() - tail; i < b.size(); ++i) mix(b[i]);
    if (b.size() > 8192)
        for (size_t k = 1; k < 64; ++k) mix(b[(b.size() / 64) * k]);
    return h;
}

void PakWriter::push(RawEntry e) {
    by_hash_[e.hash] = entries_.size();
    uint64_t sig = blob_signature(e.blob, e.attributes);
    auto& same = by_sig_[sig];
    for (size_t idx : same) {
        const RawEntry& o = entries_[idx];
        if (o.blob.size() == e.blob.size() && o.attributes == e.attributes
            && o.decompressed_size == e.decompressed_size
            && std::memcmp(o.blob.data(), e.blob.data(), e.blob.size()) == 0) {
            dedup_count_++;
            dedup_bytes_ += e.blob.size();
            e.alias = int64_t(idx);
            e.blob.clear();
            e.blob.shrink_to_fit();
            entries_.push_back(std::move(e));
            return;
        }
    }
    same.push_back(entries_.size());
    entries_.push_back(std::move(e));
}

const PakWriter::RawEntry* PakWriter::find(uint64_t hash) const {
    auto it = by_hash_.find(hash);
    return it == by_hash_.end() ? nullptr : &entries_[it->second];
}

std::vector<uint8_t> PakWriter::get(uint64_t hash) const {
    const RawEntry* e = find(hash);
    if (!e) return {};
    const RawEntry& src = (e->alias >= 0) ? entries_[size_t(e->alias)] : *e;
    return pak_decompress(src.blob, int(e->attributes & 0xFF), e->decompressed_size);
}

const std::vector<uint8_t>* PakWriter::peek(uint64_t hash) const {
    const RawEntry* e = find(hash);
    if (!e || (e->attributes & 0xFF) != 0) return nullptr;
    return (e->alias >= 0) ? &entries_[size_t(e->alias)].blob : &e->blob;
}

bool PakWriter::replace(uint64_t hash, std::vector<uint8_t> data) {
    auto it = by_hash_.find(hash);
    if (it == by_hash_.end()) return false;
    const size_t i = it->second;
    RawEntry& e = entries_[i];
    if (e.alias < 0) {
        // Other entries may share this entry's data: the first of them takes it over
        uint64_t sig = blob_signature(e.blob, e.attributes);
        auto& same = by_sig_[sig];
        same.erase(std::remove(same.begin(), same.end(), i), same.end());
        int64_t heir = -1;
        for (size_t j = 0; j < entries_.size(); ++j) {
            if (entries_[j].alias != int64_t(i)) continue;
            if (heir < 0) {
                heir = int64_t(j);
                entries_[j].blob = std::move(e.blob);
                entries_[j].alias = -1;
                same.push_back(j);
            } else {
                entries_[j].alias = heir;
            }
        }
    }
    e.blob = std::move(data);
    e.attributes = 0;
    e.decompressed_size = int64_t(e.blob.size());
    e.alias = -1;
    return true;
}

void PakWriter::add_raw(uint64_t hash, std::vector<uint8_t> blob,
                        int64_t attrib, int64_t decompressed_size) {
    RawEntry e;
    e.hash = hash; e.blob = std::move(blob); e.attributes = attrib;
    e.decompressed_size = decompressed_size;
    push(std::move(e));
}

void PakWriter::add_uncompressed(uint64_t hash, std::vector<uint8_t> data) {
    RawEntry e;
    e.hash = hash; e.attributes = 0; e.decompressed_size = int64_t(data.size());
    e.blob = std::move(data);
    push(std::move(e));
}

bool PakWriter::write(const char* path) {
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;

    uint32_t n = uint32_t(entries_.size());

    // Header
    PakHeader hdr{};
    hdr.magic       = PAK_MAGIC;
    hdr.major       = 4;
    hdr.minor       = 0;
    hdr.features    = 0;
    hdr.count       = n;
    hdr.fingerprint = 0;
    bool ok = std::fwrite(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr);

    // Compute entry table (offsets start after header + table)
    int64_t data_start = int64_t(sizeof(PakHeader)) + int64_t(n) * ENTRY_SIZE;
    int64_t offset = data_start;

    // Offsets of the entries that own their data; aliases reuse their source's offset and size.
    std::vector<int64_t> offs(entries_.size(), 0);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].alias >= 0) continue;
        offs[i] = offset;
        offset += int64_t(entries_[i].blob.size());
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
        const RawEntry& re = entries_[i];
        const RawEntry& src = (re.alias >= 0) ? entries_[size_t(re.alias)] : re;
        PakEntryDisk d{};
        d.hash_lower       = uint32_t(re.hash & 0xFFFFFFFF);
        d.hash_upper       = uint32_t(re.hash >> 32);
        d.offset            = (re.alias >= 0) ? offs[size_t(re.alias)] : offs[i];
        d.compressed_size   = int64_t(src.blob.size());
        d.decompressed_size = re.decompressed_size;
        d.attributes        = re.attributes;
        d.checksum          = 0;
        ok = ok && std::fwrite(&d, 1, sizeof(d), fp) == sizeof(d);
    }

    // Blob data, once per distinct content
    for (auto& re : entries_) {
        if (re.alias >= 0 || re.blob.empty()) continue;
        ok = ok && std::fwrite(re.blob.data(), 1, re.blob.size(), fp) == re.blob.size();
    }

    ok = (std::fclose(fp) == 0) && ok;
    return ok;
}
