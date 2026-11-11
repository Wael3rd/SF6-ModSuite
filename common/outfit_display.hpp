// What the game shows for an extra outfit: its mod's label instead of "Outfit VII" (the loader's costume name message,
// fightercostumemessage.msg.21). Shared by the loader, which writes the message, and the plugin, which compares what a costume row
// shows. Pure (plugin/tests/names_test.cpp).
//
// A label comes from strangers' file names and modinfo.ini. The message format takes any valid UTF-8 (the loader's add_msg_entries
// refuses the whole file on anything else, so an invalid byte must never reach it), no NUL, no length limit in the format itself.
// Before it goes in: valid UTF-8 only; no control character (C0, DEL, C1), line or paragraph separator, or bidirectional control; no
// < or > (a game text would read them as message tags); no space at its ends; bounded. Nothing left: "", and the outfit keeps its
// "Outfit VII". Numbers, registry.json, outfits.json's "name" and variants.json's "slot" do not depend on this.
// A label taken from an archive's file name ("Majin Vegeta.rar") is shown without its extension (.rar, .zip, .7z). That came after
// paks were built with the extension in the name: a pak keeps the names it was built with until its next rebuild (nothing forces
// one), so a costume row is compared with both (outfit_display_matches).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

constexpr size_t kOutfitDisplayMaxBytes = 120;   // far beyond any row; a longer label ends in "..."

namespace outfit_display_detail {

// The extension of an archive at the end of a label (any case), "" when there is none or nothing would be left before it
inline size_t archive_extension(const std::string& s) {
    for (const char* ext : { ".rar", ".zip", ".7z" }) {
        size_t n = 0;
        while (ext[n]) ++n;
        if (s.size() <= n) continue;
        bool same = true;
        for (size_t k = 0; k < n && same; ++k) {
            char c = s[s.size() - n + k];
            if (c >= 'A' && c <= 'Z') c = char(c + 32);
            same = c == ext[k];
        }
        if (same) return n;
    }
    return 0;
}

inline std::string clean(const std::string& label, bool strip_extension) {
    std::string out;
    for (size_t i = 0; i < label.size();) {
        const unsigned char c = static_cast<unsigned char>(label[i]);
        const int n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!n || i + static_cast<size_t>(n) > label.size()) { ++i; continue; }   // a stray or cut byte: dropped
        uint32_t cp = n == 1 ? c : c & (0xFFu >> (n + 1));
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const unsigned char t = static_cast<unsigned char>(label[i + static_cast<size_t>(k)]);
            if ((t & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (t & 0x3Fu);
        }
        if (!ok) { ++i; continue; }
        const bool overlong = (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
        const bool invalid = (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF;
        const bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
        const bool layout = cp == 0x2028 || cp == 0x2029 || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
        if (!overlong && !invalid && !control && !layout && cp != '<' && cp != '>') out.append(label, i, static_cast<size_t>(n));
        i += static_cast<size_t>(n);
    }
    size_t first = 0;
    while (first < out.size() && out[first] == ' ') ++first;
    out.erase(0, first);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (strip_extension) {
        if (const size_t n = archive_extension(out)) {
            out.resize(out.size() - n);
            while (!out.empty() && out.back() == ' ') out.pop_back();
        }
    }
    if (out.size() > kOutfitDisplayMaxBytes) {
        size_t cut = kOutfitDisplayMaxBytes - 3;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
        out += "...";
    }
    return out;
}

}  // namespace outfit_display_detail

inline std::string outfit_display_name(const std::string& label) { return outfit_display_detail::clean(label, true); }

// What a costume row shows for this label: the name of today, or the one a pak built before it was there shows (the extension kept)
inline bool outfit_display_matches(const std::string& label, const std::string& row) {
    if (row.empty()) return false;
    return outfit_display_name(label) == row || outfit_display_detail::clean(label, false) == row;
}
