#include "setup.hpp"

#include "luajson.hpp"
#include "lutil.hpp"

namespace model::setup {
namespace {

namespace lj = model::luajson;

std::optional<int64_t> int_of(const json& t, const char* k) {
    const json v = lj::index(t, k);
    if (const auto n = lutil::to_number(v)) return lutil::to_int_key(*n);
    return std::nullopt;
}

// the entries up to the first hole (ipairs), as every pack reader walks them
template <class J, class F> void each_entry(J& pack, F&& f) {
    if (!pack.is_array()) return;
    for (auto& e : pack) { if (e.is_null()) break; if (!f(e)) return; }
}

int64_t id_of(const json& e) {
    if (!e.is_object()) return 0;
    const json id = lj::index(e, "id");
    if (!id.is_number()) return 0;
    const double d = id.get<double>();
    return d == (double)(int64_t)d ? (int64_t)d : 0;
}

}  // namespace

bool Dummy::any() const {
    for (const auto& x : v) if (x) return true;
    return false;
}

void sanitize(Dummy& d) {
    for (int i = 0; i < kDummyCount; ++i) {
        const DummyField& f = kDummyFields[i];
        if (d.v[(size_t)i] && (*d.v[(size_t)i] < f.min || *d.v[(size_t)i] > f.max)) d.v[(size_t)i].reset();
    }
    for (int g = 1;; ++g) {                     // each weight group: complete and not all zero, or dropped
        bool members = false, complete = true;
        int64_t sum = 0;
        for (int i = 0; i < kDummyCount; ++i) {
            if (kDummyFields[i].group != g) continue;
            members = true;
            if (d.v[(size_t)i]) sum += *d.v[(size_t)i]; else complete = false;
        }
        if (!members) break;
        if (complete && sum > 0) continue;
        for (int i = 0; i < kDummyCount; ++i) if (kDummyFields[i].group == g) d.v[(size_t)i].reset();
    }
}

json to_json(const Setup& s) {
    json b = json::object();
    b["version"] = kVersion;
    if (s.reversal) {
        json r = json::object();
        for (int rt = 0; rt < kCats; ++rt) {
            json rows = json::array();
            for (const RdRow& row : (*s.reversal)[(size_t)rt]) rows.push_back(model::store::rd_to_json(row));
            r[kCatNames[rt]] = std::move(rows);
        }
        b["reversal"] = std::move(r);
    }
    if (s.playback) {
        json p = json::object();
        if (s.playback->PlayType) p["PlayType"] = *s.playback->PlayType;
        if (s.playback->LoopType) p["LoopType"] = *s.playback->LoopType;
        if (s.playback->InfoType) p["InfoType"] = *s.playback->InfoType;
        b["playback"] = std::move(p);
    }
    if (s.dummy && s.dummy->any()) {
        json d = json::object();
        for (int i = 0; i < kDummyCount; ++i) {
            const auto& x = s.dummy->v[(size_t)i];
            if (!x) continue;
            const DummyField& f = kDummyFields[i];
            if (f.is_bool) d[f.part][f.name] = *x != 0; else d[f.part][f.name] = *x;
        }
        b["dummy"] = std::move(d);
    }
    return b;
}

std::optional<Setup> from_json(const json& block, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return std::optional<Setup>(); };
    if (block.is_null()) return fail("no training block");
    if (!block.is_object()) return fail("training block is not an object");
    const auto v = int_of(block, "version");
    if (!v) return fail("training block without a version");
    if (*v != kVersion) return fail("training block of another version");
    Setup s;
    const json rev = lj::index(block, "reversal");
    if (rev.is_object()) {
        Rows rows;
        for (int rt = 0; rt < kCats; ++rt) {
            const json list = lj::index(rev, kCatNames[rt]);
            std::vector<RdRow>& out = rows[(size_t)rt];
            if (list.is_array())
                for (const json& t : list) {
                    if ((int)out.size() >= kRowsTotal) break;
                    out.push_back(model::store::rd_from_json(t, RdRow{}));   // a non-table element: a default row
                }
            while ((int)out.size() < kRowsPage) out.push_back(RdRow{});
        }
        s.reversal = std::move(rows);
    }
    const json pb = lj::index(block, "playback");
    if (pb.is_object()) {
        Playback p;
        p.PlayType = int_of(pb, "PlayType");
        p.LoopType = int_of(pb, "LoopType");
        p.InfoType = int_of(pb, "InfoType");
        s.playback = p;
    }
    const json dm = lj::index(block, "dummy");
    if (dm.is_object()) {
        Dummy d;
        for (int i = 0; i < kDummyCount; ++i) {
            const DummyField& f = kDummyFields[i];
            const json part = lj::index(dm, f.part);
            if (!part.is_object()) continue;
            if (f.is_bool) {                    // a boolean field takes a JSON boolean only
                const json x = lj::index(part, f.name);
                if (x.is_boolean()) d.v[(size_t)i] = x.get<bool>() ? 1 : 0;
            } else d.v[(size_t)i] = int_of(part, f.name);
        }
        sanitize(d);
        if (d.any()) s.dummy = d;
    }
    return s;
}

