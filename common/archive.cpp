// archive.cpp -- see archive.hpp.
//
// RAR support uses the UnRAR library by Alexander Roshal, vendored in third_party/unrar:
//
//   UnRAR source code may be used in any software to handle RAR archives without
//   limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible
//   archiver and to re-create RAR compression algorithm, which is proprietary.
//   Distribution of modified UnRAR source code in separate form or as a part of other
//   software is permitted, provided that full text of this paragraph, starting from
//   "UnRAR source code" words, is included in license, or in documentation if license
//   is not available, and in source code comments of resulting package.
//
// 7z support uses the LZMA SDK decoder (public domain), vendored in third_party/lzma.
// ZIP support uses miniz (MIT), already vendored for the pak code.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "archive.hpp"
#include "miniz.h"
#include "lzma/7z.h"
#include "lzma/7zAlloc.h"
#include "lzma/7zCrc.h"
#include "lzma/7zFile.h"
#include "unrar/dll.hpp"

// ---------------------------------------------------------------------------------------------
// Format detection
// ---------------------------------------------------------------------------------------------

ArchiveKind archive_sniff(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ArchiveKind::None;
    unsigned char b[8] = {};
    DWORD got = 0;
    ReadFile(h, b, sizeof(b), &got, nullptr);
    CloseHandle(h);
    if (got >= 4 && b[0] == 'P' && b[1] == 'K' && ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6)))
        return ArchiveKind::Zip;
    if (got >= 6 && memcmp(b, "7z\xBC\xAF\x27\x1C", 6) == 0)
        return ArchiveKind::SevenZip;
    if (got >= 6 && memcmp(b, "Rar!\x1A\x07", 6) == 0)          // RAR4 (..00) and RAR5 (..01 00)
        return ArchiveKind::Rar;
    return ArchiveKind::None;
}

const char* archive_kind_name(ArchiveKind k) {
    switch (k) {
        case ArchiveKind::Zip: return "zip";
        case ArchiveKind::SevenZip: return "7z";
        case ArchiveKind::Rar: return "rar";
        default: return "none";
    }
}

// ---------------------------------------------------------------------------------------------
// Paths and output files
// ---------------------------------------------------------------------------------------------

// Normalises an entry path to '/' separators and refuses anything that could land outside the
// destination folder: absolute paths, drive letters and ".." components.
static bool sanitize_entry(std::wstring& p) {
    for (auto& c : p) if (c == L'\\') c = L'/';
    while (!p.empty() && p[0] == L'/') p.erase(0, 1);
    while (p.size() >= 2 && p[0] == L'.' && p[1] == L'/') p.erase(0, 2);
    if (p.empty() || p.back() == L'/') return false;
    if (p.size() >= 2 && p[1] == L':') return false;
    size_t start = 0;
    while (start < p.size()) {
        size_t end = p.find(L'/', start);
        if (end == std::wstring::npos) end = p.size();
        if (p.compare(start, end - start, L"..") == 0) return false;
        start = end + 1;
    }
    return true;
}

static std::wstring join_path(const std::wstring& dest, const std::wstring& rel) {
    std::wstring out = dest;
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    for (wchar_t c : rel) out += (c == L'/') ? L'\\' : c;
    return out;
}

static std::wstring long_path(const std::wstring& p) {
    if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0) return p;
    return L"\\\\?\\" + p;
}

static bool make_dirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {                 // skip "C:\"
        if (i == dir.size() || dir[i] == L'\\')
            CreateDirectoryW(long_path(dir.substr(0, i)).c_str(), nullptr);
    }
    DWORD a = GetFileAttributesW(long_path(dir).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

struct OutFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open(const std::wstring& path) {
        auto sl = path.rfind(L'\\');
        if (sl != std::wstring::npos) make_dirs(path.substr(0, sl));
        h = CreateFileW(long_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }
    bool write(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, size_t(1) << 30));
            DWORD w = 0;
            if (!WriteFile(h, c, chunk, &w, nullptr) || w != chunk) return false;
            c += w; n -= w;
        }
        return true;
    }
    void close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    ~OutFile() { close(); }
};

// ---------------------------------------------------------------------------------------------
// ZIP (miniz: stored and deflate)
// ---------------------------------------------------------------------------------------------

static std::wstring decode_zip_name(const char* s, bool utf8) {
    UINT cp = utf8 ? CP_UTF8 : 437;                               // zip default is IBM 437
    int n = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
    if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0); }
    if (n <= 1) return std::wstring();
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(cp, 0, s, -1, &w[0], n);
    return w;
}

