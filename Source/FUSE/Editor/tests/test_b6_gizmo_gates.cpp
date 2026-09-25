// B6.4 / B6.13 gates — gizmo drag math, axis picking, snapping, screen-size scaling, undo wiring.
#include <fuse/core/init.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/gizmo_system.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/math/quat.hpp>
#include <fuse/math/vec.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;
constexpr float kPi = 3.14159265358979f;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float actual, float expected, float epsilon, const char* message) {
    if (!(std::fabs(actual - expected) <= epsilon)) {
        std::fprintf(stderr, "FAIL: %s (got %.7f expected %.7f)\n", message, actual, expected);
        ++g_failures;
    }
}

using fuse::editor::GizmoAxis;
using fuse::editor::GizmoMode;
using fuse::editor::GizmoRay;
using fuse::editor::GizmoSpace;
using fuse::editor::GizmoTransform;
using fuse::math::Quat;
using fuse::math::Vec3;

GizmoTransform at(float x, float y, float z, const Quat& rotation = Quat{}) {
    return fuse::editor::gizmoFromMath({x, y, z}, rotation, {1.f, 1.f, 1.f});
}

/// Ray from a camera at `eye` through world point `target`.
GizmoRay rayThrough(const Vec3& eye, const Vec3& target) {
    return GizmoRay{eye, (target - eye).normalized()};
}

bool quatNear(const Quat& a, const Quat& b, float eps) {
    const float d = std::fabs(a.dot(b)); // q and -q are the same rotation
    return std::fabs(d - 1.f) <= eps;
}

// Gate: translation gizmo moves entity along the correct world/local axis — verified numerically.
void testTranslateWorldAndLocal() {
    const GizmoTransform base = at(1.f, 2.f, 3.f, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, kPi / 2.f));
    const Vec3 eye{1.f, 2.f, 13.f}; // looking down -Z at the gizmo

    // World X handle: drag from x=1.5 to x=4.0 on the axis line -> +2.5 along world X only.
    GizmoTransform out{};
    expectTrue(fuse::editor::dragTransformFromRays(base, GizmoMode::Translate, GizmoAxis::X, GizmoSpace::World,
                                                   rayThrough(eye, {1.5f, 2.f, 3.f}),
                                                   rayThrough(eye, {4.f, 2.f, 3.f}), {}, out),
               "world translate drag resolves");
    expectNear(out.posX, 3.5f, 1e-4f, "world X drag moves +2.5 on X");
    expectNear(out.posY, 2.f, 1e-4f, "world X drag leaves Y");
    expectNear(out.posZ, 3.f, 1e-4f, "world X drag leaves Z");

    // Local X handle of an entity yawed 90deg about Z points along world +Y.
    expectTrue(fuse::editor::dragTransformFromRays(base, GizmoMode::Translate, GizmoAxis::X, GizmoSpace::Local,
                                                   rayThrough(eye, {1.f, 2.5f, 3.f}),
                                                   rayThrough(eye, {1.f, 4.f, 3.f}), {}, out),
               "local translate drag resolves");
    expectNear(out.posX, 1.f, 1e-4f, "local X drag does not move along world X");
    expectNear(out.posY, 3.5f, 1e-4f, "local X drag moves +1.5 along rotated axis (world Y)");

    // applyTranslateDelta: local delta must use the full rotated axis, not one world component.
    const GizmoTransform tilted = at(0.f, 0.f, 0.f, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, kPi / 4.f));
    const GizmoTransform moved =
        fuse::editor::applyTranslateDelta(tilted, GizmoAxis::X, {2.f, 0.f, 0.f}, GizmoSpace::Local);
    expectNear(moved.posX, std::sqrt(2.f), 1e-4f, "local delta X component along 45deg axis");
    expectNear(moved.posY, std::sqrt(2.f), 1e-4f, "local delta Y component along 45deg axis");
    const GizmoTransform movedWorld =
        fuse::editor::applyTranslateDelta(tilted, GizmoAxis::Y, {5.f, -3.f, 7.f}, GizmoSpace::World);
    expectNear(movedWorld.posX, 0.f, 1e-6f, "world Y handle ignores X component");
    expectNear(movedWorld.posY, -3.f, 1e-6f, "world Y handle applies Y component");
    expectNear(movedWorld.posZ, 0.f, 1e-6f, "world Y handle ignores Z component");

    // A ray parallel to the axis has no stable drag point.
    expectTrue(!fuse::editor::dragTransformFromRays(base, GizmoMode::Translate, GizmoAxis::Z, GizmoSpace::World,
                                                    GizmoRay{{1.f, 2.f, 10.f}, {0.f, 0.f, -1.f}},
                                                    GizmoRay{{1.f, 2.f, 10.f}, {0.f, 0.f, -1.f}}, {}, out),
               "drag along a view-parallel axis is rejected");
}

