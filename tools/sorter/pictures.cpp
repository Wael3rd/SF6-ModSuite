// SF6_ModSorter -- a mod's own picture.
//
// Which picture: authors put a screenshot next to their files (screenshot.png, 1.jpg, Logo.jpg, ...). A Fluffy-format mod
// names it in its modinfo.ini ("screenshot=..."): that one first; else a picture whose name says what it is (screenshot,
// preview, cover, ...); else the one nearest to the root, first by name. A lone .pak has none.
//
// From a folder the file is read where it is. From an archive only the wanted entries are unpacked (the pictures in the
// running, and the modinfo.ini files that may name one) into the work folder, removed at once.
//
// The small pictures made are kept as JPEG files in the cache folder, under a name made of the mod's path, size and date:
// the next time nothing is unpacked or decoded again. A mod without a picture is remembered too (an empty file).
//
// Decoding and scaling are Windows' own (WIC: PNG, JPEG, BMP, GIF, WebP when Windows has its codec).

#define NOMINMAX
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include "archive.hpp"
#include "pictures.hpp"
#include "sorter.hpp"

std::mutex& archive_mutex() {
    static std::mutex m;
    return m;
}

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

const uint32_t kUnder = 0x1B1C21;   // what shows through a picture that has transparent parts: the cells' own dark

bool ends_with(const std::wstring& s, const wchar_t* e) {
    const size_t k = wcslen(e);
    return s.size() > k && s.compare(s.size() - k, k, e) == 0;
}

bool picture_name(const std::wstring& lower) {
    return ends_with(lower, L".png") || ends_with(lower, L".jpg") || ends_with(lower, L".jpeg") || ends_with(lower, L".bmp") ||
           ends_with(lower, L".webp") || ends_with(lower, L".gif");
}

std::wstring base_of(const std::wstring& name) {
    const size_t s = name.find_last_of(L"/\\");
    return s == std::wstring::npos ? name : name.substr(s + 1);
}
std::wstring dir_of(const std::wstring& name) {   // with its last separator, "" at the root
    const size_t s = name.find_last_of(L"/\\");
    return s == std::wstring::npos ? std::wstring() : name.substr(0, s + 1);
}
std::wstring slashes(std::wstring s) {
    for (auto& c : s) if (c == L'\\') c = L'/';
    return s;
}

int compare_natural(const std::wstring& x, const std::wstring& y) {
    return CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, x.c_str(), (int)x.size(), y.c_str(), (int)y.size(),
                           nullptr, nullptr, 0) - 2;
}

// The whole of a small file
bool read_file(const std::wstring& path, std::vector<uint8_t>& out, uint64_t most) {
    HANDLE h = CreateFileW(long_path_w(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (uint64_t)sz.QuadPart <= most;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (ok && got < out.size()) {
            DWORD n = 0;
            ok = ReadFile(h, out.data() + got, (DWORD)std::min<size_t>(out.size() - got, 1u << 24), &n, nullptr) && n > 0;
            got += n;
        }
    }
    CloseHandle(h);
    return ok;
}

// "screenshot = name" of a modinfo.ini: the name, lower case, '/' separators; "" when it has none
std::wstring ini_screenshot(const std::wstring& ini_path) {
    std::vector<uint8_t> raw;
    if (!read_file(ini_path, raw, 1u << 20)) return {};
    const std::string text(raw.begin(), raw.end());
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;   // a byte order mark
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '"')) s.pop_back();
            size_t b = 0;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '"')) ++b;
            return s.substr(b);
        };
        std::string key = trim(line.substr(0, eq));
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        if (key != "screenshot") continue;
        return slashes(lower_w(from_utf8(trim(line.substr(eq + 1)))));
    }
    return {};
}

// A picture in the running
struct Cand {
    std::wstring name;                 // relative, '/' separators
    int score = 2;                     // 0: named by a modinfo.ini; 1: its name says it is the mod's picture; 2: any other
    int depth = 0;
};

