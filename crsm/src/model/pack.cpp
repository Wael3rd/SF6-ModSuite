#include "pack.hpp"

#include <cstdio>

#include "luajson.hpp"
#include "lutil.hpp"
#include "timeline.hpp"

namespace model::pack {
namespace {

namespace lj = model::luajson;

// ipairs over a decoded json table: the elements up to the first hole
std::vector<const json*> ipairs(const json& t) {
    std::vector<const json*> v;
    if (!t.is_array()) return v;
    for (const json& e : t) { if (e.is_null()) break; v.push_back(&e); }
    return v;
}

}  // namespace

bool validate(const json& data, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!lj::is_table(data) || lj::len(data) == 0) return fail("empty or invalid slot data");
    for (const json* s : ipairs(data)) {
        const json id = lj::is_table(*s) ? lj::index(*s, "id") : json();
        if (!lj::is_table(*s) || !id.is_number() || id.get<double>() < 1 || id.get<double>() > 30) return fail("invalid slot entry (bad id)");
        const bool empty = lutil::truthy(lj::index(*s, "empty"));
        if (!empty && !lj::is_table(lj::index(*s, "timeline")) && !lj::is_table(lj::index(*s, "inputs")))
            return fail("slot " + lutil::number_to_string(id) + " has no timeline/inputs");
    }
    return true;
}

Split split(const json& data) {
    Split r;
    for (const json* e : ipairs(data)) {
        const json id = lj::is_table(*e) ? lj::index(*e, "id") : json();
        if (id.is_number() && id.get<double>() <= kLiveSlots) r.live.push_back(*e);
        else r.virt.push_back(*e);
    }
    return r;
}

json live_entry(int id, int64_t weight, const std::string& name, bool is_valid, int64_t frames, const uint16_t* values) {
    json e = json::object();
    e["id"] = id;
    e["weight"] = weight;
    if (!name.empty()) e["name"] = name;
    if (is_valid && frames > 0 && values) {
        e["timeline"] = timeline::compress(values, (size_t)frames);
        e["empty"] = false;
    } else {
        e["empty"] = true;
        e["timeline"] = json::array();                  // an empty Lua table: written as null by json.dump_file
    }
    return e;
}

json assemble(const json& live_entries, const json& virtual_entries) {
    json out = json::array();
    for (const json& e : live_entries) out.push_back(e);
    for (const json& e : virtual_entries) out.push_back(e);
    return out;
}

std::string save_filename(const std::string& typed, const std::string& char_name) {
    std::string f = typed;
    if (!lutil::ends_with(f, ".json")) f += ".json";
    if (lutil::normalize_name(f).find(lutil::normalize_name(char_name)) == std::string::npos) f = char_name + "_" + f;
    return f;
}

TypedName typed_name(const std::string& typed) {
    TypedName r;
    r.display = lutil::trim(typed);
    std::string n;
    for (char c : r.display) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
        n += c;
    }
    n = lutil::trim(n);
    if (n.empty()) return r;
    if (!lutil::ends_with(n, ".json")) n += ".json";
    r.file = n;
    return r;
}

std::string backup_filename(const std::string& char_name, const std::tm& when) {
    char b[32];
    std::snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", when.tm_year + 1900, when.tm_mon + 1, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec);
    return char_name + "_pre_import_" + b + ".json";
}

bool BackupGate::allow(int fighter_id, double now_seconds) {
    for (Slot& s : slots_) {
        if (s.id != fighter_id) continue;
        if (now_seconds - s.at < 5) return false;
        s.at = now_seconds;
        return true;
    }
    slots_.push_back({ fighter_id, now_seconds });
    return true;
}

}  // namespace model::pack
