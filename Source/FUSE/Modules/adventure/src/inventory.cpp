#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

Inventory::Inventory(MaxLimits maxLimits) : m_maxLimits(std::move(maxLimits)) {}

void Inventory::setMaxLimit(ItemId item, u32 maxCount) {
    m_maxLimits[item.name] = maxCount;
}

u32 Inventory::maxInventory(ItemId item) const {
    const auto it = m_maxLimits.find(item.name);
    if (it == m_maxLimits.end()) {
        return 0;
    }
    return it->second;
}

bool Inventory::hasInventory(ItemId item) const {
    return getInventory(item) > 0;
}

u32 Inventory::getInventory(ItemId item) const {
    const auto it = m_counts.find(item.name);
    if (it == m_counts.end()) {
        return 0;
    }
    return it->second;
}

u32 Inventory::incInventory(ItemId item, u32 amount) {
    const u32 max = maxInventory(item);
    const u32 total = getInventory(item);
    if (total >= max) {
        return 0;
    }

    if (total + amount > max) {
        amount = max - total;
    }

    m_counts[item.name] = total + amount;
    return amount;
}

u32 Inventory::decInventory(ItemId item, u32 amount) {
    const u32 total = getInventory(item);
    if (total == 0) {
        return 0;
    }

    if (total < amount) {
        amount = total;
    }

    m_counts[item.name] = total - amount;
    return amount;
}

u32 Inventory::setInventory(ItemId item, u32 value) {
    value = clampToMax(item, value);
    m_counts[item.name] = value;
    return value;
}

void Inventory::clear() {
    m_counts.clear();
}

u32 Inventory::clampToMax(ItemId item, u32 value) const {
    if (value == 0) {
        return 0;
    }

    const u32 max = maxInventory(item);
    if (value > max) {
        return max;
    }
    return value;
}

} // namespace fuse::adventure
