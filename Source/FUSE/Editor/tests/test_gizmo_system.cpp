#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/gizmo_system.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s (got %.5f expected %.5f)\n", message, actual, expected);
        ++g_failures;
    }
}

fuse::editor::GizmoRay rayAlongX() {
    fuse::editor::GizmoRay ray;
    ray.origin = {-2.f, 0.f, 0.f};
    ray.direction = {1.f, 0.f, 0.f};
    return ray;
}

fuse::Handle<fuse::Object> gizmoTestTarget() {
    return fuse::Handle<fuse::Object>(1u, 1u);
}

void bindGizmoTarget(fuse::editor::GizmoSystem& gizmo) {
    gizmo.setTarget(gizmoTestTarget());
}

void testModeSwitch() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Translate, "default mode is translate");

    expectTrue(gizmo.setMode(fuse::editor::GizmoMode::Rotate),
               "setMode reports change when mode updates");
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Rotate, "mode switches to rotate");
    expectTrue(!gizmo.setMode(fuse::editor::GizmoMode::Rotate),
               "setMode reports no change when mode is unchanged");

    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Scale, "mode switches to scale");

    gizmo.cycleMode();
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Translate, "cycle mode wraps scale to translate");

    expectTrue(fuse::editor::cycleGizmoMode(fuse::editor::GizmoMode::Translate) ==
                   fuse::editor::GizmoMode::Rotate,
               "cycleGizmoMode advances translate to rotate");
    expectTrue(fuse::editor::cycleGizmoMode(fuse::editor::GizmoMode::Rotate) ==
                   fuse::editor::GizmoMode::Scale,
               "cycleGizmoMode advances rotate to scale");

    gizmo.setSpace(fuse::editor::GizmoSpace::Local);
    expectTrue(gizmo.space() == fuse::editor::GizmoSpace::Local, "space switches to local");
}

void testScreenAxisPickExtremes() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;

    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(gizmo.pickAxis(hit) == fuse::editor::GizmoAxis::X, "left edge picks X axis");

    hit.screenX = 50.f;
    hit.screenY = 10.f;
    expectTrue(gizmo.pickAxis(hit) == fuse::editor::GizmoAxis::Y, "top edge picks Y axis");

    hit.screenX = 90.f;
    hit.screenY = 90.f;
    expectTrue(gizmo.pickAxis(hit) == fuse::editor::GizmoAxis::Z, "bottom-right picks Z axis");

    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    hit.screenX = 50.f;
    hit.screenY = 50.f;
    expectTrue(gizmo.pickAxis(hit) == fuse::editor::GizmoAxis::Uniform,
               "scale center picks uniform handle");
}

void testRayAxisPickExtremes() {
    fuse::editor::GizmoTransform transform{};
    transform.posX = 0.f;
    transform.posY = 0.f;
    transform.posZ = 0.f;

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(fuse::editor::pickAxisFromRay(xRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World,
                                             fuse::editor::GizmoSystem::kAxisLength,
                                             fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::GizmoAxis::X,
               "ray along +X picks X axis");

    fuse::editor::GizmoRay yRay;
    yRay.origin = {0.f, -2.f, 0.f};
    yRay.direction = {0.f, 1.f, 0.f};
    expectTrue(fuse::editor::pickAxisFromRay(yRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World,
                                             fuse::editor::GizmoSystem::kAxisLength,
                                             fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::GizmoAxis::Y,
               "ray along +Y picks Y axis");

    fuse::editor::GizmoRay zRay;
    zRay.origin = {0.f, 0.f, -2.f};
    zRay.direction = {0.f, 0.f, 1.f};
    expectTrue(fuse::editor::pickAxisFromRay(zRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World,
                                             fuse::editor::GizmoSystem::kAxisLength,
                                             fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::GizmoAxis::Z,
               "ray along +Z picks Z axis");

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    expectTrue(fuse::editor::pickAxisFromRay(missRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World,
                                             fuse::editor::GizmoSystem::kAxisLength,
                                             fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::GizmoAxis::None,
               "ray far from axes misses");
}

void testHitTestSegmentAndPlane() {
    const fuse::editor::GizmoRay ray = rayAlongX();
    fuse::f32 hitT = -1.f;
    expectTrue(fuse::editor::hitTestAxisSegment(ray, {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.1f, hitT),
               "ray hits X axis segment");
    expectTrue(hitT >= 0.f, "segment hit returns non-negative ray parameter");

    fuse::editor::GizmoRay planeRay;
    planeRay.origin = {0.f, 0.f, -1.f};
    planeRay.direction = {0.f, 0.f, 1.f};
    fuse::f32 planeT = -1.f;
    expectTrue(fuse::editor::hitTestAxisPlane(planeRay, {0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}, planeT),
               "ray hits XY plane");
    expectNear(planeT, 1.f, 0.001f, "plane hit distance matches expectation");
}

void testHitTestMiss() {
    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    fuse::f32 missT = -1.f;
    expectTrue(!fuse::editor::hitTestAxisSegment(missRay, {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.1f,
                                                missT),
               "segment hit-test misses when ray is far from axis");

    fuse::editor::GizmoRay parallelRay;
    parallelRay.origin = {0.f, 0.f, 0.f};
    parallelRay.direction = {0.f, 1.f, 0.f};
    fuse::f32 parallelT = -1.f;
    expectTrue(!fuse::editor::hitTestAxisPlane(parallelRay, {0.f, 0.f, 1.f}, {0.f, 0.f, 0.f},
                                               parallelT),
               "plane hit-test misses when ray is parallel to plane");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 50.f;
    hit.screenY = 50.f;
    expectTrue(fuse::editor::isScreenHitMiss(hit, fuse::editor::GizmoMode::Translate),
               "screen dead zone reports miss in translate mode");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    expectTrue(gizmo.pickAxis(hit) == fuse::editor::GizmoAxis::None,
               "screen pick returns none in translate dead zone");

    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    expectTrue(gizmo.pickAxis(hit) == fuse::editor::GizmoAxis::Uniform,
               "scale mode still picks uniform handle at screen center");

    hit.screenX = 70.f;
    hit.screenY = 70.f;
    expectTrue(fuse::editor::isScreenHitMiss(hit, fuse::editor::GizmoMode::Scale),
               "scale mode reports miss outside axis bands and uniform handle");

    fuse::editor::GizmoTransform transform{};
    expectTrue(gizmo.pickAxis(missRay, transform) == fuse::editor::GizmoAxis::None,
               "gizmo ray pick misses when ray is far from axes");
}

void testSnapGrid() {
    expectNear(fuse::editor::snapToGrid(1.37f, 0.5f), 1.5f, 0.001f, "translate snaps to grid");
    expectNear(fuse::editor::snapToGrid(-2.74f, 1.f), -3.f, 0.001f, "translate snap handles negative values");
    expectNear(fuse::editor::snapScale(1.23f, 0.1f), 1.2f, 0.001f, "scale snaps to grid step");
    expectNear(fuse::editor::snapAngleRadians(0.4f, 15.f), 0.5235988f, 0.01f,
               "rotation snaps to angle step");

    fuse::editor::GizmoSnapSettings snap;
    snap.translateSnap = true;
    snap.gridSize = 1.f;
    fuse::editor::GizmoTransform transform{};
    transform.posX = 1.6f;
    transform.posY = -2.4f;
    transform.posZ = 0.1f;

    const fuse::editor::GizmoTransform snapped =
        fuse::editor::snapTransform(transform, fuse::editor::GizmoMode::Translate, snap);
    expectNear(snapped.posX, 2.f, 0.001f, "snap transform rounds X");
    expectNear(snapped.posY, -2.f, 0.001f, "snap transform rounds Y");
    expectNear(snapped.posZ, 0.f, 0.001f, "snap transform rounds Z to grid");

    snap.translateSnap = false;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;
    transform.scaleX = 1.37f;
    transform.scaleY = 0.88f;
    transform.scaleZ = 2.01f;

    const fuse::editor::GizmoTransform scaleSnapped =
        fuse::editor::snapTransform(transform, fuse::editor::GizmoMode::Scale, snap);
    expectNear(scaleSnapped.scaleX, 1.25f, 0.001f, "snap transform rounds scale X");
    expectNear(scaleSnapped.scaleY, 1.f, 0.001f, "snap transform rounds scale Y");
    expectNear(scaleSnapped.scaleZ, 2.f, 0.001f, "snap transform rounds scale Z");
}

void testDeltaApplyRoundtrip() {
    fuse::editor::GizmoTransform base{};
    base.posX = 1.f;
    base.posY = 2.f;
    base.posZ = 3.f;
    base.rotW = 1.f;
    base.scaleX = 1.f;
    base.scaleY = 1.f;
    base.scaleZ = 1.f;

    const fuse::math::Vec3 delta{0.5f, 0.f, 0.f};
    const fuse::editor::GizmoTransform translated =
        fuse::editor::applyTranslateDelta(base, fuse::editor::GizmoAxis::X, delta,
                                          fuse::editor::GizmoSpace::World);
    expectNear(translated.posX, 1.5f, 0.001f, "world translate delta applies on X");
    expectNear(translated.posY, 2.f, 0.001f, "world translate leaves Y untouched");

    const fuse::editor::GizmoTransform rotated =
        fuse::editor::applyRotateDelta(base, fuse::editor::GizmoAxis::Y, 0.25f,
                                       fuse::editor::GizmoSpace::World);
    expectTrue(rotated.rotW != base.rotW || rotated.rotY != base.rotY,
               "rotate delta changes orientation");

    const fuse::math::Vec3 scaleDelta{0.2f, 0.f, 0.f};
    const fuse::editor::GizmoTransform scaled =
        fuse::editor::applyScaleDelta(base, fuse::editor::GizmoAxis::X, scaleDelta,
                                     fuse::editor::GizmoSpace::World);
    expectNear(scaled.scaleX, 1.2f, 0.001f, "scale delta applies on X");

    const fuse::math::Vec3 position = fuse::editor::gizmoPosition(translated);
    const fuse::math::Quat rotation = fuse::editor::gizmoRotation(rotated);
    const fuse::math::Vec3 scale = fuse::editor::gizmoScale(scaled);
    const fuse::editor::GizmoTransform roundtrip =
        fuse::editor::gizmoFromMath(position, rotation, scale);
    expectNear(roundtrip.posX, translated.posX, 0.001f, "math roundtrip preserves position X");
    expectNear(roundtrip.scaleX, scaled.scaleX, 0.001f, "math roundtrip preserves scale X");
}

void testSetModeCancelsDrag() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.isDragging(), "drag starts on valid axis pick");

    expectTrue(gizmo.setMode(fuse::editor::GizmoMode::Rotate),
               "setMode reports change while dragging");
    expectTrue(!gizmo.isDragging(), "setMode cancels active drag");
}

void testTryPickAxisGuards() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay emptyRay{};
    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(fuse::editor::isRayEmpty(emptyRay), "zero-direction ray is empty");
    expectTrue(!fuse::editor::tryPickAxis(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                          fuse::editor::GizmoSpace::World,
                                          fuse::editor::GizmoSystem::kAxisLength,
                                          fuse::editor::GizmoSystem::kPickRadius, axis),
               "tryPickAxis rejects empty ray");
    expectTrue(axis == fuse::editor::GizmoAxis::None, "empty ray leaves axis unset");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(gizmo.tryPickAxis(xRay, transform, axis), "tryPickAxis succeeds on valid ray");
    expectTrue(axis == fuse::editor::GizmoAxis::X, "tryPickAxis writes picked axis");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    emptyHit.viewportHeight = 100.f;
    expectTrue(fuse::editor::isHitTestEmpty(emptyHit), "zero-width hit test is empty");
    expectTrue(!fuse::editor::tryPickAxis(emptyHit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects empty hit test");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!gizmo.tryPickAxis(deadZone, axis),
               "tryPickAxis rejects translate dead-zone hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None, "dead-zone hit leaves axis unset");

    deadZone.screenX = 10.f;
    deadZone.screenY = 50.f;
    expectTrue(gizmo.tryPickAxis(deadZone, axis), "tryPickAxis accepts valid screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::X, "valid screen hit writes picked axis");
}

void testSnapValueModeAware() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;
    snap.rotateSnap = true;
    snap.angleStepDegrees = 15.f;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;

    expectNear(fuse::editor::snapValue(1.37f, fuse::editor::GizmoMode::Translate, snap), 1.5f, 0.001f,
               "snapValue translates with grid when enabled");
    expectNear(fuse::editor::snapValue(0.4f, fuse::editor::GizmoMode::Rotate, snap), 0.5235988f, 0.01f,
               "snapValue rotates with angle step when enabled");
    expectNear(fuse::editor::snapValue(1.23f, fuse::editor::GizmoMode::Scale, snap), 1.25f, 0.001f,
               "snapValue scales with grid step when enabled");

    snap.translateSnap = false;
    expectNear(fuse::editor::snapValue(1.37f, fuse::editor::GizmoMode::Translate, snap), 1.37f, 0.001f,
               "snapValue passthrough when translate snap disabled");
}

void testTryBeginDragGuards() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoResult result{};

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportHeight = 0.f;
    expectTrue(!gizmo.tryBeginDrag(emptyHit, transform, result),
               "tryBeginDrag rejects empty viewport");
    expectTrue(!result.active, "empty viewport leaves drag inactive");
    expectTrue(!gizmo.isDragging(), "empty viewport does not start drag");

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!gizmo.tryBeginDrag(emptyRay, transform, result),
               "tryBeginDrag rejects empty ray");
    expectTrue(!gizmo.isDragging(), "empty ray does not start drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(gizmo.tryBeginDrag(hit, transform, result), "tryBeginDrag accepts valid screen hit");
    expectTrue(result.active, "valid screen hit activates drag");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "valid screen hit records axis");
    gizmo.endDrag();

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(gizmo.tryBeginDrag(xRay, transform, result), "tryBeginDrag accepts valid ray");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "valid ray records axis");
    gizmo.endDrag();
}

void testUpdateDragEmptyViewportGuard() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    hit.viewportWidth = 0.f;
    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(!update.changed, "updateDrag ignores empty viewport");
    expectTrue(gizmo.isDragging(), "empty viewport update keeps drag active");
}

void testCycleModeCancelsDrag() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.isDragging(), "drag active before cycle");

    gizmo.cycleMode();
    expectTrue(!gizmo.isDragging(), "cycleMode cancels active drag");
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Rotate, "cycleMode advances mode");
}

void testSnapDragDeltaModeAware() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;
    snap.angleStepDegrees = 15.f;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;

    expectTrue(fuse::editor::isSnapEnabled(fuse::editor::GizmoMode::Translate, snap),
               "translate snap enabled when flag set");
    expectTrue(!fuse::editor::isSnapEnabled(fuse::editor::GizmoMode::Rotate, snap),
               "rotate snap disabled when flag unset");

    expectNear(fuse::editor::snapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap), 0.5f,
               0.001f, "snapDragDelta snaps translate delta");
    expectNear(fuse::editor::snapDragDelta(0.37f, fuse::editor::GizmoMode::Rotate, snap), 0.37f,
               0.001f, "snapDragDelta passthrough when rotate snap disabled");

    snap.rotateSnap = true;
    expectNear(fuse::editor::snapDragDelta(0.4f, fuse::editor::GizmoMode::Rotate, snap), 0.5235988f,
               0.01f, "snapDragDelta snaps rotate delta when enabled");
}

void testPickAxisFromRayEmptyGuard() {
    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoRay emptyRay{};
    expectTrue(fuse::editor::pickAxisFromRay(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World, 0.f,
                                             fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::GizmoAxis::None,
               "pickAxisFromRay rejects zero axis length");
    expectTrue(fuse::editor::pickAxisFromRay(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World,
                                             fuse::editor::GizmoSystem::kAxisLength, 0.f) ==
                   fuse::editor::GizmoAxis::None,
               "pickAxisFromRay rejects zero pick radius");
}

void testCanBeginDragGuards() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::canBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate),
               "canBeginDrag rejects empty viewport");
    expectTrue(!gizmo.canBeginDrag(emptyHit), "gizmo canBeginDrag rejects empty viewport");
void testSnapGuardValid() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::isSnapGuardValid(fuse::editor::GizmoMode::Translate, snap),
               "translate snap guard rejects zero grid size");
    snap.gridSize = 0.5f;
    expectTrue(fuse::editor::isSnapGuardValid(fuse::editor::GizmoMode::Translate, snap),
               "translate snap guard accepts positive grid size");

    snap.rotateSnap = true;
    snap.angleStepDegrees = 0.f;
    expectTrue(!fuse::editor::isSnapGuardValid(fuse::editor::GizmoMode::Rotate, snap),
               "rotate snap guard rejects zero angle step");
    snap.angleStepDegrees = 15.f;
    expectTrue(fuse::editor::isSnapGuardValid(fuse::editor::GizmoMode::Rotate, snap),
               "rotate snap guard accepts positive angle step");

    snap.scaleSnap = true;
    snap.scaleGridStep = 0.f;
    expectTrue(!fuse::editor::isSnapGuardValid(fuse::editor::GizmoMode::Scale, snap),
               "scale snap guard rejects zero grid step");
    snap.scaleGridStep = 0.1f;
    expectTrue(fuse::editor::isSnapGuardValid(fuse::editor::GizmoMode::Scale, snap),
               "scale snap guard accepts positive grid step");

    snap.translateSnap = false;
               "snap guard passes when snap disabled for mode");

    expectNear(fuse::editor::snapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap), 0.37f,
               0.001f, "snapDragDelta passthrough when snap disabled");
               0.001f, "snapDragDelta passthrough when snap guard invalid");
}

void testBeginDragPreflight() {

    fuse::editor::GizmoRay emptyRay{};
    const fuse::editor::GizmoBeginDragPreflight emptyRayPreflight =
        fuse::editor::preflightBeginDrag(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                         fuse::editor::GizmoSpace::World,
                                         fuse::editor::GizmoSystem::kAxisLength,
                                         fuse::editor::GizmoSystem::kPickRadius, false, snap);
    expectTrue(emptyRayPreflight.emptyRay, "preflight marks empty ray");
    expectTrue(!emptyRayPreflight.canBegin(), "preflight blocks empty ray");

    emptyHit.viewportHeight = 0.f;
    const fuse::editor::GizmoBeginDragPreflight emptyViewportPreflight =
        fuse::editor::preflightBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate, false, snap);
    expectTrue(emptyViewportPreflight.emptyViewport, "preflight marks empty viewport");
    expectTrue(!emptyViewportPreflight.canBegin(), "preflight blocks empty viewport");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!fuse::editor::canBeginDrag(deadZone, fuse::editor::GizmoMode::Translate),
               "canBeginDrag rejects translate dead zone");
    expectTrue(!gizmo.canBeginDrag(deadZone), "gizmo canBeginDrag rejects translate dead zone");

    deadZone.screenX = 10.f;
    expectTrue(fuse::editor::canBeginDrag(deadZone, fuse::editor::GizmoMode::Translate),
               "canBeginDrag accepts valid screen hit");
    expectTrue(gizmo.canBeginDrag(deadZone), "gizmo canBeginDrag accepts valid screen hit");

    gizmo.beginDrag(deadZone, transform);
    expectTrue(!gizmo.canBeginDrag(deadZone), "canBeginDrag rejects while drag is active");
    gizmo.endDrag();

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!fuse::editor::canBeginDrag(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                           fuse::editor::GizmoSpace::World,
                                           fuse::editor::GizmoSystem::kAxisLength,
                                           fuse::editor::GizmoSystem::kPickRadius),
               "canBeginDrag rejects empty ray");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(fuse::editor::canBeginDrag(xRay, transform, fuse::editor::GizmoMode::Translate,
               "canBeginDrag accepts valid ray pick");
    expectTrue(gizmo.canBeginDrag(xRay, transform), "gizmo canBeginDrag accepts valid ray pick");

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    expectTrue(!gizmo.canBeginDrag(missRay, transform), "gizmo canBeginDrag rejects ray miss");
}

void testCanPickAxisGuards() {
void testSnapSettingsClampAndStep() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.gridSize = 0.f;
    snap.angleStepDegrees = -1.f;
    snap.scaleGridStep = 0.f;

    expectTrue(!fuse::editor::isSnapSettingsValid(snap), "invalid snap settings before clamp");
    fuse::editor::clampSnapSettings(snap);
    expectTrue(fuse::editor::isSnapSettingsValid(snap), "clampSnapSettings restores valid defaults");
    expectNear(snap.gridSize, 1.f, 0.001f, "clampSnapSettings restores translate grid");
    expectNear(snap.angleStepDegrees, 15.f, 0.001f, "clampSnapSettings restores rotate step");
    expectNear(snap.scaleGridStep, 0.1f, 0.001f, "clampSnapSettings restores scale step");

    snap.translateSnap = true;
    snap.rotateSnap = true;
    snap.scaleSnap = true;
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Translate, snap), 1.f, 0.001f,
               "snapStepForMode returns translate grid when enabled");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Rotate, snap), 0.2617994f,
               0.01f, "snapStepForMode returns rotate radians when enabled");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Scale, snap), 0.1f, 0.001f,
               "snapStepForMode returns scale step when enabled");

    snap.translateSnap = false;
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Translate, snap), 0.f, 0.001f,
               "snapStepForMode returns zero when snap disabled");

void testCanPickAxisPreflight() {

    expectTrue(fuse::editor::canPickAxis(xRay, transform, fuse::editor::GizmoMode::Translate,
               "canPickAxis succeeds on valid ray");
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoTransform transform{};

    expectTrue(!fuse::editor::canPickAxis(emptyRay, transform, fuse::editor::GizmoMode::Translate,
               "canPickAxis rejects empty ray");
    expectTrue(!gizmo.canPickAxis(emptyRay, transform), "gizmo canPickAxis rejects empty ray");

    expectTrue(gizmo.canPickAxis(xRay, transform), "gizmo canPickAxis accepts valid ray");

    expectTrue(!fuse::editor::canPickAxis(deadZone, fuse::editor::GizmoMode::Translate),
               "canPickAxis rejects translate dead zone");
    expectTrue(!gizmo.canPickAxis(deadZone), "gizmo canPickAxis rejects translate dead zone");

    expectTrue(gizmo.canPickAxis(deadZone), "gizmo canPickAxis accepts valid screen hit");

void testSnapStepForMode() {
    snap.gridSize = 0.5f;
    snap.angleStepDegrees = 15.f;
    snap.scaleGridStep = 0.25f;

    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Translate, snap), 0.5f, 0.001f,
               "snapStepForMode returns translate grid size");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Rotate, snap), 15.f, 0.001f,
               "snapStepForMode returns rotate angle step");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Scale, snap), 0.25f, 0.001f,
               "snapStepForMode returns scale grid step");

    expectNear(fuse::editor::snapValue(1.37f, fuse::editor::GizmoMode::Translate, snap),
               fuse::editor::snapToGrid(1.37f, fuse::editor::snapStepForMode(
                                                fuse::editor::GizmoMode::Translate, snap)),
               0.001f, "snapValue uses snapStepForMode grid for translate");

void testBeginDragRayUsesTryBeginDrag() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoTransform transform{};

    const fuse::editor::GizmoResult miss = gizmo.beginDrag(missRay, transform);
    expectTrue(!miss.active, "beginDrag ray rejects miss via tryBeginDrag guards");
    expectTrue(!gizmo.isDragging(), "beginDrag ray miss does not start drag");

    const fuse::editor::GizmoResult started = gizmo.beginDrag(xRay, transform);
    expectTrue(started.active, "beginDrag ray accepts valid pick");
    expectTrue(started.axis == fuse::editor::GizmoAxis::X, "beginDrag ray records axis");

void testBeginDragPreflight() {

    const fuse::editor::BeginDragPreflight emptyRayPreflight = fuse::editor::preflightBeginDrag(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(emptyRayPreflight.emptyRay, "preflight marks empty ray");
    expectTrue(!emptyRayPreflight.canBegin, "preflight rejects empty ray");

    const fuse::editor::BeginDragPreflight missPreflight = fuse::editor::preflightBeginDrag(
        missRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(missPreflight.pickMiss, "preflight marks ray pick miss");
    expectTrue(!missPreflight.canBegin, "preflight rejects ray pick miss");

    const fuse::editor::BeginDragPreflight validRayPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(validRayPreflight.canBegin, "preflight accepts valid ray pick");
    expectTrue(!validRayPreflight.emptyRay, "valid ray preflight clears emptyRay");
    expectTrue(!validRayPreflight.pickMiss, "valid ray preflight clears pickMiss");
    expectTrue(validRayPreflight.axis == fuse::editor::GizmoAxis::X,
               "valid ray preflight records picked axis");

    const fuse::editor::BeginDragPreflight invalidConfigPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World, 0.f,
        fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(invalidConfigPreflight.invalidPickConfig,
               "preflight marks invalid axis length / pick radius");
    expectTrue(!invalidConfigPreflight.canBegin, "preflight rejects invalid pick config");

    const fuse::editor::BeginDragPreflight emptyHitPreflight =
        fuse::editor::preflightBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyHitPreflight.emptyHit, "preflight marks empty viewport");
    expectTrue(!emptyHitPreflight.canBegin, "preflight rejects empty viewport");

    const fuse::editor::BeginDragPreflight screenMissPreflight =
        fuse::editor::preflightBeginDrag(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(screenMissPreflight.screenMiss, "preflight marks translate dead zone");
    expectTrue(!screenMissPreflight.canBegin, "preflight rejects translate dead zone");

    const fuse::editor::BeginDragPreflight validHitPreflight =
    expectTrue(validHitPreflight.canBegin, "preflight accepts valid screen hit");
    expectTrue(validHitPreflight.axis == fuse::editor::GizmoAxis::X,
               "valid screen preflight records picked axis");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.beginDrag(deadZone, transform);
    const fuse::editor::BeginDragPreflight draggingPreflight = gizmo.preflightBeginDrag(deadZone);
    expectTrue(draggingPreflight.alreadyDragging, "gizmo preflight marks active drag");
    expectTrue(!draggingPreflight.canBegin, "gizmo preflight rejects while dragging");

void testSnapStepGuards() {

    expectTrue(!fuse::editor::isSnapStepValid(fuse::editor::GizmoMode::Translate, snap),
               "isSnapStepValid rejects zero translate grid when snap enabled");
    expectTrue(!fuse::editor::canApplySnap(fuse::editor::GizmoMode::Translate, snap),
               "canApplySnap rejects zero translate grid when snap enabled");

    expectTrue(fuse::editor::isSnapStepValid(fuse::editor::GizmoMode::Translate, snap),
               "isSnapStepValid accepts positive translate grid");
    expectTrue(fuse::editor::canApplySnap(fuse::editor::GizmoMode::Translate, snap),
               "canApplySnap accepts positive translate grid");

               "isSnapStepValid passes when translate snap disabled");
               "canApplySnap false when translate snap disabled");

    snap.angleStepDegrees = 0.f;
    expectTrue(!fuse::editor::canApplySnap(fuse::editor::GizmoMode::Rotate, snap),
               "canApplySnap rejects zero angle step when rotate snap enabled");

    transform.posX = 1.37f;
    const fuse::editor::GizmoTransform unsnapped =
        fuse::editor::snapTransform(transform, fuse::editor::GizmoMode::Translate, snap);
    expectNear(unsnapped.posX, 1.37f, 0.001f,
               "snapTransform passthrough when step invalid (snapToGrid guard)");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    gizmo.setMode(fuse::editor::GizmoMode::Translate);
    const fuse::editor::GizmoBeginDragPreflight missPreflight =
        fuse::editor::preflightBeginDrag(deadZone, fuse::editor::GizmoMode::Translate, false, snap);
    expectTrue(missPreflight.axisMiss, "preflight marks translate dead-zone miss");
    expectTrue(!missPreflight.canBegin(), "preflight blocks axis miss");

    const fuse::editor::GizmoBeginDragPreflight validPreflight =
        fuse::editor::preflightBeginDrag(xRay, transform, fuse::editor::GizmoMode::Translate,
                                         fuse::editor::GizmoSystem::kPickRadius, false, snap);
    expectTrue(validPreflight.canBegin(), "preflight allows valid ray pick");
    expectTrue(validPreflight.axis == fuse::editor::GizmoAxis::X, "preflight records picked axis");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, transform);
    hit.screenX = 80.f;
    hit.screenX = 20.f;
    hit.screenX = 25.f;
    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(update.changed, "drag update still applies when snap step invalid");

void testPickConfigValid() {
    expectTrue(!fuse::editor::isPickConfigValid(0.f, fuse::editor::GizmoSystem::kPickRadius),
               "isPickConfigValid rejects zero axis length");
    expectTrue(!fuse::editor::isPickConfigValid(fuse::editor::GizmoSystem::kAxisLength, 0.f),
               "isPickConfigValid rejects zero pick radius");
    expectTrue(fuse::editor::isPickConfigValid(fuse::editor::GizmoSystem::kAxisLength,
               "isPickConfigValid accepts default gizmo constants");
    expectTrue(!fuse::editor::isPickConfigFinite(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                 fuse::editor::GizmoSystem::kPickRadius),
               "isPickConfigFinite rejects NaN axis length");
    expectTrue(!fuse::editor::isPickConfigValid(std::numeric_limits<fuse::f32>::infinity(),
               "isPickConfigValid rejects non-finite axis length");
}

void testPickPreflightGuards() {

    const fuse::editor::PickPreflight emptyRayPick = fuse::editor::preflightPick(
    expectTrue(emptyRayPick.emptyRay, "pick preflight marks empty ray");
    expectTrue(!emptyRayPick.canPick(), "pick preflight rejects empty ray");

    const fuse::editor::PickPreflight validRayPick = fuse::editor::preflightPick(
    expectTrue(validRayPick.canPick(), "pick preflight accepts valid ray");
    expectTrue(!validRayPick.emptyRay, "valid ray pick clears emptyRay");
    expectTrue(!validRayPick.pickMiss, "valid ray pick clears pickMiss");

    const fuse::editor::PickPreflight missPick = fuse::editor::preflightPick(
    expectTrue(missPick.pickMiss, "pick preflight marks ray miss");
    expectTrue(!missPick.canPick(), "pick preflight rejects ray miss");

    const fuse::editor::PickPreflight emptyHitPick =
        fuse::editor::preflightPick(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyHitPick.emptyHit, "pick preflight marks empty viewport");
    expectTrue(!emptyHitPick.canPick(), "pick preflight rejects empty viewport");

    const fuse::editor::PickPreflight screenMissPick =
        fuse::editor::preflightPick(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(screenMissPick.screenMiss, "pick preflight marks translate dead zone");
    expectTrue(!screenMissPick.canPick(), "pick preflight rejects translate dead zone");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    deadZone.screenX = 10.f;
    deadZone.screenY = 50.f;
    const fuse::editor::PickPreflight gizmoPick = gizmo.preflightPick(deadZone);
    expectTrue(gizmoPick.canPick(), "gizmo pick preflight accepts valid screen hit");
    expectTrue(gizmo.preflightPick(xRay, transform).canPick(),
               "gizmo pick preflight accepts valid ray");

void testPickPreflightAxisResolution() {
    fuse::editor::GizmoTransform transform{};

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickPreflight rayPick = fuse::editor::preflightPick(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(rayPick.canPick(), "ray pick preflight succeeds on valid ray");
    expectTrue(rayPick.axis == fuse::editor::GizmoAxis::X,
               "ray pick preflight resolves picked axis");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    const fuse::editor::PickPreflight screenPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(screenPick.canPick(), "screen pick preflight succeeds on valid hit");
    expectTrue(screenPick.axis == fuse::editor::GizmoAxis::X,
               "screen pick preflight resolves picked axis");

    fuse::editor::GizmoRay emptyRay{};
    const fuse::editor::PickPreflight emptyPick = fuse::editor::preflightPick(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(emptyPick.axis == fuse::editor::GizmoAxis::None,
               "failed pick preflight leaves axis unset");
    deadZone.screenX = 150.f;
    const fuse::editor::PickPreflight outOfBoundsPick =
        fuse::editor::preflightPick(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(outOfBoundsPick.outOfBounds, "pick preflight marks out-of-bounds screen coords");
    expectTrue(!outOfBoundsPick.canPick(), "pick preflight rejects out-of-bounds screen coords");
    expectTrue(!gizmo.preflightPick(deadZone).canPick(),
               "gizmo pick preflight rejects out-of-bounds screen coords");
}

void testSnapPreflightGuards() {

    const fuse::editor::SnapPreflight disabledPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap preflight marks disabled snap");
    expectTrue(!disabledPreflight.canApply(), "snap preflight rejects disabled snap");
    expectTrue(!disabledPreflight.wouldSnap(), "snap preflight wouldSnap false when disabled");

    const fuse::editor::SnapPreflight invalidStepPreflight =
    expectTrue(invalidStepPreflight.invalidStep, "snap preflight marks invalid step");
    expectTrue(!invalidStepPreflight.canApply(), "snap preflight rejects invalid step");
    expectTrue(!invalidStepPreflight.wouldSnap(), "snap preflight wouldSnap false when step invalid");

    const fuse::editor::SnapPreflight validPreflight =
    expectTrue(validPreflight.canApply(), "snap preflight accepts enabled snap with valid step");
    expectTrue(validPreflight.wouldSnap(), "snap preflight wouldSnap when snap can apply");
    expectTrue(!validPreflight.snapDisabled, "valid snap preflight clears snapDisabled");
    expectTrue(!validPreflight.invalidStep, "valid snap preflight clears invalidStep");

    fuse::editor::GizmoTransform onGrid{};
    onGrid.posX = 1.f;
    onGrid.posY = -2.f;
    const fuse::editor::SnapPreflight noChangePreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap, onGrid);
    expectTrue(noChangePreflight.canApply(), "transform snap preflight accepts valid settings");
    expectTrue(noChangePreflight.noChange, "transform snap preflight marks already-snapped transform");
    expectTrue(!noChangePreflight.wouldSnap(), "transform snap preflight wouldSnap false when no change");

    fuse::editor::GizmoTransform offGrid = onGrid;
    offGrid.posX = 1.37f;
    const fuse::editor::SnapPreflight wouldChangePreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap, offGrid);
    expectTrue(!wouldChangePreflight.noChange, "transform snap preflight clears noChange when snap would move");
    expectTrue(wouldChangePreflight.wouldSnap(), "transform snap preflight wouldSnap when transform off grid");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnap().canApply(), "gizmo snap preflight accepts valid settings");
    expectTrue(gizmo.preflightSnap(onGrid).noChange,
               "gizmo transform snap preflight marks already-snapped transform");
    expectTrue(gizmo.canApplySnapNow(), "gizmo canApplySnapNow mirrors preflight");
    expectTrue(gizmo.canSnapDragDeltaNow(), "gizmo canSnapDragDeltaNow mirrors snap drag guards");
}

void testTrySnapTransformGuards() {
    snap.gridSize = 1.f;

    transform.posX = 1.6f;
    transform.posY = -2.4f;

    fuse::editor::GizmoTransform out{};
    expectTrue(!fuse::editor::trySnapTransform(transform, fuse::editor::GizmoMode::Translate, snap,
                                               out),
               "trySnapTransform rejects disabled snap");
    expectNear(out.posX, 1.6f, 0.001f, "trySnapTransform leaves transform unchanged on reject");

               "trySnapTransform rejects invalid step");
    expectNear(out.posX, 1.6f, 0.001f, "trySnapTransform leaves transform unchanged on invalid step");

    expectTrue(fuse::editor::trySnapTransform(transform, fuse::editor::GizmoMode::Translate, snap,
               "trySnapTransform applies when snap is valid");
    expectNear(out.posX, 2.f, 0.001f, "trySnapTransform snaps position X");
    expectNear(out.posY, -2.f, 0.001f, "trySnapTransform snaps position Y");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform gizmoOut{};
    expectTrue(gizmo.trySnapTransform(transform, gizmoOut),
               "gizmo trySnapTransform applies when snap is valid");
    expectNear(gizmoOut.posX, 2.f, 0.001f, "gizmo trySnapTransform snaps position X");

void testTrySnapDragDeltaGuards() {

    expectNear(fuse::editor::trySnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               0.37f, 0.001f, "trySnapDragDelta passthrough when translate snap disabled");

               0.37f, 0.001f, "trySnapDragDelta passthrough when step invalid");

               0.5f, 0.001f, "trySnapDragDelta snaps when snap is valid");

void testUpdateDragPreflightGuards() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateDragPreflight inactivePreflight =
        fuse::editor::preflightUpdateDrag(hit, false);
    expectTrue(inactivePreflight.notDragging, "update preflight marks inactive drag");
    expectTrue(!inactivePreflight.canUpdate(), "update preflight rejects inactive drag");
    expectTrue(!gizmo.preflightUpdateDrag(hit).canUpdate(),
               "gizmo update preflight rejects inactive drag");

    const fuse::editor::UpdateDragPreflight activePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(activePreflight.canUpdate(), "update preflight accepts active drag with valid hit");

    hit.viewportWidth = 0.f;
    const fuse::editor::UpdateDragPreflight emptyPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(emptyPreflight.emptyHit, "update preflight marks empty viewport");
    expectTrue(!emptyPreflight.canUpdate(), "update preflight rejects empty viewport");

    hit.viewportWidth = 100.f;
    hit.screenX = 150.f;
    const fuse::editor::UpdateDragPreflight outOfBoundsPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(outOfBoundsPreflight.outOfBounds, "update preflight marks out-of-bounds coords");
    expectTrue(!outOfBoundsPreflight.canUpdate(), "update preflight rejects out-of-bounds coords");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects out-of-bounds coords");

    hit.viewportWidth = 0.f;
    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(!update.changed, "updateDrag ignores empty viewport via preflight guard");
    expectTrue(gizmo.isDragging(), "empty viewport update keeps drag active");

void testPickInputValidityGuards() {
    expectTrue(!fuse::editor::isRayValid(emptyRay), "isRayValid rejects empty ray");
    expectTrue(fuse::editor::isRayEmpty(emptyRay), "empty ray remains empty");

    fuse::editor::GizmoRay ray = rayAlongX();
    expectTrue(fuse::editor::isRayValid(ray), "isRayValid accepts non-empty ray");
    ray.direction = {2.f, 0.f, 0.f};
    expectTrue(fuse::editor::normalizeRay(ray), "normalizeRay succeeds on non-zero direction");
    expectNear(ray.direction.x, 1.f, 0.001f, "normalizeRay unitizes X direction");

    fuse::editor::GizmoRay zeroRay{};
    expectTrue(!fuse::editor::normalizeRay(zeroRay), "normalizeRay rejects zero direction");

    expectTrue(!fuse::editor::isHitTestValid(emptyHit), "isHitTestValid rejects empty viewport");

    expectTrue(fuse::editor::isHitTestValid(hit), "isHitTestValid accepts valid viewport");
    expectTrue(fuse::editor::isScreenCoordInViewport(hit),
               "isScreenCoordInViewport accepts origin viewport coords");

    hit.screenX = 150.f;
    expectTrue(!fuse::editor::isScreenCoordInViewport(hit),
               "isScreenCoordInViewport rejects X beyond viewport width");
    hit.screenX = -1.f;
               "isScreenCoordInViewport rejects negative X");
}

void testSnapComponentHelpers() {

    const fuse::math::Vec3 position = fuse::editor::snapPosition({1.6f, -2.4f, 0.1f}, snap);
    expectNear(position.x, 2.f, 0.001f, "snapPosition rounds X");
    expectNear(position.y, -2.f, 0.001f, "snapPosition rounds Y");
    expectNear(position.z, 0.f, 0.001f, "snapPosition rounds Z");

    const fuse::math::Vec3 euler =
        fuse::editor::snapEulerRadians({0.4f, 0.f, 0.f}, snap);
    expectNear(euler.x, 0.5235988f, 0.01f, "snapEulerRadians rounds rotation");

    const fuse::math::Vec3 scale = fuse::editor::snapScaleVec({1.37f, 0.88f, 2.01f}, snap);
    expectNear(scale.x, 1.25f, 0.001f, "snapScaleVec rounds scale X");
    expectNear(scale.y, 1.f, 0.001f, "snapScaleVec rounds scale Y");
    expectNear(scale.z, 2.f, 0.001f, "snapScaleVec rounds scale Z");

    expectTrue(!fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta rejects disabled translate snap");
               "canSnapDragDelta rejects invalid translate step");
    expectTrue(fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta accepts valid translate snap");

void testCanUpdateDragGuards() {

    expectTrue(!fuse::editor::canUpdateDrag(hit, false), "canUpdateDrag rejects inactive drag");
    expectTrue(!fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag rejects drag without axis");

    const fuse::editor::UpdateDragPreflight noAxisPreflight =
        fuse::editor::preflightUpdateDrag(hit, true);
    expectTrue(noAxisPreflight.invalidActiveAxis, "update preflight marks missing active axis");
    expectTrue(!noAxisPreflight.canUpdate(), "update preflight rejects missing active axis");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag with axis");

    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects empty viewport");

void testTryUpdateDragGuards() {

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryUpdateDrag leaves result unchanged");

    hit.screenX = 30.f;
    expectTrue(gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag accepts valid active drag");
    expectTrue(result.changed, "tryUpdateDrag marks result changed");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryUpdateDrag records active axis");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects empty viewport");
    expectTrue(gizmo.isDragging(), "tryUpdateDrag reject keeps drag active");

void testPickPreflightAxisResolution() {

    const fuse::editor::PickPreflight rayPick = fuse::editor::preflightPick(
    expectTrue(rayPick.canPick(), "pick preflight accepts valid ray");
    expectTrue(rayPick.axis == fuse::editor::GizmoAxis::X, "pick preflight resolves ray axis");

    const fuse::editor::PickPreflight screenPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(screenPick.canPick(), "pick preflight accepts valid screen hit");
    expectTrue(screenPick.axis == fuse::editor::GizmoAxis::X,
               "pick preflight resolves screen axis");

void testUpdateDragSnapDegradedPreflight() {


    const fuse::editor::UpdateDragPreflight degradedPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canUpdate(),
               "update preflight still allows drag when snap step invalid");
    expectTrue(degradedPreflight.snapDegraded, "update preflight marks snap degraded");

    const fuse::editor::UpdateDragPreflight validPreflight = fuse::editor::preflightUpdateDrag(
    expectTrue(validPreflight.canUpdate(), "update preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded");

void testEndDragPreflightGuards() {

    const fuse::editor::EndDragPreflight inactivePreflight =
        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoAxis::None,
                                       fuse::editor::GizmoMode::Translate, snap);
    expectTrue(inactivePreflight.notDragging, "end preflight marks inactive drag");
    expectTrue(!inactivePreflight.canEnd(), "end preflight rejects inactive drag");

    const fuse::editor::EndDragPreflight activePreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activePreflight.canEnd(), "end preflight accepts active drag");
    expectTrue(activePreflight.snapDegraded, "end preflight marks snap degraded");
    expectTrue(!activePreflight.invalidActiveAxis, "valid axis clears invalidActiveAxis");

    const fuse::editor::EndDragPreflight noAxisPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(noAxisPreflight.invalidActiveAxis, "end preflight marks missing active axis");
    expectTrue(noAxisPreflight.canEnd(), "end preflight still allows end without active axis");

    const fuse::editor::EndDragPreflight validSnapPreflight = fuse::editor::preflightEndDrag(
    expectTrue(!validSnapPreflight.snapDegraded, "valid snap clears snapDegraded on end");

    expectTrue(!gizmo.preflightEndDrag().canEnd(), "gizmo end preflight rejects inactive drag");

    expectTrue(gizmo.preflightEndDrag().canEnd(), "gizmo end preflight accepts active drag");

void testCanEndDragGuards() {
    expectTrue(!fuse::editor::canEndDrag(false), "canEndDrag rejects inactive drag");
    expectTrue(fuse::editor::canEndDrag(true, fuse::editor::GizmoAxis::X),
               "canEndDrag accepts active drag with axis");

    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects inactive drag");

    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag accepts active drag");

void testTryEndDragGuards() {

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryEndDrag leaves result unchanged");

    expectTrue(fuse::editor::canPickAxis(hit, fuse::editor::GizmoMode::Translate),
               "canPickAxis accepts valid screen hit");

    hit.screenX = 50.f;
    expectTrue(!fuse::editor::canPickAxis(hit, fuse::editor::GizmoMode::Translate),

void testCanBeginDragPreflight() {

    expectTrue(fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate),
    expectTrue(gizmo.canBeginDrag(hit, transform), "GizmoSystem canBeginDrag accepts valid hit");

    expectTrue(!fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate),
    expectTrue(!gizmo.canBeginDrag(hit, transform),
               "GizmoSystem canBeginDrag rejects dead-zone hit");

               "canBeginDrag accepts valid ray");
    expectTrue(gizmo.canBeginDrag(xRay, transform), "GizmoSystem canBeginDrag accepts valid ray");

void testTryBeginDragAlreadyDraggingGuard() {


    expectTrue(gizmo.tryBeginDrag(hit, transform, result), "first begin drag succeeds");
    expectTrue(gizmo.isDragging(), "drag is active after begin");

    expectTrue(!gizmo.tryBeginDrag(hit, transform, result),
               "tryBeginDrag rejects when already dragging");
               "canBeginDrag returns false while dragging");

    gizmo.cancelDrag();
    expectTrue(!gizmo.isDragging(), "cancelDrag clears dragging state");
    expectTrue(gizmo.canBeginDrag(hit, transform),
               "canBeginDrag succeeds again after cancel");

void testCancelDragWithoutDirty() {
    fuse::editor::CommandStack commandStack;
    fuse::editor::EditorState editorState;
    gizmo.setCommandStack(&commandStack);
    gizmo.setEditorState(&editorState);

void testRayNormalizeAndValidityGuards() {
    expectTrue(fuse::editor::isRayEmpty(emptyRay), "zero-direction ray is empty");
    expectTrue(!fuse::editor::normalizeRay(emptyRay), "normalizeRay rejects empty ray");

    fuse::editor::GizmoRay ray{};
    ray.direction = {3.f, 0.f, 4.f};
    expectTrue(fuse::editor::normalizeRay(ray), "normalizeRay succeeds on non-empty ray");
    expectNear(ray.direction.x, 0.6f, 0.001f, "normalizeRay normalizes X");
    expectNear(ray.direction.z, 0.8f, 0.001f, "normalizeRay normalizes Z");
    expectTrue(fuse::editor::isRayValid(ray), "normalized ray is valid");


    fuse::editor::GizmoHitTest validHit{};
    validHit.viewportWidth = 100.f;
    validHit.viewportHeight = 100.f;
    expectTrue(fuse::editor::isHitTestValid(validHit), "isHitTestValid accepts valid viewport");

void testSnapVecHelpers() {

    const fuse::math::Vec3 position{1.37f, -2.24f, 0.1f};
    const fuse::math::Vec3 snappedPos = fuse::editor::snapPosition(position, snap);
    expectNear(snappedPos.x, 1.5f, 0.001f, "snapPosition rounds X");
    expectNear(snappedPos.y, -2.f, 0.001f, "snapPosition rounds Y");
    expectNear(snappedPos.z, 0.f, 0.001f, "snapPosition rounds Z");

    const fuse::math::Vec3 passthroughPos = fuse::editor::snapPosition(position, snap);
    expectNear(passthroughPos.x, position.x, 0.001f, "snapPosition passthrough when disabled");

    const fuse::math::Vec3 euler{0.4f, 0.2f, 0.6f};
    snap.rotateSnap = false;
    const fuse::math::Vec3 snappedEuler = fuse::editor::snapEulerRadians(euler, snap);
    expectNear(snappedEuler.x, euler.x, 0.001f, "snapEulerRadians passthrough X when rotate snap off");

    const fuse::math::Vec3 snappedRotate = fuse::editor::snapEulerRadians(euler, snap);
    expectNear(snappedRotate.x, 0.5235988f, 0.01f, "snapEulerRadians snaps X to angle step");

    const fuse::math::Vec3 scale{1.37f, 0.88f, 2.01f};
    const fuse::math::Vec3 snappedScale = fuse::editor::snapScaleVec(scale, snap);
    expectNear(snappedScale.x, 1.25f, 0.001f, "snapScaleVec rounds X");
    expectNear(snappedScale.y, 1.f, 0.001f, "snapScaleVec rounds Y");
    expectNear(snappedScale.z, 2.f, 0.001f, "snapScaleVec rounds Z");

void testPreflightBeginDragGuards() {

    emptyHit.viewportHeight = 0.f;
    const fuse::editor::BeginDragPreflight emptyScreen =
        gizmo.preflightBeginDrag(emptyHit, transform);
    expectTrue(emptyScreen.emptyInput, "preflight reports empty screen input");
    expectTrue(!emptyScreen.canBegin, "preflight rejects empty screen input");

    const fuse::editor::BeginDragPreflight missScreen =
        gizmo.preflightBeginDrag(deadZone, transform);
    expectTrue(missScreen.pickMiss, "preflight reports screen pick miss");
    expectTrue(!missScreen.canBegin, "preflight rejects dead-zone hit");

    const fuse::editor::BeginDragPreflight okScreen = gizmo.preflightBeginDrag(hit, transform);
    expectTrue(okScreen.canBegin, "preflight accepts valid screen hit");
    expectTrue(okScreen.axis == fuse::editor::GizmoAxis::X, "preflight records picked axis");
    expectTrue(!gizmo.isDragging(), "preflight does not start drag");

    const fuse::editor::BeginDragPreflight emptyRayResult =
        gizmo.preflightBeginDrag(emptyRay, transform);
    expectTrue(emptyRayResult.emptyInput, "preflight reports empty ray");
    expectTrue(!emptyRayResult.canBegin, "preflight rejects empty ray");

    const fuse::editor::BeginDragPreflight okRay = gizmo.preflightBeginDrag(xRay, transform);
    expectTrue(okRay.canBegin, "preflight accepts valid ray");
    expectTrue(okRay.axis == fuse::editor::GizmoAxis::X, "preflight ray records axis");

void testCancelDragAndTryUpdateDrag() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    gizmo.updateDrag(hit);
    expectTrue(gizmo.tryEndDrag(result), "tryEndDrag accepts active drag");
    expectTrue(result.changed, "tryEndDrag marks result changed");
    expectTrue(!result.active, "tryEndDrag deactivates drag");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears dragging state");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryEndDrag records active axis");
}

void testIsSnapDegraded() {
    fuse::editor::GizmoSnapSettings snap{};
    expectTrue(!fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded false when snap disabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded true when translate snap enabled with zero grid");

               "isSnapDegraded false when translate snap has valid step");

void testPickInteractionPreflight() {

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickInteractionPreflight validRay =
        fuse::editor::preflightPickInteraction(xRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(validRay.canPick(), "pick interaction accepts valid ray");
    expectTrue(validRay.snapWillApply(), "pick interaction reports snap will apply");
    expectTrue(validRay.pick.axis == fuse::editor::GizmoAxis::X,
               "pick interaction resolves ray axis");

    fuse::editor::GizmoRay emptyRay{};
    const fuse::editor::PickInteractionPreflight emptyRayPick =
        fuse::editor::preflightPickInteraction(emptyRay, transform, fuse::editor::GizmoMode::Translate,
    expectTrue(!emptyRayPick.canPick(), "pick interaction rejects empty ray");
    expectTrue(emptyRayPick.pick.emptyRay, "pick interaction marks empty ray");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    const fuse::editor::PickInteractionPreflight screenPick = gizmo.preflightPickInteraction(hit);
    expectTrue(screenPick.canPick(), "gizmo pick interaction accepts valid screen hit");
    expectTrue(gizmo.canPickInteraction(hit), "gizmo canPickInteraction accepts valid screen hit");
    expectTrue(gizmo.canPickInteraction(xRay, transform),
               "gizmo canPickInteraction accepts valid ray");

void testBeginDragInteractionPreflight() {


    const fuse::editor::BeginDragInteractionPreflight validBegin =
        fuse::editor::preflightBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validBegin.canBegin(), "begin interaction accepts valid screen hit");
    expectTrue(validBegin.snapDegraded, "begin interaction marks snap degraded with invalid step");
    expectTrue(!validBegin.snap.canApply(), "begin interaction snap preflight rejects invalid step");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::canBeginDragInteraction(emptyHit, fuse::editor::GizmoMode::Translate,
                                                    snap),
               "canBeginDragInteraction rejects empty viewport");

    expectTrue(gizmo.canBeginDragInteraction(hit),
               "gizmo canBeginDragInteraction accepts valid screen hit");

    const fuse::editor::BeginDragInteractionPreflight rayBegin =
        gizmo.preflightBeginDragInteraction(xRay, transform);
    expectTrue(rayBegin.canBegin(), "gizmo begin interaction accepts valid ray");
    expectTrue(gizmo.canBeginDragInteraction(xRay, transform),
               "gizmo canBeginDragInteraction accepts valid ray");

void testUpdateDragInteractionPreflight() {


    const fuse::editor::UpdateDragInteractionPreflight inactive =
        fuse::editor::preflightUpdateDragInteraction(hit, false, fuse::editor::GizmoAxis::None,
    expectTrue(!inactive.canUpdate(), "update interaction rejects inactive drag");
    expectTrue(inactive.drag.notDragging, "update interaction marks inactive drag");


    const fuse::editor::UpdateDragInteractionPreflight active =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(active.canUpdate(), "update interaction accepts active drag");
    expectTrue(active.snapWillApply(), "update interaction reports snap will apply");

    expectTrue(!gizmo.canUpdateDragInteraction(hit),
               "gizmo canUpdateDragInteraction rejects empty viewport");
    gizmo.endDrag();

void testEndDragInteractionPreflight() {

    const fuse::editor::EndDragInteractionPreflight inactive =
        fuse::editor::preflightEndDragInteraction(false, fuse::editor::GizmoAxis::None,
    expectTrue(!inactive.canEnd(), "end interaction rejects inactive drag");
    expectTrue(inactive.end.notDragging, "end interaction marks inactive drag");

    const fuse::editor::EndDragInteractionPreflight active =
        fuse::editor::preflightEndDragInteraction(true, fuse::editor::GizmoAxis::X,
    expectTrue(active.canEnd(), "end interaction accepts active drag");
    expectTrue(active.snapDegraded, "end interaction marks snap degraded with invalid step");

    expectTrue(!gizmo.canEndDragInteraction(), "gizmo canEndDragInteraction rejects inactive drag");

    expectTrue(gizmo.canEndDragInteraction(), "gizmo canEndDragInteraction accepts active drag");
    expectTrue(gizmo.preflightEndDragInteraction().canEnd(),
               "gizmo end interaction preflight accepts active drag");

void testDragInteractionPreflight() {


    const fuse::editor::DragInteractionPreflight inactive =
        fuse::editor::preflightDragInteraction(hit, false, fuse::editor::GizmoAxis::None,
    expectTrue(inactive.notDragging, "drag interaction marks inactive session");
    expectTrue(!inactive.canInteract(), "drag interaction rejects when not dragging");


    const fuse::editor::DragInteractionPreflight active = gizmo.preflightDragInteraction(hit);
    expectTrue(!active.notDragging, "drag interaction clears notDragging while active");
    expectTrue(active.canUpdate(), "drag interaction can update on valid hit");
    expectTrue(active.canEnd(), "drag interaction can end while dragging");
    expectTrue(active.canInteract(), "drag interaction can interact while dragging");
    expectTrue(gizmo.canDragInteraction(hit), "gizmo canDragInteraction accepts active drag");

    const fuse::editor::DragInteractionPreflight emptyHit = gizmo.preflightDragInteraction(hit);
    expectTrue(!emptyHit.canUpdate(), "drag interaction update blocked on empty viewport");
    expectTrue(emptyHit.canEnd(), "drag interaction end still allowed on empty viewport");
    expectTrue(emptyHit.canInteract(), "drag interaction can still end on empty viewport");
    expectTrue(gizmo.canDragInteraction(hit),
               "gizmo canDragInteraction true when end path remains valid");


    expectTrue(!gizmo.isDragging(), "cancelDrag ends active drag");
    expectTrue(!gizmo.transformDirty(), "cancelDrag does not mark transform dirty");
    expectTrue(!editorState.sceneModified, "cancelDrag does not mark scene modified");
    expectTrue(!commandStack.isDirty(), "cancelDrag does not mark command stack dirty");
    transform.posX = 1.f;
    expectTrue(gizmo.isDragging(), "drag active before cancel");

    expectTrue(!gizmo.isDragging(), "cancelDrag stops active drag");

    expectTrue(gizmo.isDragging(), "beginDrag restarts after cancel");
    fuse::editor::GizmoResult updateResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, updateResult),
               "tryUpdateDrag rejects empty viewport");
    expectTrue(!updateResult.changed, "empty viewport update leaves result unchanged");
    expectTrue(gizmo.isDragging(), "empty viewport tryUpdateDrag keeps drag active");

    expectTrue(gizmo.tryUpdateDrag(hit, updateResult), "tryUpdateDrag accepts valid viewport");
    expectTrue(updateResult.changed, "valid tryUpdateDrag reports change");
    const fuse::editor::GizmoBeginDragPreflight draggingPreflight = gizmo.preflightBeginDrag(hit, transform);
    expectTrue(draggingPreflight.alreadyDragging, "preflight marks active drag");
    expectTrue(!draggingPreflight.canBegin(), "preflight blocks begin while dragging");

    const fuse::editor::GizmoBeginDragPreflight snapInvalidPreflight =
        gizmo.preflightBeginDrag(xRay, transform);
    expectTrue(snapInvalidPreflight.snapInvalid, "preflight marks invalid snap settings");
    expectTrue(snapInvalidPreflight.canBegin(),
               "invalid snap settings do not block begin-drag preflight");

void testTryBeginDragRejectsWhileDragging() {

    expectTrue(gizmo.tryBeginDrag(hit, transform, result), "first begin-drag succeeds");
    expectTrue(gizmo.isDragging(), "drag active after first begin");

               "tryBeginDrag rejects re-entrant begin while dragging");
    expectTrue(gizmo.isDragging(), "re-entrant begin leaves drag active");
}

void testRayNormalizeAndValidityGuards() {
    fuse::editor::GizmoRay emptyRay{};
    expectTrue(fuse::editor::isRayEmpty(emptyRay), "zero-direction ray is empty");
    expectTrue(!fuse::editor::isRayValid(emptyRay), "isRayValid rejects empty ray");
    expectTrue(!fuse::editor::normalizeRay(emptyRay), "normalizeRay rejects empty ray");

    fuse::editor::GizmoRay ray{};
    ray.direction = {3.f, 0.f, 4.f};
    expectTrue(fuse::editor::normalizeRay(ray), "normalizeRay succeeds on non-empty ray");
    expectNear(ray.direction.x, 0.6f, 0.001f, "normalizeRay normalizes X");
    expectNear(ray.direction.z, 0.8f, 0.001f, "normalizeRay normalizes Z");
    expectTrue(fuse::editor::isRayValid(ray), "normalized ray is valid");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::isHitTestValid(emptyHit), "isHitTestValid rejects empty viewport");

    fuse::editor::GizmoHitTest validHit{};
    validHit.viewportWidth = 100.f;
    validHit.viewportHeight = 100.f;
    expectTrue(fuse::editor::isHitTestValid(validHit), "isHitTestValid accepts valid viewport");
}

void testSnapVecHelpers() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;
    snap.rotateSnap = true;
    snap.angleStepDegrees = 15.f;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;

    const fuse::math::Vec3 position{1.37f, -2.24f, 0.1f};
    const fuse::math::Vec3 snappedPos = fuse::editor::snapPosition(position, snap);
    expectNear(snappedPos.x, 1.5f, 0.001f, "snapPosition rounds X");
    expectNear(snappedPos.y, -2.f, 0.001f, "snapPosition rounds Y");
    expectNear(snappedPos.z, 0.f, 0.001f, "snapPosition rounds Z");

    snap.translateSnap = false;
    const fuse::math::Vec3 passthroughPos = fuse::editor::snapPosition(position, snap);
    expectNear(passthroughPos.x, position.x, 0.001f, "snapPosition passthrough when disabled");

    const fuse::math::Vec3 euler{0.4f, 0.2f, 0.6f};
    snap.rotateSnap = false;
    const fuse::math::Vec3 snappedEuler = fuse::editor::snapEulerRadians(euler, snap);
    expectNear(snappedEuler.x, euler.x, 0.001f, "snapEulerRadians passthrough X when rotate snap off");

    snap.rotateSnap = true;
    const fuse::math::Vec3 snappedRotate = fuse::editor::snapEulerRadians(euler, snap);
    expectNear(snappedRotate.x, 0.5235988f, 0.01f, "snapEulerRadians snaps X to angle step");

    const fuse::math::Vec3 scale{1.37f, 0.88f, 2.01f};
    const fuse::math::Vec3 snappedScale = fuse::editor::snapScaleVec(scale, snap);
    expectNear(snappedScale.x, 1.25f, 0.001f, "snapScaleVec rounds X");
    expectNear(snappedScale.y, 1.f, 0.001f, "snapScaleVec rounds Y");
    expectNear(snappedScale.z, 2.f, 0.001f, "snapScaleVec rounds Z");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::math::Vec3 invalidGridPos = fuse::editor::snapPosition(position, snap);
    expectNear(invalidGridPos.x, position.x, 0.001f,
               "snapPosition passthrough when snap step invalid");
}

void testUpdateDragPreflightGuards() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::UpdateDragPreflight idlePreflight = fuse::editor::preflightUpdateDrag(hit, false);
    expectTrue(idlePreflight.notDragging, "update preflight marks inactive drag");
    expectTrue(!idlePreflight.canUpdate, "update preflight rejects when not dragging");
    expectTrue(!fuse::editor::canUpdateDrag(hit, false), "canUpdateDrag rejects when not dragging");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects when not dragging");
    const fuse::editor::UpdateDragPreflight inactivePreflight =
        fuse::editor::preflightUpdateDrag(hit, false);
    expectTrue(inactivePreflight.notDragging, "update preflight marks inactive drag");
    expectTrue(!inactivePreflight.canUpdate(), "update preflight rejects inactive drag");
    expectTrue(!gizmo.preflightUpdateDrag(hit).canUpdate(),
               "gizmo update preflight rejects inactive drag");
    expectTrue(!fuse::editor::canUpdateDrag(hit, false),
               "canUpdateDrag rejects inactive drag");

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.isDragging(), "drag active for update preflight tests");
    const fuse::editor::UpdateDragPreflight inactivePreflight = fuse::editor::preflightUpdateDrag(
        hit, false, fuse::editor::GizmoMode::Translate);
    expectTrue(inactivePreflight.notDragging, "update preflight marks inactive drag");
    expectTrue(!inactivePreflight.canUpdate(), "update preflight rejects inactive drag");
    expectTrue(!fuse::editor::canUpdateDrag(hit, false),
               "canUpdateDrag rejects inactive drag");
    expectTrue(!gizmo.preflightUpdateDrag(hit).canUpdate(),
               "gizmo update preflight rejects inactive drag");
    expectTrue(!fuse::editor::canUpdateDrag(hit, false, fuse::editor::GizmoMode::Translate),
    expectTrue(!fuse::editor::canUpdateDrag(hit, false),
               "canUpdateDrag rejects inactive drag");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects inactive drag");

    const fuse::editor::UpdateDragPreflight activePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(activePreflight.canUpdate(), "update preflight accepts active drag with valid hit");
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag with valid hit");

    fuse::editor::GizmoHitTest deadZone = hit;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    const fuse::editor::UpdateDragPreflight screenMissPreflight = gizmo.preflightUpdateDrag(deadZone);
    expectTrue(screenMissPreflight.screenMiss,
               "update preflight marks translate dead zone as diagnostic screenMiss");
    expectTrue(screenMissPreflight.canUpdate(),
               "update preflight still allows dead-zone hit to preserve drag behavior");

    fuse::editor::GizmoResult tryUpdateResult{};
    expectTrue(gizmo.tryUpdateDrag(deadZone, tryUpdateResult),
               "tryUpdateDrag accepts dead-zone hit while drag is active");
    expectTrue(tryUpdateResult.changed, "tryUpdateDrag applies dead-zone update");
    expectTrue(fuse::editor::canUpdateDrag(hit, true),
    expectTrue(fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoMode::Translate),
               "canUpdateDrag accepts active drag with valid hit");

    hit.viewportWidth = 0.f;
    const fuse::editor::UpdateDragPreflight emptyPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(emptyPreflight.emptyHit, "update preflight marks empty viewport");
    expectTrue(!emptyPreflight.canUpdate, "update preflight rejects empty viewport");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects empty viewport");
    expectTrue(!emptyPreflight.canUpdate(), "update preflight rejects empty viewport");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects empty viewport");

    fuse::editor::GizmoResult tryRejectResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, tryRejectResult),
               "tryUpdateDrag rejects empty viewport");
    expectTrue(!tryRejectResult.changed, "tryUpdateDrag leaves result unchanged on reject");


    fuse::editor::GizmoResult updateResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, updateResult),
    expectTrue(!updateResult.changed, "empty viewport tryUpdateDrag leaves result unchanged");
    expectTrue(gizmo.isDragging(), "empty viewport tryUpdateDrag keeps drag active");

    hit.viewportWidth = 100.f;
    hit.screenX = 30.f;
    const fuse::editor::UpdateDragPreflight okPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(okPreflight.canUpdate, "update preflight accepts valid viewport");
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts valid viewport");
    expectTrue(gizmo.tryUpdateDrag(hit, updateResult), "tryUpdateDrag accepts valid viewport");
    expectTrue(updateResult.changed, "valid tryUpdateDrag reports change");
    gizmo.endDrag();
}

void testCancelDragAndTryUpdateDrag() {
void testCanUpdateDragGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(!fuse::editor::canUpdateDrag(hit, false),
               "canUpdateDrag rejects inactive drag");
    expectTrue(fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag accepts active drag with valid hit");

    hit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::canUpdateDrag(hit, true),
               "canUpdateDrag rejects empty viewport while dragging");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoMode::Translate),
               "canUpdateDrag accepts active drag with valid hit");

    expectTrue(!fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoMode::Translate),

    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects inactive drag");

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag with valid hit");


void testTryUpdateDragGuards() {

    transform.posX = 1.f;
    expectTrue(gizmo.isDragging(), "drag active before cancel");

    gizmo.updateDrag(hit);
    gizmo.cancelDrag();
    expectTrue(!gizmo.isDragging(), "cancelDrag stops active drag");
    expectTrue(!gizmo.transformDirty(), "cancelDrag does not mark transform dirty");

    expectTrue(gizmo.isDragging(), "beginDrag restarts after cancel");

void testPickPreflight() {

    fuse::editor::GizmoRay emptyRay{};
    const fuse::editor::PickPreflight emptyRayPreflight = fuse::editor::preflightPickAxis(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(emptyRayPreflight.emptyRay, "pick preflight marks empty ray");
    expectTrue(!emptyRayPreflight.canPick, "pick preflight rejects empty ray");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickPreflight validRayPreflight = fuse::editor::preflightPickAxis(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(validRayPreflight.canPick, "pick preflight accepts valid ray");
    expectTrue(validRayPreflight.axis == fuse::editor::GizmoAxis::X,
               "pick preflight records picked axis");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    const fuse::editor::PickPreflight emptyHitPreflight =
        fuse::editor::preflightPickAxis(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyHitPreflight.emptyHit, "pick preflight marks empty viewport");
    expectTrue(!emptyHitPreflight.canPick, "pick preflight rejects empty viewport");
void testShouldSkipPickGuards() {

    expectTrue(fuse::editor::shouldSkipPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                            fuse::editor::GizmoSpace::World,
                                            fuse::editor::GizmoSystem::kAxisLength,
                                            fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipPick rejects empty ray");

    expectTrue(!fuse::editor::shouldSkipPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "shouldSkipPick accepts valid ray");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    const fuse::editor::PickPreflight screenMissPreflight =
        fuse::editor::preflightPickAxis(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(screenMissPreflight.screenMiss, "pick preflight marks translate dead zone");
    expectTrue(!screenMissPreflight.canPick, "pick preflight rejects translate dead zone");
    expectTrue(fuse::editor::shouldSkipPick(deadZone, fuse::editor::GizmoMode::Translate),
               "shouldSkipPick rejects translate dead zone");

    deadZone.screenX = 10.f;
    const fuse::editor::PickPreflight gizmoPreflight = gizmo.preflightPickAxis(deadZone);
    expectTrue(gizmoPreflight.canPick, "gizmo pick preflight accepts valid screen hit");
    expectTrue(gizmoPreflight.axis == fuse::editor::GizmoAxis::X,
               "gizmo pick preflight records axis");

void testSnapPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = false;
    const fuse::editor::SnapPreflight disabledPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap preflight marks disabled snap");
    expectTrue(!disabledPreflight.canApply, "snap preflight rejects when snap disabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::SnapPreflight invalidPreflight =
    expectTrue(invalidPreflight.invalidStep, "snap preflight marks invalid step");
    expectTrue(!invalidPreflight.canApply, "snap preflight rejects invalid step");

    snap.gridSize = 0.5f;
    const fuse::editor::SnapPreflight validPreflight =
    expectTrue(validPreflight.canApply, "snap preflight accepts valid translate snap");
    expectNear(validPreflight.step, 0.5f, 0.001f, "snap preflight records step");

    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnap().canApply, "gizmo snap preflight accepts valid settings");

void testUpdateDragPreflight() {

    const fuse::editor::UpdateDragPreflight idlePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(idlePreflight.notDragging, "update preflight marks idle drag");
    expectTrue(!idlePreflight.canUpdate, "update preflight rejects when not dragging");
    expectTrue(!fuse::editor::canUpdateDrag(hit, fuse::editor::GizmoMode::Translate, false,
                                            fuse::editor::GizmoAxis::X),
               "canUpdateDrag rejects when not dragging");

    const fuse::editor::UpdateDragPreflight activePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(activePreflight.canUpdate, "update preflight accepts active drag");
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag");

    const fuse::editor::UpdateDragPreflight emptyPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(emptyPreflight.emptyHit, "update preflight marks empty viewport");
    expectTrue(!emptyPreflight.canUpdate, "update preflight rejects empty viewport");

               "tryUpdateDrag rejects empty viewport without ending drag");
    expectTrue(!updateResult.changed, "tryUpdateDrag leaves result unchanged on reject");
    expectTrue(gizmo.isDragging(), "tryUpdateDrag reject keeps drag active");

    hit.screenX = 40.f;
    expectTrue(gizmo.tryUpdateDrag(hit, updateResult), "tryUpdateDrag accepts valid hit");
    expectTrue(updateResult.changed, "tryUpdateDrag marks result changed");

void testUpdateDragSnapSkippedPreflight() {




    const fuse::editor::UpdateDragPreflight preflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(preflight.canUpdate, "update preflight still allows drag when snap step invalid");
    expectTrue(preflight.snapSkipped, "update preflight marks snap skipped for invalid step");


    const fuse::editor::PickPreflight emptyRayPreflight = fuse::editor::preflightPick(

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    const fuse::editor::PickPreflight missPreflight = fuse::editor::preflightPick(
        missRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(missPreflight.pickMiss, "pick preflight marks ray pick miss");
    expectTrue(!missPreflight.canPick, "pick preflight rejects ray pick miss");

    const fuse::editor::PickPreflight validRayPreflight = fuse::editor::preflightPick(
    expectTrue(validRayPreflight.canPick, "pick preflight accepts valid ray pick");
    expectTrue(!validRayPreflight.emptyRay, "valid ray pick preflight clears emptyRay");
    expectTrue(!validRayPreflight.pickMiss, "valid ray pick preflight clears pickMiss");

    const fuse::editor::PickPreflight invalidConfigPreflight = fuse::editor::preflightPick(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World, 0.f,
        fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(invalidConfigPreflight.invalidPickConfig,
               "pick preflight marks invalid axis length / pick radius");
    expectTrue(!invalidConfigPreflight.canPick, "pick preflight rejects invalid pick config");

        fuse::editor::preflightPick(emptyHit, fuse::editor::GizmoMode::Translate);

        fuse::editor::preflightPick(deadZone, fuse::editor::GizmoMode::Translate);

    const fuse::editor::PickPreflight validHitPreflight =
    expectTrue(validHitPreflight.canPick, "pick preflight accepts valid screen hit");

    expectTrue(gizmo.preflightPick(xRay, transform).canPick, "gizmo pick preflight accepts valid ray");
    expectTrue(gizmo.preflightPick(deadZone).canPick, "gizmo pick preflight accepts valid screen hit");
    expectTrue(gizmo.preflightPick(deadZone).screenMiss,
               "gizmo pick preflight marks translate dead zone");



    const fuse::editor::SnapPreflight invalidStepPreflight =
    expectTrue(invalidStepPreflight.invalidStep, "snap preflight marks zero grid step");
    expectTrue(!invalidStepPreflight.canApply, "snap preflight rejects invalid step");

    expectTrue(validPreflight.canApply, "snap preflight accepts enabled snap with valid step");
    expectTrue(!validPreflight.snapDisabled, "valid snap preflight clears snapDisabled");
    expectTrue(!validPreflight.invalidStep, "valid snap preflight clears invalidStep");

    snap.rotateSnap = true;
    snap.angleStepDegrees = 0.f;
    expectTrue(fuse::editor::preflightSnap(fuse::editor::GizmoMode::Rotate, snap).invalidStep,
               "snap preflight marks zero angle step for rotate");

    gizmo.setMode(fuse::editor::GizmoMode::Rotate);
    expectTrue(gizmo.preflightSnap().invalidStep, "gizmo snap preflight uses active mode");
    gizmo.setMode(fuse::editor::GizmoMode::Translate);
    expectTrue(gizmo.preflightSnap().canApply, "gizmo snap preflight accepts translate snap");


    const fuse::editor::UpdateDragPreflight inactivePreflight =
        fuse::editor::preflightUpdateDrag(hit, false);
    expectTrue(inactivePreflight.notDragging, "update-drag preflight marks inactive drag");
    expectTrue(!inactivePreflight.canUpdate, "update-drag preflight rejects when not dragging");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects when not dragging");

    expectTrue(gizmo.preflightUpdateDrag(hit).notDragging == false,
               "update-drag preflight clears notDragging while active");

    expectTrue(emptyPreflight.emptyHit, "update-drag preflight marks empty viewport");
    expectTrue(!emptyPreflight.canUpdate, "update-drag preflight rejects empty viewport");
    expectTrue(gizmo.isDragging(), "empty viewport update keeps drag active");

    const fuse::editor::UpdateDragPreflight validPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(validPreflight.canUpdate, "update-drag preflight accepts valid hit while dragging");
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts valid hit while dragging");

void testUpdateDragSnapDegradedPreflight() {


    const fuse::editor::UpdateDragPreflight inactivePreflight = fuse::editor::preflightUpdateDrag(
        hit, false, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactivePreflight.canUpdate(), "snap-degraded preflight rejects inactive drag");
    expectTrue(!inactivePreflight.snapDegraded,
               "inactive drag preflight does not mark snap degraded");

    const fuse::editor::UpdateDragPreflight degradedPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canUpdate(),
               "snap-degraded preflight still allows update when dragging");
    expectTrue(degradedPreflight.snapDegraded,
               "snap-degraded preflight marks invalid snap step during drag");

    const fuse::editor::UpdateDragPreflight healthyPreflight = fuse::editor::preflightUpdateDrag(
    expectTrue(healthyPreflight.canUpdate(), "healthy snap preflight allows update");
    expectTrue(!healthyPreflight.snapDegraded, "healthy snap preflight clears snapDegraded");

    const fuse::editor::UpdateDragPreflight gizmoPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmoPreflight.canUpdate(), "gizmo preflight allows update with degraded snap");
    expectTrue(gizmoPreflight.snapDegraded, "gizmo preflight marks degraded snap settings");
    expectTrue(!gizmo.shouldSkipPick(deadZone), "gizmo shouldSkipPick accepts valid screen hit");
    expectTrue(!gizmo.shouldSkipPick(xRay, transform), "gizmo shouldSkipPick accepts valid ray");

void testShouldSkipSnapGuards() {

    expectTrue(fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap rejects disabled snap");

               "shouldSkipSnap rejects invalid step");

    expectTrue(!fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap accepts enabled snap with valid step");

    expectTrue(!gizmo.shouldSkipSnap(), "gizmo shouldSkipSnap accepts valid settings");


    expectTrue(fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag accepts active drag");

    expectTrue(!fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag rejects empty viewport");
    expectTrue(!fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoMode::Translate, {}),
               "canUpdateDrag overload rejects empty viewport");

    expectTrue(!fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoAxis::None),
               "canUpdateDrag rejects missing active axis");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, true, fuse::editor::GizmoAxis::None),
               "shouldSkipUpdateDrag marks missing active axis");

    const fuse::editor::UpdateDragPreflight noAxisPreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::None);
    expectTrue(noAxisPreflight.noActiveAxis, "update preflight marks missing active axis");
    expectTrue(!noAxisPreflight.canUpdate(), "update preflight rejects missing active axis");

    expectTrue(fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "canUpdateDrag accepts active drag with axis");
    expectTrue(!fuse::editor::canUpdateDrag(hit, false), "canUpdateDrag rejects inactive drag");


    expectTrue(!gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag false while dragging");

    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for empty viewport");


    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryUpdateDrag leaves result unchanged");


    expectTrue(gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag accepts active drag");
    expectTrue(result.changed, "tryUpdateDrag marks result changed");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryUpdateDrag preserves active axis");
    expectTrue(result.transform.posX != 1.f, "tryUpdateDrag applies drag delta");
    expectTrue(result.changed, "active tryUpdateDrag marks result changed");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryUpdateDrag records active axis");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects empty viewport");

void testRayNormalizeAndValidityGuards() {
    expectTrue(fuse::editor::isRayEmpty(emptyRay), "zero-direction ray is empty");
    expectTrue(!fuse::editor::isRayValid(emptyRay), "isRayValid rejects empty ray");
    expectTrue(!fuse::editor::normalizeRay(emptyRay), "normalizeRay rejects empty ray");

    fuse::editor::GizmoRay ray{};
    ray.direction = {3.f, 0.f, 4.f};
    expectTrue(fuse::editor::normalizeRay(ray), "normalizeRay succeeds on non-empty ray");
    expectNear(ray.direction.x, 0.6f, 0.001f, "normalizeRay normalizes X");
    expectNear(ray.direction.z, 0.8f, 0.001f, "normalizeRay normalizes Z");
    expectTrue(fuse::editor::isRayValid(ray), "normalized ray is valid");

    expectTrue(!fuse::editor::isHitTestValid(emptyHit), "isHitTestValid rejects empty viewport");

    fuse::editor::GizmoHitTest validHit{};
    validHit.viewportWidth = 100.f;
    validHit.viewportHeight = 100.f;
    expectTrue(fuse::editor::isHitTestValid(validHit), "isHitTestValid accepts valid viewport");

void testSnapVecHelpersAndTrySnapValue() {
    snap.angleStepDegrees = 15.f;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;

    const fuse::math::Vec3 position{1.37f, -2.24f, 0.1f};
    const fuse::math::Vec3 snappedPos = fuse::editor::snapPosition(position, snap);
    expectNear(snappedPos.x, 1.5f, 0.001f, "snapPosition rounds X");
    expectNear(snappedPos.y, -2.f, 0.001f, "snapPosition rounds Y");
    expectNear(snappedPos.z, 0.f, 0.001f, "snapPosition rounds Z");

    const fuse::math::Vec3 passthroughPos = fuse::editor::snapPosition(position, snap);
    expectNear(passthroughPos.x, position.x, 0.001f, "snapPosition passthrough when disabled");

    const fuse::math::Vec3 euler{0.4f, 0.2f, 0.6f};
    snap.rotateSnap = false;
    const fuse::math::Vec3 snappedEuler = fuse::editor::snapEulerRadians(euler, snap);
    expectNear(snappedEuler.x, euler.x, 0.001f, "snapEulerRadians passthrough when rotate snap off");

    const fuse::math::Vec3 snappedRotate = fuse::editor::snapEulerRadians(euler, snap);
    expectNear(snappedRotate.x, 0.5235988f, 0.01f, "snapEulerRadians snaps X to angle step");

    const fuse::math::Vec3 scale{1.37f, 0.88f, 2.01f};
    const fuse::math::Vec3 snappedScale = fuse::editor::snapScaleVec(scale, snap);
    expectNear(snappedScale.x, 1.25f, 0.001f, "snapScaleVec rounds X");
    expectNear(snappedScale.y, 1.f, 0.001f, "snapScaleVec rounds Y");
    expectNear(snappedScale.z, 2.f, 0.001f, "snapScaleVec rounds Z");

    fuse::f32 out = 0.f;
    expectTrue(!fuse::editor::trySnapValue(1.37f, fuse::editor::GizmoMode::Translate, snap, out),
               "trySnapValue rejects disabled snap");
    expectNear(out, 1.37f, 0.001f, "trySnapValue leaves value unchanged on reject");

               "trySnapValue rejects invalid step");
    expectNear(out, 1.37f, 0.001f, "trySnapValue leaves value unchanged on invalid step");

    expectTrue(fuse::editor::trySnapValue(1.37f, fuse::editor::GizmoMode::Translate, snap, out),
               "trySnapValue applies when snap is valid");
    expectNear(out, 1.5f, 0.001f, "trySnapValue snaps scalar value");

void testUpdateDragPreflightDeepen() {

    const fuse::editor::UpdateDragPreflight noAxisPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoAxis::None);


    fuse::editor::GizmoHitTest deadZone = hit;
    const fuse::editor::UpdateDragPreflight screenMissPreflight = gizmo.preflightUpdateDrag(deadZone);
    expectTrue(screenMissPreflight.screenMiss, "update preflight marks translate dead zone");
    expectTrue(!screenMissPreflight.canUpdate(), "update preflight rejects translate dead zone");
    expectTrue(!gizmo.canUpdateDrag(deadZone), "canUpdateDrag rejects translate dead zone");

    expectTrue(!gizmo.tryUpdateDrag(deadZone, updateResult),
               "tryUpdateDrag rejects translate dead zone");
    expectTrue(!updateResult.changed, "dead-zone tryUpdateDrag leaves result unchanged");
    expectTrue(gizmo.isDragging(), "dead-zone tryUpdateDrag keeps drag active");

    expectTrue(gizmo.canUpdateDrag(hit), "canUpdateDrag accepts valid drag hit");
    expectTrue(gizmo.tryUpdateDrag(hit, updateResult), "tryUpdateDrag accepts valid drag hit");

void testEndDragPreflightAndCancelDrag() {
void testEndDragPreflightGuards() {

    const fuse::editor::EndDragPreflight inactivePreflight = fuse::editor::preflightEndDrag(false);
    expectTrue(inactivePreflight.notDragging, "end preflight marks inactive drag");
    expectTrue(!inactivePreflight.canEnd(), "end preflight rejects inactive drag");
    expectTrue(!gizmo.preflightEndDrag().canEnd(), "gizmo end preflight rejects inactive drag");
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects inactive drag");

    const fuse::editor::GizmoResult noopEnd = gizmo.endDrag();
    expectTrue(!noopEnd.changed, "endDrag on inactive drag is unchanged");


    expectTrue(gizmo.preflightEndDrag().canEnd(), "end preflight accepts active drag");
    expectTrue(gizmo.canEndDrag(), "canEndDrag accepts active drag");

    expectTrue(!gizmo.canEndDrag(), "canEndDrag rejects after cancel");

    expectTrue(gizmo.transformDirty(), "endDrag after cancel still marks dirty");
    expectTrue(!fuse::editor::canEndDrag(false), "canEndDrag rejects inactive drag");

    fuse::editor::GizmoResult tryResult{};
    expectTrue(!gizmo.tryEndDrag(tryResult), "tryEndDrag rejects when not dragging");
    expectTrue(!tryResult.changed, "tryEndDrag leaves result unchanged on reject");

    const fuse::editor::GizmoResult endResult = gizmo.endDrag();
    expectTrue(!endResult.changed, "endDrag no-op when not dragging");
    expectTrue(!gizmo.transformDirty(), "endDrag no-op does not mark transform dirty");
    expectTrue(gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag accepts active drag with valid hit");
    expectTrue(result.changed, "valid tryUpdateDrag marks result changed");
    expectTrue(result.active, "valid tryUpdateDrag keeps drag active");
    expectTrue(gizmo.isDragging(), "valid tryUpdateDrag keeps drag session active");

    result = {};
    expectTrue(!result.changed, "empty viewport tryUpdateDrag leaves result unchanged");
    expectTrue(gizmo.isDragging(), "empty viewport tryUpdateDrag keeps drag session active");


    const fuse::editor::EndDragPreflight activePreflight = fuse::editor::preflightEndDrag(true);
    expectTrue(activePreflight.canEnd(), "end preflight accepts active drag");
    expectTrue(!activePreflight.notDragging, "active end preflight clears notDragging");
    expectTrue(fuse::editor::canEndDrag(true), "canEndDrag accepts active drag");

    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(!update.changed, "updateDrag ignores empty viewport via preflight guard");

    fuse::editor::GizmoResult tryUpdateResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, tryUpdateResult),
    expectTrue(!tryUpdateResult.changed, "tryUpdateDrag leaves result unchanged on reject");


    const fuse::editor::EndDragPreflight inactivePreflight =
        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!fuse::editor::canEndDrag(false, fuse::editor::GizmoMode::Translate, snap),
               "canEndDrag rejects inactive drag");

    const fuse::editor::EndDragPreflight snapSkippedPreflight =
        fuse::editor::preflightEndDrag(true, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(snapSkippedPreflight.canEnd(), "end preflight allows end when snap step invalid");
    expectTrue(snapSkippedPreflight.snapSkipped,
               "end preflight marks snap skipped when step invalid");

    snap.gridSize = 1.f;
    const fuse::editor::EndDragPreflight validSnapPreflight =
    expectTrue(validSnapPreflight.canEnd(), "end preflight accepts active drag with valid snap");
    expectTrue(!validSnapPreflight.snapSkipped, "valid snap preflight clears snapSkipped");

    expectTrue(!gizmo.preflightEndDrag().canEnd(), "gizmo end preflight rejects inactive drag");
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects inactive drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightEndDrag().canEnd(), "gizmo end preflight accepts active drag");
    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag accepts active drag");
    expectTrue(fuse::editor::preflightEndDrag(true).canEnd(),
               "end preflight accepts active drag");
    expectTrue(fuse::editor::canEndDrag(true), "canEndDrag accepts active drag");

    expectTrue(gizmo.tryEndDrag(tryResult), "tryEndDrag succeeds on active drag");
    expectTrue(tryResult.changed, "tryEndDrag marks result changed on success");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears drag state");

    const fuse::editor::GizmoResult ended = gizmo.endDrag();
    expectTrue(ended.changed, "endDrag commits active drag");
    expectTrue(!gizmo.isDragging(), "endDrag clears drag state");
}

void testSnapPreflightNoChange() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoTransform onGrid{};
    onGrid.posX = 2.f;
    onGrid.posY = -3.f;
    onGrid.posZ = 0.f;

    const fuse::editor::SnapPreflight onGridPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap, onGrid);
    expectTrue(onGridPreflight.canApply(), "snap preflight accepts enabled snap with valid step");
    expectTrue(onGridPreflight.noChange, "snap preflight marks transform already on grid");
    expectTrue(!onGridPreflight.wouldSnap(), "wouldSnap false when transform already on grid");

    fuse::editor::GizmoTransform offGrid = onGrid;
    offGrid.posX = 1.6f;
    const fuse::editor::SnapPreflight offGridPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap, offGrid);
    expectTrue(offGridPreflight.canApply(), "snap preflight accepts off-grid transform");
    expectTrue(!offGridPreflight.noChange, "snap preflight clears noChange for off-grid transform");
    expectTrue(offGridPreflight.wouldSnap(), "wouldSnap true when transform needs snapping");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnap(onGrid).noChange,
               "gizmo snap preflight marks on-grid transform");
    expectTrue(!gizmo.preflightSnap(offGrid).noChange,
               "gizmo snap preflight clears noChange for off-grid transform");
    gizmo.endDrag();

void testTryEndDragGuards() {

    fuse::editor::GizmoResult result{};

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryEndDrag leaves result unchanged");
    expectTrue(!gizmo.isDragging(), "inactive tryEndDrag keeps drag inactive");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    hit.screenX = 30.f;
    gizmo.updateDrag(hit);

    expectTrue(gizmo.tryEndDrag(result), "tryEndDrag accepts active drag");
    expectTrue(result.changed, "valid tryEndDrag marks result changed");
    expectTrue(!result.active, "valid tryEndDrag deactivates drag");
    expectTrue(!gizmo.isDragging(), "valid tryEndDrag ends drag session");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "valid tryEndDrag preserves axis");

    result = {};
    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects after drag already ended");
    expectTrue(!result.changed, "post-end tryEndDrag leaves result unchanged");

    fuse::editor::GizmoResult tryEndResult{};
    expectTrue(gizmo.tryEndDrag(tryEndResult), "tryEndDrag succeeds on active drag");
    expectTrue(tryEndResult.changed, "tryEndDrag marks result changed");
    expectTrue(!tryEndResult.active, "tryEndDrag deactivates drag");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears dragging state");

    expectTrue(!gizmo.tryEndDrag(tryEndResult), "tryEndDrag rejects inactive drag");
    expectTrue(!tryEndResult.changed, "inactive tryEndDrag leaves result unchanged");

    const fuse::editor::GizmoResult end = gizmo.endDrag();
    expectTrue(end.changed, "endDrag applies via tryEndDrag guard");
    expectTrue(!gizmo.isDragging(), "endDrag clears dragging state");
}

void testEndDragPreflightGuards() {
    fuse::editor::GizmoSnapSettings snap{};

    const fuse::editor::EndDragPreflight inactivePreflight =
        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(inactivePreflight.notDragging, "end preflight marks inactive drag");
    expectTrue(!inactivePreflight.canEnd(), "end preflight rejects inactive drag");
    expectTrue(!fuse::editor::canEndDrag(false), "canEndDrag rejects inactive drag");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.preflightEndDrag().canEnd(), "gizmo end preflight rejects inactive drag");
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects inactive drag");

    fuse::editor::GizmoResult endResult{};
    expectTrue(!gizmo.tryEndDrag(endResult), "tryEndDrag rejects inactive drag");
    expectTrue(!endResult.changed, "tryEndDrag leaves result unchanged on reject");
    expectTrue(!endResult.active, "tryEndDrag leaves drag inactive on reject");

    const fuse::editor::EndDragPreflight disabledSnapPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledSnapPreflight.canEnd(), "end preflight accepts active drag");
    expectTrue(disabledSnapPreflight.snapDisabled, "end preflight marks disabled snap");
    expectTrue(!disabledSnapPreflight.invalidSnapStep,
               "end preflight clears invalidSnapStep when snap disabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::EndDragPreflight invalidStepPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidStepPreflight.canEnd(), "end preflight accepts active drag with invalid snap step");
    expectTrue(invalidStepPreflight.invalidSnapStep, "end preflight marks invalid snap step");
    expectTrue(!invalidStepPreflight.snapDisabled,
               "end preflight clears snapDisabled when snap enabled");

    snap.gridSize = 1.f;
    gizmo.setSnapSettings(snap);

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    const fuse::editor::EndDragPreflight activePreflight = gizmo.preflightEndDrag();
    expectTrue(activePreflight.canEnd(), "gizmo end preflight accepts active drag");
    expectTrue(!activePreflight.snapDisabled, "gizmo end preflight clears snapDisabled");
    expectTrue(!activePreflight.invalidSnapStep, "gizmo end preflight clears invalidSnapStep");
    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag accepts active drag");

    hit.screenX = 30.f;
    gizmo.updateDrag(hit);
    expectTrue(gizmo.tryEndDrag(endResult), "tryEndDrag accepts active drag");
    expectTrue(!endResult.active, "tryEndDrag marks drag inactive");
    expectTrue(endResult.changed, "tryEndDrag reports transform change");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears dragging state");

    expectTrue(!gizmo.endDrag().changed, "endDrag no-op after tryEndDrag");
}

void testCanUpdateDragGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(!fuse::editor::canUpdateDrag(hit, false),
               "canUpdateDrag rejects inactive drag");
    expectTrue(fuse::editor::canUpdateDrag(hit, true),
               "canUpdateDrag accepts active drag with valid hit");

    hit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::canUpdateDrag(hit, true),
               "canUpdateDrag rejects empty viewport");

    fuse::editor::GizmoSystem gizmo;
    hit.viewportWidth = 100.f;
    gizmo.beginDrag(hit, fuse::editor::GizmoTransform{});
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag");
    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects empty viewport");
    gizmo.endDrag();
}

void testTryUpdateDragGuards() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryUpdateDrag leaves result unchanged");

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    hit.screenX = 30.f;
    expectTrue(gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag accepts active drag");
    expectTrue(result.changed, "active tryUpdateDrag marks result changed");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryUpdateDrag preserves active axis");

    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects empty viewport");
    expectTrue(gizmo.isDragging(), "empty viewport tryUpdateDrag keeps drag active");
    gizmo.endDrag();
}

void testEndDragPreflightGuards() {
    const fuse::editor::EndDragPreflight inactivePreflight = fuse::editor::preflightEndDrag(false);
    expectTrue(inactivePreflight.notDragging, "end preflight marks inactive drag");
    expectTrue(!inactivePreflight.canEnd, "end preflight rejects inactive drag");
    expectTrue(!inactivePreflight.canEnd(), "end preflight rejects inactive drag");

    const fuse::editor::EndDragPreflight activePreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activePreflight.canEnd(), "end preflight accepts active drag");
    expectTrue(activePreflight.snapDegraded, "end preflight marks snap degraded");
    expectTrue(!activePreflight.invalidActiveAxis, "valid axis clears invalidActiveAxis");

    const fuse::editor::EndDragPreflight noAxisPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(noAxisPreflight.invalidActiveAxis, "end preflight marks missing active axis");
    expectTrue(noAxisPreflight.canEnd(), "end preflight still allows end without active axis");

    snap.gridSize = 1.f;
    const fuse::editor::EndDragPreflight validSnapPreflight = fuse::editor::preflightEndDrag(
    expectTrue(!validSnapPreflight.snapDegraded, "valid snap clears snapDegraded on end");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    expectTrue(!gizmo.preflightEndDrag().canEnd(), "gizmo end preflight rejects inactive drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightEndDrag().canEnd(), "gizmo end preflight accepts active drag");
    gizmo.endDrag();
}

void testCanEndDragGuards() {
    expectTrue(!fuse::editor::canEndDrag(false), "canEndDrag rejects inactive drag");

    const fuse::editor::EndDragPreflight activePreflight = fuse::editor::preflightEndDrag(true);
    expectTrue(!activePreflight.notDragging, "active end preflight clears notDragging");
    expectTrue(activePreflight.canEnd, "end preflight accepts active drag");
    expectTrue(fuse::editor::canEndDrag(true), "canEndDrag accepts active drag");

    fuse::editor::GizmoSystem gizmo;
    const fuse::editor::EndDragPreflight gizmoInactivePreflight = gizmo.preflightEndDrag();
    expectTrue(gizmoInactivePreflight.notDragging, "gizmo end preflight marks inactive drag");
    bindGizmoTarget(gizmo);
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects inactive drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, fuse::editor::GizmoTransform{});
    const fuse::editor::EndDragPreflight gizmoActivePreflight = gizmo.preflightEndDrag();
    expectTrue(gizmoActivePreflight.canEnd, "gizmo end preflight accepts active drag");
    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag accepts active drag");
    gizmo.endDrag();
}

void testTryEndDragGuards() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoResult result{};

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryEndDrag leaves result unchanged");
    expectTrue(!gizmo.isDragging(), "inactive tryEndDrag leaves drag inactive");


    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    hit.screenX = 30.f;
    gizmo.updateDrag(hit);
    expectTrue(gizmo.tryEndDrag(result), "tryEndDrag accepts active drag");
    expectTrue(result.changed, "active tryEndDrag marks result changed");
    expectTrue(!result.active, "tryEndDrag clears active flag");
    expectTrue(!gizmo.isDragging(), "tryEndDrag ends drag session");

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects second end without drag");
    expectTrue(!result.changed, "second tryEndDrag leaves result unchanged");

void testUpdateDragScreenMissDiagnostic() {


    hit.screenX = 50.f;
    const fuse::editor::UpdateDragPreflight deadZonePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(deadZonePreflight.screenMiss,
               "update preflight marks translate dead zone as screen miss");
    expectTrue(deadZonePreflight.canUpdate(),
               "screen miss is diagnostic only — drag update still allowed");

    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(update.changed, "updateDrag still applies through screen miss dead zone");

void testEndDragPreflightGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    const fuse::editor::EndDragPreflight inactivePreflight =
        fuse::editor::preflightEndDrag(false);
    expectTrue(!inactivePreflight.canEnd(), "end preflight rejects inactive drag");

    const fuse::editor::EndDragPreflight snapPreflight =
        fuse::editor::preflightEndDrag(true, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(snapPreflight.canEnd(), "end preflight accepts active drag");
    expectTrue(snapPreflight.invalidSnapStep,
               "end preflight reports invalid snap step while dragging");
    expectTrue(fuse::editor::canEndDrag(true, fuse::editor::GizmoMode::Translate, snap),
               "canEndDrag accepts active drag regardless of snap validity");

    snap.gridSize = 0.5f;
    const fuse::editor::EndDragPreflight validSnapPreflight =
    expectTrue(validSnapPreflight.canEnd(), "end preflight accepts active drag with valid snap");
    expectTrue(!validSnapPreflight.snapDisabled, "valid snap end preflight clears snapDisabled");
    expectTrue(!validSnapPreflight.invalidSnapStep,
               "valid snap end preflight clears invalidSnapStep");

    gizmo.setSnapSettings(snap);
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects when not dragging");
    const fuse::editor::EndDragPreflight noAxisPreflight =
        fuse::editor::preflightEndDrag(true);
    expectTrue(noAxisPreflight.invalidActiveAxis, "end preflight marks missing active axis");
    expectTrue(!noAxisPreflight.canEnd(), "end preflight rejects drag without axis");
    expectTrue(!fuse::editor::canEndDrag(true), "canEndDrag rejects drag without axis");

    const fuse::editor::EndDragPreflight validPreflight =
        fuse::editor::preflightEndDrag(true, fuse::editor::GizmoAxis::X);
    expectTrue(validPreflight.canEnd(), "end preflight accepts active drag with axis");
    expectTrue(!validPreflight.notDragging, "valid end preflight clears notDragging");
    expectTrue(!validPreflight.invalidActiveAxis, "valid end preflight clears invalidActiveAxis");
    expectTrue(fuse::editor::canEndDrag(true, fuse::editor::GizmoAxis::X),
               "canEndDrag accepts active drag with axis");

    expectTrue(!gizmo.preflightEndDrag().canEnd(),
               "gizmo end preflight rejects inactive drag");


    expectTrue(!noAxisPreflight.canEnd(), "end preflight rejects missing active axis");

    const fuse::editor::EndDragPreflight activePreflight =
    expectTrue(activePreflight.canEnd(), "end preflight accepts active drag with axis");
    expectTrue(!activePreflight.invalidActiveAxis, "active end preflight clears invalidActiveAxis");

    expectTrue(!gizmo.preflightEndDrag().canEnd(), "gizmo end preflight rejects inactive drag");

    expectTrue(gizmo.preflightEndDrag().canEnd(), "gizmo end preflight accepts active drag");

void testCanEndDragGuards() {
    expectTrue(fuse::editor::canEndDrag(true, fuse::editor::GizmoAxis::Y),

    snap.gridSize = 1.f;

        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoAxis::X,
                                       fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactivePreflight.snapWillApply, "inactive end preflight skips snap diagnostics");

        fuse::editor::preflightEndDrag(true, fuse::editor::GizmoAxis::None,

        fuse::editor::preflightEndDrag(true, fuse::editor::GizmoAxis::X,
    expectTrue(activePreflight.snapWillApply, "end preflight marks snap on commit when enabled");

    snap.translateSnap = false;
    const fuse::editor::EndDragPreflight noSnapPreflight =
    expectTrue(noSnapPreflight.canEnd(), "end preflight accepts drag when snap disabled");
    expectTrue(!noSnapPreflight.snapWillApply, "end preflight clears snapWillApply when disabled");



    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag accepts active drag");
    expectTrue(gizmo.preflightEndDrag().canEnd(), "gizmo end preflight accepts active drag");

    fuse::editor::GizmoResult result{};
    expectTrue(gizmo.tryEndDrag(result), "tryEndDrag succeeds on active drag");
    expectTrue(result.changed, "tryEndDrag marks result changed");
    expectTrue(!result.active, "tryEndDrag clears active flag");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears dragging state");

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects when not dragging");
    expectTrue(!gizmo.endDrag().changed, "endDrag no-op when not dragging via preflight guard");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryEndDrag records active axis");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears drag state");

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryEndDrag leaves result unchanged");
}

void testEndDragUsesTryEndDrag() {
    fuse::editor::GizmoSystem gizmo;
    const fuse::editor::GizmoResult inactive = gizmo.endDrag();
    expectTrue(!inactive.changed, "endDrag no-op when inactive via tryEndDrag guard");
    expectTrue(!gizmo.isDragging(), "inactive endDrag leaves drag inactive");
    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag true while dragging");
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    const fuse::editor::PickInteractionPreflight screenPick = gizmo.preflightPickInteraction(hit);
    expectTrue(screenPick.canPick(), "gizmo pick interaction accepts valid screen hit");
    expectTrue(gizmo.canPickInteraction(hit), "gizmo canPickInteraction accepts valid screen hit");
    expectTrue(gizmo.canPickInteraction(xRay, transform),
               "gizmo canPickInteraction accepts valid ray");
}

void testBeginDragInteractionPreflight() {
    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginDragInteractionPreflight validBegin =
        fuse::editor::preflightBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validBegin.canBegin(), "begin interaction accepts valid screen hit");
    expectTrue(validBegin.snapDegraded, "begin interaction marks snap degraded with invalid step");
    expectTrue(!validBegin.snap.canApply(), "begin interaction snap preflight rejects invalid step");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::canBeginDragInteraction(emptyHit, fuse::editor::GizmoMode::Translate,
                                                    snap),
               "canBeginDragInteraction rejects empty viewport");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(gizmo.canBeginDragInteraction(hit),
               "gizmo canBeginDragInteraction accepts valid screen hit");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragInteractionPreflight rayBegin =
        gizmo.preflightBeginDragInteraction(xRay, transform);
    expectTrue(rayBegin.canBegin(), "gizmo begin interaction accepts valid ray");
    expectTrue(gizmo.canBeginDragInteraction(xRay, transform),
               "gizmo canBeginDragInteraction accepts valid ray");

void testUpdateDragInteractionPreflight() {
    snap.gridSize = 1.f;


    const fuse::editor::UpdateDragInteractionPreflight inactive =
        fuse::editor::preflightUpdateDragInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                                     fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactive.canUpdate(), "update interaction rejects inactive drag");
    expectTrue(inactive.drag.notDragging, "update interaction marks inactive drag");

    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragInteractionPreflight active =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(active.canUpdate(), "update interaction accepts active drag");
    expectTrue(active.snapWillApply(), "update interaction reports snap will apply");

    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.canUpdateDragInteraction(hit),
               "gizmo canUpdateDragInteraction rejects empty viewport");
    gizmo.endDrag();
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag false after endDrag");

void testTryEndDragGuards() {

    const fuse::editor::EndDragInteractionPreflight inactive =
        fuse::editor::preflightEndDragInteraction(false, fuse::editor::GizmoAxis::None,
                                                  fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactive.canEnd(), "end interaction rejects inactive drag");
    expectTrue(inactive.end.notDragging, "end interaction marks inactive drag");

    const fuse::editor::EndDragInteractionPreflight active =
        fuse::editor::preflightEndDragInteraction(true, fuse::editor::GizmoAxis::X,
    expectTrue(active.canEnd(), "end interaction accepts active drag");
    expectTrue(active.snapDegraded, "end interaction marks snap degraded with invalid step");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    const fuse::editor::EndDragPreflight draggingPreflight = gizmo.preflightEndDrag();
    expectTrue(draggingPreflight.canEnd(), "gizmo end preflight accepts active drag");


    expectTrue(!gizmo.transformDirty(), "inactive tryEndDrag does not mark dirty");
void testDragInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::DragInteractionPreflight inactive =
        fuse::editor::preflightDragInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                               fuse::editor::GizmoMode::Translate, snap);
    expectTrue(inactive.notDragging, "drag interaction marks inactive session");
    expectTrue(!inactive.canInteract(), "drag interaction rejects when not dragging");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::DragInteractionPreflight active = gizmo.preflightDragInteraction(hit);
    expectTrue(!active.notDragging, "drag interaction clears notDragging while active");
    expectTrue(active.canUpdate(), "drag interaction can update on valid hit");
    expectTrue(active.canEnd(), "drag interaction can end while dragging");
    expectTrue(active.canInteract(), "drag interaction can interact while dragging");
    expectTrue(gizmo.canDragInteraction(hit), "gizmo canDragInteraction accepts active drag");

    hit.viewportWidth = 0.f;
    const fuse::editor::DragInteractionPreflight emptyHit = gizmo.preflightDragInteraction(hit);
    expectTrue(!emptyHit.canUpdate(), "drag interaction update blocked on empty viewport");
    expectTrue(emptyHit.canEnd(), "drag interaction end still allowed on empty viewport");
    expectTrue(emptyHit.canInteract(), "drag interaction can still end on empty viewport");
    expectTrue(gizmo.canDragInteraction(hit),
               "gizmo canDragInteraction true when end path remains valid");

    gizmo.endDrag();
}

void testDirtyFlagOnEndDrag() {
    fuse::editor::CommandStack commandStack;
    fuse::editor::EditorState editorState;
    gizmo.setCommandStack(&commandStack);
    gizmo.setEditorState(&editorState);

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    transform.posX = 1.f;
    gizmo.beginDrag(hit, transform);
    hit.screenX = 30.f;
    gizmo.updateDrag(hit);

    const fuse::editor::GizmoResult ended = gizmo.endDrag();
    expectTrue(ended.changed, "endDrag commits transform on active drag");
    expectTrue(!gizmo.isDragging(), "endDrag clears drag state");
    expectTrue(ended.axis == fuse::editor::GizmoAxis::X, "endDrag records ended axis");
    expectTrue(gizmo.tryEndDrag(result), "tryEndDrag accepts active drag");
    expectTrue(result.changed, "tryEndDrag marks result changed");
    expectTrue(!result.active, "tryEndDrag deactivates drag");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryEndDrag records axis");
    expectTrue(!gizmo.isDragging(), "tryEndDrag clears dragging state");
    expectTrue(gizmo.transformDirty(), "tryEndDrag marks transform dirty");

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects after drag ended");
}

void testEndDragUsesTryEndDrag() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoResult inactive = gizmo.endDrag();
    expectTrue(!inactive.changed, "endDrag no-op when inactive via tryEndDrag guard");

    gizmo.beginDrag(hit, transform);
    hit.screenX = 30.f;
    gizmo.updateDrag(hit);
    expectTrue(ended.changed, "endDrag commits via tryEndDrag on active drag");
    expectTrue(!gizmo.isDragging(), "endDrag clears dragging state");
    expectTrue(!result.active, "tryEndDrag clears active flag");

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects after drag already ended");
}

void testHitTestOutOfBoundsGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = -1.f;
    hit.screenY = 50.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit),
               "negative screen X is out of bounds");
    expectTrue(!fuse::editor::isHitTestInBounds(hit), "out-of-bounds hit is not in bounds");

    hit.screenX = 101.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit), "screen X past width is out of bounds");

    hit.screenX = 50.f;
    hit.screenY = -1.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit), "negative screen Y is out of bounds");

    hit.screenY = 101.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit), "screen Y past height is out of bounds");

    hit.screenY = 50.f;
    expectTrue(fuse::editor::isHitTestInBounds(hit), "in-bounds hit clears out-of-bounds guard");

    fuse::editor::GizmoHitTest empty{};
    empty.viewportWidth = 0.f;
    expectTrue(!fuse::editor::isHitTestOutOfBounds(empty),
               "empty viewport is not classified as out of bounds");
}

void testPickOutOfBoundsPreflight() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 150.f;
    hit.screenY = 50.f;

    const fuse::editor::PickPreflight outOfBoundsPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(outOfBoundsPick.outOfBounds, "pick preflight marks out-of-bounds screen hit");
    expectTrue(!outOfBoundsPick.canPick(), "pick preflight rejects out-of-bounds screen hit");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(!fuse::editor::tryPickAxis(hit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects out-of-bounds screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None, "out-of-bounds pick leaves axis unset");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    expectTrue(!gizmo.canPickAxis(hit), "gizmo canPickAxis rejects out-of-bounds screen hit");
}

void testBeginDragSnapDegradedPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginDragPreflight degradedPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canBegin, "begin preflight still allows drag when snap degraded");
    expectTrue(degradedPreflight.snapDegraded, "begin preflight marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::BeginDragPreflight validPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid screen hit");
    expectTrue(!gizmoPreflight.snapDegraded, "gizmo begin preflight clears snapDegraded");
}

void testBeginDragOutOfBoundsPreflight() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 150.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginDragPreflight preflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(preflight.outOfBounds, "begin preflight marks out-of-bounds screen hit");
    expectTrue(!preflight.canBegin, "begin preflight rejects out-of-bounds screen hit");

    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryBeginDrag(hit, transform, result),
               "tryBeginDrag rejects out-of-bounds screen hit");
    expectTrue(!gizmo.isDragging(), "out-of-bounds hit does not start drag");
}

void testUpdateDragOutOfBoundsPreflight() {
    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    hit.screenX = 150.f;
    const fuse::editor::UpdateDragPreflight outOfBoundsPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(outOfBoundsPreflight.outOfBounds, "update preflight marks out-of-bounds screen hit");
    expectTrue(!outOfBoundsPreflight.canUpdate(),
               "update preflight rejects out-of-bounds screen hit");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects out-of-bounds screen hit");
    expectTrue(gizmo.isDragging(), "out-of-bounds update keeps drag active");
    gizmo.endDrag();
}

void testGizmoTransformEquals() {
    fuse::editor::GizmoTransform a{};
    fuse::editor::GizmoTransform b{};
    b.rotW = 1.f;
    expectTrue(fuse::editor::gizmoTransformEquals(a, b), "default transforms compare equal");

    a.posX = 1.f;
    expectTrue(!fuse::editor::gizmoTransformEquals(a, b), "position delta breaks equality");
}

void testEndDragUnchangedTransformPreflight() {
    fuse::editor::GizmoTransform transform{};
    transform.rotW = 1.f;
    transform.scaleX = 1.f;
    transform.scaleY = 1.f;
    transform.scaleZ = 1.f;

    const fuse::editor::EndDragPreflight unchangedPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {}, transform,
        transform);
    expectTrue(unchangedPreflight.canEnd(), "end preflight accepts active drag");
    expectTrue(unchangedPreflight.unchangedTransform,
               "end preflight marks unchanged transform");

    fuse::editor::GizmoTransform moved = transform;
    moved.posX = 1.f;
    const fuse::editor::EndDragPreflight changedPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {}, transform,
        moved);
    expectTrue(!changedPreflight.unchangedTransform, "moved transform clears unchangedTransform");

    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightEndDrag().unchangedTransform,
               "gizmo end preflight marks unchanged drag without updates");
    gizmo.endDrag();
}

void testBeginDragSnapDegradedPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginDragPreflight degradedPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canBegin, "begin preflight still allows drag when snap step invalid");
    expectTrue(degradedPreflight.snapDegraded, "begin preflight marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::BeginDragPreflight validPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    snap.gridSize = 0.f;
    const fuse::editor::BeginDragPreflight rayDegradedPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayDegradedPreflight.canBegin, "ray begin preflight still allows drag");
    expectTrue(rayDegradedPreflight.snapDegraded, "ray begin preflight marks snap degraded");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.snapDegraded, "gizmo begin preflight surfaces snap degraded");
}

void testUpdateDragScreenMissPreflight() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateDragPreflight deadZonePreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate);
    expectTrue(deadZonePreflight.canUpdate(), "update preflight accepts axis-band hit");

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    const fuse::editor::UpdateDragPreflight screenMissPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate);
    expectTrue(screenMissPreflight.screenMiss, "update preflight marks translate dead zone");
    expectTrue(!screenMissPreflight.canUpdate(), "update preflight rejects translate dead zone");

    const fuse::editor::UpdateDragPreflight uniformPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Scale);
    expectTrue(uniformPreflight.canUpdate(), "scale uniform handle is not a screen miss");

    fuse::editor::GizmoSystem gizmo;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    hit.screenX = 50.f;
    hit.screenY = 50.f;
    const fuse::editor::UpdateDragPreflight gizmoPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmoPreflight.screenMiss, "gizmo update preflight marks dead zone");
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects dead zone");

    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(!update.changed, "updateDrag ignores dead zone via screenMiss guard");
    expectTrue(gizmo.isDragging(), "dead zone update keeps drag active");
    gizmo.endDrag();
}

void testGizmoTrySnapDragDelta() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;
    gizmo.setSnapSettings(snap);

    expectNear(gizmo.trySnapDragDelta(0.37f), 0.5f, 0.001f,
               "gizmo trySnapDragDelta snaps when snap is valid");

    snap.gridSize = 0.f;
    gizmo.setSnapSettings(snap);
    expectNear(gizmo.trySnapDragDelta(0.37f), 0.37f, 0.001f,
               "gizmo trySnapDragDelta passthrough when step invalid");
}

void testGizmoUpdateDragSnapDegradedPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    hit.screenX = 30.f;
    const fuse::editor::UpdateDragPreflight degradedPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(degradedPreflight.canUpdate(), "gizmo update preflight still allows drag");
    expectTrue(degradedPreflight.snapDegraded, "gizmo update preflight marks snap degraded");
    expectTrue(!degradedPreflight.screenMiss, "axis-band update clears screenMiss");

    snap.gridSize = 1.f;
    gizmo.setSnapSettings(snap);
    const fuse::editor::UpdateDragPreflight validPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on gizmo update");
    gizmo.endDrag();
}

void testBeginDragSnapDegradedPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginDragPreflight degradedHitPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedHitPreflight.canBegin, "begin preflight still allows drag when snap degraded");
    expectTrue(degradedHitPreflight.snapDegraded, "begin preflight marks snap degraded");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragPreflight degradedRayPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(degradedRayPreflight.canBegin, "ray begin preflight still allows drag when snap degraded");
    expectTrue(degradedRayPreflight.snapDegraded, "ray begin preflight marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::BeginDragPreflight validPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid snap settings");
    expectTrue(!gizmoPreflight.snapDegraded, "gizmo begin preflight clears snapDegraded");
}

void testSnapPreflightStepField() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    const fuse::editor::SnapPreflight validPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canApply(), "snap preflight accepts enabled snap with valid step");
    expectNear(validPreflight.step, 0.5f, 0.001f, "snap preflight resolves translate grid step");

    snap.gridSize = 0.f;
    const fuse::editor::SnapPreflight invalidPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidPreflight.invalidStep, "snap preflight marks invalid step");
    expectNear(invalidPreflight.step, 0.f, 0.001f, "snap preflight reports zero step when invalid");

    snap.translateSnap = false;
    const fuse::editor::SnapPreflight disabledPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap preflight marks disabled snap");
    expectNear(disabledPreflight.step, 0.f, 0.001f, "snap preflight leaves step zero when disabled");
}

void testGizmoInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::GizmoInteractionPreflight idlePreflight = fuse::editor::preflightInteraction(
        hit, fuse::editor::GizmoMode::Translate, false, fuse::editor::GizmoAxis::None, snap);
    expectTrue(idlePreflight.canPick(), "interaction preflight accepts valid screen pick");
    expectTrue(idlePreflight.canApplySnap(), "interaction preflight accepts valid snap");
    expectTrue(idlePreflight.canBeginDrag(), "interaction preflight allows begin when idle");
    expectTrue(!idlePreflight.canUpdateDrag(), "interaction preflight rejects update when idle");
    expectTrue(!idlePreflight.canEndDrag(), "interaction preflight rejects end when idle");
    expectTrue(idlePreflight.pick.axis == fuse::editor::GizmoAxis::X,
               "interaction preflight resolves pick axis");

    const fuse::editor::GizmoInteractionPreflight draggingPreflight =
        fuse::editor::preflightInteraction(hit, fuse::editor::GizmoMode::Translate, true,
                                           fuse::editor::GizmoAxis::X, snap);
    expectTrue(!draggingPreflight.canBeginDrag(),
               "interaction preflight rejects begin while dragging");
    expectTrue(draggingPreflight.begin.alreadyDragging,
               "interaction preflight marks active drag on begin");
    expectTrue(draggingPreflight.canUpdateDrag(), "interaction preflight allows update while dragging");
    expectTrue(draggingPreflight.canEndDrag(), "interaction preflight allows end while dragging");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::GizmoInteractionPreflight rayPreflight = fuse::editor::preflightInteraction(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, true,
        fuse::editor::GizmoAxis::X, snap);
    expectTrue(rayPreflight.canPick(), "ray interaction preflight accepts valid pick");
    expectTrue(rayPreflight.canUpdateDrag(),
               "ray interaction preflight allows update without screen hit context");
    expectTrue(!rayPreflight.update.emptyHit,
               "ray interaction preflight clears emptyHit on active drag");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    gizmo.beginDrag(hit, transform);
    const fuse::editor::GizmoInteractionPreflight gizmoPreflight = gizmo.preflightInteraction(hit);
    expectTrue(gizmoPreflight.canUpdateDrag(), "gizmo interaction preflight allows active update");
    expectTrue(gizmoPreflight.canEndDrag(), "gizmo interaction preflight allows active end");
    gizmo.endDrag();
}

void testGizmoUpdatePreflightSnapDegraded() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragPreflight degradedPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(degradedPreflight.canUpdate(), "gizmo update preflight still allows drag when snap degraded");
    expectTrue(degradedPreflight.snapDegraded, "gizmo update preflight marks snap degraded");

    snap.gridSize = 1.f;
    gizmo.setSnapSettings(snap);
    const fuse::editor::UpdateDragPreflight validPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(validPreflight.canUpdate(), "gizmo update preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "gizmo update preflight clears snapDegraded");
    gizmo.endDrag();
}

void testPickSnapPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::PickSnapPreflight validPickSnap =
        fuse::editor::preflightPickSnap(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPickSnap.canPick(), "pick-snap preflight accepts valid screen hit");
    expectTrue(validPickSnap.canSnap(), "pick-snap preflight reports snap ready");
    expectTrue(validPickSnap.pick.axis == fuse::editor::GizmoAxis::X,
               "pick-snap preflight resolves screen axis");

    snap.gridSize = 0.f;
    const fuse::editor::PickSnapPreflight invalidSnap =
        fuse::editor::preflightPickSnap(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidSnap.canPick(), "pick-snap preflight still allows pick when snap step invalid");
    expectTrue(!invalidSnap.canSnap(), "pick-snap preflight rejects invalid snap step");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickSnapPreflight rayPickSnap = fuse::editor::preflightPickSnap(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayPickSnap.canPick(), "pick-snap preflight accepts valid ray");
    expectTrue(rayPickSnap.pick.axis == fuse::editor::GizmoAxis::X,
               "pick-snap preflight resolves ray axis");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightPickSnap(hit).canPick(),
               "gizmo pick-snap preflight accepts valid screen hit");
    expectTrue(gizmo.preflightPickSnap(xRay, transform).canPick(),
               "gizmo pick-snap preflight accepts valid ray");
}

void testBeginInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginInteractionPreflight validBegin =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validBegin.canBegin(), "begin interaction preflight accepts valid screen hit");
    expectTrue(validBegin.snapReady(), "begin interaction preflight reports snap ready");
    expectTrue(validBegin.begin.canBegin, "begin interaction embeds begin-drag approval");

    const fuse::editor::BeginInteractionPreflight draggingBegin =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                                true);
    expectTrue(!draggingBegin.canBegin(), "begin interaction preflight rejects while dragging");
    expectTrue(draggingBegin.begin.alreadyDragging,
               "begin interaction marks already-dragging guard");

    snap.gridSize = 0.f;
    const fuse::editor::BeginInteractionPreflight degradedSnap =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedSnap.canBegin(), "begin interaction still allows begin when snap degraded");
    expectTrue(!degradedSnap.snapReady(), "begin interaction marks snap not ready");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightBeginInteraction(hit).canBegin(),
               "gizmo begin interaction accepts valid hit");
}

void testUpdateInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateInteractionPreflight inactiveUpdate =
        fuse::editor::preflightUpdateInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactiveUpdate.canUpdate(), "update interaction rejects inactive drag");
    expectTrue(inactiveUpdate.update.notDragging, "update interaction embeds not-dragging flag");

    const fuse::editor::UpdateInteractionPreflight degradedUpdate =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedUpdate.canUpdate(),
               "update interaction allows drag when snap step invalid");
    expectTrue(degradedUpdate.snapDegraded(), "update interaction marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::UpdateInteractionPreflight validUpdate =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validUpdate.canUpdate(), "update interaction accepts active drag");
    expectTrue(!validUpdate.snapDegraded(), "valid snap clears snapDegraded on update");

    hit.viewportWidth = 0.f;
    const fuse::editor::UpdateInteractionPreflight emptyHit =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!emptyHit.canUpdate(), "update interaction rejects empty viewport");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    hit.viewportWidth = 100.f;
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightUpdateInteraction(hit).canUpdate(),
               "gizmo update interaction accepts active drag");
    gizmo.endDrag();
}

void testEndInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    const fuse::editor::EndInteractionPreflight inactiveEnd = fuse::editor::preflightEndInteraction(
        false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactiveEnd.canEnd(), "end interaction rejects inactive drag");
    expectTrue(inactiveEnd.end.notDragging, "end interaction embeds not-dragging flag");

    const fuse::editor::EndInteractionPreflight degradedEnd = fuse::editor::preflightEndInteraction(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedEnd.canEnd(), "end interaction accepts active drag");
    expectTrue(degradedEnd.snapDegraded(), "end interaction marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::EndInteractionPreflight validEnd = fuse::editor::preflightEndInteraction(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validEnd.canEnd(), "end interaction accepts active drag with valid snap");
    expectTrue(!validEnd.snapDegraded(), "valid snap clears snapDegraded on end");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightEndInteraction().canEnd(),
               "gizmo end interaction accepts active drag");
    gizmo.endDrag();
}

void testUpdateDragScreenMissPreflight() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateDragPreflight activePreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {});
    expectTrue(activePreflight.canUpdate(), "update preflight accepts in-bounds axis hit");
    expectTrue(!activePreflight.screenMiss, "in-bounds update clears screenMiss");

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    const fuse::editor::UpdateDragPreflight deadZonePreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {});
    expectTrue(deadZonePreflight.screenMiss, "update preflight marks translate dead zone");
    expectTrue(!deadZonePreflight.canUpdate(), "update preflight rejects translate dead zone");

    const fuse::editor::UpdateDragPreflight noModePreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(noModePreflight.canUpdate(),
               "mode-less update preflight unchanged for dead-zone hit");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    fuse::editor::GizmoTransform transform{};
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, transform);

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    const fuse::editor::UpdateDragPreflight scaleCenterPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(scaleCenterPreflight.canUpdate(),
               "scale mode update still accepts uniform handle at center");
    expectTrue(!scaleCenterPreflight.screenMiss,
               "scale center handle is not a screen miss during update");

    hit.screenX = 70.f;
    hit.screenY = 70.f;
    const fuse::editor::UpdateDragPreflight scaleMissPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(scaleMissPreflight.screenMiss,
               "scale mode update marks miss outside axis bands and uniform handle");
    expectTrue(!scaleMissPreflight.canUpdate(), "scale mode update rejects screen miss");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects screen miss during drag");
    expectTrue(gizmo.isDragging(), "screen miss update reject keeps drag active");
    gizmo.endDrag();
}

void testCanInteractionPredicates() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(fuse::editor::canPickSnap(hit, fuse::editor::GizmoMode::Translate, snap),
               "canPickSnap accepts valid screen hit with snap ready");
    expectTrue(fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginInteraction accepts valid screen hit");

    snap.gridSize = 0.f;
    expectTrue(fuse::editor::canPickSnap(hit, fuse::editor::GizmoMode::Translate, snap),
               "canPickSnap still allows pick when snap step invalid");
    expectTrue(fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginInteraction still allows begin when snap step invalid");
    expectTrue(!fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                                  true),
               "canBeginInteraction rejects while already dragging");

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    expectTrue(!fuse::editor::canPickSnap(hit, fuse::editor::GizmoMode::Translate, snap),
               "canPickSnap rejects translate dead zone");
    expectTrue(!fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginInteraction rejects translate dead zone");

    hit.screenX = 10.f;
    hit.screenY = 50.f;
    snap.gridSize = 1.f;
    expectTrue(fuse::editor::canUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                  fuse::editor::GizmoMode::Translate, snap),
               "canUpdateInteraction accepts active drag with valid hit");
    expectTrue(!fuse::editor::canUpdateInteraction(hit, false, fuse::editor::GizmoAxis::X,
                                                   fuse::editor::GizmoMode::Translate, snap),
               "canUpdateInteraction rejects inactive drag");

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    expectTrue(!fuse::editor::canUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                   fuse::editor::GizmoMode::Translate, snap),
               "canUpdateInteraction rejects translate dead zone during drag");

    expectTrue(!fuse::editor::canEndInteraction(false, fuse::editor::GizmoAxis::None,
                                                  fuse::editor::GizmoMode::Translate, snap),
               "canEndInteraction rejects inactive drag");
    expectTrue(fuse::editor::canEndInteraction(true, fuse::editor::GizmoAxis::X,
                                               fuse::editor::GizmoMode::Translate, snap),
               "canEndInteraction accepts active drag");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    snap.gridSize = 1.f;
    expectTrue(fuse::editor::canPickSnap(xRay, transform, fuse::editor::GizmoMode::Translate,
                                         fuse::editor::GizmoSpace::World,
                                         fuse::editor::GizmoSystem::kAxisLength,
                                         fuse::editor::GizmoSystem::kPickRadius, snap),
               "canPickSnap accepts valid ray pick");
    expectTrue(fuse::editor::canBeginInteraction(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, snap),
               "canBeginInteraction accepts valid ray pick");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(gizmo.canPickSnap(hit), "gizmo canPickSnap accepts valid screen hit");
    expectTrue(gizmo.canBeginInteraction(hit), "gizmo canBeginInteraction accepts valid screen hit");
    expectTrue(gizmo.canPickSnap(xRay, transform), "gizmo canPickSnap accepts valid ray");
    expectTrue(gizmo.canBeginInteraction(xRay, transform),
               "gizmo canBeginInteraction accepts valid ray");

    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canUpdateInteraction(hit), "gizmo canUpdateInteraction accepts active drag");
    expectTrue(gizmo.canEndInteraction(), "gizmo canEndInteraction accepts active drag");

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    expectTrue(!gizmo.canUpdateInteraction(hit),
               "gizmo canUpdateInteraction rejects translate dead zone");
    gizmo.endDrag();
}

void testUpdateInteractionScreenMissPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateInteractionPreflight activeUpdate =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                               fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeUpdate.canUpdate(), "update interaction accepts in-bounds hit");
    expectTrue(!activeUpdate.update.screenMiss, "update interaction clears screenMiss");

    hit.screenX = 50.f;
    hit.screenY = 50.f;
    const fuse::editor::UpdateInteractionPreflight deadZoneUpdate =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!deadZoneUpdate.canUpdate(), "update interaction rejects translate dead zone");
    expectTrue(deadZoneUpdate.update.screenMiss,
               "update interaction embeds screenMiss on dead-zone hit");

    const fuse::editor::InteractionPreflight deadZoneInteraction =
        fuse::editor::preflightInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                           fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!deadZoneInteraction.canUpdate(),
               "combined interaction rejects update on translate dead zone");
    expectTrue(deadZoneInteraction.update.update.screenMiss,
               "combined interaction embeds screenMiss on update");
    expectTrue(deadZoneInteraction.canEnd(),
               "combined interaction still allows end while update is rejected");
}

void testInteractionPreflightCombined() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::InteractionPreflight idleInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!idleInteraction.dragging, "interaction preflight reports idle state");
    expectTrue(idleInteraction.canPick(), "idle interaction allows pick");
    expectTrue(idleInteraction.canBegin(), "idle interaction allows begin");
    expectTrue(!idleInteraction.canUpdate(), "idle interaction rejects update");
    expectTrue(!idleInteraction.canEnd(), "idle interaction rejects end");
    expectTrue(idleInteraction.canApplySnap(), "idle interaction reports snap ready");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeInteraction.dragging, "interaction preflight reports dragging state");
    expectTrue(!activeInteraction.canBegin(), "active interaction rejects begin");
    expectTrue(activeInteraction.canUpdate(), "active interaction allows update");
    expectTrue(activeInteraction.canEnd(), "active interaction allows end");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::InteractionPreflight rayInteraction = fuse::editor::preflightInteraction(
        xRay, transform, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
        fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayInteraction.canPick(), "ray interaction preflight allows pick");
    expectTrue(rayInteraction.canBegin(), "ray interaction preflight allows begin");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    gizmo.setSnapSettings(snap);
    const fuse::editor::InteractionPreflight gizmoIdle = gizmo.preflightInteraction(hit);
    expectTrue(gizmoIdle.canBegin(), "gizmo interaction preflight allows begin when idle");

    gizmo.beginDrag(hit, transform);
    const fuse::editor::InteractionPreflight gizmoActive = gizmo.preflightInteraction(hit);
    expectTrue(gizmoActive.canUpdate(), "gizmo interaction preflight allows update when dragging");
    expectTrue(gizmoActive.canEnd(), "gizmo interaction preflight allows end when dragging");
    expectTrue(!gizmoActive.canBegin(), "gizmo interaction preflight rejects begin when dragging");
    gizmo.endDrag();
}

void testGizmoUpdateDragSnapDegradedPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragPreflight updatePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(updatePreflight.canUpdate(),
               "gizmo update preflight still allows drag when snap step invalid");
    expectTrue(updatePreflight.snapDegraded,
               "gizmo update preflight marks snap degraded via snap-aware overload");
    gizmo.endDrag();
}

void testPickSnapPreflight() {
    snap.gridSize = 0.5f;


    const fuse::editor::PickSnapPreflight validPickSnap =
        fuse::editor::preflightPickSnap(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPickSnap.canPick(), "pick-snap preflight accepts valid screen hit");
    expectTrue(validPickSnap.canSnap(), "pick-snap preflight reports snap ready");
    expectTrue(validPickSnap.pick.axis == fuse::editor::GizmoAxis::X,
               "pick-snap preflight resolves screen axis");

    const fuse::editor::PickSnapPreflight invalidSnap =
    expectTrue(invalidSnap.canPick(), "pick-snap preflight still allows pick when snap step invalid");
    expectTrue(!invalidSnap.canSnap(), "pick-snap preflight rejects invalid snap step");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickSnapPreflight rayPickSnap = fuse::editor::preflightPickSnap(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayPickSnap.canPick(), "pick-snap preflight accepts valid ray");
    expectTrue(rayPickSnap.pick.axis == fuse::editor::GizmoAxis::X,
               "pick-snap preflight resolves ray axis");

    expectTrue(gizmo.preflightPickSnap(hit).canPick(),
               "gizmo pick-snap preflight accepts valid screen hit");
    expectTrue(gizmo.preflightPickSnap(xRay, transform).canPick(),
               "gizmo pick-snap preflight accepts valid ray");

void testBeginInteractionPreflight() {
    snap.gridSize = 1.f;


    const fuse::editor::BeginInteractionPreflight validBegin =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validBegin.canBegin(), "begin interaction preflight accepts valid screen hit");
    expectTrue(validBegin.snapReady(), "begin interaction preflight reports snap ready");
    expectTrue(validBegin.begin.canBegin, "begin interaction embeds begin-drag approval");

    const fuse::editor::BeginInteractionPreflight draggingBegin =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                                true);
    expectTrue(!draggingBegin.canBegin(), "begin interaction preflight rejects while dragging");
    expectTrue(draggingBegin.begin.alreadyDragging,
               "begin interaction marks already-dragging guard");

    const fuse::editor::BeginInteractionPreflight degradedSnap =
    expectTrue(degradedSnap.canBegin(), "begin interaction still allows begin when snap degraded");
    expectTrue(!degradedSnap.snapReady(), "begin interaction marks snap not ready");

    expectTrue(gizmo.preflightBeginInteraction(hit).canBegin(),
               "gizmo begin interaction accepts valid hit");

void testUpdateInteractionPreflight() {


    const fuse::editor::UpdateInteractionPreflight inactiveUpdate =
        fuse::editor::preflightUpdateInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactiveUpdate.canUpdate(), "update interaction rejects inactive drag");
    expectTrue(inactiveUpdate.update.notDragging, "update interaction embeds not-dragging flag");

    const fuse::editor::UpdateInteractionPreflight degradedUpdate =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
    expectTrue(degradedUpdate.canUpdate(),
               "update interaction allows drag when snap step invalid");
    expectTrue(degradedUpdate.snapDegraded(), "update interaction marks snap degraded");

    const fuse::editor::UpdateInteractionPreflight validUpdate =
    expectTrue(validUpdate.canUpdate(), "update interaction accepts active drag");
    expectTrue(!validUpdate.snapDegraded(), "valid snap clears snapDegraded on update");

    hit.viewportWidth = 0.f;
    const fuse::editor::UpdateInteractionPreflight emptyHit =
    expectTrue(!emptyHit.canUpdate(), "update interaction rejects empty viewport");

    expectTrue(gizmo.preflightUpdateInteraction(hit).canUpdate(),
               "gizmo update interaction accepts active drag");

void testEndInteractionPreflight() {

    const fuse::editor::EndInteractionPreflight inactiveEnd = fuse::editor::preflightEndInteraction(
        false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactiveEnd.canEnd(), "end interaction rejects inactive drag");
    expectTrue(inactiveEnd.end.notDragging, "end interaction embeds not-dragging flag");

    const fuse::editor::EndInteractionPreflight degradedEnd = fuse::editor::preflightEndInteraction(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedEnd.canEnd(), "end interaction accepts active drag");
    expectTrue(degradedEnd.snapDegraded(), "end interaction marks snap degraded");

    const fuse::editor::EndInteractionPreflight validEnd = fuse::editor::preflightEndInteraction(
    expectTrue(validEnd.canEnd(), "end interaction accepts active drag with valid snap");
    expectTrue(!validEnd.snapDegraded(), "valid snap clears snapDegraded on end");

    expectTrue(gizmo.preflightEndInteraction().canEnd(),
               "gizmo end interaction accepts active drag");

void testInteractionPreflightCombined() {


    const fuse::editor::InteractionPreflight idleInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!idleInteraction.dragging, "interaction preflight reports idle state");
    expectTrue(idleInteraction.canPick(), "idle interaction allows pick");
    expectTrue(idleInteraction.canBegin(), "idle interaction allows begin");
    expectTrue(!idleInteraction.canUpdate(), "idle interaction rejects update");
    expectTrue(!idleInteraction.canEnd(), "idle interaction rejects end");
    expectTrue(idleInteraction.canApplySnap(), "idle interaction reports snap ready");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeInteraction.dragging, "interaction preflight reports dragging state");
    expectTrue(!activeInteraction.canBegin(), "active interaction rejects begin");
    expectTrue(activeInteraction.canUpdate(), "active interaction allows update");
    expectTrue(activeInteraction.canEnd(), "active interaction allows end");

    const fuse::editor::InteractionPreflight rayInteraction = fuse::editor::preflightInteraction(
        xRay, transform, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
        fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayInteraction.canPick(), "ray interaction preflight allows pick");
    expectTrue(rayInteraction.canBegin(), "ray interaction preflight allows begin");

    const fuse::editor::InteractionPreflight gizmoIdle = gizmo.preflightInteraction(hit);
    expectTrue(gizmoIdle.canBegin(), "gizmo interaction preflight allows begin when idle");

    const fuse::editor::InteractionPreflight gizmoActive = gizmo.preflightInteraction(hit);
    expectTrue(gizmoActive.canUpdate(), "gizmo interaction preflight allows update when dragging");
    expectTrue(gizmoActive.canEnd(), "gizmo interaction preflight allows end when dragging");
    expectTrue(!gizmoActive.canBegin(), "gizmo interaction preflight rejects begin when dragging");










































void testBeginDragSnapDegradedPreflight() {


    const fuse::editor::BeginDragPreflight degradedHitPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedHitPreflight.canBegin, "begin preflight still allows drag when snap degraded");
    expectTrue(degradedHitPreflight.snapDegraded, "begin preflight marks snap degraded");

    const fuse::editor::BeginDragPreflight degradedRayPreflight = fuse::editor::preflightBeginDrag(
    expectTrue(degradedRayPreflight.canBegin, "ray begin preflight still allows drag when snap degraded");
    expectTrue(degradedRayPreflight.snapDegraded, "ray begin preflight marks snap degraded");

    const fuse::editor::BeginDragPreflight validPreflight = fuse::editor::preflightBeginDrag(
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid snap settings");
    expectTrue(!gizmoPreflight.snapDegraded, "gizmo begin preflight clears snapDegraded");

void testSnapPreflightStepField() {

    const fuse::editor::SnapPreflight validPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canApply(), "snap preflight accepts enabled snap with valid step");
    expectNear(validPreflight.step, 0.5f, 0.001f, "snap preflight resolves translate grid step");

    const fuse::editor::SnapPreflight invalidPreflight =
    expectTrue(invalidPreflight.invalidStep, "snap preflight marks invalid step");
    expectNear(invalidPreflight.step, 0.f, 0.001f, "snap preflight reports zero step when invalid");

    snap.translateSnap = false;
    const fuse::editor::SnapPreflight disabledPreflight =
    expectTrue(disabledPreflight.snapDisabled, "snap preflight marks disabled snap");
    expectNear(disabledPreflight.step, 0.f, 0.001f, "snap preflight leaves step zero when disabled");

void testPickRejectReasonGuards() {

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(fuse::editor::pickRejectsForReason(
                   emptyRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSystem::kPickRadius, fuse::editor::PickRejectReason::EmptyRay),
               "pick rejects empty ray");

    expectTrue(fuse::editor::pickRejectReason(xRay, transform, fuse::editor::GizmoMode::Translate,
                                              fuse::editor::GizmoSpace::World,
                                              fuse::editor::GizmoSystem::kAxisLength,
                                              fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::PickRejectReason::None,
               "pick accepts valid ray");
    expectTrue(std::strcmp(fuse::editor::pickRejectReasonName(fuse::editor::PickRejectReason::None),
                           "None") == 0,
               "pick reject reason name for None");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(fuse::editor::pickRejectsForReason(emptyHit, fuse::editor::GizmoMode::Translate,
                                                  fuse::editor::PickRejectReason::EmptyHit),
               "pick rejects empty viewport");

    fuse::editor::GizmoHitTest outOfBounds{};
    outOfBounds.viewportWidth = 100.f;
    outOfBounds.viewportHeight = 100.f;
    outOfBounds.screenX = 150.f;
    outOfBounds.screenY = 50.f;
    expectTrue(fuse::editor::isScreenHitOutOfBounds(outOfBounds),
               "screen hit outside viewport width is out of bounds");
                   outOfBounds, fuse::editor::GizmoMode::Translate,
                   fuse::editor::PickRejectReason::ScreenOutOfBounds),
               "pick rejects out-of-bounds screen hit");

    const fuse::editor::PickPreflight outOfBoundsPick =
        fuse::editor::preflightPick(outOfBounds, fuse::editor::GizmoMode::Translate);
    expectTrue(outOfBoundsPick.screenOutOfBounds, "pick preflight marks screen out of bounds");
    expectTrue(outOfBoundsPick.reason == fuse::editor::PickRejectReason::ScreenOutOfBounds,
               "pick preflight reason is ScreenOutOfBounds");

void testSnapRejectReasonGuards() {

    expectTrue(fuse::editor::snapRejectsForReason(fuse::editor::GizmoMode::Translate, snap,
                                                  fuse::editor::SnapRejectReason::Disabled),
               "snap rejects disabled translate snap");
    expectTrue(std::strcmp(
                   fuse::editor::snapRejectReasonName(fuse::editor::SnapRejectReason::Disabled),
                   "Disabled") == 0,
               "snap reject reason name for Disabled");

                                                  fuse::editor::SnapRejectReason::InvalidStep),
               "snap rejects invalid translate step");

    expectTrue(fuse::editor::snapRejectReason(fuse::editor::GizmoMode::Translate, snap) ==
                   fuse::editor::SnapRejectReason::None,
               "snap accepts valid translate settings");

void testBeginDragRejectReasonAndAxis() {

    expectTrue(fuse::editor::beginDragRejectReason(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSystem::kPickRadius) == fuse::editor::BeginDragRejectReason::None,
               "begin-drag accepts valid ray");

        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(validPreflight.canBegin, "begin-drag preflight accepts valid ray");
    expectTrue(validPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin-drag preflight resolves axis on valid ray");

    outOfBounds.screenX = -5.f;
    expectTrue(fuse::editor::beginDragRejectsForReason(
                   fuse::editor::BeginDragRejectReason::ScreenOutOfBounds),
               "begin-drag rejects out-of-bounds screen hit");

    const fuse::editor::BeginDragPreflight outOfBoundsPreflight =
        fuse::editor::preflightBeginDrag(outOfBounds, fuse::editor::GizmoMode::Translate);
    expectTrue(outOfBoundsPreflight.screenOutOfBounds,
               "begin-drag preflight marks screen out of bounds");
    expectTrue(!outOfBoundsPreflight.canBegin, "begin-drag preflight rejects out-of-bounds hit");

void testUpdateDragRejectReasonGuards() {

    expectTrue(fuse::editor::updateDragRejectsForReason(
                   hit, false, fuse::editor::GizmoAxis::X,
                   fuse::editor::UpdateDragRejectReason::NotDragging),
               "update-drag rejects inactive drag");

    hit.screenX = 200.f;
                   hit, true, fuse::editor::GizmoAxis::X,
                   fuse::editor::UpdateDragRejectReason::ScreenOutOfBounds),
               "update-drag rejects out-of-bounds screen hit");

    const fuse::editor::UpdateDragPreflight outOfBoundsPreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
               "update preflight marks screen out of bounds");
    expectTrue(!outOfBoundsPreflight.canUpdate(),
               "update preflight rejects out-of-bounds screen hit");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects out-of-bounds viewport");
    expectTrue(gizmo.isDragging(), "out-of-bounds update keeps drag active");

void testGizmoPreflightUpdateDragWithSnap() {

void testFiniteInputGuards() {
    expectTrue(fuse::editor::isTransformFinite(transform), "default transform is finite");

    transform.posX = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::isTransformFinite(transform), "NaN position fails transform finite check");

    expectTrue(fuse::editor::isRayFinite(xRay), "valid ray is finite");

    fuse::editor::GizmoRay nanRay = xRay;
    nanRay.direction.x = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::isRayFinite(nanRay), "infinite ray direction fails finite check");

    expectTrue(fuse::editor::isHitTestFinite(hit), "valid screen hit is finite");

    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::isHitTestFinite(hit), "NaN screen Y fails hit finite check");

void testPickPreflightFiniteGuards() {
    transform.posZ = std::numeric_limits<fuse::f32>::quiet_NaN();

    const fuse::editor::PickPreflight invalidTransformPick = fuse::editor::preflightPick(
    expectTrue(invalidTransformPick.invalidTransform,
               "pick preflight marks non-finite transform");
    expectTrue(!invalidTransformPick.canPick(), "pick preflight rejects non-finite transform");

    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickPreflight nonFiniteRayPick = fuse::editor::preflightPick(
        nanRay, fuse::editor::GizmoTransform{}, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(nonFiniteRayPick.nonFiniteRay, "pick preflight marks non-finite ray");
    expectTrue(!nonFiniteRayPick.canPick(), "pick preflight rejects non-finite ray");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragPreflight degradedPreflight =
        gizmo.preflightUpdateDragWithSnap(hit);
    expectTrue(degradedPreflight.canUpdate(),
               "preflightUpdateDragWithSnap allows update with invalid snap step");
    expectTrue(degradedPreflight.snapDegraded,
               "preflightUpdateDragWithSnap marks snap degraded");

    snap.gridSize = 1.f;
    gizmo.setSnapSettings(snap);
    const fuse::editor::UpdateDragPreflight validPreflight =
    expectTrue(!validPreflight.snapDegraded,
               "preflightUpdateDragWithSnap clears snap degraded with valid step");
    gizmo.endDrag();
}

void testEndDragRejectReasonGuards() {
    expectTrue(fuse::editor::endDragRejectsForReason(false, fuse::editor::EndDragRejectReason::NotDragging),
               "end-drag rejects inactive drag");
    expectTrue(fuse::editor::endDragRejectReason(true) == fuse::editor::EndDragRejectReason::None,
               "end-drag accepts active drag");
    expectTrue(std::strcmp(
                   fuse::editor::endDragRejectReasonName(fuse::editor::EndDragRejectReason::NotDragging),
                   "NotDragging") == 0,
               "end-drag reject reason name for NotDragging");

    const fuse::editor::EndDragPreflight inactivePreflight =
        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoAxis::X,
                                       fuse::editor::GizmoMode::Translate, {});
    expectTrue(inactivePreflight.reason == fuse::editor::EndDragRejectReason::NotDragging,
               "end preflight reason is NotDragging when inactive");

void testBeginDragPreflightAxisResolution() {

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragPreflight rayPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(rayPreflight.canBegin, "begin preflight accepts valid ray pick");
    expectTrue(rayPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves ray pick axis");
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickPreflight nonFiniteHitPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(nonFiniteHitPick.nonFiniteHit, "pick preflight marks non-finite screen hit");
    expectTrue(!nonFiniteHitPick.canPick(), "pick preflight rejects non-finite screen hit");

void testBeginDragPreflightFiniteGuards() {
    transform.posX = std::numeric_limits<fuse::f32>::quiet_NaN();

    const fuse::editor::BeginDragPreflight invalidTransformBegin = fuse::editor::preflightBeginDrag(
    expectTrue(invalidTransformBegin.invalidTransform,
               "begin preflight marks non-finite transform");
    expectTrue(!invalidTransformBegin.canBegin, "begin preflight rejects non-finite transform");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    const fuse::editor::BeginDragPreflight hitPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(hitPreflight.canBegin, "begin preflight accepts valid screen hit");
    expectTrue(hitPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves screen pick axis");

    fuse::editor::GizmoSystem gizmo;
    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.axis == fuse::editor::GizmoAxis::X,
               "gizmo begin preflight resolves screen pick axis");
}

void testSnapDragPreflightGuards() {
    fuse::editor::GizmoSnapSettings snap{};

    const fuse::editor::SnapDragPreflight disabledPreflight =
        fuse::editor::preflightSnapDrag(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap-drag preflight marks disabled snap");
    expectTrue(!disabledPreflight.canApply(), "snap-drag preflight rejects disabled snap");
    expectTrue(!fuse::editor::canSnapDrag(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDrag rejects disabled snap");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::SnapDragPreflight degradedPreflight =
    expectTrue(degradedPreflight.snapDegraded, "snap-drag preflight marks degraded snap");
    expectTrue(!degradedPreflight.canApply(), "snap-drag preflight rejects degraded snap");
               "canSnapDrag rejects degraded snap");

    snap.gridSize = 1.f;
    const fuse::editor::SnapDragPreflight validPreflight =
    expectTrue(validPreflight.canApply(), "snap-drag preflight accepts valid snap");
    expectTrue(!validPreflight.snapDisabled, "valid snap-drag preflight clears snapDisabled");
    expectTrue(!validPreflight.snapDegraded, "valid snap-drag preflight clears snapDegraded");

    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDrag().canApply(), "gizmo snap-drag preflight accepts valid snap");
    expectTrue(gizmo.canSnapDragNow(), "gizmo canSnapDragNow mirrors preflight");

void testDragInteractionPreflightGuards() {

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;

    const fuse::editor::DragInteractionPreflight beginPreflight =
        fuse::editor::preflightDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                               false);
    expectTrue(beginPreflight.canBegin(), "interaction preflight accepts valid begin");
    expectTrue(!beginPreflight.canUpdate(), "interaction preflight rejects update before drag");
    expectTrue(!beginPreflight.canEnd(), "interaction preflight rejects end before drag");
    expectTrue(!beginPreflight.snapWillApply(), "interaction preflight marks degraded snap");
    expectTrue(beginPreflight.begin.axis == fuse::editor::GizmoAxis::X,
               "interaction preflight forwards begin axis");

    hit.screenX = 50.f;
    const fuse::editor::DragInteractionPreflight deadZonePreflight =
    expectTrue(!deadZonePreflight.canBegin(), "interaction preflight rejects dead-zone begin");
    expectTrue(deadZonePreflight.begin.screenMiss, "interaction preflight forwards screen miss");

    hit.screenX = 30.f;
    const fuse::editor::DragInteractionPreflight dragPreflight = fuse::editor::preflightDragInteraction(
        hit, fuse::editor::GizmoMode::Translate, snap, true, fuse::editor::GizmoAxis::X);
    expectTrue(!dragPreflight.canBegin(), "interaction preflight skips begin while dragging");
    expectTrue(dragPreflight.canUpdate(), "interaction preflight accepts active drag update");
    expectTrue(dragPreflight.canEnd(), "interaction preflight accepts active drag end");
    expectTrue(dragPreflight.update.snapDegraded, "interaction preflight forwards snap degraded");

    const fuse::editor::DragInteractionPreflight screenMissPreflight =
        fuse::editor::preflightDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap, true,
                                                fuse::editor::GizmoAxis::X);
    expectTrue(screenMissPreflight.canUpdate(),
               "interaction preflight still allows update in dead zone");
    expectTrue(screenMissPreflight.update.screenMiss,
               "interaction preflight marks update screen miss");

    const fuse::editor::DragInteractionPreflight gizmoBeginPreflight =
        gizmo.preflightDragInteraction(hit);
    expectTrue(gizmoBeginPreflight.canBegin(), "gizmo interaction preflight accepts begin");

    gizmo.beginDrag(hit, fuse::editor::GizmoTransform{});
    const fuse::editor::DragInteractionPreflight gizmoDragPreflight =
    expectTrue(gizmoDragPreflight.canUpdate(), "gizmo interaction preflight accepts drag update");
    expectTrue(gizmoDragPreflight.canEnd(), "gizmo interaction preflight accepts drag end");
    gizmo.endDrag();

void testGizmoSnapAwareUpdatePreflight() {



    const fuse::editor::UpdateDragPreflight degradedPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(degradedPreflight.canUpdate(),
               "gizmo update preflight still allows drag when snap degraded");
    expectTrue(degradedPreflight.snapDegraded, "gizmo update preflight marks snap degraded");

    const fuse::editor::UpdateDragPreflight validPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(validPreflight.canUpdate(), "gizmo update preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on gizmo update");

void testPickRejectReasonClassification() {
    fuse::editor::PickPreflight emptyRayPick{};
    emptyRayPick.emptyRay = true;
    expectTrue(fuse::editor::classifyPickReject(emptyRayPick) ==
                   fuse::editor::GizmoPickRejectReason::EmptyRay,
               "classifyPickReject prioritizes empty ray");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                               fuse::editor::GizmoPickRejectReason::EmptyRay),
                           "empty_ray") == 0,
               "pick reject reason label for empty ray");

    fuse::editor::PickPreflight emptyHitPick{};
    emptyHitPick.emptyHit = true;
    expectTrue(fuse::editor::classifyPickReject(emptyHitPick) ==
                   fuse::editor::GizmoPickRejectReason::EmptyHit,
               "classifyPickReject marks empty hit");

    fuse::editor::PickPreflight missPick{};
    missPick.pickMiss = true;
    expectTrue(fuse::editor::classifyPickReject(missPick) ==
                   fuse::editor::GizmoPickRejectReason::PickMiss,
               "classifyPickReject marks pick miss");

    fuse::editor::PickPreflight validPick{};
    validPick.axis = fuse::editor::GizmoAxis::X;
    expectTrue(fuse::editor::classifyPickReject(validPick) ==
                   fuse::editor::GizmoPickRejectReason::None,
               "classifyPickReject returns none for valid pick");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    const fuse::editor::PickPreflight screenMiss = gizmo.preflightPick(deadZone);
    expectTrue(gizmo.classifyPickReject(screenMiss) ==
                   fuse::editor::GizmoPickRejectReason::ScreenMiss,
               "gizmo classifyPickReject marks translate dead zone");

void testBeginDragRejectReasonAndAxis() {
    fuse::editor::BeginDragPreflight draggingPreflight{};
    draggingPreflight.alreadyDragging = true;
    expectTrue(fuse::editor::classifyBeginDragReject(draggingPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "classifyBeginDragReject prioritizes already dragging");
    expectTrue(std::strcmp(fuse::editor::gizmoBeginDragRejectReasonLabel(
                               fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging),
                           "already_dragging") == 0,
               "begin-drag reject reason label for already dragging");

    fuse::editor::GizmoTransform transform{};

    const fuse::editor::BeginDragPreflight degradedPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canBegin, "begin preflight still allows drag when snap degraded");
    expectTrue(degradedPreflight.snapDegraded, "begin preflight marks snap degraded");
    expectTrue(degradedPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves axis from pick");
    expectTrue(fuse::editor::classifyBeginDragReject(degradedPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::None,
               "snap degraded does not block begin-drag classification");

    const fuse::editor::BeginDragPreflight validPreflight =
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on begin");
    expectTrue(validPreflight.axis == fuse::editor::GizmoAxis::X,
               "valid begin preflight resolves X axis");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragPreflight rayPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayPreflight.canBegin, "ray begin preflight accepts valid pick");
    expectTrue(rayPreflight.axis == fuse::editor::GizmoAxis::X,
               "ray begin preflight resolves axis");

    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid screen hit");
               "gizmo begin preflight resolves axis");
    expectTrue(gizmo.classifyBeginDragReject(gizmoPreflight) ==
               "gizmo classifyBeginDragReject accepts valid begin");

void testUpdateDragRejectReasonClassification() {
    fuse::editor::UpdateDragPreflight inactivePreflight{};
    inactivePreflight.notDragging = true;
    expectTrue(fuse::editor::classifyUpdateDragReject(inactivePreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "classifyUpdateDragReject marks inactive drag");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                               fuse::editor::GizmoUpdateDragRejectReason::EmptyHit),
                           "empty_hit") == 0,
               "update-drag reject reason label for empty hit");

    fuse::editor::UpdateDragPreflight noAxisPreflight{};
    noAxisPreflight.invalidActiveAxis = true;
    expectTrue(fuse::editor::classifyUpdateDragReject(noAxisPreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis,
               "classifyUpdateDragReject marks invalid active axis");



    gizmo.beginDrag(hit, transform);
    expectTrue(degradedPreflight.snapDegraded,
               "gizmo update preflight marks snap degraded via snap settings");
    expectTrue(gizmo.classifyUpdateDragReject(degradedPreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::None,
               "snap degraded does not block update-drag classification");

    hit.viewportWidth = 0.f;
    const fuse::editor::UpdateDragPreflight emptyPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmo.classifyUpdateDragReject(emptyPreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::EmptyHit,
               "gizmo classifyUpdateDragReject marks empty viewport");

void testEndDragRejectReasonClassification() {
    fuse::editor::EndDragPreflight inactivePreflight{};
    expectTrue(fuse::editor::classifyEndDragReject(inactivePreflight) ==
                   fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "classifyEndDragReject marks inactive drag");
    expectTrue(std::strcmp(fuse::editor::gizmoEndDragRejectReasonLabel(
                               fuse::editor::GizmoEndDragRejectReason::NotDragging),
                           "not_dragging") == 0,
               "end-drag reject reason label for inactive drag");

    fuse::editor::EndDragPreflight activePreflight{};
    activePreflight.invalidActiveAxis = true;
    activePreflight.snapDegraded = true;
    expectTrue(fuse::editor::classifyEndDragReject(activePreflight) ==
                   fuse::editor::GizmoEndDragRejectReason::None,
               "invalid axis and snap degraded do not block end-drag classification");

    const fuse::editor::EndDragPreflight endPreflight = gizmo.preflightEndDrag();
    expectTrue(gizmo.classifyEndDragReject(endPreflight) ==
               "gizmo classifyEndDragReject accepts active drag");

void testBeginDragSnapDegradedPreflight() {


    const fuse::editor::BeginDragPreflight degradedHitPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedHitPreflight.canBegin, "begin preflight still allows drag when snap degraded");
    expectTrue(degradedHitPreflight.snapDegraded, "begin preflight marks snap degraded");

    const fuse::editor::BeginDragPreflight degradedRayPreflight = fuse::editor::preflightBeginDrag(
    expectTrue(degradedRayPreflight.canBegin, "ray begin preflight still allows drag when snap degraded");
    expectTrue(degradedRayPreflight.snapDegraded, "ray begin preflight marks snap degraded");

    const fuse::editor::BeginDragPreflight validPreflight = fuse::editor::preflightBeginDrag(

    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid snap settings");
    expectTrue(!gizmoPreflight.snapDegraded, "gizmo begin preflight clears snapDegraded");

void testSnapPreflightStepField() {
    snap.gridSize = 0.5f;

    const fuse::editor::SnapPreflight validPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canApply(), "snap preflight accepts enabled snap with valid step");
    expectNear(validPreflight.step, 0.5f, 0.001f, "snap preflight resolves translate grid step");

    const fuse::editor::SnapPreflight invalidPreflight =
    expectTrue(invalidPreflight.invalidStep, "snap preflight marks invalid step");
    expectNear(invalidPreflight.step, 0.f, 0.001f, "snap preflight reports zero step when invalid");

    snap.translateSnap = false;
    const fuse::editor::SnapPreflight disabledPreflight =
    expectTrue(disabledPreflight.snapDisabled, "snap preflight marks disabled snap");
    expectNear(disabledPreflight.step, 0.f, 0.001f, "snap preflight leaves step zero when disabled");

void testGizmoInteractionPreflight() {
    hit.screenX = std::numeric_limits<fuse::f32>::infinity();
    const fuse::editor::BeginDragPreflight nonFiniteHitBegin =
    expectTrue(nonFiniteHitBegin.nonFiniteHit, "begin preflight marks non-finite screen hit");
    expectTrue(!nonFiniteHitBegin.canBegin, "begin preflight rejects non-finite screen hit");

void testUpdateDragFiniteAndScreenMissPreflight() {

    hit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::UpdateDragPreflight nonFiniteUpdate = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {});
    expectTrue(nonFiniteUpdate.nonFiniteHit, "update preflight marks non-finite screen hit");
    expectTrue(!nonFiniteUpdate.canUpdate(), "update preflight rejects non-finite screen hit");

    const fuse::editor::UpdateDragPreflight screenMissUpdate = fuse::editor::preflightUpdateDrag(
    expectTrue(screenMissUpdate.canUpdate(),
               "update preflight still allows drag in translate dead zone");
    expectTrue(screenMissUpdate.screenMiss,
               "update preflight marks informational screen miss during drag");

    const fuse::editor::UpdateDragPreflight gizmoScreenMiss = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmoScreenMiss.canUpdate(),
               "gizmo update preflight still allows drag in dead zone");
    expectTrue(gizmoScreenMiss.screenMiss,
               "gizmo update preflight marks informational screen miss");

void testSnapDragPreflight() {

        fuse::editor::preflightSnapDragDelta(fuse::editor::GizmoMode::Translate, snap);

    const fuse::editor::SnapDragPreflight invalidStepPreflight =
    expectTrue(invalidStepPreflight.invalidStep, "snap-drag preflight marks invalid step");
    expectTrue(!invalidStepPreflight.canApply(), "snap-drag preflight rejects invalid step");

    expectTrue(validPreflight.canApply(), "snap-drag preflight accepts valid translate snap");

    expectTrue(gizmo.preflightSnapDragDelta().canApply(),
               "gizmo snap-drag preflight accepts valid settings");
    expectTrue(gizmo.canSnapDragDeltaNow(), "gizmo canSnapDragDeltaNow mirrors preflight");

void testCanInteractionGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::GizmoInteractionPreflight idlePreflight = fuse::editor::preflightInteraction(
        hit, fuse::editor::GizmoMode::Translate, false, fuse::editor::GizmoAxis::None, snap);
    expectTrue(idlePreflight.canPick(), "interaction preflight accepts valid screen pick");
    expectTrue(idlePreflight.canApplySnap(), "interaction preflight accepts valid snap");
    expectTrue(idlePreflight.canBeginDrag(), "interaction preflight allows begin when idle");
    expectTrue(!idlePreflight.canUpdateDrag(), "interaction preflight rejects update when idle");
    expectTrue(!idlePreflight.canEndDrag(), "interaction preflight rejects end when idle");
    expectTrue(idlePreflight.pick.axis == fuse::editor::GizmoAxis::X,
               "interaction preflight resolves pick axis");

    const fuse::editor::GizmoInteractionPreflight draggingPreflight =
        fuse::editor::preflightInteraction(hit, fuse::editor::GizmoMode::Translate, true,
                                           fuse::editor::GizmoAxis::X, snap);
    expectTrue(!draggingPreflight.canBeginDrag(),
               "interaction preflight rejects begin while dragging");
    expectTrue(draggingPreflight.begin.alreadyDragging,
               "interaction preflight marks active drag on begin");
    expectTrue(draggingPreflight.canUpdateDrag(), "interaction preflight allows update while dragging");
    expectTrue(draggingPreflight.canEndDrag(), "interaction preflight allows end while dragging");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::GizmoInteractionPreflight rayPreflight = fuse::editor::preflightInteraction(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, true,
    expectTrue(rayPreflight.canPick(), "ray interaction preflight accepts valid pick");
    expectTrue(rayPreflight.canUpdateDrag(),
               "ray interaction preflight allows update without screen hit context");
    expectTrue(!rayPreflight.update.emptyHit,
               "ray interaction preflight clears emptyHit on active drag");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    gizmo.beginDrag(hit, transform);
    const fuse::editor::GizmoInteractionPreflight gizmoPreflight = gizmo.preflightInteraction(hit);
    expectTrue(gizmoPreflight.canUpdateDrag(), "gizmo interaction preflight allows active update");
    expectTrue(gizmoPreflight.canEndDrag(), "gizmo interaction preflight allows active end");
    gizmo.endDrag();
}

void testGizmoUpdatePreflightSnapDegraded() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;


    const fuse::editor::UpdateDragPreflight degradedPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(degradedPreflight.canUpdate(), "gizmo update preflight still allows drag when snap degraded");
    expectTrue(degradedPreflight.snapDegraded, "gizmo update preflight marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::UpdateDragPreflight validPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(validPreflight.canUpdate(), "gizmo update preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "gizmo update preflight clears snapDegraded");

void testIsSnapDegraded() {
    expectTrue(!fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded false when snap disabled");

    expectTrue(fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded true when translate snap enabled with zero grid");

               "isSnapDegraded false when translate snap step is valid");

void testBeginDragPreflightAxisAndSnap() {

    const fuse::editor::BeginDragPreflight rayPreflight = fuse::editor::preflightBeginDrag(
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(rayPreflight.canBegin, "begin preflight accepts valid ray pick");
    expectTrue(rayPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves ray pick axis");

    const fuse::editor::BeginDragPreflight hitPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(hitPreflight.canBegin, "begin preflight accepts valid screen hit");
    expectTrue(hitPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves screen pick axis");

    const fuse::editor::BeginDragPreflight degradedPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canBegin,
               "begin preflight still allows drag when snap step invalid");
    expectTrue(degradedPreflight.snapDegraded, "begin preflight marks snap degraded");

    const fuse::editor::BeginDragPreflight validSnapPreflight = fuse::editor::preflightBeginDrag(
    expectTrue(validSnapPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validSnapPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid screen hit");
    expectTrue(gizmoPreflight.axis == fuse::editor::GizmoAxis::X,
               "gizmo begin preflight resolves screen pick axis");
    expectTrue(!gizmoPreflight.snapDegraded, "gizmo begin preflight clears snapDegraded");

void testCanBeginDragAlreadyDragging() {

    expectTrue(fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate),
               "canBeginDrag accepts valid screen hit");
    expectTrue(!fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate, true),
               "canBeginDrag rejects when already dragging");

    expectTrue(fuse::editor::canBeginDrag(xRay, transform, fuse::editor::GizmoMode::Translate,
                                          fuse::editor::GizmoSpace::World,
                                          fuse::editor::GizmoSystem::kAxisLength,
                                          fuse::editor::GizmoSystem::kPickRadius),
               "canBeginDrag accepts valid ray pick");
    expectTrue(!fuse::editor::canBeginDrag(xRay, transform, fuse::editor::GizmoMode::Translate,
                                           fuse::editor::GizmoSystem::kPickRadius, true),
               "canBeginDrag rejects ray pick when already dragging");

void testGizmoUpdateDragSnapDegradedPreflight() {



    expectTrue(degradedPreflight.canUpdate(),
               "gizmo update preflight still allows drag when snap step invalid");

void testScreenHitOutOfBoundsGuards() {
    expectTrue(!fuse::editor::isScreenHitOutOfBounds(hit),
               "in-bounds screen hit is not out of bounds");

    hit.screenX = -1.f;
    expectTrue(fuse::editor::isScreenHitOutOfBounds(hit),
               "negative screen X is out of bounds");

    hit.screenY = 101.f;
               "screen Y past viewport height is out of bounds");

    hit.viewportWidth = 0.f;
               "empty viewport is not classified as out of bounds");

    hit.screenX = 150.f;
    const fuse::editor::PickPreflight outOfBoundsPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(outOfBoundsPick.outOfBounds, "pick preflight marks out-of-bounds screen hit");
    expectTrue(!outOfBoundsPick.canPick(), "pick preflight rejects out-of-bounds screen hit");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(!gizmo.tryPickAxis(hit, axis), "tryPickAxis rejects out-of-bounds screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None, "out-of-bounds pick leaves axis unset");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryBeginDrag(hit, transform, result),
               "tryBeginDrag rejects out-of-bounds screen hit");
    expectTrue(!result.active, "out-of-bounds begin leaves drag inactive");

    const fuse::editor::UpdateDragPreflight outOfBoundsUpdate = gizmo.preflightUpdateDrag(hit);
    expectTrue(outOfBoundsUpdate.outOfBounds, "update preflight marks out-of-bounds screen hit");
    expectTrue(!outOfBoundsUpdate.canUpdate(), "update preflight rejects out-of-bounds screen hit");
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects out-of-bounds screen hit");
    expectTrue(gizmo.isDragging(), "out-of-bounds update reject keeps drag active");

void testBeginDragSnapDegradedPreflight() {


    const fuse::editor::BeginDragPreflight degradedPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canBegin, "begin preflight still allows drag when snap step invalid");

    const fuse::editor::BeginDragPreflight validPreflight =
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    const fuse::editor::BeginDragPreflight rayDegradedPreflight = fuse::editor::preflightBeginDrag(
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayDegradedPreflight.canBegin, "ray begin preflight still allows drag with valid pick");
    expectTrue(!rayDegradedPreflight.snapDegraded, "valid snap clears snapDegraded on ray begin");

void testDragUpdateFramePreflight() {


    const fuse::editor::DragUpdateFramePreflight degradedFrame = fuse::editor::preflightDragUpdateFrame(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedFrame.canApply(), "drag frame preflight allows update when snap step invalid");
    expectTrue(degradedFrame.isSnapDegraded(), "drag frame preflight marks snap degraded");

    const fuse::editor::DragUpdateFramePreflight validFrame = fuse::editor::preflightDragUpdateFrame(
    expectTrue(validFrame.canApply(), "drag frame preflight accepts valid update");
    expectTrue(!validFrame.isSnapDegraded(), "valid snap clears drag frame snap degraded");

    hit.screenX = 30.f;
    const fuse::editor::DragUpdateFramePreflight gizmoFrame = gizmo.preflightDragUpdateFrame(hit);
    expectTrue(gizmoFrame.canApply(), "gizmo drag frame preflight accepts active drag");
    expectTrue(!gizmoFrame.isSnapDegraded(), "gizmo drag frame preflight reports valid snap");

void testPickSnapPreflightGuards() {

    const fuse::editor::PickSnapPreflight degradedSnap =
        fuse::editor::preflightPickSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedSnap.snap.invalidStep, "pick-snap preflight marks invalid step");
    expectTrue(degradedSnap.snapDegraded, "pick-snap preflight marks snap degraded");

    snap.gridSize = 0.5f;
    const fuse::editor::PickSnapPreflight validSnap =
    expectTrue(validSnap.snap.canApply(), "pick-snap preflight accepts valid snap");
    expectTrue(!validSnap.snapDegraded, "valid snap clears pick-snap degraded");

    const fuse::editor::PickSnapPreflight rayPick = fuse::editor::preflightPickSnap(
    expectTrue(rayPick.canPick(), "pick-snap preflight accepts valid ray pick");
    expectTrue(rayPick.pick.axis == fuse::editor::GizmoAxis::X,
               "pick-snap preflight resolves ray axis");

    const fuse::editor::PickSnapPreflight screenPick =
        fuse::editor::preflightPickSnap(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(screenPick.canPick(), "pick-snap preflight accepts valid screen hit");
    expectTrue(screenPick.pick.axis == fuse::editor::GizmoAxis::X,
               "pick-snap preflight resolves screen axis");

    expectTrue(gizmo.preflightPickSnap(xRay, transform).canPick(),
               "gizmo pick-snap preflight accepts valid ray");
    expectTrue(gizmo.preflightPickSnap(hit).canPick(),
               "gizmo pick-snap preflight accepts valid screen hit");



        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, false, snap);
    expectTrue(degradedPreflight.canBegin, "begin preflight still allows drag when snap degraded");

    expectTrue(!validPreflight.snapDegraded, "valid snap clears begin snapDegraded");

    expectTrue(fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginDrag with snap accepts valid screen hit");

void testBeginInteractionPreflight() {


    const fuse::editor::BeginInteractionPreflight screenInteraction =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(screenInteraction.canBegin(), "begin interaction accepts valid screen hit");
    expectTrue(screenInteraction.snapDegraded(), "begin interaction marks snap degraded");
    expectTrue(screenInteraction.pick.axis == fuse::editor::GizmoAxis::X,
               "begin interaction resolves pick axis");
    expectTrue(screenInteraction.snap.invalidStep, "begin interaction carries snap invalid step");

    const fuse::editor::BeginInteractionPreflight rayInteraction =
        fuse::editor::preflightBeginInteraction(
    expectTrue(rayInteraction.canBegin(), "begin interaction accepts valid ray pick");
    expectTrue(rayInteraction.pick.axis == fuse::editor::GizmoAxis::X,
               "begin interaction resolves ray axis");

    const fuse::editor::BeginInteractionPreflight gizmoInteraction = gizmo.preflightBeginInteraction(hit);
    expectTrue(gizmoInteraction.canBegin(), "gizmo begin interaction accepts valid screen hit");
    expectTrue(gizmoInteraction.snapDegraded(), "gizmo begin interaction marks snap degraded");
    const fuse::editor::BeginInteractionPreflight draggingInteraction =
        gizmo.preflightBeginInteraction(hit);
    expectTrue(!draggingInteraction.canBegin(), "gizmo begin interaction rejects while dragging");
    expectTrue(draggingInteraction.begin.alreadyDragging,
               "gizmo begin interaction marks already dragging");

void testUpdateInteractionPreflight() {


    const fuse::editor::UpdateInteractionPreflight degradedInteraction =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedInteraction.canUpdate(),
               "update interaction still allows drag when snap degraded");
    expectTrue(degradedInteraction.snapDegraded(), "update interaction marks snap degraded");
    expectTrue(degradedInteraction.snap.invalidStep, "update interaction carries snap invalid step");

    const fuse::editor::UpdateInteractionPreflight gizmoInteraction =
        gizmo.preflightUpdateInteraction(hit);
    expectTrue(gizmoInteraction.canUpdate(), "gizmo update interaction accepts active drag");
    expectTrue(!gizmoInteraction.snapDegraded(), "valid snap clears update interaction degraded");

void testEndInteractionPreflight() {

    const fuse::editor::EndInteractionPreflight degradedInteraction = fuse::editor::preflightEndInteraction(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedInteraction.canEnd(), "end interaction accepts active drag");
    expectTrue(degradedInteraction.snapDegraded(), "end interaction marks snap degraded");
    expectTrue(degradedInteraction.snap.invalidStep, "end interaction carries snap invalid step");

    const fuse::editor::EndInteractionPreflight validInteraction = fuse::editor::preflightEndInteraction(
    expectTrue(validInteraction.canEnd(), "end interaction accepts valid snap settings");
    expectTrue(!validInteraction.snapDegraded(), "valid snap clears end interaction degraded");

    expectTrue(!gizmo.preflightEndInteraction().canEnd(),
               "gizmo end interaction rejects inactive drag");

    expectTrue(gizmo.preflightEndInteraction().canEnd(), "gizmo end interaction accepts active drag");





               "gizmo update preflight still allows drag when snap degraded");



    const fuse::editor::BeginDragPreflight screenPreflight =
    expectTrue(screenPreflight.canBegin, "begin preflight accepts valid screen hit");
    expectTrue(screenPreflight.axis == fuse::editor::GizmoAxis::X,



               "canBeginDrag with settings accepts valid screen hit");

void testUpdateDragScreenMissPreflight() {


    hit.screenX = 50.f;
    const fuse::editor::UpdateDragPreflight deadZonePreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {});
    expectTrue(deadZonePreflight.canUpdate(),
               "update preflight still allows drag in translate dead zone");
    expectTrue(deadZonePreflight.screenMiss, "update preflight marks translate dead zone");

    const fuse::editor::UpdateDragPreflight gizmoPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmoPreflight.canUpdate(), "gizmo update preflight allows dead-zone cursor");
    expectTrue(gizmoPreflight.screenMiss, "gizmo update preflight marks screen miss");

    const fuse::editor::UpdateDragPreflight axisPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(axisPreflight.canUpdate(), "gizmo update preflight accepts axis-band cursor");
    expectTrue(!axisPreflight.screenMiss, "axis-band cursor clears screenMiss");





    expectTrue(rayDegradedPreflight.canBegin, "ray begin preflight still allows drag");
    expectTrue(rayDegradedPreflight.snapDegraded, "ray begin preflight marks snap degraded");

    const fuse::editor::BeginDragPreflight rayValidPreflight = fuse::editor::preflightBeginDrag(
    expectTrue(!rayValidPreflight.snapDegraded, "valid snap clears snapDegraded on ray begin");

void testInteractionPreflightGuards() {


    const fuse::editor::InteractionPreflight idlePreflight = fuse::editor::preflightInteraction(
    expectTrue(!idlePreflight.canApplySnap(), "interaction preflight rejects invalid snap step");
    expectTrue(idlePreflight.canBegin(), "interaction preflight allows begin on valid pick");
    expectTrue(!idlePreflight.canUpdate(), "interaction preflight rejects update when not dragging");
    expectTrue(!idlePreflight.canEnd(), "interaction preflight rejects end when not dragging");
    expectTrue(idlePreflight.begin.snapDegraded, "interaction preflight propagates begin snapDegraded");
    expectTrue(idlePreflight.snap.invalidStep, "interaction preflight propagates snap invalidStep");

    const fuse::editor::InteractionPreflight draggingPreflight = fuse::editor::preflightInteraction(
        hit, fuse::editor::GizmoMode::Translate, true, fuse::editor::GizmoAxis::X, snap);
    expectTrue(draggingPreflight.canUpdate(), "interaction preflight accepts update while dragging");
    expectTrue(draggingPreflight.canEnd(), "interaction preflight accepts end while dragging");
    expectTrue(draggingPreflight.update.snapDegraded,
               "interaction preflight propagates update snapDegraded");
    expectTrue(draggingPreflight.end.snapDegraded,
               "interaction preflight propagates end snapDegraded");

    const fuse::editor::InteractionPreflight rayPreflight = fuse::editor::preflightInteraction(
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, false,
        fuse::editor::GizmoAxis::None, snap);
    expectTrue(rayPreflight.canBegin(), "ray interaction preflight allows begin");
    expectTrue(rayPreflight.pick.axis == fuse::editor::GizmoAxis::X,
               "ray interaction preflight resolves picked axis");

    const fuse::editor::InteractionPreflight gizmoPreflight = gizmo.preflightInteraction(hit);
    expectTrue(gizmoPreflight.canBegin(), "gizmo interaction preflight allows begin");
    expectTrue(gizmoPreflight.begin.snapDegraded,
               "gizmo interaction preflight propagates begin snapDegraded");

    const fuse::editor::InteractionPreflight activeGizmoPreflight = gizmo.preflightInteraction(hit);
    expectTrue(activeGizmoPreflight.canUpdate(), "gizmo interaction preflight accepts active update");
    expectTrue(activeGizmoPreflight.update.snapDegraded,
               "gizmo update preflight marks snap degraded via snap-aware wrapper");

void testDirtyFlagOnEndDrag() {
    fuse::editor::CommandStack commandStack;
    fuse::editor::EditorState editorState;
    gizmo.setCommandStack(&commandStack);
    gizmo.setEditorState(&editorState);



    gizmo.updateDrag(hit);

    expectTrue(gizmo.transformDirty(), "gizmo marks transform dirty after drag");
    expectTrue(editorState.sceneModified, "gizmo marks editor scene modified");
    expectTrue(commandStack.isDirty(), "gizmo marks command stack dirty");
    expectTrue(commandStack.undoDepth() == 1u, "gizmo posts one transform command");

void testHitTestOutOfBoundsGuards() {
    hit.screenX = -5.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit),
    expectTrue(fuse::editor::isHitTestValid(hit),
               "out-of-bounds hit still has valid viewport dimensions");

    hit.screenX = 105.f;
               "screen X beyond viewport width is out of bounds");

    hit.screenY = -1.f;
               "negative screen Y is out of bounds");

               "screen Y beyond viewport height is out of bounds");

    expectTrue(!fuse::editor::isHitTestOutOfBounds(hit),
               "in-bounds screen hit clears out-of-bounds guard");


    expectTrue(outOfBoundsPick.emptyHit, "empty viewport pick marks emptyHit not outOfBounds");

    const fuse::editor::PickPreflight negativePick =
    expectTrue(negativePick.outOfBounds, "pick preflight marks out-of-bounds screen hit");
    expectTrue(!negativePick.canPick(), "pick preflight rejects out-of-bounds screen hit");
void testScreenHitBoundsGuards() {

    expectTrue(!fuse::editor::isScreenHitInViewport(hit),
               "negative screen X is not in viewport");

    hit.screenX = 101.f;



               "in-bounds screen hit clears outOfBounds");
    expectTrue(fuse::editor::isScreenHitInViewport(hit),
               "in-bounds screen hit is in viewport");


void testPickPreflightOutOfBounds() {


    expectTrue(!fuse::editor::tryPickAxis(hit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects out-of-bounds screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None,
               "out-of-bounds screen hit leaves axis unset");

    expectTrue(!gizmo.canPickAxis(hit), "gizmo canPickAxis rejects out-of-bounds screen hit");
    expectTrue(!fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate),
               "canBeginDrag rejects out-of-bounds screen hit");
    expectTrue(!gizmo.canBeginDrag(hit), "gizmo canBeginDrag rejects out-of-bounds screen hit");

void testBeginDragPreflightAxisAndSnapDegraded() {
void testScreenOutOfBoundsGuards() {
               "screen hit beyond viewport width is out of bounds");
    expectTrue(!fuse::editor::isScreenHitInBounds(hit),
               "isScreenHitInBounds rejects out-of-bounds hit");


    hit.screenY = 120.f;
               "screen hit beyond viewport height is out of bounds");

    expectTrue(fuse::editor::isScreenHitInBounds(hit), "in-bounds hit passes in-bounds check");
    expectTrue(!fuse::editor::isScreenHitOutOfBounds(hit), "in-bounds hit clears out-of-bounds");

    expectTrue(outOfBoundsPick.canPick(), "in-bounds pick preflight accepts valid hit");

    const fuse::editor::PickPreflight rejectPick =
    expectTrue(rejectPick.outOfBounds, "pick preflight marks out-of-bounds screen hit");
    expectTrue(!rejectPick.canPick(), "pick preflight rejects out-of-bounds screen hit");


void testBeginDragOutOfBoundsAndSnapDegraded() {
    const fuse::editor::PickPreflight validPick =
    expectTrue(validPick.canPick(), "pick preflight accepts in-bounds screen hit");
    expectTrue(!validPick.outOfBounds, "valid screen pick clears outOfBounds");

void testBeginDragPreflightPickedAxis() {


    const fuse::editor::BeginDragPreflight degradedHitPreflight =
    expectTrue(degradedHitPreflight.canBegin,
    expectTrue(degradedHitPreflight.snapDegraded,
               "begin preflight marks snap degraded with invalid step");
    expectTrue(degradedHitPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves picked axis on valid screen hit");

    const fuse::editor::BeginDragPreflight validSnapPreflight =
    expectTrue(validPreflight.canBegin, "begin preflight accepts valid screen hit");
    expectTrue(validPreflight.pickedAxis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves picked axis from screen hit");

    const fuse::editor::BeginDragPreflight outOfBoundsPreflight =
    expectTrue(outOfBoundsPreflight.outOfBounds, "begin preflight marks out-of-bounds screen hit");
    expectTrue(!outOfBoundsPreflight.canBegin,
               "begin preflight rejects out-of-bounds screen hit");
    expectTrue(outOfBoundsPreflight.pickedAxis == fuse::editor::GizmoAxis::None,
               "out-of-bounds begin preflight leaves pickedAxis unset");


    expectTrue(outOfBoundsPreflight.outOfBounds, "begin preflight marks out-of-bounds hit");
    expectTrue(!outOfBoundsPreflight.canBegin, "begin preflight rejects out-of-bounds hit");

    expectTrue(degradedPreflight.canBegin, "begin preflight accepts valid hit with degraded snap");

    expectTrue(!validSnapPreflight.snapDegraded, "valid snap clears begin snapDegraded");

               "gizmo begin preflight resolves picked axis");

void testUpdateDragOutOfBoundsPreflight() {
    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid snap settings");



    const fuse::editor::UpdateDragPreflight outOfBoundsPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(outOfBoundsPreflight.outOfBounds, "update preflight marks out-of-bounds hit");
    expectTrue(!outOfBoundsPreflight.canUpdate(), "update preflight rejects out-of-bounds hit");

    expectTrue(!gizmo.canUpdateDrag(hit), "canUpdateDrag rejects out-of-bounds hit");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects out-of-bounds hit");


    const fuse::editor::InteractionPreflight rayInteraction = fuse::editor::preflightInteraction(
    expectTrue(rayInteraction.canInteract(), "interaction preflight accepts valid ray pick");
    expectTrue(rayInteraction.snapReady(), "interaction preflight accepts valid snap");
               "interaction preflight resolves ray axis");

    fuse::editor::GizmoHitTest missHit{};
    missHit.viewportWidth = 100.f;
    missHit.viewportHeight = 100.f;
    missHit.screenX = 50.f;
    missHit.screenY = 50.f;
    const fuse::editor::InteractionPreflight deadZoneInteraction =
        fuse::editor::preflightInteraction(missHit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!deadZoneInteraction.canInteract(),
               "interaction preflight rejects translate dead zone");
    expectTrue(deadZoneInteraction.snapReady(), "snap remains valid when pick misses");

    const fuse::editor::InteractionPreflight invalidSnapInteraction =
    expectTrue(!invalidSnapInteraction.snapReady(),
               "interaction preflight marks invalid snap step");

    missHit.screenX = 10.f;
    const fuse::editor::InteractionPreflight gizmoInteraction = gizmo.preflightInteraction(missHit);
    expectTrue(gizmoInteraction.canInteract(), "gizmo interaction preflight accepts valid hit");
    expectTrue(!gizmoInteraction.snapReady(), "gizmo interaction preflight marks invalid snap");
    expectTrue(gizmo.preflightInteraction(xRay, transform).canInteract(),
               "gizmo interaction preflight accepts valid ray");

void testHitTestInBoundsGuards() {
    expectTrue(fuse::editor::isHitTestInBounds(hit), "in-bounds screen hit is in bounds");

    expectTrue(!fuse::editor::isHitTestInBounds(hit),
               "negative screen X is not in bounds");

               "empty viewport is not in bounds");



    const fuse::editor::UpdateDragPreflight deadZonePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(!deadZonePreflight.canUpdate(), "update preflight rejects translate dead zone");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects translate dead zone");
    expectTrue(gizmo.isDragging(), "dead-zone update reject keeps drag active");
    expectTrue(!fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
                                            fuse::editor::GizmoMode::Translate),
               "canUpdateDrag rejects dead zone with mode-aware guard");

    expectTrue(gizmo.preflightUpdateDrag(hit).canUpdate(),
               "update preflight accepts valid in-bounds hit after dead zone");

void testBeginDragPreflightOutOfBounds() {

    expectTrue(!outOfBoundsPreflight.canBegin, "begin preflight rejects out-of-bounds screen hit");

void testGizmoCanUpdateDragWithSnap() {



    expectTrue(gizmo.canUpdateDrag(hit, fuse::editor::GizmoMode::Translate, snap),
               "gizmo canUpdateDrag with snap accepts valid active drag");

void testGizmoCanEndDragWithSnap() {

    expectTrue(!gizmo.canEndDrag(fuse::editor::GizmoMode::Translate, snap),
               "gizmo canEndDrag with snap rejects inactive drag");

    expectTrue(gizmo.canEndDrag(fuse::editor::GizmoMode::Translate, snap),
               "gizmo canEndDrag with snap accepts active drag");

    expectTrue(rayPreflight.pickedAxis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves picked axis from ray pick");


    const fuse::editor::UpdateDragPreflight outOfBoundsPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(outOfBoundsPreflight.outOfBounds, "update preflight marks out-of-bounds screen hit");
    expectTrue(!outOfBoundsPreflight.canUpdate(),
               "update preflight rejects out-of-bounds screen hit");

    const fuse::editor::UpdateDragPreflight validPreflight = fuse::editor::preflightUpdateDrag(
    expectTrue(validPreflight.canUpdate(), "update preflight accepts in-bounds screen hit");
    expectTrue(!validPreflight.outOfBounds, "valid update preflight clears outOfBounds");


    fuse::editor::GizmoResult updateResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, updateResult),
               "tryUpdateDrag rejects out-of-bounds screen hit");
    expectTrue(gizmo.isDragging(), "out-of-bounds update keeps drag active");

void testGizmoSnapAwareUpdatePreflight() {



    expectTrue(degradedPreflight.snapDegraded,
               "gizmo update preflight marks snap degraded with invalid step");
               "gizmo update preflight marks snap degraded via snap-aware overload");

    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on gizmo update");

void testPickSnapPreflight() {
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on gizmo preflight");

void testGizmoSnapDragDeltaWrappers() {


    const fuse::editor::PickSnapPreflight validPickSnap =
    expectTrue(validPickSnap.canPick(), "pick-snap preflight accepts valid screen hit");
    expectTrue(validPickSnap.canSnap(), "pick-snap preflight reports snap ready");
    expectTrue(validPickSnap.pick.axis == fuse::editor::GizmoAxis::X,

    const fuse::editor::PickSnapPreflight invalidSnap =
    expectTrue(invalidSnap.canPick(), "pick-snap preflight still allows pick when snap step invalid");
    expectTrue(!invalidSnap.canSnap(), "pick-snap preflight rejects invalid snap step");

    const fuse::editor::PickSnapPreflight rayPickSnap = fuse::editor::preflightPickSnap(
    expectTrue(rayPickSnap.canPick(), "pick-snap preflight accepts valid ray");
    expectTrue(rayPickSnap.pick.axis == fuse::editor::GizmoAxis::X,




    const fuse::editor::BeginInteractionPreflight validBegin =
    expectTrue(validBegin.canBegin(), "begin interaction preflight accepts valid screen hit");
    expectTrue(validBegin.snapReady(), "begin interaction preflight reports snap ready");
    expectTrue(validBegin.begin.canBegin, "begin interaction embeds begin-drag approval");
    expectTrue(validBegin.begin.axis == fuse::editor::GizmoAxis::X,
               "begin interaction resolves picked axis");

    const fuse::editor::BeginInteractionPreflight draggingBegin =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                                true);
    expectTrue(!draggingBegin.canBegin(), "begin interaction preflight rejects while dragging");
    expectTrue(draggingBegin.begin.alreadyDragging,
               "begin interaction marks already-dragging guard");

    const fuse::editor::BeginInteractionPreflight degradedSnap =
    expectTrue(degradedSnap.canBegin(), "begin interaction still allows begin when snap degraded");
    expectTrue(!degradedSnap.snapReady(), "begin interaction marks snap not ready");
    expectTrue(degradedSnap.begin.snapDegraded,
               "begin interaction embeds snap degraded on begin-drag");

    expectTrue(gizmo.preflightBeginInteraction(hit).canBegin(),
               "gizmo begin interaction accepts valid hit");



    const fuse::editor::UpdateInteractionPreflight inactiveUpdate =
        fuse::editor::preflightUpdateInteraction(hit, false, fuse::editor::GizmoAxis::None,
    expectTrue(!inactiveUpdate.canUpdate(), "update interaction rejects inactive drag");
    expectTrue(inactiveUpdate.update.notDragging, "update interaction embeds not-dragging flag");

    const fuse::editor::UpdateInteractionPreflight degradedUpdate =
    expectTrue(degradedUpdate.canUpdate(),
               "update interaction allows drag when snap step invalid");
    expectTrue(degradedUpdate.snapDegraded(), "update interaction marks snap degraded");

    const fuse::editor::UpdateInteractionPreflight validUpdate =
    expectTrue(validUpdate.canUpdate(), "update interaction accepts active drag");
    expectTrue(!validUpdate.snapDegraded(), "valid snap clears snapDegraded on update");

    const fuse::editor::UpdateInteractionPreflight emptyHit =
    expectTrue(!emptyHit.canUpdate(), "update interaction rejects empty viewport");

    const fuse::editor::UpdateInteractionPreflight outOfBounds =
    expectTrue(!outOfBounds.canUpdate(), "update interaction rejects out-of-bounds hit");
    expectTrue(outOfBounds.update.outOfBounds,
               "update interaction embeds out-of-bounds guard");

    expectTrue(gizmo.preflightUpdateInteraction(hit).canUpdate(),
               "gizmo update interaction accepts active drag");


    const fuse::editor::EndInteractionPreflight inactiveEnd = fuse::editor::preflightEndInteraction(
        false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactiveEnd.canEnd(), "end interaction rejects inactive drag");
    expectTrue(inactiveEnd.end.notDragging, "end interaction embeds not-dragging flag");

    const fuse::editor::EndInteractionPreflight degradedEnd = fuse::editor::preflightEndInteraction(
    expectTrue(degradedEnd.canEnd(), "end interaction accepts active drag");
    expectTrue(degradedEnd.snapDegraded(), "end interaction marks snap degraded");

    const fuse::editor::EndInteractionPreflight validEnd = fuse::editor::preflightEndInteraction(
    expectTrue(validEnd.canEnd(), "end interaction accepts active drag with valid snap");
    expectTrue(!validEnd.snapDegraded(), "valid snap clears snapDegraded on end");

    expectTrue(gizmo.preflightEndInteraction().canEnd(),
               "gizmo end interaction accepts active drag");

void testInteractionPreflightCombined() {


    const fuse::editor::InteractionPreflight idleInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!idleInteraction.dragging, "interaction preflight reports idle state");
    expectTrue(idleInteraction.canPick(), "idle interaction allows pick");
    expectTrue(idleInteraction.canBegin(), "idle interaction allows begin");
    expectTrue(!idleInteraction.canUpdate(), "idle interaction rejects update");
    expectTrue(!idleInteraction.canEnd(), "idle interaction rejects end");
    expectTrue(idleInteraction.canApplySnap(), "idle interaction reports snap ready");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
    expectTrue(activeInteraction.dragging, "interaction preflight reports dragging state");
    expectTrue(!activeInteraction.canBegin(), "active interaction rejects begin");
    expectTrue(activeInteraction.canUpdate(), "active interaction allows update");
    expectTrue(activeInteraction.canEnd(), "active interaction allows end");

    const fuse::editor::InteractionPreflight outOfBoundsInteraction =
        fuse::editor::preflightInteraction(hit, true, fuse::editor::GizmoAxis::X,
    expectTrue(!outOfBoundsInteraction.canUpdate(),
               "active interaction rejects update when hit is out of bounds");
    expectTrue(outOfBoundsInteraction.update.update.outOfBounds,
               "combined interaction embeds out-of-bounds on update");

        xRay, transform, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
        fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayInteraction.canPick(), "ray interaction preflight allows pick");
    expectTrue(rayInteraction.canBegin(), "ray interaction preflight allows begin");

    const fuse::editor::InteractionPreflight gizmoIdle = gizmo.preflightInteraction(hit);
    expectTrue(gizmoIdle.canBegin(), "gizmo interaction preflight allows begin when idle");

    const fuse::editor::InteractionPreflight gizmoActive = gizmo.preflightInteraction(hit);
    expectTrue(gizmoActive.canUpdate(), "gizmo interaction preflight allows update when dragging");
    expectTrue(gizmoActive.canEnd(), "gizmo interaction preflight allows end when dragging");
    expectTrue(!gizmoActive.canBegin(), "gizmo interaction preflight rejects begin when dragging");

void testHitTestInvalidDimensionsGuards() {
    hit.viewportWidth = -100.f;

    expectTrue(fuse::editor::isHitTestDimensionsInvalid(hit),
               "negative viewport width is invalid dimensions");
    expectTrue(!fuse::editor::isHitTestEmpty(hit),
               "negative viewport width is not classified as empty");
               "invalid-dimension hit still has non-zero magnitude width");

    hit.viewportHeight = -50.f;
               "negative viewport height is invalid dimensions");

    expectTrue(!fuse::editor::isHitTestDimensionsInvalid(hit),
               "positive viewport dimensions clear invalid-dimension guard");

    const fuse::editor::PickPreflight invalidPick =
    expectTrue(invalidPick.canPick(), "valid-dimension pick preflight accepts in-bounds hit");
    const fuse::editor::PickPreflight negativeWidthPick =
    expectTrue(negativeWidthPick.invalidDimensions,
               "pick preflight marks invalid viewport dimensions");
    expectTrue(!negativeWidthPick.canPick(), "pick preflight rejects invalid viewport dimensions");

    const fuse::editor::BeginDragPreflight beginPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate);
    expectTrue(beginPreflight.invalidDimensions,
               "begin preflight marks invalid viewport dimensions");
    expectTrue(!beginPreflight.canBegin, "begin preflight rejects invalid viewport dimensions");

    const fuse::editor::UpdateDragPreflight updatePreflight = fuse::editor::preflightUpdateDrag(
    expectTrue(updatePreflight.canUpdate(), "valid hit clears update invalid-dimension guard");

    const fuse::editor::UpdateDragPreflight invalidUpdatePreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(invalidUpdatePreflight.invalidDimensions,
               "update preflight marks invalid viewport dimensions");
    expectTrue(!invalidUpdatePreflight.canUpdate(),
               "update preflight rejects invalid viewport dimensions");

               "tryPickAxis rejects invalid viewport dimensions");
               "invalid-dimension pick leaves axis unset");

    fuse::editor::GizmoSystem gizmo;
    bindGizmoTarget(gizmo);
    expectTrue(!gizmo.canPickAxis(hit), "gizmo canPickAxis rejects invalid viewport dimensions");
    expectTrue(!gizmo.canBeginDrag(hit), "gizmo canBeginDrag rejects invalid viewport dimensions");

    expectTrue(fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate).canPick() ==
                   fuse::editor::canPickSnap(hit, fuse::editor::GizmoMode::Translate, {}),
               "canPickSnap mirrors pick preflight on valid hit");

void testIsSnapDegradedHelper() {

               "isSnapDegraded true when translate snap enabled with zero step");

    const fuse::editor::SnapPreflight degradedPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.isDegraded(), "snap preflight isDegraded marks invalid step");
    expectTrue(!degradedPreflight.canApply(), "degraded snap cannot apply");

    expectTrue(fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap).canApply(),
               "valid snap preflight can apply");

void testInteractionPhaseRouting() {
    expectTrue(fuse::editor::interactionPhase(false) == fuse::editor::GizmoInteractionPhase::Idle,
               "interactionPhase reports idle when not dragging");
    expectTrue(fuse::editor::interactionPhase(true) ==
                   fuse::editor::GizmoInteractionPhase::Dragging,
               "interactionPhase reports dragging when active");



    expectTrue(idleInteraction.phase() == fuse::editor::GizmoInteractionPhase::Idle,
               "interaction preflight reports idle phase");
    expectTrue(idleInteraction.canActOnPhase(), "idle phase allows begin action");
    expectTrue(!idleInteraction.canUpdate(), "idle phase rejects update action");

    expectTrue(activeInteraction.phase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "interaction preflight reports dragging phase");
    expectTrue(activeInteraction.canActOnPhase(), "dragging phase allows update action");
    expectTrue(!activeInteraction.canBegin(), "dragging phase rejects begin action");

void testFiniteInputGuards() {
    expectTrue(fuse::editor::isFiniteGizmoScalar(1.f), "finite scalar accepts normal value");
    expectTrue(!fuse::editor::isFiniteGizmoScalar(std::numeric_limits<fuse::f32>::infinity()),
               "finite scalar rejects infinity");
    expectTrue(!fuse::editor::isFiniteGizmoScalar(std::numeric_limits<fuse::f32>::quiet_NaN()),
               "finite scalar rejects NaN");

    fuse::editor::GizmoRay finiteRay = rayAlongX();
    expectTrue(fuse::editor::isRayFinite(finiteRay), "finite ray accepts normal ray");

    fuse::editor::GizmoRay nanRay = finiteRay;
    nanRay.direction.x = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::isRayFinite(nanRay), "finite ray rejects NaN direction");

    fuse::editor::GizmoHitTest finiteHit{};
    finiteHit.viewportWidth = 100.f;
    finiteHit.viewportHeight = 100.f;
    finiteHit.screenX = 10.f;
    finiteHit.screenY = 50.f;
    expectTrue(fuse::editor::isHitTestFinite(finiteHit), "finite hit accepts normal viewport");

    fuse::editor::GizmoHitTest nanHit = finiteHit;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::isHitTestFinite(nanHit), "finite hit rejects NaN screen X");

void testPickPreflightNonFiniteGuards() {

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickPreflight nanRayPick = fuse::editor::preflightPick(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(nanRayPick.nonFiniteRay, "pick preflight marks non-finite ray");
    expectTrue(!nanRayPick.canPick(), "pick preflight rejects non-finite ray");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenY = 50.f;
    const fuse::editor::PickPreflight nanHitPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(nanHitPick.nonFiniteHit, "pick preflight marks non-finite screen hit");
    expectTrue(!nanHitPick.canPick(), "pick preflight rejects non-finite screen hit");

    expectTrue(!fuse::editor::tryPickAxis(nanHit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects non-finite screen hit");
               "non-finite screen hit leaves axis unset");

    expectTrue(!gizmo.canPickAxis(nanHit), "gizmo canPickAxis rejects non-finite screen hit");
    expectTrue(!gizmo.canBeginDrag(nanHit), "gizmo canBeginDrag rejects non-finite screen hit");

void testBeginDragPreflightNonFiniteGuards() {
    nanHit.screenX = std::numeric_limits<fuse::f32>::infinity();

    const fuse::editor::BeginDragPreflight beginPreflight =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(beginPreflight.nonFiniteHit, "begin preflight marks non-finite screen hit");
    expectTrue(!beginPreflight.canBegin, "begin preflight rejects non-finite screen hit");

    nanRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(rayPreflight.nonFiniteRay, "begin preflight marks non-finite ray");
    expectTrue(!rayPreflight.canBegin, "begin preflight rejects non-finite ray");

void testUpdateDragNonFinitePreflight() {


    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::UpdateDragPreflight nanPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(nanPreflight.nonFiniteHit, "update preflight marks non-finite screen hit");
    expectTrue(!nanPreflight.canUpdate(), "update preflight rejects non-finite screen hit");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects non-finite screen hit");
    expectTrue(gizmo.isDragging(), "non-finite update reject keeps drag active");

void testSnapDragPreflightGuards() {

    const fuse::editor::SnapDragPreflight validPreflight =
        fuse::editor::preflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canApply(), "snap-drag preflight accepts valid delta and snap");
    expectTrue(!validPreflight.deltaNonFinite, "valid snap-drag clears deltaNonFinite");

    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::quiet_NaN(), fuse::editor::GizmoMode::Translate, snap);
    expectTrue(nanPreflight.deltaNonFinite, "snap-drag preflight marks non-finite delta");
    expectTrue(!nanPreflight.canApply(), "snap-drag preflight rejects non-finite delta");

    const fuse::editor::SnapDragPreflight degradedPreflight =
    expectTrue(degradedPreflight.isDegraded(), "snap-drag preflight marks degraded snap step");
    expectTrue(!degradedPreflight.canApply(), "snap-drag preflight rejects invalid snap step");

    expectTrue(
        std::isnan(fuse::editor::trySnapDragDelta(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                fuse::editor::GizmoMode::Translate, snap)),
        "trySnapDragDelta passthrough on non-finite delta");

    expectTrue(fuse::editor::canSnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta accepts valid delta and snap");
    expectTrue(!fuse::editor::canSnapDragDelta(std::numeric_limits<fuse::f32>::infinity(),
                                               fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta rejects non-finite delta");

    expectTrue(gizmo.preflightSnapDrag(0.37f).canApply(),
               "gizmo snap-drag preflight accepts valid delta");

void testPickInteractionBlockingHelpers() {


    const fuse::editor::PickInteractionPreflight validPick =
        fuse::editor::preflightPickInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!validPick.pickBlocked(), "pick interaction clears pickBlocked on valid hit");
    expectTrue(!validPick.snapBlocked(), "pick interaction clears snapBlocked on valid snap");

    hit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickInteractionPreflight blockedPick =
    expectTrue(blockedPick.pickBlocked(), "pick interaction marks pickBlocked on non-finite hit");
    expectTrue(!blockedPick.snapBlocked(),
               "pick interaction snap state remains independent of pick guards");

    const fuse::editor::InteractionPreflight degradedInteraction = fuse::editor::preflightInteraction(
    expectTrue(!degradedInteraction.pickBlocked(), "interaction preflight allows pick on valid hit");
    expectTrue(degradedInteraction.snapBlocked(),
               "interaction preflight marks snapBlocked when snap step invalid");

void testUpdateDragInteractionSnapDragPreflight() {



    const fuse::editor::UpdateDragInteractionPreflight active =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(active.canUpdate(), "update interaction accepts active drag");
    expectTrue(active.snapDragWillApply(), "update interaction reports snap-drag will apply");

    const fuse::editor::UpdateDragInteractionPreflight degraded =
    expectTrue(degraded.canUpdate(), "update interaction still allows drag when snap degraded");
    expectTrue(!degraded.snapDragWillApply(),
               "update interaction rejects snap-drag when step invalid");

void testHitTestNonFiniteGuards() {
    const fuse::f32 nan = std::numeric_limits<fuse::f32>::quiet_NaN();

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = nan;
    hit.screenY = 50.f;
    expectTrue(fuse::editor::isHitTestNonFinite(hit), "NaN screen X is non-finite");
    expectTrue(!fuse::editor::isHitTestOutOfBounds(hit),
               "NaN screen X is not classified as out of bounds");

    hit.screenX = 10.f;
    hit.viewportWidth = nan;
    expectTrue(fuse::editor::isHitTestNonFinite(hit), "NaN viewport width is non-finite");

    const fuse::editor::PickPreflight nanPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(nanPick.nonFiniteInput, "pick preflight marks non-finite viewport");
    expectTrue(nanPick.rejectReason() == fuse::editor::GizmoInteractionRejectReason::NonFiniteInput,
               "pick preflight reject reason is non-finite");
    expectTrue(!nanPick.canPick(), "pick preflight rejects non-finite hit");

    expectTrue(!fuse::editor::isHitTestNonFinite(hit), "valid hit clears non-finite guard");
}






void testRayNonFiniteGuards() {
    fuse::editor::GizmoTransform transform{};

    nanRay.direction.x = nan;
    expectTrue(fuse::editor::isRayNonFinite(nanRay), "NaN ray direction is non-finite");
    expectTrue(!fuse::editor::isRayEmpty(nanRay),
               "NaN direction is not classified as empty ray");

    const fuse::editor::PickPreflight nanPick = fuse::editor::preflightPick(
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(nanPick.nonFiniteInput, "pick preflight marks non-finite ray");
               "ray pick reject reason is non-finite");
    expectTrue(fuse::editor::shouldSkipPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                           fuse::editor::GizmoSpace::World,
                                           fuse::editor::GizmoSystem::kAxisLength,
                                           fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipPick rejects non-finite ray");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(gizmo.shouldSkipPick(nanRay, transform),
               "gizmo shouldSkipPick rejects non-finite ray");

void testAxisModeValidationGuards() {
    expectTrue(fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::X,
                                                fuse::editor::GizmoMode::Translate),
               "X axis valid for translate mode");
    expectTrue(fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::Uniform,
                                                fuse::editor::GizmoMode::Scale),
               "uniform axis valid for scale mode");
    expectTrue(!fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::Uniform,
               "uniform axis invalid for translate mode");
    expectTrue(!fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::None,
               "none axis invalid for any mode");
void testRejectReasonPreflights() {


    const fuse::editor::UpdateDragPreflight mismatchPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, {});
    expectTrue(mismatchPreflight.modeAxisMismatch, "update preflight marks mode-axis mismatch");
    expectTrue(!mismatchPreflight.canUpdate(), "update preflight rejects mode-axis mismatch");
    expectTrue(mismatchPreflight.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::ModeAxisMismatch,
               "update reject reason is mode-axis mismatch");

    const fuse::editor::EndDragPreflight endMismatchPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, {});
    expectTrue(endMismatchPreflight.modeAxisMismatch, "end preflight marks mode-axis mismatch");
    expectTrue(endMismatchPreflight.canEnd(),
               "end preflight still allows end on mode-axis mismatch");
    expectTrue(endMismatchPreflight.rejectReason() ==
               "end reject reason is mode-axis mismatch");

    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    hit.screenX = 50.f;
    gizmo.beginDrag(hit, transform);
    hit.screenX = 55.f;
    expectTrue(gizmo.preflightUpdateDrag(hit).canUpdate(),
               "uniform axis update allowed in scale mode");
    gizmo.endDrag();

void testShouldSkipInteractionPredicates() {

    expectTrue(!fuse::editor::shouldSkipPick(hit, fuse::editor::GizmoMode::Translate),
               "shouldSkipPick allows valid screen hit");
    expectTrue(fuse::editor::shouldSkipBeginDrag(hit, fuse::editor::GizmoMode::Translate, true),
               "shouldSkipBeginDrag rejects while already dragging");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(!fuse::editor::shouldSkipBeginDrag(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
               "shouldSkipBeginDrag allows valid ray pick");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
               "shouldSkipUpdateDrag rejects inactive drag");
    expectTrue(fuse::editor::shouldSkipEndDrag(false), "shouldSkipEndDrag rejects inactive drag");

    expectTrue(!gizmo.shouldSkipPick(hit), "gizmo shouldSkipPick allows valid screen hit");
    expectTrue(!gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag allows valid screen hit");
    expectTrue(!gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag allows active drag");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag allows active drag");
    expectTrue(gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag rejects while dragging");
    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag rejects after end");

void testRejectReasonDiagnostics() {
    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    const fuse::editor::PickPreflight emptyPick =
        fuse::editor::preflightPick(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyPick.rejectReason() == fuse::editor::GizmoInteractionRejectReason::EmptyHit,
               "empty hit pick reject reason");

    const fuse::editor::BeginDragPreflight draggingBegin = fuse::editor::preflightBeginDrag(
        emptyHit, fuse::editor::GizmoMode::Translate, true);
    expectTrue(draggingBegin.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::AlreadyDragging,
               "already dragging begin reject reason");

    const fuse::editor::SnapPreflight disabledSnap =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, {});
    expectTrue(disabledSnap.rejectReason() == fuse::editor::GizmoInteractionRejectReason::SnapDisabled,
               "disabled snap reject reason");

    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::SnapPreflight invalidStepSnap =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidStepSnap.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::InvalidSnapStep,
               "invalid snap step reject reason");
void testNonFiniteInputGuards() {
    expectTrue(fuse::editor::isHitTestCoordinatesInvalid(nanHit),
               "NaN screen X is invalid coordinates");
    expectTrue(!fuse::editor::isHitTestOutOfBounds(nanHit),
               "NaN coordinates are not classified as out of bounds");

    expectTrue(nanPick.invalidCoordinates, "pick preflight marks NaN screen coordinates");
    expectTrue(!nanPick.canPick(), "pick preflight rejects NaN screen coordinates");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
               "tryPickAxis rejects NaN screen coordinates");
    expectTrue(axis == fuse::editor::GizmoAxis::None,
               "NaN screen pick leaves axis unset");

    const fuse::editor::BeginDragPreflight nanBegin =
    expectTrue(nanBegin.invalidCoordinates, "begin preflight marks NaN screen coordinates");
    expectTrue(!nanBegin.canBegin, "begin preflight rejects NaN screen coordinates");

    fuse::editor::GizmoHitTest validHit{};
    validHit.viewportWidth = 100.f;
    validHit.viewportHeight = 100.f;
    validHit.screenX = 10.f;
    validHit.screenY = 50.f;
    gizmo.beginDrag(validHit, transform);

    const fuse::editor::UpdateDragPreflight nanUpdate =
        fuse::editor::preflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X);
    expectTrue(nanUpdate.invalidCoordinates, "update preflight marks NaN screen coordinates");
    expectTrue(!nanUpdate.canUpdate(), "update preflight rejects NaN screen coordinates");
    expectTrue(gizmo.isDragging(), "NaN update reject keeps drag active");

    fuse::editor::GizmoRay nanRay{};
    nanRay.origin = {0.f, 0.f, 0.f};
    nanRay.direction = {std::numeric_limits<fuse::f32>::quiet_NaN(), 0.f, 0.f};
    expectTrue(!fuse::editor::isRayEmpty(nanRay), "NaN direction is not classified as empty");


    const fuse::editor::BeginDragPreflight nanRayBegin = fuse::editor::preflightBeginDrag(
    expectTrue(nanRayBegin.nonFiniteRay, "begin preflight marks non-finite ray");
    expectTrue(!nanRayBegin.canBegin, "begin preflight rejects non-finite ray");
    expectTrue(!gizmo.canBeginDrag(nanRay, transform), "gizmo canBeginDrag rejects non-finite ray");

void testCanActOnPhaseRouting() {
    snap.gridSize = 1.f;


    expectTrue(fuse::editor::canActOnPhase(hit, false, fuse::editor::GizmoAxis::None,
               "idle phase can act via begin path");
    expectTrue(fuse::editor::canActOnPhase(hit, true, fuse::editor::GizmoAxis::X,
               "dragging phase can act via update path on valid hit");
    expectTrue(!fuse::editor::preflightInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                                   fuse::editor::GizmoMode::Translate, snap)
                    .canUpdate(),
               "idle phase rejects update action");

    expectTrue(fuse::editor::canActOnPhase(xRay, transform, false, fuse::editor::GizmoAxis::None,
                                           fuse::editor::GizmoMode::Translate,
                                           fuse::editor::GizmoSystem::kPickRadius, snap),
               "ray idle phase can act via begin path");

    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.canActOnPhase(hit), "gizmo idle phase can act on valid screen hit");
    expectTrue(gizmo.canActOnPhase(xRay, transform),
               "gizmo idle phase can act on valid ray pick");

    expectTrue(gizmo.canActOnPhase(hit), "gizmo dragging phase can act on valid update hit");
    expectTrue(!gizmo.preflightInteraction(hit).canBegin(),
               "dragging phase rejects begin via interaction preflight");

    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.canActOnPhase(hit),
               "dragging phase rejects update on empty viewport but end remains available");
    expectTrue(gizmo.canEndInteraction(), "end path remains available on empty viewport update");

void testPhaseActionPreflight() {


    const fuse::editor::PhaseActionPreflight idleAction = fuse::editor::preflightPhaseAction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(idleAction.phase == fuse::editor::GizmoInteractionPhase::Idle,
               "phase action reports idle when not dragging");
    expectTrue(idleAction.canAct, "phase action allows begin when idle with valid hit");
    expectTrue(idleAction.canInteract, "phase action allows interact when idle with valid hit");
    expectTrue(idleAction.snapWillApply, "phase action reports snap will apply when idle");
    expectTrue(!idleAction.snapDegraded, "valid snap clears snapDegraded on idle phase action");

    const fuse::editor::PhaseActionPreflight activeAction = fuse::editor::preflightPhaseAction(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeAction.phase == fuse::editor::GizmoInteractionPhase::Dragging,
               "phase action reports dragging when active");
    expectTrue(activeAction.canAct, "phase action allows update when dragging with valid hit");
    expectTrue(activeAction.canInteract, "phase action allows interact when dragging with valid hit");
    expectTrue(activeAction.snapWillApply, "phase action reports snap will apply when dragging");

    hit.screenX = -5.f;
    const fuse::editor::PhaseActionPreflight outOfBoundsAction = fuse::editor::preflightPhaseAction(
    expectTrue(!outOfBoundsAction.canAct,
               "phase action rejects primary action on out-of-bounds hit while dragging");
    expectTrue(outOfBoundsAction.canInteract,
               "phase action still allows end on out-of-bounds hit while dragging");

    const fuse::editor::PhaseActionPreflight degradedAction = fuse::editor::preflightPhaseAction(
    expectTrue(degradedAction.canAct,
               "phase action still allows update when snap step invalid");
    expectTrue(degradedAction.snapDegraded,
               "phase action marks snap degraded when step invalid");
    expectTrue(!degradedAction.snapWillApply,
               "phase action reports snap will not apply when step invalid");

               "canActOnPhase accepts valid idle hit");
    expectTrue(fuse::editor::canInteractOnPhase(hit, true, fuse::editor::GizmoAxis::X,
               "canInteractOnPhase accepts active drag with valid hit");
    expectTrue(fuse::editor::snapDegradedOnPhase(hit, true, fuse::editor::GizmoAxis::X,
               "snapDegradedOnPhase marks degraded snap while dragging");
    expectTrue(!fuse::editor::snapWillApplyOnPhase(hit, true, fuse::editor::GizmoAxis::X,
               "snapWillApplyOnPhase false when snap step invalid");

    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase accepts active drag");
    expectTrue(gizmo.canInteractOnPhase(hit), "gizmo canInteractOnPhase accepts active drag");
    expectTrue(gizmo.snapDegradedOnPhase(), "gizmo snapDegradedOnPhase marks degraded snap");
    expectTrue(!gizmo.snapWillApplyOnPhase(),
               "gizmo snapWillApplyOnPhase false when snap step invalid");

    const fuse::editor::PhaseActionPreflight gizmoAction = gizmo.preflightPhaseAction(hit);
    expectTrue(gizmoAction.canAct, "gizmo phase action allows update while dragging");
    expectTrue(gizmoAction.snapDegraded, "gizmo phase action marks snap degraded");

void testInteractionPreflightPhaseSnapHelpers() {


    const fuse::editor::InteractionPreflight idleInteraction = fuse::editor::preflightInteraction(
    expectTrue(idleInteraction.canInteractOnPhase(),
               "idle interaction allows interact on valid begin hit");
    expectTrue(!idleInteraction.begin.snapWillApply(),
               "begin interaction reports snapWillApply false when step invalid");
    expectTrue(idleInteraction.begin.snapDegraded(),
               "begin interaction marks snapDegraded when step invalid");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
    expectTrue(activeInteraction.canInteractOnPhase(),
               "active interaction allows interact on valid update hit");
    expectTrue(activeInteraction.snapDegradedOnPhase(),
               "active interaction marks snapDegradedOnPhase when step invalid");
    expectTrue(!activeInteraction.snapWillApplyOnPhase(),
               "active interaction reports snapWillApplyOnPhase false when step invalid");

    const fuse::editor::InteractionPreflight blockedUpdate = fuse::editor::preflightInteraction(
    expectTrue(!blockedUpdate.canActOnPhase(),
               "blocked update rejects canActOnPhase on out-of-bounds hit");
    expectTrue(blockedUpdate.canInteractOnPhase(),
               "blocked update still allows end via canInteractOnPhase");
    expectTrue(blockedUpdate.canEnd(), "blocked update still allows end");

    const fuse::editor::EndInteractionPreflight endPreflight = fuse::editor::preflightEndInteraction(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!endPreflight.snapWillApply(),
               "end interaction snapWillApply false when step invalid");
    expectTrue(endPreflight.snapDegraded(), "end interaction marks snapDegraded when step invalid");

void testPickRejectReasonGuards() {

    fuse::editor::GizmoRay emptyRay{};
    fuse::editor::PickPreflight pickPreflight{};
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSystem::kPickRadius, pickPreflight,
                                               pickReason),
               "tryPreflightPick rejects empty ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::EmptyRay,
               "empty ray pick reason is EmptyRay");
    expectTrue(fuse::editor::shouldSkipPick(pickPreflight), "shouldSkipPick true on empty ray");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(pickReason), "EmptyRay") == 0,
               "pick reject label matches EmptyRay");

    expectTrue(fuse::editor::tryPreflightPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "tryPreflightPick accepts valid ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::None,
               "valid ray pick reason is None");
    expectTrue(!fuse::editor::shouldSkipPick(pickPreflight), "shouldSkipPick false on valid ray");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!fuse::editor::tryPreflightPick(deadZone, fuse::editor::GizmoMode::Translate,
                                               pickPreflight, pickReason),
               "tryPreflightPick rejects translate dead zone");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::ScreenMiss,
               "dead zone pick reason is ScreenMiss");
    expectTrue(fuse::editor::classifyPickReject(pickPreflight) == pickReason,
               "classifyPickReject mirrors tryPreflightPick reason");

    deadZone.screenX = 10.f;
    expectTrue(!gizmo.shouldSkipPick(deadZone), "gizmo shouldSkipPick false on valid screen hit");
    expectTrue(gizmo.classifyPickReject(deadZone) == fuse::editor::GizmoPickRejectReason::None,
               "gizmo classifyPickReject returns None on valid screen hit");
    expectTrue(gizmo.classifyPickReject(xRay, transform) == fuse::editor::GizmoPickRejectReason::None,
               "gizmo classifyPickReject returns None on valid ray");

void testSnapRejectReasonGuards() {
    fuse::editor::SnapPreflight snapPreflight{};
    fuse::editor::GizmoSnapRejectReason snapReason = fuse::editor::GizmoSnapRejectReason::None;

    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap,
                                               snapPreflight, snapReason),
               "tryPreflightSnap rejects disabled snap");
    expectTrue(snapReason == fuse::editor::GizmoSnapRejectReason::SnapDisabled,
               "disabled snap reason is SnapDisabled");
    expectTrue(fuse::editor::shouldSkipSnap(snapPreflight), "shouldSkipSnap true when disabled");

               "tryPreflightSnap rejects invalid step");
    expectTrue(snapReason == fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "invalid step snap reason is InvalidStep");
    expectTrue(fuse::editor::classifySnapReject(snapPreflight) == snapReason,
               "classifySnapReject mirrors tryPreflightSnap reason");

    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightSnap accepts valid snap");
    expectTrue(snapReason == fuse::editor::GizmoSnapRejectReason::None,
               "valid snap reason is None");

    expectTrue(!gizmo.shouldSkipSnap(), "gizmo shouldSkipSnap false with valid settings");
    expectTrue(gizmo.classifySnapReject() == fuse::editor::GizmoSnapRejectReason::None,
               "gizmo classifySnapReject returns None with valid settings");

void testBeginDragRejectReasonGuards() {
    fuse::editor::BeginDragPreflight beginPreflight{};
    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;

    expectTrue(!fuse::editor::tryPreflightBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate, snap,
                                                    beginPreflight, beginReason),
               "tryPreflightBeginDrag rejects empty viewport");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::EmptyHit,
               "empty viewport begin reason is EmptyHit");
    expectTrue(fuse::editor::shouldSkipBeginDrag(beginPreflight),
               "shouldSkipBeginDrag true on empty viewport");

    expectTrue(fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightBeginDrag accepts valid screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::None,
               "valid begin reason is None");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap,
                                                    beginPreflight, beginReason, true),
               "tryPreflightBeginDrag rejects while already dragging");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "already dragging begin reason is AlreadyDragging");
    expectTrue(fuse::editor::classifyBeginDragReject(beginPreflight) == beginReason,
               "classifyBeginDragReject mirrors tryPreflightBeginDrag reason");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(emptyRay, transform,
                                                    fuse::editor::GizmoSystem::kPickRadius, snap,
               "tryPreflightBeginDrag rejects empty ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::EmptyRay,
               "empty ray begin reason is EmptyRay");

    expectTrue(!gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag false on valid hit");
    expectTrue(gizmo.classifyBeginDragReject(hit) ==
                   fuse::editor::GizmoBeginDragRejectReason::None,
               "gizmo classifyBeginDragReject returns None on valid hit");

void testUpdateDragRejectReasonGuards() {

    fuse::editor::UpdateDragPreflight updatePreflight{};
    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
                                                     fuse::editor::GizmoMode::Translate, snap,
                                                     updatePreflight, updateReason),
               "tryPreflightUpdateDrag rejects inactive drag");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "inactive update reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(updatePreflight),
               "shouldSkipUpdateDrag true when inactive");

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::None,
               "tryPreflightUpdateDrag rejects missing active axis");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis,
               "missing axis update reason is InvalidActiveAxis");

    expectTrue(fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
               "tryPreflightUpdateDrag accepts active drag");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::None,
               "valid update reason is None");
    expectTrue(!gizmo.shouldSkipUpdateDrag(hit),
               "gizmo shouldSkipUpdateDrag false on valid active drag");

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
               "tryPreflightUpdateDrag rejects empty viewport");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::EmptyHit,
               "empty viewport update reason is EmptyHit");
    expectTrue(gizmo.classifyUpdateDragReject(hit) == updateReason,
               "gizmo classifyUpdateDragReject mirrors tryPreflightUpdateDrag reason");

void testEndDragRejectReasonGuards() {
    fuse::editor::EndDragPreflight endPreflight{};
    fuse::editor::GizmoEndDragRejectReason endReason = fuse::editor::GizmoEndDragRejectReason::None;

    expectTrue(!fuse::editor::tryPreflightEndDrag(false, fuse::editor::GizmoAxis::None,
                                                  endPreflight, endReason),
               "tryPreflightEndDrag rejects inactive drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "inactive end reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipEndDrag(endPreflight),
               "shouldSkipEndDrag true when inactive");
    expectTrue(std::strcmp(fuse::editor::gizmoEndDragRejectReasonLabel(endReason), "NotDragging") ==
                   0,
               "end reject label matches NotDragging");

    expectTrue(fuse::editor::tryPreflightEndDrag(true, fuse::editor::GizmoAxis::X,
               "tryPreflightEndDrag accepts active drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragRejectReason::None,
               "valid end reason is None");
    expectTrue(fuse::editor::classifyEndDragReject(endPreflight) == endReason,
               "classifyEndDragReject mirrors tryPreflightEndDrag reason");

    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag true when inactive");

    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag false when dragging");
    expectTrue(gizmo.classifyEndDragReject() == fuse::editor::GizmoEndDragRejectReason::None,
               "gizmo classifyEndDragReject returns None when dragging");

void testGizmoInteractionRejectReasons() {

    fuse::editor::GizmoInteractionRejectReason reason = fuse::editor::GizmoInteractionRejectReason::None;
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::EmptyRay,
               "empty ray reject reason is EmptyRay");
    expectTrue(std::strcmp(fuse::editor::gizmoInteractionRejectReasonLabel(reason), "empty_ray") == 0,
               "empty ray reject label matches");

    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::None,
               "valid ray reject reason is None");

    expectTrue(!fuse::editor::tryPreflightPick(emptyHit, fuse::editor::GizmoMode::Translate, reason),
               "tryPreflightPick rejects empty viewport");
    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::EmptyHit,
               "empty viewport reject reason is EmptyHit");

    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::SnapDisabled,
               "disabled snap reject reason is SnapDisabled");

    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::InvalidSnapStep,
               "invalid snap step reject reason is InvalidSnapStep");

    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "valid snap reject reason is None");

    expectTrue(fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, reason),
               "valid begin reject reason is None");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, reason),
               "tryPreflightBeginDrag rejects translate dead zone");
    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::ScreenMiss,
               "dead zone begin reject reason is ScreenMiss");

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, false, fuse::editor::GizmoAxis::None, reason),
    expectTrue(reason == fuse::editor::GizmoInteractionRejectReason::NotDragging,
               "inactive update reject reason is NotDragging");

    expectTrue(fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X, reason),
               "tryPreflightUpdateDrag accepts active drag with axis");
               "valid update reject reason is None");

                                                  fuse::editor::GizmoMode::Translate, snap, reason),
               "inactive end reject reason is NotDragging");

               "valid end reject reason is None");

void testShouldSkipPreflights() {


    const fuse::editor::PickPreflight validPick =
    expectTrue(!fuse::editor::shouldSkipPick(validPick), "shouldSkipPick false on valid hit");

    const fuse::editor::PickPreflight deadZonePick =
    expectTrue(fuse::editor::shouldSkipPick(deadZonePick),
               "shouldSkipPick true on translate dead zone");
    expectTrue(fuse::editor::classifyPickReject(deadZonePick) ==
                   fuse::editor::GizmoInteractionRejectReason::ScreenMiss,
               "classifyPickReject marks screen miss");

    const fuse::editor::SnapPreflight degradedSnap =
    expectTrue(fuse::editor::shouldSkipSnap(degradedSnap),
               "shouldSkipSnap true when step invalid");
    expectTrue(fuse::editor::classifySnapReject(degradedSnap) ==
               "classifySnapReject marks invalid step");

    const fuse::editor::BeginDragPreflight validBegin =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(!fuse::editor::shouldSkipBeginDrag(validBegin),
               "shouldSkipBeginDrag false on valid hit");

    const fuse::editor::UpdateDragPreflight inactiveUpdate =
        fuse::editor::preflightUpdateDrag(hit, false);
    expectTrue(fuse::editor::shouldSkipUpdateDrag(inactiveUpdate),
               "shouldSkipUpdateDrag true when not dragging");
    expectTrue(fuse::editor::classifyUpdateDragReject(inactiveUpdate) ==
                   fuse::editor::GizmoInteractionRejectReason::NotDragging,
               "classifyUpdateDragReject marks not dragging");

    const fuse::editor::EndDragPreflight inactiveEnd =
        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoAxis::None,
                                       fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::shouldSkipEndDrag(inactiveEnd),
               "shouldSkipEndDrag true when not dragging");

    const fuse::editor::PickInteractionPreflight pickInteraction =
    expectTrue(!fuse::editor::shouldSkipPickInteraction(pickInteraction),
               "shouldSkipPickInteraction false on valid hit");

    const fuse::editor::BeginDragInteractionPreflight beginInteraction =
        fuse::editor::preflightBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!fuse::editor::shouldSkipBeginDragInteraction(beginInteraction),
               "shouldSkipBeginDragInteraction false on valid hit");

    expectTrue(!gizmo.shouldSkipPick(hit), "gizmo shouldSkipPick false on valid hit");
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true when idle");
    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag true when idle");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase true when idle with valid hit");
    expectTrue(!gizmo.shouldSkipInteraction(hit),
               "gizmo shouldSkipInteraction false when begin is allowed");

               "gizmo shouldSkipUpdateDrag false during active drag");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag false during active drag");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase true during active drag");

void testInteractionPrimaryRejectReason() {


    const fuse::editor::InteractionPreflight idleDeadZone = fuse::editor::preflightInteraction(
    expectTrue(!idleDeadZone.canActOnPhase(), "idle dead zone blocks phase action");
    expectTrue(idleDeadZone.primaryRejectReason() ==
               "idle dead zone primary reject is ScreenMiss");
    expectTrue(fuse::editor::shouldSkipInteraction(idleDeadZone),
               "shouldSkipInteraction true when phase action blocked");

    gizmo.beginDrag(hit, fuse::editor::GizmoTransform{});

    const fuse::editor::InteractionPreflight emptyUpdate =
        gizmo.preflightInteraction(hit);
    expectTrue(!emptyUpdate.canActOnPhase(), "empty viewport blocks update phase action");
    expectTrue(emptyUpdate.primaryRejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::EmptyHit,
               "empty viewport primary reject is EmptyHit");
    expectTrue(fuse::editor::classifyInteractionReject(emptyUpdate) ==
               "classifyInteractionReject matches primary reject");

    const fuse::editor::GizmoRay validRay = rayAlongX();
    expectTrue(fuse::editor::isRayFinite(validRay), "isRayFinite accepts valid ray");

    fuse::editor::GizmoRay nanRay = validRay;
    expectTrue(!fuse::editor::isRayFinite(nanRay), "isRayFinite rejects NaN direction");

    expectTrue(fuse::editor::isHitTestFinite(hit), "isHitTestFinite accepts valid screen hit");

    hit.screenX = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::isHitTestFinite(hit), "isHitTestFinite rejects infinite screen X");

    const fuse::editor::PickPreflight nonFinitePick =
    expectTrue(nonFinitePick.canPick(), "finite screen hit passes pick preflight");

    expectTrue(nanPick.nonFiniteHit, "pick preflight marks non-finite screen hit");
    expectTrue(!nanPick.canPick(), "pick preflight rejects non-finite screen hit");

    expectTrue(!fuse::editor::tryPickAxis(hit, fuse::editor::GizmoMode::Translate, axis),

    const fuse::editor::PickPreflight nonFiniteRayPick = fuse::editor::preflightPick(
        nanRay, fuse::editor::GizmoTransform{}, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(nonFiniteRayPick.nonFiniteRay, "pick preflight marks non-finite ray");
    expectTrue(!nonFiniteRayPick.canPick(), "pick preflight rejects non-finite ray");

    const fuse::editor::BeginDragPreflight beginPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate);
    expectTrue(beginPreflight.canBegin, "begin preflight accepts finite screen hit");

    const fuse::editor::BeginDragPreflight nonFiniteBegin = fuse::editor::preflightBeginDrag(
    expectTrue(nonFiniteBegin.nonFiniteHit, "begin preflight marks non-finite screen hit");
    expectTrue(!nonFiniteBegin.canBegin, "begin preflight rejects non-finite screen hit");

    const fuse::editor::UpdateDragPreflight updatePreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(updatePreflight.nonFiniteHit, "update preflight marks non-finite screen hit");
    expectTrue(!updatePreflight.canUpdate(), "update preflight rejects non-finite screen hit");
    fuse::editor::GizmoResult updateResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, updateResult),
               "tryUpdateDrag rejects non-finite screen hit");


    const fuse::editor::SnapDragPreflight disabledPreflight =
        fuse::editor::preflightSnapDragDelta(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap-drag preflight marks disabled snap");
    expectTrue(!disabledPreflight.canApply(), "snap-drag preflight rejects disabled snap");

    expectTrue(degradedPreflight.isDegraded(), "snap-drag preflight marks degraded snap");
    expectTrue(!degradedPreflight.canApply(), "snap-drag preflight rejects invalid step");

    snap.gridSize = 0.5f;
    expectTrue(validPreflight.canApply(), "snap-drag preflight accepts valid snap settings");
    expectTrue(fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta mirrors valid snap-drag preflight");

    expectTrue(gizmo.preflightSnapDragDelta().canApply(),
               "gizmo snap-drag preflight accepts valid settings");

void testCanActOnPhaseGuards() {
void testCanInteractionPredicates() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(fuse::editor::canActOnInteractionPhase(hit, false, fuse::editor::GizmoAxis::None,
                                                     fuse::editor::GizmoMode::Translate, snap),
               "canActOnInteractionPhase allows begin when idle");
    expectTrue(fuse::editor::canActOnInteractionPhase(hit, true, fuse::editor::GizmoAxis::X,
                                                     fuse::editor::GizmoMode::Translate, snap),
               "canActOnInteractionPhase allows update when dragging");

    hit.screenX = -5.f;
    expectTrue(!fuse::editor::canActOnInteractionPhase(hit, true, fuse::editor::GizmoAxis::X,
                                                       fuse::editor::GizmoMode::Translate, snap),
               "canActOnInteractionPhase rejects out-of-bounds update hit");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(fuse::editor::canActOnInteractionPhase(
                   xRay, transform, false, fuse::editor::GizmoAxis::None,
                   fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
                   fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius,
                   snap),
               "canActOnInteractionPhase allows begin for valid ray when idle");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    hit.screenX = 10.f;
    expectTrue(gizmo.interactionPhase() == fuse::editor::GizmoInteractionPhase::Idle,
               "gizmo interactionPhase reports idle before drag");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase allows begin when idle");
    expectTrue(gizmo.canActOnPhase(xRay, transform),
               "gizmo canActOnPhase allows begin for valid ray when idle");

    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.interactionPhase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "gizmo interactionPhase reports dragging during drag");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase allows update when dragging");
    expectTrue(gizmo.canActOnPhase(xRay, transform),
               "gizmo ray canActOnPhase routes to update while dragging");

    hit.screenX = -5.f;
    expectTrue(!gizmo.canActOnPhase(hit),
               "gizmo canActOnPhase rejects out-of-bounds update hit while dragging");
    gizmo.endDrag();
}

void testCanInteractionPredicates() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoPickGuardRejectReason pickReason = fuse::editor::GizmoPickGuardRejectReason::None;
    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(fuse::editor::tryPreflightPick(xRay, transform, fuse::editor::GizmoMode::Translate,
                                              fuse::editor::GizmoSystem::kPickRadius, pickReason),
               "tryPreflightPick accepts valid ray");
    expectTrue(pickReason == fuse::editor::GizmoPickGuardRejectReason::None,
               "valid ray pick reject reason is None");
    expectTrue(!fuse::editor::shouldSkipPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "shouldSkipPick false for valid ray");

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!fuse::editor::tryPreflightPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
               "tryPreflightPick rejects empty ray");
    expectTrue(pickReason == fuse::editor::GizmoPickGuardRejectReason::EmptyRay,
               "empty ray pick reject reason is EmptyRay");
    expectTrue(std::strcmp(fuse::editor::gizmoPickGuardRejectReasonLabel(pickReason), "empty_ray") ==
                   0,
               "pick reject reason label matches empty_ray");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(fuse::editor::tryPreflightPick(hit, fuse::editor::GizmoMode::Translate, pickReason),
               "tryPreflightPick accepts valid screen hit");
               "valid screen hit pick reject reason is None");

    hit.screenX = 50.f;
    expectTrue(!fuse::editor::tryPreflightPick(hit, fuse::editor::GizmoMode::Translate, pickReason),
               "tryPreflightPick rejects translate dead zone");
    expectTrue(pickReason == fuse::editor::GizmoPickGuardRejectReason::ScreenMiss,
               "dead zone pick reject reason is ScreenMiss");
    expectTrue(fuse::editor::classifyPickReject(fuse::editor::preflightPick(
                   hit, fuse::editor::GizmoMode::Translate)) ==
                   fuse::editor::GizmoPickGuardRejectReason::ScreenMiss,
               "classifyPickReject maps screen miss");

    fuse::editor::GizmoSnapGuardRejectReason snapReason =
        fuse::editor::GizmoSnapGuardRejectReason::None;
    fuse::editor::GizmoSnapSettings disabledSnap{};
    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, disabledSnap,
                                               snapReason),
               "tryPreflightSnap rejects disabled snap");
    expectTrue(snapReason == fuse::editor::GizmoSnapGuardRejectReason::SnapDisabled,
               "disabled snap reject reason is SnapDisabled");
    expectTrue(fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, disabledSnap),
               "shouldSkipSnap true when snap disabled");

    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, snapReason),
               "tryPreflightSnap rejects invalid step");
    expectTrue(snapReason == fuse::editor::GizmoSnapGuardRejectReason::InvalidStep,
               "invalid step snap reject reason is InvalidStep");
    expectTrue(fuse::editor::classifySnapReject(fuse::editor::preflightSnap(
                   fuse::editor::GizmoMode::Translate, snap)) ==
                   fuse::editor::GizmoSnapGuardRejectReason::InvalidStep,
               "classifySnapReject maps invalid step");

    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, snapReason),
               "tryPreflightSnap accepts valid snap");
    expectTrue(snapReason == fuse::editor::GizmoSnapGuardRejectReason::None,
               "valid snap reject reason is None");

    fuse::editor::GizmoBeginDragGuardRejectReason beginReason =
        fuse::editor::GizmoBeginDragGuardRejectReason::None;
    expectTrue(fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap,
                                                   beginReason),
               "tryPreflightBeginDrag accepts valid screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragGuardRejectReason::None,
               "valid begin reject reason is None");
    expectTrue(!fuse::editor::shouldSkipBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate,
                                                             snap),
               "shouldSkipBeginDragInteraction false for valid hit");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap,
                                                    beginReason, true),
               "tryPreflightBeginDrag rejects while already dragging");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragGuardRejectReason::AlreadyDragging,
               "already dragging begin reject reason is AlreadyDragging");
    expectTrue(fuse::editor::shouldSkipBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap, true),
               "shouldSkipBeginDrag true while already dragging");

    fuse::editor::GizmoUpdateDragGuardRejectReason updateReason =
        fuse::editor::GizmoUpdateDragGuardRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects inactive drag");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragGuardRejectReason::NotDragging,
               "inactive update reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, false), "shouldSkipUpdateDrag true when idle");

    expectTrue(fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
                                                    fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightUpdateDrag accepts active drag");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragGuardRejectReason::None,
               "valid update reject reason is None");

    hit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
               "tryPreflightUpdateDrag rejects empty viewport");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragGuardRejectReason::EmptyHit,
               "empty viewport update reject reason is EmptyHit");
    expectTrue(fuse::editor::classifyUpdateDragReject(fuse::editor::preflightUpdateDrag(
                   hit, true, fuse::editor::GizmoAxis::X)) ==
                   fuse::editor::GizmoUpdateDragGuardRejectReason::EmptyHit,
               "classifyUpdateDragReject maps empty hit");

    fuse::editor::GizmoEndDragGuardRejectReason endReason =
        fuse::editor::GizmoEndDragGuardRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightEndDrag(false, fuse::editor::GizmoAxis::None,
                                                  fuse::editor::GizmoMode::Translate, snap, endReason),
               "tryPreflightEndDrag rejects inactive drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragGuardRejectReason::NotDragging,
               "inactive end reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipEndDrag(false), "shouldSkipEndDrag true when idle");

    expectTrue(fuse::editor::tryPreflightEndDrag(true, fuse::editor::GizmoAxis::X,
               "tryPreflightEndDrag accepts active drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragGuardRejectReason::None,
               "valid end reject reason is None");
    expectTrue(!fuse::editor::shouldSkipEndDragInteraction(true, fuse::editor::GizmoAxis::X,
                                                           fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipEndDragInteraction false while dragging");

    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoPickGuardRejectReason gizmoPickReason =
        fuse::editor::GizmoPickGuardRejectReason::PickMiss;
    expectTrue(gizmo.tryPreflightPick(hit, gizmoPickReason),
               "gizmo tryPreflightPick accepts valid screen hit");
    expectTrue(gizmoPickReason == fuse::editor::GizmoPickGuardRejectReason::None,
               "gizmo pick reject reason is None on valid hit");
    expectTrue(!gizmo.shouldSkipPick(hit), "gizmo shouldSkipPick false for valid hit");

    fuse::editor::GizmoSnapGuardRejectReason gizmoSnapReason =
        fuse::editor::GizmoSnapGuardRejectReason::InvalidStep;
    expectTrue(gizmo.tryPreflightSnap(gizmoSnapReason), "gizmo tryPreflightSnap accepts valid snap");
    expectTrue(gizmoSnapReason == fuse::editor::GizmoSnapGuardRejectReason::None,
               "gizmo snap reject reason is None on valid snap");
    expectTrue(!gizmo.shouldSkipSnap(), "gizmo shouldSkipSnap false when snap valid");

    gizmo.beginDrag(hit, transform);
    fuse::editor::GizmoBeginDragGuardRejectReason gizmoBeginReason =
    expectTrue(!gizmo.tryPreflightBeginDrag(hit, gizmoBeginReason),
               "gizmo tryPreflightBeginDrag rejects while dragging");
    expectTrue(gizmoBeginReason == fuse::editor::GizmoBeginDragGuardRejectReason::AlreadyDragging,
               "gizmo begin reject reason is AlreadyDragging while dragging");
    expectTrue(gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag true while dragging");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag false while dragging");
    gizmo.endDrag();

void testCanInteractionPredicates() {

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateDragPreflight mismatchPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, {});
    expectTrue(mismatchPreflight.modeAxisMismatch, "update preflight marks mode-axis mismatch");
    expectTrue(!mismatchPreflight.canUpdate(), "update preflight rejects mode-axis mismatch");
    expectTrue(mismatchPreflight.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::ModeAxisMismatch,
               "update reject reason is mode-axis mismatch");

    const fuse::editor::EndDragPreflight endMismatchPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, {});
    expectTrue(endMismatchPreflight.modeAxisMismatch, "end preflight marks mode-axis mismatch");
    expectTrue(endMismatchPreflight.canEnd(),
               "end preflight still allows end on mode-axis mismatch");
    expectTrue(endMismatchPreflight.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::ModeAxisMismatch,
               "end reject reason is mode-axis mismatch");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    fuse::editor::GizmoTransform transform{};
    hit.screenX = 50.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, transform);
    hit.screenX = 55.f;
    expectTrue(gizmo.preflightUpdateDrag(hit).canUpdate(),
               "uniform axis update allowed in scale mode");
    gizmo.endDrag();
}

void testShouldSkipInteractionPredicates() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(!fuse::editor::shouldSkipPick(hit, fuse::editor::GizmoMode::Translate),
               "shouldSkipPick allows valid screen hit");
    expectTrue(fuse::editor::shouldSkipBeginDrag(hit, fuse::editor::GizmoMode::Translate, true),
               "shouldSkipBeginDrag rejects while already dragging");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    fuse::editor::GizmoTransform transform{};
    expectTrue(!fuse::editor::shouldSkipBeginDrag(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipBeginDrag allows valid ray pick");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
                                                  fuse::editor::GizmoMode::Translate),
               "shouldSkipUpdateDrag rejects inactive drag");
    expectTrue(fuse::editor::shouldSkipEndDrag(false), "shouldSkipEndDrag rejects inactive drag");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.shouldSkipPick(hit), "gizmo shouldSkipPick allows valid screen hit");
    expectTrue(!gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag allows valid screen hit");
    gizmo.beginDrag(hit, transform);
    expectTrue(!gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag allows active drag");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag allows active drag");
    expectTrue(gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag rejects while dragging");
    gizmo.endDrag();
    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag rejects after end");
}

void testRejectReasonDiagnostics() {
    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    const fuse::editor::PickPreflight emptyPick =
        fuse::editor::preflightPick(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyPick.rejectReason() == fuse::editor::GizmoInteractionRejectReason::EmptyHit,
               "empty hit pick reject reason");

    const fuse::editor::BeginDragPreflight draggingBegin = fuse::editor::preflightBeginDrag(
        emptyHit, fuse::editor::GizmoMode::Translate, true);
    expectTrue(draggingBegin.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::AlreadyDragging,
               "already dragging begin reject reason");

    const fuse::editor::SnapPreflight disabledSnap =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, {});
    expectTrue(disabledSnap.rejectReason() == fuse::editor::GizmoInteractionRejectReason::SnapDisabled,
               "disabled snap reject reason");

    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::SnapPreflight invalidStepSnap =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidStepSnap.rejectReason() ==
                   fuse::editor::GizmoInteractionRejectReason::InvalidSnapStep,
               "invalid snap step reject reason");
}

void testCanInteractionPredicates() {


    expectTrue(fuse::editor::canPickSnap(hit, fuse::editor::GizmoMode::Translate, snap),
               "canPickSnap accepts valid screen hit");
    expectTrue(fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginInteraction accepts valid screen hit");
    expectTrue(!fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                                  true),
               "canBeginInteraction rejects while dragging");
    expectTrue(fuse::editor::canUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
               "canUpdateInteraction accepts active drag");
    expectTrue(!fuse::editor::canUpdateInteraction(hit, false, fuse::editor::GizmoAxis::None,
               "canUpdateInteraction rejects inactive drag");
    expectTrue(fuse::editor::canEndInteraction(true, fuse::editor::GizmoAxis::X,
               "canEndInteraction accepts active drag");
    expectTrue(!fuse::editor::canEndInteraction(false, fuse::editor::GizmoAxis::None,
               "canEndInteraction rejects inactive drag");

    expectTrue(fuse::editor::canPickSnap(xRay, transform, fuse::editor::GizmoMode::Translate,
                                         fuse::editor::GizmoSpace::World,
                                         fuse::editor::GizmoSystem::kAxisLength,
                                         fuse::editor::GizmoSystem::kPickRadius, snap),
               "canPickSnap accepts valid ray");
    expectTrue(fuse::editor::canBeginInteraction(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
               "canBeginInteraction accepts valid ray");

    expectTrue(fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginDrag with settings accepts valid screen hit");
    expectTrue(fuse::editor::canBeginDrag(xRay, transform, fuse::editor::GizmoMode::Translate,
               "canBeginDrag with settings accepts valid ray");

    expectTrue(gizmo.canPickSnap(hit), "gizmo canPickSnap accepts valid screen hit");
    expectTrue(gizmo.canBeginInteraction(hit), "gizmo canBeginInteraction accepts valid screen hit");
    expectTrue(gizmo.canUpdateInteraction(hit), "gizmo canUpdateInteraction accepts active drag");
    expectTrue(gizmo.canEndInteraction(), "gizmo canEndInteraction accepts active drag");

void testPickRejectReasonGuards() {

    fuse::editor::GizmoRay emptyRay{};
    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
               "tryPreflightPick rejects empty ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::EmptyRay,
               "empty ray reject reason is EmptyRay");
    expectTrue(fuse::editor::shouldSkipPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                            fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipPick true for empty ray");

    expectTrue(fuse::editor::tryPreflightPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "tryPreflightPick accepts valid ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::None,
               "valid ray reject reason is None");
    expectTrue(!fuse::editor::shouldSkipPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "shouldSkipPick false for valid ray");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    fuse::editor::GizmoPickRejectReason screenReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::preflightPickReady(deadZone, fuse::editor::GizmoMode::Translate,
                                                 &screenReason),
               "preflightPickReady rejects translate dead zone");
    expectTrue(screenReason == fuse::editor::GizmoPickRejectReason::ScreenMiss,
               "dead zone reject reason is ScreenMiss");
    expectTrue(fuse::editor::shouldSkipPick(deadZone, fuse::editor::GizmoMode::Translate),
               "shouldSkipPick true for dead zone");

    deadZone.screenX = -5.f;
    expectTrue(!fuse::editor::tryPreflightPick(deadZone, fuse::editor::GizmoMode::Translate,
                                               screenReason),
               "tryPreflightPick rejects out-of-bounds hit");
    expectTrue(screenReason == fuse::editor::GizmoPickRejectReason::OutOfBounds,
               "out-of-bounds reject reason is OutOfBounds");

    const fuse::editor::PickPreflight missPick = fuse::editor::preflightPick(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(fuse::editor::classifyPickReject(missPick) ==
                   fuse::editor::GizmoPickRejectReason::EmptyRay,
               "classifyPickReject maps emptyRay flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::EmptyRay),
               "EmptyRay") == 0,
               "pick reject reason label for EmptyRay");

    deadZone.screenX = 10.f;
    expectTrue(gizmo.preflightPickReady(deadZone), "gizmo preflightPickReady accepts valid hit");
    expectTrue(gizmo.tryPreflightPick(deadZone, screenReason),
               "gizmo tryPreflightPick accepts valid hit");
    expectTrue(!gizmo.shouldSkipPick(deadZone), "gizmo shouldSkipPick false for valid hit");

void testSnapRejectReasonGuards() {

    fuse::editor::GizmoSnapRejectReason reason = fuse::editor::GizmoSnapRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::SnapDisabled,
               "disabled snap reject reason is SnapDisabled");
    expectTrue(fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap true when snap disabled");

    expectTrue(!fuse::editor::preflightSnapReady(fuse::editor::GizmoMode::Translate, snap, &reason),
               "preflightSnapReady rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "invalid step reject reason is InvalidStep");

    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap accepts valid snap");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::None,
               "valid snap reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap false when snap valid");

    const fuse::editor::SnapPreflight invalidStepPreflight =
    expectTrue(fuse::editor::classifySnapReject(invalidStepPreflight) ==
                   fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "classifySnapReject maps invalidStep flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapRejectReasonLabel(
                   fuse::editor::GizmoSnapRejectReason::SnapDisabled),
               "SnapDisabled") == 0,
               "snap reject reason label for SnapDisabled");

    expectTrue(gizmo.preflightSnapReady(), "gizmo preflightSnapReady accepts valid snap");
    expectTrue(gizmo.tryPreflightSnap(reason), "gizmo tryPreflightSnap accepts valid snap");
    expectTrue(!gizmo.shouldSkipSnap(), "gizmo shouldSkipSnap false when snap valid");

void testBeginDragRejectReasonGuards() {

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    fuse::editor::GizmoBeginDragRejectReason reason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate,
                                                    reason),
               "tryPreflightBeginDrag rejects empty viewport");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::EmptyHit,
               "empty viewport reject reason is EmptyHit");
    expectTrue(fuse::editor::shouldSkipBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate),
               "shouldSkipBeginDrag true for empty viewport");

    expectTrue(fuse::editor::preflightBeginDragReady(hit, fuse::editor::GizmoMode::Translate,
                                                     &reason),
               "preflightBeginDragReady accepts valid screen hit");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::None,
               "valid begin reject reason is None");

    expectTrue(!fuse::editor::preflightBeginDragReady(hit, fuse::editor::GizmoMode::Translate,
                                                      &reason, true),
               "preflightBeginDragReady rejects while already dragging");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "already dragging reject reason is AlreadyDragging");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(
                   emptyRay, transform, fuse::editor::GizmoMode::Translate,
               "tryPreflightBeginDrag rejects empty ray");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::EmptyRay,
               "empty ray begin reject reason is EmptyRay");

    const fuse::editor::BeginDragPreflight draggingPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, true);
    expectTrue(fuse::editor::classifyBeginDragReject(draggingPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "classifyBeginDragReject maps alreadyDragging flag");

    expectTrue(gizmo.preflightBeginDragReady(hit), "gizmo preflightBeginDragReady accepts valid hit");
    expectTrue(!gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag false for valid hit");
    expectTrue(gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag true while dragging");

void testUpdateDragRejectReasonGuards() {

    fuse::editor::GizmoUpdateDragRejectReason reason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
               "tryPreflightUpdateDrag rejects inactive drag");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "inactive drag reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, false, fuse::editor::GizmoAxis::None),
               "shouldSkipUpdateDrag true when not dragging");

    expectTrue(!fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::None,
               "preflightUpdateDragReady rejects drag without axis");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis,
               "missing axis reject reason is InvalidActiveAxis");

    expectTrue(fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::X,
               "preflightUpdateDragReady accepts active drag with axis");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::None,
               "valid update reject reason is None");

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X, reason),
               "tryPreflightUpdateDrag rejects empty viewport");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::EmptyHit,
               "empty viewport update reject reason is EmptyHit");

    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "shouldSkipUpdateDrag true for out-of-bounds hit");
    expectTrue(!fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::X,
               "preflightUpdateDragReady rejects out-of-bounds hit");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::OutOfBounds,
               "out-of-bounds update reject reason is OutOfBounds");

    const fuse::editor::UpdateDragPreflight inactivePreflight =
        fuse::editor::preflightUpdateDrag(hit, false);
    expectTrue(fuse::editor::classifyUpdateDragReject(inactivePreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "classifyUpdateDragReject maps notDragging flag");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis),
               "InvalidActiveAxis") == 0,
               "update reject reason label for InvalidActiveAxis");

    expectTrue(gizmo.preflightUpdateDragReady(hit), "gizmo preflightUpdateDragReady accepts drag");
    expectTrue(!gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag false during drag");
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for empty hit");

void testEndDragRejectReasonGuards() {
    fuse::editor::GizmoEndDragRejectReason reason = fuse::editor::GizmoEndDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightEndDrag(false, reason),
               "tryPreflightEndDrag rejects inactive drag");
    expectTrue(reason == fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "inactive end reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipEndDrag(false), "shouldSkipEndDrag true when not dragging");

    expectTrue(fuse::editor::preflightEndDragReady(true, &reason),
               "preflightEndDragReady accepts active drag");
    expectTrue(reason == fuse::editor::GizmoEndDragRejectReason::None,
               "valid end reject reason is None");
    expectTrue(!fuse::editor::shouldSkipEndDrag(true), "shouldSkipEndDrag false when dragging");

    const fuse::editor::EndDragPreflight inactivePreflight =
        fuse::editor::preflightEndDrag(false, fuse::editor::GizmoAxis::None,
                                     fuse::editor::GizmoMode::Translate, {});
    expectTrue(fuse::editor::classifyEndDragReject(inactivePreflight) ==
                   fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "classifyEndDragReject maps notDragging flag");
    expectTrue(std::strcmp(fuse::editor::gizmoEndDragRejectReasonLabel(
                   fuse::editor::GizmoEndDragRejectReason::NotDragging),
               "NotDragging") == 0,
               "end reject reason label for NotDragging");

    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag true when idle");

    expectTrue(gizmo.preflightEndDragReady(), "gizmo preflightEndDragReady accepts active drag");
    expectTrue(gizmo.tryPreflightEndDrag(reason), "gizmo tryPreflightEndDrag accepts active drag");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag false during drag");

fuse::editor::GizmoHitTest hitFromValidScreen() {
    return hit;

void testNonFiniteRejectReasonClassify() {

    nanRay.origin.x = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(fuse::editor::classifyPickReject(nanRayPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "classifyPickReject maps nonFiniteRay flag");

    expectTrue(fuse::editor::classifyPickReject(nanHitPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");

    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanHit, fuse::editor::GizmoMode::Translate,
                                                 pickReason),
               "tryPreflightPick rejects non-finite screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit pick reject reason is NonFiniteHit");

    const fuse::editor::BeginDragPreflight nanBegin =
    expectTrue(fuse::editor::classifyBeginDragReject(nanBegin) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "classifyBeginDragReject maps nonFiniteHit flag");

    gizmo.beginDrag(hitFromValidScreen(), transform);
    nanHit.screenX = 10.f;
    nanHit.screenY = std::numeric_limits<fuse::f32>::infinity();
    const fuse::editor::UpdateDragPreflight nanUpdate = gizmo.preflightUpdateDrag(nanHit);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanUpdate) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");

void testUpdateDragInteractionDeltaPreflight() {

    const fuse::editor::GizmoHitTest hit = hitFromValidScreen();


    const fuse::editor::UpdateDragInteractionPreflight zeroDelta =
    const fuse::editor::UpdateDragInteractionPreflight finiteDelta =
        gizmo.preflightUpdateDragInteraction(hit, 0.37f);
    expectTrue(zeroDelta.canUpdate(), "update interaction accepts active drag with zero delta");
    expectTrue(finiteDelta.canUpdate(), "update interaction accepts active drag with finite delta");
    expectTrue(finiteDelta.snapDragWillApply(), "finite delta enables snap-drag apply");

    const fuse::editor::UpdateDragInteractionPreflight nanDelta =
        gizmo.preflightUpdateDragInteraction(hit, std::numeric_limits<fuse::f32>::quiet_NaN());
    expectTrue(nanDelta.canUpdate(), "update interaction still allows drag with non-finite delta");
    expectTrue(!nanDelta.snapDragWillApply(),
               "non-finite delta blocks snap-drag apply in interaction preflight");

void testSnapStepFiniteGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = std::numeric_limits<fuse::f32>::quiet_NaN();

    expectTrue(!fuse::editor::isSnapStepFinite(fuse::editor::GizmoMode::Translate, snap),
               "isSnapStepFinite rejects NaN translate step");

    const fuse::editor::SnapPreflight nanStepPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(nanStepPreflight.nonFiniteStep, "snap preflight marks non-finite step");
    expectTrue(!nanStepPreflight.canApply(), "snap preflight rejects non-finite step");
    expectTrue(fuse::editor::classifySnapReject(nanStepPreflight) ==
                   fuse::editor::GizmoSnapRejectReason::NonFiniteStep,
               "classifySnapReject maps nonFiniteStep flag");

    fuse::editor::GizmoSnapRejectReason reason = fuse::editor::GizmoSnapRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap rejects non-finite step");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::NonFiniteStep,
               "non-finite step reject reason is NonFiniteStep");
}

void testNonFiniteRejectReasonGuards() {
void testPickNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.x = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, pickReason),
               "tryPreflightPick rejects non-finite ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray pick reject reason is NonFiniteRay");
    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::None;
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::preflightPickReady(nanHit, fuse::editor::GizmoMode::Translate,
                                                 &pickReason),
               "preflightPickReady rejects non-finite hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit pick reject reason is NonFiniteHit");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
               "tryPreflightBeginDrag rejects non-finite hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite hit begin reject reason is NonFiniteHit");

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite hit update reject reason is NonFiniteHit");

    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;
    fuse::editor::GizmoSnapDragRejectReason snapDragReason =
        fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.25f, fuse::editor::GizmoMode::Translate, snap,
                                                  snapDragReason),
               "tryPreflightSnapDrag accepts valid delta");
    expectTrue(snapDragReason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(snapDragReason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta snap-drag reject reason is DeltaNonFinite");
                                                 &reason),
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit reject reason is NonFiniteHit");

    const fuse::editor::PickPreflight nanPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");
}

void testBeginDragNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::infinity();
    nanHit.screenY = 50.f;
    fuse::editor::GizmoBeginDragRejectReason reason =
                                                    reason),
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.direction.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::preflightBeginDragReady(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, &reason),
               "preflightBeginDragReady rejects non-finite ray");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite begin reject reason is NonFiniteRay");

    const fuse::editor::BeginDragPreflight nanPreflight =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyBeginDragReject(nanPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "classifyBeginDragReject maps nonFiniteHit flag");

void testUpdateDragNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoUpdateDragRejectReason reason =
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X, reason),
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "shouldSkipUpdateDrag true for non-finite hit");

    const fuse::editor::UpdateDragPreflight nanPreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanPreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");

    fuse::editor::GizmoSystem gizmo;
    gizmo.beginDrag(hit, transform);
    hit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for non-finite hit");
    gizmo.endDrag();

void testSnapDragRejectReasonGuards() {

    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    snap.translateSnap = false;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");

    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightSnapDrag rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

    expectTrue(fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
               "preflightSnapDragReady accepts valid snap-drag");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false when snap-drag valid");

    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::quiet_NaN(), fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(nanPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "classifySnapDragReject maps deltaNonFinite flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDragReady(0.25f), "gizmo preflightSnapDragReady accepts delta");
    expectTrue(gizmo.shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity()),
               "gizmo shouldSkipSnapDrag true for non-finite delta");
}

void testUpdateDragInteractionUsesDragDelta() {
void testNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, pickReason),
               "tryPreflightPick rejects non-finite ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::tryPreflightPick(nanHit, fuse::editor::GizmoMode::Translate,
                                               pickReason),
               "tryPreflightPick rejects non-finite screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit reject reason is NonFiniteHit");

    const fuse::editor::PickPreflight nanPick = fuse::editor::preflightPick(nanHit,
                                                                            fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, beginReason),
               "tryPreflightBeginDrag rejects non-finite ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoTransform current{};
    gizmo.beginDrag(hit, current);
    hit.screenY = std::numeric_limits<fuse::f32>::infinity();

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");

    const fuse::editor::UpdateDragPreflight nanUpdate =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanUpdate) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");
    gizmo.endDrag();
}

void testSnapDragRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason),
               "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");
    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                  reason),
               "tryPreflightSnapDrag accepts valid delta and snap");
void testPickRejectReasonNonFiniteGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
               "tryPreflightPick rejects non-finite ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::preflightPickReady(nanHit, fuse::editor::GizmoMode::Translate,
                                                 &reason),
               "preflightPickReady rejects non-finite hit");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipPick(nanHit, fuse::editor::GizmoMode::Translate),
               "shouldSkipPick true for non-finite hit");

    const fuse::editor::PickPreflight nanPick = fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");
}

void testBeginDragRejectReasonNonFiniteGuards() {
    nanHit.screenX = std::numeric_limits<fuse::f32>::infinity();

    fuse::editor::GizmoBeginDragRejectReason reason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");

    nanRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::preflightBeginDragReady(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, &reason),
               "preflightBeginDragReady rejects non-finite ray");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite begin reject reason is NonFiniteRay");
    expectTrue(fuse::editor::shouldSkipBeginDrag(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                                 fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipBeginDrag true for non-finite ray");

    const fuse::editor::BeginDragPreflight nanPreflight =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyBeginDragReject(nanPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "classifyBeginDragReject maps nonFiniteHit flag");

void testUpdateDragRejectReasonNonFiniteGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoUpdateDragRejectReason reason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X, reason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "shouldSkipUpdateDrag true for non-finite hit");

    const fuse::editor::UpdateDragPreflight nanPreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanPreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");

    fuse::editor::GizmoSystem gizmo;
    gizmo.beginDrag(hit, transform);
    expectTrue(!gizmo.tryPreflightUpdateDrag(hit, reason),
               "gizmo tryPreflightUpdateDrag rejects non-finite hit");
               "gizmo non-finite update reject reason is NonFiniteHit");
    gizmo.endDrag();


    fuse::editor::GizmoSnapDragRejectReason reason =
        fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                   reason),
               "tryPreflightSnapDrag rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");
    expectTrue(fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true when snap disabled");

    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
    snap.translateSnap = true;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

               "tryPreflightSnapDrag accepts valid snap-drag");
    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta snap-drag reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    snap.gridSize = 0.5f;
    expectTrue(fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                    &reason),
               "preflightSnapDragReady accepts valid delta and snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false when snap-drag valid");

    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),

    snap.translateSnap = false;
               "preflightSnapDragReady rejects disabled snap");


               "non-finite delta snap-drag reject reason is DeltaNonFinite");

    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::infinity(), fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(nanPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "classifySnapDragReject maps deltaNonFinite flag");
    const fuse::editor::SnapDragPreflight invalidStepPreflight = fuse::editor::preflightSnapDrag(
        0.37f, fuse::editor::GizmoMode::Translate, snap);
    snap.gridSize = 0.f;
    const fuse::editor::SnapDragPreflight degradedPreflight = fuse::editor::preflightSnapDrag(
        0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(degradedPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "classifySnapDragReject maps invalidStep flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    gizmo.setSnapSettings(snap);
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false when valid");
    expectTrue(gizmo.shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity()),
               "gizmo shouldSkipSnapDrag true for non-finite delta");

void testNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, pickReason),
               "tryPreflightPick rejects non-finite ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray pick reject reason is NonFiniteRay");
               "non-finite ray reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::preflightPickReady(nanHit, fuse::editor::GizmoMode::Translate,
                                                 &pickReason),
               "preflightPickReady rejects non-finite hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit pick reject reason is NonFiniteHit");
               "non-finite hit reject reason is NonFiniteHit");

    const fuse::editor::PickPreflight nanPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
               "tryPreflightBeginDrag rejects non-finite hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite hit begin reject reason is NonFiniteHit");

    nanRay = rayAlongX();
    nanRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::preflightBeginDragReady(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, &beginReason),
               "non-finite begin reject reason is NonFiniteHit");

    nanRay.direction.z = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::preflightBeginDragReady(nanRay, transform,
                                                      fuse::editor::GizmoMode::Translate,
                                                      fuse::editor::GizmoSystem::kPickRadius,
                                                      &beginReason),
               "preflightBeginDragReady rejects non-finite ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform current{};
    gizmo.beginDrag(hit, current);

    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!gizmo.tryPreflightUpdateDrag(hit, updateReason),
               "gizmo tryPreflightUpdateDrag rejects non-finite hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite hit update reject reason is NonFiniteHit");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");
    gizmo.endDrag();
}

void testSnapDragRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                  reason),
               "tryPreflightSnapDrag accepts valid delta and snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false for valid delta and snap");

    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    snap.translateSnap = false;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");

    snap.gridSize = 0.f;
               "preflightSnapDragReady rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

    const fuse::editor::SnapDragPreflight degradedPreflight =
        fuse::editor::preflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(degradedPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "classifySnapDragReject maps invalidStep flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(fuse::editor::classifySnapDragReject(invalidStepPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::None,
               "classifySnapDragReject returns None for valid snap-drag");

    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason),
               "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");

void testNonFiniteRejectReasonClassifiers() {
}

void testNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickPreflight nanRayPick = fuse::editor::preflightPick(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyPickReject(nanRayPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "classifyPickReject maps nonFiniteRay flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    const fuse::editor::PickPreflight nanHitPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanHitPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");

    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanHit, fuse::editor::GizmoMode::Translate,
                                               pickReason),
               "tryPreflightPick rejects non-finite screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit pick reject reason is NonFiniteHit");

    const fuse::editor::BeginDragPreflight nanBegin =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyBeginDragReject(nanBegin) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "classifyBeginDragReject maps nonFiniteHit flag");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanRay, transform,
                                                    fuse::editor::GizmoMode::Translate,
                                                    fuse::editor::GizmoSpace::World,
                                                    fuse::editor::GizmoSystem::kAxisLength,
                                                    fuse::editor::GizmoSystem::kPickRadius,
                                                    beginReason),
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite hit begin reject reason is NonFiniteHit");
    expectTrue(!fuse::editor::tryPreflightBeginDrag(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, beginReason),
               "tryPreflightBeginDrag rejects non-finite ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

    fuse::editor::GizmoTransform dragTransform{};
    gizmo.beginDrag(hit, dragTransform);

               "gizmo tryPreflightUpdateDrag rejects non-finite screen hit");

    const fuse::editor::UpdateDragPreflight nanUpdate =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanUpdate) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");

void testNonFiniteRejectReasonClassification() {



               "non-finite pick reject reason is NonFiniteHit");

    const fuse::editor::BeginDragPreflight beginPreflight =
    expectTrue(fuse::editor::classifyBeginDragReject(beginPreflight) ==

    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");


    const fuse::editor::UpdateDragPreflight updatePreflight =
    expectTrue(fuse::editor::classifyUpdateDragReject(updatePreflight) ==

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
               "non-finite update reject reason is NonFiniteHit");

    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X,
               "tryPreflightUpdateDrag rejects non-finite hit");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");
}

void testSnapDragRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                  reason),
               "tryPreflightSnapDrag accepts valid delta and snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                    &reason),
               "preflightSnapDragReady accepts valid delta and snap");
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false when snap-drag can apply");

    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite snap-drag reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    snap.translateSnap = false;
               "shouldSkipSnapDrag false when snap-drag valid");

    expectTrue(!fuse::editor::preflightSnapDragReady(std::numeric_limits<fuse::f32>::quiet_NaN(),
               "preflightSnapDragReady rejects non-finite delta");
               "non-finite delta reject reason is DeltaNonFinite");

    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
               "tryPreflightSnapDrag rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

    expectTrue(fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true when snap disabled");
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");

               "preflightSnapDragReady rejects invalid step");

    const fuse::editor::SnapDragPreflight degradedPreflight = fuse::editor::preflightSnapDrag(
        0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(degradedPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "classifySnapDragReject maps invalidStep flag");
    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::infinity(), fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(nanPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "classifySnapDragReject maps deltaNonFinite flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason),
               "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason), "gizmo tryPreflightSnapDrag accepts valid");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false when valid");
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite hit update reject reason is NonFiniteHit");
    expectTrue(gizmo.shouldSkipUpdateDrag(hit),
               "gizmo shouldSkipUpdateDrag true for non-finite hit");
    gizmo.endDrag();
}

void testRejectReasonMirrorsExistingPreflights() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    hit.screenX = 20.f;
    const fuse::editor::UpdateDragInteractionPreflight withDelta =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(withDelta.canUpdate(), "update interaction accepts drag with delta");
    expectTrue(withDelta.snapDragWillApply(), "update interaction applies snap-drag for finite delta");

    const fuse::editor::UpdateDragInteractionPreflight nanDelta =
        fuse::editor::preflightUpdateDragInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                   fuse::editor::GizmoMode::Translate, snap,
                                                   std::numeric_limits<fuse::f32>::quiet_NaN());
    expectTrue(nanDelta.canUpdate(), "update interaction still allows drag with non-finite delta");
    expectTrue(!nanDelta.snapDragWillApply(),
               "update interaction rejects snap-drag for non-finite delta");
    gizmo.endDrag();
}

void testNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, pickReason),
               "tryPreflightPick rejects non-finite ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::tryPreflightPick(nanHit, fuse::editor::GizmoMode::Translate,
                                               pickReason),
               "tryPreflightPick rejects non-finite screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit reject reason is NonFiniteHit");

    const fuse::editor::PickPreflight nanPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipBeginDrag(nanHit, fuse::editor::GizmoMode::Translate),
               "shouldSkipBeginDrag true for non-finite screen hit");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, beginReason),
               "tryPreflightBeginDrag rejects non-finite ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoSystem gizmo;
    gizmo.beginDrag(hit, transform);
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");
    gizmo.endDrag();
}

void testSnapDragRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap,
                                                   reason),
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    snap.translateSnap = false;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                   reason),
               "tryPreflightSnapDrag rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

    snap.gridSize = 0.5f;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                  reason),
               "tryPreflightSnapDrag accepts valid delta and snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false when snap-drag valid");

    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::quiet_NaN(), fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(nanPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "classifySnapDragReject maps deltaNonFinite flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason),
               "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");
}

void testUpdateDragInteractionDeltaPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragInteractionPreflight zeroDelta =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(zeroDelta.snapDragWillApply(), "zero delta snap-drag preflight accepts valid snap");

    const fuse::editor::UpdateDragInteractionPreflight finiteDelta =
        gizmo.preflightUpdateDragInteraction(hit, 0.37f);
    expectTrue(finiteDelta.snapDragWillApply(),
               "finite delta snap-drag preflight accepts valid snap");

    const fuse::editor::UpdateDragInteractionPreflight nanDelta =
        gizmo.preflightUpdateDragInteraction(hit, std::numeric_limits<fuse::f32>::quiet_NaN());
    expectTrue(!nanDelta.snapDragWillApply(),
               "non-finite delta snap-drag preflight rejects snap application");
    expectTrue(nanDelta.canUpdate(), "non-finite delta still allows drag update");
    gizmo.endDrag();
}

void testPickRejectReasonNonFiniteGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
               "tryPreflightPick rejects non-finite ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");
    expectTrue(fuse::editor::shouldSkipPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                            fuse::editor::GizmoSpace::World,
                                            fuse::editor::GizmoSystem::kAxisLength,
                                            fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipPick true for non-finite ray");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::preflightPickReady(nanHit, fuse::editor::GizmoMode::Translate, &reason),
               "preflightPickReady rejects non-finite screen hit");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit reject reason is NonFiniteHit");

    const fuse::editor::PickPreflight nanPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "pick reject reason label for NonFiniteHit");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.preflightPickReady(nanHit), "gizmo preflightPickReady rejects non-finite hit");
    expectTrue(gizmo.shouldSkipPick(nanHit), "gizmo shouldSkipPick true for non-finite hit");
}

void testBeginDragRejectReasonNonFiniteGuards() {
    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::infinity();
    nanHit.screenY = 50.f;

    fuse::editor::GizmoBeginDragRejectReason reason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    reason),
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipBeginDrag(nanHit, fuse::editor::GizmoMode::Translate),
               "shouldSkipBeginDrag true for non-finite screen hit");

    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::preflightBeginDragReady(nanRay, transform,
                                                      fuse::editor::GizmoMode::Translate,
                                                      fuse::editor::GizmoSpace::World,
                                                      fuse::editor::GizmoSystem::kAxisLength,
                                                      fuse::editor::GizmoSystem::kPickRadius,
                                                      &reason),
               "preflightBeginDragReady rejects non-finite ray");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

    const fuse::editor::BeginDragPreflight rayPreflight = fuse::editor::preflightBeginDrag(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyBeginDragReject(rayPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "classifyBeginDragReject maps nonFiniteRay flag");
    expectTrue(std::strcmp(fuse::editor::gizmoBeginDragRejectReasonLabel(
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "begin reject reason label for NonFiniteRay");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.preflightBeginDragReady(nanHit), "gizmo preflightBeginDragReady rejects non-finite hit");
    expectTrue(gizmo.shouldSkipBeginDrag(nanHit), "gizmo shouldSkipBeginDrag true for non-finite hit");
}

void testUpdateDragRejectReasonNonFiniteGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoUpdateDragRejectReason reason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X, reason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "shouldSkipUpdateDrag true for non-finite screen hit");

    const fuse::editor::UpdateDragPreflight nanPreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanPreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");

    fuse::editor::GizmoSystem gizmo;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!gizmo.preflightUpdateDragReady(hit), "gizmo preflightUpdateDragReady rejects non-finite hit");
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for non-finite hit");
    gizmo.endDrag();
}

void testSnapDragRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                  reason),
               "tryPreflightSnapDrag accepts valid delta and snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false when snap-drag valid");

    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   fuse::editor::GizmoMode::Translate, snap,
                                                   reason),
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    snap.translateSnap = false;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                     &reason),
               "preflightSnapDragReady rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                     &reason),
               "preflightSnapDragReady rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

    const fuse::editor::SnapDragPreflight degradedPreflight = fuse::editor::preflightSnapDrag(
        0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(degradedPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "classifySnapDragReject maps invalidStep flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    snap.gridSize = 0.5f;
    expectTrue(fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap) ==
                   fuse::editor::canSnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "preflightSnapDragReady mirrors canSnapDragDelta on valid delta");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason), "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");
}

void testRejectReasonMirrorsExistingPreflights() {


    expectTrue(fuse::editor::preflightPickReady(hit, fuse::editor::GizmoMode::Translate) ==
                   fuse::editor::canPickAxis(hit, fuse::editor::GizmoMode::Translate),
               "preflightPickReady mirrors canPickAxis on valid hit");
    expectTrue(fuse::editor::preflightBeginDragReady(hit, fuse::editor::GizmoMode::Translate) ==
                   fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate),
               "preflightBeginDragReady mirrors canBeginDrag on valid hit");
    expectTrue(fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::X) ==
                   fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "preflightUpdateDragReady mirrors canUpdateDrag on valid hit");
    expectTrue(fuse::editor::preflightEndDragReady(true) == fuse::editor::canEndDrag(true),
               "preflightEndDragReady mirrors canEndDrag when dragging");
    expectTrue(fuse::editor::preflightSnapReady(fuse::editor::GizmoMode::Translate, snap) ==
                   fuse::editor::canApplySnap(fuse::editor::GizmoMode::Translate, snap),
               "preflightSnapReady mirrors canApplySnap on valid settings");
    expectTrue(fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                    snap) ==
                   fuse::editor::canSnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "preflightSnapDragReady mirrors canSnapDragDelta on valid delta");

void testNonFiniteRejectReasonGuards() {

    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSystem::kPickRadius, pickReason),
}





    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
               "tryPreflightPick rejects non-finite ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::preflightPickReady(nanHit, fuse::editor::GizmoMode::Translate,
                                                 &pickReason),
               "preflightPickReady rejects non-finite hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite hit reject reason is NonFiniteHit");

    const fuse::editor::PickPreflight nanHitPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanHitPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
               "tryPreflightBeginDrag rejects non-finite hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite hit begin reject reason is NonFiniteHit");

    nanRay = rayAlongX();
    nanRay.direction.z = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::preflightBeginDragReady(nanRay, transform,
                                                      fuse::editor::GizmoMode::Translate,
                                                      fuse::editor::GizmoSystem::kPickRadius,
                                                      &beginReason),
               "preflightBeginDragReady rejects non-finite ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, transform);
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite hit update reject reason is NonFiniteHit");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for non-finite hit");
    gizmo.endDrag();


void testSnapDragRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    fuse::editor::GizmoSnapDragRejectReason reason =
        fuse::editor::GizmoSnapDragRejectReason::None;
    fuse::editor::GizmoSnapDragRejectReason reason = fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                  reason),
               "tryPreflightSnapDrag accepts valid delta and snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag false when snap-drag valid");

    expectTrue(!fuse::editor::preflightSnapDragReady(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                     fuse::editor::GizmoMode::Translate, snap,
                                                     &reason),
               "preflightSnapDragReady rejects non-finite delta");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta reject reason is DeltaNonFinite");

    snap.translateSnap = false;
    expectTrue(!fuse::editor::tryPreflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                   reason),
               "tryPreflightSnapDrag rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "disabled snap-drag reject reason is SnapDisabled");

    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
               "shouldSkipSnapDrag false for valid delta");

    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                   reason),
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects disabled snap");

    snap.translateSnap = true;
    expectTrue(fuse::editor::shouldSkipSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true when snap step invalid");
               "preflightSnapDragReady rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "invalid step snap-drag reject reason is InvalidStep");

    const fuse::editor::SnapDragPreflight degradedPreflight = fuse::editor::preflightSnapDrag(
        0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(degradedPreflight) ==
    expectTrue(!fuse::editor::tryPreflightSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
               "tryPreflightSnapDrag rejects non-finite delta");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite delta");

    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate,
                                                     snap, &reason),
               "preflightSnapDragReady rejects disabled snap");

    const fuse::editor::SnapDragPreflight invalidStepPreflight =
        fuse::editor::preflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap);
    const fuse::editor::SnapDragPreflight invalidStepPreflight = fuse::editor::preflightSnapDrag(
    expectTrue(fuse::editor::classifySnapDragReject(invalidStepPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "classifySnapDragReject maps invalidStep flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapDragRejectReasonLabel(
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite),
               "DeltaNonFinite") == 0,
               "snap-drag reject reason label for DeltaNonFinite");

    fuse::editor::GizmoSystem gizmo;
    snap.gridSize = 0.5f;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason),
               "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false when valid");

void testNonFiniteRejectReasonClassify() {
    expectTrue(gizmo.preflightSnapDragReady(0.37f),
               "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");

}

void testNonFiniteRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(
                   nanRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
               "tryPreflightPick rejects non-finite ray");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "non-finite ray reject reason is NonFiniteRay");


               "non-finite hit reject reason is NonFiniteHit");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
               "non-finite ray pick reject reason is NonFiniteRay");
    expectTrue(fuse::editor::classifyPickReject(fuse::editor::preflightPick(
                   fuse::editor::GizmoSystem::kPickRadius)) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "classifyPickReject maps nonFiniteRay flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");


    nanRay.origin.x = std::numeric_limits<fuse::f32>::quiet_NaN();

    const fuse::editor::PickPreflight nanRayPick = fuse::editor::preflightPick(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyPickReject(nanRayPick) ==

    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::None;
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteRay,
    expectTrue(!fuse::editor::tryPreflightPick(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSystem::kPickRadius, pickReason),

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    expectTrue(!fuse::editor::preflightPickReady(nanHit, fuse::editor::GizmoMode::Translate,
                                                 &pickReason),
               "preflightPickReady rejects non-finite screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
    expectTrue(fuse::editor::classifyPickReject(
                   fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate)) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "classifyPickReject maps nonFiniteHit flag");


    const fuse::editor::PickPreflight nanHitPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyPickReject(nanHitPick) ==

        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
    expectTrue(!fuse::editor::tryPreflightPick(nanHit, fuse::editor::GizmoMode::Translate, reason),
               "tryPreflightPick rejects non-finite screen hit");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,

    const fuse::editor::BeginDragPreflight beginNanHit =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyBeginDragReject(beginNanHit) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "classifyBeginDragReject maps nonFiniteHit flag");
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");

    expectTrue(fuse::editor::classifyBeginDragReject(beginPreflight) ==

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
    expectTrue(fuse::editor::classifyBeginDragReject(fuse::editor::preflightBeginDrag(
                   nanHit, fuse::editor::GizmoMode::Translate)) ==

    nanRay = rayAlongX();
    nanRay.direction.z = std::numeric_limits<fuse::f32>::infinity();
               "preflightPickReady rejects non-finite hit");
               "non-finite hit pick reject reason is NonFiniteHit");

               "tryPreflightBeginDrag rejects non-finite hit");
               "non-finite hit begin reject reason is NonFiniteHit");
    expectTrue(fuse::editor::classifyBeginDragReject(
                   fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate)) ==

    expectTrue(!fuse::editor::tryPreflightBeginDrag(


                   fuse::editor::GizmoSystem::kPickRadius, beginReason),


    const fuse::editor::BeginDragPreflight beginPreflight =

    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanRay, transform,
                                                    fuse::editor::GizmoMode::Translate,
                                                    fuse::editor::GizmoSystem::kPickRadius,
               "tryPreflightBeginDrag rejects non-finite ray");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "non-finite ray begin reject reason is NonFiniteRay");

        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");

    const fuse::editor::UpdateDragPreflight updatePreflight =
        fuse::editor::preflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(updatePreflight) ==

    expectTrue(fuse::editor::classifyUpdateDragReject(fuse::editor::preflightUpdateDrag(
                   nanHit, true, fuse::editor::GizmoAxis::X)) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");
    fuse::editor::GizmoRay nanDirectionRay = rayAlongX();
    nanDirectionRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::BeginDragPreflight beginNanRay = fuse::editor::preflightBeginDrag(
        nanDirectionRay, transform, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyBeginDragReject(beginNanRay) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteRay,
               "classifyBeginDragReject maps nonFiniteRay flag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    hit.screenY = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
               "tryPreflightUpdateDrag rejects non-finite hit");
               "non-finite hit update reject reason is NonFiniteHit");
    expectTrue(fuse::editor::classifyUpdateDragReject(
                   fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X)) ==
    fuse::editor::GizmoRay nanDirRay = rayAlongX();
    nanDirRay.direction.z = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::preflightBeginDragReady(
                   nanDirRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSystem::kPickRadius, &beginReason),
               "preflightBeginDragReady rejects non-finite ray");
               "non-finite begin reject reason is NonFiniteRay");

    const fuse::editor::BeginDragPreflight nanBeginPreflight =
    expectTrue(fuse::editor::classifyBeginDragReject(nanBeginPreflight) ==


    const fuse::editor::UpdateDragPreflight nanUpdatePreflight =
    expectTrue(fuse::editor::classifyUpdateDragReject(nanUpdatePreflight) ==

    expectTrue(std::strcmp(fuse::editor::gizmoBeginDragRejectReasonLabel(
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "begin reject reason label for NonFiniteHit");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "update reject reason label for NonFiniteHit");

    fuse::editor::GizmoHitTest validHit{};
    validHit.viewportWidth = 100.f;
    validHit.viewportHeight = 100.f;
    validHit.screenX = 10.f;
    validHit.screenY = 50.f;

    expectTrue(gizmo.shouldSkipPick(nanHit), "gizmo shouldSkipPick true for non-finite hit");
    expectTrue(gizmo.shouldSkipBeginDrag(nanHit),
               "gizmo shouldSkipBeginDrag true for non-finite hit");
    gizmo.beginDrag(validHit, transform);
    expectTrue(gizmo.shouldSkipUpdateDrag(nanHit),
               "gizmo shouldSkipUpdateDrag true for non-finite hit during drag");

void testInteractionPreflightIsSnapDegraded() {


    const fuse::editor::PickInteractionPreflight pickInteraction =
    expectTrue(pickInteraction.isSnapDegraded(),
               "pick interaction isSnapDegraded when step invalid");

    const fuse::editor::BeginDragInteractionPreflight beginInteraction =
        fuse::editor::preflightBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(beginInteraction.isSnapDegraded(),
               "begin drag interaction isSnapDegraded when step invalid");
    expectTrue(beginInteraction.canBegin(), "begin drag interaction still allows begin");

    const fuse::editor::BeginInteractionPreflight beginPreflight =
    expectTrue(beginPreflight.isSnapDegraded(),
               "begin interaction isSnapDegraded when step invalid");

    const fuse::editor::UpdateDragInteractionPreflight updateInteraction =
        fuse::editor::preflightUpdateDragInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                     0.37f);
    expectTrue(updateInteraction.isSnapDegraded(),
               "update drag interaction isSnapDegraded when step invalid");
    expectTrue(updateInteraction.isSnapDragDegraded(),
               "update drag interaction isSnapDragDegraded when step invalid");
    expectTrue(!updateInteraction.snapDragWillApply(),
               "update drag interaction snap-drag blocked when step invalid");

    const fuse::editor::EndDragInteractionPreflight endInteraction =
        fuse::editor::preflightEndDragInteraction(true, fuse::editor::GizmoAxis::X,
    expectTrue(endInteraction.isSnapDegraded(),
               "end drag interaction isSnapDegraded when step invalid");
    expectTrue(endInteraction.canEnd(), "end drag interaction still allows end");

    expectTrue(idleInteraction.isSnapDegraded(),
               "idle interaction preflight isSnapDegraded when step invalid");

    expectTrue(activeInteraction.isSnapDegraded(),
               "active interaction preflight isSnapDegraded when step invalid");

    const fuse::editor::InteractionPreflight validInteraction = fuse::editor::preflightInteraction(
    expectTrue(!validInteraction.isSnapDegraded(),
               "interaction preflight isSnapDegraded false when step valid");


    expectTrue(!gizmo.preflightPickReady(nanHit), "gizmo preflightPickReady rejects non-finite hit");



    gizmo.beginDrag(hit, transform);
    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for non-finite hit");
    gizmo.endDrag();



               "tryPreflightSnapDrag accepts valid delta");
               "shouldSkipSnapDrag false for valid delta");

               "non-finite snap-drag reject reason is DeltaNonFinite");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
               "non-finite delta snap-drag reject reason is DeltaNonFinite");


               "disabled snap snap-drag reject reason is SnapDisabled");

               "tryPreflightSnapDrag rejects invalid step");


    expectTrue(fuse::editor::classifySnapDragReject(nanPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "classifySnapDragReject maps deltaNonFinite flag");

               "preflightSnapDragReady rejects invalid snap step");
    expectTrue(fuse::editor::classifySnapDragReject(fuse::editor::preflightSnapDrag(
                   0.37f, fuse::editor::GizmoMode::Translate, snap)) ==


    const fuse::editor::SnapDragPreflight degradedPreflight =
    expectTrue(fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
               "preflightSnapDragReady accepts valid delta and snap");

    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::quiet_NaN(), fuse::editor::GizmoMode::Translate, snap);

    expectTrue(gizmo.shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN()),
               "gizmo shouldSkipSnapDrag true for non-finite delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(), "gizmo shouldSkipSnapDrag false when snap valid");

void testInteractionRejectReasonHelpers() {


    fuse::editor::GizmoTransform dragTransform{};
    gizmo.beginDrag(hit, dragTransform);


        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);

void testInteractionPreflightReadyHelpers() {
    snap.gridSize = 1.f;


    const fuse::editor::GizmoRay xRay = rayAlongX();

    expectTrue(fuse::editor::preflightPickInteractionReady(hit, fuse::editor::GizmoMode::Translate,
                                                           snap, &pickReason),
               "pick interaction ready accepts valid screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::None,
               "valid pick interaction reject reason is None");
    expectTrue(!fuse::editor::shouldSkipPickInteraction(hit, fuse::editor::GizmoMode::Translate,
                                                        snap),
               "shouldSkipPickInteraction false for valid screen hit");

    fuse::editor::GizmoHitTest nanHit = hit;
    expectTrue(!fuse::editor::preflightPickInteractionReady(
                   nanHit, fuse::editor::GizmoMode::Translate, snap, &pickReason),
               "pick interaction ready rejects non-finite hit");
               "non-finite pick interaction reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipPickInteraction(xRay, transform,
                                                     fuse::editor::GizmoSystem::kPickRadius, snap) ==
                   false,
               "shouldSkipPickInteraction false for valid ray");

    expectTrue(fuse::editor::preflightBeginDragInteractionReady(hit, fuse::editor::GizmoMode::Translate,
                                                                snap, &beginReason),
               "begin interaction ready accepts valid screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::None,
               "valid begin interaction reject reason is None");
    expectTrue(!fuse::editor::shouldSkipBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate,
               "shouldSkipBeginDragInteraction false for valid screen hit");
    expectTrue(fuse::editor::shouldSkipBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate,
                                                            snap, true),
               "shouldSkipBeginDragInteraction true while already dragging");

    fuse::editor::GizmoSnapDragRejectReason snapDragReason =
    expectTrue(fuse::editor::preflightUpdateDragInteractionReady(
                   hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap,
                   0.37f, &updateReason, &snapDragReason),
               "update interaction ready accepts active drag with finite delta");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::None,
               "valid update interaction reject reason is None");
    expectTrue(snapDragReason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid update interaction snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipUpdateDragInteraction(hit, true, fuse::editor::GizmoAxis::X,
               "shouldSkipUpdateDragInteraction false for active drag");

                   std::numeric_limits<fuse::f32>::quiet_NaN(), &updateReason, &snapDragReason),
               "update interaction ready still allows drag with non-finite delta");
    expectTrue(snapDragReason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta surfaces snap-drag reject reason in interaction preflight");

    fuse::editor::GizmoEndDragRejectReason endReason = fuse::editor::GizmoEndDragRejectReason::None;
    expectTrue(!fuse::editor::preflightEndDragInteractionReady(false, fuse::editor::GizmoAxis::None,
                                                               snap, &endReason),
               "end interaction ready rejects inactive drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "inactive end interaction reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipEndDragInteraction(false, fuse::editor::GizmoAxis::None,
               "shouldSkipEndDragInteraction true when not dragging");

    expectTrue(fuse::editor::preflightEndDragInteractionReady(true, fuse::editor::GizmoAxis::X,
    expectTrue(fuse::editor::preflightPickInteractionReady(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSystem::kPickRadius, snap, &pickReason),
               "pick interaction ready accepts valid ray");

    expectTrue(fuse::editor::preflightBeginDragInteractionReady(hit,
    expectTrue(!fuse::editor::preflightBeginDragInteractionReady(hit,
                                                                 snap, &beginReason, true),
               "begin interaction ready rejects while already dragging");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "already dragging begin interaction reject reason is AlreadyDragging");

    expectTrue(!fuse::editor::preflightUpdateDragInteractionReady(
                   hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate,
                   snap, &updateReason),
               "update interaction ready rejects inactive drag");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "inactive update interaction reject reason is NotDragging");
                   &updateReason),
               "update interaction ready accepts active drag");

    expectTrue(!fuse::editor::preflightEndDragInteractionReady(
                   false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap,
                   &endReason),
    expectTrue(fuse::editor::preflightEndDragInteractionReady(
                   true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap,
               "end interaction ready accepts active drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragRejectReason::None,
               "valid end interaction reject reason is None");

    expectTrue(fuse::editor::shouldSkipSnapDrag(fuse::editor::GizmoMode::Translate, snap) ==
                   fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "mode-only shouldSkipSnapDrag mirrors shouldSkipSnap");

    expectTrue(gizmo.preflightPickInteractionReady(hit), "gizmo pick interaction ready accepts hit");
    expectTrue(gizmo.preflightBeginDragInteractionReady(hit),
               "gizmo begin interaction ready accepts hit");
    expectTrue(gizmo.preflightUpdateDragInteractionReady(hit, 0.37f),
               "gizmo update interaction ready accepts active drag");
    expectTrue(gizmo.preflightEndDragInteractionReady(),
               "gizmo end interaction ready accepts active drag");
    expectTrue(!gizmo.shouldSkipEndDragInteraction(),
               "gizmo shouldSkipEndDragInteraction false while dragging");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears gizmo update snapDegraded");

    expectTrue(gizmo.canSnapDragDelta(), "gizmo canSnapDragDelta accepts valid translate snap");
    expectNear(gizmo.trySnapDragDelta(0.37f), 0.5f, 0.001f,
               "gizmo trySnapDragDelta snaps translate delta");

    expectTrue(!gizmo.canSnapDragDelta(),
               "gizmo canSnapDragDelta rejects invalid translate step");
    expectNear(gizmo.trySnapDragDelta(0.37f), 0.37f, 0.001f,
               "gizmo trySnapDragDelta passthrough when step invalid");

                   xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
                   fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius,
               "canBeginInteraction accepts valid ray pick");

    expectTrue(!fuse::editor::canUpdateInteraction(hit, false, fuse::editor::GizmoAxis::X,


    expectTrue(gizmo.canBeginInteraction(hit), "gizmo canBeginInteraction accepts valid hit");
    expectTrue(!gizmo.canEndInteraction(), "gizmo canEndInteraction rejects inactive drag");

    expectTrue(!gizmo.canBeginInteraction(hit), "gizmo canBeginInteraction rejects while dragging");

void testNonFiniteInputGuards() {
    expectTrue(fuse::editor::isHitTestNonFinite(nanHit),
               "NaN screen X is non-finite input");
    expectTrue(!fuse::editor::isHitTestEmpty(nanHit),
               "non-finite hit is not classified as empty viewport");

    const fuse::editor::PickPreflight nanPick =
    expectTrue(nanPick.nonFiniteInput, "pick preflight marks non-finite screen hit");
    expectTrue(!nanPick.canPick(), "pick preflight rejects non-finite screen hit");
    expectTrue(fuse::editor::classifyPickReject(nanPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteInput,
               "classifyPickReject reports non-finite input");

    nanRay.direction.x = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(fuse::editor::isRayNonFinite(nanRay), "infinite ray direction is non-finite");
    expectTrue(!fuse::editor::isRayEmpty(nanRay), "non-finite ray is not classified as empty");

    expectTrue(nanRayPick.nonFiniteInput, "pick preflight marks non-finite ray");

    fuse::editor::PickPreflight pickOut{};
    expectTrue(!gizmo.tryPreflightPick(nanHit, pickOut, pickReason),
               "gizmo tryPreflightPick rejects non-finite hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteInput,
               "gizmo tryPreflightPick reports non-finite reason");

    nanHit.screenX = 10.f;
    gizmo.beginDrag(nanHit, transform);
    nanHit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    fuse::editor::UpdateDragPreflight updateOut{};
    expectTrue(!gizmo.tryPreflightUpdateDrag(nanHit, updateOut, updateReason),
               "gizmo tryPreflightUpdateDrag rejects non-finite hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteInput,
               "gizmo tryPreflightUpdateDrag reports non-finite reason");

void testRejectReasonLabels() {
                               fuse::editor::GizmoPickRejectReason::None),
                           "none") == 0,
               "pick reject label for none");
                               fuse::editor::GizmoPickRejectReason::NonFiniteInput),
                           "non_finite_input") == 0,
               "pick reject label for non-finite input");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapRejectReasonLabel(
                               fuse::editor::GizmoSnapRejectReason::SnapDisabled),
                           "snap_disabled") == 0,
               "snap reject label for disabled snap");
                               fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging),
                           "already_dragging") == 0,
               "begin-drag reject label for active drag");
                               fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis),
                           "invalid_active_axis") == 0,
               "update-drag reject label for missing axis");
    expectTrue(std::strcmp(fuse::editor::gizmoEndDragRejectReasonLabel(
                               fuse::editor::GizmoEndDragRejectReason::NotDragging),
                           "not_dragging") == 0,
               "end-drag reject label for inactive drag");

void testTryPreflightRejectReasons() {
void testRayAndScreenFiniteGuards() {
    fuse::editor::GizmoRay finiteRay = rayAlongX();
    expectTrue(fuse::editor::isRayFinite(finiteRay), "isRayFinite accepts finite ray");

    fuse::editor::GizmoRay nanRay = finiteRay;
    nanRay.direction.x = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::isRayFinite(nanRay), "isRayFinite rejects NaN direction");

    fuse::editor::GizmoRay infRay = finiteRay;
    infRay.origin.y = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(!fuse::editor::isRayFinite(infRay), "isRayFinite rejects infinite origin");

    expectTrue(fuse::editor::isHitTestScreenFinite(hit),
               "isHitTestScreenFinite accepts finite screen hit");

    hit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::isHitTestScreenFinite(hit),
               "isHitTestScreenFinite rejects NaN screen X");

    expectTrue(nanRayPick.nonFiniteRay, "pick preflight marks non-finite ray");
    expectTrue(!nanRayPick.canPick(), "pick preflight rejects non-finite ray");

    const fuse::editor::PickPreflight infScreenPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(infScreenPick.nonFiniteScreen, "pick preflight marks non-finite screen hit");
    expectTrue(!infScreenPick.canPick(), "pick preflight rejects non-finite screen hit");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(!fuse::editor::tryPickAxis(hit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects infinite screen Y");
               "tryPickAxis rejects NaN screen Y");

    const fuse::editor::BeginDragPreflight beginPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate);
    expectTrue(beginPreflight.canBegin, "begin preflight accepts finite screen hit");

    const fuse::editor::BeginDragPreflight nanBeginPreflight = fuse::editor::preflightBeginDrag(
    expectTrue(nanBeginPreflight.nonFiniteScreen, "begin preflight marks non-finite screen hit");
    expectTrue(!nanBeginPreflight.canBegin, "begin preflight rejects non-finite screen hit");

    const fuse::editor::UpdateDragPreflight updatePreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(updatePreflight.canUpdate(), "update preflight accepts finite screen hit");

    hit.screenX = std::numeric_limits<fuse::f32>::infinity();
    const fuse::editor::UpdateDragPreflight infUpdatePreflight = fuse::editor::preflightUpdateDrag(
    expectTrue(infUpdatePreflight.nonFiniteScreen,
               "update preflight marks non-finite screen hit");
    expectTrue(!infUpdatePreflight.canUpdate(), "update preflight rejects non-finite screen hit");

    fuse::editor::GizmoResult updateResult{};
    expectTrue(!gizmo.tryUpdateDrag(hit, updateResult),
               "gizmo tryUpdateDrag rejects non-finite screen hit");
    expectTrue(gizmo.isDragging(), "non-finite update reject keeps drag active");
    expectTrue(gizmo.canEndDrag(), "end drag still allowed after non-finite update reject");

void testInteractionPhaseRouting() {
    expectTrue(fuse::editor::interactionPhase(false) == fuse::editor::GizmoInteractionPhase::Idle,
               "interactionPhase reports idle when not dragging");
    expectTrue(fuse::editor::interactionPhase(true) ==
                   fuse::editor::GizmoInteractionPhase::Dragging,
               "interactionPhase reports dragging when active");



    expectTrue(fuse::editor::tryPreflightPick(hit, fuse::editor::GizmoMode::Translate, pickOut,
                                              pickReason),
               "tryPreflightPick accepts valid screen hit");
               "valid pick reports no reject reason");

    fuse::editor::SnapPreflight snapOut{};
    fuse::editor::GizmoSnapRejectReason snapReason = fuse::editor::GizmoSnapRejectReason::None;
    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, snapOut,
                                              snapReason),
               "tryPreflightSnap accepts enabled snap with valid step");
    expectTrue(snapReason == fuse::editor::GizmoSnapRejectReason::None,
               "valid snap reports no reject reason");

    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, snapOut,
               "tryPreflightSnap rejects invalid step");
    expectTrue(snapReason == fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "invalid step reports reject reason");

    fuse::editor::BeginDragPreflight beginOut{};
    expectTrue(fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap,
                                                   false, beginOut, beginReason),
               "tryPreflightBeginDrag accepts valid screen hit");
               "valid begin reports no reject reason");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap,
                                                   true, beginOut, beginReason),
               "tryPreflightBeginDrag rejects while already dragging");
               "already dragging reports reject reason");

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
                                                     updateOut, updateReason),
               "tryPreflightUpdateDrag rejects inactive drag");
               "inactive drag reports reject reason");

    expectTrue(fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X,
               "tryPreflightUpdateDrag accepts active drag");
               "active update reports no reject reason");

    fuse::editor::EndDragPreflight endOut{};
    expectTrue(!fuse::editor::tryPreflightEndDrag(false, fuse::editor::GizmoAxis::None,
                                                  fuse::editor::GizmoMode::Translate, snap, endOut,
                                                  endReason),
               "tryPreflightEndDrag rejects inactive drag");
               "inactive end reports reject reason");
    expectTrue(fuse::editor::tryPreflightEndDrag(true, fuse::editor::GizmoAxis::X,
               "tryPreflightEndDrag accepts active drag");
               "active end reports no reject reason");

    expectTrue(gizmo.tryPreflightPick(hit, pickOut, pickReason),
               "gizmo tryPreflightPick accepts valid screen hit");
    expectTrue(gizmo.tryPreflightSnap(snapOut, snapReason),
               "gizmo tryPreflightSnap accepts valid settings");
    gizmo.beginDrag(hit, fuse::editor::GizmoTransform{});
    expectTrue(gizmo.tryPreflightUpdateDrag(hit, updateOut, updateReason),
               "gizmo tryPreflightUpdateDrag accepts active drag");
    expectTrue(gizmo.tryPreflightEndDrag(endOut, endReason),
               "gizmo tryPreflightEndDrag accepts active drag");

void testClassifyRejectFromPreflights() {
    fuse::editor::PickPreflight emptyRayPick{};
    emptyRayPick.emptyRay = true;
    expectTrue(fuse::editor::classifyPickReject(emptyRayPick) ==
                   fuse::editor::GizmoPickRejectReason::EmptyRay,
               "classifyPickReject maps emptyRay");

    fuse::editor::BeginDragPreflight beginPreflight{};
    beginPreflight.screenMiss = true;
                   fuse::editor::GizmoBeginDragRejectReason::ScreenMiss,
               "classifyBeginDragReject maps screenMiss");

    fuse::editor::UpdateDragPreflight updatePreflight{};
    updatePreflight.outOfBounds = true;
                   fuse::editor::GizmoUpdateDragRejectReason::OutOfBounds,
               "classifyUpdateDragReject maps outOfBounds");

    fuse::editor::SnapPreflight snapPreflight{};
    snapPreflight.snapDisabled = true;
    expectTrue(fuse::editor::classifySnapReject(snapPreflight) ==
                   fuse::editor::GizmoSnapRejectReason::SnapDisabled,
               "classifySnapReject maps snapDisabled");
    const fuse::editor::InteractionPreflight idleInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(idleInteraction.phase() == fuse::editor::GizmoInteractionPhase::Idle,
               "interaction preflight reports idle phase");
    expectTrue(idleInteraction.canActOnPhase(), "idle phase allows begin action");
    expectTrue(!idleInteraction.canUpdate(), "idle phase rejects update action");
    expectTrue(!idleInteraction.snapDegraded(), "idle interaction snapDegraded false with valid step");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeInteraction.phase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "interaction preflight reports dragging phase");
    expectTrue(activeInteraction.canActOnPhase(), "dragging phase allows update action");
    expectTrue(!activeInteraction.canBegin(), "dragging phase rejects begin action");

    hit.screenX = -5.f;
    const fuse::editor::InteractionPreflight outOfBoundsInteraction =
        fuse::editor::preflightInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                           fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!outOfBoundsInteraction.canUpdate(),
               "out-of-bounds hit blocks update in interaction preflight");
    expectTrue(outOfBoundsInteraction.canEnd(),
               "out-of-bounds hit still allows end in interaction preflight");
    expectTrue(outOfBoundsInteraction.canActOnPhase(),
               "canActOnPhase routes to end when update blocked during drag");

    const fuse::editor::InteractionPreflight degradedBegin = fuse::editor::preflightInteraction(
    expectTrue(degradedBegin.snapDegraded(), "idle interaction marks snap degraded on begin path");

    const fuse::editor::InteractionPreflight degradedDrag = fuse::editor::preflightInteraction(
    expectTrue(degradedDrag.snapDegraded(), "dragging interaction marks snap degraded on update path");

void testPickSnapPreflightSnapDegraded() {


    const fuse::editor::PickSnapPreflight degradedPickSnap =
        fuse::editor::preflightPickSnap(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPickSnap.canPick(), "pick-snap still allows pick when snap degraded");
    expectTrue(!degradedPickSnap.canSnap(), "pick-snap rejects snap when step invalid");
    expectTrue(degradedPickSnap.snapDegraded(), "pick-snap marks snap degraded");

    const fuse::editor::BeginInteractionPreflight validBegin =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validBegin.snapReady(), "begin interaction snap ready with valid step");
    expectTrue(!validBegin.snapDegraded(), "begin interaction clears snap degraded with valid step");

void testInteractionPreflightSnapHelpers() {
void testInteractionCanInteractRouting() {
void testPickInteractionSnapDegraded() {

    const fuse::editor::PickInteractionPreflight degradedRay =
        fuse::editor::preflightPickInteraction(xRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(degradedRay.canPick(), "pick interaction still allows pick when snap degraded");
    expectTrue(degradedRay.isSnapDegraded(), "pick interaction marks snap degraded");
    expectTrue(!degradedRay.snapWillApply(), "pick interaction snap will not apply");
void testRayNormalizationPreflight() {
    fuse::editor::GizmoRay unitRay = rayAlongX();
    expectTrue(fuse::editor::isRayNormalized(unitRay), "unit ray is normalized");
    expectTrue(!fuse::editor::isRayEmpty(unitRay), "normalized ray is not empty");

    fuse::editor::GizmoRay scaledRay = rayAlongX();
    scaledRay.direction = {2.f, 0.f, 0.f};
    expectTrue(!fuse::editor::isRayNormalized(scaledRay), "scaled ray is not normalized");
    expectTrue(!fuse::editor::isRayEmpty(scaledRay), "scaled ray is not empty");

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!fuse::editor::isRayNormalized(emptyRay), "empty ray is not normalized");

    const fuse::editor::PickPreflight unitPick = fuse::editor::preflightPick(
        unitRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(unitPick.canPick(), "pick preflight accepts normalized ray");
    expectTrue(!unitPick.nonUnitRay, "pick preflight clears nonUnitRay on normalized ray");

    const fuse::editor::PickPreflight scaledPick = fuse::editor::preflightPick(
        scaledRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(scaledPick.nonUnitRay, "pick preflight marks non-unit ray");
    expectTrue(scaledPick.canPick(), "pick preflight still allows non-unit ray pick");
    expectTrue(scaledPick.axis == fuse::editor::GizmoAxis::X,
               "non-unit ray pick still resolves axis");

void testSnapDeltaPreflight() {

    const fuse::editor::SnapDeltaPreflight disabled =
        fuse::editor::preflightSnapDelta(0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabled.snapDisabled, "snap-delta preflight marks disabled snap");
    expectTrue(!disabled.canApply(), "snap-delta preflight rejects disabled snap");
    expectNear(disabled.snappedDelta, 0.37f, 0.001f,
               "snap-delta preflight returns raw delta when snap disabled");

    const fuse::editor::SnapDeltaPreflight degraded =
    expectTrue(degraded.isDegraded(), "snap-delta preflight marks degraded snap");
    expectTrue(!degraded.canApply(), "snap-delta preflight rejects degraded snap");
    expectNear(degraded.snappedDelta, 0.37f, 0.001f,
               "snap-delta preflight returns raw delta when snap degraded");

    const fuse::editor::SnapDeltaPreflight valid =
    expectTrue(valid.canApply(), "snap-delta preflight accepts valid snap");
    expectNear(valid.snappedDelta, 0.5f, 0.001f, "snap-delta preflight computes snapped delta");
    expectTrue(fuse::editor::canSnapDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "canSnapDelta accepts valid translate snap");

    const fuse::editor::SnapDeltaPreflight gizmoDelta = gizmo.preflightSnapDelta(0.37f);
    expectTrue(gizmoDelta.canApply(), "gizmo snap-delta preflight accepts valid snap");
    expectTrue(gizmo.canSnapDelta(0.37f), "gizmo canSnapDelta accepts valid snap");
    expectNear(gizmo.preflightSnapDelta(0.37f).snappedDelta, 0.5f, 0.001f,
               "gizmo snap-delta preflight computes snapped delta");

void testGizmoPhaseRoutingWrappers() {

    const fuse::editor::PickInteractionPreflight degradedHit =
        fuse::editor::preflightPickInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedHit.canPick(), "screen pick interaction still allows pick when snap degraded");
    expectTrue(degradedHit.isSnapDegraded(), "screen pick interaction marks snap degraded");

    const fuse::editor::PickInteractionPreflight validSnap =
    expectTrue(validSnap.snapWillApply(), "pick interaction reports snap will apply");
    expectTrue(!validSnap.isSnapDegraded(), "valid snap clears pick interaction snapDegraded");

void testUpdateDragInteractionSnapDegraded() {


    const fuse::editor::UpdateDragInteractionPreflight degraded =
    expectTrue(degraded.canUpdate(), "update interaction still allows drag when snap degraded");
    expectTrue(degraded.isSnapDegraded(), "update interaction marks top-level snap degraded");
    expectTrue(degraded.drag.snapDegraded, "update interaction embeds drag snap degraded");
    expectTrue(!degraded.snapWillApply(), "update interaction snap will not apply");

    const fuse::editor::UpdateDragInteractionPreflight valid =
    expectTrue(valid.snapWillApply(), "update interaction reports snap will apply");
    expectTrue(!valid.isSnapDegraded(), "valid snap clears update interaction snapDegraded");

void testPickSnapDegradedHelper() {


    const fuse::editor::PickSnapPreflight degraded =
    expectTrue(degraded.canPick(), "pick-snap still allows pick when snap degraded");
    expectTrue(degraded.snapDegraded(), "pick-snap snapDegraded helper marks invalid step");
    expectTrue(!degraded.canSnap(), "pick-snap rejects invalid snap step");

    const fuse::editor::PickSnapPreflight valid =
    expectTrue(!valid.snapDegraded(), "valid snap clears pick-snap snapDegraded helper");
    expectTrue(valid.canSnap(), "pick-snap accepts valid snap step");

void testSnapInteractionPreflight() {

    const fuse::editor::SnapInteractionPreflight idleDegraded = fuse::editor::preflightSnapInteraction(
        fuse::editor::GizmoInteractionPhase::Idle, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(idleDegraded.phase == fuse::editor::GizmoInteractionPhase::Idle,
               "snap interaction records idle phase");
    expectTrue(idleDegraded.isDegraded(), "idle snap interaction marks degraded step");
    expectTrue(!idleDegraded.canApply(), "idle snap interaction cannot apply with invalid step");
    expectTrue(fuse::editor::isSnapInteractionDegraded(fuse::editor::GizmoInteractionPhase::Idle,
               "isSnapInteractionDegraded mirrors idle preflight");
    expectTrue(!fuse::editor::canApplySnapInteraction(fuse::editor::GizmoInteractionPhase::Idle,
               "canApplySnapInteraction rejects degraded idle snap");

    const fuse::editor::SnapInteractionPreflight dragDegraded = fuse::editor::preflightSnapInteraction(
        fuse::editor::GizmoInteractionPhase::Dragging, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(dragDegraded.phase == fuse::editor::GizmoInteractionPhase::Dragging,
               "snap interaction records dragging phase");
    expectTrue(dragDegraded.isDegraded(), "dragging snap interaction marks degraded step");

    const fuse::editor::SnapInteractionPreflight validDrag = fuse::editor::preflightSnapInteraction(
    expectTrue(validDrag.canApply(), "dragging snap interaction accepts valid step");
    expectTrue(!validDrag.isDegraded(), "valid step clears snap interaction degraded");
    expectTrue(fuse::editor::canApplySnapInteraction(fuse::editor::GizmoInteractionPhase::Dragging,
               "canApplySnapInteraction accepts valid dragging snap");

    expectTrue(gizmo.preflightSnapInteraction().canApply(),
               "gizmo snap interaction preflight accepts valid settings");
    expectTrue(gizmo.canApplySnapOnPhase(), "gizmo canApplySnapOnPhase mirrors preflight");
    expectTrue(!gizmo.isSnapDegradedOnPhase(), "gizmo isSnapDegradedOnPhase false with valid step");

    expectTrue(gizmo.isSnapDegradedOnPhase(), "gizmo isSnapDegradedOnPhase true with invalid step");
    expectTrue(!gizmo.canApplySnapOnPhase(), "gizmo canApplySnapOnPhase rejects invalid step");

void testInteractionPreflightSnapOnPhase() {
    fuse::editor::GizmoRay nanRay{};
    nanRay.origin = {0.f, 0.f, 0.f};
    nanRay.direction = {NAN, 0.f, 0.f};
    expectTrue(fuse::editor::isRayNonFinite(nanRay), "isRayNonFinite rejects NaN direction");
    expectTrue(!fuse::editor::isRayEmpty(nanRay),
               "NaN direction is not classified as empty by isRayEmpty");

    fuse::editor::GizmoRay infRay{};
    infRay.origin = {INFINITY, 0.f, 0.f};
    infRay.direction = {1.f, 0.f, 0.f};
    expectTrue(fuse::editor::isRayNonFinite(infRay), "isRayNonFinite rejects Inf origin");

    expectTrue(!fuse::editor::isRayNonFinite(xRay), "valid ray clears non-finite guard");
void testFiniteInputGuards() {
    expectTrue(fuse::editor::isFiniteValue(1.f), "finite value accepts normal scalar");
    expectTrue(!fuse::editor::isFiniteValue(std::numeric_limits<fuse::f32>::quiet_NaN()),
               "finite value rejects NaN");
    expectTrue(!fuse::editor::isFiniteValue(std::numeric_limits<fuse::f32>::infinity()),
               "finite value rejects infinity");

    expectTrue(fuse::editor::isRayFinite(finiteRay), "valid ray is finite");

    expectTrue(!fuse::editor::isRayFinite(nanRay), "ray with NaN origin is non-finite");

    fuse::editor::GizmoHitTest finiteHit{};
    finiteHit.viewportWidth = 100.f;
    finiteHit.viewportHeight = 100.f;
    finiteHit.screenX = 10.f;
    finiteHit.screenY = 50.f;
    expectTrue(fuse::editor::isHitTestFinite(finiteHit), "valid hit test is finite");

    fuse::editor::GizmoHitTest nanHit = finiteHit;
    expectTrue(!fuse::editor::isHitTestFinite(nanHit), "hit with NaN screenX is non-finite");

    expectTrue(fuse::editor::isDragDeltaFinite(0.5f), "finite drag delta accepted");
    expectTrue(!fuse::editor::isDragDeltaFinite(std::numeric_limits<fuse::f32>::infinity()),
               "infinite drag delta rejected");

void testAxisModeValidationGuards() {
    expectTrue(fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::X,
                                                fuse::editor::GizmoMode::Translate),
               "X axis valid in translate mode");
    expectTrue(fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::Uniform,
                                                fuse::editor::GizmoMode::Scale),
               "uniform axis valid in scale mode");
    expectTrue(!fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::Uniform,
               "uniform axis invalid in translate mode");
    expectTrue(!fuse::editor::isAxisValidForMode(fuse::editor::GizmoAxis::None,
               "none axis invalid in any mode");

void testNonFinitePickPreflightGuards() {

    nanRay.direction.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::f32 nan = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::f32 inf = std::numeric_limits<fuse::f32>::infinity();

    expectTrue(fuse::editor::isGizmoScalarFinite(1.f), "finite scalar accepted");
    expectTrue(!fuse::editor::isGizmoScalarFinite(nan), "NaN scalar rejected");
    expectTrue(!fuse::editor::isGizmoScalarFinite(inf), "infinity scalar rejected");

    nanRay.origin.x = nan;
    expectTrue(fuse::editor::isRayNonFinite(nanRay), "ray with NaN origin is non-finite");
    expectTrue(fuse::editor::isRayValid(nanRay), "isRayValid unchanged for non-finite ray");

    const fuse::editor::GizmoRay validRay = rayAlongX();
    expectTrue(!fuse::editor::isRayNonFinite(validRay), "valid ray is finite");

    expectTrue(nanRayPick.nonFinite, "pick preflight marks non-finite ray");

    nanHit.screenX = NAN;
    expectTrue(fuse::editor::isHitTestNonFinite(nanHit), "isHitTestNonFinite rejects NaN screen X");

    nanHit.viewportWidth = INFINITY;
               "isHitTestNonFinite rejects Inf viewport width");

    const fuse::editor::PickPreflight nanPick = fuse::editor::preflightPick(
    expectTrue(nanPick.nonFiniteRay, "pick preflight marks non-finite ray");
    expectTrue(!nanPick.canPick(), "pick preflight rejects non-finite ray");

    expectTrue(nanHitPick.nonFiniteHit, "pick preflight marks non-finite screen hit");
    expectTrue(!nanHitPick.canPick(), "pick preflight rejects non-finite screen hit");

    const fuse::editor::BeginDragPreflight nanBegin =
    expectTrue(nanBegin.nonFiniteHit, "begin preflight marks non-finite screen hit");
    expectTrue(!nanBegin.canBegin, "begin preflight rejects non-finite screen hit");

    const fuse::editor::UpdateDragPreflight nanUpdate =
    expectTrue(nanUpdate.nonFiniteHit, "update preflight marks non-finite screen hit");
    expectTrue(!nanUpdate.canUpdate(), "update preflight rejects non-finite screen hit");

    nanHit.screenX = nan;
    expectTrue(fuse::editor::isHitTestNonFinite(nanHit), "hit test with NaN screen X is non-finite");
    expectTrue(fuse::editor::isHitTestValid(nanHit),
               "isHitTestValid unchanged for non-finite hit with positive viewport");

    expectTrue(nanHitPick.nonFinite, "pick preflight marks non-finite screen hit");

    expectTrue(!fuse::editor::tryPickAxis(nanHit, fuse::editor::GizmoMode::Translate, axis),
    const fuse::editor::InteractionPreflight blockedUpdateInteraction =
    expectTrue(!blockedUpdateInteraction.canUpdate(),
               "dragging phase rejects update when hit is out of bounds");
    expectTrue(blockedUpdateInteraction.canEnd(),
               "dragging phase still allows end when update is blocked");
    expectTrue(blockedUpdateInteraction.canActOnPhase(),
               "canActOnPhase routes end when update is blocked");

void testHitTestNonFiniteGuards() {

    expectTrue(fuse::editor::isHitTestNonFinite(hit),
               "NaN screen X is non-finite");
    expectTrue(fuse::editor::isHitTestValid(hit),
               "non-finite screen X still has valid viewport dimensions");

    expectTrue(nanPick.nonFiniteHit, "pick preflight marks non-finite screen hit");
    expectTrue(!nanPick.outOfBounds,
               "non-finite pick is not classified as out-of-bounds");

    hit.viewportHeight = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(beginPreflight.nonFiniteHit, "begin preflight marks non-finite viewport height");
    expectTrue(!beginPreflight.canBegin, "begin preflight rejects non-finite screen hit");

    expectTrue(updatePreflight.canUpdate(), "valid hit clears update non-finite guard");

    const fuse::editor::UpdateDragPreflight nanUpdatePreflight = fuse::editor::preflightUpdateDrag(
    expectTrue(nanUpdatePreflight.nonFiniteHit, "update preflight marks non-finite screen hit");
    expectTrue(!nanUpdatePreflight.canUpdate(), "update preflight rejects non-finite screen hit");

               "tryPickAxis rejects non-finite screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None,
               "non-finite screen hit leaves axis unset");

    expectTrue(!gizmo.canPickAxis(nanHit), "gizmo canPickAxis rejects non-finite screen hit");
    expectTrue(!gizmo.canBeginDrag(nanHit), "gizmo canBeginDrag rejects non-finite screen hit");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(nanHit, result),

void testGizmoInteractionPhaseRouting() {

    nanHit.screenX = std::numeric_limits<fuse::f32>::infinity();
    expectTrue(nanHitPick.nonFiniteHit, "pick preflight marks non-finite hit");
    expectTrue(!nanHitPick.canPick(), "pick preflight rejects non-finite hit");

    expectTrue(!fuse::editor::tryPickAxis(nanRay, transform, fuse::editor::GizmoMode::Translate,
                                          fuse::editor::GizmoSystem::kPickRadius, axis),
               "tryPickAxis rejects non-finite ray");
               "tryPickAxis rejects non-finite hit");

    expectTrue(!gizmo.canPickAxis(nanRay, transform), "gizmo canPickAxis rejects non-finite ray");
    expectTrue(!gizmo.canPickAxis(nanHit), "gizmo canPickAxis rejects non-finite hit");

void testNonFiniteBeginDragPreflightGuards() {

    nanRay.origin.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::BeginDragPreflight nanRayBegin = fuse::editor::preflightBeginDrag(
    expectTrue(nanRayBegin.nonFiniteRay, "begin preflight marks non-finite ray");
    expectTrue(!nanRayBegin.canBegin, "begin preflight rejects non-finite ray");

    const fuse::editor::BeginDragPreflight nanHitBegin =
    expectTrue(nanHitBegin.nonFiniteHit, "begin preflight marks non-finite hit");
    expectTrue(!nanHitBegin.canBegin, "begin preflight rejects non-finite hit");

    expectTrue(!gizmo.tryBeginDrag(nanHit, transform, result),
               "tryBeginDrag rejects non-finite hit");
    expectTrue(!gizmo.isDragging(), "non-finite begin does not start drag");

void testNonFiniteUpdateDragPreflightGuards() {
    expectTrue(nanBeginPreflight.nonFinite, "begin preflight marks non-finite screen hit");

    fuse::editor::GizmoSnapSettings nanSnap{};
    nanSnap.translateSnap = true;
    nanSnap.gridSize = nan;
    expectTrue(fuse::editor::isSnapSettingsNonFinite(nanSnap),
               "snap settings with NaN grid are non-finite");
    const fuse::editor::SnapPreflight nanSnapPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, nanSnap);
    expectTrue(nanSnapPreflight.nonFiniteSettings, "snap preflight marks non-finite settings");
    expectTrue(!nanSnapPreflight.canApply(), "snap preflight rejects non-finite settings");
    expectTrue(nanSnapPreflight.isDegraded(), "non-finite snap settings are degraded");

    gizmo.setSnapSettings(nanSnap);
    expectTrue(!gizmo.canApplySnapNow(), "gizmo rejects non-finite snap settings");


    const fuse::editor::InteractionPreflight idleDegraded = fuse::editor::preflightInteraction(
    expectTrue(idleDegraded.snapDegraded(), "idle interaction marks snap degraded");
    expectTrue(!idleDegraded.snapWillApply(), "idle interaction reports snap will not apply");
    expectTrue(idleDegraded.canActOnPhase(), "idle interaction primary action is begin");
    expectTrue(idleDegraded.canActOrEndOnPhase(), "idle interaction act-or-end mirrors begin");

    const fuse::editor::InteractionPreflight activeDegraded = fuse::editor::preflightInteraction(
    expectTrue(activeDegraded.snapDegraded(), "active interaction marks snap degraded");
    expectTrue(activeDegraded.canEndOnPhase(), "active interaction allows end on phase");
    expectTrue(activeDegraded.canActOrEndOnPhase(), "active interaction act-or-end allows update");

    const fuse::editor::InteractionPreflight outOfBoundsActive = fuse::editor::preflightInteraction(
    expectTrue(!outOfBoundsActive.canActOnPhase(),
               "out-of-bounds active interaction blocks primary update action");
    expectTrue(outOfBoundsActive.canEndOnPhase(),
               "out-of-bounds active interaction still allows end on phase");
    expectTrue(outOfBoundsActive.canActOrEndOnPhase(),
               "out-of-bounds active interaction act-or-end falls back to end");

    const fuse::editor::PickSnapPreflight validPickSnap =
    expectTrue(validPickSnap.snapWillApply(), "pick-snap preflight reports snap will apply");
    expectTrue(!validPickSnap.snapDegraded(), "valid pick-snap preflight clears snap degraded");

    const fuse::editor::UpdateInteractionPreflight validUpdate =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
    expectTrue(validUpdate.snapWillApply(), "update interaction reports snap will apply");
    expectTrue(!validUpdate.snapDegraded(), "valid update interaction clears snap degraded");

void testGizmoPreflightRouter() {
    expectTrue(idleInteraction.canInteract(), "idle interaction allows pick or begin");
    expectTrue(idleInteraction.canActOnPhase(), "idle interaction primary action is begin");
    expectTrue(fuse::editor::canInteract(hit, false, fuse::editor::GizmoAxis::None,
               "canInteract accepts idle pick/begin path");
    expectTrue(fuse::editor::canActOnPhase(hit, false, fuse::editor::GizmoAxis::None,
               "canActOnPhase accepts idle begin path");

    expectTrue(activeInteraction.canInteract(), "active interaction allows update or end");
    expectTrue(activeInteraction.canActOnPhase(), "active interaction primary action is update");

    expectTrue(!outOfBoundsInteraction.canActOnPhase(),
               "out-of-bounds hit blocks primary update action");
    expectTrue(outOfBoundsInteraction.canInteract(),
               "out-of-bounds hit still allows end interaction path");
    expectTrue(!fuse::editor::canActOnPhase(hit, true, fuse::editor::GizmoAxis::X,
               "canActOnPhase rejects out-of-bounds update");
    expectTrue(fuse::editor::canInteract(hit, true, fuse::editor::GizmoAxis::X,
               "canInteract accepts end path when update is blocked");

    expectTrue(fuse::editor::canInteract(xRay, transform, false, fuse::editor::GizmoAxis::None,
                                         fuse::editor::GizmoSystem::kPickRadius, snap),
               "canInteract accepts valid ray when idle");
    expectTrue(fuse::editor::canActOnPhase(xRay, transform, false, fuse::editor::GizmoAxis::None,
               "canActOnPhase accepts valid ray begin path when idle");

    expectTrue(gizmo.canInteract(hit), "gizmo canInteract accepts idle pick/begin path");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase accepts idle begin path");
    expectTrue(gizmo.canInteract(xRay, transform),
               "gizmo canInteract accepts valid ray when idle");
    expectTrue(gizmo.canActOnPhase(xRay, transform),
               "gizmo canActOnPhase accepts valid ray begin path when idle");

    expectTrue(gizmo.canInteract(hit), "gizmo canInteract accepts active update/end path");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase accepts active update path");

    expectTrue(!gizmo.canActOnPhase(hit),
               "gizmo canActOnPhase rejects out-of-bounds update");
    expectTrue(gizmo.canInteract(hit),
               "gizmo canInteract accepts end path when update is blocked");
    expectTrue(idleInteraction.snapWillApplyOnPhase(),
               "idle interaction snapWillApplyOnPhase uses pick-snap path");
    expectTrue(idleInteraction.snapInteraction.canApply(),
               "idle interaction embeds phase-routed snap preflight");

    expectTrue(!idleDegraded.snapWillApplyOnPhase(),
               "idle interaction snapWillApplyOnPhase false when snap degraded");
    expectTrue(idleDegraded.snapDegraded(), "idle interaction snapDegraded true with invalid step");

    expectTrue(activeInteraction.snapWillApplyOnPhase(),
               "dragging interaction snapWillApplyOnPhase uses update/end snap path");
    expectTrue(activeInteraction.snapInteraction.phase ==
               "active interaction embeds dragging snap phase");

    expectTrue(!activeDegraded.snapWillApplyOnPhase(),
               "dragging interaction snapWillApplyOnPhase false when snap degraded");
    expectTrue(activeDegraded.snapDegraded(),
               "dragging interaction snapDegraded true with invalid step");

    expectTrue(gizmo.preflightInteraction(hit).snapDegraded(),
               "gizmo interaction preflight reports snap degraded while dragging");
    expectTrue(!gizmo.preflightInteraction(hit).snapWillApplyOnPhase(),
               "gizmo interaction snapWillApplyOnPhase false when degraded");
    expectTrue(gizmo.interactionPhase() == fuse::editor::GizmoInteractionPhase::Idle,
               "gizmo interactionPhase reports idle before drag");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase allows begin when idle");

    expectTrue(gizmo.interactionPhase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "gizmo interactionPhase reports dragging after begin");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase allows update when dragging");

    hit.viewportWidth = 0.f;
               "gizmo canActOnPhase rejects update on empty viewport while dragging");
    expectTrue(fuse::editor::canActOnPhase(hit, true, fuse::editor::GizmoAxis::X,
                                           fuse::editor::GizmoMode::Translate, snap) ==
                   gizmo.canActOnPhase(hit),
               "free canActOnPhase mirrors gizmo canActOnPhase while dragging");

               "free canActOnPhase allows begin for valid ray when idle");
               "gizmo canActOnPhase allows begin for valid ray when idle");

    const fuse::editor::UpdateDragPreflight nanUpdate = gizmo.preflightUpdateDrag(hit);
    expectTrue(nanUpdate.nonFiniteHit, "update preflight marks non-finite hit");
    expectTrue(!nanUpdate.canUpdate(), "update preflight rejects non-finite hit");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects non-finite hit");

void testAxisModeUpdateEndPreflightGuards() {

    const fuse::editor::UpdateDragPreflight invalidAxisUpdate = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidAxisUpdate.invalidAxisForMode,
               "update preflight marks uniform axis invalid in translate mode");
    expectTrue(!invalidAxisUpdate.canUpdate(),
               "update preflight rejects uniform axis in translate mode");

    const fuse::editor::UpdateDragPreflight validAxisUpdate = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Scale, snap);
    expectTrue(validAxisUpdate.canUpdate(),
               "update preflight accepts uniform axis in scale mode");
    expectTrue(!validAxisUpdate.invalidAxisForMode,
               "valid axis-mode pairing clears invalidAxisForMode");

    const fuse::editor::EndDragPreflight invalidAxisEnd = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Rotate, snap);
    expectTrue(invalidAxisEnd.invalidAxisForMode,
               "end preflight marks uniform axis invalid in rotate mode");
    expectTrue(invalidAxisEnd.canEnd(), "end preflight still allows end with invalid axis for mode");

    const fuse::editor::EndDragPreflight validAxisEnd = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!validAxisEnd.invalidAxisForMode,
               "valid axis-mode pairing clears invalidAxisForMode on end");

    gizmo.setMode(fuse::editor::GizmoMode::Translate);
    expectTrue(!fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoAxis::Uniform,
               "canUpdateDrag rejects uniform axis in translate mode");

void testNonFiniteInteractionPreflightGuards() {


    const fuse::editor::PickInteractionPreflight nanPickInteraction =
        fuse::editor::preflightPickInteraction(nanRay, transform, fuse::editor::GizmoMode::Translate,
    expectTrue(!nanPickInteraction.canPick(), "pick interaction rejects non-finite ray");
    expectTrue(nanPickInteraction.pick.nonFiniteRay,
               "pick interaction embeds non-finite ray guard");

    const fuse::editor::BeginInteractionPreflight nanBeginInteraction =
        fuse::editor::preflightBeginInteraction(nanHit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!nanBeginInteraction.canBegin(), "begin interaction rejects non-finite hit");
    expectTrue(nanBeginInteraction.begin.nonFiniteHit,
               "begin interaction embeds non-finite hit guard");

    expectTrue(!gizmo.canPickInteraction(nanRay, transform),
               "gizmo canPickInteraction rejects non-finite ray");
    expectTrue(!gizmo.canBeginInteraction(nanHit),
               "gizmo canBeginInteraction rejects non-finite hit");

               "gizmo interactionPhase reports idle when not dragging");

               "gizmo interactionPhase reports dragging when active");

               "gizmo canActOnPhase rejects update when hit is out of bounds");

    hit.screenX = nan;
    const fuse::editor::UpdateDragPreflight nanUpdatePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(nanUpdatePreflight.nonFinite, "update preflight marks non-finite screen hit");

    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects non-finite screen hit");
    expectTrue(gizmo.preflightEndDrag().canEnd(),
               "end preflight still accepts drag after non-finite update reject");

    expectTrue(!gizmo.preflightPickInteraction(nanHit).canPick(),
               "pick interaction rejects non-finite screen hit");
    expectTrue(!gizmo.canPickInteraction(nanHit), "canPickInteraction rejects non-finite screen hit");
    expectTrue(!gizmo.canPickAxis(hit), "gizmo canPickAxis rejects non-finite screen hit");
    expectTrue(!gizmo.canBeginDrag(hit), "gizmo canBeginDrag rejects non-finite screen hit");
    expectTrue(!gizmo.canPickInteraction(hit),
               "gizmo canPickInteraction rejects non-finite screen hit");

void testRayNonFiniteGuards() {

    expectTrue(fuse::editor::isRayNonFinite(nanRay), "NaN ray direction is non-finite");


    expectTrue(beginPreflight.nonFiniteRay, "begin preflight marks non-finite ray");
    expectTrue(!beginPreflight.canBegin, "begin preflight rejects non-finite ray");

    fuse::editor::GizmoRay infRay = rayAlongX();
    expectTrue(fuse::editor::isRayNonFinite(infRay), "Inf ray origin is non-finite");
    expectTrue(!fuse::editor::canPickAxis(infRay, transform, fuse::editor::GizmoMode::Translate,
                                          fuse::editor::GizmoSystem::kPickRadius),
               "canPickAxis rejects non-finite ray");


    expectTrue(!gizmo.canBeginDrag(nanRay, transform),
               "gizmo canBeginDrag rejects non-finite ray");

void testRayPreflightGuards() {
    const fuse::editor::RayPreflight emptyPreflight = fuse::editor::preflightRay(emptyRay);
    expectTrue(emptyPreflight.emptyRay, "ray preflight marks empty ray");
    expectTrue(!emptyPreflight.canUse(), "ray preflight rejects empty ray");
    expectTrue(!fuse::editor::canUseRayForPick(emptyRay),
               "canUseRayForPick rejects empty ray");
    expectTrue(!fuse::editor::isRayUnnormalized(emptyRay),
               "empty ray is not classified as unnormalized");

    const fuse::editor::RayPreflight unitPreflight = fuse::editor::preflightRay(unitRay);
    expectTrue(unitPreflight.canUse(), "ray preflight accepts unit ray");
    expectTrue(!unitPreflight.unnormalized, "unit ray is not unnormalized");
    expectTrue(fuse::editor::canUseRayForPick(unitRay), "canUseRayForPick accepts unit ray");

    expectTrue(fuse::editor::isRayUnnormalized(scaledRay),
               "non-unit ray direction is unnormalized");
    const fuse::editor::RayPreflight scaledPreflight = fuse::editor::preflightRay(scaledRay);
    expectTrue(scaledPreflight.canUse(), "unnormalized ray remains usable for pick");
    expectTrue(scaledPreflight.unnormalized, "ray preflight marks unnormalized direction");

    expectTrue(fuse::editor::pickAxisFromRay(scaledRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSystem::kPickRadius) ==
                   fuse::editor::GizmoAxis::X,
               "unnormalized ray still picks axis on valid path");

void testInteractionPreflightPhaseRouting() {


    expectTrue(idleInteraction.canInteract(), "idle interaction can interact via begin or pick");
    expectTrue(idleInteraction.isSnapDegraded(), "idle interaction marks snap degraded");
    expectTrue(!idleInteraction.snapWillApplyOnPhase(),
               "idle interaction snapWillApplyOnPhase false when step invalid");
               "canInteract accepts idle begin or pick path");

    expectTrue(activeInteraction.canInteract(), "active interaction can interact via update or end");
    expectTrue(activeInteraction.isSnapDegraded(), "active interaction marks snap degraded");
    expectTrue(activeInteraction.canActOnPhase(), "active interaction canActOnPhase via update");
               "canInteract accepts active update or end path");

    const fuse::editor::InteractionPreflight emptyHitInteraction = fuse::editor::preflightInteraction(
    expectTrue(!emptyHitInteraction.canActOnPhase(),
               "active interaction rejects primary action on empty viewport");
    expectTrue(emptyHitInteraction.canInteract(),
               "active interaction can still end on empty viewport");
               "canInteract true when end path remains valid");

    const fuse::editor::PickSnapPreflight pickSnap =
    expectTrue(!pickSnap.snapDegraded(), "pick-snap preflight clears snapDegraded with valid step");
    expectTrue(pickSnap.canSnap(), "pick-snap preflight can snap with valid step");

    expectTrue(!pickInteraction.snapDegraded(),
               "pick interaction preflight clears snapDegraded with valid step");
    expectTrue(pickInteraction.snapWillApply(), "pick interaction reports snap will apply");

    const fuse::editor::BeginInteractionPreflight beginInteraction =
    expectTrue(!beginInteraction.snapDegraded(),
               "begin interaction preflight clears snapDegraded with valid step");
    expectTrue(beginInteraction.snapReady(), "begin interaction preflight snap ready");

    const fuse::editor::InteractionPreflight validSnapInteraction = fuse::editor::preflightInteraction(
    expectTrue(validSnapInteraction.snapWillApplyOnPhase(),
               "idle interaction snapWillApplyOnPhase true with valid step");
    expectTrue(!validSnapInteraction.isSnapDegraded(),
               "idle interaction clears snapDegraded with valid step");

    expectTrue(gizmo.canInteract(hit), "gizmo canInteract accepts idle path");
    expectTrue(gizmo.canInteract(hit), "gizmo canInteract accepts active update or end path");
    expectTrue(gizmo.preflightInteraction(hit).snapWillApplyOnPhase(),
               "gizmo interaction preflight snapWillApplyOnPhase while dragging");

void testSnapDragDeltaPreflight() {

    const fuse::editor::SnapDragDeltaPreflight disabledPreflight =
        fuse::editor::preflightSnapDragDelta(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap-drag preflight marks disabled snap");
    expectTrue(!disabledPreflight.canApply(), "snap-drag preflight rejects disabled snap");
    expectTrue(!disabledPreflight.isDegraded(), "disabled snap is not degraded");

    const fuse::editor::SnapDragDeltaPreflight degradedPreflight =
    expectTrue(degradedPreflight.isDegraded(), "snap-drag preflight marks degraded snap");
    expectTrue(!degradedPreflight.canApply(), "snap-drag preflight rejects invalid step");

    const fuse::editor::SnapDragDeltaPreflight validPreflight =
    expectTrue(validPreflight.canApply(), "snap-drag preflight accepts valid snap");
    expectTrue(!validPreflight.snapDisabled, "valid snap-drag preflight clears snapDisabled");
    expectTrue(!validPreflight.invalidStep, "valid snap-drag preflight clears invalidStep");
    expectTrue(fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta mirrors valid snap-drag preflight");

    expectTrue(gizmo.preflightSnapDragDelta().canApply(),
               "gizmo snap-drag preflight accepts valid settings");

void testDragSessionPreflight() {


    const fuse::editor::DragSessionPreflight idleSession = fuse::editor::preflightDragSession(
    expectTrue(idleSession.phase() == fuse::editor::GizmoInteractionPhase::Idle,
               "drag session reports idle phase");
    expectTrue(idleSession.canBegin(), "drag session allows begin when idle");
    expectTrue(idleSession.canActOnPhase(), "drag session canActOnPhase allows begin when idle");
    expectTrue(!idleSession.canUpdate(), "drag session rejects update when idle");
    expectTrue(!idleSession.canEnd(), "drag session rejects end when idle");
    expectTrue(idleSession.snapWillApply(), "drag session reports snap ready");
    expectTrue(idleSession.begin.axis == fuse::editor::GizmoAxis::X,
               "drag session resolves begin axis when idle");

    const fuse::editor::DragSessionPreflight activeSession = fuse::editor::preflightDragSession(
    expectTrue(activeSession.phase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "drag session reports dragging phase");
    expectTrue(activeSession.canUpdate(), "drag session allows update when dragging");
    expectTrue(activeSession.canEnd(), "drag session allows end when dragging");
    expectTrue(activeSession.canActOnPhase(), "drag session canActOnPhase allows update");
    expectTrue(!activeSession.canBegin(), "drag session rejects begin when dragging");

    const fuse::editor::DragSessionPreflight emptyHitSession = fuse::editor::preflightDragSession(
    expectTrue(!emptyHitSession.canUpdate(), "drag session update blocked on empty viewport");
    expectTrue(emptyHitSession.canEnd(), "drag session end still allowed on empty viewport");
    expectTrue(fuse::editor::canDragSession(hit, true, fuse::editor::GizmoAxis::X,
               "canDragSession true when end path remains valid");

    const fuse::editor::DragSessionPreflight raySession = fuse::editor::preflightDragSession(
        xRay, transform, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate,
    expectTrue(raySession.canBegin(), "drag session accepts valid ray begin");
    expectTrue(raySession.begin.axis == fuse::editor::GizmoAxis::X,
               "drag session resolves ray begin axis");

    expectTrue(gizmo.preflightDragSession(hit).canBegin(),
               "gizmo drag session allows begin when idle");
    expectTrue(gizmo.preflightDragSession(hit).canUpdate(),
               "gizmo drag session allows update when dragging");
    expectTrue(gizmo.canDragSession(hit), "gizmo canDragSession accepts active drag");
    expectTrue(gizmo.canActOnDragInteraction(hit),
               "gizmo canActOnDragInteraction accepts valid update hit");

void testDragInteractionActOnPhase() {


    const fuse::editor::DragInteractionPreflight inactive =
        fuse::editor::preflightDragInteraction(hit, false, fuse::editor::GizmoAxis::None,
    expectTrue(!inactive.canActOnPhase(), "drag interaction canActOnPhase rejects when idle");


    const fuse::editor::DragInteractionPreflight active = gizmo.preflightDragInteraction(hit);
    expectTrue(active.canActOnPhase(), "drag interaction canActOnPhase accepts valid update");
               "gizmo canActOnDragInteraction mirrors drag interaction phase guard");

    expectTrue(!gizmo.canActOnDragInteraction(hit),
               "gizmo canActOnDragInteraction rejects empty viewport update");
    expectTrue(gizmo.canDragSession(hit),
               "gizmo canDragSession still true when end remains valid");


    expectTrue(!fuse::editor::isScalarFinite(nan), "NaN scalar is non-finite");
    expectTrue(!fuse::editor::isScalarFinite(inf), "Inf scalar is non-finite");
    expectTrue(fuse::editor::isScalarFinite(1.f), "finite scalar passes guard");

    fuse::math::Vec3 finiteVec{1.f, 2.f, 3.f};
    expectTrue(fuse::editor::isVec3Finite(finiteVec), "finite vector passes guard");
    fuse::math::Vec3 nanVec{nan, 0.f, 0.f};
    expectTrue(!fuse::editor::isVec3Finite(nanVec), "vector with NaN component fails guard");

    expectTrue(!fuse::editor::isRayNonFinite(finiteRay), "valid ray is finite");
    nanRay.direction.x = nan;
    expectTrue(fuse::editor::isRayNonFinite(nanRay), "ray with NaN direction is non-finite");

    expectTrue(!fuse::editor::isHitTestNonFinite(finiteHit), "valid hit test is finite");

    expectTrue(fuse::editor::isHitTestNonFinite(nanHit), "hit with NaN screenX is non-finite");

        nanRay, {}, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,


    const fuse::editor::BeginDragPreflight nanBegin = fuse::editor::preflightBeginDrag(
        nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(nanBegin.nonFiniteHit, "begin preflight marks non-finite hit");
    expectTrue(!nanBegin.canBegin, "begin preflight rejects non-finite hit");

    const fuse::editor::UpdateDragPreflight nanUpdate = fuse::editor::preflightUpdateDrag(
        nanHit, true, fuse::editor::GizmoAxis::X);

    expectTrue(!gizmo.canBeginDrag(nanHit), "gizmo canBeginDrag rejects non-finite hit");
    gizmo.beginDrag(finiteHit, transform);
    expectTrue(!gizmo.canUpdateDrag(nanHit), "gizmo canUpdateDrag rejects non-finite hit");

void testInteractionActionRouting() {


    expectTrue(idleInteraction.canAct(fuse::editor::GizmoInteractionAction::Pick),
               "idle interaction allows pick action");
    expectTrue(idleInteraction.canAct(fuse::editor::GizmoInteractionAction::Begin),
               "idle interaction allows begin action");
    expectTrue(!idleInteraction.canAct(fuse::editor::GizmoInteractionAction::Update),
               "idle interaction rejects update action");
    expectTrue(!idleInteraction.canAct(fuse::editor::GizmoInteractionAction::End),
               "idle interaction rejects end action");
    expectTrue(fuse::editor::canActOnInteraction(idleInteraction,
                                                 fuse::editor::GizmoInteractionAction::Begin),
               "canActOnInteraction mirrors idle begin routing");

    expectTrue(activeInteraction.canAct(fuse::editor::GizmoInteractionAction::Update),
               "active interaction allows update action");
    expectTrue(activeInteraction.canAct(fuse::editor::GizmoInteractionAction::End),
               "active interaction allows end action");
    expectTrue(!activeInteraction.canAct(fuse::editor::GizmoInteractionAction::Begin),
               "active interaction rejects begin action");
    expectTrue(fuse::editor::canActOnInteraction(activeInteraction,
                                                 fuse::editor::GizmoInteractionAction::End),
               "canActOnInteraction mirrors active end routing");

void testDragLifecycleSnapPreflight() {

    const fuse::editor::DragLifecycleSnapPreflight validLifecycle =
        fuse::editor::preflightDragLifecycleSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validLifecycle.allCanApply(), "lifecycle snap preflight reports all phases ready");
    expectTrue(!validLifecycle.anyDegraded(), "valid snap is not degraded across lifecycle");

    const fuse::editor::DragLifecycleSnapPreflight degradedLifecycle =
    expectTrue(!degradedLifecycle.allCanApply(),
               "lifecycle snap preflight rejects invalid step across phases");
    expectTrue(degradedLifecycle.anyDegraded(),
               "lifecycle snap preflight marks degraded across phases");
    expectTrue(degradedLifecycle.begin.isDegraded(), "begin phase snap is degraded");
    expectTrue(degradedLifecycle.update.isDegraded(), "update phase snap is degraded");
    expectTrue(degradedLifecycle.end.isDegraded(), "end phase snap is degraded");

    const fuse::editor::DragLifecycleSnapPreflight gizmoLifecycle = gizmo.preflightDragLifecycleSnap();
    expectTrue(gizmoLifecycle.anyDegraded(), "gizmo lifecycle snap preflight marks degraded");

void testGizmoTargetPreflightGuards() {
    expectTrue(!fuse::editor::isGizmoTargetValid(fuse::Handle<fuse::Object>::invalid()),
               "invalid handle fails target validation");
    expectTrue(fuse::editor::isGizmoTargetValid(gizmoTestTarget()),
               "test target handle is valid");


    const fuse::editor::BeginDragPreflight beginWithoutTarget = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, {}, fuse::Handle<fuse::Object>::invalid());
    expectTrue(beginWithoutTarget.noTarget, "begin preflight marks missing target");
    expectTrue(!beginWithoutTarget.canBegin, "begin preflight rejects missing target");

    const fuse::editor::BeginDragPreflight beginWithTarget = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, {}, gizmoTestTarget());
    expectTrue(!beginWithTarget.noTarget, "begin preflight clears noTarget with valid handle");
    expectTrue(beginWithTarget.canBegin, "begin preflight accepts valid target");

    const fuse::editor::UpdateDragPreflight updateWithoutTarget = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {},
        fuse::Handle<fuse::Object>::invalid());
    expectTrue(updateWithoutTarget.noTarget, "update preflight marks missing target");
    expectTrue(!updateWithoutTarget.canUpdate(), "update preflight rejects missing target");

    const fuse::editor::EndDragPreflight endWithoutTarget = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, {},
    expectTrue(endWithoutTarget.noTarget, "end preflight marks missing target");
    expectTrue(!endWithoutTarget.canEnd(), "end preflight rejects missing target");

    fuse::editor::GizmoResult rejected{};
    expectTrue(!gizmo.tryBeginDrag(hit, transform, rejected),
               "gizmo tryBeginDrag rejects drag without target");
    expectTrue(!gizmo.isDragging(), "no-target begin does not start drag");

    bindGizmoTarget(gizmo);
    expectTrue(gizmo.tryBeginDrag(hit, transform, result), "gizmo tryBeginDrag accepts bound target");
    expectTrue(gizmo.isDragging(), "bound target begins drag");

void testNonUnitRayPickPreflight() {
    fuse::editor::GizmoRay ray = rayAlongX();
    ray.direction = {2.f, 0.f, 0.f};

    expectTrue(!fuse::editor::isRayUnitLength(ray),
               "non-unit ray direction fails unit-length check");
    expectTrue(fuse::editor::isRayUnitLength(rayAlongX()),
               "unit ray direction passes unit-length check");

    const fuse::editor::PickPreflight pick = fuse::editor::preflightPick(
        ray, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
    expectTrue(pick.nonUnitDirection, "pick preflight marks non-unit ray direction");
    expectTrue(pick.canPick(), "non-unit ray still allows pick on valid axis");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::None;
    expectTrue(fuse::editor::tryPickAxis(ray, transform, fuse::editor::GizmoMode::Translate,
               "tryPickAxis still succeeds on non-unit ray");
    expectTrue(axis == fuse::editor::GizmoAxis::X, "non-unit ray still resolves axis");

void testCanInteractionPredicates() {


    const fuse::editor::GizmoPreflightRouter idleRouter = fuse::editor::preflightGizmoRouter(
    expectTrue(idleRouter.phase() == fuse::editor::GizmoInteractionPhase::Idle,
               "router reports idle phase");
    expectTrue(idleRouter.canRoutePick(), "idle router allows pick");
    expectTrue(idleRouter.canRouteBegin(), "idle router allows begin");
    expectTrue(!idleRouter.canRouteUpdate(), "idle router rejects update");
    expectTrue(!idleRouter.canRouteEnd(), "idle router rejects end");
    expectTrue(idleRouter.canRoutePrimary(), "idle router primary action is begin");
    expectTrue(!idleRouter.canRouteEndOnPhase(), "idle router rejects end-on-phase");
    expectTrue(idleRouter.canRouteActOrEnd(), "idle router act-or-end mirrors begin");
    expectTrue(fuse::editor::canGizmoRouter(hit, false, fuse::editor::GizmoAxis::None,
               "canGizmoRouter accepts idle pick/begin paths");

    const fuse::editor::GizmoPreflightRouter activeRouter = fuse::editor::preflightGizmoRouter(
    expectTrue(activeRouter.phase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "router reports dragging phase");
    expectTrue(!activeRouter.canRouteBegin(), "active router rejects begin");
    expectTrue(activeRouter.canRouteUpdate(), "active router allows update");
    expectTrue(activeRouter.canRouteEnd(), "active router allows end");
    expectTrue(activeRouter.canRoutePrimary(), "active router primary action is update");
    expectTrue(activeRouter.canRouteEndOnPhase(), "active router allows end-on-phase");
    expectTrue(activeRouter.snapWillApply(), "active router reports snap will apply");

    const fuse::editor::GizmoPreflightRouter blockedUpdateRouter =
        fuse::editor::preflightGizmoRouter(hit, true, fuse::editor::GizmoAxis::X,
    expectTrue(!blockedUpdateRouter.canRouteUpdate(),
               "router rejects update on out-of-bounds hit");
    expectTrue(blockedUpdateRouter.canRouteEnd(), "router still allows end while dragging");
    expectTrue(!blockedUpdateRouter.canRoutePrimary(),
               "router primary action blocked on out-of-bounds hit");
    expectTrue(blockedUpdateRouter.canRouteEndOnPhase(),
               "router end-on-phase remains valid on out-of-bounds hit");
    expectTrue(blockedUpdateRouter.canRouteActOrEnd(),
               "router act-or-end falls back to end when update blocked");
    expectTrue(blockedUpdateRouter.canRouteAny(),
               "router any-path true when end remains valid");

    const fuse::editor::GizmoPreflightRouter rayRouter = fuse::editor::preflightGizmoRouter(
    expectTrue(rayRouter.canRoutePick(), "ray router allows pick");
    expectTrue(rayRouter.canRouteBegin(), "ray router allows begin");
    expectTrue(rayRouter.pickInteraction.pick.axis == fuse::editor::GizmoAxis::X,
               "ray router resolves pick axis");

void testGizmoSystemPreflightRouter() {



    const fuse::editor::GizmoPreflightRouter idleRouter = gizmo.preflightRouter(hit);
    expectTrue(idleRouter.canRouteBegin(), "gizmo router allows begin when idle");
    expectTrue(gizmo.canRouter(hit), "gizmo canRouter accepts idle paths");
    expectTrue(gizmo.canActOrEndOnPhase(hit), "gizmo canActOrEndOnPhase allows begin when idle");

    expectTrue(gizmo.preflightRouter(hit).canRouteUpdate(),
               "gizmo router allows update while dragging");
    expectTrue(gizmo.canActOrEndOnPhase(hit),
               "gizmo canActOrEndOnPhase allows update while dragging");

    expectTrue(!gizmo.preflightRouter(hit).canRoutePrimary(),
               "gizmo router blocks primary update on out-of-bounds hit");
    expectTrue(gizmo.preflightRouter(hit).canRouteEndOnPhase(),
               "gizmo router end-on-phase valid on out-of-bounds hit");
               "gizmo canActOrEndOnPhase falls back to end when update blocked");
    expectTrue(gizmo.canRouter(hit), "gizmo canRouter true when end path remains valid");

    expectTrue(gizmo.preflightRouter(xRay, transform).canRouteEnd(),
               "gizmo ray router allows end while dragging");

void testPickSnapPreflightDegraded() {

               "NaN screen X is non-finite hit test");
               "non-finite hit is not classified as empty");


    nanRay.direction = {std::numeric_limits<fuse::f32>::infinity(), 0.f, 0.f};
    expectTrue(fuse::editor::isRayDirectionNonFinite(nanRay),
               "Inf ray direction is non-finite");
    expectTrue(nanRayPick.nonFiniteInput, "pick preflight marks non-finite ray direction");
    expectTrue(!nanRayPick.canPick(), "pick preflight rejects non-finite ray direction");


        fuse::editor::preflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X,
                                          fuse::editor::GizmoMode::Translate, {});
    expectTrue(nanUpdatePreflight.nonFiniteInput, "update preflight marks non-finite hit");
    expectTrue(!nanUpdatePreflight.canUpdate(), "update preflight rejects non-finite hit");
    expectTrue(!gizmo.tryUpdateDrag(nanHit, updateResult),
               "tryUpdateDrag rejects non-finite hit");

    expectTrue(nanBeginPreflight.nonFiniteInput, "begin preflight marks non-finite screen hit");

void testRayUnnormalizedPreflight() {

    expectTrue(fuse::editor::isRayUnnormalized(scaledRay), "scaled ray direction is unnormalized");
    expectTrue(!fuse::editor::isRayEmpty(scaledRay), "unnormalized ray is not empty");

    const fuse::editor::PickPreflight unnormalizedPick = fuse::editor::preflightPick(
    expectTrue(unnormalizedPick.unnormalizedRay, "pick preflight marks unnormalized ray");
    expectTrue(unnormalizedPick.canPick(), "unnormalized ray still allows pick on valid path");
    expectTrue(unnormalizedPick.axis == fuse::editor::GizmoAxis::X,
               "unnormalized ray still resolves axis");

    const fuse::editor::GizmoRay unitRay = rayAlongX();
    expectTrue(!fuse::editor::isRayUnnormalized(unitRay), "unit ray is not unnormalized");

void testAxisModeMismatchGuards() {

void testHitTestPreflightGuards() {

    expectTrue(degradedPickSnap.canPick(), "pick-snap preflight still allows pick when snap degraded");
    expectTrue(!degradedPickSnap.canSnap(), "pick-snap preflight rejects invalid snap step");
    expectTrue(degradedPickSnap.isSnapDegraded(),
               "pick-snap preflight marks snap degraded with invalid step");

    expectTrue(validPickSnap.canSnap(), "pick-snap preflight accepts valid snap step");
    expectTrue(!validPickSnap.isSnapDegraded(), "valid snap clears pick-snap degraded flag");

void testBeginInteractionSnapDegraded() {
void testInteractionPreflightSnapRouting() {


    const fuse::editor::BeginInteractionPreflight degradedBegin =
    expectTrue(degradedBegin.canBegin(), "begin interaction still allows begin when snap degraded");
    expectTrue(!degradedBegin.snapReady(), "begin interaction marks snap not ready");
    expectTrue(degradedBegin.isSnapDegraded(), "begin interaction marks snap degraded");

    expectTrue(validBegin.snapReady(), "begin interaction marks snap ready");
    expectTrue(!validBegin.isSnapDegraded(), "valid snap clears begin interaction degraded flag");

void testEndInteractionSnapReady() {

    const fuse::editor::EndInteractionPreflight activeEnd = fuse::editor::preflightEndInteraction(
    expectTrue(activeEnd.canEnd(), "end interaction accepts active drag");
    expectTrue(activeEnd.snapReady(), "end interaction reports snap ready");
    expectTrue(!activeEnd.snapDegraded(), "valid snap clears end interaction degraded flag");

void testBeginDragInteractionSnapWillApply() {
    const fuse::editor::UpdateDragPreflight mismatchPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, {});
    expectTrue(mismatchPreflight.modeAxisMismatch, "update preflight marks mode-axis mismatch");
    expectTrue(!mismatchPreflight.canUpdate(), "update preflight rejects mode-axis mismatch");

    const fuse::editor::EndDragPreflight endMismatchPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::Uniform, fuse::editor::GizmoMode::Translate, {});
    expectTrue(endMismatchPreflight.modeAxisMismatch,
               "end preflight marks mode-axis mismatch");
    expectTrue(endMismatchPreflight.canEnd(),
               "end preflight still allows end on mode-axis mismatch");

    const fuse::editor::UpdateDragPreflight gizmoUpdatePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmoUpdatePreflight.canUpdate(),
               "gizmo update preflight accepts matching axis and mode");
    expectTrue(gizmo.canEndDrag(), "gizmo end allowed with valid active drag");

void testSnapNegativeStepPreflight() {
    snap.gridSize = -0.5f;

    expectTrue(fuse::editor::isSnapStepNegative(fuse::editor::GizmoMode::Translate, snap),
               "negative translate grid is snap step negative");
    expectTrue(!fuse::editor::isSnapStepValid(fuse::editor::GizmoMode::Translate, snap),
               "negative translate grid fails snap step valid");

    const fuse::editor::SnapPreflight negativePreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(negativePreflight.negativeStep, "snap preflight marks negative step");
    expectTrue(negativePreflight.invalidStep, "snap preflight marks invalid step");
    expectTrue(!negativePreflight.canApply(), "snap preflight rejects negative step");

    expectTrue(!fuse::editor::isSnapStepNegative(fuse::editor::GizmoMode::Translate, snap),
               "positive translate grid is not snap step negative");
    expectTrue(fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap).canApply(),
               "valid snap preflight can apply");

void testGizmoCanActOnPhase() {


    const fuse::editor::BeginDragInteractionPreflight validBegin =
    expectTrue(validBegin.snapWillApply(), "begin drag interaction reports snap will apply");
    expectTrue(!validBegin.snapDegraded, "valid snap clears begin drag interaction degraded flag");

    const fuse::editor::BeginDragInteractionPreflight degradedBegin =
    expectTrue(!degradedBegin.snapWillApply(),
               "begin drag interaction rejects snap when step invalid");
    expectTrue(degradedBegin.snapDegraded, "begin drag interaction marks snap degraded");

void testInteractionCanInteract() {


    expectTrue(idleInteraction.canInteract(), "idle interaction can interact via begin");
    expectTrue(!idleInteraction.isSnapDegradedForPhase(),
               "idle interaction snap not degraded with valid step");

    expectTrue(activeInteraction.canInteract(), "active interaction can interact via update");

               "out-of-bounds interaction rejects update");
    expectTrue(outOfBoundsInteraction.canEnd(), "out-of-bounds interaction still allows end");
               "out-of-bounds interaction can interact via end path");
               "primary phase action remains update-only when hit invalid");

               "canInteract accepts end path when update blocked");

               "canInteract accepts valid ray begin path");

    expectTrue(gizmo.canInteract(hit), "gizmo canInteract accepts idle begin path");
               "gizmo canInteract accepts end path when update blocked");

void testSnapPhasePreflight() {

    const fuse::editor::SnapPhasePreflight idleDegraded = fuse::editor::preflightSnapPhase(
        fuse::editor::GizmoMode::Translate, snap, fuse::editor::GizmoInteractionPhase::Idle);
    expectTrue(idleDegraded.isDegraded(), "idle snap phase marks degraded step");
    expectTrue(!idleDegraded.willApplyOnBegin(),
               "degraded snap does not apply on begin");
    expectTrue(!idleDegraded.willApplyWhileDragging(),
               "idle snap phase rejects dragging apply flag");

    const fuse::editor::SnapPhasePreflight draggingDegraded = fuse::editor::preflightSnapPhase(
        fuse::editor::GizmoMode::Translate, snap, fuse::editor::GizmoInteractionPhase::Dragging);
    expectTrue(draggingDegraded.isDegraded(), "dragging snap phase marks degraded step");
    expectTrue(!draggingDegraded.willApplyOnBegin(),
               "dragging snap phase rejects begin apply flag");
    expectTrue(!draggingDegraded.willApplyWhileDragging(),
               "degraded snap does not apply while dragging");

    const fuse::editor::SnapPhasePreflight gizmoIdle = gizmo.preflightSnapPhase();
    expectTrue(gizmoIdle.canApply(), "gizmo snap phase accepts valid step when idle");
    expectTrue(gizmoIdle.willApplyOnBegin(), "gizmo idle snap phase will apply on begin");

    const fuse::editor::SnapPhasePreflight gizmoDragging = gizmo.preflightSnapPhase();
    expectTrue(gizmoDragging.phase == fuse::editor::GizmoInteractionPhase::Dragging,
               "gizmo snap phase reports dragging lifecycle");
    expectTrue(gizmoDragging.willApplyWhileDragging(),
               "gizmo dragging snap phase will apply while dragging");

void testInteractionSnapDegradedForPhase() {


    expectTrue(idleDegraded.isSnapDegradedForPhase(),
               "idle interaction marks snap degraded for begin phase");

    expectTrue(activeDegraded.isSnapDegradedForPhase(),
               "active interaction marks snap degraded for update/end phases");

    const fuse::editor::InteractionPreflight validSnap = fuse::editor::preflightInteraction(
    expectTrue(!validSnap.isSnapDegradedForPhase(),
               "valid snap clears phase degraded flag while dragging");


               "gizmo canActOnPhase rejects update when hit is invalid while dragging");
    expectTrue(gizmo.canEndDrag(), "gizmo end still allowed when update hit is invalid");

               "gizmo ray canActOnPhase allows begin when idle");
    const fuse::editor::HitTestPreflight validPreflight = fuse::editor::preflightHitTest(hit);
    expectTrue(validPreflight.canUse(), "hit-test preflight accepts valid viewport hit");
    expectTrue(fuse::editor::canUseHitTest(hit), "canUseHitTest accepts valid viewport hit");
    expectTrue(!fuse::editor::isHitTestRejected(hit), "valid hit is not rejected");

    const fuse::editor::HitTestPreflight emptyPreflight = fuse::editor::preflightHitTest(hit);
    expectTrue(emptyPreflight.emptyHit, "hit-test preflight marks empty viewport");
    expectTrue(!emptyPreflight.canUse(), "hit-test preflight rejects empty viewport");
    expectTrue(fuse::editor::isHitTestRejected(hit), "empty viewport is rejected");

    hit.viewportWidth = -100.f;
    const fuse::editor::HitTestPreflight invalidPreflight = fuse::editor::preflightHitTest(hit);
    expectTrue(invalidPreflight.invalidDimensions,
               "hit-test preflight marks invalid viewport dimensions");
    expectTrue(!invalidPreflight.canUse(), "hit-test preflight rejects invalid dimensions");

    const fuse::editor::HitTestPreflight outOfBoundsPreflight = fuse::editor::preflightHitTest(hit);
    expectTrue(outOfBoundsPreflight.outOfBounds, "hit-test preflight marks out-of-bounds hit");
    expectTrue(!outOfBoundsPreflight.canUse(), "hit-test preflight rejects out-of-bounds hit");

    expectTrue(gizmo.preflightHitTest(hit).canUse(), "gizmo hit-test preflight accepts valid hit");
    expectTrue(gizmo.canUseHitTest(hit), "gizmo canUseHitTest accepts valid hit");

    const fuse::editor::RayPreflight emptyPreflight =
        fuse::editor::preflightRay(emptyRay, fuse::editor::GizmoSystem::kAxisLength,
    expectTrue(fuse::editor::isRayRejected(emptyRay, fuse::editor::GizmoSystem::kAxisLength,
               "empty ray is rejected");

    const fuse::editor::RayPreflight validPreflight =
        fuse::editor::preflightRay(xRay, fuse::editor::GizmoSystem::kAxisLength,
    expectTrue(validPreflight.canUse(), "ray preflight accepts valid ray");
    expectTrue(fuse::editor::canUseRay(xRay, fuse::editor::GizmoSystem::kAxisLength,
               "canUseRay accepts valid ray");

    const fuse::editor::RayPreflight invalidConfigPreflight =
        fuse::editor::preflightRay(xRay, 0.f, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(invalidConfigPreflight.invalidPickConfig,
               "ray preflight marks invalid pick config");
    expectTrue(!invalidConfigPreflight.canUse(), "ray preflight rejects invalid pick config");

    expectTrue(gizmo.preflightRay(xRay).canUse(), "gizmo ray preflight accepts valid ray");
    expectTrue(gizmo.canUseRay(xRay), "gizmo canUseRay accepts valid ray");

void testInteractionSnapDegradedAggregate() {


    expectTrue(idleDegraded.snapDegraded(), "idle interaction marks snap degraded on begin path");
    expectTrue(idleDegraded.begin.begin.snapDegraded,
               "idle interaction embeds begin snap degraded flag");

    expectTrue(activeDegraded.snapDegraded(), "active interaction marks snap degraded on update");
    expectTrue(activeDegraded.update.snapDegraded(), "active interaction embeds update degraded");

    expectTrue(!validSnap.snapDegraded(), "valid snap clears interaction snapDegraded aggregate");

void testDragInteractionCanActOnPhase() {


    expectTrue(!inactive.canActOnPhase(), "inactive drag rejects canActOnPhase");

    const fuse::editor::DragInteractionPreflight active =
        fuse::editor::preflightDragInteraction(hit, true, fuse::editor::GizmoAxis::X,
    expectTrue(active.canActOnPhase(), "active drag allows canActOnPhase on valid hit");

    const fuse::editor::DragInteractionPreflight emptyHit =
    expectTrue(!emptyHit.canActOnPhase(), "drag canActOnPhase blocked on empty viewport");
    expectTrue(emptyHit.canEnd(), "drag end still allowed when update blocked");

               "gizmo canActOnPhase rejects update on empty viewport");

void testGizmoCanActOnPhaseRouting() {



    const fuse::editor::InteractionPreflight idle = gizmo.preflightInteraction(hit);
    expectTrue(idle.canActOnPhase(), "gizmo interaction preflight routes begin when idle");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase mirrors idle begin routing");

    const fuse::editor::InteractionPreflight active = gizmo.preflightInteraction(hit);
    expectTrue(active.canActOnPhase(), "gizmo interaction preflight routes update when dragging");
    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase mirrors active update routing");
               "idle interaction reports snap will not apply with invalid step");

    const fuse::editor::InteractionPreflight idleValid = fuse::editor::preflightInteraction(
    expectTrue(!idleValid.snapDegraded(), "valid snap clears degraded on idle interaction");
    expectTrue(idleValid.snapWillApplyOnPhase(),
               "idle interaction reports snap will apply with valid step");

               "dragging interaction marks snap degraded on update path");
               "dragging interaction reports snap will not apply with invalid step");

    const fuse::editor::InteractionPreflight activeValid = fuse::editor::preflightInteraction(
    expectTrue(!activeValid.snapDegraded(), "valid snap clears degraded on dragging interaction");
    expectTrue(activeValid.snapWillApplyOnPhase(),
               "dragging interaction reports snap will apply with valid step");

void testModeChangePreflightGuards() {
    const fuse::editor::ModeChangePreflight unchanged = fuse::editor::preflightModeChange(
        fuse::editor::GizmoMode::Translate, fuse::editor::GizmoMode::Translate, false);
    expectTrue(unchanged.unchanged, "mode preflight marks unchanged mode");
    expectTrue(!unchanged.canChange(), "mode preflight rejects unchanged mode");
    expectTrue(!unchanged.wouldCancelDrag, "unchanged mode does not cancel drag");

    const fuse::editor::ModeChangePreflight idleChange = fuse::editor::preflightModeChange(
        fuse::editor::GizmoMode::Translate, fuse::editor::GizmoMode::Rotate, false);
    expectTrue(idleChange.canChange(), "mode preflight accepts mode change while idle");
    expectTrue(!idleChange.wouldCancelDrag, "idle mode change does not cancel drag");

    const fuse::editor::ModeChangePreflight draggingChange = fuse::editor::preflightModeChange(
        fuse::editor::GizmoMode::Translate, fuse::editor::GizmoMode::Rotate, true);
    expectTrue(draggingChange.canChange(), "mode preflight accepts mode change while dragging");
    expectTrue(draggingChange.wouldCancelDrag,
               "mode preflight marks drag cancellation while dragging");

    const fuse::editor::ModeChangePreflight cyclePreflight =
        fuse::editor::preflightCycleMode(fuse::editor::GizmoMode::Scale, true);
    expectTrue(cyclePreflight.canChange(), "cycle preflight accepts mode cycle while dragging");
    expectTrue(cyclePreflight.wouldCancelDrag, "cycle preflight marks drag cancellation");

    expectTrue(fuse::editor::canChangeMode(fuse::editor::GizmoMode::Translate,
                                           fuse::editor::GizmoMode::Rotate, false),
               "canChangeMode accepts valid mode change");
    expectTrue(!fuse::editor::canChangeMode(fuse::editor::GizmoMode::Translate,
                                            fuse::editor::GizmoMode::Translate, false),
               "canChangeMode rejects unchanged mode");
    expectTrue(fuse::editor::canCycleMode(fuse::editor::GizmoMode::Rotate, false),
               "canCycleMode accepts cycle while idle");


    const fuse::editor::ModeChangePreflight gizmoPreflight =
        gizmo.preflightModeChange(fuse::editor::GizmoMode::Rotate);
    expectTrue(gizmoPreflight.wouldCancelDrag, "gizmo mode preflight marks drag cancellation");
    expectTrue(gizmo.canChangeMode(fuse::editor::GizmoMode::Rotate),
               "gizmo canChangeMode accepts new mode");
    expectTrue(!gizmo.canChangeMode(fuse::editor::GizmoMode::Translate),
               "gizmo canChangeMode rejects unchanged mode");
    expectTrue(gizmo.canCycleMode(), "gizmo canCycleMode accepts cycle");
    expectTrue(gizmo.preflightCycleMode().wouldCancelDrag,
               "gizmo cycle preflight marks drag cancellation");

void testCanActOnPhaseGuards() {


               "canActOnPhase accepts begin while idle");
               "canActOnPhase accepts update while dragging with valid hit");

               "canActOnPhase rejects update when hit is out of bounds");

    hit.screenX = 30.f;
               "canActOnPhase accepts update while dragging");

               "canActOnPhase accepts ray begin while idle");

    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase accepts begin while idle");
               "gizmo canActOnPhase accepts ray begin while idle");

    expectTrue(gizmo.canActOnPhase(hit), "gizmo canActOnPhase accepts update while dragging");

                           "None") == 0,
               "pick reject label for None");
                               fuse::editor::GizmoPickRejectReason::EmptyRay),
                           "EmptyRay") == 0,
               "pick reject label for EmptyRay");
                           "SnapDisabled") == 0,
               "snap reject label for SnapDisabled");
                           "AlreadyDragging") == 0,
               "begin-drag reject label for AlreadyDragging");
                               fuse::editor::GizmoUpdateDragRejectReason::NotDragging),
                           "NotDragging") == 0,
               "update-drag reject label for NotDragging");
               "end-drag reject label for NotDragging");

void testClassifyPickReject() {
               "classifyPickReject returns EmptyRay");

    fuse::editor::PickPreflight screenMissPick{};
    screenMissPick.screenMiss = true;
    expectTrue(fuse::editor::classifyPickReject(screenMissPick) ==
                   fuse::editor::GizmoPickRejectReason::ScreenMiss,
               "classifyPickReject returns ScreenMiss");

    fuse::editor::PickPreflight validPick{};
    expectTrue(fuse::editor::classifyPickReject(validPick) ==
                   fuse::editor::GizmoPickRejectReason::None,
               "classifyPickReject returns None for valid pick");

void testTryPreflightPickRejectReason() {
    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::PickMiss;

    expectTrue(!fuse::editor::tryPreflightPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
               "tryPreflightPick rejects empty ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::EmptyRay,
               "tryPreflightPick reports EmptyRay reason");

    expectTrue(fuse::editor::tryPreflightPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "tryPreflightPick accepts valid ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::None,
               "tryPreflightPick reports None on valid ray");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!fuse::editor::tryPreflightPick(deadZone, fuse::editor::GizmoMode::Translate, reason),
               "tryPreflightPick rejects translate dead zone");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::ScreenMiss,
               "tryPreflightPick reports ScreenMiss for dead zone");

    expectTrue(gizmo.tryPreflightPick(xRay, transform, reason),
               "gizmo tryPreflightPick accepts valid ray");
               "gizmo tryPreflightPick reports None on valid ray");

void testTryPreflightSnapRejectReason() {
    fuse::editor::GizmoSnapRejectReason reason = fuse::editor::GizmoSnapRejectReason::InvalidStep;

    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::SnapDisabled,
               "tryPreflightSnap reports SnapDisabled");

    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "tryPreflightSnap reports InvalidStep");

    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap accepts valid snap");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::None,
               "tryPreflightSnap reports None on valid snap");

    expectTrue(gizmo.tryPreflightSnap(reason), "gizmo tryPreflightSnap accepts valid snap");
               "gizmo tryPreflightSnap reports None on valid snap");

void testTryPreflightBeginDragRejectReason() {
    fuse::editor::GizmoBeginDragRejectReason reason =
        fuse::editor::GizmoBeginDragRejectReason::PickMiss;

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate,
               "tryPreflightBeginDrag rejects empty viewport");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::EmptyHit,
               "tryPreflightBeginDrag reports EmptyHit");

    expectTrue(fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, reason),
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::None,
               "tryPreflightBeginDrag reports None on valid hit");

    expectTrue(!fuse::editor::tryPreflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, reason,
                                                    true),
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "tryPreflightBeginDrag reports AlreadyDragging");

    expectTrue(!gizmo.tryPreflightBeginDrag(hit, reason),
               "gizmo tryPreflightBeginDrag rejects while dragging");
               "gizmo tryPreflightBeginDrag reports AlreadyDragging");

void testTryPreflightUpdateDragRejectReason() {

    fuse::editor::GizmoUpdateDragRejectReason reason =
        fuse::editor::GizmoUpdateDragRejectReason::EmptyHit;
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "tryPreflightUpdateDrag reports NotDragging");

    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::None,
               "tryPreflightUpdateDrag rejects missing active axis");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis,
               "tryPreflightUpdateDrag reports InvalidActiveAxis");

    expectTrue(gizmo.tryPreflightUpdateDrag(hit, reason),
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::None,
               "gizmo tryPreflightUpdateDrag reports None on valid update");

    expectTrue(!gizmo.tryPreflightUpdateDrag(hit, reason),
               "gizmo tryPreflightUpdateDrag rejects empty viewport");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::EmptyHit,
               "gizmo tryPreflightUpdateDrag reports EmptyHit");

void testTryPreflightEndDragRejectReason() {
    fuse::editor::GizmoEndDragRejectReason reason =
        fuse::editor::GizmoEndDragRejectReason::NotDragging;

                                                  fuse::editor::GizmoMode::Translate, snap, reason),
    expectTrue(reason == fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "tryPreflightEndDrag reports NotDragging");

    expectTrue(reason == fuse::editor::GizmoEndDragRejectReason::None,
               "tryPreflightEndDrag reports None on valid end");

    expectTrue(!gizmo.tryPreflightEndDrag(reason),
               "gizmo tryPreflightEndDrag rejects inactive drag");
               "gizmo tryPreflightEndDrag reports NotDragging");

    expectTrue(gizmo.tryPreflightEndDrag(reason),
               "gizmo tryPreflightEndDrag reports None on valid end");

void testShouldSkipPreflights() {

    expectTrue(fuse::editor::shouldSkipPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
               "shouldSkipPick true for empty ray");
    expectTrue(!fuse::editor::shouldSkipPick(xRay, transform, fuse::editor::GizmoMode::Translate,
               "shouldSkipPick false for valid ray");

    expectTrue(fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap true when snap disabled");
    expectTrue(!fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap false when snap valid");

    expectTrue(!fuse::editor::shouldSkipBeginDrag(hit, fuse::editor::GizmoMode::Translate),
               "shouldSkipBeginDrag false for valid screen hit");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, false),
               "shouldSkipUpdateDrag true when not dragging");
    expectTrue(fuse::editor::shouldSkipEndDrag(false),
               "shouldSkipEndDrag true when not dragging");

    expectTrue(!gizmo.shouldSkipPick(xRay, transform), "gizmo shouldSkipPick false for valid ray");
    expectTrue(!gizmo.shouldSkipSnap(), "gizmo shouldSkipSnap false when snap valid");
    expectTrue(!gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag false for valid hit");
    expectTrue(!gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag false while dragging");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag false while dragging");
    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag true after end");
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid delta");
    expectTrue(gizmo.preflightPickInteractionReady(hit),
               "gizmo pick interaction ready accepts valid hit");
               "gizmo begin interaction ready accepts valid hit");
    expectTrue(gizmo.preflightUpdateDragInteractionReady(hit),
                   fuse::editor::GizmoPickRejectReason::NonFiniteHit),
               "pick reject reason label for NonFiniteHit");
    const fuse::editor::UpdateDragPreflight updateNanPreflight =
    expectTrue(fuse::editor::classifyUpdateDragReject(updateNanPreflight) ==





    const fuse::editor::SnapDragPreflight disabledPreflight = fuse::editor::preflightSnapDrag(
    expectTrue(fuse::editor::classifySnapDragReject(disabledPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::SnapDisabled,
               "classifySnapDragReject maps snapDisabled flag");

    snap.gridSize = 0.5f;

    fuse::editor::GizmoBeginDragRejectReason beginReason =

    nanRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::preflightBeginDragReady(nanRay, transform,
                                                      fuse::editor::GizmoSpace::World,
                                                      fuse::editor::GizmoSystem::kAxisLength,
                                                      &beginReason),

}

} // namespace

int main() {
    fuse::core::initialize();

    testModeSwitch();
    testScreenAxisPickExtremes();
    testRayAxisPickExtremes();
    testHitTestSegmentAndPlane();
    testHitTestMiss();
    testSnapGrid();
    testSnapValueModeAware();
    testDeltaApplyRoundtrip();
    testSetModeCancelsDrag();
    testTryPickAxisGuards();
    testTryBeginDragGuards();
    testUpdateDragEmptyViewportGuard();
    testCycleModeCancelsDrag();
    testSnapDragDeltaModeAware();
    testPickAxisFromRayEmptyGuard();
    testCanBeginDragGuards();
    testCanPickAxisGuards();
    testSnapStepForMode();
    testBeginDragRayUsesTryBeginDrag();
    testBeginDragPreflight();
    testSnapStepGuards();
    testPickConfigValid();
    testPickPreflightGuards();
    testSnapPreflightGuards();
    testTrySnapTransformGuards();
    testTrySnapDragDeltaGuards();
    testUpdateDragPreflightGuards();
    testPickInputValidityGuards();
    testSnapComponentHelpers();
    testCanUpdateDragGuards();
    testTryUpdateDragGuards();
    testPickPreflightAxisResolution();
    testUpdateDragSnapDegradedPreflight();
    testEndDragPreflightGuards();
    testCanEndDragGuards();
    testTryEndDragGuards();
    testIsSnapDegraded();
    testPickInteractionPreflight();
    testBeginDragInteractionPreflight();
    testUpdateDragInteractionPreflight();
    testEndDragInteractionPreflight();
    testDragInteractionPreflight();
    testSnapSettingsClampAndStep();
    testCanPickAxisPreflight();
    testCanBeginDragPreflight();
    testTryBeginDragAlreadyDraggingGuard();
    testCancelDragWithoutDirty();
    testRayNormalizeAndValidityGuards();
    testSnapVecHelpers();
    testPreflightBeginDragGuards();
    testCancelDragAndTryUpdateDrag();
    testSnapGuardValid();
    testTryBeginDragRejectsWhileDragging();
    testPickPreflight();
    testSnapPreflight();
    testUpdateDragPreflight();
    testUpdateDragSnapSkippedPreflight();
    testSnapVecHelpersAndTrySnapValue();
    testUpdateDragPreflightDeepen();
    testEndDragPreflightAndCancelDrag();
    testShouldSkipPickGuards();
    testShouldSkipSnapGuards();
    testSnapPreflightNoChange();
    testUpdateDragScreenMissDiagnostic();
    testEndDragUsesTryEndDrag();
    testHitTestOutOfBoundsGuards();
    testPickOutOfBoundsPreflight();
    testBeginDragSnapDegradedPreflight();
    testBeginDragOutOfBoundsPreflight();
    testUpdateDragOutOfBoundsPreflight();
    testGizmoTransformEquals();
    testEndDragUnchangedTransformPreflight();
    testUpdateDragScreenMissPreflight();
    testGizmoTrySnapDragDelta();
    testGizmoUpdateDragSnapDegradedPreflight();
    testSnapPreflightStepField();
    testGizmoInteractionPreflight();
    testGizmoUpdatePreflightSnapDegraded();
    testPickSnapPreflight();
    testBeginInteractionPreflight();
    testUpdateInteractionPreflight();
    testEndInteractionPreflight();
    testInteractionPreflightCombined();
    testPickRejectReasonGuards();
    testSnapRejectReasonGuards();
    testBeginDragRejectReasonAndAxis();
    testUpdateDragRejectReasonGuards();
    testGizmoPreflightUpdateDragWithSnap();
    testEndDragRejectReasonGuards();
    testBeginDragPreflightAxisResolution();
    testSnapDragPreflightGuards();
    testDragInteractionPreflightGuards();
    testGizmoSnapAwareUpdatePreflight();
    testPickRejectReasonClassification();
    testUpdateDragRejectReasonClassification();
    testEndDragRejectReasonClassification();
    testBeginDragPreflightAxisAndSnap();
    testCanBeginDragAlreadyDragging();
    testScreenHitOutOfBoundsGuards();
    testDragUpdateFramePreflight();
    testPickSnapPreflightGuards();
    testInteractionPreflightGuards();
    testDirtyFlagOnEndDrag();
    testHitTestOutOfBoundsGuards();
    testHitTestInBoundsGuards();
    testBeginDragPreflightAxisAndSnapDegraded();
    testBeginDragPreflightOutOfBounds();
    testUpdateDragOutOfBoundsPreflight();
    testUpdateDragScreenMissPreflight();
    testGizmoCanUpdateDragWithSnap();
    testGizmoCanEndDragWithSnap();
    testGizmoUpdateDragSnapDegradedPreflight();
    testPickSnapPreflight();
    testBeginInteractionPreflight();
    testUpdateInteractionPreflight();
    testEndInteractionPreflight();
    testUpdateDragScreenMissPreflight();
    testCanInteractionPredicates();
    testUpdateInteractionScreenMissPreflight();
    testInteractionPreflightCombined();
    testHitTestInvalidDimensionsGuards();
    testIsSnapDegradedHelper();
    testInteractionPhaseRouting();
    testFiniteInputGuards();
    testPickPreflightNonFiniteGuards();
    testBeginDragPreflightNonFiniteGuards();
    testUpdateDragNonFinitePreflight();
    testSnapDragPreflightGuards();
    testPickInteractionBlockingHelpers();
    testUpdateDragInteractionSnapDragPreflight();
    testHitTestNonFiniteGuards();
    testRayNonFiniteGuards();
    testAxisModeValidationGuards();
    testShouldSkipInteractionPredicates();
    testRejectReasonDiagnostics();
    testRejectReasonPreflights();
    testNonFiniteInputGuards();
    testCanActOnPhaseRouting();
    testPhaseActionPreflight();
    testInteractionPreflightPhaseSnapHelpers();
    testGizmoInteractionRejectReasons();
    testShouldSkipPreflights();
    testInteractionPrimaryRejectReason();
    testCanActOnPhaseGuards();
    testInteractionCanInteractRouting();
    testPickInteractionSnapDegraded();
    testUpdateDragInteractionSnapDegraded();
    testPickSnapDegradedHelper();
    testSnapInteractionPreflight();
    testInteractionPreflightSnapOnPhase();
    testGizmoInteractionPhaseRouting();
    testNonFinitePickPreflightGuards();
    testNonFiniteBeginDragPreflightGuards();
    testNonFiniteUpdateDragPreflightGuards();
    testAxisModeUpdateEndPreflightGuards();
    testNonFiniteInteractionPreflightGuards();
    testRayNormalizationPreflight();
    testSnapDeltaPreflight();
    testGizmoPhaseRoutingWrappers();
    testRayPreflightGuards();
    testInteractionPreflightPhaseRouting();
    testSnapDragDeltaPreflight();
    testDragSessionPreflight();
    testDragInteractionActOnPhase();
    testCanInteractionPredicates();
    testPickRejectReasonGuards();
    testSnapRejectReasonGuards();
    testBeginDragRejectReasonGuards();
    testUpdateDragRejectReasonGuards();
    testEndDragRejectReasonGuards();
    testNonFiniteRejectReasonClassify();
    testUpdateDragInteractionDeltaPreflight();
    testSnapStepFiniteGuards();
    testNonFiniteRejectReasonGuards();
    testUpdateDragInteractionUsesDragDelta();
    testPickRejectReasonNonFiniteGuards();
    testBeginDragRejectReasonNonFiniteGuards();
    testUpdateDragRejectReasonNonFiniteGuards();
    testPickNonFiniteRejectReasonGuards();
    testBeginDragNonFiniteRejectReasonGuards();
    testUpdateDragNonFiniteRejectReasonGuards();
    testSnapDragRejectReasonGuards();
    testNonFiniteRejectReasonClassifiers();
    testRejectReasonMirrorsExistingPreflights();
    testNonFiniteRejectReasonGuards();
    testInteractionPreflightIsSnapDegraded();
    testSnapDragRejectReasonGuards();
    testInteractionRejectReasonHelpers();
    testScreenOutOfBoundsGuards();
    testBeginDragOutOfBoundsAndSnapDegraded();
    testInteractionPreflightGuards();
    testScreenHitBoundsGuards();
    testPickPreflightOutOfBounds();
    testBeginDragPreflightPickedAxis();
    testGizmoSnapAwareUpdatePreflight();
    testGizmoSnapDragDeltaWrappers();
    testPickPreflightFiniteGuards();
    testBeginDragPreflightFiniteGuards();
    testUpdateDragFiniteAndScreenMissPreflight();
    testSnapDragPreflight();
    testCanInteractionGuards();
    testNonFiniteInputGuards();
    testRejectReasonLabels();
    testTryPreflightRejectReasons();
    testClassifyRejectFromPreflights();
    testRayAndScreenFiniteGuards();
    testPickSnapPreflightSnapDegraded();
    testInteractionPreflightSnapHelpers();
    testGizmoPreflightRouter();
    testGizmoSystemPreflightRouter();
    testPickSnapPreflightDegraded();
    testBeginInteractionSnapDegraded();
    testEndInteractionSnapReady();
    testBeginDragInteractionSnapWillApply();
    testInteractionCanInteract();
    testSnapPhasePreflight();
    testInteractionSnapDegradedForPhase();
    testRayUnnormalizedPreflight();
    testAxisModeMismatchGuards();
    testSnapNegativeStepPreflight();
    testGizmoCanActOnPhase();
    testHitTestPreflightGuards();
    testRayPreflightGuards();
    testInteractionSnapDegradedAggregate();
    testDragInteractionCanActOnPhase();
    testGizmoCanActOnPhaseRouting();
    testInteractionPreflightSnapRouting();
    testModeChangePreflightGuards();
    testCanActOnPhaseGuards();
    testInteractionActionRouting();
    testDragLifecycleSnapPreflight();
    testGizmoTargetPreflightGuards();
    testNonUnitRayPickPreflight();
    testClassifyPickReject();
    testTryPreflightPickRejectReason();
    testTryPreflightSnapRejectReason();
    testTryPreflightBeginDragRejectReason();
    testTryPreflightUpdateDragRejectReason();
    testTryPreflightEndDragRejectReason();
    testShouldSkipPreflights();
    testUpdateDragInteractionDeltaPreflight();
    testNonFiniteRejectReasonClassify();
    testInteractionPreflightReadyHelpers();
    testNonFiniteRejectReasonClassification();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_editor_gizmo_system: all tests passed\n");
    return EXIT_SUCCESS;
}
