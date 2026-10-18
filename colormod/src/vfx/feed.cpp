// vfx::feed — the pure half: the texts and values SF6_DIRecolor / SF6_DriveRecolor computed (the frame loop that
// pushes them is vfx/feed_frame.cpp). Lua semantics kept: truthiness, `a or b`, string.format("%08X") on integers
// only (a numeric string is converted, a fraction raises), tonumber on a part's colour.

#include "feed.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include "drive_cfg.hpp"

namespace vfx::feed {
namespace {

using json = nlohmann::json;
namespace dc = drive_cfg;

bool truthy(const json& v) { return !(v.is_null() || (v.is_boolean() && !v.get<bool>())); }
const json& member(const json& t, const char* k) {
    static const json nil;
    if (!t.is_object()) return nil;
    auto it = t.find(k);
    return it == t.end() ? nil : *it;
}

// Lua's tonumber: a number, or a string Lua reads as one
std::optional<double> tonumber(const json& v) {
    if (v.is_number()) return v.get<double>();
    if (!v.is_string()) return std::nullopt;
    std::string s = v.get<std::string>();
    const size_t a = s.find_first_not_of(" \f\n\r\t\v"), b = s.find_last_not_of(" \f\n\r\t\v");
    if (a == std::string::npos) return std::nullopt;
    s = s.substr(a, b - a + 1);
    const char* p = s.c_str();
    const char* q = (*p == '-' || *p == '+') ? p + 1 : p;
    char* e = nullptr;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
        const unsigned long long u = std::strtoull(q + 2, &e, 16);
        if (e == q + 2 || *e) return std::nullopt;
        return (*p == '-') ? -(double)(long long)u : (double)(long long)u;
    }
    if (!((*q >= '0' && *q <= '9') || *q == '.')) return std::nullopt;
    const double d = std::strtod(p, &e);
    if (e == p || *e) return std::nullopt;
    return d;
}

// string.format("%08X", v): nullopt where Lua raises (not a number, or a number without an integer value)
std::optional<std::string> hex8(const json& v) {
    std::optional<int64_t> n;
    if (v.is_number_integer()) n = v.get<int64_t>();
    else if (v.is_number_unsigned()) n = (int64_t)v.get<uint64_t>();
    else if (v.is_number_float() || v.is_string()) {
        auto d = tonumber(v);
        if (d && std::floor(*d) == *d && std::fabs(*d) < 9.2e18) n = (int64_t)*d;
    }
    if (!n) return std::nullopt;
    char b[24];
    std::snprintf(b, sizeof b, "%08llX", (unsigned long long)*n);
    return std::string(b);
}

bool has_space_or_eq(const std::string& k) {
    for (char c : k) if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == '=') return true;
    return false;
}

// side_colors(id, side, cfg): the colours a side wants (nil = the game's palette): Follow = the worn colour's squares
struct Two { json c1, c2; };
Two side_colors(int id, int side, const json& cfg) {
    if (!cfg.is_object() || !truthy(member(cfg, "enabled"))) return {};
    if (truthy(member(cfg, "follow"))) {
        auto q = dc::squares(id, side);
        if (q) return { json((int64_t)q->first), json((int64_t)q->second) };
        return {};
    }
    return { member(cfg, "c1"), member(cfg, "c2") };
}

}  // namespace

std::optional<std::string> di_state_text(const int fid[2], bool swap) {
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    std::string out;
    for (int side = 1; side <= 2; ++side) {
        if (!out.empty()) out += "\n";
        const int f = fid[side - 1];
        if (f <= 0) { out += "S " + std::to_string(side) + " 0 0 0"; continue; }
        const int ss = settings_side(side, swap);                       // the settings page; "S"/"C" stay the fight side
        json* cfg = dc::di_side(f, ss);
        if (!cfg || !cfg->is_object()) return std::nullopt;             // mech_cfg on it: the Lua raised
        json* rev = dc::cfg_of(dc::Mech::Reversal, f, ss);
        if (!rev) return std::nullopt;
        const bool en = truthy(member(*cfg, "enabled")) || truthy(member(*rev, "enabled"));
        const bool fol = (truthy(member(*cfg, "enabled")) && truthy(member(*cfg, "follow"))) ||
                         (truthy(member(*rev, "enabled")) && truthy(member(*rev, "follow")));
        out += "S " + std::to_string(side) + " " + std::to_string(f) + " " + (en ? "1" : "0") + " " + (fol ? "1" : "0");
        for (int m = 0; m < 2; ++m) {
            const json& mc = m == 0 ? *cfg : *rev;
            const Two c = side_colors(f, side, mc);
            std::string h1 = "-", h2 = "-";
            if (truthy(c.c1)) { auto h = hex8(c.c1); if (!h) return std::nullopt; h1 = *h; }
            if (truthy(c.c2)) { auto h = hex8(c.c2); if (!h) return std::nullopt; h2 = *h; }
            std::string line = "C " + std::to_string(side) + (m == 0 ? " impact " : " reversal ") + h1 + " " + h2 + " " +
                               (truthy(member(mc, "follow")) ? "1" : "0");
            const json& parts = member(mc, "parts");
            std::map<std::string, std::string> kv;                      // key order (the reader keeps a map)
            // c = type(v) == "table" and tonumber(v.c) or nil ; if c and not tostring(k):find("[%s=]") then %08X
            auto one = [&](const std::string& k, const json& v) -> bool {
                if (!v.is_object() && !v.is_array()) return true;
                auto c = tonumber(member(v, "c"));                      // tonumber(nil) is nil in Lua 5.4
                if (!c || has_space_or_eq(k)) return true;
                auto h = hex8(json(*c));
                if (!h) return false;                                   // %08X of a fraction: the Lua raised
                kv[k] = *h;
                return true;
            };
            if (parts.is_object()) {
                for (auto it = parts.begin(); it != parts.end(); ++it) if (!one(it.key(), it.value())) return std::nullopt;
            } else if (parts.is_array()) {
                for (size_t i = 0; i < parts.size(); ++i) if (!parts[i].is_null() && !one(std::to_string(i + 1), parts[i])) return std::nullopt;
            }
            for (auto& [k, h] : kv) line += " " + k + "=" + h;
            out += "\n" + line;
        }
    }
    return out;
}

