#pragma once

#include <fuse/adventure/conversation_interactable.hpp>
#include <fuse/adventure/door_interactable.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::adventure {

/// Spawned interactables from `OutpostStubContent` (3DAAK Outpost template bridge).
struct OutpostSpawnBundle {
    std::unordered_map<std::string, std::unique_ptr<DoorInteractable>> doors;
    std::unordered_map<std::string, std::unique_ptr<WeaponPickupInteractable>> weaponPickups;
    std::unordered_map<std::string, std::unique_ptr<ConversationInteractable>> conversations;
};

/// Instantiate door / weapon / conversation interactables from parsed outpost JSON.
bool spawnOutpostInteractables(const OutpostStubContent& content, OutpostSpawnBundle& outBundle, std::string* errorOut = nullptr);

} // namespace fuse::adventure
