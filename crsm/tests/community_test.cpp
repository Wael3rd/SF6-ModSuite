// community_test - the tests of phase P2: the Community client of the DLL, no game, no network.
//
//   community_test --work <scratch folder>
//
//   1. offline    request construction, answers -> (ok, data), error mapping, identity, the Steam fill's bytes of config.json
//   2. wire       a stub HTTP server in this process: what really goes on the wire (route, query, headers, body), the
//                 failures (refused, hung, 500, 3xx, empty body, not json), "busy", the 600-tick timeout, the 15 s watchdog
//   3. fixes      the four Community fixes of 2.0.1: refused shares, answers without their fields, vote toggles, identity
//                 re-read
//   4. auth       2.1.0's Steam-verified identity: the pure parts (hex, CFN filter, identity.json, headers, answers,
//                 verdicts), the flow against a stub Worker v4 (ticket -> token -> write, 401 -> one new token, no Steam ->
//                 nothing sent, no SteamID on the wire), the list's author + [V], and the Steam engine (vtable slots 14 / 17,
//                 callback 168, timeouts, cool-down, probe) on a FAKE steam_api: no test ever calls Steam
// It never talks to anything but the stub in this process: no test reaches the Community Worker.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "community/auth.hpp"
#include "community/client.hpp"
#include "community/http.hpp"
#include "community/identity.hpp"
#include "community/page.hpp"
#include "community/protocol.hpp"
#include "community/steam_ticket.hpp"
#include "model/luajson.hpp"
#include "model/store.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace fs = std::filesystem;
namespace lj = model::luajson;
using json = nlohmann::json;
using namespace community;

namespace {

int g_checks = 0, g_fails = 0;
std::string g_section;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_fails; std::printf("  FAIL [%s] %s\n", g_section.c_str(), what.c_str()); }
}
template <class A, class B>
void check_eq(const A& a, const B& b, const std::string& what) {
    ++g_checks;
    if (!(a == b)) {
        ++g_fails;
        std::ostringstream o;
        o << "  FAIL [" << g_section << "] " << what << "\n    got:      " << json(a).dump() << "\n    expected: " << json(b).dump();
        std::printf("%s\n", o.str().c_str());
    }
}
void section(const char* s) { g_section = s; std::printf("[section] %s\n", s); }

