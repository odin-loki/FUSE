#include <fuse/adventure/skeletal_mount_stub.hpp>

namespace fuse::adventure {

bool SkeletalMountStub::applyToMountAnimation(WeaponMountAnimationStub& mountAnim) {
    WeaponMountPose pose{};
    pose.mountPoint = m_mount.boneName;
    pose.mountYawDeg = m_mount.yawDeg;
    pose.mountPitchDeg = m_mount.pitchDeg;
    pose.shoulderOffset = m_mount.offsetX;
    mountAnim.setPose(pose);
    ++m_applyCount;
    return true;
}

} // namespace fuse::adventure
