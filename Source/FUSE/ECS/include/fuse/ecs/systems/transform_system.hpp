#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {

struct TransformSystemOptions {
    bool parallelDirtyRoots = true;
    u32 batchSize = 256;
};

class TransformSystem {
public:
    static void update(Registry& reg, const TransformSystemOptions& options = {});

private:
    static void update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix);
    static void update_dirty_roots_serial(Registry& reg);
    static void update_dirty_roots_parallel(Registry& reg, u32 batchSize);
};

} // namespace fuse::ecs
