// FUSE renderer pure-logic cores (WP-0.8, PRISM candidates) — shared definitions.
//
// Dialect rules for everything under core_logic/ (so CBMC 5.9x `--cpp11` can parse it and a
// BMC harness can take the code unchanged):
//   * only <stdint.h> / <stddef.h>; no STL, no exceptions, no RTTI, no Vulkan types;
//   * no heap: all storage is fixed-capacity arrays owned by the object (the caller decides
//     where the object lives — static, member, or a one-time allocation outside hot paths);
//   * every loop is bounded by a compile-time capacity or by a count that is itself
//     validated against such a capacity;
//   * no default member initialisers (NSDMI) and no constexpr *function* calls inside
//     constant expressions — CBMC's C++ front end silently mis-handles / rejects them;
//     objects are initialised by explicit reset()/init() calls instead;
//   * no lambdas, no range-for over containers, no nested-namespace shorthand.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(__CPROVER__)
#define FUSE_CL_STATIC_ASSERT(cond, msg)
#else
#define FUSE_CL_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#endif

namespace fuse {
namespace core_logic {

// Test-only friend of the stateful cores; tests define it to corrupt state on purpose so
// the check_invariants() negative paths are exercised. Never defined in production code.
struct ClTestAccess;

static constexpr uint32_t kClInvalid = 0xFFFFFFFFu;

// Status codes shared by all cores. Values are stable (tests and BMC harnesses print them).
enum class ClStatus : uint8_t {
    Ok = 0,
    InvalidArgument = 1,   // index out of range, duplicate declaration, bad alignment, ...
    CapacityExceeded = 2,  // a fixed-capacity container would overflow
    OutOfBudget = 3,       // residency budget cannot hold the request
    NotFound = 4,          // operation on an entry that does not exist / is not in that state
};

inline uint64_t cl_align_up(uint64_t v, uint64_t alignment) {
    // alignment must be a non-zero power of two (callers validate).
    return (v + (alignment - 1u)) & ~(alignment - 1u);
}

inline bool cl_is_pow2(uint64_t v) { return v != 0u && (v & (v - 1u)) == 0u; }

} // namespace core_logic
} // namespace fuse
