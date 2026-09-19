#include <fuse/mechanics/camera_component.hpp>

namespace fuse::mechanics {

CameraComponent::CameraComponent() : Component("CameraComponent") {}

CameraComponent::CameraComponent(std::string name, f32 fieldOfView)
    : Component(std::move(name)), m_fieldOfView(fieldOfView) {}

void CameraComponent::activate() {
    m_active = true;
    ++m_activateCount;
}

void CameraComponent::deactivate() {
    m_active = false;
}

} // namespace fuse::mechanics