void sort_cands(std::vector<Cand>& c) {
    std::sort(c.begin(), c.end(), [](const Cand& x, const Cand& y) {
        if (x.score != y.score) return x.score < y.score;
        if (x.depth != y.depth) return x.depth < y.depth;
        return compare_natural(x.name, y.name) < 0;
    });
}

// The pictures among names, best first. named: the names modinfo.ini files gave (lower case, relative to the root);
// ini_dirs: the folders that hold a modinfo.ini not read yet (their pictures go first until it is read)
std::vector<Cand> candidates(const std::vector<std::wstring>& names, const std::vector<std::wstring>& named,
                             const std::vector<std::wstring>& ini_dirs) {
    static const wchar_t* const kWords[] = { L"screenshot", L"preview", L"cover", L"thumb", L"showcase", L"banner", L"poster" };
    std::vector<Cand> out;
    for (auto& n : names) {
        const std::wstring l = lower_w(slashes(n));
        if (!picture_name(l)) continue;
        Cand c;
        c.name = slashes(n);
        c.depth = (int)std::count(l.begin(), l.end(), L'/');
        const std::wstring base = base_of(l);
        for (auto w : kWords) if (base.find(w) != std::wstring::npos) c.score = 1;
        if (std::find(named.begin(), named.end(), l) != named.end()) c.score = 0;
        else if (std::find(ini_dirs.begin(), ini_dirs.end(), dir_of(l)) != ini_dirs.end()) c.score = std::min(c.score, 1);
        out.push_back(std::move(c));
    }
    sort_cands(out);
    return out;
}

// What a folder holds, relative names with '/' (not deeper than 5 folders, not more than 4000 files: a mod's picture is near its root)
void walk(const std::wstring& root, const std::wstring& rel, int depth, std::vector<std::wstring>& names) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(long_path_w(root + L"\\" + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> dirs;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 5 && lower_w(n) != L"natives") dirs.push_back(n);   // natives: the game's files, thousands of them
        } else if (names.size() < 4000) {
            names.push_back(slashes(rel) + n);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& d : dirs) walk(root, rel + d + L"\\", depth + 1, names);
}

std::wstring to_backslashes(std::wstring s) {
    for (auto& c : s) if (c == L'/') c = L'\\';
    return s;
}

// The file's picture, its longer side at most max_side (0: as it is), over the cells' dark where it is transparent
bool decode(IWICImagingFactory* wic, const std::wstring& file, int max_side, ModPicture& out) {
    std::vector<uint8_t> raw;
    if (!wic || !read_file(file, raw, 96u << 20)) return false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICBitmapScaler* sc = nullptr;
    bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(raw.data(), (DWORD)raw.size())) &&
              SUCCEEDED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
              SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    UINT w = 0, h = 0;
    ok = ok && SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 16384 && h <= 16384;
    IWICBitmapSource* src = conv;
    UINT tw = w, th = h;
    if (ok && max_side > 0 && std::max(w, h) > (UINT)max_side) {
        const double k = double(max_side) / double(std::max(w, h));
        tw = std::max(1u, UINT(w * k + 0.5));
        th = std::max(1u, UINT(h * k + 0.5));
        ok = SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(conv, tw, th, WICBitmapInterpolationModeFant));
        src = sc;
    }
    if (ok) {
        out.w = (int)tw;
        out.h = (int)th;
        out.bgra.assign(size_t(tw) * th * 4, 0);
        ok = SUCCEEDED(src->CopyPixels(nullptr, tw * 4, (UINT)out.bgra.size(), out.bgra.data()));
    }
    if (ok) {
        const uint32_t under[3] = { kUnder & 0xFF, (kUnder >> 8) & 0xFF, (kUnder >> 16) & 0xFF };   // B, G, R
        for (size_t i = 0; i < out.bgra.size(); i += 4) {
            const uint32_t al = out.bgra[i + 3];
            if (al == 255) continue;
            for (int k = 0; k < 3; ++k) out.bgra[i + k] = (uint8_t)std::min<uint32_t>(255, out.bgra[i + k] + under[k] * (255 - al) / 255);
            out.bgra[i + 3] = 255;
        }
    }
    release(sc);
    release(conv);
    release(fr);
    release(dec);
    release(st);
    return ok;
}

