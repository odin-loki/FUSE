#include <fuse/mechanics/counter_component.hpp>

namespace fuse::mechanics {

CounterComponent::CounterComponent() = default;

CounterComponent::CounterComponent(std::string name, s32 initialValue, s32 targetValue)
    : Component(std::move(name)), m_value(initialValue), m_targetValue(targetValue) {}

void CounterComponent::increment(s32 delta) {
    m_value += delta;
    ++m_incrementCount;
}

void CounterComponent::reset() {
    m_value = 0;
    m_incrementCount = 0;
}

} // namespace fuse::mechanics