std::string read_all(const fs::path& p, bool* ok = nullptr) {
    std::ifstream f(p, std::ios::binary);
    if (ok) *ok = (bool)f;
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void write_all(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
std::string slash(const fs::path& p) { std::string s = p.string(); for (char& c : s) if (c == '\\') c = '/'; return s; }

// ---- a stub HTTP server ----------------------------------------------------------------------------------------------------
struct StubReq { std::string method, target, body; std::map<std::string, std::string> h; };
struct StubResp {
    int status = 200;
    std::string body;
    std::string extra_headers;
    enum Mode { Normal, Hang, Close } mode = Normal;
};

class Stub {
public:
    std::function<StubResp(const StubReq&)> handler;
    std::mutex mx;
    std::vector<StubReq> log;
    int port = 0;

    bool start() {
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
        ls_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        if (bind(ls_, (sockaddr*)&a, sizeof(a)) != 0) return false;
        int len = sizeof(a);
        getsockname(ls_, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        listen(ls_, 8);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        stop_ = true;
        if (ls_ != INVALID_SOCKET) { closesocket(ls_); ls_ = INVALID_SOCKET; }
        if (th_.joinable()) th_.join();
        for (SOCKET s : hung_) closesocket(s);
        hung_.clear();
    }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port); }
    std::vector<StubReq> take_log() { std::lock_guard<std::mutex> lk(mx); auto v = std::move(log); log.clear(); return v; }

private:
    void loop() {
        while (!stop_) {
            fd_set fds; FD_ZERO(&fds); FD_SET(ls_, &fds);
            timeval tv{0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(ls_, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            serve(c);
        }
    }
    static bool read_until(SOCKET c, std::string& buf, const std::string& marker) {
        char tmp[4096];
        while (buf.find(marker) == std::string::npos) {
            const int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buf.append(tmp, (size_t)n);
        }
        return true;
    }
    void serve(SOCKET c) {
        std::string buf;
        if (!read_until(c, buf, "\r\n\r\n")) { closesocket(c); return; }
        const size_t he = buf.find("\r\n\r\n");
        std::istringstream hs(buf.substr(0, he));
        StubReq rq;
        std::string line, proto;
        std::getline(hs, line);
        { std::istringstream l(line); l >> rq.method >> rq.target >> proto; }
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t c2 = line.find(':');
            if (c2 == std::string::npos) continue;
            std::string k = line.substr(0, c2), v = line.substr(c2 + 1);
            for (char& ch : k) ch = (char)tolower((unsigned char)ch);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            rq.h[k] = v;
        }
        size_t want = rq.h.count("content-length") ? (size_t)std::stoul(rq.h["content-length"]) : 0;
        rq.body = buf.substr(he + 4);
        char tmp[8192];
        while (rq.body.size() < want) {
            const int n = recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            rq.body.append(tmp, (size_t)n);
        }
        { std::lock_guard<std::mutex> lk(mx); log.push_back(rq); }
        StubResp rs = handler ? handler(rq) : StubResp{};
        if (rs.mode == StubResp::Hang) { hung_.push_back(c); return; }
        if (rs.mode == StubResp::Close) { closesocket(c); return; }
        std::string out = "HTTP/1.1 " + std::to_string(rs.status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(rs.body.size()) + "\r\nConnection: close\r\n" + rs.extra_headers + "\r\n" + rs.body;
        send(c, out.data(), (int)out.size(), 0);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    SOCKET ls_ = INVALID_SOCKET;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::vector<SOCKET> hung_;
};

// ---- helpers for the client ----------------------------------------------------------------------------------------------------
struct Answer { bool done = false; bool ok = false; json data; };

// sends and polls (one frame every `step_ms`) until the answer comes; max_ticks polls
Answer ask(Client& c, json cmd, int max_ticks = 500, int step_ms = 5) {
    Answer a;
    c.send(std::move(cmd), [&](bool ok, const json& d) { a.done = true; a.ok = ok; a.data = d; });
    for (int i = 0; i < max_ticks && !a.done; ++i) {
        c.poll();
        if (!a.done) std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return a;
}

json C(std::initializer_list<std::pair<std::string, json>> kv) {
    json o = json::object();
    for (auto& p : kv) o[p.first] = p.second;
    return o;
}

// ============================================================================================================================
// 1. offline
// ============================================================================================================================
void test_offline() {
    section("offline: requests");
    Config cfg{ "https://example.test", "K" };
    Request rq; std::string err;

    check_eq(url_encode("Ken Test/é~.-_"), std::string("Ken%20Test%2F%C3%A9~.-_"), "url_encode keeps A-Za-z0-9-_.~ only, upper-case hex");
    check_eq(url_encode(""), std::string(""), "url_encode empty");

    // list: the C bridge's format, per_page only when not empty
    check(build_request(C({{"cmd", "list"}, {"char", "Ken"}, {"sort", "recent"}, {"page", "1"}, {"voter_cfn", "Me & You"}, {"per_page", "50"}}), cfg, &rq, &err), "list builds");
    check_eq(rq.method, std::string("GET"), "list method");
    check_eq(rq.url, std::string("https://example.test/api/list?char=Ken&sort=recent&page=1&per_page=50"), "list url (v4: no voter_cfn, my_vote comes from the token)");
    check(!rq.has_body && rq.body.empty(), "list has no body");
    check(build_request(C({{"cmd", "list"}, {"char", "Dee Jay"}, {"sort", "popular"}, {"page", 2}, {"voter_cfn", ""}}), cfg, &rq, &err), "list without per_page builds");
    check_eq(rq.url, std::string("https://example.test/api/list?char=Dee%20Jay&sort=popular&page=2"), "list url without per_page, number page");

    check(build_request(C({{"cmd", "download"}, {"id", "a/b c"}}), cfg, &rq, &err), "download builds");
    check_eq(rq.method, std::string("GET"), "download method");
    check_eq(rq.url, std::string("https://example.test/api/download/a%2Fb%20c"), "download url");

    json vote = C({{"cmd", "vote"}, {"id", "id-1"}, {"cfn", "Me"}, {"value", 1}});
    check(build_request(vote, cfg, &rq, &err), "vote builds");
    check_eq(rq.method, std::string("POST"), "vote method");
    check_eq(rq.url, std::string("https://example.test/api/vote/id-1"), "vote url");
    check(json::parse(rq.body) == C({{"value", 1}, {"cfn", "Me"}}), "vote body (v4): the value and the CFN only");
    check(rq.body.find("\"value\":1") != std::string::npos, "vote value is a number");

    json del = C({{"cmd", "delete"}, {"id", "id-2"}, {"cfn", "Me"}});
    check(build_request(del, cfg, &rq, &err), "delete builds");
    check_eq(rq.method, std::string("DELETE"), "delete method");
    check_eq(rq.url, std::string("https://example.test/api/delete/id-2"), "delete url");
    check(!rq.has_body && rq.body.empty(), "delete (v4): no body, the author is the token's");

    json share = C({{"cmd", "share"}, {"char", "Ken"}, {"name", "Pack"}, {"cfn", "Me"}, {"player", "Me"}, {"description", ""}, {"file", json::array({C({{"slot", 1}})})}});
    check(build_request(share, cfg, &rq, &err), "share builds");
    check_eq(rq.method, std::string("POST"), "share method");
    check_eq(rq.url, std::string("https://example.test/api/share"), "share url");
    check(json::parse(rq.body) == C({{"char", "Ken"}, {"name", "Pack"}, {"description", ""}, {"file", json::array({C({{"slot", 1}})})}, {"cfn", "Me"}}),
          "share body (v4): char, name, description, file, cfn; no cmd, no player");

    check(!build_request(C({{"char", "Ken"}}), cfg, &rq, &err), "no cmd refused");
    check_eq(err, std::string("no cmd"), "no cmd text");
    check(!build_request(C({{"cmd", "list"}}), Config{}, &rq, &err), "no api_base refused");
    check_eq(err, std::string("api_base not set in config.json"), "no api_base text");
    check(!build_request(C({{"cmd", "ping"}}), cfg, &rq, &err), "unknown cmd refused");
    check_eq(err, std::string("unknown cmd"), "unknown cmd text");

    check_eq(config_from_json(C({{"api_base", "B"}, {"api_key", "K"}})).api_key, std::string("K"), "config key");
    check(config_from_json(json()).api_base.empty(), "config of a missing file");

    section("offline: answers -> (ok, data)");
    HttpResult r; r.sent = true; r.status = 200; r.body = R"({"items":[{"id":"a","my_vote":null,"score":2}],"total":1})";
    Reply rep = make_reply("list", r);
    check(rep.ok, "200 json: ok");
    check(rep.data["items"][0].contains("id") && !rep.data["items"][0].contains("my_vote"), "null member dropped like json.load_file");
    r.status = 404; r.body = R"({"error":"Not found"})";
    rep = make_reply("download", r);
    check(rep.ok && rep.data["error"] == "Not found", "404 body reaches the caller as data with error, ok = true (curl without -f)");
    r.body = "";
    rep = make_reply("download", r);
    check(!rep.ok && rep.data["error"] == "empty curl response" && rep.data["ok"] == false, "empty body");
    r.body = "<html>502</html>"; r.status = 502;
    std::string why;
    rep = make_reply("list", r, &why);
    check(!rep.ok && rep.data["error"] == "timeout" && !why.empty(), "not json -> the 'timeout' text, journal says why");
    HttpResult f; f.sent = false; f.curl_code = 7;
    rep = make_reply("share", f);
    check(!rep.ok && rep.data["error"] == "curl exit 7" && rep.data["cmd"] == "share" && rep.data["ok"] == false, "transport failure text");
    r.status = 200; r.body = "null";
    rep = make_reply("vote", r);
    check(rep.ok && rep.data["ok"] == true && rep.data["cmd"] == "vote", "null answer leaves the wrapper (data = raw.data or raw)");

    section("offline: error mapping");
    check_eq(curl_code_for_winhttp_error(12002), 28, "timeout -> 28");
    check_eq(curl_code_for_winhttp_error(12017), 28, "cancelled (watchdog) -> 28");
    check_eq(curl_code_for_winhttp_error(12007), 6, "name not resolved -> 6");
    check_eq(curl_code_for_winhttp_error(12029), 7, "cannot connect -> 7");
    check_eq(curl_code_for_winhttp_error(12030), 56, "connection error -> 56");
    check_eq(curl_code_for_winhttp_error(12175), 35, "secure failure -> 35");
    check_eq(curl_code_for_winhttp_error(12038), 60, "certificate -> 60");
    check_eq(lua_tostring(json("x")), std::string("x"), "tostring string");
    check_eq(lua_tostring(json(3)), std::string("3"), "tostring integer");
    check_eq(lua_tostring(json(3.0)), std::string("3.0"), "tostring float");
    check_eq(lua_tostring(json(true)), std::string("true"), "tostring bool");
    check_eq(lua_tostring(json()), std::string("nil"), "tostring nil");

    section("offline: identity");
    int calls = 0;
    auto rnd = [&] { ++calls; return 123456; };
    Identity id = resolve_identity("PlayerOne", C({{"player_id", "7656"}}), rnd);
    check(id.cfn == "PlayerOne" && id.name == "PlayerOne", "CFN banner first");
    id = resolve_identity("", C({{"player_id", "7656"}, {"player_name", "Persona"}}), rnd);
    check(id.cfn == "7656" && id.name == "7656", "then config player_id (cfn = name = the id, as the Lua)");
    id = resolve_identity("", json(), rnd);
    check(id.cfn == "anon_123456" && id.name.empty() && calls == 1, "then anon_");
    id = resolve_identity("", C({{"player_id", ""}}), rnd);
    check(id.cfn == "anon_123456", "an empty player_id is no id");
    // (2.1.0: the Steam fill of config.json, player_id / player_name, is gone: auth section)
}

// ============================================================================================================================
// 2. wire
// ============================================================================================================================
fs::path g_work;

void write_cfg(const fs::path& p, const std::string& base, const std::string& key) {
    json c = json::object();
    c["api_base"] = base; c["api_key"] = key;
    write_all(p, c.dump(2));
}

void test_wire() {
    section("wire: requests as the server sees them");
    Stub stub;
    stub.handler = [](const StubReq& r) {
        StubResp s;
        if (r.target.rfind("/api/list", 0) == 0) s.body = R"({"items":[],"page":1,"per_page":50,"total":0,"has_more":false})";
        else if (r.target.rfind("/api/download/", 0) == 0) s.body = R"({"id":"i","name":"N","char":"Ken","uploader":"U","file":[{"a":1}]})";
        else s.body = R"({"ok":true,"id":"x","version":1,"action":"created"})";
        return s;
    };
    check(stub.start(), "stub starts");
    const fs::path cfgp = g_work / "wire" / "config.json";
    write_cfg(cfgp, stub.base(), "SECRET-KEY");
    Client c;
    c.configure(slash(cfgp), slash(g_work / "wire" / "response.json"));
    c.start();

    Answer a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}, {"sort", "popular"}, {"page", "1"}, {"voter_cfn", "Me"}, {"per_page", "50"}}));
    check(a.done && a.ok, "list answered");
    auto log = stub.take_log();
    check_eq(log.size(), (size_t)1, "one request");
    if (!log.empty()) {
        StubReq r = log[0];
        check_eq(r.method, std::string("GET"), "wire: method");
        check_eq(r.target, std::string("/api/list?char=Ken&sort=popular&page=1&per_page=50"), "wire: target");
        check_eq(r.h["x-client-version"], std::string("2.1.0"), "wire: x-client-version");
        check(!r.h.count("authorization"), "wire: no Authorization without identity.json");
        check_eq(r.h["x-api-key"], std::string("SECRET-KEY"), "wire: x-api-key");
        check_eq(r.h["content-type"], std::string("application/json"), "wire: content-type");
        check_eq(r.h["accept"], std::string("application/json"), "wire: accept");
        check(r.body.empty(), "wire: no body on GET");
    }
    // the answer file the Lua shim reads
    bool ok = false;
    json resp = json::parse(read_all(g_work / "wire" / "response.json", &ok), nullptr, false);
    check(ok && resp.is_object() && resp["ok"] == true && resp["data"]["total"] == 0, "response file is { ok, data }");

    a = ask(c, C({{"cmd", "download"}, {"id", "abc"}}));
    log = stub.take_log();
    check(a.ok && a.data["file"].is_array() && a.data["name"] == "N", "download answer decoded");
    check(!log.empty() && log[0].method == "GET" && log[0].target == "/api/download/abc", "wire: download");

    json vote = C({{"cmd", "vote"}, {"id", "abc"}, {"cfn", "Me"}, {"value", 1}});
    a = ask(c, vote);
    log = stub.take_log();
    check(!log.empty() && log[0].method == "POST" && log[0].target == "/api/vote/abc", "wire: vote route");
    check(!log.empty() && json::parse(log[0].body, nullptr, false) == C({{"value", 1}, {"cfn", "Me"}}), "wire: vote body (v4)");
    check(!log.empty() && log[0].h["content-length"] == std::to_string(log[0].body.size()), "wire: content-length");

    a = ask(c, C({{"cmd", "delete"}, {"id", "abc"}, {"cfn", "Me"}}));
    log = stub.take_log();
    check(!log.empty() && log[0].method == "DELETE" && log[0].target == "/api/delete/abc", "wire: delete");

    // share through a path (what the Lua shim does): the worker reads and embeds the pack
    json pack = json::array({C({{"slot", 1}, {"timeline", "5 + LP"}}), C({{"slot", 2}, {"timeline", "2 + MK"}})});
    write_all(g_work / "wire" / "pack.json", "");
    lj::dump_file(g_work / "wire" / "pack.json", pack, nullptr);
    a = ask(c, C({{"cmd", "share"}, {"char", "Ken"}, {"name", "P"}, {"cfn", "Me"}, {"player", "Me"}, {"description", ""}, {"file_path", slash(g_work / "wire" / "pack.json")}}));
    log = stub.take_log();
    check(a.ok && a.data["action"] == "created", "share answered");
    if (!log.empty()) {
        json body = json::parse(log[0].body, nullptr, false);
        check(log[0].method == "POST" && log[0].target == "/api/share", "wire: share route");
        check(body.is_object() && !body.contains("cmd") && !body.contains("player") && body["cfn"] == "Me" && body["file"].is_array() && body["file"][0]["slot"] == 1,
              "wire: share body (v4) holds the pack");
        check(body["file"].size() == 2 && body["file"][1]["slot"] == 2, "wire: the whole pack goes out");
        check(!body.contains("file_path"), "wire: the path is not sent");
    }
    a = ask(c, C({{"cmd", "share"}, {"char", "Ken"}, {"file_path", slash(g_work / "wire" / "missing.json")}}));
    check(a.done && !a.ok && a.data["error"] == "cannot read the loadout file", "share of an unreadable pack");

    // build errors become answers (they never reach the network)
    a = ask(c, C({{"cmd", "nope"}}));
    check(a.done && !a.ok && a.data["error"] == "unknown cmd" && a.data["ok"] == false, "unknown cmd answered like the bridge");
    write_cfg(cfgp, "", "K");
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && !a.ok && a.data["error"] == "api_base not set in config.json", "api_base missing, config re-read at every request");
    write_cfg(cfgp, stub.base(), "SECRET-KEY");
    write_all(cfgp, "garbage");
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && !a.ok && a.data["error"] == "api_base not set in config.json", "unreadable config.json");
    write_cfg(cfgp, stub.base(), "SECRET-KEY");

    section("wire: server answers that are not the happy path");
    stub.handler = [](const StubReq&) { StubResp s; s.status = 500; s.body = R"({"error":"Internal error","detail":"boom"})"; return s; };
    a = ask(c, C({{"cmd", "download"}, {"id", "x"}}));
    check(a.done && a.ok && a.data["error"] == "Internal error", "HTTP 500 + json = data with error, ok");
    stub.handler = [](const StubReq&) { StubResp s; s.status = 401; s.body = R"({"error":"Unauthorized"})"; return s; };
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && a.ok && a.data["error"] == "Unauthorized", "HTTP 401");
    stub.handler = [](const StubReq&) { StubResp s; s.status = 302; s.extra_headers = "Location: /elsewhere\r\n"; return s; };
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && !a.ok && a.data["error"] == "empty curl response", "a redirect is not followed (curl without -L): empty answer");
    stub.take_log();
    stub.handler = [](const StubReq&) { StubResp s; s.status = 200; s.body = ""; return s; };
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && !a.ok && a.data["error"] == "empty curl response", "200 with no body");
    stub.handler = [](const StubReq&) { StubResp s; s.status = 200; s.body = "<html>nope</html>"; return s; };
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && !a.ok && a.data["error"] == "timeout", "200 with a body that is not json: the 'timeout' text");
    stub.handler = [](const StubReq&) { StubResp s; s.mode = StubResp::Close; return s; };
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}));
    check(a.done && !a.ok && a.data["error"].get<std::string>().rfind("curl exit ", 0) == 0, "connection closed without an answer: curl exit N (" + (a.data.contains("error") ? a.data["error"].get<std::string>() : "?") + ")");

    section("wire: busy, refused");
    stub.handler = [](const StubReq&) { StubResp s; s.body = R"({"items":[]})"; return s; };
    {
        Answer first, second;
        c.send(C({{"cmd", "list"}, {"char", "Ken"}}), [&](bool ok, const json& d) { first.done = true; first.ok = ok; first.data = d; });
        check(c.busy(), "busy while a request is in flight");
        const bool sent = c.send(C({{"cmd", "list"}, {"char", "Ken"}}), [&](bool ok, const json& d) { second.done = true; second.ok = ok; second.data = d; });
        check(!sent && second.done && !second.ok && second.data["error"] == "busy", "second send: { error = busy } at once");
        for (int i = 0; i < 2000 && !first.done; ++i) { c.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        check(first.done && first.ok && !c.busy(), "first answered, not busy any more");
    }
    {   // a real Worker answers { "items": [] } for a character nobody shared: an empty (truthy) table, "0 loadouts"
        std::vector<std::string> tk;
        Page::Env env;
        env.root = slash(g_work / "wire" / "game") + "/";
        env.current_char = [] { return std::string("Ken"); };
        env.ticker = [&](const std::string& m) { tk.push_back(m); };
        env.identity = [] { return Identity{ "Me", "Me" }; };
        Page pg(c, env);
        pg.open();
        for (int i = 0; i < 500 && c.busy(); ++i) { c.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        check_eq(tk, std::vector<std::string>{ "Community: 0 loadouts" }, "Page: { items = [] } is '0 loadouts', not a failure");
        check(pg.rows().empty() && pg.title() == "New  Ken Loadouts\n<ICON KeyA>   Page 1/1   <ICON KeyE>", "Page: empty list, title");
    }
    stub.stop();
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}), 500, 17);
    check(a.done && !a.ok && a.data["error"] == "curl exit 7" && a.data["ok"] == false, "server off: curl exit 7 (connection refused)");

    section("wire: timeouts (real time, about 17 s)");
    Stub hang;
    hang.handler = [](const StubReq&) { StubResp s; s.mode = StubResp::Hang; return s; };
    check(hang.start(), "hang stub starts");
    write_cfg(cfgp, hang.base(), "K");
    const auto t0 = std::chrono::steady_clock::now();
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}), 3000, 17);          // ~60 polls per second
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(a.done && !a.ok && a.data["error"] == "timeout", "no answer: { error = timeout } after 600 polls");
    check(secs > 9.0 && secs < 12.5, "...which is about 10 s at 60 polls per second (" + std::to_string(secs) + " s)");
    check(!c.busy(), "free again after the timeout");
    // the abandoned request is still hanging in the worker; the next one waits behind it and the 15 s watchdog frees the worker
    hang.handler = [](const StubReq&) { StubResp s; s.body = R"({"items":[],"total":0})"; return s; };
    const auto t1 = std::chrono::steady_clock::now();
    a = ask(c, C({{"cmd", "list"}, {"char", "Ken"}}), 3000, 17);
    const double secs2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    check(a.done && a.ok && a.data["total"] == 0, "after the watchdog killed the hung request, the next one is served");
    check(secs2 > 3.0 && secs2 < 8.5, "...about 5 s later, at the 15 s mark of the first (" + std::to_string(secs2) + " s)");
    hang.stop();
    c.stop();
    const Client::Counters k = c.counters();
    check(k.timeouts == 1 && k.busy_refusals == 1, "counters: 1 timeout, 1 busy refusal");
}

