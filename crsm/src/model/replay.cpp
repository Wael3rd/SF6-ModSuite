#include "replay.hpp"

#include <algorithm>

#include "luajson.hpp"
#include "lutil.hpp"

namespace model::replay {
namespace {
namespace lj = model::luajson;
}

std::string fill_signature(const json* e) {
    model::vslots::VSlots tmp;
    if (!e || !e->is_object()) return "0";
    tmp.put(9, *e);
    const int64_t frames = tmp.frames(9);
    if (frames <= 0) return "0";
    const json tl = lj::index(*e, "timeline");
    const size_t len = lj::len(tl);
    auto at = [&](size_t i1) -> std::string {            // tostring(e.timeline[i1]) (1-based)
        if (i1 < 1 || i1 > len) return "nil";
        const json& x = tl[i1 - 1];
        if (x.is_string()) return x.get<std::string>();
        if (x.is_number()) return lutil::number_to_string(x);
        if (x.is_boolean()) return x.get<bool>() ? "true" : "false";
        return "table";
    };
    return std::to_string(frames) + "|" + std::to_string(len) + "|" + at(1) + "|" + at(std::max<size_t>(1, len / 2)) + "|" + at(len);
}

void Engine::write8_keep(Port& p, const json& entry) {
    Live before;
    const bool had = p.live(kWorkSlot, &before);
    p.write8(entry);
    Live after;
    if (had && p.live(kWorkSlot, &after) && after.active != before.active) p.set_active(kWorkSlot, before.active);
}

void Engine::load_virtual(Port& p, const model::vslots::VSlots& v, int n) {
    if (snap_ == Snap::None) {                             // slot 8's own content, once
        const auto s = p.read8();
        if (s) { snap_ = Snap::Entry; snapshot_ = *s; } else snap_ = Snap::Failed;
    }
    const json* e = v.get(n);
    if (!e) return;
    json copy = *e;
    if (copy.is_object()) {
        copy["id"] = kWorkSlot + 1;
        copy.erase("active");
        copy.erase("weight");
    }
    write8_keep(p, copy);
    loaded_ = n;
}

// Slot 8's own content is a recording: slot 8 as it is (no snapshot: nothing was written over it), or the snapshot taken
// before a virtual was written over it. Its weight: the snapshot's, else the slot's.
bool Engine::own8_recorded(bool valid_now, double weight_now, double* w) const {
    *w = weight_now > 1 ? weight_now : 1.0;
    if (snap_ == Snap::None) return valid_now;
    if (snap_ != Snap::Entry || !snapshot_.is_object() || lutil::truthy(lj::index(snapshot_, "empty"))) return false;
    model::vslots::VSlots tmp;
    tmp.put(9, snapshot_);
    if (tmp.frames(9) <= 0) return false;
    if (const auto sw = lutil::to_number(lj::index(snapshot_, "weight"))) {
        const double x = sw->get<double>();
        *w = x > 1 ? x : 1.0;
    }
    return true;
}

void Engine::unload_virtual(Port& p) {
    if (snap_ == Snap::Entry) {
        json snap = snapshot_;
        if (snap.is_object()) snap["id"] = kWorkSlot + 1;
        write8_keep(p, snap);
    }
    snap_ = Snap::None;
    snapshot_ = json();
    loaded_ = 0;
}

void Engine::menu_open(Port& p, const model::vslots::VSlots&) {
    if (loaded_) unload_virtual(p);
    if (forced8_) {
        Live l;
        if (p.live(kWorkSlot, &l)) p.set_active(kWorkSlot, false);
        forced8_ = false;
    }
    p.install();
}

void Engine::menu_close(Port& p, const std::function<const model::vslots::VSlots&()>& vslots,
                        const std::function<void(const json&)>& apply_patch) {
    if (p.installed()) {                                   // uninstall_big: entries read back, the game's array put back
        const model::vslots::VSlots& v = vslots();
        json patch = json::object();
        for (int n = model::vslots::kFirst; n <= model::vslots::kLast; ++n) {
            bool act = false;
            int64_t w = 1;
            if (!v.get(n) || !p.entry(n, &act, &w)) continue;
            patch[std::to_string(n)] = json::array({ act, w > 1 ? w : 1 });
        }
        if (!patch.empty()) apply_patch(patch);
        p.uninstall();
    }
    const model::vslots::VSlots& v = vslots();
    const std::vector<int> list = v.checked();
    if (list.empty()) return;
    int longest = list[0];                                 // the first longest, in slot order
    for (int n : list) if (v.frames(n) > v.frames(longest)) longest = n;
    if (!loaded_) load_virtual(p, v, longest);             // never over a virtual slot already in place
    bool any = false;
    for (int k0 = 0; k0 <= 6; ++k0) { Live l; if (p.live(k0, &l) && l.active && l.valid) any = true; }
    Live s8;
    if (!any && p.live(kWorkSlot, &s8) && !s8.active) { p.set_active(kWorkSlot, true); forced8_ = true; }
}

bool Engine::pre_draw(Port& p, const model::vslots::VSlots& v, model::draw::Rng& rng, Draw* d) {
    *d = Draw{};
    const std::vector<int> list = v.checked();
    if (list.empty()) return false;
    model::draw::LiveSlot live[8];
    for (int k0 = 0; k0 < 8; ++k0) {
        Live l;
        if (p.live(k0, &l)) { live[k0].present = true; live[k0].active = l.active; live[k0].valid = l.valid; live[k0].weight = (double)l.weight; }
    }
    auto cands = model::draw::candidates(live, forced8_, list, [&](int n) { return v.weight(n); });
    // Slot 8's own recording. The Lua (RMT:1116, kept by draw::candidates) left slot 8 out as soon as a virtual slot was
    // checked ("slot 8 stands for the virtual set"): a slot 8 the player checked was never played (Wael 13/10: 8 + 16 on,
    // 4 + 8 + 16 on: 8 never played). It is drawn here, by its weight, when the player checked it (active, not forced on
    // by the menu close) and its own content is a recording: slot 8 itself, or the snapshot kept aside while a virtual
    // rides it (put back before the game plays it, below).
    double w8 = 0;
    if (live[kWorkSlot].present && live[kWorkSlot].active && !forced8_ && own8_recorded(live[kWorkSlot].valid, live[kWorkSlot].weight, &w8)) {
        auto at = std::find_if(cands.begin(), cands.end(), [](const model::draw::Candidate& c) { return c.n > kWorkSlot + 1; });
        cands.insert(at, model::draw::Candidate{ kWorkSlot + 1, w8 });   // in slot order: after 1-7, before 9-30
    }
    const auto pick = model::draw::pick(cands, rng);
    if (!pick) return false;
    const int n = *pick;
    for (int k0 = 0; k0 < 8; ++k0) { Live l; d->saved[k0] = p.live(k0, &l) && l.active; }
    if (n > 8) { if (n != loaded_) load_virtual(p, v, n); }
    else if (n == 8 && snap_ == Snap::Entry) unload_virtual(p);   // slot 8's own content back (a virtual rode it)
    const int id = (n > 8) ? kWorkSlot : (n - 1);
    for (int k0 = 0; k0 < 8; ++k0) { Live l; if (p.live(k0, &l)) p.set_active(k0, k0 == id); }
    if (p.installed()) {                                   // menu open (Start Replay): our 22 entries are not drawn either
        d->has_vs = true;
        for (int vn = model::vslots::kFirst; vn <= model::vslots::kLast; ++vn) {
            bool act = false;
            int64_t w = 1;
            if (!p.entry(vn, &act, &w)) continue;
            d->saved_vs[vn] = act;
            if (act) p.entry_set_active(vn, false);
        }
    }
    d->active = true;
    d->n = n;
    return true;
}

void Engine::post_draw(Port& p, const Draw& d) {
    if (!d.active) return;
    for (int k0 = 0; k0 < 8; ++k0) { Live l; if (p.live(k0, &l) && l.active != d.saved[k0]) p.set_active(k0, d.saved[k0]); }
    if (d.has_vs && p.installed()) {
        for (const auto& [vn, was] : d.saved_vs) {
            bool act = false;
            int64_t w = 1;
            if (p.entry(vn, &act, &w) && act != was) p.entry_set_active(vn, was);
        }
    }
}

void Engine::forget(Port& p, int n) {
    p.sync(n);
    if (loaded_ == n) loaded_ = 0;
}

void Engine::imported(Port& p) {
    if (loaded_) loaded_ = 0;                              // the snapshot stays (RMT:2101)
    for (int n = model::vslots::kFirst; n <= model::vslots::kLast; ++n) p.sync(n);
}

void Engine::unload_if_loaded(Port& p) { if (loaded_) unload_virtual(p); }

void Engine::own8_back() {
    loaded_ = 0;
    snap_ = Snap::None;
    snapshot_ = json();
}

void Engine::char_changed() {
    loaded_ = 0;
    snap_ = Snap::None;
    snapshot_ = json();
}

void Engine::hand_back(Port& p) {
    if (p.installed()) p.uninstall();
    if (loaded_) unload_virtual(p);
    if (forced8_) {
        Live l;
        if (p.live(kWorkSlot, &l)) p.set_active(kWorkSlot, false);
        forced8_ = false;
    }
}

}  // namespace model::replay
