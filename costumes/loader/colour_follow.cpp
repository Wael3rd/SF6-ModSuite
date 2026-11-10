// colour_follow.cpp -- the Color Mod's colours follow their outfit to its number (see colour_follow.hpp)

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "colour_follow.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>

#define NOMINMAX
#include <windows.h>

namespace colour_follow {

// ============================================================================
// Small helpers
// ============================================================================

namespace {

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
bool digits(const std::string& s, size_t at, size_t n) {
    if (at + n > s.size()) return false;
    for (size_t i = at; i < at + n; ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
int number_at(const std::string& s, size_t at, size_t n) {
    int v = 0;
    for (size_t i = at; i < at + n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}
std::string three(int v) { char b[16]; snprintf(b, sizeof b, "%03d", v); return b; }
std::string esf(int f) { char b[16]; snprintf(b, sizeof b, "esf%03d", f); return b; }
std::string v2(int n) { char b[16]; snprintf(b, sizeof b, "v%02d", n); return b; }
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return out;
        a = b + 1;
    }
}
std::vector<std::string> lines_of(const std::string& text) {   // without their "\r\n" / "\n"; no last empty line
    std::vector<std::string> out = split(text, '\n');
    for (auto& l : out) if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}
bool to_int(const std::string& s, int& v) {
    if (s.empty() || s.size() > 9) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size() || !digits(s, i, s.size() - i)) return false;
    v = atoi(s.c_str());
    return true;
}
// "12_5": a fighter and a costume number, as the Color Mod keys an outfit (chip_slots.json)
bool parse_fc(const std::string& key, int& f, int& n) {
    const size_t u = key.find('_');
    if (u == std::string::npos || u == 0 || u > 4 || key.size() - u - 1 == 0 || key.size() - u - 1 > 4) return false;
    if (!digits(key, 0, u) || !digits(key, u + 1, key.size() - u - 1)) return false;
    f = number_at(key, 0, u);
    n = number_at(key, u + 1, key.size() - u - 1);
    return true;
}
}  // namespace

// ============================================================================
// Outfits
// ============================================================================

std::string identity_key(const Outfit& o) {
    std::string id;
    for (char c : o.mod_id) id += (isalnum((unsigned char)c) && (unsigned char)c < 0x80) ? c : '_';
    if (id.empty()) id = "none";
    if (id.size() > 32) id.resize(32);
    char b[32];
    snprintf(b, sizeof b, "_f%03d_c%02d", o.fighter, o.based_on);
    return lower(id) + b;
}

bool is_identity_key(const std::string& s) {
    std::string k = s;
    // "_n<number>": an identity two outfits had at once, kept apart
    const size_t n = k.rfind("_n");
    if (n != std::string::npos && n + 2 < k.size() && digits(k, n + 2, k.size() - n - 2)) k.resize(n);
    const size_t c = k.rfind("_c");
    if (c == std::string::npos || c + 2 >= k.size() || !digits(k, c + 2, k.size() - c - 2)) return false;
    if (c < 6 || k.compare(c - 5, 2, "_f") != 0 || !digits(k, c - 3, 3)) return false;
    const size_t f = c - 5;
    if (f == 0) return false;
    for (size_t i = 0; i < f; ++i) if (!isalnum((unsigned char)k[i]) && k[i] != '_') return false;
    return true;
}

std::string normal_name(const std::string& name) {
    std::string out;
    bool space = false;
    for (unsigned char c : name) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = !out.empty(); continue; }
        if (space) { out += ' '; space = false; }
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : char(c);
    }
    return out;
}

std::vector<Step> decide(const std::vector<Outfit>& former_in, const std::vector<Outfit>& now_in,
                         const std::set<std::string>& aside, const std::string& stamp) {
    // one outfit per number of a fighter (the first one listed)
    auto one_per_number = [](const std::vector<Outfit>& v) {
        std::vector<Outfit> out;
        std::set<std::pair<int, int>> seen;
        for (auto& o : v) if (o.number >= 0 && seen.insert({ o.fighter, o.number }).second) out.push_back(o);
        return out;
    };
    const std::vector<Outfit> former = one_per_number(former_in), now = one_per_number(now_in);
    auto id = [](const Outfit& o) { return o.mod_id + "\t" + std::to_string(o.fighter) + "\t" + std::to_string(o.based_on); };
    std::map<std::string, int> in_former, in_now;
    for (auto& o : former) in_former[id(o)]++;
    for (auto& o : now) in_now[id(o)]++;
    std::vector<int> mf(former.size(), -1), mn(now.size(), -1);
    std::vector<Why> why(now.size(), Why::Same);
    std::map<std::pair<int, int>, size_t> at_former;
    for (size_t i = 0; i < former.size(); ++i) at_former[{ former[i].fighter, former[i].number }] = i;
    auto match = [&](size_t i, size_t j, Why w) { mf[i] = (int)j; mn[j] = (int)i; why[j] = w; };

    // 1. the same outfit at the same number
    for (size_t j = 0; j < now.size(); ++j) {
        auto it = at_former.find({ now[j].fighter, now[j].number });
        if (it != at_former.end() && id(former[it->second]) == id(now[j])) match(it->second, j, Why::Same);
    }
    // 2. the same outfit at another number: an identity once in each list
    {
        std::map<std::string, std::pair<std::vector<size_t>, std::vector<size_t>>> by_id;
        for (size_t i = 0; i < former.size(); ++i) if (mf[i] < 0) by_id[id(former[i])].first.push_back(i);
        for (size_t j = 0; j < now.size(); ++j) if (mn[j] < 0) by_id[id(now[j])].second.push_back(j);
        for (auto& [k, g] : by_id)
            if (g.first.size() == 1 && g.second.size() == 1) match(g.first[0], g.second[0], Why::Same);
    }
    // 3. installed again: its colours wait in .former_outfits\<identity> (an identity two outfits have now: neither)
    std::vector<bool> back(now.size(), false);
    for (size_t j = 0; j < now.size(); ++j)
        if (mn[j] < 0 && in_now[id(now[j])] == 1 && aside.count(identity_key(now[j]))) back[j] = true;
    // 4. its mod updated: the one outfit of that fighter and name that went away, the one that appeared. Fighter, name and
    // game costume first (a mod with several outfits of one fighter), then fighter and name.
    {
        std::map<std::string, std::pair<std::vector<size_t>, std::vector<size_t>>> by_name, by_costume;
        for (size_t i = 0; i < former.size(); ++i) {
            const std::string nm = normal_name(former[i].name);
            if (mf[i] >= 0 || nm.empty()) continue;
            const std::string k = std::to_string(former[i].fighter) + "\t" + nm;
            by_name[k].first.push_back(i);
            by_costume[k + "\t" + std::to_string(former[i].based_on)].first.push_back(i);
        }
        for (size_t j = 0; j < now.size(); ++j) {
            const std::string nm = normal_name(now[j].name);
            if (mn[j] >= 0 || back[j] || nm.empty()) continue;
            const std::string k = std::to_string(now[j].fighter) + "\t" + nm;
            by_name[k].second.push_back(j);
            by_costume[k + "\t" + std::to_string(now[j].based_on)].second.push_back(j);
        }
        for (auto& [k, g] : by_costume)
            if (g.first.size() == 1 && g.second.size() == 1) match(g.first[0], g.second[0], Why::Updated);
        for (auto& [k, g] : by_name)
            if (g.first.size() == 1 && g.second.size() == 1 && mf[g.first[0]] < 0 && mn[g.second[0]] < 0)
                match(g.first[0], g.second[0], Why::Updated);
    }
    auto label = [](const Outfit& o) { return o.name.empty() ? o.mod_id : o.name; };
    std::vector<Step> steps;
    // moved: the colours go with their outfit
    for (size_t j = 0; j < now.size(); ++j) {
        if (mn[j] < 0) continue;
        const Outfit& f = former[(size_t)mn[j]];
        if (f.number != now[j].number)
            steps.push_back({ why[j], now[j].fighter, f.number, now[j].number, identity_key(now[j]), label(now[j]) });
    }
    // installed again
    for (size_t j = 0; j < now.size(); ++j)
        if (back[j]) steps.push_back({ Why::Back, now[j].fighter, -1, now[j].number, identity_key(now[j]), label(now[j]) });
    // gone: set aside under their identity (one two outfits had: with their number, never put back on its own)
    for (size_t i = 0; i < former.size(); ++i) {
        if (mf[i] >= 0) continue;
        std::string key = identity_key(former[i]);
        if (in_former[id(former[i])] > 1) key += "_n" + std::to_string(former[i].number);
        steps.push_back({ Why::Gone, former[i].fighter, former[i].number, -1, key, label(former[i]) });
    }
    // a number an outfit gets that no known outfit had: whatever is there is not its colours
    for (size_t j = 0; j < now.size(); ++j) {
        const bool settled = mn[j] >= 0 && former[(size_t)mn[j]].number == now[j].number;
        if (!settled && !at_former.count({ now[j].fighter, now[j].number }))
            steps.push_back({ Why::Unknown, now[j].fighter, now[j].number, -1, stamp, label(now[j]) });
    }
    return steps;
}

