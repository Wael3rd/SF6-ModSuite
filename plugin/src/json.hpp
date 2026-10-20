// SF6 Slots — native plugin: the small JSON reader (json.cpp). No game access.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace slots {

// The files the loader and this plugin write
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* key) const;
    int64_t as_int(int64_t def = 0) const { return kind == Number ? static_cast<int64_t>(num) : def; }
    const std::string& as_str() const { static const std::string e; return kind == String ? s : e; }
};
bool json_parse(const std::string& text, Json& out);
std::string json_escape(const std::string& s);
// A value written back as JSON text (objects and arrays indented by 4 per level)
std::string json_write(const Json& j, int indent = 0);

} // namespace slots