// Gate: rotation gizmo produces the correct quaternion from a drag — no gimbal lock in world space.
void testRotateQuaternion() {
    const GizmoTransform base = at(0.f, 0.f, 0.f);
    const Vec3 eye{0.f, 0.f, 10.f};
    GizmoTransform out{};

    // Sweep on the Z ring from +X to +Y: +90deg about Z.
    expectTrue(fuse::editor::dragTransformFromRays(base, GizmoMode::Rotate, GizmoAxis::Z, GizmoSpace::World,
                                                   rayThrough(eye, {1.f, 0.f, 0.f}),
                                                   rayThrough(eye, {0.f, 1.f, 0.f}), {}, out),
               "rotate drag resolves");
    const Quat expected = fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, kPi / 2.f);
    expectTrue(quatNear(fuse::editor::gizmoRotation(out), expected, 1e-5f), "Z ring sweep gives +90deg about Z");
    const Vec3 xAxis = fuse::editor::gizmoRotation(out).rotate({1.f, 0.f, 0.f});
    expectNear(xAxis.y, 1.f, 1e-5f, "rotated X axis points along +Y");
    expectNear(fuse::editor::gizmoRotation(out).length(), 1.f, 1e-6f, "result is unit length");

    // Gimbal-lock scenario: pitch 90deg about X, then a world-Y rotation must still be a pure
    // world-Y rotation (Euler-based code would couple it into roll).
    const Quat pitched = fuse::math::fromAxisAngle({1.f, 0.f, 0.f}, kPi / 2.f);
    const GizmoTransform locked = at(0.f, 0.f, 0.f, pitched);
    const GizmoTransform worldYaw =
        fuse::editor::applyRotateDelta(locked, GizmoAxis::Y, kPi / 3.f, GizmoSpace::World);
    const Quat expectedWorld = fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, kPi / 3.f) * pitched;
    expectTrue(quatNear(fuse::editor::gizmoRotation(worldYaw), expectedWorld, 1e-5f),
               "world-space rotate pre-multiplies (rotation about fixed world Y)");
    const GizmoTransform localYaw =
        fuse::editor::applyRotateDelta(locked, GizmoAxis::Y, kPi / 3.f, GizmoSpace::Local);
    const Quat expectedLocal = pitched * fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, kPi / 3.f);
    expectTrue(quatNear(fuse::editor::gizmoRotation(localYaw), expectedLocal, 1e-5f),
               "local-space rotate post-multiplies (rotation about the entity's own Y)");
    expectTrue(!quatNear(fuse::editor::gizmoRotation(worldYaw), fuse::editor::gizmoRotation(localYaw), 1e-3f),
               "world and local rotation differ for a pitched entity");

    // Ray drag on a local ring of the pitched entity rotates about its own (world-space) axis.
    const Vec3 localY = pitched.rotate({0.f, 1.f, 0.f}); // = world +Z
    expectNear(localY.z, 1.f, 1e-5f, "pitched local Y is world Z");
    expectTrue(fuse::editor::dragTransformFromRays(locked, GizmoMode::Rotate, GizmoAxis::Y, GizmoSpace::Local,
                                                   rayThrough(eye, {1.f, 0.f, 0.f}),
                                                   rayThrough(eye, {0.f, 1.f, 0.f}), {}, out),
               "local ring drag resolves");
    expectTrue(quatNear(fuse::editor::gizmoRotation(out),
                        fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, kPi / 2.f) * pitched, 1e-5f),
               "local ring sweep rotates about the entity axis");

    // Snapped rotation: a 50deg sweep with 15deg steps gives 45deg.
    fuse::editor::GizmoSnapSettings snap{};
    snap.rotateSnap = true;
    snap.angleStepDegrees = 15.f;
    const float a = 50.f * kPi / 180.f;
    fuse::editor::dragTransformFromRays(base, GizmoMode::Rotate, GizmoAxis::Z, GizmoSpace::World,
                                        rayThrough(eye, {1.f, 0.f, 0.f}),
                                        rayThrough(eye, {std::cos(a), std::sin(a), 0.f}), snap, out);
    expectTrue(quatNear(fuse::editor::gizmoRotation(out),
                        fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, kPi / 4.f), 1e-5f),
               "rotate snap quantises the swept angle to 15deg steps");

    // snapTransform must treat rot* as a quaternion (Euler snap), keeping it unit length.
    const GizmoTransform odd = at(0.f, 0.f, 0.f, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 20.f * kPi / 180.f));
    const GizmoTransform snappedOdd = fuse::editor::snapTransform(odd, GizmoMode::Rotate, snap);
    expectNear(fuse::editor::gizmoRotation(snappedOdd).length(), 1.f, 1e-5f, "rotation snap keeps a unit quaternion");
    expectTrue(quatNear(fuse::editor::gizmoRotation(snappedOdd),
                        fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 15.f * kPi / 180.f), 1e-5f),
               "20deg yaw snaps to 15deg");
    const Vec3 euler{0.3f, -0.7f, 1.1f};
    const Vec3 roundTrip = fuse::editor::quatToEulerRadians(fuse::editor::eulerRadiansToQuat(euler));
    expectNear(roundTrip.x, euler.x, 1e-5f, "euler round trip x");
    expectNear(roundTrip.y, euler.y, 1e-5f, "euler round trip y");
    expectNear(roundTrip.z, euler.z, 1e-5f, "euler round trip z");
}

