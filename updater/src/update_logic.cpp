// SF6 Mod Suite updater: the pure part (update_logic.hpp).
#include "update_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include "nlohmann/json.hpp"

namespace sf6up {

using json = nlohmann::json;

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string asset_family(const std::string& asset) {
    if (asset.size() < 5 || lower(asset.substr(asset.size() - 4)) != ".zip") return "";
    std::string stem = asset.substr(0, asset.size() - 4);
    const size_t us = stem.find_last_of('_');
    if (us != std::string::npos && us > 0) {
        size_t v = us + 1;
        if (v < stem.size() && (stem[v] == 'v' || stem[v] == 'V')) ++v;
        if (v < stem.size() && std::isdigit((unsigned char)stem[v])) stem.resize(us);   // "_1.2.3", "_v1.2.3": the version
    }
    return lower(stem);
}

// ---- versions ----------------------------------------------------------------------------------------------------------------------
std::optional<Version> parse_version(const std::string& s) {
    if (s.empty() || s.size() > 32) return std::nullopt;
    Version v;
    size_t i = 0;
    while (true) {
        size_t start = i;
        uint64_t x = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            x = x * 10 + (uint64_t)(s[i] - '0');
            if (++i - start > 6) return std::nullopt;
        }
        if (i == start) return std::nullopt;
        v.n.push_back((uint32_t)x);
        if (v.n.size() > 4) return std::nullopt;
        if (i == s.size()) break;
        if (s[i] == '.') { ++i; continue; }
        if (s[i] == '-') {
            v.pre = s.substr(i + 1);
            if (v.pre.empty()) return std::nullopt;
            for (char c : v.pre)
                if (!std::isalnum((unsigned char)c) && c != '.') return std::nullopt;
            break;
        }
        return std::nullopt;
    }
    return v;
}

std::optional<int> compare_versions(const std::string& a, const std::string& b) {
    auto va = parse_version(a), vb = parse_version(b);
    if (!va || !vb) return std::nullopt;
    const size_t n = std::max(va->n.size(), vb->n.size());
    for (size_t i = 0; i < n; ++i) {
        const uint32_t x = i < va->n.size() ? va->n[i] : 0, y = i < vb->n.size() ? vb->n[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (va->pre == vb->pre) return 0;
    if (va->pre.empty()) return 1;           // a release after its pre-releases
    if (vb->pre.empty()) return -1;
    return va->pre < vb->pre ? -1 : 1;
}

bool newer(const std::string& a, const std::string& b) {
    auto c = compare_versions(a, b);
    return c && *c > 0;
}

// ---- the packages ------------------------------------------------------------------------------------------------------------------
const std::vector<KnownPackage>& known_packages() {
    static const std::vector<KnownPackage> k = {
        { "slots_loader", "SF6 Slots Loader", "reframework/plugins/SF6_CostumeSlotsNative.dll" },
        { "color_mod", "SF6 Color Mod", "reframework/plugins/SF6_ColorModNative.dll" },
        { "native_core", "SF6 Native Core", "reframework/plugins/sf6core_native.dll" },
        { "recording_manager", "SF6 Recording Manager", "reframework/plugins/SF6_CRSMNative.dll" },
        { "profile_mod", "SF6 Profile Mod", "reframework/plugins/profile_mod.dll" },
    };
    return k;
}

const KnownPackage* find_known(const std::string& id) {
    for (auto& k : known_packages())
        if (id == k.id) return &k;
    return nullptr;
}

bool is_sha256_hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

namespace {

// The deepest nesting of a JSON text (strings skipped), so that a hostile one is refused before it is parsed
int json_depth(const std::string& t) {
    int depth = 0, best = 0;
    bool in_str = false, esc = false;
    for (char c : t) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '[' || c == '{') best = std::max(best, ++depth);
        else if (c == ']' || c == '}') --depth;
    }
    return best;
}

bool asset_name_ok(const std::string& a) {
    if (a.size() < 5 || a.size() > 100 || lower(a.substr(a.size() - 4)) != ".zip") return false;
    if (!std::isalnum((unsigned char)a[0])) return false;
    for (char c : a)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') return false;
    return a.find("..") == std::string::npos;
}

// A tool's program: a bare .exe name (letters, digits, _ - .), nothing that is a path
bool tool_file_ok(const std::string& f) {
    if (f.size() < 5 || f.size() > 64 || lower(f.substr(f.size() - 4)) != ".exe") return false;
    if (!std::isalnum((unsigned char)f[0])) return false;
    for (char c : f)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '.' && c != '-') return false;
    return f.find("..") == std::string::npos;
}

