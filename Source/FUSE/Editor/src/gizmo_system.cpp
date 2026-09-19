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

bool isSnapGuardValid(GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings)) {
        return true;
    }

    switch (mode) {
    case GizmoMode::Translate:
        return settings.gridSize > kEpsilon;
    case GizmoMode::Rotate:
        return settings.angleStepDegrees > kEpsilon;
    case GizmoMode::Scale:
        return settings.scaleGridStep > kEpsilon;

f32 snapValue(f32 value, GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings) || !isSnapGuardValid(mode, settings)) {
f32 snapStepForMode(GizmoMode mode, const GizmoSnapSettings& settings) {
        return settings.gridSize;
        return settings.angleStepDegrees;
        return settings.scaleGridStep;
    return 0.f;

bool isSnapStepValid(GizmoMode mode, const GizmoSnapSettings& settings) {
    return snapStepForMode(mode, settings) > kEpsilon;

bool canApplySnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    return isSnapEnabled(mode, settings) && isSnapStepValid(mode, settings);

    if (!canApplySnap(mode, settings)) {
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

f32 snapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings) || !isSnapGuardValid(mode, settings)) {
    if (!canApplySnap(mode, settings)) {
        return delta;
    }
    return snapValue(delta, mode, settings);
}

f32 snapStepForMode(GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings)) {
        return 0.f;
    }

    switch (mode) {
    case GizmoMode::Translate:
        return settings.gridSize;
    case GizmoMode::Rotate:
        return settings.angleStepDegrees * 3.14159265f / 180.f;
    case GizmoMode::Scale:
        return settings.scaleGridStep;
    }
    return 0.f;
}

void clampSnapSettings(GizmoSnapSettings& settings) {
    if (settings.gridSize <= kEpsilon) {
        settings.gridSize = 1.f;
    }
    if (settings.angleStepDegrees <= kEpsilon) {
        settings.angleStepDegrees = 15.f;
    }
    if (settings.scaleGridStep <= kEpsilon) {
        settings.scaleGridStep = 0.1f;
    }
}

