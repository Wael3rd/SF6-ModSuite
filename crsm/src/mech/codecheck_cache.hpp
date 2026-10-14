// mech::codecache — the pure part of the per-game-version cache of the start-up code scan (hooks.cpp fold_scan): text
// format, parse, merge across install passes, hit decision. No REFramework, no game: tested offline (tests/codecache_bench.cpp).
// Same idea as the Native Core's pm_codecheck.txt (core/src/pausemenu/pausemenu.cpp), own copy.
//
// File (reframework/data/SF6_CRSMNative_data/codecheck.txt), every line ends with \n, the last one is "end <n>":
//   SF6_CRSMNative code scan cache 1
//   key exe 6A97881D 2618F000 25BB1081 size 632956832 time 1DD5AD806B56D43 methods 1455453
//   t 1 194895 87272288 87272288 0 0 -\tN:app.training.TrainingMenuFunc|GetIsActive
//   t 0 0 0 0 2 2 31,57\tV:0x1409A1230
//   end 2
// t <named> <index> <raw rva> <code rva> <shared> <n sharers> <sharers, comma list or ->; id after the tab.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace codecache {

constexpr const char* kHeader = "SF6_CRSMNative code scan cache 1";
constexpr size_t kMaxSharers = 16;

struct Entry {
    std::string id;                   // "N:Type|sig" (named hook) or "V:0x<static VA>" (code address: hook or survey)
    bool named = true;                // false: by VA (index / raw / code are not compared: the VA is static)
    uint32_t index = 0;               // named: the method's index
    long long raw = 0, code = 0;      // entry and code as RVAs (address - module base)
    int shared = 0;                   // others entering the same code (by VA: the entering count)
    std::vector<uint32_t> sharers;    // their method indexes, the first 16
};

inline std::string named_id(const char* type, const char* sig) { return std::string("N:") + type + "|" + sig; }
inline std::string va_id(unsigned long long va) { char b[40]; std::snprintf(b, sizeof b, "V:0x%llX", va); return b; }

inline std::string serialize(const std::string& key, const std::vector<Entry>& es) {
    std::string o = std::string(kHeader) + "\nkey " + key + "\n";
    char b[160];
    for (const Entry& e : es) {
        std::snprintf(b, sizeof b, "t %d %u %lld %lld %d %d ", e.named ? 1 : 0, e.index, e.raw, e.code, e.shared, (int)e.sharers.size());
        o += b;
        if (e.sharers.empty()) o += "-";
        for (size_t i = 0; i < e.sharers.size(); ++i) { if (i) o += ","; o += std::to_string(e.sharers[i]); }
        o += "\t" + e.id + "\n";
    }
    return o + "end " + std::to_string(es.size()) + "\n";
}

// False on anything that is not exactly a complete file of this format (truncated, garbage, count mismatch): never partial.
inline bool parse(const std::string& text, std::string& key, std::vector<Entry>& out) {
    out.clear();
    key.clear();
    if (text.empty() || text.back() != '\n') return false;
    size_t pos = 0;
    auto next = [&](std::string& line) {
        if (pos >= text.size()) return false;
        const size_t e = text.find('\n', pos);
        line = text.substr(pos, e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = e + 1;
        return true;
    };
    std::string line;
    if (!next(line) || line != kHeader) return false;
    if (!next(line) || line.compare(0, 4, "key ") != 0 || line.size() < 5) return false;
    key = line.substr(4);
    std::vector<Entry> es;
    for (;;) {
        if (!next(line)) return false;                        // no "end": truncated
        if (line.compare(0, 4, "end ") == 0) {
            char* e = nullptr;
            const unsigned long n = std::strtoul(line.c_str() + 4, &e, 10);
            if (e == line.c_str() + 4 || *e || n != es.size() || pos != text.size()) return false;
            out = std::move(es);
            return true;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size() || line.compare(0, 2, "t ") != 0) return false;
        Entry e;
        e.id = line.substr(tab + 1);
        if (e.id.size() < 3 || (e.id[0] != 'N' && e.id[0] != 'V') || e.id[1] != ':') return false;
        int named = 0, nsh = 0, used = 0;
        unsigned idx = 0;
        const std::string head = line.substr(0, tab);
        if (std::sscanf(head.c_str(), "t %d %u %lld %lld %d %d %n", &named, &idx, &e.raw, &e.code, &e.shared, &nsh, &used) < 6 ||
            used <= 0 || (named != 0 && named != 1) || nsh < 0 || nsh > (int)kMaxSharers || e.shared < 0 || (e.id[0] == 'N') != (named == 1))
            return false;
        e.named = named == 1;
        e.index = idx;
        const char* p = head.c_str() + used;
        if (nsh == 0) {
            if (std::strcmp(p, "-") != 0) return false;
        } else {
            for (int k = 0; k < nsh; ++k) {
                if (*p < '0' || *p > '9') return false;
                char* q = nullptr;
                e.sharers.push_back((uint32_t)std::strtoul(p, &q, 10));
                p = q;
                if (k + 1 < nsh) { if (*p != ',') return false; ++p; }
            }
            if (*p) return false;
        }
        es.push_back(std::move(e));
    }
}

enum class Miss { None, NoFile, Unreadable, Key, Missing, Moved };
inline const char* miss_text(Miss m) {
    switch (m) {
        case Miss::None: return "hit";
        case Miss::NoFile: return "no cache file yet";
        case Miss::Unreadable: return "unreadable or truncated cache file";
        case Miss::Key: return "another game version";
        case Miss::Missing: return "a target is not in the cache";
        case Miss::Moved: return "a hooked method's index or code moved";
    }
    return "?";
}

inline const Entry* find(const std::vector<Entry>& es, const std::string& id) {
    for (const Entry& e : es) if (e.id == id) return &e;
    return nullptr;
}

// cur: this pass's targets as they are NOW (id, named, index, raw, code; shared / sharers ignored). On a hit, found[i] is
// the cached entry of cur[i] (shared / sharers to copy). Hit = same key, every target cached, named ones at the same index / RVAs.
inline Miss decide(bool have_file, const std::string& file_key, const std::vector<Entry>& file, const std::string& key,
                   const std::vector<Entry>& cur, std::vector<Entry>& found) {
    found.clear();
    if (!have_file) return Miss::NoFile;
    if (file_key != key) return Miss::Key;
    std::vector<Entry> got;
    for (const Entry& c : cur) {
        const Entry* f = find(file, c.id);
        if (!f) return Miss::Missing;
        if (c.named && (!f->named || f->index != c.index || f->raw != c.raw || f->code != c.code)) return Miss::Moved;
        if (!c.named && f->named) return Miss::Moved;
        got.push_back(*f);
    }
    found = std::move(got);
    return Miss::None;
}

// What to write after a successful scan: the file's entries of other targets (same key only) + this pass's (replaced / added).
inline std::vector<Entry> merge(bool have_file, const std::string& file_key, const std::vector<Entry>& file, const std::string& key,
                                const std::vector<Entry>& fresh) {
    std::vector<Entry> o;
    if (have_file && file_key == key)
        for (const Entry& e : file) if (!find(fresh, e.id)) o.push_back(e);
    for (const Entry& e : fresh) if (!find(o, e.id)) o.push_back(e);
    return o;
}

}  // namespace codecache
