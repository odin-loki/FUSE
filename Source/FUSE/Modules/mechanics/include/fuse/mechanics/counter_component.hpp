#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/counterComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Integer counter leaf (GMK CounterComponent without SimObject/Con::).
class CounterComponent : public Component {
public:
    CounterComponent();
    explicit CounterComponent(std::string name, s32 initialValue = 0, s32 targetValue = 1);

    const char* typeName() const override { return "CounterComponent"; }

    s32 value() const { return m_value; }
    s32 targetValue() const { return m_targetValue; }
    u32 incrementCount() const { return m_incrementCount; }
    bool reachedTarget() const { return m_value >= m_targetValue; }

    void increment(s32 delta = 1);
    void reset();

private:
    s32 m_value = 0;
    s32 m_targetValue = 1;
    u32 m_incrementCount = 0;
};

} // namespace fuse::mechanics
