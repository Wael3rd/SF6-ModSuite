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

// What a re_chunk_000.pak.patch_NNN.pak is, by the marker file it holds (a pak nobody marked is a
// mod's). Shared by the loader and SF6_SlotsClean, so both see the same paks as ours.
enum class PatchPakKind { Unreadable, Mod, Costume, Stage };
// entries (optional) receives the number of entries; a loader pak with exactly one is a marker-only
// placeholder (write_marker_pak), never a real pak: those hold the marker and at least one more file.
PatchPakKind classify_patch_pak(const char* path, uint32_t* entries = nullptr);

// Writes a valid pak that holds only the marker of a loader (Costume or Stage): it keeps its file
// name in the row of patch paks, loads nothing, and is recognised as that loader's at the next scan.
bool write_marker_pak(const char* path, PatchPakKind kind);

// Which of these file names Fluffy Mod Manager's ArchiveList.bin ("ARCH" header, then NUL-separated
// names) lists, compared on the file name alone, case-insensitively. Empty when the file is absent.
std::vector<std::string> archive_list_hits(const std::string& archive_list_path,
                                           const std::vector<std::string>& names);


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
    // Tests only: the entry reads as turned off (zeroed by a mod manager's PAK method). From ZZtaii's
    // Slotsloader fork (1.9.9), MIT.
    void forget(uint64_t hash) { entries_.erase(hash); }

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
        std::vector<uint8_t> blob;         // the data, when held in memory
        int64_t              attributes;
        int64_t              decompressed_size;
        int64_t              alias = -1;   // index of an entry holding identical data, or -1
        int64_t              spill_at = -1;   // offset of the data in the spill file, or -1
        uint64_t             size = 0;     // size of the stored (possibly compressed) data
        uint64_t             sig = 0;      // content signature of that data
        bool                 replaced = false;
    };

    PakWriter() = default;
    ~PakWriter();
    PakWriter(const PakWriter&) = delete;
    PakWriter& operator=(const PakWriter&) = delete;

    // Data of min_size bytes or more goes to this scratch file as it is added instead of staying in
    // memory (a costume pak is many GB: held in memory it paged the whole PC out), once such data
    // already held in memory reaches keep_in_memory bytes. The file is deleted when the writer goes
    // away, crash included. False (everything stays in memory) when it cannot be made. The pak written
    // is the same either way.
    bool spill_to(const char* path, uint64_t min_size = 1 << 20, uint64_t keep_in_memory = 0);

    // Store a pre-compressed blob verbatim (passthrough).
    void add_raw(uint64_t hash, std::vector<uint8_t> blob,
                 int64_t attrib, int64_t decompressed_size);

    // Store uncompressed data (attrib = 0).
    void add_uncompressed(uint64_t hash, std::vector<uint8_t> data);

    // The same source again (a mod file, a pak entry, read the same way): the new entry shares the
    // data of the entry first added from it, exactly as if the data had been read and compared again,
    // without reading it. False when that cannot be decided without the data (source not seen, or its
    // entry replaced since): read the source, add it, then note_source().
    bool add_same_source(uint64_t hash, const std::string& source);
    // The entry just added was read from this source, unchanged.
    void note_source(const std::string& source);

    // The entry last added under this hash, its data decompressed, or null / empty.
    const RawEntry* find(uint64_t hash) const;
    std::vector<uint8_t> get(uint64_t hash) const;
    // The first n bytes (fewer if shorter) of an entry stored uncompressed; false otherwise.
    bool head(uint64_t hash, size_t n, std::vector<uint8_t>& out) const;
    // n bytes at `at` of an entry stored uncompressed; false otherwise or past its end. total: its size.
    bool range(uint64_t hash, uint64_t at, size_t n, std::vector<uint8_t>& out, uint64_t* total = nullptr) const;
    // Replaces the data of an entry (stored uncompressed). Entries that shared its data keep it.
    bool replace(uint64_t hash, std::vector<uint8_t> data);

    // durable: the file is flushed to the disk before returning (it is then renamed over a pak).
    // With a spill file, write() gives its space back as it copies: the writer is done afterwards.
    bool write(const char* path, bool durable = false);

    size_t entry_count() const { return entries_.size(); }

    // Entries whose data was identical to an earlier entry, and the bytes that saved. The pak
    // stores such data once; both entries point to the same offset. Costume variants built from
    // one bundle share most of their textures, so this keeps the pak close to the mods' size.
    size_t dedup_count() const { return dedup_count_; }
    uint64_t dedup_bytes() const { return dedup_bytes_; }
    // Entries shared through add_same_source, and the bytes not read for them.
    size_t source_hits() const { return source_hits_; }
    uint64_t source_bytes() const { return source_bytes_; }
    uint64_t spilled_bytes() const { return spill_end_; }

private:
    void push(RawEntry e);
    void store(RawEntry& e);   // spills e's data when it is big enough
    bool read_stored(const RawEntry& owner, uint64_t at, void* dst, size_t n) const;
    bool stored_equals(const RawEntry& owner, const std::vector<uint8_t>& data) const;
    bool stored_equal(const RawEntry& a, const RawEntry& b) const;
    void stored_data(const RawEntry& owner, std::vector<uint8_t>& out) const;

    std::vector<RawEntry> entries_;
    std::unordered_map<uint64_t, size_t> by_hash_;
    std::unordered_map<uint64_t, std::vector<size_t>> by_sig_;
    std::unordered_map<std::string, size_t> by_source_;
    size_t dedup_count_ = 0;
    uint64_t dedup_bytes_ = 0;
    size_t source_hits_ = 0;
    uint64_t source_bytes_ = 0;
    void* spill_ = nullptr;    // HANDLE of the spill file
    uint64_t spill_min_ = 0;
    uint64_t keep_in_memory_ = 0, kept_in_memory_ = 0;
    uint64_t spill_end_ = 0;
    uint64_t given_back_ = 0;  // spill bytes below this were given back by write()
};
