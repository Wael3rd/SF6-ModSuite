// vfx::settings — implementation. Lua tables as json and REFramework's json module at both ends: engine/luajson.

#include "settings.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "data.hpp"
#include "../engine/luajson.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace vfx::settings {
namespace {

using namespace engine::luajson;

// for k, v in pairs(c) do t[k] = v end
json copy_cfg(const json& c) {
    if (!is_table(c)) throw LuaError("bad argument #1 to 'for iterator' (table expected)");
    return c;
}

// tonumber(v): a number, or a string Lua reads as one; nil otherwise
bool tonumber(const json& v, double& out) {
    if (v.is_number()) { out = v.get<double>(); return true; }
    if (!v.is_string()) return false;
    std::string s = v.get<std::string>();
    size_t a = s.find_first_not_of(" \f\n\r\t\v"), b = s.find_last_not_of(" \f\n\r\t\v");
    if (a == std::string::npos) return false;
    s = s.substr(a, b - a + 1);
    const char* p = s.c_str();
    const char* q = (*p == '-' || *p == '+') ? p + 1 : p;
    if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
        char* e = nullptr;
        unsigned long long u = std::strtoull(q + 2, &e, 16);
        if (e == q + 2 || *e) return false;
        out = (*p == '-') ? -(double)(long long)u : (double)(long long)u;
        return true;
    }
    if (!(std::isdigit((unsigned char)*q) || *q == '.')) return false;     // strtod would take inf / nan
    char* e = nullptr;
    out = std::strtod(p, &e);
    return e != p && !*e;
}

// ---- SF6_DIRecolor ------------------------------------------------------------------------------------------------

const char* const kNames[] = {
    nullptr, "Ryu", "Luke", "Kimberly", "ChunLi", "Manon", "Zangief", "JP", "Dhalsim", "Cammy", "Ken", "DeeJay", "Lily",
    "AKI", "Rashid", "Blanka", "Juri", "Marisa", "Guile", "Ed", "EHonda", "Jamie", "Akuma", nullptr, nullptr, "Sagat",
    "Bison", "Terry", "Mai", "Elena", "CViper", "Alex", "Ingrid", "Yasmine",
};

constexpr int64_t kRed = 0xFF0000FF, kWhite = 0xFFFFFFFF;

json preset(const char* name, int64_t c1, int64_t c2) { return json{ { "name", name }, { "c1", c1 }, { "c2", c2 } }; }

fs::path di_path(const fs::path& root, int id) { return root / "SF6_DIRecolor_data" / (char_name(id) + ".json"); }
fs::path legacy_jamie(const fs::path& root) { return root / "SF6_JamieDIRecolor.json"; }

// get_cfg's load: { p1, p2 } (written back when the file was missing or flat)
json di_load(const fs::path& root, int id, std::string* err) {
    json c = load_file(di_path(root, id));
    if (truthy(c) && truthy(index(c, "p1"))) {                      // current layout
        json p1 = index(c, "p1"), p2 = index(c, "p2");
        json file = json::object();
        file["p1"] = p1;
        file["p2"] = truthy(p2) ? p2 : copy_cfg(p1);
        return file;
    }
    if (!truthy(c)) {                                               // nothing yet: the defaults (+ the old Jamie file)
        c = json{ { "enabled", false }, { "c1", kRed }, { "c2", kWhite }, { "selected", 1 } };
        if (id == 21) {
            json legacy = load_file(legacy_jamie(root));
            if (truthy(legacy)) {
                set(c, "enabled", truthy(index(legacy, "enabled")));
                json v = index(legacy, "c1");       if (truthy(v)) set(c, "c1", v);
                v = index(legacy, "c2");            if (truthy(v)) set(c, "c2", v);
                v = index(legacy, "selected");      set(c, "selected", truthy(v) ? v : json(1));
            }
        }
    }
    json file = json::object();                                     // flat (one setting): P1's, copied to P2
    file["p1"] = c;
    file["p2"] = copy_cfg(c);
    dump_file(di_path(root, id), file, err);
    return file;
}

// the presets block of the script's main chunk (seeds and writes what is missing)
json presets_load(const fs::path& root, std::string* err) {
    const fs::path path = root / "SF6_DIRecolor_data" / "_presets.json";
    json pc = load_file(path);
    bool write = false;
    if (!truthy(pc) || !is_table(index(pc, "presets")) || len(index(pc, "presets")) == 0) {
        json legacy = load_file(legacy_jamie(root));
        json list = json::array();
        json lp = truthy(legacy) ? index(legacy, "presets") : json();
        if (truthy(legacy) && is_table(lp) && len(lp) > 0) {
            for (size_t i = 0; i < len(lp); ++i) {
                const json& p = lp[i];
                json e = json::object();
                for (const char* k : { "name", "c1", "c2" }) { json v = index(p, k); if (!v.is_null()) e[k] = v; }
                list.push_back(e);
            }
        } else {
            list.push_back(preset("DI 1", kRed, kWhite));           // DEFAULT_PRESETS
        }
        pc = json{ { "presets", list } };
        write = true;
    }
    if (!is_table(index(pc, "presets_dr")) || len(index(pc, "presets_dr")) == 0) {
        set(pc, "presets_dr", json::array({ preset("DR 1", kRed, kRed) }));
        write = true;
    }
    if (!is_table(index(pc, "presets_sp")) || len(index(pc, "presets_sp")) == 0) {
        set(pc, "presets_sp", json::array({
            preset("SP Red", 0xFF0000FF, 0xFF0000FF), preset("SP Orange", 0xFF002FFF, 0xFF002FFF),
            preset("SP Yellow", 0xFF00BFFF, 0xFF00BFFF), preset("SP Lemon Green", 0xFF00FF80, 0xFF00FF80),
            preset("SP Green", 0xFF00FF00, 0xFF00FF00), preset("SP Cyan", 0xFFFFFF00, 0xFFFFFF00),
            preset("SP Purple", 0xFFFF0080, 0xFFFF0080), preset("SP Magenta", 0xFFFF00FF, 0xFFFF00FF),
            preset("SP Pink", 0xFF8000FF, 0xFF8000FF), preset("SP White", 0xFFFFFFFF, 0xFFFFFFFF) }));
        write = true;
    }
    if (write) dump_file(path, pc, err);                            // the Lua wrote after each step: same last bytes
    return pc;
}