// ============================================================================================================================
// helpers: list items and request lines
// ============================================================================================================================
json item(const std::string& id, const std::string& name, const std::string& by, int version, int score, int dls, int slots,
          const std::string& created, const std::string& updated, bool my_vote) {
    json i = C({{"id", id}, {"char_name", "Ken"}, {"display_name", name}, {"uploader_name", by}, {"uploader_cfn", by}, {"created_at", created},
                {"updated_at", updated}, {"downloads", dls}, {"score", score}, {"slot_count", slots}, {"description", ""}, {"version", version}});
    if (my_vote) i["my_vote"] = 1;
    return i;
}

// request lines: cmd|...
std::string wire_line(const StubReq& r) {
    const std::string& t = r.target;
    auto qp = [&](const std::string& key) {
        const size_t q = t.find('?');
        const std::string qs = q == std::string::npos ? "" : t.substr(q + 1);
        std::istringstream ss(qs);
        std::string kv;
        while (std::getline(ss, kv, '&')) {
            const size_t e = kv.find('=');
            if (kv.substr(0, e) == key) return e == std::string::npos ? std::string() : kv.substr(e + 1);
        }
        return std::string();
    };
    if (t.rfind("/api/list", 0) == 0) return "list|" + qp("char") + "|" + qp("sort") + "|" + qp("page") + "|" + qp("voter_cfn") + "|" + qp("per_page");
    if (t.rfind("/api/download/", 0) == 0) return "download|" + t.substr(14);
    json b = json::parse(r.body, nullptr, false);
    if (t.rfind("/api/vote/", 0) == 0) return "vote|" + t.substr(10) + "|" + b.value("cfn", "") + "|" + b["value"].dump();
    if (t.rfind("/api/share", 0) == 0) return "share|" + b.value("char", "") + "|" + b.value("name", "") + "|" + b.value("cfn", "") + "|" + b.value("player", "") + "|" + b.value("description", "");
    return "?|" + t;
}

