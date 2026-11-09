#include "report/report_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../common/outfit_display.hpp"            // what the loader writes in the game's own list for a label

namespace report {
namespace {

char low(char c) { return (char)std::tolower((unsigned char)c); }
bool sep(char c) { return c == '\\' || c == '/'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alnum(char c) { return std::isalnum((unsigned char)c) != 0; }

bool starts_nocase(const std::string& s, size_t at, const char* what) {
    for (size_t i = 0; what[i]; ++i) if (at + i >= s.size() || low(s[at + i]) != low(what[i])) return false;
    return true;
}

// Replaces every case-insensitive occurrence of `what`
std::string replace_nocase(const std::string& s, const std::string& what, const std::string& by) {
    if (what.empty()) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (starts_nocase(s, i, what.c_str())) { out += by; i += what.size(); }
        else out += s[i++];
    }
    return out;
}

// A path's separators, whichever way it was written: compared one for one
bool path_here(const std::string& s, size_t at, const std::string& path, size_t* end) {
    size_t i = at;
    for (size_t k = 0; k < path.size(); ++k) {
        if (i >= s.size()) return false;
        if (sep(path[k])) {
            if (!sep(s[i])) return false;
            while (i < s.size() && sep(s[i])) ++i;       // a doubled one: JSON's escaped backslash
        } else {
            if (low(s[i]) != low(path[k])) return false;
            ++i;
        }
    }
    *end = i;
    return true;
}

// Paths under a user's folder: <drive>:\Users\<name>, \Users\<name>, /Users/<name>, /home/<name>, Documents and Settings: all down to "<user>"
std::string cut_user_paths(const std::string& s) {
    static const char* const kRoots[] = { "users", "documents and settings", "home" };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool done = false;
        if (sep(s[i])) {
            size_t p = i;
            while (p < s.size() && sep(s[p])) ++p;
            for (const char* root : kRoots) {
                const size_t rl = std::strlen(root);
                if (!starts_nocase(s, p, root) || p + rl >= s.size() || !sep(s[p + rl])) continue;
                size_t q = p + rl;
                while (q < s.size() && sep(s[q])) ++q;
                // the user's name: up to the next separator, quote or end of line (a name may hold spaces)
                size_t e = q;
                while (e < s.size() && !sep(s[e]) && s[e] != '"' && s[e] != '\'' && s[e] != '\r' && s[e] != '\n' && s[e] != '|' && s[e] != '<' && s[e] != '>') ++e;
                if (e == q) continue;
                // what stands before: a drive and the \\?\ prefix go with it
                size_t cut = out.size();
                if (cut >= 2 && out[cut - 1] == ':' && std::isalpha((unsigned char)out[cut - 2])) {
                    cut -= 2;
                    if (cut >= 4 && out.compare(cut - 4, 4, "\\\\?\\") == 0) cut -= 4;
                }
                out.resize(cut);
                out += "<user>";
                i = e;
                done = true;
                break;
            }
        }
        if (!done) out += s[i++];
    }
    return out;
}

std::string cut_game_root(const std::string& s, const std::string& root) {
    if (root.size() < 3) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t e = 0;
        if (path_here(s, i, root, &e)) { out += "<game>"; i = e; }
        else out += s[i++];
    }
    return out;
}