static size_t zip_write_cb(void* opaque, mz_uint64, const void* buf, size_t n) {
    return static_cast<OutFile*>(opaque)->write(buf, n) ? n : 0;
}

static ArchiveResult extract_zip(const std::wstring& archive, const std::wstring& dest,
                                 ArchiveFilter keep) {
    ArchiveResult r;
    FILE* f = _wfopen(archive.c_str(), L"rb");
    if (!f) { r.error = "cannot open archive"; return r; }
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_cfile(&z, f, 0, 0)) { fclose(f); r.error = "unreadable zip"; return r; }
    mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
        std::wstring name = decode_zip_name(st.m_filename, (st.m_bit_flag & 0x800) != 0);
        if (!sanitize_entry(name) || !keep(name)) { r.files_skipped++; continue; }
        if (st.m_is_encrypted) { r.error = "password-protected entry"; break; }
        if (!st.m_is_supported) {
            char b[96];
            sprintf(b, "unsupported zip compression method %u", unsigned(st.m_method));
            r.error = b; break;
        }
        OutFile of;
        if (!of.open(join_path(dest, name))) { r.error = "cannot create an output file"; break; }
        if (!mz_zip_reader_extract_to_callback(&z, i, zip_write_cb, &of, 0)) {
            r.error = "corrupted zip data"; break;
        }
        r.files_written++;
        r.bytes_written += st.m_uncomp_size;
    }
    mz_zip_reader_end(&z);
    fclose(f);
    r.ok = r.error.empty();
    return r;
}

// ---------------------------------------------------------------------------------------------
// 7z (LZMA SDK: LZMA, LZMA2, PPMd, BCJ, BCJ2, ARM64 and delta filters; no AES)
// ---------------------------------------------------------------------------------------------

static const char* sz_error(SRes res) {
    switch (res) {
        case SZ_ERROR_UNSUPPORTED: return "unsupported 7z method (encrypted or unusual codec)";
        case SZ_ERROR_MEM: return "not enough memory to unpack the 7z block";
        case SZ_ERROR_CRC: return "corrupted 7z archive (CRC)";
        case SZ_ERROR_DATA: return "corrupted 7z archive (data)";
        case SZ_ERROR_ARCHIVE: return "damaged 7z headers";
        case SZ_ERROR_NO_ARCHIVE: return "not a 7z archive";
        case SZ_ERROR_READ: return "cannot read the 7z archive";
        default: return "7z error";
    }
}

static ArchiveResult extract_7z(const std::wstring& archive, const std::wstring& dest,
                                ArchiveFilter keep) {
    ArchiveResult r;
    const size_t kInputBufSize = size_t(1) << 18;
    ISzAlloc allocImp = { SzAlloc, SzFree };
    ISzAlloc allocTempImp = { SzAllocTemp, SzFreeTemp };
    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;

    if (InFile_OpenW(&archiveStream.file, archive.c_str()) != 0) {
        r.error = "cannot open archive"; return r;
    }
    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;
    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocImp, kInputBufSize));
    if (!lookStream.buf) { File_Close(&archiveStream.file); r.error = "out of memory"; return r; }
    lookStream.bufSize = kInputBufSize;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream)

    static bool crc_ready = false;
    if (!crc_ready) { CrcGenerateTable(); crc_ready = true; }

    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
    if (res != SZ_OK) {
        r.error = sz_error(res);
    } else {
        UInt32 blockIndex = 0xFFFFFFFF;       // solid block cache, kept across files
        Byte* outBuffer = nullptr;
        size_t outBufferSize = 0;
        std::vector<UInt16> nameBuf;
        for (UInt32 i = 0; i < db.NumFiles; ++i) {
            if (SzArEx_IsDir(&db, i)) continue;
            size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
            nameBuf.assign(len + 1, 0);
            SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
            std::wstring name(reinterpret_cast<const wchar_t*>(nameBuf.data()));
            if (!sanitize_entry(name) || !keep(name)) { r.files_skipped++; continue; }
            size_t offset = 0, outSizeProcessed = 0;
            res = SzArEx_Extract(&db, &lookStream.vt, i, &blockIndex, &outBuffer, &outBufferSize,
                                 &offset, &outSizeProcessed, &allocImp, &allocTempImp);
            if (res != SZ_OK) { r.error = sz_error(res); break; }
            OutFile of;
            if (!of.open(join_path(dest, name)) || !of.write(outBuffer + offset, outSizeProcessed)) {
                r.error = "cannot write an output file"; break;
            }
            r.files_written++;
            r.bytes_written += outSizeProcessed;
        }
        ISzAlloc_Free(&allocImp, outBuffer);
    }
    SzArEx_Free(&db, &allocImp);
    ISzAlloc_Free(&allocImp, lookStream.buf);
    File_Close(&archiveStream.file);
    r.ok = r.error.empty();
    return r;
}

