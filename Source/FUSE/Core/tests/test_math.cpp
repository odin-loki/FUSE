#include <fuse/math/math.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

using fuse::f32;
using fuse::u32;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(f32 value, f32 expected, f32 epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void expectVec3Near(const fuse::math::Vec3& value, const fuse::math::Vec3& expected, f32 epsilon,
                    const char* message) {
    expectNear(value.x, expected.x, epsilon, message);
    expectNear(value.y, expected.y, epsilon, message);
    expectNear(value.z, expected.z, epsilon, message);
}

void testVecBasics() {
    const fuse::math::Vec3 a{1.f, 2.f, 3.f};
    const fuse::math::Vec3 b{4.f, 5.f, 6.f};
    const fuse::math::Vec3 sum = a + b;
    expectVec3Near(sum, {5.f, 7.f, 9.f}, 1e-5f, "Vec3 addition");

    const fuse::math::Vec3 scaled = 2.f * a;
    expectVec3Near(scaled, {2.f, 4.f, 6.f}, 1e-5f, "Vec3 scalar multiply");

    expectNear(a.dot(b), 32.f, 1e-5f, "Vec3 dot product");
    expectNear(a.length(), std::sqrt(14.f), 1e-5f, "Vec3 length");

    const fuse::math::Vec3 cross = fuse::math::cross({1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    expectVec3Near(cross, {0.f, 0.f, 1.f}, 1e-5f, "Vec3 cross product");
}

void testMat4MultiplyIdentity() {
    const fuse::math::Mat4 identity = fuse::math::Mat4::identity();
    const fuse::math::Mat4 translated = fuse::math::fromTRS({3.f, 4.f, 5.f}, fuse::math::Quat::identity(),
                                                            {1.f, 1.f, 1.f});
    const fuse::math::Mat4 result = identity * translated;
    expectNear(result.data[12], 3.f, 1e-5f, "Mat4 identity multiply preserves translation x");
    expectNear(result.data[13], 4.f, 1e-5f, "Mat4 identity multiply preserves translation y");
    expectNear(result.data[14], 5.f, 1e-5f, "Mat4 identity multiply preserves translation z");
}

void testMat4TransformPoint() {
    const fuse::math::Mat4 matrix =
        fuse::math::fromTRS({10.f, 0.f, -2.f}, fuse::math::Quat::identity(), {1.f, 1.f, 1.f});
    const fuse::math::Vec3 point = fuse::math::transformPoint(matrix, {1.f, 2.f, 3.f});
    expectVec3Near(point, {11.f, 2.f, 1.f}, 1e-5f, "Mat4 transformPoint applies translation");
}

void testMat4InverseAffine() {
    const fuse::math::Mat4 matrix =
        fuse::math::fromTRS({2.f, -1.f, 5.f}, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 0.5f), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 inverse = fuse::math::inverseAffine(matrix);
    const fuse::math::Mat4 product = matrix * inverse;
    const fuse::math::Vec3 round_trip = fuse::math::transformPoint(product, {1.f, 2.f, 3.f});
    expectVec3Near(round_trip, {1.f, 2.f, 3.f}, 1e-4f, "Mat4 inverseAffine round-trip preserves points");
}

void testMat3Upper3x3() {
    const fuse::math::Mat4 matrix =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::Quat::identity(), {2.f, 3.f, 4.f});
    const fuse::math::Mat3 upper = matrix.upper3x3();
    expectNear(upper.data[0], 2.f, 1e-4f, "Mat3 extracts scaled X axis");
    expectNear(upper.data[4], 3.f, 1e-4f, "Mat3 extracts scaled Y axis");
    expectNear(upper.data[8], 4.f, 1e-4f, "Mat3 extracts scaled Z axis");
}

void testQuatRotation() {
    const fuse::math::Quat rotation = fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 1.5707963f);
    const fuse::math::Vec3 rotated = rotation.rotate({1.f, 0.f, 0.f});
    expectVec3Near(rotated, {0.f, 0.f, -1.f}, 1e-4f, "Quat rotates +X toward -Z");

    const fuse::math::Quat composed = rotation * rotation.conjugate();
    expectNear(composed.w, 1.f, 1e-4f, "Quat times conjugate yields identity w");
    expectNear(composed.x, 0.f, 1e-4f, "Quat times conjugate yields identity x");
}

void testQuatSlerp() {
    const fuse::math::Quat a = fuse::math::Quat::identity();
    const fuse::math::Quat b = fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 1.5707963f);
    const fuse::math::Quat mid = fuse::math::slerp(a, b, 0.5f);
    const fuse::math::Vec3 rotated = mid.rotate({1.f, 0.f, 0.f});
    expectNear(rotated.y, 0.f, 1e-4f, "Slerp midpoint keeps rotation in XZ plane");
    expectTrue(rotated.x > 0.5f && rotated.z < 0.f, "Slerp midpoint rotates between endpoints");
}