// Steam's other forms and a profile's address
std::string cut_steam_forms(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[' && (starts_nocase(s, i, "[u:1:") || starts_nocase(s, i, "[g:1:"))) {
            size_t e = i + 5;
            while (e < s.size() && digit(s[e])) ++e;
            if (e < s.size() && s[e] == ']' && e > i + 5) { out += "<id>"; i = e + 1; continue; }
        }
        if ((s[i] == 'S' || s[i] == 's') && starts_nocase(s, i, "steam_") && i + 10 < s.size() && digit(s[i + 6]) && s[i + 7] == ':' && digit(s[i + 8]) && s[i + 9] == ':') {
            size_t e = i + 10;
            while (e < s.size() && digit(s[e])) ++e;
            if (e > i + 10) { out += "<id>"; i = e; continue; }
        }
        if ((s[i] == 's' || s[i] == 'S') && starts_nocase(s, i, "steamcommunity.com/")) {
            size_t p = i + 19;
            const bool id = starts_nocase(s, p, "id/"), prof = starts_nocase(s, p, "profiles/");
            if (id || prof) {
                p += id ? 3 : 9;
                size_t e = p;
                while (e < s.size() && (alnum(s[e]) || s[e] == '_' || s[e] == '-')) ++e;
                if (e > p) { out += "<id>"; i = e; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

// A SteamID64 wherever it stands; any other run of 9 digits or more when it stands alone (not part of a hexadecimal or a name)
std::string cut_numbers(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (!digit(s[i])) { out += s[i++]; continue; }
        size_t e = i;
        while (e < s.size() && digit(s[e])) ++e;
        const size_t n = e - i;
        const bool lone = (i == 0 || !alnum(s[i - 1])) && (e >= s.size() || !alnum(s[e]));
        const bool steam64 = n == 17 && s.compare(i, 7, "7656119") == 0;
        if (steam64 || (lone && n >= 9)) out += "<id>";
        else out.append(s, i, n);
        i = e;
    }
    return out;
}

std::string lower_hex_only(const std::string& s) {
    std::string out;
    for (char c : s) { const char l = low(c); if ((l >= 'a' && l <= 'z') || digit(l)) out += l; }
    return out;
}

// No control characters, no tag marks, no spaces around
std::string plain(const std::string& s) {   // (a row's own text)
    std::string out;
    for (const unsigned char c : s) if (c >= 0x20 && c != 0x7F && c != '<' && c != '>') out += (char)c;
    size_t a = 0, b = out.size();
    while (a < b && out[a] == ' ') ++a;
    while (b > a && out[b - 1] == ' ') --b;
    return out.substr(a, b - a);
}

std::string str_of(const json& o, const char* k) {
    if (!o.is_object()) return "";
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
int int_of(const json& o, const char* k, int dflt) {
    if (!o.is_object()) return dflt;
    auto it = o.find(k);
    return it != o.end() && it->is_number_integer() ? it->get<int>() : dflt;
}

}  // namespace

std::string clean_paths(const std::string& text, const Scrub& s) {
    std::string out = cut_game_root(text, s.game_root);      // first: a game folder under a user's folder stays one word
    out = cut_user_paths(out);
    for (const std::string& n : s.names) if (n.size() >= 3) out = replace_nocase(out, n, "<user>");
    return out;
}

std::string clean_log(const std::string& text, const Scrub& s) { return cut_numbers(cut_steam_forms(clean_paths(text, s))); }

std::string cut_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string cap_log(const std::string& text, size_t max_bytes, size_t head_bytes) {
    if (text.size() <= max_bytes) return text;
    constexpr size_t kMarker = 64;
    head_bytes = std::min(head_bytes, max_bytes / 2);
    size_t head = text.rfind('\n', head_bytes);              // whole lines where it can
    head = head == std::string::npos || head == 0 ? head_bytes : head + 1;
    while (head > 0 && head < text.size() && ((unsigned char)text[head] & 0xC0) == 0x80) --head;
    const size_t keep_tail = max_bytes - head - std::min(kMarker, max_bytes - head);
    size_t tail = text.size() - keep_tail;
    const size_t nl = text.find('\n', tail);
    if (nl != std::string::npos && nl + 1 < text.size()) tail = nl + 1;
    while (tail < text.size() && ((unsigned char)text[tail] & 0xC0) == 0x80) ++tail;
    char mark[kMarker];
    std::snprintf(mark, sizeof mark, "\n... %zu bytes left out ...\n", tail - head);
    return text.substr(0, head) + mark + text.substr(tail);
}

// ---- the loader's log: the last run, and what it says of one outfit -----------------------------------------------------------------------------
// Lines: "[2026-11-08 21:06:21] message"; a message may hold new lines (the next lines have no stamp). A run starts at "=== SF6 Costume Slot Loader ===".
// A mod's scan block starts at a stampless "  archive: <name>" (or "  add-on <name>") line and ends at the empty stamped line before the next block, or at the
// first stamped line of the next step (an indent of 2 that is no "option:").
namespace {

struct LogLine { size_t a = 0, b = 0; int indent = 0; bool stamped = false, empty = false, block_start = false, archive = false; };
struct Piece { size_t a, b; int prio; };        // [a, b) of the text; prio 0 goes first when the room is short

std::string lower_of(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = low(c);
    return out;
}

LogLine read_line(const std::string& t, size_t a, size_t b) {   // [a, b): the line without its new line
    LogLine l;
    l.a = a;
    l.b = b;
    size_t m = a;
    if (b - a >= 22 && t[a] == '[' && t[a + 20] == ']' && t[a + 21] == ' ') { l.stamped = true; m = a + 22; }
    size_t q = m;
    while (q < b && t[q] == ' ') ++q;
    l.indent = (int)(q - m);
    l.empty = l.stamped && q == b;
    if (!l.stamped) {
        const bool arch = t.compare(q, 9, "archive: ") == 0;
        l.block_start = l.indent == 2 && (arch || t.compare(q, 7, "add-on ") == 0);
        l.archive = l.block_start && arch;
    }
    return l;
}

bool block_ends(const std::string& t, const LogLine& l) {
    if (l.empty || l.block_start) return true;
    if (!l.stamped || l.indent > 2) return false;
    const size_t q = l.a + 22 + l.indent;
    return t.compare(q, 8, "option: ") != 0 && t.compare(q, 16, "partial option: ") != 0;
}

bool holds(const std::string& low_text, size_t a, size_t b, const std::string& key) {    // the key (lower case) in [a, b)
    return std::string_view(low_text).substr(a, b - a).find(key) != std::string_view::npos;
}

}  // namespace

std::string cap_log(const std::string& text, const LogKeys& keys, size_t max_bytes, size_t head_bytes) {
    if (text.size() <= max_bytes) return text;
    constexpr size_t kMarker = 64;
    size_t run = text.rfind("=== SF6 Costume Slot Loader ===");
    if (run == std::string::npos || keys.empty()) return cap_log(text, max_bytes, head_bytes);
    if (run > 0) { const size_t nl = text.rfind('\n', run); run = nl == std::string::npos ? 0 : nl + 1; }   // the line's start (its stamp)
    std::vector<LogLine> lines;
    for (size_t a = run; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        lines.push_back(read_line(text, a, b));
        a = b + 1;
    }
    const std::string lo = lower_of(text);
    std::vector<std::string> bk, nm;
    for (const std::string& k : keys.blocks) if (k.size() >= 4) bk.push_back(lower_of(k));
    for (const std::string& k : keys.names) if (k.size() >= 4) nm.push_back(lower_of(k));
    const std::string slot = keys.slot.size() >= 4 ? lower_of(keys.slot) : std::string();
    const auto in_line = [&](const LogLine& l, const std::vector<std::string>& ks) { for (const std::string& k : ks) if (holds(lo, l.a, l.b, k)) return true; return false; };

    // the header: up to the first empty stamped line (the scan of the mods follows), kLogRunHeaderBytes at most
    size_t h_end = run;
    for (const LogLine& l : lines) { if (l.empty && l.a > run) break; h_end = l.b + 1; }
    h_end = std::min(h_end, text.size());
    if (h_end - run > kLogRunHeaderBytes) {
        const size_t nl = text.rfind('\n', run + kLogRunHeaderBytes);
        h_end = nl == std::string::npos || nl <= run ? run + kLogRunHeaderBytes : nl + 1;
    }

    std::vector<Piece> found;
    for (size_t i = 0; i < lines.size(); ++i) {            // the outfit's mod: its whole scan block, when it holds the archive's name or the mod's id
        if (!lines[i].block_start) continue;
        size_t j = i + 1;
        while (j < lines.size() && !block_ends(text, lines[j])) ++j;
        const size_t a = lines[i].a, b = std::min(lines[j - 1].b + 1, text.size());
        bool hit = false;
        for (const std::string& k : bk) hit = hit || holds(lo, a, b, k);
        if (hit) found.push_back({ a, b, lines[i].archive ? 1 : 0 });
        i = j - 1;
    }
    for (size_t i = 0; i < lines.size(); ++i) {            // the lines
        const LogLine& l = lines[i];
        const size_t e = std::min(l.b + 1, text.size());
        if (!slot.empty() && holds(lo, l.a, l.b, "slot " + slot)) {      // "slot esf028/012 (...)": the deeper lines just before it tell how it was built
            size_t first = i;
            while (first > 0 && i - first < 12 && lines[first - 1].stamped && lines[first - 1].indent > l.indent) --first;
            found.push_back({ lines[first].a, e, 2 });
        } else if ((!slot.empty() && holds(lo, l.a, l.b, slot)) || in_line(l, bk)) {
            found.push_back({ l.a, e, 1 });
        } else if (in_line(l, nm)) {
            found.push_back({ l.a, e, 0 });
        }
    }
    std::sort(found.begin(), found.end(), [](const Piece& x, const Piece& y) { return x.a < y.a || (x.a == y.a && x.b > y.b); });
    std::vector<Piece> kept;                               // after the header, in order, joined where they touch
    for (Piece p : found) {
        p.a = std::max(p.a, h_end);
        if (p.a >= p.b) continue;
        if (!kept.empty() && p.a <= kept.back().b) { kept.back().b = std::max(kept.back().b, p.b); kept.back().prio = std::max(kept.back().prio, p.prio); }
        else kept.push_back(p);
    }
    if (kept.empty()) return cap_log(text, max_bytes, head_bytes);

    // the room: the header, a marker for each piece and for the end, kLogTailBytes of the end at least; what does not fit goes by priority, last first
    const size_t tail_min = std::min(kLogTailBytes, text.size() - h_end);
    const size_t fixed = (h_end - run) + tail_min + kMarker * (kept.size() + 2);
    const size_t room = max_bytes > fixed ? max_bytes - fixed : 0;
    size_t total = 0;
    for (const Piece& p : kept) total += p.b - p.a;
    for (int prio = 0; prio <= 2 && total > room; ++prio) {
        for (size_t i = kept.size(); i-- > 0 && total > room;) {
            Piece& p = kept[i];
            if (p.prio != prio) continue;
            size_t nb = p.b - std::min(p.b - p.a, total - room);
            const size_t nl = nb > p.a ? text.rfind('\n', nb - 1) : std::string::npos;      // whole lines
            nb = nl != std::string::npos && nl >= p.a ? nl + 1 : p.a;
            total -= p.b - nb;
            p.b = nb;
        }
    }
    kept.erase(std::remove_if(kept.begin(), kept.end(), [](const Piece& p) { return p.a >= p.b; }), kept.end());

    // the end of the log: what room is left (tail_min at least)
    const size_t used = (h_end - run) + total + kMarker * (kept.size() + 2);
    const size_t want = std::min(text.size() - h_end, std::max(tail_min, max_bytes > used ? max_bytes - used : 0));
    const size_t lowest = kept.empty() ? h_end : kept.back().b;       // (never back over a piece)
    size_t tail = std::max(text.size() - want, lowest);
    if (tail > lowest) {
        const size_t nl = text.find('\n', tail);
        if (nl != std::string::npos && nl + 1 < text.size()) tail = nl + 1;
    }
    while (tail < text.size() && ((unsigned char)text[tail] & 0xC0) == 0x80) ++tail;

    std::string out;
    const auto gap = [&](size_t from, size_t to) {
        if (to <= from) return;
        char mark[kMarker];
        std::snprintf(mark, sizeof mark, "... %zu bytes left out ...\n", to - from);
        out += mark;
    };
    if (run > 0) { char mark[kMarker]; std::snprintf(mark, sizeof mark, "... %zu bytes left out (the earlier runs) ...\n", run); out += mark; }
    out += text.substr(run, h_end - run);
    size_t at = h_end;
    for (const Piece& p : kept) { gap(at, p.a); out += text.substr(p.a, p.b - p.a); at = p.b; }
    gap(at, tail);
    out += text.substr(tail);
    return out;
}

// ---- hashes ---------------------------------------------------------------------------------------------------------------------
std::string sha256_hex(const void* data, size_t n) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const auto rr = [](uint32_t x, int c) { return (x >> c) | (x << (32 - c)); };
    const auto block = [&](const uint8_t* b) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
        for (int i = 16; i < 64; ++i) w[i] = w[i - 16] + (rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t v[8];
        std::memcpy(v, h, sizeof v);
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = v[7] + (rr(v[4], 6) ^ rr(v[4], 11) ^ rr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
            const uint32_t t2 = (rr(v[0], 2) ^ rr(v[0], 13) ^ rr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) h[i] += v[i];
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = n;
    for (; left >= 64; left -= 64, p += 64) block(p);
    uint8_t last[128] = {};
    if (left) std::memcpy(last, p, left);
    last[left] = 0x80;
    const size_t tail = left < 56 ? 64 : 128;
    const uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; ++i) last[tail - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    block(last);
    if (tail == 128) block(last + 64);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

std::string fnv64_hex(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = (uint32_t)p[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (n - i == 2) {
        const uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ---- the outfit -----------------------------------------------------------------------------------------------------------------
std::string row_name(const std::string& row) {
    std::string s = row;
    if (const size_t at = s.find("  <ICON"); at != std::string::npos) s.resize(at);
    return plain(s);
}

const json* find_outfit(const json& outfits, int fighter, const std::string& row) {
    if (!outfits.is_object() || fighter <= 0 || row.empty()) return nullptr;
    auto it = outfits.find("outfits");
    if (it == outfits.end() || !it->is_array()) return nullptr;
    const std::string want = plain(row);
    const json* by_label = nullptr;
    for (const json& o : *it) {
        if (int_of(o, "fighter", -1) != fighter) continue;
        if (str_of(o, "name") == want) return &o;            // "Outfit VII": one per fighter, the row's own word
        if (!by_label && outfit_display_name(str_of(o, "label")) == want) by_label = &o;
    }
    return by_label;
}

const json* find_slot(const json& registry, int fighter, int costume_no) {
    if (!registry.is_object() || fighter <= 0 || costume_no < 0) return nullptr;
    auto it = registry.find("slots");
    if (it == registry.end() || !it->is_array()) return nullptr;
    for (const json& s : *it)
        if (int_of(s, "fighter", -1) == fighter && int_of(s, "new_costume_no", -1) == costume_no) return &s;
    return nullptr;
}

const json* find_outfit_number(const json& outfits, int fighter, int costume_no) {
    if (!outfits.is_object() || fighter <= 0 || costume_no < 0) return nullptr;
    auto it = outfits.find("outfits");
    if (it == outfits.end() || !it->is_array()) return nullptr;
    for (const json& o : *it) if (int_of(o, "fighter", -1) == fighter && int_of(o, "costume_no", -1) == costume_no) return &o;
    return nullptr;
}

std::string costume_key(int fighter, const std::string& row, const json* outfit, const json* slot, bool data_read, int costume_no) {
    const int f = fighter > 0 ? fighter : 0;
    std::string lowered;
    for (const char c : plain(row)) lowered += low(c);
    const std::string word = fnv64_hex(lowered);
    if (outfit) {
        std::string id = slot ? lower_hex_only(str_of(*slot, "mod_id")) : std::string();
        // a record without its slot: what makes the outfit itself, which a mod's update changes too
        if (id.empty()) id = fnv64_hex(str_of(*outfit, "source") + "|" + str_of(*outfit, "label"));
        const int based = slot ? int_of(*slot, "original_costume_no", 0) : int_of(*outfit, "based_on", 0);
        return "mod:" + id.substr(0, 40) + ":f" + std::to_string(f) + ":c" + std::to_string(based < 0 ? 0 : based);
    }
    return std::string(data_read ? "game:" : "unk:") + std::to_string(f) + ":" + (costume_no >= 0 ? "c" + std::to_string(costume_no) : word);
}

bool costume_key_ok(const std::string& key) {
    const size_t c = key.find(':');
    if (c == std::string::npos) return false;
    const std::string kind = key.substr(0, c), rest = key.substr(c + 1);
    if ((kind != "mod" && kind != "game" && kind != "unk") || rest.empty() || rest.size() > 72) return false;
    return std::all_of(rest.begin(), rest.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || digit(ch) || ch == ':' || ch == '.' || ch == '_' || ch == '-'; });
}

// ---- what goes out --------------------------------------------------------------------------------------------------------------
json make_command(const Report& r) {
    json c = json::object();
    c["cmd"] = "report";
    c["costume_key"] = r.costume_key;
    c["text"] = r.text;
    c["character"] = r.character;
    if (r.fighter > 0) c["fighter_id"] = r.fighter;
    c["outfit"] = r.outfit;
    c["outfit_row"] = r.outfit_row;
    if (r.costume_no >= 0) c["costume_no"] = r.costume_no;
    c["colour"] = r.colour;
    c["versions"] = r.versions;
    c["logs"] = r.logs;
    c["record"] = r.record;
    c["mods"] = r.mods;
    c["notes"] = r.notes;
    if (!r.jpeg.empty()) c["image"] = base64(r.jpeg.data(), r.jpeg.size());
    return c;
}

// ---- the menu ---------------------------------------------------------------------------------------------------------------------
Server server_from(bool exists, const std::string& status) {
    if (!exists) return Server::None;
    if (status == "fixed") return Server::Fixed;
    if (status == "confirmed") return Server::Confirmed;
    if (status == "fixed_soon") return Server::FixedSoon;
    if (status == "fixed_update") return Server::FixedUpdate;
    return Server::Waiting;
}

void Session::reset() {
    stage_ = Stage::Idle;
    server_ = Server::Checking;
    pick_ = Pick::Held;
    busy_ = Busy::None;
    std::string().swap(text_);
    std::string().swap(sent_text_);
    fixed_in_.clear();
    error_.clear();
    outcome_ = Outcome::None;
    field_ok_ = true;
    window_gone_ = false;
    text_typed_ = false;
}

bool Session::begin() {
    if (stage_ != Stage::Idle) return false;
    reset();
    stage_ = Stage::Shooting;
    return true;
}

void Session::server_answer(Server s, const std::string& text, const std::string& fixed_in) {
    if (server_ != Server::Checking) return;
    server_ = s;
    fixed_in_ = fixed_in;
    sent_text_ = text;
    if (!text_typed_) text_ = text;
    pick_ = waiting_like() || s == Server::Fixed ? Pick::Held : Pick::Ok;
}

std::string Session::option_label(const std::string& key, const Tr& tr) const {
    const bool named = key == "report_st_fixed_soon" || key == "report_st_fixed_update";
    return tr(key, named ? std::vector<std::string>{ shown_safe(fixed_in_) } : std::vector<std::string>{});
}

std::vector<std::string> Session::options() const {
    switch (server_) {
        case Server::Checking: return { "report_st_checking" };
        case Server::Waiting: return { "report_st_waiting" };
        case Server::FixedSoon: return { "report_st_fixed_soon" };
        case Server::FixedUpdate: return { "report_st_fixed_update" };
        case Server::Fixed: return { "report_st_to_test", "report_st_ok", "report_st_bug" };
        default: return { "report_st_ok", "report_st_bug" };
    }
}

int Session::option_index() const {
    switch (server_) {
        case Server::Checking: return 0;
        case Server::Waiting: case Server::FixedSoon: case Server::FixedUpdate: return 0;
        case Server::Fixed: return pick_ == Pick::Held ? 0 : pick_ == Pick::Ok ? 1 : 2;
        default: return pick_ == Pick::Bug ? 1 : 0;
    }
}

bool Session::choose(int index) {
    if (stage_ != Stage::Open || busy_ != Busy::None || server_ == Server::Checking || index == option_index()) return false;
    const std::vector<std::string> o = options();
    if (index < 0 || index >= (int)o.size()) return false;
    const std::string& k = o[(size_t)index];
    pick_ = k == "report_st_ok" ? Pick::Ok : k == "report_st_bug" ? Pick::Bug : Pick::Held;
    outcome_ = Outcome::None;
    error_.clear();
    return true;
}

std::vector<Row> Session::rows() const {
    std::vector<Row> r{ Row::Status };
    if (stage_ != Stage::Open || server_ == Server::Checking) return r;
    if (pick_ == Pick::Bug || waiting_like()) { r.push_back(Row::Text); r.push_back(Row::Send); }
    else if (pick_ == Pick::Ok && server_ == Server::Fixed) r.push_back(Row::MarkFixed);
    return r;
}

bool Session::send() {
    if (stage_ != Stage::Open || busy_ != Busy::None || send_disabled()) return false;
    busy_ = Busy::Sending;
    outcome_ = Outcome::None;
    error_.clear();
    return true;
}

bool Session::start_status() {
    if (stage_ != Stage::Open || busy_ != Busy::None) return false;
    busy_ = Busy::Status;
    outcome_ = Outcome::None;
    error_.clear();
    return true;
}

void Session::answered(bool sent, const std::string& error) {
    if (busy_ != Busy::Sending) return;
    busy_ = Busy::None;
    if (window_gone_) { reset(); return; }               // nobody is looking any more
    outcome_ = sent ? Outcome::Sent : Outcome::Failed;
    error_ = sent ? std::string() : error;
    if (sent) { server_ = Server::Waiting; pick_ = Pick::Held; sent_text_ = text_; }
}

void Session::status_answered(bool ok, Server now, const std::string& error) {
    if (busy_ != Busy::Status) return;
    busy_ = Busy::None;
    if (window_gone_) { reset(); return; }
    if (ok) { server_ = now; pick_ = Pick::Ok; outcome_ = Outcome::None; error_.clear(); return; }
    outcome_ = Outcome::Failed;                          // (the rows stay as they are: the row can try again)
    error_ = error;
}

bool Session::closed() {
    if (stage_ == Stage::Idle) return true;
    if (busy_ != Busy::None) { window_gone_ = true; return false; }
    reset();
    return true;
}

std::string shown_safe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '<' || c == '[') out += '(';
        else if (c == '>' || c == ']') out += ')';
        else if (c == '\n' || c == '\r' || c == '\t') out += ' ';
        else out += c;
    }
    return out;
}

std::string row_title(const Session& s, Row row, const Tr& tr) {
    switch (row) {
        case Row::Status: return tr("report_status", {});
        case Row::Text: return tr(s.text().empty() ? "report_row_describe" : "report_row_change", {});
        case Row::MarkFixed: return tr("report_row_mark_fixed", {});
        case Row::Send: break;
    }
    return tr(s.has_report() ? "report_row_update" : "report_row_send", {});
}

static bool form_shown(const Session& s) {
    const std::vector<Row> rows = s.rows();
    return rows.size() >= 3 && rows[1] == Row::Text;
}

std::vector<SummaryLine> summary_lines(const Session& s, const MenuValues& v, const Tr& tr) {
    std::vector<SummaryLine> out;
    if (s.stage() != Stage::Open || !form_shown(s)) return out;
    out.push_back({ tr("report_sum_character", { shown_safe(v.character) }) });
    const std::string colour = shown_safe(v.colour);
    out.push_back({ tr("report_sum_outfit", { shown_safe(v.outfit) + (colour.empty() ? "" : ", " + colour) }), 2 });
    if (!s.text().empty()) out.push_back({ tr("report_sum_description", {}) });
    return out;
}

std::string message_text(const Session& s, Row focus, const Tr& tr) {
    if (s.stage() != Stage::Open) return "";
    if (s.busy() != Busy::None) return tr("report_sending", {});
    if (s.outcome() == Session::Outcome::Failed) return tr("report_failed", { tr(error_key(s.error()), {}) });
    if (s.outcome() == Session::Outcome::Sent && s.update_blocked()) return tr("report_msg_sent", {});
    if (s.server() == Server::Fixed && s.pick() == Pick::Held) return tr("report_msg_to_test", {});
    const bool soon = s.server() == Server::FixedSoon, update = s.server() == Server::FixedUpdate;
    if ((soon || update) && focus != Row::Send)      // (on Send the form's rules come first: the text, the warning on Update)
        return tr(soon ? "report_msg_fixed_soon" : "report_msg_fixed_update", { shown_safe(s.fixed_in()) });
    const std::string unreachable = s.server() == Server::Unknown ? tr("report_msg_unreachable", {}) : "";
    if (!form_shown(s)) return unreachable;
    const std::string replace = s.has_report() ? tr("report_msg_replace", {}) : "";
    if (focus != Row::Send) return !unreachable.empty() ? unreachable : replace;
    if (s.needs_text()) return tr("report_msg_need_text", {});
    if (s.update_blocked()) return tr("report_msg_same_text", {});
    const std::string also = tr("report_msg_also", {});   // Wael, 07/11: only here, with the warning on Update
    return replace.empty() ? also : replace + " " + also;
}

int scroll_top(int total, int window, uint64_t frames) {
    const int steps = total - window;
    if (steps <= 0 || window <= 0) return 0;
    const uint64_t period = 2 * kScrollRestFrames + (uint64_t)steps * kScrollStepFrames;
    const uint64_t x = frames % period;
    if (x < kScrollRestFrames) return 0;
    const uint64_t done = 1 + (x - kScrollRestFrames) / kScrollStepFrames;
    return done >= (uint64_t)steps ? steps : (int)done;
}

std::string summary_text(const Session& s, const MenuValues& v, const Tr& tr, const Wrap& wrap, uint64_t frames, SummaryScroll* scroll) {
    std::vector<std::string> lines;
    for (const SummaryLine& line : summary_lines(s, v, tr))
        for (const std::string& piece : wrap(line.text, line.max_lines)) lines.push_back(piece);
    if (scroll) *scroll = SummaryScroll{};
    if (!lines.empty() && !s.text().empty() && (int)lines.size() < kSummaryLines) {
        std::string safe = s.text();                     // its own line breaks stay: the rest as in shown_safe
        for (char& c : safe) c = c == '<' || c == '[' ? '(' : c == '>' || c == ']' ? ')' : c == '\r' || c == '\t' ? ' ' : c;
        const std::vector<std::string> d = wrap(safe, kDescriptionMaxLines);
        const int window = kSummaryLines - (int)lines.size();
        const int top = scroll_top((int)d.size(), window, frames);
        if (scroll) *scroll = SummaryScroll{ (int)lines.size(), (int)d.size(), window, top };
        for (int i = top; i < (int)d.size() && i < top + window; ++i) lines.push_back(d[(size_t)i]);
    }
    std::string out;
    for (const std::string& l : lines) { if (!out.empty()) out += '\n'; out += l; }
    return out;
}

size_t wide_chars(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (len == 3 && i + 2 < s.size()) {
            const uint32_t cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu);
            if ((cp >= 0x2E80 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF01 && cp <= 0xFF60)) ++n;
        } else if (len == 4) {
            ++n;
        }
        i += len;
    }
    return n;
}

static size_t u8_len_at(const std::string& s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return i + n > s.size() ? s.size() - i : n;
}
// One character: px per em of the width table, read in game at this font. The classes the font is the widest on, each on its own measure.
static double char_px(const std::string& ch, double em) {
    if (ch.size() == 1) {
        const char k = ch[0];
        if (k >= '0' && k <= '9') return em * 21.0;
        if (k >= 'A' && k <= 'Z') return em * 20.0;
        return em * 18.2;
    }
    return wide_chars(ch) ? em * 18.2 * kWideDrawn : em * 21.0;
}

double drawn_px(const std::string& s, const EmOf& em_of) {
    double px = 0;
    for (size_t i = 0; i < s.size();) {
        const size_t n = u8_len_at(s, i);
        const std::string ch = s.substr(i, n);
        px += char_px(ch, em_of(ch));
        i += n;
    }
    return px;
}

std::vector<std::string> wrap_px(const std::string& text, double max_px, int max_lines, const EmOf& em_of) {
    std::vector<std::string> lines;
    bool more = false;
    const auto push = [&](std::string l) {
        while (!l.empty() && l.back() == ' ') l.pop_back();
        size_t b = 0;
        while (b < l.size() && l[b] == ' ') ++b;
        l.erase(0, b);
        if (l.empty()) return;
        if ((int)lines.size() < max_lines) lines.push_back(std::move(l));
        else more = true;
    };
    std::string cur;
    double cur_px = 0;
    size_t last_space = std::string::npos;
    for (size_t i = 0; i < text.size();) {
        const size_t n = u8_len_at(text, i);
        const std::string ch = text.substr(i, n);
        i += n;
        if (ch == "\n") { push(cur); cur.clear(); cur_px = 0; last_space = std::string::npos; continue; }
        const double w = char_px(ch, em_of(ch));
        if (cur_px + w > max_px && !cur.empty()) {
            if (last_space != std::string::npos && ch != " ") {
                push(cur.substr(0, last_space));
                cur.erase(0, last_space + 1);
            } else {
                push(cur);
                cur.clear();
            }
            cur_px = drawn_px(cur, em_of);
            last_space = cur.rfind(' ');
        }
        cur += ch;
        cur_px += w;
        if (ch == " ") last_space = cur.size() - 1;
    }
    push(cur);
    if (more && !lines.empty()) {
        std::string& last = lines.back();
        const std::string dots = "...";
        while (!last.empty() && drawn_px(last + dots, em_of) > max_px) {
            size_t k = last.size() - 1;
            while (k > 0 && ((unsigned char)last[k] & 0xC0) == 0x80) --k;
            last.erase(k);
        }
        last += dots;
    }
    return lines;
}

std::wstring paste_text(const std::wstring& clipboard, size_t max_units) {
    std::wstring out;
    const auto room = [&](size_t units) { return out.size() + units <= max_units; };
    for (size_t i = 0; i < clipboard.size(); ++i) {
        const wchar_t ch = clipboard[i];
        if (ch == L'\r' || ch == L'\n') {
            if (ch == L'\r' && i + 1 < clipboard.size() && clipboard[i + 1] == L'\n') ++i;
            if (!room(2)) break;
            out += L"/n";
        } else if (ch == L'\t') {
            if (!room(1)) break;
            out += L' ';
        } else if (ch < 0x20 || (ch >= 0x7F && ch < 0xA0) || ch == L'<' || ch == L'>') {
            continue;
        } else if (ch >= 0xD800 && ch < 0xDC00) {        // a pair goes whole
            if (i + 1 >= clipboard.size() || clipboard[i + 1] < 0xDC00 || clipboard[i + 1] > 0xDFFF) continue;
            if (!room(2)) break;
            out += ch;
            out += clipboard[++i];
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            continue;
        } else {
            if (!room(1)) break;
            out += ch;
        }
    }
    return out;
}

std::string error_key(const std::string& code) {
    if (code == "banned" || code == "steam_banned") return "report_err_banned";
    if (code == "verification_unavailable" || code == "invalid_token" || code == "update_required") return "report_err_steam";
    if (code == "storage_full") return "report_err_full";
    if (code == "account_full") return "report_err_account";
    if (code == "bad_transition" || code == "not_found") return "report_err_status";
    if (code == "too_large" || code == "bad_image" || code == "bad_key" || code == "bad_json") return "report_err_refused";
    return "report_err_network";
}

// ---- the guide entry and the key ---------------------------------------------------------------------------------------------------
unsigned qwerty_scan(const std::string& symbol) {
    const std::string tag = "<ICON Key";
    const size_t at = symbol.find(tag);
    if (at == std::string::npos || at + tag.size() + 1 >= symbol.size() || symbol[at + tag.size() + 1] != '>') return 0;
    const char c = (char)std::toupper((unsigned char)symbol[at + tag.size()]);
    static const char* rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const unsigned base[] = { 0x10, 0x1E, 0x2C };
    for (int r = 0; r < 3; ++r)
        for (const char* k = rows[r]; *k; ++k)
            if (*k == c) return base[r] + (unsigned)(k - rows[r]);
    if (c >= '1' && c <= '9') return 0x02 + (unsigned)(c - '1');
    if (c == '0') return 0x0B;
    return 0;
}

bool entry_allowed(const ScreenRead& s) {
    return s.can_report && s.agent_drawn && s.tab == 0 && s.costume_row && s.input_on && !s.window_open && !s.dialog_up && !s.key_taken;
}

}  // namespace report
