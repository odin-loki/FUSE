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

GizmoAxis resolveScreenAxis(const GizmoHitTest& hit, GizmoMode mode) {
    const f32 x = normalizedX(hit);
    const f32 y = normalizedY(hit);

    if (mode == GizmoMode::Scale && x > 0.4f && x < 0.6f && y > 0.4f && y < 0.6f) {
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
    if (isHitTestDimensionsInvalid(hit)) {
        return false;
    }
    return hit.viewportWidth <= kEpsilon || hit.viewportHeight <= kEpsilon;
}

bool isHitTestOutOfBounds(const GizmoHitTest& hit) {
    if (isHitTestEmpty(hit)) {
        return false;
    }
    return hit.screenX < 0.f || hit.screenY < 0.f || hit.screenX > hit.viewportWidth ||
           hit.screenY > hit.viewportHeight;
}

bool isHitTestDimensionsInvalid(const GizmoHitTest& hit) {
    return hit.viewportWidth < 0.f || hit.viewportHeight < 0.f;
}

bool isFiniteGizmoScalar(f32 value) {
    return std::isfinite(value);
}

bool isRayFinite(const GizmoRay& ray) {
    return isFiniteGizmoScalar(ray.origin.x) && isFiniteGizmoScalar(ray.origin.y) &&
           isFiniteGizmoScalar(ray.origin.z) && isFiniteGizmoScalar(ray.direction.x) &&
           isFiniteGizmoScalar(ray.direction.y) && isFiniteGizmoScalar(ray.direction.z);
}

bool isHitTestFinite(const GizmoHitTest& hit) {
    return isFiniteGizmoScalar(hit.screenX) && isFiniteGizmoScalar(hit.screenY) &&
           isFiniteGizmoScalar(hit.viewportWidth) && isFiniteGizmoScalar(hit.viewportHeight);
}

GizmoInteractionPhase interactionPhase(bool dragging) {
    return dragging ? GizmoInteractionPhase::Dragging : GizmoInteractionPhase::Idle;
}

bool isSnapDegraded(GizmoMode mode, const GizmoSnapSettings& settings) {
    return isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings);
}

bool isRayValid(const GizmoRay& ray) {
    return !isRayEmpty(ray);
}

bool isHitTestValid(const GizmoHitTest& hit) {
    return !isHitTestEmpty(hit);
}

bool normalizeRay(GizmoRay& ray) {
    const f32 len = ray.direction.length();
    if (len < kEpsilon) {
        return false;
    }
    ray.direction = ray.direction * (1.f / len);
    return true;
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
    if (!isRayFinite(ray)) {
        preflight.nonFiniteRay = true;
        return preflight;
    }

    if (isRayEmpty(ray)) {
        preflight.emptyRay = true;
        return preflight;
    }

    if (!isPickConfigValid(axisLength, pickRadius)) {
        preflight.invalidPickConfig = true;
        return preflight;
    }

    const GizmoAxis axis =
        pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius);
    if (axis == GizmoAxis::None) {
        preflight.pickMiss = true;
    } else {
        preflight.axis = axis;
    }

    return preflight;
}

PickPreflight preflightPick(const GizmoHitTest& hit, GizmoMode mode) {
    PickPreflight preflight{};
    if (!isHitTestFinite(hit)) {
        preflight.nonFiniteHit = true;
        return preflight;
    }

    if (isHitTestDimensionsInvalid(hit)) {
        preflight.invalidDimensions = true;
        return preflight;
    }

    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;
        return preflight;
    }

    if (isHitTestOutOfBounds(hit)) {
        preflight.outOfBounds = true;
        return preflight;
    }

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;
        return preflight;
    }

    preflight.axis = resolveScreenAxis(hit, mode);
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

SnapDragPreflight preflightSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapDragPreflight preflight{};
    if (!isFiniteGizmoScalar(delta)) {
        preflight.deltaNonFinite = true;
        return preflight;
    }

    const SnapPreflight snap = preflightSnap(mode, settings);
    preflight.snapDisabled = snap.snapDisabled;
    preflight.invalidStep = snap.invalidStep;
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
    if (!preflightSnapDrag(delta, mode, settings).canApply()) {
        return delta;
    }
    return snapValue(delta, mode, settings);
}

math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings) {
    if (!settings.translateSnap) {
        return position;
    }
    return {snapToGrid(position.x, settings.gridSize), snapToGrid(position.y, settings.gridSize),
            snapToGrid(position.z, settings.gridSize)};
}

math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings) {
    if (!settings.rotateSnap) {
        return eulerRadians;
    }
    return {snapAngleRadians(eulerRadians.x, settings.angleStepDegrees),
            snapAngleRadians(eulerRadians.y, settings.angleStepDegrees),
            snapAngleRadians(eulerRadians.z, settings.angleStepDegrees)};
}

math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings) {
    if (!settings.scaleSnap) {
        return scale;
    }
    return {snapScale(scale.x, settings.scaleGridStep), snapScale(scale.y, settings.scaleGridStep),
            snapScale(scale.z, settings.scaleGridStep)};
}

bool canSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings) {
    return canApplySnap(mode, settings);
}

bool canSnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightSnapDrag(delta, mode, settings).canApply();
}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging,
                                        GizmoAxis activeAxis) {
    UpdateDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
        return preflight;
    }

    if (activeAxis == GizmoAxis::None) {
        preflight.invalidActiveAxis = true;
        return preflight;
    }

    if (!isHitTestFinite(hit)) {
        preflight.nonFiniteHit = true;
    } else if (isHitTestDimensionsInvalid(hit)) {
        preflight.invalidDimensions = true;
    } else if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;
    } else if (isHitTestOutOfBounds(hit)) {
        preflight.outOfBounds = true;
    }

    return preflight;
}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging,
                                        GizmoAxis activeAxis, GizmoMode mode,
                                        const GizmoSnapSettings& settings) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis);
    if (!preflight.canUpdate()) {
        return preflight;
    }

    if (isSnapDegraded(mode, settings)) {
        preflight.snapDegraded = true;
    }

    return preflight;
}

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    return preflightUpdateDrag(hit, dragging, activeAxis).canUpdate();
}

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings) {
    return preflightUpdateDrag(hit, dragging, activeAxis, mode, settings).canUpdate();
}

EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                  const GizmoSnapSettings& settings) {
    EndDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
        return preflight;
    }

    if (activeAxis == GizmoAxis::None) {
        preflight.invalidActiveAxis = true;
    }

    if (isSnapDegraded(mode, settings)) {
        preflight.snapDegraded = true;
    }

    return preflight;
}

PickInteractionPreflight preflightPickInteraction(const GizmoRay& ray,
                                                  const GizmoTransform& transform, GizmoMode mode,
                                                  GizmoSpace space, f32 axisLength,
                                                  f32 pickRadius,
                                                  const GizmoSnapSettings& settings) {
    PickInteractionPreflight preflight{};
    preflight.pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

PickInteractionPreflight preflightPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                  const GizmoSnapSettings& settings) {
    PickInteractionPreflight preflight{};
    preflight.pick = preflightPick(hit, mode);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

BeginDragInteractionPreflight preflightBeginDragInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginDragInteractionPreflight preflight{};
    preflight.begin =
        preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
    preflight.snap = preflightSnap(mode, settings);
    preflight.snapDegraded = isSnapDegraded(mode, settings);
    return preflight;
}

BeginDragInteractionPreflight preflightBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                            const GizmoSnapSettings& settings,
                                                            bool alreadyDragging) {
    BeginDragInteractionPreflight preflight{};
    preflight.begin = preflightBeginDrag(hit, mode, alreadyDragging);
    preflight.snap = preflightSnap(mode, settings);
    preflight.snapDegraded = isSnapDegraded(mode, settings);
    return preflight;
}

UpdateDragInteractionPreflight preflightUpdateDragInteraction(const GizmoHitTest& hit,
                                                              bool dragging, GizmoAxis activeAxis,
                                                              GizmoMode mode,
                                                              const GizmoSnapSettings& settings) {
    return preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings, 0.f);
}

UpdateDragInteractionPreflight preflightUpdateDragInteraction(const GizmoHitTest& hit,
                                                              bool dragging, GizmoAxis activeAxis,
                                                              GizmoMode mode,
                                                              const GizmoSnapSettings& settings,
                                                              f32 delta) {
    UpdateDragInteractionPreflight preflight{};
    preflight.drag = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    preflight.snap = preflightSnap(mode, settings);
    preflight.snapDrag = preflightSnapDrag(delta, mode, settings);
    return preflight;
}

EndDragInteractionPreflight preflightEndDragInteraction(bool dragging, GizmoAxis activeAxis,
                                                      GizmoMode mode,
                                                      const GizmoSnapSettings& settings) {
    EndDragInteractionPreflight preflight{};
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings);
    preflight.snap = preflightSnap(mode, settings);
    preflight.snapDegraded = isSnapDegraded(mode, settings);
    return preflight;
}

DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit, bool dragging,
                                                  GizmoAxis activeAxis, GizmoMode mode,
                                                  const GizmoSnapSettings& settings) {
    DragInteractionPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
        return preflight;
    }

    preflight.update = preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings);
    preflight.end = preflightEndDragInteraction(dragging, activeAxis, mode, settings);
    return preflight;
}

bool canPickInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                        GizmoSpace space, f32 axisLength, f32 pickRadius,
                        const GizmoSnapSettings& settings) {
    return preflightPickInteraction(ray, transform, mode, space, axisLength, pickRadius, settings)
        .canPick();
}

bool canPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
                        const GizmoSnapSettings& settings) {
    return preflightPickInteraction(hit, mode, settings).canPick();
}

bool canBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                             GizmoSpace space, f32 axisLength, f32 pickRadius,
                             const GizmoSnapSettings& settings) {
    return preflightBeginDragInteraction(ray, transform, mode, space, axisLength, pickRadius,
                                         settings)
        .canBegin();
}

bool canBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                             const GizmoSnapSettings& settings) {
    return preflightBeginDragInteraction(hit, mode, settings).canBegin();
}

bool canUpdateDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                              GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings).canUpdate();
}

bool canEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                           const GizmoSnapSettings& settings) {
    return preflightEndDragInteraction(dragging, activeAxis, mode, settings).canEnd();
}

bool canDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                        GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightDragInteraction(hit, dragging, activeAxis, mode, settings).canInteract();
}

bool canEndDrag(bool dragging, GizmoAxis activeAxis) {
    return preflightEndDrag(dragging, activeAxis, GizmoMode::Translate, {}).canEnd();
}

bool canEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                const GizmoSnapSettings& settings) {
    return preflightEndDrag(dragging, activeAxis, mode, settings).canEnd();
}

PickSnapPreflight preflightPickSnap(const GizmoRay& ray, const GizmoTransform& transform,
                                    GizmoMode mode, GizmoSpace space, f32 axisLength,
                                    f32 pickRadius, const GizmoSnapSettings& settings) {
    PickSnapPreflight preflight{};
    preflight.pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit, GizmoMode mode,
                                    const GizmoSnapSettings& settings) {
    PickSnapPreflight preflight{};
    preflight.pick = preflightPick(hit, mode);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

BeginInteractionPreflight preflightBeginInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginInteractionPreflight preflight{};
    preflight.begin = preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius,
                                         settings, alreadyDragging);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                    const GizmoSnapSettings& settings,
                                                    bool alreadyDragging) {
    BeginInteractionPreflight preflight{};
    preflight.begin = preflightBeginDrag(hit, mode, settings, alreadyDragging);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
                                                      GizmoAxis activeAxis, GizmoMode mode,
                                                      const GizmoSnapSettings& settings) {
    UpdateInteractionPreflight preflight{};
    preflight.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis,
                                                GizmoMode mode, const GizmoSnapSettings& settings) {
    EndInteractionPreflight preflight{};
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

InteractionPreflight preflightInteraction(const GizmoHitTest& hit, bool dragging,
                                          GizmoAxis activeAxis, GizmoMode mode,
                                          const GizmoSnapSettings& settings) {
    InteractionPreflight preflight{};
    preflight.dragging = dragging;
    preflight.pickSnap = preflightPickSnap(hit, mode, settings);
    preflight.begin = preflightBeginInteraction(hit, mode, settings, dragging);
    preflight.update = preflightUpdateInteraction(hit, dragging, activeAxis, mode, settings);
    preflight.end = preflightEndInteraction(dragging, activeAxis, mode, settings);
    return preflight;
}

InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                          GizmoSpace space, f32 axisLength, f32 pickRadius,
                                          const GizmoSnapSettings& settings) {
    InteractionPreflight preflight{};
    preflight.dragging = dragging;
    preflight.pickSnap =
        preflightPickSnap(ray, transform, mode, space, axisLength, pickRadius, settings);
    preflight.begin = preflightBeginInteraction(ray, transform, mode, space, axisLength,
                                                pickRadius, settings, dragging);
    preflight.update = preflightUpdateInteraction({}, dragging, activeAxis, mode, settings);
    preflight.end = preflightEndInteraction(dragging, activeAxis, mode, settings);
    return preflight;
}

