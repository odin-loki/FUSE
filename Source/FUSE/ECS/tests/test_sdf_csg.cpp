// SDF CSG op on ecs::SDFObject: primitive distances, union / subtract / intersect / smooth union,
// fold order, roughness and conservative bounds, checked with the CPU reference evaluator.
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/sdf_csg.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace fuse::ecs;
using fuse::u32;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool near(f32 a, f32 b, f32 eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

vec3 v(f32 x, f32 y, f32 z) {
    return {x, y, z, 0.f};
}

EntityID addSdf(Registry& reg, SDFPrimitive type, vec3 params, vec3 position, SDFCsgOp op, u32 order,
                f32 blend = 0.f, u32 material = 0u) {
    const EntityID id = reg.create();
    Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    reg.add(id, t);
    SDFObject sdf{};
    sdf.type = type;
    sdf.params = params;
    sdf.op = op;
    sdf.csg_order = order;
    sdf.blend_radius = blend;
    sdf.material_id = material;
    reg.add(id, sdf);
    return id;
}

void testPrimitiveDistances() {
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Sphere, v(1.f, 0, 0), v(3.f, 0, 0)), 2.f), "sphere outside");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Sphere, v(1.f, 0, 0), v(0, 0, 0)), -1.f), "sphere centre");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Box, v(1, 2, 3), v(3.f, 0, 0)), 2.f), "box face distance");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Box, v(1, 2, 3), v(0, 0, 0)), -1.f), "box inside = -min half extent");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Box, v(1, 1, 1), v(2, 2, 1)), std::sqrt(2.f)), "box edge distance");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Capsule, v(0.5f, 1.f, 0), v(0, 3.f, 0)), 1.5f), "capsule cap");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Capsule, v(0.5f, 1.f, 0), v(2.f, 0.5f, 0)), 1.5f), "capsule side");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Torus, v(2.f, 0.5f, 0), v(2.f, 0, 0)), -0.5f), "torus tube centre");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Torus, v(2.f, 0.5f, 0), v(0, 0, 0)), 1.5f), "torus hole");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Cylinder, v(1.f, 2.f, 0), v(0, 5.f, 0)), 3.f), "cylinder cap");
    expectTrue(near(sdf_primitive_distance(SDFPrimitive::Cylinder, v(1.f, 2.f, 0), v(0, 0, 4.f)), 3.f), "cylinder side");

    // Transform: position + rotation + uniform scale.
    SDFObject box{};
    box.type = SDFPrimitive::Box;
    box.params = v(2.f, 0.5f, 0.5f);
    Transform t{};
    t.position = {10.f, 0.f, 0.f, 1.f};
    t.rotation = {0.f, 0.f, 0.70710678f, 0.70710678f}; // 90 deg about Z: long axis now along Y
    t.scale = {2.f, 2.f, 2.f, 0.f};
    expectTrue(near(sdf_object_distance(box, t, v(10.f, 5.f, 0.f)), 1.f, 1e-4f), "rotated+scaled box along Y");
    expectTrue(near(sdf_object_distance(box, t, v(13.f, 0.f, 0.f)), 2.f, 1e-4f), "rotated+scaled box along X");
}

