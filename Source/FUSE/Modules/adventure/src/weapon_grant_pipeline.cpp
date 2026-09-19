#include <fuse/adventure/weapon_grant_pipeline.hpp>

namespace fuse::adventure {

bool WeaponGrantPipeline::grantOnPickup(InteractContext& ctx,
                                        WeaponPickupInteractable& pickup,
                                        const WeaponGrantRequest& request,
                                        WeaponRuntime& runtime) {
    const InteractResult result = pickup.onPickup(ctx, request.weapon, 1);
    if (result != InteractResult::PickedUp || ctx.inventory == nullptr) {
        return false;
    }

    runtime.setAmmoType(request.ammo);
    runtime.setStats(request.stats);
    ++m_grantCount;
    return true;
}

} // namespace fuse::adventure
