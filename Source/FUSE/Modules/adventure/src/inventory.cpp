#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

void Inventory::addItem(std::string id, u32 quantity) {
    for (InventoryItem& item : m_items) {
        if (item.id == id) {
            item.quantity += quantity;
            return;
        }
    }
    m_items.push_back({std::move(id), quantity});
}

u32 Inventory::quantityOf(const std::string& id) const {
    for (const InventoryItem& item : m_items) {
        if (item.id == id) {
            return item.quantity;
        }
    }
    return 0;
}

} // namespace fuse::adventure