bool save_jpeg(IWICImagingFactory* wic, const std::wstring& path, const ModPicture& p) {
    if (!wic || p.w <= 0 || p.h <= 0) return false;
    std::vector<uint8_t> bgr(size_t(p.w) * p.h * 3);
    for (size_t i = 0, o = 0; i < p.bgra.size(); i += 4, o += 3) { bgr[o] = p.bgra[i]; bgr[o + 1] = p.bgra[i + 1]; bgr[o + 2] = p.bgra[i + 2]; }
    const std::wstring tmp = path + L".tmp";
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* bag = nullptr;
    bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &bag));
    if (ok && bag) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.88f;
        bag->Write(1, &opt, &v);
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(fr->SetSize((UINT)p.w, (UINT)p.h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
         IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
         SUCCEEDED(fr->WritePixels((UINT)p.h, (UINT)p.w * 3, (UINT)bgr.size(), bgr.data())) && SUCCEEDED(fr->Commit()) &&
         SUCCEEDED(enc->Commit());
    release(bag);
    release(fr);
    release(enc);
    release(st);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// A name for what was made of a file: its path, size and date (and this code's version: 1)
std::wstring stamp_key(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(long_path_w(path).c_str(), GetFileExInfoStandard, &d)) return {};
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { hsh ^= b[i]; hsh *= 1099511628211ull; }
    };
    const std::wstring l = lower_w(path);
    mix("v1", 2);
    mix(l.data(), l.size() * sizeof(wchar_t));
    mix(&d.nFileSizeLow, sizeof d.nFileSizeLow);
    mix(&d.nFileSizeHigh, sizeof d.nFileSizeHigh);
    mix(&d.ftLastWriteTime, sizeof d.ftLastWriteTime);
    wchar_t hex[20];
    swprintf(hex, 20, L"%016llx", (unsigned long long)hsh);
    return hex;
}

// The entries wanted from the archive being read (this thread's)
thread_local const std::vector<std::wstring>* t_wanted = nullptr;
bool keep_wanted(const std::wstring& entry) {
    if (!t_wanted) return false;
    const std::wstring l = lower_w(slashes(entry));
    return std::find(t_wanted->begin(), t_wanted->end(), l) != t_wanted->end();
}

}  // namespace

PictureLoader::PictureLoader(const std::wstring& work_dir, const std::wstring& cache_dir) : work_dir_(work_dir), cache_dir_(cache_dir) {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    if (!cache_dir_.empty()) {
        // its folder, and the one above it
        const size_t s = cache_dir_.find_last_of(L'\\');
        if (s != std::wstring::npos) CreateDirectoryW(cache_dir_.substr(0, s).c_str(), nullptr);
        CreateDirectoryW(cache_dir_.c_str(), nullptr);
        if (!dir_exists_w(cache_dir_)) cache_dir_.clear();
    }
}

PictureLoader::~PictureLoader() { release(wic_); }

bool PictureLoader::cached(const std::wstring& key, int max_side, ModPicture& out, bool& none) {
    none = false;
    if (cache_dir_.empty() || key.empty()) return false;
    if (file_exists_w(cache_dir_ + L"\\" + key + L".none")) { none = true; return false; }
    return decode(wic_, cache_dir_ + L"\\" + key + L"_" + std::to_wstring(max_side) + L".jpg", 0, out);
}

