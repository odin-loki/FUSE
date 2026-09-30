// UNI-WP05-1 gates: full TRS world transforms for SceneObject2D / SceneObject3D and HandleTable
// publication in World2D / World3D.
//
//   1. A rotated + non-uniformly scaled parent chain moves a child exactly like the reference matrix
//      product (reference built from explicit axis rotation matrices, independent of the quaternion
//      path the scene nodes use).
//   2. The per-node world cache is invalidated by a change anywhere up the chain (and only then).
//   3. reparentKeepWorld keeps the world pose.
//   4. World2D / World3D publish Handle<Object>: a handle goes stale when its node is removed or
//      destroyed, and a reused slot does not revive it.
//   5. The snapshot SoA world matrices equal the cached world matrices (bitwise).

#include <fuse/math/mat.hpp>
#include <fuse/object.hpp>
#include <fuse/world2d/scene_handle_table.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/scene_snapshot.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/scene_snapshot.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
namespace math = fuse::math;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (!(std::fabs(value - expected) <= epsilon)) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

constexpr float kDeg = 3.14159265358979323846f / 180.f;

/// Double-precision reference: T * Rz(yaw) * Rx(pitch) * Ry(roll) * S from explicit rotation matrices.
struct DMat {
    double m[4][4]; // [row][col]
};

DMat dIdentity() {
    DMat r{};
    for (int i = 0; i < 4; ++i) {
        r.m[i][i] = 1.0;
    }
    return r;
}

DMat dMul(const DMat& a, const DMat& b) {
    DMat r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                s += a.m[i][k] * b.m[k][j];
            }
            r.m[i][j] = s;
        }
    }
    return r;
}

DMat dTRS(double tx, double ty, double tz, double yawDeg, double pitchDeg, double rollDeg, double sx, double sy,
          double sz) {
    const double y = yawDeg * 3.14159265358979323846 / 180.0;
    const double p = pitchDeg * 3.14159265358979323846 / 180.0;
    const double r = rollDeg * 3.14159265358979323846 / 180.0;
    DMat rz = dIdentity();
    rz.m[0][0] = std::cos(y);
    rz.m[0][1] = -std::sin(y);
    rz.m[1][0] = std::sin(y);
    rz.m[1][1] = std::cos(y);
    DMat rx = dIdentity();
    rx.m[1][1] = std::cos(p);
    rx.m[1][2] = -std::sin(p);
    rx.m[2][1] = std::sin(p);
    rx.m[2][2] = std::cos(p);
    DMat ry = dIdentity();
    ry.m[0][0] = std::cos(r);
    ry.m[0][2] = std::sin(r);
    ry.m[2][0] = -std::sin(r);
    ry.m[2][2] = std::cos(r);
    DMat s = dIdentity();
    s.m[0][0] = sx;
    s.m[1][1] = sy;
    s.m[2][2] = sz;
    DMat t = dIdentity();
    t.m[0][3] = tx;
    t.m[1][3] = ty;
    t.m[2][3] = tz;
    return dMul(t, dMul(rz, dMul(rx, dMul(ry, s))));
}

double maxDiff(const math::Mat4& a, const DMat& b) {
    double worst = 0.0;
    for (u32 row = 0; row < 4; ++row) {
        for (u32 col = 0; col < 4; ++col) {
            worst = std::fmax(worst, std::fabs(static_cast<double>(a.at(row, col)) - b.m[row][col]));
        }
    }
    return worst;
}

bool bitwiseEqual(const math::Mat4& a, const math::Mat4& b) {
    return std::memcmp(a.data.data(), b.data.data(), sizeof(float) * 16u) == 0;
}

void setNode(fuse::SceneObject3D& node, float x, float y, float z, float yaw, float pitch, float roll, float sx,
             float sy, float sz) {
    node.setPosition3D(x, y, z);
    node.setEulerDeg(yaw, pitch, roll);
    node.setLocalScale({sx, sy, sz});
}

