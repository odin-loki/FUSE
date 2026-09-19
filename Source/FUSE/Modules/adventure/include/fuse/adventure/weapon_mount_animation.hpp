#pragma once

// Ore: 3DAAK weapon mount animation stub (without TorqueScript ShapeBase mount)

#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/item_id.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

struct WeaponMountPose {
    f32 mountYawDeg = 0.f;
    f32 mountPitchDeg = 0.f;
    f32 shoulderOffset = 0.f;
    std::string mountPoint = "weapon_shoulder";
};

/// Applies a weapon mount pose when a weapon is granted/equipped (3DAAK mount animation ore).
class WeaponMountAnimationStub {
public:
    void setPose(const WeaponMountPose& pose) { m_pose = pose; }
    const WeaponMountPose& pose() const { return m_pose; }

    bool applyOnGrant(Inventory& inventory, const ItemId& weapon);
    u32 applyCount() const { return m_applyCount; }
    const ItemId& lastWeaponApplied() const { return m_lastWeaponApplied; }

private:
    WeaponMountPose m_pose{};
    u32 m_applyCount = 0;
    ItemId m_lastWeaponApplied;
};

} // namespace fuse::adventure
