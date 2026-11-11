// SF6 Mod Suite updater: the one network call, an HTTPS GET (WinHTTP: the system's TLS, its certificate checks, its proxy).
//
// Redirects are followed by hand, 5 at most, each one only to https and only to a host of host_allowed() (github.com and its release
// CDN). No Authorization header, no cookie, no body: nothing about the player is sent; the only header of ours is the User-Agent.
// Runs off the game thread only (it blocks: 20 s per request at most).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sf6up {

struct HttpResult {
    bool ok = false;              // a 200, within max_bytes
    int status = 0;
    std::string why;              // when not ok
    std::string final_host;
};
// The body into `body` (max_bytes at most: more = failure)
HttpResult http_get(const std::string& url, size_t max_bytes, std::string* body);
// A download's progress: the bytes of the body received so far, told as they arrive (on the thread that downloads)
using BytesFn = std::function<void(int64_t)>;
// The body into a file (written beside it, then moved in place); more than max_bytes = failure, the file removed
HttpResult http_get_file(const std::string& url, int64_t max_bytes, const std::wstring& path, const BytesFn& bytes = {});

}  // namespace sf6up