// ============================================================================
// File names and paths
// ============================================================================

bool parse_manual_name(const std::string& name, int& fighter, int& number) {
    // esfFFF_CCC_cmd_KKK.user.2, letters of any case (the Color Mod's own rule)
    if (name.size() != 25) return false;
    const std::string l = lower(name);
    if (l.compare(0, 3, "esf") != 0 || l[6] != '_' || l.compare(10, 5, "_cmd_") != 0 || l.compare(18, 7, ".user.2") != 0) return false;
    if (!digits(l, 3, 3) || !digits(l, 7, 3) || !digits(l, 15, 3)) return false;
    const int c = number_at(l, 7, 3);
    if (c < 1) return false;
    fighter = number_at(l, 3, 3);
    number = c - 1;               // folder 001 = costume 0
    return true;
}

std::string renumber_manual_name(const std::string& name, int number) {
    int f = 0, n = 0;
    if (!parse_manual_name(name, f, n) || number < 0 || number > 998) return name;
    std::string out = name;
    out.replace(7, 3, three(number + 1));
    return out;
}

bool parse_data_name(const std::string& name, int& fighter, int& number, std::string& rest) {
    const std::string l = lower(name);
    if (l.size() < 12 || l.compare(l.size() - 7, 7, ".user.2") != 0) return false;
    size_t a = 0;
    while (a < name.size() && name[a] >= '0' && name[a] <= '9') ++a;
    if (a == 0 || a > 4 || a >= name.size() || name[a] != '_') return false;
    size_t b = a + 1;
    while (b < name.size() && name[b] >= '0' && name[b] <= '9') ++b;
    if (b == a + 1 || b - a - 1 > 4 || b >= name.size() || name[b] != '_') return false;
    if (b + 1 >= name.size() - 7) return false;
    fighter = number_at(name, 0, a);
    number = number_at(name, a + 1, b - a - 1);
    rest = name.substr(b + 1, name.size() - 7 - (b + 1));
    return true;
}

bool override_follows(const std::string& rest) {
    // "<f>_<n>_file_<the colour's path, sanitised>": an edited copy of a colour file. An MC colour's (MANUALCOLORS in its
    // path, not in an archive's .cache: the Color Mod's own test) goes with the outfit; a colour pack's stays with its file
    // and its number. "<f>_<n>_game_<id>" (the game colours edited) and the rest are the outfit's.
    if (rest.compare(0, 5, "file_") != 0) return true;
    return rest.find("MANUALCOLORS") != std::string::npos && rest.find("__cache_") == std::string::npos;
}

std::string renumber_data_name(const std::string& name, int number) {
    int f = 0, n = 0;
    std::string rest;
    if (!parse_data_name(name, f, n, rest) || number < 0) return name;
    // an MC colour's edited copy ends with its colour's file name, sanitised: "..._esf001_006_cmd_000_user_2"
    if (rest.compare(0, 5, "file_") == 0 && rest.size() >= 30 && number <= 998) {
        const size_t at = rest.size() - 25;
        const std::string t = lower(rest.substr(at));
        if (t.compare(0, 3, "esf") == 0 && t[6] == '_' && t.compare(10, 5, "_cmd_") == 0 && t.compare(18, 7, "_user_2") == 0
            && digits(t, 3, 3) && digits(t, 7, 3) && digits(t, 15, 3))
            rest.replace(at + 7, 3, three(number + 1));
    }
    return std::to_string(f) + "_" + std::to_string(number) + "_" + rest + name.substr(name.size() - 7);
}

bool parse_manual_path(const std::string& path, int& fighter, int& number) {
    // an MC colour as the Color Mod's files name it: "..\color_mods\<folders>\<file>", MANUALCOLORS among the folders,
    // none of them starting with a dot (an archive's .cache)
    std::string p = path;
    for (char& c : p) if (c == '/') c = '\\';
    static const std::string pre = "..\\color_mods\\";
    if (p.compare(0, pre.size(), pre) != 0) return false;
    const std::vector<std::string> parts = split(p.substr(pre.size()), '\\');
    if (parts.size() < 2) return false;
    bool manual = false;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty() || parts[i][0] == '.') return false;
        if (i + 1 < parts.size() && parts[i].find("MANUALCOLORS") != std::string::npos) manual = true;
    }
    return manual && parse_manual_name(parts.back(), fighter, number);
}

