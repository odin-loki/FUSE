#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/detail/parallel_iteration.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/transform_system.hpp>

#include <atomic>
#include <unordered_map>

namespace fuse::ecs::detail {

/// Count entities visited by serial `each`.
template <typename... Ts>
u32 count_each(Registry& reg) {
    u32 count = 0;
    reg.each<Ts...>([&](EntityID, Ts&...) { ++count; });
    return count;
}

/// Count entities visited by `each_parallel`.
template <typename... Ts>
u32 count_each_parallel(Registry& reg, u32 batchSize) {
    std::atomic<u32> count{0};
    reg.each_parallel<Ts...>([&](EntityID, Ts&...) {
        count.fetch_add(1u, std::memory_order_relaxed);
    }, normalize_batch_size(batchSize));
    return count.load(std::memory_order_relaxed);
}

/// Returns true when serial and parallel `each` iteration visit the same entity count.
template <typename... Ts>
bool each_parallel_matches_serial(Registry& reg, u32 batchSize) {
    return count_each<Ts...>(reg) == count_each_parallel<Ts...>(reg, batchSize);
}

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

/// Compare every transform in two registries keyed by entity index.
[[nodiscard]] inline bool transform_registries_match(Registry& lhs, Registry& rhs) {
    std::unordered_map<u32, Transform> lhsByIndex;
    lhs.each<Transform>([&](EntityID id, Transform& transform) { lhsByIndex[id.index] = transform; });

    usize rhsCount = 0;
    bool matched = true;
    rhs.each<Transform>([&](EntityID id, Transform& transform) {
        ++rhsCount;
        const auto it = lhsByIndex.find(id.index);
        if (it == lhsByIndex.end() || !transform_matrices_equal(transform, it->second)) {
            matched = false;
        }
    });
    return matched && lhsByIndex.size() == rhsCount;
}

/// Run dirty-root serial/parallel stubs on cloned registries and compare matrices.
/// Caller must initialize `JobScheduler` before calling when `batchSize` uses workers.
[[nodiscard]] inline bool transform_dirty_roots_serial_parallel_match(Registry& serialReg,
                                                                      Registry& parallelReg,
                                                                      u32 batchSize) {
    TransformSystem::update_dirty_roots_serial(serialReg);
    TransformSystem::update_dirty_roots_parallel(parallelReg, batchSize);
    return transform_registries_match(serialReg, parallelReg);
}

} // namespace fuse::ecs::detail