bool canPickSnap(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius,
                 const GizmoSnapSettings& settings) {
    return preflightPickSnap(ray, transform, mode, space, axisLength, pickRadius, settings)
        .canPick();
}

bool canPickSnap(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightPickSnap(hit, mode, settings).canPick();
}

bool canBeginInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         const GizmoSnapSettings& settings, bool alreadyDragging) {
    return preflightBeginInteraction(ray, transform, mode, space, axisLength, pickRadius, settings,
                                     alreadyDragging)
        .canBegin();
}

bool canBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging) {
    return preflightBeginInteraction(hit, mode, settings, alreadyDragging).canBegin();
}

bool canUpdateInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightUpdateInteraction(hit, dragging, activeAxis, mode, settings).canUpdate();
}

bool canEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                       const GizmoSnapSettings& settings) {
    return preflightEndInteraction(dragging, activeAxis, mode, settings).canEnd();
}

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, const GizmoSnapSettings& settings,
                                      bool alreadyDragging) {
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;
        return preflight;
    }

    const PickPreflight pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.emptyRay = pick.emptyRay;
    preflight.nonFiniteRay = pick.nonFiniteRay;
    preflight.invalidPickConfig = pick.invalidPickConfig;
    preflight.pickMiss = pick.pickMiss;
    if (!pick.canPick()) {
        return preflight;
    }

    preflight.axis = pick.axis;
    if (isSnapDegraded(mode, settings)) {
        preflight.snapDegraded = true;
    }
    preflight.canBegin = true;
    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, bool alreadyDragging) {
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, {},
                              alreadyDragging);
}

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;
        return preflight;
    }

    const PickPreflight pick = preflightPick(hit, mode);
    preflight.emptyHit = pick.emptyHit;
    preflight.nonFiniteHit = pick.nonFiniteHit;
    preflight.invalidDimensions = pick.invalidDimensions;
    preflight.outOfBounds = pick.outOfBounds;
    preflight.screenMiss = pick.screenMiss;
    if (!pick.canPick()) {
        return preflight;
    }

    preflight.axis = pick.axis;
    if (isSnapDegraded(mode, settings)) {
        preflight.snapDegraded = true;
    }
    preflight.canBegin = true;
    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      bool alreadyDragging) {
    return preflightBeginDrag(hit, mode, {}, alreadyDragging);
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

bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                  GizmoSpace space, f32 axisLength, f32 pickRadius,
                  const GizmoSnapSettings& settings) {
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, settings)
        .canBegin;
}

bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode) {
    return preflightBeginDrag(hit, mode).canBegin;
}

bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightBeginDrag(hit, mode, settings).canBegin;
}

const char* gizmoPickRejectReasonLabel(GizmoPickRejectReason reason) {
    switch (reason) {
    case GizmoPickRejectReason::None:
        return "None";
    case GizmoPickRejectReason::NonFiniteRay:
        return "NonFiniteRay";
    case GizmoPickRejectReason::NonFiniteHit:
        return "NonFiniteHit";
    case GizmoPickRejectReason::EmptyRay:
        return "EmptyRay";
    case GizmoPickRejectReason::EmptyHit:
        return "EmptyHit";
    case GizmoPickRejectReason::InvalidPickConfig:
        return "InvalidPickConfig";
    case GizmoPickRejectReason::InvalidDimensions:
        return "InvalidDimensions";
    case GizmoPickRejectReason::OutOfBounds:
        return "OutOfBounds";
    case GizmoPickRejectReason::ScreenMiss:
        return "ScreenMiss";
    case GizmoPickRejectReason::PickMiss:
        return "PickMiss";
    }
    return "Unknown";
}

const char* gizmoSnapRejectReasonLabel(GizmoSnapRejectReason reason) {
    switch (reason) {
    case GizmoSnapRejectReason::None:
        return "None";
    case GizmoSnapRejectReason::SnapDisabled:
        return "SnapDisabled";
    case GizmoSnapRejectReason::InvalidStep:
        return "InvalidStep";
    }
    return "Unknown";
}

const char* gizmoSnapDragRejectReasonLabel(GizmoSnapDragRejectReason reason) {
    switch (reason) {
    case GizmoSnapDragRejectReason::None:
        return "None";
    case GizmoSnapDragRejectReason::DeltaNonFinite:
        return "DeltaNonFinite";
    case GizmoSnapDragRejectReason::SnapDisabled:
        return "SnapDisabled";
    case GizmoSnapDragRejectReason::InvalidStep:
        return "InvalidStep";
    }
    return "Unknown";
}

const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason) {
    switch (reason) {
    case GizmoBeginDragRejectReason::None:
        return "None";
    case GizmoBeginDragRejectReason::NonFiniteRay:
        return "NonFiniteRay";
    case GizmoBeginDragRejectReason::NonFiniteHit:
        return "NonFiniteHit";
    case GizmoBeginDragRejectReason::EmptyRay:
        return "EmptyRay";
    case GizmoBeginDragRejectReason::EmptyHit:
        return "EmptyHit";
    case GizmoBeginDragRejectReason::InvalidPickConfig:
        return "InvalidPickConfig";
    case GizmoBeginDragRejectReason::InvalidDimensions:
        return "InvalidDimensions";
    case GizmoBeginDragRejectReason::OutOfBounds:
        return "OutOfBounds";
    case GizmoBeginDragRejectReason::ScreenMiss:
        return "ScreenMiss";
    case GizmoBeginDragRejectReason::PickMiss:
        return "PickMiss";
    case GizmoBeginDragRejectReason::AlreadyDragging:
        return "AlreadyDragging";
    }
    return "Unknown";
}

const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason) {
    switch (reason) {
    case GizmoUpdateDragRejectReason::None:
        return "None";
    case GizmoUpdateDragRejectReason::NotDragging:
        return "NotDragging";
    case GizmoUpdateDragRejectReason::NonFiniteHit:
        return "NonFiniteHit";
    case GizmoUpdateDragRejectReason::EmptyHit:
        return "EmptyHit";
    case GizmoUpdateDragRejectReason::InvalidDimensions:
        return "InvalidDimensions";
    case GizmoUpdateDragRejectReason::OutOfBounds:
        return "OutOfBounds";
    case GizmoUpdateDragRejectReason::InvalidActiveAxis:
        return "InvalidActiveAxis";
    }
    return "Unknown";
}

const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason) {
    switch (reason) {
    case GizmoEndDragRejectReason::None:
        return "None";
    case GizmoEndDragRejectReason::NotDragging:
        return "NotDragging";
    }
    return "Unknown";
}

GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight) {
    if (preflight.nonFiniteRay) {
        return GizmoPickRejectReason::NonFiniteRay;
    }
    if (preflight.nonFiniteHit) {
        return GizmoPickRejectReason::NonFiniteHit;
    }
    if (preflight.emptyRay) {
        return GizmoPickRejectReason::EmptyRay;
    }
    if (preflight.emptyHit) {
        return GizmoPickRejectReason::EmptyHit;
    }
    if (preflight.invalidPickConfig) {
        return GizmoPickRejectReason::InvalidPickConfig;
    }
    if (preflight.invalidDimensions) {
        return GizmoPickRejectReason::InvalidDimensions;
    }
    if (preflight.outOfBounds) {
        return GizmoPickRejectReason::OutOfBounds;
    }
    if (preflight.screenMiss) {
        return GizmoPickRejectReason::ScreenMiss;
    }
    if (preflight.pickMiss) {
        return GizmoPickRejectReason::PickMiss;
    }
    return GizmoPickRejectReason::None;
}

GizmoSnapRejectReason classifySnapReject(const SnapPreflight& preflight) {
    if (preflight.snapDisabled) {
        return GizmoSnapRejectReason::SnapDisabled;
    }
    if (preflight.invalidStep) {
        return GizmoSnapRejectReason::InvalidStep;
    }
    return GizmoSnapRejectReason::None;
}

GizmoBeginDragRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight) {
    if (preflight.alreadyDragging) {
        return GizmoBeginDragRejectReason::AlreadyDragging;
    }
    if (preflight.nonFiniteRay) {
        return GizmoBeginDragRejectReason::NonFiniteRay;
    }
    if (preflight.nonFiniteHit) {
        return GizmoBeginDragRejectReason::NonFiniteHit;
    }
    if (preflight.emptyRay) {
        return GizmoBeginDragRejectReason::EmptyRay;
    }
    if (preflight.emptyHit) {
        return GizmoBeginDragRejectReason::EmptyHit;
    }
    if (preflight.invalidPickConfig) {
        return GizmoBeginDragRejectReason::InvalidPickConfig;
    }
    if (preflight.invalidDimensions) {
        return GizmoBeginDragRejectReason::InvalidDimensions;
    }
    if (preflight.outOfBounds) {
        return GizmoBeginDragRejectReason::OutOfBounds;
    }
    if (preflight.screenMiss) {
        return GizmoBeginDragRejectReason::ScreenMiss;
    }
    if (preflight.pickMiss) {
        return GizmoBeginDragRejectReason::PickMiss;
    }
    return GizmoBeginDragRejectReason::None;
}

GizmoUpdateDragRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight) {
    if (preflight.notDragging) {
        return GizmoUpdateDragRejectReason::NotDragging;
    }
    if (preflight.invalidActiveAxis) {
        return GizmoUpdateDragRejectReason::InvalidActiveAxis;
    }
    if (preflight.nonFiniteHit) {
        return GizmoUpdateDragRejectReason::NonFiniteHit;
    }
    if (preflight.invalidDimensions) {
        return GizmoUpdateDragRejectReason::InvalidDimensions;
    }
    if (preflight.emptyHit) {
        return GizmoUpdateDragRejectReason::EmptyHit;
    }
    if (preflight.outOfBounds) {
        return GizmoUpdateDragRejectReason::OutOfBounds;
    }
    return GizmoUpdateDragRejectReason::None;
}

GizmoEndDragRejectReason classifyEndDragReject(const EndDragPreflight& preflight) {
    if (preflight.notDragging) {
        return GizmoEndDragRejectReason::NotDragging;
    }
    return GizmoEndDragRejectReason::None;
}

bool preflightPickReady(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                        GizmoSpace space, f32 axisLength, f32 pickRadius,
                        GizmoPickRejectReason* reason) {
    const PickPreflight preflight =
        preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    if (reason != nullptr) {
        *reason = classifyPickReject(preflight);
    }
    return preflight.canPick();
}

bool preflightPickReady(const GizmoHitTest& hit, GizmoMode mode, GizmoPickRejectReason* reason) {
    const PickPreflight preflight = preflightPick(hit, mode);
    if (reason != nullptr) {
        *reason = classifyPickReject(preflight);
    }
    return preflight.canPick();
}

bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoSpace space, f32 axisLength, f32 pickRadius,
                      GizmoPickRejectReason& reason) {
    return preflightPickReady(ray, transform, mode, space, axisLength, pickRadius, &reason);
}

bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoPickRejectReason& reason) {
    return preflightPickReady(hit, mode, &reason);
}

bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius) {
    return !preflightPickReady(ray, transform, mode, space, axisLength, pickRadius);
}

bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode) {
    return !preflightPickReady(hit, mode);
}

bool preflightSnapReady(GizmoMode mode, const GizmoSnapSettings& settings,
                        GizmoSnapRejectReason* reason) {
    const SnapPreflight preflight = preflightSnap(mode, settings);
    if (reason != nullptr) {
        *reason = classifySnapReject(preflight);
    }
    return preflight.canApply();
}

bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
                      GizmoSnapRejectReason& reason) {
    return preflightSnapReady(mode, settings, &reason);
}

bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    return !preflightSnapReady(mode, settings);
}

bool preflightSnapDragReady(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings,
                            GizmoSnapDragRejectReason* reason) {
    const SnapDragPreflight preflight = preflightSnapDrag(delta, mode, settings);
    if (reason != nullptr) {
        *reason = classifySnapDragReject(preflight);
    }
    return preflight.canApply();
}

bool tryPreflightSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings,
                          GizmoSnapDragRejectReason& reason) {
    return preflightSnapDragReady(delta, mode, settings, &reason);
}

bool shouldSkipSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    return !preflightSnapDragReady(delta, mode, settings);
}

bool shouldSkipSnapDrag(GizmoMode mode, const GizmoSnapSettings& settings) {
    return shouldSkipSnap(mode, settings);
}

bool preflightPickInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                   GizmoMode mode, GizmoSpace space, f32 axisLength,
                                   f32 pickRadius, const GizmoSnapSettings& settings,
                                   GizmoPickRejectReason* reason) {
    const PickInteractionPreflight preflight =
        preflightPickInteraction(ray, transform, mode, space, axisLength, pickRadius, settings);
    if (reason != nullptr) {
        *reason = classifyPickReject(preflight.pick);
    }
    return preflight.canPick();
}

bool preflightPickInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
                                   const GizmoSnapSettings& settings,
                                   GizmoPickRejectReason* reason) {
    const PickInteractionPreflight preflight = preflightPickInteraction(hit, mode, settings);
    if (reason != nullptr) {
        *reason = classifyPickReject(preflight.pick);
    }
    return preflight.canPick();
}

bool shouldSkipPickInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                               GizmoMode mode, GizmoSpace space, f32 axisLength, f32 pickRadius,
                               const GizmoSnapSettings& settings) {
    return !preflightPickInteractionReady(ray, transform, mode, space, axisLength, pickRadius,
                                          settings);
}

bool shouldSkipPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
                               const GizmoSnapSettings& settings) {
    return !preflightPickInteractionReady(hit, mode, settings);
}

bool preflightBeginDragInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoMode mode, GizmoSpace space, f32 axisLength,
                                        f32 pickRadius, const GizmoSnapSettings& settings,
                                        GizmoBeginDragRejectReason* reason,
                                        bool alreadyDragging) {
    const BeginDragInteractionPreflight preflight = preflightBeginDragInteraction(
        ray, transform, mode, space, axisLength, pickRadius, settings, alreadyDragging);
    if (reason != nullptr) {
        *reason = classifyBeginDragReject(preflight.begin);
    }
    return preflight.canBegin();
}

bool preflightBeginDragInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
                                        const GizmoSnapSettings& settings,
                                        GizmoBeginDragRejectReason* reason,
                                        bool alreadyDragging) {
    const BeginDragInteractionPreflight preflight =
        preflightBeginDragInteraction(hit, mode, settings, alreadyDragging);
    if (reason != nullptr) {
        *reason = classifyBeginDragReject(preflight.begin);
    }
    return preflight.canBegin();
}

bool shouldSkipBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                    GizmoMode mode, GizmoSpace space, f32 axisLength,
                                    f32 pickRadius, const GizmoSnapSettings& settings,
                                    bool alreadyDragging) {
    return !preflightBeginDragInteractionReady(ray, transform, mode, space, axisLength, pickRadius,
                                               settings, nullptr, alreadyDragging);
}

bool shouldSkipBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                    const GizmoSnapSettings& settings, bool alreadyDragging) {
    return !preflightBeginDragInteractionReady(hit, mode, settings, nullptr, alreadyDragging);
}

bool preflightUpdateDragInteractionReady(const GizmoHitTest& hit, bool dragging,
                                         GizmoAxis activeAxis, GizmoMode mode,
                                         const GizmoSnapSettings& settings,
                                         GizmoUpdateDragRejectReason* reason) {
    return preflightUpdateDragInteractionReady(hit, dragging, activeAxis, mode, settings, 0.f,
                                               reason, nullptr);
}

bool preflightUpdateDragInteractionReady(const GizmoHitTest& hit, bool dragging,
                                         GizmoAxis activeAxis, GizmoMode mode,
                                         const GizmoSnapSettings& settings, f32 delta,
                                         GizmoUpdateDragRejectReason* reason,
                                         GizmoSnapDragRejectReason* snapDragReason) {
    const UpdateDragInteractionPreflight preflight =
        preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings, delta);
    if (reason != nullptr) {
        *reason = classifyUpdateDragReject(preflight.drag);
    }
    if (snapDragReason != nullptr) {
        *snapDragReason = classifySnapDragReject(preflight.snapDrag);
    }
    return preflight.canUpdate();
}

bool shouldSkipUpdateDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                     GizmoMode mode, const GizmoSnapSettings& settings) {
    return !preflightUpdateDragInteractionReady(hit, dragging, activeAxis, mode, settings);
}

bool preflightEndDragInteractionReady(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                      const GizmoSnapSettings& settings,
                                      GizmoEndDragRejectReason* reason) {
    const EndDragInteractionPreflight preflight =
        preflightEndDragInteraction(dragging, activeAxis, mode, settings);
    if (reason != nullptr) {
        *reason = classifyEndDragReject(preflight.end);
    }
    return preflight.canEnd();
}

bool shouldSkipEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                  const GizmoSnapSettings& settings) {
    return !preflightEndDragInteractionReady(dragging, activeAxis, mode, settings);
}

bool preflightBeginDragReady(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                             GizmoSpace space, f32 axisLength, f32 pickRadius,
                             GizmoBeginDragRejectReason* reason, bool alreadyDragging) {
    const BeginDragPreflight preflight =
        preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
    if (reason != nullptr) {
        *reason = classifyBeginDragReject(preflight);
    }
    return preflight.canBegin;
}

bool preflightBeginDragReady(const GizmoHitTest& hit, GizmoMode mode,
                             GizmoBeginDragRejectReason* reason, bool alreadyDragging) {
    const BeginDragPreflight preflight = preflightBeginDrag(hit, mode, alreadyDragging);
    if (reason != nullptr) {
        *reason = classifyBeginDragReject(preflight);
    }
    return preflight.canBegin;
}

bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoSpace space, f32 axisLength, f32 pickRadius,
                           GizmoBeginDragRejectReason& reason, bool alreadyDragging) {
    return preflightBeginDragReady(ray, transform, mode, space, axisLength, pickRadius, &reason,
                                   alreadyDragging);
}

bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                           GizmoBeginDragRejectReason& reason, bool alreadyDragging) {
    return preflightBeginDragReady(hit, mode, &reason, alreadyDragging);
}

bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius, bool alreadyDragging) {
    return !preflightBeginDragReady(ray, transform, mode, space, axisLength, pickRadius, nullptr,
                                    alreadyDragging);
}

bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging) {
    return !preflightBeginDragReady(hit, mode, nullptr, alreadyDragging);
}

bool preflightUpdateDragReady(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                              GizmoUpdateDragRejectReason* reason) {
    const UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis);
    if (reason != nullptr) {
        *reason = classifyUpdateDragReject(preflight);
    }
    return preflight.canUpdate();
}

bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoUpdateDragRejectReason& reason) {
    return preflightUpdateDragReady(hit, dragging, activeAxis, &reason);
}

bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    return !preflightUpdateDragReady(hit, dragging, activeAxis);
}

bool preflightEndDragReady(bool dragging, GizmoEndDragRejectReason* reason) {
    const EndDragPreflight preflight =
        preflightEndDrag(dragging, GizmoAxis::None, GizmoMode::Translate, {});
    if (reason != nullptr) {
        *reason = classifyEndDragReject(preflight);
    }
    return preflight.canEnd();
}

bool tryPreflightEndDrag(bool dragging, GizmoEndDragRejectReason& reason) {
    return preflightEndDragReady(dragging, &reason);
}

bool shouldSkipEndDrag(bool dragging) { return !preflightEndDragReady(dragging); }

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
    case GizmoMode::Translate: {
        const math::Vec3 snapped =
            snapPosition({out.posX, out.posY, out.posZ}, settings);
        out.posX = snapped.x;
        out.posY = snapped.y;
        out.posZ = snapped.z;
        break;
    }
    case GizmoMode::Rotate: {
        const math::Vec3 snapped =
            snapEulerRadians({out.rotX, out.rotY, out.rotZ}, settings);
        out.rotX = snapped.x;
        out.rotY = snapped.y;
        out.rotZ = snapped.z;
        break;
    }
    case GizmoMode::Scale: {
        const math::Vec3 snapped =
            snapScaleVec({out.scaleX, out.scaleY, out.scaleZ}, settings);
        out.scaleX = snapped.x;
        out.scaleY = snapped.y;
        out.scaleZ = snapped.z;
        break;
    }
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

EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap);
}

EndDragInteractionPreflight GizmoSystem::preflightEndDragInteraction() const {
    return fuse::editor::preflightEndDragInteraction(m_dragging, m_activeAxis, m_mode, m_snap);
}

DragInteractionPreflight GizmoSystem::preflightDragInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightDragInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

bool GizmoSystem::canPickInteraction(const GizmoRay& ray, const GizmoTransform& transform) const {
    return preflightPickInteraction(ray, transform).canPick();
}

bool GizmoSystem::canPickInteraction(const GizmoHitTest& hit) const {
    return preflightPickInteraction(hit).canPick();
}

bool GizmoSystem::canBeginDragInteraction(const GizmoHitTest& hit) const {
    return preflightBeginDragInteraction(hit).canBegin();
}

bool GizmoSystem::canBeginDragInteraction(const GizmoRay& ray,
                                          const GizmoTransform& transform) const {
    return preflightBeginDragInteraction(ray, transform).canBegin();
}

bool GizmoSystem::canUpdateDragInteraction(const GizmoHitTest& hit) const {
    return preflightUpdateDragInteraction(hit).canUpdate();
}

bool GizmoSystem::canEndDragInteraction() const {
    return preflightEndDragInteraction().canEnd();
}

bool GizmoSystem::canDragInteraction(const GizmoHitTest& hit) const {
    return preflightDragInteraction(hit).canInteract();
}

bool GizmoSystem::canEndDrag() const {
    return preflightEndDrag().canEnd();
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
    return fuse::editor::preflightBeginDrag(hit, m_mode, m_snap, m_dragging);
}

BeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoRay& ray,
                                                   const GizmoTransform& transform) const {
    return fuse::editor::preflightBeginDrag(ray, transform, m_mode, m_space, kAxisLength,
                                            kPickRadius, m_snap, m_dragging);
}

BeginDragInteractionPreflight GizmoSystem::preflightBeginDragInteraction(
    const GizmoHitTest& hit) const {
    return fuse::editor::preflightBeginDragInteraction(hit, m_mode, m_snap, m_dragging);
}

BeginDragInteractionPreflight GizmoSystem::preflightBeginDragInteraction(
    const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::preflightBeginDragInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                                       kPickRadius, m_snap, m_dragging);
}

PickPreflight GizmoSystem::preflightPick(const GizmoRay& ray,
                                         const GizmoTransform& transform) const {
    return fuse::editor::preflightPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

PickPreflight GizmoSystem::preflightPick(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPick(hit, m_mode);
}

PickInteractionPreflight GizmoSystem::preflightPickInteraction(
    const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::preflightPickInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                                  kPickRadius, m_snap);
}

