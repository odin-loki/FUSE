#include <fuse/editor/gizmo_system.hpp>

#include <cmath>

namespace fuse::editor {

namespace {

constexpr f32 kEpsilon = 1e-6f;

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

math::Vec3 axisDirection(GizmoAxis axis, const math::Quat& orientation, GizmoSpace space) {
    math::Vec3 local{};
    switch (axis) {
    case GizmoAxis::X:
        local = {1.f, 0.f, 0.f};
        break;
    case GizmoAxis::Y:
        local = {0.f, 1.f, 0.f};
        break;
    case GizmoAxis::Z:
        local = {0.f, 0.f, 1.f};
        break;
    default:
        return {};
    }

    if (space == GizmoSpace::Local) {
        return orientation.rotate(local).normalized();
    }
    return local;
}

math::Vec3 segmentEnd(const math::Vec3& origin, GizmoAxis axis, const math::Quat& orientation,
                      GizmoSpace space, f32 axisLength) {
    return origin + axisDirection(axis, orientation, space) * axisLength;
}

} // namespace

bool hitTestAxisSegment(const GizmoRay& ray, const math::Vec3& segmentStart,
                        const math::Vec3& segmentEnd, f32 radius, f32& outT) {
    const math::Vec3 segment = segmentEnd - segmentStart;
    const math::Vec3 w = ray.origin - segmentStart;
    const math::Vec3 crossDir = math::cross(segment, ray.direction);
    const f32 crossLenSq = crossDir.dot(crossDir);

    if (crossLenSq < kEpsilon) {
        const f32 segLen = segment.length();
        if (segLen < kEpsilon) {
            if ((ray.origin - segmentStart).length() > radius) {
                return false;
            }
            outT = 0.f;
            return true;
        }

        const math::Vec3 along = segment * (1.f / segLen);
        const f32 alongDist = w.dot(along);
        const math::Vec3 closestOnLine = segmentStart + along * alongDist;
        if ((ray.origin - closestOnLine).length() > radius) {
            return false;
        }

        const auto rayParamForPoint = [&](const math::Vec3& point) {
            return (point - ray.origin).dot(ray.direction);
        };

        const f32 t0 = rayParamForPoint(segmentStart);
        const f32 t1 = rayParamForPoint(segmentEnd);
        const f32 tMin = std::min(t0, t1);
        const f32 tMax = std::max(t0, t1);
        if (tMax < 0.f) {
            return false;
        }

        outT = std::max(0.f, tMin);
        return true;
    }

    const f32 dist = std::fabs(w.dot(crossDir)) / std::sqrt(crossLenSq);
    if (dist > radius) {
        return false;
    }

    const f32 a = segment.dot(segment);
    const f32 b = segment.dot(ray.direction);
    const f32 c = ray.direction.dot(ray.direction);
    const f32 d = segment.dot(w);
    const f32 e = ray.direction.dot(w);
    const f32 denom = a * c - b * b;

    f32 segmentT = 0.f;
    f32 rayT = 0.f;
    if (std::fabs(denom) < kEpsilon) {
        segmentT = 0.f;
        rayT = (b > c ? d / b : e / c);
    } else {
        segmentT = (b * e - c * d) / denom;
        rayT = (a * e - b * d) / denom;
    }

    segmentT = std::max(0.f, std::min(1.f, segmentT));
    const math::Vec3 pointOnSegment = segmentStart + segment * segmentT;
    const math::Vec3 pointOnRay = ray.origin + ray.direction * rayT;
    if ((pointOnSegment - pointOnRay).length() > radius) {
        return false;
    }

    outT = rayT;
    return true;
}

bool hitTestAxisPlane(const GizmoRay& ray, const math::Vec3& planeNormal,
                      const math::Vec3& planePoint, f32& outT) {
    const f32 denom = planeNormal.dot(ray.direction);
    if (std::fabs(denom) < kEpsilon) {
        return false;
    }

    const f32 t = planeNormal.dot(planePoint - ray.origin) / denom;
    if (t < 0.f) {
        return false;
    }

    outT = t;
    return true;
}

GizmoAxis pickAxisFromRay(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                          GizmoSpace space, f32 axisLength, f32 pickRadius) {
    const math::Vec3 origin = gizmoPosition(transform);
    const math::Quat orientation = gizmoRotation(transform);

    if (mode == GizmoMode::Scale) {
        const math::Vec3 toCenter = origin - ray.origin;
        const f32 centerDist = math::cross(toCenter, ray.direction).length();
        if (centerDist <= pickRadius) {
            return GizmoAxis::Uniform;
        }
    }

    GizmoAxis bestAxis = GizmoAxis::None;
    f32 bestT = 1e30f;

    const GizmoAxis axes[] = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};
    for (const GizmoAxis axis : axes) {
        f32 hitT = 0.f;
        const math::Vec3 end = segmentEnd(origin, axis, orientation, space, axisLength);
        if (!hitTestAxisSegment(ray, origin, end, pickRadius, hitT)) {
            continue;
        }
        if (hitT < bestT) {
            bestT = hitT;
            bestAxis = axis;
        }
    }

