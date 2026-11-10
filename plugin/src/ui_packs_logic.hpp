// SF6 Slots -- the interface packs' rules, pure (no game access; plugin/tests/ui_packs_test.cpp). The loader's list of packs
// (reframework/data/SF6_UISlots_Data/registry.json, written at each launch), what the player ticked (settings.json's "ui_choices"),
// and a pack as the game's own Options rows (ui_packs.cpp: one section per pack): a one-of group is one row whose values are the
// game's own files then its options, a check box is one row Off / On; which picture goes with a row's value.
//
// The registry (version 1, the loader's stages/loader/ui_slots.hpp):
//   { "version": 1, "packs": [ { "key": "u...", "name", "author", "picture": "_uislots/u.../preview.tex" or "",
//       "nodes": [ { "id": "n...", "parent": "" or "n...", "name", "description", "kind": "title" | "option",
//                    "choice": "one" | "check", "group": "n..." or "", "files": 12, "preview": "..." or "", "depth": 1,
//                    "source": "<archive>/<folder>/<node folder>" } ] } ],
//     "applied": { "u...": [ "n...", ... ] },
//     "game_textures": { "<real texture resource>": "<the game's own copy in the stage pak>", ... } }
// A node with files under a fighter's folder (natives/stm/.../character_image/<fid>/...) also has "fighters": { "<fid>": [ the real
// texture resources of its files there, the ones the pak holds ], ... } and "fighters_only": true when every file of it is under one.
// Nodes come depth-first in display order: the rows follow that order. A pack without one option is left out (nothing to choose),
// a node without an id or with one seen before in its pack too.
// The choices: { "u<pack>": [ "n<ticked option>", ... ], ... }; a pack left out = nothing ticked = the game's own files. A tick
// "n<option>@<fid>" ticks that option for one fighter only (its files under that fighter's folder: the loader's subset choice). A pack the
// registry does not list (its mod taken out for now) keeps its choices: only the pack edited is written.
#pragma once

#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace slots::uipacks {

constexpr int kRegistryVersion = 1;

struct Node {
    std::string id, parent, name, description, preview, group, source;   // source: "<archive>/<folder>/<node folder>"
    bool title = false;      // "kind": "title": a heading (Fluffy's DummyMod), no box
    bool one = false;        // "choice": "one": in a one-of group (a radio); else a check box
    int files = 0, depth = 1;
    std::map<std::string, std::vector<std::string>> fighters;   // fid -> its real texture resources under that fighter's folder
    bool fighters_only = false;                                  // every file of it is under a fighter's folder
};
struct Pack {
    std::string key, name, author, picture;
    std::vector<Node> nodes;
};
struct Registry {
    int version = 0;
    std::vector<Pack> packs;
    std::map<std::string, std::vector<std::string>> applied;   // what this launch serves, per pack
    std::map<std::string, std::string> game_textures;          // real texture resource -> the game's own copy the pak holds
};
using Ticks = std::vector<std::string>;              // one pack's ticked options, in display order
using Choices = std::map<std::string, Ticks>;        // pack key -> its ticks

inline std::string strip_bom(std::string text) {
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    return text;
}
inline std::string str_of(const Json& o, const char* key) {
    const Json* v = o.get(key);
    return v && v->kind == Json::String ? v->s : std::string();
}