// 1 ---------------------------------------------------------------------------------------------------
void testRotatedScaledParentChain() {
    fuse::SceneObject3D root("root");
    fuse::SceneObject3D parent("parent");
    fuse::SceneObject3D child("child");
    fuse::Object plainGroup("plain"); // non-scene Object in the chain: transparent
    root.addChild(&parent);
    parent.addChild(&plainGroup);
    plainGroup.addChild(&child);

    setNode(root, 3.f, -2.f, 1.f, 30.f, 10.f, -20.f, 2.f, 1.f, 0.5f);
    setNode(parent, 1.f, 4.f, -3.f, -45.f, 25.f, 60.f, 0.5f, 3.f, 1.5f);
    setNode(child, -2.f, 0.5f, 2.f, 90.f, -35.f, 15.f, 1.f, 2.f, 0.75f);

    const DMat reference = dMul(dTRS(3, -2, 1, 30, 10, -20, 2, 1, 0.5),
                                dMul(dTRS(1, 4, -3, -45, 25, 60, 0.5, 3, 1.5), dTRS(-2, 0.5, 2, 90, -35, 15, 1, 2, 0.75)));
    const double err = maxDiff(child.worldMatrix(), reference);
    std::printf("[gate] rotated+scaled chain: |world - reference| = %.3e\n", err);
    expectTrue(err < 2e-5, "child world matrix == parent chain reference product (rotation + non-uniform scale)");

    // A point in the child's frame lands where the reference puts it.
    const math::Vec3 p = math::transformPoint(child.worldMatrix(), {0.25f, -1.f, 2.f});
    const double rx = reference.m[0][0] * 0.25 + reference.m[0][1] * -1.0 + reference.m[0][2] * 2.0 + reference.m[0][3];
    const double ry = reference.m[1][0] * 0.25 + reference.m[1][1] * -1.0 + reference.m[1][2] * 2.0 + reference.m[1][3];
    const double rz = reference.m[2][0] * 0.25 + reference.m[2][1] * -1.0 + reference.m[2][2] * 2.0 + reference.m[2][3];
    expectNear(p.x, static_cast<float>(rx), 1e-4f, "child-space point x");
    expectNear(p.y, static_cast<float>(ry), 1e-4f, "child-space point y");
    expectNear(p.z, static_cast<float>(rz), 1e-4f, "child-space point z");

    // World decomposition of a rigidly transformed node.
    fuse::SceneObject3D rigid("rigid");
    setNode(rigid, 1.f, 2.f, 3.f, 40.f, 0.f, 0.f, 2.f, 2.f, 2.f);
    const fuse::WorldTransform3D wt = rigid.worldTransform3D();
    expectNear(wt.yaw_deg, 40.f, 1e-3f, "world yaw of a yawed node");
    expectNear(wt.scale.x, 2.f, 1e-5f, "world scale decomposed");
    expectNear(wt.z, 3.f, 1e-6f, "world z");

    // 2D: rotation + scale compose too.
    fuse::SceneObject2D sprite("sprite");
    fuse::SceneObject2D childSprite("childSprite");
    sprite.addChild(&childSprite);
    sprite.setPosition(10.f, 5.f);
    sprite.setRotation(90.f * kDeg);
    sprite.setScale(2.f, 2.f);
    childSprite.setPosition(1.f, 0.f);
    const fuse::WorldTransform2D w2 = childSprite.worldTransform();
    expectNear(w2.x, 10.f, 1e-5f, "2D child x after parent 90deg rotation + scale 2");
    expectNear(w2.y, 7.f, 1e-5f, "2D child y after parent 90deg rotation + scale 2");
    expectNear(w2.rotation, 90.f * kDeg, 1e-5f, "2D child inherits parent rotation");
    expectNear(w2.scaleX, 2.f, 1e-5f, "2D child inherits parent scale");
}

