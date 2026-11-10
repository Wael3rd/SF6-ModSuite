#pragma once
// UI packs: the tree Fluffy Mod Manager makes of an interface mod's options, the keys, the private paths, the files served for
// a set of ticked options, settings.json's "ui_choices" and the registry's text (pure logic, no game, no Windows:
// stagetest --selftest). The stage loader does the rest (stage_loader.cpp), the redirect serves the choice (stage_redirect.cpp).
//
// A UI pack is one or more Fluffy-style mods dropped as downloaded in reframework\ui_mods. Every folder holding a modinfo.ini or
// a natives tree is a node. Fluffy's rules, as deduced from the mods (FG Artwork, Comic Art, Loading Animations, 10/11):
//   - DummyMod=True: a title (no box), its files unused. A node with no file is a title too (nothing to tick).
//   - AddonFor=<name> hangs the node under the node of that name (trimmed, any case), across archives; a node of the same archive
//     is preferred when several have the name. No AddonFor, or a name no node has: a root. A root and its sub tree are a pack.
//   - Under one parent, options that write a file in common (transitively) are a one-of group ("Choose one", none allowed);
//     the others are check boxes.
//   - Roots that are alternatives of each other (the Loading Animations archives, each its own root): their trees' files nearly
//     the same, a Jaccard index of 1/2 or more pair by pair (then transitively), are gathered into one pack, where they are a
//     one-of group by the same rule. Its name: the words their names start with, else those they end with, else a category they
//     all have, else the first name. Roots that only share some files stay packs of their own.
//   - Display order under a parent: the nodes without children by MenuPriority descending then name, then those with children by
//     name. Packs: by name, any case.
//   - Ticked options that write the same file: the one in the later pack wins it (two packs that share files), then the deepest in
//     the tree, then the later in display order (a preset, an option whose files other options also have, is a box: its files are
//     taken where nothing deeper is ticked).
//
// Choices, in the plugin's reframework\data\SF6_CostumeSlots_data\settings.json (other keys ignored):
//   "ui_choices": { "<pack key>": ["<ticked node id>", ...], ... }   a pack missing: nothing ticked, the game's own files
// An id may be "<node id>@<fid>": that option for one fighter only (its files under character_image/<fid>/). It is kept when the
// node is an option that has files under that folder, and dropped when the whole id is ticked too (the whole covers it).
//
// In the stage pak: every option's files at private paths (private_path), the launch's winner of each real file at its real path
// too (the game reads some files once at boot and keeps them: the character select's portraits), and the game's own copy of each
// real path written that way (game_path), so a choice changed in the game switches live both ways (serve).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "title_slots.hpp"   // its detail helpers: case, trim, FNV-1a, the JSON reader

namespace ui_slots {

namespace d = title_slots::detail;

// One modinfo.ini as Fluffy reads it: keys in any case, spaces round keys and values trimmed, category may repeat
struct ModInfo {
    std::string name, author, description, screenshot, addon_for, version, bundle;
    std::vector<std::string> categories;
    bool dummy = false;
    int priority = 0;            // MenuPriority
};

namespace detail {

inline bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (n < 0 || i + size_t(n) >= s.size()) return false;
        for (int k = 1; k <= n; ++k) if (((unsigned char)s[i + k] >> 6) != 0x2) return false;
        i += size_t(n) + 1;
    }
    return true;
}

inline void put_utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
}

inline std::string json(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}

inline std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t e = s.find(' ', i);
        if (e == std::string::npos) e = s.size();
        if (e > i) out.push_back(s.substr(i, e - i));
        i = e;
    }
    return out;
}

// The words cut from names may end on a separator ("Comic Art -") or an opening bracket
inline std::string trim_name(std::string s) {
    while (!s.empty() && std::string(" -:|,;/(").find(s.back()) != std::string::npos) s.pop_back();
    while (!s.empty() && std::string(" -:|,;/").find(s.front()) != std::string::npos) s.erase(0, 1);
    return s;
}

// A small union-find
struct Sets {
    std::vector<size_t> up;
    explicit Sets(size_t n) : up(n) { std::iota(up.begin(), up.end(), size_t(0)); }
    size_t find(size_t x) { while (up[x] != x) x = up[x] = up[up[x]]; return x; }
    void join(size_t a, size_t b) { a = find(a); b = find(b); if (a != b) up[a < b ? b : a] = a < b ? a : b; }   // no std::max: windows.h's macro
};

}  // namespace detail

