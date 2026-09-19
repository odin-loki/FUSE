#include <fuse/mechanics/light_component.hpp>

namespace fuse::mechanics {

LightComponent::LightComponent() : Component("LightComponent") {}

LightComponent::LightComponent(std::string name, f32 intensity)
    : Component(std::move(name)), m_intensity(intensity) {}

void LightComponent::enable() {
    m_enabled = true;
    ++m_enableCount;
}

void LightComponent::disable() {
    m_enabled = false;
    ++m_disableCount;
}

} // namespace fuse::mechanics
