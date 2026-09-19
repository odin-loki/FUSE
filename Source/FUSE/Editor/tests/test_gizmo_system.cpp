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

void testModeSwitch() {
    fuse::editor::GizmoSystem gizmo;
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
    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(update.changed, "drag update still applies when snap step invalid");

void testPickConfigValid() {
    expectTrue(!fuse::editor::isPickConfigValid(0.f, fuse::editor::GizmoSystem::kPickRadius),
               "isPickConfigValid rejects zero axis length");
    expectTrue(!fuse::editor::isPickConfigValid(fuse::editor::GizmoSystem::kAxisLength, 0.f),
               "isPickConfigValid rejects zero pick radius");
    expectTrue(fuse::editor::isPickConfigValid(fuse::editor::GizmoSystem::kAxisLength,
               "isPickConfigValid accepts default gizmo constants");

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
    expectTrue(!fuse::editor::canEndDrag(false), "canEndDrag rejects inactive drag");

    const fuse::editor::EndDragPreflight activePreflight = fuse::editor::preflightEndDrag(true);
    expectTrue(!activePreflight.notDragging, "active end preflight clears notDragging");
    expectTrue(activePreflight.canEnd, "end preflight accepts active drag");
    expectTrue(fuse::editor::canEndDrag(true), "canEndDrag accepts active drag");

    fuse::editor::GizmoSystem gizmo;
    const fuse::editor::EndDragPreflight gizmoInactivePreflight = gizmo.preflightEndDrag();
    expectTrue(gizmoInactivePreflight.notDragging, "gizmo end preflight marks inactive drag");
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
    gizmo.endDrag();
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag false after endDrag");

void testTryEndDragGuards() {

    gizmo.setSnapSettings(snap);
    const fuse::editor::EndDragPreflight draggingPreflight = gizmo.preflightEndDrag();
    expectTrue(draggingPreflight.canEnd(), "gizmo end preflight accepts active drag");


    expectTrue(!gizmo.transformDirty(), "inactive tryEndDrag does not mark dirty");

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

void testDirtyFlagOnEndDrag() {
    fuse::editor::GizmoSystem gizmo;
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
    gizmo.beginDrag(hit, transform);

    hit.screenX = 30.f;
    gizmo.updateDrag(hit);
    gizmo.endDrag();

    expectTrue(gizmo.transformDirty(), "gizmo marks transform dirty after drag");
    expectTrue(editorState.sceneModified, "gizmo marks editor scene modified");
    expectTrue(commandStack.isDirty(), "gizmo marks command stack dirty");
    expectTrue(commandStack.undoDepth() == 1u, "gizmo posts one transform command");
}

void testHitTestOutOfBoundsGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = -5.f;
    hit.screenY = 50.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit),
               "negative screen X is out of bounds");
    expectTrue(fuse::editor::isHitTestValid(hit),
               "out-of-bounds hit still has valid viewport dimensions");

    hit.screenX = 105.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit),
               "screen X beyond viewport width is out of bounds");

    hit.screenX = 10.f;
    hit.screenY = -1.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit),
               "negative screen Y is out of bounds");

    hit.screenY = 101.f;
    expectTrue(fuse::editor::isHitTestOutOfBounds(hit),
               "screen Y beyond viewport height is out of bounds");

    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(!fuse::editor::isHitTestOutOfBounds(hit),
               "in-bounds screen hit clears out-of-bounds guard");

    hit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::isHitTestOutOfBounds(hit),
               "empty viewport is not classified as out of bounds");

    const fuse::editor::PickPreflight outOfBoundsPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(outOfBoundsPick.emptyHit, "empty viewport pick marks emptyHit not outOfBounds");

    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = -5.f;
    hit.screenY = 50.f;
    const fuse::editor::PickPreflight negativePick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(negativePick.outOfBounds, "pick preflight marks out-of-bounds screen hit");
    expectTrue(!negativePick.canPick(), "pick preflight rejects out-of-bounds screen hit");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(!fuse::editor::tryPickAxis(hit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects out-of-bounds screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None,
               "out-of-bounds screen hit leaves axis unset");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.canPickAxis(hit), "gizmo canPickAxis rejects out-of-bounds screen hit");
    expectTrue(!fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate),
               "canBeginDrag rejects out-of-bounds screen hit");
    expectTrue(!gizmo.canBeginDrag(hit), "gizmo canBeginDrag rejects out-of-bounds screen hit");
}

void testBeginDragPreflightAxisAndSnapDegraded() {
    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::BeginDragPreflight degradedHitPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedHitPreflight.canBegin,
               "begin preflight still allows drag when snap step invalid");
    expectTrue(degradedHitPreflight.snapDegraded,
               "begin preflight marks snap degraded with invalid step");
    expectTrue(degradedHitPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves picked axis on valid screen hit");

    snap.gridSize = 1.f;
    const fuse::editor::BeginDragPreflight validSnapPreflight =
        fuse::editor::preflightBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validSnapPreflight.canBegin, "begin preflight accepts valid snap settings");
    expectTrue(!validSnapPreflight.snapDegraded, "valid snap clears snapDegraded on begin");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragPreflight rayPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayPreflight.canBegin, "begin preflight accepts valid ray pick");
    expectTrue(rayPreflight.axis == fuse::editor::GizmoAxis::X,
               "begin preflight resolves ray pick axis");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    const fuse::editor::BeginDragPreflight gizmoPreflight = gizmo.preflightBeginDrag(hit);
    expectTrue(gizmoPreflight.canBegin, "gizmo begin preflight accepts valid screen hit");
    expectTrue(gizmoPreflight.axis == fuse::editor::GizmoAxis::X,
               "gizmo begin preflight resolves picked axis");
}

