// Character-swap repairs (swap_repairs.hpp). From ZZtaii's Slotsloader fork 1.10.4 (MIT), unchanged.
#include "swap_repairs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

namespace swaprepair {
uint32_t u32(const std::vector<uint8_t>& d, size_t o) { uint32_t n; memcpy(&n, d.data() + o, 4); return n; }
uint64_t u64(const std::vector<uint8_t>& d, size_t o) { uint64_t n; memcpy(&n, d.data() + o, 8); return n; }
bool range(uint64_t start, uint64_t count, uint64_t width, size_t length) {
    return start <= length && count <= (length - start) / width;
}
SwapRepairResult refused(const std::string& reason) { return {SwapRepairState::Refused, 0, reason}; }
std::string lower(std::string s) { for (char& c : s) { if (c == '\\') c = '/'; else c = (char)std::tolower((unsigned char)c); } return s; }
std::string fold(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool digits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }
bool fighter(const std::string& s) { return s.size() == 6 && s.substr(0, 3) == "esf" && digits(s.substr(3)); }

// Only the root RSZ instance table is inspected. No raw byte search and no speculative layout edit.
bool rsz_table(const std::vector<uint8_t>& d, bool scene, size_t& table, uint32_t& count) {
    const size_t header = scene ? 64 : 48;
    if (d.size() < header + 48 || memcmp(d.data(), scene ? "SCN\0" : "USR\0", 4)) return false;
    uint64_t r = u64(d, scene ? 56 : 32);
    if (r < header || r % 2 || !range(r, 48, 1, d.size()) || memcmp(d.data() + r, "RSZ\0", 4) || u32(d, (size_t)r + 4) != 16) return false;
    if (scene) {
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    auto top = [&](uint64_t offset, uint32_t n, size_t width) {
        if (!n) return true;
        if (offset < 64 || offset % 4 || !range(offset, n, width, (size_t)r)) return false;
        spans.push_back({offset, offset + uint64_t(n) * width}); return true;
    };
    if (!top(64, u32(d, 4), 32) || !top(u64(d, 24), u32(d, 12), 8)
        || !top(u64(d, 32), u32(d, 8), 8) || !top(u64(d, 40), u32(d, 16), 8)
        || !top(u64(d, 48), u32(d, 20), 16)) return false;
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); ++i) if (spans[i].first < spans[i - 1].second) return false;
    }
    uint32_t objects = u32(d, (size_t)r + 8), users = u32(d, (size_t)r + 16);
    count = u32(d, (size_t)r + 12);
    uint64_t io = u64(d, (size_t)r + 24), data = u64(d, (size_t)r + 32), uo = u64(d, (size_t)r + 40);
    size_t remain = d.size() - (size_t)r;
    if (!count || !range(48, objects, 4, remain) || io < 48ull + 4ull * objects || io % 4 || !range(io, count, 8, remain)
        || data < io + 8ull * count || data > remain || (r + data) % 16 || !range(uo, users, 16, remain)
        || (users && (uo < io + 8ull * count || uo + 16ull * users > data))
        || (!users && uo && uo < io + 8ull * count)) return false;
    table = (size_t)(r + io);
    if (u32(d, table) || u32(d, table + 4)) return false;
    for (uint32_t i = 0; i < objects; ++i) if (u32(d, (size_t)r + 48 + 4ull * i) >= count) return false;
    return true;
}
bool scene_table(const std::vector<uint8_t>& d, size_t& table, uint32_t& count) { return rsz_table(d, true, table, count); }

