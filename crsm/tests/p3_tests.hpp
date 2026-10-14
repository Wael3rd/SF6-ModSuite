// p3_tests — the pure tests of phase P3 (replay, reversal, record), run by p3_bench (no game, no data).
#pragma once

#include <functional>
#include <string>

namespace p3 {
void begin(const std::string& name);
void check(bool ok, const std::string& what, const std::function<std::string()>& detail = nullptr);
void run_pure_tests();
}  // namespace p3