// 2 ---------------------------------------------------------------------------------------------------
void testCacheInvalidation() {
    fuse::SceneObject3D root("root");
    fuse::SceneObject3D mid("mid");
    fuse::SceneObject3D leaf("leaf");
    root.addChild(&mid);
    mid.addChild(&leaf);
    leaf.setPosition3D(1.f, 0.f, 0.f);

    const u64 v0 = leaf.worldVersion();
    expectTrue(leaf.worldVersion() == v0, "unchanged chain keeps the cached world matrix");

    root.setYawDeg(90.f);
    const u64 v1 = leaf.worldVersion();
    expectTrue(v1 != v0, "a root change invalidates the leaf");
    expectNear(leaf.worldTranslation().x, 0.f, 1e-6f, "leaf follows root yaw (x)");
    expectNear(leaf.worldTranslation().y, 1.f, 1e-6f, "leaf follows root yaw (y)");

    mid.setLocalScale({3.f, 3.f, 3.f});
    expectNear(leaf.worldTranslation().y, 3.f, 1e-5f, "leaf follows mid scale");
    const u64 v2 = leaf.worldVersion();
    expectTrue(v2 != v1, "a mid change invalidates the leaf");
    expectTrue(leaf.worldVersion() == v2, "no change -> same version");

    // Reparent (plain Object::reparent, no transform fix-up) is detected without a hook.
    fuse::SceneObject3D other("other");
    other.setPosition3D(0.f, 0.f, 10.f);
    leaf.reparent(&other);
    expectNear(leaf.worldTranslation().z, 10.f, 1e-6f, "reparent picks up the new parent's matrix");
    expectNear(leaf.worldTranslation().x, 1.f, 1e-6f, "reparent drops the old parent's matrix");
}

// 3 ---------------------------------------------------------------------------------------------------
void testReparentKeepsWorldPose() {
    fuse::SceneObject3D a("a");
    fuse::SceneObject3D b("b");
    fuse::SceneObject3D bChild("bChild");
    fuse::SceneObject3D node("node");
    setNode(a, 5.f, -1.f, 2.f, 35.f, 20.f, -10.f, 2.f, 2.f, 2.f);
    setNode(b, -3.f, 7.f, 0.5f, -70.f, 15.f, 40.f, 0.5f, 0.5f, 0.5f);
    setNode(bChild, 1.f, 1.f, 1.f, 10.f, -5.f, 5.f, 1.5f, 1.5f, 1.5f);
    b.addChild(&bChild);
    a.addChild(&node);
    setNode(node, 1.f, 2.f, 3.f, 25.f, -40.f, 70.f, 1.f, 1.5f, 0.8f);

    const math::Mat4 before = node.worldMatrix();
    node.reparentKeepWorld(&bChild);
    expectTrue(node.parent() == &bChild, "reparentKeepWorld moved the node");
    const float err = fuse::scene_math::maxAbsDiff(before, node.worldMatrix());
    std::printf("[gate] reparentKeepWorld: |world after - world before| = %.3e\n", static_cast<double>(err));
    expectTrue(err < 5e-5f, "reparent keeps the world pose (rotated + uniformly scaled parents)");

    node.reparentKeepWorld(nullptr);
    expectTrue(fuse::scene_math::maxAbsDiff(before, node.worldMatrix()) < 5e-5f, "unparent keeps the world pose");
    expectTrue(fuse::scene_math::maxAbsDiff(before, node.localMatrix()) < 5e-5f, "unparented local == world");
}

