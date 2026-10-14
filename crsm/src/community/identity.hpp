// community::identity - who the player is for the Community (CommunityBridge.lua M.identity).
//
//   1. the CFN handle name read from the game's own banner (OnlineBannerUI, e_text_fid_name): needs the game, so the caller
//      reads it (the Lua shim today, the DLL's game adapter in P4) and hands it over
//   2. config.json "player_id" (the Steam ID the old DLL and 2.0.0 wrote, or local_<n>)
//   3. "anon_<6 digits>", regenerated at every launch
// Since 2.1.0 (ADR-0018) only a real CFN name leaves the PC: protocol.cpp sends `cfn` only when auth::public_cfn accepts it,
// so 2. and 3. stay local (the author is the Steam-verified token's account; the Worker ignores declared identities).
// Since 2.0.1 the result is NOT cached for the session (the Lua's _identity was): the CFN can change (another account logged
// in, the banner not yet drawn at the first read), so it is resolved again at each Community dialog open, share and vote.
// `Session` keeps the two things that must stay stable between those calls: the anon_ number and the last CFN read.
//
// 2.1.0 dropped the Steam fill of 2.0.0 (the SteamID and persona name written into config.json as player_id / player_name):
// nothing reads the Steam identity for the Community any more but the ticket of steam_ticket.hpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "nlohmann/json.hpp"

namespace community {

using json = nlohmann::json;

struct Identity {
    std::string cfn;
    std::string name;    // "" for the anon_ fallback
};

// `banner_cfn` = the CFN read from the banner ("" when not available), `config` = config.json decoded (null when missing),
// `random6` = a random number 100000..999999 (injected for the tests).
Identity resolve_identity(const std::string& banner_cfn, const json& config, const std::function<int()>& random6);

// Per-session memory for the re-resolved identity: anon_<n> stays the same all session (one number per launch, as before),
// and the last CFN really read from the banner is kept for when the banner cannot be read at that moment (the dialog of
// another menu is up, a screen change): without it a vote could go out under the config id after one under the CFN.
// Order: banner now > banner read earlier this session > config player_id > anon_.
struct IdentitySession {
    std::string last_banner;
    int anon = 0;
};
Identity resolve_identity_session(IdentitySession& s, const std::string& banner_cfn, const json& config, const std::function<int()>& random6);

}  // namespace community
