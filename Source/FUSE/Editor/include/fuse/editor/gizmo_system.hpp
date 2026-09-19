#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/handle.hpp>
#include <fuse/math/quat.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

enum class GizmoMode {
    Translate,
    Rotate,
    Scale,
};

/// Advance translate → rotate → scale → translate (B6.4 deepen).
GizmoMode cycleGizmoMode(GizmoMode mode);

enum class GizmoAxis {
    None,
    X,
    Y,
    Z,
    Uniform,
};

enum class GizmoSpace {
    Local,
    World,
};

/// Drag lifecycle phase for routing interaction preflights (B6.4 deepen pass).
enum class GizmoInteractionPhase {
    Idle,
    Dragging,
};

/// Why a pick / snap / drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoInteractionRejectReason {
    None,
    EmptyRay,
    EmptyHit,
    NonFiniteInput,
/// Why pick/snap/drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoInteractionRejectReason : u8 {
    None = 0,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
    SnapDisabled,
    InvalidSnapStep,
    NotDragging,
    AlreadyDragging,
    InvalidActiveAxis,
    ModeAxisMismatch,
};


/// Human-readable label for gizmo interaction reject reasons (B6.4 deepen pass).
const char* gizmoInteractionRejectReasonLabel(GizmoInteractionRejectReason reason);

/// Screen-space hit payload for legacy drag stubs (B6.4).
struct GizmoHitTest {
    f32 screenX = 0.f;
    f32 screenY = 0.f;
    f32 viewportWidth = 1.f;
    f32 viewportHeight = 1.f;
};

/// World-space pick ray for analytic axis / plane hit tests (B6.4 deepen).
struct GizmoRay {
    math::Vec3 origin;
    math::Vec3 direction;
};

struct GizmoSnapSettings {
    bool translateSnap = false;
    f32 gridSize = 1.f;
    bool rotateSnap = false;
    f32 angleStepDegrees = 15.f;
    bool scaleSnap = false;
    f32 scaleGridStep = 0.1f;
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

struct GizmoResult {
    bool active = false;
    bool changed = false;
    GizmoAxis axis = GizmoAxis::None;
    GizmoTransform transform;
};

/// Ray vs finite axis segment (capsule radius) — returns closest ray parameter `outT`.
bool hitTestAxisSegment(const GizmoRay& ray, const math::Vec3& segmentStart,
                        const math::Vec3& segmentEnd, f32 radius, f32& outT);

/// Ray vs infinite plane — returns ray parameter `outT` when the ray hits the plane.
bool hitTestAxisPlane(const GizmoRay& ray, const math::Vec3& planeNormal,
                      const math::Vec3& planePoint, f32& outT);

/// Pick the closest gizmo axis (or uniform scale handle) along `ray`.
GizmoAxis pickAxisFromRay(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                          GizmoSpace space, f32 axisLength, f32 pickRadius);

f32 snapToGrid(f32 value, f32 gridSize);
f32 snapAngleRadians(f32 radians, f32 stepDegrees);
f32 snapScale(f32 value, f32 gridStep);

/// Mode-aware scalar snap for the active gizmo channel (B6.4 deepen follow-up).
f32 snapValue(f32 value, GizmoMode mode, const GizmoSnapSettings& settings);

/// True when the active gizmo mode has snap enabled (B6.4 deepen follow-up).
bool isSnapEnabled(GizmoMode mode, const GizmoSnapSettings& settings);

/// Grid / angle / scale step for the active gizmo mode (B6.4 deepen pass).
f32 snapStepForMode(GizmoMode mode, const GizmoSnapSettings& settings);
/// True when snap is enabled for `mode` and its grid/step parameters are usable (B6.4 deepen).
bool isSnapGuardValid(GizmoMode mode, const GizmoSnapSettings& settings);

/// Mode-aware snap for accumulated screen-space drag deltas (B6.4 deepen follow-up).
f32 snapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);

/// Effective snap step for the active mode — 0 when snap is disabled (B6.4 deepen follow-up).
f32 snapStepForMode(GizmoMode mode, const GizmoSnapSettings& settings);

/// Clamp snap grid/step values to sane authoring ranges (B6.4 deepen follow-up).
void clampSnapSettings(GizmoSnapSettings& settings);

/// True when snap grid/step values are positive (B6.4 deepen follow-up).
bool isSnapSettingsValid(const GizmoSnapSettings& settings);
/// Component-wise translate snap (B6.4 deepen — snap helpers).
math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings);

/// Euler rotation snap in radians (B6.4 deepen — snap helpers).
math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings);

/// Component-wise scale snap (B6.4 deepen — snap helpers).
math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings);
/// Why a pick would fail before axis resolution (B6.4 deepen follow-up pass).
enum class PickRejectReason : u8 {
    None = 0,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    ScreenOutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Human-readable label for pick reject reasons (logging / tests).
const char* pickRejectReasonName(PickRejectReason reason);

/// Diagnose why a pick would fail; vacuously succeeds on valid inputs.
PickRejectReason pickRejectReason(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                                  GizmoSpace space, f32 axisLength, f32 pickRadius);
PickRejectReason pickRejectReason(const GizmoHitTest& hit, GizmoMode mode);

/// Returns true when `pickRejectReason` matches `expected` (B6.4 deepen follow-up pass).
bool pickRejectsForReason(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                          GizmoSpace space, f32 axisLength, f32 pickRadius,
                          PickRejectReason expected);
bool pickRejectsForReason(const GizmoHitTest& hit, GizmoMode mode, PickRejectReason expected);

/// Empty-hit guards — reject degenerate pick inputs before axis tests (B6.4 deepen follow-up).
bool isRayEmpty(const GizmoRay& ray);
bool isHitTestEmpty(const GizmoHitTest& hit);

/// True when screen coordinates fall outside the viewport rectangle (B6.4 deepen pass).
bool isHitTestOutOfBounds(const GizmoHitTest& hit);

/// True when viewport width/height are negative (B6.4 deepen pass).
bool isHitTestDimensionsInvalid(const GizmoHitTest& hit);

/// True when a scalar gizmo input is finite (B6.4 deepen pass — finite guard).
bool isFiniteGizmoScalar(f32 value);

/// True when ray origin and direction contain only finite values (B6.4 deepen pass — finite guard).
bool isRayFinite(const GizmoRay& ray);

/// True when screen coordinates and viewport dimensions are finite (B6.4 deepen pass — finite guard).
bool isHitTestFinite(const GizmoHitTest& hit);
/// True when screen coordinates or viewport dimensions are non-finite (B6.4 deepen pass).
/// True when screen or viewport values are NaN or infinite (B6.4 deepen pass).
bool isHitTestNonFinite(const GizmoHitTest& hit);

/// True when ray origin or direction contains non-finite values (B6.4 deepen pass).
bool isRayNonFinite(const GizmoRay& ray);

/// True when the axis is usable for the active gizmo mode (B6.4 deepen pass).
bool isAxisValidForMode(GizmoAxis axis, GizmoMode mode);
/// True when a scalar component is finite (rejects NaN / Inf) (B6.4 deepen pass).
bool isScalarFinite(f32 value);

/// True when all vector components are finite (B6.4 deepen pass).
bool isVec3Finite(const math::Vec3& value);

/// True when ray origin or direction contains non-finite components (B6.4 deepen pass).

/// True when screen coordinates or viewport size contain non-finite components (B6.4 deepen pass).

/// Current drag lifecycle phase (B6.4 deepen pass).
GizmoInteractionPhase interactionPhase(bool dragging);
/// Screen coordinates outside the viewport rectangle (B6.4 deepen pass).
bool isScreenHitOutOfBounds(const GizmoHitTest& hit);
/// True when the viewport is valid and screen coordinates lie inside it (B6.4 deepen pass).
bool isHitTestInBounds(const GizmoHitTest& hit);
/// True when screen or viewport coordinates contain NaN (B6.4 deepen pass).

/// True when ray origin or direction contain NaN or Inf (B6.4 deepen pass).


/// Convenience inverse of `isRayEmpty` / `isHitTestEmpty` (B6.4 deepen follow-up).
bool isRayValid(const GizmoRay& ray);
bool isHitTestValid(const GizmoHitTest& hit);

/// Convenience inverse of `isHitTestOutOfBounds` when the viewport is non-empty (B6.4 deepen pass).
bool isHitTestInBounds(const GizmoHitTest& hit);

/// Normalize ray direction; returns false when the ray is empty (B6.4 deepen follow-up).
/// Convenience inverse of `isRayEmpty` / `isHitTestEmpty` (B6.4 deepen — empty-ray guards).

/// Normalize ray direction; returns false when the ray is empty (B6.4 deepen — empty-ray guard).
/// Convenience inverse of `isRayEmpty` / `isHitTestEmpty` (B6.4 deepen follow-up — pick guard).

/// Normalize ray direction; returns false when the ray is empty (B6.4 deepen follow-up — pick guard).
bool normalizeRay(GizmoRay& ray);

/// True when axis length and pick radius are positive and finite (B6.4 deepen pass).
bool isPickConfigValid(f32 axisLength, f32 pickRadius);

/// True when axis length and pick radius are finite (B6.4 deepen pass — finite guard).
bool isPickConfigFinite(f32 axisLength, f32 pickRadius);

/// True when snap is enabled and the mode step is positive (B6.4 deepen pass).
bool isSnapStepValid(GizmoMode mode, const GizmoSnapSettings& settings);

/// True when the mode step scalar is finite (B6.4 deepen pass — finite guard).
bool isSnapStepFinite(GizmoMode mode, const GizmoSnapSettings& settings);

/// True when snap is enabled with a usable step for the active mode (B6.4 deepen pass).
bool canApplySnap(GizmoMode mode, const GizmoSnapSettings& settings);

/// True when snap is enabled but the mode step is unusable (B6.4 deepen pass).
bool isSnapDegraded(GizmoMode mode, const GizmoSnapSettings& settings);
/// True when screen coordinates lie within viewport bounds (B6.4 deepen follow-up).
bool isScreenCoordInViewport(const GizmoHitTest& hit);
/// Why pick preflight rejected the request (B6.4 deepen pass).
enum class GizmoPickRejectReason : u8 {
    None = 0,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    NonFiniteInput,
    ScreenMiss,
    PickMiss,
};

/// Human-readable label for pick reject reasons (B6.4 deepen pass).
const char* gizmoPickRejectReasonLabel(GizmoPickRejectReason reason);

/// Classify the first blocking pick reject reason from diagnostics (B6.4 deepen pass).
GizmoPickRejectReason classifyPickReject(const struct PickPreflight& preflight);
/// True when screen coordinates fall outside the viewport rectangle (B6.4 deepen pass follow-up).
bool isScreenHitOutOfBounds(const GizmoHitTest& hit);

/// Convenience inverse of `isScreenHitOutOfBounds` (B6.4 deepen pass follow-up).
bool isScreenHitInBounds(const GizmoHitTest& hit);
/// True when snap is enabled but the mode step is unusable — informational only (B6.4 deepen pass).
/// True when screen coordinates fall outside the viewport rectangle (B6.4 deepen pass).

/// True when the hit test has a valid viewport and in-bounds screen coordinates (B6.4 deepen pass).
bool isScreenHitInViewport(const GizmoHitTest& hit);
/// True when all transform components are finite (B6.4 deepen pass).
bool isTransformFinite(const GizmoTransform& transform);

/// True when ray origin and direction are finite (B6.4 deepen pass).
/// True when a scalar input is finite (B6.4 deepen pass).
bool isFiniteValue(f32 value);

/// True when ray origin and direction contain only finite values (B6.4 deepen pass).
bool isRayFinite(const GizmoRay& ray);

/// True when screen coordinates and viewport dimensions are finite (B6.4 deepen pass).
bool isHitTestFinite(const GizmoHitTest& hit);
/// Why snap preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapRejectReason : u8 {
    SnapDisabled,
    InvalidStep,

/// Why begin-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoBeginDragRejectReason : u8 {
    AlreadyDragging,

/// Why update-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoUpdateDragRejectReason : u8 {
    NotDragging,
    InvalidActiveAxis,

/// Why end-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoEndDragRejectReason : u8 {

/// Human-readable labels for pick/snap/drag reject reasons (B6.4 deepen pass).
const char* gizmoSnapRejectReasonLabel(GizmoSnapRejectReason reason);
const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason);
const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason);
const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason);
/// True when screen coordinates are non-finite (B6.4 deepen pass).
bool isHitTestCoordinatesInvalid(const GizmoHitTest& hit);

/// True when ray origin or direction contains non-finite values (B6.4 deepen pass).
bool isRayNonFinite(const GizmoRay& ray);
/// True when ray origin/direction contain non-finite values (B6.4 deepen pass — pick guard).

/// True when screen coordinates and viewport dimensions are finite (B6.4 deepen pass — pick guard).
/// True when ray origin/direction components are finite (B6.4 deepen pass — pick guard).

/// True when screen coordinates are finite (B6.4 deepen pass — pick/update guard).
bool isHitTestScreenFinite(const GizmoHitTest& hit);
/// True when screen or viewport fields are NaN / Inf (B6.4 deepen pass).
bool isHitTestNonFinite(const GizmoHitTest& hit);

/// True when ray direction components are NaN / Inf (B6.4 deepen pass).
bool isRayDirectionNonFinite(const GizmoRay& ray);

/// True when ray direction length is non-zero but not unit length (B6.4 deepen pass).
bool isRayUnnormalized(const GizmoRay& ray);

/// True when the active axis is incompatible with the gizmo mode (B6.4 deepen pass).
bool isAxisValidForMode(GizmoAxis axis, GizmoMode mode);

/// True when snap is enabled and the mode step is negative (B6.4 deepen pass).
bool isSnapStepNegative(GizmoMode mode, const GizmoSnapSettings& settings);
/// True when ray origin or direction contains NaN/Inf (B6.4 deepen pass).

/// True when screen coordinates or viewport dimensions contain NaN/Inf (B6.4 deepen pass).

/// True when the active axis is valid for the current gizmo mode (B6.4 deepen pass).

/// True when a drag delta is finite (B6.4 deepen pass).
bool isDragDeltaFinite(f32 delta);
/// True when ray direction is non-zero and unit length (B6.4 deepen pass).
bool isRayNormalized(const GizmoRay& ray);

/// True when a scalar gizmo input is finite (rejects NaN / infinity) (B6.4 deepen pass).
bool isGizmoScalarFinite(f32 value);

/// True when ray origin or direction contains non-finite components (B6.4 deepen pass).

/// True when screen coordinates or viewport dimensions are non-finite (B6.4 deepen pass).

/// True when snap step fields contain non-finite values (B6.4 deepen pass).
bool isSnapSettingsNonFinite(const GizmoSnapSettings& settings);
/// Read-only ray diagnostics — no mutation (B6.4 deepen pass — pick guard).
struct RayPreflight {
    bool emptyRay = false;
    bool unnormalized = false;

    bool canUse() const { return !emptyRay; }

RayPreflight preflightRay(const GizmoRay& ray);

/// True when ray direction is non-zero but not unit length (B6.4 deepen pass).

/// Non-mutating ray predicate — same guards as `preflightRay` (B6.4 deepen pass).
bool canUseRayForPick(const GizmoRay& ray);

/// Read-only hit-test diagnostics — shared by pick/begin/update guards (B6.4 deepen pass).
struct HitTestPreflight {
    bool emptyHit = false;
    bool invalidDimensions = false;
    bool outOfBounds = false;

    bool canUse() const { return !emptyHit && !invalidDimensions && !outOfBounds; }

HitTestPreflight preflightHitTest(const GizmoHitTest& hit);

/// Non-mutating hit-test predicate — same guards as `preflightHitTest` (B6.4 deepen pass).
bool canUseHitTest(const GizmoHitTest& hit);
bool isHitTestRejected(const GizmoHitTest& hit);

/// Read-only ray diagnostics — shared by pick/begin guards (B6.4 deepen pass).
    bool invalidPickConfig = false;