bool isSnapSettingsValid(const GizmoSnapSettings& settings) {
    return settings.gridSize > kEpsilon && settings.angleStepDegrees > kEpsilon &&
           settings.scaleGridStep > kEpsilon;
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

bool isHitTestDimensionsInvalid(const GizmoHitTest& hit) {
    return hit.viewportWidth < 0.f || hit.viewportHeight < 0.f;

bool isFiniteGizmoScalar(f32 value) {
    return std::isfinite(value);

bool isRayFinite(const GizmoRay& ray) {
    return isFiniteGizmoScalar(ray.origin.x) && isFiniteGizmoScalar(ray.origin.y) &&
           isFiniteGizmoScalar(ray.origin.z) && isFiniteGizmoScalar(ray.direction.x) &&
           isFiniteGizmoScalar(ray.direction.y) && isFiniteGizmoScalar(ray.direction.z);

bool isHitTestFinite(const GizmoHitTest& hit) {
    return isFiniteGizmoScalar(hit.screenX) && isFiniteGizmoScalar(hit.screenY) &&
           isFiniteGizmoScalar(hit.viewportWidth) && isFiniteGizmoScalar(hit.viewportHeight);

GizmoInteractionPhase interactionPhase(bool dragging) {
    return dragging ? GizmoInteractionPhase::Dragging : GizmoInteractionPhase::Idle;

bool isSnapDegraded(GizmoMode mode, const GizmoSnapSettings& settings) {
    return isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings);

    return hit.screenX < 0.f || hit.screenX > hit.viewportWidth || hit.screenY < 0.f ||
}

bool isRayValid(const GizmoRay& ray) {
    return !isRayEmpty(ray);

bool isHitTestValid(const GizmoHitTest& hit) {
    return !isHitTestEmpty(hit);




bool isScreenCoordInViewport(const GizmoHitTest& hit) {
    if (isHitTestEmpty(hit)) {
        return false;
    }
    return hit.screenX >= 0.f && hit.screenX <= hit.viewportWidth && hit.screenY >= 0.f &&
           hit.screenY <= hit.viewportHeight;
}

bool isHitTestInBounds(const GizmoHitTest& hit) {
    return isHitTestValid(hit) && !isHitTestOutOfBounds(hit);
}

bool normalizeRay(GizmoRay& ray) {
    const f32 len = ray.direction.length();
    if (len < kEpsilon) {
    ray.direction = ray.direction * (1.f / len);
    return true;
        return false;
    }

math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings) {
    if (!canApplySnap(GizmoMode::Translate, settings)) {
        return position;
    return {snapToGrid(position.x, settings.gridSize), snapToGrid(position.y, settings.gridSize),
            snapToGrid(position.z, settings.gridSize)};

math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings) {
    if (!canApplySnap(GizmoMode::Rotate, settings)) {
        return eulerRadians;
    return {snapAngleRadians(eulerRadians.x, settings.angleStepDegrees),
            snapAngleRadians(eulerRadians.y, settings.angleStepDegrees),
            snapAngleRadians(eulerRadians.z, settings.angleStepDegrees)};

math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings) {
    if (!canApplySnap(GizmoMode::Scale, settings)) {
        return scale;
    return {snapScale(scale.x, settings.scaleGridStep), snapScale(scale.y, settings.scaleGridStep),
            snapScale(scale.z, settings.scaleGridStep)};

bool isPickConfigValid(f32 axisLength, f32 pickRadius) {
    return axisLength > kEpsilon && pickRadius > kEpsilon;

bool isSnapStepValid(GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!isSnapEnabled(mode, settings)) {
    return snapStepForMode(mode, settings) > kEpsilon;

bool canApplySnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    return isSnapEnabled(mode, settings) && isSnapStepValid(mode, settings);
    return preflightSnap(mode, settings).canApply;
}

PickPreflight preflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                            GizmoSpace space, f32 axisLength, f32 pickRadius) {
    PickPreflight preflight{};
    if (isRayEmpty(ray)) {
        preflight.emptyRay = true;
        return preflight;

    if (!isPickConfigValid(axisLength, pickRadius)) {
        preflight.invalidPickConfig = true;

    const GizmoAxis axis =
        pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius);
    if (axis == GizmoAxis::None) {
        preflight.pickMiss = true;
    } else {
        preflight.axis = axis;


PickPreflight preflightPick(const GizmoHitTest& hit, GizmoMode mode) {
    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;

    if (!isScreenCoordInViewport(hit)) {
        preflight.outOfBounds = true;
        return preflight;
    }

    if (isHitTestOutOfBounds(hit)) {
        preflight.outOfBounds = true;
        return preflight;
    }

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;

    preflight.axis = resolveScreenAxis(hit, mode);
}

SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapPreflight preflight{};
    preflight.step = snapStepForMode(mode, settings);
    if (!isSnapEnabled(mode, settings)) {
        preflight.snapDisabled = true;
        return preflight;
    if (!isSnapStepValid(mode, settings)) {
        preflight.invalidStep = true;
    preflight.canApply = true;

PickPreflight preflightPickAxis(const GizmoRay& ray, const GizmoTransform& transform,
                                GizmoMode mode, GizmoSpace space, f32 axisLength,
                                f32 pickRadius) {
    PickPreflight preflight{};
    if (isRayEmpty(ray)) {
        preflight.emptyRay = true;

    if (!isPickConfigValid(axisLength, pickRadius)) {
        preflight.invalidPickConfig = true;

    preflight.axis =
        pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius);
    if (preflight.axis == GizmoAxis::None) {

    preflight.canPick = true;

PickPreflight preflightPickAxis(const GizmoHitTest& hit, GizmoMode mode) {
    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;

    const f32 x = normalizedX(hit);
    const f32 y = normalizedY(hit);

    if (mode == GizmoMode::Scale && x > 0.4f && x < 0.6f && y > 0.4f && y < 0.6f) {
        preflight.axis = GizmoAxis::Uniform;
    } else if (x < 0.33f) {
        preflight.axis = GizmoAxis::X;
    } else if (y < 0.33f) {
        preflight.axis = GizmoAxis::Y;
    } else {
        preflight.axis = GizmoAxis::Z;


UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, GizmoMode mode, bool dragging,
                                        GizmoAxis activeAxis, const GizmoSnapSettings& snap) {
    UpdateDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;


    if (activeAxis == GizmoAxis::None) {
        preflight.noActiveAxis = true;

    const SnapPreflight snapPreflight = preflightSnap(mode, snap);
    if (isSnapEnabled(mode, snap) && !snapPreflight.canApply) {
        preflight.snapSkipped = true;

    preflight.canUpdate = true;

bool canUpdateDrag(const GizmoHitTest& hit, GizmoMode mode, bool dragging, GizmoAxis activeAxis) {
    (void)hit;
    (void)mode;
        return false;
    return activeAxis != GizmoAxis::None;

PickPreflight preflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                            GizmoSpace space, f32 axisLength, f32 pickRadius) {
    if (!isRayFinite(ray)) {
        preflight.nonFiniteRay = true;
    }


    if (pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius) == GizmoAxis::None) {
        preflight.pickMiss = true;


PickPreflight preflightPick(const GizmoHitTest& hit, GizmoMode mode) {






UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging) {



BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, bool alreadyDragging) {
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;
        return preflight;

    if (isRayEmpty(ray)) {
        preflight.emptyRay = true;

    if (!isPickConfigValid(axisLength, pickRadius)) {
        preflight.invalidPickConfig = true;

    const GizmoAxis axis =
        pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius);
    if (axis == GizmoAxis::None) {
    } else {
        preflight.axis = axis;

    preflight.axis = pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius);
    preflight.canBegin = true;
}

    if (!isHitTestFinite(hit)) {
        preflight.nonFiniteHit = true;

    if (isHitTestDimensionsInvalid(hit)) {
        preflight.invalidDimensions = true;

        preflight.emptyHit = true;

    if (isHitTestOutOfBounds(hit)) {
        preflight.outOfBounds = true;

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;

    preflight.axis = resolveScreenAxis(hit, mode);

SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapPreflight preflight{};
        preflight.snapDisabled = true;

    preflight.step = snapStepForMode(mode, settings);
    if (!isSnapStepValid(mode, settings)) {
        preflight.invalidStep = true;


SnapDragPreflight preflightSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapDragPreflight preflight{};
    if (!isFiniteGizmoScalar(delta)) {
        preflight.deltaNonFinite = true;

    const SnapPreflight snap = preflightSnap(mode, settings);
    preflight.snapDisabled = snap.snapDisabled;
    preflight.invalidStep = snap.invalidStep;

bool trySnapTransform(const GizmoTransform& transform, GizmoMode mode,
                      const GizmoSnapSettings& settings, GizmoTransform& out) {
    out = transform;
    if (!canApplySnap(mode, settings)) {

    out = snapTransform(transform, mode, settings);

bool trySnapValue(f32 value, GizmoMode mode, const GizmoSnapSettings& settings, f32& out) {
    out = value;
    if (!canApplySnap(mode, settings)) {
        return false;
    }

    out = snapValue(value, mode, settings);
    return true;
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

f32 trySnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    if (!preflightSnapDrag(delta, mode, settings).canApply()) {
        return delta;
    return snapValue(delta, mode, settings);

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, const GizmoTransform& transform,
                                      GizmoMode mode) {
    (void)transform;
    BeginDragPreflight out{};
        out.emptyInput = true;
        return out;

    GizmoAxis axis = GizmoAxis::None;
    if (!tryPickAxis(hit, mode, axis)) {
        out.pickMiss = true;

    out.canBegin = true;
    out.axis = axis;

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius) {

    if (!tryPickAxis(ray, transform, mode, space, axisLength, pickRadius, axis)) {


math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings) {
    if (!settings.translateSnap) {
        return position;
    return {snapToGrid(position.x, settings.gridSize), snapToGrid(position.y, settings.gridSize),
            snapToGrid(position.z, settings.gridSize)};

math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings) {
    if (!settings.rotateSnap) {
        return eulerRadians;
    return {snapAngleRadians(eulerRadians.x, settings.angleStepDegrees),
            snapAngleRadians(eulerRadians.y, settings.angleStepDegrees),
            snapAngleRadians(eulerRadians.z, settings.angleStepDegrees)};

math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings) {
    if (!settings.scaleSnap) {
        return scale;
    return {snapScale(scale.x, settings.scaleGridStep), snapScale(scale.y, settings.scaleGridStep),
            snapScale(scale.z, settings.scaleGridStep)};

bool canSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings) {
    return canApplySnap(mode, settings);

bool canSnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightSnapDrag(delta, mode, settings).canApply();

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging,
                                        GizmoAxis activeAxis) {
    UpdateDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;

    if (activeAxis == GizmoAxis::None) {
        preflight.invalidActiveAxis = true;

    } else if (isHitTestDimensionsInvalid(hit)) {
    } else if (isHitTestEmpty(hit)) {
    } else if (isHitTestOutOfBounds(hit)) {


                                        GizmoAxis activeAxis, GizmoMode mode,
                                        const GizmoSnapSettings& settings) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis);
    if (!preflight.canUpdate()) {

    if (isSnapDegraded(mode, settings)) {
        preflight.snapDegraded = true;


bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    return preflightUpdateDrag(hit, dragging, activeAxis).canUpdate();

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    return preflightUpdateDrag(hit, dragging, activeAxis, mode, settings).canUpdate();

EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    EndDragPreflight preflight{};




PickInteractionPreflight preflightPickInteraction(const GizmoRay& ray,
                                                  const GizmoTransform& transform, GizmoMode mode,
                                                  GizmoSpace space, f32 axisLength,
                                                  f32 pickRadius,
    PickInteractionPreflight preflight{};
    preflight.pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.snap = preflightSnap(mode, settings);

PickInteractionPreflight preflightPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
    preflight.pick = preflightPick(hit, mode);

BeginDragInteractionPreflight preflightBeginDragInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginDragInteractionPreflight preflight{};
    preflight.begin =
        preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
    preflight.snapDegraded = isSnapDegraded(mode, settings);

BeginDragInteractionPreflight preflightBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                            const GizmoSnapSettings& settings,
                                                            bool alreadyDragging) {
    preflight.begin = preflightBeginDrag(hit, mode, alreadyDragging);

UpdateDragInteractionPreflight preflightUpdateDragInteraction(const GizmoHitTest& hit,
                                                              bool dragging, GizmoAxis activeAxis,
                                                              GizmoMode mode,
    return preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings, 0.f);

                                                              f32 delta) {
    UpdateDragInteractionPreflight preflight{};
    preflight.drag = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    preflight.snapDrag = preflightSnapDrag(delta, mode, settings);

EndDragInteractionPreflight preflightEndDragInteraction(bool dragging, GizmoAxis activeAxis,
    EndDragInteractionPreflight preflight{};
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings);

DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit, bool dragging,
    DragInteractionPreflight preflight{};

    preflight.update = preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings);
    preflight.end = preflightEndDragInteraction(dragging, activeAxis, mode, settings);

bool canPickInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                        GizmoSpace space, f32 axisLength, f32 pickRadius,
    return preflightPickInteraction(ray, transform, mode, space, axisLength, pickRadius, settings)
        .canPick();

bool canPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
    return preflightPickInteraction(hit, mode, settings).canPick();

bool canBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
    return preflightBeginDragInteraction(ray, transform, mode, space, axisLength, pickRadius,
                                         settings)
        .canBegin();

bool canBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
    return preflightBeginDragInteraction(hit, mode, settings).canBegin();

bool canUpdateDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                              GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings).canUpdate();

bool canEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    return preflightEndDragInteraction(dragging, activeAxis, mode, settings).canEnd();

bool canDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
    return preflightDragInteraction(hit, dragging, activeAxis, mode, settings).canInteract();

bool canEndDrag(bool dragging, GizmoAxis activeAxis) {
    return preflightEndDrag(dragging, activeAxis, GizmoMode::Translate, {}).canEnd();

bool canEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    return preflightEndDrag(dragging, activeAxis, mode, settings).canEnd();

PickSnapPreflight preflightPickSnap(const GizmoRay& ray, const GizmoTransform& transform,
                                    f32 pickRadius, const GizmoSnapSettings& settings) {
    PickSnapPreflight preflight{};

PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit, GizmoMode mode,

BeginInteractionPreflight preflightBeginInteraction(
    BeginInteractionPreflight preflight{};
    preflight.begin = preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius,
                                         settings, alreadyDragging);

BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
    preflight.begin = preflightBeginDrag(hit, mode, settings, alreadyDragging);

UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
    UpdateInteractionPreflight preflight{};
    preflight.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis,
    EndInteractionPreflight preflight{};

InteractionPreflight preflightInteraction(const GizmoHitTest& hit, bool dragging,
    InteractionPreflight preflight{};
    preflight.dragging = dragging;
    preflight.pickSnap = preflightPickSnap(hit, mode, settings);
    preflight.begin = preflightBeginInteraction(hit, mode, settings, dragging);
    preflight.update = preflightUpdateInteraction(hit, dragging, activeAxis, mode, settings);
    preflight.end = preflightEndInteraction(dragging, activeAxis, mode, settings);

InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    preflight.pickSnap =
        preflightPickSnap(ray, transform, mode, space, axisLength, pickRadius, settings);
    preflight.begin = preflightBeginInteraction(ray, transform, mode, space, axisLength,
                                                pickRadius, settings, dragging);
    preflight.update = preflightUpdateInteraction({}, dragging, activeAxis, mode, settings);

bool canPickSnap(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
    return preflightPickSnap(ray, transform, mode, space, axisLength, pickRadius, settings)

bool canPickSnap(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightPickSnap(hit, mode, settings).canPick();

bool canBeginInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging) {
    return preflightBeginInteraction(ray, transform, mode, space, axisLength, pickRadius, settings,
                                     alreadyDragging)

bool canBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
    return preflightBeginInteraction(hit, mode, settings, alreadyDragging).canBegin();

bool canUpdateInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
    return preflightUpdateInteraction(hit, dragging, activeAxis, mode, settings).canUpdate();

bool canEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    return preflightEndInteraction(dragging, activeAxis, mode, settings).canEnd();

                                      f32 pickRadius, const GizmoSnapSettings& settings,
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;

    const PickPreflight pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.emptyRay = pick.emptyRay;
    preflight.nonFiniteRay = pick.nonFiniteRay;
    preflight.invalidPickConfig = pick.invalidPickConfig;
    preflight.pickMiss = pick.pickMiss;
    if (!pick.canPick()) {

    preflight.axis = pick.axis;
    if (tryPickAxis(hit, mode, axis)) {
        preflight.axis = axis;
    }
    preflight.canBegin = true;

                                      f32 pickRadius, bool alreadyDragging) {
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, {},
                              alreadyDragging);

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,

    const PickPreflight pick = preflightPick(hit, mode);
    preflight.emptyHit = pick.emptyHit;
    preflight.nonFiniteHit = pick.nonFiniteHit;
    preflight.invalidDimensions = pick.invalidDimensions;
    preflight.outOfBounds = pick.outOfBounds;
    preflight.screenMiss = pick.screenMiss;


    return preflightBeginDrag(hit, mode, {}, alreadyDragging);
bool canPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
    return tryPickAxis(ray, transform, mode, space, axisLength, pickRadius, axis);

bool canPickAxis(const GizmoHitTest& hit, GizmoMode mode) {
    return tryPickAxis(hit, mode, axis);

bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode) {
    return !isHitTestEmpty(hit) && canPickAxis(hit, mode);

bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
    return canPickAxis(ray, transform, mode, space, axisLength, pickRadius);
GizmoBeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                           f32 pickRadius, bool alreadyDragging,
                                           const GizmoSnapSettings& snap) {
    GizmoBeginDragPreflight preflight{};
    preflight.alreadyDragging = alreadyDragging;
    preflight.snapInvalid = !isSnapGuardValid(mode, snap);



        preflight.axisMiss = true;


GizmoBeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                           bool alreadyDragging, const GizmoSnapSettings& snap) {


        preflight.emptyViewport = true;


}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,
                                        GizmoAxis activeAxis) {
    UpdateDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
        return preflight;
    }

    if (activeAxis == GizmoAxis::None) {
        preflight.noActiveAxis = true;
        return preflight;
    }

    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;
        return preflight;
    }

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;
    }

    if (!isScreenCoordInViewport(hit)) {
    if (isHitTestOutOfBounds(hit)) {
        preflight.outOfBounds = true;
    }

    return preflight;
}

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode, GizmoAxis activeAxis) {
    return preflightUpdateDrag(hit, dragging, mode, activeAxis).canUpdate();
}

EndDragPreflight preflightEndDrag(bool dragging) {
    EndDragPreflight preflight{};
namespace {

bool gizmoTransformNear(const GizmoTransform& a, const GizmoTransform& b, f32 tolerance) {
    return std::fabs(a.posX - b.posX) <= tolerance && std::fabs(a.posY - b.posY) <= tolerance &&
           std::fabs(a.posZ - b.posZ) <= tolerance && std::fabs(a.rotX - b.rotX) <= tolerance &&
           std::fabs(a.rotY - b.rotY) <= tolerance && std::fabs(a.rotZ - b.rotZ) <= tolerance &&
           std::fabs(a.rotW - b.rotW) <= tolerance && std::fabs(a.scaleX - b.scaleX) <= tolerance &&
           std::fabs(a.scaleY - b.scaleY) <= tolerance && std::fabs(a.scaleZ - b.scaleZ) <= tolerance;

} // namespace

SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    SnapPreflight preflight{};
    if (!isSnapEnabled(mode, settings)) {
        preflight.snapDisabled = true;
        return preflight;

    if (!isSnapStepValid(mode, settings)) {
        preflight.invalidStep = true;


SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
                            const GizmoTransform& transform) {
    SnapPreflight preflight = preflightSnap(mode, settings);
    if (!preflight.canApply()) {

    GizmoTransform snapped{};
    if (trySnapTransform(transform, mode, settings, snapped) &&
        gizmoTransformNear(transform, snapped, kEpsilon)) {
        preflight.noChange = true;





UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging,
                                        GizmoAxis activeAxis, GizmoMode mode) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis);
    if (!preflight.canUpdate()) {

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;


                                        GizmoAxis activeAxis, GizmoMode mode,
                                        const GizmoSnapSettings& settings) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis, mode);
        return preflight;
    }

    GizmoTransform snapped{};
    if (trySnapTransform(transform, mode, settings, snapped) &&
        gizmoTransformNear(transform, snapped, kEpsilon)) {
        preflight.noChange = true;
    }

    return preflight;
}

bool trySnapTransform(const GizmoTransform& transform, GizmoMode mode,
                      const GizmoSnapSettings& settings, GizmoTransform& out) {
    out = transform;
    if (!canApplySnap(mode, settings)) {
        return false;

    out = snapTransform(transform, mode, settings);
    return true;

f32 trySnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
        return delta;
    return snapValue(delta, mode, settings);

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return preflightUpdateDrag(hit, dragging, GizmoMode::Translate);
}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode) {
    UpdateDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
    }

    if (isHitTestEmpty(hit)) {
        preflight.emptyHit = true;
        return preflight;
    }

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;
    }

    if (isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;

    return preflight;

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,
                                        const GizmoSnapSettings& settings) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging);
    if (!preflight.canUpdate()) {

    if (isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings)) {
        preflight.snapDegraded = true;


bool canUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return preflightUpdateDrag(hit, dragging).canUpdate();

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,
    return preflightUpdateDrag(hit, dragging, mode, settings).canUpdate();

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

EndDragPreflight preflightEndDrag(bool dragging) {
    EndDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;
    }
    return preflight;

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode) {
    return preflightUpdateDrag(hit, dragging, mode).canUpdate();

bool canEndDrag(bool dragging) {
    return preflightEndDrag(dragging).canEnd();

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return preflightUpdateDrag(hit, dragging).canUpdate();







EndDragPreflight preflightEndDrag(bool dragging, GizmoMode mode, const GizmoSnapSettings& settings) {
EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis) {
bool gizmoTransformEquals(const GizmoTransform& a, const GizmoTransform& b) {
    return a.posX == b.posX && a.posY == b.posY && a.posZ == b.posZ && a.rotX == b.rotX &&
           a.rotY == b.rotY && a.rotZ == b.rotZ && a.rotW == b.rotW && a.scaleX == b.scaleX &&
           a.scaleY == b.scaleY && a.scaleZ == b.scaleZ;
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode) {
    return preflightUpdateDrag(hit, dragging, activeAxis, mode).canUpdate();

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

    if (isSnapEnabled(mode, settings) && !canApplySnap(mode, settings)) {
        preflight.snapSkipped = true;
    if (activeAxis == GizmoAxis::None) {
        preflight.invalidActiveAxis = true;
        return preflight;
    }

    if (canApplySnap(mode, settings)) {
        preflight.snapWillApply = true;
    }

    return preflight;
}

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode) {
    UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging);
    if (preflight.canUpdate() && isScreenHitMiss(hit, mode)) {
        preflight.screenMiss = true;
    }
    return preflight;
EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                  const GizmoSnapSettings& settings,
                                  const GizmoTransform& startTransform,
                                  const GizmoTransform& currentTransform) {
    EndDragPreflight preflight = preflightEndDrag(dragging, activeAxis, mode, settings);
    if (!preflight.canEnd()) {

    if (gizmoTransformEquals(startTransform, currentTransform)) {
        preflight.unchangedTransform = true;


bool canEndDrag(bool dragging, GizmoAxis activeAxis) {
    return preflightEndDrag(dragging, activeAxis, GizmoMode::Translate, {}).canEnd();

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return preflightUpdateDrag(hit, dragging).canUpdate();

bool canEndDrag(bool dragging, GizmoMode mode, const GizmoSnapSettings& settings) {
    return preflightEndDrag(dragging, mode, settings).canEnd();
EndDragPreflight preflightEndDrag(bool dragging, GizmoMode mode, const GizmoSnapSettings& settings) {
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode) {
    return preflightUpdateDrag(hit, dragging, mode).canUpdate();

EndDragPreflight preflightEndDrag(bool dragging) {
    EndDragPreflight preflight{};
    if (!dragging) {
        preflight.notDragging = true;

EndDragPreflight preflightEndDrag(bool dragging, GizmoMode mode,
                                  const GizmoSnapSettings& settings) {
    EndDragPreflight preflight = preflightEndDrag(dragging);
    if (!preflight.canEnd()) {

    const SnapPreflight snap = preflightSnap(mode, settings);
    preflight.snapDisabled = snap.snapDisabled;
    preflight.invalidSnapStep = snap.invalidStep;

bool canEndDrag(bool dragging) {
    return preflightEndDrag(dragging, GizmoMode::Translate, GizmoSnapSettings{}).canEnd();
    preflight.canEnd = true;

    return preflightEndDrag(dragging).canEnd;

    return preflightEndDrag(dragging).canEnd();

bool canEndDrag(bool dragging, GizmoAxis activeAxis) {
    return preflightEndDrag(dragging, activeAxis).canEnd();
    return preflightEndDrag(dragging, activeAxis, GizmoMode::Translate, GizmoSnapSettings{}).canEnd();
}

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, bool alreadyDragging) {
    return preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, {},
                              alreadyDragging);
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

    preflight.canUpdate = true;
    const PickPreflight pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.emptyRay = pick.emptyRay;
    preflight.invalidPickConfig = pick.invalidPickConfig;
    preflight.pickMiss = pick.pickMiss;
    if (!pick.canPick()) {
        return preflight;
    }

    if (isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings)) {
        preflight.snapDegraded = true;

    preflight.canBegin = true;

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, const GizmoSnapSettings& settings,
                                      bool alreadyDragging) {
    BeginDragPreflight preflight =
        preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
    if (!preflight.canBegin) {
        return preflight;
    }

    if (isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings)) {
        preflight.snapDegraded = true;
    }

    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      bool alreadyDragging) {
    return preflightBeginDrag(hit, mode, {}, alreadyDragging);

                                      const GizmoSnapSettings& settings, bool alreadyDragging) {
bool canEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                const GizmoSnapSettings& settings) {
    return preflightEndDrag(dragging, activeAxis, mode, settings).canEnd();
}

PickSnapPreflight preflightPickSnap(const GizmoRay& ray, const GizmoTransform& transform,
                                    GizmoMode mode, GizmoSpace space, f32 axisLength,
                                    f32 pickRadius, const GizmoSnapSettings& settings) {
    PickSnapPreflight preflight{};
    preflight.pick =
        preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;

PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit, GizmoMode mode,
    preflight.pick = preflightPick(hit, mode);
}

                                    const GizmoSnapSettings& settings) {
    PickSnapPreflight preflight{};
    preflight.snap = preflightSnap(mode, settings);
    return preflight;


BeginInteractionPreflight preflightBeginInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginInteractionPreflight preflight{};
    preflight.begin = preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius,
                                         alreadyDragging);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                    const GizmoSnapSettings& settings,
                                                    bool alreadyDragging) {
    preflight.begin = preflightBeginDrag(hit, mode, alreadyDragging);

UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
                                                      GizmoAxis activeAxis, GizmoMode mode,
    UpdateInteractionPreflight preflight{};
    preflight.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    BeginInteractionPreflight preflight{};
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

                                                      const GizmoSnapSettings& settings) {
UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
                                                      GizmoAxis activeAxis, GizmoMode mode,
    UpdateInteractionPreflight preflight{};
    preflight.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    preflight.snap = preflightSnap(mode, settings);
    return preflight;
}

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis,
                                                GizmoMode mode, const GizmoSnapSettings& settings) {
    EndInteractionPreflight preflight{};
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings);

InteractionPreflight preflightInteraction(const GizmoHitTest& hit, bool dragging,
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

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      f32 pickRadius, bool alreadyDragging) {
    BeginDragPreflight preflight{};
    if (alreadyDragging) {
        preflight.alreadyDragging = true;

    const PickPreflight pick = preflightPick(hit, mode);
    preflight.emptyHit = pick.emptyHit;
    preflight.outOfBounds = pick.outOfBounds;
    preflight.screenMiss = pick.screenMiss;


    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginDragPreflight preflight = preflightBeginDrag(hit, mode, alreadyDragging);
    if (!preflight.canBegin) {
        return preflight;
    }

    if (isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings)) {
        preflight.snapDegraded = true;
    }

    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, const GizmoSnapSettings& settings,
                                      bool alreadyDragging) {
    BeginDragPreflight preflight =
        preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
    if (preflight.canBegin && isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings)) {
        preflight.snapDegraded = true;
    }
    return preflight;
}

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      const GizmoSnapSettings& settings, bool alreadyDragging) {
    BeginDragPreflight preflight = preflightBeginDrag(hit, mode, alreadyDragging);
    if (preflight.canBegin && isSnapEnabled(mode, settings) && !isSnapStepValid(mode, settings)) {
        preflight.snapDegraded = true;
    }
    return preflight;
}

GizmoInteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                               GizmoMode mode, GizmoSpace space, f32 axisLength,
                                               f32 pickRadius, bool dragging, GizmoAxis activeAxis,
                                               const GizmoSnapSettings& settings) {
    GizmoInteractionPreflight preflight{};
    preflight.pick = preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    preflight.snap = preflightSnap(mode, settings);
    preflight.begin = preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius,
                                         settings, dragging);
    preflight.update =
        preflightUpdateDrag({}, dragging, activeAxis, mode, settings);
    if (dragging) {
        preflight.update.emptyHit = false;
    }
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings);
    return preflight;
}

GizmoInteractionPreflight preflightInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                               bool dragging, GizmoAxis activeAxis,
                                               const GizmoSnapSettings& settings) {
    GizmoInteractionPreflight preflight{};
    preflight.pick = preflightPick(hit, mode);
    preflight.snap = preflightSnap(mode, settings);
    preflight.begin = preflightBeginDrag(hit, mode, settings, dragging);
    preflight.update = preflightUpdateDrag(hit, dragging, activeAxis, mode, settings);
    preflight.end = preflightEndDrag(dragging, activeAxis, mode, settings);
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
    if (x < 0.33f) {
        outAxis = GizmoAxis::X;
    if (y < 0.33f) {
        outAxis = GizmoAxis::Y;
    outAxis = GizmoAxis::Z;
    const PickPreflight preflight = preflightPickAxis(hit, mode);
    outAxis = preflight.axis;
    return preflight.canPick;
    outAxis = pick.axis;
    return outAxis != GizmoAxis::None;
}

bool canPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius) {
    return preflightPick(ray, transform, mode, space, axisLength, pickRadius).canPick() &&
           pickAxisFromRay(ray, transform, mode, space, axisLength, pickRadius) != GizmoAxis::None;
}

bool canPickAxis(const GizmoHitTest& hit, GizmoMode mode) {
    return preflightPick(hit, mode).canPick();
    return preflightPickAxis(ray, transform, mode, space, axisLength, pickRadius).canPick;

    return preflightPickAxis(hit, mode).canPick;
    return preflightPick(ray, transform, mode, space, axisLength, pickRadius).canPick;

    return preflightPick(hit, mode).canPick;
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
    return "Unknown";

const char* gizmoSnapRejectReasonLabel(GizmoSnapRejectReason reason) {
    case GizmoSnapRejectReason::None:
    case GizmoSnapRejectReason::SnapDisabled:
        return "SnapDisabled";
    case GizmoSnapRejectReason::InvalidStep:
        return "InvalidStep";

const char* gizmoSnapDragRejectReasonLabel(GizmoSnapDragRejectReason reason) {
    case GizmoSnapDragRejectReason::None:
    case GizmoSnapDragRejectReason::DeltaNonFinite:
        return "DeltaNonFinite";
    case GizmoSnapDragRejectReason::SnapDisabled:
    case GizmoSnapDragRejectReason::InvalidStep:

const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason) {
    case GizmoBeginDragRejectReason::None:
    case GizmoBeginDragRejectReason::NonFiniteRay:
    case GizmoBeginDragRejectReason::NonFiniteHit:
    case GizmoBeginDragRejectReason::EmptyRay:
    case GizmoBeginDragRejectReason::EmptyHit:
    case GizmoBeginDragRejectReason::InvalidPickConfig:
    case GizmoBeginDragRejectReason::InvalidDimensions:
    case GizmoBeginDragRejectReason::OutOfBounds:
    case GizmoBeginDragRejectReason::ScreenMiss:
    case GizmoBeginDragRejectReason::PickMiss:
    case GizmoBeginDragRejectReason::AlreadyDragging:
        return "AlreadyDragging";

const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason) {
    case GizmoUpdateDragRejectReason::None:
    case GizmoUpdateDragRejectReason::NotDragging:
        return "NotDragging";
    case GizmoUpdateDragRejectReason::NonFiniteHit:
    case GizmoUpdateDragRejectReason::EmptyHit:
    case GizmoUpdateDragRejectReason::InvalidDimensions:
    case GizmoUpdateDragRejectReason::OutOfBounds:
    case GizmoUpdateDragRejectReason::InvalidActiveAxis:
        return "InvalidActiveAxis";

const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason) {
    case GizmoEndDragRejectReason::None:
    case GizmoEndDragRejectReason::NotDragging:

GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight) {
    if (preflight.nonFiniteRay) {
        return GizmoPickRejectReason::NonFiniteRay;
    if (preflight.nonFiniteHit) {
        return GizmoPickRejectReason::NonFiniteHit;
    if (preflight.emptyRay) {
        return GizmoPickRejectReason::EmptyRay;
    if (preflight.emptyHit) {
        return GizmoPickRejectReason::EmptyHit;
    if (preflight.invalidPickConfig) {
        return GizmoPickRejectReason::InvalidPickConfig;
    if (preflight.invalidDimensions) {
        return GizmoPickRejectReason::InvalidDimensions;
    if (preflight.outOfBounds) {
        return GizmoPickRejectReason::OutOfBounds;
    if (preflight.screenMiss) {
        return GizmoPickRejectReason::ScreenMiss;
    if (preflight.pickMiss) {
        return GizmoPickRejectReason::PickMiss;
    return GizmoPickRejectReason::None;

GizmoSnapRejectReason classifySnapReject(const SnapPreflight& preflight) {
    if (preflight.snapDisabled) {
        return GizmoSnapRejectReason::SnapDisabled;
    if (preflight.invalidStep) {
        return GizmoSnapRejectReason::InvalidStep;
    return GizmoSnapRejectReason::None;

GizmoBeginDragRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight) {
    if (preflight.alreadyDragging) {
        return GizmoBeginDragRejectReason::AlreadyDragging;
        return GizmoBeginDragRejectReason::NonFiniteRay;
        return GizmoBeginDragRejectReason::NonFiniteHit;
        return GizmoBeginDragRejectReason::EmptyRay;
        return GizmoBeginDragRejectReason::EmptyHit;
        return GizmoBeginDragRejectReason::InvalidPickConfig;
        return GizmoBeginDragRejectReason::InvalidDimensions;
        return GizmoBeginDragRejectReason::OutOfBounds;
        return GizmoBeginDragRejectReason::ScreenMiss;
        return GizmoBeginDragRejectReason::PickMiss;
    return GizmoBeginDragRejectReason::None;

GizmoUpdateDragRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight) {
    if (preflight.notDragging) {
        return GizmoUpdateDragRejectReason::NotDragging;
    if (preflight.invalidActiveAxis) {
        return GizmoUpdateDragRejectReason::InvalidActiveAxis;
        return GizmoUpdateDragRejectReason::NonFiniteHit;
        return GizmoUpdateDragRejectReason::InvalidDimensions;
        return GizmoUpdateDragRejectReason::EmptyHit;
        return GizmoUpdateDragRejectReason::OutOfBounds;
    return GizmoUpdateDragRejectReason::None;

GizmoEndDragRejectReason classifyEndDragReject(const EndDragPreflight& preflight) {
        return GizmoEndDragRejectReason::NotDragging;
    return GizmoEndDragRejectReason::None;

bool preflightPickReady(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                        GizmoSpace space, f32 axisLength, f32 pickRadius,
                        GizmoPickRejectReason* reason) {
    const PickPreflight preflight =
        preflightPick(ray, transform, mode, space, axisLength, pickRadius);
    if (reason != nullptr) {
        *reason = classifyPickReject(preflight);
    return preflight.canPick();

bool preflightPickReady(const GizmoHitTest& hit, GizmoMode mode, GizmoPickRejectReason* reason) {
    const PickPreflight preflight = preflightPick(hit, mode);

bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoPickRejectReason& reason) {
    return preflightPickReady(ray, transform, mode, space, axisLength, pickRadius, &reason);

bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoPickRejectReason& reason) {
    return preflightPickReady(hit, mode, &reason);

bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius) {
    return !preflightPickReady(ray, transform, mode, space, axisLength, pickRadius);

bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode) {
    return !preflightPickReady(hit, mode);

bool preflightSnapReady(GizmoMode mode, const GizmoSnapSettings& settings,
                        GizmoSnapRejectReason* reason) {
    const SnapPreflight preflight = preflightSnap(mode, settings);
        *reason = classifySnapReject(preflight);
    return preflight.canApply();

bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
                      GizmoSnapRejectReason& reason) {
    return preflightSnapReady(mode, settings, &reason);

bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings) {
    return !preflightSnapReady(mode, settings);

bool preflightSnapDragReady(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings,
                            GizmoSnapDragRejectReason* reason) {
    const SnapDragPreflight preflight = preflightSnapDrag(delta, mode, settings);
        *reason = classifySnapDragReject(preflight);

bool tryPreflightSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings,
                          GizmoSnapDragRejectReason& reason) {
    return preflightSnapDragReady(delta, mode, settings, &reason);

bool shouldSkipSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings) {
    return !preflightSnapDragReady(delta, mode, settings);

bool shouldSkipSnapDrag(GizmoMode mode, const GizmoSnapSettings& settings) {
    return shouldSkipSnap(mode, settings);

bool preflightPickInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                   GizmoMode mode, GizmoSpace space, f32 axisLength,
                                   f32 pickRadius, const GizmoSnapSettings& settings,
    const PickInteractionPreflight preflight =
        preflightPickInteraction(ray, transform, mode, space, axisLength, pickRadius, settings);
        *reason = classifyPickReject(preflight.pick);

bool preflightPickInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
                                   const GizmoSnapSettings& settings,
    const PickInteractionPreflight preflight = preflightPickInteraction(hit, mode, settings);

bool shouldSkipPickInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                               GizmoMode mode, GizmoSpace space, f32 axisLength, f32 pickRadius,
                               const GizmoSnapSettings& settings) {
    return !preflightPickInteractionReady(ray, transform, mode, space, axisLength, pickRadius,
                                          settings);

bool shouldSkipPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
    return !preflightPickInteractionReady(hit, mode, settings);

bool preflightBeginDragInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoBeginDragRejectReason* reason,
                                        bool alreadyDragging) {
    const BeginDragInteractionPreflight preflight = preflightBeginDragInteraction(
        ray, transform, mode, space, axisLength, pickRadius, settings, alreadyDragging);
        *reason = classifyBeginDragReject(preflight.begin);
    return preflight.canBegin();

bool preflightBeginDragInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
    const BeginDragInteractionPreflight preflight =
        preflightBeginDragInteraction(hit, mode, settings, alreadyDragging);

bool shouldSkipBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform,
    return !preflightBeginDragInteractionReady(ray, transform, mode, space, axisLength, pickRadius,
                                               settings, nullptr, alreadyDragging);

bool shouldSkipBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                    const GizmoSnapSettings& settings, bool alreadyDragging) {
    return !preflightBeginDragInteractionReady(hit, mode, settings, nullptr, alreadyDragging);

bool preflightUpdateDragInteractionReady(const GizmoHitTest& hit, bool dragging,
                                         GizmoAxis activeAxis, GizmoMode mode,
                                         GizmoUpdateDragRejectReason* reason) {
    return preflightUpdateDragInteractionReady(hit, dragging, activeAxis, mode, settings, 0.f,
                                               reason, nullptr);

                                         const GizmoSnapSettings& settings, f32 delta,
                                         GizmoUpdateDragRejectReason* reason,
                                         GizmoSnapDragRejectReason* snapDragReason) {
    const UpdateDragInteractionPreflight preflight =
        preflightUpdateDragInteraction(hit, dragging, activeAxis, mode, settings, delta);
        *reason = classifyUpdateDragReject(preflight.drag);
    if (snapDragReason != nullptr) {
        *snapDragReason = classifySnapDragReject(preflight.snapDrag);
    return preflight.canUpdate();

bool shouldSkipUpdateDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                     GizmoMode mode, const GizmoSnapSettings& settings) {
    return !preflightUpdateDragInteractionReady(hit, dragging, activeAxis, mode, settings);

bool preflightEndDragInteractionReady(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                      GizmoEndDragRejectReason* reason) {
    const EndDragInteractionPreflight preflight =
        preflightEndDragInteraction(dragging, activeAxis, mode, settings);
        *reason = classifyEndDragReject(preflight.end);
    return preflight.canEnd();

bool shouldSkipEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    return !preflightEndDragInteractionReady(dragging, activeAxis, mode, settings);

bool preflightBeginDragReady(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                             GizmoBeginDragRejectReason* reason, bool alreadyDragging) {
    const BeginDragPreflight preflight =
        preflightBeginDrag(ray, transform, mode, space, axisLength, pickRadius, alreadyDragging);
        *reason = classifyBeginDragReject(preflight);
    return preflight.canBegin;

bool preflightBeginDragReady(const GizmoHitTest& hit, GizmoMode mode,
    const BeginDragPreflight preflight = preflightBeginDrag(hit, mode, alreadyDragging);

bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoBeginDragRejectReason& reason, bool alreadyDragging) {
    return preflightBeginDragReady(ray, transform, mode, space, axisLength, pickRadius, &reason,
                                   alreadyDragging);

bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
    return preflightBeginDragReady(hit, mode, &reason, alreadyDragging);

bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius, bool alreadyDragging) {
    return !preflightBeginDragReady(ray, transform, mode, space, axisLength, pickRadius, nullptr,

bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging) {
    return !preflightBeginDragReady(hit, mode, nullptr, alreadyDragging);

bool preflightUpdateDragReady(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
    const UpdateDragPreflight preflight = preflightUpdateDrag(hit, dragging, activeAxis);
        *reason = classifyUpdateDragReject(preflight);

bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoUpdateDragRejectReason& reason) {
    return preflightUpdateDragReady(hit, dragging, activeAxis, &reason);

bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis) {
    return !preflightUpdateDragReady(hit, dragging, activeAxis);

bool preflightEndDragReady(bool dragging, GizmoEndDragRejectReason* reason) {
    const EndDragPreflight preflight =
        preflightEndDrag(dragging, GizmoAxis::None, GizmoMode::Translate, {});
        *reason = classifyEndDragReject(preflight);

bool tryPreflightEndDrag(bool dragging, GizmoEndDragRejectReason& reason) {
    return preflightEndDragReady(dragging, &reason);

bool shouldSkipEndDrag(bool dragging) { return !preflightEndDragReady(dragging); }

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging) {
    return preflightUpdateDrag(hit, dragging).canUpdate;

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
    if (!canApplySnap(mode, settings)) {
        return transform;
    }

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
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap,
                                          m_startTransform, m_currentTransform);
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

PickPreflight GizmoSystem::preflightPick(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPick(hit, m_mode);
}

PickPreflight GizmoSystem::preflightPick(const GizmoRay& ray,
                                           const GizmoTransform& transform) const {
    return fuse::editor::preflightPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

SnapPreflight GizmoSystem::preflightSnap() const {
    return fuse::editor::preflightSnap(m_mode, m_snap);
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

    const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::preflightBeginDragInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                                       kPickRadius, m_snap, m_dragging);
GizmoInteractionPreflight GizmoSystem::preflightInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightInteraction(hit, m_mode, m_dragging, m_activeAxis, m_snap);
}

GizmoInteractionPreflight GizmoSystem::preflightInteraction(
    return fuse::editor::preflightInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                              kPickRadius, m_dragging, m_activeAxis, m_snap);
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
SnapPreflight GizmoSystem::preflightSnap(const GizmoTransform& transform) const {
    return fuse::editor::preflightSnap(m_mode, m_snap, transform);
}

bool GizmoSystem::canApplySnapNow() const {
    return fuse::editor::canApplySnap(m_mode, m_snap);
}

bool GizmoSystem::canSnapDragDeltaNow() const {
    return fuse::editor::canSnapDragDelta(m_mode, m_snap);
}

bool GizmoSystem::trySnapTransform(const GizmoTransform& transform, GizmoTransform& out) const {
    return fuse::editor::trySnapTransform(transform, m_mode, m_snap, out);
}

f32 GizmoSystem::trySnapDragDelta(f32 delta) const {
    return fuse::editor::trySnapDragDelta(delta, m_mode, m_snap);
UpdateDragPreflight GizmoSystem::preflightUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

UpdateDragPreflight GizmoSystem::preflightUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis, m_mode, m_snap);
}

UpdateDragInteractionPreflight GizmoSystem::preflightUpdateDragInteraction(
    const GizmoHitTest& hit) const {
    return preflightUpdateDragInteraction(hit, 0.f);

    const GizmoHitTest& hit, f32 delta) const {
    return fuse::editor::preflightUpdateDragInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap,
                                                      delta);
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_mode, m_snap);

bool GizmoSystem::canUpdateDrag(const GizmoHitTest& hit) const {
    return preflightUpdateDrag(hit).canUpdate();

PickSnapPreflight GizmoSystem::preflightPickSnap(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPickSnap(hit, m_mode, m_snap);

PickSnapPreflight GizmoSystem::preflightPickSnap(const GizmoRay& ray,
                                                 const GizmoTransform& transform) const {
    return fuse::editor::preflightPickSnap(ray, transform, m_mode, m_space, kAxisLength,
                                           kPickRadius, m_snap);

BeginInteractionPreflight GizmoSystem::preflightBeginInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightBeginInteraction(hit, m_mode, m_snap, m_dragging);

BeginInteractionPreflight GizmoSystem::preflightBeginInteraction(
    const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::preflightBeginInteraction(ray, transform, m_mode, m_space, kAxisLength,
                                                   kPickRadius, m_snap, m_dragging);

UpdateInteractionPreflight GizmoSystem::preflightUpdateInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);

EndInteractionPreflight GizmoSystem::preflightEndInteraction() const {
    return fuse::editor::preflightEndInteraction(m_dragging, m_activeAxis, m_mode, m_snap);

InteractionPreflight GizmoSystem::preflightInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::preflightInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);

InteractionPreflight GizmoSystem::preflightInteraction(const GizmoRay& ray,
    return fuse::editor::preflightInteraction(ray, transform, m_dragging, m_activeAxis, m_mode,
                                              m_space, kAxisLength, kPickRadius, m_snap);

bool GizmoSystem::canPickSnap(const GizmoHitTest& hit) const {
    return fuse::editor::canPickSnap(hit, m_mode, m_snap);

bool GizmoSystem::canPickSnap(const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::canPickSnap(ray, transform, m_mode, m_space, kAxisLength, kPickRadius,
                                     m_snap);

bool GizmoSystem::canBeginInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::canBeginInteraction(hit, m_mode, m_snap, m_dragging);

bool GizmoSystem::canBeginInteraction(const GizmoRay& ray,
    return fuse::editor::canBeginInteraction(ray, transform, m_mode, m_space, kAxisLength,

bool GizmoSystem::canUpdateInteraction(const GizmoHitTest& hit) const {
    return fuse::editor::canUpdateInteraction(hit, m_dragging, m_activeAxis, m_mode, m_snap);

bool GizmoSystem::canEndInteraction() const {
    return fuse::editor::canEndInteraction(m_dragging, m_activeAxis, m_mode, m_snap);

PickPreflight GizmoSystem::preflightPickAxis(const GizmoRay& ray,
    return fuse::editor::preflightPickAxis(ray, transform, m_mode, m_space, kAxisLength,
                                           kPickRadius);

PickPreflight GizmoSystem::preflightPickAxis(const GizmoHitTest& hit) const {
    return fuse::editor::preflightPickAxis(hit, m_mode);

SnapPreflight GizmoSystem::preflightSnap() const {
    return fuse::editor::preflightSnap(m_mode, m_snap);

UpdateDragPreflight GizmoSystem::preflightUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateDrag(hit, m_mode, m_dragging, m_activeAxis, m_snap);

    return preflightUpdateDrag(hit).canUpdate;
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_mode, m_activeAxis);

    return fuse::editor::canUpdateDrag(hit, m_dragging, m_mode, m_activeAxis);
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_activeAxis);

    return fuse::editor::canUpdateDrag(hit, m_dragging, m_activeAxis);

bool GizmoSystem::shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform) const {
    return fuse::editor::shouldSkipPick(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);

bool GizmoSystem::shouldSkipPick(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipPick(hit, m_mode);

bool GizmoSystem::shouldSkipSnap() const {
    return fuse::editor::shouldSkipSnap(m_mode, m_snap);

bool GizmoSystem::shouldSkipUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::shouldSkipUpdateDrag(hit, m_dragging, m_activeAxis);
    return fuse::editor::preflightUpdateDrag(hit, m_dragging, m_mode);

    return fuse::editor::canUpdateDrag(hit, m_dragging, m_mode);

bool GizmoSystem::tryUpdateDrag(const GizmoHitTest& hit, GizmoResult& out) {
    out = {};
    if (!canUpdateDrag(hit)) {
        return false;

    out = updateDrag(hit);
    return out.changed;

EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging);

bool GizmoSystem::canEndDrag() const {
    return fuse::editor::canEndDrag(m_dragging);

    return fuse::editor::canUpdateDrag(hit, m_dragging);


    if (!preflightUpdateDrag(hit).canUpdate()) {

    m_lastHit = hit;
    m_currentTransform = applySnapping_(applyAxisDelta_(hit, m_startTransform));

    out.active = true;
    out.changed = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;
    return true;



bool GizmoSystem::tryEndDrag(GizmoResult& out) {
    if (!preflightEndDrag().canEnd()) {

    m_currentTransform = applySnapping_(m_currentTransform);
    markDirty_();

    out.active = false;

    m_dragging = false;
    m_activeAxis = GizmoAxis::None;

    return fuse::editor::preflightEndDrag(m_dragging, m_mode, m_snap);


    return fuse::editor::canEndDrag(m_dragging, m_mode, m_snap);

}

EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging, m_mode, m_snap);
}

bool GizmoSystem::canEndDrag() const {
    return fuse::editor::canEndDrag(m_dragging);
}

bool GizmoSystem::canUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::canUpdateDrag(hit, m_dragging);

EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging);

bool GizmoSystem::canEndDrag() const {
    return preflightEndDrag().canEnd();
}

EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis);
}

bool GizmoSystem::canEndDrag() const {
    return preflightEndDrag().canEnd();
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
    if (m_dragging || !fuse::editor::canBeginDrag(hit, m_mode)) {
        return false;
    }

    m_activeAxis = preflight.axis;
    if (m_activeAxis == GizmoAxis::None) {
    const GizmoBeginDragPreflight preflight =
        fuse::editor::preflightBeginDrag(hit, m_mode, m_dragging, m_snap);
    if (!preflight.canBegin()) {
        return false;
    }

    m_activeAxis = preflight.axis;
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
    if (m_dragging ||
        !fuse::editor::canBeginDrag(ray, current, m_mode, m_space, kAxisLength, kPickRadius)) {

    if (!tryPickAxis(ray, current, m_activeAxis)) {
    const GizmoBeginDragPreflight preflight = fuse::editor::preflightBeginDrag(
        ray, current, m_mode, m_space, kAxisLength, kPickRadius, m_dragging, m_snap);
    if (!preflight.canBegin()) {

    m_activeAxis = preflight.axis;
    if (m_activeAxis == GizmoAxis::None) {
        return false;
    }

    m_activeAxis = preflight.axis;
    m_dragging = true;
    m_startTransform = current;
    m_currentTransform = current;
    m_lastHit = {};

    out.active = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;
    return true;
}

GizmoBeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoHitTest& hit,
                                                        const GizmoTransform& current) const {
    (void)current;
    return fuse::editor::preflightBeginDrag(hit, m_mode, m_dragging, m_snap);
}

GizmoBeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoRay& ray,
                                                        const GizmoTransform& current) const {
    return fuse::editor::preflightBeginDrag(ray, current, m_mode, m_space, kAxisLength,
                                            kPickRadius, m_dragging, m_snap);
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
    if (!m_dragging || isHitTestEmpty(hit)) {
    const UpdateDragPreflight preflight = preflightUpdateDrag(hit);
    if (!preflight.canUpdate()) {
        return false;
GizmoResult GizmoSystem::updateDrag(const GizmoHitTest& hit) {
    GizmoResult result;
    if (!tryUpdateDrag(hit, result)) {
bool GizmoSystem::canUpdateDrag(const GizmoHitTest& hit) const {
    return preflightUpdateDrag(hit).canUpdate;
}

UpdateDragPreflight GizmoSystem::preflightUpdateDrag(const GizmoHitTest& hit) const {
    return fuse::editor::preflightUpdateDrag(hit, m_dragging);

    const UpdateDragPreflight preflight = preflightUpdateDrag(hit);
    if (!preflight.canUpdate) {
    if (!canUpdateDrag(hit)) {
        return result;



bool GizmoSystem::tryUpdateDrag(const GizmoHitTest& hit, GizmoResult& out) {
    out = {};
    if (shouldSkipUpdateDrag(hit)) {

    if (!preflightUpdateDrag(hit).canUpdate()) {

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
void GizmoSystem::cancelDrag() {
    if (!m_dragging) {
        return;

    m_dragging = false;
    m_activeAxis = GizmoAxis::None;
    m_currentTransform = m_startTransform;



BeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoHitTest& hit,
                                                 const GizmoTransform& current) const {
    return fuse::editor::preflightBeginDrag(hit, current, m_mode);

BeginDragPreflight GizmoSystem::preflightBeginDrag(const GizmoRay& ray,
    return fuse::editor::preflightBeginDrag(ray, current, m_mode, m_space, kAxisLength,
                                            kPickRadius);

    if (!preflightUpdateDrag(hit).canUpdate()) {
        return false;



EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging);

bool GizmoSystem::canEndDrag() const {
    return preflightEndDrag().canEnd();


}

GizmoResult GizmoSystem::endDrag() {
    GizmoResult result;
    if (!tryEndDrag(result)) {
    if (!preflightEndDrag().canEnd()) {
    if (!canEndDrag()) {
        return result;

bool GizmoSystem::tryEndDrag(GizmoResult& out) {
    out = {};
    if (!preflightEndDrag().canEnd) {
bool GizmoSystem::canEndDrag() const {
    return fuse::editor::canEndDrag(m_dragging, m_activeAxis);
}

EndDragPreflight GizmoSystem::preflightEndDrag() const {
    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis);

    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap).canEnd();

    return fuse::editor::preflightEndDrag(m_dragging, m_activeAxis, m_mode, m_snap);

        return false;
    }
    return result;
}

bool GizmoSystem::tryEndDrag(GizmoResult& out) {
    out = {};
    if (!preflightEndDrag().canEnd()) {
        return false;
    }

bool GizmoSystem::tryEndDrag(GizmoResult& out) {
    out = {};
        return false;

    m_currentTransform = applySnapping_(m_currentTransform);
    markDirty_();

    out.active = false;
    out.changed = true;
    out.axis = m_activeAxis;
    out.transform = m_currentTransform;

    m_dragging = false;
    m_activeAxis = GizmoAxis::None;
    return true;

void GizmoSystem::cancelDrag() {
    if (!m_dragging) {
        return;

    m_startTransform = {};
    m_currentTransform = {};
    m_lastHit = {};

bool GizmoSystem::canBeginDrag(const GizmoHitTest& hit, const GizmoTransform& transform) const {
    (void)transform;
    return !m_dragging && fuse::editor::canBeginDrag(hit, m_mode);

bool GizmoSystem::canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform) const {
    return !m_dragging &&
           fuse::editor::canBeginDrag(ray, transform, m_mode, m_space, kAxisLength, kPickRadius);
}

bool GizmoSystem::tryEndDrag(GizmoResult& out) {
    out = {};
    if (!canEndDrag()) {
        return false;

    out = endDrag();
    return true;




GizmoResult GizmoSystem::endDrag() {
    GizmoResult result;
    if (!tryEndDrag(result)) {
        return result;
    }
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
    const f32 delta = this->trySnapDragDelta(dx + dy);

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
    const SnapPreflight preflight = preflightSnap();
    if (!preflight.canApply) {
        return transform;
    }
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
