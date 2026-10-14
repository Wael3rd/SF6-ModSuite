// model::draw — the random choices: the weighted draw among the checked slots 1-30 (RMT:1045-1066 full_pick) and the
// random ten among the active reversal rows (RSL:553-583 present_rows). Pure: the random numbers come from the caller,
// so the Lua and the C++ can be compared on the same sequence and the production source can be seeded.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace model::draw {

// A source of random numbers. next01() is math.random() (a float in [0, 1)); below(n) is math.random(n) (1..n).
struct Rng {
    virtual ~Rng() = default;
    virtual double next01() = 0;
    virtual int64_t below(int64_t n) = 0;
};

// Production source: mt19937_64, seeded once (os.time() in the Lua; the sequence differs, the distribution does not).
class MtRng final : public Rng {
public:
    explicit MtRng(uint64_t seed) : gen_(seed) {}
    double next01() override { return std::uniform_real_distribution<double>(0.0, 1.0)(gen_); }
    int64_t below(int64_t n) override { return n < 1 ? 1 : std::uniform_int_distribution<int64_t>(1, n)(gen_); }
private:
    std::mt19937_64 gen_;
};

// A live slot as the draw sees it (k0 = 0..7).
struct LiveSlot { bool present = false; bool active = false; bool valid = false; double weight = 1; };
struct Candidate { int n; double w; };       // slot number 1..30 and its weight (>= 1)

constexpr int kWorkSlot = 7;                 // live slot 8 (0-based): stands for the checked virtual slots

// full_pick's candidates: the live slots that are active and valid (math.max(1, weight)), slot 8 left out when a virtual
// slot is checked (it stands for them) or when it is forced (forced8); then the checked virtual slots with their weights.
// `virtual_checked` is virtual_checked() sorted; `virtual_weight(n)` is vweight(n).
std::vector<Candidate> candidates(const LiveSlot (&live)[8], bool forced8, const std::vector<int>& virtual_checked,
                                  const std::function<double(int)>& virtual_weight);

// The pick: r = next01() * total, then the first candidate that brings it to 0 or below; the last one when none does.
std::optional<int> pick(const std::vector<Candidate>& cands, double r01);
inline std::optional<int> pick(const std::vector<Candidate>& cands, Rng& rng) { return cands.empty() ? std::nullopt : pick(cands, rng.next01()); }

// present_rows: `active` = the indices (0-29) of the valid and active rows, ascending. When more than kRowsPage, a random
// ten of them (Fisher-Yates over the list, then the tail dropped); the order of the result is the shuffled order.
constexpr int kRowsPage = 10;
std::vector<int> choose_rows(std::vector<int> active, Rng& rng);

// The trigger only presents the work array when a row >= 10 is valid and active (RSL:555-557).
bool has_extra_row(const std::vector<int>& active);

}  // namespace model::draw