// The registry's text -> the packs. False: unreadable, or not an object (no pack then).
inline bool parse_registry(const std::string& text, Registry& out) {
    out = Registry{};
    Json j;
    if (!json_parse(strip_bom(text), j) || j.kind != Json::Object) return false;
    out.version = static_cast<int>(j.get("version") ? j.get("version")->as_int(0) : 0);
    if (const Json* packs = j.get("packs"); packs && packs->kind == Json::Array) {
        std::set<std::string> keys;
        for (const Json& p : packs->arr) {
            if (p.kind != Json::Object) continue;
            Pack pk;
            pk.key = str_of(p, "key");
            if (pk.key.empty() || !keys.insert(pk.key).second) continue;
            pk.name = str_of(p, "name");
            if (pk.name.empty()) pk.name = pk.key;
            pk.author = str_of(p, "author");
            pk.picture = str_of(p, "picture");
            std::map<std::string, int> depth_of;
            bool any_option = false;
            if (const Json* nodes = p.get("nodes"); nodes && nodes->kind == Json::Array) {
                for (const Json& n : nodes->arr) {
                    if (n.kind != Json::Object) continue;
                    Node nd;
                    nd.id = str_of(n, "id");
                    if (nd.id.empty() || depth_of.count(nd.id)) continue;
                    nd.parent = str_of(n, "parent");
                    nd.name = str_of(n, "name");
                    if (nd.name.empty()) nd.name = nd.id;
                    nd.description = str_of(n, "description");
                    nd.preview = str_of(n, "preview");
                    nd.title = str_of(n, "kind") == "title";
                    nd.one = !nd.title && str_of(n, "choice") == "one";
                    nd.group = str_of(n, "group");
                    nd.source = str_of(n, "source");
                    if (const Json* f = n.get("files")) nd.files = static_cast<int>(f->as_int(0));
                    if (const Json* fs = n.get("fighters"); fs && fs->kind == Json::Object)
                        for (const auto& [fid, list] : fs->obj) {
                            if (fid.empty() || fid.find('@') != std::string::npos) continue;
                            auto& v = nd.fighters[fid];
                            if (list.kind == Json::Array) for (const Json& e : list.arr) if (e.kind == Json::String && !e.s.empty()) v.push_back(e.s);
                        }
                    if (const Json* fo = n.get("fighters_only")) nd.fighters_only = fo->kind == Json::Bool && fo->b && !nd.fighters.empty();
                    const Json* d = n.get("depth");
                    if (d && d->kind == Json::Number && d->num >= 0) nd.depth = static_cast<int>(d->num);
                    else nd.depth = depth_of.count(nd.parent) ? depth_of[nd.parent] + 1 : 1;   // not given: its parent's + 1
                    depth_of[nd.id] = nd.depth;
                    any_option = any_option || !nd.title;
                    pk.nodes.push_back(std::move(nd));
                }
            }
            if (any_option) out.packs.push_back(std::move(pk));
        }
    }
    if (const Json* ap = j.get("applied"); ap && ap->kind == Json::Object)
        for (const auto& [k, v] : ap->obj) {
            if (v.kind != Json::Array) continue;
            auto& ids = out.applied[k];
            for (const Json& e : v.arr) if (e.kind == Json::String && !e.s.empty()) ids.push_back(e.s);
        }
    if (const Json* gt = j.get("game_textures"); gt && gt->kind == Json::Object)
        for (const auto& [k, v] : gt->obj) if (!k.empty() && v.kind == Json::String && !v.s.empty()) out.game_textures[k] = v.s;
    return true;
}

// The one-of group a radio belongs to: its "group" (its parent's id), else its parent ("" too: the pack's own group, the roots the
// loader gathered into one pack)
inline const std::string& group_of(const Node& n) { return n.group.empty() ? n.parent : n.group; }
inline int index_of(const Pack& p, const std::string& id) {
    for (size_t i = 0; i < p.nodes.size(); ++i) if (p.nodes[i].id == id) return static_cast<int>(i);
    return -1;
}
inline bool is_ticked(const Ticks& t, const std::string& id) { return std::find(t.begin(), t.end(), id) != t.end(); }
// The pack's lowest depth (its first level is drawn at the list's left edge)
inline int min_depth(const Pack& p) {
    int d = 1 << 20;
    for (const Node& n : p.nodes) d = std::min(d, n.depth);
    return p.nodes.empty() ? 0 : d;
}

// A tick for one fighter: "<option id>@<fid>"
inline std::string fighter_tick(const std::string& id, const std::string& fid) { return id + "@" + fid; }
// What can be ticked of `t` in this pack, in display order: its options (one per one-of group: the first in display order), and an
// option for one of its fighters ("<id>@<fid>": not when the option itself is ticked, nor in a one-of group that has an option
// ticked, or that fighter's already)
inline Ticks sanitize(const Pack& p, const Ticks& t) {
    Ticks out;
    std::set<std::string> groups;
    std::map<std::string, std::set<std::string>> group_fids;
    for (const Node& n : p.nodes) {
        if (n.title) continue;
        if (is_ticked(t, n.id)) {
            if (n.one && !groups.insert(group_of(n)).second) continue;
            out.push_back(n.id);
            continue;
        }
        for (const auto& [fid, tex] : n.fighters) {
            const std::string id = fighter_tick(n.id, fid);
            if (!is_ticked(t, id)) continue;
            if (n.one && (groups.count(group_of(n)) || !group_fids[group_of(n)].insert(fid).second)) continue;
            out.push_back(id);
        }
    }
    return out;
}

