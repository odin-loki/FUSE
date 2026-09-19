#pragma once

// Ore: 3DAAK skeletal weapon mount (bone attachment without Torque ShapeBase)

#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::adventure {

struct SkeletalBoneMount {
    std::string boneName = "weapon_shoulder";
    f32 offsetX = 0.f;
    f32 offsetY = 0.f;
    f32 offsetZ = 0.f;
    f32 yawDeg = 0.f;
    f32 pitchDeg = 0.f;
    f32 rollDeg = 0.f;
};

/// Applies skeletal bone mount pose on weapon grant (3DAAK mount ore without DTS skeleton).
class SkeletalMountStub {
public:
    void setBoneMount(const SkeletalBoneMount& mount) { m_mount = mount; }
    const SkeletalBoneMount& boneMount() const { return m_mount; }

    bool applyToMountAnimation(WeaponMountAnimationStub& mountAnim);
    u32 applyCount() const { return m_applyCount; }

private:
    SkeletalBoneMount m_mount{};
    u32 m_applyCount = 0;
};

} // namespace fuse::adventure
