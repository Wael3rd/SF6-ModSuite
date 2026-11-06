// profile_mod: the HTTPS GET (WinHTTP, no process, about 10 s at most). No game access. Used by the plugin and net_test.cpp.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <string>

namespace psn {

// GET https://steamcommunity.com<path> (redirects followed); true on a 200, `out` its body (1 MiB at most)
inline bool http_get(const std::wstring& path, std::string& out) {
    HINTERNET ses = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 10000);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, L"steamcommunity.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            ok = true;
            char buf[8192];
            DWORD got = 0;
            while (out.size() < (1u << 20) && WinHttpReadData(req, buf, sizeof buf, &got) && got) out.append(buf, got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

}  // namespace psn
