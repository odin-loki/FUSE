#pragma once

// Ore: 3DAAK Weapon::onPickup runtime grant pipeline (weapon.cs + inventory bridge)

#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/item_id.hpp>
#include <fuse/adventure/skeletal_mount_stub.hpp>
#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>
#include <fuse/adventure/weapon_runtime.hpp>

namespace fuse::adventure {

struct WeaponGrantRequest {
    ItemId weapon;
    ItemId ammo;
    u32 ammoAmount = 10;
    WeaponStats stats{};
};

/// Grants weapon + ammo on pickup and wires runtime stats (3DAAK grant pipeline ore).
class WeaponGrantPipeline {
public:
    bool grantOnPickup(InteractContext& ctx,
                       WeaponPickupInteractable& pickup,
                       const WeaponGrantRequest& request,
                       WeaponRuntime& runtime);

    bool grantOnPickupWithMount(InteractContext& ctx,
                                WeaponPickupInteractable& pickup,
                                const WeaponGrantRequest& request,
                                WeaponRuntime& runtime,
                                WeaponMountAnimationStub& mountAnim,
                                SkeletalMountStub& skeletalMount);

    u32 grantCount() const { return m_grantCount; }
    u32 mountGrantCount() const { return m_mountGrantCount; }

private:
    u32 m_grantCount = 0;
    u32 m_mountGrantCount = 0;
};

} // namespace fuse::adventure
