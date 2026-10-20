#include "pak.hpp"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <io.h>

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

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

int PakReader::source_of(const PakEntry& e) const {
    auto it = entries_.find(e.combined_hash());
    if (it != entries_.end() && &it->second == &e) return 0;
    for (size_t i = 0; i < over_.size(); i++) {
        auto jt = over_[i]->entries_.find(e.combined_hash());
        if (jt != over_[i]->entries_.end() && &jt->second == &e) return int(i) + 1;
    }
    return -1;
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

PakWriter::~PakWriter() {
    if (spill_) CloseHandle(HANDLE(spill_));   // FILE_FLAG_DELETE_ON_CLOSE: the spill file goes with it
    if (prev_) CloseHandle(HANDLE(prev_));
}

bool PakWriter::use_previous(const char* pak_path, std::unordered_map<std::string, SourceData> sources) {
    if (prev_) return true;
    // shared for deletion: the loader replaces or retires that pak once write() has closed it
    HANDLE h = CreateFileA(pak_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    prev_ = h;
    prev_sources_ = std::move(sources);
    return true;
}

bool PakWriter::add_previous(uint64_t hash, const std::string& lasting) {
    if (!prev_ || lasting.empty()) return false;
    auto it = prev_sources_.find(lasting);
    if (it == prev_sources_.end()) return false;
    const SourceData& s = it->second;
    RawEntry e;
    e.hash = hash;
    e.attributes = s.attributes;
    e.decompressed_size = s.decompressed_size;
    e.size = s.size;
    e.sig = s.sig;
    e.prev_at = int64_t(s.offset);
    previous_hits_++;
    previous_bytes_ += s.size;
    push_stored(std::move(e));
    return true;
}

std::vector<std::pair<std::string, PakWriter::SourceData>> PakWriter::lasting_sources() const {
    std::vector<std::pair<std::string, SourceData>> out;
    if (written_at_.size() != entries_.size()) return out;
    for (auto& [key, i] : lasting_) {
        const RawEntry& e = entries_[i];
        if (e.replaced) continue;   // its data is not the source's any more
        const size_t holder = e.alias >= 0 ? size_t(e.alias) : i;
        SourceData s;
        s.offset = uint64_t(written_at_[holder]);
        s.size = entries_[holder].size;
        s.attributes = e.attributes;
        s.decompressed_size = e.decompressed_size;
        s.sig = e.sig;
        out.push_back({key, s});
    }
    return out;
}

bool PakWriter::spill_to(const char* path, uint64_t min_size, uint64_t keep_in_memory) {
    if (spill_) return true;
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // Sparse: write() gives back the space of what it has copied, so that the disk does not hold the
    // data three times (spill file, new pak, old pak) at the end
    DWORD ret = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    spill_ = h;
    spill_min_ = min_size;
    keep_in_memory_ = keep_in_memory;
    spill_end_ = 0;
    return true;
}

void PakWriter::store(RawEntry& e) {
    e.size = e.blob.size();
    if (!spill_ || given_back_ || e.blob.size() < spill_min_) return;
    if (kept_in_memory_ + e.blob.size() <= keep_in_memory_) { kept_in_memory_ += e.blob.size(); return; }
    const uint64_t at = spill_end_;
    for (size_t put = 0; put < e.blob.size();) {
        const uint64_t pos = at + put;
        OVERLAPPED ov{};
        ov.Offset = DWORD(pos);
        ov.OffsetHigh = DWORD(pos >> 32);
        const DWORD n = DWORD(std::min<size_t>(e.blob.size() - put, size_t(64) << 20));
        DWORD w = 0;
        if (!WriteFile(HANDLE(spill_), e.blob.data() + put, n, &w, &ov) || w != n) return;   // disk full: kept in memory
        put += n;
    }
    spill_end_ = at + e.blob.size();
    e.spill_at = int64_t(at);
    std::vector<uint8_t>().swap(e.blob);
}

bool PakWriter::read_stored(const RawEntry& o, uint64_t at, void* dst, size_t n) const {
    if (at + n > o.size) return false;
    if (in_memory(o)) {
        if (n) std::memcpy(dst, o.blob.data() + at, n);
        return true;
    }
    if (o.spill_at >= 0 && uint64_t(o.spill_at) < given_back_) return false;   // given back by write()
    if (o.spill_at < 0 && !prev_) return false;   // closed by write()
    const HANDLE file = HANDLE(o.spill_at >= 0 ? spill_ : prev_);
    const uint64_t base = uint64_t(o.spill_at >= 0 ? o.spill_at : o.prev_at);
    for (size_t got = 0; got < n;) {
        const uint64_t pos = base + at + got;
        OVERLAPPED ov{};
        ov.Offset = DWORD(pos);
        ov.OffsetHigh = DWORD(pos >> 32);
        const DWORD want = DWORD(std::min<size_t>(n - got, size_t(64) << 20));
        DWORD r = 0;
        if (!ReadFile(file, static_cast<uint8_t*>(dst) + got, want, &r, &ov) || r != want) return false;
        got += r;
    }
    return true;
}

void PakWriter::stored_data(const RawEntry& o, std::vector<uint8_t>& out) const {
    if (in_memory(o)) { out = o.blob; return; }
    out.resize(size_t(o.size));
    if (!read_stored(o, 0, out.data(), out.size())) out.clear();
}

bool PakWriter::stored_equals(const RawEntry& o, const std::vector<uint8_t>& d) const {
    if (o.size != d.size()) return false;
    if (in_memory(o)) return d.empty() || std::memcmp(o.blob.data(), d.data(), d.size()) == 0;
    std::vector<uint8_t> buf(std::min<size_t>(d.size(), size_t(4) << 20));
    for (size_t at = 0; at < d.size(); at += buf.size()) {
        const size_t n = std::min(buf.size(), d.size() - at);
        if (!read_stored(o, at, buf.data(), n) || std::memcmp(buf.data(), d.data() + at, n) != 0) return false;
    }
    return true;
}

bool PakWriter::stored_equal(const RawEntry& a, const RawEntry& b) const {
    if (a.size != b.size) return false;
    if (a.prev_at >= 0 && a.prev_at == b.prev_at) return true;   // the same bytes of the previous pak
    if (in_memory(a)) return stored_equals(b, a.blob);
    if (in_memory(b)) return stored_equals(a, b.blob);
    const size_t chunk = size_t(std::min<uint64_t>(a.size, uint64_t(4) << 20));
    std::vector<uint8_t> x(chunk), y(chunk);
    for (uint64_t at = 0; at < a.size; at += chunk) {
        const size_t n = size_t(std::min<uint64_t>(chunk, a.size - at));
        if (!read_stored(a, at, x.data(), n) || !read_stored(b, at, y.data(), n)
            || std::memcmp(x.data(), y.data(), n) != 0) return false;
    }
    return true;
}

void PakWriter::push(RawEntry e) {
    by_hash_[e.hash] = entries_.size();
    e.size = e.blob.size();
    e.sig = blob_signature(e.blob, e.attributes);
    auto& same = by_sig_[e.sig];
    for (size_t idx : same) {
        const RawEntry& o = entries_[idx];
        if (o.size == e.size && o.attributes == e.attributes
            && o.decompressed_size == e.decompressed_size && stored_equals(o, e.blob)) {
            dedup_count_++;
            dedup_bytes_ += e.size;
            e.alias = int64_t(idx);
            std::vector<uint8_t>().swap(e.blob);
            entries_.push_back(std::move(e));
            return;
        }
    }
    same.push_back(entries_.size());
    store(e);
    entries_.push_back(std::move(e));
}

void PakWriter::push_stored(RawEntry e) {
    by_hash_[e.hash] = entries_.size();
    auto& same = by_sig_[e.sig];
    for (size_t idx : same) {
        const RawEntry& o = entries_[idx];
        if (o.size == e.size && o.attributes == e.attributes
            && o.decompressed_size == e.decompressed_size && stored_equal(o, e)) {
            dedup_count_++;
            dedup_bytes_ += e.size;
            e.alias = int64_t(idx);
            e.prev_at = -1;
            entries_.push_back(std::move(e));
            return;
        }
    }
    same.push_back(entries_.size());
    entries_.push_back(std::move(e));
}

bool PakWriter::add_same_source(uint64_t hash, const std::string& source) {
    auto it = by_source_.find(source);
    if (it == by_source_.end()) return false;
    const RawEntry& first = entries_[it->second];
    if (first.replaced) return false;   // its data is not the source's any more
    // What push() would do with that data: share the first entry of its signature that holds the same
    // data. The data is the one of the entry holding it for `first`: comparing needs no read of the source.
    const size_t holder = first.alias >= 0 ? size_t(first.alias) : it->second;
    auto sit = by_sig_.find(first.sig);
    if (sit == by_sig_.end()) return false;
    for (size_t idx : sit->second) {
        const RawEntry& o = entries_[idx];
        if (o.size != first.size || o.attributes != first.attributes
            || o.decompressed_size != first.decompressed_size) continue;
        if (idx != holder && !stored_equal(o, entries_[holder])) continue;
        RawEntry e;
        e.hash = hash;
        e.attributes = first.attributes;
        e.decompressed_size = first.decompressed_size;
        e.alias = int64_t(idx);
        e.size = first.size;
        e.sig = first.sig;
        by_hash_[hash] = entries_.size();
        dedup_count_++;
        dedup_bytes_ += e.size;
        source_hits_++;
        source_bytes_ += e.size;
        entries_.push_back(std::move(e));
        return true;
    }
    return false;   // no entry holds that data any more: read the source
}

void PakWriter::note_source(const std::string& source, const std::string& lasting) {
    if (entries_.empty()) return;
    by_source_[source] = entries_.size() - 1;
    if (!lasting.empty()) lasting_.push_back({lasting, entries_.size() - 1});
}

const PakWriter::RawEntry* PakWriter::find(uint64_t hash) const {
    auto it = by_hash_.find(hash);
    return it == by_hash_.end() ? nullptr : &entries_[it->second];
}

std::vector<uint8_t> PakWriter::get(uint64_t hash) const {
    const RawEntry* e = find(hash);
    if (!e) return {};
    const RawEntry& src = (e->alias >= 0) ? entries_[size_t(e->alias)] : *e;
    std::vector<uint8_t> raw;
    stored_data(src, raw);
    return pak_decompress(std::move(raw), int(e->attributes & 0xFF), e->decompressed_size);
}

bool PakWriter::head(uint64_t hash, size_t n, std::vector<uint8_t>& out) const {
    const RawEntry* e = find(hash);
    if (!e || (e->attributes & 0xFF) != 0) return false;
    const RawEntry& src = (e->alias >= 0) ? entries_[size_t(e->alias)] : *e;
    out.resize(size_t(std::min<uint64_t>(n, src.size)));
    return read_stored(src, 0, out.data(), out.size());
}

bool PakWriter::range(uint64_t hash, uint64_t at, size_t n, std::vector<uint8_t>& out, uint64_t* total) const {
    const RawEntry* e = find(hash);
    if (!e || (e->attributes & 0xFF) != 0) return false;
    const RawEntry& src = (e->alias >= 0) ? entries_[size_t(e->alias)] : *e;
    if (total) *total = src.size;
    if (at > src.size || n > src.size - at) return false;
    out.resize(n);
    return read_stored(src, at, out.data(), n);
}

bool PakWriter::replace(uint64_t hash, std::vector<uint8_t> data) {
    auto it = by_hash_.find(hash);
    if (it == by_hash_.end()) return false;
    const size_t i = it->second;
    RawEntry& e = entries_[i];
    if (e.alias < 0) {
        // Other entries may share this entry's data: the first of them takes it over
        auto& same = by_sig_[e.sig];
        same.erase(std::remove(same.begin(), same.end(), i), same.end());
        int64_t heir = -1;
        for (size_t j = 0; j < entries_.size(); ++j) {
            if (entries_[j].alias != int64_t(i)) continue;
            if (heir < 0) {
                heir = int64_t(j);
                RawEntry& h = entries_[j];
                h.blob = std::move(e.blob);
                h.spill_at = e.spill_at;
                h.prev_at = e.prev_at;
                h.size = e.size;
                h.alias = -1;
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
    e.spill_at = -1;
    e.prev_at = -1;
    e.replaced = true;
    e.sig = blob_signature(e.blob, e.attributes);
    store(e);
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

bool PakWriter::write(const char* path, bool durable) {
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    std::setvbuf(fp, nullptr, _IOFBF, size_t(1) << 20);

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
        offset += int64_t(entries_[i].size);
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
        const RawEntry& re = entries_[i];
        const RawEntry& src = (re.alias >= 0) ? entries_[size_t(re.alias)] : re;
        PakEntryDisk d{};
        d.hash_lower       = uint32_t(re.hash & 0xFFFFFFFF);
        d.hash_upper       = uint32_t(re.hash >> 32);
        d.offset            = (re.alias >= 0) ? offs[size_t(re.alias)] : offs[i];
        d.compressed_size   = int64_t(src.size);
        d.decompressed_size = re.decompressed_size;
        d.attributes        = re.attributes;
        d.checksum          = 0;
        ok = ok && std::fwrite(&d, 1, sizeof(d), fp) == sizeof(d);
    }

    // Blob data, once per distinct content. Spilled data is copied in big blocks, and the spill file's
    // space below the first spilled data not copied yet is given back as the copy goes.
    std::vector<std::pair<int64_t, size_t>> live;   // (spill offset, entry) of the spilled data, in file order
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].alias < 0 && entries_[i].spill_at >= 0) live.push_back({entries_[i].spill_at, i});
    std::sort(live.begin(), live.end());
    std::vector<uint8_t> copied(entries_.size(), 0);
    size_t next_live = 0;
    uint64_t given_back = 0;
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < entries_.size() && ok; ++i) {
        const RawEntry& re = entries_[i];
        if (re.alias >= 0 || re.size == 0) continue;
        if (in_memory(re)) {
            ok = std::fwrite(re.blob.data(), 1, re.blob.size(), fp) == re.blob.size();
            continue;
        }
        if (buf.empty()) buf.resize(size_t(16) << 20);
        for (uint64_t at = 0; at < re.size && ok; at += buf.size()) {
            const size_t k = size_t(std::min<uint64_t>(buf.size(), re.size - at));
            ok = read_stored(re, at, buf.data(), k) && std::fwrite(buf.data(), 1, k, fp) == k;
        }
        if (re.spill_at < 0) continue;   // from the previous pak
        copied[i] = 1;
        while (next_live < live.size() && copied[live[next_live].second]) next_live++;
        const uint64_t upto = next_live < live.size() ? uint64_t(live[next_live].first) : spill_end_;
        if (upto >= given_back + (uint64_t(256) << 20) || (next_live == live.size() && upto > given_back)) {
            FILE_ZERO_DATA_INFORMATION z{};
            z.FileOffset.QuadPart = LONGLONG(given_back);
            z.BeyondFinalZero.QuadPart = LONGLONG(upto);
            DWORD ret = 0;
            DeviceIoControl(HANDLE(spill_), FSCTL_SET_ZERO_DATA, &z, sizeof(z), nullptr, 0, &ret, nullptr);
            given_back = upto;
            given_back_ = upto;   // that data is gone: the writer is done
        }
    }

    if (prev_) {   // the previous pak is replaced or retired next
        CloseHandle(HANDLE(prev_));
        prev_ = nullptr;
    }
    if (ok && durable) {
        ok = std::fflush(fp) == 0;
        ok = ok && FlushFileBuffers(HANDLE(_get_osfhandle(_fileno(fp))));
    }
    ok = (std::fclose(fp) == 0) && ok;
    if (ok) {
        written_at_.assign(entries_.size(), 0);
        for (size_t i = 0; i < entries_.size(); ++i)
            written_at_[i] = entries_[i].alias >= 0 ? offs[size_t(entries_[i].alias)] : offs[i];
    }
    return ok;
}

// ============================================================================
// Patch pak kinds, marker-only paks, Fluffy's ArchiveList.bin
// ============================================================================

PatchPakKind classify_patch_pak(const char* path, uint32_t* entries) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return PatchPakKind::Unreadable;
    PatchPakKind kind = PatchPakKind::Mod;
    PakHeader hdr{};
    if (entries) *entries = 0;
    if (std::fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr)) {
        if (entries) *entries = hdr.count;
        const uint64_t costume_h = pak_path_hash(std::string_view(COSTUME_SLOTS_MARKER));
        const uint64_t stage_h = pak_path_hash(std::string_view(STAGE_SLOTS_MARKER));
        for (uint32_t i = 0; i < hdr.count; i++) {
            PakEntryDisk d;
            if (std::fread(&d, 1, sizeof(d), fp) != sizeof(d)) break;
            const uint64_t h = (uint64_t(d.hash_upper) << 32) | d.hash_lower;
            if (h == costume_h) { kind = PatchPakKind::Costume; break; }
            if (h == stage_h) { kind = PatchPakKind::Stage; break; }
        }
    }
    std::fclose(fp);
    return kind;
}

