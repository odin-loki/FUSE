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

    gizmo.setMode(fuse::editor::GizmoMode::Rotate);
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Rotate, "mode switches to rotate");

    gizmo.setMode(fuse::editor::GizmoMode::Scale);
    expectTrue(gizmo.mode() == fuse::editor::GizmoMode::Scale, "mode switches to scale");

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

void testSnapHelpers() {
    expectNear(fuse::editor::snapToGrid(1.37f, 0.5f), 1.5f, 0.001f, "translate snaps to grid");
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
    testSnapHelpers();
    testDeltaApplyRoundtrip();
    testDirtyFlagOnEndDrag();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_editor_gizmo_system: all tests passed\n");
    return EXIT_SUCCESS;
}
