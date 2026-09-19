#include <fuse/mechanics/rotate_component.hpp>

namespace fuse::mechanics {

RotateComponent::RotateComponent() = default;

RotateComponent::RotateComponent(std::string name, float degreesPerSecond)
    : Component(std::move(name))
    , m_degreesPerSecond(degreesPerSecond) {}

void RotateComponent::advance(float deltaSeconds) {
    if (m_degreesPerSecond == 0.f) {
        return;
    }

    m_angleDeg += m_degreesPerSecond * deltaSeconds;
    ++m_tickCount;
}

} // namespace fuse::mechanics