void testUpdateDragOutOfBoundsPreflight() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    hit.screenX = -5.f;
    const fuse::editor::UpdateDragPreflight outOfBoundsPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(outOfBoundsPreflight.outOfBounds, "update preflight marks out-of-bounds hit");
    expectTrue(!outOfBoundsPreflight.canUpdate(), "update preflight rejects out-of-bounds hit");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects out-of-bounds hit");
    expectTrue(gizmo.isDragging(), "out-of-bounds update reject keeps drag active");
    gizmo.endDrag();
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
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragPreflight degradedPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(degradedPreflight.canUpdate(),
               "gizmo update preflight still allows drag when snap step invalid");
    expectTrue(degradedPreflight.snapDegraded,
               "gizmo update preflight marks snap degraded with invalid step");

    snap.gridSize = 1.f;
    gizmo.setSnapSettings(snap);
    const fuse::editor::UpdateDragPreflight validPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(validPreflight.canUpdate(), "gizmo update preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded on gizmo update");
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
    expectTrue(validBegin.begin.axis == fuse::editor::GizmoAxis::X,
               "begin interaction resolves picked axis");

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
    expectTrue(degradedSnap.begin.snapDegraded,
               "begin interaction embeds snap degraded on begin-drag");

    fuse::editor::GizmoSystem gizmo;
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

    hit.viewportWidth = 100.f;
    hit.screenX = -5.f;
    const fuse::editor::UpdateInteractionPreflight outOfBounds =
        fuse::editor::preflightUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!outOfBounds.canUpdate(), "update interaction rejects out-of-bounds hit");
    expectTrue(outOfBounds.update.outOfBounds,
               "update interaction embeds out-of-bounds guard");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    hit.screenX = 10.f;
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

    hit.screenX = -5.f;
    const fuse::editor::InteractionPreflight outOfBoundsInteraction =
        fuse::editor::preflightInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                           fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!outOfBoundsInteraction.canUpdate(),
               "active interaction rejects update when hit is out of bounds");
    expectTrue(outOfBoundsInteraction.update.update.outOfBounds,
               "combined interaction embeds out-of-bounds on update");

    hit.screenX = 10.f;
    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::InteractionPreflight rayInteraction = fuse::editor::preflightInteraction(
        xRay, transform, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate,
        fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
        fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(rayInteraction.canPick(), "ray interaction preflight allows pick");
    expectTrue(rayInteraction.canBegin(), "ray interaction preflight allows begin");

    fuse::editor::GizmoSystem gizmo;
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

void testHitTestInvalidDimensionsGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = -100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(fuse::editor::isHitTestDimensionsInvalid(hit),
               "negative viewport width is invalid dimensions");
    expectTrue(!fuse::editor::isHitTestEmpty(hit),
               "negative viewport width is not classified as empty");
    expectTrue(fuse::editor::isHitTestValid(hit),
               "invalid-dimension hit still has non-zero magnitude width");

    hit.viewportWidth = 100.f;
    hit.viewportHeight = -50.f;
    expectTrue(fuse::editor::isHitTestDimensionsInvalid(hit),
               "negative viewport height is invalid dimensions");

    hit.viewportHeight = 100.f;
    expectTrue(!fuse::editor::isHitTestDimensionsInvalid(hit),
               "positive viewport dimensions clear invalid-dimension guard");

    const fuse::editor::PickPreflight invalidPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(invalidPick.canPick(), "valid-dimension pick preflight accepts in-bounds hit");
    hit.viewportWidth = -100.f;
    const fuse::editor::PickPreflight negativeWidthPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(negativeWidthPick.invalidDimensions,
               "pick preflight marks invalid viewport dimensions");
    expectTrue(!negativeWidthPick.canPick(), "pick preflight rejects invalid viewport dimensions");

    hit.viewportWidth = 100.f;
    hit.viewportHeight = -50.f;
    const fuse::editor::BeginDragPreflight beginPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate);
    expectTrue(beginPreflight.invalidDimensions,
               "begin preflight marks invalid viewport dimensions");
    expectTrue(!beginPreflight.canBegin, "begin preflight rejects invalid viewport dimensions");

    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    const fuse::editor::UpdateDragPreflight updatePreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(updatePreflight.canUpdate(), "valid hit clears update invalid-dimension guard");

    hit.viewportWidth = -100.f;
    const fuse::editor::UpdateDragPreflight invalidUpdatePreflight =
        fuse::editor::preflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X);
    expectTrue(invalidUpdatePreflight.invalidDimensions,
               "update preflight marks invalid viewport dimensions");
    expectTrue(!invalidUpdatePreflight.canUpdate(),
               "update preflight rejects invalid viewport dimensions");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(!fuse::editor::tryPickAxis(hit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects invalid viewport dimensions");
    expectTrue(axis == fuse::editor::GizmoAxis::None,
               "invalid-dimension pick leaves axis unset");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.canPickAxis(hit), "gizmo canPickAxis rejects invalid viewport dimensions");
    expectTrue(!gizmo.canBeginDrag(hit), "gizmo canBeginDrag rejects invalid viewport dimensions");

    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    expectTrue(fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate).canPick() ==
                   fuse::editor::canPickSnap(hit, fuse::editor::GizmoMode::Translate, {}),
               "canPickSnap mirrors pick preflight on valid hit");
}