std::optional<std::array<int64_t, 12>> dr_colors(const int fid[2], bool swap) {
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    std::array<int64_t, 12> v{};
    auto arg_int = [](const json& x) -> int64_t {                       // the bridge's arg_int(..., -1)
        if (x.is_number_integer()) return x.get<int64_t>();
        if (x.is_number_unsigned()) return (int64_t)x.get<uint64_t>();
        if (x.is_number_float()) return (int64_t)x.get<double>();
        return -1;
    };
    static const char* kMech[3] = { "parry", "rush", "special" };
    size_t i = 0;
    for (int side = 1; side <= 2; ++side) {
        for (int m = 0; m < 3; ++m) {
            json c1, c2;
            const int id = fid[side - 1];
            if (id > 0) {
                json* t = dc::dr_side(id, settings_side(side, swap));
                if (t && (t->is_number() || (t->is_boolean() && t->get<bool>()))) return std::nullopt;   // cfg[mech] on it: raised
                static const json nil;
                const json& cfg = (t && t->is_object()) ? member(*t, kMech[m]) : nil;
                if (cfg.is_number() || (cfg.is_boolean() && cfg.get<bool>())) return std::nullopt;   // cfg.enabled on it: raised
                if (cfg.is_object() && truthy(member(cfg, "enabled"))) {
                    if (truthy(member(cfg, "follow"))) {
                        auto q = dc::squares(id, side);
                        if (q) { c1 = (int64_t)q->first; c2 = (int64_t)q->second; }
                    } else {
                        const json red = (int64_t)0xFF0000FF;
                        c1 = truthy(member(cfg, "c1")) ? member(cfg, "c1") : red;
                        c2 = truthy(member(cfg, "c2")) ? member(cfg, "c2") : c1;
                    }
                }
            }
            v[i++] = truthy(c1) ? arg_int(c1) : -1;                     // c1 or -1
            v[i++] = truthy(c2) ? arg_int(c2) : -1;
        }
    }
    return v;
}

bool parts_merge(int fighter, const std::string& reply) {
    if (fighter <= 0 || reply.compare(0, 3, "ok\n") != 0) return false;
    // gmatch("([^\t\n]+)\t([^\n]*)\n") on what follows "ok\n"
    json out = json::array();
    const std::string s = reply.substr(3);
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        while (j < s.size() && s[j] != '\t' && s[j] != '\n') ++j;
        if (j > i && j < s.size() && s[j] == '\t') {
            size_t k = j + 1;
            while (k < s.size() && s[k] != '\n') ++k;
            if (k < s.size()) {
                out.push_back(json{ { "key", s.substr(i, j - i) }, { "name", s.substr(j + 1, k - j - 1) } });
                i = k + 1;
                continue;
            }
        }
        ++i;
    }
    std::lock_guard<std::recursive_mutex> lk(dc::lock());
    json* file = dc::di_file(fighter);
    if (!file) return false;
    const json& old = member(*file, "parts_list");
    size_t old_n = 0;                                                   // #old
    if (old.is_array()) while (old_n < old.size() && !old[old_n].is_null()) ++old_n;
    bool same = old_n == out.size();
    for (size_t n = 0; same && n < out.size(); ++n) {
        const json& o = old[n];
        if (!o.is_object() || !member(o, "key").is_string() || member(o, "key").get<std::string>() != out[n]["key"].get<std::string>()) same = false;
    }
    if (same) return false;
    (*file)["parts_list"] = out.empty() ? json::array() : out;
    dc::save_di(fighter);
    return true;
}

}  // namespace vfx::feed
