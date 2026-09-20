#include <fuse/mechanics/animate_component.hpp>

namespace fuse::mechanics {

AnimateComponent::AnimateComponent() : Component("AnimateComponent") {}

AnimateComponent::AnimateComponent(std::string name, f32 durationSec)
    : Component(std::move(name)), m_durationSec(durationSec) {}

void AnimateComponent::advance(f32 dt) {
    if (!m_active || m_durationSec <= 0.f) {
        return;
    }

    m_elapsedSec += dt;
    while (m_elapsedSec >= m_durationSec) {
        m_elapsedSec -= m_durationSec;
        ++m_cycleCount;
    }
    m_normalizedTime = m_elapsedSec / m_durationSec;
}

} // namespace fuse::mechanics