std::string renumber_manual_path(const std::string& path, int number) {
    const size_t sl = path.find_last_of("\\/");
    const size_t at = sl == std::string::npos ? 0 : sl + 1;
    return path.substr(0, at) + renumber_manual_name(path.substr(at), number);
}

std::string live_path(const ColourFile& f) {
    return f.base + (f.sub.empty() ? "" : "\\" + f.sub) + "\\" + f.name;
}
std::string aside_path(const ColourFile& f, const std::string& key) {
    return f.base + "\\.former_outfits\\" + key + (f.sub.empty() ? "" : "\\" + f.sub) + "\\" + f.name;
}

// ============================================================================
// Minimal JSON
// ============================================================================

namespace {

struct JsonReader {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    explicit JsonReader(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; }
    bool str(std::string& raw) {
        if (i >= s.size() || s[i] != '"') return false;
        const size_t a = i++;
        while (i < s.size() && s[i] != '"') {
            if ((unsigned char)s[i] < 0x20) return false;
            if (s[i] == '\\') ++i;
            ++i;
        }
        if (i >= s.size()) return false;
        raw = s.substr(a, ++i - a);
        return true;
    }
    bool value(JVal& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        bool ok = true;
        if (s[i] == '{') {
            v.t = 'o';
            ++i; ws();
            if (i < s.size() && s[i] == '}') ++i;
            else for (;;) {
                std::string k;
                ws();
                JVal m;
                if (!str(k)) { ok = false; break; }
                ws();
                if (i >= s.size() || s[i] != ':') { ok = false; break; }
                ++i;
                if (!value(m)) { ok = false; break; }
                v.members.push_back({ k, std::move(m) });
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                ok = false; break;
            }
        } else if (s[i] == '[') {
            v.t = 'a';
            ++i; ws();
            if (i < s.size() && s[i] == ']') ++i;
            else for (;;) {
                JVal m;
                if (!value(m)) { ok = false; break; }
                v.items.push_back(std::move(m));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                ok = false; break;
            }
        } else if (s[i] == '"') {
            v.t = 's';
            ok = str(v.raw);
        } else {
            const size_t a = i;
            while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.')) ++i;
            v.t = 'v';
            v.raw = s.substr(a, i - a);
            ok = !v.raw.empty();
        }
        --depth;
        return ok;
    }
};

const char* eol_of(const std::string& text) { return text.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

JVal* member(JVal& o, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    for (auto& [n, v] : o.members) if (n == k) return &v;
    return nullptr;
}

}  // namespace

bool json_parse(const std::string& text, JVal& out, std::string* tail) {
    JsonReader r(text);
    out = JVal();
    if (!r.value(out)) return false;
    const size_t end = r.i;
    r.ws();
    if (r.i != text.size()) return false;
    if (tail) *tail = text.substr(end);
    return true;
}

std::string json_dump(const JVal& v, const char* eol, int level) {
    const bool lines = eol && *eol;
    auto pad = [&](int l) { return lines ? std::string(size_t(l) * 4, ' ') : std::string(); };
    if (v.t == 'o' || v.t == 'a') {
        const bool obj = v.t == 'o';
        const size_t n = obj ? v.members.size() : v.items.size();
        if (n == 0) return obj ? "{}" : "[]";
        std::string out = obj ? "{" : "[";
        for (size_t k = 0; k < n; ++k) {
            out += (lines ? std::string(eol) : std::string()) + pad(level + 1);
            if (obj) out += v.members[k].first + (lines ? ": " : ":") + json_dump(v.members[k].second, eol, level + 1);
            else out += json_dump(v.items[k], eol, level + 1);
            if (k + 1 < n) out += ",";
        }
        out += (lines ? std::string(eol) : std::string()) + pad(level) + (obj ? "}" : "]");
        return out;
    }
    return v.raw;
}

std::string json_unquote(const std::string& token) {
    if (token.size() < 2 || token.front() != '"' || token.back() != '"') return token;
    std::string out;
    auto utf8 = [&](uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    };
    auto hex4 = [&](size_t at, uint32_t& cp) {
        if (at + 4 > token.size() - 1) return false;
        cp = 0;
        for (size_t k = at; k < at + 4; ++k) {
            const char c = token[k];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    };
    for (size_t i = 1; i + 1 < token.size(); ++i) {
        if (token[i] != '\\' || i + 2 >= token.size()) { out += token[i]; continue; }
        const char e = token[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            uint32_t cp = 0;
            if (!hex4(i + 1, cp)) { out += "\\u"; break; }
            i += 4;
            uint32_t lo = 0;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < token.size() && token[i + 1] == '\\' && token[i + 2] == 'u' && hex4(i + 3, lo)
                && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            utf8(cp);
            break;
        }
        default: out += e; break;   // \" \\ \/
        }
    }
    return out;
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += char(c);
        }
    }
    return out + "\"";
}

// ============================================================================
// The Color Mod's three files, and the entries set aside with the files
// ============================================================================

static const char* kRecordHeader = "SF6 Slots Loader colours set aside 1";
static const char* kTicksHeader = "SF6 Color Mod opponent colours 1";   // engine/opponent_ticks.hpp kFileHeader

Record parse_record(const std::string& text) {
    Record r;
    const auto ls = lines_of(text);
    if (ls.empty() || ls[0] != kRecordHeader) return r;
    for (size_t i = 1; i < ls.size(); ++i) {
        const auto p = split(ls[i], '\t');
        int f = 0, n = 0;
        if (p.size() == 5 && p[0] == "chip" && to_int(p[1], f) && to_int(p[2], n)) r.chips.push_back({ f, n, p[3], p[4] });
        else if (p.size() == 4 && p[0] == "tick" && to_int(p[1], f) && to_int(p[2], n)) r.ticks.push_back({ f, n, p[3] });
    }
    return r;
}

std::string record_text(const Record& r) {
    std::string out = std::string(kRecordHeader) + "\n";
    for (auto& c : r.chips)
        out += "chip\t" + std::to_string(c.fighter) + "\t" + std::to_string(c.number) + "\t" + c.key + "\t" + c.value + "\n";
    for (auto& t : r.ticks)
        out += "tick\t" + std::to_string(t.fighter) + "\t" + std::to_string(t.number) + "\t" + t.id + "\n";
    return out;
}

namespace {
void append(Record& to, const Record& from) {
    to.chips.insert(to.chips.end(), from.chips.begin(), from.chips.end());
    to.ticks.insert(to.ticks.end(), from.ticks.begin(), from.ticks.end());
}
}  // namespace

