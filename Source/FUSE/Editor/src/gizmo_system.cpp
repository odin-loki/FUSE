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

GizmoMode cycleGizmoMode(GizmoMode mode) {
    switch (mode) {
    case GizmoMode::Translate:
        return GizmoMode::Rotate;
    case GizmoMode::Rotate:
        return GizmoMode::Scale;
    case GizmoMode::Scale:
        return GizmoMode::Translate;
    }
    return GizmoMode::Translate;
}

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
    if (isRayEmpty(ray) || axisLength <= kEpsilon || pickRadius <= kEpsilon) {
        return GizmoAxis::None;
    }

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

f32 snapScale(f32 value, f32 gridStep) {
    if (gridStep <= kEpsilon) {
        return value;
    }
    return std::round(value / gridStep) * gridStep;
}

bool isSnapEnabled(GizmoMode mode, const GizmoSnapSettings& settings) {
    switch (mode) {
    case GizmoMode::Translate:
        return settings.translateSnap;
    case GizmoMode::Rotate:
        return settings.rotateSnap;
    case GizmoMode::Scale:
        return settings.scaleSnap;
    }
    return false;
}

f32 snapValue(f32 value, GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings)) {
        return value;
    }

    switch (mode) {
    case GizmoMode::Translate:
        return snapToGrid(value, settings.gridSize);
    case GizmoMode::Rotate:
        return snapAngleRadians(value, settings.angleStepDegrees);
    case GizmoMode::Scale:
        return snapScale(value, settings.scaleGridStep);
    }
    return value;
}

f32 snapStepForMode(GizmoMode mode, const GizmoSnapSettings& settings) {
    switch (mode) {
    case GizmoMode::Translate:
        return settings.gridSize;
    case GizmoMode::Rotate:
        return settings.angleStepDegrees;
    case GizmoMode::Scale:
        return settings.scaleGridStep;
    }
    return 0.f;
}

f32 snapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings)) {
        return delta;
    }
    return snapValue(delta, mode, settings);
}

bool isRayEmpty(const GizmoRay& ray) {
    return ray.direction.length() < kEpsilon;
}

bool isHitTestEmpty(const GizmoHitTest& hit) {
    return hit.viewportWidth <= kEpsilon || hit.viewportHeight <= kEpsilon;
}

bool isPickConfigValid(f32 axisLength, f32 pickRadius) {
    return axisLength > kEpsilon && pickRadius > kEpsilon;
}

bool isSnapStepValid(GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings)) {
        return true;
    }
    return snapStepForMode(mode, settings) > kEpsilon;
}

bool canApplySnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    return isSnapEnabled(mode, settings) && isSnapStepValid(mode, settings);
}

PickPreflight preflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                            GizmoSpace space, f32 axisLength, f32 pickRadius) {
    PickPreflight preflight{};
    if (isRayEmpty(ray)) {
        preflight.emptyRay = true;
        return preflight;
    }

    if (!isPickConfigValid(axisLength, pickRadius)) {
        preflight.invalidPickConfig = true;
        return preflight;
    }

    if (pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius) == GizmoAxis::None) {
        preflight.pickMiss = true;
    }

    return preflight;
}

PickPreflight preflightPick(const GizmoHitTest& hit, GizmoMode mode) {
    PickPreflight preflight{};
    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;
        return preflight;
    }

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;
        return preflight;
    }

    return preflight;
}

SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapPreflight preflight{};
    if (!isSnapEnabled(mode, settings)) {
        preflight.snapDisabled = true;
        return preflight;
    }

    if (!isSnapStepValid(mode, settings)) {
        preflight.invalidStep = true;
    }

    return preflight;
}

bool trySnapTransform(const GizmoTransform& transform, GizmoMode mode,
                      const GizmoSnapSettings& settings, GizmoTransform& out) {
    out = transform;
    if (!canApplySnap(mode, settings)) {
        return false;
    }

    out = snapTransform(transform, mode, settings);
    return true;
}

f32 trySnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!canApplySnap(mode, settings)) {
        return delta;
    }
    return snapValue(delta, mode, settings);
}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    UpdateDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
        return preflight;
    }

    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;
    }

    return preflight;
}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging);
    if (!preflight.notDragging && activeAxis == GizmoAxis::None) {
        preflight.noActiveAxis = true;
    }
    return preflight;
}

bool shouldSkipPick(const PickPreflight& preflight) { return !preflight.canPick(); }

bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius) {
    return shouldSkipPick(preflightPick(ray, transform, mode, space, axisLength, pickRadius));
}

bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode) {
    return shouldSkipPick(preflightPick(hit, mode));
}

bool shouldSkipSnap(const SnapPreflight& preflight) { return !preflight.canApply(); }

bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    return shouldSkipSnap(preflightSnap(mode, settings));
}

bool shouldSkipUpdateDrag(const UpdateDragPreflight& preflight) { return !preflight.canUpdate(); }

bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return shouldSkipUpdateDrag(preflightUpdateDrag(hit, dragging));
}

bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    return shouldSkipUpdateDrag(preflightUpdateDrag(hit, dragging, activeAxis));
}

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return preflightUpdateDrag(hit, dragging).canUpdate();
}

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    return preflightUpdateDrag(hit, dragging, activeAxis).canUpdate();
}

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, bool alreadyDragging) {
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;
        return preflight;
    }

    const PickPreflight pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.emptyRay = pick.emptyRay;
    preflight.invalidPickConfig = pick.invalidPickConfig;
    preflight.pickMiss = pick.pickMiss;
    if (!pick.canPick()) {
        return preflight;
    }

    preflight.canBegin = true;
    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      bool alreadyDragging) {
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;
        return preflight;
    }

    const PickPreflight pick = preflightPick(hit, mode);
    preflight.emptyHit = pick.emptyHit;
    preflight.screenMiss = pick.screenMiss;
    if (!pick.canPick()) {
        return preflight;
    }

    preflight.canBegin = true;
    return preflight;
}

bool tryPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius, GizmoAxis& outAxis) {
    outAxis = GizmoAxis::None;
    const PickPreflight pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    if (!pick.canPick()) {
        return false;
    }

    outAxis = pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius);
    return outAxis != GizmoAxis::None;
}

bool tryPickAxis(const GizmoHitTest& hit, GizmoMode mode, GizmoAxis& outAxis) {
    outAxis = GizmoAxis::None;
    const PickPreflight pick = preflightPick(hit, mode);
    if (!pick.canPick()) {
        return false;
    }

    const f32 x = normalizedX(hit);
    const f32 y = normalizedY(hit);

    if (mode == GizmoMode::Scale && x > 0.4f && x < 0.6f && y > 0.4f && y < 0.6f) {
        outAxis = GizmoAxis::Uniform;
        return true;
    }
    if (x < 0.33f) {
        outAxis = GizmoAxis::X;
        return true;
    }
    if (y < 0.33f) {
        outAxis = GizmoAxis::Y;
        return true;
    }
    outAxis = GizmoAxis::Z;
    return true;
}

bool canPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius) {
    return preflightPick(ray, transform, mode, space, axisLength, pickRadius).canPick() &&
           pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius) != GizmoAxis::None;
}

bool canPickAxis(const GizmoHitTest& hit, GizmoMode mode) {
    return preflightPick(hit, mode).canPick();
}

bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                  GizmoSpace space, f32 axisLength, f32 pickRadius) {
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius).canBegin;
}

bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode) {
    return preflightBeginDrag(hit, mode).canBegin;
}

bool isScreenHitMiss(const GizmoHitTest& hit, GizmoMode mode) {
    if (isHitTestEmpty(hit)) {
        return true;
    }

    const f32 x = normalizedX(hit);
    const f32 y = normalizedY(hit);

    if (mode == GizmoMode::Scale) {
        const bool inUniformHandle = x > 0.4f && x < 0.6f && y > 0.4f && y < 0.6f;
        const bool onAxisBand = x < 0.33f || y < 0.33f;
        return !inUniformHandle && !onAxisBand;
    }

    const bool inDeadZone = x > 0.33f && x < 0.66f && y > 0.33f && y < 0.66f;
    return inDeadZone;
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
        if (settings.scaleSnap) {
            out.scaleX = snapScale(out.scaleX, settings.scaleGridStep);
            out.scaleY = snapScale(out.scaleY, settings.scaleGridStep);
            out.scaleZ = snapScale(out.scaleZ, settings.scaleGridStep);
        }
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

bool GizmoSystem::setMode(GizmoMode mode) {
    if (m_mode == mode) {
        return false;
    }

    m_mode = mode;
    if (m_dragging) {
        m_dragging = false;
        m_activeAxis = GizmoAxis::None;
    }
    return true;
}

void GizmoSystem::cycleMode() {
    const GizmoMode next = cycleGizmoMode(m_mode);
    if (next == m_mode) {
        return;
    }

    m_mode = next;
    if (m_dragging) {
        m_dragging = false;
        m_activeAxis = GizmoAxis::None;
    }
}

GizmoAxis GizmoSystem::pickAxis(const GizmoRay& ray, const GizmoTransform& transform) const {
    return pickAxisFromRay(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

GizmoAxis GizmoSystem::pickAxis(const GizmoHitTest& hit) const {
    return pickAxisScreen_(hit);
}

bool GizmoSystem::tryPickAxis(const GizmoRay& ray, const GizmoTransform& transform,
                              GizmoAxis& outAxis) const {
    return fuse::editor::tryPickAxis(ray, transform, m_mode, m_space, kAxisLength, kPickRadius,
                                     outAxis);
}

bool GizmoSystem::tryPickAxis(const GizmoHitTest& hit, GizmoAxis& outAxis) const {
    return fuse::editor::tryPickAxis(hit, m_mode, outAxis);
}

bool GizmoSystem::canPickAxis(const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::canPickAxis(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

bool GizmoSystem::canPickAxis(const GizmoHitTest& hit) const {
    return fuse::editor::canPickAxis(hit, m_mode);
}

bool GizmoSystem::canBeginDrag(const GizmoHitTest& hit) const {
    return preflightBeginDrag(hit).canBegin;
}

bool GizmoSystem::canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform) const {
    return preflightBeginDrag(ray, transform).canBegin;
}

BeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightBeginDrag(hit, m_mode, m_dragging);
}

BeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoRay& ray,
                                                   const GizmoTransform& transform) const {
    return fuse::editor::preflightBeginDrag(ray, transform, m_mode, m_space, kAxisLength,
                                            kPickRadius, m_dragging);
}

PickPreflight GizmoSystem::preflightPick(const GizmoRay& ray,
                                         const GizmoTransform& transform) const {
    return fuse::editor::preflightPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

PickPreflight GizmoSystem::preflightPick(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPick(hit, m_mode);
}

SnapPreflight GizmoSystem::preflightSnap() const {
    return fuse::editor::preflightSnap(m_mode, m_snap);
}

bool GizmoSystem::canApplySnapNow() const {
    return fuse::editor::canApplySnap(m_mode, m_snap);
}

bool GizmoSystem::trySnapTransform(const GizmoTransform& transform, GizmoTransform& out) const {
    return fuse::editor::trySnapTransform(transform, m_mode, m_snap, out);
}

UpdateDragPreflight GizmoSystem::preflightUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis);
}

bool GizmoSystem::canUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::canUpdateDrag(hit, m_dragging, m_activeAxis);
}

