#pragma once

#include <fuse/mechanics/component_interface.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// String-keyed inventory surface for instigators (ore: GMK item hooks; adventure maps ItemId).
class IInventoryProvider {
public:
    virtual ~IInventoryProvider() = default;

    virtual bool hasItem(const std::string& item) const = 0;
    virtual u32 getItemCount(const std::string& item) const = 0;

    /// Returns the amount actually granted (may be less than requested).
    virtual u32 grantItem(const std::string& item, u32 amount) = 0;

    /// Returns the amount actually consumed.
    virtual u32 consumeItem(const std::string& item, u32 amount) = 0;
};

/// Cached `IInventoryProvider` accessor (ore: GMK `SimpleComponentInterface`).
class InventoryProviderInterface : public ComponentInterface {
public:
    bool hasItem(const std::string& item) const;
    u32 getItemCount(const std::string& item) const;
    u32 grantItem(const std::string& item, u32 amount);
    u32 consumeItem(const std::string& item, u32 amount);
};

} // namespace fuse::mechanics
