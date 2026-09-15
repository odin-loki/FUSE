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

void expectAabbNear(const fuse::math::AABB& value, const fuse::math::AABB& expected, f32 epsilon,
                    const char* message) {
    expectVec3Near(value.min, expected.min, epsilon, message);
    expectVec3Near(value.max, expected.max, epsilon, message);
}

bool mat4Near(const fuse::math::Mat4& value, const fuse::math::Mat4& expected, f32 epsilon) {
    for (u32 i = 0; i < 16; ++i) {
        if (std::fabs(value.data[i] - expected.data[i]) > epsilon) {
            return false;
        }
    }
    return true;
}

void expectMat4Near(const fuse::math::Mat4& value, const fuse::math::Mat4& expected, f32 epsilon,
                    const char* message) {
    if (!mat4Near(value, expected, epsilon)) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
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

void testMat4MultiplyEdgeCases() {
    const fuse::math::Mat4 identity = fuse::math::Mat4::identity();
    const fuse::math::Mat4 translate =
        fuse::math::fromTRS({3.f, -2.f, 7.f}, fuse::math::Quat::identity(), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 rotate =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, 1.2f), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 chain = translate * rotate;

    expectMat4Near(identity * translate, translate, 1e-5f, "Mat4 left identity multiply");
    expectMat4Near(translate * identity, translate, 1e-5f, "Mat4 right identity multiply");

    const fuse::math::Mat4 assocLeft = (translate * rotate) * chain;
    const fuse::math::Mat4 assocRight = translate * (rotate * chain);
    expectMat4Near(assocLeft, assocRight, 1e-4f, "Mat4 multiply associativity");

    const fuse::math::Vec3 probe{1.f, 0.f, 0.f};
    const fuse::math::Vec3 chained = fuse::math::transformPoint(chain, probe);
    const fuse::math::Vec3 staged =
        fuse::math::transformPoint(translate, fuse::math::transformPoint(rotate, probe));
    expectVec3Near(chained, staged, 1e-4f, "Mat4 chained multiply matches staged transforms");
}

void testMat4InverseEdgeCases() {
    const fuse::math::Mat4 identity = fuse::math::Mat4::identity();
    expectMat4Near(fuse::math::inverseAffine(identity), identity, 1e-5f, "Mat4 inverse of identity");

    const fuse::math::Mat4 translation =
        fuse::math::fromTRS({-4.f, 2.f, 1.f}, fuse::math::Quat::identity(), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 inverseTranslation = fuse::math::inverseAffine(translation);
    expectVec3Near(fuse::math::transformPoint(translation * inverseTranslation, {5.f, 6.f, 7.f}),
                   {5.f, 6.f, 7.f}, 1e-4f, "Mat4 translation inverse round-trip");

    const fuse::math::Mat4 rotation =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::fromAxisAngle({1.f, 0.f, 0.f}, 0.75f),
                             {1.f, 1.f, 1.f});
    const fuse::math::Mat4 inverseRotation = fuse::math::inverseAffine(rotation);
    expectMat4Near(rotation * inverseRotation, identity, 1e-4f, "Mat4 pure rotation inverse product");

    const fuse::math::Mat4 rigid =
        fuse::math::fromTRS({1.f, -3.f, 2.f}, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, -0.4f),
                            {1.f, 1.f, 1.f});
    const fuse::math::Mat4 inverseRigid = fuse::math::inverseAffine(rigid);
    expectMat4Near(inverseRigid * rigid, identity, 1e-4f, "Mat4 rigid-body inverse on left");
    expectMat4Near(rigid * inverseRigid, identity, 1e-4f, "Mat4 rigid-body inverse on right");
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

void testAabbTransformHelpers() {
    const fuse::math::AABB local{{-1.f, -2.f, -3.f}, {1.f, 2.f, 3.f}};

    const fuse::math::Mat4 translate =
        fuse::math::fromTRS({5.f, 0.f, -1.f}, fuse::math::Quat::identity(), {1.f, 1.f, 1.f});
    const fuse::math::AABB translated = fuse::math::transformAabb(translate, local);
    expectAabbNear(translated, {{4.f, -2.f, -4.f}, {6.f, 2.f, 2.f}}, 1e-4f,
                   "AABB transformAabb applies translation");

    const fuse::math::Mat4 rotate =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 1.5707963f),
                            {1.f, 1.f, 1.f});
    const fuse::math::AABB rotatedFast = fuse::math::transformAabb(rotate, local);
    const fuse::math::AABB rotatedExact = fuse::math::transformAabbCorners(rotate, local);
    expectAabbNear(rotatedFast, rotatedExact, 1e-4f, "AABB transformAabb matches corner reference");

    const fuse::math::Mat4 rigid =
        fuse::math::fromTRS({2.f, 1.f, -1.f}, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, 0.5f),
                            {1.f, 1.f, 1.f});
    const fuse::math::AABB worldFast = fuse::math::transformAabb(rigid, local);
    const fuse::math::AABB worldExact = fuse::math::transformAabbCorners(rigid, local);
    expectAabbNear(worldFast, worldExact, 1e-4f, "AABB rigid transform envelope matches corners");

    const fuse::math::Mat4 scaled =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::Quat::identity(), {2.f, 3.f, 4.f});
    const fuse::math::AABB scaledExact = fuse::math::transformAabbCorners(scaled, local);
    expectAabbNear(scaledExact, {{-2.f, -6.f, -12.f}, {2.f, 6.f, 12.f}}, 1e-4f,
                   "AABB corner transform handles non-uniform scale");
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

