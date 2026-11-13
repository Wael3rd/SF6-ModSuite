// cc_util.hpp - Custom Combos: JSON and file helpers, no game dependency (compiled into the DLL and the offline test).
//
// The trial files are written by hand, by the editor and by the recorder, so their values are read the way the Lua read
// them: tonumber() semantics (a number, or a string holding one), math.floor for every integer.
#pragma once

#include "nlohmann/json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cc {

using json = nlohmann::json;
namespace fs = std::filesystem;

// Lua tonumber(v): a number, or a string that is one ("3", " 4.5 "); anything else is no number.
inline std::optional<double> num(const json* v) {
    if (!v) return std::nullopt;
    if (v->is_number()) return v->get<double>();
    if (v->is_string()) {
        const std::string& s = v->get_ref<const std::string&>();
        const char* b = s.c_str();
        while (*b == ' ' || *b == '\t') ++b;
        if (!*b) return std::nullopt;
        char* e = nullptr;
        const double d = std::strtod(b, &e);
        if (e == b) return std::nullopt;
        while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r') ++e;
        if (*e) return std::nullopt;
        return d;
    }
    return std::nullopt;
}
inline std::optional<double> num(const json& v) { return num(&v); }

// math.floor(tonumber(v) or def)
inline int64_t inum(const json* v, int64_t def) {
    const auto d = num(v);
    return d ? static_cast<int64_t>(std::floor(*d)) : def;
}

// The member `key` of an object, nullptr when absent or not an object (Lua: t.key, nil).
inline const json* at(const json* o, const char* key) {
    if (!o || !o->is_object()) return nullptr;
    auto it = o->find(key);
    return it == o->end() || it->is_null() ? nullptr : &*it;
}
inline const json* at(const json& o, const char* key) { return at(&o, key); }
inline const json* obj(const json* o, const char* key) { const json* v = at(o, key); return v && v->is_object() ? v : nullptr; }
inline const json* arr(const json* o, const char* key) { const json* v = at(o, key); return v && v->is_array() ? v : nullptr; }
inline std::string str_of(const json* v) { return v && v->is_string() ? v->get<std::string>() : std::string{}; }
inline bool is_str(const json* v, const char* s) { return v && v->is_string() && v->get_ref<const std::string&>() == s; }

// Lua tostring of a JSON number or string (used for keys such as "1235": integers print without a fraction).
inline std::string key_of(const json* v) {
    if (!v) return {};
    if (v->is_string()) return v->get<std::string>();
    if (v->is_number_integer()) return std::to_string(v->get<int64_t>());
    if (v->is_number_float()) {
        const double d = v->get<double>();
        if (std::floor(d) == d && std::fabs(d) < 1e15) return std::to_string(static_cast<int64_t>(d)) + ".0";
        char b[64];
        std::snprintf(b, sizeof(b), "%.14g", d);
        return b;
    }
    return {};
}

// A path as UTF-8 text
inline std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

inline bool read_file(const fs::path& p, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// The file as JSON; a discarded value when it is missing or does not parse.
inline json read_json(const fs::path& p) {
    std::string text;
    if (!read_file(p, text)) return json(json::value_t::discarded);
    return json::parse(text, nullptr, false);
}

}  // namespace cc