// ---------------------------------------------------------------------------------------------
// RAR (UnRAR: RAR4 and RAR5, solid archives included; single-threaded build)
// ---------------------------------------------------------------------------------------------

static const char* rar_error(int code) {
    switch (code) {
        case ERAR_NO_MEMORY: return "not enough memory";
        case ERAR_BAD_DATA: return "corrupted rar data";
        case ERAR_BAD_ARCHIVE: return "damaged rar archive";
        case ERAR_UNKNOWN_FORMAT: return "unknown rar format";
        case ERAR_EOPEN: return "cannot open the rar archive or one of its volumes";
        case ERAR_ECREATE: return "cannot create an output file";
        case ERAR_EREAD: return "cannot read the rar archive";
        case ERAR_EWRITE: return "cannot write an output file";
        case ERAR_MISSING_PASSWORD: return "password-protected archive";
        case ERAR_BAD_PASSWORD: return "password-protected archive";
        case ERAR_LARGE_DICT: return "rar dictionary too large";
        default: return "rar error";
    }
}

static int CALLBACK rar_callback(UINT msg, LPARAM, LPARAM, LPARAM p2) {
    switch (msg) {
        case UCM_NEEDPASSWORD:
        case UCM_NEEDPASSWORDW: return -1;                       // no password to give: stop
        case UCM_CHANGEVOLUME:
        case UCM_CHANGEVOLUMEW: return (p2 == RAR_VOL_ASK) ? -1 : 1;  // missing volume: stop
        case UCM_LARGEDICT: return 1;
        default: return 0;
    }
}

static ArchiveResult extract_rar(const std::wstring& archive, const std::wstring& dest,
                                 ArchiveFilter keep) {
    ArchiveResult r;
    std::vector<wchar_t> arcName(archive.begin(), archive.end());
    arcName.push_back(0);
    RAROpenArchiveDataEx od;
    memset(&od, 0, sizeof(od));
    od.ArcNameW = arcName.data();
    od.OpenMode = RAR_OM_EXTRACT;
    od.Callback = rar_callback;
    HANDLE h = RAROpenArchiveEx(&od);
    if (!h || od.OpenResult != ERAR_SUCCESS) {
        r.error = rar_error(int(od.OpenResult));
        if (h) RARCloseArchive(h);
        return r;
    }
    if ((od.Flags & ROADF_VOLUME) && !(od.Flags & ROADF_FIRSTVOLUME)) {
        RARCloseArchive(h); r.error = "not the first volume of a multi-part rar"; return r;
    }
    if (od.Flags & ROADF_ENCHEADERS) {
        RARCloseArchive(h); r.error = "password-protected archive"; return r;
    }

    RARHeaderDataEx* hd = new RARHeaderDataEx;               // large struct, keep it off the stack
    int rc;
    for (;;) {
        memset(hd, 0, sizeof(*hd));
        rc = RARReadHeaderEx(h, hd);
        if (rc != ERAR_SUCCESS) break;
        std::wstring name(hd->FileNameW);
        bool is_dir = (hd->Flags & RHDF_DIRECTORY) != 0;
        bool want = !is_dir && sanitize_entry(name) && keep(name);
        if (want && (hd->Flags & RHDF_ENCRYPTED)) { r.error = "password-protected entry"; break; }
        int prc;
        if (want) {
            std::wstring out = join_path(dest, name);
            auto sl = out.rfind(L'\\');
            if (sl != std::wstring::npos) make_dirs(out.substr(0, sl));
            std::vector<wchar_t> outBuf(out.begin(), out.end());
            outBuf.push_back(0);
            prc = RARProcessFileW(h, RAR_EXTRACT, nullptr, outBuf.data());
        } else {
            prc = RARProcessFileW(h, RAR_SKIP, nullptr, nullptr);
        }
        if (prc != ERAR_SUCCESS) { r.error = rar_error(prc); break; }
        if (want) {
            r.files_written++;
            r.bytes_written += (static_cast<unsigned long long>(hd->UnpSizeHigh) << 32) | hd->UnpSize;
        } else if (!is_dir) {
            r.files_skipped++;
        }
    }
    if (r.error.empty() && rc != ERAR_END_ARCHIVE) r.error = rar_error(rc);
    delete hd;
    RARCloseArchive(h);
    r.ok = r.error.empty();
    return r;
}

