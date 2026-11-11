// SF6_ModSorter -- a mod's own picture: the screenshot its author put in the archive or the folder (the one a Fluffy-format
// modinfo.ini names, else the best named one, else the first), as a small picture ready to draw. See pictures.cpp.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct IWICImagingFactory;

struct ModPicture {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;         // 32-bit BGRA rows from the top, opaque
};

// One per thread that loads pictures (it holds the thread's imaging factory: COM must be initialised on that thread).
class PictureLoader {
public:
    // work_dir: where an archive's picture is unpacked for a moment; cache_dir: the small pictures already made ("" : none kept)
    PictureLoader(const std::wstring& work_dir, const std::wstring& cache_dir);
    ~PictureLoader();
    PictureLoader(const PictureLoader&) = delete;
    PictureLoader& operator=(const PictureLoader&) = delete;

    // The picture of the mod at mod_path (a folder or an archive), its longer side at most max_side. False: it has none.
    bool load(const std::wstring& mod_path, int max_side, ModPicture& out);

private:
    IWICImagingFactory* wic_ = nullptr;
    std::wstring work_dir_, cache_dir_;
    int seq_ = 0;

    bool cached(const std::wstring& key, int max_side, ModPicture& out, bool& none);
    void keep(const std::wstring& key, int max_side, const ModPicture* pic);
    bool from_folder(const std::wstring& dir, int max_side, ModPicture& out);
    bool from_archive(const std::wstring& arc, int max_side, ModPicture& out);
};

// The archive readers are the loaders' own code (one archive at a time): held by whoever reads an archive
std::mutex& archive_mutex();
