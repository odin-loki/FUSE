#include <fuse/editor/viewport_panel.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::editor {

namespace {

constexpr f32 kDegToRad = 3.14159265358979f / 180.f;

ecs::vec3 normalize3(const ecs::vec3& v) {
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(len > 0.f)) {
        return {0.f, 0.f, 1.f, 0.f};
    }
    return {v.x / len, v.y / len, v.z / len, 0.f};
}

ecs::vec3 cross3(const ecs::vec3& a, const ecs::vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.f};
}

f32 dot3(const ecs::vec3& a, const ecs::vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

f32 wrapYaw(f32 yaw) {
    if (yaw > 180.f || yaw <= -180.f) {
        yaw = std::fmod(yaw + 180.f, 360.f);
        if (yaw <= 0.f) {
            yaw += 360.f;
        }
        yaw -= 180.f;
    }
    return yaw;
}

} // namespace

void ViewportPanel::setMode(ViewportMode mode) {
    m_mode = mode;
}

void ViewportPanel::setProjectLabel(std::string label) {
    m_projectLabel = std::move(label);
}

void ViewportPanel::setDimensions(u32 width, u32 height) {
    const u32 w = std::max(1u, width);
    const u32 h = std::max(1u, height);
    if (w == m_width && h == m_height) {
        return;
    }
    m_width = w;
    m_height = h;
    m_needsResize = true;
    ++m_resizeGeneration;
}

void ViewportPanel::submitInput(const ViewportInput& input) {
    const f32 dx = m_input.mouseDeltaX + input.mouseDeltaX;
    const f32 dy = m_input.mouseDeltaY + input.mouseDeltaY;
    m_input = input;
    m_input.mouseDeltaX = dx;
    m_input.mouseDeltaY = dy;
}

void ViewportPanel::applyInput(f32 dt) {
    m_camera.isFlying = m_input.lookHeld;
    if (m_input.lookHeld) {
        const f32 sensitivity = m_camera.lookSensitivity;
        m_camera.yaw = wrapYaw(m_camera.yaw + m_input.mouseDeltaX * sensitivity);
        // Screen +y is down: dragging the mouse up (negative dy) looks up.
        m_camera.pitch = std::clamp(m_camera.pitch - m_input.mouseDeltaY * sensitivity,
                                    -ViewportCamera::kMaxPitchDeg, ViewportCamera::kMaxPitchDeg);

        f32 along = 0.f;
        f32 side = 0.f;
        f32 vertical = 0.f;
        along += m_input.moveForward ? 1.f : 0.f;
        along -= m_input.moveBack ? 1.f : 0.f;
        side += m_input.moveRight ? 1.f : 0.f;
        side -= m_input.moveLeft ? 1.f : 0.f;
        vertical += m_input.moveUp ? 1.f : 0.f;
        vertical -= m_input.moveDown ? 1.f : 0.f;

        if (dt > 0.f && (along != 0.f || side != 0.f || vertical != 0.f)) {
            const ecs::vec3 f = forward();
            const ecs::vec3 r = right();
            ecs::vec3 move{f.x * along + r.x * side, f.y * along + r.y * side + vertical,
                           f.z * along + r.z * side, 0.f};
            move = normalize3(move); // diagonal movement is not faster
            const f32 speed = m_camera.moveSpeed * (m_input.fast ? m_camera.fastMultiplier : 1.f);
            const f32 step = speed * dt;
            m_camera.positionX += move.x * step;
            m_camera.positionY += move.y * step;
            m_camera.positionZ += move.z * step;
        }
    }
    m_input.mouseDeltaX = 0.f;
    m_input.mouseDeltaY = 0.f;
}

void ViewportPanel::tick(f32 dt) {
    applyInput(dt);
    ++m_tickCount;
}

ecs::vec3 ViewportPanel::position() const {
    return {m_camera.positionX, m_camera.positionY, m_camera.positionZ, 1.f};
}

ecs::vec3 ViewportPanel::forward() const {
    const f32 yaw = m_camera.yaw * kDegToRad;
    const f32 pitch = m_camera.pitch * kDegToRad;
    const f32 cp = std::cos(pitch);
    return {-std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp, 0.f};
}

ecs::vec3 ViewportPanel::right() const {
    // forward x worldUp, as ecs::look_at builds its side vector (pitch is clamped below 90).
    return normalize3(cross3(forward(), ecs::vec3{0.f, 1.f, 0.f, 0.f}));
}

ecs::vec3 ViewportPanel::up() const {
    return cross3(right(), forward());
}

ecs::mat4 ViewportPanel::viewMatrix() const {
    const ecs::vec3 eye = position();
    const ecs::vec3 f = forward();
    const ecs::vec3 target{eye.x + f.x, eye.y + f.y, eye.z + f.z, 1.f};
    return ecs::look_at(eye, target, ecs::vec3{0.f, 1.f, 0.f, 0.f});
}

ecs::mat4 ViewportPanel::projectionMatrix() const {
    return ecs::perspective(m_camera.fovDeg, aspect(), m_camera.nearPlane, m_camera.farPlane);
}

ecs::mat4 ViewportPanel::viewProjection() const {
    return ecs::multiply(projectionMatrix(), viewMatrix());
}

ViewportRay ViewportPanel::screenRay(f32 px, f32 py) const {
    const f32 ndcX = 2.f * (px / static_cast<f32>(m_width)) - 1.f;
    const f32 ndcY = 1.f - 2.f * (py / static_cast<f32>(m_height));
    const f32 tanHalf = std::tan(m_camera.fovDeg * 0.5f * kDegToRad);
    const ecs::vec3 f = forward();
    const ecs::vec3 r = right();
    const ecs::vec3 u = up();
    const f32 sx = ndcX * tanHalf * aspect();
    const f32 sy = ndcY * tanHalf;
    ViewportRay ray{};
    ray.origin = position();
    ray.direction = normalize3({f.x + r.x * sx + u.x * sy, f.y + r.y * sx + u.y * sy,
                                f.z + r.z * sx + u.z * sy, 0.f});
    return ray;
}

bool ViewportPanel::worldToScreen(const ecs::vec3& world, f32& px, f32& py) const {
    const ecs::vec3 eye = position();
    const ecs::vec3 d{world.x - eye.x, world.y - eye.y, world.z - eye.z, 0.f};
    const f32 depth = dot3(d, forward());
    if (!(depth > 0.f)) {
        return false;
    }
    const f32 tanHalf = std::tan(m_camera.fovDeg * 0.5f * kDegToRad);
    const f32 ndcX = dot3(d, right()) / (depth * tanHalf * aspect());
    const f32 ndcY = dot3(d, up()) / (depth * tanHalf);
    px = (ndcX + 1.f) * 0.5f * static_cast<f32>(m_width);
    py = (1.f - ndcY) * 0.5f * static_cast<f32>(m_height);
    return true;
}

} // namespace fuse::editor