    if (mode == GizmoMode::Rotate && bestAxis == GizmoAxis::None) {
        for (const GizmoAxis axis : axes) {
            f32 hitT = 0.f;
            const math::Vec3 normal = axisDirection(axis, orientation, space);
            if (!hitTestAxisPlane(ray, normal, origin, hitT)) {
                continue;
            }
            const math::Vec3 hitPoint = ray.origin + ray.direction * hitT;
            const math::Vec3 radial = hitPoint - origin;
            const f32 radialLen = radial.length();
            if (radialLen < axisLength - pickRadius || radialLen > axisLength + pickRadius) {
                continue;
            }
            if (hitT < bestT) {
                bestT = hitT;
                bestAxis = axis;
            }
        }
    }

    return bestAxis;
}

f32 snapToGrid(f32 value, f32 gridSize) {
    if (gridSize <= kEpsilon) {
        return value;
    }
    return std::round(value / gridSize) * gridSize;
}

f32 snapAngleRadians(f32 radians, f32 stepDegrees) {
    if (stepDegrees <= kEpsilon) {
        return radians;
    }
    const f32 stepRadians = stepDegrees * 3.14159265f / 180.f;
    return std::round(radians / stepRadians) * stepRadians;
}

GizmoTransform snapTransform(const GizmoTransform& transform, GizmoMode mode,
                             const GizmoSnapSettings& settings) {
    GizmoTransform out = transform;
    switch (mode) {
    case GizmoMode::Translate:
        if (settings.translateSnap) {
            out.posX = snapToGrid(out.posX, settings.gridSize);
            out.posY = snapToGrid(out.posY, settings.gridSize);
            out.posZ = snapToGrid(out.posZ, settings.gridSize);
        }
        break;
    case GizmoMode::Rotate:
        if (settings.rotateSnap) {
            out.rotX = snapAngleRadians(out.rotX, settings.angleStepDegrees);
            out.rotY = snapAngleRadians(out.rotY, settings.angleStepDegrees);
            out.rotZ = snapAngleRadians(out.rotZ, settings.angleStepDegrees);
        }
        break;
    case GizmoMode::Scale:
        break;
    }
    return out;
}

math::Vec3 gizmoPosition(const GizmoTransform& transform) {
    return {transform.posX, transform.posY, transform.posZ};
}

math::Quat gizmoRotation(const GizmoTransform& transform) {
    return {transform.rotX, transform.rotY, transform.rotZ, transform.rotW};
}

math::Vec3 gizmoScale(const GizmoTransform& transform) {
    return {transform.scaleX, transform.scaleY, transform.scaleZ};
}

GizmoTransform gizmoFromMath(const math::Vec3& position, const math::Quat& rotation,
                             const math::Vec3& scale) {
    GizmoTransform out{};
    out.posX = position.x;
    out.posY = position.y;
    out.posZ = position.z;
    out.rotX = rotation.x;
    out.rotY = rotation.y;
    out.rotZ = rotation.z;
    out.rotW = rotation.w;
    out.scaleX = scale.x;
    out.scaleY = scale.y;
    out.scaleZ = scale.z;
    return out;
}

GizmoTransform applyTranslateDelta(const GizmoTransform& base, GizmoAxis axis,
                                   const math::Vec3& delta, GizmoSpace space) {
    GizmoTransform out = base;
    math::Vec3 worldDelta = delta;

    if (space == GizmoSpace::Local) {
        worldDelta = gizmoRotation(base).rotate(delta);
    }

    switch (axis) {
    case GizmoAxis::X:
        out.posX += worldDelta.x;
        break;
    case GizmoAxis::Y:
        out.posY += worldDelta.y;
        break;
    case GizmoAxis::Z:
        out.posZ += worldDelta.z;
        break;
    default:
        out.posX += worldDelta.x;
        out.posY += worldDelta.y;
        out.posZ += worldDelta.z;
        break;
    }
    return out;
}

GizmoTransform applyRotateDelta(const GizmoTransform& base, GizmoAxis axis, f32 deltaRadians,
                                GizmoSpace space) {
    GizmoTransform out = base;
    math::Vec3 axisVec{};
    switch (axis) {
    case GizmoAxis::X:
        axisVec = {1.f, 0.f, 0.f};
        break;
    case GizmoAxis::Y:
        axisVec = {0.f, 1.f, 0.f};
        break;
    case GizmoAxis::Z:
        axisVec = {0.f, 0.f, 1.f};
        break;
    default:
        return out;
    }

    if (space == GizmoSpace::World) {
        axisVec = gizmoRotation(base).rotate(axisVec);
    }

    const math::Quat delta = math::fromAxisAngle(axisVec.normalized(), deltaRadians);
    const math::Quat combined = delta * gizmoRotation(base);
    out.rotX = combined.x;
    out.rotY = combined.y;
    out.rotZ = combined.z;
    out.rotW = combined.w;
    return out;
}

