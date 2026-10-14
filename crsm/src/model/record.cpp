#include "record.hpp"

#include "luajson.hpp"
#include "lutil.hpp"

namespace model::record {
namespace {
namespace lj = model::luajson;
}

void Engine::arm(Port& p, int n, uint64_t now) {
    restore_.reset();                                      // a deferred restore of an earlier recording must not fire now
    restore_at_.reset();
    snapshot_ = p.read8();
    armed_ = true;
    n_ = n;
    seen_now_ = false;
    deadline_ = now + kDeadline;
}

json recorded_entry(int n, const json& rec, const json* old) {
    json e = rec.is_object() ? rec : json::object();
    for (const char* k : { "active", "weight", "name" }) e.erase(k);   // slot 8's (or the virtual's riding it): never n's
    if (old && old->is_object()) {
        for (const char* k : { "active", "weight", "name" })
            if (json v = lj::index(*old, k); !v.is_null()) e[k] = std::move(v);
    } else e["weight"] = 1;                                // a new virtual slot (vslots' new_empty_entry)
    e["id"] = n;
    return e;
}

void Engine::store_from8(Port& p, int n, const json& e) {
    if (!e.is_object()) return;
    p.store(n, recorded_entry(n, e, p.vslot(n)));
}

void Engine::finalize(Port& p, bool paused) {
    if (!armed_) return;
    if (seen_now_) {
        const auto e = p.read8();
        if (e && e->is_object() && !lutil::truthy(lj::index(*e, "empty")) && p.frames(*e) > 0) store_from8(p, n_, *e);
    }
    if (snapshot_ && snapshot_->is_object()) {             // slot 8 back to what it was
        json snap = *snapshot_;
        snap["id"] = kWorkSlot + 1;
        p.write8(snap);
        restore_ = snap;                                   // and once more after the unpause (see the header)
        restore_at_.reset();
        if (!paused) p.own8_back();                        // (paused: the menu open's replay unload follows)
    }
    armed_ = false;
    seen_now_ = false;
    snapshot_.reset();
}

bool Engine::tick(Port& p, uint64_t now, int64_t state, bool paused) {
    if (restore_ && !paused) {
        if (!restore_at_) restore_at_ = now + kRestoreDelay;
        else if (now >= *restore_at_) { p.write8(*restore_); restore_.reset(); restore_at_.reset(); }
    }
    if (!armed_) return false;
    if (state == kRecordNow) seen_now_ = true;
    // done: left RECORD_NOW, or the pause menu opened while recording (pausing stops it), or the deadline
    const bool done = (seen_now_ && (state != kRecordNow || paused)) || now > deadline_;
    if (!done) return false;
    finalize(p, paused);
    return true;
}

bool Engine::capture8(Port& p, int n) {
    const auto e = p.read8();
    if (!e || !e->is_object() || lutil::truthy(lj::index(*e, "empty")) || p.frames(*e) <= 0) return false;
    store_from8(p, n, *e);
    return true;
}

void Engine::hand_back(Port& p) {
    if (armed_) finalize(p, true);                         // a started recording stored, slot 8 back to its own content
    restore_.reset();
    restore_at_.reset();
}

}  // namespace model::record