    bool canUse() const { return !emptyRay && !invalidPickConfig; }

RayPreflight preflightRay(const GizmoRay& ray, f32 axisLength, f32 pickRadius);

bool canUseRay(const GizmoRay& ray, f32 axisLength, f32 pickRadius);
bool isRayRejected(const GizmoRay& ray, f32 axisLength, f32 pickRadius);
/// True when a gizmo drag target handle is set (B6.4 deepen pass — interaction guard).
bool isGizmoTargetValid(Handle<Object> target);

/// True when ray direction is unit length within epsilon (B6.4 deepen pass — pick guard).
bool isRayUnitLength(const GizmoRay& ray);
/// True when snap is enabled but the mode step is unusable — drag still applies without rounding (B6.4 deepen pass).
/// Snap enabled but mode step unusable — begin/update/end still apply (B6.4 deepen pass).
/// True when snap is enabled but the mode step is unusable — drag still applies (B6.4 deepen pass).

/// Read-only pick diagnostics — no mutation (B6.4 deepen follow-up — pick guard).
struct PickPreflight {
    PickRejectReason reason = PickRejectReason::None;
    bool emptyRay = false;
    bool nonFiniteRay = false;
    bool emptyHit = false;
    bool nonFiniteRay = false;
    bool nonFiniteHit = false;
    bool outOfBounds = false;
    bool nonFiniteInput = false;
    bool invalidPickConfig = false;
    bool invalidDimensions = false;
    bool invalidCoordinates = false;
    bool nonFiniteRay = false;
    bool nonFiniteScreen = false;
    bool nonFinite = false;
    bool nonFiniteHit = false;
    bool outOfBounds = false;
    bool screenOutOfBounds = false;
    bool nonFiniteInput = false;
    bool screenMiss = false;
    bool outOfBounds = false;
    bool pickMiss = false;
    bool invalidTransform = false;
    bool nonFiniteRay = false;
    bool nonFiniteHit = false;
    bool nonFiniteInput = false;
    /// Non-unit direction — pick still proceeds (B6.4 deepen pass).
    bool unnormalizedRay = false;
    /// Non-unit ray direction — informational only; does not block pick (B6.4 deepen pass).
    bool nonUnitRay = false;
    /// Ray direction is non-zero but not unit length — pick still applies (B6.4 deepen pass).
    bool nonUnitDirection = false;
    GizmoAxis axis = GizmoAxis::None;

    bool canPick() const {
        return !emptyRay && !emptyHit && !nonFiniteRay && !nonFiniteHit && !invalidPickConfig &&
               !invalidDimensions && !outOfBounds && !screenMiss && !pickMiss;
    bool outOfBounds = false;

        return !emptyRay && !emptyHit && !invalidPickConfig && !screenMiss && !pickMiss &&
               !outOfBounds;
        return !emptyRay && !emptyHit && !outOfBounds && !invalidPickConfig && !screenMiss &&
        return !emptyRay && !emptyHit && !invalidPickConfig && !outOfBounds && !screenMiss &&
        return !emptyRay && !emptyHit && !invalidPickConfig && !screenMiss && !outOfBounds &&
               !pickMiss;
               !pickMiss && !invalidTransform && !nonFiniteRay && !nonFiniteHit;
        return !emptyRay && !emptyHit && !invalidPickConfig && !invalidDimensions && !outOfBounds &&
               !nonFiniteInput && !screenMiss && !pickMiss;
        return !emptyRay && !nonFiniteRay && !emptyHit && !invalidPickConfig && !invalidDimensions &&
               !invalidCoordinates && !outOfBounds && !screenMiss && !pickMiss;
        return !emptyRay && !emptyHit && !invalidPickConfig && !invalidDimensions && !nonFiniteRay &&
               !nonFiniteScreen && !outOfBounds && !screenMiss && !pickMiss;
               !screenMiss && !pickMiss && !nonFiniteInput;
               !screenMiss && !pickMiss && !nonFiniteRay && !nonFiniteHit;
        return !emptyRay && !emptyHit && !invalidPickConfig && !invalidDimensions && !nonFinite &&
               !outOfBounds && !screenMiss && !pickMiss;
        return !emptyRay && !emptyHit && !invalidPickConfig && !invalidDimensions && !nonFiniteHit &&
               !nonFiniteRay && !outOfBounds && !screenMiss && !pickMiss;
    }
/// Read-only pick diagnostics — no mutation (B6.4 deepen follow-up).
    bool canPick = false;
    bool canPick() const { return reason == PickRejectReason::None; }
        return !emptyRay && !emptyHit && !nonFiniteInput && !invalidPickConfig &&

    GizmoInteractionRejectReason rejectReason() const;
};

PickPreflight preflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                            GizmoSpace space, f32 axisLength, f32 pickRadius);
PickPreflight preflightPick(const GizmoHitTest& hit, GizmoMode mode);

/// Why snap cannot apply for the active mode (B6.4 deepen follow-up pass).
enum class SnapRejectReason : u8 {
    None = 0,
    Disabled,
    InvalidStep,
};

/// Human-readable label for snap reject reasons (logging / tests).
const char* snapRejectReasonName(SnapRejectReason reason);

/// Diagnose why snap cannot apply; vacuously succeeds when snap is usable.
SnapRejectReason snapRejectReason(GizmoMode mode, const GizmoSnapSettings& settings);

/// Returns true when `snapRejectReason` matches `expected` (B6.4 deepen follow-up pass).
bool snapRejectsForReason(GizmoMode mode, const GizmoSnapSettings& settings, SnapRejectReason expected);

/// Read-only snap diagnostics — no mutation (B6.4 deepen follow-up — snap guard).
struct SnapPreflight {
    SnapRejectReason reason = SnapRejectReason::None;
    bool snapDisabled = false;
    bool nonFiniteStep = false;
    bool invalidStep = false;
    /// Transform already satisfies snap for the active mode (B6.4 deepen pass).
    bool noChange = false;
    /// Resolved step for the active gizmo mode (B6.4 deepen follow-up).
    /// Resolved step for the active gizmo mode (B6.4 deepen pass).
    f32 step = 0.f;
    bool negativeStep = false;
    bool nonFiniteSettings = false;

    bool canApply() const { return !snapDisabled && !invalidStep && !nonFiniteSettings; }
    /// Enabled snap with unusable step — drag still applies without grid rounding (B6.4 deepen pass).
    bool isDegraded() const { return !snapDisabled && invalidStep; }
    bool wouldSnap() const { return canApply() && !noChange; }
    bool canApply() const { return reason == SnapRejectReason::None; }

    GizmoInteractionRejectReason rejectReason() const;
    bool isDegraded() const { return !snapDisabled && (invalidStep || nonFiniteSettings); }
    bool canApply() const { return !snapDisabled && !nonFiniteStep && !invalidStep; }
    bool isDegraded() const { return !snapDisabled && (nonFiniteStep || invalidStep); }
};

SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings);
SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
                            const GizmoTransform& transform);

/// Read-only snap-drag diagnostics — no mutation (B6.4 deepen pass — snap guard).
struct SnapDragPreflight {
    bool deltaNonFinite = false;
    bool snapDisabled = false;
    bool nonFiniteStep = false;
    bool invalidStep = false;

    bool canApply() const {
        return !deltaNonFinite && !snapDisabled && !nonFiniteStep && !invalidStep;
    }
    /// Enabled snap with unusable step — drag delta still applies without rounding (B6.4 deepen pass).
    bool isDegraded() const { return !deltaNonFinite && !snapDisabled && invalidStep; }
    bool isDegraded() const {
        return !deltaNonFinite && !snapDisabled && (nonFiniteStep || invalidStep);
    }
};

SnapDragPreflight preflightSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);

/// Read-only snap-drag diagnostics — distinguishes disabled vs degraded snap (B6.4 deepen pass).
struct SnapDragPreflight {
    bool snapDisabled = false;
    /// Snap is enabled but the mode step is unusable — drag delta passthrough (B6.4 deepen pass).
    bool snapDegraded = false;

    bool canApply() const { return !snapDisabled && !snapDegraded; }
};

/// Drag-context snap diagnostics — disabled vs degraded (B6.4 deepen pass).
SnapDragPreflight preflightSnapDrag(GizmoMode mode, const GizmoSnapSettings& settings);

/// Non-mutating snap-drag predicate — same guards as `preflightSnapDrag` (B6.4 deepen pass).
bool canSnapDrag(GizmoMode mode, const GizmoSnapSettings& settings);

/// Read-only hover/pick frame diagnostics — pick + snap, no drag mutation (B6.4 deepen pass follow-up).
struct InteractionPreflight {
    PickPreflight pick;
    SnapPreflight snap;

    bool canInteract() const { return pick.canPick(); }
    bool snapReady() const { return snap.canApply(); }
};

InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          GizmoMode mode, GizmoSpace space, f32 axisLength,
                                          f32 pickRadius, const GizmoSnapSettings& settings);
InteractionPreflight preflightInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                          const GizmoSnapSettings& settings);

/// Read-only drag-delta snap diagnostics — no mutation (B6.4 deepen pass).
struct SnapDragPreflight {
    bool snapDisabled = false;
    bool invalidStep = false;

    bool canApply() const { return !snapDisabled && !invalidStep; }
};

SnapDragPreflight preflightSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings);

/// Why pick preflight rejected the request (B6.4 deepen pass — reject-reason guards).
enum class GizmoPickGuardRejectReason : u8 {
    None = 0,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Why snap preflight rejected apply (B6.4 deepen pass — reject-reason guards).
enum class GizmoSnapGuardRejectReason : u8 {
    None = 0,
    SnapDisabled,
    InvalidStep,
};

/// Why begin-drag preflight rejected the request (B6.4 deepen pass — reject-reason guards).
enum class GizmoBeginDragGuardRejectReason : u8 {
    None = 0,
    AlreadyDragging,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Why update-drag preflight rejected the request (B6.4 deepen pass — reject-reason guards).
enum class GizmoUpdateDragGuardRejectReason : u8 {
    None = 0,
    NotDragging,
    EmptyHit,
    InvalidDimensions,
    OutOfBounds,
    InvalidActiveAxis,
};

/// Why end-drag preflight rejected the request (B6.4 deepen pass — reject-reason guards).
enum class GizmoEndDragGuardRejectReason : u8 {
    None = 0,
    NotDragging,
};

/// Human-readable labels for pick/snap/drag guard reject reasons (B6.4 deepen pass).
const char* gizmoPickGuardRejectReasonLabel(GizmoPickGuardRejectReason reason);
const char* gizmoSnapGuardRejectReasonLabel(GizmoSnapGuardRejectReason reason);
const char* gizmoBeginDragGuardRejectReasonLabel(GizmoBeginDragGuardRejectReason reason);
const char* gizmoUpdateDragGuardRejectReasonLabel(GizmoUpdateDragGuardRejectReason reason);
const char* gizmoEndDragGuardRejectReasonLabel(GizmoEndDragGuardRejectReason reason);

/// Map read-only preflight diagnostics to a single reject reason (B6.4 deepen pass).
GizmoPickGuardRejectReason classifyPickReject(const PickPreflight& preflight);
GizmoSnapGuardRejectReason classifySnapReject(const SnapPreflight& preflight);

/// Pick preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoSpace space, f32 axisLength, f32 pickRadius,
                      GizmoPickGuardRejectReason& reason);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoPickGuardRejectReason& reason);

/// Snap preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
                      GizmoSnapGuardRejectReason& reason);

/// Begin-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoSpace space, f32 axisLength, f32 pickRadius,
                           GizmoBeginDragGuardRejectReason& reason, bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoSpace space, f32 axisLength, f32 pickRadius,
                           const GizmoSnapSettings& settings, GizmoBeginDragGuardRejectReason& reason,
                           bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                           GizmoBeginDragGuardRejectReason& reason, bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                           const GizmoSnapSettings& settings, GizmoBeginDragGuardRejectReason& reason,
                           bool alreadyDragging = false);

/// Update-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoUpdateDragGuardRejectReason& reason);
bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoMode mode, const GizmoSnapSettings& settings,
                            GizmoUpdateDragGuardRejectReason& reason);

/// End-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                         const GizmoSnapSettings& settings, GizmoEndDragGuardRejectReason& reason);

/// Non-mutating skip predicates — inverse of pick/snap/drag preflights (B6.4 deepen pass).
bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius);
bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode);
bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings);
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging,
                          GizmoAxis activeAxis = GizmoAxis::None);
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode, const GizmoSnapSettings& settings);
bool shouldSkipEndDrag(bool dragging, GizmoAxis activeAxis = GizmoAxis::None);
bool shouldSkipEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                       const GizmoSnapSettings& settings);

/// Combined interaction skip predicates (B6.4 deepen pass).
bool shouldSkipPickInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                               GizmoSpace space, f32 axisLength, f32 pickRadius,
                               const GizmoSnapSettings& settings);
bool shouldSkipPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
                               const GizmoSnapSettings& settings);
bool shouldSkipBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                    GizmoMode mode, GizmoSpace space, f32 axisLength,
                                    f32 pickRadius, const GizmoSnapSettings& settings,
                                    bool alreadyDragging = false);
bool shouldSkipBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                    const GizmoSnapSettings& settings,
                                    bool alreadyDragging = false);
bool shouldSkipUpdateDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                     GizmoMode mode, const GizmoSnapSettings& settings);
bool shouldSkipEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                  const GizmoSnapSettings& settings);

/// Why pick preflight rejected the request (B6.4 deepen pass — pick guard).
enum class GizmoPickRejectReason : u8 {
    None = 0,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Why snap preflight rejected the request (B6.4 deepen pass — snap guard).
enum class GizmoSnapRejectReason : u8 {
    None = 0,
    SnapDisabled,
    InvalidStep,
};

/// Why begin-drag preflight rejected the request (B6.4 deepen pass — begin guard).
enum class GizmoBeginDragRejectReason : u8 {
    None = 0,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
    AlreadyDragging,
};

/// Why update-drag preflight rejected the request (B6.4 deepen pass — update guard).
enum class GizmoUpdateDragRejectReason : u8 {
    None = 0,
    NotDragging,
    EmptyHit,
    InvalidDimensions,
    OutOfBounds,
    InvalidActiveAxis,
};

/// Why end-drag preflight rejected the request (B6.4 deepen pass — end guard).
enum class GizmoEndDragRejectReason : u8 {
    None = 0,
    NotDragging,
};

const char* gizmoPickRejectReasonLabel(GizmoPickRejectReason reason);
const char* gizmoSnapRejectReasonLabel(GizmoSnapRejectReason reason);
const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason);
const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason);
const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason);

GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight);
GizmoSnapRejectReason classifySnapReject(const SnapPreflight& preflight);

bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoSpace space, f32 axisLength, f32 pickRadius, PickPreflight& out,
                      GizmoPickRejectReason& outReason);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, PickPreflight& out,
                      GizmoPickRejectReason& outReason);
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings, SnapPreflight& out,
                      GizmoSnapRejectReason& outReason);

bool shouldSkipPick(const PickPreflight& preflight);
bool shouldSkipSnap(const SnapPreflight& preflight);

/// Read-only drag-delta snap diagnostics — no mutation (B6.4 deepen pass).
struct SnapDeltaPreflight {
    bool snapDisabled = false;
    bool invalidStep = false;
    f32 delta = 0.f;
    f32 snappedDelta = 0.f;

    bool canApply() const { return !snapDisabled && !invalidStep; }
    bool isDegraded() const { return !snapDisabled && invalidStep; }
};

SnapDeltaPreflight preflightSnapDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);

/// Non-mutating drag-delta snap predicate — same guards as `preflightSnapDelta` (B6.4 deepen pass).
bool canSnapDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);

/// Why pick preflight rejected the request (B6.4 deepen pass — pick guard).
enum class GizmoPickRejectReason : u8 {
    None = 0,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Why snap preflight rejected the request (B6.4 deepen pass — snap guard).
enum class GizmoSnapRejectReason : u8 {
    None = 0,
    SnapDisabled,
    InvalidStep,
};

/// Why begin-drag preflight rejected the request (B6.4 deepen pass — drag guard).
enum class GizmoBeginDragRejectReason : u8 {
    None = 0,
    AlreadyDragging,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Why update-drag preflight rejected the request (B6.4 deepen pass — drag guard).
enum class GizmoUpdateDragRejectReason : u8 {
    None = 0,
    NotDragging,
    EmptyHit,
    InvalidDimensions,
    OutOfBounds,
    InvalidActiveAxis,
};

/// Why end-drag preflight rejected the request (B6.4 deepen pass — drag guard).
enum class GizmoEndDragRejectReason : u8 {
    None = 0,
    NotDragging,
};

/// Human-readable labels for pick/snap/drag reject reasons (B6.4 deepen pass).
const char* gizmoPickRejectReasonLabel(GizmoPickRejectReason reason);
const char* gizmoSnapRejectReasonLabel(GizmoSnapRejectReason reason);
const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason);
const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason);
const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason);

/// Derive primary reject reason from read-only preflight diagnostics (B6.4 deepen pass).
GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight);
GizmoSnapRejectReason classifySnapReject(const SnapPreflight& preflight);

/// Pick preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoSpace space, f32 axisLength, f32 pickRadius, GizmoPickRejectReason& reason);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoPickRejectReason& reason);

/// Snap preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings, GizmoSnapRejectReason& reason);

