#include "store.hpp"

#include <algorithm>

#include "chars.hpp"
#include "luajson.hpp"
#include "lutil.hpp"

namespace model::store {
namespace {

namespace lj = model::luajson;

// pairs(t) of a decoded json table: (key, value) with an object's string keys and a sequence's 1-based integer keys
struct KV { json key; const json* value; };
std::vector<KV> pairs_of(const json& t) {
    std::vector<KV> v;
    if (t.is_object()) {
        for (auto it = t.begin(); it != t.end(); ++it) v.push_back({ json(it.key()), &it.value() });
    } else if (t.is_array()) {
        int64_t k = 1;
        for (const json& e : t) { if (!e.is_null()) v.push_back({ json(k), &e }); ++k; }
    }
    return v;
}

// tonumber(key) normalised as a table key
std::optional<int64_t> num_key(const json& k) {
    const auto n = lutil::to_number(k);
    return n ? lutil::to_int_key(*n) : std::nullopt;
}

std::string k_to_s(int64_t v) { return std::to_string(v); }

}  // namespace

// ---- paths ------------------------------------------------------------------------------------------------------------
std::string char_folder(const std::string& c) { return std::string(kDataFolder) + "/" + c; }
std::string pack_path(const std::string& c, const std::string& f) { return char_folder(c) + "/" + f; }
std::string erased_path(const std::string& c) { return char_folder(c) + "/_erased.json"; }
std::string names_path(const std::string& c) { return char_folder(c) + "/_names.json"; }
std::string virtual_slots_path(const std::string& c) { return char_folder(c) + "/_virtual_slots.json"; }
std::string reversal_virtual_path(const std::string& c) { return char_folder(c) + "/_reversal_virtual.json"; }
std::string reversal_rows_path(const std::string& c) { return char_folder(c) + "/_reversal_rows.json"; }
std::string backups_folder(const std::string& c) { return char_folder(c) + "/Backups"; }
std::string config_path() { return std::string(kDataFolder) + "/config.json"; }
std::string com_state_path() { return std::string(kCommunityFolder) + "/com_state.json"; }

// ---- the pack list ------------------------------------------------------------------------------------------------------
bool is_backup(const std::string& p) { return p.find("Backups") != std::string::npos || p.find("pre_import") != std::string::npos; }

std::string basename_of(const std::string& path) {
    // "^.+\\(.+)$": the last backslash that has a character before it and one after it
    if (path.size() >= 3) {
        for (size_t i = path.size() - 2; i >= 1; --i) {
            if (path[i] == '\\') return path.substr(i + 1);
            if (i == 1) break;
        }
    }
    return path;
}

std::vector<std::string> pack_files(const std::vector<std::string>& paths, const std::set<std::string>& erased) {
    std::vector<std::string> out;
    for (const std::string& p : paths) {
        if (is_backup(p)) continue;
        const std::string name = basename_of(p);
        if (!name.empty() && name[0] == '_') continue;
        if (erased.count(name)) continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string display_name(const std::string& file, const std::map<std::string, std::string>& names) {
    auto it = names.find(file);
    if (it != names.end()) return it->second;
    return lutil::strip_json_ext(file);
}

std::vector<std::string> display_list(const std::vector<std::string>& files, const std::map<std::string, std::string>& names) {
    std::vector<std::string> out{ "-- Select File --" };
    for (const std::string& f : files) out.push_back(display_name(f, names));
    return out;
}

// ---- _erased.json ---------------------------------------------------------------------------------------------------------
std::set<std::string> erased_load(const json& file) {
    std::set<std::string> s;
    if (!file.is_array()) return s;
    for (const json& n : file) { if (!n.is_string()) break; s.insert(n.get<std::string>()); }     // ipairs
    return s;
}

json erased_save(const std::set<std::string>& erased) {
    json t = json::array();
    for (const std::string& n : erased) t.push_back(n);        // std::set: byte-wise order = table.sort
    return t;
}

std::string erased_pack_bytes() {
    std::string out, err;
    lj::dump_string(json::object(), &out, &err);
    return out;
}

// ---- _names.json -----------------------------------------------------------------------------------------------------------
std::map<std::string, std::string> names_load(const json& file) {
    std::map<std::string, std::string> m;
    if (!file.is_object()) return m;                           // a sequence has integer keys: none kept
    for (auto it = file.begin(); it != file.end(); ++it)
        if (it.value().is_string()) m[it.key()] = it.value().get<std::string>();
    return m;
}

json names_save(const std::map<std::string, std::string>& names) {
    json t = json::object();
    for (const auto& [k, v] : names) t[k] = v;
    return t;
}

void set_display_name(std::map<std::string, std::string>& names, const std::string& filename, const std::string* display) {
    if (filename.empty()) return;
    const std::string stem = lutil::strip_json_ext(filename);
    if (!display || display->empty() || *display == stem) names.erase(filename);
    else names[filename] = *display;
}

// ---- config.json ----------------------------------------------------------------------------------------------------------------
bool config_activate_on_load(const json& cfg, bool current) {
    if (!cfg.is_object()) return current;
    const json v = lj::index(cfg, "activate_on_load");
    if (v.is_null()) return current;
    return lutil::truthy(v);
}

json config_with_activate_on_load(const json& cfg, bool on) {
    json c = cfg.is_object() ? cfg : json::object();
    c["activate_on_load"] = on;
    return c;
}

bool config_import_setup(const json& cfg) {
    if (!cfg.is_object()) return true;
    const json v = lj::index(cfg, "import_training_setup");
    return v.is_null() || lutil::truthy(v);
}

bool config_migration_done(const json& cfg) { return cfg.is_object() && lutil::truthy(lj::index(cfg, "migration_done")); }

json config_with_migration(const json& cfg, int64_t count) {
    json c = cfg.is_object() ? cfg : json::object();
    c["migration_done"] = true;
    c["migrated_count"] = count;
    return c;
}

// ---- legacy migration -------------------------------------------------------------------------------------------------------------
std::optional<Migration> legacy_target(const std::string& filename) {
    if (filename == "overlay_config.json" || filename == "settings.json" || filename == "migration_done.json") return std::nullopt;
    const std::string norm_file = lutil::normalize_name(filename);
    for (const auto& c : chars::all()) {
        const std::string norm_char = lutil::normalize_name(c.name);
        if (norm_file.find(norm_char) == std::string::npos) continue;
        Migration m;
        m.char_name = c.name;
        m.dst_name = filename;
        if (norm_file == lutil::normalize_name(std::string(c.name) + ".json")) m.dst_name = std::string(c.name) + "_Base.json";
        return m;
    }
    return std::nullopt;
}

// ---- reversal map ----------------------------------------------------------------------------------------------------------------
void RevMap::load(const json& file) {
    m_.clear();
    for (const KV& rt : pairs_of(file)) {
        std::map<int64_t, json> m;
        if (lj::is_table(*rt.value)) {
            for (const KV& r : pairs_of(*rt.value)) {
                const auto row = num_key(r.key);
                const auto n = lutil::to_number(*r.value);
                if (row && n) m[*row] = *n;
            }
        }
        if (const auto k = num_key(rt.key)) m_[*k] = std::move(m);
    }
}

json RevMap::to_file() const {
    json t = json::object();
    for (const auto& [rt, rows] : m_) {
        json m = json::object();
        for (const auto& [r, n] : rows) m[k_to_s(r)] = n;
        t[k_to_s(rt)] = std::move(m);
    }
    return t;
}

std::optional<json> RevMap::get(int64_t rt, int64_t row) const {
    auto a = m_.find(rt);
    if (a == m_.end()) return std::nullopt;
    auto b = a->second.find(row);
    if (b == a->second.end()) return std::nullopt;
    return b->second;
}

void RevMap::set(int64_t rt, int64_t row, const json& n) { m_[rt][row] = n; }

// ---- extra reversal rows ----------------------------------------------------------------------------------------------------------
json rd_to_json(const RdRow& r) {
    json t = json::object();
    t["Type"] = r.Type; t["IsActive"] = r.IsActive; t["IsValid"] = r.IsValid; t["IsFrame"] = r.IsFrame;
    t["Delay"] = r.Delay; t["Count"] = r.Count; t["SkillIndex"] = r.SkillIndex; t["Level"] = r.Level; t["Frame"] = r.Frame;
    return t;
}

RdRow rd_from_json(const json& t, const RdRow& base) {
    RdRow r = base;
    if (!t.is_object()) return r;
    auto num = [&](const char* k, int64_t* dst) {
        const json v = lj::index(t, k);
        if (const auto n = lutil::to_number(v)) { if (const auto i = lutil::to_int_key(*n)) *dst = *i; }
    };
    auto flag = [&](const char* k, bool* dst) {
        const json v = lj::index(t, k);
        if (v.is_boolean()) *dst = v.get<bool>();
        else if (v.is_number()) *dst = v.get<double>() != 0;
    };
    num("Type", &r.Type); flag("IsActive", &r.IsActive); flag("IsValid", &r.IsValid); flag("IsFrame", &r.IsFrame);
    num("Delay", &r.Delay); num("Count", &r.Count); num("SkillIndex", &r.SkillIndex); num("Level", &r.Level); num("Frame", &r.Frame);
    return r;
}

json rev_rows_to_file(const RevRows& rows) {
    json t = json::object();
    for (int rt = 0; rt < kRevTypes; ++rt) {
        json r = json::object();
        for (int k = kRowsPage; k < kRowsTotal; ++k) {
            auto it = rows[(size_t)rt].find(k);
            r[std::to_string(k)] = rd_to_json(it == rows[(size_t)rt].end() ? RdRow{} : it->second);
        }
        t[std::to_string(rt)] = std::move(r);
    }
    return t;
}

RevRows rev_rows_from_file(const json& file) {
    RevRows out;
    for (int rt = 0; rt < kRevTypes; ++rt) {
        const json rows = file.is_object() ? lj::index(file, std::to_string(rt).c_str()) : json();
        for (int k = kRowsPage; k < kRowsTotal; ++k) {
            const json t = rows.is_object() ? lj::index(rows, std::to_string(k).c_str()) : json();
            out[(size_t)rt][k] = lj::is_table(t) ? rd_from_json(t, RdRow{}) : RdRow{};
        }
    }
    return out;
}

// ---- com_state.json -------------------------------------------------------------------------------------------------------------------
ComState com_state_load(const json& file) {
    ComState s;
    if (!file.is_object()) return s;
    s.last_id = lj::index(file, "last_id");
    const json d = lj::index(file, "downloaded");
    if (d.is_object()) for (auto it = d.begin(); it != d.end(); ++it) s.downloaded[it.key()] = it.value();
    return s;
}

json com_state_save(const ComState& s) {
    json t = json::object();
    if (!s.last_id.is_null()) t["last_id"] = s.last_id;
    json d = json::object();
    for (const auto& [k, v] : s.downloaded) d[k] = v;
    t["downloaded"] = std::move(d);
    return t;
}

std::string community_filename(const std::string& display_name) {
    std::string stem = lutil::strip_json_ext(display_name), out;
    for (char c : stem) if (lutil::is_alnum(c) || c == '-' || c == '_' || c == '.' || c == ' ') out += c;
    return out + ".json";
}

std::optional<json> dl_version(const ComState& s, const std::string& display_name) {
    auto it = s.downloaded.find(community_filename(display_name));
    if (it == s.downloaded.end()) return std::nullopt;
    if (it->second.is_boolean() && it->second.get<bool>()) return json(1);
    return it->second;
}

bool sync_downloaded(ComState& s, const std::set<std::string>& erased, const std::function<bool(const std::string&)>& file_exists) {
    bool changed = false;
    for (auto it = s.downloaded.begin(); it != s.downloaded.end();) {
        if (erased.count(it->first) || !file_exists(it->first)) { it = s.downloaded.erase(it); changed = true; }
        else ++it;
    }
    return changed;
}

}  // namespace model::store