// Gate: scale gizmo scales uniformly on the XYZ handle, per-axis on individual handles.
void testScaleUniformAndPerAxis() {
    GizmoTransform base = at(0.f, 0.f, 0.f);
    base.scaleX = 2.f;
    base.scaleY = 1.f;
    base.scaleZ = 0.5f;
    const Vec3 eye{0.f, 0.f, 10.f};
    GizmoTransform out{};

    expectTrue(fuse::editor::dragTransformFromRays(base, GizmoMode::Scale, GizmoAxis::X, GizmoSpace::World,
                                                   rayThrough(eye, {1.f, 0.f, 0.f}),
                                                   rayThrough(eye, {1.5f, 0.f, 0.f}), {}, out),
               "X scale drag resolves");
    expectNear(out.scaleX, 3.f, 1e-4f, "X handle scales X by drag ratio 1.5");
    expectNear(out.scaleY, 1.f, 1e-6f, "X handle leaves Y scale");
    expectNear(out.scaleZ, 0.5f, 1e-6f, "X handle leaves Z scale");

    expectTrue(fuse::editor::dragTransformFromRays(base, GizmoMode::Scale, GizmoAxis::Uniform, GizmoSpace::World,
                                                   rayThrough(eye, {0.5f, 0.5f, 0.f}),
                                                   rayThrough(eye, {1.f, 1.f, 0.f}), {}, out),
               "uniform scale drag resolves");
    expectNear(out.scaleX, 4.f, 1e-4f, "uniform handle doubles X");
    expectNear(out.scaleY, 2.f, 1e-4f, "uniform handle doubles Y");
    expectNear(out.scaleZ, 1.f, 1e-4f, "uniform handle doubles Z (ratios preserved)");

    // Scale never collapses to zero / negative.
    fuse::editor::dragTransformFromRays(base, GizmoMode::Scale, GizmoAxis::Y, GizmoSpace::World,
                                        rayThrough(eye, {0.f, 1.f, 0.f}), rayThrough(eye, {0.f, -1.f, 0.f}),
                                        {}, out);
    expectTrue(out.scaleY >= 0.01f, "scale clamps at the minimum");
}