Rewrite rewrite_state(const std::string& text, const Renumber& r) {
    // state.json: the choices { p1, p2, fs } = { rw, path, fighter, costume } and the one parked until its colour is back
    // (path_pending = { path, fighter, costume }). A choice of an MC colour of an outfit that moved goes with it: its costume
    // and its colour's path. Set aside or a pack's colour: left as it is (the Color Mod parks a colour it can't find).
    Rewrite w;
    if (r.move.empty()) return w;
    JVal root;
    std::string tail;
    if (!json_parse(text, root, &tail)) return w;
    auto num = [](JVal* v, int& out) { return v && v->t == 'v' && to_int(v->raw, out); };
    // the MC colour of an outfit that moved this choice names: its new number (-1: none)
    auto moved_to = [&](int f, int c, JVal* path) {
        int pf = 0, pn = 0;
        if (!path || path->t != 's') return -1;
        auto m = r.move.find({ f, c });
        if (m == r.move.end() || !parse_manual_path(json_unquote(path->raw), pf, pn) || pf != f || pn != c) return -1;
        return m->second.first;
    };
    std::function<void(JVal&)> walk = [&](JVal& o) {
        if (o.t == 'a') for (auto& it : o.items) walk(it);
        if (o.t != 'o') return;
        int f = 0, c = 0;
        if (num(member(o, "fighter"), f) && num(member(o, "costume"), c) && r.move.count({ f, c })) {
            JVal* path = member(o, "path");
            JVal* pend = member(o, "path_pending");
            const int to_path = moved_to(f, c, path);
            int pf = 0, pc = 0, to_pend = -1;
            const bool has_pend = pend && pend->t == 'o';
            if (has_pend && num(member(*pend, "fighter"), pf) && num(member(*pend, "costume"), pc) && pf == f && pc == c)
                to_pend = moved_to(f, c, member(*pend, "path"));
            const bool no_path = !path || (path->t == 's' && json_unquote(path->raw).empty()) || (path->t == 'v' && path->raw == "null");
            const bool pend_ok = !has_pend || to_pend >= 0;
            const bool path_ok = no_path || to_path >= 0;
            if ((to_path >= 0 || to_pend >= 0) && path_ok && pend_ok) {
                const int to = r.move.at({ f, c }).first;
                member(o, "costume")->raw = std::to_string(to);
                if (to_path >= 0) path->raw = json_quote(renumber_manual_path(json_unquote(path->raw), to));
                if (to_pend >= 0) {
                    member(*pend, "costume")->raw = std::to_string(to);
                    JVal* pp = member(*pend, "path");
                    pp->raw = json_quote(renumber_manual_path(json_unquote(pp->raw), to));
                }
                ++w.changed;
            }
            return;   // a choice holds no other choice
        }
        for (auto& [k, v] : o.members) walk(v);
    };
    walk(root);
    if (w.changed) w.text = json_dump(root, text.find('\n') != std::string::npos ? eol_of(text) : "") + tail;
    return w;
}

Rewrite rewrite_chips(const std::string& text, const Renumber& r, const std::map<std::string, Record>& records) {
    // chip_slots.json: "<fighter>_<costume>" (the reference slots of the outfit's squares) and "<MC colour's path>" (the
    // squares saved with it). Moved: renamed; set aside: taken out into the record; back: put in again where nothing is.
    Rewrite w;
    auto leave_all = [&]() {
        for (auto& [ft, key] : r.back)
            if (auto rec = records.find(key); rec != records.end()) w.left[key].chips = rec->second.chips;
    };
    if (r.move.empty() && r.out.empty() && r.back.empty()) return w;
    JVal root;
    std::string tail;
    const bool none = text.find_first_not_of(" \t\r\n") == std::string::npos;
    if (none) root.t = 'o';
    else if (!json_parse(text, root, &tail) || root.t != 'o') { leave_all(); return w; }
    auto where = [](const std::string& key, int& f, int& n) { return parse_fc(key, f, n) || parse_manual_path(key, f, n); };
    auto renamed = [](const std::string& key, int to) {
        int f = 0, n = 0;
        return parse_fc(key, f, n) ? std::to_string(f) + "_" + std::to_string(to) : renumber_manual_path(key, to);
    };
    std::vector<std::pair<std::string, JVal>> kept;
    struct Moved { std::string key; JVal value; int fighter, to; std::string owner; };
    std::vector<Moved> moved;
    for (auto& [tok, val] : root.members) {
        const std::string key = json_unquote(tok);
        int f = 0, n = 0;
        if (where(key, f, n)) {
            if (auto o = r.out.find({ f, n }); o != r.out.end()) {
                w.add[o->second].chips.push_back({ f, n, tok, json_dump(val, "") });
                ++w.changed;
                continue;
            }
            if (auto m = r.move.find({ f, n }); m != r.move.end()) {
                moved.push_back({ renamed(key, m->second.first), val, f, m->second.first, m->second.second });
                ++w.changed;
                continue;
            }
        }
        kept.push_back({ tok, val });
    }
    std::set<std::string> have;
    for (auto& [tok, v] : kept) have.insert(json_unquote(tok));
    for (auto& m : moved) {
        if (have.insert(m.key).second) kept.push_back({ json_quote(m.key), m.value });
        else w.add[m.owner].chips.push_back({ m.fighter, m.to, json_quote(m.key), json_dump(m.value, "") });   // taken: kept aside
    }
    for (auto& [ft, key] : r.back) {
        auto rec = records.find(key);
        if (rec == records.end()) continue;
        for (auto& c : rec->second.chips) {
            JVal v;
            const std::string k = renamed(json_unquote(c.key), ft.second);
            if (c.fighter != ft.first || !json_parse(c.value, v) || !have.insert(k).second) { w.left[key].chips.push_back(c); continue; }
            kept.push_back({ json_quote(k), v });
            ++w.changed;
        }
    }
    if (!w.changed) return w;
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return json_unquote(a.first) < json_unquote(b.first); });
    root.members = std::move(kept);
    w.text = json_dump(root, none || text.find('\n') != std::string::npos ? (none ? "\r\n" : eol_of(text)) : "") + tail;
    return w;
}

