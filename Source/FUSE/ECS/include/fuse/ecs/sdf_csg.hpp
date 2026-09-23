#pragma once

#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ecs {

class Registry;

/// Distance scene value before any object applies ("nothing here"); finite so smooth ops stay
/// NaN-free.
static constexpr f32 kSdfEmptyDistance = 1e20f;

/// Signed distance of a primitive in its local space (params per `SDFPrimitive`).
f32 sdf_primitive_distance(SDFPrimitive type, const vec3& params, const vec3& local);

/// Deterministic, continuous surface noise in [-1, 1] used by `SDFObject::roughness`.
f32 sdf_roughness_noise(const vec3& local);

/// Signed distance of `sdf` placed by `transform` (own TRS, no parent chain), including roughness
/// displacement. Non-uniform scale uses the smallest axis (conservative bound).
f32 sdf_object_distance(const SDFObject& sdf, const Transform& transform, const vec3& world);

/// Polynomial smooth minimum: continuous, <= min(a, b), equal to min(a, b) when |a - b| >= k,
/// never more than k / 4 below it. k <= 0 is a hard min.
f32 sdf_smooth_min(f32 a, f32 b, f32 k);

/// scene' = op(scene, d).
f32 sdf_csg_apply(SDFCsgOp op, f32 scene, f32 d, f32 blend_radius);

/// Smooth-union blend width actually applied for `sdf`: `blend_radius` scaled by the GRIA alpha,
/// k = blend_radius * blend_alpha / kSdfDefaultBlendAlpha. alpha = 0 (exact) is a hard union, the
/// default alpha (0.5, edge of chaos) applies `blend_radius` as authored, alpha = 1 (approximate)
/// doubles it. Alpha is clamped to [0, 1] (NaN -> 0). Non-smooth ops return 0.
f32 sdf_effective_blend_radius(const SDFObject& sdf);

/// Conservative local bounding radius (params, roughness and smooth-union bulge included).
f32 sdf_bounding_radius(const SDFObject& sdf);

struct SdfCsgSample {
    f32 distance = kSdfEmptyDistance;
    u32 material_id = 0;
    EntityID entity = EntityID::null(); ///< object that defines the surface nearest the sample
};

/// CPU reference evaluator for the CSG scene formed by every visible SDFObject + Transform:
/// objects fold in ascending (`csg_order`, entity index) order via `sdf_csg_apply`. Used by the
/// editor sculpt panel and gate tests; the GPU path must match it.
class SdfCsgScene {
public:
    struct Entry {
        EntityID entity = EntityID::null();
        SDFObject sdf{};
        Transform transform{};
    };

    void build(Registry& registry);
    [[nodiscard]] SdfCsgSample sample(const vec3& world) const;
    [[nodiscard]] f32 distance(const vec3& world) const { return sample(world).distance; }
    [[nodiscard]] const std::vector<Entry>& entries() const { return m_entries; }
    /// One past the largest `csg_order` present (0 for an empty scene).
    [[nodiscard]] u32 next_csg_order() const;

private:
    std::vector<Entry> m_entries;
};

} // namespace fuse::ecs
