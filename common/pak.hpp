#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

// ============================================================================
// Murmur3-based path hash (RE Engine pak format)
// ============================================================================

// Hash a path for pak lookup. Normalises separators, collapses double slashes.
// Upper 32 bits = murmur3(UPPER(path) as UTF-16LE, seed 0xFFFFFFFF)
// Lower 32 bits = murmur3(lower(path) as UTF-16LE, seed 0xFFFFFFFF)
uint64_t pak_path_hash(std::wstring_view path);
uint64_t pak_path_hash(std::string_view  path); // ASCII convenience

// A file each loader stores in the patch pak it writes, so that both tell their paks apart from
// the mod paks and from each other. Patch order: mod paks < stage pak < costume pak.
constexpr const char* COSTUME_SLOTS_MARKER = "natives/stm/sf6_costume_slots.marker";
constexpr const char* STAGE_SLOTS_MARKER   = "natives/stm/sf6_stage_slots.marker";

// ============================================================================
// PakEntry  (48 bytes on disk, v4 format)
// ============================================================================

struct PakEntry {
    uint32_t hash_lower;
    uint32_t hash_upper;
    int64_t  offset;
    int64_t  compressed_size;
    int64_t  decompressed_size;
    int64_t  attributes;
    int64_t  checksum;

    uint64_t combined_hash() const {
        return (uint64_t(hash_upper) << 32) | hash_lower;
    }
    int compression() const { return int(attributes & 0xFF); }
};

// ============================================================================
// PakReader  --  read-only index over one RE Engine v4 pak
// ============================================================================

// Decompresses a pak blob (zstd / deflate / stored) given its compression code (attributes & 0xFF)
// and its decompressed size.
std::vector<uint8_t> pak_decompress(std::vector<uint8_t> raw, int comp, int64_t decompressed_size);

class PakReader {
public:
    PakReader() = default;
    ~PakReader();

    PakReader(const PakReader&) = delete;
    PakReader& operator=(const PakReader&) = delete;

    bool open(const char* path);
    void close();

    size_t entry_count() const { return entries_.size(); }

    const PakEntry* find(uint64_t hash) const;

    // A pak the game reads under this one (the DLC paks under re_chunk_000.pak): find() looks in
    // this pak first, then in the added paks in the order they were added, and read() reads an
    // entry from the pak it came from. The added pak must outlive this reader.
    void add_overlay(PakReader* over) { over_.push_back(over); }

    // Returns the raw (possibly compressed) blob.
    std::vector<uint8_t> read_raw(const PakEntry& e);

    // Returns decompressed data.  Handles zstd / deflate / raw.
    std::vector<uint8_t> read(const PakEntry& e);

    using EntryMap = std::unordered_map<uint64_t, PakEntry>;
    const EntryMap& entries() const { return entries_; }

private:
    FILE*    fp_ = nullptr;
    EntryMap entries_;
    std::vector<PakReader*> over_;

    PakReader* owner_of(const PakEntry& e);
};

// ============================================================================
// PakWriter  --  accumulate entries and write a single v4 pak
// ============================================================================

class PakWriter {
public:
    struct RawEntry {
        uint64_t             hash;
        std::vector<uint8_t> blob;
        int64_t              attributes;
        int64_t              decompressed_size;
        int64_t              alias = -1;   // index of an entry holding identical data, or -1
    };

    // Store a pre-compressed blob verbatim (passthrough).
    void add_raw(uint64_t hash, std::vector<uint8_t> blob,
                 int64_t attrib, int64_t decompressed_size);

    // Store uncompressed data (attrib = 0).
    void add_uncompressed(uint64_t hash, std::vector<uint8_t> data);

    // The entry last added under this hash, its data decompressed, or null / empty.
    const RawEntry* find(uint64_t hash) const;
    std::vector<uint8_t> get(uint64_t hash) const;
    // Data of an entry stored uncompressed, without copy; null otherwise.
    const std::vector<uint8_t>* peek(uint64_t hash) const;
    // Replaces the data of an entry (stored uncompressed). Entries that shared its data keep it.
    bool replace(uint64_t hash, std::vector<uint8_t> data);

    bool write(const char* path);

    size_t entry_count() const { return entries_.size(); }

    // Entries whose data was identical to an earlier entry, and the bytes that saved. The pak
    // stores such data once; both entries point to the same offset. Costume variants built from
    // one bundle share most of their textures, so this keeps the pak close to the mods' size.
    size_t dedup_count() const { return dedup_count_; }
    uint64_t dedup_bytes() const { return dedup_bytes_; }

private:
    void push(RawEntry e);

    std::vector<RawEntry> entries_;
    std::unordered_map<uint64_t, size_t> by_hash_;
    std::unordered_map<uint64_t, std::vector<size_t>> by_sig_;
    size_t dedup_count_ = 0;
    uint64_t dedup_bytes_ = 0;
};
