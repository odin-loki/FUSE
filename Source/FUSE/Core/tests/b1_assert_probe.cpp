// Codegen probe for the B1.8 gate "FUSE_ASSERT fires and breaks in debug, is a no-op in release —
// verified by disassembly". check_b1_assert_codegen.cmake inspects this object's relocations: a
// Debug object must reference fuse::assertion::fatal, any other configuration must not.
#include <fuse/assert.hpp>

extern "C" int fuse_b1_assert_probe(int value) {
    FUSE_ASSERT(value > 0, "fuse_b1_assert_probe: value must be positive");
    FUSE_PRECONDITION(value < 1000000);
    return value * 2;
}