// 4 ---------------------------------------------------------------------------------------------------
void testHandlesWorld3D() {
    fuse::world3d::World3D world;
    auto first = std::make_unique<fuse::SceneObject3D>("first");
    auto second = std::make_unique<fuse::SceneObject3D>("second");
    const fuse::Handle<fuse::Object> h1 = world.addObject(first.get());
    const fuse::Handle<fuse::Object> h2 = world.addObject(second.get());
    expectTrue(h1.isValid() && h2.isValid() && h1 != h2, "World3D publishes distinct handles");
    expectTrue(first->handle() == h1, "object carries its published handle");
    expectTrue(world.resolve(h1) == first.get(), "live handle resolves");
    expectTrue(world.addObject(first.get()) == h1, "re-adding keeps the handle");

    // Destroy while published: the node's destructor unpublishes it.
    first.reset();
    expectTrue(!world.isLive(h1), "stale handle after destroy is rejected");
    expectTrue(world.resolve(h1) == nullptr, "stale handle resolves to nullptr");
    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 60.f;
    world.tick(ctx);
    expectTrue(world.objectCount() == 1u, "tick prunes the destroyed object");
    expectTrue(world.readTransformSoA().object.size() == 1u && world.readTransformSoA().object[0] == h2,
               "snapshot only carries the live handle");

    // Slot reuse must not revive the old handle.
    auto third = std::make_unique<fuse::SceneObject3D>("third");
    const fuse::Handle<fuse::Object> h3 = world.addObject(third.get());
    expectTrue(h3.index() == h1.index() && h3.generation() != h1.generation(), "freed slot reused with a new generation");
    expectTrue(!world.isLive(h1) && world.isLive(h3), "old handle stays stale after slot reuse");

    // Remove (not destroy): the handle goes stale, the object survives unpublished.
    world.removeObject(second.get());
    expectTrue(!world.isLive(h2), "removed object's handle is stale");
    expectTrue(!second->handle().isValid(), "removed object's handle is cleared");
    expectTrue(second->parent() == nullptr, "removed object left the root");
}

void testHandlesWorld2D() {
    fuse::world2d::World2D world;
    auto sprite = std::make_unique<fuse::SceneObject2D>("sprite");
    const fuse::Handle<fuse::Object> h = world.addSprite(sprite.get());
    expectTrue(world.isLive(h) && world.resolve(h) == sprite.get(), "World2D publishes the sprite");
    sprite.reset();
    expectTrue(!world.isLive(h) && world.resolve(h) == nullptr, "World2D rejects the handle of a destroyed sprite");
    fuse::frame::FrameCtx ctx{};
    world.tick(ctx);
    expectTrue(world.spriteCount() == 0u, "World2D prunes the destroyed sprite");

    auto owned = std::make_unique<fuse::SceneObject2D>("owned");
    fuse::SceneObject2D* ownedRaw = owned.get();
    const fuse::Handle<fuse::Object> ho = world.addSprite(ownedRaw);
    world.adoptOwnedSprite(std::move(owned));
    expectTrue(world.destroySprite(ho), "destroySprite accepts a live handle");
    expectTrue(!world.isLive(ho), "destroySprite invalidates the handle");
    expectTrue(!world.destroySprite(ho), "destroySprite rejects a stale handle");

    // A table that dies first detaches its survivors.
    fuse::SceneObject2D survivor("survivor");
    {
        fuse::SceneHandleTable table;
        table.publish(survivor);
        expectTrue(survivor.handle().isValid(), "published in a scoped table");
    }
    expectTrue(!survivor.handle().isValid() && survivor.handleTable() == nullptr, "table teardown detaches nodes");
}

