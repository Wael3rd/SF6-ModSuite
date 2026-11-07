// R18 in the Color Mod: a game method is hooked only when no other method of the game enters its code (REFramework's hook lands on the code: a method sharing it,
// a folded copy, would run our hook too). The same check as the Slots Loader's (plugin/src/codecheck.hpp, its pure part; plugin/src/codecheck.cpp): a scan over
// every method of the game on a thread of its own, its result cached per game version.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "live_internal.hpp"
#include "../../../plugin/src/codecheck.hpp"

namespace engine::code_check {

using Method = reframework::API::Method;
using Entry = slots::codecheck::Entry;

class Batch {
public:
    void add(Method* m, const std::string& label);   // before start(); nullptr: "missing", never hooked
    void start(const char* cache_file, const char* who);
    bool done() const { return m_state.load(std::memory_order_acquire) != 0; }
    bool failed() const { return m_state.load(std::memory_order_acquire) == 2; }
    bool unique(size_t i) const;                     // found, and no other method enters its code
    const Entry& entry(size_t i) const { return m_found.size() == m_now.size() ? m_found[i] : m_now[i]; }
    Method* method(size_t i) const { return m_methods[i]; }
    size_t size() const { return m_methods.size(); }
    void scan_thread();                              // internal

private:
    std::vector<Method*> m_methods;
    std::vector<Entry> m_now, m_found;
    std::vector<uintptr_t> m_raw, m_code;
    std::string m_key, m_cache, m_who;
    std::atomic<int> m_state{ 0 };                   // 0 pending, 1 done, 2 failed
};

}  // namespace engine::code_check