// A modinfo.ini written in Windows-1252 (Ryu's with a typographic apostrophe, Loading Animations) made UTF-8; UTF-8 kept as it is
inline std::string to_utf8(const std::string& s) {
    if (detail::valid_utf8(s)) return s;
    static const unsigned cp1252[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
        0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178 };
    std::string o;
    for (unsigned char c : s) detail::put_utf8(o, c < 0x80 ? c : c < 0xA0 ? cp1252[c - 0x80] : c);
    return o;
}

inline ModInfo parse_modinfo(std::string t) {
    ModInfo m;
    if (t.size() >= 3 && (uint8_t)t[0] == 0xEF && (uint8_t)t[1] == 0xBB && (uint8_t)t[2] == 0xBF) t.erase(0, 3);
    t = to_utf8(t);
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        const std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = d::lower(d::trim(line.substr(0, eq))), v = d::trim(line.substr(eq + 1));
        if (k.empty() || k[0] == ';' || k[0] == '#') continue;
        if (k == "name") { if (!v.empty()) m.name = v; }
        else if (k == "author") m.author = v;
        else if (k == "description") m.description = v;
        else if (k == "screenshot") m.screenshot = v;
        else if (k == "addonfor") m.addon_for = v;
        else if (k == "dummymod") m.dummy = d::lower(v) == "true" || v == "1";
        else if (k == "category") { if (!v.empty()) m.categories.push_back(v); }
        else if (k == "menupriority") m.priority = int(strtol(v.c_str(), nullptr, 10));
        else if (k == "nameasbundle") m.bundle = v;
        else if (k == "version") m.version = v;
    }
    return m;
}

// ---- Keys and paths ----
// From "<source>/<node folder>" in lower case (the archive or folder of ui_mods, then the folders inside), FNV-1a 64, 12 hex digits:
// a choice outlives a new download of the same file (its size and date are not part of it)
inline std::string hex12(const std::string& path) {
    char b[17];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)d::fnv1a64(d::lower(path)));
    return std::string(b, 12);
}
inline std::string pack_key(const std::string& root_path) { return "u" + hex12(root_path); }
inline std::string node_id(const std::string& node_path) { return "n" + hex12(node_path); }
// The game's own copies (a pack's key is "u" and 12 hex digits: never this)
constexpr const char* kGameKey = "game";

// natives/stm/<folder>/<the path after natives/stm/>, a streamed file natives/stm/streaming/<folder>/<the path after
// natives/stm/streaming/> (as title_slots::private_title_path)
inline std::string under(const std::string& folder, const std::string& path) {
    const std::string root = "natives/stm/", streaming = "natives/stm/streaming/", low = d::lower(path);
    if (low.compare(0, streaming.size(), streaming) == 0) return streaming + folder + "/" + path.substr(streaming.size());
    return root + folder + "/" + (low.compare(0, root.size(), root) == 0 ? path.substr(root.size()) : path);
}
// An option's copy of one of its files, and the game's own copy of a real path
inline std::string private_path(const std::string& pack, const std::string& node, const std::string& path) {
    return under("_uislots/" + pack + "/" + node, path);
}
inline std::string game_path(const std::string& path) { return under(std::string("_uislots/") + kGameKey, path); }
// The pictures, as the game's resource API takes them (in the pak: natives/stm/<this>.241101895)
inline std::string node_preview(const std::string& pack, const std::string& node) { return "_uislots/" + pack + "/" + node + "/preview.tex"; }
inline std::string pack_preview(const std::string& pack) { return "_uislots/" + pack + "/preview.tex"; }

// ---- The tree ----
// A node as found: path "<source>/<node folder>" (collect_options' rel), its modinfo, its files' real hashes
struct Found {
    std::string path;
    ModInfo info;
    std::vector<uint64_t> files;
};

struct Node {
    std::string id, parent;      // parent: "" for a root
    std::string name, description, path;
    bool title = false;          // no box
    bool one = false;            // an option of a one-of group (else a check box)
    std::string group;           // that group's id: its parent's id; the pack's key for gathered roots; a second group under the
                                 // same parent: its first member's id
    int depth = 0;               // 0: a root
    size_t files = 0;
    size_t found = 0;            // its index in build_packs' input
    bool preview = false;        // the loader made its picture
    std::vector<std::string> fids;   // the fighter folders (fighter_of_path) of its listed files, sorted: what a "<id>@<fid>" choice may name
    std::map<std::string, std::vector<std::string>> fighters;   // fid -> the real texture resources of its files under that fighter
                                                                // (held copies only, sorted; empty list when none is a texture)
    bool fighters_only = false;  // every held file is under a fighter folder (and it has at least one)
};

