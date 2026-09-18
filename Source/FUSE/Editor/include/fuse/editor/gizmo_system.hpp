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

/// Mode-aware snap for accumulated screen-space drag deltas (B6.4 deepen follow-up).
f32 snapDragDelta(f32 delta, GizmoMode mode, const GizmoSnapSettings& settings);

/// Component-wise translate snap (B6.4 deepen — snap helpers).
math::Vec3 snapPosition(const math::Vec3& position, const GizmoSnapSettings& settings);

/// Euler rotation snap in radians (B6.4 deepen — snap helpers).
math::Vec3 snapEulerRadians(const math::Vec3& eulerRadians, const GizmoSnapSettings& settings);

/// Component-wise scale snap (B6.4 deepen — snap helpers).
math::Vec3 snapScaleVec(const math::Vec3& scale, const GizmoSnapSettings& settings);

/// Empty-hit guards — reject degenerate pick inputs before axis tests (B6.4 deepen follow-up).
bool isRayEmpty(const GizmoRay& ray);
bool isHitTestEmpty(const GizmoHitTest& hit);

/// Convenience inverse of `isRayEmpty` / `isHitTestEmpty` (B6.4 deepen — empty-ray guards).
bool isRayValid(const GizmoRay& ray);
bool isHitTestValid(const GizmoHitTest& hit);

/// Normalize ray direction; returns false when the ray is empty (B6.4 deepen — empty-ray guard).
bool normalizeRay(GizmoRay& ray);

/// True when axis length and pick radius are positive (B6.4 deepen pass).
bool isPickConfigValid(f32 axisLength, f32 pickRadius);

/// True when snap is enabled and the mode step is positive (B6.4 deepen pass).
bool isSnapStepValid(GizmoMode mode, const GizmoSnapSettings& settings);

/// True when snap is enabled with a usable step for the active mode (B6.4 deepen pass).
bool canApplySnap(GizmoMode mode, const GizmoSnapSettings& settings);

/// Read-only begin-drag diagnostics — no mutation (B6.4 deepen pass).
struct BeginDragPreflight {
    bool canBegin = false;
    bool emptyHit = false;
    bool emptyRay = false;
    bool invalidPickConfig = false;
    bool screenMiss = false;
    bool pickMiss = false;
    bool alreadyDragging = false;
    GizmoAxis axis = GizmoAxis::None;
};

/// Read-only drag-update diagnostics — no mutation (B6.4 deepen pass).
struct UpdateDragPreflight {
    bool canUpdate = false;
    bool notDragging = false;
    bool emptyHit = false;
};

BeginDragPreflight preflightBeginDrag(const GizmoRay& ray, const GizmoTransform& transform,
                                      GizmoMode mode, GizmoSpace space, f32 axisLength,
                                      f32 pickRadius, bool alreadyDragging = false);
BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit, GizmoMode mode,
                                      bool alreadyDragging = false);

UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit, bool dragging);

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
bool canBeginDrag(const GizmoHitTest& hit, GizmoMode mode);

/// Non-mutating drag-update predicate — empty viewport / inactive drag early-outs (B6.4 deepen pass).
bool canUpdateDrag(const GizmoHitTest& hit, bool dragging);

/// Screen-space dead-zone check before axis pick (B6.4 deepen).
bool isScreenHitMiss(const GizmoHitTest& hit, GizmoMode mode);
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

    GizmoResult beginDrag(const GizmoHitTest& hit, const GizmoTransform& current);
    GizmoResult beginDrag(const GizmoRay& ray, const GizmoTransform& current);
    /// Non-mutating begin-drag predicate — rejects empty hits and active drags (B6.4 deepen pass).
    [[nodiscard]] bool canBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canBeginDrag(const GizmoRay& ray, const GizmoTransform& transform) const;
    /// Read-only begin-drag diagnostics — same guards as `canBeginDrag` (B6.4 deepen pass).
    [[nodiscard]] BeginDragPreflight preflightBeginDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] BeginDragPreflight preflightBeginDrag(const GizmoRay& ray,
                                                         const GizmoTransform& transform) const;
    /// Guarded begin-drag — returns false on empty viewport / miss picks (B6.4 deepen follow-up).
    bool tryBeginDrag(const GizmoHitTest& hit, const GizmoTransform& current, GizmoResult& out);
    bool tryBeginDrag(const GizmoRay& ray, const GizmoTransform& current, GizmoResult& out);
    /// Read-only drag-update diagnostics — same guards as `canUpdateDrag` (B6.4 deepen pass).
    [[nodiscard]] UpdateDragPreflight preflightUpdateDrag(const GizmoHitTest& hit) const;
    [[nodiscard]] bool canUpdateDrag(const GizmoHitTest& hit) const;
    /// Guarded drag update — returns false on empty viewport or inactive drag (B6.4 deepen pass).
    bool tryUpdateDrag(const GizmoHitTest& hit, GizmoResult& out);
    GizmoResult updateDrag(const GizmoHitTest& hit);
    GizmoResult endDrag();
    /// Cancel an active drag without dirty marking (B6.4 deepen pass).
    void cancelDrag();

    bool isDragging() const { return m_dragging; }

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
