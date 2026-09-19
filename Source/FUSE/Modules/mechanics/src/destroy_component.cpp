#include <fuse/mechanics/destroy_component.hpp>

namespace fuse::mechanics {

DestroyComponent::DestroyComponent() = default;

DestroyComponent::DestroyComponent(std::string name, bool destroyOnTrigger)
    : Component(std::move(name))
    , m_destroyOnTrigger(destroyOnTrigger) {}

bool DestroyComponent::triggerDestroy() {
    if (!m_destroyOnTrigger || m_triggered) {
        return false;
    }

    m_triggered = true;
    ++m_destroyCount;
    return true;
}

} // namespace fuse::mechanics