struct Pack {
    std::string key, name, author;
    bool gathered = false;       // several roots that write the same files
    bool picture = false;        // the loader made its picture
    std::vector<Node> nodes;     // depth first, display order
};

// The packs of every node found, in registry order (by name, any case, then key). notes: what was not as expected (an AddonFor
// that names no node, a loop of AddonFor, a DummyMod with files, a pack with nothing to tick), for the log.
inline std::vector<Pack> build_packs(const std::vector<Found>& in, std::vector<std::string>* notes = nullptr) {
    const size_t n = in.size();
    auto note = [&](const std::string& s) { if (notes) notes->push_back(s); };
    auto label = [&](size_t i) { return "\"" + in[i].info.name + "\" (" + in[i].path + ")"; };
    std::vector<std::string> lpath(n), lname(n), src(n);
    for (size_t i = 0; i < n; ++i) {
        lpath[i] = d::lower(in[i].path);
        lname[i] = d::lower(d::trim(in[i].info.name));
        src[i] = lpath[i].substr(0, lpath[i].find('/'));
    }
    std::vector<size_t> ord(n);
    std::iota(ord.begin(), ord.end(), size_t(0));
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return lpath[a] < lpath[b]; });

    std::map<std::string, std::vector<size_t>> by_name;
    for (size_t i : ord) by_name[lname[i]].push_back(i);
    std::vector<long> parent(n, -1);
    for (size_t i : ord) {
        const std::string want = d::lower(d::trim(in[i].info.addon_for));
        if (want.empty()) continue;
        long pick = -1, any = -1;
        auto it = by_name.find(want);
        if (it != by_name.end()) {
            for (size_t c : it->second) {
                if (c == i) continue;
                if (any < 0) any = long(c);
                if (src[c] == src[i]) { pick = long(c); break; }
            }
        }
        if (pick < 0) pick = any;
        if (pick < 0) { note(label(i) + ": AddonFor \"" + d::trim(in[i].info.addon_for) + "\" names no node, a root"); continue; }
        parent[i] = pick;
    }
    for (size_t i : ord) {                                  // a loop of AddonFor: cut where it is met first
        long p = parent[i];
        for (size_t steps = 0; p >= 0 && steps <= n; ++steps) {
            if (size_t(p) == i) { parent[i] = -1; note(label(i) + ": its AddonFor loops back to it, a root"); break; }
            p = parent[size_t(p)];
        }
    }

    std::vector<bool> title(n);
    std::vector<std::vector<uint64_t>> files(n);
    std::vector<std::vector<size_t>> children(n);
    std::vector<size_t> roots;
    for (size_t i : ord) {
        title[i] = in[i].info.dummy || in[i].files.empty();
        if (in[i].info.dummy && !in[i].files.empty()) note(label(i) + ": DummyMod, its " + std::to_string(in[i].files.size()) + " file(s) not used");
        if (!title[i]) {
            files[i] = in[i].files;
            std::sort(files[i].begin(), files[i].end());
            files[i].erase(std::unique(files[i].begin(), files[i].end()), files[i].end());
        }
        if (parent[i] >= 0) children[size_t(parent[i])].push_back(i);
        else roots.push_back(i);
    }
    auto before = [&](size_t a, size_t b) {
        const bool ga = !children[a].empty(), gb = !children[b].empty();
        if (ga != gb) return !ga;
        if (!ga && in[a].info.priority != in[b].info.priority) return in[a].info.priority > in[b].info.priority;
        if (lname[a] != lname[b]) return lname[a] < lname[b];
        return lpath[a] < lpath[b];
    };
    for (auto& c : children) std::stable_sort(c.begin(), c.end(), before);

    std::vector<size_t> root_of(n);
    for (size_t i = 0; i < n; ++i) {
        size_t r = i;
        while (parent[r] >= 0) r = size_t(parent[r]);
        root_of[i] = r;
    }
    // Roots that are alternatives of each other: one pack. Alternatives write nearly the same files: the Jaccard index of their
    // trees' files (shared / either) is 1/2 or more, pair by pair, then transitively. The 9 Loading archives (one texture each, one
    // with a second file) are; Round Announcement UI, which ships one loading texture among its many files, and two portrait packs
    // that both have battle HUD portraits are not (a pack each; in game 10/11, sharing any file gathered far too much).
    std::vector<std::vector<uint64_t>> tree(n);
    for (size_t i = 0; i < n; ++i) tree[root_of[i]].insert(tree[root_of[i]].end(), files[i].begin(), files[i].end());
    std::map<uint64_t, std::vector<size_t>> holders;    // file -> the roots whose trees have it
    for (size_t r : roots) {
        auto& t = tree[r];
        std::sort(t.begin(), t.end());
        t.erase(std::unique(t.begin(), t.end()), t.end());
        for (uint64_t h : t) holders[h].push_back(r);
    }
    std::map<std::pair<size_t, size_t>, bool> pairs;    // roots that share a file (no std::min / max: windows.h's macros)
    for (auto& [h, rs] : holders)
        for (size_t a = 0; a < rs.size(); ++a)
            for (size_t b = a + 1; b < rs.size(); ++b) pairs[rs[a] < rs[b] ? std::make_pair(rs[a], rs[b]) : std::make_pair(rs[b], rs[a])] = false;
    detail::Sets sets(n);
    for (auto& [pr, alike] : pairs) {
        const auto& x = tree[pr.first];
        const auto& y = tree[pr.second];
        size_t shared = 0;
        for (size_t i = 0, j = 0; i < x.size() && j < y.size();) {
            if (x[i] < y[j]) ++i;
            else if (y[j] < x[i]) ++j;
            else { ++shared; ++i; ++j; }
        }
        alike = 2 * shared >= x.size() + y.size() - shared;
        if (alike) sets.join(pr.first, pr.second);
    }
    std::map<size_t, std::vector<size_t>> by_set;
    for (size_t r : roots) by_set[sets.find(r)].push_back(r);

    // One-of groups among siblings: options sharing files, transitively
    std::vector<bool> one(n);
    std::vector<std::string> group(n), id(n);
    for (size_t i = 0; i < n; ++i) id[i] = node_id(in[i].path);
    auto group_siblings = [&](const std::vector<size_t>& sibs, const std::string& parent_id) {
        detail::Sets s(sibs.size());
        std::map<uint64_t, size_t> first;
        for (size_t k = 0; k < sibs.size(); ++k) {
            for (uint64_t h : files[sibs[k]]) {
                auto it = first.find(h);
                if (it == first.end()) first[h] = k;
                else s.join(it->second, k);
            }
        }
        std::map<size_t, size_t> size;
        for (size_t k = 0; k < sibs.size(); ++k) if (!title[sibs[k]]) ++size[s.find(k)];
        bool parent_used = false;
        std::map<size_t, std::string> gid;
        for (size_t k = 0; k < sibs.size(); ++k) {
            const size_t i = sibs[k], c = s.find(k);
            if (title[i] || size[c] < 2) continue;
            auto g = gid.find(c);
            if (g == gid.end()) g = gid.emplace(c, parent_used ? id[i] : parent_id).first;
            parent_used = true;
            one[i] = true;
            group[i] = g->second;
        }
    };

    std::vector<Pack> packs;
    for (auto& [set, rs] : by_set) {
        std::vector<size_t> rlist = rs;
        std::stable_sort(rlist.begin(), rlist.end(), before);
        size_t options = 0;
        for (size_t i = 0; i < n; ++i) if (!title[i] && sets.find(root_of[i]) == set) ++options;
        if (!options) {
            for (size_t r : rlist) note(label(r) + ": nothing to tick (no file in a natives tree), left out");
            continue;
        }
        Pack p;
        p.gathered = rlist.size() > 1;
        size_t first_path = rlist[0];
        for (size_t r : rlist) if (lpath[r] < lpath[first_path]) first_path = r;
        p.key = pack_key(in[first_path].path);
        if (!p.gathered) {
            p.name = d::trim(in[rlist[0]].info.name);
            p.author = in[rlist[0]].info.author;
        } else {
            // the words every name starts with, else ends with, else a category they all have, else the first name
            std::vector<std::vector<std::string>> w;
            for (size_t r : rlist) w.push_back(detail::words(in[r].info.name));
            auto same = [](const std::string& a, const std::string& b) { return d::lower(a) == d::lower(b); };
            size_t pre = w[0].size(), suf = w[0].size();
            for (auto& x : w) {
                size_t k = 0;
                while (k < pre && k < x.size() && same(x[k], w[0][k])) ++k;
                pre = k;
                k = 0;
                while (k < suf && k < x.size() && same(x[x.size() - 1 - k], w[0][w[0].size() - 1 - k])) ++k;
                suf = k;
            }
            auto join = [](const std::vector<std::string>& v, size_t a, size_t b) {
                std::string s;
                for (size_t k = a; k < b; ++k) s += (s.empty() ? "" : " ") + v[k];
                return detail::trim_name(s);
            };
            p.name = join(w[0], 0, pre);
            if (p.name.size() < 3) p.name = join(w[0], w[0].size() - suf, w[0].size());
            if (p.name.size() < 3) {
                p.name.clear();
                for (auto& c : in[rlist[0]].info.categories) {
                    bool all = true;
                    for (size_t r : rlist) {
                        bool has = false;
                        for (auto& c2 : in[r].info.categories) has = has || same(d::trim(c), d::trim(c2));
                        all = all && has;
                    }
                    if (!all) continue;
                    std::string last = c.substr(c.find_last_of('>') == std::string::npos ? 0 : c.find_last_of('>') + 1);
                    last = d::trim(last);
                    while (!last.empty() && last[0] == '!') last.erase(0, 1);
                    p.name = d::trim(last);
                    break;
                }
            }
            if (p.name.empty()) p.name = d::trim(in[rlist[0]].info.name);
            for (size_t r : rlist) {
                const std::string a = d::trim(in[r].info.author);
                if (!a.empty() && (", " + d::lower(p.author) + ", ").find(", " + d::lower(a) + ", ") == std::string::npos)
                    p.author += (p.author.empty() ? "" : ", ") + a;
            }
        }
        group_siblings(rlist, p.key);
        for (size_t i = 0; i < n; ++i) if (sets.find(root_of[i]) == set && !children[i].empty()) group_siblings(children[i], id[i]);
        // depth first, display order
        std::vector<std::pair<size_t, int>> stack;
        for (auto it = rlist.rbegin(); it != rlist.rend(); ++it) stack.push_back({ *it, 0 });
        while (!stack.empty()) {
            const auto [i, depth] = stack.back();
            stack.pop_back();
            Node nd;
            nd.id = id[i];
            nd.parent = parent[i] >= 0 ? id[size_t(parent[i])] : std::string();
            nd.name = d::trim(in[i].info.name);
            nd.description = in[i].info.description;
            nd.path = in[i].path;
            nd.title = title[i];
            nd.one = one[i];
            nd.group = group[i];
            nd.depth = depth;
            nd.files = files[i].size();
            nd.found = i;
            p.nodes.push_back(std::move(nd));
            for (auto c = children[i].rbegin(); c != children[i].rend(); ++c) stack.push_back({ *c, depth + 1 });
        }
        packs.push_back(std::move(p));
    }
    std::stable_sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) {
        const std::string x = d::lower(a.name), y = d::lower(b.name);
        if (x != y) return x < y;
        return a.key < b.key;
    });
    // Separate packs that have files in common: the later one wins them (winners), said once per pair
    std::map<uint64_t, std::vector<size_t>> in_pack;
    for (size_t k = 0; k < packs.size(); ++k) {
        for (auto& nd : packs[k].nodes)
            for (uint64_t h : files[nd.found]) {
                auto& v = in_pack[h];
                if (v.empty() || v.back() != k) v.push_back(k);
            }
    }
    std::map<std::pair<size_t, size_t>, size_t> common;
    for (auto& [h, ks] : in_pack)
        for (size_t a = 0; a < ks.size(); ++a)
            for (size_t b = a + 1; b < ks.size(); ++b) ++common[{ ks[a], ks[b] }];
    for (auto& [pr, count] : common)
        note("\"" + packs[pr.first].name + "\" and \"" + packs[pr.second].name + "\" both have " + std::to_string(count) +
             " file(s): \"" + packs[pr.second].name + "\" wins them when both are ticked (the later by name)");
    return packs;
}

