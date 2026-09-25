#pragma once

#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

enum class ViewportMode {
    View3D,
    View2D,
    Hybrid,
};

/// Editor fly camera. Angles are in degrees; yaw rotates about world +Y (positive = turn right),
/// pitch about the camera right axis (positive = look up). Yaw 0 / pitch 0 looks down world +Z.
/// The basis matches `ecs::look_at` (right-handed: right = forward x up).
struct ViewportCamera {
    f32 positionX = 0.f;
    f32 positionY = 2.f;
    f32 positionZ = -5.f;
    f32 yaw = 0.f;
    f32 pitch = 0.f;
    f32 moveSpeed = 10.f;
    /// Degrees of rotation per pixel of mouse travel (same value on both axes).
    f32 lookSensitivity = 0.2f;
    /// Speed multiplier while the "fast" modifier (Shift) is held.
    f32 fastMultiplier = 4.f;
    f32 fovDeg = 60.f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 10000.f;
    /// True while the look button (RMB) is held: mouse deltas rotate, WASDQE move.
    bool isFlying = false;

    static constexpr f32 kMaxPitchDeg = 89.f;
};

/// One frame of viewport input as the Qt shell reports it (Qt-free).
struct ViewportInput {
    /// Mouse travel since the previous report, in pixels (+x right, +y down, Qt convention).
    f32 mouseDeltaX = 0.f;
    f32 mouseDeltaY = 0.f;
    /// Right mouse button: enables mouse look + fly movement while held.
    bool lookHeld = false;
    bool moveForward = false;
    bool moveBack = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
    bool fast = false;
};

/// World-space ray through a viewport pixel.
struct ViewportRay {
    ecs::vec3 origin{};
    ecs::vec3 direction{0.f, 0.f, 1.f, 0.f}; ///< unit length
};

/// Headless viewport panel API (B6.3): fly/look camera, view/projection, pixel rays and the
/// render-target size the renderer framebuffer follows. The Qt shell forwards input and resizes.
class ViewportPanel {
public:
    void setMode(ViewportMode mode);
    ViewportMode mode() const { return m_mode; }

    ViewportCamera& camera() { return m_camera; }
    const ViewportCamera& camera() const { return m_camera; }

    void setProjectLabel(std::string label);
    const std::string& projectLabel() const { return m_projectLabel; }

    /// Resize the viewport. Zero clamps to 1. A real change raises `needsResize()` and bumps
    /// `resizeGeneration()`; framebuffer owners (`ViewportFramebuffer`) rebuild and clear it.
    void setDimensions(u32 width, u32 height);
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    f32 aspect() const { return static_cast<f32>(m_width) / static_cast<f32>(m_height); }

    bool needsResize() const { return m_needsResize; }
    void clearResizeFlag() { m_needsResize = false; }
    u32 resizeGeneration() const { return m_resizeGeneration; }

    /// Queue input for the next `tick`. Mouse deltas accumulate until consumed; button / key
    /// state is replaced (it is level-triggered).
    void submitInput(const ViewportInput& input);
    /// Apply queued input immediately: mouse look (delta * lookSensitivity degrees, pitch clamped
    /// to +-89) and, while flying, movement of moveSpeed (x fastMultiplier) units/s along the
    /// camera basis. Consumes the queued mouse deltas.
    void applyInput(f32 dt);

    void tick(f32 dt);
    u32 tickCount() const { return m_tickCount; }

    [[nodiscard]] ecs::vec3 position() const;
    [[nodiscard]] ecs::vec3 forward() const;
    [[nodiscard]] ecs::vec3 right() const;
    [[nodiscard]] ecs::vec3 up() const;
    [[nodiscard]] ecs::mat4 viewMatrix() const;
    [[nodiscard]] ecs::mat4 projectionMatrix() const;
    [[nodiscard]] ecs::mat4 viewProjection() const;

    /// Ray through window-space pixel (px, py) (origin top-left, pixel centres at +0.5).
    [[nodiscard]] ViewportRay screenRay(f32 px, f32 py) const;
    /// Project a world point to window pixels; false when it lies behind the camera.
    [[nodiscard]] bool worldToScreen(const ecs::vec3& world, f32& px, f32& py) const;

private:
    ViewportMode m_mode = ViewportMode::View3D;
    ViewportCamera m_camera;
    ViewportInput m_input{};
    std::string m_projectLabel;
    u32 m_width = 1;
    u32 m_height = 1;
    u32 m_tickCount = 0;
    u32 m_resizeGeneration = 0;
    bool m_needsResize = false;
};

} // namespace fuse::editor
