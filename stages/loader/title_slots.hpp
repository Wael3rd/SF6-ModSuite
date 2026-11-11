#pragma once
// Title screen packs: which pack is chosen and which of its files are written (pure logic, no game, no Windows:
// stagetest --selftest). The stage loader does the rest (stage_loader.cpp).
//
// A title pack is a Fluffy-style mod dropped as it is in reframework\title_mods (an archive or a folder holding
// modinfo.ini and a natives tree or a .pak, or a .pak alone). Its files are of two kinds, by their natives path:
//   title files    the title screen's movies, natives/stm/streaming/product/gui/gm/ui10000/part/title_*.mov.1
//                  (any path holding "/gui/gm/ui10000/part/title_")
//   startup files  every other file: the boot logos and the health warning skipped (ui10001 / ui10004 gui, the logo
//                  textures and movie, re_logo.sbnk)
// The chosen pack's title files are written into the stage pak at their real paths. With skip_intro, startup files
// are written too: the chosen pack's, or the first pack's that has some (registry order) when the chosen pack has
// none or the game's own title is chosen. Nothing else.
//
// Settings, in the plugin's reframework\data\SF6_CostumeSlots_data\settings.json (other keys ignored):
//   "title_pack": "" or missing = the game's own title; a pack's key; "random" = one pack drawn at each launch
//   "skip_intro": true / false, missing = false

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace title_slots {

struct Settings {
    std::string pack;            // "title_pack" as written: "", a key or "random"
    bool skip_intro = false;
    bool has_pool = false;       // "title_random_pool" present: the backgrounds Random draws from (keys; "game" the game's own)
    std::vector<std::string> pool;
};

struct Pack {
    std::string key, name, author;
    std::string source;          // where it came from, relative to title_mods ("Pack.zip", "Folder/Option")
    size_t title_files = 0, startup_files = 0;
    bool preview = false;        // its picture is in the stage pak at preview_resource(key)
};

// Where a pack's picture is in the stage pak (natives/stm/<this>.<tex version>), as the game loads it
inline std::string preview_resource(const std::string& key) { return "_titleslots/" + key + "/preview.tex"; }
// The game's own title's picture: reframework\data\SF6_TitleSlots_Data\game_title.png (or .jpg / .jpeg), in the pak under this key
// (a pack's key is "t" and 12 hex digits: never this)
constexpr const char* kGamePictureKey = "game";
// Random's picture: the mosaic of every pack's picture, in the pak under this key
constexpr const char* kRandomPictureKey = "random";

// A background's copy of a title file in the stage pak: natives/stm/_titleslots/<key>/<the path after natives/stm/>, a streamed
// file natives/stm/streaming/_titleslots/<key>/<the path after natives/stm/streaming/>. key: a pack's, or kGamePictureKey for the
// game's own. Used by the switch on a return to the title (stage_redirect's title table, by hash) and, being where the engine
// looks for a streamed resource of path "_titleslots/<key>/product/...", by the core's title.cpp, which gives the title's
// movies those resources while the title stays on screen.
inline std::string private_title_path(const std::string& key, const std::string& path) {
    const std::string root = "natives/stm/", streaming = "natives/stm/streaming/";
    if (path.compare(0, streaming.size(), streaming) == 0) return streaming + "_titleslots/" + key + "/" + path.substr(streaming.size());
    return root + "_titleslots/" + key + "/" + (path.compare(0, root.size(), root) == 0 ? path.substr(root.size()) : path);
}
inline std::string thumb_resource(const std::string& key) { return "_titleslots/" + key + "/thumb.tex"; }
inline std::string band_resource(const std::string& key) { return "_titleslots/" + key + "/band.tex"; }
inline std::string check_resource(bool on) { return on ? "_titleslots/ui/check_on.tex" : "_titleslots/ui/check_off.tex"; }

// A streamed resource is two files: the data under natives/stm/streaming/ and a small resource file at the same path without
// "streaming/" (a movie's: 38 bytes, "REMV", version 1, "dummy.mp4", the same for the three title movies, read 08/11). Its
// copy must sit beside the private data, or a movie given that resource waits for it forever (the game froze, 08/11).
// "" when path is not a streamed file.
inline std::string resource_file_of(const std::string& streamed_path) {
    const std::string root = "natives/stm/", streaming = "natives/stm/streaming/";
    if (streamed_path.compare(0, streaming.size(), streaming) != 0) return {};
    return root + streamed_path.substr(streaming.size());
}

struct Choice {
    int title = -1;              // pack whose title files are written; -1: the game's own title
    int startup = -1;            // pack whose startup files are written (skip_intro); -1: none
    bool random = false;         // the settings asked "random" (title is the pack drawn)
    bool unknown = false;        // the settings named a key that no pack has: the game's own title
};

namespace detail {

inline std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

inline uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;   // the FNV-1a 64 offset basis (stage_loader.cpp's fnv64 lacks its last digit: kept there, its keys depend on it)
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

inline void skip_ws(const std::string& t, size_t& i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
}

// The JSON string whose opening quote is at t[i]: its value (escapes decoded, \u to UTF-8), i past its closing quote.
inline bool read_string(const std::string& t, size_t& i, std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    out.clear();
    for (++i; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '"') { ++i; return true; }
        if (c != '\\') { out += c; continue; }
        if (++i >= t.size()) return false;
        switch (t[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 >= t.size()) return false;
                unsigned cp = 0;
                for (int k = 1; k <= 4; ++k) {
                    const char h = t[i + k];
                    cp = cp * 16 + unsigned(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                            : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                i += 4;
                if (cp < 0x80) out += char(cp);
                else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out += t[i];   // \" \\ \/
        }
    }
    return false;
}

// Steps over any JSON value at t[i] (strings, numbers, true / false / null, nested objects and arrays).
inline bool skip_value(const std::string& t, size_t& i) {
    skip_ws(t, i);
    if (i >= t.size()) return false;
    std::string s;
    if (t[i] == '"') return read_string(t, i, s);
    if (t[i] == '{' || t[i] == '[') {
        int depth = 0;
        while (i < t.size()) {
            const char c = t[i];
            if (c == '"') { if (!read_string(t, i, s)) return false; continue; }
            if (c == '{' || c == '[') ++depth;
            else if ((c == '}' || c == ']') && --depth == 0) { ++i; return true; }
            ++i;
        }
        return false;
    }
    const size_t start = i;
    while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && t[i] != ' ' && t[i] != '\t' && t[i] != '\r' && t[i] != '\n') ++i;
    return i > start;
}

}  // namespace detail

// settings.json's "title_pack" and "skip_intro", at the top level of its object. Read tolerantly: a text that is not a
// JSON object gives the defaults, a key of another type keeps its default, a text cut short keeps what came before.
inline Settings parse_settings(std::string t) {
    Settings s;
    if (t.size() >= 3 && (uint8_t)t[0] == 0xEF && (uint8_t)t[1] == 0xBB && (uint8_t)t[2] == 0xBF) t.erase(0, 3);
    size_t i = 0;
    detail::skip_ws(t, i);
    if (i >= t.size() || t[i] != '{') return s;
    ++i;
    for (;;) {
        detail::skip_ws(t, i);
        std::string key, v;
        if (i >= t.size() || t[i] == '}' || !detail::read_string(t, i, key)) break;
        detail::skip_ws(t, i);
        if (i >= t.size() || t[i] != ':') break;
        ++i;
        detail::skip_ws(t, i);
        if (key == "title_pack" && i < t.size() && t[i] == '"') {
            if (!detail::read_string(t, i, v)) break;
            s.pack = v;
        } else if (key == "skip_intro" && t.compare(i, 4, "true") == 0) {
            s.skip_intro = true;
            i += 4;
        } else if (key == "skip_intro" && t.compare(i, 5, "false") == 0) {
            s.skip_intro = false;
            i += 5;
        } else if (key == "title_random_pool" && i < t.size() && t[i] == '[') {
            s.has_pool = true;
            ++i;
            for (;;) {
                detail::skip_ws(t, i);
                if (i >= t.size() || t[i] == ']') { if (i < t.size()) ++i; break; }
                if (t[i] == ',') { ++i; continue; }
                std::string k;
                if (!detail::read_string(t, i, k)) break;
                if (!k.empty()) s.pool.push_back(detail::lower(k));
            }
        } else if (!detail::skip_value(t, i)) {
            break;
        }
        detail::skip_ws(t, i);
        if (i >= t.size() || t[i] != ',') break;
        ++i;
    }
    return s;
}

// A pack's key: "t" + the first 12 hex digits of the FNV-1a 64 hash of its source in lower case. Not its size or date:
// the choice outlives a new download of the same file.
inline std::string pack_key(const std::string& source) {
    char b[17];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)detail::fnv1a64(detail::lower(source)));
    return "t" + std::string(b, 12);
}

