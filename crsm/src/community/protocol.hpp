// community::protocol - the Community HTTP protocol, pure (no network, no file): how a bridge command becomes a request
// for the Community Worker (v4 since 2.1.0: ADR-0018), and how the Worker's answer becomes what the callbacks of
// CommunityBridge.lua received (the v1.x bridge DLL's (ok, data) rule).
//
// A command is a json object like the table CommunityBridge.lua wrote to request.json. 2.1.0 sends the v4 shapes: no
// identity field but `cfn`, and `cfn` only when auth::public_cfn says it is a CFN name (never a SteamID / anon_ / local_):
//   list      { cmd, char, sort, page, voter_cfn, per_page }     GET    /api/list?char=&sort=&page=[&per_page=]  (no voter_cfn)
//   download  { cmd, id }                                         GET    /api/download/<id>
//   vote      { cmd, id, cfn, value }                             POST   /api/vote/<id>      { value, cfn? }
//   delete    { cmd, id, cfn }                                    DELETE /api/delete/<id>    no body
//   share     { cmd, char, name, cfn, player, description, file } POST   /api/share          { char, name, description, file, cfn? }
//   ticket    { cmd, ticket, cfn }                                POST   /auth/steam/ticket  { ticket, cfn?, client_version }
// (page / per_page / value may be numbers or strings; the bridge read them as strings.) The headers (x-api-key,
// x-client-version, Authorization) are auth::request_headers'.
#pragma once

#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

namespace community {

using json = nlohmann::json;

struct Config {
    std::string api_base;   // "https://<api_base from config.json>" (no trailing slash is added or removed)
    std::string api_key;    // the Worker's API_KEY, from SF6_Community_data/config.json only, never in the code
    std::string client_version;   // "2.1.0": x-client-version and the ticket's client_version (set by the Worker thread)
};
// config.json already decoded (luajson::load_file). Missing / non-string members stay empty, as the bridge's key search.
Config config_from_json(const json& cfg);

struct Request {
    std::string method;     // GET | POST | DELETE
    std::string url;
    std::string body;       // empty for GET
    bool has_body = false;
};

// The escape of the bridge: A-Z a-z 0-9 - _ . ~ stay, every other byte becomes %XX (upper case)
std::string url_encode(const std::string& s);

// Builds the request of a command. Returns false with *error set to the text the bridge wrote:
// "no cmd", "api_base not set in config.json", "unknown cmd" (and "no ticket" for a ticket command without one).
bool build_request(const json& cmd, const Config& cfg, Request* out, std::string* error);

// What the HTTP layer brings back.
struct HttpResult {
    bool sent = false;      // false = no HTTP answer (network or TLS failure, abort, timeout)
    int curl_code = 0;      // when !sent: the curl exit code the old bridge reported (28 timeout, 7 refused, 6 DNS...)
    int status = 0;         // HTTP status (logs only: curl without -f hid it)
    std::string body;
    std::string detail;     // for the journal: WinHTTP error number and name
};

// The (ok, data) pair the callbacks of CommunityBridge.lua receive (see its poll()):
//   ok   = the request completed and the answer parsed ("ok": true in response.json), false for a transport failure
//   data = the Worker's answer, decoded as json.load_file does (null members dropped); a 4xx / 5xx answer is data with
//          "error" and ok = true (curl without -f); a failure is { ok = false, cmd, error = "curl exit N" } or
//          { ok = false, error = "empty curl response" }. A v4 refusal { error = <code>, message = <text> } becomes
//          { error = <text>, error_code = <code>, message = <text> }: every callback prints error, so the player reads the
//          server's sentence; the code stays for the decisions (auth::error_code).
struct Reply {
    bool ok = false;
    json data;
};
// An answer that is not json never reached the Lua (it waited for its 600-tick timeout): reported as { error = "timeout" }
// at once, the same ticker text; *why is set so that the journal says what really happened.
Reply make_reply(const std::string& cmd, const HttpResult& r, std::string* why = nullptr);

// tostring() of a json value as the tickers print it ("nil", "true", a number, a string; a table prints an address).
std::string lua_tostring(const json& v);

}  // namespace community