/// Begin-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoSpace space, f32 axisLength, f32 pickRadius,
                           GizmoBeginDragRejectReason& reason, bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoSpace space, f32 axisLength, f32 pickRadius,
                           const GizmoSnapSettings& settings, GizmoBeginDragRejectReason& reason,
                           bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                           GizmoBeginDragRejectReason& reason, bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                           const GizmoSnapSettings& settings, GizmoBeginDragRejectReason& reason,
                           bool alreadyDragging = false);

/// Update-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoUpdateDragRejectReason& reason);
bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoMode mode, const GizmoSnapSettings& settings,
                            GizmoUpdateDragRejectReason& reason);

/// End-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                         const GizmoSnapSettings& settings, GizmoEndDragRejectReason& reason);

/// Early-out when pick preflight would reject (B6.4 deepen pass).
bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius);
bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode);

/// Early-out when snap preflight would reject (B6.4 deepen pass).
bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings);

/// Early-out when begin-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);

/// Early-out when update-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis = GizmoAxis::None);
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode, const GizmoSnapSettings& settings);

/// Early-out when end-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipEndDrag(bool dragging, GizmoAxis activeAxis = GizmoAxis::None);
bool shouldSkipEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                       const GizmoSnapSettings& settings);

/// Guarded transform snap — returns false when snap cannot apply (B6.4 deepen follow-up).
bool trySnapTransform(const GizmoTransform& transform, GizmoMode mode,
                      const GizmoSnapSettings& settings, GizmoTransform& out);

/// Guarded scalar snap — returns false when snap cannot apply (B6.4 deepen follow-up — snap guard).
bool trySnapValue(f32 value, GizmoMode mode, const GizmoSnapSettings& settings, f32& out);

/// Component-wise snap helpers (B6.4 deepen follow-up — snap guard).
math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings);
math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings);
math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings);

/// Mode-aware drag-delta snap with step validation (B6.4 deepen follow-up).
f32 trySnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);

/// Component-wise translate / rotate / scale snap helpers (B6.4 deepen follow-up).
math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings);
math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings);
math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings);

/// Non-mutating snap-drag predicate — same guards as `trySnapDragDelta` (B6.4 deepen follow-up).
bool canSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings);
bool canSnapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);
/// Read-only snap diagnostics — no mutation (B6.4 deepen follow-up).
    bool canApply = false;

/// Why begin-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoBeginDragRejectReason : u8 {
    None = 0,
    AlreadyDragging,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    ScreenMiss,
    PickMiss,
};

/// Human-readable label for begin-drag reject reasons (B6.4 deepen pass).
const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason);

/// Classify the first blocking begin-drag reject reason from diagnostics (B6.4 deepen pass).
GizmoBeginDragRejectReason classifyBeginDragReject(const struct BeginDragPreflight& preflight);

/// Combined pick + snap diagnostics — read-only, no mutation (B6.4 deepen pass).
struct PickSnapPreflight {
    PickPreflight pick{};
    SnapPreflight snap{};
    /// Snap is enabled but the mode step is unusable — pick still proceeds (B6.4 deepen pass).
    bool snapDegraded = false;

    bool canPick() const { return pick.canPick(); }
};

PickSnapPreflight preflightPickSnap(GizmoMode mode, const GizmoSnapSettings& settings);
PickSnapPreflight preflightPickSnap(const GizmoRay& ray, const GizmoTransform& transform,
                                    GizmoMode mode, GizmoSpace space, f32 axisLength,
                                    f32 pickRadius, const GizmoSnapSettings& settings);
PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit, GizmoMode mode,
                                    const GizmoSnapSettings& settings);

/// Read-only drag-delta snap diagnostics — same guards as `trySnapDragDelta` (B6.4 deepen pass).
struct SnapDragPreflight {
    bool snapDisabled = false;
    bool invalidStep = false;

    bool canApply() const { return !snapDisabled && !invalidStep; }
    /// Enabled snap with unusable step — drag delta still applies without grid rounding (B6.4 deepen pass).
    bool isDegraded() const { return !snapDisabled && invalidStep; }
};

SnapDragPreflight preflightSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings);

/// Read-only drag-delta snap diagnostics — no mutation (B6.4 deepen pass).
struct SnapDragDeltaPreflight {
    bool snapDisabled = false;
    bool invalidStep = false;

    bool canApply() const { return !snapDisabled && !invalidStep; }
    bool isDegraded() const { return !snapDisabled && invalidStep; }
};

SnapDragDeltaPreflight preflightSnapDragDelta(GizmoMode mode, const GizmoSnapSettings& settings);

/// Read-only begin-drag diagnostics — no mutation (B6.4 deepen pass).
struct BeginDragPreflight {
    bool canBegin = false;
    bool emptyHit = false;
    bool emptyRay = false;
    bool nonFiniteInput = false;
    bool nonFiniteRay = false;
    bool nonFiniteHit = false;
    bool invalidPickConfig = false;
    bool nonFiniteRay = false;
    bool invalidDimensions = false;
    bool invalidCoordinates = false;
    bool nonFiniteRay = false;
    bool nonFiniteScreen = false;
    bool nonFinite = false;
    bool nonFiniteHit = false;
    bool outOfBounds = false;
    bool nonFiniteInput = false;
    bool screenMiss = false;
    bool outOfBounds = false;
    bool pickMiss = false;
    bool invalidTransform = false;
    bool nonFiniteRay = false;
    bool nonFiniteHit = false;
    bool nonFiniteInput = false;
    bool alreadyDragging = false;
    /// No valid gizmo target — begin blocked (B6.4 deepen pass — interaction guard).
    bool noTarget = false;
    /// Resolved axis from pick preflight when `canBegin` (B6.4 deepen pass).
    GizmoAxis axis = GizmoAxis::None;
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen pass follow-up).
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen follow-up).
    /// Resolved axis from pick preflight when `canBegin` (B6.4 deepen pass).
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen pass).
    bool snapDegraded = false;
    GizmoAxis pickedAxis = GizmoAxis::None;
    /// Resolved pick axis when begin can proceed (B6.4 deepen pass).

    GizmoInteractionRejectReason rejectReason() const;
};

    /// Resolved axis from pick preflight when `canBegin` (B6.4 deepen pass).
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen pass).
    bool snapDegraded = false;

/// Why update-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoUpdateDragRejectReason : u8 {
    None = 0,
    NotDragging,
    InvalidActiveAxis,
    EmptyHit,

/// Human-readable label for update-drag reject reasons (B6.4 deepen pass).
const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason);

/// Classify the first blocking update-drag reject reason from diagnostics (B6.4 deepen pass).
GizmoUpdateDragRejectReason classifyUpdateDragReject(const struct UpdateDragPreflight& preflight);

/// Read-only update-drag diagnostics — no mutation (B6.4 deepen follow-up).
struct UpdateDragPreflight {
    bool canUpdate = false;
    bool notDragging = false;
    bool emptyHit = false;

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging);

/// Why begin-drag would fail before mutating drag state (B6.4 deepen follow-up pass).
enum class BeginDragRejectReason : u8 {
    None = 0,
    AlreadyDragging,
    EmptyRay,
    EmptyHit,
    InvalidPickConfig,
    ScreenOutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Human-readable label for begin-drag reject reasons (logging / tests).
const char* beginDragRejectReasonName(BeginDragRejectReason reason);

/// Diagnose why begin-drag would fail; vacuously succeeds when drag can start.
BeginDragRejectReason beginDragRejectReason(const GizmoRay& ray, const GizmoTransform& transform,
                                            GizmoMode mode, GizmoSpace space, f32 axisLength,
                                            f32 pickRadius, bool alreadyDragging = false);
BeginDragRejectReason beginDragRejectReason(const GizmoHitTest& hit, GizmoMode mode,
                                            bool alreadyDragging = false);

/// Returns true when `beginDragRejectReason` matches `expected` (B6.4 deepen follow-up pass).
bool beginDragRejectsForReason(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                               GizmoSpace space, f32 axisLength, f32 pickRadius,
                               BeginDragRejectReason expected, bool alreadyDragging = false);
bool beginDragRejectsForReason(const GizmoHitTest& hit, GizmoMode mode, BeginDragRejectReason expected,

/// Read-only begin-drag diagnostics — no mutation (B6.4 deepen pass).
struct BeginDragPreflight {
    BeginDragRejectReason reason = BeginDragRejectReason::None;
    bool canBegin = false;
    bool alreadyDragging = false;
    /// Resolved axis from pick preflight when `canBegin` (B6.4 deepen pass).
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen pass).
    bool nonFiniteHit = false;
    bool invalidDimensions = false;
    bool invalidCoordinates = false;
    bool nonFiniteScreen = false;
    bool nonFinite = false;
    bool nonFiniteHit = false;
    bool outOfBounds = false;
    bool nonFiniteInput = false;
    bool invalidActiveAxis = false;
    /// Hit is in the screen dead zone — update still applies (B6.4 deepen pass).
    /// Viewport is valid but the cursor is in the mode dead zone — update still applies (B6.4 deepen pass).
    bool screenMiss = false;
    bool invalidDimensions = false;
    bool modeAxisMismatch = false;
    bool nonFiniteHit = false;
    bool invalidAxisForMode = false;
    /// No valid gizmo target — update blocked (B6.4 deepen pass — interaction guard).
    bool noTarget = false;
    /// Snap is enabled but the mode step is unusable — update still applies (B6.4 deepen pass).
    bool snapDegraded = false;
    bool emptyHit = false;
    bool emptyRay = false;
    bool outOfBounds = false;
    bool invalidPickConfig = false;
    bool screenOutOfBounds = false;
    bool screenMiss = false;
    bool pickMiss = false;
    bool alreadyDragging = false;
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen pass).
    /// Snap is enabled but the mode step is unusable — begin still applies (B6.4 deepen follow-up).
    bool snapDegraded = false;
    GizmoAxis axis = GizmoAxis::None;
};

/// Why update-drag would fail before applying a delta (B6.4 deepen follow-up pass).
enum class UpdateDragRejectReason : u8 {
    None = 0,
    NotDragging,
    EmptyHit,
    ScreenOutOfBounds,
    InvalidActiveAxis,
};

/// Human-readable label for update-drag reject reasons (logging / tests).
const char* updateDragRejectReasonName(UpdateDragRejectReason reason);

/// Diagnose why update-drag would fail; vacuously succeeds when update can apply.
UpdateDragRejectReason updateDragRejectReason(const GizmoHitTest& hit, bool dragging,
                                              GizmoAxis activeAxis = GizmoAxis::None);

/// Returns true when `updateDragRejectReason` matches `expected` (B6.4 deepen follow-up pass).
bool updateDragRejectsForReason(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                UpdateDragRejectReason expected);

/// Read-only update-drag diagnostics — no mutation (B6.4 deepen follow-up).
struct UpdateDragPreflight {
    UpdateDragRejectReason reason = UpdateDragRejectReason::None;
    bool notDragging = false;
    bool emptyHit = false;
    bool outOfBounds = false;
    bool screenOutOfBounds = false;
    bool screenMiss = false;
    bool invalidActiveAxis = false;
    bool nonFiniteHit = false;
    /// Screen dead-zone during drag — informational only; update still applies (B6.4 deepen pass).
    bool screenMiss = false;
    /// Snap is enabled but the mode step is unusable — update still applies (B6.4 deepen pass).
    bool emptyHit = false;
    /// Screen dead-zone hit during drag — diagnostic only; does not block `canUpdate()` (B6.4 deepen pass).
    /// Diagnostic only — does not affect `canUpdate()` (drag may continue through dead zones).
    bool screenMiss = false;

    bool canUpdate() const {
        return !notDragging && !emptyHit && !nonFiniteHit && !invalidDimensions && !outOfBounds &&
               !invalidActiveAxis;
    bool outOfBounds = false;

        return !notDragging && !emptyHit && !invalidActiveAxis && !outOfBounds;
        return !notDragging && !emptyHit && !outOfBounds && !invalidActiveAxis;
        return !notDragging && !emptyHit && !invalidActiveAxis && !screenMiss;
        return !notDragging && !emptyHit && !outOfBounds && !screenMiss && !invalidActiveAxis;
        return !notDragging && !emptyHit && !outOfBounds && !nonFiniteHit && !invalidActiveAxis;
        return !notDragging && !emptyHit && !invalidDimensions && !outOfBounds && !nonFiniteInput &&
        return !notDragging && !emptyHit && !invalidDimensions && !invalidCoordinates &&
               !outOfBounds && !invalidActiveAxis;
        return !notDragging && !emptyHit && !invalidDimensions && !nonFiniteScreen && !outOfBounds &&
        return !notDragging && !emptyHit && !invalidDimensions && !outOfBounds && !invalidActiveAxis &&
               !nonFiniteInput && !modeAxisMismatch;
        return !notDragging && !emptyHit && !invalidDimensions && !outOfBounds && !nonFiniteHit &&
               !invalidActiveAxis && !invalidAxisForMode;
        return !notDragging && !emptyHit && !invalidDimensions && !nonFinite && !outOfBounds &&
        return !notDragging && !emptyHit && !invalidDimensions && !nonFiniteHit && !outOfBounds &&
        return !notDragging && !emptyHit && !invalidDimensions && !outOfBounds &&
               !invalidActiveAxis && !noTarget;
    }
    bool canUpdate() const { return reason == UpdateDragRejectReason::None; }
        return !notDragging && !emptyHit && !nonFiniteInput && !invalidDimensions && !outOfBounds &&
               !invalidActiveAxis && !modeAxisMismatch;

    GizmoInteractionRejectReason rejectReason() const;
};

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging,
                                        GizmoAxis activeAxis = GizmoAxis::None);
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                        GizmoMode mode);
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                        GizmoMode mode, const GizmoSnapSettings& settings);
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                                        GizmoMode mode, const GizmoSnapSettings& settings,
                                        Handle<Object> target);

/// Combined update-drag interaction — drag update + snap guards (B6.4 deepen pass).
struct UpdateInteractionPreflight {
    UpdateDragPreflight drag{};
    SnapPreflight snap{};

    bool canUpdate() const { return drag.canUpdate(); }
    bool snapDegraded() const { return drag.snapDegraded; }
};

UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
                                                      GizmoAxis activeAxis, GizmoMode mode,
                                                      const GizmoSnapSettings& settings);

/// Non-mutating update-drag predicate — same guards as `preflightUpdateDrag` (B6.4 deepen follow-up).
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis = GizmoAxis::None);
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode);
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings);

/// Why end-drag would fail before committing drag state (B6.4 deepen follow-up pass).
enum class EndDragRejectReason : u8 {
/// Why end-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoEndDragRejectReason : u8 {
    None = 0,
    NotDragging,
};

/// Human-readable label for end-drag reject reasons (logging / tests).
const char* endDragRejectReasonName(EndDragRejectReason reason);

/// Diagnose why end-drag would fail; vacuously succeeds when end can apply.
EndDragRejectReason endDragRejectReason(bool dragging);

/// Returns true when `endDragRejectReason` matches `expected` (B6.4 deepen follow-up pass).
bool endDragRejectsForReason(bool dragging, EndDragRejectReason expected);

/// Read-only end-drag diagnostics — no mutation (B6.4 deepen pass).
struct EndDragPreflight {
    EndDragRejectReason reason = EndDragRejectReason::None;
    bool notDragging = false;
    bool invalidActiveAxis = false;
    bool modeAxisMismatch = false;
    bool invalidAxisForMode = false;
    /// No valid gizmo target — end blocked (B6.4 deepen pass — interaction guard).
    bool noTarget = false;
    /// Snap is enabled but the mode step is unusable — end still applies (B6.4 deepen pass).
    bool snapDegraded = false;
    /// Transform matches drag start — end still applies (B6.4 deepen pass).
    bool unchangedTransform = false;

    bool canEnd() const { return !notDragging; }
    bool canEnd() const { return reason == EndDragRejectReason::None; }

    GizmoInteractionRejectReason rejectReason() const;
};

EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

GizmoBeginDragGuardRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight);
GizmoUpdateDragGuardRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight);
GizmoEndDragGuardRejectReason classifyEndDragReject(const EndDragPreflight& preflight);

/// Early-out when pick preflight would reject (B6.4 deepen pass).
bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius);
bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode);

/// Early-out when begin-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging = false);

/// Early-out when update-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode);

/// Early-out when end-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipEndDrag(bool dragging);

/// Classify reject reasons from read-only preflight diagnostics (B6.4 deepen pass).
GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight);
GizmoSnapRejectReason classifySnapReject(const SnapPreflight& preflight);
GizmoBeginDragRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight);
GizmoUpdateDragRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight);
GizmoEndDragRejectReason classifyEndDragReject(const EndDragPreflight& preflight);

/// Preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoSpace space, f32 axisLength, f32 pickRadius, PickPreflight& out,
                      GizmoPickRejectReason& reason);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, PickPreflight& out,
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings, SnapPreflight& out,
                      GizmoSnapRejectReason& reason);
bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           const GizmoSnapSettings& settings, bool alreadyDragging,
                           BeginDragPreflight& out, GizmoBeginDragRejectReason& reason);
bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoMode mode, const GizmoSnapSettings& settings,
                            UpdateDragPreflight& out, GizmoUpdateDragRejectReason& reason);
bool tryPreflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                         const GizmoSnapSettings& settings, EndDragPreflight& out,
                         GizmoEndDragRejectReason& reason);


                           const GizmoSnapSettings& settings, BeginDragPreflight& out,
                           GizmoBeginDragRejectReason& outReason, bool alreadyDragging = false);
                            UpdateDragPreflight& out, GizmoUpdateDragRejectReason& outReason);
                         GizmoEndDragRejectReason& outReason);

bool shouldSkipBeginDrag(const BeginDragPreflight& preflight);
bool shouldSkipUpdateDrag(const UpdateDragPreflight& preflight);
bool shouldSkipEndDrag(const EndDragPreflight& preflight);

/// Classify why pick preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyPickReject(const PickPreflight& preflight);
/// Classify why snap preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifySnapReject(const SnapPreflight& preflight);
/// Classify why begin-drag preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight);
/// Classify why update-drag preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight);
/// Classify why end-drag preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyEndDragReject(const EndDragPreflight& preflight);

/// True when pick preflight would reject the request (B6.4 deepen pass).
bool shouldSkipPick(const PickPreflight& preflight);
/// True when snap preflight would reject apply (B6.4 deepen pass).
bool shouldSkipSnap(const SnapPreflight& preflight);
/// True when begin-drag preflight would reject (B6.4 deepen pass).
/// True when update-drag preflight would reject (B6.4 deepen pass).
/// True when end-drag preflight would reject (B6.4 deepen pass).

/// Pick preflight with mandatory reject-reason output (B6.4 deepen pass).
                      GizmoInteractionRejectReason& reason);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode,
/// Snap preflight with mandatory reject-reason output (B6.4 deepen pass).
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
/// Begin-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
                           GizmoInteractionRejectReason& reason, bool alreadyDragging = false);
/// Update-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
/// End-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
                         const GizmoSnapSettings& settings, GizmoInteractionRejectReason& reason);
    bool canEnd() const { return !notDragging && !noTarget; }

                                  const GizmoSnapSettings& settings);
                                  const GizmoSnapSettings& settings, Handle<Object> target);

/// Derive drag reject reasons from read-only preflight diagnostics (B6.4 deepen pass).
GizmoBeginDragRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight);
GizmoUpdateDragRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight);
GizmoEndDragRejectReason classifyEndDragReject(const EndDragPreflight& preflight);

/// Combined pick + snap diagnostics — no mutation (B6.4 deepen pass — interaction guard).
struct PickInteractionPreflight {
    PickPreflight pick{};
    SnapPreflight snap{};
    /// Snap is enabled but the mode step is unusable — pick still applies (B6.4 deepen pass).
    bool snapDegraded = false;

    bool canPick() const { return pick.canPick(); }
    bool snapWillApply() const { return snap.canApply(); }
    bool pickBlocked() const { return !canPick(); }
    bool snapBlocked() const { return !snapWillApply(); }
    /// Enabled snap with unusable step — pick still applies (B6.4 deepen pass).
    bool isSnapDegraded() const { return snap.isDegraded(); }
    bool isSnapDegraded() const { return snapDegraded; }
    bool snapDegraded() const { return snap.isDegraded(); }
};

PickInteractionPreflight preflightPickInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                                  GizmoMode mode, GizmoSpace space, f32 axisLength,
                                                  f32 pickRadius,
PickInteractionPreflight preflightPickInteraction(const GizmoHitTest& hit, GizmoMode mode,

/// Combined begin-drag + snap diagnostics — no mutation (B6.4 deepen pass — interaction guard).
struct BeginDragInteractionPreflight {
    BeginDragPreflight begin{};

    bool canBegin() const { return begin.canBegin; }
    bool isSnapDegraded() const { return snapDegraded; }
    bool snapWillApply() const { return snap.canApply(); }
};

BeginDragInteractionPreflight preflightBeginDragInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging = false);
BeginDragInteractionPreflight preflightBeginDragInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, Handle<Object> target,
    bool alreadyDragging = false);
BeginDragInteractionPreflight preflightBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                            const GizmoSnapSettings& settings,
                                                            bool alreadyDragging = false);
BeginDragInteractionPreflight preflightBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                            const GizmoSnapSettings& settings,
                                                            Handle<Object> target,
                                                            bool alreadyDragging = false);

/// Combined update-drag + snap diagnostics — no mutation (B6.4 deepen pass — interaction guard).
struct UpdateDragInteractionPreflight {
    UpdateDragPreflight drag{};
    SnapDragPreflight snapDrag{};

    bool canUpdate() const { return drag.canUpdate(); }
    bool snapDragWillApply() const { return snapDrag.canApply(); }
    bool isSnapDegraded() const { return drag.snapDegraded; }
    bool isSnapDragDegraded() const { return snapDrag.isDegraded(); }
    SnapPreflight snap{};
    /// Snap is enabled but the mode step is unusable — update still applies (B6.4 deepen pass).
    bool snapDegraded = false;

    bool snapWillApply() const { return snap.canApply(); }
    bool isSnapDegraded() const { return snapDegraded; }
};

UpdateDragInteractionPreflight preflightUpdateDragInteraction(const GizmoHitTest& hit, bool dragging,
                                                              GizmoAxis activeAxis, GizmoMode mode,
                                                              f32 delta);
                                                              const GizmoSnapSettings& settings);
UpdateDragInteractionPreflight preflightUpdateDragInteraction(const GizmoHitTest& hit, bool dragging,
                                                              GizmoAxis activeAxis, GizmoMode mode,
                                                              const GizmoSnapSettings& settings,
                                                              Handle<Object> target);
                                                              f32 dragDelta = 0.f);
                                                              f32 delta = 0.f);

/// Combined end-drag + snap diagnostics — no mutation (B6.4 deepen pass — interaction guard).
struct EndDragInteractionPreflight {
    EndDragPreflight end{};

    bool canEnd() const { return end.canEnd(); }

EndDragInteractionPreflight preflightEndDragInteraction(bool dragging, GizmoAxis activeAxis,
                                                        GizmoMode mode,
                                                        const GizmoSnapSettings& settings);
EndDragInteractionPreflight preflightEndDragInteraction(bool dragging, GizmoAxis activeAxis,
                                                        GizmoMode mode,
                                                        const GizmoSnapSettings& settings,
                                                        Handle<Object> target);

/// Combined drag-cycle diagnostics — update and end paths for active drags (B6.4 deepen pass).
struct DragInteractionPreflight {
    UpdateDragInteractionPreflight update{};
    EndDragInteractionPreflight end{};

    bool canUpdate() const { return !notDragging && update.canUpdate(); }
    bool canEnd() const { return !notDragging && end.canEnd(); }
    bool canInteract() const { return canUpdate() || canEnd(); }
    /// Primary update action allowed while dragging (B6.4 deepen pass).
    bool canActOnPhase() const { return canUpdate(); }

    /// Primary action allowed while dragging — update when hit is usable (B6.4 deepen pass).
    bool canActOnPhase() const { return !notDragging && canUpdate(); }
};

DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit, bool dragging,

/// Combined begin/update/end + snap diagnostics for drag lifecycle routing (B6.4 deepen pass).
struct DragSessionPreflight {
    bool dragging = false;
    BeginDragPreflight begin{};
    UpdateDragPreflight update{};
    EndDragPreflight end{};
    SnapPreflight snap{};

    GizmoInteractionPhase phase() const {
        return dragging ? GizmoInteractionPhase::Dragging : GizmoInteractionPhase::Idle;
    }

    bool canBegin() const { return !dragging && begin.canBegin; }
    bool canUpdate() const { return dragging && update.canUpdate(); }
    bool canEnd() const { return dragging && end.canEnd(); }
    bool snapWillApply() const { return snap.canApply(); }

    /// Primary drag action allowed for the active lifecycle phase (B6.4 deepen pass).
    bool canActOnPhase() const {
        switch (phase()) {
        case GizmoInteractionPhase::Idle:
            return canBegin();
        case GizmoInteractionPhase::Dragging:
            return canUpdate();
        return false;
};

DragSessionPreflight preflightDragSession(const GizmoHitTest& hit, bool dragging,
                                          GizmoAxis activeAxis, GizmoMode mode,
                                          const GizmoSnapSettings& settings,
                                          bool alreadyDragging = false);
DragSessionPreflight preflightDragSession(const GizmoRay& ray, const GizmoTransform& transform,
                                          bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                          GizmoSpace space, f32 axisLength, f32 pickRadius,
                                                  const GizmoSnapSettings& settings);
DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit, bool dragging,
                                                  Handle<Object> target);

/// Non-mutating combined interaction predicates (B6.4 deepen pass).
bool canPickInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                        GizmoSpace space, f32 axisLength, f32 pickRadius,
bool canPickInteraction(const GizmoHitTest& hit, GizmoMode mode,
bool canBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
bool canBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
bool canUpdateDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
bool canEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
bool canDragInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                        GizmoMode mode, const GizmoSnapSettings& settings);
bool canDragSession(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                    const GizmoSnapSettings& settings, bool alreadyDragging = false);
bool canDragSession(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                    GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                    f32 pickRadius, const GizmoSnapSettings& settings,
                    bool alreadyDragging = false);

/// Non-mutating end-drag predicate — same guards as `preflightEndDrag` (B6.4 deepen pass).
bool canEndDrag(bool dragging, GizmoAxis activeAxis = GizmoAxis::None);
bool canEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

/// Combined pick + snap diagnostics — no mutation (B6.4 deepen pass).
struct PickSnapPreflight {

    bool canSnap() const { return snap.canApply(); }
    /// Snap enabled but step unusable — pick still allowed (B6.4 deepen pass).
    bool isSnapDegraded() const { return snap.isDegraded(); }
    bool snapDegraded() const { return snap.isDegraded(); }
};

/// Phase-routed snap diagnostics for interaction lifecycle (B6.4 deepen pass).
struct SnapInteractionPreflight {
    GizmoInteractionPhase phase = GizmoInteractionPhase::Idle;
    SnapPreflight snap{};

    bool canApply() const { return snap.canApply(); }
    bool isDegraded() const { return snap.isDegraded(); }
    bool canPick() const { return pick.canPick(); }
    bool canSnap() const { return snap.canApply(); }
    bool snapDegraded() const { return snap.isDegraded(); }
};

SnapInteractionPreflight preflightSnapInteraction(GizmoInteractionPhase phase, GizmoMode mode,
                                                  const GizmoSnapSettings& settings);

/// Non-mutating phase-routed snap predicates (B6.4 deepen pass).
bool canApplySnapInteraction(GizmoInteractionPhase phase, GizmoMode mode,
                             const GizmoSnapSettings& settings);
bool isSnapInteractionDegraded(GizmoInteractionPhase phase, GizmoMode mode,
                               const GizmoSnapSettings& settings);

/// Combined begin-drag + snap diagnostics — no mutation (B6.4 deepen pass).
struct BeginInteractionPreflight {

    bool snapReady() const { return snap.canApply(); }
    bool isSnapDegraded() const { return begin.snapDegraded; }
    /// Snap enabled but step unusable — begin still allowed (B6.4 deepen pass).
    bool snapDegraded() const { return begin.snapDegraded || snap.isDegraded(); }
};

/// Combined update-drag + snap diagnostics — no mutation (B6.4 deepen pass).
struct UpdateInteractionPreflight {
    UpdateDragPreflight update{};

    bool canUpdate() const { return update.canUpdate(); }
    bool snapDegraded() const { return update.snapDegraded; }
    bool snapReady() const { return snap.canApply(); }
};

/// Combined end-drag + snap diagnostics — no mutation (B6.4 deepen pass).
struct EndInteractionPreflight {

    bool snapDegraded() const { return end.snapDegraded; }
    bool snapReady() const { return snap.canApply(); }
};

/// Explicit interaction action for lifecycle routing (B6.4 deepen pass).
enum class GizmoInteractionAction {
    Pick,
    Begin,
    Update,
    End,
};

/// Snap diagnostics bundled across begin/update/end drag phases (B6.4 deepen pass).
struct DragLifecycleSnapPreflight {
    SnapPreflight begin{};
    SnapPreflight update{};
    SnapPreflight end{};

    bool anyDegraded() const {
        return begin.isDegraded() || update.isDegraded() || end.isDegraded();
    }
    bool allCanApply() const {
        return begin.canApply() && update.canApply() && end.canApply();
    }
};

DragLifecycleSnapPreflight preflightDragLifecycleSnap(GizmoMode mode,
                                                      const GizmoSnapSettings& settings);

/// Combined interaction diagnostics for the active drag phase (B6.4 deepen pass).
struct InteractionPreflight {
    bool dragging = false;
    PickSnapPreflight pickSnap{};
    BeginInteractionPreflight begin{};
    UpdateInteractionPreflight update{};
    EndInteractionPreflight end{};

    GizmoInteractionPhase phase() const {
        return dragging ? GizmoInteractionPhase::Dragging : GizmoInteractionPhase::Idle;

    bool canPick() const { return pickSnap.canPick(); }
    bool canApplySnap() const { return pickSnap.canSnap(); }
    bool snapBlocked() const { return !canApplySnap(); }
    bool canBegin() const { return !dragging && begin.canBegin(); }
    bool canUpdate() const { return dragging && update.canUpdate(); }
    bool canEnd() const { return dragging && end.canEnd(); }
    bool isSnapDegraded() const {
        return dragging ? (update.snapDegraded() || end.snapDegraded()) : begin.isSnapDegraded();

    /// Primary action allowed for the active lifecycle phase (B6.4 deepen pass).
    /// While dragging, end remains routable when update is blocked (B6.4 deepen pass).
    bool canActOnPhase() const {
        switch (phase()) {
        case GizmoInteractionPhase::Idle:
            return canBegin();
        case GizmoInteractionPhase::Dragging:
            return canUpdate();
        return false;

PickSnapPreflight preflightPickSnap(const GizmoRay& ray, const GizmoTransform& transform,
                                    f32 pickRadius, const GizmoSnapSettings& settings);
PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit, GizmoMode mode,

BeginInteractionPreflight preflightBeginInteraction(
BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,

UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

InteractionPreflight preflightInteraction(const GizmoHitTest& hit, bool dragging,
InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          bool dragging, GizmoAxis activeAxis, GizmoMode mode,

/// Non-mutating pick-snap predicate — same guards as `preflightPickSnap` (B6.4 deepen pass).
bool canPickSnap(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
bool canPickSnap(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings);

/// Non-mutating begin-interaction predicate — same guards as `preflightBeginInteraction` (B6.4 deepen pass).
bool canBeginInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);
bool canBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,

/// Non-mutating update-interaction predicate — same guards as `preflightUpdateInteraction` (B6.4 deepen pass).
bool canUpdateInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,

/// Non-mutating end-interaction predicate — same guards as `preflightEndInteraction` (B6.4 deepen pass).
bool canEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
    GizmoAxis axis = GizmoAxis::None;

/// Read-only drag-update diagnostics — no mutation (B6.4 deepen pass).
    bool canUpdate = false;
    bool emptyHit = false;
    bool screenMiss = false;
    bool noActiveAxis = false;

        return !notDragging && !emptyHit && !screenMiss && !noActiveAxis;
    }

                                        GizmoMode mode = GizmoMode::Translate,

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,
                   GizmoAxis activeAxis);

/// Read-only end-drag diagnostics — no mutation (B6.4 deepen follow-up).


EndDragPreflight preflightEndDrag(bool dragging);

    bool canUpdate() const { return !notDragging && !emptyHit && !noActiveAxis; }

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging);
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis);

/// Early-out when pick preflight would reject (B6.4 deepen follow-up — pick guard).
bool shouldSkipPick(const PickPreflight& preflight);
bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius);
bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode);

/// Early-out when snap preflight would reject (B6.4 deepen follow-up — snap guard).
bool shouldSkipSnap(const SnapPreflight& preflight);
bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings);

/// Early-out when update-drag preflight would reject (B6.4 deepen follow-up).
bool shouldSkipUpdateDrag(const UpdateDragPreflight& preflight);
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging);
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis);

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging);
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis);
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode);

/// Read-only end-drag diagnostics — no mutation (B6.4 deepen pass — end-drag guard).



