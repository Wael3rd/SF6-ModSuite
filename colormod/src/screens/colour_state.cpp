// screens::colour — state.json and chip_slots.json (see colour_state.hpp).

#include "colour_state.hpp"

#include "../engine/luajson.hpp"

namespace screens::colour {
namespace {
using namespace engine::luajson;

json fresh() { return json{ { "rw", 0 }, { "fighter", 0 }, { "costume", 0 } }; }

// a table the main chunk can index and assign into; a number, a boolean or a string made it raise
bool tableish(json& t) {
    if (!truthy(t)) { t = json::object(); return true; }                // json.load_file(...) or {}
    return is_table(t);
}
}  // namespace

std::optional<json> load_state(const std::filesystem::path& file) {
    json st = load_file(file);
    if (!tableish(st)) return std::nullopt;
    try {
        if (!index(st, "rw").is_null() || !index(st, "rw1").is_null()) {    // state.rw ~= nil or state.rw1 ~= nil
            json p1 = json::object();
            const json rw = index(st, "rw");
            set(p1, "rw", truthy(rw) ? rw : json(truthy(index(st, "rw1")) ? 1 : 0));
            set(p1, "path", index(st, "path"));
            const json f = index(st, "fighter"), c = index(st, "costume");
            set(p1, "fighter", truthy(f) ? f : json(0));
            set(p1, "costume", truthy(c) ? c : json(0));
            st = json{ { "p1", p1 } };
        }
        for (const char* k : { "p1", "p2", "fs" })
            if (!truthy(index(st, k))) set(st, k, fresh());
    } catch (const LuaError&) {
        return std::nullopt;
    }
    return st;
}

bool save_state(const std::filesystem::path& file, const json& state, std::string* err) { return dump_file(file, state, err); }

std::optional<json> load_chip_cfg(const std::filesystem::path& file) {
    json cfg = load_file(file);
    if (!tableish(cfg)) return std::nullopt;
    set(cfg, "seen", json());
    return cfg;
}

bool learn_chip_slots(json& chip_cfg, const std::string& key, const std::string& s0, const std::string& s1,
                      const std::filesystem::path& file, std::string* err) {
    set(chip_cfg, key.c_str(), json::array({ s0, s1 }));
    return dump_file(file, chip_cfg, err);
}

namespace {
bool num_of(const json& v, int64_t& out) { if (!v.is_number()) return false; out = (int64_t)v.get<double>(); return true; }
}

bool resolve_selection(json& s, const engine::Catalog& cat, std::string* note) {
    if (!is_table(s)) return false;
    int64_t f = 0, c = 0, rw = 0;
    if (!num_of(index(s, "fighter"), f) || !num_of(index(s, "costume"), c)) return false;
    num_of(index(s, "rw"), rw);
    bool changed = false;
    auto say = [&](const std::string& t) { if (note) *note += (note->empty() ? "" : "; ") + t; };
    // 1. a parked identity: the colour may be back now
    if (const json pend = index(s, "path_pending"); truthy(pend) && is_table(pend)) {
        int64_t pf = 0, pc = 0;
        const json pp = index(pend, "path");
        num_of(index(pend, "fighter"), pf); num_of(index(pend, "costume"), pc);
        if (rw > 0 || pf != f || pc != c || !pp.is_string()) { set(s, "path_pending", json()); changed = true; }   // the player chose elsewhere
        else if (int k = cat.find_ident((int)pf, (int)pc, pp.get<std::string>()); k > 0) {
            set(s, "rw", (int64_t)k); set(s, "path", pp); set(s, "path_pending", json());
            rw = k; changed = true;
            say("colour " + pp.get<std::string>() + " is back at rank " + std::to_string(k));
        }
    }
    // 2. the stored identity
    const json p = index(s, "path");
    if (truthy(p) && p.is_string() && !p.get<std::string>().empty()) {
        const int64_t k = cat.find_ident((int)f, (int)c, p.get<std::string>());
        if (k != rw) {
            set(s, "rw", k);
            changed = true;
            if (k == 0) {
                set(s, "path_pending", json{ { "path", p }, { "fighter", f }, { "costume", c } });
                set(s, "path", json());
                say("colour " + p.get<std::string>() + " not found now (kept for later)");
            } else say("colour " + p.get<std::string>() + " moved from rank " + std::to_string(rw) + " to " + std::to_string(k));
        }
    } else if (rw > 0) {
        if (auto* l = cat.entries((int)f, (int)c); l && (size_t)rw <= l->size()) { set(s, "path", (*l)[(size_t)rw - 1].ident); changed = true; }   // migration
    }
    return changed;
}

}  // namespace screens::colour
