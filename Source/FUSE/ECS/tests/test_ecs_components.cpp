#include <fuse/ecs/comp_align.hpp>
#include <fuse/ecs/component_bytes.hpp>
#include <fuse/ecs/component_name.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testCompAlignOk() {
    expectTrue(fuse::ecs::comp_align_ok(1), "alignment 1 is valid");
    expectTrue(fuse::ecs::comp_align_ok(8), "alignment 8 is valid");
    expectTrue(fuse::ecs::comp_align_ok(64), "alignment 64 is valid");
    expectTrue(!fuse::ecs::comp_align_ok(0), "alignment 0 is invalid");
    expectTrue(!fuse::ecs::comp_align_ok(3), "alignment 3 is invalid");
    expectTrue(!fuse::ecs::comp_align_ok(128), "alignment 128 is invalid");
}

void testComponentBytesOk() {
    expectTrue(fuse::ecs::component_bytes_ok(1), "one byte is a valid component size");
    expectTrue(fuse::ecs::component_bytes_ok(4096), "4096 bytes is a valid component size");
    expectTrue(!fuse::ecs::component_bytes_ok(0), "zero bytes is invalid");
    expectTrue(!fuse::ecs::component_bytes_ok(4097), "4097 bytes is invalid");
}

void testComponentNameOk() {
    expectTrue(fuse::ecs::component_name_ok("Transform"), "Transform is a valid component name");
    expectTrue(!fuse::ecs::component_name_ok("transform"), "lowercase first letter is invalid");
    expectTrue(fuse::ecs::component_name_ok("T"), "single uppercase letter is valid");
    expectTrue(!fuse::ecs::component_name_ok(""), "empty name is invalid");
    expectTrue(!fuse::ecs::component_name_ok("T_pose"), "underscore is invalid");
    expectTrue(fuse::ecs::component_name_ok("Mesh2"), "digits are valid after the first letter");
    expectTrue(fuse::ecs::component_name_ok("A1234567890123456789012345678901"),
               "32-character name is valid");
    expectTrue(!fuse::ecs::component_name_ok("A12345678901234567890123456789012"),
               "33-character name is invalid");
}

void testComponentNames() {
    expectTrue(std::strcmp(fuse::ecs::componentName<fuse::ecs::Transform>(), "Transform") == 0,
               "Transform component name");
    expectTrue(std::strcmp(fuse::ecs::componentName<fuse::ecs::Mesh>(), "Mesh") == 0, "Mesh component name");
    expectTrue(std::strcmp(fuse::ecs::componentName<fuse::ecs::SDFObject>(), "SDFObject") == 0,
               "SDFObject component name");
    expectTrue(std::strcmp(fuse::ecs::componentName<fuse::ecs::TagPlayer>(), "TagPlayer") == 0,
               "TagPlayer component name");
}

void testTransformDefaults() {
    fuse::ecs::Transform transform{};
    expectTrue(transform.parent == fuse::ecs::EntityID::null(), "default parent is null");
    expectTrue(transform.dirty, "transform starts dirty");
    expectTrue(transform.scale.x == 1.f, "default unit scale");
}

void testPhysicsAndCameraDefaults() {
    fuse::ecs::RigidBody body{};
    expectTrue(body.mass == 1.f, "default mass");
    expectTrue(!body.is_static, "dynamic by default");

    fuse::ecs::Camera camera{};
    expectTrue(camera.fov_deg == 75.f, "default fov");
    expectTrue(!camera.is_active, "camera inactive by default");
}

void testMeshAndSdfHandles() {
    fuse::ecs::Mesh mesh{};
    expectTrue(!mesh.vertex_buffer.isValid(), "mesh vertex handle starts invalid");
    expectTrue(mesh.visible, "mesh visible by default");

    fuse::ecs::SDFObject sdf{};
    expectTrue(sdf.type == fuse::ecs::SDFPrimitive::Sphere, "default sdf primitive");
    expectTrue(sdf.blend_alpha == fuse::ecs::kSdfDefaultBlendAlpha, "default blend alpha");
}

void testLightsAndTagsInRegistry() {
    fuse::ecs::Registry reg;
    reg.init(32);

    const fuse::ecs::EntityID light = reg.create();
    reg.add<fuse::ecs::Transform>(light);
    reg.add<fuse::ecs::PointLight>(light);
    reg.add<fuse::ecs::TagStatic>(light);

    expectTrue(reg.has<fuse::ecs::PointLight>(light), "point light stored");
    expectTrue(reg.has<fuse::ecs::TagStatic>(light), "tag marker stored");
    expectTrue(reg.get<fuse::ecs::PointLight>(light)->radius == 10.f, "point light default radius");
}

} // namespace

int main() {
    testCompAlignOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_ecs_components_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testComponentBytesOk();
    testComponentNameOk();
    testComponentNames();
    testTransformDefaults();
    testPhysicsAndCameraDefaults();
    testMeshAndSdfHandles();
    testLightsAndTagsInRegistry();

    if (g_failures == 0) {
        std::printf("fuse_ecs_components_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_components_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