// ---- SF6_DriveRecolor ---------------------------------------------------------------------------------------------

fs::path dr_path(const fs::path& root, int id) { return root / "SF6_DriveRecolor_data" / (char_name(id) + ".json"); }

// hue_col((tonumber(h) or 0))
int64_t hue_of(const json& h) {
    double d = 0;
    if (!tonumber(h, d)) d = 0;
    return (int64_t)data::hue_col(d);
}

json dr_mech() { return json{ { "enabled", false }, { "follow", false }, { "c1", hue_of(0) }, { "c2", hue_of(0) } }; }
json dr_default() { return json{ { "parry", dr_mech() }, { "rush", dr_mech() }, { "special", dr_mech() } }; }

// upgrade(m): an old mechanic (a hue, maybe a particle hue) as two full colours
json dr_upgrade(json m) {
    if (!is_table(m)) return dr_mech();
    if (!truthy(index(m, "c1"))) {
        json h = index(m, "hue");
        set(m, "c1", hue_of(truthy(h) ? h : json(0)));
    }
    if (!truthy(index(m, "c2"))) {
        json h = index(m, "tint_hue");
        if (!truthy(h)) h = index(m, "hue");
        set(m, "c2", hue_of(truthy(h) ? h : json(0)));
    }
    set(m, "hue", json()); set(m, "tint_hue", json()); set(m, "tint", json());
    return m;
}

json or_mech(const json& v) { return truthy(v) ? v : dr_mech(); }

// migrer(c)
json dr_migrer(const json& c) {
    if (!is_table(c)) return dr_default();
    if (truthy(index(c, "parry")) || truthy(index(c, "rush"))) {
        return json{ { "parry", dr_upgrade(or_mech(index(c, "parry"))) }, { "rush", dr_upgrade(or_mech(index(c, "rush"))) },
                     { "special", dr_upgrade(or_mech(index(c, "special"))) } };
    }
    json flat = json::object();                                     // one flat setting: the same for parry and rush
    json v = index(c, "enabled");   set(flat, "enabled", truthy(v) ? v : json(false));
    v = index(c, "follow");         set(flat, "follow", truthy(v) ? v : json(false));
    v = index(c, "hue");            set(flat, "hue", truthy(v) ? v : json(0));
    set(flat, "tint_hue", index(c, "tint_hue"));
    return json{ { "parry", dr_upgrade(flat) }, { "rush", dr_upgrade(flat) }, { "special", dr_mech() } };
}

// get_cfg's load (no write: the Lua wrote on save only)
json dr_load(const fs::path& root, int id) {
    json c = load_file(dr_path(root, id));
    if (truthy(c) && truthy(index(c, "p1"))) {
        json p1 = index(c, "p1"), p2 = index(c, "p2");
        json file = json::object();
        file["p1"] = dr_migrer(p1);
        file["p2"] = dr_migrer(truthy(p2) ? p2 : p1);
        return file;
    }
    return json{ { "p1", dr_default() }, { "p2", dr_default() } };
}

}  // namespace

std::string char_name(int id) {
    if (id >= 0 && id < (int)(sizeof(kNames) / sizeof(kNames[0])) && kNames[id]) return kNames[id];
    char b[16];
    std::snprintf(b, sizeof(b), "esf%03d", id);
    return b;
}

json load_table(const std::string& root, const std::string& kind, int id, std::string* err) {
    try {
        if (kind == "di") return di_load(root, id, err);
        if (kind == "presets") return presets_load(root, err);
        if (kind == "dr") return dr_load(root, id);
        if (err) *err = "unknown kind " + kind;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
    }
    return json();
}

std::string load(const std::string& root, const std::string& kind, int id, std::string* err) {
    json t = load_table(root, kind, id, err);
    if (t.is_null()) return {};
    try { return t.dump(); }                                        // the table itself: json.load_string gives it back
    catch (const std::exception& e) { if (err) *err = e.what(); return {}; }
}

namespace {
fs::path file_of(const std::string& root, const std::string& kind, int id) {
    if (kind == "di") return di_path(root, id);
    if (kind == "presets") return fs::path(root) / "SF6_DIRecolor_data" / "_presets.json";
    if (kind == "dr") return dr_path(root, id);
    return {};
}
}  // namespace

bool save_table(const std::string& root, const std::string& kind, int id, const json& table, std::string* err) {
    const fs::path p = file_of(root, kind, id);
    if (p.empty()) { if (err) *err = "unknown kind " + kind; return false; }
    return dump_file(p, table, err);
}

bool save(const std::string& root, const std::string& kind, int id, const std::string& text, std::string* err) {
    const fs::path p = file_of(root, kind, id);
    if (p.empty()) { if (err) *err = "unknown kind " + kind; return false; }
    json j;
    try { j = json::parse(text); }                                  // json.dump_string's text: encode_any already done
    catch (const json::exception& e) { if (err) *err = e.what(); return false; }
    std::string out;
    try { out = j.dump(4); }
    catch (const json::exception& e) { if (err) *err = e.what(); return false; }
    return write_text(p, out, err);
}

}  // namespace vfx::settings
