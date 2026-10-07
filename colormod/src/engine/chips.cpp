#include "chips.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

#include "nlohmann/json.hpp"

namespace engine::chips {
namespace {

const rsz::Value* fv(const rsz::Node* n, const char* name) { return n ? n->field(name) : nullptr; }

bool enabled(const rsz::Node* cc) {                      // Lua: cc and cc.Enable
    const rsz::Value* e = fv(cc, "Enable");
    return cc && e && e->b;
}
uint32_t color_of(const rsz::Node* cc) {                 // Lua: cc.Color (or 0)
    const rsz::Value* c = fv(cc, "Color");
    return c ? c->u : 0u;
}

}  // namespace

Pair chip_colors(const rsz::Node* tree) {
    struct Ranked { int n; uint32_t c; bool cloth; };
    std::vector<Ranked> ranked;
    const rsz::Value* parts = fv(tree, "Parts");
    const rsz::Node* part = (parts && !parts->arr.empty()) ? parts->arr[0].get() : nullptr;
    const rsz::Value* clusters = fv(part, "Clusters");
    if (clusters) {
        for (const auto& cl : clusters->arr) {
            if (!cl) continue;                           // (a nil slot ends ipairs in Lua: never seen in the files)
            int n = 0;
            std::optional<uint32_t> first;
            if (const rsz::Value* ccs = fv(cl.get(), "CustomizeColors")) {
                for (const auto& cc : ccs->arr) {
                    if (cc && enabled(cc.get())) { ++n; if (!first) first = color_of(cc.get()); }
                }
            }
            if (first) {
                const rsz::Value* nm = fv(cl.get(), "Name");
                ranked.push_back({ n, *first, nm && nm->s.find("Cloth") != std::string::npos });
            }
        }
    }
    lua_sort(ranked, [](const Ranked& a, const Ranked& b) {
        if (a.cloth != b.cloth) return a.cloth;
        return a.n > b.n;
    });
    const uint32_t c0 = ranked.size() > 0 ? ranked[0].c : 0xFFFFFFFFu;
    const uint32_t c1 = ranked.size() > 1 ? ranked[1].c : c0;
    return { c0, c1 };
}

std::optional<std::pair<std::string, int>> nearest_slot(const rsz::Node* tree, uint32_t rgba) {
    const int r = (int)(rgba & 0xFF), g = (int)((rgba >> 8) & 0xFF), b = (int)((rgba >> 16) & 0xFF);
    std::optional<std::pair<std::string, int>> best;
    int best_d = 1000000000;
    const rsz::Value* parts = fv(tree, "Parts");
    if (!parts) return best;
    for (const auto& part : parts->arr) {
        const rsz::Value* clusters = fv(part.get(), "Clusters");
        if (!clusters) continue;
        for (const auto& cl : clusters->arr) {
            const rsz::Value* ccs = fv(cl.get(), "CustomizeColors");
            if (!ccs) continue;
            for (size_t k = 0; k < ccs->arr.size(); ++k) {
                const rsz::Node* cc = ccs->arr[k].get();
                if (!cc || !enabled(cc)) continue;
                const uint32_t c = color_of(cc);
                const int d = std::abs((int)(c & 0xFF) - r) + std::abs((int)((c >> 8) & 0xFF) - g) + std::abs((int)((c >> 16) & 0xFF) - b);
                if (d < best_d) {
                    const rsz::Value* nm = fv(cl.get(), "Name");
                    best = std::make_pair((nm ? nm->s : std::string()) + "/" + std::to_string(k + 1), d);
                    best_d = d;
                }
            }
        }
    }
    return best;
}

std::optional<uint32_t> slot_color(const rsz::Node* tree, const std::string& spec) {
    // Lua: spec:match("^(.-)/(%w+)$") - the name is everything before the last '/', the rest alphanumeric
    const size_t slash = spec.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= spec.size()) return std::nullopt;
    const std::string name = spec.substr(0, slash), n = spec.substr(slash + 1);
    for (char ch : n) if (!std::isalnum((unsigned char)ch)) return std::nullopt;
    const rsz::Value* parts = fv(tree, "Parts");
    if (!parts) return std::nullopt;
    for (const auto& part : parts->arr) {
        const rsz::Value* clusters = fv(part.get(), "Clusters");
        if (!clusters) continue;
        for (const auto& cl : clusters->arr) {
            const rsz::Value* nm = fv(cl.get(), "Name");
            if (!cl || !nm || nm->s != name) continue;
            const rsz::Value* ccs = fv(cl.get(), "CustomizeColors");
            if (n == "avg") {
                uint64_t r = 0, g = 0, b = 0, cnt = 0;
                if (ccs) for (const auto& cc : ccs->arr) {
                    if (cc && enabled(cc.get())) {
                        const uint32_t c = color_of(cc.get());
                        r += c & 0xFF; g += (c >> 8) & 0xFF; b += (c >> 16) & 0xFF; ++cnt;
                    }
                }
                if (cnt == 0) return std::nullopt;
                return 0xFF000000u + (uint32_t)(b / cnt) * 0x10000u + (uint32_t)(g / cnt) * 0x100u + (uint32_t)(r / cnt);
            }
            // tonumber(n): a decimal index (1-based); anything else indexes nothing
            char* end = nullptr;
            const long idx = std::strtol(n.c_str(), &end, 10);
            if (!end || *end != '\0' || idx < 1 || !ccs || (size_t)idx > ccs->arr.size()) return std::nullopt;
            const rsz::Node* cc = ccs->arr[(size_t)idx - 1].get();
            if (!cc) return std::nullopt;
            const rsz::Value* c = fv(cc, "Color");
            if (!c) return std::nullopt;
            return c->u;
        }
    }
    return std::nullopt;
}