// ---- Choices and what is served ----
using Choices = std::map<std::string, std::vector<std::string>>;   // pack key -> ticked node ids (lower case)
using HashMap = std::vector<std::pair<uint64_t, uint64_t>>;         // sorted by its first hash, one entry per first hash

// An option as the redirect knows it: real hash -> its copy's hash, for the files the pak holds
struct Served {
    std::string pack, id;
    int depth = 0, order = 0;    // order: its place in its pack's list
    HashMap files;
    int rank = 0;                // its pack's place in the registry order (by name): a later pack wins a file two packs have
    std::map<uint64_t, std::string> fid;   // real hash -> its fighter folder, for the files under one (a "<id>@<fid>" choice takes these)
};

// What the loader gives the redirect (stage_loader_ui)
struct Tables {
    bool present = false;        // reframework\ui_mods existed at launch
    std::vector<Pack> packs;
    std::vector<Served> options;
    Choices launch;              // the ticked options this launch wrote at the real paths
    HashMap written;             // real hash -> the copy of the option written there (the launch's winners)
    HashMap game;                // real hash -> the game's own copy, for the real paths written
    std::map<uint64_t, std::string> names;   // the textures' hashes (real, copies) -> their resource paths (texture_resource)
    std::map<std::string, std::string> game_textures;   // real fighter texture resource -> the resource of the game's own copy
};

