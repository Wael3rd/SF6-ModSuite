#include "http.hpp"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "auth.hpp"
#include "model/luajson.hpp"

#pragma comment(lib, "winhttp.lib")

namespace community {

struct CancelToken {
    std::mutex m;
    HINTERNET req = nullptr;
    bool aborted = false;
    // closes the request handle once, whichever thread gets there first
    void close_req() {
        std::lock_guard<std::mutex> lk(m);
        if (req) { WinHttpCloseHandle(req); req = nullptr; }
    }
    void abort() {
        std::lock_guard<std::mutex> lk(m);
        aborted = true;
        if (req) { WinHttpCloseHandle(req); req = nullptr; }
    }
};

namespace {

void (*g_sink)(const std::string&) = nullptr;
void logf(const std::string& s) { if (g_sink) g_sink(s); }

std::mutex g_cv_mx;
std::string g_client_version;
std::string client_version() { std::lock_guard<std::mutex> lk(g_cv_mx); return g_client_version; }

std::string now_text() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

struct Flag {                                   // authenticating_ for the scope of a ticket wait / ticket request
    std::atomic<bool>& f;
    explicit Flag(std::atomic<bool>& x) : f(x) { f.store(true); }
    ~Flag() { f.store(false); }
    Flag(const Flag&) = delete;
    Flag& operator=(const Flag&) = delete;
};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
};

HttpResult failure(unsigned long err, const char* where) {
    HttpResult r;
    r.sent = false;
    r.curl_code = curl_code_for_winhttp_error(err);
    r.detail = std::string(where) + " error " + std::to_string(err);
    return r;
}

}  // namespace

void set_log(void (*sink)(const std::string&)) { g_sink = sink; }
void set_client_version(const std::string& v) { std::lock_guard<std::mutex> lk(g_cv_mx); g_client_version = v; }

int curl_code_for_winhttp_error(unsigned long err) {
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT: return 28;
        case ERROR_WINHTTP_OPERATION_CANCELLED: return 28;      // our own 15 s watchdog
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return 6;
        case ERROR_WINHTTP_CANNOT_CONNECT: return 7;
        case ERROR_WINHTTP_CONNECTION_ERROR: return 56;
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return 56;
        case ERROR_WINHTTP_SECURE_FAILURE: return 35;
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE: return 60;
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        case ERROR_WINHTTP_INVALID_URL: return 3;
        default: return 56;
    }
}

HttpResult http_perform(const Request& rq, const std::string& api_key, CancelToken* cancel, const std::string& bearer) {
    const std::wstring wurl = widen(rq.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1; uc.dwHostNameLength = (DWORD)-1; uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return failure(GetLastError(), "crack-url");
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return failure(ERROR_WINHTTP_UNRECOGNIZED_SCHEME, "scheme");
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    const std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    // the system's proxy settings (the browser's, WPAD / PAC included); an older Windows falls back to the default proxy
    Handle session(WinHttpOpen(L"SF6_CRSMNative (WinHTTP)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) session.h = WinHttpOpen(L"SF6_CRSMNative (WinHTTP)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return failure(GetLastError(), "open");
    WinHttpSetTimeouts(session.h, kMaxTimeMs, kMaxTimeMs, kMaxTimeMs, kMaxTimeMs);

    Handle conn(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!conn) return failure(GetLastError(), "connect");

    const std::wstring wmethod = widen(rq.method);
    HINTERNET hreq = WinHttpOpenRequest(conn.h, wmethod.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hreq) return failure(GetLastError(), "open-request");
    if (cancel) { std::lock_guard<std::mutex> lk(cancel->m); cancel->req = hreq; }
    struct Closer {
        HINTERNET h; CancelToken* c;
        ~Closer() { if (c) c->close_req(); else WinHttpCloseHandle(h); }
    } closer{hreq, cancel};
    // The first abort may have run between OpenRequest and the lock above: the handle is then closed by it
    auto err_now = [&](const char* where) { return failure(cancel && cancel->aborted ? (unsigned long)ERROR_WINHTTP_OPERATION_CANCELLED : GetLastError(), where); };

    DWORD no_redirect = WINHTTP_DISABLE_REDIRECTS;                 // curl without -L: a 3xx is the answer
    WinHttpSetOption(hreq, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirect, sizeof(no_redirect));

    const std::wstring headers = widen(auth::request_headers(api_key, bearer, client_version()));
    const void* body = rq.has_body ? (const void*)rq.body.data() : WINHTTP_NO_REQUEST_DATA;
    const DWORD blen = rq.has_body ? (DWORD)rq.body.size() : 0;
    if (!WinHttpSendRequest(hreq, headers.c_str(), (DWORD)headers.size(), (LPVOID)body, blen, blen, 0)) return err_now("send");
    if (!WinHttpReceiveResponse(hreq, nullptr)) return err_now("receive");

    HttpResult r;
    DWORD status = 0, sz = sizeof(status);
    if (WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) r.status = (int)status;
    constexpr size_t kMaxBody = 32u * 1024 * 1024;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hreq, &avail)) return err_now("read");
        if (avail == 0) break;
        if (r.body.size() + avail > kMaxBody) { r.sent = false; r.curl_code = 63; r.detail = "answer over 32 MB"; return r; }
        const size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hreq, r.body.data() + at, avail, &got)) return err_now("read");
        r.body.resize(at + got);
        if (got == 0) break;
    }
    r.sent = true;
    return r;
}

