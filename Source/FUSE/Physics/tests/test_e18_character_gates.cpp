// E18 part 3a (GAP-PHYS-CHARACTER): shape casts and the kinematic capsule character controller.
//  - sweep TOI == analytic for sphere vs plane (random directions), sphere vs box (face and edge regions),
//    capsule vs plane, rotated box vs plane; casts against hulls and triangle meshes agree with the
//    equivalent primitives
//  - 0.3 m steps are climbed, 0.6 m steps block (step height 0.35 m)
//  - a 30 deg slope holds the character (max slope 45 deg), a 55 deg slope slides it down
//  - no tunnelling at 20 m/s through a 5 cm wall (box and triangle-mesh walls)
//  - moving platforms carry the character; walking into a dynamic body pushes it
//  - determinism: two runs bit-identical
#include "e18_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/character_controller.hpp>
#include <fuse/physics/queries/shape_queries.hpp>
#include <fuse/physics/shapes/shape_pool.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

using namespace e18;
namespace np = fuse::physics::narrowphase;

namespace {

constexpr f32 kDt = 1.f / 60.f;
constexpr f32 kPi = 3.14159265f;

np::ShapeInstance makeShape(CollisionShapeType type, vec3 params, vec3 position, quat orientation = {},
                            f32 scalar = 0.f, u32 ref = kNoShapeRef) {
    np::ShapeInstance s{};
    s.type = type;
    s.params = params;
    s.position = position;
    s.orientation = orientation;
    s.scalar = scalar;
    s.shapeRef = ref;
    return s;
}

void testSweepToiAnalytic() {
    Rng rng(5u);
    f32 worst = 0.f;
    u32 misses = 0;
    // Sphere vs plane, random downward directions.
    const np::ShapeInstance plane = makeShape(CollisionShapeType::Plane, {0.f, 1.f, 0.f}, {}, {}, 0.f);
    for (u32 i = 0; i < 500u; ++i) {
        const f32 radius = rng.range(0.1f, 1.f);
        const vec3 start{rng.range(-5.f, 5.f), rng.range(radius + 0.01f, 8.f), rng.range(-5.f, 5.f)};
        vec3 dir = rng.onSphere();
        dir.y = -std::fabs(dir.y) - 0.1f;
        dir = dir.normalized();
        const f32 analytic = (start.y - radius) / -dir.y;
        ShapeCastHit hit{};
        const bool ok = shapeCast(makeShape(CollisionShapeType::Sphere, {radius, 0.f, 0.f}, start), dir, 100.f, plane, hit);
        misses += ok ? 0u : 1u;
        worst = std::max(worst, ok ? std::fabs(hit.distance - analytic) : 1.f);
    }
    std::printf("sweep sphere vs plane: 500 casts, worst TOI error %.2e m, misses %u\n", static_cast<double>(worst), misses);
    expectTrue(misses == 0u && worst < 1e-4f, "sphere-plane sweep TOI == analytic");

    // Sphere vs box: face region (tilted rays that still hit the -X face) and the top edge.
    const np::ShapeInstance box = makeShape(CollisionShapeType::Box, {1.f, 1.f, 1.f}, {});
    ConvexHull boxHull;
    makeBoxHull({1.f, 1.f, 1.f}, boxHull);
    const u32 hullRef = ShapePool::global().addHull(std::move(boxHull));
    const np::ShapeInstance hull = makeShape(CollisionShapeType::ConvexHull, {1.f, 1.f, 1.f}, {}, {}, 0.f, hullRef);
    worst = 0.f;
    f32 worstHull = 0.f;
    for (u32 i = 0; i < 500u; ++i) {
        const f32 r = 0.5f;
        const vec3 start{-5.f, rng.range(-0.4f, 0.4f), rng.range(-0.4f, 0.4f)};
        const vec3 dir = (vec3{1.f, rng.range(-0.08f, 0.08f), rng.range(-0.08f, 0.08f)}).normalized();
        const f32 analytic = (-1.f - r - start.x) / dir.x; // centre reaches x = -1.5 (face contact)
        ShapeCastHit hit{};
        const bool ok = shapeCast(makeShape(CollisionShapeType::Sphere, {r, 0.f, 0.f}, start), dir, 20.f, box, hit);
        worst = std::max(worst, ok ? std::fabs(hit.distance - analytic) : 1.f);
        ShapeCastHit hitHull{};
        const bool okHull = shapeCast(makeShape(CollisionShapeType::Sphere, {r, 0.f, 0.f}, start), dir, 20.f, hull, hitHull);
        worstHull = std::max(worstHull, okHull ? std::fabs(hitHull.distance - analytic) : 1.f);
    }
    // Edge region: centre at y = 1.3 moving +x touches the edge (x = -1, y = 1) when (x + 1)^2 + 0.3^2 = 0.5^2.
    ShapeCastHit edge{};
    shapeCast(makeShape(CollisionShapeType::Sphere, {0.5f, 0.f, 0.f}, {-5.f, 1.3f, 0.f}), {1.f, 0.f, 0.f}, 20.f, box, edge);
    const f32 edgeError = std::fabs(edge.distance - 3.6f);
    const vec3 edgeNormal = (vec3{-0.4f, 0.3f, 0.f}).normalized();
    std::printf("sweep sphere vs box: face worst %.2e m (hull %.2e m), edge TOI %.5f (analytic 3.6), normal dot %.5f\n",
                static_cast<double>(worst), static_cast<double>(worstHull), static_cast<double>(edge.distance),
                static_cast<double>(edge.normal.dot(edgeNormal)));
    expectTrue(worst < 1e-4f && worstHull < 1e-4f, "sphere-box (and box-shaped hull) sweep TOI == analytic (face)");
    expectTrue(edgeError < 1e-4f && edge.normal.dot(edgeNormal) > 0.9999f, "sphere-box sweep hits the edge at the analytic TOI");

    // Capsule vs plane and a rotated box vs plane.
    ShapeCastHit cap{};
    shapeCast(makeShape(CollisionShapeType::Capsule, {0.3f, 0.5f, 0.f}, {1.f, 3.f, 2.f}), {0.f, -1.f, 0.f}, 10.f, plane, cap);
    const quat tilt = quatFromAxisAngle({0.f, 0.f, 1.f}, kPi / 4.f);
    ShapeCastHit rotated{};
    shapeCast(makeShape(CollisionShapeType::Box, {0.5f, 0.5f, 0.5f}, {0.f, 3.f, 0.f}, tilt), {0.f, -1.f, 0.f}, 10.f, plane,
              rotated);
    const f32 rotatedAnalytic = 3.f - 0.5f * std::sqrt(2.f);
    std::printf("sweep capsule vs plane TOI %.5f (analytic 2.2); rotated box vs plane %.5f (analytic %.5f)\n",
                static_cast<double>(cap.distance), static_cast<double>(rotated.distance),
                static_cast<double>(rotatedAnalytic));
    expectTrue(std::fabs(cap.distance - 2.2f) < 1e-4f, "capsule-plane sweep TOI == analytic");
    expectTrue(std::fabs(rotated.distance - rotatedAnalytic) < 1e-4f, "rotated box-plane sweep TOI == analytic");

    // Triangle-mesh floor == analytic plane.
    std::vector<vec3> vertices;
    std::vector<u32> indices;
    gridMesh(8u, 10.f, 0.f, vertices, indices);
    TriMesh floorMesh;
    floorMesh.build(vertices, indices);
    const u32 meshRef = ShapePool::global().addMesh(std::move(floorMesh));
    const np::ShapeInstance meshShape = makeShape(CollisionShapeType::TriMesh, {10.f, 0.f, 10.f}, {}, {}, 0.f, meshRef);
    worst = 0.f;
    for (u32 i = 0; i < 300u; ++i) {
        const vec3 start{rng.range(-6.f, 6.f), rng.range(1.f, 4.f), rng.range(-6.f, 6.f)};
        const vec3 dir = (vec3{rng.range(-0.3f, 0.3f), -1.f, rng.range(-0.3f, 0.3f)}).normalized();
        const u32 kind = i % 3u;
        const np::ShapeInstance caster =
            kind == 0u ? makeShape(CollisionShapeType::Sphere, {0.4f, 0.f, 0.f}, start)
            : kind == 1u ? makeShape(CollisionShapeType::Capsule, {0.3f, 0.4f, 0.f}, start, rng.rotation())
                         : makeShape(CollisionShapeType::Box, {0.3f, 0.2f, 0.4f}, start, rng.rotation());
        ShapeCastHit onMesh{};
        ShapeCastHit onPlane{};
        const bool a = shapeCast(caster, dir, 10.f, meshShape, onMesh);
        const bool b = shapeCast(caster, dir, 10.f, plane, onPlane);
        const f32 diff = (a && b) ? std::fabs(onMesh.distance - onPlane.distance) : 1.f;
        if (diff > 2e-4f) {
            std::printf("  mesh sweep mismatch: kind %u start (%.3f %.3f %.3f) mesh %d %.5f plane %d %.5f\n", kind,
                        static_cast<double>(start.x), static_cast<double>(start.y), static_cast<double>(start.z), a ? 1 : 0,
                        static_cast<double>(onMesh.distance), b ? 1 : 0, static_cast<double>(onPlane.distance));
        }
        worst = std::max(worst, diff);
    }
    std::printf("sweep sphere / capsule / box vs trimesh floor == vs plane: worst TOI difference %.2e m\n",
                static_cast<double>(worst));
    expectTrue(worst < 2e-4f, "sweeps against a triangle-mesh floor match the analytic plane");
}

struct World {
    Registry reg;
    PhysicsManager manager;
    PhysicsStreamManager streams{};

