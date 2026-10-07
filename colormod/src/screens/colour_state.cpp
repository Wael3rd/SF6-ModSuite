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

}  // namespace screens::colour
