#include <fuse/fx/missile_descriptor.hpp>

#include <cmath>

namespace fuse::fx {

MissileDescriptor MissileDescriptor::makeFireballMissile() {
    MissileDescriptor desc;
    desc.id = "fireball_missile";
    desc.muzzleVelocity = 25.f;
    desc.isBallistic = true;
    desc.gravityMod = 9.81f;
    desc.lifetime = 3.f;
    desc.fadeDelay = 0.25f;
    return desc;
}

void MissilePipeline::fire(const MissileDescriptor& descriptor,
                           const fuse::math::Vec3& origin,
                           const fuse::math::Vec3& direction) {
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                direction.z * direction.z);
    if (len < 1e-6f) {
        return;
    }

    MissileInstance instance;
    instance.descriptor = &descriptor;
    instance.position = origin;
    instance.velocity.x = (direction.x / len) * descriptor.muzzleVelocity;
    instance.velocity.y = (direction.y / len) * descriptor.muzzleVelocity;
    instance.velocity.z = (direction.z / len) * descriptor.muzzleVelocity;
    m_instances.push_back(instance);
}

void MissilePipeline::tick(float dt) {
    if (dt <= 0.f) {
        return;
    }

    for (MissileInstance& instance : m_instances) {
        if (instance.finished || instance.descriptor == nullptr) {
            continue;
        }

        instance.elapsed += dt;
        instance.position.x += instance.velocity.x * dt;
        instance.position.y += instance.velocity.y * dt;
        instance.position.z += instance.velocity.z * dt;

        if (instance.descriptor->isBallistic) {
            instance.velocity.z -= instance.descriptor->gravityMod * dt;
        }

        if (instance.elapsed >= instance.descriptor->lifetime) {
            instance.finished = true;
            ++m_completedCount;
        }
    }
}

u32 MissilePipeline::activeCount() const {
    u32 count = 0;
    for (const MissileInstance& instance : m_instances) {
        if (!instance.finished) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::fx
