#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/gizmo_system.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!gizmo.tryBeginDrag(deadZone, transform, result),
               "tryBeginDrag rejects translate dead-zone hit");
    expectTrue(!gizmo.isDragging(), "dead-zone hit does not start drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(gizmo.tryBeginDrag(hit, transform, result), "tryBeginDrag accepts valid screen hit");
    expectTrue(result.active, "valid screen hit activates drag");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "valid screen hit records axis");
    expectTrue(!gizmo.tryBeginDrag(hit, transform, result),
               "tryBeginDrag rejects second begin while dragging");
    gizmo.endDrag();

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(gizmo.tryBeginDrag(xRay, transform, result), "tryBeginDrag accepts valid ray");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "valid ray records axis");
    gizmo.endDrag();

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    expectTrue(!gizmo.tryBeginDrag(missRay, transform, result),
               "tryBeginDrag rejects ray miss");
    expectTrue(!gizmo.isDragging(), "ray miss does not start drag");
}

void testCanBeginDragPreflight() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!gizmo.canBeginDrag(emptyHit), "canBeginDrag rejects empty viewport");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!gizmo.canBeginDrag(deadZone), "canBeginDrag rejects translate dead zone");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    expectTrue(gizmo.canBeginDrag(hit), "canBeginDrag accepts valid screen hit");

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!gizmo.canBeginDrag(emptyRay, transform), "canBeginDrag rejects empty ray");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(gizmo.canBeginDrag(xRay, transform), "canBeginDrag accepts valid ray");

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    expectTrue(!gizmo.canBeginDrag(missRay, transform), "canBeginDrag rejects ray miss");
}

void testDragLifecycleGuards() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    fuse::editor::GizmoTransform transform{};
    const fuse::editor::GizmoResult idleUpdate = gizmo.updateDrag(hit);
    expectTrue(!idleUpdate.changed, "updateDrag is no-op when not dragging");

    const fuse::editor::GizmoResult idleEnd = gizmo.endDrag();
    expectTrue(!idleEnd.changed, "endDrag is no-op when not dragging");
    expectTrue(!gizmo.transformDirty(), "idle endDrag does not mark dirty");

    gizmo.beginDrag(hit, transform);
    hit.screenX = 30.f;
    const fuse::editor::GizmoResult activeUpdate = gizmo.updateDrag(hit);
    expectTrue(activeUpdate.changed, "updateDrag applies delta while dragging");
    gizmo.endDrag();
}

void testSnapStepForMode() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.5f;
    snap.rotateSnap = true;
    snap.angleStepDegrees = 15.f;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;

    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Translate, snap), 0.5f, 0.001f,
               "snapStepForMode returns translate grid");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Rotate, snap), 15.f, 0.001f,
               "snapStepForMode returns rotate step degrees");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Scale, snap), 0.25f, 0.001f,
               "snapStepForMode returns scale grid step");

    snap.rotateSnap = false;
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Rotate, snap), 0.f, 0.001f,
               "snapStepForMode returns zero when snap disabled");

    expectTrue(fuse::editor::isAxisEmpty(fuse::editor::GizmoAxis::None),
               "isAxisEmpty reports none axis");
    expectTrue(!fuse::editor::isAxisEmpty(fuse::editor::GizmoAxis::X),
               "isAxisEmpty rejects active axis");
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
    testCanBeginDragPreflight();
    testDragLifecycleGuards();
    testSnapStepForMode();
    testUpdateDragEmptyViewportGuard();
    testCycleModeCancelsDrag();
    testSnapDragDeltaModeAware();
    testPickAxisFromRayEmptyGuard();
    testDirtyFlagOnEndDrag();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_editor_gizmo_system: all tests passed\n");
    return EXIT_SUCCESS;
}
