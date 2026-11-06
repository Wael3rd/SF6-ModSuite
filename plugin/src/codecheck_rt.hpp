// SF6 Slots — R18 code check of several methods (codecheck.hpp), game side: a scan over every method of the game on a thread
// of its own, its result cached per game version. Used by stages.cpp for its stage-grid hooks.
#pragma once

#include "slots.hpp"
#include "codecheck.hpp"

namespace slots::codecheck {

class Batch {
public:
    // Methods to check (nullptr ones are kept as "missing", never hooked). Before start().
    void add(Method* m, const std::string& label);
    // From the cache file when it matches, else a scan on a thread of its own. `who`: the module, for the log.
    void start(const char* cache_file, const char* who);
    bool done() const;                    // the verdicts are known (or the check failed)
    bool failed() const;
    bool unique(size_t i) const;          // method i was found and no other method enters its code
    size_t size() const { return m_methods.size(); }
    Method* method(size_t i) const { return m_methods[i]; }
    const Entry& entry(size_t i) const { return m_found.size() == m_now.size() ? m_found[i] : m_now[i]; }
    std::string other_label(size_t i) const;   // one of the methods sharing method i's code
    // One log line about the check (where the verdicts came from), once, after done().
    void log_summary() const;

    // internal (the scan thread)
    void scan_thread();

private:
    std::vector<Method*> m_methods;
    std::vector<Entry> m_now, m_found;
    std::vector<uintptr_t> m_raw, m_code;
    std::string m_key, m_cache, m_who;
    const char* m_why = "";
    bool m_from_cache = false, m_written = false;
    uint64_t m_ms = 0;
    uint32_t m_scanned = 0;
    std::atomic<int> m_state{ 0 };       // 0 pending, 1 done, 2 failed
};

}  // namespace slots::codecheck
