// The Color row's Random value (include/sf6colormod_entries.h): one more value after a list's RW / MC entries, on colour 02
// like them, while the Slots Loader draws Random colours (SF6Slots_RandomColour). Remembered where an entry is (state.json p1 /
// p2 / fs) under a rank no list of entries reaches, so a new pack (which moves the entries' ranks) never moves it, and under
// the identity "random"; worn as nothing of ours (the plain colour 02, or its edited copy). Pure: tests/random_value_test.cpp.
#pragma once

namespace engine::random_value {

constexpr int kK = 1000;                  // its rank in a remembered selection ("rw"); entry ranks are 1..count
constexpr const char* kIdent = "random";  // its identity ("path")

// The rank of the value at `idx` of a list whose entries start at `first` (`count` of them) and whose Random value is at
// `at` (-1: none): 1..count an entry, kK the Random value, 0 one of the game's colours
inline int rank_at(int idx, int first, int count, int at) {
    if (at >= 0 && idx == at) return kK;
    if (first < 0 || idx < first || idx >= first + count) return 0;
    return idx - first + 1;
}
// The index of rank `k` in that list, -1 when it has none
inline int index_of(int k, int first, int count, int at) {
    if (k == kK) return at;
    if (first < 0 || k < 1 || k > count) return -1;
    return first + k - 1;
}

}  // namespace engine::random_value