// The natives paths a title pack's files have (every title pack on PC3, 11/11, holds these twelve and no other). A pak keeps
// only the hashes of its paths: its entries are named from these and from the paths of the packs shipped as files; an entry
// named by neither is a startup file (a startup file is written at its own hash, its path is never needed).
inline const std::vector<std::string>& known_paths() {
    static const std::vector<std::string> paths = {
        "natives/stm/streaming/product/gui/gm/ui10000/part/title_start.mov.1",
        "natives/stm/streaming/product/gui/gm/ui10000/part/title_loop.mov.1",
        "natives/stm/streaming/product/gui/gm/ui10000/part/title_end.mov.1",
        "natives/stm/streaming/product/gui/gm/ui10000/part/re logo animation.mov.1.x64",
        "natives/stm/product/sound/wwise/re_logo.sbnk.1.x64",
        "natives/stm/product/gui/gm/ui10000/ui10001.gui.530035",
        "natives/stm/product/gui/gm/ui10000/ui10004.gui.530035",
        "natives/stm/product/gui/gm/ui10000/part/tex_caplogo.tex.241101895",
        "natives/stm/product/gui/gm/ui10000/part/tex_havok_logo_im.tex.241101895",
        "natives/stm/product/gui/gm/ui10000/part/tex_megascans_logo_im.tex.241101895",
        "natives/stm/product/gui/gm/ui10000/part/tex_microsoft_logo_im.tex.241101895",
        "natives/stm/product/gui/gm/ui10000/part/tex_st_logo_im.tex.241101895",
    };
    return paths;
}