// ============================================================================================================================
// 3. The four Community fixes (of 2.0.0's defects): refused shares, answers without their fields, vote toggles, identity re-read
// ============================================================================================================================
void test_fixes() {
    section("fixes: pure verdicts on the Worker's answers");
    {
        {   // busy / Steam-verification messages: pure decisions
            check_eq(busy_verdict(true).en, "Steam verification in progress, try again in a moment", "busy while verifying: the Steam text");
            check_eq(busy_verdict(true).key, "com_busy_verifying", "busy while verifying: its locale key");
            check_eq(busy_verdict(false).en, "Community: please wait, a request is in progress", "busy otherwise: the wait text");
            check_eq(busy_verdict(false).key, "com_busy", "busy otherwise: its locale key");
            check_eq(verifying_notice().en, "Verifying with Steam...", "the verifying ticker text");
            bool an = false;
            check(!verify_notice(false, true, &an), "verify_notice: nothing in flight, nothing said");
            check(!verify_notice(true, false, &an) && !an, "verify_notice: in flight, no Steam wait yet");
            check(verify_notice(true, true, &an) && an, "verify_notice: the Steam wait starts: said");
            check(!verify_notice(true, true, &an), "verify_notice: said once per request");
            check(!verify_notice(true, false, &an) && !verify_notice(true, true, &an), "verify_notice: still once after the wait blinks off/on");
            check(!verify_notice(false, false, &an) && !an, "verify_notice: request done, flag reset");
            check(verify_notice(true, true, &an), "verify_notice: the next request says it again");
        }
        {   // the hook is wired by Page and cleared by its destructor
            Client c2;
            int n = 0;
            c2.set_verifying_hook([&] { ++n; });
            check(!c2.busy() && !c2.verifying(), "client: idle, not verifying");
            c2.set_verifying_hook(nullptr);
            c2.poll();
            check(n == 0, "client: no request, hook not called");
        }
        Msg m = share_verdict(true, C({{"ok", true}, {"id", "x"}, {"version", 1}, {"action", "created"}}), "P");
        check(m.en.empty(), "share: { ok = true } is shared");
        m = share_verdict(true, C({{"error", "No changes: content is identical to v1"}}), "P");
        check(m.key == "com_share_dup" && m.en == "Already shared: nothing changed", "share 409: Already shared");
        m = share_verdict(true, C({{"error", "File too large (max 1 MB)"}}), "P");
        check(m.key == "com_share_big" && m.en == "Loadout too large to share", "share 413: too large");
        m = share_verdict(true, C({{"error", "Upload limit reached (20 per day)"}}), "P");
        check(m.key == "com_share_limit" && m.en == "Share limit reached, try again later", "share 429: limit");
        m = share_verdict(true, C({{"error", "Missing required fields: char, name, cfn, player, file"}}), "P");
        check(m.key == "com_share_refused" && m.arg.rfind("Missing required", 0) == 0 && m.en.rfind("Share refused: Missing", 0) == 0, "share 400: Share refused: <reason>");
        m = share_verdict(true, C({{"error", "Unauthorized"}}), "P");
        check(m.en == "Share refused: Unauthorized", "share 401: refused with the reason");
        m = share_verdict(true, json::object(), "P");
        check(m.key == "com_share_bad" && !m.en.empty(), "share: ok and nothing else is not 'shared'");
        m = share_verdict(true, json::array(), "P");
        check(m.key == "com_share_bad", "share: an answer that is not an object");
        m = share_verdict(false, C({{"error", "curl exit 7"}}), "P");
        check(m.en == "Share failed: curl exit 7", "share: network failure text kept");

        check(download_verdict(true, C({{"file", json::array()}, {"name", "n"}})).en.empty(), "download: file present");
        Msg d = download_verdict(true, json::object());
        check(d.key == "com_dl_nofile" && d.en == "Download failed: the server sent no file", "download: no error and no file");
        d = download_verdict(true, json::array({1, 2}));
        check(d.key == "com_dl_nofile", "download: an answer that is not an object");
        check(download_verdict(true, C({{"error", "Not found"}})).en == "Download failed: Not found", "download: server error kept");
        check(download_verdict(false, C({{"error", "timeout"}})).en == "Download failed: timeout", "download: network error kept");

        check(list_verdict(true, C({{"items", json::array()}})).en.empty(), "list: items present (empty is fine)");
        Msg l = list_verdict(true, C({{"total", 0}}));
        check(l.key == "com_list_bad" && l.en == "Community: the server sent no list", "list: no items, no error");
        check(list_verdict(true, C({{"error", "Unauthorized"}})).en == "Community: Unauthorized", "list: server error kept");
        check(list_verdict(false, json()).en == "Community: failed", "list: failed request without text");

        VoteEffect v = vote_effect(true, C({{"ok", true}, {"action", "voted"}, {"new_value", 1}}), 1);
        check(v.valid && v.delta == 1 && v.my_vote == 1, "vote voted: +1, liked");
        v = vote_effect(true, C({{"ok", true}, {"action", "removed"}, {"new_value", 0}}), 1);
        check(v.valid && v.delta == -1 && v.my_vote == 0, "vote removed: -1, not liked");
        v = vote_effect(true, C({{"ok", true}, {"action", "flipped"}, {"new_value", 1}}), 1);
        check(v.valid && v.delta == 2 && v.my_vote == 1, "vote flipped from -1 to +1: +2, liked");
        v = vote_effect(true, C({{"ok", true}, {"action", "flipped"}, {"new_value", -1}}), -1);
        check(v.valid && v.delta == -2 && v.my_vote == -1, "vote flipped to -1: -2");
        v = vote_effect(true, C({{"ok", true}}), 1);
        check(!v.valid && v.err.key == "com_vote_bad", "vote: ok without action");
        v = vote_effect(true, C({{"ok", true}, {"action", "banana"}}), 1);
        check(!v.valid && v.err.key == "com_vote_bad", "vote: unknown action");
        v = vote_effect(true, C({{"ok", false}}), 1);
        check(!v.valid && v.err.key == "com_vote_bad", "vote: ok false without error");
        v = vote_effect(true, C({{"error", "Not found"}}), 1);
        check(!v.valid && v.err.en == "Vote failed: Not found", "vote: server error kept");
        v = vote_effect(false, C({{"error", "timeout"}}), 1);
        check(!v.valid && v.err.en == "Vote failed: timeout", "vote: network error kept");
    }

    section("fixes: identity re-read");
    {
        IdentitySession s;
        int calls = 0;
        auto rnd = [&] { ++calls; return 555555; };
        const json cfg = C({{"player_id", "7656"}});
        Identity id = resolve_identity_session(s, "", cfg, rnd);
        check(id.cfn == "7656", "no banner yet: config player_id");
        id = resolve_identity_session(s, "PlayerOne", cfg, rnd);
        check(id.cfn == "PlayerOne" && id.name == "PlayerOne", "the banner is read later: the CFN replaces the fallback");
        id = resolve_identity_session(s, "Other", cfg, rnd);
        check(id.cfn == "Other", "the CFN changed: the new one is used");
        id = resolve_identity_session(s, "", cfg, rnd);
        check(id.cfn == "Other", "banner unreadable now: the last CFN read this session");
        id = resolve_identity_session(s, "", C({{"player_id", "9999"}}), rnd);
        check(id.cfn == "Other", "...which wins over the config id");
        IdentitySession t;
        id = resolve_identity_session(t, "", json(), rnd);
        const std::string a1 = id.cfn;
        id = resolve_identity_session(t, "", json(), rnd);
        check(a1 == "anon_555555" && id.cfn == a1 && calls == 1, "anon_ number stable all session (one random draw)");
        id = resolve_identity_session(t, "", C({{"player_id", "7"}}), rnd);
        check(id.cfn == "7", "config now has an id: it replaces anon_");
    }

    section("fixes: Page against a stub Worker");
    {
        const fs::path root = g_work / "fixes";
        fs::remove_all(root);
        fs::create_directories(root / "game/SF6_Community_data");
        fs::create_directories(root / "game/SF6_RecordingSlotManager_data/Ken");
        lj::dump_file(root / "game/SF6_Community_data/com_state.json", C({{"last_id", ""}, {"downloaded", C({{"Ken_Test.json", 1}})}}), nullptr);
        lj::dump_file(root / "game/SF6_RecordingSlotManager_data/Ken/Mine.json", json::array({ C({{"slot", 1}}) }), nullptr);
        Stub stub;
        std::string body;
        stub.handler = [&](const StubReq&) { StubResp s; s.body = body; return s; };
        check(stub.start(), "fixes: stub starts");
        write_cfg(root / "config.json", stub.base(), "K");
        Client c;
        c.configure(slash(root / "config.json"));
        c.start();
        std::vector<std::string> tk;
        std::string cfn = "First";
        bool translate = false;
        Page::Env env;
        env.root = slash(root / "game") + "/";
        env.current_char = [] { return std::string("Ken"); };
        env.ticker = [&](const std::string& m) { tk.push_back(m); };
        env.identity = [&] { return Identity{ cfn, cfn }; };
        env.tr = [&](const std::string& key, const std::vector<std::string>& args) {
            if (!translate) return key;                                  // the locale has no such key: English fallback
            return "[fr] " + key + (args.empty() ? std::string() : " " + args[0]);
        };
        Page pg(c, env);
        pg.load_state();
        auto pump = [&] { for (int i = 0; i < 3000 && c.busy(); ++i) { c.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } };
        auto score = [&] { return pg.all_items()[0]["score"].get<int>(); };
        auto liked = [&] { return pg.all_items()[0].contains("my_vote"); };
        const std::string list_body = json::object({ {"items", json::array({
            item("id-1", "Ken_Test", "Sheldon", 1, 2, 3, 10, "2026-09-12 16:07:06", "", false),
            item("id-2", "Fresh", "Alice", 1, 0, 0, 10, "2026-09-13 16:07:06", "", false) })} }).dump();

        body = list_body;
        pg.fetch(1); pump();
        check(score() == 2 && !liked(), "fixes: list loaded, score 2, not liked");
        {
            const auto lg = stub.take_log();
            check(lg.size() == 1 && wire_line(lg[0]) == "list|Ken|recent|1||50", "list request: no voter_cfn any more (v4: my_vote comes from the token)");
        }

        // 4: a share refused by the Worker
        auto share_with = [&](const std::string& answer) { tk.clear(); body = answer; pg.share("Ken", "Mine.json", "Mine"); pump(); return tk; };
        check_eq(share_with(R"({"ok":true,"id":"a","version":1,"action":"created"})"), std::vector<std::string>{ "Sharing loadout...", "Loadout shared: Mine" }, "share ok: Loadout shared");
        check_eq(share_with(R"({"error":"No changes: content is identical to v1"})"), std::vector<std::string>{ "Sharing loadout...", "Already shared: nothing changed" }, "share 409: no 'Loadout shared'");
        check_eq(share_with(R"j({"error":"File too large (max 1 MB)"})j"), std::vector<std::string>{ "Sharing loadout...", "Loadout too large to share" }, "share 413");
        check_eq(share_with(R"j({"error":"Upload limit reached (20 per day)"})j"), std::vector<std::string>{ "Sharing loadout...", "Share limit reached, try again later" }, "share 429");
        check_eq(share_with(R"({"error":"cfn and value are bad"})"), std::vector<std::string>{ "Sharing loadout...", "Share refused: cfn and value are bad" }, "share 400: Share refused: <reason>");
        check_eq(share_with(R"({})"), std::vector<std::string>{ "Sharing loadout...", "Share failed: unexpected server answer" }, "share: empty answer");
        translate = true;
        check_eq(share_with(R"({"error":"No changes: x"})"), std::vector<std::string>{ "Sharing loadout...", "[fr] com_share_dup" }, "share 409 through the locale");
        check_eq(share_with(R"({"error":"nope"})"), std::vector<std::string>{ "Sharing loadout...", "[fr] com_share_refused nope" }, "share refused through the locale, %s = the reason");
        translate = false;

        // 5: a download answer without error and without file; a list answer without items
        std::string msg;
        check(pg.download_prompt(2, &msg), "fixes: download prompt on a row not downloaded");
        tk.clear(); body = "{}";
        pg.download_confirmed(); pump();
        check_eq(tk, std::vector<std::string>{ "Downloading...", "Download failed: the server sent no file" }, "download: no error, no file -> a clear message");
        check(!fs::exists(root / "game/SF6_RecordingSlotManager_data/Ken/Fresh.json"), "...and nothing written");
        tk.clear(); body = R"({"total":0})";
        pg.fetch(1); pump();
        check_eq(tk, std::vector<std::string>{ "Community: the server sent no list" }, "list: no items -> a clear message");
        check(score() == 2, "...and the previous list is kept");
        translate = true;
        tk.clear(); body = "{}";
        check(pg.download_prompt(2, &msg), "fixes: prompt again");
        pg.download_confirmed(); pump();
        check_eq(tk, std::vector<std::string>{ "Downloading...", "[fr] com_dl_nofile" }, "download: message through the locale");
        translate = false;

        // 6: vote toggles follow the answer
        auto vote_with = [&](const std::string& answer) { tk.clear(); body = answer; pg.vote(1); pump(); return tk; };
        check_eq(vote_with(R"({"ok":true,"action":"voted","new_value":1})"), std::vector<std::string>{ "Liked" }, "vote voted: Liked");
        check(score() == 3 && liked(), "...score 3, liked");
        check_eq(vote_with(R"({"ok":true,"action":"removed","new_value":0})"), std::vector<std::string>{ "Unliked" }, "vote removed: Unliked");
        check(score() == 2 && !liked(), "...score back to 2, not liked");
        check_eq(vote_with(R"({"ok":true,"action":"flipped","new_value":1})"), std::vector<std::string>{ "Liked" }, "vote flipped (from an old dislike): Liked");
        check(score() == 4 && liked(), "...score +2 (a swing of 2), liked, counted once");
        check_eq(vote_with(R"({"ok":true,"action":"removed","new_value":0})"), std::vector<std::string>{ "Unliked" }, "then toggled off");
        check(score() == 3 && !liked(), "...score -1, not liked");
        check_eq(vote_with(R"({"ok":true})"), std::vector<std::string>{ "Vote failed: unexpected server answer" }, "vote: ok without action is an error");
        check(score() == 3 && !liked(), "...nothing counted");
        check_eq(vote_with(R"({"error":"Not found"})"), std::vector<std::string>{ "Vote failed: Not found" }, "vote: server error");

        // 7: the identity is asked again at each list / vote / share
        cfn = "Second";
        stub.take_log();
        body = list_body; pg.fetch(1); pump();
        body = R"({"ok":true,"action":"voted","new_value":1})"; pg.vote(1); pump();
        body = R"({"ok":true})"; pg.share("Ken", "Mine.json", "Mine"); pump();
        const auto lg = stub.take_log();
        check(lg.size() == 3, "fixes: three requests after the CFN changed");
        if (lg.size() == 3) {
            check(wire_line(lg[0]) == "list|Ken|recent|1||50", "list: no voter_cfn");
            check(wire_line(lg[1]).find("|Second|1") != std::string::npos, "vote: cfn is the CFN read now");
            check(wire_line(lg[2]).find("|Second||") != std::string::npos, "share: cfn is the CFN read now, no player");
        }

        // 8: a download writes the loadout's description and author on entry 1 (info, docs/PACK_FORMAT.md)
        body = list_body; pg.fetch(1); pump();
        check(pg.download_prompt(2, &msg), "info: prompt on Fresh");
        const std::string drill = "Pick Ken and throw him. He will auto break it. Then, STOP THE SLOP";
        body = json::object({ {"name", "Fresh"}, {"description", drill},
                              {"file", json::array({ C({{"id", 2}, {"empty", true}}), C({{"id", 1}, {"empty", true}, {"info", C({{"url", "u"}})}}) })} }).dump();
        pg.download_confirmed(); pump();
        const json got = lj::load_file(root / "game/SF6_RecordingSlotManager_data/Ken/Fresh.json");
        check(got.is_array() && got.size() == 2, "info: the pack written, its 2 entries");
        if (got.is_array() && got.size() == 2) {
            const json& in = got[1]["info"];
            check(in.is_object() && in["description"] == drill && in["author"] == "Alice" && in["source"] == "community" && in["url"] == "u",
                  "info: description, author, source on entry 1; its other keys kept");
            check(!got[0].contains("info"), "info: only on entry 1");
        }
        // 9: Share sends the pack's own description, cut to the Worker's 1000 characters
        lj::dump_file(root / "game/SF6_RecordingSlotManager_data/Ken/Drill.json",
                      json::array({ C({{"id", 1}, {"empty", true}, {"info", C({{"description", std::string(1200, 'x')}})}}) }), nullptr);
        stub.take_log();
        body = R"({"ok":true})"; pg.share("Ken", "Drill.json", "Drill"); pump();
        {
            const auto sl = stub.take_log();
            const json sb = sl.size() == 1 ? json::parse(sl[0].body, nullptr, false) : json();
            check(sb.is_object() && sb.value("description", std::string()) == std::string(1000, 'x'), "share: the pack's description, 1000 characters");
        }
        c.stop();
        stub.stop();
    }
}


