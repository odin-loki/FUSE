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

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!fuse::editor::canBeginDrag(deadZone, fuse::editor::GizmoMode::Translate),
               "canBeginDrag rejects translate dead zone");
    expectTrue(!gizmo.canBeginDrag(deadZone), "gizmo canBeginDrag rejects translate dead zone");

    deadZone.screenX = 10.f;
    deadZone.screenY = 50.f;
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
                                          fuse::editor::GizmoSpace::World,
                                          fuse::editor::GizmoSystem::kAxisLength,
                                          fuse::editor::GizmoSystem::kPickRadius),
               "canBeginDrag accepts valid ray pick");
    expectTrue(gizmo.canBeginDrag(xRay, transform), "gizmo canBeginDrag accepts valid ray pick");

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    expectTrue(!gizmo.canBeginDrag(missRay, transform), "gizmo canBeginDrag rejects ray miss");
}

void testCanPickAxisGuards() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!fuse::editor::canPickAxis(emptyRay, transform, fuse::editor::GizmoMode::Translate,
                                          fuse::editor::GizmoSpace::World,
                                          fuse::editor::GizmoSystem::kAxisLength,
                                          fuse::editor::GizmoSystem::kPickRadius),
               "canPickAxis rejects empty ray");
    expectTrue(!gizmo.canPickAxis(emptyRay, transform), "gizmo canPickAxis rejects empty ray");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    expectTrue(gizmo.canPickAxis(xRay, transform), "gizmo canPickAxis accepts valid ray");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    expectTrue(!fuse::editor::canPickAxis(deadZone, fuse::editor::GizmoMode::Translate),
               "canPickAxis rejects translate dead zone");
    expectTrue(!gizmo.canPickAxis(deadZone), "gizmo canPickAxis rejects translate dead zone");

    deadZone.screenX = 10.f;
    expectTrue(gizmo.canPickAxis(deadZone), "gizmo canPickAxis accepts valid screen hit");
}

void testSnapStepForMode() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.gridSize = 0.5f;
    snap.angleStepDegrees = 15.f;
    snap.scaleGridStep = 0.25f;

    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Translate, snap), 0.5f, 0.001f,
               "snapStepForMode returns translate grid size");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Rotate, snap), 15.f, 0.001f,
               "snapStepForMode returns rotate angle step");
    expectNear(fuse::editor::snapStepForMode(fuse::editor::GizmoMode::Scale, snap), 0.25f, 0.001f,
               "snapStepForMode returns scale grid step");

    snap.translateSnap = true;
    expectNear(fuse::editor::snapValue(1.37f, fuse::editor::GizmoMode::Translate, snap),
               fuse::editor::snapToGrid(1.37f, fuse::editor::snapStepForMode(
                                                fuse::editor::GizmoMode::Translate, snap)),
               0.001f, "snapValue uses snapStepForMode grid for translate");
}

void testBeginDragRayUsesTryBeginDrag() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    const fuse::editor::GizmoResult miss = gizmo.beginDrag(missRay, transform);
    expectTrue(!miss.active, "beginDrag ray rejects miss via tryBeginDrag guards");
    expectTrue(!gizmo.isDragging(), "beginDrag ray miss does not start drag");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::GizmoResult started = gizmo.beginDrag(xRay, transform);
    expectTrue(started.active, "beginDrag ray accepts valid pick");
    expectTrue(started.axis == fuse::editor::GizmoAxis::X, "beginDrag ray records axis");
    gizmo.endDrag();
}

