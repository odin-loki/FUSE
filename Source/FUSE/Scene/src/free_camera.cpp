#include <fuse/scene/free_camera.hpp>

#include <fuse/ecs/components/transform.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::scene {

namespace {

constexpr f32 kDegToRad = 3.14159265358979323846f / 180.f;

} // namespace

void FreeCameraController::attach(fuse::ecs::Registry& registry, fuse::ecs::EntityID camera, f32 yawDeg,
                                  f32 pitchDeg) {
    m_camera = camera;
    fuse::ecs::vec3 position = m_position;
    if (const fuse::ecs::Transform* transform = registry.get<fuse::ecs::Transform>(camera)) {
        position = transform->position;
    }
    setPose(registry, position, yawDeg, pitchDeg);
}

fuse::ecs::vec3 FreeCameraController::forward() const {
    const f32 yaw = m_yawDeg * kDegToRad;
    const f32 pitch = m_pitchDeg * kDegToRad;
    const f32 cp = std::cos(pitch);
    return {std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp, 0.f};
}

fuse::ecs::vec3 FreeCameraController::right() const {
    // Screen right of `look_at` (forward x world up), kept horizontal: at yaw 0 (view +Z) the
    // right-handed view basis puts screen right on -X.
    const f32 yaw = m_yawDeg * kDegToRad;
    return {-std::cos(yaw), 0.f, std::sin(yaw), 0.f};
}

fuse::ecs::quat FreeCameraController::orientation() const {
    // q = qYaw(+Y, yaw) * qPitch(+X, -pitch): rotating +Z about +X by -pitch raises it by `pitch`.
    const f32 hy = 0.5f * m_yawDeg * kDegToRad;
    const f32 hp = -0.5f * m_pitchDeg * kDegToRad;
    const f32 sy = std::sin(hy);
    const f32 cy = std::cos(hy);
    const f32 sp = std::sin(hp);
    const f32 cp = std::cos(hp);
    // (0, sy, 0, cy) * (sp, 0, 0, cp)
    return {cy * sp, sy * cp, -sy * sp, cy * cp};
}

bool FreeCameraController::setPose(fuse::ecs::Registry& registry, fuse::ecs::vec3 position, f32 yawDeg,
                                   f32 pitchDeg) {
    m_position = {position.x, position.y, position.z, 1.f};
    m_yawDeg = std::remainder(yawDeg, 360.f);
    m_pitchDeg = std::clamp(pitchDeg, -maxPitchDeg, maxPitchDeg);
    return writeTransform_(registry);
}

bool FreeCameraController::update(fuse::ecs::Registry& registry, const FreeCameraInput& input, f32 dt) {
    if (!registry.alive(m_camera) || registry.get<fuse::ecs::Transform>(m_camera) == nullptr) {
        return false;
    }
    m_yawDeg = std::remainder(m_yawDeg + input.yawDeltaDeg, 360.f);
    m_pitchDeg = std::clamp(m_pitchDeg + input.pitchDeltaDeg, -maxPitchDeg, maxPitchDeg);

    const f32 speed = moveSpeed * (input.boost ? boostMultiplier : 1.f) * std::max(dt, 0.f);
    const fuse::ecs::vec3 f = forward();
    const fuse::ecs::vec3 r = right();
    m_position.x += (f.x * input.moveForward + r.x * input.moveRight) * speed;
    m_position.y += (f.y * input.moveForward + input.moveUp) * speed;
    m_position.z += (f.z * input.moveForward + r.z * input.moveRight) * speed;
    return writeTransform_(registry);
}

bool FreeCameraController::writeTransform_(fuse::ecs::Registry& registry) const {
    fuse::ecs::Transform* transform = registry.get<fuse::ecs::Transform>(m_camera);
    if (transform == nullptr) {
        return false;
    }
    transform->position = m_position;
    transform->rotation = orientation();
    transform->dirty = true;
    return true;
}

} // namespace fuse::scene
