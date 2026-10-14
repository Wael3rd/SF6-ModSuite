#include "reversal.hpp"

#include "lutil.hpp"

namespace model::reversal {

SetResult on_set(std::optional<int64_t> rt, int64_t row, int64_t type, int64_t skill, bool mapped) {
    SetResult r;
    if (!rt || row < 0 || row > 29) { r.ignored = true; return r; }
    if (type == kRecording && skill >= 8 && skill < kSlotTotal) {
        r.map_set = true;
        r.n = (int)skill + 1;
        r.rewrite_skill = true;
    } else if (mapped) {
        r.map_clear = true;
    }
    return r;
}

Plan present(const Row (&big)[kRowsTotal], const std::function<std::optional<int64_t>(int)>& map_of,
             const std::function<int64_t(int)>& vframes, model::draw::Rng& rng) {
    Plan p;
    std::vector<int> active;
    for (int k = 0; k < kRowsTotal; ++k) if (big[k].present && big[k].valid && big[k].active) active.push_back(k);
    if (!model::draw::has_extra_row(active)) return p;
    p.use = true;
    p.cands = model::draw::choose_rows(active, rng);
    for (int k : p.cands) {
        int n = 0;
        const auto m = map_of(k);                                  // virtual? from the map and OUR row
        if (m && big[k].type == kRecording && big[k].skill == kWork && vframes((int)*m) > 0) { n = (int)*m; p.need_push = true; }
        p.virt.push_back(n);
    }
    return p;
}

std::vector<Change> mapped(const std::map<int64_t, json>& rt_map, const std::function<bool(int, Row*)>& game_row,
                           const std::function<int64_t(int)>& vframes) {
    std::vector<Change> out;
    for (const auto& [row, nj] : rt_map) {
        const auto num = lutil::to_number(nj);
        const auto n = num ? lutil::to_int_key(*num) : std::nullopt;
        if (!n) continue;
        Row d;
        if (row < 0 || row > 0x7FFFFFFF || !game_row((int)row, &d) || !d.present) continue;
        if (d.type == kRecording && d.skill == kWork && vframes((int)*n) > 0) out.push_back({ (int)row, (int)*n });
    }
    return out;
}

}  // namespace model::reversal