void testAabbOverlap() {
    const fuse::math::AABB a{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const fuse::math::AABB b{{0.5f, 0.5f, 0.5f}, {2.f, 2.f, 2.f}};
    const fuse::math::AABB c{{3.f, 3.f, 3.f}, {4.f, 4.f, 4.f}};

    expectTrue(a.overlaps(b), "AABB overlap detects intersection");
    expectTrue(!a.overlaps(c), "AABB overlap rejects separated boxes");
    expectTrue(a.contains({0.f, 0.f, 0.f}), "AABB contains origin");
    expectTrue(!a.contains({2.f, 0.f, 0.f}), "AABB rejects exterior point");

    const fuse::math::AABB merged = a.merge(c);
    expectVec3Near(merged.min, {-1.f, -1.f, -1.f}, 1e-5f, "AABB merge min");
    expectVec3Near(merged.max, {4.f, 4.f, 4.f}, 1e-5f, "AABB merge max");
}

void testAabbRayIntersect() {
    const fuse::math::AABB box{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const f32 hit = box.rayIntersect({-3.f, 0.f, 0.f}, {1.f, 0.f, 0.f});
    expectNear(hit, 2.f, 1e-4f, "AABB ray hit along +X");

    const f32 miss = box.rayIntersect({-3.f, 2.f, 0.f}, {1.f, 0.f, 0.f});
    expectNear(miss, -1.f, 1e-5f, "AABB ray miss above box");
}

void testFrustumCulling() {
    const fuse::math::Mat4 view = fuse::math::lookAt({0.f, 0.f, 5.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    const fuse::math::Mat4 projection = fuse::math::perspective(60.f, 16.f / 9.f, 0.1f, 100.f);
    const fuse::math::Frustum frustum = fuse::math::Frustum::fromViewProjection(projection * view);

    const fuse::math::AABB visible{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    const fuse::math::AABB hidden{{0.f, 10.f, 0.f}, {1.f, 11.f, 1.f}};

    expectTrue(frustum.intersectsAabb(visible), "Frustum intersects centered AABB");
    expectTrue(!frustum.intersectsAabb(hidden), "Frustum rejects AABB above view");
    expectTrue(frustum.intersectsSphere({0.f, 0.f, 0.f}, 0.25f), "Frustum intersects near sphere");
    expectTrue(!frustum.intersectsSphere({0.f, 20.f, 0.f}, 0.25f), "Frustum rejects distant sphere");
}

void testSdfPrimitives() {
    expectNear(fuse::math::SDF::sphere({0.f, 0.f, 0.f}, 2.f), -2.f, 1e-5f, "SDF sphere at origin");
    expectNear(fuse::math::SDF::sphere({3.f, 0.f, 0.f}, 2.f), 1.f, 1e-5f, "SDF sphere surface distance");
}

} // namespace

int main() {
    testVecBasics();
    testMat4MultiplyIdentity();
    testMat4TransformPoint();
    testMat4InverseAffine();
    testMat3Upper3x3();
    testQuatRotation();
    testQuatSlerp();
    testAabbOverlap();
    testAabbRayIntersect();
    testFrustumCulling();
    testSdfPrimitives();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d math test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_math_tests: all tests passed.\n");
    return EXIT_SUCCESS;
}
