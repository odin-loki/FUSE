#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/delayComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Delay/timer leaf (GMK DelayComponent without SimObject/Con::).
class DelayComponent : public Component {
public:
    DelayComponent();
    explicit DelayComponent(std::string name, u32 delayMs = 0);

    const char* typeName() const override { return "DelayComponent"; }

    u32 delayMs() const { return m_delayMs; }
    void setDelayMs(u32 delayMs) { m_delayMs = delayMs; }

    u32 elapsedMs() const { return m_elapsedMs; }
    bool fired() const { return m_fired; }
    u32 fireCount() const { return m_fireCount; }

    void advance(u32 deltaMs);
    void reset();

private:
    u32 m_delayMs = 0;
    u32 m_elapsedMs = 0;
    bool m_fired = false;
    u32 m_fireCount = 0;
};

} // namespace fuse::mechanics
