#pragma once

// Ore: 3DAAK Weapon::onPickup runtime grant pipeline (weapon.cs + inventory bridge)

#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/item_id.hpp>
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

    u32 grantCount() const { return m_grantCount; }

private:
    u32 m_grantCount = 0;
};

} // namespace fuse::adventure
