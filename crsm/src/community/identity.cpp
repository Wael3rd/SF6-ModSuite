#include "identity.hpp"

namespace community {

Identity resolve_identity(const std::string& banner_cfn, const json& config, const std::function<int()>& random6) {
    std::string pid;
    if (config.is_object()) {
        auto it = config.find("player_id");
        if (it != config.end() && it->is_string()) pid = it->get<std::string>();
    }
    Identity id;
    if (!banner_cfn.empty()) { id.cfn = banner_cfn; id.name = banner_cfn; }
    else if (!pid.empty()) { id.cfn = pid; id.name = pid; }
    else { id.cfn = "anon_" + std::to_string(random6()); id.name = ""; }
    return id;
}

Identity resolve_identity_session(IdentitySession& s, const std::string& banner_cfn, const json& config, const std::function<int()>& random6) {
    if (!banner_cfn.empty()) s.last_banner = banner_cfn;
    return resolve_identity(s.last_banner, config, [&] { if (!s.anon) s.anon = random6(); return s.anon; });
}

}  // namespace community
