// community::http - the WinHTTP client and its worker thread (WinHTTP instead of the v1.x bridge's curl.exe).
//
// Everything here runs off the game thread. A Worker owns one thread that takes jobs in order; a job is a bridge command
// (protocol.hpp), the answer comes back as a Done record taken by the owner on its own thread (Client::poll). The worker
// touches no game object and calls nothing of the core: it logs through the sink set by set_log().
//
// Equivalence with the v1.x bridge, SF6_CommunityBridge.dll (curl.exe -s -S --max-time 15):
//   - 15 s for the whole request (a watchdog, Worker::tick, closes the request handle past it) -> "curl exit 28"
//   - no redirect followed, the HTTP status is not an error (a 4xx / 5xx body reaches the callbacks as data)
//   - the system's automatic proxy (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY), TLS as the system has it
//   - headers Content-Type / Accept: application/json and x-api-key (when the key is not empty); since 2.1.0 also
//     x-client-version and Authorization: Bearer (auth.hpp: the Steam-verified identity of ADR-0018)
//   - network failures keep the text the tickers show today: "curl exit N", N = the curl code of the same failure
//
// 2.1.0: a write (share / vote / delete) needs the token of identity.json; without one the Worker thread gets it first
// (set_tickets: a Steam ticket from the game thread, then POST /auth/steam/ticket), a 401 invalid_token gets a new one once,
// and a write that can get none is answered at once without reaching the network (auth::refused_reply). Without a ticket
// source (the tests of the older sections) writes go out without a token, as in 2.0.0.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "protocol.hpp"
#include "steam_ticket.hpp"

namespace community {

constexpr int kMaxTimeMs = 15000;           // --max-time 15
constexpr int kTicketWaitMs = 8000;         // a Steam ticket: 5 s for its callback + the game thread's frames

// x-client-version of every request and the ticket's client_version ("2.1.0": cmn::version(), set at init)
void set_client_version(const std::string& v);

void set_log(void (*sink)(const std::string&));   // journal line sink (nullptr = silent); never receives the API key

// One request, synchronous. `cancel_token` lets another thread abort it (see Worker::tick).
struct CancelToken;
HttpResult http_perform(const Request& rq, const std::string& api_key, CancelToken* cancel_token, const std::string& bearer = "");
// WinHTTP error number -> the exit code curl reports for the same failure (28 timeout, 6 DNS, 7 refused, 35 TLS, 60 certificate, 56 other)
int curl_code_for_winhttp_error(unsigned long err);

class Worker {
public:
    struct Job {
        uint64_t id = 0;
        json cmd;                      // a bridge command; "file_path" (share) names a pack file the worker reads
        std::string config_path;       // SF6_Community_data/config.json, read at every job (api_base and api_key may change)
        std::string response_path;     // when not empty, the answer is also written there as { "ok": b, "data": ... }
    };
    struct Done {
        uint64_t id = 0;
        std::string cmd;
        Reply reply;
        int status = 0;                // HTTP status, 0 = none
        int64_t ms = 0;
    };

    Worker();
    ~Worker();
    void start();                      // idempotent
    void stop();                       // joins; for the tests (the DLL lets the thread go with the process)
    void submit(Job job);
    bool take(Done* out);              // non-blocking
    void tick();                       // call often (every frame): aborts the request in flight past kMaxTimeMs
    bool working() const { return running_since_ms_.load() != 0; }
    uint64_t jobs_done() const { return jobs_done_.load(); }
    // The Steam ticket source of the writes (nullptr = writes without a token). Set before start().
    void set_tickets(TicketSource* t) { tickets_ = t; }
    // true while the job in flight waits for a Steam ticket or for /auth/steam/ticket (Client::poll stops its tick count:
    // these waits have their own bounds, kTicketWaitMs and the 15 s watchdog)
    bool authenticating() const { return authenticating_.load(); }
    // identity.json at the last job: -1 not read yet, 0 no token for the api_base, 1 a token (status.json, never the token)
    int token_state() const { return token_state_.load(); }

private:
    void run();
    Done run_job(const Job& j);
    HttpResult perform(const Request& rq, const Config& cfg, const std::string& bearer);
    // A token from a Steam ticket: "" + *code (auth::refusal_code / kUnavailable) when none could be had
    std::string acquire(const Config& cfg, const std::string& token_path, const json& cmd, std::string* code);

    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    std::atomic<int64_t> running_since_ms_{0};
    std::atomic<uint64_t> jobs_done_{0};
    TicketSource* tickets_ = nullptr;
    std::atomic<bool> authenticating_{false};
    std::atomic<int> token_state_{-1};
    CancelToken* token_ = nullptr;     // the request in flight, guarded by token_mx_
    std::mutex token_mx_;
};

}  // namespace community
