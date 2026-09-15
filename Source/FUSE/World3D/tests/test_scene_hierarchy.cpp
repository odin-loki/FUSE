#include <fuse/legacy/t2d/scene_adapter.hpp>
#include <fuse/legacy/t3d/scene_adapter.hpp>
#include <fuse/object.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/scene_snapshot.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/scene_snapshot.hpp>

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

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void test2dTo3dInheritance() {
    fuse::SceneObject2D sprite("sprite");
    fuse::SceneObject3D mesh("mesh");

    sprite.setPosition(10.f, 20.f);
    sprite.setLayer(3);
    mesh.setPosition(1.f, 2.f);
    mesh.setZ(5.f);

    expectTrue(sprite.typeName() != nullptr, "2D type name available");
    expectTrue(mesh.typeName() != nullptr, "3D type name available");
    expectNear(mesh.y(), 2.f, 1e-4f, "3D object inherits 2D transform");
    expectNear(mesh.z(), 5.f, 1e-4f, "3D object stores depth");
}

void testMixedHierarchy() {
    fuse::SceneObject2D root("root");
    fuse::SceneObject3D child("child3d");
    fuse::SceneObject2D sibling("sibling2d");

    root.addChild(&child);
    root.addChild(&sibling);

    expectTrue(root.children().size() == 2u, "2D root holds 2D and 3D children");
    expectTrue(child.parent() == &root, "3D child parent is 2D node");
}

void testObjectReparent() {
    fuse::SceneObject2D root("root");
    fuse::SceneObject2D group("group");
    fuse::SceneObject2D child("child");

    root.addChild(&group);
    root.addChild(&child);
    expectTrue(child.parent() == &root, "child starts under root");

    child.reparent(&group);
    expectTrue(child.parent() == &group, "reparent moves child under group");
    expectTrue(root.children().size() == 1u, "root no longer lists reparented child");
    expectTrue(group.children().size() == 1u, "group tracks reparented child");

    child.reparent(nullptr);
    expectTrue(child.parent() == nullptr, "reparent to nullptr unparents");
    expectTrue(group.children().empty(), "group releases unparented child");
}

void testLocalWorldTransformStubs() {
    fuse::SceneObject2D root("root");
    fuse::SceneObject3D child("child");
    root.setPosition(10.f, 20.f);
    child.setPosition(1.f, 2.f);
    child.setZ(3.f);
    root.addChild(&child);

    const fuse::LocalTransform2D childLocal = child.localTransform();
    expectNear(childLocal.x, 1.f, 1e-4f, "2D local x");
    expectNear(childLocal.y, 2.f, 1e-4f, "2D local y");

    const fuse::WorldTransform2D childWorld = child.worldTransform();
    expectNear(childWorld.x, 11.f, 1e-4f, "2D world x accumulates parent");
    expectNear(childWorld.y, 22.f, 1e-4f, "2D world y accumulates parent");

    const fuse::LocalTransform3D childLocal3d = child.localTransform3D();
    expectNear(childLocal3d.z, 3.f, 1e-4f, "3D local z");

    const fuse::WorldTransform3D childWorld3d = child.worldTransform3D();
    expectNear(childWorld3d.x, 11.f, 1e-4f, "3D world x");
    expectNear(childWorld3d.y, 22.f, 1e-4f, "3D world y");
    expectNear(childWorld3d.z, 3.f, 1e-4f, "3D world z stub");
}