void testIsSnapDegradedHelper() {
    fuse::editor::GizmoSnapSettings snap{};
    expectTrue(!fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded false when snap disabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded true when translate snap enabled with zero step");

    const fuse::editor::SnapPreflight degradedPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.isDegraded(), "snap preflight isDegraded marks invalid step");
    expectTrue(!degradedPreflight.canApply(), "degraded snap cannot apply");

    snap.gridSize = 1.f;
    expectTrue(!fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded false when translate snap step is valid");
    expectTrue(fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap).canApply(),
               "valid snap preflight can apply");
}

void testInteractionPhaseRouting() {
    expectTrue(fuse::editor::interactionPhase(false) == fuse::editor::GizmoInteractionPhase::Idle,
               "interactionPhase reports idle when not dragging");
    expectTrue(fuse::editor::interactionPhase(true) ==
                   fuse::editor::GizmoInteractionPhase::Dragging,
               "interactionPhase reports dragging when active");

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
    expectTrue(idleInteraction.phase() == fuse::editor::GizmoInteractionPhase::Idle,
               "interaction preflight reports idle phase");
    expectTrue(idleInteraction.canActOnPhase(), "idle phase allows begin action");
    expectTrue(!idleInteraction.canUpdate(), "idle phase rejects update action");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeInteraction.phase() == fuse::editor::GizmoInteractionPhase::Dragging,
               "interaction preflight reports dragging phase");
    expectTrue(activeInteraction.canActOnPhase(), "dragging phase allows update action");
    expectTrue(!activeInteraction.canBegin(), "dragging phase rejects begin action");
}

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
}

void testPickPreflightNonFiniteGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.y = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickPreflight nanRayPick = fuse::editor::preflightPick(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(nanRayPick.nonFiniteRay, "pick preflight marks non-finite ray");
    expectTrue(!nanRayPick.canPick(), "pick preflight rejects non-finite ray");

    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    nanHit.screenY = 50.f;
    const fuse::editor::PickPreflight nanHitPick =
        fuse::editor::preflightPick(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(nanHitPick.nonFiniteHit, "pick preflight marks non-finite screen hit");
    expectTrue(!nanHitPick.canPick(), "pick preflight rejects non-finite screen hit");

    fuse::editor::GizmoAxis axis = fuse::editor::GizmoAxis::X;
    expectTrue(!fuse::editor::tryPickAxis(nanHit, fuse::editor::GizmoMode::Translate, axis),
               "tryPickAxis rejects non-finite screen hit");
    expectTrue(axis == fuse::editor::GizmoAxis::None,
               "non-finite screen hit leaves axis unset");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.canPickAxis(nanHit), "gizmo canPickAxis rejects non-finite screen hit");
    expectTrue(!gizmo.canBeginDrag(nanHit), "gizmo canBeginDrag rejects non-finite screen hit");
}

void testBeginDragPreflightNonFiniteGuards() {
    fuse::editor::GizmoHitTest nanHit{};
    nanHit.viewportWidth = 100.f;
    nanHit.viewportHeight = 100.f;
    nanHit.screenX = std::numeric_limits<fuse::f32>::infinity();
    nanHit.screenY = 50.f;

    const fuse::editor::BeginDragPreflight beginPreflight =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(beginPreflight.nonFiniteHit, "begin preflight marks non-finite screen hit");
    expectTrue(!beginPreflight.canBegin, "begin preflight rejects non-finite screen hit");

    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.direction.z = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::BeginDragPreflight rayPreflight = fuse::editor::preflightBeginDrag(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(rayPreflight.nonFiniteRay, "begin preflight marks non-finite ray");
    expectTrue(!rayPreflight.canBegin, "begin preflight rejects non-finite ray");
}

void testUpdateDragNonFinitePreflight() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    hit.screenY = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::UpdateDragPreflight nanPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(nanPreflight.nonFiniteHit, "update preflight marks non-finite screen hit");
    expectTrue(!nanPreflight.canUpdate(), "update preflight rejects non-finite screen hit");

    fuse::editor::GizmoResult result{};
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects non-finite screen hit");
    expectTrue(gizmo.isDragging(), "non-finite update reject keeps drag active");
    gizmo.endDrag();
}

void testSnapDragPreflightGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    const fuse::editor::SnapDragPreflight validPreflight =
        fuse::editor::preflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canApply(), "snap-drag preflight accepts valid delta and snap");
    expectTrue(!validPreflight.deltaNonFinite, "valid snap-drag clears deltaNonFinite");

    const fuse::editor::SnapDragPreflight nanPreflight = fuse::editor::preflightSnapDrag(
        std::numeric_limits<fuse::f32>::quiet_NaN(), fuse::editor::GizmoMode::Translate, snap);
    expectTrue(nanPreflight.deltaNonFinite, "snap-drag preflight marks non-finite delta");
    expectTrue(!nanPreflight.canApply(), "snap-drag preflight rejects non-finite delta");

    snap.gridSize = 0.f;
    const fuse::editor::SnapDragPreflight degradedPreflight =
        fuse::editor::preflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.isDegraded(), "snap-drag preflight marks degraded snap step");
    expectTrue(!degradedPreflight.canApply(), "snap-drag preflight rejects invalid snap step");

    expectTrue(
        std::isnan(fuse::editor::trySnapDragDelta(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                fuse::editor::GizmoMode::Translate, snap)),
        "trySnapDragDelta passthrough on non-finite delta");

    snap.gridSize = 0.5f;
    expectTrue(fuse::editor::canSnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta accepts valid delta and snap");
    expectTrue(!fuse::editor::canSnapDragDelta(std::numeric_limits<fuse::f32>::infinity(),
                                               fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta rejects non-finite delta");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapDrag(0.37f).canApply(),
               "gizmo snap-drag preflight accepts valid delta");
}

void testPickInteractionBlockingHelpers() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::PickInteractionPreflight validPick =
        fuse::editor::preflightPickInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!validPick.pickBlocked(), "pick interaction clears pickBlocked on valid hit");
    expectTrue(!validPick.snapBlocked(), "pick interaction clears snapBlocked on valid snap");

    hit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickInteractionPreflight blockedPick =
        fuse::editor::preflightPickInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(blockedPick.pickBlocked(), "pick interaction marks pickBlocked on non-finite hit");
    expectTrue(!blockedPick.snapBlocked(),
               "pick interaction snap state remains independent of pick guards");

    snap.gridSize = 0.f;
    hit.screenX = 10.f;
    const fuse::editor::InteractionPreflight degradedInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!degradedInteraction.pickBlocked(), "interaction preflight allows pick on valid hit");
    expectTrue(degradedInteraction.snapBlocked(),
               "interaction preflight marks snapBlocked when snap step invalid");
}

void testUpdateDragInteractionSnapDragPreflight() {
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

    const fuse::editor::UpdateDragInteractionPreflight active =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(active.canUpdate(), "update interaction accepts active drag");
    expectTrue(active.snapDragWillApply(), "update interaction reports snap-drag will apply");

    snap.gridSize = 0.f;
    gizmo.setSnapSettings(snap);
    const fuse::editor::UpdateDragInteractionPreflight degraded =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(degraded.canUpdate(), "update interaction still allows drag when snap degraded");
    expectTrue(!degraded.snapDragWillApply(),
               "update interaction rejects snap-drag when step invalid");
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
               "canPickSnap accepts valid screen hit");
    expectTrue(fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginInteraction accepts valid screen hit");
    expectTrue(!fuse::editor::canBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap,
                                                  true),
               "canBeginInteraction rejects while dragging");
    expectTrue(fuse::editor::canUpdateInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                  fuse::editor::GizmoMode::Translate, snap),
               "canUpdateInteraction accepts active drag");
    expectTrue(!fuse::editor::canUpdateInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                                 fuse::editor::GizmoMode::Translate, snap),
               "canUpdateInteraction rejects inactive drag");
    expectTrue(fuse::editor::canEndInteraction(true, fuse::editor::GizmoAxis::X,
                                               fuse::editor::GizmoMode::Translate, snap),
               "canEndInteraction accepts active drag");
    expectTrue(!fuse::editor::canEndInteraction(false, fuse::editor::GizmoAxis::None,
                                                fuse::editor::GizmoMode::Translate, snap),
               "canEndInteraction rejects inactive drag");

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(fuse::editor::canPickSnap(xRay, transform, fuse::editor::GizmoMode::Translate,
                                         fuse::editor::GizmoSpace::World,
                                         fuse::editor::GizmoSystem::kAxisLength,
                                         fuse::editor::GizmoSystem::kPickRadius, snap),
               "canPickSnap accepts valid ray");
    expectTrue(fuse::editor::canBeginInteraction(
                   xRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, snap),
               "canBeginInteraction accepts valid ray");

    expectTrue(fuse::editor::canBeginDrag(hit, fuse::editor::GizmoMode::Translate, snap),
               "canBeginDrag with settings accepts valid screen hit");
    expectTrue(fuse::editor::canBeginDrag(xRay, transform, fuse::editor::GizmoMode::Translate,
                                          fuse::editor::GizmoSpace::World,
                                          fuse::editor::GizmoSystem::kAxisLength,
                                          fuse::editor::GizmoSystem::kPickRadius, snap),
               "canBeginDrag with settings accepts valid ray");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.canPickSnap(hit), "gizmo canPickSnap accepts valid screen hit");
    expectTrue(gizmo.canBeginInteraction(hit), "gizmo canBeginInteraction accepts valid screen hit");
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canUpdateInteraction(hit), "gizmo canUpdateInteraction accepts active drag");
    expectTrue(gizmo.canEndInteraction(), "gizmo canEndInteraction accepts active drag");
    gizmo.endDrag();
}

void testPickRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay emptyRay{};
    fuse::editor::GizmoPickRejectReason reason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, reason),
               "tryPreflightPick rejects empty ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::EmptyRay,
               "empty ray reject reason is EmptyRay");
    expectTrue(fuse::editor::shouldSkipPick(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                            fuse::editor::GizmoSpace::World,
                                            fuse::editor::GizmoSystem::kAxisLength,
                                            fuse::editor::GizmoSystem::kPickRadius),
               "shouldSkipPick true for empty ray");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(fuse::editor::tryPreflightPick(xRay, transform, fuse::editor::GizmoMode::Translate,
                                              fuse::editor::GizmoSpace::World,
                                              fuse::editor::GizmoSystem::kAxisLength,
                                              fuse::editor::GizmoSystem::kPickRadius, reason),
               "tryPreflightPick accepts valid ray");
    expectTrue(reason == fuse::editor::GizmoPickRejectReason::None,
               "valid ray reject reason is None");
    expectTrue(!fuse::editor::shouldSkipPick(xRay, transform, fuse::editor::GizmoMode::Translate,
                                             fuse::editor::GizmoSpace::World,
                                             fuse::editor::GizmoSystem::kAxisLength,
                                             fuse::editor::GizmoSystem::kPickRadius),
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
    deadZone.screenY = 50.f;
    expectTrue(!fuse::editor::tryPreflightPick(deadZone, fuse::editor::GizmoMode::Translate,
                                               screenReason),
               "tryPreflightPick rejects out-of-bounds hit");
    expectTrue(screenReason == fuse::editor::GizmoPickRejectReason::OutOfBounds,
               "out-of-bounds reject reason is OutOfBounds");

    const fuse::editor::PickPreflight missPick = fuse::editor::preflightPick(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyPickReject(missPick) ==
                   fuse::editor::GizmoPickRejectReason::EmptyRay,
               "classifyPickReject maps emptyRay flag");
    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::EmptyRay),
               "EmptyRay") == 0,
               "pick reject reason label for EmptyRay");

    fuse::editor::GizmoSystem gizmo;
    deadZone.screenX = 10.f;
    deadZone.screenY = 50.f;
    expectTrue(gizmo.preflightPickReady(deadZone), "gizmo preflightPickReady accepts valid hit");
    expectTrue(gizmo.tryPreflightPick(deadZone, screenReason),
               "gizmo tryPreflightPick accepts valid hit");
    expectTrue(!gizmo.shouldSkipPick(deadZone), "gizmo shouldSkipPick false for valid hit");
}

void testSnapRejectReasonGuards() {
    fuse::editor::GizmoSnapSettings snap{};

    fuse::editor::GizmoSnapRejectReason reason = fuse::editor::GizmoSnapRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap rejects disabled snap");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::SnapDisabled,
               "disabled snap reject reason is SnapDisabled");
    expectTrue(fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap true when snap disabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::preflightSnapReady(fuse::editor::GizmoMode::Translate, snap, &reason),
               "preflightSnapReady rejects invalid step");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "invalid step reject reason is InvalidStep");

    snap.gridSize = 1.f;
    expectTrue(fuse::editor::tryPreflightSnap(fuse::editor::GizmoMode::Translate, snap, reason),
               "tryPreflightSnap accepts valid snap");
    expectTrue(reason == fuse::editor::GizmoSnapRejectReason::None,
               "valid snap reject reason is None");
    expectTrue(!fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnap false when snap valid");

    snap.gridSize = 0.f;
    const fuse::editor::SnapPreflight invalidStepPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapReject(invalidStepPreflight) ==
                   fuse::editor::GizmoSnapRejectReason::InvalidStep,
               "classifySnapReject maps invalidStep flag");
    expectTrue(std::strcmp(fuse::editor::gizmoSnapRejectReasonLabel(
                   fuse::editor::GizmoSnapRejectReason::SnapDisabled),
               "SnapDisabled") == 0,
               "snap reject reason label for SnapDisabled");

    fuse::editor::GizmoSystem gizmo;
    snap.gridSize = 1.f;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnapReady(), "gizmo preflightSnapReady accepts valid snap");
    expectTrue(gizmo.tryPreflightSnap(reason), "gizmo tryPreflightSnap accepts valid snap");
    expectTrue(!gizmo.shouldSkipSnap(), "gizmo shouldSkipSnap false when snap valid");
}

void testBeginDragRejectReasonGuards() {
    fuse::editor::GizmoTransform transform{};

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

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
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

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!fuse::editor::tryPreflightBeginDrag(
                   emptyRay, transform, fuse::editor::GizmoMode::Translate,
                   fuse::editor::GizmoSpace::World, fuse::editor::GizmoSystem::kAxisLength,
                   fuse::editor::GizmoSystem::kPickRadius, reason),
               "tryPreflightBeginDrag rejects empty ray");
    expectTrue(reason == fuse::editor::GizmoBeginDragRejectReason::EmptyRay,
               "empty ray begin reject reason is EmptyRay");

    const fuse::editor::BeginDragPreflight draggingPreflight = fuse::editor::preflightBeginDrag(
        hit, fuse::editor::GizmoMode::Translate, true);
    expectTrue(fuse::editor::classifyBeginDragReject(draggingPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::AlreadyDragging,
               "classifyBeginDragReject maps alreadyDragging flag");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(gizmo.preflightBeginDragReady(hit), "gizmo preflightBeginDragReady accepts valid hit");
    expectTrue(!gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag false for valid hit");
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.shouldSkipBeginDrag(hit), "gizmo shouldSkipBeginDrag true while dragging");
    gizmo.endDrag();
}

void testUpdateDragRejectReasonGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoUpdateDragRejectReason reason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, false, fuse::editor::GizmoAxis::None,
                                                     reason),
               "tryPreflightUpdateDrag rejects inactive drag");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::NotDragging,
               "inactive drag reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, false, fuse::editor::GizmoAxis::None),
               "shouldSkipUpdateDrag true when not dragging");

    expectTrue(!fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::None,
                                                       &reason),
               "preflightUpdateDragReady rejects drag without axis");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::InvalidActiveAxis,
               "missing axis reject reason is InvalidActiveAxis");

    expectTrue(fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::X,
                                                      &reason),
               "preflightUpdateDragReady accepts active drag with axis");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::None,
               "valid update reject reason is None");

    hit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(hit, true, fuse::editor::GizmoAxis::X, reason),
               "tryPreflightUpdateDrag rejects empty viewport");
    expectTrue(reason == fuse::editor::GizmoUpdateDragRejectReason::EmptyHit,
               "empty viewport update reject reason is EmptyHit");

    hit.viewportWidth = 100.f;
    hit.screenX = -5.f;
    expectTrue(fuse::editor::shouldSkipUpdateDrag(hit, true, fuse::editor::GizmoAxis::X),
               "shouldSkipUpdateDrag true for out-of-bounds hit");
    expectTrue(!fuse::editor::preflightUpdateDragReady(hit, true, fuse::editor::GizmoAxis::X,
                                                       &reason),
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

    fuse::editor::GizmoSystem gizmo;
    hit.screenX = 10.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightUpdateDragReady(hit), "gizmo preflightUpdateDragReady accepts drag");
    expectTrue(!gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag false during drag");
    hit.viewportWidth = 0.f;
    expectTrue(gizmo.shouldSkipUpdateDrag(hit), "gizmo shouldSkipUpdateDrag true for empty hit");
    gizmo.endDrag();
}

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

    fuse::editor::GizmoSystem gizmo;
    expectTrue(gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag true when idle");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightEndDragReady(), "gizmo preflightEndDragReady accepts active drag");
    expectTrue(gizmo.tryPreflightEndDrag(reason), "gizmo tryPreflightEndDrag accepts active drag");
    expectTrue(!gizmo.shouldSkipEndDrag(), "gizmo shouldSkipEndDrag false during drag");
    gizmo.endDrag();
}

fuse::editor::GizmoHitTest hitFromValidScreen() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    return hit;
}

void testNonFiniteRejectReasonClassify() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay nanRay = rayAlongX();
    nanRay.origin.x = std::numeric_limits<fuse::f32>::quiet_NaN();
    const fuse::editor::PickPreflight nanRayPick = fuse::editor::preflightPick(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyPickReject(nanRayPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "classifyPickReject maps nonFiniteRay flag");

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

    fuse::editor::GizmoSystem gizmo;
    gizmo.beginDrag(hitFromValidScreen(), transform);
    nanHit.screenX = 10.f;
    nanHit.screenY = std::numeric_limits<fuse::f32>::infinity();
    const fuse::editor::UpdateDragPreflight nanUpdate = gizmo.preflightUpdateDrag(nanHit);
    expectTrue(fuse::editor::classifyUpdateDragReject(nanUpdate) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");
    gizmo.endDrag();
}

void testUpdateDragInteractionDeltaPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;

    const fuse::editor::GizmoHitTest hit = hitFromValidScreen();

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragInteractionPreflight zeroDelta =
        gizmo.preflightUpdateDragInteraction(hit);
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
    gizmo.endDrag();
}

void testRejectReasonMirrorsExistingPreflights() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

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
}

void testNonFiniteRejectReasonGuards() {
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
               "non-finite ray reject reason is NonFiniteRay");

    const fuse::editor::PickPreflight nanRayPick = fuse::editor::preflightPick(
        nanRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(fuse::editor::classifyPickReject(nanRayPick) ==
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay,
               "classifyPickReject maps nonFiniteRay flag");

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

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate,
                                                    beginReason),
               "tryPreflightBeginDrag rejects non-finite screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "non-finite begin reject reason is NonFiniteHit");

    const fuse::editor::BeginDragPreflight beginPreflight =
        fuse::editor::preflightBeginDrag(nanHit, fuse::editor::GizmoMode::Translate);
    expectTrue(fuse::editor::classifyBeginDragReject(beginPreflight) ==
                   fuse::editor::GizmoBeginDragRejectReason::NonFiniteHit,
               "classifyBeginDragReject maps nonFiniteHit flag");

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    expectTrue(!fuse::editor::tryPreflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X,
                                                     updateReason),
               "tryPreflightUpdateDrag rejects non-finite screen hit");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "non-finite update reject reason is NonFiniteHit");

    const fuse::editor::UpdateDragPreflight updatePreflight =
        fuse::editor::preflightUpdateDrag(nanHit, true, fuse::editor::GizmoAxis::X);
    expectTrue(fuse::editor::classifyUpdateDragReject(updatePreflight) ==
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit,
               "classifyUpdateDragReject maps nonFiniteHit flag");

    expectTrue(std::strcmp(fuse::editor::gizmoPickRejectReasonLabel(
                   fuse::editor::GizmoPickRejectReason::NonFiniteRay),
               "NonFiniteRay") == 0,
               "pick reject reason label for NonFiniteRay");
    expectTrue(std::strcmp(fuse::editor::gizmoUpdateDragRejectReasonLabel(
                   fuse::editor::GizmoUpdateDragRejectReason::NonFiniteHit),
               "NonFiniteHit") == 0,
               "update reject reason label for NonFiniteHit");

    fuse::editor::GizmoHitTest validHit{};
    validHit.viewportWidth = 100.f;
    validHit.viewportHeight = 100.f;
    validHit.screenX = 10.f;
    validHit.screenY = 50.f;

    fuse::editor::GizmoSystem gizmo;
    expectTrue(gizmo.shouldSkipPick(nanHit), "gizmo shouldSkipPick true for non-finite hit");
    expectTrue(gizmo.shouldSkipBeginDrag(nanHit),
               "gizmo shouldSkipBeginDrag true for non-finite hit");
    gizmo.beginDrag(validHit, transform);
    expectTrue(gizmo.shouldSkipUpdateDrag(nanHit),
               "gizmo shouldSkipUpdateDrag true for non-finite hit during drag");
    gizmo.endDrag();
}

