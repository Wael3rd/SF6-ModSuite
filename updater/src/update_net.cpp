// SF6 Mod Suite updater: the HTTPS GET (update_net.hpp).
#include "update_net.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <functional>
#include <vector>

#include "update_fs.hpp"
#include "update_logic.hpp"

#pragma comment(lib, "winhttp.lib")

namespace sf6up {

namespace {

constexpr int kTimeoutMs = 20000;
constexpr ULONGLONG kWholeMs = 180000;      // one download, redirects included
constexpr int kMaxHops = 5;

struct H {
    HINTERNET h = nullptr;
    explicit H(HINTERNET x = nullptr) : h(x) {}
    ~H() { if (h) WinHttpCloseHandle(h); }
    H(const H&) = delete;
    H& operator=(const H&) = delete;
};

std::string err(const char* what) {
    char b[96];
    std::snprintf(b, sizeof b, "%s failed (WinHTTP %lu)", what, GetLastError());
    return b;
}

std::wstring header(HINTERNET req, DWORD which) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 16384) return {};
    std::wstring v(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, v.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    v.resize(size / sizeof(wchar_t));
    return v;
}

using Sink = std::function<bool(const char*, size_t)>;

HttpResult get(const std::string& first_url, int64_t max_bytes, const Sink& sink) {
    HttpResult r;
    std::wstring url = widen(first_url);
    const ULONGLONG start = GetTickCount64();
    H session(WinHttpOpen(L"SF6ModSuite-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) session.h = WinHttpOpen(L"SF6ModSuite-Updater/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { r.why = err("WinHttpOpen"); return r; }
    WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    for (int hop = 0; hop <= kMaxHops; ++hop) {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        uc.dwHostNameLength = (DWORD)-1;
        uc.dwUrlPathLength = (DWORD)-1;
        uc.dwExtraInfoLength = (DWORD)-1;
        uc.dwSchemeLength = (DWORD)-1;
        if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) { r.why = "not an address"; return r; }
        const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        r.final_host = narrow(host);
        if (uc.nScheme != INTERNET_SCHEME_HTTPS || uc.nPort != INTERNET_DEFAULT_HTTPS_PORT) { r.why = "not https: " + r.final_host; return r; }
        if (!host_allowed(r.final_host)) { r.why = "a host the updater does not talk to: " + r.final_host; return r; }
        H conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
        if (!conn.h) { r.why = err("connect"); return r; }
        H req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!req.h) { r.why = err("open request"); return r; }
        DWORD off = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;           // never the Windows account's credentials
        WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { r.why = err("send"); return r; }
        if (!WinHttpReceiveResponse(req.h, nullptr)) { r.why = err("receive"); return r; }
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)status;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::wstring loc = header(req.h, WINHTTP_QUERY_LOCATION);
            if (loc.empty()) { r.why = "a redirect without its address"; return r; }
            if (loc[0] == L'/') loc = L"https://" + host + loc;        // a path on the same host
            url = loc;
            continue;
        }
        if (status != 200) { r.why = "HTTP " + std::to_string(status) + " from " + r.final_host; return r; }
        int64_t total = 0;
        std::vector<char> buf(64 * 1024);
        for (;;) {
            if (GetTickCount64() - start > kWholeMs) { r.why = "too slow"; return r; }
            DWORD got = 0;
            if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) { r.why = err("read"); return r; }
            if (got == 0) break;
            total += got;
            if (total > max_bytes) { r.why = "more data than expected"; return r; }
            if (!sink(buf.data(), got)) { r.why = "the data cannot be written"; return r; }
        }
        r.ok = true;
        return r;
    }
    r.why = "too many redirects";
    return r;
}

}  // namespace

HttpResult http_get(const std::string& url, size_t max_bytes, std::string* body) {
    body->clear();
    HttpResult r = get(url, (int64_t)max_bytes, [&](const char* p, size_t n) { body->append(p, n); return true; });
    if (!r.ok) body->clear();
    return r;
}

HttpResult http_get_file(const std::string& url, int64_t max_bytes, const std::wstring& path, const BytesFn& bytes) {
    const std::wstring tmp = path + L".part";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { HttpResult r; r.why = "the download cannot be written"; return r; }
    int64_t received = 0;
    HttpResult r = get(url, max_bytes, [&](const char* p, size_t n) {
        DWORD put = 0;
        if (!WriteFile(h, p, (DWORD)n, &put, nullptr) || put != n) return false;
        received += (int64_t)n;
        if (bytes) bytes(received);
        return true;
    });
    if (r.ok && !FlushFileBuffers(h)) { r.ok = false; r.why = "the download cannot be written"; }
    CloseHandle(h);
    if (r.ok && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { r.ok = false; r.why = "the download cannot be written"; }
    if (!r.ok) DeleteFileW(tmp.c_str());
    return r;
}

}  // namespace sf6up
