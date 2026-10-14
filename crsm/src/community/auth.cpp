#include "auth.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace community::auth {
namespace {

bool lower_starts(const std::string& s, const char* p) {
    for (size_t i = 0; p[i]; ++i)
        if (i >= s.size() || std::tolower((unsigned char)s[i]) != p[i]) return false;
    return true;
}

std::string str_of(const json& j, const char* k) {
    auto it = j.find(k);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

}  // namespace

std::string hex_encode(const uint8_t* p, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string r;
    r.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { r += hex[p[i] >> 4]; r += hex[p[i] & 0xF]; }
    return r;
}

std::string ticket_brief(const uint8_t* p, size_t n) {
    return std::to_string(n) + " bytes, starts " + (n ? hex_encode(p, n < 2 ? n : 2) : std::string("-"));
}

bool public_cfn(const std::string& in) {
    size_t a = 0, b = in.size();
    while (a < b && std::isspace((unsigned char)in[a])) ++a;
    while (b > a && std::isspace((unsigned char)in[b - 1])) --b;
    const std::string s = in.substr(a, b - a);
    if (s.empty()) return false;
    size_t chars = 0;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
        if ((c & 0xC0) != 0x80) ++chars;                   // UTF-8: count the lead bytes
    }
    if (chars > 32) return false;
    bool digits = true;
    for (unsigned char c : s) if (!std::isdigit(c)) { digits = false; break; }
    if (digits && s.size() >= 5) return false;                                   // a SteamID64 or another number
    for (const char* p : { "anon_", "local_" }) {                                // 2.0.0's fallbacks: anon_<n>, local_<n>
        if (!lower_starts(s, p)) continue;
        bool rest = true;
        for (size_t i = std::char_traits<char>::length(p); i < s.size(); ++i) if (!std::isdigit((unsigned char)s[i])) rest = false;
        if (rest) return false;
    }
    return !lower_starts(s, "steam:") && !lower_starts(s, "v4:");
}

bool token_ok(const std::string& t) {
    if (t.size() != 43) return false;
    for (unsigned char c : t) if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
    return true;
}

bool needs_token(const std::string& cmd) { return cmd == "share" || cmd == "vote" || cmd == "delete"; }

json token_json(const Token& t) {
    json j = json::object();
    j["token"] = t.token;
    j["api_base"] = t.api_base;
    if (!t.cfn.empty()) j["cfn"] = t.cfn;
    j["method"] = t.method;
    j["linked_at"] = t.linked_at;
    j["client_version"] = t.client_version;
    return j;
}

bool token_from_json(const json& j, Token* out) {
    if (!j.is_object()) return false;
    Token t;
    t.token = str_of(j, "token");
    if (!token_ok(t.token)) return false;
    t.api_base = str_of(j, "api_base");
    t.cfn = str_of(j, "cfn");
    t.method = str_of(j, "method");
    t.linked_at = str_of(j, "linked_at");
    t.client_version = str_of(j, "client_version");
    *out = std::move(t);
    return true;
}

std::string token_path(const std::string& config_path) {
    const std::filesystem::path dir = std::filesystem::path(config_path).parent_path();
    std::string p = (dir / kTokenFile).generic_string();
    return p;
}

std::string load_token(const std::string& path, const std::string& api_base) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Token t;
    if (!token_from_json(json::parse(text, nullptr, false), &t)) return "";
    return t.api_base == api_base ? t.token : std::string();
}

bool save_token(const std::string& path, const Token& t, std::string* err) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f << token_json(t).dump(2, ' ', false, json::error_handler_t::replace);
    f.close();
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    return true;
}

bool drop_token(const std::string& path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

std::string request_headers(const std::string& api_key, const std::string& bearer, const std::string& client_version) {
    std::string h = "Content-Type: application/json\r\nAccept: application/json";
    if (!api_key.empty()) h += "\r\nx-api-key: " + api_key;
    if (!client_version.empty()) h += "\r\nx-client-version: " + client_version;
    if (!bearer.empty()) h += "\r\nAuthorization: Bearer " + bearer;
    return h;
}

std::string error_code(const json& data) {
    if (!data.is_object()) return "";
    const std::string c = str_of(data, "error_code");
    return c.empty() ? str_of(data, "error") : c;
}

TicketAnswer ticket_answer(const Reply& rep) {
    TicketAnswer a;
    const json& d = rep.data;
    if (!d.is_object()) { a.code = "bad_answer"; return a; }
    a.code = error_code(d);
    a.message = str_of(d, "error");                 // make_reply put the message there (the code is in error_code)
    const std::string tok = str_of(d, "token");
    if (rep.ok && a.code.empty() && token_ok(tok)) {
        a.ok = true;
        a.token = tok;
        a.cfn = str_of(d, "cfn");
        a.message.clear();
        return a;
    }
    if (a.code.empty()) a.code = rep.ok ? "bad_answer" : "network";
    return a;
}

std::string refusal_code(const TicketAnswer& a) {
    if (a.code == "banned" || a.code == "steam_banned") return "banned";
    if (a.code == "rate_limited") return "rate_limited";
    return kUnavailable;
}

Reply refused_reply(const std::string& code) {
    Reply r;
    r.ok = true;
    r.data = json::object();
    r.data["ok"] = false;
    r.data["error_code"] = code;
    r.data["error"] = code == "banned" ? "This Steam account is banned from SF6 Community"
                    : code == "rate_limited" ? "Too many requests: try again later"
                    : "Steam verification unavailable: sharing and likes are off for now";
    return r;
}

}  // namespace community::auth