PickInteractionPreflight GizmoSystem::preflightPickInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPickInteraction(hit, m_mode, m_snap);
}

SnapPreflight GizmoSystem::preflightSnap() const {
    return fuse::editor::preflightSnap(m_mode, m_snap);
}

SnapDragPreflight GizmoSystem::preflightSnapDrag(f32 delta) const {
    return fuse::editor::preflightSnapDrag(delta, m_mode, m_snap);
}

bool GizmoSystem::canApplySnapNow() const {
    return fuse::editor::canApplySnap(m_mode, m_snap);
}

bool GizmoSystem::trySnapTransform(const GizmoTransform& transform, GizmoTransform& out) const {
    return fuse::editor::trySnapTransform(transform, m_mode, m_snap, out);
}

UpdateDragPreflight GizmoSystem::preflightUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

UpdateDragInteractionPreflight GizmoSystem::preflightUpdateDragInteraction(
    const GizmoHitTest& hit) const {
    return preflightUpdateDragInteraction(hit, 0.f);
}

UpdateDragInteractionPreflight GizmoSystem::preflightUpdateDragInteraction(
    const GizmoHitTest& hit, f32 delta) const {
    return fuse::editor::preflightUpdateDragInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap,
                                                      delta);
}

bool GizmoSystem::canUpdateDrag(const GizmoHitTest& hit) const {
    return preflightUpdateDrag(hit).canUpdate();
}

PickSnapPreflight GizmoSystem::preflightPickSnap(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPickSnap(hit, m_mode, m_snap);
}

PickSnapPreflight GizmoSystem::preflightPickSnap(const GizmoRay& ray,
                                                 const GizmoTransform& transform) const {
    return fuse::editor::preflightPickSnap(ray, transform, m_mode, m_space, kAxisLength,
                                           kPickRadius, m_snap);
}

BeginInteractionPreflight GizmoSystem::preflightBeginInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightBeginInteraction(hit, m_mode, m_snap, m_dragging);
}

BeginInteractionPreflight GizmoSystem::preflightBeginInteraction(
    const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::preflightBeginInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                                   kPickRadius, m_snap, m_dragging);
}

UpdateInteractionPreflight GizmoSystem::preflightUpdateInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

EndInteractionPreflight GizmoSystem::preflightEndInteraction() const {
    return fuse::editor::preflightEndInteraction(m_dragging, m_activeAxis, m_mode, m_snap);
}

InteractionPreflight GizmoSystem::preflightInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

InteractionPreflight GizmoSystem::preflightInteraction(const GizmoRay& ray,
                                                       const GizmoTransform& transform) const {
    return fuse::editor::preflightInteraction(ray, transform, m_dragging, m_activeAxis, m_mode,
                                              m_space, kAxisLength, kPickRadius, m_snap);
}

bool GizmoSystem::canPickSnap(const GizmoHitTest& hit) const {
    return fuse::editor::canPickSnap(hit, m_mode, m_snap);
}

bool GizmoSystem::canPickSnap(const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::canPickSnap(ray, transform, m_mode, m_space, kAxisLength, kPickRadius,
                                     m_snap);
}

bool GizmoSystem::canBeginInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::canBeginInteraction(hit, m_mode, m_snap, m_dragging);
}

bool GizmoSystem::canBeginInteraction(const GizmoRay& ray,
                                      const GizmoTransform& transform) const {
    return fuse::editor::canBeginInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                             kPickRadius, m_snap, m_dragging);
}

bool GizmoSystem::canUpdateInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::canUpdateInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

bool GizmoSystem::canEndInteraction() const {
    return fuse::editor::canEndInteraction(m_dragging, m_activeAxis, m_mode, m_snap);
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
    if (!preflightUpdateDrag(hit).canUpdate()) {
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
    if (!tryEndDrag(result)) {
        return result;
    }
    return result;
}

bool GizmoSystem::tryEndDrag(GizmoResult& out) {
    out = {};
    if (!preflightEndDrag().canEnd()) {
        return false;
    }

    m_currentTransform = applySnapping_(m_currentTransform);
    markDirty_();

    out.active = false;
    out.changed = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;

    m_dragging = false;
    m_activeAxis = GizmoAxis::None;
    return true;
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

bool GizmoSystem::preflightPickReady(const GizmoRay& ray, const GizmoTransform& transform,
                                     GizmoPickRejectReason* reason) const {
    return fuse::editor::preflightPickReady(ray, transform, m_mode, m_space, kAxisLength,
                                            kPickRadius, reason);
}

bool GizmoSystem::preflightPickReady(const GizmoHitTest& hit,
                                     GizmoPickRejectReason* reason) const {
    return fuse::editor::preflightPickReady(hit, m_mode, reason);
}

bool GizmoSystem::tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform,
                                   GizmoPickRejectReason& reason) const {
    return fuse::editor::tryPreflightPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius,
                                          reason);
}

bool GizmoSystem::tryPreflightPick(const GizmoHitTest& hit, GizmoPickRejectReason& reason) const {
    return fuse::editor::tryPreflightPick(hit, m_mode, reason);
}

bool GizmoSystem::shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::shouldSkipPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

bool GizmoSystem::shouldSkipPick(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipPick(hit, m_mode);
}

bool GizmoSystem::preflightSnapReady(GizmoSnapRejectReason* reason) const {
    return fuse::editor::preflightSnapReady(m_mode, m_snap, reason);
}

bool GizmoSystem::tryPreflightSnap(GizmoSnapRejectReason& reason) const {
    return fuse::editor::tryPreflightSnap(m_mode, m_snap, reason);
}

bool GizmoSystem::shouldSkipSnap() const { return fuse::editor::shouldSkipSnap(m_mode, m_snap); }

bool GizmoSystem::preflightSnapDragReady(f32 delta, GizmoSnapDragRejectReason* reason) const {
    return fuse::editor::preflightSnapDragReady(delta, m_mode, m_snap, reason);
}

bool GizmoSystem::tryPreflightSnapDrag(f32 delta, GizmoSnapDragRejectReason& reason) const {
    return fuse::editor::tryPreflightSnapDrag(delta, m_mode, m_snap, reason);
}

bool GizmoSystem::shouldSkipSnapDrag(f32 delta) const {
    return fuse::editor::shouldSkipSnapDrag(delta, m_mode, m_snap);
}

bool GizmoSystem::shouldSkipSnapDrag() const {
    return fuse::editor::shouldSkipSnapDrag(m_mode, m_snap);
}

bool GizmoSystem::preflightPickInteractionReady(const GizmoRay& ray,
                                                const GizmoTransform& transform,
                                                GizmoPickRejectReason* reason) const {
    return fuse::editor::preflightPickInteractionReady(ray, transform, m_mode, m_space, kAxisLength,
                                                       kPickRadius, m_snap, reason);
}

bool GizmoSystem::preflightPickInteractionReady(const GizmoHitTest& hit,
                                                GizmoPickRejectReason* reason) const {
    return fuse::editor::preflightPickInteractionReady(hit, m_mode, m_snap, reason);
}

bool GizmoSystem::shouldSkipPickInteraction(const GizmoRay& ray,
                                            const GizmoTransform& transform) const {
    return fuse::editor::shouldSkipPickInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                                   kPickRadius, m_snap);
}

bool GizmoSystem::shouldSkipPickInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipPickInteraction(hit, m_mode, m_snap);
}

bool GizmoSystem::preflightBeginDragInteractionReady(
    const GizmoHitTest& hit, GizmoBeginDragRejectReason* reason) const {
    return fuse::editor::preflightBeginDragInteractionReady(hit, m_mode, m_snap, reason,
                                                            m_dragging);
}

bool GizmoSystem::preflightBeginDragInteractionReady(
    const GizmoRay& ray, const GizmoTransform& transform,
    GizmoBeginDragRejectReason* reason) const {
    return fuse::editor::preflightBeginDragInteractionReady(ray, transform, m_mode, m_space,
                                                            kAxisLength, kPickRadius, m_snap, reason,
                                                            m_dragging);
}

bool GizmoSystem::shouldSkipBeginDragInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipBeginDragInteraction(hit, m_mode, m_snap, m_dragging);
}

bool GizmoSystem::shouldSkipBeginDragInteraction(const GizmoRay& ray,
                                                 const GizmoTransform& transform) const {
    return fuse::editor::shouldSkipBeginDragInteraction(ray, transform, m_mode, m_space,
                                                        kAxisLength, kPickRadius, m_snap, m_dragging);
}

bool GizmoSystem::preflightUpdateDragInteractionReady(
    const GizmoHitTest& hit, GizmoUpdateDragRejectReason* reason) const {
    return fuse::editor::preflightUpdateDragInteractionReady(hit, m_dragging, m_activeAxis, m_mode,
                                                             m_snap, reason);
}

bool GizmoSystem::preflightUpdateDragInteractionReady(
    const GizmoHitTest& hit, f32 delta, GizmoUpdateDragRejectReason* reason,
    GizmoSnapDragRejectReason* snapDragReason) const {
    return fuse::editor::preflightUpdateDragInteractionReady(hit, m_dragging, m_activeAxis, m_mode,
                                                             m_snap, delta, reason, snapDragReason);
}

bool GizmoSystem::shouldSkipUpdateDragInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipUpdateDragInteraction(hit, m_dragging, m_activeAxis, m_mode,
                                                         m_snap);
}

bool GizmoSystem::preflightEndDragInteractionReady(GizmoEndDragRejectReason* reason) const {
    return fuse::editor::preflightEndDragInteractionReady(m_dragging, m_activeAxis, m_mode, m_snap,
                                                          reason);
}

bool GizmoSystem::shouldSkipEndDragInteraction() const {
    return fuse::editor::shouldSkipEndDragInteraction(m_dragging, m_activeAxis, m_mode, m_snap);
}

bool GizmoSystem::preflightBeginDragReady(const GizmoHitTest& hit,
                                          GizmoBeginDragRejectReason* reason) const {
    return fuse::editor::preflightBeginDragReady(hit, m_mode, reason, m_dragging);
}

bool GizmoSystem::preflightBeginDragReady(const GizmoRay& ray, const GizmoTransform& transform,
                                          GizmoBeginDragRejectReason* reason) const {
    return fuse::editor::preflightBeginDragReady(ray, transform, m_mode, m_space, kAxisLength,
                                                 kPickRadius, reason, m_dragging);
}

bool GizmoSystem::tryPreflightBeginDrag(const GizmoHitTest& hit,
                                        GizmoBeginDragRejectReason& reason) const {
    return fuse::editor::tryPreflightBeginDrag(hit, m_mode, reason, m_dragging);
}

bool GizmoSystem::tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoBeginDragRejectReason& reason) const {
    return fuse::editor::tryPreflightBeginDrag(ray, transform, m_mode, m_space, kAxisLength,
                                               kPickRadius, reason, m_dragging);
}

bool GizmoSystem::shouldSkipBeginDrag(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipBeginDrag(hit, m_mode, m_dragging);
}

bool GizmoSystem::shouldSkipBeginDrag(const GizmoRay& ray,
                                      const GizmoTransform& transform) const {
    return fuse::editor::shouldSkipBeginDrag(ray, transform, m_mode, m_space, kAxisLength,
                                             kPickRadius, m_dragging);
}

bool GizmoSystem::preflightUpdateDragReady(const GizmoHitTest& hit,
                                           GizmoUpdateDragRejectReason* reason) const {
    return fuse::editor::preflightUpdateDragReady(hit, m_dragging, m_activeAxis, reason);
}

bool GizmoSystem::tryPreflightUpdateDrag(const GizmoHitTest& hit,
                                         GizmoUpdateDragRejectReason& reason) const {
    return fuse::editor::tryPreflightUpdateDrag(hit, m_dragging, m_activeAxis, reason);
}

bool GizmoSystem::shouldSkipUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipUpdateDrag(hit, m_dragging, m_activeAxis);
}

