#include <fuse/adventure/skeletal_mount_stub.hpp>

#include <unordered_map>

namespace fuse::adventure {

namespace {

const std::unordered_map<std::string, u32> kBoneNameToIndex{
    {"weapon_shoulder", 12u},
    {"spine_weapon", 8u},
    {"spine_mount", 6u},
    {"turret_pivot", 4u},
};

} // namespace

u32 SkeletalMountStub::resolveBoneIndex(const std::string& boneName) const {
    const auto it = kBoneNameToIndex.find(boneName);
    if (it != kBoneNameToIndex.end()) {
        return it->second;
    }
    return m_mount.boneIndex;
}

bool SkeletalMountStub::applyToMountAnimation(WeaponMountAnimationStub& mountAnim) {
    WeaponMountPose pose{};
    pose.mountPoint = m_mount.boneName;
    pose.mountYawDeg = m_mount.yawDeg;
    pose.mountPitchDeg = m_mount.pitchDeg;
    pose.shoulderOffset = m_mount.offsetX + m_mount.offsetY * 0.01f + m_mount.rollDeg * 0.001f;
    mountAnim.setPose(pose);
    m_mount.boneIndex = resolveBoneIndex(m_mount.boneName);
    ++m_applyCount;
    return true;
}

} // namespace fuse::adventure
