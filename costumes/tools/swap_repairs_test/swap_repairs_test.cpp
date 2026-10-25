// Swap-repair checks (costumes/loader/swap_repairs.cpp). From ZZtaii's Slotsloader fork 1.10.4 (MIT, tests/swap_repairs.cpp),
// without its "cmd" mode (the colour-file upgrade is tested elsewhere). Build: build_swap_repairs_test.bat.
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "swap_repairs.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <algorithm>

using Bytes = std::vector<uint8_t>;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void w32(Bytes& b, size_t p, uint32_t n) { memcpy(b.data() + p, &n, 4); }
static void w64(Bytes& b, size_t p, uint64_t n) { memcpy(b.data() + p, &n, 8); }
static std::string sha(const Bytes& b) {
    BCRYPT_ALG_HANDLE a = nullptr; BCRYPT_HASH_HANDLE h = nullptr; uint8_t digest[32];
    require(BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA provider");
    require(BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) >= 0, "SHA hash");
    require(BCryptHashData(h, (PUCHAR)b.data(), (ULONG)b.size(), 0) >= 0 && BCryptFinishHash(h, digest, 32, 0) >= 0, "SHA data");
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(a, 0);
    std::string out; const char* hex = "0123456789abcdef";
    for (uint8_t n : digest) { out += hex[n >> 4]; out += hex[n & 15]; } return out;
}
static Bytes read(const std::string& p) { std::ifstream f(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(f), {}); }
static void write(const std::string& p, const Bytes& b) { std::ofstream f(p, std::ios::binary); require((bool)f, "cannot write output"); f.write((const char*)b.data(), b.size()); require((bool)f, "output write failed"); }
static Bytes fixture(bool scene, uint32_t crc = 0xADA9B6AE) {
    size_t r = scene ? 64 : 48; Bytes b(r + 112, 0);
    memcpy(b.data(), scene ? "SCN\0" : "USR\0", 4); w64(b, scene ? 56 : 32, r);
    memcpy(b.data() + r, "RSZ\0", 4); w32(b, r + 4, 16); w32(b, r + 8, 1); w32(b, r + 12, 3);
    w64(b, r + 24, 52); w64(b, r + 32, 80); w64(b, r + 40, 80); w32(b, r + 48, 1);
    w32(b, r + 60, 0x63515A59); w32(b, r + 64, crc); w32(b, r + 68, 0x12345678); w32(b, r + 72, 0xADA9B6AE);
    w32(b, r + 88, 0xADA9B6AE); return b;
}
static void text(Bytes& b, const std::string& s) { for (char c : s) { b.push_back((uint8_t)c); b.push_back(0); } b.push_back(0); b.push_back(0); }
static Bytes user(const std::string& dir, bool updated) {
    auto b = fixture(false); text(b, dir + "/esf028_001_01_chain.chain"); text(b, "Product/Model/esf/esf028/000/shared.user");
    if (updated) text(b, "new update-only fields"); return b;
}
static void generated() {
    int n = 0;
    auto check = [&](bool ok, const char* m) { require(ok, m); ++n; };
    auto old = fixture(true), current = fixture(true, 0xCD9A2E7B), b = old;
    auto r = repair_swap_scene(b, current); auto expected = old; w32(expected, 64 + 64, 0xCD9A2E7B);
    check(r.state == SwapRepairState::Repaired && r.changes == 1 && b == expected, "typed scene repair must preserve payload and unrelated CRC");
    check(repair_swap_scene(b, current).state == SwapRepairState::Unchanged && b == expected, "scene idempotence");
    for (int variant = 0; variant < 7; ++variant) {
        auto bad = old;
        if (variant == 0) w64(bad, 64 + 24, UINT64_MAX);
        if (variant == 1) w64(bad, 64 + 32, 81);
        if (variant == 2) w32(bad, 64 + 48, 7);
        if (variant == 3) w32(bad, 64 + 64, 0xDEADBEEF);
        if (variant == 4) w64(bad, 64 + 40, UINT64_MAX);
        if (variant == 5) { w32(bad, 8, 1); w64(bad, 32, 60); }
        if (variant == 6) bad.resize(100);
        auto before = bad; check(repair_swap_scene(bad, current).state == SwapRepairState::Refused && before == bad, "malformed scene unchanged");
    }
    b = old; check(repair_swap_scene(b, {}).state == SwapRepairState::Refused && b == old, "missing official scene refused");
    b = old; check(repair_swap_scene(b, old).state == SwapRepairState::Refused && b == old, "old official signature refused");
    const std::string donor = "Product/Model/esf/esf028/001", target = "Product/Model/esf/esf001/002";
    const std::string path = "natives/stm/product/model/esf/esf001/002/esf028_001_01_chain.user.2";
    auto source = user(target, false), official = user(donor, true), normalized = user("Product/Model/esf/esf000/000", false);
    SwapUserProfile p{"chain", "esf028_001_01_chain.user.2", "natives/stm/product/model/esf/esf028/001/esf028_001_01_chain.user.2", sha(normalized), sha(official)};
    auto get = [&](const std::string& name) { return name == p.donor_path ? official : Bytes(); };
    b = source; r = repair_swap_user(b, path, {p}, get, sha);
    check(r.state == SwapRepairState::Repaired && b == user(target, true), "USER variable-size donor repair keeps target/shared references");
    expected = b; check(repair_swap_user(b, path, {p}, get, sha).state == SwapRepairState::Unchanged && b == expected, "USER idempotence");
    b = source; b.back() = 1; expected = b;
    check(repair_swap_user(b, path, {p}, get, sha).state == SwapRepairState::Refused && b == expected, "custom USER preserved");
    b = source;
    Bytes prefix_bytes; for (char c : target) { prefix_bytes.push_back((uint8_t)c); prefix_bytes.push_back(0); }
    auto at = std::search(b.begin(), b.end(), prefix_bytes.begin(), prefix_bytes.end());
    require(at != b.end(), "test resource prefix missing");
    for (size_t k = 0; k < target.size(); ++k) if (target[k] == '/') at[2 * k] = '\\';
    expected = b;
    check(repair_swap_user(b, path, {p}, get, sha).state == SwapRepairState::Refused && b == expected, "resource prefix backslashes do not broaden approved fingerprint normalization");
    b = source; check(repair_swap_user(b, path, {p}, [](const std::string&) { return Bytes(); }, sha).state == SwapRepairState::Refused && b == source, "missing donor preserved");
    b = source; auto future = official; future.back() = 1;
    check(repair_swap_user(b, path, {p}, [&](const std::string&) { return future; }, sha).state == SwapRepairState::Refused && b == source, "unreviewed future donor preserved");
    b = source; auto second = p; second.old_normalized_sha256 = std::string(64, '0');
    check(repair_swap_user(b, path, {second, p}, get, sha).state == SwapRepairState::Repaired && b == user(target, true), "multiple older approved profiles supported");
    p.kind = "mai_self_chain"; b = source;
    check(repair_swap_user(b, path, {p}, get, sha).state == SwapRepairState::Unchanged && b == source, "Mai throw repair excludes cross-character swaps");
    b = user("Product/Model/esf/esf028/002", false);
    check(repair_swap_user(b, "natives/stm/product/model/esf/esf028/002/esf028_001_01_chain.user.2", {p}, get, sha).state == SwapRepairState::Repaired && b == user("Product/Model/esf/esf028/002", true), "Mai same-fighter target accepted");
    b = user(donor, false); expected = b;
    check(repair_swap_user(b, p.donor_path, {p}, get, sha).state == SwapRepairState::Unchanged && b == expected, "Mai original slot untouched");
    p.kind = "chain"; b = source; w64(b, 48 + 24, UINT64_MAX); expected = b;
    check(repair_swap_user(b, path, {p}, get, sha).state == SwapRepairState::Refused && b == expected, "malformed USER table preserved");
    p.kind = "model_parts"; p.basename = "esf030v01_model_parts.user.2";
    p.donor_path = "natives/stm/product/charparam/esf/esf030/battle_ud/esf030v00_model_parts.user.2";
    official = user("Product/Model/esf/esf030/001", true); p.current_sha256 = sha(official);
    p.old_normalized_sha256 = sha(normalized); b = user("Product/Model/esf/esf030/002", false);
    check(repair_swap_user(b, "natives/stm/product/charparam/esf/esf030/battle_ud/esf030v01_model_parts.user.2", {p}, get, sha).state == SwapRepairState::Repaired && b == user("Product/Model/esf/esf030/002", true), "model_parts target inferred independently of scene filename and ignores shared000");
    b = user("Product/Model/esf/esf030/002", false); text(b, "Product/Model/esf/esf030/003/extra.mesh"); expected = b;
    check(repair_swap_user(b, "natives/stm/product/charparam/esf/esf030/battle_ud/esf030v01_model_parts.user.2", {p}, get, sha).state != SwapRepairState::Repaired && b == expected, "ambiguous model_parts preserved");
    std::printf("PASS %d generated native swap repair checks\n", n);
}
int main(int argc, char** argv) {
    try {
        if (argc == 1) { generated(); return 0; }
        if (argc == 6 && std::string(argv[1]) == "repair") {
            auto b = read(argv[3]); require(!b.empty(), "input missing");
            std::string root = argv[4];
            auto get = [&](const std::string& path) { std::string flat; for (char c : path) { if (c == '/') flat += "__"; else flat += c; } return read(root + "\\" + flat); };
            auto result = repair_swap_file(b, argv[2], get, sha);
            write(argv[5], b);
            std::printf("%d %zu %s %s\n", (int)result.state, result.changes, sha(b).c_str(), result.reason.c_str());
            return 0;
        }
        throw std::runtime_error("usage: repair nativepath inputfile officialroot outputfile");
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