Rewrite rewrite_ticks(const std::string& text, const Renumber& r, const std::map<std::string, Record>& records) {
    // opponent_colours.txt: a header, then "fighter<TAB>costume<TAB>entry's path" per tick. An MC colour's tick goes with its
    // outfit; a pack's colour (RW) stays with its number.
    Rewrite w;
    if (r.move.empty() && r.out.empty() && r.back.empty()) return w;
    const auto ls = lines_of(text);
    if (!ls.empty() && ls[0] != kTicksHeader) {   // another format: the Color Mod leaves it aside too
        for (auto& [ft, key] : r.back)
            if (auto rec = records.find(key); rec != records.end()) w.left[key].ticks = rec->second.ticks;
        return w;
    }
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> out{ kTicksHeader };
    std::set<std::string> have;
    for (size_t i = 1; i < ls.size(); ++i) {
        std::string line = ls[i];
        const auto p = split(line, '\t');
        int f = 0, c = 0, pf = 0, pn = 0;
        if (p.size() >= 3 && to_int(p[0], f) && to_int(p[1], c)) {
            const std::string id = line.substr(p[0].size() + p[1].size() + 2);
            if (parse_manual_path(id, pf, pn) && pf == f && pn == c) {
                if (auto o = r.out.find({ f, c }); o != r.out.end()) {
                    w.add[o->second].ticks.push_back({ f, c, id });
                    ++w.changed;
                    continue;
                }
                if (auto m = r.move.find({ f, c }); m != r.move.end()) {
                    line = std::to_string(f) + "\t" + std::to_string(m->second.first) + "\t" + renumber_manual_path(id, m->second.first);
                    ++w.changed;
                }
            }
        }
        if (have.insert(line).second) out.push_back(line);
    }
    for (auto& [ft, key] : r.back) {
        auto rec = records.find(key);
        if (rec == records.end()) continue;
        for (auto& t : rec->second.ticks) {
            if (t.fighter != ft.first) { w.left[key].ticks.push_back(t); continue; }
            const std::string line = std::to_string(t.fighter) + "\t" + std::to_string(ft.second) + "\t" + renumber_manual_path(t.id, ft.second);
            if (have.insert(line).second) out.push_back(line);   // already ticked: the same tick
            ++w.changed;
        }
    }
    if (!w.changed) return w;
    for (auto& l : out) w.text += l + eol;
    return w;
}

// ============================================================================
// Owners
// ============================================================================

static const char* kOwnersHeader = "SF6 Slots Loader colour owners 1";

std::string registry_digest(const std::vector<Outfit>& slots) {
    std::vector<std::string> items;
    for (auto& o : slots)
        items.push_back(o.mod_id + "|" + std::to_string(o.fighter) + "|" + std::to_string(o.based_on) + "|" + std::to_string(o.number));
    std::sort(items.begin(), items.end());
    std::string all;
    for (auto& s : items) all += s + "\n";
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)fnv1a(all));
    return b;
}

bool parse_owners(const std::string& text, Owners& out) {
    out = Owners();
    const auto ls = lines_of(text);
    if (ls.empty() || ls[0] != kOwnersHeader) return false;
    for (size_t i = 1; i < ls.size(); ++i) {
        const auto p = split(ls[i], '\t');
        Outfit o;
        if (p.size() >= 2 && p[0] == "registry") { out.registries.insert(p.begin() + 1, p.end()); continue; }
        if (p.size() == 5 && to_int(p[0], o.fighter) && to_int(p[1], o.number) && to_int(p[3], o.based_on)) {
            o.mod_id = p[2];
            o.name = p[4];
            out.outfits.push_back(o);
        }
    }
    out.found = true;
    return true;
}

std::string owners_text(const std::vector<Outfit>& outfits, const std::set<std::string>& registries) {
    std::string out = std::string(kOwnersHeader) + "\nregistry";
    for (auto& r : registries) out += "\t" + r;
    out += "\n";
    for (auto& o : outfits) {
        std::string nm = o.name, id = o.mod_id;
        for (char& c : nm) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        for (char& c : id) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        out += std::to_string(o.fighter) + "\t" + std::to_string(o.number) + "\t" + id + "\t" + std::to_string(o.based_on) + "\t" + nm + "\n";
    }
    return out;
}

std::vector<Outfit> choose_former(const Owners& owners, const std::vector<Outfit>& registry,
                                  const std::map<std::pair<int, int>, std::string>& labels, std::string& source) {
    // colour_owners.txt, written with the registry it goes with: what the files on disk follow. A registry it does not
    // know was written by another loader version since (its colours set aside by number, as that version does): the
    // registry is then what they follow, as at the first launch with this version. Neither: nothing is known (first install).
    if (owners.found && (registry.empty() || owners.registries.count(registry_digest(registry)))) {
        source = "colour_owners.txt";
        return owners.outfits;
    }
    if (registry.empty()) { source.clear(); return {}; }
    source = owners.found ? "registry.json (written by another loader version since)" : "registry.json (first launch of this loader)";
    std::vector<Outfit> out = registry;
    for (auto& o : out)
        if (auto it = labels.find({ o.fighter, o.number }); it != labels.end()) o.name = it->second;
    return out;
}

std::map<std::pair<int, int>, std::string> outfit_labels(const std::string& outfits_json) {
    std::map<std::pair<int, int>, std::string> out;
    JVal root;
    if (!json_parse(outfits_json, root) || root.t != 'o') return out;
    JVal* list = member(root, "outfits");
    if (!list || list->t != 'a') return out;
    for (auto& o : list->items) {
        if (o.t != 'o') continue;
        JVal *f = member(o, "fighter"), *c = member(o, "costume_no"), *l = member(o, "label"), *k = member(o, "kind");
        int fi = 0, cn = 0;
        if (!f || !c || !l || f->t != 'v' || c->t != 'v' || l->t != 's' || !to_int(f->raw, fi) || !to_int(c->raw, cn)) continue;
        const bool pak_name = k && k->t == 's' && json_unquote(k->raw) == "fluffy pak";   // a pak's file name, not the mod's
        out[{ fi, cn }] = pak_name ? std::string() : json_unquote(l->raw);
    }
    return out;
}

// ============================================================================
// The plan
// ============================================================================

std::set<std::string> aside_keys(const Listing& in) {
    std::set<std::string> keys;
    for (auto& f : in.files) if (!f.key.empty()) keys.insert(f.key);
    for (auto& [k, text] : in.records) if (!parse_record(text).empty()) keys.insert(k);
    return keys;
}

