#pragma once

#include <fuse/adventure/inventory.hpp>
#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/inventory_provider.hpp>

namespace fuse::adventure {

/// Player/world inventory component — bridges adventure `Inventory` to mechanics interfaces.
class InventoryComponent : public mechanics::Component, public mechanics::IInventoryProvider {
public:
    InventoryComponent();
    explicit InventoryComponent(std::string name);

    const char* typeName() const override { return "InventoryComponent"; }

    void registerInterfaces(mechanics::Component* owner) override;

    Inventory& inventory() { return m_inventory; }
    const Inventory& inventory() const { return m_inventory; }

    bool hasItem(const std::string& item) const override;
    u32 getItemCount(const std::string& item) const override;
    u32 grantItem(const std::string& item, u32 amount) override;
    u32 consumeItem(const std::string& item, u32 amount) override;

private:
    Inventory m_inventory;
    mechanics::InventoryProviderInterface m_inventoryInterface;
};

} // namespace fuse::adventure
