#include <fuse/adventure/inventory_component.hpp>

namespace fuse::adventure {

InventoryComponent::InventoryComponent() = default;

InventoryComponent::InventoryComponent(std::string name) : mechanics::Component(std::move(name)) {}

void InventoryComponent::registerInterfaces(mechanics::Component* owner) {
    mechanics::Component::registerInterfaces(owner);
    owner->registerCachedInterface("mechanics", "inventory", this, &m_inventoryInterface);
}

bool InventoryComponent::hasItem(const std::string& item) const {
    return m_inventory.hasInventory(ItemId(item));
}

u32 InventoryComponent::getItemCount(const std::string& item) const {
    return m_inventory.getInventory(ItemId(item));
}

u32 InventoryComponent::grantItem(const std::string& item, u32 amount) {
    return m_inventory.incInventory(ItemId(item), amount);
}

u32 InventoryComponent::consumeItem(const std::string& item, u32 amount) {
    return m_inventory.decInventory(ItemId(item), amount);
}

} // namespace fuse::adventure