// ---- Worker ----------------------------------------------------------------------------------------------------------------------

Worker::Worker() = default;
Worker::~Worker() { if (th_.joinable()) stop(); }

void Worker::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return;
    th_ = std::thread([this] { run(); });
}

void Worker::stop() {
    stop_.store(true);
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    started_.store(false);
}

void Worker::submit(Job job) {
    { std::lock_guard<std::mutex> lk(mx_); jobs_.push_back(std::move(job)); }
    cv_.notify_one();
}

bool Worker::take(Done* out) {
    std::lock_guard<std::mutex> lk(mx_);
    if (done_.empty()) return false;
    *out = std::move(done_.front());
    done_.pop_front();
    return true;
}

void Worker::tick() {
    const int64_t since = running_since_ms_.load();
    if (since == 0 || now_ms() - since <= kMaxTimeMs) return;
    std::lock_guard<std::mutex> lk(token_mx_);
    if (token_) token_->abort();
}

void Worker::run() {
    while (!stop_.load()) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return stop_.load() || !jobs_.empty(); });
            if (stop_.load()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done d;
        try { d = run_job(job); }
        catch (const std::exception& e) {
            d.id = job.id;
            d.reply.ok = false;
            d.reply.data = json::object();
            d.reply.data["ok"] = false;
            d.reply.data["error"] = "internal error";
            logf(std::string("http: job failed with an exception: ") + e.what());
        }
        jobs_done_.fetch_add(1);
        std::lock_guard<std::mutex> lk(mx_);
        done_.push_back(std::move(d));
    }
}

Worker::Done Worker::run_job(const Job& j) {
    const int64_t t0 = now_ms();
    Done d;
    d.id = j.id;
    d.cmd = (j.cmd.is_object() && j.cmd.contains("cmd") && j.cmd["cmd"].is_string()) ? j.cmd["cmd"].get<std::string>() : "";

    auto finish = [&](Reply rep) {
        d.reply = std::move(rep);
        d.ms = now_ms() - t0;
        if (!j.response_path.empty()) {
            json out = json::object();
            out["ok"] = d.reply.ok;
            out["data"] = d.reply.data;
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(j.response_path).parent_path(), ec);
            std::ofstream f(j.response_path, std::ios::binary | std::ios::trunc);
            if (f) f << out.dump(-1, ' ', false, json::error_handler_t::replace);
            else logf("http: cannot write " + j.response_path);
        }
        return d;
    };
    auto error_reply = [&](const std::string& text) {
        Reply rep;
        rep.ok = false;
        rep.data = json::object();
        rep.data["ok"] = false;
        rep.data["error"] = text;
        return rep;
    };

    Config cfg = config_from_json(model::luajson::load_file(j.config_path));
    cfg.client_version = client_version();
    json cmd = j.cmd;
    // share: the pack travels as a path and is read here, off the game thread (what the old request file carried inline)
    if (cmd.is_object() && cmd.contains("file_path")) {
        const std::string fp = cmd["file_path"].is_string() ? cmd["file_path"].get<std::string>() : "";
        cmd.erase("file_path");
        const json file = model::luajson::load_file(fp);
        if (file.is_null()) { logf("http: share: cannot read " + fp); return finish(error_reply("cannot read the drill file")); }
        cmd["file"] = model::luajson::encode(file);
    }
    Request rq;
    std::string err;
    if (!build_request(cmd, cfg, &rq, &err)) { logf("http: " + d.cmd + ": " + err); return finish(error_reply(err)); }

    // the Steam-verified identity (auth.hpp): the token of this api_base, got first when a write has none
    const std::string tpath = auth::token_path(j.config_path);
    std::string bearer = auth::load_token(tpath, cfg.api_base);
    token_state_.store(bearer.empty() ? 0 : 1);
    const bool write = tickets_ && auth::needs_token(d.cmd);
    bool fresh = false;
    // 2.2.3: the first list of the session links the Steam account the game runs with (testers see test loadouts, a
    // changed Steam account replaces the old one's token); one try per session, the list goes out whatever its result
    if (tickets_ && d.cmd == "list" && !linked_session_) {
        linked_session_ = true;
        std::string code;
        const std::string t = acquire(cfg, tpath, cmd, &code);
        if (!t.empty()) bearer = t;
        else logf(std::string("http: list: no new token (") + code + "), sent with " + (bearer.empty() ? "none" : "the stored one"));
    }
    if (write && bearer.empty()) {
        std::string code;
        bearer = acquire(cfg, tpath, cmd, &code);
        if (bearer.empty()) { logf("http: " + d.cmd + ": not sent (" + code + ")"); return finish(auth::refused_reply(code)); }
        fresh = true;
        linked_session_ = true;
    }

    HttpResult hr = perform(rq, cfg, bearer);
    std::string why;
    Reply rep = make_reply(d.cmd, hr, &why);
    // 401 invalid_token: the token is gone (revoked, or more than 5 newer ones): identity.json deleted, a new one once
    auto refused = [&] { return write && hr.sent && hr.status == 401 && auth::error_code(rep.data) == "invalid_token"; };
    auto drop = [&] {
        auth::drop_token(tpath);
        token_state_.store(0);
        logf("http: " + d.cmd + ": the token was refused (401 invalid_token): identity.json deleted");
    };
    if (refused()) {
        drop();
        if (!fresh) {                                   // once: a token just issued and refused is not asked again
            std::string code;
            bearer = acquire(cfg, tpath, cmd, &code);
            if (bearer.empty()) { logf("http: " + d.cmd + ": not sent again (" + code + ")"); return finish(auth::refused_reply(code)); }
            hr = perform(rq, cfg, bearer);
            why.clear();
            rep = make_reply(d.cmd, hr, &why);
            if (refused()) drop();
        }
    }
    d.status = hr.status;
    char line[256];
    std::snprintf(line, sizeof(line), "http: %s -> %s, %zu bytes, %lld ms%s%s", d.cmd.c_str(),
                  hr.sent ? ("HTTP " + std::to_string(hr.status)).c_str() : ("failed (curl " + std::to_string(hr.curl_code) + ")").c_str(),
                  hr.body.size(), (long long)(now_ms() - t0), hr.detail.empty() ? "" : ", ", hr.detail.c_str());
    logf(line);
    if (!why.empty()) logf("http: " + d.cmd + ": " + why);
    return finish(std::move(rep));
}