void testCsgOps() {
    const f32 a = 0.5f;
    const f32 b = -0.25f;
    expectTrue(sdf_csg_apply(SDFCsgOp::Union, a, b, 0.f) == -0.25f, "union = min");
    expectTrue(sdf_csg_apply(SDFCsgOp::Subtract, a, b, 0.f) == 0.5f, "subtract = max(scene, -d)");
    expectTrue(sdf_csg_apply(SDFCsgOp::Subtract, -1.f, -0.25f, 0.f) == 0.25f, "subtract carves inside");
    expectTrue(sdf_csg_apply(SDFCsgOp::Intersect, a, b, 0.f) == 0.5f, "intersect = max");
    expectTrue(sdf_csg_apply(SDFCsgOp::SmoothUnion, a, b, 0.f) == -0.25f, "smooth union k=0 is hard union");
    expectTrue(sdf_csg_apply(SDFCsgOp::SmoothUnion, 2.f, -1.f, 0.5f) == -1.f, "smooth union far apart = min");
    const f32 blended = sdf_csg_apply(SDFCsgOp::SmoothUnion, 0.1f, 0.1f, 0.4f);
    expectTrue(near(blended, 0.1f - 0.1f), "smooth union bulges k/4 at equal distances");
    expectTrue(sdf_csg_apply(SDFCsgOp::SmoothUnion, kSdfEmptyDistance, 0.3f, 1.f) == 0.3f, "smooth union on empty scene");

    // Two spheres (r=1) at x=0 and x=1.5.
    Registry reg;
    reg.init(64);
    addSdf(reg, SDFPrimitive::Sphere, v(1.f, 0, 0), v(0, 0, 0), SDFCsgOp::Union, 0u, 0.f, 1u);
    const EntityID second = addSdf(reg, SDFPrimitive::Sphere, v(1.f, 0, 0), v(1.5f, 0, 0), SDFCsgOp::Union, 1u, 0.f, 2u);
    SdfCsgScene scene;
    scene.build(reg);
    expectTrue(scene.distance(v(-0.5f, 0, 0)) < 0.f && scene.distance(v(2.f, 0, 0)) < 0.f, "union: inside both");
    expectTrue(scene.sample(v(2.2f, 0, 0)).material_id == 2u, "union: material of the nearer object");

    reg.get<SDFObject>(second)->op = SDFCsgOp::Subtract;
    scene.build(reg);
    expectTrue(scene.distance(v(-0.5f, 0, 0)) < 0.f, "subtract: outside the cutter stays solid");
    expectTrue(scene.distance(v(0.75f, 0, 0)) > 0.f, "subtract: overlap removed (sign flips)");
    expectTrue(scene.distance(v(2.f, 0, 0)) > 0.f, "subtract: cutter adds no volume");
    expectTrue(near(scene.distance(v(0.5f, 0, 0)), 0.f), "subtract: new surface at the cutter boundary");
    expectTrue(scene.sample(v(0.45f, 0, 0)).entity == second, "subtract: carved wall belongs to the cutter");

    reg.get<SDFObject>(second)->op = SDFCsgOp::Intersect;
    scene.build(reg);
    expectTrue(scene.distance(v(0.75f, 0, 0)) < 0.f, "intersect: overlap kept");
    expectTrue(scene.distance(v(-0.5f, 0, 0)) > 0.f && scene.distance(v(2.f, 0, 0)) > 0.f,
               "intersect: non-overlap removed");

    // Order: a subtract that precedes the union cannot carve it.
    reg.get<SDFObject>(second)->op = SDFCsgOp::Subtract;
    reg.get<SDFObject>(second)->csg_order = 0u;
    reg.each<SDFObject>([&](EntityID id, SDFObject& s) {
        if (id != second) {
            s.csg_order = 5u;
        }
    });
    scene.build(reg);
    expectTrue(scene.distance(v(0.75f, 0, 0)) < 0.f, "csg_order: earlier subtract does not carve later union");
    expectTrue(scene.next_csg_order() == 6u, "next_csg_order = max + 1");
    reg.destroy();
}

