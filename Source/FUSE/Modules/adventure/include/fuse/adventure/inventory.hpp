#pragma once

#include <fuse/adventure/item_id.hpp>
#include <fuse/types.hpp>

#include <unordered_map>

namespace fuse::adventure {

/// Scripted inventory bag — mirrors 3DAAK `ShapeBase::incInventory` /
/// `decInventory` / `hasInventory` / `getInventory` / `setInventory`.
class Inventory {
public:
    using MaxLimits = std::unordered_map<std::string, u32>;

    Inventory() = default;
    explicit Inventory(MaxLimits maxLimits);

    void setMaxLimit(ItemId item, u32 maxCount);
    u32 maxInventory(ItemId item) const;

    bool hasInventory(ItemId item) const;
    u32 getInventory(ItemId item) const;

    /// Returns the amount actually added (may be less than requested).
    u32 incInventory(ItemId item, u32 amount);

    /// Returns the amount actually removed.
    u32 decInventory(ItemId item, u32 amount);

    /// Sets inventory count, clamped to [0, maxInventory]. Returns final value.
    u32 setInventory(ItemId item, u32 value);

    void clear();

private:
    u32 clampToMax(ItemId item, u32 value) const;

    MaxLimits m_maxLimits;
    std::unordered_map<std::string, u32> m_counts;
};

} // namespace fuse::adventure
