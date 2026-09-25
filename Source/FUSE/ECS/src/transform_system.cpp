#include <fuse/ecs/systems/transform_system.hpp>

#include <fuse/ecs/detail/parallel_iteration.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <vector>

namespace fuse::ecs {

void TransformSystem::recompute_world_matrix(Transform& transform, const mat4& parent_matrix) {
    const mat4 local = from_trs(transform.position, transform.rotation, transform.scale);
    transform.local_to_world = parent_matrix * local;
    transform.world_to_local = inverse_affine(transform.local_to_world);
    transform.dirty = false;
}

bool TransformSystem::should_recompute_in_hierarchy(const Transform& transform) {
    return transform.dirty || transform.parent.valid();
}

bool TransformSystem::is_root_transform(const Transform& transform) {
    return !transform.parent.valid();
}

bool TransformSystem::is_dirty_root_transform(const Transform& transform) {
    return is_root_transform(transform) && transform.dirty;
}

bool TransformSystem::should_recompute_dirty_root(const Transform& transform) {
    return is_dirty_root_transform(transform);
}

bool TransformSystem::should_skip_dirty_roots_update(Registry& reg) {
    if (!has_any_transforms(reg)) {
        return true;
    }

    return !has_dirty_roots(reg);
}

bool TransformSystem::should_skip_hierarchy_subtree(Registry& reg, EntityID id) {
    return !subtree_has_dirty(reg, id);
}

bool TransformSystem::subtree_has_dirty(Registry& reg, EntityID id) {
    const Transform* transform = reg.get<Transform>(id);
    if (transform == nullptr) {
        return false;
    }

    if (transform->dirty) {
        return true;
    }

    bool found = false;
    reg.each<Transform>([&](EntityID child_id, Transform& child) {
        if (found || child.parent != id) {
            return;
        }

        found = subtree_has_dirty(reg, child_id);
    });
    return found;
}

bool TransformSystem::has_dirty_transforms(Registry& reg) {
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

bool TransformSystem::should_skip_hierarchy_update(Registry& reg) {
    if (!has_any_transforms(reg)) {
        return true;
    }

    return !has_dirty_transforms(reg);
}

void TransformSystem::update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix) {
    Transform* transform = reg.get<Transform>(id);
    if (transform == nullptr) {
        return;
    }

    if (should_recompute_in_hierarchy(*transform)) {
        recompute_world_matrix(*transform, parent_matrix);
    }

    reg.each<Transform>([&](EntityID child_id, Transform& child) {
        if (child.parent != id) {
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
    if (should_skip_dirty_roots_update(reg)) {
        return;
    }

    reg.each<Transform>([&](EntityID, Transform& transform) {
        if (!should_recompute_dirty_root(transform)) {
            return;
        }

        recompute_world_matrix(transform, mat4::identity());
    });
}

void TransformSystem::update_dirty_roots_parallel(Registry& reg, u32 batchSize) {
    if (should_skip_dirty_roots_update(reg)) {
        return;
    }

    const u32 grain = detail::normalize_batch_size(batchSize);
    reg.each_parallel<Transform>([&](EntityID, Transform& transform) {
        if (!should_recompute_dirty_root(transform)) {
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
        if (is_dirty_root_transform(transform)) {
            ++count;
        }
    });
    return count;
}

void TransformSystem::update(Registry& reg, const TransformSystemOptions& options) {
    // 1. Dirty roots (no parent) — independent, so parallel when requested. Clean roots are skipped
    //    inside the pass; no separate "any dirty?" scans.
    if (options.parallelDirtyRoots) {
        const u32 grain = detail::normalize_batch_size(options.batchSize);
        reg.each_parallel<Transform>([](EntityID, Transform& transform) {
            if (is_dirty_root_transform(transform)) {
                recompute_world_matrix(transform, mat4::identity());
            }
        }, grain);
    } else {
        reg.each<Transform>([](EntityID, Transform& transform) {
            if (is_dirty_root_transform(transform)) {
                recompute_world_matrix(transform, mat4::identity());
            }
        });
    }

    // 2. Hierarchy: one pass collects (parent, child) links, sorted by parent, so each subtree walk
    //    finds its children by binary search instead of rescanning every transform per node
    //    (that rescan made updates O(roots * n)).
    struct Link {
        EntityID parent;
        EntityID child;
    };
    std::vector<Link> links;
    reg.each<Transform>([&](EntityID id, Transform& transform) {
        if (transform.parent.valid()) {
            links.push_back({transform.parent, id});
        }
    });
    if (links.empty()) {
        return;
    }
    auto byParent = [](const Link& a, const Link& b) {
        return a.parent.index != b.parent.index ? a.parent.index < b.parent.index
                                                : a.parent.generation < b.parent.generation;
    };
    std::sort(links.begin(), links.end(), byParent);

    std::vector<EntityID> stack;
    for (const Link& link : links) {
        const Transform* parent = reg.get<Transform>(link.parent);
        if (parent == nullptr || parent->parent.valid()) {
            continue; // start walks only at root parents; deeper links are reached from their root
        }
        // Each root appears once per child link; walk it only from its first link.
        if (&link != &links.front() && (&link)[-1].parent == link.parent) {
            continue;
        }
        stack.clear();
        stack.push_back(link.parent);
        while (!stack.empty()) {
            const EntityID node = stack.back();
            stack.pop_back();
            const Transform* nodeTransform = reg.get<Transform>(node);
            if (nodeTransform == nullptr) {
                continue;
            }
            const mat4 parentMatrix = nodeTransform->local_to_world;
            const auto range = std::equal_range(links.begin(), links.end(), Link{node, EntityID::null()}, byParent);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->parent != node) {
                    continue;
                }
                if (Transform* child = reg.get<Transform>(it->child)) {
                    recompute_world_matrix(*child, parentMatrix); // children always follow their parent
                    stack.push_back(it->child);
                }
            }
        }
    }
}

} // namespace fuse::ecs
