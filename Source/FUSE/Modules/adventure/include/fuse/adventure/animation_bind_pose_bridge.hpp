#pragma once

// Ore: 3DAAK weapon mount → skeletal bind pose bridge (without Torque ShapeBase)

#include <fuse/adventure/skeletal_mount_stub.hpp>
#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

/// Seeds animation bind pose from weapon/skeletal mount stubs (3DAAK mount ore).
class AnimationBindPoseBridge {
public:
    bool applyMountToBindPose(const SkeletalMountStub& skeletalMount,
                              const WeaponMountAnimationStub& mountAnim,
                              fuse::animation::Skeleton& skeleton,
                              fuse::animation::PoseSoA& pose);

    u32 applyCount() const { return m_applyCount; }
    u32 lastBoneIndex() const { return m_lastBoneIndex; }

private:
    u32 m_applyCount = 0;
    u32 m_lastBoneIndex = 0;
};

} // namespace fuse::adventure