void testSmoothUnionContinuity() {
    Registry reg;
    reg.init(16);
    addSdf(reg, SDFPrimitive::Sphere, v(1.f, 0, 0), v(-1.2f, 0, 0), SDFCsgOp::Union, 0u);
    const EntityID blend = addSdf(reg, SDFPrimitive::Sphere, v(1.f, 0, 0), v(1.2f, 0, 0), SDFCsgOp::SmoothUnion, 1u, 1.0f);
    SdfCsgScene smooth;
    smooth.build(reg);
    reg.get<SDFObject>(blend)->op = SDFCsgOp::Union;
    SdfCsgScene hard;
    hard.build(reg);

    // Along the line through the gap, the smooth field is continuous (1-Lipschitz), never above the
    // hard union, fills the gap (negative at the midpoint where hard union is positive) and
    // deviates by at most k/4.
    const f32 h = 0.001f;
    bool lipschitz = true;
    bool below = true;
    f32 maxDev = 0.f;
    for (int i = 0; i <= 4000; ++i) {
        const f32 y = -2.f + static_cast<f32>(i) * h;
        const f32 s0 = smooth.distance(v(0.f, y, 0.f));
        const f32 s1 = smooth.distance(v(0.f, y + h, 0.f));
        const f32 hd = hard.distance(v(0.f, y, 0.f));
        lipschitz = lipschitz && std::fabs(s1 - s0) <= h * 1.001f + 1e-6f;
        below = below && s0 <= hd + 1e-6f;
        maxDev = std::max(maxDev, hd - s0);
    }
    expectTrue(lipschitz, "smooth union field is continuous (|df| <= step)");
    expectTrue(below, "smooth union never above hard union");
    expectTrue(maxDev <= 1.0f * 0.25f + 1e-5f && maxDev > 0.1f, "smooth union bulge bounded by k/4");
    expectTrue(hard.distance(v(0, 0, 0)) > 0.f && smooth.distance(v(0, 0, 0)) < 0.f,
               "smooth union bridges the gap between the spheres");
    expectTrue(near(smooth.distance(v(-3.f, 0, 0)), hard.distance(v(-3.f, 0, 0))), "far from the seam: identical");
    reg.destroy();
}

void testRoughnessAndBounds() {
    SDFObject sphere{};
    sphere.params = v(1.f, 0, 0);
    Transform t{};
    SDFObject rough = sphere;
    rough.roughness = 0.05f;
    f32 maxDelta = 0.f;
    int changed = 0;
    for (int i = 0; i < 200; ++i) {
        const f32 a = static_cast<f32>(i) * 0.0314159f;
        const vec3 p = v(std::cos(a), std::sin(a) * 0.6f, std::sin(a) * 0.8f);
        const f32 delta = sdf_object_distance(rough, t, p) - sdf_object_distance(sphere, t, p);
        maxDelta = std::max(maxDelta, std::fabs(delta));
        changed += std::fabs(delta) > 1e-4f ? 1 : 0;
    }
    expectTrue(maxDelta <= 0.05f + 1e-6f && changed > 100, "roughness displaces the surface within its amplitude");

    // Bounds: every primitive/op is positive (empty) just outside its bounding radius.
    const SDFPrimitive types[] = {SDFPrimitive::Sphere, SDFPrimitive::Box, SDFPrimitive::Capsule,
                                  SDFPrimitive::Torus, SDFPrimitive::Cylinder};
    bool conservative = true;
    for (SDFPrimitive type : types) {
        SDFObject s{};
        s.type = type;
        s.params = v(0.7f, 0.4f, 0.3f);
        s.roughness = 0.02f;
        const f32 r = sdf_bounding_radius(s) * 1.001f;
        for (int i = 0; i < 64; ++i) {
            const f32 a = static_cast<f32>(i) * 0.3f;
            const f32 b = static_cast<f32>(i) * 0.7f;
            const vec3 p = v(r * std::cos(a) * std::cos(b), r * std::sin(b), r * std::sin(a) * std::cos(b));
            conservative = conservative && sdf_object_distance(s, t, p) > 0.f;
        }
    }
    expectTrue(conservative, "bounding radius encloses every primitive (incl. roughness)");
}

} // namespace

int main() {
    testPrimitiveDistances();
    testCsgOps();
    testSmoothUnionContinuity();
    testRoughnessAndBounds();

    if (g_failures == 0) {
        std::printf("fuse_ecs_sdf_csg: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_ecs_sdf_csg: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
