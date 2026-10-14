// community::Client - one request in flight, a callback, a tick timeout: CommunityBridge.lua's send() / poll() / busy(),
// over the WinHTTP worker. Used on one thread (the core's frame callback; the Lua bridge calls come from the same
// LateUpdateBehavior thread, and a mutex protects the state anyway). Callbacks run inside poll(), outside the lock.
//
//   send(cmd, cb)  nothing in flight: the command goes to the worker. Otherwise cb(false, { error = "busy" }) at once.
//   poll()         once per frame: a finished answer is handed to the callback (ok, data); after kTimeoutTicks polls without
//                  one, cb(false, { error = "timeout" }) and the answer, if it comes, is dropped. The polls while the worker
//                  gets a Steam-verified token (Worker::authenticating) do not count: that wait has its own bounds.
// A request that timed out here may still be running in the worker (at most 15 s): the next job waits behind it.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "http.hpp"

namespace community {

constexpr int kTimeoutTicks = 600;     // CommunityBridge.lua TIMEOUT: ~10 s at 60 fps

class Client {
public:
    using Callback = std::function<void(bool ok, const json& data)>;

    // `config_path` = SF6_Community_data/config.json. `response_path` (optional) = where the worker also writes each answer
    // as { ok, data }: the file the Lua shim reads with json.load_file (a table cannot cross the bridge).
    void configure(std::string config_path, std::string response_path = "");
    void start();                            // starts the worker thread
    void stop();                             // tests only

    bool busy() const;
    // false = refused (busy): the callback was called with the busy error.
    bool send(json cmd, Callback cb);
    void poll();
    // Forget the request in flight without calling its callback (the Lua shim timed out first); its answer is dropped.
    void abandon();

    struct Counters { uint64_t sent = 0, answered = 0, timeouts = 0, busy_refusals = 0, failures = 0; };
    Counters counters() const;
    Worker& worker() { return worker_; }

private:
    struct Pending { uint64_t id = 0; Callback cb; int ticks = 0; };

    mutable std::mutex mx_;
    Worker worker_;
    std::string config_path_, response_path_;
    bool in_flight_ = false;
    Pending pending_;
    uint64_t next_id_ = 1;
    Counters counters_;
};

}  // namespace community
