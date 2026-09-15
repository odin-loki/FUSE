#include <fuse/mechanics/inventory_provider.hpp>

#include <fuse/mechanics/component.hpp>

namespace fuse::mechanics {

namespace {

const IInventoryProvider* resolveProvider(const ComponentInterface* iface) {
    if (iface == nullptr || !iface->isValid()) {
        return nullptr;
    }

    return dynamic_cast<const IInventoryProvider*>(iface->owner());
}

IInventoryProvider* resolveProvider(ComponentInterface* iface) {
    if (iface == nullptr || !iface->isValid()) {
        return nullptr;
    }

    return dynamic_cast<IInventoryProvider*>(iface->owner());
}

} // namespace

bool InventoryProviderInterface::hasItem(const std::string& item) const {
    const IInventoryProvider* provider = resolveProvider(this);
    return provider != nullptr && provider->hasItem(item);
}

u32 InventoryProviderInterface::getItemCount(const std::string& item) const {
    const IInventoryProvider* provider = resolveProvider(this);
    if (provider == nullptr) {
        return 0;
    }
    return provider->getItemCount(item);
}

u32 InventoryProviderInterface::grantItem(const std::string& item, u32 amount) {
    IInventoryProvider* provider = resolveProvider(this);
    if (provider == nullptr) {
        return 0;
    }
    return provider->grantItem(item, amount);
}

u32 InventoryProviderInterface::consumeItem(const std::string& item, u32 amount) {
    IInventoryProvider* provider = resolveProvider(this);
    if (provider == nullptr) {
        return 0;
    }
    return provider->consumeItem(item, amount);
}

} // namespace fuse::mechanics
