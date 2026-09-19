#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/lightComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Light leaf (GMK LightComponent without SimObject/Con::).
class LightComponent : public Component {
public:
    LightComponent();
    explicit LightComponent(std::string name, f32 intensity = 1.f);

    const char* typeName() const override { return "LightComponent"; }

    f32 intensity() const { return m_intensity; }
    void setIntensity(f32 intensity) { m_intensity = intensity; }

    bool enabled() const { return m_enabled; }
    u32 enableCount() const { return m_enableCount; }
    u32 disableCount() const { return m_disableCount; }

    void enable();
    void disable();

private:
    f32 m_intensity = 1.f;
    bool m_enabled = false;
    u32 m_enableCount = 0;
    u32 m_disableCount = 0;
};

} // namespace fuse::mechanics