void testInteractionPreflightIsSnapDegraded() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::PickInteractionPreflight pickInteraction =
        fuse::editor::preflightPickInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(pickInteraction.isSnapDegraded(),
               "pick interaction isSnapDegraded when step invalid");

    const fuse::editor::BeginDragInteractionPreflight beginInteraction =
        fuse::editor::preflightBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(beginInteraction.isSnapDegraded(),
               "begin drag interaction isSnapDegraded when step invalid");
    expectTrue(beginInteraction.canBegin(), "begin drag interaction still allows begin");

    const fuse::editor::BeginInteractionPreflight beginPreflight =
        fuse::editor::preflightBeginInteraction(hit, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(beginPreflight.isSnapDegraded(),
               "begin interaction isSnapDegraded when step invalid");

    const fuse::editor::UpdateDragInteractionPreflight updateInteraction =
        fuse::editor::preflightUpdateDragInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                     fuse::editor::GizmoMode::Translate, snap,
                                                     0.37f);
    expectTrue(updateInteraction.isSnapDegraded(),
               "update drag interaction isSnapDegraded when step invalid");
    expectTrue(updateInteraction.isSnapDragDegraded(),
               "update drag interaction isSnapDragDegraded when step invalid");
    expectTrue(!updateInteraction.snapDragWillApply(),
               "update drag interaction snap-drag blocked when step invalid");

    const fuse::editor::EndDragInteractionPreflight endInteraction =
        fuse::editor::preflightEndDragInteraction(true, fuse::editor::GizmoAxis::X,
                                                 fuse::editor::GizmoMode::Translate, snap);
    expectTrue(endInteraction.isSnapDegraded(),
               "end drag interaction isSnapDegraded when step invalid");
    expectTrue(endInteraction.canEnd(), "end drag interaction still allows end");

    const fuse::editor::InteractionPreflight idleInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(idleInteraction.isSnapDegraded(),
               "idle interaction preflight isSnapDegraded when step invalid");

    const fuse::editor::InteractionPreflight activeInteraction = fuse::editor::preflightInteraction(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(activeInteraction.isSnapDegraded(),
               "active interaction preflight isSnapDegraded when step invalid");

    snap.gridSize = 1.f;
    const fuse::editor::InteractionPreflight validInteraction = fuse::editor::preflightInteraction(
        hit, false, fuse::editor::GizmoAxis::None, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!validInteraction.isSnapDegraded(),
               "interaction preflight isSnapDegraded false when step valid");
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
               "shouldSkipSnapDrag false for valid delta");

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
    const fuse::editor::SnapDragPreflight degradedPreflight =
        fuse::editor::preflightSnapDrag(0.37f, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(fuse::editor::classifySnapDragReject(degradedPreflight) ==
                   fuse::editor::GizmoSnapDragRejectReason::InvalidStep,
               "classifySnapDragReject maps invalidStep flag");
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
    expectTrue(gizmo.preflightSnapDragReady(0.37f), "gizmo preflightSnapDragReady accepts valid");
    expectTrue(gizmo.tryPreflightSnapDrag(0.37f, reason),
               "gizmo tryPreflightSnapDrag accepts valid delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(0.37f), "gizmo shouldSkipSnapDrag false for valid delta");
    expectTrue(gizmo.shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN()),
               "gizmo shouldSkipSnapDrag true for non-finite delta");
    expectTrue(!gizmo.shouldSkipSnapDrag(), "gizmo shouldSkipSnapDrag false when snap valid");
}

