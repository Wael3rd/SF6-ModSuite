// vfx::drive_cfg — implementation. Lua semantics kept where they show: truthiness (nil / false), `a or b`, ipairs
// stopping at the first hole, Lua's floor modulo, tostring of numbers in a concatenation.

#include "drive_cfg.hpp"

#include <cmath>
#include <cstdio>
#include <map>

#include "settings.hpp"

namespace vfx::drive_cfg {
namespace {

std::recursive_mutex g_mx;
Env g_env;
std::map<int, json> g_di, g_dr;                    // [fighter] = { p1, p2 } (loaded files only)
json g_presets;                                    // null = not loaded
json g_empty_presets = json::object();

const MechDesc kDesc[kMechs] = {
    { Mech::Impact, "impact", Kind::Di, "vfx.impact" },
    { Mech::Reversal, "reversal", Kind::Di, "vfx.reversal" },
    { Mech::Parry, "parry", Kind::Hue, "vfx.parry" },
    { Mech::Rush, "rush", Kind::Hue, "vfx.rush" },
    { Mech::Special, "special", Kind::Hue, "vfx.special" },
};

bool truthy(const json& v) { return !(v.is_null() || (v.is_boolean() && !v.get<bool>())); }
const json& member(const json& t, const char* k) {
    static const json nil;
    if (!t.is_object()) return nil;
    auto it = t.find(k);
    return it == t.end() ? nil : *it;
}
void set(json& t, const char* k, json v) {
    if (v.is_null()) t.erase(k);
    else t[k] = std::move(v);
}
std::string L(const char* key) { return g_env.L ? g_env.L(key) : std::string(key); }

json* file_of(std::map<int, json>& cache, const char* kind, int fighter) {
    auto it = cache.find(fighter);
    if (it != cache.end()) return &it->second;
    json t = settings::load_table(g_env.root, kind, fighter);
    if (!t.is_object()) return nullptr;                                 // get_cfg returned nil: tried again next time
    return &cache.emplace(fighter, std::move(t)).first->second;
}
json* side_of(json* file, int side) {
    if (!file) return nullptr;
    auto it = file->find(side == 2 ? "p2" : "p1");
    return (it == file->end() || it->is_null()) ? nullptr : &*it;
}

int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }
int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

// a json number as the integer Lua's arithmetic + %02X accept (an integral value); nullopt otherwise
std::optional<int64_t> as_int(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_unsigned()) return (int64_t)v.get<uint64_t>();
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (std::floor(d) == d && std::fabs(d) < 9.2e18) return (int64_t)d;
        return std::nullopt;
    }
    if (v.is_string()) {                                                // arithmetic on a numeric string coerces it
        const std::string& s = v.get_ref<const std::string&>();
        char* e = nullptr;
        const double d = std::strtod(s.c_str(), &e);
        if (e != s.c_str() && !*e && std::floor(d) == d) return (int64_t)d;
    }
    return std::nullopt;
}

// tostring of a value in a concatenation: a string as is, a number as Lua prints it; nullopt = Lua's error
std::optional<std::string> concat_text(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer() || v.is_number_unsigned()) return v.dump();
    if (v.is_number_float()) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.14g", v.get<double>());
        std::string s = b;
        if (s.find_first_of(".eEni") == std::string::npos) s += ".0";   // lua_Number2str: "2.0", not "2"
        return s;
    }
    return std::nullopt;
}

}  // namespace

const MechDesc& desc(Mech m) { return kDesc[(int)m]; }
void set_env(Env env) { std::lock_guard<std::recursive_mutex> lk(g_mx); g_env = std::move(env); }
std::recursive_mutex& lock() { return g_mx; }
std::optional<std::pair<uint32_t, uint32_t>> squares(int fighter, int side) {
    std::function<std::optional<std::pair<uint32_t, uint32_t>>(int, int)> f;
    { std::lock_guard<std::recursive_mutex> lk(g_mx); f = g_env.squares; }
    return f ? f(fighter, side) : std::nullopt;
}

// ---- the tables ----------------------------------------------------------------------------------------------------

json* di_file(int fighter) { std::lock_guard<std::recursive_mutex> lk(g_mx); return file_of(g_di, "di", fighter); }
json* di_side(int fighter, int side) { std::lock_guard<std::recursive_mutex> lk(g_mx); return side_of(file_of(g_di, "di", fighter), side); }
json* dr_side(int fighter, int side) { std::lock_guard<std::recursive_mutex> lk(g_mx); return side_of(file_of(g_dr, "dr", fighter), side); }

