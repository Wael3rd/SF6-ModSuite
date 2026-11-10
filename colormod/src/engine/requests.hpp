// Requests from other mods (include/sf6colormod_entries.h): what a fighter / costume wears, over the player's own
// choices. Pure (no game access): the decisions (decide.cpp) hold it under their own lock and give it the running flow
// map and the frame; tests: tests/requests_test.cpp.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace engine::requests {

constexpr int kPreview = 1, kReplay = 2, kBattle1 = 3, kBattle2 = 4;   // kBattle1 / 2: the battle fighter of P1 / P2
inline bool known(int scope) { return scope >= kPreview && scope <= kBattle2; }

struct Request {
    std::string id;             // entry id ("" = the game's own colours)
    uintptr_t battle = 0;       // scope 2: the replay's flow map (compared only, never dereferenced)
    bool ran = false;           // scope 2: its battle has been seen running
    uint64_t set_at = 0;        // frame of the request
};

class Table {
public:
    // false: refused (unknown scope, bad fighter / costume, scope 2 without its battle)
    bool set(int scope, int fighter, int costume, const std::string& id, uintptr_t battle, uint64_t now) {
        if (!known(scope) || fighter < 0 || costume < 0) return false;
        if (scope == kReplay && !battle) return false;
        auto& t = t_[scope];
        if (scope == kReplay)                     // a request for another battle: that battle's requests are over
            for (auto it = t.begin(); it != t.end();) it = it->second.battle != battle ? t.erase(it) : std::next(it);
        Request r;
        r.id = id;
        r.battle = scope == kReplay ? battle : 0;
        r.set_at = now;
        t[{ fighter, costume }] = r;
        return true;
    }
    void clear(int scope) { if (known(scope)) t_[scope].clear(); }
    bool any(int scope) const { return known(scope) && !t_[scope].empty(); }
    // What applies to a fighter / costume: scopes 1, 3, 4 as set (the requester clears them); scope 2 only while `running`
    // is its battle
    const Request* find(int scope, int fighter, int costume, uintptr_t running) const {
        if (!known(scope)) return nullptr;
        auto it = t_[scope].find({ fighter, costume });
        if (it == t_[scope].end()) return nullptr;
        if (scope == kReplay && (!running || it->second.battle != running)) return nullptr;
        return &it->second;
    }
    // Housekeeping (scope 2): a request whose battle ran and no longer runs is over; one whose battle never ran within
    // `timeout` frames is dropped
    void tick(uintptr_t running, uint64_t now, uint64_t timeout) {
        auto& t = t_[kReplay];
        for (auto it = t.begin(); it != t.end();) {
            Request& r = it->second;
            if (running && running == r.battle) r.ran = true;
            const bool over = r.ran ? running != r.battle : now - r.set_at > timeout;
            it = over ? t.erase(it) : std::next(it);
        }
    }
    size_t size(int scope) const { return known(scope) ? t_[scope].size() : 0; }

private:
    std::map<std::pair<int, int>, Request> t_[kBattle2 + 1];
};

}  // namespace engine::requests