Plan make_plan(const std::vector<Step>& steps, const Listing& in, const Place& place,
               const std::function<bool(const std::string&)>& exists) {
    Plan p;
    std::set<std::string> taken;   // places this plan gives (lower case: Windows ignores case)
    auto is_free = [&](const std::string& path) { return !taken.count(lower(path)) && !exists(path); };
    auto take = [&](const std::string& path) { taken.insert(lower(path)); return path; };
    // a place in .former_outfits\<key>, never over a file there: "name (2).user.2"...
    auto aside_place = [&](const ColourFile& f, const std::string& key) {
        std::string path = aside_path(f, key);
        const size_t ext = f.name.size() >= 7 ? f.name.size() - 7 : f.name.size();
        for (int k = 2; !is_free(path); ++k) {
            ColourFile g = f;
            g.name = f.name.substr(0, ext) + " (" + std::to_string(k) + ")" + f.name.substr(ext);
            path = aside_path(g, key);
        }
        return take(path);
    };
    // the temporary name beside it (a dot name: the Color Mod reads none)
    auto temp_place = [&](const ColourFile& f) {
        ColourFile g = f;
        for (int k = 1;; ++k) {
            g.name = ".colfollow" + (k > 1 ? std::to_string(k) : std::string()) + "_" + f.name;
            if (is_free(live_path(g))) return take(live_path(g));
        }
    };
    auto renamed = [](const ColourFile& f, int number) {
        ColourFile g = f;
        g.key.clear();
        g.number = number;
        g.name = f.kind == ColourFile::Manual ? renumber_manual_name(f.name, number) : renumber_data_name(f.name, number);
        return g;
    };
    Renumber rn;
    for (const Step& st : steps) {
        if (st.from >= 0 && st.to >= 0) rn.move[{ st.fighter, st.from }] = { st.to, st.key };
        else if (st.from >= 0) rn.out[{ st.fighter, st.from }] = st.key;
        else if (st.to >= 0) rn.back[{ st.fighter, st.to }] = st.key;
        int n = 0;
        for (const ColourFile& f : in.files) {
            if (f.fighter != st.fighter) continue;
            if (st.from >= 0 ? !(f.key.empty() && f.number == st.from) : f.key != st.key) continue;
            if (st.to >= 0) {
                const ColourFile g = renamed(f, st.to);
                const std::string dst = live_path(g);
                if (taken.count(lower(dst))) continue;   // two set-aside copies of one colour: the first comes back
                take(dst);
                if (st.from >= 0) {
                    const std::string t = temp_place(f);
                    p.first.push_back({ live_path(f), t, "", false });
                    p.second.push_back({ t, dst, aside_place(g, st.key), false });   // taken then: kept aside for the outfit
                } else {
                    p.second.push_back({ aside_path(f, f.key), dst, "", false });   // taken then: stays where it is
                }
            } else {
                const std::string t = temp_place(f);
                p.first.push_back({ live_path(f), t, "", false });
                p.second.push_back({ t, aside_place(f, st.key), "", false });
            }
            ++n;
        }
        p.files += n;
        if (!n) continue;
        char line[1024];
        const std::string who = esf(st.fighter);
        switch (st.why) {
        case Why::Same:
            snprintf(line, sizeof line, "  colours follow their outfit: %s (%s) %s -> %s, %d file(s)\n", st.label.c_str(), who.c_str(),
                     v2(st.from).c_str(), v2(st.to).c_str(), n);
            break;
        case Why::Updated:
            snprintf(line, sizeof line, "  colours follow their outfit, its mod updated (same name): %s (%s) %s -> %s, %d file(s)\n",
                     st.label.c_str(), who.c_str(), v2(st.from).c_str(), v2(st.to).c_str(), n);
            break;
        case Why::Back:
            snprintf(line, sizeof line, "  colours put back: %s (%s/%s), %d file(s) from .former_outfits\\%s\n", st.label.c_str(),
                     who.c_str(), v2(st.to).c_str(), n, st.key.c_str());
            break;
        case Why::Gone:
            snprintf(line, sizeof line, "  colours set aside: %s (%s/%s, no longer installed), %d file(s) in .former_outfits\\%s\n",
                     st.label.c_str(), who.c_str(), v2(st.from).c_str(), n, st.key.c_str());
            break;
        default:
            snprintf(line, sizeof line, "  colours of no known outfit set aside: %s/%s (now %s), %d file(s) in .former_outfits\\%s\n",
                     who.c_str(), v2(st.from).c_str(), st.label.c_str(), n, st.key.c_str());
            break;
        }
        p.lines.push_back(line);
    }

    // the Color Mod's three files and the records of the entries set aside
    std::map<std::string, Record> records, add, left;
    for (auto& [k, text] : in.records) records[k] = parse_record(text);
    std::set<std::string> consumed;
    for (auto& [ft, key] : rn.back) consumed.insert(key);
    std::string summary;
    auto take_rewrite = [&](Rewrite w, const char* name, bool has) {
        for (auto& [k, rec] : w.add) append(add[k], rec);
        for (auto& [k, rec] : w.left) append(left[k], rec);
        if (w.text.empty() || (!has && w.changed == 0)) return;
        p.writes.push_back({ place.data + "\\" + name, w.text });
        p.entries += w.changed;
        summary += (summary.empty() ? "" : ", ") + std::string(name) + " (" + std::to_string(w.changed) + ")";
    };
    if (in.has_state) take_rewrite(rewrite_state(in.state, rn), "state.json", true);
    take_rewrite(rewrite_chips(in.has_chips ? in.chips : std::string(), rn, records), "chip_slots.json", in.has_chips);
    take_rewrite(rewrite_ticks(in.has_ticks ? in.ticks : std::string(), rn, records), "opponent_colours.txt", in.has_ticks);
    if (!summary.empty()) p.lines.push_back("  Color Mod entries following the colours: " + summary + "\n");
    std::set<std::string> keys = consumed;
    for (auto& [k, rec] : add) keys.insert(k);
    for (const std::string& k : keys) {
        Record rec;
        if (consumed.count(k)) rec = left[k];                                    // what could not be put back
        else if (auto it = records.find(k); it != records.end()) rec = it->second;  // what was set aside before
        if (auto it = add.find(k); it != add.end()) append(rec, it->second);
        const std::string path = place.data + "\\.former_outfits\\" + k + "\\colour_entries.txt";
        auto old = in.records.find(k);
        if (!rec.empty()) {
            const std::string text = record_text(rec);
            if (old == in.records.end() || old->second != text) p.writes.push_back({ path, text });
        } else if (old != in.records.end()) {
            p.second.push_back({ path, place.data + "\\.former_outfits\\" + k + "\\colour_entries.restored.txt", "", true });
        }
    }
    for (auto& [path, text] : p.writes) p.second.push_back({ path + ".colfollow_new", path, "", true });
    return p;
}

// ============================================================================
// The journal
// ============================================================================

static const char* kJournalHeader = "SF6 Slots Loader colour journal 1";

std::string journal_text(const Plan& plan, int phase) {
    std::string out = std::string(kJournalHeader) + "\nphase\t" + std::to_string(phase) + "\n";
    for (auto& op : plan.first) out += "1\t" + op.src + "\t" + op.dst + "\t" + op.fallback + "\n";
    for (auto& op : plan.second) out += std::string(op.replace ? "R" : "2") + "\t" + op.src + "\t" + op.dst + "\t" + op.fallback + "\n";
    return out + "end\n";
}

