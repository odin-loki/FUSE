#include <fuse/mechanics/toggle_component.hpp>

namespace fuse::mechanics {

ToggleComponent::ToggleComponent() = default;

ToggleComponent::ToggleComponent(std::string name, bool initialState)
    : Component(std::move(name)), m_state(initialState) {}

bool ToggleComponent::toggle() {
    m_state = !m_state;
    ++m_toggleCount;
    return m_state;
}

void ToggleComponent::setState(bool state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    ++m_toggleCount;
}

} // namespace fuse::mechanics
