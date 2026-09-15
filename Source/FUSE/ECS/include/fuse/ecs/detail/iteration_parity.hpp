#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/detail/parallel_iteration.hpp>
#include <fuse/ecs/registry.hpp>

#include <atomic>

namespace fuse::ecs::detail {

/// Count entities visited by serial `each_query`.
template <typename... WithTs>
u32 count_each_query(Registry& reg) {
    u32 count = 0;
    reg.each_query<WithTs...>([&](EntityID, WithTs&...) { ++count; });
    return count;
}

/// Count entities visited by `each_query_parallel`.
template <typename... WithTs>
u32 count_each_query_parallel(Registry& reg, u32 batchSize) {
    std::atomic<u32> count{0};
    reg.each_query_parallel<WithTs...>([&](EntityID, WithTs&...) {
        count.fetch_add(1u, std::memory_order_relaxed);
    }, normalize_batch_size(batchSize));
    return count.load(std::memory_order_relaxed);
}

/// Returns true when serial and parallel query iteration visit the same entity count.
template <typename... WithTs>
bool each_query_parallel_matches_serial(Registry& reg, u32 batchSize) {
    return count_each_query<WithTs...>(reg) == count_each_query_parallel<WithTs...>(reg, batchSize);
}

[[nodiscard]] inline bool transform_matrices_equal(const Transform& lhs, const Transform& rhs) {
    for (u32 i = 0; i < 16; ++i) {
        if (lhs.local_to_world.data[i] != rhs.local_to_world.data[i]) {
            return false;
        }
        if (lhs.world_to_local.data[i] != rhs.world_to_local.data[i]) {
            return false;
        }
    }
    return lhs.dirty == rhs.dirty;
}

} // namespace fuse::ecs::detail
