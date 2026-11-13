// SHA-256 of a file as lower case hex, with Windows' own BCrypt (no other library). "" when the file cannot be
// opened or read. Read shared (a DLL the process has loaded can be hashed). Used by the plugin, the updater and
// Report a bug for the Manager-mode test (manager_mode.hpp); the loader has its own (loader_core.cpp).
#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace file_sha256 {

inline std::string hex(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
        && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
            if (got == 0) break;
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) { ok = false; break; }
        }
        unsigned char d[32];
        if (ok && BCryptFinishHash(hash, d, sizeof d, 0) == 0) {
            static const char* hx = "0123456789abcdef";
            for (unsigned char c : d) { out += hx[c >> 4]; out += hx[c & 15]; }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return out;
}

} // namespace file_sha256
