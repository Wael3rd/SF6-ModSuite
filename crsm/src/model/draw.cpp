#include "draw.hpp"

#include <utility>

namespace model::draw {

std::vector<Candidate> candidates(const LiveSlot (&live)[8], bool forced8, const std::vector<int>& virtual_checked,
                                  const std::function<double(int)>& virtual_weight) {
    std::vector<Candidate> c;
    for (int k0 = 0; k0 < 8; ++k0) {
        const LiveSlot& sd = live[k0];
        const bool own8 = (k0 == kWorkSlot) && !virtual_checked.empty();
        if (sd.present && !own8 && sd.active && sd.valid && !(k0 == kWorkSlot && forced8))
            c.push_back({ k0 + 1, sd.weight > 1 ? sd.weight : 1 });
    }
    for (int n : virtual_checked) c.push_back({ n, virtual_weight(n) });
    return c;
}

std::optional<int> pick(const std::vector<Candidate>& cands, double r01) {
    if (cands.empty()) return std::nullopt;
    double total = 0;
    for (const Candidate& c : cands) total += c.w;
    double r = r01 * total;
    for (const Candidate& c : cands) {
        r -= c.w;
        if (r <= 0) return c.n;
    }
    return cands.back().n;
}

std::vector<int> choose_rows(std::vector<int> active, Rng& rng) {
    if ((int)active.size() > kRowsPage) {
        for (int i = (int)active.size(); i >= 2; --i) {          // for i = #cands, 2, -1: j = math.random(i); swap(i, j) (1-based)
            const int j = (int)rng.below(i);
            std::swap(active[(size_t)i - 1], active[(size_t)j - 1]);
        }
        while ((int)active.size() > kRowsPage) active.pop_back();
    }
    return active;
}

bool has_extra_row(const std::vector<int>& active) {
    for (int k : active) if (k >= kRowsPage) return true;
    return false;
}

}  // namespace model::draw