// Gate: axis highlight fires on hover (pick), deactivates on mouse release; analytic picking.
void testAxisPickingAndRelease() {
    const GizmoTransform base = at(0.f, 0.f, 0.f);
    const Vec3 eye{0.3f, 0.4f, 10.f};

    fuse::editor::GizmoSystem gizmo;
    expectTrue(gizmo.pickAxis(rayThrough(eye, {0.7f, 0.f, 0.f}), base) == GizmoAxis::X, "hover X arrow picks X");
    expectTrue(gizmo.pickAxis(rayThrough(eye, {0.f, 0.6f, 0.f}), base) == GizmoAxis::Y, "hover Y arrow picks Y");
    expectTrue(gizmo.pickAxis(rayThrough(eye, {0.6f, 0.6f, 0.f}), base) == GizmoAxis::None,
               "empty space between arrows picks nothing");
    expectTrue(gizmo.pickAxis(rayThrough(eye, {1.5f, 0.f, 0.f}), base) == GizmoAxis::None,
               "beyond the arrow tip picks nothing");

    // An axis behind the camera must not be picked.
    const GizmoRay away{{0.5f, 0.f, 1.f}, {0.f, 0.f, 1.f}}; // starts in front of the X arrow, looks away
    expectTrue(gizmo.pickAxis(away, base) == GizmoAxis::None, "segment behind the ray origin is not picked");

    // Overlap near the origin: all three arrows are within the pick radius; the Z arrow tip is
    // nearest along the ray, so it wins.
    const GizmoRay overlap{{0.05f, 0.05f, 10.f}, {0.f, 0.f, -1.f}};
    expectTrue(gizmo.pickAxis(overlap, base) == GizmoAxis::Z, "front-most handle wins on overlap");

    gizmo.setMode(GizmoMode::Scale);
    expectTrue(gizmo.pickAxis(rayThrough(eye, {0.f, 0.f, 0.f}), base) == GizmoAxis::Uniform,
               "scale mode centre picks the uniform handle");
    gizmo.setMode(GizmoMode::Rotate);
    expectTrue(gizmo.pickAxis(rayThrough(eye, {0.f, 0.f, 0.f}), base) == GizmoAxis::None,
               "rotate mode has no arrows: the centre picks nothing");
    expectTrue(gizmo.pickAxis(rayThrough(eye, {0.6f, 0.8f, 0.f}), base) == GizmoAxis::Z,
               "rotate mode picks the Z ring on its radius");

    gizmo.setMode(GizmoMode::Translate);
    fuse::editor::GizmoResult begin = gizmo.beginDrag(rayThrough(eye, {0.7f, 0.f, 0.f}), base);
    expectTrue(begin.active && begin.axis == GizmoAxis::X && gizmo.isDragging(), "press on X starts drag");
    const fuse::editor::GizmoResult update = gizmo.updateDrag(rayThrough(eye, {1.7f, 0.f, 0.f}));
    expectTrue(update.changed && update.axis == GizmoAxis::X, "drag update keeps X active");
    expectNear(update.transform.posX, 1.f, 1e-4f, "ray drag moves by the axis distance");
    const fuse::editor::GizmoResult release = gizmo.endDrag();
    expectTrue(!release.active && !gizmo.isDragging(), "release deactivates the axis");
    expectNear(release.transform.posX, 1.f, 1e-4f, "release commits the dragged transform");

    // Screen-space (legacy hit) drags measure from the press point.
    fuse::editor::GizmoSystem screen;
    fuse::editor::GizmoHitTest hit{};
    hit.viewportWidth = 100.f;
    hit.viewportHeight = 100.f;
    hit.screenX = 10.f;
    hit.screenY = 50.f;
    screen.beginDrag(hit, base);
    hit.screenX = 20.f;
    const fuse::editor::GizmoResult first = screen.updateDrag(hit);
    const fuse::editor::GizmoResult second = screen.updateDrag(hit);
    expectTrue(first.transform.posX != base.posX, "screen drag actually moves the entity");
    expectNear(second.transform.posX, first.transform.posX, 1e-6f, "repeating the same hit is idempotent");
}