// ---- a pack as the game's rows ----
// A row of the pack's sections, in display order:
//   Group   a one-of group: its values the game's own files (1), then its options (2...) in display order; its title its heading's
//           name (the heading whose id is the group's, else the option its radios hang under, else, for one of several groups under
//           one parent, that parent's), the pack's name for the pack's own group (gathered roots)
//   Box     a check box option: Off (1) / On (2), its title its own name
//   Header  a heading that holds rows other than its own group's (only when the core has header rows): its name alone
// A title is made unique within its section only (unique_titles): the nearest heading in front that tells the rows apart, else the
// source archive's name, else what the group's options have in common ("AKI C1-C4 (P2)"), else a number.
// The pack's single root heading is never a header: the section already bears the pack's name.
struct Row {
    enum Kind { Group, Box, Header } kind = Box;
    std::string title;
    std::vector<int> nodes;                 // Group: its options' node indexes, in display order; Box: the option; Header: the heading
    int head = -1;                          // the node that names it (its description is the row's), -1 none
    int anchor = -1;                        // the node it stands for in the tree: its heading's place (whose headings tell it apart)
};
// The literal "\n" of a modinfo (Fluffy writes "line \n\nArt By: ..."), its real line breaks and tabs: one space each, runs of spaces
// one, none at the ends (the game's description line is one line)
inline std::string one_line(const std::string& in) {
    std::string out;
    bool space = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char ch = in[i];
        if (ch == '\\' && i + 1 < in.size() && in[i + 1] == '\\') {   // an escaped backslash: both kept
            if (space) out += ' ';
            space = false;
            out += "\\\\";
            ++i;
            continue;
        }
        if (ch == '\\' && i + 1 < in.size() && (in[i + 1] == 'n' || in[i + 1] == 'r' || in[i + 1] == 't')) { ch = ' '; ++i; }
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        if (ch == ' ') { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false;
        out += ch;
    }
    return out;
}
// "%s: %s" and the like, its two %s in order
inline std::string fold2(const char* fmt, const std::string& a, const std::string& b) {
    std::string r;
    const std::string arg[2] = { a, b };
    int k = 0;
    for (const char* c = fmt; *c; ++c) {
        if (c[0] == '%' && c[1] == 's') { if (k < 2) r += arg[k++]; ++c; }
        else r += *c;
    }
    return r;
}
// The source's archive, without its extension ("Battlehud Addon AKI C1-C4.rar/..." -> "Battlehud Addon AKI C1-C4")
inline std::string archive_of(const std::string& source) {
    std::string a = source.substr(0, source.find('/'));
    const size_t dot = a.rfind('.');
    if (dot != std::string::npos && dot > 0 && a.size() - dot <= 5) a.erase(dot);
    return a;
}
inline int pack_root(const Pack& p) {                  // the pack's single root heading, -1 when it has several roots or none
    const int base = min_depth(p);
    int root = -1, roots = 0;
    for (size_t i = 0; i < p.nodes.size(); ++i)
        if (p.nodes[i].depth == base) { ++roots; root = p.nodes[i].title ? static_cast<int>(i) : -1; }
    return roots == 1 ? root : -1;
}
// The headings that hold rows other than the group they head, but the pack's single root
inline std::set<std::string> header_ids(const Pack& p) {
    std::set<std::string> out;
    const int root = pack_root(p);
    for (size_t i = 0; i < p.nodes.size(); ++i) {
        const Node& t = p.nodes[i];
        if (!t.title || static_cast<int>(i) == root) continue;
        for (size_t k = i + 1; k < p.nodes.size() && p.nodes[k].depth > t.depth; ++k) {
            const Node& n = p.nodes[k];
            if (!n.title && (!n.one || group_of(n) != t.id)) { out.insert(t.id); break; }
        }
    }
    return out;
}
// A node's parent's index (-1 none)
inline int parent_of(const Pack& p, size_t i) { return p.nodes[i].parent.empty() ? -1 : index_of(p, p.nodes[i].parent); }
// The headings above a node, nearest first
inline std::vector<int> headings_above(const Pack& p, size_t i) {
    std::vector<int> out;
    int up = parent_of(p, i);
    for (int guard = 0; up >= 0 && guard < 64; ++guard) {
        if (p.nodes[static_cast<size_t>(up)].title) out.push_back(up);
        up = parent_of(p, static_cast<size_t>(up));
    }
    return out;
}
inline std::vector<Row> rows_of(const Pack& p, bool headers) {
    std::vector<Row> out;
    const std::set<std::string> heads = headers ? header_ids(p) : std::set<std::string>{};
    std::set<std::string> groups;
    for (size_t i = 0; i < p.nodes.size(); ++i) {
        const Node& n = p.nodes[i];
        const int me = static_cast<int>(i);
        if (n.title) {
            if (heads.count(n.id)) { Row h; h.kind = Row::Header; h.title = n.name; h.nodes = { me }; h.head = h.anchor = me; out.push_back(h); }
            continue;
        }
        if (!n.one) {
            Row b;
            b.kind = Row::Box;
            b.nodes = { me };
            b.head = b.anchor = me;
            b.title = n.name;
            out.push_back(b);
            continue;
        }
        const std::string& key = group_of(n);
        if (!groups.insert(key).second) continue;           // its group's row is made at its first option
        Row g;
        g.kind = Row::Group;
        for (size_t k = i; k < p.nodes.size(); ++k) if (p.nodes[k].one && group_of(p.nodes[k]) == key) g.nodes.push_back(static_cast<int>(k));
        int named = key.empty() ? -1 : index_of(p, key);
        if (named >= 0 && std::find(g.nodes.begin(), g.nodes.end(), named) != g.nodes.end()) named = parent_of(p, static_cast<size_t>(named));
        if (named < 0 && !key.empty() && index_of(p, key) < 0) named = parent_of(p, i);     // a group named by none of its nodes
        g.head = named;
        g.anchor = named >= 0 ? named : me;
        g.title = named >= 0 ? p.nodes[static_cast<size_t>(named)].name : p.name;
        out.push_back(g);
    }
    return out;
}
// The pack's rows cut into sections of at most `max` rows (the game's right list has 25 parts), only between its top-level blocks
// (each heading under the root, with all its rows; a row of its own at that level) when they fit; a block larger than `max` alone
// is cut in pieces of `max`. Row indexes, in order.
inline std::vector<std::vector<int>> split_rows(const Pack& p, const std::vector<Row>& rows, size_t max = 25) {
    std::vector<std::vector<int>> out;
    if (rows.empty() || max == 0) return out;
    const int root = pack_root(p);
    auto block_of = [&](const Row& r) {
        int at = r.anchor >= 0 ? r.anchor : (r.nodes.empty() ? -1 : r.nodes[0]);
        for (int guard = 0; at >= 0 && guard < 64; ++guard) {
            const int up = parent_of(p, static_cast<size_t>(at));
            if (up < 0 || up == root) return at;
            at = up;
        }
        return at;
    };
    std::vector<std::vector<int>> blocks;
    int last = -2;
    for (size_t i = 0; i < rows.size(); ++i) {
        const int b = block_of(rows[i]);
        if (blocks.empty() || b != last || b == root) blocks.emplace_back();
        blocks.back().push_back(static_cast<int>(i));
        last = b;
    }
    for (const auto& b : blocks) {
        if (b.size() > max) {                               // too big for one section: in pieces, each its own
            for (size_t k = 0; k < b.size(); k += max) out.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(k), b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), k + max)));
            continue;
        }
        if (out.empty() || out.back().size() + b.size() > max) out.emplace_back();
        out.back().insert(out.back().end(), b.begin(), b.end());
    }
    return out;
}
// What a group's options' names have in common at their end (else at their start), whole words, without spaces or one pair of
// brackets around it ("AKI C1 Alt (P2)", "AKI C2 (P2)" -> "P2"); "" none
inline std::string common_part(const Pack& p, const Row& r) {
    if (r.kind != Row::Group || r.nodes.size() < 2) return std::string();
    std::vector<std::string> names;
    for (int k : r.nodes) names.push_back(p.nodes[static_cast<size_t>(k)].name);
    auto is_cut = [](char c) { return c == ' ' || c == '(' || c == ')' || c == '[' || c == ']' || c == '-' || c == '_'; };
    auto clean = [&](std::string x) {
        while (!x.empty() && (x.front() == ' ' || x.front() == '-' || x.front() == '_')) x.erase(0, 1);
        while (!x.empty() && (x.back() == ' ' || x.back() == '-' || x.back() == '_')) x.pop_back();
        if (x.size() >= 2 && ((x.front() == '(' && x.back() == ')') || (x.front() == '[' && x.back() == ']'))) x = x.substr(1, x.size() - 2);
        return x.size() >= 2 && x.find_first_of("()[]") == std::string::npos ? x : std::string();
    };
    size_t suf = names[0].size();
    for (const std::string& n : names) {
        size_t k = 0;
        while (k < suf && k < n.size() && n[n.size() - 1 - k] == names[0][names[0].size() - 1 - k]) ++k;
        suf = k;
    }
    if (suf > 0) {                                          // from a word's start in every name
        size_t cut = names[0].size() - suf;
        bool whole = true;
        for (const std::string& n : names) { const size_t at = n.size() - suf; whole = whole && (at == 0 || is_cut(n[at - 1]) || is_cut(n[at])); }
        std::string x = names[0].substr(cut);
        if (!whole) { const size_t sp = x.find_first_of(" ("); x = sp == std::string::npos ? std::string() : x.substr(sp); }
        if (std::string c = clean(x); !c.empty()) return c;
    }
    size_t pre = names[0].size();
    for (const std::string& n : names) {
        size_t k = 0;
        while (k < pre && k < n.size() && n[k] == names[0][k]) ++k;
        pre = k;
    }
    std::string x = names[0].substr(0, pre);
    bool whole = true;
    for (const std::string& n : names) whole = whole && (n.size() == pre || is_cut(n[pre]));
    if (!whole) { const size_t sp = x.find_last_of(' '); x = sp == std::string::npos ? std::string() : x.substr(0, sp); }
    return clean(x);
}
// The titles of one section's rows made unique (rows with the same title: the nearest heading level that tells them all apart in
// front, "<heading>: <title>" in `fold`; else their archives'; else "<title> (<what its options have in common>)"; else a number)
inline void unique_titles(const Pack& p, std::vector<Row>& rows, const std::vector<int>& section, const char* fold = "%s: %s") {
    std::map<std::string, std::vector<int>> same;
    for (int i : section) if (rows[static_cast<size_t>(i)].kind != Row::Header) same[rows[static_cast<size_t>(i)].title].push_back(i);
    for (auto& [title, set] : same) {
        if (set.size() < 2) continue;
        auto distinct = [&](const std::vector<std::string>& v, bool one_empty) {
            std::set<std::string> u(v.begin(), v.end());
            size_t empty = 0;
            for (const std::string& x : v) empty += x.empty() ? 1 : 0;
            return u.size() == v.size() && (empty == 0 || (one_empty && empty == 1));
        };
        std::vector<std::vector<int>> chains;
        size_t levels = 0;
        for (int i : set) {
            const Row& r = rows[static_cast<size_t>(i)];
            std::vector<int> c;
            if (r.anchor >= 0) for (int h : headings_above(p, static_cast<size_t>(r.anchor))) if (p.nodes[static_cast<size_t>(h)].name != title) c.push_back(h);
            levels = std::max(levels, c.size());
            chains.push_back(c);
        }
        bool done = false;
        for (size_t lv = 0; lv < levels && !done; ++lv) {
            std::vector<std::string> v;
            for (const auto& c : chains) v.push_back(lv < c.size() ? p.nodes[static_cast<size_t>(c[lv])].name : std::string());
            if (!distinct(v, false)) continue;
            for (size_t k = 0; k < set.size(); ++k) rows[static_cast<size_t>(set[k])].title = fold2(fold, v[k], title);
            done = true;
        }
        if (done) continue;
        std::vector<std::string> arch, common;
        for (int i : set) {
            const Row& r = rows[static_cast<size_t>(i)];
            const int at = r.nodes.empty() ? r.anchor : r.nodes[0];
            arch.push_back(at >= 0 ? archive_of(p.nodes[static_cast<size_t>(at)].source) : std::string());
            common.push_back(common_part(p, r));
        }
        if (distinct(arch, false)) {
            for (size_t k = 0; k < set.size(); ++k) rows[static_cast<size_t>(set[k])].title = fold2(fold, arch[k], title);
        } else if (distinct(common, true)) {
            for (size_t k = 0; k < set.size(); ++k) if (!common[k].empty()) rows[static_cast<size_t>(set[k])].title = title + " (" + common[k] + ")";
        } else {
            for (size_t k = 0; k < set.size(); ++k) rows[static_cast<size_t>(set[k])].title = title + " " + std::to_string(k + 1);
        }
    }
}
// The row's values (1-based in the game): a group's the game's own word then its options' names (two of the same name: their
// archives', else a number, after them), a box's Off / On
inline std::vector<std::string> labels_of(const Pack& p, const Row& r, const std::string& game_own, const std::string& off, const std::string& on) {
    std::vector<std::string> out;
    if (r.kind == Row::Box) return { off, on };
    if (r.kind == Row::Header) return {};
    out.push_back(game_own);
    std::map<std::string, std::vector<size_t>> same;
    for (size_t k = 0; k < r.nodes.size(); ++k) {
        out.push_back(p.nodes[static_cast<size_t>(r.nodes[k])].name);
        same[out.back()].push_back(k);
    }
    for (const auto& [name, ks] : same) {
        if (ks.size() < 2) continue;
        std::set<std::string> arch;
        for (size_t k : ks) arch.insert(archive_of(p.nodes[static_cast<size_t>(r.nodes[k])].source));
        for (size_t j = 0; j < ks.size(); ++j) {
            const std::string a = archive_of(p.nodes[static_cast<size_t>(r.nodes[ks[j]])].source);
            out[ks[j] + 1] = arch.size() == ks.size() && !a.empty() ? name + " (" + a + ")" : name + " " + std::to_string(j + 1);
        }
    }
    return out;
}
// The row's value for these ticks (1-based; a header 0)
inline int value_of(const Pack& p, const Row& r, const Ticks& t) {
    if (r.kind == Row::Header) return 0;
    if (r.kind == Row::Box) return is_ticked(t, p.nodes[static_cast<size_t>(r.nodes[0])].id) ? 2 : 1;
    for (size_t k = 0; k < r.nodes.size(); ++k)
        if (is_ticked(t, p.nodes[static_cast<size_t>(r.nodes[k])].id)) return static_cast<int>(k) + 2;
    return 1;
}
// The row set to `value` (a group: its other options unticked, 1 none of them; a box: 2 on): true when the ticks changed. A value out
// of the row's range changes nothing.
inline bool set_value(const Pack& p, const Row& r, int value, Ticks& t) {
    if (r.kind == Row::Header || value < 1) return false;
    if (r.kind == Row::Box && value > 2) return false;
    if (r.kind == Row::Group && value > static_cast<int>(r.nodes.size()) + 1) return false;
    std::set<std::string> on(t.begin(), t.end());
    if (r.kind == Row::Box) {
        const std::string& id = p.nodes[static_cast<size_t>(r.nodes[0])].id;
        if (value == 2) on.insert(id); else on.erase(id);
    } else {
        for (int k : r.nodes) on.erase(p.nodes[static_cast<size_t>(k)].id);
        if (value >= 2) on.insert(p.nodes[static_cast<size_t>(r.nodes[static_cast<size_t>(value - 2)])].id);
    }
    Ticks next = sanitize(p, Ticks(on.begin(), on.end()));
    if (next == t) return false;
    t = std::move(next);
    return true;
}
// The picture of a row's value: a group's option (its game's own value: none), a box's option whatever its value; "" none
inline std::string picture_of(const Pack& p, const Row& r, int value) {
    if (r.kind == Row::Box) return p.nodes[static_cast<size_t>(r.nodes[0])].preview;
    if (r.kind == Row::Group && value >= 2 && value - 2 < static_cast<int>(r.nodes.size()))
        return p.nodes[static_cast<size_t>(r.nodes[static_cast<size_t>(value - 2)])].preview;
    return std::string();
}

