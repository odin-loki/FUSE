#pragma once

// Ore: Engine/source/afx/afxMagicMissile.h (afxMagicMissileData projectile fields)

#include <fuse/fx/fx_defs.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <span>
#include <string>
#include <vector>

namespace fuse::fx {

class ResidualEffectQueue;

/// Projectile datablock distilled from `afxMagicMissileData`.
struct MissileDescriptor {
    std::string id;
    float muzzleVelocity = 20.f;
    bool isBallistic = false;
    float gravityMod = 1.f;
    float lifetime = 5.f;
    float fadeDelay = 0.f;

    static MissileDescriptor makeFireballMissile();
};

/// Caller-owned analytic collider. Spheres and AABBs only — no physics-engine link.
enum class MissileColliderShape : u8 {
    Sphere = 0,
    Aabb,
};

struct MissileCollider {
    MissileColliderShape shape = MissileColliderShape::Sphere;
    /// Sphere center. Unused for AABBs.
    fuse::math::Vec3 center{};
    /// Sphere radius. Negative or non-finite radii never hit.
    float radius = 0.f;
    /// Inclusive AABB corners. A hit requires `aabbMin` <= `aabbMax` on every axis.
    fuse::math::Vec3 aabbMin{};
    fuse::math::Vec3 aabbMax{};

    static MissileCollider makeSphere(const fuse::math::Vec3& sphereCenter, float sphereRadius) {
        MissileCollider collider;
        collider.shape = MissileColliderShape::Sphere;
        collider.center = sphereCenter;
        collider.radius = sphereRadius;
        return collider;
    }

    static MissileCollider makeAabb(const fuse::math::Vec3& minCorner, const fuse::math::Vec3& maxCorner) {
        MissileCollider collider;
        collider.shape = MissileColliderShape::Aabb;
        collider.aabbMin = minCorner;
        collider.aabbMax = maxCorner;
        return collider;
    }
};

/// One active projectile instance (ore: `afxMagicMissile` runtime state).
struct MissileInstance {
    MissileDescriptor descriptor{};
    fuse::math::Vec3 position{};
    fuse::math::Vec3 velocity{};
    float elapsed = 0.f;
    bool finished = false;
    /// Set when a collider stops the missile. Lifetime expiry leaves this false.
    bool impacted = false;
    /// Lowest index wins when two colliders share the same hit time.
    u32 impactColliderIndex = 0;
    fuse::math::Vec3 impactPoint{};
    fuse::math::Vec3 impactNormal{};
};

/// Game-thread projectile sim. Linear / ballistic step, then a swept-segment test
/// against caller-provided spheres and AABBs (no T3D / Bullet world).
class MissilePipeline {
public:
    void fire(const MissileDescriptor& descriptor,
              const fuse::math::Vec3& origin,
              const fuse::math::Vec3& direction);

    /// Integrate one step, then test the segment against `colliders`.
    /// The earliest t in [0, 1] stops the missile and records the impact point and outward normal.
    /// Exact t ties keep the lowest collider index. Grazing and zero-length segments stay finite.
    /// A non-null `residuals` enqueues one zodiac on that existing queue; null only records the hit.
    /// An empty collider span flies until lifetime expiry.
    void tick(float dt,
              std::span<const MissileCollider> colliders = {},
              ResidualEffectQueue* residuals = nullptr);

    u32 activeCount() const;
    u32 completedCount() const { return m_completedCount; }
    const std::vector<MissileInstance>& instances() const { return m_instances; }

private:
    std::vector<MissileInstance> m_instances;
    u32 m_completedCount = 0;
};

} // namespace fuse::fx