json& presets() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_presets.is_null()) {
        json t = settings::load_table(g_env.root, "presets", 0);       // seeds the lists that are missing
        if (t.is_object()) g_presets = std::move(t);
    }
    return g_presets.is_object() ? g_presets : g_empty_presets;        // pcfg(): presets_cfg or {}
}

json* cfg_of(Mech m, int fighter, int side) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (desc(m).kind == Kind::Di) {
        json* base = di_side(fighter, side);
        if (!base || !base->is_object()) return nullptr;
        if (m == Mech::Impact) return base;
        auto it = base->find("rev");                                    // mech_cfg(base, "reversal")
        if (it == base->end() || !it->is_object()) {
            json rev = json::object();
            for (const char* k : { "enabled", "follow", "c1", "c2", "selected" }) set(rev, k, member(*base, k));
            (*base)["rev"] = std::move(rev);
            it = base->find("rev");
        }
        return &*it;
    }
    json* c = dr_side(fighter, side);
    if (!c || !c->is_object()) return nullptr;
    auto it = c->find(desc(m).name);
    return (it == c->end() || !it->is_object()) ? nullptr : &*it;
}

void save_di(int fighter) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_di.find(fighter);
    if (it != g_di.end()) settings::save_table(g_env.root, "di", fighter, it->second);
}
void save_dr(int fighter) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_dr.find(fighter);
    if (it != g_dr.end()) settings::save_table(g_env.root, "dr", fighter, it->second);
}
void save_presets() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (g_presets.is_object()) settings::save_table(g_env.root, "presets", 0, g_presets);
}
void forget() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    g_di.clear(); g_dr.clear(); g_presets = json();
}

// ---- the value model -----------------------------------------------------------------------------------------------

std::vector<Item> items(Mech m) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const json& pc = presets();
    const bool sp = (m == Mech::Special), dr = (m != Mech::Impact && m != Mech::Reversal);
    // (sp and presets_sp()) or (dr and presets_dr()) or presets()
    const json* src = nullptr;
    if (sp && truthy(member(pc, "presets_sp"))) src = &member(pc, "presets_sp");
    else if (dr && truthy(member(pc, "presets_dr"))) src = &member(pc, "presets_dr");
    else src = &member(pc, "presets");
    std::vector<Item> out;
    if (!src->is_array()) return out;                                   // ipairs: a sequence only
    for (size_t i = 0; i < src->size(); ++i) {
        const json& p = (*src)[i];
        if (p.is_null()) break;                                         // ipairs stops at the first hole
        if (!p.is_object() && !p.is_string()) break;                    // p.name on a number: the Lua raised there
        Item it;
        it.name = member(p, "name"); it.c1 = member(p, "c1"); it.c2 = member(p, "c2");
        it.index = (int)i + 1;
        it.key = "p:" + std::to_string(it.index);
        out.push_back(std::move(it));
    }
    return out;
}

namespace {
int item_index(const std::vector<Item>& items, const json& cfg) {
    const json& sel = member(cfg, "sel");
    if (sel.is_string())
        for (size_t i = 0; i < items.size(); ++i) if (sel.get_ref<const std::string&>() == items[i].key) return (int)i;
    json want = member(cfg, "selected");
    if (!truthy(want)) want = 1;                                        // cfg.selected or 1
    if (want.is_number())
        for (size_t i = 0; i < items.size(); ++i) if ((double)items[i].index == want.get<double>()) return (int)i;
    return 0;
}
}  // namespace

int value_of(const json* cfg, const std::vector<Item>& items) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!cfg || !cfg->is_object() || !truthy(member(*cfg, "enabled"))) return 0;
    if (truthy(member(*cfg, "follow"))) return 1;
    return 2 + item_index(items, *cfg);
}

void apply_value(json& cfg, const std::vector<Item>& items, int v) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!cfg.is_object()) return;
    if (v <= 0) { cfg["enabled"] = false; cfg["follow"] = false; return; }
    if (v == 1) { cfg["enabled"] = true; cfg["follow"] = true; cfg["ll"] = false; return; }
    if ((size_t)(v - 2) >= items.size()) return;
    const Item& it = items[(size_t)(v - 2)];
    cfg["enabled"] = true; cfg["follow"] = false; cfg["sel"] = it.key;
    cfg["ll"] = false;
    set(cfg, "c1", it.c1);
    set(cfg, "c2", truthy(it.c2) ? it.c2 : it.c1);
    cfg["selected"] = it.index;
}

