// The two "squares" (chip colours) of a colour: the pair the select screens paint next to the colour name, and what
// the DI / Drive "Follow" mode copies. Port of SF6_ColorSpinExtra.lua chip_colors / slot_color / chip_for (lines
// ~388-589) and of the chip_slots.json reading. Pure functions over parsed trees (no REFramework dependency): the
// game's own pair (hGUI.GetFighterCostumeColorData) is handed in by the caller. Checked offline against the shipped
// Lua by tools/rsz_harness/run.ps1 (chips section).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rsz.hpp"

namespace engine::chips {

using Pair = std::pair<uint32_t, uint32_t>;

// table.sort of Lua 5.4 (ltablib.c auxsort, middle pivot): the same order as the Lua for equal elements too, which
// std::sort does not give. Only arrays under 128 elements are deterministic in Lua (no randomized pivot): ours are.
template <class T, class Less>
void lua_sort(std::vector<T>& a, Less less) {
    struct S {
        static void aux(std::vector<T>& v, size_t lo, size_t up, Less& lt) {          // 1-based, inclusive
            auto A = [&](size_t i) -> T& { return v[i - 1]; };
            while (lo < up) {
                if (lt(A(up), A(lo))) std::swap(A(lo), A(up));
                if (up - lo == 1) return;
                size_t p = (lo + up) / 2;
                if (lt(A(p), A(lo))) std::swap(A(p), A(lo));
                else if (lt(A(up), A(p))) std::swap(A(p), A(up));
                if (up - lo == 2) return;
                const T P = A(p);
                std::swap(A(p), A(up - 1));
                size_t i = lo, j = up - 1;
                for (;;) {
                    while (lt(A(++i), P)) if (i == up - 1) return;            // Lua: "invalid order function"
                    while (lt(P, A(--j))) if (j < i) return;
                    if (j < i) { std::swap(A(up - 1), A(i)); p = i; break; }
                    std::swap(A(i), A(j));
                }
                if (p - lo < up - p) { aux(v, lo, p - 1, lt); lo = p + 1; }
                else { aux(v, p + 1, up, lt); up = p - 1; }
            }
        }
    };
    if (a.size() > 1) S::aux(a, 1, a.size(), less);
}

// Lua chip_colors: the first enabled colour of the two part-0 clusters with the most enabled colours, cloth clusters
// first (white when none)
Pair chip_colors(const rsz::Node* tree);

// Lua slot_color: "<cluster>/<n>" = colour n (1-based) of the first cluster of that name, "<cluster>/avg" = the mean of
// its enabled colours. nullopt = nil.
std::optional<uint32_t> slot_color(const rsz::Node* tree, const std::string& spec);

// Lua nearest_slot(tree, rgba): the enabled colour slot closest to rgba (|dR| + |dG| + |dB|, the first one on ties):
// "<cluster>/<k>" (k = 1-based position in its CustomizeColors) and that distance; nullopt = none (the Lua's nil, 1e9).
// learn_chip_slots keeps a pair only when both distances are <= 48.
std::optional<std::pair<std::string, int>> nearest_slot(const rsz::Node* tree, uint32_t rgba);

// data/SF6_ColorSpinExtra_data/chip_slots.json (written by the Lua: learned slots, the editor's saved squares):
//   "<fighter>_<costume>": ["<cluster>/<n>", "<cluster>/<n>"]   the reference slots of that costume
//   "<colour file path>":  [rgba, rgba]                          the squares saved with that colour
class Config {
public:
    void load(const std::filesystem::path& file);         // re-read only when the file changed (size / write time)
    bool load_text(const std::string& json_text);         // offline test
    const Pair* manual(const std::string& path) const;    // chip_cfg[path] = { c1, c2 } (both numbers)
    const std::pair<std::string, std::string>* slots(int fighter, int costume) const;   // both strings
    bool has_key(const std::string& key) const { return keys_.count(key) != 0; }

private:
    uintmax_t size_ = (uintmax_t)-1;
    std::filesystem::file_time_type mtime_{};
    std::map<std::string, Pair> manual_;
    std::map<std::string, std::pair<std::string, std::string>> slots_;
    std::map<std::string, bool> keys_;
};

// Lua chip_for(tree, fighter, costume, entry, num):
//   the pair saved with the entry's file, else (game colour, no entry) the game's own pair, else the costume's
//   reference slots read in this tree, else chip_colors.
// entry_path = nullptr for "no entry"; num = nullopt for nil; game(f, c, num) = dbg.game_chips (nullopt = nil).
using GameChips = std::function<std::optional<Pair>(int fighter, int costume, uint32_t num)>;
Pair chip_for(const rsz::Node* tree, int fighter, int costume, const std::string* entry_path, std::optional<uint32_t> num,
              const Config& cfg, const GameChips& game);

}  // namespace engine::chips
