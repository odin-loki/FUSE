#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {

struct TransformSystemOptions {
    /// When true, dirty root transforms (no parent) recompute via `each_parallel`.
    bool parallelDirtyRoots = true;
    /// Grain size forwarded to `Registry::each_parallel` for the dirty-root pass.
    u32 batchSize = 256;
};

class TransformSystem {
public:
    static void update(Registry& reg, const TransformSystemOptions& options = {});

    /// CPU stub: recompute `local_to_world` / `world_to_local` from TRS relative to `parent_matrix`.
    static void recompute_world_matrix(Transform& transform, const mat4& parent_matrix);

    /// CPU stub: serial dirty-root world-matrix pass (entities with no parent).
    static void update_dirty_roots_serial(Registry& reg);

    /// CPU stub: parallel dirty-root world-matrix pass (entities with no parent).
    static void update_dirty_roots_parallel(Registry& reg, u32 batchSize);

private:
    static void update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix);
};

} // namespace fuse::ecs
