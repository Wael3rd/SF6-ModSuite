#include "catalog.hpp"

#include "color_mods.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>

namespace engine {
namespace fs = std::filesystem;

namespace {

constexpr const char* kColorsRel = "SF6_ColorSpinExtra_data\\colors";
constexpr const char* kOverridesRel = "SF6_ColorSpinExtra_data/overrides/";
constexpr const char* kManualDir = "MANUALCOLORS";

std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string dir_of(const std::string& path) {          // Lua: path:match("^(.*)[/\\]") or ""
    const size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? std::string() : path.substr(0, i);
}

}  // namespace

std::string Catalog::ident_of(const std::string& rel) {
    // "..\color_mods\.cache\<name24>_<8 hex>\rest" -> "..\color_mods\.cache\<name24>\rest": the cache folder name carries
    // a hash of the archive's size and date, which changes whenever the archive is replaced
    static const std::string pre = "..\\color_mods\\.cache\\";
    if (rel.compare(0, pre.size(), pre) != 0) return rel;
    const size_t end = rel.find('\\', pre.size());
    if (end == std::string::npos || end < pre.size() + 9 || rel[end - 9] != '_') return rel;
    for (size_t i = end - 8; i < end; ++i) if (!std::isxdigit((unsigned char)rel[i])) return rel;
    return rel.substr(0, end - 9) + rel.substr(end);
}

int Catalog::find_ident(int fighter, int costume, const std::string& ident) const {
    if (auto* l = entries(fighter, costume)) for (auto& e : *l) if (e.ident == ident) return e.k;
    return 0;
}

std::string Catalog::sanitize(const std::string& path) {
    std::string s = path;
    for (char& c : s) if (!std::isalnum((unsigned char)c)) c = '_';
    return s;
}

void Catalog::build(const fs::path& data_dir) {
    data_dir_ = data_dir;
    lists_.clear();
    trees_.clear();
    static const std::regex name_rx(R"(^esf(\d\d\d)_(\d\d\d)_cmd_(\d\d\d)\.user\.2$)");
    std::error_code ec;
    auto add = [&](const fs::path& file, const std::string& rel, int src) {
        const std::string fname = file.filename().string();
        std::smatch m;
        if (!std::regex_match(fname, m, name_rx)) return;
        std::error_code e2;
        if (fs::file_size(file, e2) == 0 || e2) return;                // an emptied file counts as deleted
        const int f = std::stoi(m[1].str()), c = std::stoi(m[2].str()), n = std::stoi(m[3].str());
        const std::string key = std::to_string(f) + "_" + std::to_string(c - 1);   // costume folder 001 = id 0
        Entry e;
        e.n = n;
        e.path = rel;
        e.src = src;
        e.manual = src == 0 && rel.find(kManualDir) != std::string::npos;
        e.ident = ident_of(rel);
        e.okey = key + "_file_" + sanitize(e.ident);
        lists_[key].push_back(std::move(e));
    };
    const fs::path root = data_dir / kColorsRel;
    if (fs::exists(root, ec)) {
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            add(it->path(), fs::relative(it->path(), data_dir, ec).string(), 0);   // backslashes, like fs.glob
        }
    }
    // reframework/color_mods: colour mods as downloaded (folders, loose files, the cache of their archives).
    // Paths stay relative to data_dir ("..\color_mods\..."), so read_tree and the override keys work unchanged.
    const fs::path cm_root = color_mods::root_of(data_dir);
    for (const fs::path& f : color_mods::candidate_files(cm_root)) {
        std::string rel = "..\\color_mods\\" + fs::relative(f, cm_root, ec).string();
        for (char& ch : rel) if (ch == '/') ch = '\\';
        add(f, rel, 1);
    }
    // packs first (folder, then colour number) = RW 1..N, then the editor's colours = MC 1..M
    for (auto& [key, list] : lists_) {
        std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
            if (a.manual != b.manual) return !a.manual;
            if (a.src != b.src) return a.src < b.src;                   // colors\ packs keep their RW numbers, color_mods\ come after
            const std::string da = dir_of(a.path), db = dir_of(b.path);
            if (da != db) return da < db;
            return a.n < b.n;
        });
        int rw = 0, mc = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            list[i].k = (int)i + 1;
            list[i].label = list[i].manual ? "MC " + std::to_string(++mc) : "RW " + std::to_string(++rw);
        }
    }
}

const std::vector<Entry>* Catalog::entries(int fighter, int costume) const {
    auto it = lists_.find(std::to_string(fighter) + "_" + std::to_string(costume));
    return it == lists_.end() ? nullptr : &it->second;
}

int Catalog::label_to_k(int fighter, int costume, const std::string& label) const {
    if (auto* l = entries(fighter, costume)) for (auto& e : *l) if (e.label == label) return e.k;
    return 0;
}

rsz::NodeRef Catalog::read_tree(const std::string& rel_path) {
    std::error_code ec;
    const fs::path p = data_dir_ / rel_path;
    if (!fs::exists(p, ec) || fs::file_size(p, ec) == 0) return nullptr;
    auto bytes = read_all(p);
    auto r = rsz::parse(bytes);
    return r.root;                                                    // null on a parse error
}

rsz::NodeRef Catalog::entry_tree(const Entry& e, bool raw) {
    if (!raw && !e.okey.empty()) if (auto ov = override_tree(e.okey)) return ov;
    auto it = trees_.find(e.path);
    if (it != trees_.end()) return it->second;
    auto t = read_tree(e.path);
    trees_[e.path] = t;
    return t;
}

rsz::NodeRef Catalog::override_tree(const std::string& key) {
    if (key.empty()) return nullptr;
    if (!override_known_[key]) {
        overrides_[key] = read_tree(std::string(kOverridesRel) + key + ".user.2");
        override_known_[key] = true;
    }
    return overrides_[key];
}

bool Catalog::set_override(const std::string& key, const rsz::NodeRef& tree) {
    if (key.empty()) return false;
    std::error_code ec;
    const fs::path p = data_dir_ / (std::string(kOverridesRel) + key + ".user.2");
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (tree) { auto bytes = rsz::serialize(tree); f.write((const char*)bytes.data(), (std::streamsize)bytes.size()); }
    overrides_[key] = tree;
    override_known_[key] = true;
    return true;
}

}  // namespace engine