namespace {

std::optional<uint32_t> num_of(const nlohmann::json& v) {
    if (v.is_number_integer()) return (uint32_t)(int64_t)v.get<int64_t>();
    if (v.is_number_unsigned()) return (uint32_t)v.get<uint64_t>();
    if (v.is_number_float()) return (uint32_t)(int64_t)v.get<double>();
    return std::nullopt;
}

}  // namespace

bool Config::load_text(const std::string& text) {
    manual_.clear(); slots_.clear(); keys_.clear();
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;      // Lua: json.load_file(...) or {}
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "seen") continue;                      // Lua: chip_cfg.seen = nil
        keys_[it.key()] = true;
        const auto& v = it.value();
        if (!v.is_array() || v.size() < 2) continue;           // Lua: type(...) == "table" and [1] and [2]
        if (auto a = num_of(v[0]), b = num_of(v[1]); a && b) manual_[it.key()] = { *a, *b };
        if (v[0].is_string() && v[1].is_string()) slots_[it.key()] = { v[0].get<std::string>(), v[1].get<std::string>() };
    }
    return true;
}

void Config::load(const std::filesystem::path& file) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) { if (size_ != 0) { manual_.clear(); slots_.clear(); keys_.clear(); size_ = 0; } return; }
    const auto mtime = std::filesystem::last_write_time(file, ec);
    if (!ec && size == size_ && mtime == mtime_) return;
    std::ifstream f(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    load_text(text);
    size_ = size; mtime_ = mtime;
}

const Pair* Config::manual(const std::string& path) const {
    auto it = manual_.find(path);
    return it == manual_.end() ? nullptr : &it->second;
}

const std::pair<std::string, std::string>* Config::slots(int fighter, int costume) const {
    auto it = slots_.find(std::to_string(fighter) + "_" + std::to_string(costume));
    return it == slots_.end() ? nullptr : &it->second;
}

Pair chip_for(const rsz::Node* tree, int fighter, int costume, const std::string* entry_path, std::optional<uint32_t> num,
              const Config& cfg, const GameChips& game) {
    if (entry_path) if (const Pair* o = cfg.manual(*entry_path)) return *o;          // manual pair saved with the colour
    if (!entry_path && num && game) if (auto g = game(fighter, costume, *num)) return *g;   // plain game colour
    if (const auto* s = cfg.slots(fighter, costume)) {
        auto c0 = slot_color(tree, s->first), c1 = slot_color(tree, s->second);
        if (c0 && c1) return { *c0, *c1 };
    }
    return chip_colors(tree);
}

}  // namespace engine::chips