/// Non-mutating update-drag predicate — same guards as `preflightUpdateDrag` (B6.4 deepen pass).




/// Non-mutating end-drag predicate — rejects inactive drags (B6.4 deepen pass).
bool canEndDrag(bool dragging);

/// Read-only end-drag diagnostics — no mutation (B6.4 deepen pass — end guard).
    bool snapSkipped = false;


EndDragPreflight preflightEndDrag(bool dragging, GizmoMode mode, const GizmoSnapSettings& settings);


bool canEndDrag(bool dragging, GizmoMode mode, const GizmoSnapSettings& settings);


/// Read-only end-drag diagnostics — no mutation (B6.4 deepen follow-up — end-drag guard).

bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode);

    bool snapDisabled = false;
    bool invalidSnapStep = false;




/// Non-mutating update-drag predicate — empty-hit / inactive-drag early-outs (B6.4 deepen pass).

    bool canEnd = false;


EndDragPreflight preflightEndDrag(bool dragging, GizmoMode mode,
                                  const GizmoSnapSettings& settings);

/// Human-readable label for end-drag reject reasons (B6.4 deepen pass).
const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason);

/// Classify the first blocking end-drag reject reason from diagnostics (B6.4 deepen pass).
GizmoEndDragRejectReason classifyEndDragReject(const struct EndDragPreflight& preflight);

/// Read-only end-drag diagnostics — no mutation (B6.4 deepen pass).
struct EndDragPreflight {
    bool notDragging = false;
    bool invalidActiveAxis = false;

    bool canEnd() const { return !notDragging && !invalidActiveAxis; }
};

EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis = GizmoAxis::None);

/// Non-mutating end-drag predicate — same guards as `preflightEndDrag` (B6.4 deepen pass).
bool canEndDrag(bool dragging, GizmoAxis activeAxis = GizmoAxis::None);




/// Non-mutating end-drag predicate — same guards as `preflightEndDrag` (B6.4 deepen follow-up).

    bool snapWillApply = false;

    bool modeAxisMismatch = false;
    /// Snap is enabled but the mode step is unusable — end still applies (B6.4 deepen pass).
    bool snapDegraded = false;

    bool canEnd() const { return !notDragging; }

    GizmoInteractionRejectReason rejectReason() const;

EndDragPreflight preflightEndDrag(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                  const GizmoSnapSettings& settings,
                                  const GizmoTransform& startTransform,
                                  const GizmoTransform& currentTransform);

/// Combined end-drag interaction — end drag + snap guards (B6.4 deepen pass).
struct EndInteractionPreflight {
    EndDragPreflight drag{};
    SnapPreflight snap{};

    bool canEnd() const { return drag.canEnd(); }
    bool snapDegraded() const { return drag.snapDegraded; }



/// Early-out when pick preflight would reject (B6.4 deepen pass).

/// Early-out when begin-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         GizmoSpace space, f32 axisLength, f32 pickRadius,
                         bool alreadyDragging = false);
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging = false);

/// Early-out when update-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode);

/// Early-out when end-drag preflight would reject (B6.4 deepen pass).
bool shouldSkipEndDrag(bool dragging);

/// Combined pick + snap diagnostics — no mutation (B6.4 deepen pass).
struct PickSnapPreflight {
    PickPreflight pick{};

    bool canPick() const { return pick.canPick(); }
    bool canSnap() const { return snap.canApply(); }
    bool snapDegraded() const { return snap.isDegraded(); }

/// Combined begin-drag + snap diagnostics — no mutation (B6.4 deepen pass).
struct BeginInteractionPreflight {
    BeginDragPreflight begin{};

    bool canBegin() const { return begin.canBegin; }
    bool snapReady() const { return snap.canApply(); }
    bool snapWillApply() const { return snap.canApply(); }
    bool snapDegraded() const { return begin.snapDegraded; }

/// Combined update-drag + snap diagnostics — no mutation (B6.4 deepen pass).
struct UpdateInteractionPreflight {
    UpdateDragPreflight update{};

    bool canUpdate() const { return update.canUpdate(); }
    bool snapDegraded() const { return update.snapDegraded; }

/// Combined end-drag + snap diagnostics — no mutation (B6.4 deepen pass).
    EndDragPreflight end{};

    bool canEnd() const { return end.canEnd(); }
    bool snapDegraded() const { return end.snapDegraded; }

/// Combined interaction diagnostics for the active drag phase (B6.4 deepen pass).
struct InteractionPreflight {
    bool dragging = false;
    PickSnapPreflight pickSnap{};
    BeginInteractionPreflight begin{};
    UpdateInteractionPreflight update{};
    EndInteractionPreflight end{};

    bool canPick() const { return pickSnap.canPick(); }
    bool canApplySnap() const { return pickSnap.canSnap(); }
    bool canBegin() const { return !dragging && begin.canBegin(); }
    bool canUpdate() const { return dragging && update.canUpdate(); }
    bool canEnd() const { return dragging && end.canEnd(); }

                                    GizmoMode mode, GizmoSpace space, f32 axisLength,


    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging = false);


                                                      GizmoAxis activeAxis, GizmoMode mode,

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis,
                                                GizmoMode mode, const GizmoSnapSettings& settings);












    SnapInteractionPreflight snapInteraction{};


    /// Snap is enabled but the mode step is unusable for the active phase (B6.4 deepen pass).
    bool snapDegraded() const { return snapInteraction.isDegraded(); }

    /// Whether snap will apply to the primary action for the active lifecycle phase (B6.4 deepen pass).
    bool snapWillApplyOnPhase() const {
        switch (phase()) {
        case GizmoInteractionPhase::Idle:
            return pickSnap.canSnap();
        case GizmoInteractionPhase::Dragging:
            return update.snap.canApply() || end.snap.canApply();
            return canUpdate() || canEnd();
        }
        return false;
    }

    /// Primary action allowed for the active lifecycle phase (B6.4 deepen pass).
    bool canActOnPhase() const {
        switch (phase()) {
        case GizmoInteractionPhase::Idle:
            return canBegin();
        case GizmoInteractionPhase::Dragging:
            return canUpdate() || canEnd();
        }
        return false;
    }

    /// Primary reject reason for the active lifecycle phase (B6.4 deepen pass).
    GizmoInteractionRejectReason primaryRejectReason() const;
    /// Snap is enabled but the mode step is unusable for the active lifecycle phase (B6.4 deepen pass).
    bool snapDegraded() const {
        return dragging ? update.snapDegraded() : begin.snapDegraded();
    /// Snap is enabled but the mode step is unusable — drag still applies (B6.4 deepen pass).
        return begin.snapDegraded() || update.snapDegraded() || end.snapDegraded();
    }

    /// Snap will apply on the active mode (B6.4 deepen pass).
    bool snapWillApply() const { return pickSnap.canSnap(); }

    /// End remains routable while dragging (B6.4 deepen pass).
    bool canEndOnPhase() const { return dragging && end.canEnd(); }

    /// Primary or end action allowed for the active phase (B6.4 deepen pass).
    bool canActOrEndOnPhase() const {
        switch (phase()) {
        case GizmoInteractionPhase::Idle:
            return canBegin();
        case GizmoInteractionPhase::Dragging:
            return canUpdate() || canEnd();
    /// Any drag-cycle action allowed — update or end while dragging (B6.4 deepen pass).
    bool canInteract() const {
        if (!dragging) {

    /// Snap degraded for the active lifecycle phase (B6.4 deepen pass).
    bool isSnapDegradedForPhase() const {
            return begin.isSnapDegraded();
            return update.snapDegraded() || end.snapDegraded();
        return false;
    /// Any interaction path allowed for the active lifecycle phase (B6.4 deepen pass).
            return canPick() || canBegin();
            return canBegin() || canPick();

    /// Snap is enabled but the mode step is unusable for the active phase (B6.4 deepen pass).
    bool isSnapDegraded() const {
            return begin.snapDegraded() || pickSnap.snapDegraded();

    /// Snap will apply on the primary action for the active phase (B6.4 deepen pass).
    bool snapWillApplyOnPhase() const {
            return begin.snapReady();
            return update.snap.canApply();
    /// Snap is enabled but the mode step is unusable for the active lifecycle (B6.4 deepen pass).
        return begin.begin.snapDegraded || update.snapDegraded() || end.snapDegraded();
    /// Explicit action routing — pick / begin / update / end (B6.4 deepen pass).
    bool canAct(GizmoInteractionAction action) const {
        switch (action) {
        case GizmoInteractionAction::Pick:
            return canPick();
        case GizmoInteractionAction::Begin:
        case GizmoInteractionAction::Update:
            return canUpdate();
        case GizmoInteractionAction::End:
            return canEnd();
};

/// Non-mutating phase-routing predicate — same guards as `InteractionPreflight::canActOnPhase`.
            return begin.begin.snapDegraded;
            return update.snapDegraded();


/// Read-only mode-change diagnostics — no mutation (B6.4 deepen pass — mode guard).
struct ModeChangePreflight {
    bool unchanged = false;
    bool wouldCancelDrag = false;

    bool canChange() const { return !unchanged; }

ModeChangePreflight preflightModeChange(GizmoMode current, GizmoMode next, bool dragging);
ModeChangePreflight preflightCycleMode(GizmoMode current, bool dragging);

/// Non-mutating mode-change predicates — same guards as mode preflights (B6.4 deepen pass).
bool canChangeMode(GizmoMode current, GizmoMode next, bool dragging);
bool canCycleMode(GizmoMode current, bool dragging);

/// Non-mutating phase-routing predicate — same guards as `InteractionPreflight::canActOnPhase` (B6.4 deepen pass).
bool canActOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings);
bool canActOnPhase(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                   GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                   f32 pickRadius, const GizmoSnapSettings& settings);
    }

    /// Any interaction path allowed for the active lifecycle phase (B6.4 deepen pass).
    bool canInteractOnPhase() const {
        switch (phase()) {
        case GizmoInteractionPhase::Idle:
            return canBegin();
        case GizmoInteractionPhase::Dragging:
            return canUpdate() || canEnd();
        return false;

    /// Snap is enabled but the mode step is unusable for the active phase (B6.4 deepen pass).
    bool snapDegradedOnPhase() const {
            return begin.snapDegraded();
            return update.snapDegraded();

    /// Snap will apply on the primary action for the active phase (B6.4 deepen pass).
    bool snapWillApplyOnPhase() const {
            return begin.snapWillApply();
            return update.snapWillApply();

/// Phase-routed action diagnostics — primary path + snap readiness (B6.4 deepen pass).
struct PhaseActionPreflight {
    GizmoInteractionPhase phase = GizmoInteractionPhase::Idle;
    bool canAct = false;
    bool canInteract = false;
    bool snapDegraded = false;
    bool snapWillApply = false;

PhaseActionPreflight preflightPhaseAction(const GizmoHitTest& hit, bool dragging,
                                          GizmoAxis activeAxis, GizmoMode mode,

PickSnapPreflight preflightPickSnap(const GizmoRay& ray, const GizmoTransform& transform,
                                    GizmoMode mode, GizmoSpace space, f32 axisLength,
                                    f32 pickRadius, const GizmoSnapSettings& settings);
PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit, GizmoMode mode,
                                    const GizmoSnapSettings& settings);

BeginInteractionPreflight preflightBeginInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, bool alreadyDragging = false);
BeginInteractionPreflight preflightBeginInteraction(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings, Handle<Object> target,
    bool alreadyDragging = false);
BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                    const GizmoSnapSettings& settings,
                                                    bool alreadyDragging = false);
BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                    const GizmoSnapSettings& settings,
                                                    Handle<Object> target,
                                                    bool alreadyDragging = false);

UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
                                                        GizmoAxis activeAxis, GizmoMode mode,

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis,
                                                GizmoMode mode, const GizmoSnapSettings& settings);
                                                      const GizmoSnapSettings& settings);
UpdateInteractionPreflight preflightUpdateInteraction(const GizmoHitTest& hit, bool dragging,
                                                      const GizmoSnapSettings& settings,
                                                      Handle<Object> target);

EndInteractionPreflight preflightEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

InteractionPreflight preflightInteraction(const GizmoHitTest& hit, bool dragging,
InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                          GizmoSpace space, f32 axisLength, f32 pickRadius,
















/// Combined pick/snap/drag frame diagnostics — no mutation (B6.4 deepen pass).
struct DragInteractionPreflight {
    SnapDragPreflight snap{};

    bool snapWillApply() const { return snap.canApply(); }

DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                                  bool dragging,
                                                  GizmoAxis activeAxis = GizmoAxis::None,
/// Non-mutating begin-interaction predicate — same guards as `preflightBeginInteraction` (B6.4 deepen pass).
bool canBeginInteraction(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings,
bool canBeginInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);

/// Non-mutating update-interaction predicate — same guards as `preflightUpdateInteraction` (B6.4 deepen pass).
bool canUpdateInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,

/// Non-mutating end-interaction predicate — same guards as `preflightEndInteraction` (B6.4 deepen pass).
bool canEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

/// Non-mutating phase-action predicates — same guards as `InteractionPreflight` (B6.4 deepen pass).
bool canActOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings);
bool canInteractOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                        GizmoMode mode, const GizmoSnapSettings& settings);
bool snapDegradedOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                         GizmoMode mode, const GizmoSnapSettings& settings);
bool snapWillApplyOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode, const GizmoSnapSettings& settings);

/// Classify why combined pick-interaction preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyPickInteractionReject(const PickInteractionPreflight& preflight);
/// Classify why combined begin-drag-interaction preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyBeginDragInteractionReject(
    const BeginDragInteractionPreflight& preflight);
/// Classify why combined update-drag-interaction preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyUpdateDragInteractionReject(
    const UpdateDragInteractionPreflight& preflight);
/// Classify why combined end-drag-interaction preflight would reject (B6.4 deepen pass).
GizmoInteractionRejectReason classifyEndDragInteractionReject(
    const EndDragInteractionPreflight& preflight);
/// Classify primary reject for combined interaction preflight (B6.4 deepen pass).
GizmoInteractionRejectReason classifyInteractionReject(const InteractionPreflight& preflight);

/// True when combined pick-interaction preflight would reject (B6.4 deepen pass).
bool shouldSkipPickInteraction(const PickInteractionPreflight& preflight);
/// True when combined begin-drag-interaction preflight would reject (B6.4 deepen pass).
bool shouldSkipBeginDragInteraction(const BeginDragInteractionPreflight& preflight);
/// True when combined update-drag-interaction preflight would reject (B6.4 deepen pass).
bool shouldSkipUpdateDragInteraction(const UpdateDragInteractionPreflight& preflight);
/// True when combined end-drag-interaction preflight would reject (B6.4 deepen pass).
bool shouldSkipEndDragInteraction(const EndDragInteractionPreflight& preflight);
/// True when combined interaction preflight blocks the active phase action (B6.4 deepen pass).
bool shouldSkipInteraction(const InteractionPreflight& preflight);

/// Non-mutating phase-routed action predicate — same guards as `InteractionPreflight::canActOnPhase` (B6.4 deepen pass).
bool canActOnInteractionPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                              GizmoMode mode, const GizmoSnapSettings& settings);
bool canActOnInteractionPhase(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                              GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space,
                              f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings);

/// Phase-routed gizmo diagnostics — bundles all interaction preflight layers (B6.4 deepen pass).
struct GizmoPreflightRouter {
    InteractionPreflight interaction{};
    DragInteractionPreflight drag{};
    PickInteractionPreflight pickInteraction{};
    BeginDragInteractionPreflight beginDragInteraction{};

    GizmoInteractionPhase phase() const { return interaction.phase(); }

    bool snapDegraded() const {
        return interaction.snapDegraded() || beginDragInteraction.snapDegraded;
    }

    bool snapWillApply() const { return interaction.snapWillApply(); }

    bool canRoutePick() const { return pickInteraction.canPick(); }
    bool canRouteBegin() const { return !interaction.dragging && beginDragInteraction.canBegin(); }
    bool canRouteUpdate() const { return drag.canUpdate(); }
    bool canRouteEnd() const { return drag.canEnd(); }
    /// Primary routed action for the active phase — begin when idle, update when dragging.
    bool canRoutePrimary() const { return interaction.canActOnPhase(); }
    /// End remains routable while dragging even when update is blocked.
    bool canRouteEndOnPhase() const { return interaction.canEndOnPhase(); }
    /// Primary or end action allowed for the active phase.
    bool canRouteActOrEnd() const { return interaction.canActOrEndOnPhase(); }
    /// Any interaction path allowed for the active inputs.
    bool canRouteAny() const {
        return canRoutePick() || canRouteBegin() || canRouteUpdate() || canRouteEnd();
    }
};

GizmoPreflightRouter preflightGizmoRouter(const GizmoHitTest& hit, bool dragging,
                                          GizmoAxis activeAxis, GizmoMode mode,
                                          const GizmoSnapSettings& settings);
GizmoPreflightRouter preflightGizmoRouter(const GizmoRay& ray, const GizmoTransform& transform,
InteractionPreflight preflightInteraction(const GizmoHitTest& hit, bool dragging,
                                          GizmoAxis activeAxis, GizmoMode mode,
                                          const GizmoSnapSettings& settings, Handle<Object> target);
InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                          GizmoSpace space, f32 axisLength, f32 pickRadius,
                                          const GizmoSnapSettings& settings);

/// Non-mutating router predicate — same guards as `preflightGizmoRouter` (B6.4 deepen pass).
bool canGizmoRouter(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                    const GizmoSnapSettings& settings);
bool canGizmoRouter(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                    GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                    f32 pickRadius, const GizmoSnapSettings& settings);
/// Phase-routed snap diagnostics — no mutation (B6.4 deepen pass — snap guard).
struct SnapPhasePreflight {
    GizmoInteractionPhase phase = GizmoInteractionPhase::Idle;
    SnapPreflight snap{};

    bool snapDisabled() const { return snap.snapDisabled; }
    bool invalidStep() const { return snap.invalidStep; }
    bool isDegraded() const { return snap.isDegraded(); }
    bool canApply() const { return snap.canApply(); }

    /// Snap applies on begin when idle and begin path is allowed (B6.4 deepen pass).
    bool willApplyOnBegin() const {
        return phase == GizmoInteractionPhase::Idle && canApply();
    }

    /// Snap applies on update/end while dragging (B6.4 deepen pass).
    bool willApplyWhileDragging() const {
        return phase == GizmoInteractionPhase::Dragging && canApply();
};

SnapPhasePreflight preflightSnapPhase(GizmoMode mode, const GizmoSnapSettings& settings,
                                      GizmoInteractionPhase phase);

/// Non-mutating pick-snap predicate — same guards as `preflightPickSnap` (B6.4 deepen pass).
bool canPickSnap(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius,
bool canPickSnap(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings);

/// Non-mutating begin-interaction predicate — same guards as `preflightBeginInteraction` (B6.4 deepen pass).
bool canBeginInteraction(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                         const GizmoSnapSettings& settings, bool alreadyDragging = false);
bool canBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,

/// Non-mutating update-interaction predicate — same guards as `preflightUpdateInteraction` (B6.4 deepen pass).
bool canUpdateInteraction(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                          GizmoMode mode, const GizmoSnapSettings& settings);

/// Non-mutating end-interaction predicate — same guards as `preflightEndInteraction` (B6.4 deepen pass).
bool canEndInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

/// Non-mutating combined interaction predicate — mirrors `InteractionPreflight::canInteract` (B6.4 deepen pass).
bool canInteract(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                 const GizmoSnapSettings& settings);
bool canInteract(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                 GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                 f32 pickRadius, const GizmoSnapSettings& settings);

/// Non-mutating phase-routing predicates — same guards as `InteractionPreflight` (B6.4 deepen pass).
bool canActOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings);
bool canActOnPhase(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                   GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                   f32 pickRadius, const GizmoSnapSettings& settings);
bool canInteract(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                 const GizmoSnapSettings& settings);
bool canInteract(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                 GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                 f32 pickRadius, const GizmoSnapSettings& settings);

/// Non-mutating phase-routing predicate — same guards as `InteractionPreflight::canActOnPhase` (B6.4 deepen pass).
bool canActOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings);
bool canActOnPhase(const GizmoRay& ray, const GizmoTransform& transform, bool dragging,
                   GizmoAxis activeAxis, GizmoMode mode, GizmoSpace space, f32 axisLength,
                   f32 pickRadius, const GizmoSnapSettings& settings);

/// Non-mutating phase-routing predicates — same guards as `InteractionPreflight` (B6.4 deepen pass).
bool canActOnPhase(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                   const GizmoSnapSettings& settings);
bool canInteract(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                 const GizmoSnapSettings& settings);

/// Non-mutating lifecycle action predicate — same guards as `InteractionPreflight::canAct` (B6.4 deepen pass).
bool canActOnInteraction(const InteractionPreflight& preflight, GizmoInteractionAction action);

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      f32 pickRadius, bool alreadyDragging = false);
                                      f32 pickRadius, const GizmoSnapSettings& settings,
BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, const GizmoSnapSettings& settings,
                                      bool alreadyDragging = false);
BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      Handle<Object> target, bool alreadyDragging = false);
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      bool alreadyDragging = false);
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
/// Pick-axis preflight — no axis output when inputs are invalid (B6.4 deepen follow-up).
bool canPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
bool canPickAxis(const GizmoHitTest& hit, GizmoMode mode);