bool str_field(const json& o, const char* k, std::string* out, size_t max) {
    auto it = o.find(k);
    if (it == o.end() || !it->is_string()) return false;
    *out = it->get<std::string>();
    return !out->empty() && out->size() <= max;
}

}  // namespace

bool parse_manifest(const std::string& text, Manifest* out, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    if (text.size() > kManifestMaxBytes) return fail("manifest too large");
    if (json_depth(text) > 8) return fail("manifest nested too deep");
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return fail("manifest is not JSON");
    }
    if (!j.is_object()) return fail("manifest is not an object");
    Manifest m;
    if (!str_field(j, "format", &m.format, 64) || m.format != kManifestFormat) return fail("unknown manifest format");
    if (!str_field(j, "suite", &m.suite, 32) || !parse_version(m.suite)) return fail("no suite version");
    if (!str_field(j, "tag", &m.tag, 33) || m.tag != "v" + m.suite) return fail("tag is not v<suite>");
    auto pk = j.find("packages");
    if (pk == j.end() || !pk->is_array()) return fail("no packages");
    if (pk->size() > kMaxPackages) return fail("too many packages");
    std::set<std::string> ids, assets;
    for (const json& p : *pk) {
        if (!p.is_object()) return fail("a package is not an object");
        ManifestPackage mp;
        if (!str_field(p, "id", &mp.id, 40)) return fail("a package has no id");
        if (!ids.insert(mp.id).second) return fail("package " + mp.id + " twice");
        const KnownPackage* k = find_known(mp.id);
        if (!k) {
            for (char c : mp.id)
                if (!std::isalnum((unsigned char)c) && c != '_') return fail("a package id is not a name");
            m.skipped.push_back(mp.id);
            continue;
        }
        if (!str_field(p, "name", &mp.name, 80)) return fail(mp.id + ": no name");
        if (!str_field(p, "version", &mp.version, 32) || !parse_version(mp.version)) return fail(mp.id + ": no version");
        if (!str_field(p, "asset", &mp.asset, 100) || !asset_name_ok(mp.asset)) return fail(mp.id + ": the asset is not a zip name");
        if (!assets.insert(lower(mp.asset)).second) return fail(mp.id + ": its asset twice");
        if (!str_field(p, "sha256", &mp.sha256, 64) || !is_sha256_hex(mp.sha256)) return fail(mp.id + ": no sha256");
        if (!str_field(p, "proof", &mp.proof, 200) || mp.proof != k->proof) return fail(mp.id + ": its proof file is not " + k->proof);
        auto sz = p.find("size");
        if (sz == p.end() || !sz->is_number_integer()) return fail(mp.id + ": no size");
        mp.size = sz->get<int64_t>();
        if (mp.size <= 0 || mp.size > kAssetMaxBytes) return fail(mp.id + ": size out of range");
        m.packages.push_back(std::move(mp));
    }
    // the tools: optional, each entry on its own (one that is not as expected is left out, the manifest stays good)
    if (auto tl = j.find("tools"); tl != j.end()) {
        if (!tl->is_array() || tl->size() > kMaxTools) {
            m.tool_notes.push_back("tools: not a list of " + std::to_string(kMaxTools) + " entries at most: left out");
        } else {
            std::set<std::string> tool_ids;
            for (const json& t : *tl) {
                ManifestTool mt;
                if (!t.is_object() || !str_field(t, "id", &mt.id, 40)) { m.tool_notes.push_back("a tool without an id: left out"); continue; }
                bool id_ok = true;
                for (char c : mt.id)
                    if (!std::isalnum((unsigned char)c) && c != '_') id_ok = false;
                if (!id_ok) { m.tool_notes.push_back("a tool's id is not a name: left out"); continue; }
                auto out_tool = [&](const char* w) { m.tool_notes.push_back("tool " + mt.id + ": " + w + ": left out"); };
                if (!tool_ids.insert(mt.id).second) { out_tool("twice"); continue; }
                if (!str_field(t, "name", &mt.name, 80)) { out_tool("no name"); continue; }
                if (!str_field(t, "version", &mt.version, 32) || !parse_version(mt.version)) { out_tool("no version"); continue; }
                if (!str_field(t, "asset", &mt.asset, 100) || !asset_name_ok(mt.asset)) { out_tool("the asset is not a zip name"); continue; }
                if (assets.count(lower(mt.asset))) { out_tool("its asset is another entry's"); continue; }
                if (!str_field(t, "sha256", &mt.sha256, 64) || !is_sha256_hex(mt.sha256)) { out_tool("no sha256"); continue; }
                if (!str_field(t, "file", &mt.file, 64) || !tool_file_ok(mt.file)) { out_tool("its file is not a program's name"); continue; }
                auto sz = t.find("size");
                if (sz == t.end() || !sz->is_number_integer()) { out_tool("no size"); continue; }
                mt.size = sz->get<int64_t>();
                if (mt.size <= 0 || mt.size > kAssetMaxBytes) { out_tool("size out of range"); continue; }
                assets.insert(lower(mt.asset));
                m.tools.push_back(std::move(mt));
            }
        }
    }
    *out = std::move(m);
    return true;
}

