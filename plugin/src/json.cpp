// SF6 Slots — native plugin: the small JSON reader of the files the loader and the plugin write.
// No game access: also compiled by the offline tests (plugin/tests).
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace slots {

const Json* Json::get(const char* key) const {
    if (kind != Object) return nullptr;
    for (auto& [k, v] : obj) if (k == key) return &v;
    return nullptr;
}

namespace {
struct Parser {
    const std::string& t; size_t i = 0; int depth = 0;
    explicit Parser(const std::string& text) : t(text) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }
    bool lit(const char* w) { size_t n = strlen(w); if (t.compare(i, n, w) != 0) return false; i += n; return true; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
        else { o += static_cast<char>(0xE0 | (c >> 12)); o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); o += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    bool string(std::string& o) {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
                case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) return false;
                    uint32_t c16 = static_cast<uint32_t>(strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    utf8(o, c16);
                    break;
                }
                default: o += e;
            }
        }
        if (i >= t.size()) return false;
        ++i;
        return true;
    }
    bool value(Json& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= t.size()) return false;
        char c = t[i];
        bool ok = true;
        if (c == '{') {
            v.kind = Json::Object; ++i; ws();
            if (i < t.size() && t[i] == '}') { ++i; }
            else for (;;) {
                ws(); std::string k;
                if (!string(k)) { ok = false; break; }
                ws(); if (i >= t.size() || t[i] != ':') { ok = false; break; }
                ++i;
                Json child;
                if (!value(child)) { ok = false; break; }
                v.obj.emplace_back(std::move(k), std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v.kind = Json::Array; ++i; ws();
            if (i < t.size() && t[i] == ']') { ++i; }
            else for (;;) {
                Json child;
                if (!value(child)) { ok = false; break; }
                v.arr.push_back(std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            v.kind = Json::String; ok = string(v.s);
        } else if (lit("true")) { v.kind = Json::Bool; v.b = true; }
        else if (lit("false")) { v.kind = Json::Bool; v.b = false; }
        else if (lit("null")) { v.kind = Json::Null; }
        else {
            char* end = nullptr;
            v.num = strtod(t.c_str() + i, &end);
            if (end == t.c_str() + i) ok = false;
            else { v.kind = Json::Number; i = static_cast<size_t>(end - t.c_str()); }
        }
        --depth;
        return ok;
    }
};
} // namespace

bool json_parse(const std::string& text, Json& out) {
    Parser p(text);
    out = Json{};
    return p.value(out);
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

std::string json_write(const Json& j, int indent) {
    const std::string pad(static_cast<size_t>(indent + 4), ' '), end(static_cast<size_t>(indent), ' ');
    switch (j.kind) {
    case Json::Bool: return j.b ? "true" : "false";
    case Json::Number: {
        char b[32];
        if (j.num == static_cast<double>(static_cast<int64_t>(j.num))) snprintf(b, sizeof b, "%lld", static_cast<long long>(j.num));
        else snprintf(b, sizeof b, "%.17g", j.num);
        return b;
    }
    case Json::String: return "\"" + json_escape(j.s) + "\"";
    case Json::Array: {
        if (j.arr.empty()) return "[]";
        std::string o = "[\n";
        for (size_t i = 0; i < j.arr.size(); ++i) o += pad + json_write(j.arr[i], indent + 4) + (i + 1 < j.arr.size() ? ",\n" : "\n");
        return o + end + "]";
    }
    case Json::Object: {
        if (j.obj.empty()) return "{}";
        std::string o = "{\n";
        for (size_t i = 0; i < j.obj.size(); ++i)
            o += pad + "\"" + json_escape(j.obj[i].first) + "\": " + json_write(j.obj[i].second, indent + 4) + (i + 1 < j.obj.size() ? ",\n" : "\n");
        return o + end + "}";
    }
    default: return "null";
    }
}

} // namespace slots