// Gate: snap-to-grid quantises position to the snap_translate increment.
void testTranslateSnap() {
    fuse::editor::GizmoSnapSettings snap{};
    snap.translateSnap = true;
    snap.gridSize = 0.25f;
    const GizmoTransform base = at(0.1f, 0.f, 0.f);
    const Vec3 eye{0.f, 0.f, 10.f};
    GizmoTransform out{};
    fuse::editor::dragTransformFromRays(base, GizmoMode::Translate, GizmoAxis::X, GizmoSpace::World,
                                        rayThrough(eye, {0.5f, 0.f, 0.f}), rayThrough(eye, {1.33f, 0.f, 0.f}),
                                        snap, out);
    const float steps = out.posX / snap.gridSize;
    expectNear(steps, std::round(steps), 1e-4f, "snapped position lies on the 0.25 grid");
    expectNear(out.posX, 1.f, 1e-4f, "0.93 raw -> 1.00 snapped");

    fuse::editor::GizmoSystem gizmo;
    gizmo.setSnapSettings(snap);
    gizmo.beginDrag(rayThrough(eye, {0.5f, 0.f, 0.f}), base);
    const fuse::editor::GizmoResult update = gizmo.updateDrag(rayThrough(eye, {0.81f, 0.f, 0.f}));
    expectNear(update.transform.posX, 0.5f, 1e-4f, "GizmoSystem ray drag snaps too (0.41 -> 0.5)");
    gizmo.endDrag();

    // Local-space snapping quantises the displacement along the rotated axis.
    const GizmoTransform yawed = at(0.1f, 0.f, 0.f, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, kPi / 2.f));
    fuse::editor::dragTransformFromRays(yawed, GizmoMode::Translate, GizmoAxis::X, GizmoSpace::Local,
                                        rayThrough(eye, {0.1f, 0.f, 0.f}), rayThrough(eye, {0.1f, 0.6f, 0.f}),
                                        snap, out);
    expectNear(out.posY, 0.5f, 1e-4f, "local displacement 0.6 snaps to 0.5");
    expectNear(out.posX, 0.1f, 1e-5f, "local snap stays on the rotated axis");
}