bool write_marker_pak(const char* path, PatchPakKind kind) {
    // The same bytes the loaders write into their own paks
    static const char costume_data[20] = { 'S','F','6','_','C','o','s','t','u','m','e','S','l','o','t','s',' ','v','1','\n' };
    static const char stage_data[21] = { 'S','F','6','_','S','t','a','g','e','S','l','o','t','s',' ','v','1','\n',0,0,0 };
    PakWriter w;
    if (kind == PatchPakKind::Costume)
        w.add_uncompressed(pak_path_hash(std::string_view(COSTUME_SLOTS_MARKER)),
                           std::vector<uint8_t>(costume_data, costume_data + sizeof(costume_data)));
    else if (kind == PatchPakKind::Stage)
        w.add_uncompressed(pak_path_hash(std::string_view(STAGE_SLOTS_MARKER)),
                           std::vector<uint8_t>(stage_data, stage_data + sizeof(stage_data)));
    else
        return false;
    return w.write(path);
}

std::vector<std::string> archive_list_hits(const std::string& archive_list_path,
                                           const std::vector<std::string>& names) {
    std::vector<std::string> hits;
    FILE* fp = std::fopen(archive_list_path.c_str(), "rb");
    if (!fp) return hits;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) data.append(buf, n);
    std::fclose(fp);
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; };
    std::vector<std::string> listed;   // file names (no folder), lower case
    for (size_t i = 0; i < data.size();) {
        size_t e = data.find('\0', i);
        if (e == std::string::npos) e = data.size();
        std::string tok = data.substr(i, e - i);
        size_t sl = tok.find_last_of("\\/");
        if (sl != std::string::npos) tok = tok.substr(sl + 1);
        if (!tok.empty()) listed.push_back(lower(tok));
        i = e + 1;
    }
    for (auto& nm : names) {
        const std::string l = lower(nm);
        if (std::find(listed.begin(), listed.end(), l) != listed.end()) hits.push_back(nm);
    }
    return hits;
}