/// Begin-drag preflight — no side effects (B6.4 deepen follow-up).
bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode);
bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
/// Convenience inverse of `isRayEmpty` / `isHitTestEmpty` (B6.4 deepen — begin-drag guards).

/// Normalize ray direction; returns false when the ray is empty (B6.4 deepen — empty-ray guard).

/// Read-only begin-drag preflight — no drag state mutation (B6.4 deepen — begin-drag guards).
    bool emptyInput = false;

BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, const GizmoTransform& transform,
                                      GizmoMode mode);
                                      f32 pickRadius);
/// Read-only begin-drag diagnostics without mutating gizmo state (B6.4 deepen).
struct GizmoBeginDragPreflight {
    bool emptyViewport = false;
    bool axisMiss = false;
    bool snapInvalid = false;

    [[nodiscard]] bool canBegin() const {
        return !emptyRay && !emptyViewport && !axisMiss && !alreadyDragging;

GizmoBeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                           f32 pickRadius, bool alreadyDragging,
                                           const GizmoSnapSettings& snap);
GizmoBeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                           bool alreadyDragging, const GizmoSnapSettings& snap);

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging);
UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,
                                        const GizmoSnapSettings& settings);

/// Non-mutating update-drag predicate — same guards as `preflightUpdateDrag` (B6.4 deepen pass).
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging);
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode,

/// Read-only pick diagnostics — no mutation (B6.4 deepen follow-up).
struct PickPreflight {
    bool canPick = false;
    bool emptyRay = false;
    bool emptyHit = false;
    bool invalidPickConfig = false;
    bool screenMiss = false;
    GizmoAxis axis = GizmoAxis::None;
};

PickPreflight preflightPickAxis(const GizmoRay& ray, const GizmoTransform& transform,
                                GizmoMode mode, GizmoSpace space, f32 axisLength, f32 pickRadius);
PickPreflight preflightPickAxis(const GizmoHitTest& hit, GizmoMode mode);

/// Read-only snap diagnostics for drag/transform apply (B6.4 deepen follow-up).
struct SnapPreflight {
    bool canApply = false;
    bool snapDisabled = false;
    bool invalidStep = false;
    f32 step = 0.f;

SnapPreflight preflightSnap(GizmoMode mode, const GizmoSnapSettings& settings);

/// Read-only update-drag diagnostics — no mutation (B6.4 deepen follow-up).
struct UpdateDragPreflight {
    bool canUpdate = false;
    bool notDragging = false;
    bool noActiveAxis = false;
    bool snapSkipped = false;

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, GizmoMode mode, bool dragging,
                                        GizmoAxis activeAxis, const GizmoSnapSettings& snap);

/// Non-mutating update-drag predicate — same guards as `preflightUpdateDrag` (B6.4 deepen follow-up).
bool canUpdateDrag(const GizmoHitTest& hit, GizmoMode mode, bool dragging, GizmoAxis activeAxis);
                                      const GizmoSnapSettings& settings,

/// Component-wise transform equality for end-drag diagnostics (B6.4 deepen pass).
bool gizmoTransformEquals(const GizmoTransform& a, const GizmoTransform& b);
                                      const GizmoSnapSettings& settings, bool alreadyDragging = false);

/// Combined pick / snap / drag preflight for one interaction frame (B6.4 deepen follow-up).
struct GizmoInteractionPreflight {

/// Combined pick / snap / drag interaction diagnostics — no mutation (B6.4 deepen pass).
struct InteractionPreflight {
    PickPreflight pick{};
    SnapPreflight snap{};
    BeginDragPreflight begin{};
    UpdateDragPreflight update{};
    EndDragPreflight end{};

    bool canPick() const { return pick.canPick(); }
    bool canApplySnap() const { return snap.canApply(); }
    bool canBeginDrag() const { return begin.canBegin; }
    bool canUpdateDrag() const { return update.canUpdate(); }
    bool canEndDrag() const { return end.canEnd(); }

GizmoInteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                               f32 pickRadius, bool dragging, GizmoAxis activeAxis,
GizmoInteractionPreflight preflightInteraction(const GizmoHitTest& hit, GizmoMode mode,
                                               bool dragging, GizmoAxis activeAxis,
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging,


/// Combined update-drag + snap diagnostics for one drag frame (B6.4 deepen pass).
struct DragUpdateFramePreflight {
    UpdateDragPreflight update;
    SnapPreflight snap;

    bool canApply() const { return update.canUpdate(); }
    bool isSnapDegraded() const {
        return update.snapDegraded || (!snap.snapDisabled && snap.invalidStep);
    }

DragUpdateFramePreflight preflightDragUpdateFrame(const GizmoHitTest& hit, bool dragging,
                                                 GizmoAxis activeAxis, GizmoMode mode,
                                      bool alreadyDragging, const GizmoSnapSettings& settings);

/// Combined begin-drag interaction — pick + snap + drag guards (B6.4 deepen pass).
struct BeginInteractionPreflight {

    bool canBegin() const { return begin.canBegin; }
    bool snapDegraded() const { return begin.snapDegraded; }

BeginInteractionPreflight preflightBeginInteraction(const GizmoRay& ray,
                                                    const GizmoTransform& transform, GizmoMode mode,
                                                    GizmoSpace space, f32 axisLength,
BeginInteractionPreflight preflightBeginInteraction(const GizmoHitTest& hit, GizmoMode mode,
    bool canUpdate() const { return update.canUpdate(); }
    bool canEnd() const { return end.canEnd(); }
};

InteractionPreflight preflightInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                                          GizmoMode mode, GizmoSpace space, f32 axisLength,
                                          const GizmoSnapSettings& settings);
InteractionPreflight preflightInteraction(const GizmoHitTest& hit, GizmoMode mode, bool dragging,
                                          GizmoAxis activeAxis, const GizmoSnapSettings& settings);
                                      bool alreadyDragging = false);
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      const GizmoSnapSettings& settings, Handle<Object> target,

/// Pick axis with empty-hit guards — returns false when pick misses (B6.4 deepen follow-up).
bool tryPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius, GizmoAxis& outAxis);
bool tryPickAxis(const GizmoHitTest& hit, GizmoMode mode, GizmoAxis& outAxis);

/// Non-mutating pick predicate — same guards as `tryPickAxis` (B6.4 deepen pass).
bool canPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                 GizmoSpace space, f32 axisLength, f32 pickRadius);
bool canPickAxis(const GizmoHitTest& hit, GizmoMode mode);

/// Non-mutating begin-drag predicate — empty-hit / miss early-outs (B6.4 deepen pass).
bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                  GizmoSpace space, f32 axisLength, f32 pickRadius);
bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                  GizmoSpace space, f32 axisLength, f32 pickRadius,
                  const GizmoSnapSettings& settings);
bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode);
bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode, const GizmoSnapSettings& settings);

/// Why pick preflight rejected the request (B6.4 deepen pass).
enum class GizmoPickRejectReason : u8 {
    None = 0,
    NonFiniteRay,
    NonFiniteHit,
    EmptyRay,
    NonFiniteHit,
    EmptyHit,
    NonFiniteRay,
    NonFiniteHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
};

/// Why snap preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapRejectReason : u8 {
    SnapDisabled,
    NonFiniteStep,
    InvalidStep,

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why begin-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoBeginDragRejectReason : u8 {
    None = 0,
    NonFiniteRay,
    NonFiniteHit,
    EmptyRay,
    NonFiniteHit,
    EmptyHit,
    NonFiniteRay,
    NonFiniteHit,
    InvalidPickConfig,
    InvalidDimensions,
    OutOfBounds,
    ScreenMiss,
    PickMiss,
    AlreadyDragging,

/// Why update-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoUpdateDragRejectReason : u8 {
    NotDragging,
    NonFiniteHit,
    EmptyHit,
    InvalidDimensions,
    OutOfBounds,
    InvalidActiveAxis,

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    DeltaNonFinite,
};

    None = 0,
    SnapDisabled,
    NonFiniteStep,
    InvalidStep,



};

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why end-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoEndDragRejectReason : u8 {

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

/// Why snap-drag preflight rejected the request (B6.4 deepen pass).
enum class GizmoSnapDragRejectReason : u8 {
    None = 0,
    DeltaNonFinite,
    SnapDisabled,
    InvalidStep,
};

const char* gizmoPickRejectReasonLabel(GizmoPickRejectReason reason);
const char* gizmoSnapRejectReasonLabel(GizmoSnapRejectReason reason);
const char* gizmoSnapDragRejectReasonLabel(GizmoSnapDragRejectReason reason);
const char* gizmoBeginDragRejectReasonLabel(GizmoBeginDragRejectReason reason);
const char* gizmoUpdateDragRejectReasonLabel(GizmoUpdateDragRejectReason reason);
const char* gizmoEndDragRejectReasonLabel(GizmoEndDragRejectReason reason);
const char* gizmoSnapDragRejectReasonLabel(GizmoSnapDragRejectReason reason);

GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight);
GizmoSnapRejectReason classifySnapReject(const SnapPreflight& preflight);
GizmoSnapDragRejectReason classifySnapDragReject(const SnapDragPreflight& preflight);
GizmoBeginDragRejectReason classifyBeginDragReject(const BeginDragPreflight& preflight);
GizmoUpdateDragRejectReason classifyUpdateDragReject(const UpdateDragPreflight& preflight);
GizmoEndDragRejectReason classifyEndDragReject(const EndDragPreflight& preflight);
GizmoSnapDragRejectReason classifySnapDragReject(const SnapDragPreflight& preflight);

/// Pick preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightPickReady(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                        GizmoSpace space, f32 axisLength, f32 pickRadius,
                        GizmoPickRejectReason* reason = nullptr);
bool preflightPickReady(const GizmoHitTest& hit, GizmoMode mode,
bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                      GizmoPickRejectReason& reason);
bool tryPreflightPick(const GizmoHitTest& hit, GizmoMode mode, GizmoPickRejectReason& reason);
bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                    GizmoSpace space, f32 axisLength, f32 pickRadius);
bool shouldSkipPick(const GizmoHitTest& hit, GizmoMode mode);

/// Snap preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightSnapReady(GizmoMode mode, const GizmoSnapSettings& settings,
                        GizmoSnapRejectReason* reason = nullptr);
bool tryPreflightSnap(GizmoMode mode, const GizmoSnapSettings& settings,
                      GizmoSnapRejectReason& reason);
bool shouldSkipSnap(GizmoMode mode, const GizmoSnapSettings& settings);

/// Snap-drag preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightSnapDragReady(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings,
                            GizmoSnapDragRejectReason* reason = nullptr);
bool tryPreflightSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings,
                          GizmoSnapDragRejectReason& reason);
bool shouldSkipSnapDrag(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);
/// Mode-only snap-drag skip — same snap guards without a delta (B6.4 deepen follow-up).
bool shouldSkipSnapDrag(GizmoMode mode, const GizmoSnapSettings& settings);

/// Pick-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
bool preflightPickInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                   GizmoMode mode, GizmoSpace space, f32 axisLength,
                                   f32 pickRadius, const GizmoSnapSettings& settings,
bool preflightPickInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
                                   const GizmoSnapSettings& settings,
bool shouldSkipPickInteraction(const GizmoRay& ray, const GizmoTransform& transform,
                               GizmoMode mode, GizmoSpace space, f32 axisLength, f32 pickRadius,
                               const GizmoSnapSettings& settings);
bool shouldSkipPickInteraction(const GizmoHitTest& hit, GizmoMode mode,

/// Begin-drag-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
bool preflightBeginDragInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoBeginDragRejectReason* reason = nullptr,
                                        bool alreadyDragging = false);
