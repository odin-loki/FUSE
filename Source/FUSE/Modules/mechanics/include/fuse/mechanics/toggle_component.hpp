#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/simpleComponent.h (toggle / switch leaf)

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Binary state leaf (GMK toggle / switch SimComponent distilled without Con::).
class ToggleComponent : public Component {
public:
    ToggleComponent();
    explicit ToggleComponent(std::string name, bool initialState = false);

    const char* typeName() const override { return "ToggleComponent"; }

    bool state() const { return m_state; }
    u32 toggleCount() const { return m_toggleCount; }

    bool toggle();
    void setState(bool state);

private:
    bool m_state = false;
    u32 m_toggleCount = 0;
};

} // namespace fuse::mechanics