HttpResult Worker::perform(const Request& rq, const Config& cfg, const std::string& bearer) {
    CancelToken token;
    { std::lock_guard<std::mutex> lk(token_mx_); token_ = &token; }
    running_since_ms_.store(now_ms());
    HttpResult hr = http_perform(rq, cfg.api_key, &token, bearer);
    running_since_ms_.store(0);
    { std::lock_guard<std::mutex> lk(token_mx_); token_ = nullptr; }
    return hr;
}

std::string Worker::acquire(const Config& cfg, const std::string& tpath, const json& cmd, std::string* code) {
    Flag busy(authenticating_);
    *code = auth::kUnavailable;
    const int64_t t0 = now_ms();
    TicketSource::Ticket t = tickets_->get(kTicketWaitMs);
    if (!t.ok || t.bytes.empty()) {
        logf("auth: no Steam ticket (" + (t.why.empty() ? std::string("?") : t.why) + ")");
        if (t.handle) tickets_->release(t.handle);
        return "";
    }
    json tc = json::object();
    tc["cmd"] = "ticket";
    tc["ticket"] = auth::hex_encode(t.bytes.data(), t.bytes.size());
    if (cmd.is_object() && cmd.contains("cfn")) tc["cfn"] = cmd["cfn"];      // build_request keeps it only when it is a CFN
    Request rq;
    std::string err;
    if (!build_request(tc, cfg, &rq, &err)) { tickets_->release(t.handle); logf("auth: " + err); return ""; }
    const HttpResult hr = perform(rq, cfg, "");
    tickets_->release(t.handle);                        // CancelAuthTicket: the Worker has answered (or never will)
    const auth::TicketAnswer a = auth::ticket_answer(make_reply("ticket", hr));
    logf("auth: POST /auth/steam/ticket (ticket " + auth::ticket_brief(t.bytes.data(), t.bytes.size()) + ") -> " +
         (hr.sent ? "HTTP " + std::to_string(hr.status) : "failed (curl " + std::to_string(hr.curl_code) + ")") +
         (a.ok ? std::string(", token received") : ", refused: " + a.code + (a.message.empty() ? std::string() : " (" + a.message + ")")) +
         ", " + std::to_string(now_ms() - t0) + " ms");
    if (!a.ok) { *code = auth::refusal_code(a); return ""; }
    auth::Token tk;
    tk.token = a.token;
    tk.api_base = cfg.api_base;
    tk.cfn = auth::public_cfn(a.cfn) ? a.cfn : std::string();
    tk.method = "ticket";
    tk.linked_at = now_text();
    tk.client_version = cfg.client_version;
    std::string werr;
    if (!auth::save_token(tpath, tk, &werr)) logf("auth: " + werr + " (the token serves this request only)");
    token_state_.store(1);
    code->clear();
    return a.token;
}

}  // namespace community