void testSimdMat4Parity() {
    const fuse::math::Mat4 scalarTranslate =
        fuse::math::fromTRS({3.f, -2.f, 7.f}, fuse::math::Quat::identity(), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 scalarRotate =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, 1.2f), {1.f, 1.f, 1.f});
    const fuse::math::simd::Mat4 simdTranslate = fuse::math::simd::Mat4::fromScalar(scalarTranslate);
    const fuse::math::simd::Mat4 simdRotate = fuse::math::simd::Mat4::fromScalar(scalarRotate);

    const fuse::math::simd::Mat4 simdProduct = fuse::math::simd::multiply(simdTranslate, simdRotate);
    const fuse::math::Mat4 scalarProduct = scalarTranslate * scalarRotate;
    expectMat4Near(simdProduct.toScalar(), scalarProduct, 1e-4f, "simd Mat4 multiply matches scalar");

    const fuse::math::simd::Mat4 simdIdentity = fuse::math::simd::Mat4::identity();
    expectMat4Near(fuse::math::simd::multiply(simdIdentity, simdTranslate).toScalar(), scalarTranslate, 1e-5f,
                   "simd Mat4 left identity multiply");

    const fuse::math::Vec3 probe{1.f, 0.f, 0.f};
    const fuse::math::Vec3 simdPoint = fuse::math::simd::transformPoint(simdProduct, probe);
    const fuse::math::Vec3 scalarPoint = fuse::math::transformPoint(scalarProduct, probe);
    expectVec3Near(simdPoint, scalarPoint, 1e-4f, "simd transformPoint matches scalar");
}

void testSimdMat4InverseEdgeCases() {
    const fuse::math::Mat4 scalarRigid =
        fuse::math::fromTRS({1.f, -3.f, 2.f}, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, -0.4f), {1.f, 1.f, 1.f});
    const fuse::math::simd::Mat4 simdRigid = fuse::math::simd::Mat4::fromScalar(scalarRigid);
    const fuse::math::simd::Mat4 simdInverse = fuse::math::simd::inverseAffine(simdRigid);
    const fuse::math::Mat4 scalarInverse = fuse::math::inverseAffine(scalarRigid);

    expectMat4Near(simdInverse.toScalar(), scalarInverse, 1e-4f, "simd inverseAffine matches scalar");
    expectMat4Near(fuse::math::simd::multiply(simdRigid, simdInverse).toScalar(), fuse::math::Mat4::identity(),
                   1e-4f, "simd rigid inverse product is identity");

    const fuse::math::simd::Mat4 skewed = fuse::math::simd::Mat4::fromScalar(
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::fromAxisAngle({1.f, 2.f, 0.5f}, 0.9f), {1.f, 1.f, 1.f}));
    const fuse::math::simd::Mat4 ortho = fuse::math::simd::orthonormalize(skewed);
    expectTrue(fuse::math::simd::isOrthogonalUpper3x3(ortho), "simd orthonormalize yields orthogonal upper 3x3");
}

void testSimdAabbStubs() {
    const fuse::math::AABB local{{-1.f, -2.f, -3.f}, {1.f, 2.f, 3.f}};
    const fuse::math::Mat4 scalarRigid =
        fuse::math::fromTRS({2.f, 1.f, -1.f}, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, 0.5f), {1.f, 1.f, 1.f});
    const fuse::math::simd::Mat4 simdRigid = fuse::math::simd::Mat4::fromScalar(scalarRigid);

    const fuse::math::AABB simdWorld = fuse::math::simd::transformAabb(simdRigid, local);
    const fuse::math::AABB scalarWorld = fuse::math::transformAabb(scalarRigid, local);
    expectAabbNear(simdWorld, scalarWorld, 1e-4f, "simd transformAabb matches scalar");

    const fuse::math::AABB merged = fuse::math::simd::mergeAabb(local, {{4.f, 4.f, 4.f}, {5.f, 5.f, 5.f}});
    expectAabbNear(merged, local.merge({{4.f, 4.f, 4.f}, {5.f, 5.f, 5.f}}), 1e-5f, "simd mergeAabb matches scalar");

    const f32 hit = fuse::math::simd::rayIntersectAabb(local, {-3.f, 0.f, 0.f}, {1.f, 0.f, 0.f});
    expectNear(hit, 2.f, 1e-4f, "simd rayIntersectAabb hits along +X");

    const f32 miss = fuse::math::simd::rayIntersectAabb(local, {-3.f, 5.f, 0.f}, {1.f, 0.f, 0.f});
    expectNear(miss, -1.f, 1e-5f, "simd rayIntersectAabb misses separated ray");
}

