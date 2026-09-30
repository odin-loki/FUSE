#pragma once

// Generic signed-distance sampler for SdfMesh collision shapes (UNI-B4-VOX-1). Contacts and casts only
// ever call `distance` (body frame, negative inside) and `gradient`, so any field plugs in: the analytic
// shapes below, a Scene SVO's interpolated distance samples (SvoSdfSampler), and later the A-SDF
// `.fusesdf` bricks. Samplers are immutable once registered in the shape pool and are read concurrently.

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <memory>

namespace fuse::scene {
class SVO;
}

namespace fuse::physics {

class SdfSampler {
public:
    virtual ~SdfSampler() = default;

    /// Signed distance at a body-frame point (negative inside). Casts treat it as a lower bound of
    /// the true distance (1-Lipschitz), so conservative fields are fine.
    [[nodiscard]] virtual f32 distance(vec3 p) const = 0;
    /// Body-frame bounds of the surface (anything outside is at positive distance).
    [[nodiscard]] virtual vec3 boundsMin() const = 0;
    [[nodiscard]] virtual vec3 boundsMax() const = 0;
    /// Central-difference step for `gradient`.
    [[nodiscard]] virtual f32 gradientStep() const { return 1e-3f; }
    /// Unnormalised gradient (central differences of `distance`).
    [[nodiscard]] virtual vec3 gradient(vec3 p) const;
};

enum class AnalyticSdfKind : u32 {
    Sphere = 0,     ///< params.x = radius
    Box = 1,        ///< params = half extents, rounding = corner radius (0: sharp)
    Capsule = 2,    ///< params.x = radius, params.y = half height (local Y)
    Torus = 3,      ///< params.x = major radius, params.y = minor radius (ring in the XZ plane)
    Cylinder = 4,   ///< params.x = radius, params.y = half height (local Y)
};

/// Exact analytic distance fields.
class AnalyticSdf final : public SdfSampler {
public:
    AnalyticSdf(AnalyticSdfKind kind, vec3 params, f32 rounding = 0.f);

    [[nodiscard]] f32 distance(vec3 p) const override;
    [[nodiscard]] vec3 boundsMin() const override;
    [[nodiscard]] vec3 boundsMax() const override;
    [[nodiscard]] vec3 gradient(vec3 p) const override;

    [[nodiscard]] AnalyticSdfKind kind() const { return m_kind; }
    [[nodiscard]] vec3 params() const { return m_params; }

private:
    AnalyticSdfKind m_kind;
    vec3 m_params;
    f32 m_rounding;
};

/// Any callable distance (CSG trees, test fields). Must be thread-safe.
class FunctionSdf final : public SdfSampler {
public:
    FunctionSdf(std::function<f32(vec3)> field, vec3 boundsMin, vec3 boundsMax, f32 gradientStep = 1e-3f);

    [[nodiscard]] f32 distance(vec3 p) const override { return m_field(p); }
    [[nodiscard]] vec3 boundsMin() const override { return m_min; }
    [[nodiscard]] vec3 boundsMax() const override { return m_max; }
    [[nodiscard]] f32 gradientStep() const override { return m_step; }

private:
    std::function<f32(vec3)> m_field;
    vec3 m_min;
    vec3 m_max;
    f32 m_step;
};

/// The Scene SVO as a distance field (body frame = the SVO's world frame): its trilinearly interpolated
/// distance samples (`SVO::sdfQuery`) inside the narrow band it stores (about half a voxel around the
/// written voxels), the exact distance to the nearest solid voxel beyond it (searched up to 8 voxels away,
/// a lower bound past that). Penetrations deeper than the band saturate. The SVO is shared-owned.
class SvoSdfSampler final : public SdfSampler {
public:
    SvoSdfSampler(std::shared_ptr<const scene::SVO> svo, vec3 boundsMin, vec3 boundsMax);

    [[nodiscard]] f32 distance(vec3 p) const override;
    [[nodiscard]] vec3 boundsMin() const override { return m_min; }
    [[nodiscard]] vec3 boundsMax() const override { return m_max; }
    [[nodiscard]] f32 gradientStep() const override { return m_step; }

private:
    std::shared_ptr<const scene::SVO> m_svo;
    vec3 m_min;
    vec3 m_max;
    f32 m_step;
};

} // namespace fuse::physics
