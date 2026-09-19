#include <fuse/adventure/outpost_spawn.hpp>

#include <fuse/world3d/scene_object_3d.hpp>

namespace fuse::adventure {

bool spawnOutpostInteractables(const OutpostStubContent& content,
                               OutpostSpawnBundle& outBundle,
                               std::string* errorOut) {
    outBundle.doors.clear();
    outBundle.weaponPickups.clear();
    outBundle.conversations.clear();

    for (const auto& entry : content.doors) {
        outBundle.doors[entry.first] =
            std::make_unique<DoorInteractable>(entry.second.keyItem, entry.second.openMessage);
    }

    for (const auto& entry : content.weaponPickups) {
        outBundle.weaponPickups[entry.first] = std::make_unique<WeaponPickupInteractable>(
            entry.second.weapon, entry.second.ammo, entry.second.ammoCount);
    }

    for (const auto& entry : content.conversations) {
        outBundle.conversations[entry.first] =
            std::make_unique<ConversationInteractable>(entry.second.lines, entry.second.branches);
    }

    outBundle.placements = content.placements;

    if (outBundle.doors.empty() && outBundle.weaponPickups.empty() && outBundle.conversations.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no interactables spawned";
        }
        return false;
    }

    return true;
}

bool applyOutpostScenePlacements(const OutpostSpawnBundle& bundle,
                                 fuse::SceneObject3D& guardObject,
                                 fuse::SceneObject3D& leverObject) {
    bool applied = false;

    const auto guardIt = bundle.placements.find("outpost_guard");
    if (guardIt != bundle.placements.end()) {
        guardObject.setPosition(guardIt->second.x, guardIt->second.y);
        guardObject.setZ(guardIt->second.z);
        applied = true;
    }

    const auto leverIt = bundle.placements.find("lever_interactable");
    if (leverIt != bundle.placements.end()) {
        leverObject.setPosition(leverIt->second.x, leverIt->second.y);
        leverObject.setZ(leverIt->second.z);
        applied = true;
    }

    return applied;
}

} // namespace fuse::adventure
