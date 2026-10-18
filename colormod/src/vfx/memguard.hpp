// vfx::memguard - the page check in front of every raw read / write of the Drive colour code (engine, scene).
// VirtualQuery answers for a whole region of pages with the same state; the answer is kept for that region during
// the current game frame (and 50 ms at most: hooks can run on a loading thread while no frame passes), so hundreds
// of reads in one pass cost a handful of VirtualQuery calls instead of one or two each (07/10: 7 to 27 ms per
// relay pass, 1 to 3.5 ms per Drive effect, measured with perf). Raw accesses stay under SEH as before.
#pragma once

#include <cstdint>
#include <cstddef>
#include <map>

namespace vfx::memguard {

int state(uint64_t a);                       // bit 0 readable, bit 1 writable (committed, no guard / no-access page)
bool readable(uint64_t a, size_t n);         // every page of [a, a + n)
uint64_t queries();                          // VirtualQuery calls so far (status)

// The same page check for one background thread (the Drive scan walk): its own region cache, no lock, no frame
// (regions kept kMaxAgeMs of wall time, like state() on a thread without frames). Not shared between threads.
class Local {
public:
    int state(uint64_t a);
private:
    struct Region { uint64_t end; int st; };
    std::map<uint64_t, Region> regions_;
    unsigned long long born_ = 0;
    uint64_t n_ = 0;
};

}  // namespace vfx::memguard