// Gate: gizmo keeps the same on-screen size at all camera distances.
void testConstantScreenSize() {
    const float fov = 60.f * kPi / 180.f;
    const float height = 1080.f;
    const Vec3 gizmoPos{3.f, -1.f, 2.f};
    for (float distance : {0.5f, 1.f, 10.f, 250.f, 5000.f}) {
        const Vec3 camera{gizmoPos.x, gizmoPos.y, gizmoPos.z + distance};
        const float world = fuse::editor::gizmoWorldScale(camera, gizmoPos, fov, height,
                                                          fuse::editor::GizmoSystem::kScreenSize);
        const float projectedPx = world / (2.f * distance * std::tan(fov * 0.5f)) * height;
        expectNear(projectedPx, fuse::editor::GizmoSystem::kScreenSize, 1e-2f,
                   "gizmo projects to kScreenSize pixels at every distance");
    }

    // Picking scales with the world size so a far gizmo is still hittable on its (large) arrow.
    fuse::editor::GizmoSystem gizmo;
    const GizmoTransform far = at(0.f, 0.f, -200.f);
    const Vec3 eye{0.f, 0.f, 0.f};
    const float scale = fuse::editor::gizmoWorldScale(eye, fuse::editor::gizmoPosition(far), fov, height,
                                                      fuse::editor::GizmoSystem::kScreenSize);
    const GizmoRay toArrow = rayThrough(eye, {0.6f * scale, 0.f, -200.f});
    expectTrue(gizmo.pickAxis(toArrow, far) == GizmoAxis::None, "unit-size gizmo misses at distance");
    gizmo.setWorldScale(scale);
    expectTrue(gizmo.pickAxis(toArrow, far) == GizmoAxis::X, "world-scaled gizmo picks at distance");
}

// Gizmo drags go through the CommandStack as exact, coalescing `transform.trs` edits.
void testGizmoUndoThroughHost() {
    fuse::editor::EditorHost host;
    fuse::ecs::Registry& registry = host.editorScene().registry();
    const fuse::ecs::EntityID entity = registry.create();
    fuse::ecs::Transform transform{};
    transform.position = {0.1f, 0.2f, 0.3f, 1.f};
    registry.add(entity, transform);

    fuse::editor::GizmoSystem gizmo;
    gizmo.setCommandStack(&host.commandStack());
    gizmo.setEditorState(&host.editorState());
    gizmo.setTarget(fuse::Handle<fuse::Object>(entity.index, entity.generation));

    const Vec3 eye{0.1f, 0.2f, 10.f};
    GizmoTransform current = at(0.1f, 0.2f, 0.3f);
    for (int drag = 0; drag < 2; ++drag) {
        gizmo.beginDrag(rayThrough(eye, {current.posX + 0.5f, 0.2f, 0.3f}), current);
        gizmo.updateDrag(rayThrough(eye, {current.posX + 1.25f, 0.2f, 0.3f}));
        current = gizmo.endDrag().transform;
    }
    expectTrue(host.commandStack().undoDepth() == 1u, "two consecutive drags of one entity = one undo step");
    const fuse::editor::EditorCommand* top = host.commandStack().peekUndo();
    GizmoTransform before{};
    GizmoTransform after{};
    expectTrue(top != nullptr && fuse::editor::parseGizmoTransform(top->propertyValueBefore, before) &&
                   fuse::editor::parseGizmoTransform(top->propertyValue, after),
               "command carries parseable before/after TRS");
    expectTrue(before.posX == 0.1f && after.posX == current.posX, "coalesced step spans first-start..last-end");

    host.undoPropertyEdit(); // applies pending forward edit, then the inverse
    const fuse::ecs::Transform* live = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(live->position.x == 0.1f && live->position.y == 0.2f && live->position.z == 0.3f,
               "undo restores the exact pre-drag position");
    host.redoPropertyEdit();
    expectTrue(live == registry.get<fuse::ecs::Transform>(entity) && live->position.x == current.posX,
               "redo re-applies the exact post-drag position");

    GizmoTransform parsed{};
    expectTrue(!fuse::editor::parseGizmoTransform("1,2,3", parsed) &&
                   !fuse::editor::parseGizmoTransform("1,2,3,4,5,6,7,8,9,10,11", parsed),
               "malformed TRS text rejected");
}

} // namespace

int main() {
    fuse::core::initialize();

    testTranslateWorldAndLocal();
    testRotateQuaternion();
    testScaleUniformAndPerAxis();
    testAxisPickingAndRelease();
    testTranslateSnap();
    testConstantScreenSize();
    testGizmoUndoThroughHost();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_gizmo_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_gizmo_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
