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

    /// Count root transforms (`parent` invalid) with `dirty == true`.
    [[nodiscard]] static u32 count_dirty_roots(Registry& reg);

    /// Returns true when at least one root transform has `dirty == true`.
    [[nodiscard]] static bool has_dirty_roots(Registry& reg);

    /// Count root transforms (`parent` invalid), regardless of `dirty`.
    [[nodiscard]] static u32 count_roots(Registry& reg);

    /// Count transforms with `dirty == true` (roots and children).
    [[nodiscard]] static u32 count_dirty_transforms(Registry& reg);

    /// Returns true when at least one transform has `dirty == true`.
    [[nodiscard]] static bool has_dirty_transforms(Registry& reg);

    /// Returns true when the hierarchy walk can be skipped (empty registry or all transforms clean).
    [[nodiscard]] static bool should_skip_hierarchy_update(Registry& reg);

    /// Returns true when `id` or any descendant transform has `dirty == true`.
    [[nodiscard]] static bool subtree_has_dirty(Registry& reg, EntityID id);

    /// Returns true when a hierarchy visit should recompute world matrices for `transform`.
    [[nodiscard]] static bool should_recompute_in_hierarchy(const Transform& transform);

    /// Returns true when `transform` has no parent entity.
    [[nodiscard]] static bool is_root_transform(const Transform& transform);

    /// Returns true when `transform` is a root with `dirty == true`.
    [[nodiscard]] static bool is_dirty_root_transform(const Transform& transform);

    /// Returns true when the dirty-root pass should recompute `transform`.
    [[nodiscard]] static bool should_recompute_dirty_root(const Transform& transform);

    /// Returns true when dirty-root serial/parallel passes can early-out.
    [[nodiscard]] static bool should_skip_dirty_roots_update(Registry& reg);

    /// Returns true when a hierarchy subtree walk can be skipped for `id`.
    [[nodiscard]] static bool should_skip_hierarchy_subtree(Registry& reg, EntityID id);

    /// Returns false when the registry has no `Transform` components (empty-transform guard).
    [[nodiscard]] static bool has_any_transforms(Registry& reg);

    /// Count entities with a `Transform` component.
    [[nodiscard]] static u32 count_transforms(Registry& reg);

private:
    static void update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix);
};

} // namespace fuse::ecs
