#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/viewport_panel.hpp>

#include <array>

namespace fuse::editor {

/// Active viewport transform tool (GUI-2). `Select` hides the handles: left click only picks.
enum class TransformTool : u8 { Select, Translate, Rotate, Scale };
/// Axis frame for Translate / Rotate. Scale always uses the entity's local axes.
enum class TransformSpace : u8 { World, Local };

struct TransformGizmoSettings {
    TransformTool tool = TransformTool::Translate;
    TransformSpace space = TransformSpace::World;
    bool snap = false;
    f32 translateStep = 0.5f;   ///< world units
    f32 rotateStepDeg = 15.f;   ///< degrees
    f32 scaleStep = 0.1f;       ///< scale factor increment
    f32 handlePixels = 90.f;    ///< on-screen axis length
    f32 hitPixels = 8.f;        ///< pick tolerance around a handle
    f32 rotateDegPerPixel = 0.5f;
};

/// One projected handle, in viewport pixels (same space as `ViewportPanel::worldToScreen`).
struct GizmoHandle2D {
    bool visible = false;
    f32 x0 = 0.f, y0 = 0.f; ///< gizmo origin
    f32 x1 = 0.f, y1 = 0.f; ///< handle tip
};

/// Qt-free screen-space transform gizmo for the editor viewport (GUI-2): projects X/Y/Z handles at
/// an entity, hit-tests the mouse against them and turns a drag into a `TransformCommand::State`.
/// Callers push `TransformCommand`s inside an UndoStack macro so one drag is one undo step.
class TransformGizmo {
public:
    TransformGizmoSettings& settings() { return m_settings; }
    const TransformGizmoSettings& settings() const { return m_settings; }

    /// Recompute handles for `transform` seen through `panel`. Returns false (nothing to draw)
    /// for the Select tool or when the entity is behind the camera.
    bool layout(const ViewportPanel& panel, const ecs::Transform& transform);
    void clearLayout() { m_visible = false; }
    [[nodiscard]] bool visible() const { return m_visible; }
    [[nodiscard]] const std::array<GizmoHandle2D, 3>& handles() const { return m_handles; }

    /// Axis index (0 = X, 1 = Y, 2 = Z) under the pixel, or -1.
    [[nodiscard]] int hitTest(f32 px, f32 py) const;

    /// Start dragging `axis` from pixel (px, py); requires a current layout.
    bool beginDrag(int axis, f32 px, f32 py, const ecs::Transform& transform);
    /// New local TRS for the mouse at (px, py) during a drag.
    [[nodiscard]] TransformCommand::State dragTo(f32 px, f32 py) const;
    void endDrag() { m_dragAxis = -1; }
    [[nodiscard]] bool dragging() const { return m_dragAxis >= 0; }
    [[nodiscard]] int dragAxis() const { return m_dragAxis; }
    [[nodiscard]] const TransformCommand::State& dragStart() const { return m_start; }

    /// World-space unit axis `axis` for the current tool / space and rotation `rotation`.
    [[nodiscard]] ecs::vec3 axisDirection(int axis, const ecs::quat& rotation) const;

    static ecs::vec3 gizmoOrigin(const ecs::Transform& transform);
    static ecs::vec3 rotate(const ecs::quat& q, const ecs::vec3& v);
    static ecs::quat multiply(const ecs::quat& a, const ecs::quat& b);
    static ecs::quat axisAngle(const ecs::vec3& axis, f32 radians);

private:
    TransformGizmoSettings m_settings{};
    std::array<GizmoHandle2D, 3> m_handles{};
    std::array<ecs::vec3, 3> m_axes{};
    f32 m_worldLength = 1.f;
    bool m_visible = false;

    int m_dragAxis = -1;
    f32 m_dragX = 0.f, m_dragY = 0.f;
    TransformCommand::State m_start{};
};

} // namespace fuse::editor