// ============================================================================================================================
// 4. The Steam-verified identity (2.1.0, ADR-0018 method A)
// ============================================================================================================================
const std::string kTok1 = std::string(43, 'A'), kTok2 = std::string(42, 'B') + "_", kTok3 = std::string(43, 'c');

// The Worker thread's ticket source in the flow tests (the Steam engine has its own tests below, on a fake steam_api)
class FakeTickets final : public TicketSource {
public:
    std::atomic<bool> fail{false};
    std::atomic<int> delay_ms{0}, gets{0};
    std::mutex m;
    std::vector<uint32_t> released;
    std::vector<uint8_t> bytes;
    uint32_t next = 0x100;
    Ticket get(int) override {
        if (delay_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
        ++gets;
        Ticket t;
        if (fail.load()) { t.why = "fake: no Steam"; return t; }
        std::lock_guard<std::mutex> lk(m);
        t.ok = true;
        t.handle = ++next;
        t.bytes = bytes;
        return t;
    }
    void release(uint32_t h) override { std::lock_guard<std::mutex> lk(m); released.push_back(h); }
    size_t released_count() { std::lock_guard<std::mutex> lk(m); return released.size(); }
};

json answer_of(int status, const std::string& body) {
    HttpResult r;
    r.sent = true;
    r.status = status;
    r.body = body;
    return make_reply("x", r).data;
}

void test_auth_pure() {
    section("auth: pure parts");
    const uint8_t b[] = { 0x14, 0x00, 0xab, 0xff };
    check_eq(auth::hex_encode(b, 4), std::string("1400abff"), "ticket hex, lower case");
    check_eq(auth::ticket_brief(b, 4), std::string("4 bytes, starts 1400"), "ticket brief: its length and 4 hex digits, never more");
    check_eq(auth::ticket_brief(b, 0), std::string("0 bytes, starts -"), "brief of nothing");

    check(auth::public_cfn("PlayerOne") && auth::public_cfn("  Ryu Fan ") && auth::public_cfn("1234"), "CFN names go out");
    check(!auth::public_cfn("76561198012345678") && !auth::public_cfn("12345"), "a SteamID64 (any 5+ digits) never goes out as a CFN");
    check(!auth::public_cfn("anon_123456") && !auth::public_cfn("ANON_1") && !auth::public_cfn("local_42") && !auth::public_cfn("anon_"), "anon_ / local_ fallbacks stay local");
    check(auth::public_cfn("anon_x"), "anon_ + letters: a name (the Worker's rule)");
    check(!auth::public_cfn("steam:1") && !auth::public_cfn("V4:abc"), "steam: / v4: keys");
    check(!auth::public_cfn("") && !auth::public_cfn("   ") && !auth::public_cfn("a\tb"), "empty, blank, control character");
    std::string jp32;
    for (int i = 0; i < 32; ++i) jp32 += "\xE3\x83\xAA";
    check(auth::public_cfn(std::string(32, 'x')) && !auth::public_cfn(std::string(33, 'x')), "32 characters at most");
    check(auth::public_cfn(jp32) && !auth::public_cfn(jp32 + "\xE3\x83\xAA"), "32 UTF-8 characters (96 bytes) at most");

    check(auth::token_ok(kTok1) && auth::token_ok(kTok2) && !auth::token_ok(kTok1 + "A") && !auth::token_ok(std::string(42, 'A') + "="), "token shape (43 base64url)");
    check(auth::needs_token("share") && auth::needs_token("vote") && auth::needs_token("delete"), "writes need the token");
    check(!auth::needs_token("list") && !auth::needs_token("download") && !auth::needs_token("ticket"), "reads and the ticket do not");
    check_eq(auth::token_path("a/SF6_Community_data/config.json"), std::string("a/SF6_Community_data/identity.json"), "identity.json next to config.json");

    const auth::Token tk{ kTok1, "http://x", "Alpha", "ticket", "2026-10-14 12:00:00", "2.1.0" };
    const json tj = auth::token_json(tk);
    check(tj.size() == 6 && tj["token"] == kTok1 && tj["api_base"] == "http://x" && tj["cfn"] == "Alpha" && tj["method"] == "ticket",
          "identity.json: token, api_base, cfn, method, linked_at, client_version (no Steam id)");
    auth::Token back;
    check(auth::token_from_json(tj, &back) && back.token == kTok1 && back.linked_at == tk.linked_at, "identity.json read back");
    check(!auth::token_from_json(C({{"token", "short"}}), &back) && !auth::token_from_json(json::array(), &back), "a malformed token is no token");
    const fs::path tp = g_work / "auth_pure" / "identity.json";
    fs::remove_all(tp.parent_path());
    check(auth::load_token(slash(tp), "http://x").empty(), "no file: no token");
    std::string err;
    check(auth::save_token(slash(tp), tk, &err), "token saved");
    check_eq(auth::load_token(slash(tp), "http://x"), kTok1, "token loaded for its api_base");
    check(auth::load_token(slash(tp), "https://sf6-community.example").empty(), "another api_base: no token (a local Worker's token never goes to the real one)");
    write_all(tp, "garbage");
    check(auth::load_token(slash(tp), "http://x").empty(), "unreadable identity.json: no token");
    check(auth::drop_token(slash(tp)) && !fs::exists(tp), "identity.json deleted");

    check_eq(auth::request_headers("K", "", ""), std::string("Content-Type: application/json\r\nAccept: application/json\r\nx-api-key: K"), "headers as 2.0.0");
    check_eq(auth::request_headers("", kTok1, "2.1.0"), "Content-Type: application/json\r\nAccept: application/json\r\nx-client-version: 2.1.0\r\nAuthorization: Bearer " + kTok1,
             "headers: x-client-version and the Bearer token");

    section("auth: answers and verdicts");
    json d = answer_of(401, R"({"error":"invalid_token","message":"Your Steam link is no longer valid: link your Steam account again."})");
    check(d["error"] == "Your Steam link is no longer valid: link your Steam account again." && d["error_code"] == "invalid_token", "v4 refusal: error = the message (what callbacks print), error_code = the code");
    check_eq(auth::error_code(d), std::string("invalid_token"), "error_code");
    check_eq(auth::error_code(C({{"error", "Not found"}})), std::string("Not found"), "an old-style error is its own code");
    check(auth::error_code(json::array()).empty() && auth::error_code(C({{"ok", true}})).empty(), "no error, no code");

    HttpResult r;
    r.sent = true;
    r.status = 200;
    r.body = R"({"ok":true,"status":"linked","token":")" + kTok1 + R"(","verified":true,"method":"ticket","cfn":"Alpha"})";
    auth::TicketAnswer ta = auth::ticket_answer(make_reply("ticket", r));
    check(ta.ok && ta.token == kTok1 && ta.cfn == "Alpha", "ticket answer: the token");
    r.status = 401; r.body = R"({"error":"invalid_ticket","message":"Steam refused the ticket."})";
    ta = auth::ticket_answer(make_reply("ticket", r));
    check(!ta.ok && ta.code == "invalid_ticket" && ta.message == "Steam refused the ticket." && auth::refusal_code(ta) == auth::kUnavailable, "ticket refused: verification unavailable");
    r.status = 403; r.body = R"({"error":"steam_banned","message":"This Steam account is banned by the publisher."})";
    check(auth::refusal_code(auth::ticket_answer(make_reply("ticket", r))) == "banned", "publisher ban: banned");
    r.status = 403; r.body = R"({"error":"banned","message":"This Steam account is banned from SF6 Community."})";
    check(auth::refusal_code(auth::ticket_answer(make_reply("ticket", r))) == "banned", "Community ban: banned");
    r.status = 429; r.body = R"({"error":"rate_limited","message":"Too many requests: try again later.","retry_after":30})";
    check(auth::refusal_code(auth::ticket_answer(make_reply("ticket", r))) == "rate_limited", "rate limited");
    r.status = 200; r.body = R"({"ok":true,"token":"short"})";
    ta = auth::ticket_answer(make_reply("ticket", r));
    check(!ta.ok && ta.code == "bad_answer", "a malformed token is refused");
    HttpResult f;
    f.sent = false; f.curl_code = 7;
    ta = auth::ticket_answer(make_reply("ticket", f));
    check(!ta.ok && auth::refusal_code(ta) == auth::kUnavailable, "network failure: verification unavailable");
    const Reply rr = auth::refused_reply(auth::kUnavailable);
    check(rr.ok && rr.data["ok"] == false && rr.data["error_code"] == auth::kUnavailable &&
          rr.data["error"].get<std::string>().rfind("Steam verification unavailable", 0) == 0, "the answer of a write never sent");

    Config cfg{ "https://example.test", "K", "2.1.0" };
    Request rq;
    check(build_request(C({{"cmd", "ticket"}, {"ticket", "abcd"}, {"cfn", "Alpha"}}), cfg, &rq, &err), "ticket builds");
    check(rq.method == "POST" && rq.url == "https://example.test/auth/steam/ticket" && json::parse(rq.body) == C({{"ticket", "abcd"}, {"cfn", "Alpha"}, {"client_version", "2.1.0"}}),
          "ticket: POST /auth/steam/ticket { ticket, cfn, client_version }");
    check(build_request(C({{"cmd", "ticket"}, {"ticket", "abcd"}, {"cfn", "76561198012345678"}}), cfg, &rq, &err) && !json::parse(rq.body).contains("cfn"), "ticket: a SteamID is never sent as the CFN");
    check(!build_request(C({{"cmd", "ticket"}}), cfg, &rq, &err) && err == "no ticket", "ticket command without a ticket");
    check(build_request(C({{"cmd", "vote"}, {"id", "x"}, {"cfn", "anon_123456"}, {"value", 1}}), cfg, &rq, &err) && json::parse(rq.body) == C({{"value", 1}}), "vote: anon_ is not sent");
    check(build_request(C({{"cmd", "share"}, {"char", "Ken"}, {"name", "P"}, {"cfn", "local_77"}, {"player", "Player local_77"}, {"description", ""}, {"file", "[]"}}), cfg, &rq, &err) &&
          json::parse(rq.body) == C({{"char", "Ken"}, {"name", "P"}, {"description", ""}, {"file", "[]"}}), "share: no player, no id as the CFN");

    const std::string m426 = R"({"error":"update_required","message":"Update SF6 Recording Manager to 2.1.0 to share or like."})";
    check(share_verdict(true, rr.data, "P").key == "com_auth_unavailable", "share never sent: Steam verification unavailable");
    check(share_verdict(true, answer_of(426, m426), "P").key == "com_auth_update", "share 426: update required");
    check(share_verdict(true, answer_of(401, R"({"error":"invalid_token","message":"x"})"), "P").key == "com_auth_invalid", "share 401 invalid_token");
    check(share_verdict(true, answer_of(403, R"({"error":"banned","message":"x"})"), "P").key == "com_auth_banned", "share 403 banned");
    check(share_verdict(true, answer_of(429, R"({"error":"rate_limited","message":"Too many requests: try again later.","retry_after":9})"), "P").key == "com_rate_limited", "share 429");
    check(share_verdict(true, answer_of(409, R"({"error":"no_changes","message":"No changes: content is identical to v2"})"), "P").key == "com_share_dup", "share 409 no_changes");
    check(share_verdict(true, answer_of(413, R"j({"error":"too_large","message":"File too large (max 1 MB)"})j"), "P").key == "com_share_big", "share 413 too_large");
    const Msg other = share_verdict(true, answer_of(400, R"({"error":"bad_request","message":"Required: char"})"), "P");
    check(other.key == "com_share_refused" && other.arg == "Required: char", "share 400: refused with the server's message");
    check(share_verdict(true, C({{"ok", true}, {"id", "i"}, {"version", 1}, {"action", "created"}}), "P").en.empty(), "share ok: shared");
    check(auth_verdict(answer_of(403, R"({"error":"not_owner","message":"Only its author can delete this loadout."})")).key == "com_not_owner", "delete 403 not_owner");
    check(auth_verdict(C({{"error", "Not found"}})).en.empty(), "not an identity refusal");
    check(vote_effect(true, rr.data, 1).err.key == "com_auth_unavailable" && !vote_effect(true, rr.data, 1).valid, "vote never sent: Steam verification unavailable");
    check(vote_effect(true, answer_of(426, m426), 1).err.key == "com_auth_update", "vote 426");
    check(vote_effect(true, answer_of(403, R"({"error":"banned","message":"x"})"), 1).err.key == "com_auth_banned", "vote 403 banned");
    check(vote_effect(true, C({{"ok", true}, {"action", "voted"}, {"new_value", 1}, {"score", 3}}), 1).valid, "vote ok");

    check(item_author(C({{"author_cfn", "Alpha"}, {"uploader_name", "Old"}, {"author_verified", true}})) == "Alpha" &&
          item_verified(C({{"author_cfn", "Alpha"}, {"author_verified", true}})), "v4 item: author_cfn, verified");
    check(item_author(C({{"uploader_name", "Bob"}})) == "Bob" && !item_verified(C({{"uploader_name", "Bob"}})), "2.0.0 item: uploader_name, not verified");
    check(item_author(json::object()) == "?" && !item_verified(C({{"author_verified", "true"}})), "no author: ?; a string is not the flag");

    const json p1 = json::array({ C({{"id", 1}, {"empty", true}, {"info", C({{"description", "Mine"}, {"source", "drillcodes.com"}})}}) });
    const json w1 = with_info(p1, "", "Bob");
    check(w1[0]["info"]["description"] == "Mine" && w1[0]["info"]["author"] == "Bob" && w1[0]["info"]["source"] == "community", "info: no description from the server keeps the pack's");
    check(with_info(p1, "New", "?")[0]["info"]["description"] == "New" && !with_info(json::array({ C({{"id", 1}}) }), "", "?")[0]["info"].contains("author"), "info: the server's description wins; no author when unknown");
    check(with_info(json::array({ C({{"id", 2}}) }), "D", "A") == json::array({ C({{"id", 2}}) }) && with_info(C({{"id", 1}}), "D", "A") == C({{"id", 1}}), "info: no entry 1 / not an array: unchanged");
}

void test_auth_flow() {
    section("auth: the flow against a stub Worker v4");
    Stub stub;
    std::mutex sm;
    std::set<std::string> valid;                 // tokens the stub accepts
    std::string next_token = kTok1;              // the token /auth/steam/ticket hands out
    bool accept_new = true;                      // ...and accepts afterwards
    std::string ticket_refusal;                  // non-empty: /auth/steam/ticket answers 403 with it
    stub.handler = [&](const StubReq& r) {
        StubResp s;
        std::lock_guard<std::mutex> lk(sm);
        const auto a = r.h.find("authorization");
        const std::string bearer = (a == r.h.end() || a->second.size() < 7) ? std::string() : a->second.substr(7);
        if (r.target == "/auth/steam/ticket") {
            if (!ticket_refusal.empty()) { s.status = 403; s.body = ticket_refusal; return s; }
            if (accept_new) valid.insert(next_token);
            s.body = C({{"ok", true}, {"status", "linked"}, {"token", next_token}, {"verified", true}, {"method", "ticket"}, {"cfn", "Alpha"}}).dump();
            return s;
        }
        if (r.target.rfind("/api/list", 0) == 0) { s.body = R"({"items":[],"total":0})"; return s; }
        if (r.target.rfind("/api/download/", 0) == 0) { s.body = R"({"id":"x","name":"N","file":[]})"; return s; }
        if (bearer.empty()) { s.status = 426; s.body = R"({"error":"update_required","message":"Update SF6 Recording Manager to 2.1.0 to share or like."})"; return s; }
        if (!valid.count(bearer)) { s.status = 401; s.body = R"({"error":"invalid_token","message":"Your Steam link is no longer valid: link your Steam account again."})"; return s; }
        s.body = r.method == "DELETE" ? std::string(R"({"ok":true})") : std::string(R"({"ok":true,"action":"voted","new_value":1,"score":1})");
        return s;
    };
    check(stub.start(), "auth stub starts");
    const fs::path dir = g_work / "auth_flow";
    fs::remove_all(dir);
    const fs::path cfgp = dir / "config.json", tp = dir / "identity.json";
    write_cfg(cfgp, stub.base(), "K");
    FakeTickets tickets;
    tickets.bytes = { 0x14, 0x00, 0x00, 0x00, 0xde, 0xad, 0xbe, 0xef };
    Client c;
    c.configure(slash(cfgp));
    c.worker().set_tickets(&tickets);
    c.start();
    auto vote = [&](const std::string& cfn) { return ask(c, C({{"cmd", "vote"}, {"id", "L1"}, {"cfn", cfn}, {"value", 1}}), 3000, 5); };
    auto list = [&] { return ask(c, C({{"cmd", "list"}, {"char", "Ken"}, {"sort", "recent"}, {"page", "1"}, {"voter_cfn", "Alpha"}}), 3000, 5); };
    auto bearer_of = [](const StubReq& r) { auto a = r.h.find("authorization"); return a == r.h.end() ? std::string() : a->second; };

    Answer a = list();
    auto lg = stub.take_log();
    check(a.ok && lg.size() == 1 && bearer_of(lg[0]).empty() && tickets.gets == 0, "a read without a token: sent as is, no ticket asked");
    check(!lg.empty() && lg[0].h["x-client-version"] == "2.1.0" && lg[0].target.find("voter_cfn") == std::string::npos, "x-client-version, no voter_cfn");
    check(c.worker().token_state() == 0, "token state: none");

    a = vote("Alpha");
    lg = stub.take_log();
    check(a.ok && a.data["action"] == "voted", "the first vote: answered once the token came");
    check(lg.size() == 2 && lg[0].target == "/auth/steam/ticket" && lg[1].target == "/api/vote/L1", "the ticket first, then the vote");
    if (lg.size() == 2) {
        check(json::parse(lg[0].body, nullptr, false) == C({{"ticket", "14000000deadbeef"}, {"cfn", "Alpha"}, {"client_version", "2.1.0"}}), "ticket body: hex, the CFN, the version");
        check(bearer_of(lg[0]).empty() && lg[0].h["x-api-key"] == "K", "the ticket request: the shared key, no token");
        check(bearer_of(lg[1]) == "Bearer " + kTok1, "the vote carries the new token");
        check(json::parse(lg[1].body, nullptr, false) == C({{"value", 1}, {"cfn", "Alpha"}}), "the vote body (v4)");
    }
    check(tickets.released_count() == 1 && tickets.released[0] == tickets.next, "the ticket released (CancelAuthTicket) after /auth/steam/ticket");
    bool okf = false;
    const std::string idtext = read_all(tp, &okf);
    const json idj = json::parse(idtext, nullptr, false);
    check(okf && idj.is_object() && idj.size() == 6 && idj["token"] == kTok1 && idj["api_base"] == stub.base() && idj["cfn"] == "Alpha" &&
          idj["method"] == "ticket" && idj["client_version"] == "2.1.0", "identity.json: the token for this api_base and nothing of Steam");
    check(idtext.find("dead") == std::string::npos, "identity.json holds no ticket byte");
    check(c.worker().token_state() == 1, "token state: yes");

    a = vote("Alpha");
    Answer b = list();
    lg = stub.take_log();
    check(a.ok && b.ok && lg.size() == 2 && bearer_of(lg[0]) == "Bearer " + kTok1 && bearer_of(lg[1]) == "Bearer " + kTok1 && tickets.gets == 1,
          "the token is reused: no new ticket; reads carry it too (my_vote)");

    { std::lock_guard<std::mutex> lk(sm); valid.clear(); next_token = kTok2; }
    a = vote("Alpha");
    lg = stub.take_log();
    check(a.ok && a.data["action"] == "voted", "token revoked on the server: the vote still goes through");
    check(lg.size() == 3 && lg[0].target == "/api/vote/L1" && lg[1].target == "/auth/steam/ticket" && lg[2].target == "/api/vote/L1" &&
          bearer_of(lg[0]) == "Bearer " + kTok1 && bearer_of(lg[2]) == "Bearer " + kTok2, "...vote (401), ticket, the vote again with the new token");
    check(auth::load_token(slash(tp), stub.base()) == kTok2, "...identity.json holds the new token");

    { std::lock_guard<std::mutex> lk(sm); valid.clear(); next_token = kTok3; accept_new = false; }
    a = vote("Alpha");
    lg = stub.take_log();
    check(lg.size() == 3 && lg[0].target == "/api/vote/L1" && lg[1].target == "/auth/steam/ticket" && lg[2].target == "/api/vote/L1",
          "a new token refused too: one new ticket only (no loop)");
    check(a.ok && auth::error_code(a.data) == "invalid_token" && !fs::exists(tp), "...the 401 reaches the caller, identity.json deleted");
    check(vote_effect(a.ok, a.data, 1).err.key == "com_auth_invalid", "...shown as 'Steam verification failed'");
    a = vote("Alpha");
    lg = stub.take_log();
    check(lg.size() == 2 && lg[0].target == "/auth/steam/ticket" && lg[1].target == "/api/vote/L1" && auth::error_code(a.data) == "invalid_token",
          "a token issued for this write and refused: not asked again in the same write");
    { std::lock_guard<std::mutex> lk(sm); accept_new = true; }

    fs::remove(tp);
    tickets.fail = true;
    const int gets0 = tickets.gets.load();
    a = vote("Alpha");
    lg = stub.take_log();
    check(lg.empty() && tickets.gets == gets0 + 1 && a.done && a.ok && a.data["error_code"] == auth::kUnavailable, "no Steam ticket: nothing sent, 'verification unavailable'");
    check(vote_effect(a.ok, a.data, 1).err.key == "com_auth_unavailable", "...its message");
    a = ask(c, C({{"cmd", "download"}, {"id", "x"}}), 3000, 5);
    check(a.ok && a.data["name"] == "N" && stub.take_log().size() == 1, "reads keep working without Steam");
    tickets.fail = false;

    { std::lock_guard<std::mutex> lk(sm); ticket_refusal = R"({"error":"steam_banned","message":"This Steam account is banned by the publisher."})"; }
    const size_t rel = tickets.released_count();
    a = vote("Alpha");
    lg = stub.take_log();
    check(lg.size() == 1 && lg[0].target == "/auth/steam/ticket" && a.data["error_code"] == "banned", "ticket refused (publisher ban): no vote sent, banned");
    check(tickets.released_count() == rel + 1, "...the refused ticket is released too");
    check(share_verdict(a.ok, a.data, "P").key == "com_auth_banned", "...shown as banned");
    { std::lock_guard<std::mutex> lk(sm); ticket_refusal.clear(); }

    a = vote("76561198012345678");
    lg = stub.take_log();
    bool leak = false;
    for (const StubReq& q : lg) if (q.body.find("7656119801") != std::string::npos || q.target.find("7656119801") != std::string::npos) leak = true;
    check(a.ok && lg.size() == 2 && !leak, "a SteamID as the cfn (the config fallback) never reaches the Worker, ticket nor vote");

    auth::Token other{ kTok1, "https://elsewhere.example", "", "ticket", "x", "2.1.0" };
    std::string err;
    auth::save_token(slash(tp), other, &err);
    b = list();
    a = ask(c, C({{"cmd", "delete"}, {"id", "L1"}, {"cfn", "Alpha"}}), 3000, 5);
    lg = stub.take_log();
    check(b.ok && !lg.empty() && bearer_of(lg[0]).empty(), "a token of another api_base is not sent");
    check(lg.size() == 3 && lg[1].target == "/auth/steam/ticket" && lg[2].method == "DELETE" && lg[2].target == "/api/delete/L1" &&
          lg[2].body.empty() && bearer_of(lg[2]) == "Bearer " + kTok3, "delete: a token for this api_base first, then DELETE with no body");
    check(a.ok && a.data["ok"] == true, "...deleted");

    fs::remove(tp);
    tickets.delay_ms = 3500;                     // 700 polls of 5 ms: past the 600-poll timeout if the wait counted
    a = vote("Alpha");
    tickets.delay_ms = 0;
    check(a.done && a.ok && a.data["action"] == "voted", "a slow Steam ticket (3.5 s) does not time the write out");
    c.stop();
    stub.stop();
}

void test_auth_page() {
    section("auth: the list shows author_cfn and the verified mark");
    const fs::path root = g_work / "auth_page";
    fs::remove_all(root);
    fs::create_directories(root / "game/SF6_Community_data");
    fs::create_directories(root / "game/SF6_RecordingSlotManager_data/Ken");
    Stub stub;
    stub.handler = [](const StubReq&) {
        StubResp s;
        json v4 = item("id-1", "Verified Pack", "Alpha", 1, 2, 3, 10, "2026-10-14 10:00:00", "", false);
        v4["author_cfn"] = "Alpha";
        v4["author_verified"] = true;
        json legacy = item("id-2", "Old Pack", "Bob", 1, 0, 0, 4, "2026-09-01 10:00:00", "", false);
        legacy["author_cfn"] = "Bob";
        legacy["author_verified"] = false;
        json unknown = item("id-3", "Nobody", "Unknown fighter", 1, 0, 0, 4, "2026-09-01 10:00:00", "", false);
        unknown["author_cfn"] = "Unknown fighter";
        unknown["author_verified"] = true;
        s.body = C({{"items", json::array({ v4, legacy, unknown })}, {"total", 3}}).dump();
        return s;
    };
    check(stub.start(), "page stub starts");
    write_cfg(root / "config.json", stub.base(), "K");
    Client c;
    c.configure(slash(root / "config.json"));
    c.start();
    std::string mark;                            // "" = the locale has no rsm.com_verified_mark
    Page::Env env;
    env.root = slash(root / "game") + "/";
    env.current_char = [] { return std::string("Ken"); };
    env.identity = [] { return Identity{ "Alpha", "Alpha" }; };
    env.tr = [&](const std::string& key, const std::vector<std::string>&) { return key == "com_verified_mark" && !mark.empty() ? mark : key; };
    Page pg(c, env);
    pg.load_state();
    pg.fetch(1);
    for (int i = 0; i < 3000 && c.busy(); ++i) { c.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    const auto& rows = pg.rows();
    check(rows.size() == 3, "three rows");
    if (rows.size() == 3) {
        check(rows[0].label.find("  by Alpha <color 66DD66>[V]</color>  ") != std::string::npos, "verified: 'by Alpha [V]' in green (" + rows[0].label + ")");
        check(rows[1].label.find("  by Bob  ") != std::string::npos && rows[1].label.find("[V]") == std::string::npos, "legacy row: plain author");
        check(rows[2].label.find("by Unknown fighter <color 66DD66>[V]</color>") != std::string::npos, "verified without a CFN: the server's label + [V]");
    }
    std::string msg;
    check(pg.download_prompt(1, &msg) && msg == "Download \"Verified Pack\" by Alpha?", "the confirm box names the author without the mark");
    mark = "\xE2\x9C\x93";
    pg.rebuild();
    check(!pg.rows().empty() && pg.rows()[0].label.find("by Alpha <color 66DD66>\xE2\x9C\x93</color>") != std::string::npos, "the mark comes from the locale (rsm.com_verified_mark)");
    check_eq(pg.tr_or("com_verified", "Steam-verified"), std::string("Steam-verified"), "tr_or: English when the locale lacks the key");
    c.stop();
    stub.stop();
}

// ---- a fake steam_api64.dll for the Steam engine (nothing here reaches Steam) ----------------------------------------------
namespace fake {
std::atomic<int> wrong{0};
std::vector<std::string> idents;
std::vector<uint32_t> cancelled;
steam::CallbackBase* cb = nullptr;
int reg_id = 0, regs = 0, unregs = 0;
int32_t user = 1;
bool no_iface = false;
uint32_t handle = 0x2a;
std::string version;
uint32_t get_ticket(void*, const char* id) { idents.push_back(id ? id : ""); return handle; }
void cancel(void*, uint32_t h) { cancelled.push_back(h); }
void bad(void*) { wrong.store(1); }                     // every slot but 14 and 17: calling it is a wrong slot
void* vtable[33];
struct Obj { void** vt; } obj{ vtable };
int32_t hsteamuser() { return user; }
void* find(int32_t, const char* v) { version = v ? v : ""; return no_iface ? nullptr : &obj; }
void reg(steam::CallbackBase* c, int id) { cb = c; reg_id = id; ++regs; }
void unreg(steam::CallbackBase*) { ++unregs; }
bool resolve(steam::Api* a, std::string*) {
    a->get_hsteamuser = hsteamuser; a->find_or_create_user_interface = find; a->register_callback = reg; a->unregister_callback = unreg;
    a->where = "fake steam_api64.dll"; a->dispatch = "classic: fake";
    return true;
}
void reset() {
    for (void*& v : vtable) v = reinterpret_cast<void*>(&bad);
    // the slots of isteamuser.h (SteamUser023), written here as numbers: a wrong constant in the engine calls bad()
    vtable[14] = reinterpret_cast<void*>(&get_ticket);
    vtable[17] = reinterpret_cast<void*>(&cancel);
    wrong = 0; idents.clear(); cancelled.clear(); cb = nullptr; reg_id = regs = unregs = 0; user = 1; no_iface = false; handle = 0x2a; version.clear();
}
bool has_cancel(uint32_t h) { return std::find(cancelled.begin(), cancelled.end(), h) != cancelled.end(); }
}  // namespace fake

void test_steam_engine() {
    section("auth: Steam method A on a fake steam_api (vtable slots, callback 168, timeouts, probe)");
    fake::reset();
    std::atomic<uint64_t> clk{ 1000 };
    std::vector<std::string> lines;
    auto mk = [&](steam::Engine::Resolver r) {
        return std::make_unique<steam::Engine>(std::move(r), [&](const std::string& s) { lines.push_back(s); }, [&] { return clk.load(); });
    };
    auto logged = [&](const std::string& what) { for (const auto& l : lines) if (l.find(what) != std::string::npos) return true; return false; };
    // get() on a thread (the Worker's) while this thread runs the frames (the game's); act(i) after each frame
    auto run = [&](steam::Engine& e, int timeout_ms, const std::function<void(int)>& act) {
        TicketSource::Ticket t;
        std::atomic<bool> done{ false };
        std::thread th([&] { t = e.get(timeout_ms); done = true; });
        for (int i = 0; i < 5000 && !done; ++i) {
            e.frame();
            if (act) act(i);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        th.join();
        return t;
    };
    steam::TicketResponse resp{};
    resp.hAuthTicket = 0x2a; resp.eResult = 1; resp.cubTicket = 234;
    for (int i = 0; i < 234; ++i) resp.rgubTicket[i] = (uint8_t)(i == 0 ? 0x14 : i);
    // the game's SteamAPI_RunCallbacks: the answer once the ticket was asked (the n_asked-th GetAuthTicketForWebApi call)
    auto deliver_when_asked = [](size_t n_asked, const steam::TicketResponse* rp) {
        return std::function<void(int)>([n_asked, rp](int) {
            if (fake::cb && fake::idents.size() == n_asked) { steam::TicketResponse copy = *rp; fake::cb->Run(&copy); fake::cb = nullptr; }
        });
    };

    auto e = mk(fake::resolve);
    TicketSource::Ticket t = run(*e, 3000, deliver_when_asked(1, &resp));
    check(t.ok && t.handle == 0x2a && t.bytes.size() == 234 && t.bytes[0] == 0x14 && t.bytes[233] == 233, "a ticket: handle, 234 bytes");
    check(fake::version == "SteamUser023" && fake::reg_id == 168 && fake::idents == std::vector<std::string>{ "sf6-community" }, "SteamUser023, callback 168, identity sf6-community");
    check(fake::wrong == 0, "only slot 14 was called (no other slot of the vtable)");
    check(fake::regs == 1 && fake::unregs == 1, "callback registered for the wait, unregistered after the answer");
    check(fake::cancelled.empty(), "not cancelled before the Worker answered");
    e->release(0x2a);
    e->frame();
    check(fake::cancelled == std::vector<uint32_t>{ 0x2a } && fake::wrong == 0, "release -> CancelAuthTicket through slot 17");
    check(logged("GetAuthTicketForWebApi(\"sf6-community\") = handle 0x0000002a") && logged("callback 168 after") && logged("eResult 1 (OK)"), "the probe log: each step");
    check(logged("234 bytes, starts 1401") && !logged("140102"), "the log holds the ticket's length and 4 hex digits, never more");
    check(logged("vtable slot 14 GetAuthTicketForWebApi -> community_test.exe+0x") && logged("callback dispatch: classic: fake"), "the log: where slot 14 points, the dispatch");
    steam::CallbackBase* cbp = nullptr;
    { fake::handle = 0x2b; }
    steam::TicketResponse other = resp;
    other.hAuthTicket = 0x99;
    resp.hAuthTicket = 0x2b;
    t = run(*e, 3000, [&](int) {
        if (fake::cb && fake::idents.size() == 2) {
            cbp = fake::cb;
            check(cbp->GetCallbackSizeBytes() == 2572, "GetCallbackSizeBytes = sizeof GetTicketForWebApiResponse_t");
            steam::TicketResponse c1 = other; cbp->Run(&c1);           // another ticket's answer (the game's own) first
            fake::cb = nullptr;
        } else if (cbp) {
            steam::TicketResponse c2 = resp; cbp->Run(&c2, false, 0);   // the second Run overload: the same handling
            cbp = nullptr;
        }
    });
    check(t.ok && t.handle == 0x2b && logged("not ours, still waiting"), "an answer for another ticket is ignored, ours taken");
    check(fake::regs == 2 && fake::unregs == 2 && fake::idents.size() == 2, "the interface is kept, the callback registered again");

    fake::handle = 0x2c;
    bool moved = false;
    t = run(*e, 3000, [&](int) { if (fake::idents.size() == 3 && !moved) { moved = true; clk += 5001; } });
    check(!t.ok && t.why.find("no callback 168 within 5000 ms") != std::string::npos && fake::has_cancel(0x2c) && fake::unregs == fake::regs,
          "no callback in 5 s: failure, the ticket cancelled, the callback unregistered (" + t.why + ")");
    t = e->get(500);
    check(!t.ok && t.why.find("no new try") != std::string::npos && fake::idents.size() == 3, "then 30 s without a new try (" + t.why + ")");
    clk += 30001;

    fake::handle = 0x2d;
    resp.hAuthTicket = 0x2d;
    resp.eResult = 2;
    t = run(*e, 3000, deliver_when_asked(4, &resp));
    check(!t.ok && t.why.find("eResult 2") != std::string::npos && fake::has_cancel(0x2d), "Steam refused the ticket (eResult 2): failure, cancelled");
    clk += 30001;
    resp.eResult = 1;
    fake::handle = 0x2e;
    resp.hAuthTicket = 0x2e;
    resp.cubTicket = 5000;
    t = run(*e, 3000, deliver_when_asked(5, &resp));
    check(!t.ok && t.why.find("no ticket") != std::string::npos && fake::has_cancel(0x2e), "a size over 2560: no ticket, nothing copied past the buffer");
    clk += 30001;
    resp.cubTicket = 234;

    t = e->get(100);                             // no frame runs meanwhile: the game thread never answers
    for (int i = 0; i < 5; ++i) e->frame();
    check(!t.ok && t.why.find("no answer from the game thread") != std::string::npos && fake::idents.size() == 5, "no game frame: a failure, and the request withdrawn");

    fake::handle = 0x2f;
    resp.hAuthTicket = 0x2f;
    t = run(*e, 60, nullptr);                    // the request starts, then gives up (60 ms) before any callback
    steam::CallbackBase* late = fake::cb;
    check(!t.ok && late && fake::idents.size() == 6, "the Worker stopped waiting after the ticket was asked");
    if (late) { steam::TicketResponse c3 = resp; late->Run(&c3); }
    e->frame();
    check(fake::has_cancel(0x2f) && logged("after the request stopped waiting"), "...the late ticket is cancelled by the engine itself");

    // the probe (switch file): once, the ticket cancelled at once, never handed to anyone
    auto p = mk(fake::resolve);
    fake::handle = 0x30;
    resp.hAuthTicket = 0x30;
    check(p->probe("test") && !p->probe("again"), "one probe per session");
    for (int i = 0; i < 50 && fake::idents.size() < 7; ++i) p->frame();
    if (fake::cb) { steam::TicketResponse c4 = resp; fake::cb->Run(&c4); }
    p->frame();
    check(fake::has_cancel(0x30) && p->status().rfind("probe OK: a ticket of 234 bytes", 0) == 0 && logged("probe result: OK"), "probe: OK, cancelled at once (" + p->status() + ")");

    fake::no_iface = true;
    auto n = mk(fake::resolve);
    t = run(*n, 3000, nullptr);
    check(!t.ok && t.why.find("no SteamUser023 interface") != std::string::npos, "no SteamUser023 interface: failure");
    t = n->get(100);
    check(!t.ok && t.why.find("method A is off for this session") != std::string::npos, "...and method A off for the session");
    fake::no_iface = false;

    auto r = mk([](steam::Api* a, std::string* why) { a->where = "steam_api64.dll is not loaded"; *why = a->where; return false; });
    t = run(*r, 3000, nullptr);
    check(!t.ok && t.why.find("not loaded") != std::string::npos && r->get(50).why.find("off for this session") != std::string::npos, "no steam_api64.dll: off for the session");

    fake::user = 0;
    auto u = mk(fake::resolve);
    t = run(*u, 3000, nullptr);
    check(!t.ok && t.why.find("HSteamUser 0") != std::string::npos, "HSteamUser 0: failure");
    fake::user = 1;
    clk += 30001;
    fake::handle = 0x31;
    resp.hAuthTicket = 0x31;
    const size_t asked = fake::idents.size();
    t = run(*u, 3000, [&](int) { if (fake::cb && fake::idents.size() == asked + 1) { steam::TicketResponse c5 = resp; fake::cb->Run(&c5); fake::cb = nullptr; } });
    check(t.ok && t.handle == 0x31, "...not for the session: a later try works");

    check(steam::module_of(reinterpret_cast<void*>(&fake::bad)).rfind("community_test.exe+0x", 0) == 0 && steam::module_of(nullptr).empty(), "module_of: module + offset");
    check(steam::dispatch_hint().rfind("unknown", 0) == 0, "dispatch_hint of the test exe (no steam_api64.dll): unknown");
    check(fake::wrong == 0, "no wrong vtable slot in the whole section");
}

}  // namespace

int main(int argc, char** argv) {
    std::string work;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--work") work = argv[i + 1];
    }
    if (work.empty()) { std::printf("usage: community_test --work <dir>\n"); return 2; }
    g_work = work;
    fs::create_directories(g_work);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    timeBeginPeriod(1);                       // Sleep(17) must last 17 ms: the 600-tick timeout is measured in real time

    if (std::getenv("CT_VERBOSE")) set_log([](const std::string& s) { std::printf("    log: %s\n", s.c_str()); });
    set_client_version("2.1.0");
    test_offline();
    test_wire();
    test_fixes();
    test_auth_pure();
    test_auth_flow();
    test_auth_page();
    test_steam_engine();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
