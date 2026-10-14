// community::auth - the client side of the Steam-verified identity (ADR-0018, method A only: the Steam OpenID link is
// rejected), its pure parts and the token file. The ticket comes from steam_ticket.hpp (the Steam client the game runs
// with); http.cpp's Worker runs the flow, off the game thread:
//
//   a write (share / vote / delete) and no token for this api_base in identity.json
//     -> a GetAuthTicketForWebApi("sf6-community") ticket (TicketSource: the game thread asks Steam, ~0.1-1 s)
//     -> POST /auth/steam/ticket { ticket = hex, cfn = the banner CFN when it is one, client_version } -> { token }
//     -> identity.json { token, api_base, cfn, method, linked_at, client_version }, then CancelAuthTicket
//   every request: x-client-version; Authorization: Bearer <token> when identity.json holds one for this api_base
//   401 invalid_token on a write: identity.json deleted, a new ticket, the write sent again once
//   no ticket, or the ticket refused: the write never goes out; its answer is { error = <text>, error_code = <code> }
// No SteamID, no Steam name and no ticket byte is ever written or logged (a ticket's length and first 4 hex digits at most).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "protocol.hpp"

namespace community::auth {

constexpr const char* kTokenFile = "identity.json";            // next to config.json (SF6_Community_data/), never packaged
constexpr const char* kUnavailable = "verification_unavailable";   // error_code of a write that could get no token

// The ticket as the Worker reads it (/^[0-9a-fA-F]{32,8192}$/): lower-case hex
std::string hex_encode(const uint8_t* p, size_t n);
// For the journal: "<n> bytes, starts ab12" (the first 2 bytes at most, never the ticket)
std::string ticket_brief(const uint8_t* p, size_t n);
// The Worker's rule: a name that may go out as the player's CFN. Never an id: digits only (a SteamID64), anon_ /
// local_ fallbacks, steam: / v4: keys; 1-32 characters, no control character.
bool public_cfn(const std::string& s);
// 32 random bytes in base64url, as the Worker mints them (/^[A-Za-z0-9_-]{43}$/)
bool token_ok(const std::string& t);
// The commands that need the token: share, vote, delete
bool needs_token(const std::string& cmd);

struct Token {
    std::string token, api_base, cfn, method, linked_at, client_version;
};
json token_json(const Token& t);
bool token_from_json(const json& j, Token* out);     // false: not an object, or no well-formed token
// <folder of config.json>/identity.json
std::string token_path(const std::string& config_path);
// The token of identity.json when it was issued by this api_base ("" otherwise: a token of a local test Worker never goes
// to the real one, and the other way round)
std::string load_token(const std::string& path, const std::string& api_base);
bool save_token(const std::string& path, const Token& t, std::string* err);
bool drop_token(const std::string& path);

// The request headers (CRLF separated, no trailing CRLF): Content-Type / Accept json, x-api-key when set, x-client-version
// when set, Authorization: Bearer <token> when a token is given
std::string request_headers(const std::string& api_key, const std::string& bearer, const std::string& client_version);

// The code of a Worker refusal: error_code (make_reply keeps the code there when the answer also had a message) or error;
// "" when the answer has neither
std::string error_code(const json& data);

// POST /auth/steam/ticket's answer (make_reply's data): { ok, token, cfn } or a refusal
struct TicketAnswer {
    bool ok = false;
    std::string token, cfn, code, message;
};
TicketAnswer ticket_answer(const Reply& rep);
// What the write that waited for this ticket answers: banned (an account the Worker or the publisher banned), rate_limited,
// else verification_unavailable
std::string refusal_code(const TicketAnswer& a);
// The answer of a write that never went out: { ok = false, error = <English text>, error_code = code }, ok = true (the
// request completed: the callbacks show a message, as for any refusal of the Worker)
Reply refused_reply(const std::string& code);

}  // namespace community::auth
