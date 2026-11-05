#include "pack.hpp"

#include <algorithm>
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

bool same_file_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
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

namespace {

// info.<key> of the entry whose id is 1, a string ("" otherwise)
std::string info_string(const json& data, const char* key) {
    if (!data.is_array()) return "";
    for (const json& e : data) {
        if (!e.is_object()) continue;
        const auto id = e.find("id");
        if (id == e.end() || !id->is_number() || id->get<double>() != 1.0) continue;
        const auto info = e.find("info");
        if (info == e.end() || !info->is_object()) return "";
        const auto d = info->find(key);
        return (d != info->end() && d->is_string()) ? d->get<std::string>() : std::string();
    }
    return "";
}

}  // namespace

std::string description(const json& data) { return info_string(data, "description"); }
std::string author(const json& data) { return info_string(data, "author"); }
std::string source(const json& data) { return info_string(data, "source"); }

bool with_description(const json& data, const std::string& desc, json* out) {
    if (!data.is_array() || !out) return false;
    json copy = data;
    for (json& e : copy) {
        if (!e.is_object()) continue;
        const auto id = e.find("id");
        if (id == e.end() || !id->is_number() || id->get<double>() != 1.0) continue;
        json& info = e["info"];
        if (!info.is_object()) info = json::object();
        info["description"] = desc;
        *out = std::move(copy);
        return true;
    }
    return false;
}

std::vector<SlotSummary> slot_summaries(const json& data) {
    std::vector<SlotSummary> out;
    if (!data.is_array()) return out;
    for (const json& e : data) {
        if (!e.is_object()) continue;
        const auto id = e.find("id");
        if (id == e.end() || !id->is_number()) continue;
        const double v = id->get<double>();
        if (v < 1 || v > kSlotTotal || v != (double)(int)v) continue;
        const int n = (int)v;
        if (std::any_of(out.begin(), out.end(), [n](const SlotSummary& s) { return s.id == n; })) continue;
        SlotSummary s;
        s.id = n;
        if (const auto nm = e.find("name"); nm != e.end() && nm->is_string()) s.name = nm->get<std::string>();
        const auto tl = e.find("timeline");
        const auto in = e.find("inputs");
        if (tl != e.end() && tl->is_array()) {
            std::vector<std::string> lines;
            for (const json& l : *tl) if (l.is_string()) lines.push_back(l.get<std::string>());
            s.frames = timeline::entry_frames(lines);
        } else if (in != e.end() && in->is_array()) {
            s.frames = (int64_t)in->size();
        }
        const auto em = e.find("empty");
        const bool has_data = (tl != e.end() && tl->is_array()) || (in != e.end() && in->is_array());
        s.empty = (em != e.end() && lutil::truthy(*em)) || !has_data;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const SlotSummary& a, const SlotSummary& b) { return a.id < b.id; });
    return out;
}

}  // namespace model::pack
