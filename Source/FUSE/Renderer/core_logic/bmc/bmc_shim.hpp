// Bounded-model-checking shim for the WP-0.8 harnesses.
//
// Under CBMC (`__CPROVER__` defined by goto-cc/cbmc) nondet_u32() is an uninterpreted
// nondeterministic function and BMC_ASSUME / BMC_ASSERT map to __CPROVER_assume/assert, so
// the checker explores every input up to the unwinding bound. ESBMC accepts the same
// spelling (it defines __ESBMC__ and understands __CPROVER_* built-ins); pass
// -D__CPROVER__ if needed. Natively (ctest smoke) nondet_u32() is a seeded PRNG,
// BMC_ASSUME skips the run and BMC_ASSERT aborts, and BMC_MAIN runs the harness N times.
#pragma once

#include <stdint.h>

#if defined(__CPROVER__) || defined(__ESBMC__)
extern "C" uint32_t nondet_u32();
#define BMC_ASSUME(c) __CPROVER_assume(c)
#define BMC_ASSERT(c, msg) __CPROVER_assert((c), msg)
#define BMC_MAIN(fn)   \
    int main() {       \
        return fn();   \
    }
#else
#include <stdio.h>
#include <stdlib.h>
static uint64_t g_bmc_rng = 1u;
inline uint32_t nondet_u32() {
    uint64_t z = (g_bmc_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<uint32_t>(z ^ (z >> 31));
}
#define BMC_ASSUME(c)  \
    do {               \
        if (!(c)) {    \
            return 0;  \
        }              \
    } while (0)
#define BMC_ASSERT(c, msg)                                                                     \
    do {                                                                                       \
        if (!(c)) {                                                                            \
            fprintf(stderr, "BMC assertion failed: %s (%s:%d)\n", msg, __FILE__, __LINE__);    \
            abort();                                                                           \
        }                                                                                      \
    } while (0)
#define BMC_MAIN(fn)                                                                           \
    int main(int argc, char** argv) {                                                          \
        const long n = argc > 1 ? strtol(argv[1], 0, 10) : 20000;                              \
        for (long i = 0; i < n; ++i) {                                                         \
            g_bmc_rng = 0x5EEDull + static_cast<uint64_t>(i) * 0x2545F4914F6CDD1Dull;          \
            fn();                                                                              \
        }                                                                                      \
        printf("%s: %ld native runs, all assertions held\n", argv[0], n);                      \
        return 0;                                                                              \
    }
#endif