bool preflightBeginDragInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
bool shouldSkipBeginDragInteraction(const GizmoRay& ray, const GizmoTransform& transform,
bool shouldSkipBeginDragInteraction(const GizmoHitTest& hit, GizmoMode mode,

/// Update-drag-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
bool preflightUpdateDragInteractionReady(const GizmoHitTest& hit, bool dragging,
                                         GizmoAxis activeAxis, GizmoMode mode,
                                         GizmoUpdateDragRejectReason* reason = nullptr);
                                         const GizmoSnapSettings& settings, f32 delta,
                                         GizmoUpdateDragRejectReason* reason = nullptr,
                                         GizmoSnapDragRejectReason* snapDragReason = nullptr);
bool shouldSkipUpdateDragInteraction(const GizmoHitTest& hit, bool dragging,

/// End-drag-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
bool preflightEndDragInteractionReady(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                      GizmoEndDragRejectReason* reason = nullptr);
bool shouldSkipEndDragInteraction(bool dragging, GizmoAxis activeAxis, GizmoMode mode,

/// Begin-drag preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightBeginDragReady(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
bool preflightBeginDragReady(const GizmoHitTest& hit, GizmoMode mode,
bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                           GizmoBeginDragRejectReason& reason, bool alreadyDragging = false);
bool tryPreflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
bool shouldSkipBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging = false);

/// Update-drag preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightUpdateDragReady(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
bool tryPreflightUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis,
                            GizmoUpdateDragRejectReason& reason);
bool shouldSkipUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoAxis activeAxis);

/// End-drag preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightEndDragReady(bool dragging, GizmoEndDragRejectReason* reason = nullptr);
bool tryPreflightEndDrag(bool dragging, GizmoEndDragRejectReason& reason);
bool shouldSkipEndDrag(bool dragging);

/// Non-mutating drag-update predicate — empty viewport / inactive drag early-outs (B6.4 deepen pass).
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging);

/// Non-mutating update-drag predicate — same guards as `preflightUpdateDrag` (B6.4 deepen pass).
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging, GizmoMode mode);

/// Non-mutating end-drag predicate — rejects when no drag is active (B6.4 deepen pass).
bool canEndDrag(bool dragging);


bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode,
                  GizmoSpace space, f32 axisLength, f32 pickRadius, bool alreadyDragging);
bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode, bool alreadyDragging);
/// Pick-interaction preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightPickInteractionReady(const GizmoRay& ray, const GizmoTransform& transform,
                                   GizmoMode mode, GizmoSpace space, f32 axisLength,
                                   f32 pickRadius, const GizmoSnapSettings& settings,
                                   GizmoPickRejectReason* reason = nullptr);
bool preflightPickInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
                                   const GizmoSnapSettings& settings,

/// Begin-drag interaction preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightBeginDragInteractionReady(
    const GizmoRay& ray, const GizmoTransform& transform, GizmoMode mode, GizmoSpace space,
    f32 axisLength, f32 pickRadius, const GizmoSnapSettings& settings,
    GizmoBeginDragRejectReason* reason = nullptr, bool alreadyDragging = false);
bool preflightBeginDragInteractionReady(const GizmoHitTest& hit, GizmoMode mode,
                                        GizmoBeginDragRejectReason* reason = nullptr,
                                        bool alreadyDragging = false);

/// Update-drag interaction preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightUpdateDragInteractionReady(const GizmoHitTest& hit, bool dragging,
                                         GizmoAxis activeAxis, GizmoMode mode,
                                         GizmoUpdateDragRejectReason* reason = nullptr);

/// End-drag interaction preflight with optional reject-reason output (B6.4 deepen pass).
bool preflightEndDragInteractionReady(bool dragging, GizmoAxis activeAxis, GizmoMode mode,
                                      GizmoEndDragRejectReason* reason = nullptr);

/// Screen-space dead-zone check before axis pick (B6.4 deepen).
bool isScreenHitMiss(const GizmoHitTest& hit, GizmoMode mode);

/// True when screen coordinates fall outside the viewport bounds (B6.4 deepen follow-up pass).
bool isScreenHitOutOfBounds(const GizmoHitTest& hit);
GizmoTransform snapTransform(const GizmoTransform& transform, GizmoMode mode,
                             const GizmoSnapSettings& settings);

math::Vec3 gizmoPosition(const GizmoTransform& transform);
math::Quat gizmoRotation(const GizmoTransform& transform);
math::Vec3 gizmoScale(const GizmoTransform& transform);
GizmoTransform gizmoFromMath(const math::Vec3& position, const math::Quat& rotation,
                             const math::Vec3& scale);

GizmoTransform applyTranslateDelta(const GizmoTransform& base, GizmoAxis axis,
                                   const math::Vec3& delta, GizmoSpace space);
GizmoTransform applyRotateDelta(const GizmoTransform& base, GizmoAxis axis, f32 deltaRadians,
                                GizmoSpace space);
GizmoTransform applyScaleDelta(const GizmoTransform& base, GizmoAxis axis,
                               const math::Vec3& delta, GizmoSpace space);

/// Headless in-viewport gizmo API (B6.4).
class GizmoSystem {
public:
    static constexpr f32 kScreenSize = 120.f;
    static constexpr f32 kLineWidth = 2.5f;
    static constexpr f32 kArrowSize = 0.2f;
    static constexpr f32 kAxisLength = 1.f;
    static constexpr f32 kPickRadius = 0.08f;

    /// Returns true when the mode changed; cancels an active drag on change.
    bool setMode(GizmoMode mode);
    GizmoMode mode() const { return m_mode; }
    void cycleMode();

    void setSpace(GizmoSpace space) { m_space = space; }
    GizmoSpace space() const { return m_space; }

    void setSnapSettings(const GizmoSnapSettings& settings) { m_snap = settings; }
    const GizmoSnapSettings& snapSettings() const { return m_snap; }

    void setTarget(Handle<Object> target) { m_target = target; }
    Handle<Object> target() const { return m_target; }

    void setCommandStack(CommandStack* stack) { m_commandStack = stack; }
    void setEditorState(EditorState* state) { m_editorState = state; }

    [[nodiscard]] bool transformDirty() const { return m_transformDirty; }
    void clearTransformDirty() { m_transformDirty = false; }

