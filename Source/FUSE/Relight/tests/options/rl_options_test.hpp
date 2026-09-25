// FUSE Relight RL-0.6 options tests: minimal harness shared by the rl_options suites.
// Checks record failures and keep going; main() returns non-zero when any check failed.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace rl_options_test {

struct Totals {
    int checks = 0;
    int failures = 0;
    const char* currentTest = "";
};

inline Totals& totals() {
    static Totals t;
    return t;
}

inline void recordFailure(const char* file, int line, const std::string& what) {
    ++totals().failures;
    std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", file, line, totals().currentTest, what.c_str());
}

using TestFn = void (*)();
struct TestCase {
    const char* name;
    TestFn fn;
};

template <std::size_t N>
void runSuite(const char* suite, const TestCase (&cases)[N]) {
    for (const TestCase& test : cases) {
        totals().currentTest = test.name;
        const int before = totals().failures;
        test.fn();
        std::printf("[%s] %s: %s\n", suite, test.name, totals().failures == before ? "ok" : "FAILED");
    }
    totals().currentTest = "";
}

// Suites, run in this order by main (later suites build on earlier global state).
void runConfigTests();
void runSemanticsTests();
void runExportTests();
void runSystemTests();

} // namespace rl_options_test

// Variadic so conditions with template-argument commas need no extra parentheses.
#define RL_CHECK(...)                                                                                              \
    do {                                                                                                           \
        ++::rl_options_test::totals().checks;                                                                      \
        if (!(__VA_ARGS__)) {                                                                                      \
            ::rl_options_test::recordFailure(__FILE__, __LINE__, #__VA_ARGS__);                                    \
        }                                                                                                          \
    } while (0)

#define RL_CHECK_MSG(cond, msg)                                                                                    \
    do {                                                                                                           \
        ++::rl_options_test::totals().checks;                                                                      \
        if (!(cond)) {                                                                                             \
            ::rl_options_test::recordFailure(__FILE__, __LINE__, std::string(#cond) + " -- " + (msg));             \
        }                                                                                                          \
    } while (0)

#define RL_CHECK_NEAR(a, b, eps)                                                                                   \
    do {                                                                                                           \
        ++::rl_options_test::totals().checks;                                                                      \
        const double rlA_ = static_cast<double>(a);                                                                \
        const double rlB_ = static_cast<double>(b);                                                                \
        if (!(std::fabs(rlA_ - rlB_) <= static_cast<double>(eps))) {                                               \
            ::rl_options_test::recordFailure(__FILE__, __LINE__,                                                   \
                                             std::string(#a " ~= " #b " (") + std::to_string(rlA_) + " vs " +      \
                                                 std::to_string(rlB_) + ")");                                      \
        }                                                                                                          \
    } while (0)

#define RL_CHECK_STR(a, b)                                                                                         \
    do {                                                                                                           \
        ++::rl_options_test::totals().checks;                                                                      \
        const std::string rlA_ = (a);                                                                              \
        const std::string rlB_ = (b);                                                                              \
        if (rlA_ != rlB_) {                                                                                        \
            ::rl_options_test::recordFailure(__FILE__, __LINE__,                                                   \
                                             std::string(#a " == " #b "\n  got:      [") + rlA_ +                 \
                                                 "]\n  expected: [" + rlB_ + "]");                                  \
        }                                                                                                          \
    } while (0)
