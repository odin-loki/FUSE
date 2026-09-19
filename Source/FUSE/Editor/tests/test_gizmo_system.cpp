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

void testPickInputValidityGuards() {
    fuse::editor::GizmoRay emptyRay{};
    expectTrue(!fuse::editor::isRayValid(emptyRay), "isRayValid rejects empty ray");
    expectTrue(fuse::editor::isRayEmpty(emptyRay), "empty ray remains empty");

    fuse::editor::GizmoRay ray = rayAlongX();
    expectTrue(fuse::editor::isRayValid(ray), "isRayValid accepts non-empty ray");
    ray.direction = {2.f, 0.f, 0.f};
    expectTrue(fuse::editor::normalizeRay(ray), "normalizeRay succeeds on non-zero direction");
    expectNear(ray.direction.x, 1.f, 0.001f, "normalizeRay unitizes X direction");

    fuse::editor::GizmoRay zeroRay{};
    expectTrue(!fuse::editor::normalizeRay(zeroRay), "normalizeRay rejects zero direction");

    fuse::editor::GizmoHitTest emptyHit{};
    emptyHit.viewportWidth = 0.f;
    expectTrue(!fuse::editor::isHitTestValid(emptyHit), "isHitTestValid rejects empty viewport");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    expectTrue(fuse::editor::isHitTestValid(hit), "isHitTestValid accepts valid viewport");
}

void testSnapComponentHelpers() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;
    snap.rotateSnap = true;
    snap.angleStepDegrees = 15.f;
    snap.scaleSnap = true;
    snap.scaleGridStep = 0.25f;

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

    snap.translateSnap = false;
    expectTrue(!fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta rejects disabled translate snap");
    snap.translateSnap = true;
    snap.gridSize = 0.f;
    expectTrue(!fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta rejects invalid translate step");
    snap.gridSize = 1.f;
    expectTrue(fuse::editor::canSnapDragDelta(fuse::editor::GizmoMode::Translate, snap),
               "canSnapDragDelta accepts valid translate snap");
}

void testCanUpdateDragGuards() {
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    expectTrue(!fuse::editor::canUpdateDrag(hit, false), "canUpdateDrag rejects inactive drag");
    expectTrue(!fuse::editor::canUpdateDrag(hit, true), "canUpdateDrag rejects drag without axis");

    const fuse::editor::UpdateDragPreflight noAxisPreflight =
        fuse::editor::preflightUpdateDrag(hit, true);
    expectTrue(noAxisPreflight.invalidActiveAxis, "update preflight marks missing active axis");
    expectTrue(!noAxisPreflight.canUpdate(), "update preflight rejects missing active axis");

    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canUpdateDrag(hit), "gizmo canUpdateDrag accepts active drag with axis");

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
    expectTrue(gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag accepts valid active drag");
    expectTrue(result.changed, "tryUpdateDrag marks result changed");
    expectTrue(result.axis == fuse::editor::GizmoAxis::X, "tryUpdateDrag records active axis");

    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.tryUpdateDrag(hit, result), "tryUpdateDrag rejects empty viewport");
    expectTrue(gizmo.isDragging(), "tryUpdateDrag reject keeps drag active");
    gizmo.endDrag();
}

void testPickPreflightAxisResolution() {
    fuse::editor::GizmoTransform transform{};

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::PickPreflight rayPick = fuse::editor::preflightPick(
        xRay, transform, fuse::editor::GizmoMode::Translate, fuse::editor::GizmoSpace::World,
        fuse::editor::GizmoSystem::kAxisLength, fuse::editor::GizmoSystem::kPickRadius);
    expectTrue(rayPick.canPick(), "pick preflight accepts valid ray");
    expectTrue(rayPick.axis == fuse::editor::GizmoAxis::X, "pick preflight resolves ray axis");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    const fuse::editor::PickPreflight screenPick =
        fuse::editor::preflightPick(hit, fuse::editor::GizmoMode::Translate);
    expectTrue(screenPick.canPick(), "pick preflight accepts valid screen hit");
    expectTrue(screenPick.axis == fuse::editor::GizmoAxis::X,
               "pick preflight resolves screen axis");
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

    const fuse::editor::UpdateDragPreflight degradedPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(degradedPreflight.canUpdate(),
               "update preflight still allows drag when snap step invalid");
    expectTrue(degradedPreflight.snapDegraded, "update preflight marks snap degraded");

    snap.gridSize = 1.f;
    const fuse::editor::UpdateDragPreflight validPreflight = fuse::editor::preflightUpdateDrag(
        hit, true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(validPreflight.canUpdate(), "update preflight accepts valid snap settings");
    expectTrue(!validPreflight.snapDegraded, "valid snap clears snapDegraded");
}

void testEndDragPreflightGuards() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

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

    snap.gridSize = 1.f;
    const fuse::editor::EndDragPreflight validSnapPreflight = fuse::editor::preflightEndDrag(
        true, fuse::editor::GizmoAxis::X, fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!validSnapPreflight.snapDegraded, "valid snap clears snapDegraded on end");

    fuse::editor::GizmoSystem gizmo;
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
    expectTrue(fuse::editor::canEndDrag(true, fuse::editor::GizmoAxis::X),
               "canEndDrag accepts active drag with axis");

    fuse::editor::GizmoSystem gizmo;
    expectTrue(!gizmo.canEndDrag(), "gizmo canEndDrag rejects inactive drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canEndDrag(), "gizmo canEndDrag accepts active drag");
    gizmo.endDrag();
}

void testTryEndDragGuards() {
    fuse::editor::GizmoSystem gizmo;
    fuse::editor::GizmoResult result{};

    expectTrue(!gizmo.tryEndDrag(result), "tryEndDrag rejects inactive drag");
    expectTrue(!result.changed, "inactive tryEndDrag leaves result unchanged");

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

    snap.gridSize = 1.f;
    expectTrue(!fuse::editor::isSnapDegraded(fuse::editor::GizmoMode::Translate, snap),
               "isSnapDegraded false when translate snap has valid step");
}

void testPickInteractionPreflight() {
    fuse::editor::GizmoTransform transform{};
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

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
                                               fuse::editor::GizmoSpace::World,
                                               fuse::editor::GizmoSystem::kAxisLength,
                                               fuse::editor::GizmoSystem::kPickRadius, snap);
    expectTrue(!emptyRayPick.canPick(), "pick interaction rejects empty ray");
    expectTrue(emptyRayPick.pick.emptyRay, "pick interaction marks empty ray");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoSystem gizmo;
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
    gizmo.setSnapSettings(snap);
    expectTrue(gizmo.canBeginDragInteraction(hit),
               "gizmo canBeginDragInteraction accepts valid screen hit");

    const fuse::editor::GizmoRay xRay = rayAlongX();
    const fuse::editor::BeginDragInteractionPreflight rayBegin =
        gizmo.preflightBeginDragInteraction(xRay, transform);
    expectTrue(rayBegin.canBegin(), "gizmo begin interaction accepts valid ray");
    expectTrue(gizmo.canBeginDragInteraction(xRay, transform),
               "gizmo canBeginDragInteraction accepts valid ray");
}

void testUpdateDragInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 1.f;

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;

    const fuse::editor::UpdateDragInteractionPreflight inactive =
        fuse::editor::preflightUpdateDragInteraction(hit, false, fuse::editor::GizmoAxis::None,
                                                     fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactive.canUpdate(), "update interaction rejects inactive drag");
    expectTrue(inactive.drag.notDragging, "update interaction marks inactive drag");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);

    const fuse::editor::UpdateDragInteractionPreflight active =
        gizmo.preflightUpdateDragInteraction(hit);
    expectTrue(active.canUpdate(), "update interaction accepts active drag");
    expectTrue(active.snapWillApply(), "update interaction reports snap will apply");

    hit.viewportWidth = 0.f;
    expectTrue(!gizmo.canUpdateDragInteraction(hit),
               "gizmo canUpdateDragInteraction rejects empty viewport");
    gizmo.endDrag();
}

void testEndDragInteractionPreflight() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.f;

    const fuse::editor::EndDragInteractionPreflight inactive =
        fuse::editor::preflightEndDragInteraction(false, fuse::editor::GizmoAxis::None,
                                                  fuse::editor::GizmoMode::Translate, snap);
    expectTrue(!inactive.canEnd(), "end interaction rejects inactive drag");
    expectTrue(inactive.end.notDragging, "end interaction marks inactive drag");

    const fuse::editor::EndDragInteractionPreflight active =
        fuse::editor::preflightEndDragInteraction(true, fuse::editor::GizmoAxis::X,
                                                  fuse::editor::GizmoMode::Translate, snap);
    expectTrue(active.canEnd(), "end interaction accepts active drag");
    expectTrue(active.snapDegraded, "end interaction marks snap degraded with invalid step");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    expectTrue(!gizmo.canEndDragInteraction(), "gizmo canEndDragInteraction rejects inactive drag");

    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    fuse::editor::GizmoTransform transform{};
    gizmo.beginDrag(hit, transform);
    expectTrue(gizmo.canEndDragInteraction(), "gizmo canEndDragInteraction accepts active drag");
    expectTrue(gizmo.preflightEndDragInteraction().canEnd(),
               "gizmo end interaction preflight accepts active drag");
    gizmo.endDrag();
}

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
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite NaN delta");
    expectTrue(fuse::editor::shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::infinity(),
                                                fuse::editor::GizmoMode::Translate, snap),
               "shouldSkipSnapDrag true for non-finite infinity delta");

    snap.translateSnap = false;
    expectTrue(!fuse::editor::preflightSnapDragReady(0.37f, fuse::editor::GizmoMode::Translate, snap,
                                                     &reason),
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
               "tryPreflightSnapDrag accepts valid delta after step reset");
    expectTrue(reason == fuse::editor::GizmoSnapDragRejectReason::None,
               "valid snap-drag reject reason is None after step reset");

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
    expectTrue(gizmo.shouldSkipSnapDrag(std::numeric_limits<fuse::f32>::quiet_NaN()),
               "gizmo shouldSkipSnapDrag true for non-finite delta");
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
    testSnapDragRejectReasonGuards();
    testUpdateDragInteractionDeltaPreflight();
    testRejectReasonMirrorsExistingPreflights();
    testNonFiniteRejectReasonGuards();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_editor_gizmo_system: all tests passed\n");
    return EXIT_SUCCESS;
}
