// perf_hook: per-method timing wrappers for game hooks, plus the "declaring type" journal line.
// timed_pre<Id, fn> / timed_post<Id, fn> are outer wrappers (no __try, no object with a destructor):
// the wrapped hook function is untouched, one Id counts exactly one hooked game method.
#pragma once

#include <reframework/API.hpp>

#include "common.hpp"
#include "perf.hpp"

namespace perf {

template <Id ID, int (*F)(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long)>
int timed_pre(int argc, void** argv, REFrameworkTypeDefinitionHandle* th, unsigned long long u) {
    const int64_t t0 = begin();
    const int r = F(argc, argv, th, u);
    end(ID, t0);
    return r;
}

template <Id ID, void (*F)(void**, REFrameworkTypeDefinitionHandle, unsigned long long)>
void timed_post(void** ret, REFrameworkTypeDefinitionHandle th, unsigned long long u) {
    const int64_t t0 = begin();
    F(ret, th, u);
    end(ID, t0);
}

// one journal line per hooked method: which type really declares it (a base class would be shared by every UI object)
inline void journal_hooked(const char* what, reframework::API::Method* m) {
    auto* dt = m ? m->get_declaring_type() : nullptr;
    cmn::journal("hook %s: declaring type %s", what, dt ? dt->get_full_name().c_str() : "?");
}

}  // namespace perf
