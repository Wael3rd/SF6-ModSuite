#include "drive.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "nlohmann/json.hpp"

#include "data.hpp"

namespace vfx::drive {
namespace {

struct Label { int cont, cat, el; };
// ETIQUETTES (SF6_DriveRecolor.lua): what the game asks requestEffect for each mechanic
const std::vector<Label> kParry = { { 530, 0, 0 }, { 530, 0, 1 } };
const std::vector<Label> kRush = { { 530, 0, 10 }, { 530, 0, 20 }, { 530, 0, 21 }, { 530, 0, 50 }, { 530, 0, 51 }, { 530, 0, 61 } };
// element 100 follows both mechanics (Impact: 2 then 100, Reversal: 1 then 100, seen 18/10): no block of its own, it
// keeps what element 2 or 1 just painted (as an Impact block it repainted every Drive Reversal with the Impact colours)
const std::vector<Label> kImpact = { { 201, 2, 2 } };
const std::vector<Label> kReversal = { { 201, 2, 1 } };

std::string e_line(uint64_t mgr, int cont, int cat, int el) {
    char b[96];
    std::snprintf(b, sizeof b, "E %llX %d %d %d", (unsigned long long)mgr, cont, cat, el);
    return b;
}

}  // namespace

std::string w_line(uint64_t addr, uint32_t val, uint32_t orig) {
    char b[64];
    std::snprintf(b, sizeof b, "W %llX %08X %08X", (unsigned long long)addr, val, orig);
    return b;
}
std::string w_line(uint64_t addr, uint32_t val, uint32_t orig, uint64_t obj, uint64_t ti) {
    char b[128];
    std::snprintf(b, sizeof b, "W %llX %08X %08X %llX %llX", (unsigned long long)addr, val, orig, (unsigned long long)obj, (unsigned long long)ti);
    return b;
}

void remember(Spot& sp, uint32_t col) {
    for (uint32_t v : sp.written) if (v == col) return;
    sp.written.push_back(col);
    if (sp.written.size() > 8) sp.written.erase(sp.written.begin());
}

bool spot_ok(std::optional<uint32_t> cur, int r, int g, int b, const std::vector<uint32_t>& written, const std::vector<uint32_t>& cands) {
    if (!cur || (*cur >> 24) != 255) return false;
    for (uint32_t w : written) if (w == *cur) return true;
    const auto o = data::to_hsv(r, g, b);
    const auto c = data::to_hsv((int)(*cur & 0xFF), (int)((*cur >> 8) & 0xFF), (int)((*cur >> 16) & 0xFF));
    if (std::fabs(o.s - c.s) < 0.02 && std::fabs(o.v - c.v) < 0.02) return true;
    for (uint32_t cand : cands) if (data::tint_with(cand, r, g, b) == *cur) return true;
    return false;
}

std::string famille(const std::string& file) {
    static const std::regex kEsf("esf\\d\\d\\d");
    if (file.find("parry") != std::string::npos) return "parry";
    // the Drive Reversal ring behind the fighter (seen in game 18/10: it took the Drive Rush colour as a "drive" file)
    if (file.find("drive_000_10") != std::string::npos) return "reversal";
    // the Drive Impact's common effects: start (000_00) and hits (001_*); they took the Drive Rush colour too
    if (file.find("drive_000_00") != std::string::npos || file.find("drive_001_") != std::string::npos) return "impact";
    if (file.find("drive") != std::string::npos) return "rush";
    if (std::regex_search(file, kEsf)) return "special";
    return "";
}

std::string palette_sig() {
    size_t nc = 0;
    for (size_t i = 0; i < data::DRIVE_COLORS_COUNT; ++i) nc += data::DRIVE_COLORS[i].count;
    return std::to_string(data::DRIVE_COLORS_COUNT) + "/" + std::to_string(nc);
}

std::string bases_sig(const std::vector<Base>& bases) {
    uint32_t sum = 0;
    for (auto& b : bases) sum = (uint32_t)((sum + b.arr) & 0xFFFFFFFF);
    char s[48];
    std::snprintf(s, sizeof s, "%zu:%X", bases.size(), sum);
    return s;
}

std::string scan_request(uint64_t seq, const std::vector<Base>& bases) {
    std::string out = "R " + std::to_string(seq);
    std::set<std::string> files;
    for (auto& b : bases) {
        if (files.insert(b.file).second) {
            std::string line = "P " + b.file + " ";
            if (const auto* e = data::drive_colors_find(b.file)) {
                for (size_t i = 0; i < e->count; ++i) {
                    if (i) line += " ";
                    line += std::to_string(e->colors[i].r) + "," + std::to_string(e->colors[i].g) + "," + std::to_string(e->colors[i].b);
                }
            }
            out += "\n" + line;
        }
        char f[160];
        std::snprintf(f, sizeof f, "F %s %llX %u", b.file.c_str(), (unsigned long long)b.arr, b.count);
        out += "\n";
        out += f;
    }
    return out + "\nEND\n";
}

bool scan_result(const std::string& body, uint64_t seq, std::vector<Spot>& found, int& nodes, int& ms) {
    if (body.find("\nEND") == std::string::npos) return false;
    if (body.rfind("R ", 0) != 0) return false;
    if (std::strtoull(body.c_str() + 2, nullptr, 10) != seq) return false;
    nodes = ms = 0;
    found.clear();
    std::unordered_set<uint64_t> seen;
    std::istringstream f(body);
    std::string l;
    bool first = true, have_n = false;
    while (std::getline(f, l)) {
        if (first) { first = false; continue; }                       // the "R" line (Lua: matches after "\n" only)
        if (l.rfind("N ", 0) == 0) {
            unsigned long long spots = 0; int n = 0, m = 0;
            // the Lua's body:match takes the first N line
            if (!have_n && std::sscanf(l.c_str(), "N %llu %d %d", &spots, &n, &m) == 3) { nodes = n; ms = m; have_n = true; }
        } else if (l.rfind("S ", 0) == 0) {
            char a[32], file[512];
            int r, g, b;
            if (std::sscanf(l.c_str(), "S %31[0-9A-Fa-f] %d %d %d %511s", a, &r, &g, &b, file) != 5) continue;
            const uint64_t addr = std::strtoull(a, nullptr, 16);
            if (!seen.insert(addr).second) continue;
            found.push_back(Spot{ addr, r, g, b, file, {} });
        }
    }
    return true;
}

std::vector<Spot> load_spots_file(const std::string& path, size_t& total) {
    std::vector<Spot> out;
    total = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return out;
    total = j.size();
    for (auto& e : j) {
        Spot sp;
        const nlohmann::json* written = nullptr;
        auto num = [](const nlohmann::json& v, bool& ok) { if (!v.is_number()) { ok = false; return 0; } return (int)v.get<double>(); };
        bool ok = true;
        if (e.is_array() && e.size() >= 4 && e[0].is_string()) {
            sp.addr = std::strtoull(e[0].get<std::string>().c_str(), nullptr, 16);
            sp.r = num(e[1], ok); sp.g = num(e[2], ok); sp.b = num(e[3], ok);
            if (e.size() >= 5 && e[4].is_string()) sp.file = e[4].get<std::string>();
            if (e.size() >= 6 && e[5].is_array()) written = &e[5];
        } else if (e.is_object() && e.contains("1") && e["1"].is_string()) {     // a Lua table with an array part and "written"
            sp.addr = std::strtoull(e["1"].get<std::string>().c_str(), nullptr, 16);
            sp.r = e.contains("2") ? num(e["2"], ok) : (ok = false, 0);
            sp.g = e.contains("3") ? num(e["3"], ok) : (ok = false, 0);
            sp.b = e.contains("4") ? num(e["4"], ok) : (ok = false, 0);
            if (e.contains("5") && e["5"].is_string()) sp.file = e["5"].get<std::string>();
            if (e.contains("written") && e["written"].is_array()) written = &e["written"];
        } else continue;
        if (!ok) continue;
        if (written) for (auto& w : *written) if (w.is_number()) sp.written.push_back((uint32_t)w.get<double>());
        out.push_back(std::move(sp));
    }
    return out;
}

std::string spots_json(const std::vector<Spot>& spots) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& sp : spots) {
        char a[32];
        std::snprintf(a, sizeof a, "%llX", (unsigned long long)sp.addr);
        nlohmann::json e = nlohmann::json::array({ a, sp.r, sp.g, sp.b, sp.file });
        if (!sp.written.empty()) e.push_back(sp.written);              // the Lua reads e[1..5] and ignores a 6th element
        j.push_back(std::move(e));
    }
    return j.dump(4);
}

