#include <fuse/editor/gizmo_system.hpp>

#include <cmath>

namespace fuse::editor {

namespace {

f32 normalizedX(const GizmoHitTest& hit) {
    if (hit.viewportWidth <= 0.f) {
        return 0.f;
    }
    return hit.screenX / hit.viewportWidth;
}

f32 normalizedY(const GizmoHitTest& hit) {
    if (hit.viewportHeight <= 0.f) {
        return 0.f;
    }
    return hit.screenY / hit.viewportHeight;
}

} // namespace

void GizmoSystem::setMode(GizmoMode mode) {
    m_mode = mode;
}

GizmoResult GizmoSystem::beginDrag(const GizmoHitTest& hit, const GizmoTransform& current) {
    GizmoResult result;
    m_activeAxis = pickAxis_(hit);
    if (m_activeAxis == GizmoAxis::None) {
        return result;
    }

    m_dragging = true;
    m_startTransform = current;
    m_currentTransform = current;
    m_lastHit = hit;

    result.active = true;
    result.axis = m_activeAxis;
    result.transform = m_currentTransform;
    return result;
}

GizmoResult GizmoSystem::updateDrag(const GizmoHitTest& hit) {
    GizmoResult result;
    if (!m_dragging) {
        return result;
    }

    m_lastHit = hit;
    const GizmoTransform next = applyAxisDelta_(hit, m_startTransform);
    m_currentTransform = next;

    result.active = true;
    result.changed = true;
    result.axis = m_activeAxis;
    result.transform = m_currentTransform;
    return result;
}

GizmoResult GizmoSystem::endDrag() {
    GizmoResult result;
    if (!m_dragging) {
        return result;
    }

    result.active = false;
    result.changed = true;
    result.axis = m_activeAxis;
    result.transform = m_currentTransform;

    m_dragging = false;
    m_activeAxis = GizmoAxis::None;
    return result;
}

GizmoAxis GizmoSystem::pickAxis_(const GizmoHitTest& hit) const {
    const f32 x = normalizedX(hit);
    const f32 y = normalizedY(hit);

    if (m_mode == GizmoMode::Scale && x > 0.4f && x < 0.6f && y > 0.4f && y < 0.6f) {
        return GizmoAxis::Uniform;
    }
    if (x < 0.33f) {
        return GizmoAxis::X;
    }
    if (y < 0.33f) {
        return GizmoAxis::Y;
    }
    return GizmoAxis::Z;
}

GizmoTransform GizmoSystem::applyAxisDelta_(const GizmoHitTest& hit, const GizmoTransform& base) const {
    GizmoTransform out = base;
    const f32 dx = normalizedX(hit) - normalizedX(m_lastHit);
    const f32 dy = normalizedY(hit) - normalizedY(m_lastHit);
    const f32 delta = dx + dy;

    switch (m_mode) {
    case GizmoMode::Translate:
        switch (m_activeAxis) {
        case GizmoAxis::X:
            out.posX = base.posX + delta * kScreenSize;
            break;
        case GizmoAxis::Y:
            out.posY = base.posY + delta * kScreenSize;
            break;
        case GizmoAxis::Z:
            out.posZ = base.posZ + delta * kScreenSize;
            break;
        default:
            break;
        }
        break;
    case GizmoMode::Rotate:
        switch (m_activeAxis) {
        case GizmoAxis::X:
            out.rotX = base.rotX + delta;
            break;
        case GizmoAxis::Y:
            out.rotY = base.rotY + delta;
            break;
        case GizmoAxis::Z:
            out.rotZ = base.rotZ + delta;
            break;
        default:
            break;
        }
        break;
    case GizmoMode::Scale:
        if (m_activeAxis == GizmoAxis::Uniform) {
            const f32 scale = std::max(0.01f, 1.f + delta);
            out.scaleX = base.scaleX * scale;
            out.scaleY = base.scaleY * scale;
            out.scaleZ = base.scaleZ * scale;
        } else {
            switch (m_activeAxis) {
            case GizmoAxis::X:
                out.scaleX = std::max(0.01f, base.scaleX + delta);
                break;
            case GizmoAxis::Y:
                out.scaleY = std::max(0.01f, base.scaleY + delta);
                break;
            case GizmoAxis::Z:
                out.scaleZ = std::max(0.01f, base.scaleZ + delta);
                break;
            default:
                break;
            }
        }
        break;
    }

    return out;
}

} // namespace fuse::editor