void testSnapshotSoAFill2D() {
    fuse::SceneObject2D root("root");
    fuse::SceneObject2D child("child");
    root.setPosition(5.f, 0.f);
    child.setPosition(2.f, 3.f);
    child.setLayer(7);
    root.addChild(&child);

    fuse::world2d::SceneSnapshot2D snapshot;
    fuse::world2d::SceneTransformSoA2D soa;
    fuse::world2d::fillSnapshotSoA(root, snapshot, soa);

    expectTrue(snapshot.sprites().size() == 2u, "2D snapshot includes root and child");
    expectTrue(soa.object.size() == 2u, "2D SoA parallel to snapshot");
    expectNear(soa.worldX[1], 7.f, 1e-4f, "2D SoA child world x");
    expectNear(soa.worldY[1], 3.f, 1e-4f, "2D SoA child world y");
    expectTrue(soa.layer[1] == 7u, "2D SoA preserves layer");
    expectNear(snapshot.sprites()[1].x, 7.f, 1e-4f, "2D snapshot cmd uses world x");
}

void testSnapshotSoAFill3D() {
    fuse::SceneObject3D root("root");
    fuse::SceneObject3D child("child");
    root.setPosition(1.f, 0.f);
    root.setZ(10.f);
    child.setPosition(0.f, 2.f);
    child.setZ(4.f);
    root.addChild(&child);

    fuse::world3d::SceneSnapshot3D snapshot;
    fuse::world3d::SceneTransformSoA3D soa;
    fuse::world3d::fillSnapshotSoA(root, snapshot, soa);

    expectTrue(snapshot.objects().size() == 2u, "3D snapshot includes hierarchy");
    expectTrue(soa.worldZ.size() == 2u, "3D SoA z column filled");
    expectNear(soa.worldX[1], 1.f, 1e-4f, "3D SoA child world x");
    expectNear(soa.worldY[1], 2.f, 1e-4f, "3D SoA child world y");
    expectNear(soa.worldZ[1], 14.f, 1e-4f, "3D SoA child world z accumulates parent");
}

void testLegacyAdapterRoundTrip() {
    fuse::legacy::t2d::LegacySceneObjectStub t2dLegacy{};
    t2dLegacy.legacyId = 42;
    t2dLegacy.name = "legacy_sprite";
    t2dLegacy.x = 8.f;
    t2dLegacy.y = 9.f;
    t2dLegacy.layer = 2;

    fuse::SceneObject2D imported2d("placeholder");
    expectTrue(fuse::legacy::t2d::importSceneObject(t2dLegacy, imported2d), "T2D adapter import stub");
    fuse::legacy::t2d::LegacySceneObjectStub exported2d{};
    exported2d.legacyId = t2dLegacy.legacyId;
    expectTrue(fuse::legacy::t2d::exportSceneObject(imported2d, exported2d), "T2D adapter export stub");
    expectNear(exported2d.x, 8.f, 1e-4f, "T2D round-trip x");
    expectNear(exported2d.y, 9.f, 1e-4f, "T2D round-trip y");
    expectTrue(exported2d.layer == 2, "T2D round-trip layer");

    fuse::legacy::t3d::LegacySceneObjectStub t3dLegacy{};
    t3dLegacy.legacyId = 7;
    t3dLegacy.name = "legacy_mesh";
    t3dLegacy.x = 1.f;
    t3dLegacy.y = 2.f;
    t3dLegacy.z = 3.f;

    fuse::SceneObject3D imported3d("placeholder");
    expectTrue(fuse::legacy::t3d::importSceneObject(t3dLegacy, imported3d), "T3D adapter import stub");
    fuse::legacy::t3d::LegacySceneObjectStub exported3d{};
    exported3d.legacyId = t3dLegacy.legacyId;
    expectTrue(fuse::legacy::t3d::exportSceneObject(imported3d, exported3d), "T3D adapter export stub");
    expectNear(exported3d.z, 3.f, 1e-4f, "T3D round-trip z");
}

} // namespace

int main() {
    test2dTo3dInheritance();
    testMixedHierarchy();
    testObjectReparent();
    testLocalWorldTransformStubs();
    testSnapshotSoAFill2D();
    testSnapshotSoAFill3D();
    testLegacyAdapterRoundTrip();

    if (g_failures == 0) {
        std::printf("fuse scene hierarchy tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse scene hierarchy tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
