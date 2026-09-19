#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/simpleComponent.h (damageable component leaf)

#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/component_interface.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Damageable surface for mechanics-driven actors (GMK SimpleComponent pattern).
class IHealthProvider {
public:
    virtual ~IHealthProvider() = default;

    virtual s32 currentHealth() const = 0;
    virtual s32 maxHealth() const = 0;
    virtual bool isAlive() const = 0;

    virtual s32 applyDamage(s32 amount) = 0;
    virtual s32 heal(s32 amount) = 0;
};

class HealthProviderInterface : public ComponentInterface {
public:
    s32 currentHealth() const;
    s32 maxHealth() const;
    bool isAlive() const;

    s32 applyDamage(s32 amount);
    s32 heal(s32 amount);
};

/// Reference health component with cached interface registration.
class HealthComponent : public Component, public IHealthProvider {
public:
    HealthComponent();
    explicit HealthComponent(std::string name, s32 maxHealth);

    const char* typeName() const override { return "HealthComponent"; }

    void registerInterfaces(Component* owner) override;

    s32 currentHealth() const override { return m_currentHealth; }
    s32 maxHealth() const override { return m_maxHealth; }
    bool isAlive() const override { return m_currentHealth > 0; }

    s32 applyDamage(s32 amount) override;
    s32 heal(s32 amount) override;

    u32 damageEvents() const { return m_damageEvents; }
    u32 healEvents() const { return m_healEvents; }

private:
    HealthProviderInterface m_healthInterface;
    s32 m_maxHealth = 100;
    s32 m_currentHealth = 100;
    u32 m_damageEvents = 0;
    u32 m_healEvents = 0;
};

} // namespace fuse::mechanics