const json* entry_one(const json& pack) {
    const json* found = nullptr;
    each_entry(pack, [&](const json& e) { if (id_of(e) == 1) { found = &e; return false; } return true; });
    return found;
}
json* entry_one(json& pack) { return const_cast<json*>(entry_one(static_cast<const json&>(pack))); }

std::optional<Setup> of_pack(const json& pack, std::string* why) {
    const json* e = entry_one(pack);
    if (!e) { if (why) *why = "no entry with id 1"; return std::nullopt; }
    return from_json(lj::index(*e, kKey), why);
}

bool stamp(json& pack, const Setup& s) {
    json* e = entry_one(pack);
    if (!e) return false;
    (*e)[kKey] = to_json(s);
    return true;
}

void stamp_active(json& pack, const std::array<bool, kSlotTotal>& on) {
    each_entry(pack, [&](json& e) {
        const int64_t id = id_of(e);
        if (id >= 1 && id <= kSlotTotal) e["active"] = on[(size_t)id - 1] && !lutil::truthy(lj::index(e, "empty"));
        return true;
    });
}

std::optional<bool> active_of(const json& entry) {
    if (!entry.is_object()) return std::nullopt;
    const json a = lj::index(entry, "active");
    if (!a.is_boolean()) return std::nullopt;
    return a.get<bool>();
}

bool is_default(const RdRow& r) {
    const RdRow d;
    return r.Type == d.Type && r.IsActive == d.IsActive && r.IsValid == d.IsValid && r.IsFrame == d.IsFrame &&
           r.Delay == d.Delay && r.Count == d.Count && r.SkillIndex == d.SkillIndex && r.Level == d.Level && r.Frame == d.Frame;
}

bool rows_in_use(const Rows& r) {
    for (const auto& cat : r) for (const RdRow& row : cat) if (row.IsValid) return true;
    return false;
}

void trim(Rows& r) {
    bool extra = false;
    for (const auto& cat : r)
        for (size_t k = kRowsPage; k < cat.size(); ++k) if (!is_default(cat[k])) extra = true;
    for (auto& cat : r) cat.resize(extra ? kRowsTotal : kRowsPage);
}

RdRow from_game(const RdRow& game, std::optional<int64_t> mapped) {
    RdRow r = game;
    if (mapped && *mapped > 8 && *mapped <= kSlotTotal && r.Type == kRecording && r.SkillIndex == kWork) r.SkillIndex = *mapped - 1;
    return r;
}

GameRow to_game(const RdRow& pack) {
    GameRow g;
    g.row = pack;
    if (pack.Type < kTypeMin || pack.Type > kTypeMax) { g.row = RdRow{}; return g; }
    if (pack.Type != kRecording) return g;
    if (pack.SkillIndex < 0 || pack.SkillIndex >= kSlotTotal) { g.row = RdRow{}; return g; }
    if (pack.SkillIndex > kWork) { g.virt = (int)pack.SkillIndex + 1; g.row.SkillIndex = kWork; }
    return g;
}

}  // namespace model::setup
