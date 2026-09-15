#include <fuse/object.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

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

void test2dTo3dInheritance() {
    fuse::SceneObject2D sprite("sprite");
    fuse::SceneObject3D mesh("mesh");

    sprite.setPosition(10.f, 20.f);
    sprite.setLayer(3);
    mesh.setPosition(1.f, 2.f);
    mesh.setZ(5.f);

    expectTrue(sprite.typeName() != nullptr, "2D type name available");
    expectTrue(mesh.typeName() != nullptr, "3D type name available");
    expectTrue(mesh.y() == 2.f, "3D object inherits 2D transform");
    expectTrue(mesh.z() == 5.f, "3D object stores depth");
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

} // namespace

int main() {
    test2dTo3dInheritance();
    testMixedHierarchy();

    if (g_failures == 0) {
        std::printf("fuse scene hierarchy tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse scene hierarchy tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