// Recipe prefixes have fixed lengths. Match at UTF-16 boundaries and require the next slash so
// that 001 cannot match 0010. Everything outside those exact directory prefixes remains byte-exact.
size_t prefix(std::vector<uint8_t>& d, const std::string& from, const std::string& to) {
    if (from.size() != to.size()) return 0;
    size_t count = 0;
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        size_t start = i; std::string s;
        while (i + 1 < d.size() && !d[i + 1] && d[i] >= 32 && d[i] < 127) { s += (char)d[i]; i += 2; }
        if (i + 1 >= d.size() || d[i] || d[i + 1]) continue;
        size_t lead = s.find_first_not_of("@ ");
        if (lead == std::string::npos || fold(s.substr(lead)).compare(0, from.size() + 1, fold(from) + "/")) continue;
        for (size_t j = 0; j < to.size(); ++j) { d[start + 2 * (lead + j)] = (uint8_t)to[j]; d[start + 2 * (lead + j) + 1] = 0; }
        ++count;
    }
    return count;
}
std::string model_dir(const std::string& path) {
    const std::string head = "natives/stm/product/model/esf/";
    if (path.compare(0, head.size(), head)) return {};
    std::string tail = path.substr(head.size());
    if (tail.size() < 12 || tail[6] != '/' || tail[10] != '/' || !fighter(tail.substr(0, 6)) || !digits(tail.substr(7, 3)) || tail.find('/', 11) != std::string::npos) return {};
    return "Product/Model/esf/" + tail.substr(0, 10);
}
bool user_header(const std::vector<uint8_t>& d) {
    size_t table = 0; uint32_t count = 0;
    return rsz_table(d, false, table, count);
}
std::string model_parts_target(const std::vector<uint8_t>& d) {
    std::unordered_set<std::string> dirs;
    const std::string head = "product/model/esf/";
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        std::string s;
        while (i + 1 < d.size() && !d[i + 1] && d[i] >= 32 && d[i] < 127) { s += (char)d[i]; i += 2; }
        if (i + 1 >= d.size() || d[i] || d[i + 1]) continue;
        s = fold(s); auto lead = s.find_first_not_of("@ ");
        if (lead == std::string::npos) continue;
        s = s.substr(lead);
        if (s.compare(0, head.size(), head)) continue;
        s = s.substr(head.size());
        if (s.size() < 12 || s[6] != '/' || s[10] != '/' || !fighter(s.substr(0, 6)) || !digits(s.substr(7, 3))) continue;
        if (s.substr(7, 3) == "000") continue;
        if (s.substr(0, 6) != "esf030") return {};
        dirs.insert(s.substr(0, 10));
    }
    return dirs.size() == 1 ? "Product/Model/esf/" + *dirs.begin() : std::string();
}
}

SwapRepairResult repair_swap_scene(std::vector<uint8_t>& data, const std::vector<uint8_t>& current_official) {
    using namespace swaprepair;
    size_t table = 0; uint32_t count = 0;
    if (!scene_table(data, table, count)) return refused("malformed or unsupported SCN/RSZ instance table");
    constexpr uint32_t type = 0x63515A59, old_crc = 0xADA9B6AE, new_crc = 0xCD9A2E7B;
    std::vector<size_t> offsets;
    for (uint32_t i = 1; i < count; ++i) {
        size_t o = table + 8ull * i;
        if (u32(data, o) != type) continue;
        uint32_t crc = u32(data, o + 4);
        if (crc == old_crc) offsets.push_back(o + 4);
        else if (crc != new_crc) return refused("unknown Alex scene type CRC; scene preserved");
    }
    if (offsets.empty()) return {};
    if (!scene_table(current_official, table, count)) return refused("current official scene unavailable or unsupported");
    bool confirmed = false;
    for (uint32_t i = 1; i < count; ++i) {
        size_t o = table + 8ull * i;
        if (u32(current_official, o) != type) continue;
        if (u32(current_official, o + 4) != new_crc) return refused("current official scene does not confirm the audited Alex CRC");
        confirmed = true;
    }
    if (!confirmed) return refused("current official scene lacks the audited Alex type");
    for (size_t o : offsets) memcpy(data.data() + o, &new_crc, 4);
    return {SwapRepairState::Repaired, offsets.size(), "Alex scene type signature updated"};
}

SwapRepairResult repair_swap_user(std::vector<uint8_t>& data, const std::string& mod_path,
                                  const std::vector<SwapUserProfile>& profiles,
                                  const SwapOfficialRead& official_read, const SwapSha256& sha256) {
    using namespace swaprepair;
    const std::string path = lower(mod_path), name = path.substr(path.rfind('/') + 1);
    bool candidate_seen = false, repaired = false;
    std::string reason = "unrecognized or customized swap USER; preserved";
    std::vector<uint8_t> output;
    for (const auto& profile : profiles) {
        if (name != lower(profile.basename)) continue;
        std::string target;
        if (profile.kind == "chain" || profile.kind == "mai_self_chain") target = model_dir(path);
        else if (profile.kind == "model_parts" && path == "natives/stm/product/charparam/esf/esf030/battle_ud/esf030v01_model_parts.user.2") target = model_parts_target(data);
        if (target.empty()) continue;
        const std::string mai = "Product/Model/esf/esf028/";
        if (profile.kind == "mai_self_chain" && (target.compare(0, mai.size(), mai) || target == model_dir(lower(profile.donor_path)))) continue;
        candidate_seen = true;
        if (!official_read || !sha256 || !user_header(data)) return refused("swap USER input or repair services unavailable");
        std::string donor = profile.kind == "model_parts" ? "Product/Model/esf/esf030/001" : model_dir(lower(profile.donor_path));
        if (donor.empty() || profile.current_sha256.size() != 64 || profile.old_normalized_sha256.size() != 64) { reason = "incomplete swap repair profile"; continue; }
        auto current = official_read(profile.donor_path);
        if (!user_header(current) || lower(sha256(current)) != lower(profile.current_sha256)) { reason = "current official donor missing or changed; USER preserved"; continue; }
        if (!prefix(current, donor, target)) { reason = "current official donor lacks the documented model directory"; continue; }
        if (current == data) return {SwapRepairState::Unchanged, 0, "documented swap USER already current"};
        auto normalized = data;
        if (!prefix(normalized, target, "Product/Model/esf/esf000/000") || lower(sha256(normalized)) != lower(profile.old_normalized_sha256)) continue;
        if (repaired && output != current) return refused("ambiguous swap repair profile; USER preserved");
        output = std::move(current); repaired = true;
    }
    if (!candidate_seen) return {};
    if (!repaired) return refused(reason);
    data.swap(output);
    return {SwapRepairState::Repaired, 1, "documented swap USER rebuilt from current official donor"};
}

