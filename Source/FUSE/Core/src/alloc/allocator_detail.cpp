#include <fuse/alloc/allocator.hpp>

namespace fuse::alloc::detail {

usize alignUp(usize value, usize alignment) {
    if (alignment <= 1u) {
        return value;
    }
    const usize mask = alignment - 1u;
    return (value + mask) & ~mask;
}

} // namespace fuse::alloc::detail