// ---- a pack of portraits per character, as a grid (Wael 10/11: "the portraits system", as the Replays' Outfit Settings) ----
// A fighter's group: a one-of group whose options each have their files under one same fighter's folder only (Battlehud Portraits'
// "AKI C1 Alt (P1)", "AKI C2 (P1)"...). A pack with at least one is shown as a grid of characters (ui_packs_grid.cpp) instead of
// rows. In the grid each character has its own values, cycled on its cell:
//   0   the game's own (no tick of the grid for that fighter)
//   then each option the grid splits per fighter (in display order): an option not of a fighter's group whose files are all under
//       fighters' folders and that shares a texture with a fighter's group of one of them ("C1 Battlehud Colour (Full)"): for this
//       character, its files under its folder only ("<id>@<fid>"; every character set to it: the option itself)
//   then the fighter's groups' options, side by side: the k-th option of each of its groups together (its P1 and P2 groups:
//       "AKI C2 (P1)" + "AKI C2 (P2)", named "C2": the words they do not share, then the words every such value shares, taken off)
// A value's picture: the texture of the character the most of its values have (its main texture, "..._013_1p_iam.tex"), from the
// value's first node that has it (else that node's first texture), as the stage pak holds the copy ("_uislots/<pack>/<node>/<res>");
// the game's own value: the game's copy of the main texture (registry "game_textures"), "" when the pak has none.
// The nodes the grid takes are no rows; the pack's other options stay rows (a section of their own).
struct GridValue {
    std::string name;
    Ticks ids;                              // what it ticks (options, or "<id>@<fid>")
    std::vector<int> nodes;                 // its nodes (indexes), in display order
    std::string picture;                    // the texture shown on the cell (a resource of the stage pak), "" none
};
struct GridFighter {
    std::string fid;                        // as the loader writes it ("013")
    std::vector<GridValue> values;          // [0] the game's own
};
struct Grid {
    std::vector<GridFighter> fighters;      // by fighter number (the page orders them as the game's grid)
    std::set<int> taken;                    // the nodes the grid shows (not rows)
    std::vector<int> split;                 // the options it splits per fighter
    bool empty() const { return fighters.empty(); }
};
inline std::vector<std::string> name_words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        if (c == ' ') { if (!w.empty()) out.push_back(w); w.clear(); }
        else w += c;
    }
    if (!w.empty()) out.push_back(w);
    return out;
}
inline std::string join_words(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& w : v) { if (!out.empty()) out += ' '; out += w; }
    return out;
}
// The words of the first name every name has, in its order ("AKI C2 (P1)", "AKI C2 (P2)" -> "AKI C2"); none: the first name
inline std::string shared_words(const std::vector<std::string>& names) {
    if (names.empty()) return std::string();
    std::vector<std::string> keep;
    for (const std::string& w : name_words(names[0])) {
        bool all = true;
        for (size_t i = 1; i < names.size() && all; ++i) { const auto v = name_words(names[i]); all = std::find(v.begin(), v.end(), w) != v.end(); }
        if (all) keep.push_back(w);
    }
    return keep.empty() ? names[0] : join_words(keep);
}
inline int fid_number(const std::string& fid) {
    int n = 0;
    for (char c : fid) { if (c < '0' || c > '9') return 1 << 20; n = n * 10 + (c - '0'); if (n > 100000) return 1 << 20; }
    return n;
}
inline std::string private_texture(const Pack& p, const Node& n, const std::string& res) { return "_uislots/" + p.key + "/" + n.id + "/" + res; }