void testPlaneClassify() {
    const fuse::math::Vec4 plane{0.f, 1.f, 0.f, -2.f};

    expectTrue(fuse::math::classifyPoint(plane, {0.f, 3.f, 0.f}) == fuse::math::PlaneSide::InFront,
               "Plane classifyPoint in front");
    expectTrue(fuse::math::classifyPoint(plane, {0.f, 1.f, 0.f}) == fuse::math::PlaneSide::Behind,
               "Plane classifyPoint behind");
    expectTrue(fuse::math::classifyPoint(plane, {0.f, 2.f, 0.f}) == fuse::math::PlaneSide::On,
               "Plane classifyPoint on plane");

    const fuse::math::AABB above{{0.f, 3.f, 0.f}, {1.f, 4.f, 1.f}};
    const fuse::math::AABB below{{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}};
    const fuse::math::AABB crossing{{0.f, 1.f, 0.f}, {1.f, 3.f, 1.f}};

    expectTrue(fuse::math::classifyAabb(plane, above) == fuse::math::PlaneSide::InFront,
               "Plane classifyAabb fully in front");
    expectTrue(fuse::math::classifyAabb(plane, below) == fuse::math::PlaneSide::Behind,
               "Plane classifyAabb fully behind");
    expectTrue(fuse::math::classifyAabb(plane, crossing) == fuse::math::PlaneSide::Straddling,
               "Plane classifyAabb straddles plane");
}

void testPlaneClip() {
    const fuse::math::Vec4 plane{1.f, 0.f, 0.f, 0.f};

    fuse::math::Vec3 a{-2.f, 0.f, 0.f};
    fuse::math::Vec3 b{2.f, 0.f, 0.f};
    expectTrue(fuse::math::clipSegmentAgainstPlane(plane, a, b), "Plane clip keeps crossing segment");
    expectNear(a.x, 0.f, 1e-5f, "Plane clip segment start moves to plane");
    expectNear(b.x, 2.f, 1e-5f, "Plane clip segment end stays in front");

    fuse::math::Vec3 culledA{-3.f, 0.f, 0.f};
    fuse::math::Vec3 culledB{-1.f, 0.f, 0.f};
    expectTrue(!fuse::math::clipSegmentAgainstPlane(plane, culledA, culledB),
               "Plane clip rejects fully behind segment");

    const fuse::math::Vec3 square[4] = {
        {1.f, -1.f, 0.f},
        {2.f, -1.f, 0.f},
        {2.f, 1.f, 0.f},
        {1.f, 1.f, 0.f},
    };
    fuse::math::Vec3 clipped[8]{};
    const u32 clippedCount =
        fuse::math::clipPolygonAgainstPlane(plane, square, 4, clipped, 8);
    expectTrue(clippedCount == 4, "Plane polygon clip keeps square fully in front");
    expectNear(clipped[0].x, 1.f, 1e-5f, "Plane polygon clip preserves in-front square");

    const fuse::math::Vec3 halfSquare[4] = {
        {-2.f, -1.f, 0.f},
        {2.f, -1.f, 0.f},
        {2.f, 1.f, 0.f},
        {-2.f, 1.f, 0.f},
    };
    fuse::math::Vec3 halfClipped[8]{};
    const u32 halfCount =
        fuse::math::clipPolygonAgainstPlane(plane, halfSquare, 4, halfClipped, 8);
    expectTrue(halfCount == 4, "Plane polygon clip trims crossing quad");
    expectNear(halfClipped[0].x, 0.f, 1e-4f, "Plane polygon clip inserts plane intersection");
}

} // namespace

int main() {
    testVecBasics();
    testMat4MultiplyIdentity();
    testMat4MultiplyEdgeCases();
    testMat4TransformPoint();
    testMat4InverseAffine();
    testMat4InverseEdgeCases();
    testMat3Upper3x3();
    testQuatRotation();
    testQuatSlerp();
    testAabbOverlap();
    testAabbRayIntersect();
    testAabbTransformHelpers();
    testFrustumCulling();
    testSdfPrimitives();
    testSimdMat4Parity();
    testSimdMat4InverseEdgeCases();
    testSimdAabbStubs();
    testPlaneClassify();
    testPlaneClip();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d math test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_math_tests: all tests passed.\n");
    return EXIT_SUCCESS;
}
