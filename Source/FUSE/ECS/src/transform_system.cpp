#include <fuse/ecs/systems/transform_system.hpp>

#include <fuse/ecs/detail/parallel_iteration.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ecs {

void TransformSystem::recompute_world_matrix(Transform& transform, const mat4& parent_matrix) {
    const mat4 local = from_trs(transform.position, transform.rotation, transform.scale);
    transform.local_to_world = parent_matrix * local;
    transform.world_to_local = inverse_affine(transform.local_to_world);
    transform.dirty = false;
}

bool TransformSystem::should_skip_hierarchy_recompute(const Transform& transform) {
    return !transform.dirty && !transform.parent.valid();
}

bool TransformSystem::subtree_has_dirty_transforms(Registry& reg, EntityID id) {
    const Transform* transform = reg.get<Transform>(id);
    if (transform == nullptr) {
        return false;
    }
    if (transform->dirty) {
        return true;
    }

    bool found = false;
    reg.each<Transform>([&](EntityID child_id, Transform& child) {
        if (child.parent != id) {
            return;
        }
        if (subtree_has_dirty_transforms(reg, child_id)) {
            found = true;
        }
    });
    return found;
}

void TransformSystem::update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix) {
    Transform* transform = reg.get<Transform>(id);
    if (transform == nullptr) {
        return;
    }

    if (!should_skip_hierarchy_recompute(*transform)) {
        recompute_world_matrix(*transform, parent_matrix);
    }

    reg.each<Transform>([&](EntityID child_id, Transform& child) {
        if (child.parent != id) {
            return;
        }
        if (transform->parent.valid() && should_skip_hierarchy_recompute(*transform) &&
            !subtree_has_dirty_transforms(reg, child_id)) {
            return;
        }
        update_hierarchy(reg, child_id, transform->local_to_world);
    });
}

bool TransformSystem::has_any_transforms(Registry& reg) {
    bool found = false;
    reg.each<Transform>([&](EntityID, Transform&) { found = true; });
    return found;
}

u32 TransformSystem::count_transforms(Registry& reg) {
    u32 count = 0;
    reg.each<Transform>([&](EntityID, Transform&) { ++count; });
    return count;
}

bool TransformSystem::has_any_dirty_transforms(Registry& reg) {
    return count_dirty_transforms(reg) > 0;
}

u32 TransformSystem::count_dirty_transforms(Registry& reg) {
    if (!has_any_transforms(reg)) {
        return 0;
    }

    u32 count = 0;
    reg.each<Transform>([&](EntityID, Transform& transform) {
        if (transform.dirty) {
            ++count;
        }
    });
    return count;
}

bool TransformSystem::has_dirty_roots(Registry& reg) {
    return count_dirty_roots(reg) > 0;
}

u32 TransformSystem::count_roots(Registry& reg) {
    if (!has_any_transforms(reg)) {
        return 0;
    }

    u32 count = 0;
    reg.each<Transform>([&](EntityID, Transform& transform) {
        if (!transform.parent.valid()) {
            ++count;
        }
    });
    return count;
}

void TransformSystem::update_dirty_roots_serial(Registry& reg) {
    if (!has_any_transforms(reg) || !has_dirty_roots(reg)) {
        return;
    }

    reg.each<Transform>([&](EntityID, Transform& transform) {
        if (transform.parent.valid() || !transform.dirty) {
            return;
        }

        recompute_world_matrix(transform, mat4::identity());
    });
}

void TransformSystem::update_dirty_roots_parallel(Registry& reg, u32 batchSize) {
    if (!has_any_transforms(reg) || !has_dirty_roots(reg)) {
        return;
    }

    const u32 grain = detail::normalize_batch_size(batchSize);
    reg.each_parallel<Transform>([&](EntityID, Transform& transform) {
        if (transform.parent.valid() || !transform.dirty) {
            return;
        }

        recompute_world_matrix(transform, mat4::identity());
    }, grain);
}

u32 TransformSystem::count_dirty_roots(Registry& reg) {
    if (!has_any_transforms(reg)) {
        return 0;
    }

    u32 count = 0;
    reg.each<Transform>([&](EntityID, Transform& transform) {
        if (!transform.parent.valid() && transform.dirty) {
            ++count;
        }
    });
    return count;
}

void TransformSystem::update(Registry& reg, const TransformSystemOptions& options) {
    if (!has_any_transforms(reg)) {
        return;
    }

    std::vector<EntityID> roots;
    reg.each<Transform>([&](EntityID id, Transform& transform) {
        if (!transform.parent.valid()) {
            roots.push_back(id);
        }
    });

    const bool dirty_roots_before = has_dirty_roots(reg);
    if (dirty_roots_before) {
        if (options.parallelDirtyRoots) {
            update_dirty_roots_parallel(reg, options.batchSize);
        } else {
            update_dirty_roots_serial(reg);
        }
    }

    if (!has_any_dirty_transforms(reg) && !dirty_roots_before) {
        return;
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