bool GizmoSystem::preflightEndDragReady(GizmoEndDragRejectReason* reason) const {
    return fuse::editor::preflightEndDragReady(m_dragging, reason);
}

bool GizmoSystem::tryPreflightEndDrag(GizmoEndDragRejectReason& reason) const {
    return fuse::editor::tryPreflightEndDrag(m_dragging, reason);
}

bool GizmoSystem::shouldSkipEndDrag() const { return fuse::editor::shouldSkipEndDrag(m_dragging); }

} // namespace fuse::editor

// --- deepen additive from deepen-b6-gizmo-begin-drag-snap-ce02 ---
    return tryPickAxis(ray, transform, mode, space, axisLength, pickRadius, axis);
    return tryPickAxis(hit, mode, axis);

// --- deepen additive from deepen-b6-gizmo-begin-drag-snap-bfd5 ---
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, const GizmoTransform& transform,
    BeginDragPreflight out{};
    if (!tryPickAxis(hit, mode, axis)) {
    if (!tryPickAxis(ray, transform, mode, space, axisLength, pickRadius, axis)) {
BeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoHitTest& hit,
    return fuse::editor::preflightBeginDrag(hit, current, m_mode);
    return fuse::editor::preflightBeginDrag(ray, current, m_mode, m_space, kAxisLength,

// --- deepen additive from deepen-b6-gizmo-begin-drag-snap-guards-7da7 ---
GizmoBeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
    GizmoBeginDragPreflight preflight{};
GizmoBeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
    const GizmoBeginDragPreflight preflight =
        fuse::editor::preflightBeginDrag(hit, m_mode, m_dragging, m_snap);
    const GizmoBeginDragPreflight preflight = fuse::editor::preflightBeginDrag(
GizmoBeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoHitTest& hit,
    return fuse::editor::preflightBeginDrag(hit, m_mode, m_dragging, m_snap);
GizmoBeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoRay& ray,

// --- deepen additive from deepen-b6-gizmo-pick-snap-preflight-2b84 ---
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging) {
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging);
    return preflightUpdateDrag(hit, dragging).canUpdate();
    return preflightUpdateDrag(hit, dragging, mode, settings).canUpdate();
    return fuse::editor::preflightBeginDrag(hit, m_mode, m_dragging);
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_mode, m_snap);

// --- deepen additive from deepen-b6-gizmo-pick-snap-preflight-1066 ---
bool trySnapValue(f32 value, GizmoMode mode, const GizmoSnapSettings& settings, f32& out) {
    return preflightUpdateDrag(hit, dragging, mode, activeAxis).canUpdate();
EndDragPreflight preflightEndDrag(bool dragging) {
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_mode, m_activeAxis);
    return fuse::editor::preflightEndDrag(m_dragging);

// --- deepen additive from deepen-b6-gizmo-preflights-1fb6 ---
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
bool shouldSkipPick(const PickPreflight& preflight) { return !preflight.canPick(); }
    return shouldSkipPick(preflightPick(ray, transform, mode, space, axisLength, pickRadius));
    return shouldSkipPick(preflightPick(hit, mode));
bool shouldSkipSnap(const SnapPreflight& preflight) { return !preflight.canApply(); }
    return shouldSkipSnap(preflightSnap(mode, settings));
bool shouldSkipUpdateDrag(const UpdateDragPreflight& preflight) { return !preflight.canUpdate(); }
    return shouldSkipUpdateDrag(preflightUpdateDrag(hit, dragging));
    return shouldSkipUpdateDrag(preflightUpdateDrag(hit, dragging, activeAxis));
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis);

// --- deepen additive from deepen-gizmo-end-drag-preflights-339b ---
SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
    if (trySnapTransform(transform, mode, settings, snapped) &&
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode) {
    return preflightUpdateDrag(hit, dragging, mode).canUpdate();
    return preflightEndDrag(dragging).canEnd();
SnapPreflight GizmoSystem::preflightSnap(const GizmoTransform& transform) const {
    return fuse::editor::preflightSnap(m_mode, m_snap, transform);
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_mode);

// --- deepen additive from deepen-gizmo-preflights-9967 ---
    return fuse::editor::preflightUpdateDrag(hit, m_dragging);

// --- deepen additive from deepen-gizmo-end-drag-preflights-fffd ---
    return preflightUpdateDrag(hit, dragging, GizmoMode::Translate);

// --- deepen additive from deepen-b6-gizmo-end-drag-preflight-709c ---
EndDragPreflight preflightEndDrag(bool dragging, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightEndDrag(dragging, mode, settings).canEnd();
    return fuse::editor::preflightEndDrag(m_dragging, m_mode, m_snap);

// --- deepen additive from deepen-b6-gizmo-end-drag-preflight-b429 ---
    return preflightEndDrag(dragging, GizmoMode::Translate, GizmoSnapSettings{}).canEnd();

// --- deepen additive from deepen-b6-gizmo-end-drag-preflight-9b89 ---
    return preflightEndDrag(dragging).canEnd;
    if (!preflightEndDrag().canEnd) {

// --- deepen additive from gizmo-end-drag-preflight-67f9 ---
    EndDragPreflight preflight = preflightEndDrag(dragging);

// --- deepen additive from deepen-b6-gizmo-end-drag-preflight-1fb5 ---
EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis) {
    return preflightEndDrag(dragging, activeAxis).canEnd();
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis);

// --- deepen additive from deepen-gizmo-preflights-9a97 ---
    return preflightEndDrag(dragging, activeAxis, GizmoMode::Translate, GizmoSnapSettings{}).canEnd();
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap).canEnd();

// --- deepen additive from deepen-gizmo-preflights-907f ---
    EndDragPreflight preflight = preflightEndDrag(dragging, activeAxis, mode, settings);
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap,

// --- deepen additive from deepen-gizmo-preflight-guards-ff59 ---
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis, mode);
    return preflightUpdateDrag(hit, dragging, activeAxis, mode).canUpdate();
f32 GizmoSystem::trySnapDragDelta(f32 delta) const {
    return fuse::editor::trySnapDragDelta(delta, m_mode, m_snap);
    const UpdateDragPreflight preflight = preflightUpdateDrag(hit);
    const f32 delta = this->trySnapDragDelta(dx + dy);

// --- deepen additive from deepen-b6-gizmo-preflight-guards-e028 ---
GizmoInteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
    GizmoInteractionPreflight preflight{};
        preflightUpdateDrag({}, dragging, activeAxis, mode, settings);
GizmoInteractionPreflight preflightInteraction(const GizmoHitTest& hit, GizmoMode mode,
    preflight.begin = preflightBeginDrag(hit, mode, settings, dragging);
GizmoInteractionPreflight GizmoSystem::preflightInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightInteraction(hit, m_mode, m_dragging, m_activeAxis, m_snap);
GizmoInteractionPreflight GizmoSystem::preflightInteraction(
    return fuse::editor::preflightInteraction(ray, transform, m_mode, m_space, kAxisLength,

// --- deepen additive from deepen-b6-gizmo-preflight-guards-1907 ---
const char* pickRejectReasonName(PickRejectReason reason) {
    case PickRejectReason::None:
    case PickRejectReason::EmptyRay:
    case PickRejectReason::EmptyHit:
    case PickRejectReason::InvalidPickConfig:
    case PickRejectReason::ScreenOutOfBounds:
    case PickRejectReason::ScreenMiss:
    case PickRejectReason::PickMiss:
PickRejectReason pickRejectReason(const GizmoRay& ray, const GizmoTransform& transform,
        return PickRejectReason::EmptyRay;
        return PickRejectReason::InvalidPickConfig;
        return PickRejectReason::PickMiss;
    return PickRejectReason::None;
PickRejectReason pickRejectReason(const GizmoHitTest& hit, GizmoMode mode) {
        return PickRejectReason::EmptyHit;
        return PickRejectReason::ScreenOutOfBounds;
        return PickRejectReason::ScreenMiss;
                          PickRejectReason expected) {
    return pickRejectReason(ray, transform, mode, space, axisLength, pickRadius) == expected;
bool pickRejectsForReason(const GizmoHitTest& hit, GizmoMode mode, PickRejectReason expected) {
    return pickRejectReason(hit, mode) == expected;
    preflight.reason = pickRejectReason(ray, transform, mode, space, axisLength, pickRadius);
    preflight.emptyRay = preflight.reason == PickRejectReason::EmptyRay;
    preflight.invalidPickConfig = preflight.reason == PickRejectReason::InvalidPickConfig;
    preflight.pickMiss = preflight.reason == PickRejectReason::PickMiss;
    preflight.reason = pickRejectReason(hit, mode);
    preflight.emptyHit = preflight.reason == PickRejectReason::EmptyHit;
    preflight.screenOutOfBounds = preflight.reason == PickRejectReason::ScreenOutOfBounds;
    preflight.screenMiss = preflight.reason == PickRejectReason::ScreenMiss;
const char* snapRejectReasonName(SnapRejectReason reason) {
    case SnapRejectReason::None:
    case SnapRejectReason::Disabled:
    case SnapRejectReason::InvalidStep:
SnapRejectReason snapRejectReason(GizmoMode mode, const GizmoSnapSettings& settings) {
        return SnapRejectReason::Disabled;
        return SnapRejectReason::InvalidStep;
    return SnapRejectReason::None;
                          SnapRejectReason expected) {
    return snapRejectReason(mode, settings) == expected;
    preflight.reason = snapRejectReason(mode, settings);
    preflight.snapDisabled = preflight.reason == SnapRejectReason::Disabled;
    preflight.invalidStep = preflight.reason == SnapRejectReason::InvalidStep;
const char* updateDragRejectReasonName(UpdateDragRejectReason reason) {
    case UpdateDragRejectReason::None:
    case UpdateDragRejectReason::NotDragging:
    case UpdateDragRejectReason::EmptyHit:
    case UpdateDragRejectReason::ScreenOutOfBounds:
    case UpdateDragRejectReason::InvalidActiveAxis:
UpdateDragRejectReason updateDragRejectReason(const GizmoHitTest& hit, bool dragging,
        return UpdateDragRejectReason::NotDragging;
        return UpdateDragRejectReason::InvalidActiveAxis;
        return UpdateDragRejectReason::EmptyHit;
        return UpdateDragRejectReason::ScreenOutOfBounds;
    return UpdateDragRejectReason::None;
                                UpdateDragRejectReason expected) {
    return updateDragRejectReason(hit, dragging, activeAxis) == expected;
    preflight.reason = updateDragRejectReason(hit, dragging, activeAxis);
    preflight.notDragging = preflight.reason == UpdateDragRejectReason::NotDragging;
    preflight.invalidActiveAxis = preflight.reason == UpdateDragRejectReason::InvalidActiveAxis;
    preflight.emptyHit = preflight.reason == UpdateDragRejectReason::EmptyHit;
    preflight.screenOutOfBounds = preflight.reason == UpdateDragRejectReason::ScreenOutOfBounds;
const char* endDragRejectReasonName(EndDragRejectReason reason) {
    case EndDragRejectReason::None:
    case EndDragRejectReason::NotDragging:
EndDragRejectReason endDragRejectReason(bool dragging) {
        return EndDragRejectReason::NotDragging;
    return EndDragRejectReason::None;
bool endDragRejectsForReason(bool dragging, EndDragRejectReason expected) {
    return endDragRejectReason(dragging) == expected;
    preflight.reason = endDragRejectReason(dragging);
    preflight.notDragging = preflight.reason == EndDragRejectReason::NotDragging;
const char* beginDragRejectReasonName(BeginDragRejectReason reason) {
    case BeginDragRejectReason::None:
    case BeginDragRejectReason::AlreadyDragging:
    case BeginDragRejectReason::EmptyRay:
    case BeginDragRejectReason::EmptyHit:
    case BeginDragRejectReason::InvalidPickConfig:
    case BeginDragRejectReason::ScreenOutOfBounds:
    case BeginDragRejectReason::ScreenMiss:
    case BeginDragRejectReason::PickMiss:
BeginDragRejectReason beginDragRejectReason(const GizmoRay& ray, const GizmoTransform& transform,
        return BeginDragRejectReason::AlreadyDragging;
    const PickRejectReason pickReason =
        pickRejectReason(ray, transform, mode, space, axisLength, pickRadius);
        return BeginDragRejectReason::EmptyRay;
        return BeginDragRejectReason::InvalidPickConfig;
        return BeginDragRejectReason::PickMiss;
        return BeginDragRejectReason::None;
BeginDragRejectReason beginDragRejectReason(const GizmoHitTest& hit, GizmoMode mode,
    const PickRejectReason pickReason = pickRejectReason(hit, mode);
        return BeginDragRejectReason::EmptyHit;
        return BeginDragRejectReason::ScreenOutOfBounds;
        return BeginDragRejectReason::ScreenMiss;
                               BeginDragRejectReason expected, bool alreadyDragging) {
    return beginDragRejectReason(ray, transform, mode, space, axisLength, pickRadius,
    return beginDragRejectReason(hit, mode, alreadyDragging) == expected;
        beginDragRejectReason(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
    preflight.alreadyDragging = preflight.reason == BeginDragRejectReason::AlreadyDragging;
    preflight.emptyRay = preflight.reason == BeginDragRejectReason::EmptyRay;
    preflight.invalidPickConfig = preflight.reason == BeginDragRejectReason::InvalidPickConfig;
    preflight.pickMiss = preflight.reason == BeginDragRejectReason::PickMiss;
    if (preflight.reason != BeginDragRejectReason::None) {
    preflight.reason = beginDragRejectReason(hit, mode, alreadyDragging);
    preflight.emptyHit = preflight.reason == BeginDragRejectReason::EmptyHit;
    preflight.screenOutOfBounds = preflight.reason == BeginDragRejectReason::ScreenOutOfBounds;
    preflight.screenMiss = preflight.reason == BeginDragRejectReason::ScreenMiss;
UpdateDragPreflight GizmoSystem::preflightUpdateDragWithSnap(const GizmoHitTest& hit) const {

// --- deepen additive from deepen-b6-gizmo-preflight-guards-4f45 ---
SnapDragPreflight preflightSnapDrag(GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightSnapDrag(mode, settings).canApply();
    const SnapDragPreflight snapDrag = preflightSnapDrag(mode, settings);
DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
    preflight.snap = preflightSnapDrag(mode, settings);
SnapDragPreflight GizmoSystem::preflightSnapDrag() const {
    return fuse::editor::preflightSnapDrag(m_mode, m_snap);
    return fuse::editor::preflightDragInteraction(hit, m_mode, m_snap, m_dragging, m_activeAxis,

// --- deepen additive from deepen-b6-gizmo-preflight-guards-e564 ---
void applyBeginDragSnapDegraded_(BeginDragPreflight& preflight, GizmoMode mode,
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, {}, alreadyDragging);
GizmoPickRejectReason GizmoSystem::classifyPickReject(const PickPreflight& preflight) const {
    return fuse::editor::classifyPickReject(preflight);
GizmoBeginDragRejectReason GizmoSystem::classifyBeginDragReject(
    const BeginDragPreflight& preflight) const {
    return fuse::editor::classifyBeginDragReject(preflight);
GizmoUpdateDragRejectReason GizmoSystem::classifyUpdateDragReject(
    const UpdateDragPreflight& preflight) const {
    return fuse::editor::classifyUpdateDragReject(preflight);
GizmoEndDragRejectReason GizmoSystem::classifyEndDragReject(
    const EndDragPreflight& preflight) const {
    return fuse::editor::classifyEndDragReject(preflight);

// --- deepen additive from gizmo-preflight-guards-8adb ---
BeginDragPreflight buildBeginDragFromPick_(const PickPreflight& pick, bool alreadyDragging) {
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging,
    return fuse::editor::preflightInteraction(hit, m_mode, m_snap);

// --- deepen additive from deepen-b6-gizmo-preflights-deb7 ---
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging)
    return preflightBeginDrag(hit, mode, alreadyDragging).canBegin;

// --- deepen additive from deepen-gizmo-preflight-guards-f71a ---
void applyPickToBeginDragPreflight(const PickPreflight& pick, BeginDragPreflight& preflight) {
void applySnapDegradedToBeginDragPreflight(GizmoMode mode, const GizmoSnapSettings& settings,
    applyPickToBeginDragPreflight(pick, preflight);
    applySnapDegradedToBeginDragPreflight(mode, settings, preflight);
DragUpdateFramePreflight preflightDragUpdateFrame(const GizmoHitTest& hit, bool dragging,
    DragUpdateFramePreflight frame{};
    frame.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    frame.snap = preflightSnap(mode, settings);
DragUpdateFramePreflight GizmoSystem::preflightDragUpdateFrame(const GizmoHitTest& hit) const {
    return fuse::editor::preflightDragUpdateFrame(hit, m_dragging, m_activeAxis, m_mode, m_snap);

// --- deepen additive from deepen-b6-gizmo-interaction-preflights-c853 ---
PickSnapPreflight preflightPickSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    PickSnapPreflight preflight = preflightPickSnap(mode, settings);
    preflight.drag = preflightEndDrag(dragging, activeAxis, mode, settings);
BeginInteractionPreflight preflightBeginInteraction(const GizmoRay& ray,
    preflight.begin = preflightBeginDrag(hit, mode, alreadyDragging, settings);
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, false, settings)
    return preflightBeginDrag(hit, mode, false, settings).canBegin;

// --- deepen additive from deepen-b6-gizmo-preflight-guards-5c39 ---
    const f32 delta = fuse::editor::trySnapDragDelta(dx + dy, m_mode, m_snap);

// --- deepen additive from deepen-b6-gizmo-interaction-preflights-10fa ---
void annotateBeginDragSnapDegraded_(BeginDragPreflight& preflight, GizmoMode mode,
InteractionPreflight preflightInteraction(const GizmoHitTest& hit, GizmoMode mode, bool dragging,

// --- deepen additive from gizmo-interaction-preflight-guards-d31b ---
SnapDragPreflight preflightSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings) {
SnapDragPreflight GizmoSystem::preflightSnapDragDelta() const {
    return fuse::editor::preflightSnapDragDelta(m_mode, m_snap);
    return preflightSnapDragDelta().canApply();
    return preflightBeginInteraction(hit).canBegin();
    return preflightBeginInteraction(ray, transform).canBegin();
    return preflightUpdateInteraction(hit).canUpdate();
    return preflightEndInteraction().canEnd();

// --- deepen additive from gizmo-preflight-reject-reasons-f595 ---
GizmoInteractionRejectReason PickPreflight::rejectReason() const {
        return GizmoInteractionRejectReason::EmptyRay;
        return GizmoInteractionRejectReason::EmptyHit;
        return GizmoInteractionRejectReason::NonFiniteInput;
        return GizmoInteractionRejectReason::InvalidPickConfig;
        return GizmoInteractionRejectReason::InvalidDimensions;
        return GizmoInteractionRejectReason::OutOfBounds;
        return GizmoInteractionRejectReason::ScreenMiss;
        return GizmoInteractionRejectReason::PickMiss;
    return GizmoInteractionRejectReason::None;
GizmoInteractionRejectReason SnapPreflight::rejectReason() const {
        return GizmoInteractionRejectReason::SnapDisabled;
        return GizmoInteractionRejectReason::InvalidSnapStep;
GizmoInteractionRejectReason BeginDragPreflight::rejectReason() const {
        return GizmoInteractionRejectReason::AlreadyDragging;
GizmoInteractionRejectReason UpdateDragPreflight::rejectReason() const {
        return GizmoInteractionRejectReason::NotDragging;
        return GizmoInteractionRejectReason::InvalidActiveAxis;
        return GizmoInteractionRejectReason::ModeAxisMismatch;
GizmoInteractionRejectReason EndDragPreflight::rejectReason() const {
    return !preflightPick(ray, transform, mode, space, axisLength, pickRadius).canPick();
    return !preflightPick(hit, mode).canPick();
    return !preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging)
    return !preflightBeginDrag(hit, mode, alreadyDragging).canBegin;
    return !preflightUpdateDrag(hit, dragging, activeAxis, mode, {}).canUpdate();
    return !preflightEndDrag(dragging, GizmoAxis::None, GizmoMode::Translate, {}).canEnd();

// --- deepen additive from b6-gizmo-reject-reason-preflights-509d ---
const char* gizmoPickGuardRejectReasonLabel(GizmoPickGuardRejectReason reason) {
    case GizmoPickGuardRejectReason::None:
    case GizmoPickGuardRejectReason::EmptyRay:
    case GizmoPickGuardRejectReason::EmptyHit:
    case GizmoPickGuardRejectReason::InvalidPickConfig:
    case GizmoPickGuardRejectReason::InvalidDimensions:
    case GizmoPickGuardRejectReason::OutOfBounds:
    case GizmoPickGuardRejectReason::ScreenMiss:
    case GizmoPickGuardRejectReason::PickMiss:
const char* gizmoSnapGuardRejectReasonLabel(GizmoSnapGuardRejectReason reason) {
    case GizmoSnapGuardRejectReason::None:
    case GizmoSnapGuardRejectReason::SnapDisabled:
    case GizmoSnapGuardRejectReason::InvalidStep:
const char* gizmoBeginDragGuardRejectReasonLabel(GizmoBeginDragGuardRejectReason reason) {
    case GizmoBeginDragGuardRejectReason::None:
    case GizmoBeginDragGuardRejectReason::AlreadyDragging:
    case GizmoBeginDragGuardRejectReason::EmptyRay:
    case GizmoBeginDragGuardRejectReason::EmptyHit:
    case GizmoBeginDragGuardRejectReason::InvalidPickConfig:
    case GizmoBeginDragGuardRejectReason::InvalidDimensions:
    case GizmoBeginDragGuardRejectReason::OutOfBounds:
    case GizmoBeginDragGuardRejectReason::ScreenMiss:
    case GizmoBeginDragGuardRejectReason::PickMiss:
const char* gizmoUpdateDragGuardRejectReasonLabel(GizmoUpdateDragGuardRejectReason reason) {
    case GizmoUpdateDragGuardRejectReason::None:
    case GizmoUpdateDragGuardRejectReason::NotDragging:
    case GizmoUpdateDragGuardRejectReason::EmptyHit:
    case GizmoUpdateDragGuardRejectReason::InvalidDimensions:
    case GizmoUpdateDragGuardRejectReason::OutOfBounds:
    case GizmoUpdateDragGuardRejectReason::InvalidActiveAxis:
const char* gizmoEndDragGuardRejectReasonLabel(GizmoEndDragGuardRejectReason reason) {
    case GizmoEndDragGuardRejectReason::None:
    case GizmoEndDragGuardRejectReason::NotDragging:
GizmoPickGuardRejectReason classifyPickReject(const PickPreflight& preflight) {
        return GizmoPickGuardRejectReason::EmptyRay;
        return GizmoPickGuardRejectReason::EmptyHit;
        return GizmoPickGuardRejectReason::InvalidPickConfig;
        return GizmoPickGuardRejectReason::InvalidDimensions;
        return GizmoPickGuardRejectReason::OutOfBounds;
        return GizmoPickGuardRejectReason::ScreenMiss;
        return GizmoPickGuardRejectReason::PickMiss;
    return GizmoPickGuardRejectReason::None;
GizmoSnapGuardRejectReason classifySnapReject(const SnapPreflight& preflight) {
        return GizmoSnapGuardRejectReason::SnapDisabled;
        return GizmoSnapGuardRejectReason::InvalidStep;
    return GizmoSnapGuardRejectReason::None;
GizmoBeginDragGuardRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight) {
        return GizmoBeginDragGuardRejectReason::AlreadyDragging;
        return GizmoBeginDragGuardRejectReason::EmptyRay;
        return GizmoBeginDragGuardRejectReason::EmptyHit;
        return GizmoBeginDragGuardRejectReason::InvalidPickConfig;
        return GizmoBeginDragGuardRejectReason::InvalidDimensions;
        return GizmoBeginDragGuardRejectReason::OutOfBounds;
        return GizmoBeginDragGuardRejectReason::ScreenMiss;
        return GizmoBeginDragGuardRejectReason::PickMiss;
    return GizmoBeginDragGuardRejectReason::None;
GizmoUpdateDragGuardRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight) {
        return GizmoUpdateDragGuardRejectReason::NotDragging;
        return GizmoUpdateDragGuardRejectReason::InvalidActiveAxis;
        return GizmoUpdateDragGuardRejectReason::InvalidDimensions;
        return GizmoUpdateDragGuardRejectReason::EmptyHit;
        return GizmoUpdateDragGuardRejectReason::OutOfBounds;
    return GizmoUpdateDragGuardRejectReason::None;
GizmoEndDragGuardRejectReason classifyEndDragReject(const EndDragPreflight& preflight) {
        return GizmoEndDragGuardRejectReason::NotDragging;
    return GizmoEndDragGuardRejectReason::None;
                      GizmoPickGuardRejectReason& reason) {
    const PickPreflight preflight = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoPickGuardRejectReason& reason) {
                      GizmoSnapGuardRejectReason& reason) {
                           GizmoBeginDragGuardRejectReason& reason, bool alreadyDragging) {
                           const GizmoSnapSettings& settings, GizmoBeginDragGuardRejectReason& reason,
    const BeginDragPreflight preflight = preflightBeginDrag(ray, transform, mode, space, axisLength,
    const BeginDragPreflight preflight = preflightBeginDrag(hit, mode, settings, alreadyDragging);
                            GizmoUpdateDragGuardRejectReason& reason) {
bool tryPreflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                         const GizmoSnapSettings& settings, GizmoEndDragGuardRejectReason& reason) {
    const EndDragPreflight preflight = preflightEndDrag(dragging, activeAxis, mode, settings);
    GizmoPickGuardRejectReason reason = GizmoPickGuardRejectReason::None;
    return !tryPreflightPick(ray, transform, mode, space, axisLength, pickRadius, reason);
    return !tryPreflightPick(hit, mode, reason);
    GizmoSnapGuardRejectReason reason = GizmoSnapGuardRejectReason::None;
    return !tryPreflightSnap(mode, settings, reason);
    GizmoBeginDragGuardRejectReason reason = GizmoBeginDragGuardRejectReason::None;
    return !tryPreflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, reason,
    return !tryPreflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, settings,
    return !tryPreflightBeginDrag(hit, mode, reason, alreadyDragging);
    return !tryPreflightBeginDrag(hit, mode, settings, reason, alreadyDragging);
    GizmoUpdateDragGuardRejectReason reason = GizmoUpdateDragGuardRejectReason::None;
    return !tryPreflightUpdateDrag(hit, dragging, activeAxis, reason);
    return !tryPreflightUpdateDrag(hit, dragging, activeAxis, mode, settings, reason);
    GizmoEndDragGuardRejectReason reason = GizmoEndDragGuardRejectReason::None;
    return !tryPreflightEndDrag(dragging, activeAxis, GizmoMode::Translate, {}, reason);
    return !tryPreflightEndDrag(dragging, activeAxis, mode, settings, reason);
                                   GizmoPickGuardRejectReason& reason) const {
bool GizmoSystem::tryPreflightSnap(GizmoSnapGuardRejectReason& reason) const {
                                        GizmoBeginDragGuardRejectReason& reason) const {
    return fuse::editor::tryPreflightBeginDrag(hit, m_mode, m_snap, reason, m_dragging);
                                         GizmoUpdateDragGuardRejectReason& reason) const {
    return fuse::editor::tryPreflightUpdateDrag(hit, m_dragging, m_activeAxis, m_mode, m_snap,
bool GizmoSystem::tryPreflightEndDrag(GizmoEndDragGuardRejectReason& reason) const {
    return fuse::editor::tryPreflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap, reason);

// --- deepen additive from deepen-b6-gizmo-guards-9adb ---
    case GizmoPickRejectReason::NonFiniteInput:
    case GizmoBeginDragRejectReason::NonFiniteInput:
    case GizmoUpdateDragRejectReason::NonFiniteInput:
        return GizmoPickRejectReason::NonFiniteInput;
        return GizmoBeginDragRejectReason::NonFiniteInput;
        return GizmoUpdateDragRejectReason::NonFiniteInput;
                      GizmoSpace space, f32 axisLength, f32 pickRadius, PickPreflight& out,
    out = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    reason = classifyPickReject(out);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, PickPreflight& out,
    out = preflightPick(hit, mode);
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings, SnapPreflight& out,
    out = preflightSnap(mode, settings);
    reason = classifySnapReject(out);
                           BeginDragPreflight& out, GizmoBeginDragRejectReason& reason) {
    out = preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, settings,
    reason = classifyBeginDragReject(out);
    out = preflightBeginDrag(hit, mode, settings, alreadyDragging);
                            UpdateDragPreflight& out, GizmoUpdateDragRejectReason& reason) {
    out = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    reason = classifyUpdateDragReject(out);
                         const GizmoSnapSettings& settings, EndDragPreflight& out,
    out = preflightEndDrag(dragging, activeAxis, mode, settings);
    reason = classifyEndDragReject(out);
bool GizmoSystem::tryPreflightPick(const GizmoHitTest& hit, PickPreflight& out,
    return fuse::editor::tryPreflightPick(hit, m_mode, out, reason);
                                   PickPreflight& out, GizmoPickRejectReason& reason) const {
bool GizmoSystem::tryPreflightSnap(SnapPreflight& out, GizmoSnapRejectReason& reason) const {
    return fuse::editor::tryPreflightSnap(m_mode, m_snap, out, reason);
bool GizmoSystem::tryPreflightBeginDrag(const GizmoHitTest& hit, BeginDragPreflight& out,
    return fuse::editor::tryPreflightBeginDrag(hit, m_mode, m_snap, m_dragging, out, reason);
                                          BeginDragPreflight& out,
bool GizmoSystem::tryPreflightUpdateDrag(const GizmoHitTest& hit, UpdateDragPreflight& out,
    return fuse::editor::tryPreflightUpdateDrag(hit, m_dragging, m_activeAxis, m_mode, m_snap, out,
bool GizmoSystem::tryPreflightEndDrag(EndDragPreflight& out,
    return fuse::editor::tryPreflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap, out, reason);

// --- deepen additive from deepen-gizmo-preflight-guards-2e07 ---
    return preflightInteraction(hit, dragging, activeAxis, mode, settings).canActOnPhase();
    return preflightInteraction(ray, transform, dragging, activeAxis, mode, space, axisLength,
    return preflightInteraction(hit).canActOnPhase();
    return preflightInteraction(ray, transform).canActOnPhase();

// --- deepen additive from deepen-gizmo-preflights-8404 ---
PhaseActionPreflight preflightPhaseAction(const GizmoHitTest& hit, bool dragging,
    const InteractionPreflight interaction =
        preflightInteraction(hit, dragging, activeAxis, mode, settings);
    PhaseActionPreflight preflight{};
    return preflightPhaseAction(hit, dragging, activeAxis, mode, settings).canAct;
    return preflightPhaseAction(hit, dragging, activeAxis, mode, settings).canInteract;
    return preflightInteraction(hit, dragging, activeAxis, mode, settings).snapDegradedOnPhase();
    return preflightInteraction(hit, dragging, activeAxis, mode, settings).snapWillApplyOnPhase();
    return preflightInteraction(hit).canInteractOnPhase();
    return preflightInteraction(m_lastHit).snapDegradedOnPhase();
    return preflightInteraction(m_lastHit).snapWillApplyOnPhase();
PhaseActionPreflight GizmoSystem::preflightPhaseAction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPhaseAction(hit, m_dragging, m_activeAxis, m_mode, m_snap);

// --- deepen additive from deepen-b6-gizmo-preflight-reject-reasons-9a19 ---
                      GizmoPickRejectReason& outReason) {
    outReason = classifyPickReject(out);
                      GizmoSnapRejectReason& outReason) {
    outReason = classifySnapReject(out);
                           const GizmoSnapSettings& settings, BeginDragPreflight& out,
                           GizmoBeginDragRejectReason& outReason, bool alreadyDragging) {
    outReason = classifyBeginDragReject(out);
                            UpdateDragPreflight& out, GizmoUpdateDragRejectReason& outReason) {
    outReason = classifyUpdateDragReject(out);
                         GizmoEndDragRejectReason& outReason) {
    outReason = classifyEndDragReject(out);
bool shouldSkipBeginDrag(const BeginDragPreflight& preflight) {
bool shouldSkipEndDrag(const EndDragPreflight& preflight) {
GizmoEndDragRejectReason GizmoSystem::classifyEndDragReject() const {
    return fuse::editor::classifyEndDragReject(preflightEndDrag());
    return fuse::editor::shouldSkipEndDrag(preflightEndDrag());
GizmoBeginDragRejectReason GizmoSystem::classifyBeginDragReject(const GizmoHitTest& hit) const {
    return fuse::editor::classifyBeginDragReject(preflightBeginDrag(hit));
    return fuse::editor::classifyBeginDragReject(preflightBeginDrag(ray, transform));
    return fuse::editor::shouldSkipBeginDrag(preflightBeginDrag(hit));
    return fuse::editor::shouldSkipBeginDrag(preflightBeginDrag(ray, transform));
GizmoPickRejectReason GizmoSystem::classifyPickReject(const GizmoHitTest& hit) const {
    return fuse::editor::classifyPickReject(preflightPick(hit));
GizmoPickRejectReason GizmoSystem::classifyPickReject(const GizmoRay& ray,
    return fuse::editor::classifyPickReject(preflightPick(ray, transform));
    return fuse::editor::shouldSkipPick(preflightPick(hit));
    return fuse::editor::shouldSkipPick(preflightPick(ray, transform));
GizmoSnapRejectReason GizmoSystem::classifySnapReject() const {
    return fuse::editor::classifySnapReject(preflightSnap());
    return fuse::editor::shouldSkipSnap(preflightSnap());
GizmoUpdateDragRejectReason GizmoSystem::classifyUpdateDragReject(const GizmoHitTest& hit) const {
    return fuse::editor::classifyUpdateDragReject(preflightUpdateDrag(hit));
    return fuse::editor::shouldSkipUpdateDrag(preflightUpdateDrag(hit));

// --- deepen additive from gizmo-interaction-reject-reasons-f5f0 ---
const char* gizmoInteractionRejectReasonLabel(GizmoInteractionRejectReason reason) {
    case GizmoInteractionRejectReason::None:
    case GizmoInteractionRejectReason::EmptyRay:
    case GizmoInteractionRejectReason::EmptyHit:
    case GizmoInteractionRejectReason::InvalidPickConfig:
    case GizmoInteractionRejectReason::InvalidDimensions:
    case GizmoInteractionRejectReason::OutOfBounds:
    case GizmoInteractionRejectReason::ScreenMiss:
    case GizmoInteractionRejectReason::PickMiss:
    case GizmoInteractionRejectReason::SnapDisabled:
    case GizmoInteractionRejectReason::InvalidSnapStep:
    case GizmoInteractionRejectReason::NotDragging:
    case GizmoInteractionRejectReason::AlreadyDragging:
    case GizmoInteractionRejectReason::InvalidActiveAxis:
GizmoInteractionRejectReason classifyPickReject(const PickPreflight& preflight) {
GizmoInteractionRejectReason classifySnapReject(const SnapPreflight& preflight) {
GizmoInteractionRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight) {
GizmoInteractionRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight) {
GizmoInteractionRejectReason classifyEndDragReject(const EndDragPreflight& preflight) {
                      GizmoInteractionRejectReason& reason) {
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoInteractionRejectReason& reason) {
                           GizmoInteractionRejectReason& reason, bool alreadyDragging) {
                         const GizmoSnapSettings& settings, GizmoInteractionRejectReason& reason) {
GizmoInteractionRejectReason classifyPickInteractionReject(const PickInteractionPreflight& preflight) {
    return classifyPickReject(preflight.pick);
GizmoInteractionRejectReason classifyBeginDragInteractionReject(
    const BeginDragInteractionPreflight& preflight) {
    const GizmoInteractionRejectReason beginReason = classifyBeginDragReject(preflight.begin);
    if (beginReason != GizmoInteractionRejectReason::None) {
GizmoInteractionRejectReason classifyUpdateDragInteractionReject(
    const UpdateDragInteractionPreflight& preflight) {
    return classifyUpdateDragReject(preflight.drag);
GizmoInteractionRejectReason classifyEndDragInteractionReject(
    const EndDragInteractionPreflight& preflight) {
    return classifyEndDragReject(preflight.end);
GizmoInteractionRejectReason classifyInteractionReject(const InteractionPreflight& preflight) {
    return preflight.primaryRejectReason();
bool shouldSkipPickInteraction(const PickInteractionPreflight& preflight) {
bool shouldSkipBeginDragInteraction(const BeginDragInteractionPreflight& preflight) {
bool shouldSkipUpdateDragInteraction(const UpdateDragInteractionPreflight& preflight) {
bool shouldSkipEndDragInteraction(const EndDragInteractionPreflight& preflight) {
bool shouldSkipInteraction(const InteractionPreflight& preflight) {
GizmoInteractionRejectReason InteractionPreflight::primaryRejectReason() const {
        return classifyBeginDragReject(begin.begin);
        return classifyUpdateDragReject(update.update);
    return fuse::editor::shouldSkipInteraction(preflightInteraction(hit));

// --- deepen additive from deepen-gizmo-preflight-guards-cebc ---
GizmoPreflightRouter preflightGizmoRouter(const GizmoHitTest& hit, bool dragging,
    GizmoPreflightRouter router{};
    router.interaction = preflightInteraction(hit, dragging, activeAxis, mode, settings);
    router.drag = preflightDragInteraction(hit, dragging, activeAxis, mode, settings);
    router.pickInteraction = preflightPickInteraction(hit, mode, settings);
    router.beginDragInteraction = preflightBeginDragInteraction(hit, mode, settings, dragging);
GizmoPreflightRouter preflightGizmoRouter(const GizmoRay& ray, const GizmoTransform& transform,
    router.drag = preflightDragInteraction({}, dragging, activeAxis, mode, settings);
    router.beginDragInteraction = preflightBeginDragInteraction(
    return preflightGizmoRouter(hit, dragging, activeAxis, mode, settings).canRouteAny();
    return preflightGizmoRouter(ray, transform, dragging, activeAxis, mode, space, axisLength,
GizmoPreflightRouter GizmoSystem::preflightRouter(const GizmoHitTest& hit) const {
    return fuse::editor::preflightGizmoRouter(hit, m_dragging, m_activeAxis, m_mode, m_snap);
GizmoPreflightRouter GizmoSystem::preflightRouter(const GizmoRay& ray,
    return fuse::editor::preflightGizmoRouter(ray, transform, m_dragging, m_activeAxis, m_mode,
    return preflightRouter(hit).canRouteAny();
    return preflightRouter(ray, transform).canRouteAny();
    return preflightRouter(hit).canRouteActOrEnd();

// --- deepen additive from deepen-gizmo-preflight-guards-4ea2 ---
SnapPhasePreflight preflightSnapPhase(GizmoMode mode, const GizmoSnapSettings& settings,
    SnapPhasePreflight preflight{};
    return preflightInteraction(hit, dragging, activeAxis, mode, settings).canInteract();
SnapPhasePreflight GizmoSystem::preflightSnapPhase() const {
    return fuse::editor::preflightSnapPhase(m_mode, m_snap,

// --- deepen additive from deepen-gizmo-interaction-preflights-e207 ---
    return preflightInteraction(hit).canInteract();
    return preflightInteraction(ray, transform).canInteract();

// --- deepen additive from deepen-gizmo-preflight-guards-8907 ---
SnapInteractionPreflight preflightSnapInteraction(GizmoInteractionPhase phase, GizmoMode mode,
    SnapInteractionPreflight preflight{};
    return preflightSnapInteraction(phase, mode, settings).canApply();
    return preflightSnapInteraction(phase, mode, settings).isDegraded();
        preflightSnapInteraction(preflight.phase(), mode, settings);
SnapInteractionPreflight GizmoSystem::preflightSnapInteraction() const {
    return fuse::editor::preflightSnapInteraction(interactionPhase(m_dragging), m_mode, m_snap);
    return preflightSnapInteraction().canApply();
    return preflightSnapInteraction().isDegraded();

// --- deepen additive from deepen-b6-gizmo-preflights-66a7 ---
SnapDeltaPreflight preflightSnapDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapDeltaPreflight preflight{};
    return preflightSnapDelta(delta, mode, settings).canApply();
SnapDeltaPreflight GizmoSystem::preflightSnapDelta(f32 delta) const {
    return fuse::editor::preflightSnapDelta(delta, m_mode, m_snap);

// --- deepen additive from deepen-gizmo-interaction-preflights-fe30 ---
RayPreflight preflightRay(const GizmoRay& ray) {
    RayPreflight preflight{};
    return preflightRay(ray).canUse();

// --- deepen additive from deepen-gizmo-preflights-cff3 ---
SnapDragDeltaPreflight preflightSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapDragDeltaPreflight preflight{};
DragSessionPreflight preflightDragSession(const GizmoHitTest& hit, bool dragging,
    DragSessionPreflight preflight{};
DragSessionPreflight preflightDragSession(const GizmoRay& ray, const GizmoTransform& transform,
        preflight.update = preflightUpdateDrag({}, dragging, activeAxis, mode, settings);
    const DragSessionPreflight preflight =
        preflightDragSession(hit, dragging, activeAxis, mode, settings, alreadyDragging);
    const DragSessionPreflight preflight = preflightDragSession(
DragSessionPreflight GizmoSystem::preflightDragSession(const GizmoHitTest& hit) const {
    return fuse::editor::preflightDragSession(hit, m_dragging, m_activeAxis, m_mode, m_snap,
DragSessionPreflight GizmoSystem::preflightDragSession(const GizmoRay& ray,
    return fuse::editor::preflightDragSession(ray, transform, m_dragging, m_activeAxis, m_mode,
    return preflightDragInteraction(hit).canActOnPhase();
SnapDragDeltaPreflight GizmoSystem::preflightSnapDragDelta() const {

// --- deepen additive from deepen-b6-gizmo-preflights-258e ---
HitTestPreflight preflightHitTest(const GizmoHitTest& hit) {
    HitTestPreflight preflight{};
    return preflightHitTest(hit).canUse();
RayPreflight preflightRay(const GizmoRay& ray, f32 axisLength, f32 pickRadius) {
    return preflightRay(ray, axisLength, pickRadius).canUse();
    const RayPreflight rayPreflight = preflightRay(ray, axisLength, pickRadius);
    preflight.emptyRay = rayPreflight.emptyRay;
    preflight.invalidPickConfig = rayPreflight.invalidPickConfig;
    if (!rayPreflight.canUse()) {
    const HitTestPreflight hitPreflight = preflightHitTest(hit);
    preflight.emptyHit = hitPreflight.emptyHit;
    preflight.invalidDimensions = hitPreflight.invalidDimensions;
    preflight.outOfBounds = hitPreflight.outOfBounds;
    if (!hitPreflight.canUse()) {
HitTestPreflight GizmoSystem::preflightHitTest(const GizmoHitTest& hit) const {
    return fuse::editor::preflightHitTest(hit);
RayPreflight GizmoSystem::preflightRay(const GizmoRay& ray) const {
    return fuse::editor::preflightRay(ray, kAxisLength, kPickRadius);

// --- deepen additive from deepen-gizmo-preflight-guards-fbfd ---
ModeChangePreflight preflightModeChange(GizmoMode current, GizmoMode next, bool dragging) {
    ModeChangePreflight preflight{};
ModeChangePreflight preflightCycleMode(GizmoMode current, bool dragging) {
    return preflightModeChange(current, cycleGizmoMode(current), dragging);
    return preflightModeChange(current, next, dragging).canChange();
    return preflightCycleMode(current, dragging).canChange();
ModeChangePreflight GizmoSystem::preflightModeChange(GizmoMode mode) const {
    return fuse::editor::preflightModeChange(m_mode, mode, m_dragging);
ModeChangePreflight GizmoSystem::preflightCycleMode() const {
    return fuse::editor::preflightCycleMode(m_mode, m_dragging);
    return preflightModeChange(mode).canChange();
    return preflightCycleMode().canChange();

// --- deepen additive from deepen-gizmo-preflights-c18e ---
bool canActOnInteraction(const InteractionPreflight& preflight, GizmoInteractionAction action) {
DragLifecycleSnapPreflight preflightDragLifecycleSnap(GizmoMode mode,
    DragLifecycleSnapPreflight preflight{};
DragLifecycleSnapPreflight GizmoSystem::preflightDragLifecycleSnap() const {
    return fuse::editor::preflightDragLifecycleSnap(m_mode, m_snap);

// --- deepen additive from gizmo-interaction-preflights-d149 ---
void applyNoTargetGuard(Handle<Object> target, BeginDragPreflight& preflight) {
void applyNoTargetGuard(Handle<Object> target, UpdateDragPreflight& preflight) {
void applyNoTargetGuard(Handle<Object> target, EndDragPreflight& preflight) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    preflight.begin = preflightBeginDrag(hit, mode, settings, target, alreadyDragging);
    preflight.drag = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings, target);
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings, target);
    preflight.update = preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings,
    preflight.end = preflightEndDragInteraction(dragging, activeAxis, mode, settings, target);
    preflight.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings, target);
    preflight.begin = preflightBeginInteraction(hit, mode, settings, target, dragging);
    preflight.update = preflightUpdateInteraction(hit, dragging, activeAxis, mode, settings, target);
    preflight.end = preflightEndInteraction(dragging, activeAxis, mode, settings, target);
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap, m_target);
    return fuse::editor::preflightEndDragInteraction(m_dragging, m_activeAxis, m_mode, m_snap,
    return fuse::editor::preflightDragInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap,
    return fuse::editor::preflightBeginDrag(hit, m_mode, m_snap, m_target, m_dragging);
    return fuse::editor::preflightBeginDragInteraction(hit, m_mode, m_snap, m_target, m_dragging);
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis, m_mode, m_snap,
    return fuse::editor::preflightBeginInteraction(hit, m_mode, m_snap, m_target, m_dragging);
    return fuse::editor::preflightUpdateInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap,
    return fuse::editor::preflightEndInteraction(m_dragging, m_activeAxis, m_mode, m_snap,
    return fuse::editor::preflightInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap,

// --- deepen additive from deepen-gizmo-b6-preflights-b7f3 ---
                           const GizmoSnapSettings& settings, GizmoBeginDragRejectReason& reason,
                         const GizmoSnapSettings& settings, GizmoEndDragRejectReason& reason) {
    return !preflightSnap(mode, settings).canApply();
    return !preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, settings,
    return !preflightBeginDrag(hit, mode, settings, alreadyDragging).canBegin;
    return !preflightUpdateDrag(hit, dragging, activeAxis).canUpdate();
    return !preflightUpdateDrag(hit, dragging, activeAxis, mode, settings).canUpdate();
    return !preflightEndDrag(dragging, activeAxis, GizmoMode::Translate, {}).canEnd();
    return !preflightEndDrag(dragging, activeAxis, mode, settings).canEnd();

// --- deepen additive from deepen-gizmo-preflight-guards-0cc5 ---
    preflight.snapDrag = preflightSnapDrag(0.f, mode, settings);
GizmoSnapDragRejectReason classifySnapDragReject(const SnapDragPreflight& preflight) {
        return GizmoSnapDragRejectReason::DeltaNonFinite;
        return GizmoSnapDragRejectReason::SnapDisabled;
        return GizmoSnapDragRejectReason::InvalidStep;
    return GizmoSnapDragRejectReason::None;
