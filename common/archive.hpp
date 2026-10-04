#pragma once
// Archive extraction for the costume loader: .zip, .7z and .rar (RAR4 and RAR5).
//
// Runs inside DllMain, like the rest of the loader, so every decoder here is single-threaded
// and never loads a library at runtime: zip through miniz, 7z through the LZMA SDK decoder,
// rar through UnRAR built with SF6_LOADER_NO_SMP and SF6_LOADER_NO_CRYPT32 (see
// third_party/unrar/SF6_PATCHES.txt).
//
// The format is recognised by its signature, never by its extension.

#include <string>

enum class ArchiveKind { None, Zip, SevenZip, Rar };

// Reads the first bytes of the file and recognises the archive format.
ArchiveKind archive_sniff(const std::wstring& path);
const char* archive_kind_name(ArchiveKind k);

struct ArchiveResult {
    bool ok = false;
    int files_written = 0;
    int files_skipped = 0;             // entries the filter did not want
    unsigned long long bytes_written = 0;
    std::string error;                 // set when ok == false
};

// Decides whether an entry is worth extracting. The path uses '/' separators and is relative to
// the archive root. Returning false skips the entry (its data is still decoded in solid archives).
typedef bool (*ArchiveFilter)(const std::wstring& entry_path);

// Extracts every wanted entry of `archive` under `dest_dir`, recreating the relative paths.
// Entries that would escape dest_dir (absolute paths, drive letters, "..") are refused.
// Encrypted entries make the whole extraction fail: the loader has no password to offer.
ArchiveResult archive_extract(const std::wstring& archive, const std::wstring& dest_dir,
                              ArchiveFilter keep);

// The file entries of `archive` ('/' separators, relative to the archive root), read from its
// headers without unpacking anything. False when the archive cannot be read.
#include <vector>
bool archive_list(const std::wstring& archive, std::vector<std::wstring>& names);
