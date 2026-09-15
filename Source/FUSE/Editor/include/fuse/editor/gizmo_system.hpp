#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

enum class GizmoMode {
    Translate,
    Rotate,
    Scale,
};

enum class GizmoAxis {
    None,
    X,
    Y,
    Z,
    Uniform,
};

struct GizmoTransform {
    f32 posX = 0.f;
    f32 posY = 0.f;
    f32 posZ = 0.f;
    f32 rotX = 0.f;
    f32 rotY = 0.f;
    f32 rotZ = 0.f;
    f32 rotW = 1.f;
    f32 scaleX = 1.f;
    f32 scaleY = 1.f;
    f32 scaleZ = 1.f;
};

struct GizmoHitTest {
    f32 screenX = 0.f;
    f32 screenY = 0.f;
    f32 viewportWidth = 1.f;
    f32 viewportHeight = 1.f;
};

struct GizmoResult {
    bool active = false;
    bool changed = false;
    GizmoAxis axis = GizmoAxis::None;
    GizmoTransform transform;
};

/// Headless in-viewport gizmo API (B6.4).
class GizmoSystem {
public:
    static constexpr f32 kScreenSize = 120.f;
    static constexpr f32 kLineWidth = 2.5f;
    static constexpr f32 kArrowSize = 0.2f;

    void setMode(GizmoMode mode);
    GizmoMode mode() const { return m_mode; }

    void setTarget(Handle<Object> target) { m_target = target; }
    Handle<Object> target() const { return m_target; }

    GizmoResult beginDrag(const GizmoHitTest& hit, const GizmoTransform& current);
    GizmoResult updateDrag(const GizmoHitTest& hit);
    GizmoResult endDrag();

    bool isDragging() const { return m_dragging; }

private:
    GizmoAxis pickAxis_(const GizmoHitTest& hit) const;
    GizmoTransform applyAxisDelta_(const GizmoHitTest& hit, const GizmoTransform& base) const;

    GizmoMode m_mode = GizmoMode::Translate;
    Handle<Object> m_target = Handle<Object>::invalid();
    GizmoAxis m_activeAxis = GizmoAxis::None;
    bool m_dragging = false;
    GizmoTransform m_startTransform;
    GizmoTransform m_currentTransform;
    GizmoHitTest m_lastHit;
};

} // namespace fuse::editor