void testBeginDragPreflight() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay emptyRay{};
    const fuse::editor::BeginDragPreflight emptyRayPreflight = fuse::editor::preflightBeginDrag(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(emptyRayPreflight.emptyRay, "preflight marks empty ray");
    expectTrue(!emptyRayPreflight.canBegin, "preflight rejects empty ray");

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    const fuse::editor::BeginDragPreflight missPreflight = fuse::editor::preflightBeginDrag(
        missRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(missPreflight.pickMiss, "preflight marks ray pick miss");
    expectTrue(!missPreflight.canBegin, "preflight rejects ray pick miss");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragPreflight validRayPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(validRayPreflight.canBegin, "preflight accepts valid ray pick");
    expectTrue(!validRayPreflight.emptyRay, "valid ray preflight clears emptyRay");
    expectTrue(!validRayPreflight.pickMiss, "valid ray preflight clears pickMiss");

    const fuse::editor::BeginDragPreflight invalidConfigPreflight = fuse::editor::preflightBeginDrag(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World, 0.f,
        fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(invalidConfigPreflight.invalidPickConfig,
               "preflight marks invalid axis length / pick radius");
    expectTrue(!invalidConfigPreflight.canBegin, "preflight rejects invalid pick config");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    const fuse::editor::BeginDragPreflight emptyHitPreflight =
        fuse::editor::preflightBeginDrag(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyHitPreflight.emptyHit, "preflight marks empty viewport");
    expectTrue(!emptyHitPreflight.canBegin, "preflight rejects empty viewport");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    const fuse::editor::BeginDragPreflight screenMissPreflight =
        fuse::editor::preflightBeginDrag(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(screenMissPreflight.screenMiss, "preflight marks translate dead zone");
    expectTrue(!screenMissPreflight.canBegin, "preflight rejects translate dead zone");

    deadZone.screenX = 10.f;
    deadZone.screenY = 50.f;
    const fuse::editor::BeginDragPreflight validHitPreflight =
        fuse::editor::preflightBeginDrag(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(validHitPreflight.canBegin, "preflight accepts valid screen hit");

    fuse::editor::GizmoSystem gizmo;
    gizmo.beginDrag(deadZone, transform);
    const fuse::editor::BeginDragPreflight draggingPreflight = gizmo.preflightBeginDrag(deadZone);
    expectTrue(draggingPreflight.alreadyDragging, "gizmo preflight marks active drag");
    expectTrue(!draggingPreflight.canBegin, "gizmo preflight rejects while dragging");
    gizmo.endDrag();
}

void testSnapStepGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    expectTrue(!fuse::editor::isSnapStepValid(fuse::editor::GizmoMode::Translate, snap),
               "isSnapStepValid rejects zero translate grid when snap enabled");
    expectTrue(!fuse::editor::canApplySnap(fuse::editor::GizmoMode::Translate, snap),
               "canApplySnap rejects zero translate grid when snap enabled");

    snap.gridSize = 0.5f;
    expectTrue(fuse::editor::isSnapStepValid(fuse::editor::GizmoMode::Translate, snap),
               "isSnapStepValid accepts positive translate grid");
    expectTrue(fuse::editor::canApplySnap(fuse::editor::GizmoMode::Translate, snap),
               "canApplySnap accepts positive translate grid");

    snap.translateSnap = false;
    expectTrue(fuse::editor::isSnapStepValid(fuse::editor::GizmoMode::Translate, snap),
               "isSnapStepValid passes when translate snap disabled");
    expectTrue(!fuse::editor::canApplySnap(fuse::editor::GizmoMode::Translate, snap),
               "canApplySnap false when translate snap disabled");

    snap.rotateSnap = true;
    snap.angleStepDegrees = 0.f;
    expectTrue(!fuse::editor::canApplySnap(fuse::editor::GizmoMode::Rotate, snap),
               "canApplySnap rejects zero angle step when rotate snap enabled");

    fuse::editor::GizmoTransform transform{};
    transform.posX = 1.37f;
    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::GizmoTransform unsnapped =
        fuse::editor::snapTransform(transform, fuse::editor::GizmoMode::Translate, snap);
    expectNear(unsnapped.posX, 1.37f, 0.001f,
               "snapTransform passthrough when step invalid (snapToGrid guard)");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    gizmo.setMode(fuse::editor::GizmoMode::Translate);

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    gizmo.beginDrag(hit, transform);
    hit.screenX = 40.f;
    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(update.changed, "drag update still applies when snap step invalid");
    gizmo.endDrag();
}

void testPickConfigValid() {
    expectTrue(!fuse::editor::isPickConfigValid(0.f, fuse::editor::GizmoSystem::kPickRadius),
               "isPickConfigValid rejects zero axis length");
    expectTrue(!fuse::editor::isPickConfigValid(fuse::editor::GizmoSystem::kAxisLength, 0.f),
               "isPickConfigValid rejects zero pick radius");
    expectTrue(fuse::editor::isPickConfigValid(fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius),
               "isPickConfigValid accepts default gizmo constants");
}

void testPickPreflightGuards() {
    fuse::editor::GizmoTransform transform{};

    fuse::editor::GizmoRay emptyRay{};
    const fuse::editor::PickPreflight emptyRayPick = fuse::editor::preflightPick(
        emptyRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(emptyRayPick.emptyRay, "pick preflight marks empty ray");
    expectTrue(!emptyRayPick.canPick(), "pick preflight rejects empty ray");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickPreflight validRayPick = fuse::editor::preflightPick(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(validRayPick.canPick(), "pick preflight accepts valid ray");
    expectTrue(!validRayPick.emptyRay, "valid ray pick clears emptyRay");
    expectTrue(!validRayPick.pickMiss, "valid ray pick clears pickMiss");

    fuse::editor::GizmoRay missRay;
    missRay.origin = {0.f, 5.f, 0.f};
    missRay.direction = {1.f, 0.f, 0.f};
    const fuse::editor::PickPreflight missPick = fuse::editor::preflightPick(
        missRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(missPick.pickMiss, "pick preflight marks ray miss");
    expectTrue(!missPick.canPick(), "pick preflight rejects ray miss");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    const fuse::editor::PickPreflight emptyHitPick =
        fuse::editor::preflightPick(emptyHit, fuse::editor::GizmoMode::Translate);
    expectTrue(emptyHitPick.emptyHit, "pick preflight marks empty viewport");
    expectTrue(!emptyHitPick.canPick(), "pick preflight rejects empty viewport");

    fuse::editor::GizmoHitTest deadZone{};
    deadZone.viewportWidth = 100.f;
    deadZone.viewportHeight = 100.f;
    deadZone.screenX = 50.f;
    deadZone.screenY = 50.f;
    const fuse::editor::PickPreflight screenMissPick =
        fuse::editor::preflightPick(deadZone, fuse::editor::GizmoMode::Translate);
    expectTrue(screenMissPick.screenMiss, "pick preflight marks translate dead zone");
    expectTrue(!screenMissPick.canPick(), "pick preflight rejects translate dead zone");

    fuse::editor::GizmoSystem gizmo;
    deadZone.screenX = 10.f;
    deadZone.screenY = 50.f;
    const fuse::editor::PickPreflight gizmoPick = gizmo.preflightPick(deadZone);
    expectTrue(gizmoPick.canPick(), "gizmo pick preflight accepts valid screen hit");
    expectTrue(gizmo.preflightPick(xRay, transform).canPick(),
               "gizmo pick preflight accepts valid ray");
}

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
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(emptyPick.axis == fuse::editor::GizmoAxis::None,
               "failed pick preflight leaves axis unset");
}

void testSnapPreflightGuards() {
    fuse::editor::GizmoSnapSettings snap{};

    const fuse::editor::SnapPreflight disabledPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(disabledPreflight.snapDisabled, "snap preflight marks disabled snap");
    expectTrue(!disabledPreflight.canApply(), "snap preflight rejects disabled snap");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    const fuse::editor::SnapPreflight invalidStepPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(invalidStepPreflight.invalidStep, "snap preflight marks invalid step");
    expectTrue(!invalidStepPreflight.canApply(), "snap preflight rejects invalid step");

    snap.gridSize = 0.5f;
    const fuse::editor::SnapPreflight validPreflight =
        fuse::editor::preflightSnap(fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canApply(), "snap preflight accepts enabled snap with valid step");
    expectTrue(!validPreflight.snapDisabled, "valid snap preflight clears snapDisabled");
    expectTrue(!validPreflight.invalidStep, "valid snap preflight clears invalidStep");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.preflightSnap().canApply(), "gizmo snap preflight accepts valid settings");
    expectTrue(gizmo.canApplySnapNow(), "gizmo canApplySnapNow mirrors preflight");
}

void testTrySnapTransformGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.gridSize = 1.f;

    fuse::editor::GizmoTransform transform{};
    transform.posX = 1.6f;
    transform.posY = -2.4f;

    fuse::editor::GizmoTransform out{};
    expectTrue(!fuse::editor::trySnapTransform(transform, fuse::editor::GizmoMode::Translate, snap,
                                               out),
               "trySnapTransform rejects disabled snap");
    expectNear(out.posX, 1.6f, 0.001f, "trySnapTransform leaves transform unchanged on reject");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::trySnapTransform(transform, fuse::editor::GizmoMode::Translate, snap,
                                               out),
               "trySnapTransform rejects invalid step");
    expectNear(out.posX, 1.6f, 0.001f, "trySnapTransform leaves transform unchanged on invalid step");

    snap.gridSize = 1.f;
    expectTrue(fuse::editor::trySnapTransform(transform, fuse::editor::GizmoMode::Translate, snap,
                                                out),
               "trySnapTransform applies when snap is valid");
    expectNear(out.posX, 2.f, 0.001f, "trySnapTransform snaps position X");
    expectNear(out.posY, -2.f, 0.001f, "trySnapTransform snaps position Y");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform gizmoOut{};
    expectTrue(gizmo.trySnapTransform(transform, gizmoOut),
               "gizmo trySnapTransform applies when snap is valid");
    expectNear(gizmoOut.posX, 2.f, 0.001f, "gizmo trySnapTransform snaps position X");
}

void testTrySnapDragDeltaGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.gridSize = 0.5f;

    expectNear(fuse::editor::trySnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               0.37f, 0.001f, "trySnapDragDelta passthrough when translate snap disabled");

    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectNear(fuse::editor::trySnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               0.37f, 0.001f, "trySnapDragDelta passthrough when step invalid");

    snap.gridSize = 0.5f;
    expectNear(fuse::editor::trySnapDragDelta(0.37f, fuse::editor::GizmoMode::Translate, snap),
               0.5f, 0.001f, "trySnapDragDelta snaps when snap is valid");
}

void testUpdateDragPreflightGuards() {
    fuse::editor::GizmoSystem gizmo;
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

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    const fuse::editor::UpdateDragPreflight activePreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(activePreflight.canUpdate(), "update preflight accepts active drag with valid hit");

    hit.viewportWidth = 0.f;
    const fuse::editor::UpdateDragPreflight emptyPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(emptyPreflight.emptyHit, "update preflight marks empty viewport");
    expectTrue(!emptyPreflight.canUpdate(), "update preflight rejects empty viewport");

    const fuse::editor::GizmoResult update = gizmo.updateDrag(hit);
    expectTrue(!update.changed, "updateDrag ignores empty viewport via preflight guard");
    expectTrue(gizmo.isDragging(), "empty viewport update keeps drag active");
    gizmo.endDrag();
}

void testUpdateDragSnapDegradedPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

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

    snap.gridSize = 0.5f;
    const fuse::editor::UpdateDragPreflight healthyPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(healthyPreflight.canUpdate(), "healthy snap preflight allows update");
    expectTrue(!healthyPreflight.snapDegraded, "healthy snap preflight clears snapDegraded");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    snap.gridSize = 0.f;
    gizmo.setSnapSettings(snap);
    const fuse::editor::UpdateDragPreflight gizmoPreflight = gizmo.preflightUpdateDrag(hit);
    expectTrue(gizmoPreflight.canUpdate(), "gizmo preflight allows update with degraded snap");
    expectTrue(gizmoPreflight.snapDegraded, "gizmo preflight marks degraded snap settings");
    gizmo.endDrag();
}

void testCanUpdateDragGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(!fuse::editor::canUpdateDrag(hit, false),
               "canUpdateDrag rejects inactive drag");
    expectTrue(fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag accepts active drag");

    hit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag rejects empty viewport");
    expectTrue(!fuse::editor::canUpdateDrag(hit, true, fuse::editor::GizmoMode::Translate, {}),
               "canUpdateDrag overload rejects empty viewport");

    fuse::editor::GizmoSystem gizmo;
    hit.viewportWidth = 100.f;
    expectTrue(!gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag rejects inactive drag");

    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag");
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
    transform.posX = 1.f;
    gizmo.beginDrag(hit, transform);

    hit.screenX = 30.f;
    expectTrue(gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag accepts active drag");
    expectTrue(result.changed, "tryUpdateDrag marks result changed");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryUpdateDrag preserves active axis");
    expectTrue(result.transform.posX != 1.f, "tryUpdateDrag applies drag delta");

    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects empty viewport");
    expectTrue(gizmo.isDragging(), "tryUpdateDrag reject keeps drag active");
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
    testUpdateDragSnapDegradedPreflight();
    testCanUpdateDragGuards();
    testTryUpdateDragGuards();
    testPickPreflightAxisResolution();
    testDirtyFlagOnEndDrag();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_editor_gizmo_system: all tests passed\n");
    return EXIT_SUCCESS;
}
