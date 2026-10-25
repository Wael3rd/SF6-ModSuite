// Character-swap repairs: audited fixes for a few character-swap mods that a game update broke.
// From ZZtaii's Slotsloader fork 1.10.4 (MIT, https://github.com/ZZtaii/slotsloader-manager), unchanged.
// Run only in Manager mode (common/manager_mode.hpp) while the Slotsloader Manager's setting is on
// (an empty swap_repairs.txt in reframework/data/SF6_Costumes_Data): loader_core.cpp, repair_mod_swap.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Audited patch recipes only. Refused inputs are never modified. The caller supplies bytes from
// official base/DLC PAKs, not loose replacements or generated patch PAKs.
enum class SwapRepairState { Unchanged, Repaired, Refused };
struct SwapRepairResult {
    SwapRepairState state = SwapRepairState::Unchanged;
    size_t changes = 0;
    std::string reason;
};
SwapRepairResult repair_swap_scene(std::vector<uint8_t>& data,
                                   const std::vector<uint8_t>& current_official);

struct SwapUserProfile {
    std::string kind;                 // "chain", "mai_self_chain", or C. Viper "model_parts"
    std::string basename;             // donor basename for chains; target basename for model_parts
    std::string donor_path;           // exact current official PAK path
    std::string old_normalized_sha256;
    std::string current_sha256;       // required: later, unreviewed official updates are refused
};
using SwapOfficialRead = std::function<std::vector<uint8_t>(const std::string&)>;
using SwapSha256 = std::function<std::string(const std::vector<uint8_t>&)>;
SwapRepairResult repair_swap_user(std::vector<uint8_t>& data, const std::string& mod_path,
                                  const std::vector<SwapUserProfile>& profiles,
                                  const SwapOfficialRead& official_read, const SwapSha256& sha256);
const std::vector<SwapUserProfile>& swap_user_profiles();
SwapRepairResult repair_swap_file(std::vector<uint8_t>& data, const std::string& mod_path,
                                  const SwapOfficialRead& official_read, const SwapSha256& sha256);