// A texture's resource path as the engine reports it: without natives/stm/ and without what follows ".tex" (its version, a platform
// or language suffix), in the case it is given. "" for a file that is not a texture, and for the streamed half of one (the engine
// finds that from the resource's own path).
inline std::string texture_resource(const std::string& path) {
    const std::string low = d::lower(path), root = "natives/stm/";
    if (low.compare(0, root.size(), root) != 0 || low.compare(0, root.size() + 10, root + "streaming/") == 0) return {};
    const size_t slash = low.find_last_of('/');
    const size_t t = low.find(".tex.", slash == std::string::npos ? 0 : slash);
    if (t == std::string::npos || t + 5 >= low.size() || low[t + 5] < '0' || low[t + 5] > '9') return {};
    return path.substr(root.size(), t + 4 - root.size());
}

// For the plugin's live swap of textures on screen (SF6UISlots_Overrides): "<real resource>\t<resource shown instead>\n" for each
// texture served now (serve: what differs from what the launch put at the real path, which is what the engine holds), sorted.
inline std::string overrides_text(const HashMap& served, const std::map<uint64_t, std::string>& names) {
    std::vector<std::string> lines;
    for (auto& [real, to] : served) {
        auto a = names.find(real), b = names.find(to);
        if (a == names.end() || b == names.end() || a->second.empty() || b->second.empty()) continue;
        lines.push_back(a->second + "\t" + b->second + "\n");
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (auto& l : lines) out += l;
    return out;
}

namespace detail {

// The object at t[i]: true when it closes (i past it), false when it is cut short or malformed (what came before is kept)
inline bool read_choices(const std::string& t, size_t& i, Choices& out) {
    d::skip_ws(t, i);
    if (i >= t.size() || t[i] != '{') return false;
    ++i;
    for (;;) {
        d::skip_ws(t, i);
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        std::string key;
        if (!d::read_string(t, i, key)) return false;
        d::skip_ws(t, i);
        if (i >= t.size() || t[i] != ':') return false;
        ++i;
        d::skip_ws(t, i);
        if (i < t.size() && t[i] == '[') {
            ++i;
            std::vector<std::string> ids;
            for (;;) {
                d::skip_ws(t, i);
                if (i >= t.size()) break;
                if (t[i] == ']') { ++i; break; }
                if (t[i] == ',') { ++i; continue; }
                std::string v;
                if (t[i] == '"') { if (!d::read_string(t, i, v)) break; }
                else if (!d::skip_value(t, i)) break;
                v = d::lower(d::trim(v));
                if (!v.empty() && std::find(ids.begin(), ids.end(), v) == ids.end()) ids.push_back(v);
            }
            const std::string k = d::lower(d::trim(key));
            if (!k.empty()) out[k] = ids;
        } else if (!d::skip_value(t, i)) {
            return false;
        }
        d::skip_ws(t, i);
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return false;
    }
}

}  // namespace detail

// The ui_choices object itself (what the plugin gives SF6UISlots_Apply). Read tolerantly: not an object, nothing ticked; a value
// that is not a list of strings, that pack ignored; a text cut short keeps what came before.
inline Choices parse_choices(std::string t) {
    Choices c;
    if (t.size() >= 3 && (uint8_t)t[0] == 0xEF && (uint8_t)t[1] == 0xBB && (uint8_t)t[2] == 0xBF) t.erase(0, 3);
    size_t i = 0;
    detail::read_choices(t, i, c);
    return c;
}

// settings.json's "ui_choices", at the top level of its object
inline Choices parse_settings_choices(std::string t) {
    Choices c;
    if (t.size() >= 3 && (uint8_t)t[0] == 0xEF && (uint8_t)t[1] == 0xBB && (uint8_t)t[2] == 0xBF) t.erase(0, 3);
    size_t i = 0;
    d::skip_ws(t, i);
    if (i >= t.size() || t[i] != '{') return c;
    ++i;
    for (;;) {
        d::skip_ws(t, i);
        std::string key;
        if (i >= t.size() || t[i] == '}' || !d::read_string(t, i, key)) break;
        d::skip_ws(t, i);
        if (i >= t.size() || t[i] != ':') break;
        ++i;
        d::skip_ws(t, i);
        if (key == "ui_choices" && i < t.size() && t[i] == '{') {
            c.clear();
            if (!detail::read_choices(t, i, c)) break;
        } else if (!d::skip_value(t, i)) {
            break;
        }
        d::skip_ws(t, i);
        if (i >= t.size() || t[i] != ',') break;
        ++i;
    }
    return c;
}

// The character folder a file is under: the folder name right after "character_image/" (any case, anywhere in the path) when it
// is made of digits, else "". The folder decides, not the file's own name.
inline std::string fighter_of_path(const std::string& path) {
    const std::string low = d::lower(path), key = "character_image/";
    const size_t at = low.find(key);
    if (at == std::string::npos) return {};
    const size_t a = at + key.size();
    size_t e = low.find('/', a);
    if (e == std::string::npos) e = low.size();
    if (e == a) return {};
    for (size_t i = a; i < e; ++i) if (low[i] < '0' || low[i] > '9') return {};
    return low.substr(a, e - a);
}

// A choice id for one fighter only: "<node id>@<fid>". false (whole = id, fid = "") for a whole-option id.
inline bool split_choice(const std::string& id, std::string& whole, std::string& fid) {
    const size_t at = id.find('@');
    if (at == std::string::npos) { whole = id; fid.clear(); return false; }
    whole = id.substr(0, at);
    fid = id.substr(at + 1);
    return true;
}
inline std::string subset_id(const std::string& node, const std::string& fid) { return node + "@" + fid; }

// The choices that name an option of a pack, in the pack's order: what is applied (the registry's "applied"). Per option: its
// whole id, else (not both) its "<id>@<fid>" ids for the fighters it has (Node::fids), in fid order.
inline Choices applied(const std::vector<Pack>& packs, const Choices& ch) {
    Choices out;
    for (auto& p : packs) {
        auto it = ch.find(p.key);
        if (it == ch.end()) continue;
        auto ticked = [&](const std::string& id) { return std::find(it->second.begin(), it->second.end(), id) != it->second.end(); };
        std::vector<std::string> ids;
        for (auto& nd : p.nodes) {
            if (nd.title) continue;
            if (ticked(nd.id)) { ids.push_back(nd.id); continue; }
            for (auto& fid : nd.fids)
                if (ticked(subset_id(nd.id, fid))) ids.push_back(subset_id(nd.id, fid));
        }
        if (!ids.empty()) out[p.key] = ids;
    }
    return out;
}

// Real hash -> the copy of the ticked option that wins it: in the later pack (by name) when two packs have it, then the deepest in
// the tree, then the later in display order
inline HashMap winners(const std::vector<Served>& opts, const Choices& ch) {
    struct Best { int rank, depth, order; uint64_t to; };
    std::map<uint64_t, Best> best;
    for (auto& o : opts) {
        auto it = ch.find(o.pack);
        if (it == ch.end()) continue;
        const bool whole = std::find(it->second.begin(), it->second.end(), o.id) != it->second.end();
        std::vector<std::string> only;                      // the fighters named by "<id>@<fid>" ids (used when the whole is not ticked)
        if (!whole)
            for (auto& id : it->second) {
                std::string w, fid;
                if (split_choice(id, w, fid) && w == o.id && !fid.empty()) only.push_back(fid);
            }
        if (!whole && only.empty()) continue;
        for (auto& [real, to] : o.files) {
            if (!whole) {
                auto f = o.fid.find(real);
                if (f == o.fid.end() || std::find(only.begin(), only.end(), f->second) == only.end()) continue;
            }
            auto b = best.find(real);
            const bool wins = b == best.end() || o.rank > b->second.rank ||
                              (o.rank == b->second.rank && (o.depth > b->second.depth || (o.depth == b->second.depth && o.order > b->second.order)));
            if (wins) best[real] = { o.rank, o.depth, o.order, to };
        }
    }
    HashMap out;
    out.reserve(best.size());
    for (auto& [real, b] : best) out.push_back({ real, b.to });
    return out;
}

// What the redirect serves for a set of ticked options: each real file the choice gives to another option than the one the launch
// wrote there, and, for a real file the launch wrote that nothing ticked has now, the game's own copy (when the pak holds one).
// A real file whose winner is the one written: nothing (the real path holds it).
inline HashMap serve(const std::vector<Served>& opts, const Choices& ch, const HashMap& written, const HashMap& game) {
    const HashMap w = winners(opts, ch);
    HashMap out;
    auto game_of = [&](uint64_t real, uint64_t& to) {
        auto it = std::lower_bound(game.begin(), game.end(), real, [](const std::pair<uint64_t, uint64_t>& e, uint64_t v) { return e.first < v; });
        if (it == game.end() || it->first != real) return false;
        to = it->second;
        return true;
    };
    size_t i = 0, j = 0;
    while (i < w.size() || j < written.size()) {
        uint64_t to = 0;
        if (j >= written.size() || (i < w.size() && w[i].first < written[j].first)) { out.push_back(w[i++]); continue; }
        if (i >= w.size() || written[j].first < w[i].first) {
            if (game_of(written[j].first, to)) out.push_back({ written[j].first, to });
            ++j;
            continue;
        }
        if (w[i].second != written[j].second) out.push_back(w[i]);
        ++i;
        ++j;
    }
    return out;
}

// reframework\data\SF6_UISlots_Data\registry.json. Nodes depth first in display order; "source" (where a node comes from) is
// there for the log and for telling two nodes of the same name apart.
// A node with fighters lists them ("fighters": fid -> its real texture resources, "fighters_only"); "game_textures" maps each real
// fighter texture resource to the resource of the game's own copy in the pak.
inline std::string registry_text(const std::vector<Pack>& packs, const Choices& applied_now,
                                 const std::map<std::string, std::string>& game_textures) {
    std::string js = "{\n  \"version\": 1,\n  \"packs\": [";
    for (size_t k = 0; k < packs.size(); ++k) {
        const Pack& p = packs[k];
        js += k ? ",\n" : "\n";
        js += "    { \"key\": \"" + p.key + "\", \"name\": \"" + detail::json(p.name) + "\", \"author\": \"" + detail::json(p.author) +
              "\", \"picture\": \"" + (p.picture ? pack_preview(p.key) : std::string()) + "\",\n      \"nodes\": [";
        for (size_t i = 0; i < p.nodes.size(); ++i) {
            const Node& nd = p.nodes[i];
            js += i ? ",\n" : "\n";
            js += "        { \"id\": \"" + nd.id + "\", \"parent\": \"" + nd.parent + "\", \"name\": \"" + detail::json(nd.name) +
                  "\", \"description\": \"" + detail::json(nd.description) + "\", \"kind\": \"" + (nd.title ? "title" : "option") +
                  "\", \"choice\": \"" + (nd.title ? "" : nd.one ? "one" : "check") + "\", \"group\": \"" + nd.group +
                  "\", \"files\": " + std::to_string(nd.files) + ", \"preview\": \"" + (nd.preview ? node_preview(p.key, nd.id) : std::string()) +
                  "\", \"depth\": " + std::to_string(nd.depth) + ", \"source\": \"" + detail::json(nd.path) + "\"";
            if (!nd.fighters.empty()) {
                js += ", \"fighters\": {";
                bool f1 = true;
                for (auto& [fid, res] : nd.fighters) {
                    js += f1 ? " \"" : ", \"";
                    f1 = false;
                    js += detail::json(fid) + "\": [";
                    for (size_t r = 0; r < res.size(); ++r) js += (r ? ", \"" : "\"") + detail::json(res[r]) + "\"";
                    js += "]";
                }
                js += std::string(" }, \"fighters_only\": ") + (nd.fighters_only ? "true" : "false");
            }
            js += " }";
        }
        js += p.nodes.empty() ? "] }" : "\n      ] }";
    }
    js += packs.empty() ? "],\n" : "\n  ],\n";
    // before "applied": the loader compares the text from "applied" on to see whether the choices changed
    js += "  \"game_textures\": {";
    bool first = true;
    for (auto& [real, game] : game_textures) {
        js += first ? "\n    \"" : ",\n    \"";
        first = false;
        js += detail::json(real) + "\": \"" + detail::json(game) + "\"";
    }
    js += first ? "},\n" : "\n  },\n";
    js += "  \"applied\": {";
    first = true;
    for (auto& [key, ids] : applied_now) {
        js += first ? " \"" : ", \"";
        first = false;
        js += key + "\": [";
        for (size_t i = 0; i < ids.size(); ++i) js += (i ? ", \"" : "\"") + ids[i] + "\"";
        js += "]";
    }
    js += first ? "}\n}\n" : " }\n}\n";
    return js;
}

}  // namespace ui_slots
