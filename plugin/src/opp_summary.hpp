// SF6 Slots — the summary of Opponent Outfit Settings with extra outfits: pure logic, no game access (plugin/tests).
// The game side is opp_summary.cpp.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

namespace slots::oppsum {

// The length the working copy's outfit list gets while the summary is drawn: the number of summary labels when the outfits
// outnumber them (the game writes one label per outfit and throws past its last label), else -1 (the list is left alone).
inline int draw_length(int outfits, int labels) {
    return labels > 0 && outfits > labels ? labels : -1;
}

// ---- the lines past the game's own (opp_summary.cpp) -------------------------------------------------------------------
constexpr int kOwnLinesMax = 19;      // 24 lines in all: down to about 840 at 1080p, above the Rotation guide

// A line as the game writes it, "<before><name><sep><count>": what goes round the name and the count, in that language, from a line of the game's whose name
// and count are known. Not found that way: ": ".
struct LineFormat { std::string before, sep = ": "; };
inline LineFormat line_format(const std::string& text, const std::string& name, int count) {
    LineFormat f;
    const size_t at = name.empty() ? std::string::npos : text.find(name);
    if (at == std::string::npos) return f;
    const std::string tail = text.substr(at + name.size());
    const std::string digits = std::to_string(count);
    if (tail.size() < digits.size() || tail.compare(tail.size() - digits.size(), digits.size(), digits) != 0) return f;
    f.before = text.substr(0, at);
    f.sep = tail.substr(0, tail.size() - digits.size());
    return f;
}

// Which of `extras` lines show in `room` rows, around `current` (its index among them, -1 when the outfit shown is not one of them). All of them when they fit.
// Otherwise the rows follow the current one, and a row at an end says how many are past it, so every line can be reached by changing the outfit.
struct LineWindow { int first = 0, count = 0, more_above = 0, more_below = 0; };
inline LineWindow line_window(int extras, int room, int current) {
    LineWindow w;
    if (extras <= 0 || room <= 0) return w;
    if (extras <= room) { w.count = extras; return w; }
    int top = current < 0 ? 0 : current - room / 2;
    if (top > extras - room) top = extras - room;
    if (top < 0) top = 0;
    const bool above = top > 0, below = top + room < extras;
    w.first = top + (above ? 1 : 0);
    w.count = room - (above ? 1 : 0) - (below ? 1 : 0);
    w.more_above = w.first;
    w.more_below = extras - w.first - w.count;
    return w;
}

// ---- R18: the code check of the hooked method, kept per game version ----------------------------------------------------
// reframework/data/SF6_CostumeSlots_data/codecheck.txt, three lines:
//   SF6 Slots code check 1
//   key exe 6A97881D 2618F000 25BB1081 size 632956832 time 1DD5AD806B56D43 methods 1455453
//   t <method index> <entry rva> <code rva> <other methods entering that code> <one of them, -1 none>|<Type.Method>
// The code rva is -1 when the code is outside the game's executable (another module hooked the method first: its trampoline
// moves at each start); the entry rva and the method index still pin the method.
constexpr const char* kCacheHeader = "SF6 Slots code check 1";

struct CodeCheck {
    unsigned index = 0;
    long long raw = 0, code = 0;      // entry and code, as RVAs
    int shared = -1;                  // other methods of the game entering the same entry or code (-1: not known)
    long long other = -1;             // the index of one of them
};

inline std::string cache_text(const std::string& key, const std::string& label, const CodeCheck& c) {
    char b[160];
    std::snprintf(b, sizeof b, "t %u %lld %lld %d %lld|", c.index, c.raw, c.code, c.shared, c.other);
    return std::string(kCacheHeader) + "\nkey " + key + "\n" + b + label + "\n";
}

// True, with the cached result in `out`, when the file is exactly one written for this game (key) and this method (label)
// and the method is still where it was (index, entry, code); false otherwise, with the reason in *why.
inline bool cache_hit(const std::string& text, const std::string& key, const std::string& label, const CodeCheck& now,
                      CodeCheck& out, const char** why) {
    if (text.empty()) { *why = "no cache file yet"; return false; }
    auto line = [&](size_t& pos, std::string& l) {
        if (pos >= text.size()) return false;
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) return false;   // a line not ended: a half-written file
        l = text.substr(pos, e - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        pos = e + 1;
        return true;
    };
    size_t pos = 0;
    std::string l;
    *why = "";
    if (!line(pos, l) || l != kCacheHeader) { *why = "a cache of another format"; return false; }
    if (!line(pos, l) || l != "key " + key) { *why = "another game executable"; return false; }
    if (!line(pos, l)) { *why = "no result line"; return false; }
    size_t bar = l.find('|');
    if (bar == std::string::npos || l.substr(bar + 1) != label) { *why = "another method"; return false; }
    CodeCheck c;
    int n = 0;
    if (std::sscanf(l.c_str(), "t %u %lld %lld %d %lld%n", &c.index, &c.raw, &c.code, &c.shared, &c.other, &n) != 5 ||
        static_cast<size_t>(n) != bar || c.shared < 0) {
        *why = "unreadable result line";
        return false;
    }
    if (c.index != now.index || c.raw != now.raw || c.code != now.code) { *why = "the method's entry or code moved"; return false; }
    out = c;
    return true;
}

}  // namespace slots::oppsum
