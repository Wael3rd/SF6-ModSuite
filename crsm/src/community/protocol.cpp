#include "protocol.hpp"

#include "auth.hpp"
#include "model/luajson.hpp"
#include "model/lutil.hpp"

namespace community {
namespace {

// the bridge read page / per_page / value as quoted strings (jv()); a number is written the way Lua tostring()s it
std::string field_text(const json& cmd, const char* key) {
    auto it = cmd.find(key);
    if (it == cmd.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number()) return model::lutil::number_to_string(*it);
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return "";
}

std::string cmd_name(const json& cmd) {
    auto it = cmd.find("cmd");
    return (it != cmd.end() && it->is_string()) ? it->get<std::string>() : "";
}

// the player's CFN name in a write: only a name that may be shown (never the SteamID / anon_ / local_ fallbacks of identity)
void put_cfn(json& body, const json& cmd) {
    const std::string c = field_text(cmd, "cfn");
    if (auth::public_cfn(c)) body["cfn"] = c;
}

// a member copied as the command has it (absent = absent)
void put(json& body, const json& cmd, const char* key) {
    auto it = cmd.find(key);
    if (it != cmd.end() && !it->is_null()) body[key] = *it;
}

}  // namespace

Config config_from_json(const json& cfg) {
    Config c;
    if (!cfg.is_object()) return c;
    auto b = cfg.find("api_base");
    if (b != cfg.end() && b->is_string()) c.api_base = b->get<std::string>();
    auto k = cfg.find("api_key");
    if (k != cfg.end() && k->is_string()) c.api_key = k->get<std::string>();
    return c;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 0xF]; }
    }
    return r;
}

bool build_request(const json& cmd, const Config& cfg, Request* out, std::string* error) {
    *out = Request{};
    const std::string name = cmd.is_object() ? cmd_name(cmd) : "";
    if (name.empty()) { *error = "no cmd"; return false; }
    if (cfg.api_base.empty()) { *error = "api_base not set in config.json"; return false; }
    const std::string& base = cfg.api_base;
    json body = json::object();
    if (name == "share") {
        out->method = "POST"; out->url = base + "/api/share"; out->has_body = true;
        put(body, cmd, "char"); put(body, cmd, "name"); put(body, cmd, "description"); put(body, cmd, "file"); put_cfn(body, cmd);
    } else if (name == "list") {
        // voter_cfn is no longer sent: my_vote comes from the token (the Worker's list answer)
        const std::string pp = field_text(cmd, "per_page");
        out->method = "GET";
        out->url = base + "/api/list?char=" + url_encode(field_text(cmd, "char")) + "&sort=" + url_encode(field_text(cmd, "sort")) +
                   "&page=" + url_encode(field_text(cmd, "page"));
        if (!pp.empty()) out->url += "&per_page=" + url_encode(pp);
    } else if (name == "download") {
        out->method = "GET"; out->url = base + "/api/download/" + url_encode(field_text(cmd, "id"));
    } else if (name == "vote") {
        out->method = "POST"; out->url = base + "/api/vote/" + url_encode(field_text(cmd, "id")); out->has_body = true;
        put(body, cmd, "value"); put_cfn(body, cmd);
    } else if (name == "delete") {
        out->method = "DELETE"; out->url = base + "/api/delete/" + url_encode(field_text(cmd, "id"));   // the author is the token's
    } else if (name == "ticket") {
        const std::string t = field_text(cmd, "ticket");
        if (t.empty()) { *error = "no ticket"; return false; }
        out->method = "POST"; out->url = base + "/auth/steam/ticket"; out->has_body = true;
        body["ticket"] = t; put_cfn(body, cmd);
        if (!cfg.client_version.empty()) body["client_version"] = cfg.client_version;
    } else {
        *error = "unknown cmd"; return false;
    }
    if (out->has_body) out->body = body.dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

Reply make_reply(const std::string& cmd, const HttpResult& r, std::string* why) {
    Reply rep;
    if (!r.sent) {
        rep.ok = false;
        rep.data = json::object();
        rep.data["ok"] = false;
        rep.data["cmd"] = cmd;
        rep.data["error"] = "curl exit " + std::to_string(r.curl_code);
        return rep;
    }
    if (r.body.empty()) {
        rep.ok = false;
        rep.data = json::object();
        rep.data["ok"] = false;
        rep.data["error"] = "empty curl response";
        return rep;
    }
    json parsed = json::parse(r.body, nullptr, false);
    if (parsed.is_discarded()) {
        if (why) *why = "the answer (HTTP " + std::to_string(r.status) + ", " + std::to_string(r.body.size()) + " bytes) is not json";
        rep.ok = false;
        rep.data = json::object();
        rep.data["error"] = "timeout";
        return rep;
    }
    rep.ok = true;
    json data = model::luajson::decode(parsed);
    // v4 refusals: the code goes to error_code, the player's sentence to error (what every callback prints)
    if (data.is_object()) {
        auto e = data.find("error"), m = data.find("message");
        if (e != data.end() && e->is_string() && m != data.end() && m->is_string() && !m->get<std::string>().empty()) {
            data["error_code"] = *e;
            data["error"] = *m;
        }
    }
    // local data = raw.data or raw : a nil / false answer leaves the wrapper
    if (!model::lutil::truthy(data)) {
        data = json::object();
        data["ok"] = true;
        data["cmd"] = cmd;
    }
    rep.data = std::move(data);
    return rep;
}

std::string lua_tostring(const json& v) {
    if (v.is_null()) return "nil";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number()) return model::lutil::number_to_string(v);
    return "table: 0x00000000";
}

}  // namespace community
