#include "client.hpp"

namespace community {
namespace {

json error_data(const char* text) {
    json d = json::object();
    d["error"] = text;
    return d;
}

void call(const Client::Callback& cb, bool ok, const json& data) {
    if (!cb) return;
    try { cb(ok, data); } catch (...) { /* pcall(cb, ...) in the Lua: a callback error never reaches the poll */ }
}

}  // namespace

void Client::configure(std::string config_path, std::string response_path) {
    std::lock_guard<std::mutex> lk(mx_);
    config_path_ = std::move(config_path);
    response_path_ = std::move(response_path);
}

void Client::start() { worker_.start(); }
void Client::stop() { worker_.stop(); }

void Client::set_verifying_hook(std::function<void()> h) {
    std::lock_guard<std::mutex> lk(mx_);
    verifying_hook_ = std::move(h);
}

bool Client::busy() const {
    std::lock_guard<std::mutex> lk(mx_);
    return in_flight_;
}

bool Client::send(json cmd, Callback cb) {
    Worker::Job job;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (!in_flight_) {
            pending_ = Pending{};
            announced_ = false;
            pending_.id = next_id_++;
            pending_.cb = cb;
            in_flight_ = true;
            ++counters_.sent;
            job.id = pending_.id;
            job.cmd = std::move(cmd);
            job.config_path = config_path_;
            job.response_path = response_path_;
        } else {
            ++counters_.busy_refusals;
        }
    }
    if (job.id == 0) {                                  // refused
        call(cb, false, error_data("busy"));
        return false;
    }
    worker_.submit(std::move(job));
    return true;
}

void Client::abandon() {
    std::lock_guard<std::mutex> lk(mx_);
    in_flight_ = false;
    pending_ = Pending{};
}

void Client::poll() {
    worker_.tick();
    // Answers of requests nobody waits for any more (abandoned or timed out) are taken and dropped.
    Worker::Done d;
    bool have = false;
    uint64_t waiting = 0;
    {
        std::lock_guard<std::mutex> lk(mx_);
        waiting = in_flight_ ? pending_.id : 0;
    }
    while (worker_.take(&d)) {
        if (d.id != waiting) continue;                  // stale
        have = true;
        break;
    }
    Callback cb;
    bool ok = false;
    json data;
    bool deliver = false;
    std::function<void()> notice;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (!in_flight_) { announced_ = false; return; }
        if (!have && verify_notice(true, worker_.authenticating(), &announced_)) notice = verifying_hook_;
        if (!worker_.authenticating()) ++pending_.ticks;   // a Steam ticket wait has its own bound (kTicketWaitMs)
        if (have && d.id == pending_.id) {
            cb = std::move(pending_.cb);
            ok = d.reply.ok;
            data = std::move(d.reply.data);
            in_flight_ = false;
            pending_ = Pending{};
            ++counters_.answered;
            if (!ok) ++counters_.failures;
            deliver = true;
        } else if (pending_.ticks > kTimeoutTicks) {
            cb = std::move(pending_.cb);
            ok = false;
            data = error_data("timeout");
            in_flight_ = false;
            pending_ = Pending{};
            ++counters_.timeouts;
            deliver = true;
        }
    }
    if (notice) { try { notice(); } catch (...) {} }
    if (deliver) call(cb, ok, data);
}

Client::Counters Client::counters() const {
    std::lock_guard<std::mutex> lk(mx_);
    return counters_;
}

}  // namespace community