const ManifestTool* find_tool(const Manifest& m, const std::string& id) {
    for (auto& t : m.tools)
        if (t.id == id) return &t;
    return nullptr;
}

std::string manifest_json(const Manifest& m) {
    json j = json::object();
    j["format"] = m.format;
    j["suite"] = m.suite;
    j["tag"] = m.tag;
    json arr = json::array();
    for (auto& p : m.packages)
        arr.push_back({ { "id", p.id }, { "name", p.name }, { "version", p.version }, { "asset", p.asset }, { "size", p.size },
                        { "sha256", p.sha256 }, { "proof", p.proof } });
    j["packages"] = arr;
    if (!m.tools.empty()) {
        json tools = json::array();
        for (auto& t : m.tools)
            tools.push_back({ { "id", t.id }, { "name", t.name }, { "version", t.version }, { "asset", t.asset }, { "size", t.size },
                              { "sha256", t.sha256 }, { "file", t.file } });
        j["tools"] = tools;
    }
    return j.dump(2);
}

// ---- the plan ----------------------------------------------------------------------------------------------------------------------
Plan make_plan(const Manifest& m, const std::vector<Installed>& installed, bool manager_mode, const std::string& ignored) {
    Plan plan;
    plan.suite = m.suite;
    plan.tag = m.tag;
    for (auto& s : m.skipped) plan.notes.push_back(s + ": not a package this version knows");
    for (auto& s : m.tool_notes) plan.notes.push_back(s);
    for (auto& p : m.packages) {
        const Installed* in = nullptr;
        for (auto& i : installed)
            if (i.id == p.id) in = &i;
        if (!in || !in->present) continue;
        if (in->version.empty()) { plan.notes.push_back(p.id + ": installed, but its DLL reports no version: not offered"); continue; }
        if (!parse_version(in->version)) { plan.notes.push_back(p.id + ": its DLL reports \"" + in->version + "\": not a version"); continue; }
        if (!newer(p.version, in->version)) continue;
        if (manager_mode && (in->managed || p.id == "slots_loader")) {   // nothing of this outside Manager mode
            plan.notes.push_back(p.id + ": installed by the Slotsloader Manager: its updates are the Manager's");
            continue;
        }
        plan.items.push_back({ p, in->version });
    }
    if (!plan.items.empty() && !ignored.empty() && ignored == m.suite) {
        plan.notes.push_back("suite " + m.suite + " was ignored by the player: not offered");
        plan.items.clear();
    }
    return plan;
}

