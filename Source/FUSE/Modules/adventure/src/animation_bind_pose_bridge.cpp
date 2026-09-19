#include <fuse/adventure/animation_bind_pose_bridge.hpp>

namespace fuse::adventure {

bool AnimationBindPoseBridge::applyMountToBindPose(const SkeletalMountStub& skeletalMount,
                                                   const WeaponMountAnimationStub& mountAnim,
                                                   fuse::animation::Skeleton& skeleton,
                                                   fuse::animation::PoseSoA& pose) {
    if (skeleton.bone_count == 0) {
        return false;
    }

    fuse::animation::ensure_pose_soa_bind_fallback(pose, skeleton);
    const SkeletalBoneMount& mount = skeletalMount.boneMount();
    const WeaponMountPose& weaponPose = mountAnim.pose();
    const u32 boneIndex = skeletalMount.resolveBoneIndex(mount.boneName.empty() ? weaponPose.mountPoint
                                                                                : mount.boneName);
    if (boneIndex >= pose.bone_count) {
        return false;
    }

    pose.local_positions[boneIndex].x += mount.offsetX + weaponPose.shoulderOffset;
    pose.local_positions[boneIndex].y += mount.offsetY;
    pose.local_positions[boneIndex].z += mount.offsetZ;
    m_lastBoneIndex = boneIndex;
    ++m_applyCount;
    return true;
}

} // namespace fuse::adventure