// A title file (the title screen's movies) rather than a startup file; path: the file's natives path, any case.
inline bool is_title_file(const std::string& natives_path) {
    return detail::lower(natives_path).find("/gui/gm/ui10000/part/title_") != std::string::npos;
}

// Registry order: by name, case-insensitive, then by key.
inline bool pack_before(const Pack& a, const Pack& b) {
    const std::string x = detail::lower(a.name), y = detail::lower(b.name);
    if (x != y) return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
                                                    [](char p, char q) { return (unsigned char)p < (unsigned char)q; });
    return a.key < b.key;
}

// What is written, given the packs in registry order, the settings and a random number (used for "random" only: one of
// the packs that have title files). Keys and "random" are compared case-insensitively.
inline Choice resolve_choice(const std::vector<Pack>& packs, const Settings& s, uint64_t random) {
    Choice c;
    const std::string want = detail::lower(detail::trim(s.pack));
    if (want == "random") {
        c.random = true;
        std::vector<int> pool;
        for (size_t i = 0; i < packs.size(); ++i) if (packs[i].title_files) pool.push_back(int(i));
        if (!pool.empty()) c.title = pool[size_t(random % pool.size())];
    } else if (!want.empty()) {
        for (size_t i = 0; i < packs.size() && c.title < 0; ++i) if (detail::lower(packs[i].key) == want) c.title = int(i);
        c.unknown = c.title < 0;
    }
    if (s.skip_intro) {
        if (c.title >= 0 && packs[size_t(c.title)].startup_files) c.startup = c.title;
        for (size_t i = 0; i < packs.size() && c.startup < 0; ++i) if (packs[i].startup_files) c.startup = int(i);
    }
    return c;
}

// What Random draws from: every background in `all` ("game" and the packs' keys) that the selection keeps; without a selection, every
// pack (the game's own title only when chosen, Wael 08/11). The selection's order does not matter; `all`'s does.
inline std::vector<std::string> random_selection(const std::vector<std::string>& all, bool has_pool, const std::vector<std::string>& pool) {
    std::vector<std::string> out;
    for (auto& k : all) {
        const bool keep = has_pool ? std::find(pool.begin(), pool.end(), k) != pool.end() : k != kGamePictureKey;
        if (keep) out.push_back(k);
    }
    return out;
}

// ---- Random without repeats (Wael 08/11): a shuffled bag ----
// The keys still to show this round, in order, and the last one shown. bag_next gives the next key and takes it out; when the bag is
// empty it is refilled with every key of `pool`, shuffled (Fisher-Yates on rnd()), the first of the new round never the last one
// shown when there are two keys or more. Keys no longer in the pool are dropped. Kept in SF6_TitleSlots_Data\shuffle.json across
// launches: the launch's draw, each return to the title and each cycle on the title screen all take from it.
struct Bag {
    std::vector<std::string> left;
    std::string last;
};
template <class Rng>
std::string bag_next(Bag& b, const std::vector<std::string>& pool, Rng&& rnd) {
    if (pool.empty()) return {};
    b.left.erase(std::remove_if(b.left.begin(), b.left.end(),
                                [&](const std::string& k) { return std::find(pool.begin(), pool.end(), k) == pool.end(); }),
                 b.left.end());
    if (b.left.empty()) {
        b.left = pool;
        for (size_t i = b.left.size(); i > 1; --i) std::swap(b.left[i - 1], b.left[size_t(rnd() % i)]);
        if (b.left.size() > 1 && b.left.front() == b.last) std::swap(b.left.front(), b.left.back());
    }
    std::string k = b.left.front();
    b.left.erase(b.left.begin());
    b.last = k;
    return k;
}
inline std::string bag_text(const Bag& b) {
    std::string out = "{\n  \"last\": \"" + b.last + "\",\n  \"left\": [";
    for (size_t i = 0; i < b.left.size(); ++i) out += (i ? ", \"" : "\"") + b.left[i] + "\"";
    return out + "]\n}\n";
}
inline Bag parse_bag(const std::string& t) {
    Bag b;
    size_t i = t.find("\"last\"");
    if (i != std::string::npos) {
        i = t.find(':', i);
        if (i != std::string::npos) { ++i; detail::skip_ws(t, i); detail::read_string(t, i, b.last); }
    }
    i = t.find("\"left\"");
    if (i != std::string::npos) i = t.find('[', i);
    if (i == std::string::npos) return b;
    ++i;
    for (;;) {
        detail::skip_ws(t, i);
        if (i >= t.size() || t[i] == ']') break;
        if (t[i] == ',') { ++i; continue; }
        std::string k;
        if (!detail::read_string(t, i, k)) break;
        if (!k.empty()) b.left.push_back(k);
    }
    return b;
}

}  // namespace title_slots
