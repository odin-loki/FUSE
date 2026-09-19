#pragma once

// Ore: Engine/source/afx/afxMagicMissile.h (afxMagicMissileData projectile fields)

#include <fuse/fx/fx_defs.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::fx {

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

/// One active projectile instance (ore: `afxMagicMissile` runtime state).
struct MissileInstance {
    const MissileDescriptor* descriptor = nullptr;
    fuse::math::Vec3 position{};
    fuse::math::Vec3 velocity{};
    float elapsed = 0.f;
    bool finished = false;
};

/// Game-thread projectile sim stub — linear / ballistic integration without T3D collision.
class MissilePipeline {
public:
    void fire(const MissileDescriptor& descriptor,
              const fuse::math::Vec3& origin,
              const fuse::math::Vec3& direction);

    void tick(float dt);

    u32 activeCount() const;
    u32 completedCount() const { return m_completedCount; }
    const std::vector<MissileInstance>& instances() const { return m_instances; }

private:
    std::vector<MissileInstance> m_instances;
    u32 m_completedCount = 0;
};

} // namespace fuse::fx
