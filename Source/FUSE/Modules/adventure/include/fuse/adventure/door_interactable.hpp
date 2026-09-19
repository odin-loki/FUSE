#pragma once

// Ore: third_party/addons/3DAAK (locked door / key use scripts in Outpost template)

#include <fuse/adventure/interactable.hpp>

#include <string>

namespace fuse::adventure {

/// Locked door opened by consuming a key item (3DAAK door / switch pattern).
class DoorInteractable : public IInteractable {
public:
    DoorInteractable(ItemId keyItem, std::string openMessage = "Door opened");

    bool isOpen() const { return m_open; }
    const std::string& openMessage() const { return m_openMessage; }

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

private:
    ItemId m_keyItem;
    std::string m_openMessage;
    bool m_open = false;
};

} // namespace fuse::adventure
