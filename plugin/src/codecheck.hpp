// SF6 Slots — R18, the code check of hooked methods, for several methods at once: pure logic (plugin/tests). The game side is
// codecheck.cpp (a scan over every method of the game on a thread of its own, as opp_summary.cpp's for one method).
//
// A method is hooked only when no other method of the game enters its code (REFramework's hook lands on the code: a method
// sharing it, a folded copy, would run our hook too: the 1 fps of the state OnEnter / OnExit hooks). The result is kept
// per game version in a cache file:
//   SF6 Slots code check 2
//   key exe 6A97881D 2618F000 25BB1081 size 632956832 time 1DD5AD806B56D43 methods 1455453
//   t <method index> <entry rva> <code rva> <other methods entering that code> <one of them, -1 none>|<Type.Method>
//   ...one t line per method, in the order they were asked for
// The cache answers only when it was written for this executable and this exact list, each method still where it was.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace slots::codecheck {

constexpr const char* kCacheHeader = "SF6 Slots code check 2";

struct Entry {
    unsigned index = 0;
    long long raw = 0, code = 0;      // entry and code, as RVAs (code -1: outside the executable, another module's hook)
    int shared = -1;                  // other methods entering the same entry or code (-1: not known yet)
    long long other = -1;             // the index of one of them
    std::string label;                // Type.Method(params)
};

inline std::string cache_text(const std::string& key, const std::vector<Entry>& list) {
    std::string o = std::string(kCacheHeader) + "\nkey " + key + "\n";
    for (auto& e : list) {
        char b[160];
        std::snprintf(b, sizeof b, "t %u %lld %lld %d %lld|", e.index, e.raw, e.code, e.shared, e.other);
        o += b + e.label + "\n";
    }
    return o;
}

// True, with the cached results in `out` (same order as `now`), when the file is exactly one written for this executable
// and these methods, each still at its index, entry and code; false otherwise, with the reason in *why.
inline bool cache_hit(const std::string& text, const std::string& key, const std::vector<Entry>& now, std::vector<Entry>& out,
                      const char** why) {
    out.clear();
    if (text.empty()) { *why = "no cache file yet"; return false; }
    size_t pos = 0;
    auto line = [&](std::string& l) {
        if (pos >= text.size()) return false;
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) return false;   // a line not ended: a half-written file
        l = text.substr(pos, e - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        pos = e + 1;
        return true;
    };
    std::string l;
    *why = "";
    if (!line(l) || l != kCacheHeader) { *why = "a cache of another format"; return false; }
    if (!line(l) || l != "key " + key) { *why = "another game executable"; return false; }
    for (auto& want : now) {
        if (!line(l)) { *why = "fewer methods than asked"; out.clear(); return false; }
        size_t bar = l.find('|');
        if (bar == std::string::npos || l.substr(bar + 1) != want.label) { *why = "another list of methods"; out.clear(); return false; }
        Entry c;
        int n = 0;
        if (std::sscanf(l.c_str(), "t %u %lld %lld %d %lld%n", &c.index, &c.raw, &c.code, &c.shared, &c.other, &n) != 5 ||
            static_cast<size_t>(n) != bar || c.shared < 0) {
            *why = "unreadable result line";
            out.clear();
            return false;
        }
        if (c.index != want.index || c.raw != want.raw || c.code != want.code) { *why = "a method's entry or code moved"; out.clear(); return false; }
        c.label = want.label;
        out.push_back(c);
    }
    if (line(l) && !l.empty()) { *why = "more methods than asked"; out.clear(); return false; }
    return true;
}

}  // namespace slots::codecheck
