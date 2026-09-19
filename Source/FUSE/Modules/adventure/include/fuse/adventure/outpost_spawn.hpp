#pragma once

#include <fuse/adventure/conversation_interactable.hpp>
#include <fuse/adventure/door_interactable.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

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
    std::unordered_map<std::string, OutpostTransformSpec> placements;
};

/// Instantiate door / weapon / conversation interactables from parsed outpost JSON.
bool spawnOutpostInteractables(const OutpostStubContent& content, OutpostSpawnBundle& outBundle, std::string* errorOut = nullptr);

/// Apply JSON scene transforms to hybrid scene objects (Outpost placement stub).
bool applyOutpostScenePlacements(const OutpostSpawnBundle& bundle,
                                 fuse::SceneObject3D& guardObject,
                                 fuse::SceneObject3D& leverObject);

} // namespace fuse::adventure
