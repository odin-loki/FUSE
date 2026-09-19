#pragma once

// Ore: Samples/Modules/adventure/outpost_stub.json (3DAAK Outpost template)

#include <fuse/adventure/item_id.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::adventure {

struct OutpostDoorSpec {
    ItemId keyItem;
    std::string openMessage;
};

struct OutpostWeaponPickupSpec {
    ItemId weapon;
    ItemId ammo;
    u32 ammoCount = 0;
};

struct ConversationBranch {
    std::string id;
    std::vector<std::string> lines;
};

struct OutpostConversationSpec {
    std::vector<std::string> lines;
    std::vector<ConversationBranch> branches;
};

struct OutpostStubContent {
    std::string scene;
    std::unordered_map<std::string, OutpostDoorSpec> doors;
    std::unordered_map<std::string, OutpostWeaponPickupSpec> weaponPickups;
    std::unordered_map<std::string, OutpostConversationSpec> conversations;
};

/// Parse `outpost_stub.json` into adventure interactable specs (minimal JSON reader).
bool loadOutpostStubFromJson(const std::string& jsonText, OutpostStubContent& outContent, std::string* errorOut = nullptr);

/// Load embedded Samples outpost stub content.
bool loadEmbeddedOutpostStub(OutpostStubContent& outContent, std::string* errorOut = nullptr);

} // namespace fuse::adventure