const std::vector<SwapUserProfile>& swap_user_profiles() {
    static const std::vector<SwapUserProfile> profiles = {
        {"chain", "esf028_001_01_chain.user.2", "natives/stm/product/model/esf/esf028/001/esf028_001_01_chain.user.2",
         "545126e3dbd0b816c87345a85d369966c28238ddca57df54078dc972987483c2", "1447e93a3804f3d679830b5ab1ff9d961d96d8ab0f14072d11844d98cbbaff01"},
        {"chain", "esf029_001_01_chain.user.2", "natives/stm/product/model/esf/esf029/001/esf029_001_01_chain.user.2",
         "b820dbb3fcd680ff9f7e3cba06be2bb35d1b86da97bad9d608fa546a54693c75", "abb9d96fa7b4666964c4e87c67ed7328e105322c4da4171d343bffa75e890495"},
        {"mai_self_chain", "esf028_001_30_chain.user.2", "natives/stm/product/model/esf/esf028/001/esf028_001_30_chain.user.2",
         "a7341623c4ffe40d0f364fd982f8b857c267fec8d4f70229be58a3baaf3ea557", "5327c7ecd5d6b373903bb84675705078cfe7e027f26f70e0be35def6a53d47f0"},
        {"mai_self_chain", "esf028_001_30_throw_chain.user.2", "natives/stm/product/model/esf/esf028/001/esf028_001_30_throw_chain.user.2",
         "eab461fb038e507a65219dd42dbd58d2418127fb0cdccbcafdd5e521ad15e33d", "c4a97af806f3da8fcd9ce315b1671e48ed23f8345cd98c4dc60567210c473f20"},
        {"mai_self_chain", "esf028_002_30_chain.user.2", "natives/stm/product/model/esf/esf028/002/esf028_002_30_chain.user.2",
         "1fd8da67664765a278639897b08a69c77cd8d2633c7d927b119705359ec2b3e6", "e100765efb0c5eb152aeb63dc026fd75ee2c42844635a4f121ec08354cd0e98f"},
        {"mai_self_chain", "esf028_002_30_throw_chain.user.2", "natives/stm/product/model/esf/esf028/002/esf028_002_30_throw_chain.user.2",
         "8019251583c9ee672cfe258a8904852a58f369e90d5c04d4451eada4b9778a78", "a8b7332b7ee6b60e57bb79c7fd53a223991c3b8b7f178279751124091ab4f628"},
    };
    return profiles;
}
SwapRepairResult repair_swap_file(std::vector<uint8_t>& data, const std::string& mod_path,
                                  const SwapOfficialRead& official_read, const SwapSha256& sha256) {
    const std::string path = swaprepair::lower(mod_path);
    if (path.compare(0, 8, "natives/")) return {};
    if (path.size() >= 7 && path.compare(path.size() - 7, 7, ".scn.20") == 0) {
        std::vector<uint8_t> current;
        if (official_read) {
            current = official_read(path);
            if (current.empty()) for (const char* fallback : {
                "natives/stm/product/charparam/esf/esf001/esf001v00.scn.20",
                "natives/stm/product/charparam/esf/esf002/esf002v00.scn.20",
                "natives/stm/product/charparam/esf/esf003/esf003v00.scn.20",
                "natives/stm/product/charparam/esf/esf005/esf005v00.scn.20",
                "natives/stm/product/charparam/esf/esf030/esf030v00.scn.20"}) {
                current = official_read(fallback); if (!current.empty()) break;
            }
        }
        return repair_swap_scene(data, current);
    }
    auto result = repair_swap_user(data, path, swap_user_profiles(), official_read, sha256);
    if (result.state == SwapRepairState::Unchanged && result.reason.empty()
        && path == "natives/stm/product/charparam/esf/esf030/battle_ud/esf030v01_model_parts.user.2")
        return swaprepair::refused("model-parts repair needs an approved older file profile; preserved");
    return result;
}
