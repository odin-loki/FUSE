#pragma once

// Ore: third_party/addons/GMK/Engine/source/component/cameraComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Camera leaf (GMK CameraComponent without SimObject/Con::).
class CameraComponent : public Component {
public:
    CameraComponent();
    explicit CameraComponent(std::string name, f32 fieldOfView = 60.f);

    const char* typeName() const override { return "CameraComponent"; }

    f32 fieldOfView() const { return m_fieldOfView; }
    void setFieldOfView(f32 fieldOfView) { m_fieldOfView = fieldOfView; }

    bool active() const { return m_active; }
    u32 activateCount() const { return m_activateCount; }

    void activate();
    void deactivate();

private:
    f32 m_fieldOfView = 60.f;
    bool m_active = false;
    u32 m_activateCount = 0;
};

} // namespace fuse::mechanics
