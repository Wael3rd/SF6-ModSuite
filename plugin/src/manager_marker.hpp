// SF6 Slots - the Slotsloader Manager's opt-out of the owned-outfit check (ownership.hpp), Manager mode only.
// From ZZtaii's Slotsloader fork 1.10.4 (MIT, plugin/src/marker.hpp), unchanged. Looked for only in Manager mode
// (common/manager_mode.hpp): outside it, the file is not looked for and changes nothing.
// Opt-out marker, by name hash: a file directly in the plugin's data folder whose lowercased name
// (UTF-8) has this FNV-1a 64 hash turns the check off. Only the hash is kept here.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace slots::marker {

constexpr uint64_t kOptOutNameHash = 0xbe6f7ec4026f9784ull;   // opt-out marker, by name hash

inline uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h;
}

// A file name as the marker compares it: lowercased (invariant locale), then UTF-8
inline std::string folded_name(const wchar_t* name) {
    std::wstring w(name);
    if (w.empty()) return {};
    std::wstring low(w.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), (int)w.size(),
                          &low[0], (int)low.size(), nullptr, nullptr, 0);
    if (n <= 0) low = w; else low.resize((size_t)n);
    int u = WideCharToMultiByte(CP_UTF8, 0, low.c_str(), (int)low.size(), nullptr, 0, nullptr, nullptr);
    std::string out(u > 0 ? (size_t)u : 0, '\0');
    if (u > 0) WideCharToMultiByte(CP_UTF8, 0, low.c_str(), (int)low.size(), &out[0], u, nullptr, nullptr);
    return out;
}

inline bool is_opt_out_name(const wchar_t* name) {
    return fnv1a64(folded_name(name)) == kOptOutNameHash;
}

// True when a file directly in `dir` (not in its sub-folders) is the marker
inline bool opt_out_present(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (is_opt_out_name(fd.cFileName)) { found = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

} // namespace slots::marker