bool save_spots_file(const std::string& path, const std::vector<Spot>& spots) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& sp : spots) {
        char a[32];
        std::snprintf(a, sizeof a, "%llX", (unsigned long long)sp.addr);
        nlohmann::json e = nlohmann::json::array({ a, sp.r, sp.g, sp.b, sp.file });
        if (!sp.written.empty()) e.push_back(sp.written);              // the Lua reads e[1..5] and ignores a 6th element
        j.push_back(std::move(e));
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(4);
    return (bool)f;
}

std::string handoff(const HandoffIn& in, std::vector<Spot>& spots, const std::vector<size_t>& live, size_t& nlines) {
    std::vector<std::string> out;
    for (int side = 1; side <= 2; ++side) {
        const uint64_t mgr = in.mgr[side];
        if (!mgr) continue;
        // Drive Impact / Reversal: SF6_DIRecolor's lines, container of the side (declared 201 when unknown)
        for (int mi = 0; mi < 2; ++mi) {
            const DiBlock& d = in.di[side][mi];
            if (!d.present || d.lines.empty()) continue;
            for (const Label& lab : (mi == 0 ? kImpact : kReversal)) {
                out.push_back(e_line(mgr, d.container >= 0 ? d.container : lab.cont, lab.cat, lab.el));
                out.insert(out.end(), d.lines.begin(), d.lines.end());
            }
        }
        // Parry / Rush / Special: the spots tinted with the side's colour 1, the element tints with its colour 2
        if (live.empty()) continue;                                     // (no special-attack pattern spots: dormant)
        for (int mi = 0; mi < 3; ++mi) {
            const ColorPair& mc = in.col[side][mi];
            const bool want_special = (mi == 2);
            std::vector<std::string> lignes;
            for (size_t idx : live) {
                Spot& sp = spots[idx];
                const std::string fam = famille(sp.file);
                if (fam == "reversal" || fam == "impact") continue;     // painted by the light fight only
                if ((fam == "special") != want_special) continue;
                const uint32_t orig = data::pack(sp.r, sp.g, sp.b);
                const uint32_t col = mc.c1 ? data::tint_with(*mc.c1, sp.r, sp.g, sp.b) : orig;
                remember(sp, col);
                lignes.push_back(w_line(sp.addr, col, orig));
            }
            if (!want_special) {
                const uint32_t tcol = mc.c2 ? *mc.c2 : 0xFFFFFFFF;
                for (auto& t : in.tints) lignes.push_back(w_line(t.addr, tcol, t.cur, t.obj, t.ti));
            }
            if (lignes.empty()) continue;
            if (want_special) {
                char m[48];
                std::snprintf(m, sizeof m, "M %llX", (unsigned long long)mgr);
                out.push_back(m);
                out.insert(out.end(), lignes.begin(), lignes.end());
            } else {
                for (const Label& lab : (mi == 0 ? kParry : kRush)) {
                    out.push_back(e_line(mgr, lab.cont, lab.cat, lab.el));
                    out.insert(out.end(), lignes.begin(), lignes.end());
                }
            }
        }
    }
    nlines = out.size();
    if (out.empty()) return std::string();
    std::string text;
    for (auto& l : out) { text += l; text += "\n"; }
    return text;
}

}  // namespace vfx::drive