bool parse_journal(const std::string& text, Plan& plan, int& phase) {
    plan = Plan();
    phase = 0;
    const auto ls = lines_of(text);
    if (ls.size() < 3 || ls[0] != kJournalHeader || ls.back() != "end") return false;   // "end": written whole
    for (size_t i = 1; i + 1 < ls.size(); ++i) {
        const auto p = split(ls[i], '\t');
        if (p.size() == 2 && p[0] == "phase" && to_int(p[1], phase)) continue;
        if (p.size() != 4 || p[1].empty() || p[2].empty()) return false;
        Op op{ p[1], p[2], p[3], p[0] == "R" };
        if (p[0] == "1") plan.first.push_back(op);
        else if (p[0] == "2" || p[0] == "R") plan.second.push_back(op);
        else return false;
    }
    return phase == 1 || phase == 2;
}

// ============================================================================
// The pass (files)
// ============================================================================

namespace {

using Log = std::function<void(const std::string&)>;

std::wstring widen(const std::string& s, UINT cp) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {   // UTF-8: every name round-trips
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
// paths are UTF-8 inside the pass; a long one gets the \\?\ prefix
std::wstring wp(const std::string& path) {
    std::wstring w = widen(path, CP_UTF8);
    if (w.size() >= 240 && w.size() > 2 && w[1] == L':') w = L"\\\\?\\" + w;
    return w;
}
std::string full_path(const std::string& ansi) {
    const std::wstring w = widen(ansi, CP_ACP);
    wchar_t buf[4096];
    const DWORD n = GetFullPathNameW(w.c_str(), 4096, buf, nullptr);
    std::string out = narrow(n > 0 && n < 4096 ? std::wstring(buf, n) : w);
    while (out.size() > 3 && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    return out;
}
bool exists(const std::string& p) { return GetFileAttributesW(wp(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
std::string parent(const std::string& p) {
    const size_t s = p.find_last_of('\\');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
void make_dirs(const std::string& dir) {
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '\\') CreateDirectoryW(wp(dir.substr(0, i)).c_str(), nullptr);
}
std::string read_text(const std::string& p, bool* found = nullptr) {
    if (found) *found = false;
    HANDLE h = CreateFileW(wp(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    if (found) *found = true;
    return out;
}
bool write_text(const std::string& p, const std::string& s) {   // flushed to the disk
    make_dirs(parent(p));
    HANDLE h = CreateFileW(wp(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = (s.empty() || (WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size())) && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(wp(p).c_str());
    return ok;
}
bool write_atomic(const std::string& p, const std::string& s) {   // a temporary file, then renamed over
    const std::string tmp = p + ".tmp";
    if (!write_text(tmp, s)) return false;
    if (MoveFileExW(wp(tmp).c_str(), wp(p).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(wp(tmp).c_str());
    return false;
}
template <class F> void each(const std::string& dir, F f) {   // f(name, is_dir); dot names left out (the Color Mod's rule)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        f(narrow(fd.cFileName), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
// every file below dir (depth levels at most): f(sub, name), sub relative to dir
template <class F> void each_below(const std::string& dir, const std::string& sub, int depth, F& f) {
    each(sub.empty() ? dir : dir + "\\" + sub, [&](const std::string& n, bool d) {
        if (!d) f(sub, n);
        else if (depth > 0) each_below(dir, sub.empty() ? n : sub + "\\" + n, depth - 1, f);
    });
}

Listing list_files(const Place& pl) {
    Listing L;
    // MC colours: a colour file below color_mods with MANUALCOLORS in its path (how the Color Mod tells them from RW)
    each(pl.color_mods, [&](const std::string& top, bool dir) {
        if (!dir) return;
        const std::string base = pl.color_mods + "\\" + top;
        auto live = [&](const std::string& sub, const std::string& n) {
            int f = 0, num = 0;
            if (parse_manual_name(n, f, num) && (top + "\\" + sub).find("MANUALCOLORS") != std::string::npos)
                L.files.push_back({ ColourFile::Manual, base, sub, n, "", f, num });
        };
        each_below(base, "", 12, live);
        // set aside: <top>\.former_outfits\<identity>\...
        const std::string fo = base + "\\.former_outfits";
        each(fo, [&](const std::string& key, bool d) {
            if (!d || !is_identity_key(key)) return;
            auto aside = [&](const std::string& sub, const std::string& n) {
                int f = 0, num = 0;
                if (parse_manual_name(n, f, num)) L.files.push_back({ ColourFile::Manual, base, sub, n, key, f, num });
            };
            each_below(fo + "\\" + key, "", 12, aside);
        });
    });
    // edited copies and captured game colours
    for (auto kind : { ColourFile::Override, ColourFile::Pristine }) {
        const std::string dir = pl.data + (kind == ColourFile::Override ? "\\overrides" : "\\pristine");
        auto add = [&](const std::string& n, const std::string& key) {
            int f = 0, num = 0;
            std::string rest;
            if (parse_data_name(n, f, num, rest) && (kind == ColourFile::Pristine || override_follows(rest)))
                L.files.push_back({ kind, dir, "", n, key, f, num });
        };
        each(dir, [&](const std::string& n, bool d) { if (!d) add(n, ""); });
        each(dir + "\\.former_outfits", [&](const std::string& key, bool d) {
            if (d && is_identity_key(key))
                each(dir + "\\.former_outfits\\" + key, [&](const std::string& n, bool dd) { if (!dd) add(n, key); });
        });
    }
    // the entries set aside with them (a date folder's too: a second pass in the same minute adds to it)
    each(pl.data + "\\.former_outfits", [&](const std::string& key, bool d) {
        bool found = false;
        const std::string text = d ? read_text(pl.data + "\\.former_outfits\\" + key + "\\colour_entries.txt", &found) : "";
        if (found) L.records[key] = text;
    });
    L.state = read_text(pl.data + "\\state.json", &L.has_state);
    L.chips = read_text(pl.data + "\\chip_slots.json", &L.has_chips);
    L.ticks = read_text(pl.data + "\\opponent_colours.txt", &L.has_ticks);
    return L;
}

// One move: never over a file (a place taken: the fallback, else left where it is); a replace is for our own files
bool do_op(const Op& op, const Log& log) {
    if (!exists(op.src)) return true;   // done already, or nothing there
    make_dirs(parent(op.dst));
    if (op.replace) {
        if (MoveFileExW(wp(op.src).c_str(), wp(op.dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        log("  WARN: colours: cannot replace " + op.dst + " (error " + std::to_string(GetLastError()) + ")\n");
        return false;
    }
    if (!exists(op.dst)) {
        if (MoveFileExW(wp(op.src).c_str(), wp(op.dst).c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        if (!exists(op.dst)) {
            log("  WARN: colours: cannot move " + op.src + " (error " + std::to_string(GetLastError()) + "), left where it is\n");
            return false;
        }
    }
    if (op.fallback.empty()) {
        log("  WARN: colours: " + op.dst + " is taken, " + op.src + " left where it is\n");
        return false;
    }
    std::string fb = op.fallback;
    const size_t ext = fb.size() >= 7 ? fb.size() - 7 : fb.size();
    for (int k = 2; exists(fb); ++k) fb = op.fallback.substr(0, ext) + " (" + std::to_string(k) + ")" + op.fallback.substr(ext);
    make_dirs(parent(fb));
    if (MoveFileExW(wp(op.src).c_str(), wp(fb).c_str(), MOVEFILE_WRITE_THROUGH)) {
        log("  WARN: colours: " + op.dst + " is taken, the outfit's file set aside in " + fb + "\n");
        return true;
    }
    log("  WARN: colours: cannot move " + op.src + " (error " + std::to_string(GetLastError()) + "), left where it is\n");
    return false;
}

// Both phases. The journal says phase 2 once the first is done: phase 1 never runs again (an outfit's file already in its
// new place would be taken for the one that was there).
void execute(const Plan& plan, int phase, const std::string& journal, const Log& log) {
    // Tests only: SF6_COLOUR_FOLLOW_TEST_CUT=1 stops after the first phase, =2 halfway through the second, as a crash
    // would (the journal stays, the next launch finishes the pass)
    char cut[8] = {};
    const DWORD cn = GetEnvironmentVariableA("SF6_COLOUR_FOLLOW_TEST_CUT", cut, sizeof cut);
    const int stop = cn > 0 && cn < sizeof cut ? atoi(cut) : 0;
    if (phase == 1) {
        for (auto& op : plan.first) do_op(op, log);
        if (!write_atomic(journal, journal_text(plan, 2))) {
            log("  WARN: colours: cannot write " + journal + ": a pass cut short now is not finished at the next launch\n");
            DeleteFileW(wp(journal).c_str());
        }
        if (stop == 1) { log("  test: colour pass cut after its first phase\n"); return; }
    }
    for (size_t i = 0; i < plan.second.size(); ++i) {
        if (stop == 2 && i == plan.second.size() / 2) { log("  test: colour pass cut in its second phase\n"); return; }
        do_op(plan.second[i], log);
    }
    DeleteFileW(wp(journal).c_str());
}

}  // namespace

void run(const RunInput& in) {
    const Log log = [&](const std::string& s) { if (in.log) in.log(s); };
    const std::string game = full_path(in.game_dir), reg = full_path(in.registry_dir);
    const Place place{ game + "\\reframework\\color_mods", game + "\\reframework\\data\\SF6_ColorSpinExtra_data" };
    const std::string journal = reg + "\\colour_follow.journal", owners_path = reg + "\\colour_owners.txt";

    // 1. a pass cut short at an earlier launch is finished first: then colour_owners.txt says where the colours are
    if (exists(journal)) {
        Plan old;
        int phase = 0;
        if (parse_journal(read_text(journal), old, phase)) {
            log("  colours: the pass of an earlier launch was cut short, finished now\n");
            execute(old, phase, journal, log);
        } else {
            log("  WARN: colours: " + journal + " unreadable, kept as colour_follow.journal.bad\n");
            MoveFileExW(wp(journal).c_str(), wp(journal + ".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    // 2. who the colours on disk belong to
    Owners own;
    bool found = false;
    const std::string owners_old = read_text(owners_path, &found);
    if (found && !parse_owners(owners_old, own)) log("  WARN: colours: " + owners_path + " unreadable, registry.json taken instead\n");
    std::string source;
    const std::vector<Outfit> former = choose_former(own, in.registry, outfit_labels(read_text(reg + "\\outfits.json")), source);
    std::set<std::string> regs{ registry_digest(in.now), registry_digest(in.registry) };
    const std::string owners_new = owners_text(in.now, regs);
    if (source.empty()) {   // first install: the colours at each number are its outfit's
        if (owners_old != owners_new && !write_atomic(owners_path, owners_new))
            log("  WARN: colours: cannot write " + owners_path + "\n");
        log("  colours: owners of " + std::to_string(in.now.size()) + " outfit(s) recorded (colour_owners.txt)\n");
        return;
    }
    if (source != "colour_owners.txt") log("  colours: who they belong to taken from " + source + "\n");
    // Nothing has moved yet: the files still follow `former`, and that goes with the registry this launch saves too. Written
    // first, so a pass that stops before its journal (an error, a crash) leaves a colour_owners.txt the next launch trusts.
    std::set<std::string> accepted = own.registries;
    accepted.insert(regs.begin(), regs.end());
    const std::string owners_now = owners_text(former, accepted);
    if (owners_old != owners_now && !write_atomic(owners_path, owners_now))
        log("  WARN: colours: cannot write " + owners_path + "\n");

    // 3. the steps, the plan
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d_%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    const Listing listing = list_files(place);
    const std::vector<Step> steps = decide(former, in.now, aside_keys(listing), stamp);
    Plan plan = make_plan(steps, listing, place, [](const std::string& p) { return exists(p); });
    if (plan.first.empty() && plan.second.empty()) {   // nothing moves: only who owns what
        if (owners_now != owners_new && !write_atomic(owners_path, owners_new))
            log("  WARN: colours: cannot write " + owners_path + "\n");
        return;
    }
    plan.writes.push_back({ owners_path, owners_new });
    plan.second.push_back({ owners_path + ".colfollow_new", owners_path, "", true });

    // 4. every new file written beside its place, the journal, then the moves
    for (auto& [path, text] : plan.writes) {
        if (write_text(path + ".colfollow_new", text)) continue;
        log("  WARN: colours: cannot write " + path + ".colfollow_new (disk full?): the colours stay where they are this launch\n");
        for (auto& [p2, t2] : plan.writes) DeleteFileW(wp(p2 + ".colfollow_new").c_str());
        return;   // colour_owners.txt (written above) still says where they are
    }
    if (!write_atomic(journal, journal_text(plan, 1))) {
        log("  WARN: colours: cannot write " + journal + ": the colours stay where they are this launch\n");
        for (auto& [p2, t2] : plan.writes) DeleteFileW(wp(p2 + ".colfollow_new").c_str());
        return;
    }
    for (auto& l : plan.lines) log(l);
    execute(plan, 1, journal, log);
}

}  // namespace colour_follow
