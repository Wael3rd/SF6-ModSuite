// community - the DLL's side of phase P2: owner "community" (settings.json), the Client over WinHTTP, the Steam-verified
// identity of 2.1.0 (auth.hpp, steam_ticket.hpp: the Worker thread gets a token before a write, the game thread asks Steam
// for the ticket), and the functions the Lua shim (CommunityBridge.lua) calls through the core's Lua bridge:
//
//   SF6Core.clients.SF6_CRSMNative.com_request(cmd, p1..p6)   -> true when the request went out
//        list      char, sort, page, voter_cfn, per_page
//        download  id
//        vote      id, cfn, value
//        delete    id, cfn
//        share     char, name, cfn, player, description, pack_file   (a path relative to reframework/data, read by the worker)
//   com_poll()            -> "idle" | "pending" | "ready", ok   ("ready": the answer is in SF6_Community_data/_native_response.json
//                            as { ok, data }; reading it consumes the answer) | "timeout"
//   com_busy()            -> bool
//   com_abandon()         the shim gave up (its own 600-tick timeout): the answer, if it ever comes, is dropped
//   com_identity(banner)  -> cfn, name   (the shim reads the CFN banner, the DLL does the rest and caches)
//
// Nothing here runs unless the owner "community" is native; with the owner "lua" the DLL stays as P0 left it.
#pragma once

#include <cstdint>
#include <string>

#include "identity.hpp"
#include "sf6core_api.h"

namespace community {

class Client;

// Called once from cmn::init() when the core was found and settings.json says community = native.
void init();
// Core frame callback (game thread): hands finished answers to their callbacks, enforces the timeouts, runs the Steam calls
// of a ticket (and the probe of the switch file SF6_CRSMNative_data/steam_ticket_probe.txt, ~10 s after the first frame).
void on_frame(uint64_t frame);
// Registers the com_* functions of the Lua bridge (only when the owner is native).
void register_lua(const SF6CoreAPI* api, SF6CoreClient* client);
// One JSON object for status.json: owner, old bridge DLL seen, request counters, token present (never the token), the last
// Steam ticket attempt.
std::string status();

// For the DLL's own Community dialog (owner "ui" native, P4): the client the Lua shim also uses (one request in flight,
// shared), and the session's identity (CFN read from the banner by the caller, then config.json, then anon_; resolved
// again at every call, only anon_ and the last banner CFN are kept for the session).
bool on();
Client& client();
Identity identity(const std::string& banner_cfn);

}  // namespace community
