#include <fuse/mechanics/health_component.hpp>

namespace fuse::mechanics {

namespace {

const IHealthProvider* resolveProvider(const ComponentInterface* iface) {
    if (iface == nullptr || !iface->isValid()) {
        return nullptr;
    }
    return dynamic_cast<const IHealthProvider*>(iface->owner());
}

IHealthProvider* resolveProvider(ComponentInterface* iface) {
    if (iface == nullptr || !iface->isValid()) {
        return nullptr;
    }
    return dynamic_cast<IHealthProvider*>(iface->owner());
}

} // namespace

s32 HealthProviderInterface::currentHealth() const {
    const IHealthProvider* provider = resolveProvider(this);
    return provider != nullptr ? provider->currentHealth() : 0;
}

s32 HealthProviderInterface::maxHealth() const {
    const IHealthProvider* provider = resolveProvider(this);
    return provider != nullptr ? provider->maxHealth() : 0;
}

bool HealthProviderInterface::isAlive() const {
    const IHealthProvider* provider = resolveProvider(this);
    return provider != nullptr && provider->isAlive();
}

s32 HealthProviderInterface::applyDamage(s32 amount) {
    IHealthProvider* provider = resolveProvider(this);
    return provider != nullptr ? provider->applyDamage(amount) : 0;
}

s32 HealthProviderInterface::heal(s32 amount) {
    IHealthProvider* provider = resolveProvider(this);
    return provider != nullptr ? provider->heal(amount) : 0;
}

HealthComponent::HealthComponent() = default;

HealthComponent::HealthComponent(std::string name, s32 maxHealth)
    : Component(std::move(name)), m_maxHealth(maxHealth), m_currentHealth(maxHealth) {}

void HealthComponent::registerInterfaces(Component* owner) {
    Component::registerInterfaces(owner);
    owner->registerCachedInterface("mechanics", "health", this, &m_healthInterface);
}

s32 HealthComponent::applyDamage(s32 amount) {
    if (!isEnabled() || amount <= 0 || !isAlive()) {
        return 0;
    }

    const s32 applied = amount > m_currentHealth ? m_currentHealth : amount;
    m_currentHealth -= applied;
    ++m_damageEvents;
    return applied;
}

s32 HealthComponent::heal(s32 amount) {
    if (!isEnabled() || amount <= 0 || !isAlive()) {
        return 0;
    }

    const s32 missing = m_maxHealth - m_currentHealth;
    const s32 applied = amount > missing ? missing : amount;
    m_currentHealth += applied;
    ++m_healEvents;
    return applied;
}

} // namespace fuse::mechanics
