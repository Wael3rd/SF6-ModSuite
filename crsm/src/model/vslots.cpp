#include "vslots.hpp"

#include <algorithm>

#include "luajson.hpp"
#include "lutil.hpp"
#include "timeline.hpp"

namespace model::vslots {
namespace {

namespace lj = model::luajson;

// ipairs(e.timeline): the strings up to the first hole
std::vector<std::string> lines_of(const json& timeline) {
    std::vector<std::string> v;
    if (!timeline.is_array()) return v;
    for (const json& x : timeline) { if (!x.is_string()) break; v.push_back(x.get<std::string>()); }
    return v;
}

// entry_frames(e) of RMT:480
int64_t entry_frames(const json* e) {
    if (!e || !e->is_object()) return 0;
    if (lutil::truthy(lj::index(*e, "empty"))) return 0;
    const json tl = lj::index(*e, "timeline");
    if (!lj::is_table(tl)) return 0;
    return timeline::entry_frames(lines_of(tl));
}

json new_empty_entry(int n) {
    json e = json::object();
    e["id"] = n;
    e["empty"] = true;
    e["timeline"] = json::array();
    e["weight"] = 1;
    return e;
}

}  // namespace

void VSlots::load(const json& file) {
    slots_.clear();
    if (!lj::is_table(file)) return;
    if (file.is_object()) {
        for (auto it = file.begin(); it != file.end(); ++it) {
            const auto num = lutil::to_number(json(it.key()));
            const auto key = num ? lutil::to_int_key(*num) : std::nullopt;
            if (key) slots_[*key] = it.value();
        }
    } else {                                           // a sequence: keys 1..n
        int64_t k = 1;
        for (const json& v : file) { if (!v.is_null()) slots_[k] = v; ++k; }
    }
}

json VSlots::to_file() const {
    json t = json::object();
    for (const auto& [n, e] : slots_) t[std::to_string(n)] = e;
    return t;
}

const json* VSlots::get(int n) const {
    auto it = slots_.find(n);
    return it == slots_.end() ? nullptr : &it->second;
}

double VSlots::weight(int n) const {
    const json* e = get(n);
    double w = 1;
    if (e && e->is_object()) {
        const json wj = lj::index(*e, "weight");
        if (auto x = lutil::to_number(wj)) w = x->get<double>();
    }
    return w > 1 ? w : 1;
}

int64_t VSlots::frames(int n) const { return entry_frames(get(n)); }

std::string VSlots::text(int n, const std::string& empty_word) const {
    const int64_t f = frames(n);
    return f > 0 ? std::to_string(f) + "F" : empty_word;
}

std::string VSlots::name(int n) const {
    const json* e = get(n);
    if (!e || !e->is_object()) return "";
    const json nm = lj::index(*e, "name");
    return nm.is_string() ? nm.get<std::string>() : "";
}

std::vector<int> VSlots::checked() const {
    std::vector<int> t;
    for (const auto& [n, e] : slots_)
        if (e.is_object() && lutil::truthy(lj::index(e, "active")) && entry_frames(&e) > 0) t.push_back((int)n);
    std::sort(t.begin(), t.end());
    return t;
}

std::vector<Row> VSlots::rows() const {
    std::vector<Row> t;
    for (int n = kFirst; n <= kLast; ++n) {
        const json* e = get(n);
        Row r;
        r.n = n;
        r.name = name(n);
        r.active = e && e->is_object() && lj::index(*e, "active") == json(true);
        r.weight = weight(n);
        r.frames = entry_frames(e);
        t.push_back(r);
    }
    return t;
}

json VSlots::export_virtual() const {
    json t = json::array();
    for (int n = kFirst; n <= kLast; ++n) {
        const json* e = get(n);
        if (e && entry_frames(e) > 0) {
            json c = *e;
            c["id"] = n;
            c["empty"] = false;
            c.erase("active");
            t.push_back(std::move(c));
        } else {
            json c = json::object();
            c["id"] = n;
            c["empty"] = true;
            c["timeline"] = json::array();
            json w = e && e->is_object() ? lj::index(*e, "weight") : json();
            c["weight"] = lutil::truthy(w) ? w : json(1);
            json nm = e && e->is_object() ? lj::index(*e, "name") : json();
            c["name"] = lutil::truthy(nm) ? nm : json("");
            t.push_back(std::move(c));
        }
    }
    return t;
}

void VSlots::import_virtual(const json& list) {
    std::map<int64_t, json> keep;
    for (const auto& [n, e] : slots_) {
        const json a = e.is_object() ? lj::index(e, "active") : json();
        if (!a.is_null()) keep[n] = a;
    }
    slots_.clear();
    if (!list.is_array()) return;
    for (const json& e : list) {
        if (e.is_null()) break;                          // ipairs
        if (!e.is_object()) continue;
        const auto id = lutil::to_number(lj::index(e, "id"));
        if (!id) continue;
        const auto key = lutil::to_int_key(*id);
        const double d = id->get<double>();
        if (d < kFirst || d > kLast) continue;
        if (lutil::truthy(lj::index(e, "empty")) || !lj::is_table(lj::index(e, "timeline"))) continue;
        json c = e;
        c["id"] = *id;
        auto k = keep.find(key ? *key : -1);
        if (lj::index(e, "active").is_boolean()) {}      // a pack that carries the flag (docs/PACK_FORMAT.md): its own wins
        else if (k != keep.end()) c["active"] = k->second;
        else c.erase("active");
        if (key) slots_[*key] = std::move(c);
    }
}

void VSlots::set_virtual(int n, const Fields& f) {
    if (n < kFirst || n > kLast) return;
    json& e = slots_[n];
    if (!e.is_object()) e = new_empty_entry(n);
    if (f.name) e["name"] = lutil::utf8_cut(*f.name, kNameMax);
    if (f.active) e["active"] = *f.active;
    if (f.weight) {
        const auto w = lutil::to_number(*f.weight);
        e["weight"] = (w && w->get<double>() > 1) ? *w : json(1);
    }
}

void VSlots::set_virtual_entry(int n, const json& entry) {
    if (n < kFirst || n > kLast || !entry.is_object()) return;
    json c = entry;
    c["id"] = n;
    c["empty"] = false;
    auto it = slots_.find(n);
    if (it != slots_.end() && it->second.is_object()) {
        const json& old = it->second;
        const json oa = lj::index(old, "active");
        if (oa.is_null()) c.erase("active"); else c["active"] = oa;
        if (!lutil::truthy(lj::index(c, "weight"))) {
            const json ow = lj::index(old, "weight");
            if (ow.is_null()) c.erase("weight"); else c["weight"] = ow;
        }
        const json cn = lj::index(c, "name");
        if ((!lutil::truthy(cn) || cn == json("")) && lutil::truthy(lj::index(old, "name"))) c["name"] = lj::index(old, "name");
    }
    slots_[n] = std::move(c);
}

void VSlots::set_name(int n, const std::string& name) {
    if (n < kFirst || n > kLast) return;
    json& e = slots_[n];
    if (!e.is_object()) e = new_empty_entry(n);
    e["name"] = lutil::utf8_cut(name, kNameMax);
}

}  // namespace model::vslots