    World() {
        reg.init(512);
        manager.init({});
    }
    void step(u32 frames = 1u) {
        for (u32 i = 0; i < frames; ++i) {
            manager.step(reg, kDt, streams);
        }
    }
};

EntityID spawnCharacter(Registry& reg, vec3 position) {
    const EntityID id = reg.create();
    fuse::ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    reg.add(id, t);
    fuse::ecs::CharacterController cc{};
    cc.radius = 0.3f;
    cc.half_height = 0.6f;
    cc.step_height = 0.35f;
    cc.max_slope_deg = 45.f;
    reg.add(id, cc);
    return id;
}

void setMove(Registry& reg, EntityID id, vec3 velocity) {
    reg.get<fuse::ecs::CharacterController>(id)->move_velocity = {velocity.x, velocity.y, velocity.z, 0.f};
}

EntityID spawnStaticBox(Registry& reg, vec3 centre, vec3 half, quat rotation = {}) {
    return spawnBody(reg, centre, fuse::ecs::Collider::Box, {half.x, half.y, half.z, 0.f}, true, 1.f, rotation);
}

vec3 walkIntoStep(f32 stepHeight, bool& grounded) {
    World w;
    spawnGroundPlane(w.reg);
    spawnStaticBox(w.reg, {3.f, stepHeight * 0.5f, 0.f}, {1.5f, stepHeight * 0.5f, 3.f}); // front face at x = 1.5
    const EntityID c = spawnCharacter(w.reg, {0.f, 0.95f, 0.f});
    w.step(10u); // settle onto the ground
    setMove(w.reg, c, {2.f, 0.f, 0.f});
    w.step(120u);
    grounded = w.reg.get<fuse::ecs::CharacterController>(c)->grounded;
    return positionOf(w.reg, c);
}

void testSteps() {
    bool groundedLow = false;
    bool groundedHigh = false;
    const vec3 low = walkIntoStep(0.3f, groundedLow);
    const vec3 high = walkIntoStep(0.6f, groundedHigh);
    std::printf("steps: 0.3 m -> x %.3f y %.4f (on top: y 1.2), 0.6 m -> x %.3f y %.4f (blocked at x 1.19)\n",
                static_cast<double>(low.x), static_cast<double>(low.y), static_cast<double>(high.x),
                static_cast<double>(high.y));
    expectTrue(low.x > 3.5f && std::fabs(low.y - 1.2f) < 0.02f && groundedLow, "a 0.3 m step is climbed");
    expectTrue(high.x < 1.2f + 0.02f && high.x > 1.1f && std::fabs(high.y - 0.9f) < 0.02f && groundedHigh,
               "a 0.6 m step blocks the character");
}

/// A 20 x 12 m ramp (two triangles) rising along +x at `angleDeg`.
u32 rampMesh(f32 angleDeg) {
    const f32 slope = std::tan(angleDeg * kPi / 180.f);
    const std::vector<vec3> vertices = {{-10.f, -10.f * slope, -6.f}, {10.f, 10.f * slope, -6.f},
                                        {-10.f, -10.f * slope, 6.f}, {10.f, 10.f * slope, 6.f}};
    const std::vector<u32> indices = {0, 2, 1, 1, 2, 3};
    TriMesh mesh;
    mesh.build(vertices, indices);
    return ShapePool::global().addMesh(std::move(mesh));
}

vec3 standOnSlope(f32 angleDeg, bool& grounded, bool& steep) {
    World w;
    spawnPooled(w.reg, {}, fuse::ecs::Collider::TriMesh, rampMesh(angleDeg), true);
    const f32 lift = 0.3f / std::cos(angleDeg * kPi / 180.f) + 0.6f + 0.02f;
    const EntityID c = spawnCharacter(w.reg, {0.f, lift, 0.f});
    w.step(120u);
    grounded = w.reg.get<fuse::ecs::CharacterController>(c)->grounded;
    steep = w.reg.get<fuse::ecs::CharacterController>(c)->on_steep_slope;
    return positionOf(w.reg, c);
}

void testSlopes() {
    bool groundedGentle = false;
    bool steepGentle = false;
    bool groundedSteep = false;
    bool steepSteep = false;
    const vec3 gentle = standOnSlope(30.f, groundedGentle, steepGentle);
    const vec3 steep = standOnSlope(55.f, groundedSteep, steepSteep);
    std::printf("slopes (max 45 deg): 30 deg -> x %.4f (grounded %d), 55 deg -> x %.3f y %.3f (grounded %d)\n",
                static_cast<double>(gentle.x), groundedGentle ? 1 : 0, static_cast<double>(steep.x),
                static_cast<double>(steep.y), groundedSteep ? 1 : 0);
    expectTrue(std::fabs(gentle.x) < 0.02f && groundedGentle, "a walkable 30 deg slope holds the standing character");
    expectTrue(steep.x < -1.f && !groundedSteep, "a 55 deg slope (above max slope) slides the character down");

    // Walking up a walkable slope.
    World w;
    spawnPooled(w.reg, {}, fuse::ecs::Collider::TriMesh, rampMesh(25.f), true);
    const EntityID c = spawnCharacter(w.reg, {0.f, 0.3f / std::cos(25.f * kPi / 180.f) + 0.62f, 0.f});
    w.step(10u);
    setMove(w.reg, c, {2.f, 0.f, 0.f});
    w.step(60u);
    const vec3 up = positionOf(w.reg, c);
    std::printf("walking up a 25 deg slope for 1 s: x %.3f y %.3f\n", static_cast<double>(up.x), static_cast<double>(up.y));
    expectTrue(up.x > 1.5f && up.y > 0.6f + 1.5f * std::tan(25.f * kPi / 180.f), "the character walks up a walkable slope");
}

f32 runIntoThinWall(bool meshWall) {
    World w;
    spawnGroundPlane(w.reg);
    if (meshWall) {
        // A single vertical quad at x = 3 (two triangles, two-sided).
        const std::vector<vec3> vertices = {{3.f, 0.f, -3.f}, {3.f, 0.f, 3.f}, {3.f, 4.f, -3.f}, {3.f, 4.f, 3.f}};
        const std::vector<u32> indices = {0, 1, 2, 2, 1, 3};
        TriMesh mesh;
        mesh.build(vertices, indices);
        spawnPooled(w.reg, {}, fuse::ecs::Collider::TriMesh, ShapePool::global().addMesh(std::move(mesh)), true);
    } else {
        spawnStaticBox(w.reg, {3.f, 2.f, 0.f}, {0.025f, 2.f, 3.f}); // 5 cm thick
    }
    const EntityID c = spawnCharacter(w.reg, {0.f, 0.92f, 0.f});
    w.step(5u);
    setMove(w.reg, c, {20.f, 0.f, 0.f}); // 0.33 m per step
    f32 furthest = -1e30f;
    for (u32 frame = 0; frame < 60u; ++frame) {
        w.step();
        furthest = std::max(furthest, positionOf(w.reg, c).x);
    }
    return furthest;
}

void testNoTunnelling() {
    const f32 box = runIntoThinWall(false);
    const f32 mesh = runIntoThinWall(true);
    std::printf("20 m/s into a 5 cm wall: furthest x %.4f (wall face 2.975 - radius 0.3); into a mesh quad: %.4f "
                "(3.0 - 0.3)\n",
                static_cast<double>(box), static_cast<double>(mesh));
    expectTrue(box < 2.975f - 0.3f + 1e-3f && box > 2.6f, "no tunnelling through a thin box wall at 20 m/s");
    expectTrue(mesh < 3.f - 0.3f + 1e-3f && mesh > 2.6f, "no tunnelling through a triangle-mesh wall at 20 m/s");
}

void testPlatformAndPush() {
    // Moving platform: a kinematic box driven by game code at 1 m/s.
    World w;
    spawnGroundPlane(w.reg, -5.f);
    const EntityID platform = spawnBody(w.reg, {0.f, 0.25f, 0.f}, fuse::ecs::Collider::Box, {2.f, 0.25f, 2.f, 0.f}, false);
    w.reg.add(platform, fuse::ecs::TagKinematic{});
    const EntityID c = spawnCharacter(w.reg, {0.f, 0.5f + 0.9f + 0.01f, 0.f});
    w.step(10u);
    const vec3 start = positionOf(w.reg, c);
    for (u32 frame = 0; frame < 120u; ++frame) {
        fuse::ecs::Transform* t = w.reg.get<fuse::ecs::Transform>(platform);
        t->position.x += 1.f * kDt;
        w.step();
    }
    const vec3 end = positionOf(w.reg, c);
    const f32 carried = end.x - start.x;
    std::printf("moving platform (1 m/s for 2 s): character carried %.3f m, height %.3f\n", static_cast<double>(carried),
                static_cast<double>(end.y));
    expectTrue(std::fabs(carried - 2.f) < 0.1f && std::fabs(end.y - 1.4f) < 0.05f,
               "the character inherits the moving platform's velocity");

    // Pushing a dynamic box.
    World p;
    spawnGroundPlane(p.reg);
    const EntityID crate = spawnBody(p.reg, {1.5f, 0.4f, 0.f}, fuse::ecs::Collider::Box, {0.4f, 0.4f, 0.4f, 0.f}, false, 5.f);
    const EntityID pusher = spawnCharacter(p.reg, {0.f, 0.92f, 0.f});
    p.step(10u);
    const f32 crateStart = positionOf(p.reg, crate).x;
    setMove(p.reg, pusher, {1.5f, 0.f, 0.f});
    p.step(90u);
    const f32 pushed = positionOf(p.reg, crate).x - crateStart;
    std::printf("walking into a 5 kg crate for 1.5 s: crate moved %.3f m\n", static_cast<double>(pushed));
    expectTrue(pushed > 0.1f, "walking into a dynamic body pushes it");
}

void testDeterminismAndRegistration() {
    const auto run = [] {
        World w;
        spawnGroundPlane(w.reg);
        spawnStaticBox(w.reg, {3.f, 0.15f, 0.f}, {1.5f, 0.15f, 3.f});
        spawnStaticBox(w.reg, {2.f, 1.f, 3.5f}, {0.3f, 1.f, 0.3f}, quatFromAxisAngle({0.f, 1.f, 0.f}, 0.4f));
        const EntityID c = spawnCharacter(w.reg, {0.f, 0.95f, 1.f});
        w.step(5u);
        std::vector<vec3> trail;
        for (u32 frame = 0; frame < 180u; ++frame) {
            const f32 a = 0.02f * static_cast<f32>(frame);
            setMove(w.reg, c, {3.f * std::cos(a), 0.f, 1.5f * std::sin(a)});
            if (frame == 60u) {
                w.reg.get<fuse::ecs::CharacterController>(c)->jump_speed = 4.f;
            }
            w.step();
            trail.push_back(positionOf(w.reg, c));
        }
        return trail;
    };
    const std::vector<vec3> first = run();
    const std::vector<vec3> second = run();
    bool identical = first.size() == second.size();
    for (usize i = 0; identical && i < first.size(); ++i) {
        identical = std::memcmp(&first[i], &second[i], sizeof(vec3)) == 0;
    }
    expectTrue(identical, "two character runs (walk, jump, step, obstacle) are bit-identical");
    expectTrue(fuse::ecs::ComponentTypes::find("CharacterController") != nullptr,
               "the CharacterController component is registered (serialisable)");
}

} // namespace

int main() {
    fuse::core::initialize();
    testSweepToiAnalytic();
    testSteps();
    testSlopes();
    testNoTunnelling();
    testPlatformAndPush();
    testDeterminismAndRegistration();
    fuse::core::shutdown();
    return finish("fuse_e18_character_gates");
}