inline Grid grid_of(const Pack& p, const std::map<std::string, std::string>& game_textures, const std::string& game_own) {
    Grid g;
    // the fighters' groups
    std::vector<std::string> order;                                  // the groups, in display order
    std::map<std::string, std::vector<int>> members;
    for (size_t i = 0; i < p.nodes.size(); ++i) {
        const Node& n = p.nodes[i];
        if (n.title || !n.one) continue;
        if (!members.count(group_of(n))) order.push_back(group_of(n));
        members[group_of(n)].push_back(static_cast<int>(i));
    }
    std::map<std::string, std::vector<std::string>> fgroups;          // fid -> its groups, in display order
    for (const std::string& key : order) {
        std::string fid;
        bool ok = true;
        for (int k : members[key]) {
            const Node& n = p.nodes[static_cast<size_t>(k)];
            if (!n.fighters_only || n.fighters.size() != 1) { ok = false; break; }
            const std::string& f = n.fighters.begin()->first;
            if (fid.empty()) fid = f;
            else if (f != fid) { ok = false; break; }
        }
        if (!ok || fid.empty()) continue;
        fgroups[fid].push_back(key);
        for (int k : members[key]) g.taken.insert(k);
    }
    if (fgroups.empty()) return Grid{};
    std::map<std::string, std::set<std::string>> gtex;               // fid -> the textures of its groups' options
    for (const auto& [fid, keys] : fgroups)
        for (const std::string& key : keys)
            for (int k : members[key])
                for (const std::string& t : p.nodes[static_cast<size_t>(k)].fighters.begin()->second) gtex[fid].insert(t);
    // the options split per fighter
    std::set<std::string> fids;
    for (const auto& [fid, keys] : fgroups) fids.insert(fid);
    for (size_t i = 0; i < p.nodes.size(); ++i) {
        const Node& n = p.nodes[i];
        if (n.title || g.taken.count(static_cast<int>(i)) || !n.fighters_only || n.fighters.empty()) continue;
        bool shares = false;
        for (const auto& [fid, tex] : n.fighters) {
            auto it = gtex.find(fid);
            if (it == gtex.end()) continue;
            for (const std::string& t : tex) shares = shares || it->second.count(t) != 0;
        }
        if (!shares) continue;
        g.split.push_back(static_cast<int>(i));
        g.taken.insert(static_cast<int>(i));
        for (const auto& [fid, tex] : n.fighters) fids.insert(fid);
    }
    std::vector<std::string> sorted(fids.begin(), fids.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
        const int x = fid_number(a), y = fid_number(b);
        return x != y ? x < y : a < b;
    });
    for (const std::string& fid : sorted) {
        GridFighter gf;
        gf.fid = fid;
        std::vector<GridValue> vals;
        for (int i : g.split) {
            const Node& n = p.nodes[static_cast<size_t>(i)];
            if (!n.fighters.count(fid)) continue;
            GridValue v;
            v.name = n.name;
            v.ids = { fighter_tick(n.id, fid) };
            v.nodes = { i };
            vals.push_back(v);
        }
        const size_t first_paired = vals.size();
        std::vector<std::vector<int>> groups;
        if (auto it = fgroups.find(fid); it != fgroups.end()) for (const std::string& key : it->second) groups.push_back(members[key]);
        size_t most = 0;
        for (const auto& gr : groups) most = std::max(most, gr.size());
        for (size_t k = 0; k < most; ++k) {
            GridValue v;
            std::vector<std::string> names;
            for (const auto& gr : groups) {
                if (k >= gr.size()) continue;
                const Node& n = p.nodes[static_cast<size_t>(gr[k])];
                v.ids.push_back(n.id);
                v.nodes.push_back(gr[k]);
                names.push_back(n.name);
            }
            v.name = shared_words(names);
            vals.push_back(v);
        }
        if (vals.size() - first_paired >= 2) {                           // the words every paired value has ("AKI"): taken off
            std::vector<std::string> names;
            for (size_t k = first_paired; k < vals.size(); ++k) names.push_back(vals[k].name);
            const std::string shared = shared_words(names);
            const std::vector<std::string> common = name_words(shared);
            bool all_left = true;
            std::vector<std::string> left;
            for (const std::string& nm : names) {
                std::vector<std::string> keep;
                for (const std::string& w : name_words(nm)) if (std::find(common.begin(), common.end(), w) == common.end()) keep.push_back(w);
                all_left = all_left && !keep.empty();
                left.push_back(join_words(keep));
            }
            bool any_common = true;                                       // shared_words gives the first name back when none is shared
            for (const std::string& w : common) {
                for (const std::string& nm : names) { const auto v = name_words(nm); any_common = any_common && std::find(v.begin(), v.end(), w) != v.end(); }
            }
            if (all_left && any_common) for (size_t k = first_paired; k < vals.size(); ++k) vals[k].name = left[k - first_paired];
        }
        // the main texture: the one the most values have (ties: the first by name)
        std::map<std::string, int> count;
        for (const GridValue& v : vals) {
            std::set<std::string> seen;
            for (int i : v.nodes) {
                auto it = p.nodes[static_cast<size_t>(i)].fighters.find(fid);
                if (it != p.nodes[static_cast<size_t>(i)].fighters.end()) for (const std::string& t : it->second) seen.insert(t);
            }
            for (const std::string& t : seen) ++count[t];
        }
        std::string main;
        int best = 0;
        for (const auto& [t, c] : count) if (c > best) { best = c; main = t; }
        for (GridValue& v : vals) {
            for (int i : v.nodes) {
                const Node& n = p.nodes[static_cast<size_t>(i)];
                auto it = n.fighters.find(fid);
                if (it == n.fighters.end() || it->second.empty()) continue;
                if (std::find(it->second.begin(), it->second.end(), main) != it->second.end()) { v.picture = private_texture(p, n, main); break; }
                if (v.picture.empty()) v.picture = private_texture(p, n, it->second.front());
            }
        }
        GridValue own;
        own.name = game_own;
        if (auto it = game_textures.find(main); it != game_textures.end()) own.picture = it->second;
        gf.values.push_back(own);
        for (GridValue& v : vals) gf.values.push_back(std::move(v));
        g.fighters.push_back(std::move(gf));
    }
    return g;
}
// Whether a tick is on: itself, or "<id>@<fid>" with its option ticked whole
inline bool tick_on(const Ticks& t, const std::string& id) {
    if (is_ticked(t, id)) return true;
    const size_t at = id.find('@');
    return at != std::string::npos && is_ticked(t, id.substr(0, at));
}
// A character's value for these ticks: the first whose ticks are all on, 0 (the game's own) none
inline int grid_value(const GridFighter& f, const Ticks& t) {
    for (size_t v = 1; v < f.values.size(); ++v) {
        bool all = !f.values[v].ids.empty();
        for (const std::string& id : f.values[v].ids) all = all && tick_on(t, id);
        if (all) return static_cast<int>(v);
    }
    return 0;
}
inline const GridFighter* grid_fighter(const Grid& g, const std::string& fid) {
    for (const GridFighter& f : g.fighters) if (f.fid == fid) return &f;
    return nullptr;
}
// The character set to `value`: its other values' ticks off (a split option ticked whole: kept for its other fighters), the value's
// on; a split option ticked for each of its fighters: the option itself. True when the ticks changed.
inline bool grid_set(const Pack& p, const Grid& g, const std::string& fid, int value, Ticks& t) {
    const GridFighter* f = grid_fighter(g, fid);
    if (!f || value < 0 || value >= static_cast<int>(f->values.size())) return false;
    std::set<std::string> on(t.begin(), t.end());
    for (int i : g.split) {
        const Node& n = p.nodes[static_cast<size_t>(i)];
        if (!n.fighters.count(fid) || !on.count(n.id)) continue;
        on.erase(n.id);
        for (const auto& [other, tex] : n.fighters) if (other != fid) on.insert(fighter_tick(n.id, other));
    }
    for (const GridValue& v : f->values) for (const std::string& id : v.ids) on.erase(id);
    for (const std::string& id : f->values[static_cast<size_t>(value)].ids) on.insert(id);
    for (int i : g.split) {
        const Node& n = p.nodes[static_cast<size_t>(i)];
        bool every = !n.fighters.empty();
        for (const auto& [other, tex] : n.fighters) every = every && on.count(fighter_tick(n.id, other)) != 0;
        if (!every) continue;
        for (const auto& [other, tex] : n.fighters) on.erase(fighter_tick(n.id, other));
        on.insert(n.id);
    }
    Ticks next = sanitize(p, Ticks(on.begin(), on.end()));
    if (next == t) return false;
    t = std::move(next);
    return true;
}
// The rows of the pack the grid does not take (a group keeps its options the grid does not take)
inline std::vector<Row> rows_left(const std::vector<Row>& rows, const Grid& g) {
    std::vector<Row> out;
    for (Row r : rows) {
        if (r.kind == Row::Header) { out.push_back(r); continue; }
        std::vector<int> keep;
        for (int k : r.nodes) if (!g.taken.count(k)) keep.push_back(k);
        if (keep.empty()) continue;
        r.nodes = keep;
        out.push_back(r);
    }
    return out;
}