    GizmoAxis pickAxis(const GizmoRay& ray, const GizmoTransform& transform) const;
    GizmoAxis pickAxis(const GizmoHitTest& hit) const;
    bool tryPickAxis(const GizmoRay& ray, const GizmoTransform& transform, GizmoAxis& outAxis) const;
    bool tryPickAxis(const GizmoHitTest& hit, GizmoAxis& outAxis) const;
    [[nodiscard]] bool canPickAxis(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool canPickAxis(const GizmoHitTest& hit) const;
    /// Read-only pick diagnostics — same guards as `canPickAxis` (B6.4 deepen follow-up).
    [[nodiscard]] PickPreflight preflightPick(const GizmoHitTest& hit) const;
    [[nodiscard]] PickPreflight preflightPick(const GizmoRay& ray,
                                              const GizmoTransform& transform) const;
    /// Read-only snap diagnostics for the active mode (B6.4 deepen follow-up).
    [[nodiscard]] SnapPreflight preflightSnap() const;

    GizmoResult beginDrag(const GizmoHitTest& hit, const GizmoTransform& current);
    GizmoResult beginDrag(const GizmoRay& ray, const GizmoTransform& current);
    /// Non-mutating begin-drag predicate — rejects empty hits and active drags (B6.4 deepen pass).
    [[nodiscard]] bool canBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform) const;
    /// Read-only begin-drag diagnostics — same guards as `canBeginDrag` (B6.4 deepen pass).
    [[nodiscard]] BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] BeginDragPreflight preflightBeginDrag(const GizmoRay& ray,
                                                         const GizmoTransform& transform) const;
    [[nodiscard]] GizmoBeginDragRejectReason classifyBeginDragReject(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoBeginDragRejectReason classifyBeginDragReject(
        const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoRay& ray,
                                           const GizmoTransform& transform) const;
    [[nodiscard]] BeginDragInteractionPreflight preflightBeginDragInteraction(
        const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoInteractionPreflight preflightInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoInteractionPreflight preflightInteraction(
    [[nodiscard]] BeginInteractionPreflight preflightBeginInteraction(
        const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] PickPreflight preflightPick(const GizmoRay& ray,
                                              const GizmoTransform& transform) const;
    [[nodiscard]] PickPreflight preflightPick(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoPickRejectReason classifyPickReject(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoPickRejectReason classifyPickReject(const GizmoRay& ray,
                                                           const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipPick(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipPick(const GizmoRay& ray,
                                      const GizmoTransform& transform) const;
    [[nodiscard]] PickInteractionPreflight preflightPickInteraction(
    [[nodiscard]] InteractionPreflight preflightInteraction(const GizmoRay& ray,
                                                             const GizmoTransform& transform) const;
    [[nodiscard]] InteractionPreflight preflightInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] PickSnapPreflight preflightPickSnap(const GizmoRay& ray,
    [[nodiscard]] PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit) const;
    [[nodiscard]] SnapPreflight preflightSnap() const;
    [[nodiscard]] SnapDragPreflight preflightSnapDrag(f32 delta) const;
    [[nodiscard]] SnapPreflight preflightSnap(const GizmoTransform& transform) const;
    [[nodiscard]] SnapDragPreflight preflightSnapDrag() const;
    [[nodiscard]] bool canSnapDragNow() const;
    [[nodiscard]] SnapDragPreflight preflightSnapDragDelta() const;
    [[nodiscard]] GizmoSnapRejectReason classifySnapReject() const;
    [[nodiscard]] bool shouldSkipSnap() const;
    [[nodiscard]] GizmoInteractionPhase interactionPhase() const;
    [[nodiscard]] bool canActOnPhase(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canActOnPhase(const GizmoRay& ray,
                                     const GizmoTransform& transform) const;
    [[nodiscard]] SnapDragDeltaPreflight preflightSnapDragDelta() const;
    [[nodiscard]] bool canApplySnapNow() const;
    [[nodiscard]] bool canSnapDragDeltaNow() const;
    [[nodiscard]] SnapDeltaPreflight preflightSnapDelta(f32 delta) const;
    [[nodiscard]] bool canSnapDelta(f32 delta) const;
    /// Pick preflight with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoPickRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightPick(const GizmoHitTest& hit,
    /// Snap preflight with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightSnap(GizmoSnapRejectReason& reason) const;
    /// Begin-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoHitTest& hit,
                                             GizmoBeginDragRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
    /// Update-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightUpdateDrag(const GizmoHitTest& hit,
                                              GizmoUpdateDragRejectReason& reason) const;
    /// End-drag preflight with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightEndDrag(GizmoEndDragRejectReason& reason) const;
    /// Early-out when pick/snap/drag preflights would reject (B6.4 deepen pass).
    [[nodiscard]] bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipPick(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipSnap() const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoRay& ray,
                                           const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipUpdateDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipEndDrag() const;
    [[nodiscard]] bool trySnapTransform(const GizmoTransform& transform,
                                        GizmoTransform& out) const;
    [[nodiscard]] f32 trySnapDragDelta(f32 delta) const;
    [[nodiscard]] UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoUpdateDragRejectReason classifyUpdateDragReject(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipUpdateDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] UpdateDragInteractionPreflight preflightUpdateDragInteraction(
        const GizmoHitTest& hit, f32 delta) const;
    [[nodiscard]] DragUpdateFramePreflight preflightDragUpdateFrame(const GizmoHitTest& hit) const;
    [[nodiscard]] UpdateInteractionPreflight preflightUpdateInteraction(
        const GizmoHitTest& hit) const;
        const GizmoHitTest& hit, f32 delta = 0.f) const;
    [[nodiscard]] bool canUpdateDrag(const GizmoHitTest& hit) const;
    /// Combined pick + snap diagnostics (B6.4 deepen pass).
    [[nodiscard]] PickSnapPreflight preflightPickSnap(const GizmoHitTest& hit) const;
    [[nodiscard]] PickSnapPreflight preflightPickSnap(const GizmoRay& ray,
    /// Combined begin-drag + snap diagnostics (B6.4 deepen pass).
    [[nodiscard]] BeginInteractionPreflight preflightBeginInteraction(
                                                       const GizmoTransform& transform) const;
    /// Non-mutating pick-snap predicate — same guards as `preflightPickSnap` (B6.4 deepen pass).
    [[nodiscard]] bool canPickSnap(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canPickSnap(const GizmoRay& ray, const GizmoTransform& transform) const;
        const GizmoHitTest& hit) const;
        const GizmoRay& ray, const GizmoTransform& transform) const;
    /// Non-mutating begin-interaction predicate — same guards as `preflightBeginInteraction` (B6.4 deepen pass).
    [[nodiscard]] bool canBeginInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canBeginInteraction(const GizmoRay& ray,
    /// Combined update-drag + snap diagnostics (B6.4 deepen pass).
    [[nodiscard]] UpdateInteractionPreflight preflightUpdateInteraction(
                                                       const GizmoTransform& transform) const;
        const GizmoHitTest& hit) const;
        const GizmoRay& ray, const GizmoTransform& transform) const;
    /// Non-mutating update-interaction predicate — same guards as `preflightUpdateInteraction` (B6.4 deepen pass).
    [[nodiscard]] bool canUpdateInteraction(const GizmoHitTest& hit) const;
    /// Combined end-drag + snap diagnostics (B6.4 deepen pass).
    [[nodiscard]] EndInteractionPreflight preflightEndInteraction() const;
    /// Non-mutating end-interaction predicate — same guards as `preflightEndInteraction` (B6.4 deepen pass).
    [[nodiscard]] bool canEndInteraction() const;
    /// Combined interaction diagnostics for the active drag phase (B6.4 deepen pass).
    [[nodiscard]] InteractionPreflight preflightInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] InteractionPreflight preflightInteraction(
        const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] HitTestPreflight preflightHitTest(const GizmoHitTest& hit) const;
    [[nodiscard]] RayPreflight preflightRay(const GizmoRay& ray) const;
    [[nodiscard]] bool canUseHitTest(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canUseRay(const GizmoRay& ray) const;
    [[nodiscard]] bool canActOnPhase(const GizmoHitTest& hit) const;
    [[nodiscard]] DragLifecycleSnapPreflight preflightDragLifecycleSnap() const;
    /// Non-mutating interaction predicates — same guards as combined preflights (B6.4 deepen pass).
    [[nodiscard]] bool canPickSnap(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canPickSnap(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool canBeginInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canBeginInteraction(const GizmoRay& ray,
    [[nodiscard]] bool canUpdateInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canEndInteraction() const;
    [[nodiscard]] PickPreflight preflightPickAxis(const GizmoRay& ray,
    [[nodiscard]] PickPreflight preflightPickAxis(const GizmoHitTest& hit) const;
    /// Non-mutating update-drag predicate — same guards as `preflightUpdateDrag` (B6.4 deepen pass).
    /// Guarded update-drag — returns false when preflight rejects (B6.4 deepen pass).
    bool tryUpdateDrag(const GizmoHitTest& hit, GizmoResult& out);
    /// Read-only end-drag diagnostics — same guard as `endDrag` early-out (B6.4 deepen pass).
    [[nodiscard]] EndDragPreflight preflightEndDrag() const;
    /// Non-mutating end-drag predicate — rejects when no drag is active (B6.4 deepen pass).
    [[nodiscard]] bool canEndDrag() const;
    /// Guarded update-drag — returns false when drag is inactive or hit is empty (B6.4 deepen pass).
    /// Read-only update-drag diagnostics with snap-degraded flag (B6.4 deepen follow-up pass).
    [[nodiscard]] UpdateDragPreflight preflightUpdateDragWithSnap(const GizmoHitTest& hit) const;
    /// Combined pick/snap/drag frame diagnostics for the current drag state (B6.4 deepen pass).
    [[nodiscard]] DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoPickRejectReason classifyPickReject(const PickPreflight& preflight) const;
    [[nodiscard]] GizmoBeginDragRejectReason classifyBeginDragReject(
        const BeginDragPreflight& preflight) const;
    [[nodiscard]] GizmoUpdateDragRejectReason classifyUpdateDragReject(
        const UpdateDragPreflight& preflight) const;
    [[nodiscard]] GizmoEndDragRejectReason classifyEndDragReject(
        const EndDragPreflight& preflight) const;
    [[nodiscard]] bool canSnapDragDelta() const;
    [[nodiscard]] f32 trySnapDragDelta(f32 delta) const;
    /// Combined pick / snap / drag interaction diagnostics (B6.4 deepen pass).
    [[nodiscard]] InteractionPreflight preflightInteraction(const GizmoRay& ray,
    [[nodiscard]] bool canUpdateDrag(const GizmoHitTest& hit, GizmoMode mode,
                                     const GizmoSnapSettings& settings) const;
        const GizmoRay& ray, const GizmoTransform& transform) const;
    /// Non-mutating begin-interaction predicate — same guards as `preflightBeginInteraction` (B6.4 deepen pass).
                                          const GizmoTransform& transform) const;
    /// Non-mutating update-interaction predicate — same guards as `preflightUpdateInteraction` (B6.4 deepen pass).
    /// Non-mutating end-interaction predicate — same guards as `preflightEndInteraction` (B6.4 deepen pass).
    /// Primary action allowed for the active lifecycle phase (B6.4 deepen pass).
    [[nodiscard]] bool canActOnPhase(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canActOnPhase(const GizmoRay& ray, const GizmoTransform& transform) const;
    /// Phase-routed action predicates — same guards as `InteractionPreflight` (B6.4 deepen pass).
    [[nodiscard]] bool canInteractOnPhase(const GizmoHitTest& hit) const;
    [[nodiscard]] bool snapDegradedOnPhase() const;
    [[nodiscard]] bool snapWillApplyOnPhase() const;
    [[nodiscard]] PhaseActionPreflight preflightPhaseAction(const GizmoHitTest& hit) const;
    /// Phase-routed gizmo diagnostics — bundles all interaction preflight layers (B6.4 deepen pass).
    [[nodiscard]] GizmoPreflightRouter preflightRouter(const GizmoHitTest& hit) const;
    [[nodiscard]] GizmoPreflightRouter preflightRouter(const GizmoRay& ray,
    [[nodiscard]] bool canRouter(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canRouter(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool canActOrEndOnPhase(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canInteract(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canInteract(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] SnapPhasePreflight preflightSnapPhase() const;
    /// Non-mutating phase-routing predicates — same guards as `preflightInteraction` (B6.4 deepen pass).
    /// Phase-routed snap diagnostics (B6.4 deepen pass).
    [[nodiscard]] SnapInteractionPreflight preflightSnapInteraction() const;
    [[nodiscard]] bool canApplySnapOnPhase() const;
    [[nodiscard]] bool isSnapDegradedOnPhase() const;
    /// Current drag lifecycle phase (B6.4 deepen pass).
    [[nodiscard]] GizmoInteractionPhase interactionPhase() const;
    [[nodiscard]] bool canActOnPhase(const GizmoRay& ray,
    /// Phase-routing predicates — same guards as `preflightInteraction` (B6.4 deepen pass).
    /// Phase-aware primary action predicate — same guards as `preflightInteraction` (B6.4 deepen pass).
    /// Read-only mode-change diagnostics — same guards as `setMode` / `cycleMode` (B6.4 deepen pass).
    [[nodiscard]] ModeChangePreflight preflightModeChange(GizmoMode mode) const;
    [[nodiscard]] ModeChangePreflight preflightCycleMode() const;
    [[nodiscard]] bool canChangeMode(GizmoMode mode) const;
    [[nodiscard]] bool canCycleMode() const;
    /// Guarded begin-drag — returns false on empty viewport / miss picks (B6.4 deepen follow-up).
    bool tryBeginDrag(const GizmoHitTest& hit, const GizmoTransform& current, GizmoResult& out);
    bool tryBeginDrag(const GizmoRay& ray, const GizmoTransform& current, GizmoResult& out);
    /// Pick/snap/drag preflights with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoPickGuardRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightPick(const GizmoHitTest& hit,
                                        GizmoPickGuardRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightSnap(GizmoSnapGuardRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoHitTest& hit,
                                             GizmoBeginDragGuardRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                             GizmoBeginDragGuardRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightUpdateDrag(const GizmoHitTest& hit,
                                              GizmoUpdateDragGuardRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightEndDrag(GizmoEndDragGuardRejectReason& reason) const;
    /// Guarded drag update — returns false when preflight rejects the hit (B6.4 deepen follow-up).
    /// Read-only begin-drag preflight using current mode/space (B6.4 deepen — begin-drag guards).
    BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit,
                                          const GizmoTransform& current) const;
    BeginDragPreflight preflightBeginDrag(const GizmoRay& ray,
    /// Guarded drag update — returns false on empty viewport (B6.4 deepen — begin-drag guards).
    /// Read-only begin-drag preflight without mutating drag state (B6.4 deepen).
    GizmoBeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit,
    GizmoBeginDragPreflight preflightBeginDrag(const GizmoRay& ray,
    /// Read-only drag-update diagnostics — same guards as `canUpdateDrag` (B6.4 deepen pass).
    [[nodiscard]] UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit) const;
    /// Guarded drag update — returns false on empty viewport or inactive drag (B6.4 deepen pass).
    /// Non-mutating update-drag predicate — rejects inactive drags and empty hits (B6.4 deepen pass).
    /// Guarded drag update — returns false when preflight rejects (B6.4 deepen pass).
    /// Non-mutating update-drag predicate — rejects inactive drags and empty hits (B6.4 deepen follow-up).
    [[nodiscard]] bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipPick(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipSnap() const;
    [[nodiscard]] bool shouldSkipUpdateDrag(const GizmoHitTest& hit) const;
    /// Guarded update-drag — returns false when preflight rejects (B6.4 deepen follow-up).
    GizmoResult updateDrag(const GizmoHitTest& hit);
    /// Read-only end-drag diagnostics — same guards as `canEndDrag` (B6.4 deepen pass).
    [[nodiscard]] EndDragPreflight preflightEndDrag() const;
    [[nodiscard]] GizmoEndDragRejectReason classifyEndDragReject() const;
    [[nodiscard]] bool shouldSkipEndDrag() const;
    [[nodiscard]] EndDragInteractionPreflight preflightEndDragInteraction() const;
    [[nodiscard]] DragInteractionPreflight preflightDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] EndDragPreflight preflightEndDrag() const;
    [[nodiscard]] EndInteractionPreflight preflightEndInteraction() const;
    /// Preflight with mandatory reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool tryPreflightPick(const GizmoHitTest& hit, PickPreflight& out,
                                        GizmoPickRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform,
                                        PickPreflight& out, GizmoPickRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightSnap(SnapPreflight& out, GizmoSnapRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoHitTest& hit, BeginDragPreflight& out,
                                             GizmoBeginDragRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                             BeginDragPreflight& out,
    [[nodiscard]] bool tryPreflightUpdateDrag(const GizmoHitTest& hit, UpdateDragPreflight& out,
                                              GizmoUpdateDragRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightEndDrag(EndDragPreflight& out,
                                           GizmoEndDragRejectReason& reason) const;
    [[nodiscard]] DragSessionPreflight preflightDragSession(const GizmoHitTest& hit) const;
    [[nodiscard]] DragSessionPreflight preflightDragSession(const GizmoRay& ray,
                                                            const GizmoTransform& transform) const;
    /// Non-mutating end-drag predicate — rejects inactive drags (B6.4 deepen pass).
    [[nodiscard]] bool canPickInteraction(const GizmoRay& ray,
                                          const GizmoTransform& transform) const;
    [[nodiscard]] bool canPickInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canBeginDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canBeginDragInteraction(const GizmoRay& ray,
    [[nodiscard]] bool canUpdateDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canEndDragInteraction() const;
    [[nodiscard]] bool canDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canEndDrag() const;
    [[nodiscard]] bool canEndDrag(GizmoMode mode, const GizmoSnapSettings& settings) const;
    /// Early-out when pick / begin / update / end preflight would reject (B6.4 deepen pass).
    /// Early-out predicates — same guards as preflights (B6.4 deepen pass).
    [[nodiscard]] bool shouldSkipPick(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipPick(const GizmoRay& ray, const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoRay& ray,
                                            const GizmoTransform& transform) const;
    [[nodiscard]] bool shouldSkipUpdateDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipEndDrag() const;
    /// Non-mutating skip predicates — inverse of interaction preflights (B6.4 deepen pass).
    [[nodiscard]] bool shouldSkipSnap() const;
    [[nodiscard]] bool shouldSkipPickInteraction(const GizmoRay& ray,
    [[nodiscard]] bool shouldSkipPickInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDragInteraction(const GizmoRay& ray,
    [[nodiscard]] bool shouldSkipUpdateDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipEndDragInteraction() const;
    /// True when pick preflight would reject (B6.4 deepen pass).
    /// True when begin-drag preflight would reject (B6.4 deepen pass).
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoRay& ray, const GizmoTransform& transform) const;
    /// True when update-drag preflight would reject (B6.4 deepen pass).
    /// True when end-drag preflight would reject (B6.4 deepen pass).
    /// True when combined interaction preflight blocks the active phase action (B6.4 deepen pass).
    [[nodiscard]] bool shouldSkipInteraction(const GizmoHitTest& hit) const;
    /// Primary action allowed for the active lifecycle phase (B6.4 deepen pass).
    [[nodiscard]] bool canActOnPhase(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canDragSession(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canDragSession(const GizmoRay& ray,
    [[nodiscard]] bool canActOnDragInteraction(const GizmoHitTest& hit) const;
    /// Guarded end-drag — returns false when preflight rejects (B6.4 deepen pass).
    bool tryEndDrag(GizmoResult& out);
    /// Guarded update-drag — returns false when preflight rejects the hit (B6.4 deepen follow-up).
    /// Non-mutating update-drag predicate — rejects inactive drag and empty viewport (B6.4 deepen follow-up).
    /// Read-only update-drag diagnostics — same guards as `canUpdateDrag` (B6.4 deepen follow-up).
    /// Guarded end-drag — returns false when no drag is active (B6.4 deepen pass).
    /// Guarded end-drag — returns false when drag is inactive (B6.4 deepen pass).
    /// Guarded update-drag — returns false when drag inactive or hit empty (B6.4 deepen pass).
    /// Guarded end-drag — returns false when drag inactive (B6.4 deepen pass).
    /// Guarded update-drag — returns false on empty viewport / inactive drag (B6.4 deepen pass).
    [[nodiscard]] bool canEndDrag() const;
    [[nodiscard]] EndDragPreflight preflightEndDrag() const;
    /// Guarded end-drag — returns false when preflight rejects inactive drag (B6.4 deepen pass).
    /// Guarded end-drag — returns false when preflight rejects inactive drag (B6.4 deepen follow-up).
    /// Guarded end-drag — returns false when preflight rejects the commit (B6.4 deepen pass).
    GizmoResult endDrag();
    /// Cancel an active drag without committing transform changes (B6.4 deepen follow-up).
    void cancelDrag();

    /// Pick preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightPickReady(const GizmoRay& ray, const GizmoTransform& transform,
                                          GizmoPickRejectReason* reason = nullptr) const;
    [[nodiscard]] bool preflightPickReady(const GizmoHitTest& hit,
    [[nodiscard]] bool tryPreflightPick(const GizmoRay& ray, const GizmoTransform& transform,
                                        GizmoPickRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightPick(const GizmoHitTest& hit,
    [[nodiscard]] bool shouldSkipPick(const GizmoRay& ray,
    [[nodiscard]] bool shouldSkipPick(const GizmoHitTest& hit) const;

    /// Snap preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightSnapReady(GizmoSnapRejectReason* reason = nullptr) const;
    [[nodiscard]] bool tryPreflightSnap(GizmoSnapRejectReason& reason) const;
    [[nodiscard]] bool shouldSkipSnap() const;

    /// Snap-drag preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightSnapDragReady(f32 delta,
                                              GizmoSnapDragRejectReason* reason = nullptr) const;
    [[nodiscard]] bool tryPreflightSnapDrag(f32 delta, GizmoSnapDragRejectReason& reason) const;
    [[nodiscard]] bool shouldSkipSnapDrag(f32 delta) const;
    [[nodiscard]] bool shouldSkipSnapDrag() const;

    /// Pick-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
    [[nodiscard]] bool preflightPickInteractionReady(
        const GizmoRay& ray, const GizmoTransform& transform,
    [[nodiscard]] bool preflightPickInteractionReady(const GizmoHitTest& hit,
    [[nodiscard]] bool shouldSkipPickInteraction(const GizmoRay& ray,
    [[nodiscard]] bool shouldSkipPickInteraction(const GizmoHitTest& hit) const;

    /// Begin-drag-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
    [[nodiscard]] bool preflightBeginDragInteractionReady(
        const GizmoHitTest& hit, GizmoBeginDragRejectReason* reason = nullptr) const;
        GizmoBeginDragRejectReason* reason = nullptr) const;
    [[nodiscard]] bool shouldSkipBeginDragInteraction(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDragInteraction(const GizmoRay& ray,

    /// Update-drag-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
    [[nodiscard]] bool preflightUpdateDragInteractionReady(
        const GizmoHitTest& hit, GizmoUpdateDragRejectReason* reason = nullptr) const;
        const GizmoHitTest& hit, f32 delta, GizmoUpdateDragRejectReason* reason = nullptr,
        GizmoSnapDragRejectReason* snapDragReason = nullptr) const;
    [[nodiscard]] bool shouldSkipUpdateDragInteraction(const GizmoHitTest& hit) const;

    /// End-drag-interaction preflight with optional reject-reason output (B6.4 deepen follow-up).
    [[nodiscard]] bool preflightEndDragInteractionReady(
        GizmoEndDragRejectReason* reason = nullptr) const;
    [[nodiscard]] bool shouldSkipEndDragInteraction() const;

    /// Begin-drag preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightBeginDragReady(const GizmoHitTest& hit,
    [[nodiscard]] bool preflightBeginDragReady(const GizmoRay& ray,
                                               const GizmoTransform& transform,
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoHitTest& hit,
                                             GizmoBeginDragRejectReason& reason) const;
    [[nodiscard]] bool tryPreflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool shouldSkipBeginDrag(const GizmoRay& ray,

    /// Update-drag preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightUpdateDragReady(const GizmoHitTest& hit,
                                                GizmoUpdateDragRejectReason* reason = nullptr) const;
    [[nodiscard]] bool tryPreflightUpdateDrag(const GizmoHitTest& hit,
                                              GizmoUpdateDragRejectReason& reason) const;
    [[nodiscard]] bool shouldSkipUpdateDrag(const GizmoHitTest& hit) const;

    /// End-drag preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightEndDragReady(GizmoEndDragRejectReason* reason = nullptr) const;
    [[nodiscard]] bool tryPreflightEndDrag(GizmoEndDragRejectReason& reason) const;
    [[nodiscard]] bool shouldSkipEndDrag() const;
    /// Cancel an active drag without dirty marking (B6.4 deepen — begin-drag guards).
    /// Cancel an active drag without dirty marking (B6.4 deepen pass).
    /// Guarded drag update — returns false on empty viewport / screen miss (B6.4 deepen follow-up).
    /// Cancel an active drag without dirty marking (B6.4 deepen follow-up).
    /// Guarded end-drag — returns false when no drag is active (B6.4 deepen follow-up).
    bool tryEndDrag(GizmoResult& out);

    /// Pick-interaction preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightPickInteractionReady(
        const GizmoRay& ray, const GizmoTransform& transform,
        GizmoPickRejectReason* reason = nullptr) const;
    [[nodiscard]] bool preflightPickInteractionReady(const GizmoHitTest& hit,
                                                   GizmoPickRejectReason* reason = nullptr) const;

    /// Begin-drag interaction preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightBeginDragInteractionReady(
        const GizmoHitTest& hit, GizmoBeginDragRejectReason* reason = nullptr) const;
    [[nodiscard]] bool preflightBeginDragInteractionReady(
        const GizmoRay& ray, const GizmoTransform& transform,
        GizmoBeginDragRejectReason* reason = nullptr) const;

    /// Update-drag interaction preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightUpdateDragInteractionReady(
        const GizmoHitTest& hit, GizmoUpdateDragRejectReason* reason = nullptr) const;

    /// End-drag interaction preflight with optional reject-reason output (B6.4 deepen pass).
    [[nodiscard]] bool preflightEndDragInteractionReady(
        GizmoEndDragRejectReason* reason = nullptr) const;

    bool isDragging() const { return m_dragging; }
    bool canBeginDrag(const GizmoHitTest& hit, const GizmoTransform& transform) const;
    bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform) const;

private:
    GizmoAxis pickAxisScreen_(const GizmoHitTest& hit) const;
    GizmoTransform applyAxisDelta_(const GizmoHitTest& hit, const GizmoTransform& base) const;
    GizmoTransform applySnapping_(const GizmoTransform& transform) const;
    void markDirty_();

    GizmoMode m_mode = GizmoMode::Translate;
    GizmoSpace m_space = GizmoSpace::World;
    GizmoSnapSettings m_snap{};
    Handle<Object> m_target = Handle<Object>::invalid();
    CommandStack* m_commandStack = nullptr;
    EditorState* m_editorState = nullptr;
    GizmoAxis m_activeAxis = GizmoAxis::None;
    bool m_dragging = false;
    bool m_transformDirty = false;
    GizmoTransform m_startTransform;
    GizmoTransform m_currentTransform;
    GizmoHitTest m_lastHit;
};

} // namespace fuse::editor