void PictureLoader::keep(const std::wstring& key, int max_side, const ModPicture* pic) {
    if (cache_dir_.empty() || key.empty()) return;
    if (pic) { save_jpeg(wic_, cache_dir_ + L"\\" + key + L"_" + std::to_wstring(max_side) + L".jpg", *pic); return; }
    HANDLE h = CreateFileW((cache_dir_ + L"\\" + key + L".none").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

bool PictureLoader::from_folder(const std::wstring& dir, int max_side, ModPicture& out) {
    std::vector<std::wstring> names;
    walk(dir, L"", 0, names);
    std::vector<std::wstring> named;
    for (auto& n : names) {
        if (lower_w(base_of(n)) != L"modinfo.ini" || named.size() >= 8) continue;
        const std::wstring shot = ini_screenshot(dir + L"\\" + to_backslashes(n));
        if (!shot.empty()) named.push_back(lower_w(dir_of(n)) + shot);
    }
    const std::vector<Cand> cands = candidates(names, named, {});
    for (size_t i = 0; i < cands.size() && i < 4; ++i) {
        const std::wstring file = dir + L"\\" + to_backslashes(cands[i].name);
        const std::wstring key = stamp_key(file);
        bool none = false;
        if (cached(key, max_side, out, none)) return true;
        if (none) continue;
        if (decode(wic_, file, max_side, out)) { keep(key, max_side, &out); return true; }
        keep(key, max_side, nullptr);
    }
    return false;
}

bool PictureLoader::from_archive(const std::wstring& arc, int max_side, ModPicture& out) {
    const std::wstring key = stamp_key(arc);
    bool none = false;
    if (cached(key, max_side, out, none)) return true;
    if (none) return false;

    bool got = false;
    const std::wstring tmp = work_dir_ + L"\\pic" + std::to_wstring(GetCurrentThreadId()) + L"_" + std::to_wstring(++seq_);
    {
        std::lock_guard<std::mutex> lock(archive_mutex());
        std::vector<std::wstring> names;
        if (!archive_list(arc, names)) return false;   // not readable now: asked again the next time
        std::vector<std::wstring> inis, ini_dirs;
        for (auto& n : names) {
            const std::wstring l = lower_w(slashes(n));
            if (base_of(l) == L"modinfo.ini" && inis.size() < 8) { inis.push_back(l); ini_dirs.push_back(dir_of(l)); }
        }
        const std::vector<Cand> first = candidates(names, {}, ini_dirs);
        if (!first.empty() && !work_dir_.empty()) {
            std::vector<std::wstring> wanted = inis;
            for (size_t i = 0; i < first.size() && i < 4; ++i) wanted.push_back(lower_w(first[i].name));
            t_wanted = &wanted;
            const ArchiveResult r = archive_extract(arc, tmp, keep_wanted);
            t_wanted = nullptr;
            if (!r.ok && r.files_written == 0) { delete_tree(tmp); RemoveDirectoryW(tmp.c_str()); return false; }
        }
    }
    if (dir_exists_w(tmp)) {
        // what was unpacked is a small folder mod: the same choice
        std::vector<std::wstring> names;
        walk(tmp, L"", 0, names);
        std::vector<std::wstring> named;
        for (auto& n : names) {
            if (lower_w(base_of(n)) != L"modinfo.ini") continue;
            const std::wstring shot = ini_screenshot(tmp + L"\\" + to_backslashes(n));
            if (!shot.empty()) named.push_back(lower_w(dir_of(n)) + shot);
        }
        const std::vector<Cand> cands = candidates(names, named, {});
        for (size_t i = 0; i < cands.size() && !got; ++i) got = decode(wic_, tmp + L"\\" + to_backslashes(cands[i].name), max_side, out);
        delete_tree(tmp);
        RemoveDirectoryW(long_path_w(tmp).c_str());
    }
    keep(key, max_side, got ? &out : nullptr);
    return got;
}

bool PictureLoader::load(const std::wstring& mod_path, int max_side, ModPicture& out) {
    out = ModPicture();
    const DWORD at = GetFileAttributesW(long_path_w(mod_path).c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return false;
    if (at & FILE_ATTRIBUTE_DIRECTORY) return from_folder(mod_path, max_side, out);
    if (archive_sniff(mod_path) == ArchiveKind::None) return false;   // a lone .pak, a colour file: no picture
    return from_archive(mod_path, max_side, out);
}