bool GizmoSystem::shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::shouldSkipPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

bool GizmoSystem::shouldSkipPick(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipPick(hit, m_mode);
}

bool GizmoSystem::shouldSkipSnap() const {
    return fuse::editor::shouldSkipSnap(m_mode, m_snap);
}

bool GizmoSystem::shouldSkipUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipUpdateDrag(hit, m_dragging, m_activeAxis);
}

GizmoResult GizmoSystem::beginDrag(const GizmoHitTest& hit, const GizmoTransform& current) {
    GizmoResult result;
    if (!tryBeginDrag(hit, current, result)) {
        return result;
    }
    return result;
}

bool GizmoSystem::tryBeginDrag(const GizmoHitTest& hit, const GizmoTransform& current,
                               GizmoResult& out) {
    out = {};
    const BeginDragPreflight preflight = preflightBeginDrag(hit);
    if (!preflight.canBegin) {
        return false;
    }

    m_activeAxis = pickAxisScreen_(hit);
    if (m_activeAxis == GizmoAxis::None) {
        return false;
    }

    m_dragging = true;
    m_startTransform = current;
    m_currentTransform = current;
    m_lastHit = hit;

    out.active = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;
    return true;
}

bool GizmoSystem::tryBeginDrag(const GizmoRay& ray, const GizmoTransform& current,
                               GizmoResult& out) {
    out = {};
    const BeginDragPreflight preflight = preflightBeginDrag(ray, current);
    if (!preflight.canBegin) {
        return false;
    }
    if (!tryPickAxis(ray, current, m_activeAxis)) {
        return false;
    }

    m_dragging = true;
    m_startTransform = current;
    m_currentTransform = current;
    m_lastHit = {};

    out.active = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;
    return true;
}

GizmoResult GizmoSystem::beginDrag(const GizmoRay& ray, const GizmoTransform& current) {
    GizmoResult result;
    if (!tryBeginDrag(ray, current, result)) {
        return result;
    }
    return result;
}

bool GizmoSystem::tryUpdateDrag(const GizmoHitTest& hit, GizmoResult& out) {
    out = {};
    if (shouldSkipUpdateDrag(hit)) {
        return false;
    }

    m_lastHit = hit;
    m_currentTransform = applySnapping_(applyAxisDelta_(hit, m_startTransform));

    out.active = true;
    out.changed = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;
    return true;
}

GizmoResult GizmoSystem::updateDrag(const GizmoHitTest& hit) {
    GizmoResult result;
    if (!tryUpdateDrag(hit, result)) {
        return result;
    }
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
    GizmoAxis axis = GizmoAxis::None;
    if (!fuse::editor::tryPickAxis(hit, m_mode, axis)) {
        return GizmoAxis::None;
    }
    return axis;
}

GizmoTransform GizmoSystem::applyAxisDelta_(const GizmoHitTest& hit,
                                            const GizmoTransform& base) const {
    const f32 dx = normalizedX(hit) - normalizedX(m_lastHit);
    const f32 dy = normalizedY(hit) - normalizedY(m_lastHit);
    const f32 delta = trySnapDragDelta(dx + dy, m_mode, m_snap);

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
    if (shouldSkipSnap()) {
        return transform;
    }

    GizmoTransform snapped = transform;
    static_cast<void>(trySnapTransform(transform, snapped));
    return snapped;
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