GizmoTransform applyScaleDelta(const GizmoTransform& base, GizmoAxis axis,
                               const math::Vec3& delta, GizmoSpace space) {
    (void)space;
    GizmoTransform out = base;
    switch (axis) {
    case GizmoAxis::Uniform:
        out.scaleX = std::max(0.01f, out.scaleX + delta.x);
        out.scaleY = std::max(0.01f, out.scaleY + delta.y);
        out.scaleZ = std::max(0.01f, out.scaleZ + delta.z);
        break;
    case GizmoAxis::X:
        out.scaleX = std::max(0.01f, out.scaleX + delta.x);
        break;
    case GizmoAxis::Y:
        out.scaleY = std::max(0.01f, out.scaleY + delta.y);
        break;
    case GizmoAxis::Z:
        out.scaleZ = std::max(0.01f, out.scaleZ + delta.z);
        break;
    default:
        break;
    }
    return out;
}

void GizmoSystem::setMode(GizmoMode mode) {
    m_mode = mode;
}

GizmoAxis GizmoSystem::pickAxis(const GizmoRay& ray, const GizmoTransform& transform) const {
    return pickAxisFromRay(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

GizmoAxis GizmoSystem::pickAxis(const GizmoHitTest& hit) const {
    return pickAxisScreen_(hit);
}

GizmoResult GizmoSystem::beginDrag(const GizmoHitTest& hit, const GizmoTransform& current) {
    GizmoResult result;
    m_activeAxis = pickAxisScreen_(hit);
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

GizmoResult GizmoSystem::beginDrag(const GizmoRay& ray, const GizmoTransform& current) {
    GizmoResult result;
    m_activeAxis = pickAxis(ray, current);
    if (m_activeAxis == GizmoAxis::None) {
        return result;
    }

    m_dragging = true;
    m_startTransform = current;
    m_currentTransform = current;
    m_lastHit = {};

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
    m_currentTransform = applySnapping_(applyAxisDelta_(hit, m_startTransform));

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

    m_currentTransform = applySnapping_(m_currentTransform);
    markDirty_();

    result.active = false;
    result.changed = true;
    result.axis = m_activeAxis;
    result.transform = m_currentTransform;

    m_dragging = false;
    m_activeAxis = GizmoAxis::None;
    return result;
}

GizmoAxis GizmoSystem::pickAxisScreen_(const GizmoHitTest& hit) const {
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

GizmoTransform GizmoSystem::applyAxisDelta_(const GizmoHitTest& hit,
                                            const GizmoTransform& base) const {
    const f32 dx = normalizedX(hit) - normalizedX(m_lastHit);
    const f32 dy = normalizedY(hit) - normalizedY(m_lastHit);
    const f32 delta = dx + dy;

    switch (m_mode) {
    case GizmoMode::Translate: {
        math::Vec3 move{};
        switch (m_activeAxis) {
        case GizmoAxis::X:
            move = {delta * kScreenSize, 0.f, 0.f};
            break;
        case GizmoAxis::Y:
            move = {0.f, delta * kScreenSize, 0.f};
            break;
        case GizmoAxis::Z:
            move = {0.f, 0.f, delta * kScreenSize};
            break;
        default:
            break;
        }
        return applyTranslateDelta(base, m_activeAxis, move, m_space);
    }
    case GizmoMode::Rotate:
        return applyRotateDelta(base, m_activeAxis, delta, m_space);
    case GizmoMode::Scale: {
        const math::Vec3 scaleDelta{delta, delta, delta};
        if (m_activeAxis == GizmoAxis::Uniform) {
            const f32 scale = std::max(0.01f, 1.f + delta);
            GizmoTransform out = base;
            out.scaleX = base.scaleX * scale;
            out.scaleY = base.scaleY * scale;
            out.scaleZ = base.scaleZ * scale;
            return out;
        }
        return applyScaleDelta(base, m_activeAxis, scaleDelta, m_space);
    }
    }
    return base;
}

GizmoTransform GizmoSystem::applySnapping_(const GizmoTransform& transform) const {
    return snapTransform(transform, m_mode, m_snap);
}

void GizmoSystem::markDirty_() {
    m_transformDirty = true;
    if (m_editorState != nullptr) {
        m_editorState->sceneModified = true;
    }
    if (m_commandStack != nullptr) {
        EditorCommand command;
        command.kind = CommandKind::SetProperty;
        command.target = m_target;
        command.propertyName = "transform";
        command.propertyValue = "gizmo";
        m_commandStack->execute(std::move(command));
    }
}

} // namespace fuse::editor