// ---- settings.json's "ui_choices" ----
inline Choices parse_choices(const Json& settings) {
    Choices out;
    const Json* c = settings.get("ui_choices");
    if (!c || c->kind != Json::Object) return out;
    for (const auto& [k, v] : c->obj) {
        if (k.empty() || v.kind != Json::Array) continue;
        Ticks t;
        for (const Json& e : v.arr) if (e.kind == Json::String && !e.s.empty() && !is_ticked(t, e.s)) t.push_back(e.s);
        if (!t.empty()) out[k] = std::move(t);
    }
    return out;
}
// The "ui_choices" object (a pack with nothing ticked left out)
inline Json choices_json(const Choices& c) {
    Json j;
    j.kind = Json::Object;
    for (const auto& [k, t] : c) {
        if (t.empty()) continue;
        Json a;
        a.kind = Json::Array;
        for (const std::string& id : t) {
            Json e;
            e.kind = Json::String;
            e.s = id;
            a.arr.push_back(std::move(e));
        }
        j.obj.emplace_back(k, std::move(a));
    }
    return j;
}
// One pack's ticks set (none: the pack left out), every other pack's kept
inline void set_pack(Choices& c, const std::string& key, const Ticks& t) {
    if (t.empty()) c.erase(key);
    else c[key] = t;
}

}  // namespace slots::uipacks
