#include <fuse/adventure/outpost_spawn.hpp>

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

    if (outBundle.doors.empty() && outBundle.weaponPickups.empty() && outBundle.conversations.empty()) {
        if (errorOut != nullptr) {
            *errorOut = "no interactables spawned";
        }
        return false;
    }

    return true;
}

} // namespace fuse::adventure
