#include <fuse/ecs/systems/transform_system.hpp>

#include <fuse/types.hpp>

#include <vector>

namespace fuse::ecs {

u32 TransformSystem::normalize_batch_size(u32 batchSize) {
    return batchSize == 0 ? 1u : batchSize;
}

void TransformSystem::recompute_world_matrix(Transform& transform, const mat4& parent_matrix) {
    const mat4 local = from_trs(transform.position, transform.rotation, transform.scale);
    transform.local_to_world = parent_matrix * local;
    transform.world_to_local = inverse_affine(transform.local_to_world);
    transform.dirty = false;
}

void TransformSystem::update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix) {
    Transform* transform = reg.get<Transform>(id);
    if (transform == nullptr) {
        return;
    }

    if (transform->dirty || transform->parent.valid()) {
        recompute_world_matrix(*transform, parent_matrix);
    }

    reg.each<Transform>([&](EntityID child_id, Transform& child) {
        if (child.parent == id) {
            update_hierarchy(reg, child_id, transform->local_to_world);
        }
    });
}

void TransformSystem::update_dirty_roots_serial(Registry& reg) {
    reg.each<Transform>([&](EntityID, Transform& transform) {
        if (transform.parent.valid() || !transform.dirty) {
            return;
        }

        recompute_world_matrix(transform, mat4::identity());
    });
}

void TransformSystem::update_dirty_roots_parallel(Registry& reg, u32 batchSize) {
    const u32 grain = normalize_batch_size(batchSize);
    reg.each_parallel<Transform>([&](EntityID, Transform& transform) {
        if (transform.parent.valid() || !transform.dirty) {
            return;
        }

        recompute_world_matrix(transform, mat4::identity());
    }, grain);
}

void TransformSystem::update(Registry& reg, const TransformSystemOptions& options) {
    std::vector<EntityID> roots;
    reg.each<Transform>([&](EntityID id, Transform& transform) {
        if (!transform.parent.valid()) {
            roots.push_back(id);
        }
    });

    if (options.parallelDirtyRoots) {
        update_dirty_roots_parallel(reg, options.batchSize);
    } else {
        update_dirty_roots_serial(reg);
    }

    for (EntityID root : roots) {
        Transform* transform = reg.get<Transform>(root);
        if (transform == nullptr) {
            continue;
        }
        update_hierarchy(reg, root, mat4::identity());
    }
}

} // namespace fuse::ecs
