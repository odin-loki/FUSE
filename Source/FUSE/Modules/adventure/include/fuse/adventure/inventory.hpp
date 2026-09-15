#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::adventure {

/// Inventory slot stub — ore analogue: 3DAAK adventure scripts/systems.
/// TODO(U5 extract): third_party/addons/3DAAK/Templates/.../scripts/ inventory flow.
struct InventoryItem {
    std::string id;
    u32 quantity = 0;
};

class Inventory {
public:
    void addItem(std::string id, u32 quantity);
    u32 itemCount() const { return static_cast<u32>(m_items.size()); }
    u32 quantityOf(const std::string& id) const;

private:
    std::vector<InventoryItem> m_items;
};

} // namespace fuse::adventure