void testInteractionRejectReasonHelpers() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    const fuse::editor::GizmoHitTest hit = hitFromValidScreen();
    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoRay xRay = rayAlongX();

    fuse::editor::GizmoPickRejectReason pickReason = fuse::editor::GizmoPickRejectReason::None;
    expectTrue(fuse::editor::preflightPickInteractionReady(hit, fuse::editor::GizmoMode::Translate,
                                                           snap, &pickReason),
               "pick interaction ready accepts valid screen hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::None,
               "valid pick interaction reject reason is None");
    expectTrue(!fuse::editor::shouldSkipPickInteraction(hit, fuse::editor::GizmoMode::Translate,
                                                        snap),
               "shouldSkipPickInteraction false for valid screen hit");

    fuse::editor::GizmoHitTest nanHit = hit;
    nanHit.screenX = std::numeric_limits<fuse::f32>::quiet_NaN();
    expectTrue(!fuse::editor::preflightPickInteractionReady(
                   nanHit, fuse::editor::GizmoMode::Translate, snap, &pickReason),
               "pick interaction ready rejects non-finite hit");
    expectTrue(pickReason == fuse::editor::GizmoPickRejectReason::NonFiniteHit,
               "non-finite pick interaction reject reason is NonFiniteHit");
    expectTrue(fuse::editor::shouldSkipPickInteraction(xRay, transform,
                                                     fuse::editor::GizmoMode::Translate,
                                                     fuse::editor::GizmoSpace::World,
                                                     fuse::editor::GizmoSystem::kAxisLength,
                                                     fuse::editor::GizmoSystem::kPickRadius, snap) ==
                   false,
               "shouldSkipPickInteraction false for valid ray");

    fuse::editor::GizmoBeginDragRejectReason beginReason =
        fuse::editor::GizmoBeginDragRejectReason::None;
    expectTrue(fuse::editor::preflightBeginDragInteractionReady(hit, fuse::editor::GizmoMode::Translate,
                                                                snap, &beginReason),
               "begin interaction ready accepts valid screen hit");
    expectTrue(beginReason == fuse::editor::GizmoBeginDragRejectReason::None,
               "valid begin interaction reject reason is None");
    expectTrue(!fuse::editor::shouldSkipBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate,
                                                             snap),
               "shouldSkipBeginDragInteraction false for valid screen hit");
    expectTrue(fuse::editor::shouldSkipBeginDragInteraction(hit, fuse::editor::GizmoMode::Translate,
                                                            snap, true),
               "shouldSkipBeginDragInteraction true while already dragging");

    fuse::editor::GizmoUpdateDragRejectReason updateReason =
        fuse::editor::GizmoUpdateDragRejectReason::None;
    fuse::editor::GizmoSnapDragRejectReason snapDragReason =
        fuse::editor::GizmoSnapDragRejectReason::None;
    expectTrue(fuse::editor::preflightUpdateDragInteractionReady(
                   hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap,
                   0.37f, &updateReason, &snapDragReason),
               "update interaction ready accepts active drag with finite delta");
    expectTrue(updateReason == fuse::editor::GizmoUpdateDragRejectReason::None,
               "valid update interaction reject reason is None");
    expectTrue(snapDragReason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid update interaction snap-drag reject reason is None");
    expectTrue(!fuse::editor::shouldSkipUpdateDragInteraction(hit, true, fuse::editor::GizmoAxis::X,
                                                              fuse::editor::GizmoMode::Translate,
                                                              snap),
               "shouldSkipUpdateDragInteraction false for active drag");

    expectTrue(fuse::editor::preflightUpdateDragInteractionReady(
                   hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap,
                   std::numeric_limits<fuse::f32>::quiet_NaN(), &updateReason, &snapDragReason),
               "update interaction ready still allows drag with non-finite delta");
    expectTrue(snapDragReason == fuse::editor::GizmoSnapDragRejectReason::DeltaNonFinite,
               "non-finite delta surfaces snap-drag reject reason in interaction preflight");

    fuse::editor::GizmoEndDragRejectReason endReason = fuse::editor::GizmoEndDragRejectReason::None;
    expectTrue(!fuse::editor::preflightEndDragInteractionReady(false, fuse::editor::GizmoAxis::None,
                                                               fuse::editor::GizmoMode::Translate,
                                                               snap, &endReason),
               "end interaction ready rejects inactive drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragRejectReason::NotDragging,
               "inactive end interaction reject reason is NotDragging");
    expectTrue(fuse::editor::shouldSkipEndDragInteraction(false, fuse::editor::GizmoAxis::None,
                                                          fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipEndDragInteraction true when not dragging");

    expectTrue(fuse::editor::preflightEndDragInteractionReady(true, fuse::editor::GizmoAxis::X,
                                                              fuse::editor::GizmoMode::Translate,
                                                              snap, &endReason),
               "end interaction ready accepts active drag");
    expectTrue(endReason == fuse::editor::GizmoEndDragRejectReason::None,
               "valid end interaction reject reason is None");

    expectTrue(fuse::editor::shouldSkipSnapDrag(fuse::editor::GizmoMode::Translate, snap) ==
                   fuse::editor::shouldSkipSnap(fuse::editor::GizmoMode::Translate, snap),
               "mode-only shouldSkipSnapDrag mirrors shouldSkipSnap");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightPickInteractionReady(hit), "gizmo pick interaction ready accepts hit");
    expectTrue(gizmo.preflightBeginDragInteractionReady(hit),
               "gizmo begin interaction ready accepts hit");
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.preflightUpdateDragInteractionReady(hit, 0.37f),
               "gizmo update interaction ready accepts active drag");
    expectTrue(gizmo.preflightEndDragInteractionReady(),
               "gizmo end interaction ready accepts active drag");
    expectTrue(!gizmo.shouldSkipEndDragInteraction(),
               "gizmo shouldSkipEndDragInteraction false while dragging");
    gizmo.endDrag();
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
    testGizmoUpdateDragSnapDegradedPreflight();
    testDirtyFlagOnEndDrag();
    testHitTestOutOfBoundsGuards();
    testBeginDragPreflightAxisAndSnapDegraded();
    testUpdateDragOutOfBoundsPreflight();
    testGizmoUpdateDragSnapDegradedPreflight();
    testPickSnapPreflight();
    testBeginInteractionPreflight();
    testUpdateInteractionPreflight();
    testEndInteractionPreflight();
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
    testCanInteractionPredicates();
    testPickRejectReasonGuards();
    testSnapRejectReasonGuards();
    testBeginDragRejectReasonGuards();
    testUpdateDragRejectReasonGuards();
    testEndDragRejectReasonGuards();
    testNonFiniteRejectReasonClassify();
    testUpdateDragInteractionDeltaPreflight();
    testRejectReasonMirrorsExistingPreflights();
    testNonFiniteRejectReasonGuards();
    testInteractionPreflightIsSnapDegraded();
    testSnapDragRejectReasonGuards();
    testInteractionRejectReasonHelpers();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_editor_gizmo_system: all tests passed\n");
    return EXIT_SUCCESS;
}