int stepped(const json* cfg, const std::vector<Item>& items, int dir) {
    const int64_t n = 2 + (int64_t)items.size();
    return (int)floor_mod(value_of(cfg, items) + dir, n);
}

// ---- texts ---------------------------------------------------------------------------------------------------------

std::string camel(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    auto word = [&](char c) { return alpha(c) || (c >= '0' && c <= '9') || c == '\''; };
    size_t i = 0;
    while (i < r.size()) {
        if (alpha(r[i])) {
            if (r[i] >= 'a' && r[i] <= 'z') r[i] = (char)(r[i] - 'a' + 'A');
            ++i;
            while (i < r.size() && word(r[i])) ++i;
        } else {
            ++i;
        }
    }
    return r;
}

std::string title(Mech m) { return camel(L(desc(m).tkey)); }

std::string rgb_hex(int64_t col) {
    char b[16];
    std::snprintf(b, sizeof(b), "%02X%02X%02X", (unsigned)floor_mod(col, 0x100), (unsigned)floor_mod(floor_div(col, 0x100), 0x100),
                  (unsigned)floor_mod(floor_div(col, 0x10000), 0x100));
    return b;
}

namespace {
std::optional<std::string> swatch_of(const json& col) {
    const json c = truthy(col) ? col : json(0);                         // col or 0
    auto v = as_int(c);
    if (!v) return std::nullopt;
    return "<color " + rgb_hex(*v) + ">\xE2\x96\xA0</color>";            // ctx.SQUARE:sub(1, 3): one square
}
}  // namespace

std::string swatch(const json& col) { auto s = swatch_of(col); return s ? *s : std::string(); }

std::optional<std::string> label(const json* cfg, const std::vector<Item>& items, Kind kind, bool no_squares) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const int v = value_of(cfg, items);
    if (v == 0) return L("toggle.off");
    if (v == 1) return L("di.follow");
    if ((size_t)(v - 2) >= items.size()) return L("toggle.off");
    const Item& it = items[(size_t)(v - 2)];
    auto name = concat_text(it.name);
    if (no_squares) return name ? *name : std::string();
    if (!name) return std::nullopt;
    auto s1 = swatch_of(it.c1);
    auto s2 = swatch_of(kind == Kind::Hue ? (truthy(it.c2) ? it.c2 : it.c1) : it.c2);
    if (!s1 || !s2) return std::nullopt;
    return *name + "  " + *s1 + *s2;
}

std::optional<RowShow> row_show(Mech m, const json* cfg, const std::vector<Item>& items, int fighter, int side) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!cfg) return std::nullopt;
    RowShow r;
    r.title = title(m);
    const int v = value_of(cfg, items);
    json c1, c2;
    if (v == 0) {
        r.label = L("toggle.off");
    } else if (v == 1) {
        r.label = L("di.follow");
        auto w = g_env.squares ? g_env.squares(fighter, side) : std::nullopt;   // worn(): the colour worn's two squares
        if (w) { c1 = (int64_t)w->first; c2 = (int64_t)w->second; }
    } else {
        const Item* it = ((size_t)(v - 2) < items.size()) ? &items[(size_t)(v - 2)] : nullptr;
        // label = it and it.name or L("toggle.off"); a name that is not a string reaches the bridge as no text
        if (it && truthy(it->name)) r.label = it->name.is_string() ? it->name.get<std::string>() : std::string();
        else r.label = L("toggle.off");
        if (it) { c1 = it->c1; c2 = truthy(it->c2) ? it->c2 : it->c1; }
    }
    auto num = [](const json& v) -> int64_t {                           // arg_int of the bridge: a number, else -1
        if (v.is_number_integer()) return v.get<int64_t>();
        if (v.is_number_unsigned()) return (int64_t)v.get<uint64_t>();
        if (v.is_number_float()) return (int64_t)v.get<double>();
        return -1;
    };
    r.chip1 = truthy(c1) ? num(c1) : -1;                                // ch1 = c1 or -1
    r.chip2 = truthy(c2) ? num(c2) : r.chip1;                           // ch2 = c2 or c1 or -1
    // if ch1 == -1 then ch2 = -1: on the Lua value (a text colour reaches the bridge as -1 but is not -1 there)
    if (!truthy(c1) || (c1.is_number() && c1.get<double>() == -1.0)) r.chip2 = -1;
    const json& en = member(*cfg, "enabled");
    r.enabled = !(en.is_boolean() && !en.get<bool>());                  // cfg.enabled ~= false
    return r;
}

}  // namespace vfx::drive_cfg