// ---------------------------------------------------------------------------------------------

static bool list_zip(const std::wstring& archive, std::vector<std::wstring>& names) {
    FILE* f = _wfopen(archive.c_str(), L"rb");
    if (!f) return false;
    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_cfile(&z, f, 0, 0)) { fclose(f); return false; }
    mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
        std::wstring name = decode_zip_name(st.m_filename, (st.m_bit_flag & 0x800) != 0);
        if (sanitize_entry(name)) names.push_back(name);
    }
    mz_zip_reader_end(&z);
    fclose(f);
    return true;
}

static bool list_7z(const std::wstring& archive, std::vector<std::wstring>& names) {
    const size_t kInputBufSize = size_t(1) << 18;
    ISzAlloc allocImp = { SzAlloc, SzFree };
    ISzAlloc allocTempImp = { SzAllocTemp, SzFreeTemp };
    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
    if (InFile_OpenW(&archiveStream.file, archive.c_str()) != 0) return false;
    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;
    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&allocImp, kInputBufSize));
    if (!lookStream.buf) { File_Close(&archiveStream.file); return false; }
    lookStream.bufSize = kInputBufSize;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream)
    static bool crc_ready = false;
    if (!crc_ready) { CrcGenerateTable(); crc_ready = true; }
    SzArEx_Init(&db);
    bool ok = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp) == SZ_OK;
    if (ok) {
        std::vector<UInt16> nameBuf;
        for (UInt32 i = 0; i < db.NumFiles; ++i) {
            if (SzArEx_IsDir(&db, i)) continue;
            size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
            nameBuf.assign(len + 1, 0);
            SzArEx_GetFileNameUtf16(&db, i, nameBuf.data());
            std::wstring name(reinterpret_cast<const wchar_t*>(nameBuf.data()));
            if (sanitize_entry(name)) names.push_back(name);
        }
    }
    SzArEx_Free(&db, &allocImp);
    ISzAlloc_Free(&allocImp, lookStream.buf);
    File_Close(&archiveStream.file);
    return ok;
}

static bool list_rar(const std::wstring& archive, std::vector<std::wstring>& names) {
    std::vector<wchar_t> arcName(archive.begin(), archive.end());
    arcName.push_back(0);
    RAROpenArchiveDataEx od;
    memset(&od, 0, sizeof(od));
    od.ArcNameW = arcName.data();
    od.OpenMode = RAR_OM_LIST;                                // headers only, nothing unpacked
    od.Callback = rar_callback;
    HANDLE h = RAROpenArchiveEx(&od);
    if (!h || od.OpenResult != ERAR_SUCCESS) { if (h) RARCloseArchive(h); return false; }
    if (od.Flags & ROADF_ENCHEADERS) { RARCloseArchive(h); return false; }
    RARHeaderDataEx* hd = new RARHeaderDataEx;
    int rc;
    for (;;) {
        memset(hd, 0, sizeof(*hd));
        rc = RARReadHeaderEx(h, hd);
        if (rc != ERAR_SUCCESS) break;
        std::wstring name(hd->FileNameW);
        if (!(hd->Flags & RHDF_DIRECTORY) && sanitize_entry(name)) names.push_back(name);
        if (RARProcessFileW(h, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS) { rc = -1; break; }
    }
    delete hd;
    RARCloseArchive(h);
    return rc == ERAR_END_ARCHIVE;
}

bool archive_list(const std::wstring& archive, std::vector<std::wstring>& names) {
    names.clear();
    switch (archive_sniff(archive)) {
        case ArchiveKind::Zip: return list_zip(archive, names);
        case ArchiveKind::SevenZip: return list_7z(archive, names);
        case ArchiveKind::Rar: return list_rar(archive, names);
        default: return false;
    }
}

ArchiveResult archive_extract(const std::wstring& archive, const std::wstring& dest_dir,
                              ArchiveFilter keep) {
    ArchiveResult r;
    if (!make_dirs(dest_dir)) { r.error = "cannot create the cache folder"; return r; }
    switch (archive_sniff(archive)) {
        case ArchiveKind::Zip: return extract_zip(archive, dest_dir, keep);
        case ArchiveKind::SevenZip: return extract_7z(archive, dest_dir, keep);
        case ArchiveKind::Rar: return extract_rar(archive, dest_dir, keep);
        default: r.error = "not a zip, 7z or rar archive"; return r;
    }
}