// ---- zip entries -------------------------------------------------------------------------------------------------------------------
const std::vector<std::string>& keep_if_present() {
    // The Recording Manager's settings.json: its owners (lua / native) are the player's choice once written. The loader's
    // costume_mods/settings.ini (the player's Fluffy Mod Manager folder, NSFW words): no package ships it today.
    static const std::vector<std::string> k = { "reframework/data/sf6_crsmnative_data/settings.json", "reframework/costume_mods/settings.ini" };
    return k;
}

namespace {

bool reserved_segment(const std::string& seg) {
    std::string base = lower(seg.substr(0, seg.find('.')));
    static const char* names[] = { "con", "prn", "aux", "nul", "clock$", "conin$", "conout$" };
    for (const char* n : names)
        if (base == n) return true;
    if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) && base[3] >= '0' && base[3] <= '9') return true;
    return false;
}

constexpr const char* kModFolders[] = { "costume_mods", "costume_mods_nsfw", "color_mods", "stage_mods", "title_mods", "ui_mods" };
constexpr const char* kRootFiles[] = { "amd_ags_x64.dll", "amd_ags_x64_real.dll", "sf6_slotsclean.exe" };

}  // namespace

EntryRule classify_entry(const std::string& raw) {
    EntryRule r;
    auto refuse = [&](const std::string& w) { r.kind = Entry::Refuse; r.why = w; return r; };
    if (raw.empty() || raw.size() > 200) return refuse("empty or too long");
    std::string p = raw;
    for (char& c : p) if (c == '\\') c = '/';
    const bool is_dir = p.back() == '/';
    if (is_dir) p.pop_back();
    if (p.empty() || p[0] == '/') return refuse("an absolute path");
    std::vector<std::string> seg;
    size_t a = 0;
    while (true) {
        size_t b = p.find('/', a);
        seg.push_back(p.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (seg.size() > 8) return refuse("too deep");
    for (auto& s : seg) {
        if (s.empty() || s == "." || s == "..") return refuse("an empty, . or .. segment");
        if (s.back() == '.' || s.back() == ' ' || s.front() == ' ') return refuse("a segment ending with a dot or a space");
        for (unsigned char c : s)
            if (c < 0x20 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return refuse("a character Windows refuses");
        if (reserved_segment(s)) return refuse("a reserved name");
    }
    r.path = p;
    const std::string lp = lower(p);
    std::vector<std::string> ls;
    for (auto& s : seg) ls.push_back(lower(s));
    if (seg.size() == 1) {
        if (is_dir) return refuse("a folder at the game's root");
        if (ls[0] == "modinfo.ini" || ls[0] == "readme.txt") { r.kind = Entry::Skip; return r; }
        for (const char* f : kRootFiles)
            if (ls[0] == f) { r.kind = Entry::File; return r; }
        return refuse("a file at the game's root that no package installs");
    }
    if (ls[0] != "reframework") return refuse("outside reframework/");
    if (seg.size() == 2) {
        if (is_dir) { r.kind = Entry::Dir; return r; }
        return refuse("a file directly in reframework/");
    }
    if (ls[1] == "plugins") {
        if (is_dir) return refuse("a folder in reframework/plugins/");
        if (seg.size() != 3 || ls[2].size() < 5 || ls[2].substr(ls[2].size() - 4) != ".dll") return refuse("not a DLL in reframework/plugins/");
        r.kind = Entry::File;
        return r;
    }
    if (ls[1] == "data") {
        if (seg.size() >= 3 && ls[2] == "sf6_update_data") return refuse("the updater's own folder");
        if (is_dir) { r.kind = Entry::Dir; return r; }
        if (seg.size() < 4) return refuse("a file directly in reframework/data/");
        if (ls.back() == "identity.json") return refuse("a player's token");
        if (is_backup_name(ls.back())) return refuse("a backup name");
        for (auto& k : keep_if_present())
            if (lp == k) { r.kind = Entry::KeepIfPresent; return r; }
        r.kind = Entry::File;
        return r;
    }
    for (const char* f : kModFolders) {
        if (ls[1] != f) continue;
        if (is_dir) {
            if (seg.size() > 3) return refuse("a folder inside a mod folder's folder");
            r.kind = Entry::Dir;
            return r;
        }
        if (seg.size() != 3) return refuse("a file inside a player's mod folder");
        for (auto& k : keep_if_present())
            if (lp == k) { r.kind = Entry::KeepIfPresent; return r; }
        r.kind = Entry::File;     // the folder's own readme and tools (README.txt, SF6_CostumeAudit.bat)
        return r;
    }
    return refuse("not a folder a package installs to");
}

// ---- state -------------------------------------------------------------------------------------------------------------------------
State parse_state(const std::string& text) {
    State s;
    s.raw = text;
    if (text.empty() || text.size() > 256 * 1024 || json_depth(text) > 8) return s;
    try {
        json j = json::parse(text);
        if (!j.is_object()) return s;
        if (auto it = j.find("check_for_updates"); it != j.end() && it->is_boolean()) s.check = it->get<bool>();
        if (auto it = j.find("check_in_manager_mode"); it != j.end() && it->is_boolean()) s.check_managed = it->get<bool>();
        if (auto it = j.find("ignored_suite_version"); it != j.end() && it->is_string()) s.ignored = it->get<std::string>();
        if (auto it = j.find("pending_cleanup"); it != j.end() && it->is_array())
            for (auto& e : *it)
                if (e.is_string() && s.cleanup.size() < 256) s.cleanup.push_back(e.get<std::string>());
    } catch (...) {
    }
    return s;
}

std::string state_json(const State& s) {
    json j = json::object();
    try {
        if (!s.raw.empty() && json_depth(s.raw) <= 8) {
            json old = json::parse(s.raw);
            if (old.is_object()) j = old;
        }
    } catch (...) {
    }
    j["check_for_updates"] = s.check;
    j["ignored_suite_version"] = s.ignored;
    j["pending_cleanup"] = s.cleanup;
    return j.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

// "check_in_manager_mode" is the player's own key: state_json keeps it as it was read (s.raw), the module never writes it.
bool check_allowed(const State& s, bool manager_mode, std::string* why) {
    if (!s.check) {
        if (why) *why = "off (check_for_updates is false in state.json): nothing fetched";
        return false;
    }
    if (manager_mode && !s.check_managed) {
        if (why) *why = "off (installed by the Slotsloader Manager: its updates are the Manager's; \"check_in_manager_mode\": true in "
                        "state.json turns the check on): nothing fetched";
        return false;
    }
    return true;
}

// ---- backups -----------------------------------------------------------------------------------------------------------------------
std::string backup_name(const std::string& file_name, const std::string& version, int n) {
    std::string v;
    for (char c : version) v += (std::isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    if (v.empty()) v = "unknown";
    std::string out = file_name + ".old-" + v;
    if (n > 0) out += "." + std::to_string(n);
    return out;
}

bool is_backup_name(const std::string& file_name) {
    return lower(file_name).find(".old-") != std::string::npos;
}

// ---- addresses ---------------------------------------------------------------------------------------------------------------------
std::string asset_url(const std::string& tag, const std::string& asset) {
    return std::string("https://github.com/") + kRepo + "/releases/download/" + tag + "/" + asset;
}

bool host_allowed(const std::string& host) {
    const std::string h = lower(host);
    return h == "github.com" || h == "objects.githubusercontent.com" || h == "release-assets.githubusercontent.com";
}

}  // namespace sf6up
