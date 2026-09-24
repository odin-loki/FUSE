// Tiny test helpers for the core_logic suites (WP-0.8). Test code may use the STL.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace cltest {

inline int& failures() {
    static int n = 0;
    return n;
}

#define CL_CHECK(cond)                                                                          \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            ++::cltest::failures();                                                             \
            if (::cltest::failures() < 50) {                                                    \
                std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            }                                                                                   \
        }                                                                                       \
    } while (0)

// splitmix64: deterministic across platforms.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return n == 0u ? 0u : static_cast<uint32_t>(next() % n); }
    bool chance(uint32_t percent) { return below(100u) < percent; }
};

// Iteration count scale: FUSE_CORE_LOGIC_ITERS (default 1). Sanitizer/coverage runs keep 1.
inline uint32_t iter_scale() {
    const char* e = std::getenv("FUSE_CORE_LOGIC_ITERS");
    if (e == nullptr) {
        return 1u;
    }
    const long v = std::strtol(e, nullptr, 10);
    return v > 0 ? static_cast<uint32_t>(v) : 1u;
}

} // namespace cltest

int run_rg_tests();
int run_vsm_tests();
int run_residency_tests();