// 5 ---------------------------------------------------------------------------------------------------
void testSnapshotMatricesMatchCache() {
    fuse::world3d::World3D world;
    fuse::SceneObject3D a("a");
    fuse::SceneObject3D b("b");
    fuse::SceneObject3D aChild("aChild");
    fuse::SceneObject2D aSprite("aSprite");
    setNode(a, 1.f, 2.f, 3.f, 30.f, 15.f, -5.f, 1.f, 2.f, 3.f);
    setNode(b, -4.f, 0.f, 8.f, -60.f, 0.f, 25.f, 0.5f, 0.5f, 0.5f);
    setNode(aChild, 0.f, 1.f, 0.f, 90.f, 45.f, 0.f, 2.f, 1.f, 1.f);
    aSprite.setPosition(2.f, -1.f);
    aSprite.setRotation(0.5f);
    world.addObject(&a);
    world.addObject(&b);
    a.addChild(&aChild);
    aChild.addChild(&aSprite);

    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 60.f;
    for (int frame = 0; frame < 3; ++frame) {
        a.setYawDeg(30.f + 10.f * static_cast<float>(frame));
        world.tick(ctx);
        const fuse::world3d::SceneTransformSoA3D& soa = world.readTransformSoA();
        expectTrue(soa.worldMatrix.size() == 4u && soa.object.size() == 4u, "3D snapshot covers the hierarchy");
        const fuse::SceneObject2D* nodes[4] = {&a, &aChild, &aSprite, &b};
        bool allEqual = soa.worldMatrix.size() == 4u;
        for (u32 i = 0; allEqual && i < 4u; ++i) {
            allEqual = bitwiseEqual(soa.worldMatrix[i], nodes[i]->worldMatrix()) &&
                       soa.worldX[i] == nodes[i]->worldMatrix().data[12] &&
                       soa.worldZ[i] == nodes[i]->worldMatrix().data[14];
        }
        expectTrue(allEqual, "3D snapshot matrices == cached world matrices (bitwise)");
    }
    // Only published nodes carry handles; nested nodes that were never added stay invalid.
    expectTrue(world.readTransformSoA().object[0] == a.handle() && !world.readTransformSoA().object[1].isValid(),
               "snapshot rows carry the published handles");

    fuse::SceneObject2D root2d("root2d");
    fuse::SceneObject2D c1("c1");
    fuse::SceneObject2D c2("c2");
    root2d.addChild(&c1);
    c1.addChild(&c2);
    root2d.setRotation(0.3f);
    root2d.setScale(2.f, 0.5f);
    c1.setPosition(3.f, 1.f);
    c1.setRotation(-0.7f);
    c2.setPosition(-1.f, 2.f);
    fuse::world2d::SceneSnapshot2D snapshot2d;
    fuse::world2d::SceneTransformSoA2D soa2d;
    fuse::world2d::fillSnapshotSoA(root2d, snapshot2d, soa2d);
    const fuse::SceneObject2D* nodes2d[3] = {&root2d, &c1, &c2};
    bool equal2d = soa2d.worldMatrix.size() == 3u;
    for (u32 i = 0; equal2d && i < 3u; ++i) {
        equal2d = bitwiseEqual(soa2d.worldMatrix[i], nodes2d[i]->worldMatrix()) &&
                  soa2d.worldX[i] == nodes2d[i]->worldMatrix().data[12] &&
                  snapshot2d.sprites()[i].rotation == soa2d.worldRotation[i];
    }
    expectTrue(equal2d, "2D snapshot matrices == cached world matrices (bitwise)");
}

void testEulerSetterCompatibility() {
    fuse::SceneObject3D node("node");
    node.setYawDeg(100.f);
    node.setPitchDeg(20.f);
    node.setRollDeg(-30.f);
    expectNear(node.yawDeg(), 100.f, 1e-5f, "yaw getter returns the set value");
    expectNear(node.pitchDeg(), 20.f, 1e-5f, "pitch getter returns the set value");
    expectNear(node.rollDeg(), -30.f, 1e-5f, "roll getter returns the set value");
    fuse::SceneObject3D fromQuat("fromQuat");
    fromQuat.setLocalRotation(node.localRotation());
    expectNear(fromQuat.yawDeg(), 100.f, 1e-3f, "yaw decomposed from the quaternion");
    expectNear(fromQuat.pitchDeg(), 20.f, 1e-3f, "pitch decomposed from the quaternion");
    expectNear(fromQuat.rollDeg(), -30.f, 1e-3f, "roll decomposed from the quaternion");
}

} // namespace

int main() {
    testRotatedScaledParentChain();
    testCacheInvalidation();
    testReparentKeepsWorldPose();
    testHandlesWorld3D();
    testHandlesWorld2D();
    testSnapshotMatricesMatchCache();
    testEulerSetterCompatibility();

    if (g_failures == 0) {
        std::printf("fuse scene transform gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse scene transform gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
