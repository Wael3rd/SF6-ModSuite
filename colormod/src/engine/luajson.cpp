// engine::luajson — implementation (moved out of vfx/settings.cpp, phase 5c-2).

#include "luajson.hpp"

#include <fstream>

namespace engine::luajson {

namespace fs = std::filesystem;

// ---- REFramework's json module ------------------------------------------------------------------------------------

// decode_any: what json.load_file hands the Lua
json decode(const json& j) {
    if (j.is_object()) {
        json t = json::object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            json v = decode(it.value());
            if (!v.is_null()) t[it.key()] = std::move(v);           // t[k] = nil stores nothing
        }
        return t;
    }
    if (j.is_array()) {                                             // t[i + 1] = v: a null leaves a hole
        size_t n = j.size();
        while (n > 0 && j[n - 1].is_null()) --n;
        json t = json::array();
        for (size_t i = 0; i < n; ++i) t.push_back(decode(j[i]));
        return t;
    }
    if (j.is_number_unsigned() && j.get<uint64_t>() > (uint64_t)INT64_MAX) return json((int64_t)j.get<uint64_t>());
    return j;
}

// encode_any: what json.dump_file writes for a Lua value
json encode(const json& v) {
    if (v.is_object()) {
        if (v.empty()) return json();                               // json j{} with nothing pushed = null
        json j = json::object();
        for (auto it = v.begin(); it != v.end(); ++it) j[it.key()] = encode(it.value());
        return j;
    }
    if (v.is_array()) {
        if (v.empty()) return json();
        bool holes = false;
        for (const auto& e : v) holes = holes || e.is_null();
        json j = holes ? json::object() : json::array();            // a sequence with a hole is not an array: its keys
        for (size_t i = 0; i < v.size(); ++i) {                     // are written as strings (never in the mod's files)
            if (!holes) j.push_back(encode(v[i]));
            else if (!v[i].is_null()) j[std::to_string(i + 1)] = encode(v[i]);
        }
        return j;
    }
    return v;
}

json load_file(const fs::path& p) {
    try { return decode(json::parse(std::ifstream{p})); }           // missing or invalid file: nil
    catch (const json::exception&) { return json(); }
}

namespace {
std::string to_crlf(const std::string& lf) {                        // the text-mode LF -> CRLF of json.dump_file
    std::string crlf;
    crlf.reserve(lf.size() + lf.size() / 8);
    for (char ch : lf) { if (ch == '\n') crlf += '\r'; crlf += ch; }
    return crlf;
}
}  // namespace

// json.dump_file's bytes, written to a temporary file then moved over the old one (a crash never leaves half a file)
bool write_text(const fs::path& p, const std::string& lf, std::string* err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const std::string crlf = to_crlf(lf);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp.string(); return false; }
        f.write(crlf.data(), (std::streamsize)crlf.size());
        if (!f) { if (err) *err = "write failed " + tmp.string(); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); if (err) *err = "cannot replace " + p.string(); return false; }
    return true;
}

bool dump_bytes(const json& v, std::string& out, std::string* err) {
    try { out = to_crlf(encode(v).dump(4)); }
    catch (const json::exception& e) { if (err) *err = e.what(); return false; }
    return true;
}

bool dump_file(const fs::path& p, const json& v, std::string* err) {
    std::string text;
    try { text = encode(v).dump(4); }
    catch (const json::exception& e) { if (err) *err = e.what(); return false; }
    return write_text(p, text, err);
}

// ---- Lua semantics on those values --------------------------------------------------------------------------------

bool truthy(const json& v) { return !(v.is_null() || (v.is_boolean() && !v.get<bool>())); }
bool is_table(const json& v) { return v.is_object() || v.is_array(); }

// t.k
json index(const json& t, const char* k) {
    if (t.is_object()) { auto it = t.find(k); return it == t.end() ? json() : *it; }
    if (t.is_array() || t.is_string()) return json();               // no such key / the string library
    throw LuaError(std::string("attempt to index a ") + (t.is_null() ? "nil" : t.is_boolean() ? "boolean" : "number") + " value");
}

// t.k = v (t a table)
void set(json& t, const char* k, json v) {
    if (v.is_null()) { if (t.is_object()) t.erase(k); return; }    // t.k = nil on an absent key: nothing changes
    if (t.is_array()) {                                             // a sequence that gets a named field: keyed by index
        json o = json::object();
        for (size_t i = 0; i < t.size(); ++i) if (!t[i].is_null()) o[std::to_string(i + 1)] = t[i];
        t = std::move(o);
    }
    t[k] = std::move(v);
}

// #t (a sequence with a hole: its first border)
size_t len(const json& t) {
    if (!t.is_array()) return 0;
    size_t n = 0;
    while (n < t.size() && !t[n].is_null()) ++n;
    return n;
}


}  // namespace engine::luajson
